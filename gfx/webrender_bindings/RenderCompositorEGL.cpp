/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RenderCompositorEGL.h"

#include "GLContext.h"
#include "GLContextEGL.h"
#include "GLContextProvider.h"
#include "GLLibraryEGL.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/layers/BuildConstants.h"
#include "mozilla/webrender/RenderThread.h"
#include "mozilla/widget/CompositorWidget.h"

#ifdef MOZ_WIDGET_GTK
#  include "mozilla/WidgetUtilsGtk.h"
#  include "mozilla/widget/GtkCompositorWidget.h"
#endif

#ifdef MOZ_WIDGET_ANDROID
#  include <android/native_window.h>
#  include <android/native_window_jni.h>

#  include "mozilla/java/GeckoSurfaceTextureWrappers.h"
#  include "mozilla/layers/AndroidHardwareBuffer.h"
#  include "mozilla/widget/AndroidCompositorWidget.h"
#endif

#if defined(GECKO_W10M)
#  include <cstring>
#  include <d3d10.h>
#  include <d3d11_3.h>
#  include "ScopedGLHelpers.h"
#  include "nsPrintfCString.h"
#  include "mozilla/Sprintf.h"
#  include "mozilla/gfx/DeviceManagerDx.h"
#  include "mozilla/layers/SyncObject.h"
#  include "mozilla/layers/FenceD3D11.h"
extern "C" void GeckoW10mNote(const char* text);
extern "C" uint64_t gecko_w10m_managed_id_for_widget(mozilla::widget::CompositorWidget*);

// GeckoW10mVideoLayer.cpp: hides the video layer once its video is gone.
void GeckoW10mVideoLayerEndFrame();
extern "C" void* gecko_w10m_egl_window_for_key(const void* aKey);
extern "C" void gecko_w10m_managed_frame_presented(uintptr_t aKey);
extern "C" bool gecko_w10m_managed_direct_present(uint64_t);
extern "C" void gecko_w10m_window_graphics_failed(uint64_t);
extern "C" void gecko_w10m_publish_managed_pixels(
    uintptr_t, int32_t, int32_t, const uint8_t*);
#endif

namespace mozilla::wr {

extern LazyLogModule gRenderThreadLog;
#define LOG(...) MOZ_LOG(gRenderThreadLog, LogLevel::Debug, (__VA_ARGS__))

#if defined(GECKO_W10M)
static RefPtr<gl::GLContext> CreatePrivateWindowGL(
    uint64_t id, RefPtr<ID3D11Device>& outDevice, nsACString& error) {
  const RefPtr<ID3D11Device> primary = gfx::DeviceManagerDx::Get()->GetCompositorDevice();
  if (!primary) { error.AssignLiteral("PrivateGL(no adapter device)"); return nullptr; }
  nsCString failure;
  RefPtr<gl::GLContext> primaryContext = RenderThread::Get()->SingletonGL(failure);
  if (!primaryContext || !primaryContext->IsANGLE()) {
    error.AssignLiteral("PrivateGL(primary EGL context unavailable)");
    return nullptr;
  }
  const auto display = gl::GLContextEGL::Cast(primaryContext)->mEgl;
  outDevice = primary;
  const UINT flags = primary->GetCreationFlags();
  gl::CreateContextFlags contextFlags = gl::CreateContextFlags::PREFER_ES3;
  if (StaticPrefs::gfx_webrender_prefer_robustness_AtStartup()) {
    contextFlags |= gl::CreateContextFlags::PREFER_ROBUSTNESS;
  }
  RefPtr<gl::GLContext> context = gl::GLContextEGL::CreateWithoutSurface(
      display, {contextFlags}, &failure);
  if (!context || !context->IsANGLE() || !context->MakeCurrent()) {
    error.Assign(nsPrintfCString("PrivateGL(context %s)", failure.get())); return nullptr;
  }
  char note[288];
  SprintfLiteral(note, "gfx: private-window-v2 shared-device id=%llu device=%p primary=%p context=%p display=%p flags=0x%x",
      static_cast<unsigned long long>(id), static_cast<void*>(outDevice.get()),
      static_cast<void*>(primary.get()), static_cast<void*>(context.get()),
      reinterpret_cast<void*>(display->mDisplay), flags);
  GeckoW10mNote(note);
  return context;
}

gfx::DeviceResetReason RenderCompositorEGL::IsContextLost(bool force) {
  const auto reset = RenderCompositor::IsContextLost(force);
  if (!mPrivateDevice) return reset;
  const HRESULT reason = mPrivateDevice->GetDeviceRemovedReason();
  if (SUCCEEDED(reason)) return reset;
  if (!mPrivateFailureNoted) {
    mPrivateFailureNoted = true;
    GeckoW10mNote(nsPrintfCString("gfx: private-window-v1 id=%llu device-lost=0x%08lx GLreset=%u",
        static_cast<unsigned long long>(mPrivateWindowId), static_cast<unsigned long>(reason),
        static_cast<unsigned>(reset)).get());
    const HMODULE angle = ::GetModuleHandleW(L"libGLESv2.dll");
    using Dump = void (*)(void*);
    const auto dump = angle ? reinterpret_cast<Dump>(::GetProcAddress(angle, "angle_uwp_dump_device_removed")) : nullptr;
    if (dump) dump(mPrivateDevice.get());
  }
  return reset == gfx::DeviceResetReason::OK ? gfx::DeviceResetReason::UNKNOWN : reset;
}

void RenderCompositorEGL::InsertFrameCompletionQuery(RenderedFrameId aFrameId) {
  const RefPtr<ID3D11Device> device = DiagnosticDevice();
  if (!device || FAILED(device->GetDeviceRemovedReason())) return;

  RefPtr<ID3D11Query> query = mRecycledFrameQuery.forget();
  if (!query) {
    const D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
    const HRESULT hr = device->CreateQuery(&desc, getter_AddRefs(query));
    if (FAILED(hr)) {
      gfxCriticalNoteOnce << "EGL frame completion query failed: " << gfx::hexa(hr);
      return;
    }
  }
  RefPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(getter_AddRefs(context));
  gl()->fFlush();
  context->End(query);
  context->Flush();
  mFrameQueries.emplace(aFrameId, std::move(query));
}

RenderedFrameId RenderCompositorEGL::GetLastCompletedFrameId() {
  const RefPtr<ID3D11Device> device = DiagnosticDevice();
  if (!device || FAILED(device->GetDeviceRemovedReason())) {
    return mLastCompletedFrameId;
  }
  RefPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(getter_AddRefs(context));
  while (!mFrameQueries.empty()) {
    auto& frame = mFrameQueries.front();
    if (context->GetData(frame.second, nullptr, 0,
                         D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
      break;
    }
    mLastCompletedFrameId = frame.first;
    mRecycledFrameQuery = std::move(frame.second);
    mFrameQueries.pop();
  }
  return mLastCompletedFrameId;
}

RenderedFrameId RenderCompositorEGL::UpdateFrameId() {
  const RenderedFrameId frameId = GetNextRenderFrameId();
  if (gl()->MakeCurrent()) InsertFrameCompletionQuery(frameId);
  return frameId;
}

RefPtr<ID3D11Device> RenderCompositorEGL::DiagnosticDevice() const {
  return mPrivateDevice ? mPrivateDevice : gfx::DeviceManagerDx::Get()->GetCompositorDevice();
}
#endif

/* static */
UniquePtr<RenderCompositor> RenderCompositorEGL::Create(
    const RefPtr<widget::CompositorWidget>& aWidget, nsACString& aError) {
  if (kIsLinux && !gfx::gfxVars::UseEGL()) {
    return nullptr;
  }
#if defined(GECKO_W10M)
  const uint64_t privateId = gecko_w10m_managed_id_for_widget(aWidget.get());
  if (privateId) {
    RefPtr<ID3D11Device> device;
    RefPtr<gl::GLContext> context = CreatePrivateWindowGL(privateId, device, aError);
    if (!context) return nullptr;
    const bool readback = !gecko_w10m_managed_direct_present(privateId);
    auto compositor = MakeUnique<RenderCompositorEGL>(aWidget, std::move(context), readback);
    GeckoW10mNote(readback ? "gfx: webapp GPU explicit framebuffer readback"
                           : "gfx: webapp GPU direct presentation");
    compositor->mPrivateDevice = device;
    compositor->mPrivateWindowId = privateId;
    compositor->mSyncObject = layers::SyncObjectHost::CreateSyncObjectHost(device);
    if (!compositor->mSyncObject || !compositor->mSyncObject->Init()) {
      aError.AssignLiteral("PrivateGL(texture synchronization initialization)");
      return nullptr;
    }
    GeckoW10mNote(nsPrintfCString("gfx: private-window-v1 id=%llu texture-sync ready",
        static_cast<unsigned long long>(privateId)).get());
    if (compositor->mEGLSurface == EGL_NO_SURFACE || !compositor->MakeCurrent()) {
      aError.AssignLiteral("PrivateGL(window surface)"); return nullptr;
    }
    return compositor;
  }
#endif
  RefPtr<gl::GLContext> gl = RenderThread::Get()->SingletonGL(aError);
  if (!gl) {
    if (aError.IsEmpty()) {
      aError.Assign("RcANGLE(no shared GL)"_ns);
    } else {
      aError.Append("(Create)"_ns);
    }
    return nullptr;
  }
  return MakeUnique<RenderCompositorEGL>(aWidget, std::move(gl));
}

EGLSurface RenderCompositorEGL::CreateEGLSurface() {
  EGLSurface surface = EGL_NO_SURFACE;
#if defined(GECKO_W10M)
  if (mReadbackSurface) {
    const auto* context = gl::GLContextEGL::Cast(gl());
    const EGLint attributes[] = {LOCAL_EGL_WIDTH, 16, LOCAL_EGL_HEIGHT, 16,
                                 LOCAL_EGL_NONE};
    return context->mEgl->fCreatePbufferSurface(context->mSurfaceConfig,
                                              attributes);
  }
  // The surface has to be made on the context's own display, not on the
  // default one. CreateEGLSurfaceForCompositorWidget fetches DefaultEglDisplay
  // and is handed the context's config -- fine on Android and Wayland, where
  // those are the same display, and wrong here: RenderThread::SingletonGL
  // builds its context on a display made from Gecko's D3D11 device, so the
  // config belongs to that one. EGL refused with 0x3005, EGL_BAD_CONFIG,
  // before it ever looked at the window, which is why nothing in ANGLE's
  // SwapChainPanel code ever ran.
  //
  // CreateCompatibleSurface takes both the display and the config from the
  // context, which is the pairing EGL requires.
  {
    const auto& gle = gl::GLContextEGL::Cast(gl());
    void* const window = gecko_w10m_egl_window_for_key(reinterpret_cast<const void*>(mWidget->GetWidgetKey()));
    if (!window) {
      GeckoW10mNote("gfx: no panel to make a surface for");
      return EGL_NO_SURFACE;
    }
    // ANGLE draws into an offscreen texture of its own and, on every
    // present, copies it into the swap chain's back buffer -- turning it the
    // right way up on the way, since GL counts rows from the bottom and
    // Direct3D from the top. In fullscreen that copy is a whole extra pass
    // over 2560x1440 pixels a frame. Asked for a surface whose rows run top
    // down, ANGLE leaves the texture out and WebRender draws straight into
    // the back buffer, turning its own picture the right way up instead
    // (SurfaceOriginIsTopLeft).
    const auto& egl = gle->mEgl;
    const char* extensions =
        egl->mLib->fQueryString(egl->mDisplay, LOCAL_EGL_EXTENSIONS);
    if (StaticPrefs::gfx_webrender_gecko_w10m_direct_back_buffer_AtStartup() &&
        extensions && strstr(extensions, "EGL_ANGLE_surface_orientation")) {
      const EGLint attribs[] = {0x33A8 /* EGL_SURFACE_ORIENTATION_ANGLE */,
                                0x0002 /* ..._INVERT_Y_ANGLE */,
                                LOCAL_EGL_NONE};
      surface = egl->fCreateWindowSurface(
          gle->mSurfaceConfig, reinterpret_cast<EGLNativeWindowType>(window),
          attribs);
      mTopLeftOrigin = surface != EGL_NO_SURFACE;
      GeckoW10mNote(mTopLeftOrigin
                        ? "gfx: drawing straight into the swap chain, no "
                          "offscreen copy"
                        : "gfx: a top-down surface was refused, back to the "
                          "offscreen copy");
    }
    if (surface == EGL_NO_SURFACE) {
      surface = gle->CreateCompatibleSurface(window);
    }
    GeckoW10mNote(surface == EGL_NO_SURFACE
                    ? "gfx: the context's own display refused the panel too"
                    : "gfx: the panel has a surface on the context's display");
    return surface;
  }
#endif
  surface = gl::GLContextEGL::CreateEGLSurfaceForCompositorWidget(
      mWidget, gl::GLContextEGL::Cast(gl())->mSurfaceConfig);
  if (surface == EGL_NO_SURFACE) {
    const auto* renderThread = RenderThread::Get();
    gfxCriticalNote << "Failed to create EGLSurface. "
                    << renderThread->RendererCount() << " renderers, "
                    << renderThread->ActiveRendererCount() << " active.";
  }
  return surface;
}

RenderCompositorEGL::RenderCompositorEGL(
    const RefPtr<widget::CompositorWidget>& aWidget,
    RefPtr<gl::GLContext>&& aGL, [[maybe_unused]] bool aReadbackSurface)
    : RenderCompositor(aWidget), mGL(aGL), mEGLSurface(EGL_NO_SURFACE) {
#if defined(GECKO_W10M)
  mReadbackSurface = aReadbackSurface;
#endif
  MOZ_ASSERT(mGL);
  LOG("RenderCompositorEGL::RenderCompositorEGL()");
#if defined(GECKO_W10M)
  const RefPtr<ID3D11Device> device = DiagnosticDevice();
  if (device && layers::FenceD3D11::IsSupported(device)) {
    mReadFenceRequired = true;
    mReadFence = layers::FenceD3D11::Create(device);
    GeckoW10mNote(mReadFence ? "gfx: EGL D3D11 read fence ready"
                             : "gfx: EGL D3D11 read fence creation failed");
  }
#endif
#if defined(GECKO_W10M)
  // Nobody was making the surface. Wayland makes it here, Android makes it in
  // Resume, and this target is neither -- so mEGLSurface stayed EGL_NO_SURFACE
  // for the whole of a run. WebRender then drew into a 1x1 pbuffer and
  // SwapBuffers returned false every frame, which is why ANGLE's window code
  // was never reached and nothing ever appeared. The panel is the surface, and
  // it is made the same way Wayland makes its own.
  mEGLSurface = CreateEGLSurface();
  if (mEGLSurface == EGL_NO_SURFACE) {
    GeckoW10mNote("gfx: the panel would not give an EGL surface");
  } else {
    gl::GLContextEGL::Cast(gl())->SetEGLSurfaceOverride(mEGLSurface);
    GeckoW10mNote("gfx: the panel has an EGL surface, frames go to the GPU");

    // Every frame was drawn and presented whole: 1440x2308 pixels composited,
    // then copied again by ANGLE from its offscreen texture to the swap
    // chain, for a video frame or a progress bar that changed a strip of it.
    // WebRender asked for partial present and was told the buffer's age is
    // unknown, which to it means "redraw everything". ANGLE's offscreen
    // texture is never swapped, so it always holds the previous frame; what
    // is missing is only a way to present part of it, and that is
    // EGL_NV_post_sub_buffer.
    const auto& egl = gl::GLContextEGL::Cast(gl())->mEgl;
    const char* extensions =
        egl->mLib->fQueryString(egl->mDisplay, LOCAL_EGL_EXTENSIONS);
    if (extensions && strstr(extensions, "EGL_NV_post_sub_buffer")) {
      mPostSubBuffer = reinterpret_cast<PostSubBufferFn>(
          egl->mLib->fGetProcAddress("eglPostSubBufferNV"));
    }
    GeckoW10mNote(mPostSubBuffer
                      ? "gfx: partial present -- only what changed is drawn "
                        "and presented"
                      : "gfx: no EGL_NV_post_sub_buffer -- every frame is "
                        "drawn whole");
  }
#endif
#ifdef MOZ_WAYLAND
  if (widget::GdkIsWaylandDisplay()) {
    mEGLSurface = CreateEGLSurface();
    if (!mEGLSurface) {
      RenderThread::Get()->HandleWebRenderError(WebRenderError::NEW_SURFACE);
      return;
    }
    // We have a new EGL surface, which on wayland needs to be configured for
    // non-blocking buffer swaps. We need MakeCurrent() to set our current EGL
    // context before we call eglSwapInterval, which is why we do it here
    // rather than where the surface was created.
    const auto& gle = gl::GLContextEGL::Cast(gl());
    const auto& egl = gle->mEgl;
    MakeCurrent();

    const int interval = gfx::gfxVars::SwapIntervalEGL() ? 1 : 0;
    egl->fSwapInterval(interval);
  }
#endif
}

RenderCompositorEGL::~RenderCompositorEGL() {
  LOG("RenderCompositorEGL::~RenderCompositorEGL()");
#ifdef MOZ_WIDGET_ANDROID
  java::GeckoSurfaceTexture::DestroyUnused((int64_t)gl());
#endif
#if defined(GECKO_W10M)
  if (mReadbackSurface && gl()->MakeCurrent()) DestroyReadbackTarget();
#endif
  DestroyEGLSurface();
}

#if defined(GECKO_W10M)
void RenderCompositorEGL::TraceFrameRouting(const char* aStage) {
  const auto& egl = gl::GLContextEGL::Cast(gl())->mEgl;
  const auto draw = egl->mLib->fGetCurrentSurface(LOCAL_EGL_DRAW);
  const auto read = egl->mLib->fGetCurrentSurface(LOCAL_EGL_READ);
  const RefPtr<ID3D11Device> device = DiagnosticDevice();
  const HRESULT reason = device ? device->GetDeviceRemovedReason() : E_POINTER;
  const auto currentDisplay = egl->mLib->fGetCurrentDisplay();
  const bool bad = FAILED(reason) || currentDisplay != egl->mDisplay ||
                   !gl::GLContextEGL::Cast(gl())->IsCurrentImpl() ||
                   draw != mEGLSurface || read != mEGLSurface;
  // The pre-bind stage is expected to see the previous window's surface.
  const bool beforeBind = strcmp(aStage, "before-bind") == 0;
  if (mRoutingFrame > 8 && mRoutingFrame % 600 != 0 &&
      (!bad || beforeBind || mRoutingFailures >= 16)) {
    return;
  }
  if (bad && !beforeBind) ++mRoutingFailures;
  const auto size = GetBufferSize();
  char note[512];
  SprintfLiteral(note,
                 "gfx: frame-routing-v1 key=%p frame=%llu stage=%s tid=%lu "
                 "widget=%dx%d expected=%p draw=%p read=%p context=%p "
                 "display=%p expected_display=%p device=0x%08lx",
                 reinterpret_cast<void*>(mWidget->GetWidgetKey()),
                 static_cast<unsigned long long>(mRoutingFrame), aStage,
                 static_cast<unsigned long>(::GetCurrentThreadId()), size.width,
                 size.height, reinterpret_cast<void*>(mEGLSurface),
                 reinterpret_cast<void*>(draw), reinterpret_cast<void*>(read),
                 reinterpret_cast<void*>(egl->mLib->fGetCurrentContext()),
                 reinterpret_cast<void*>(currentDisplay),
                 reinterpret_cast<void*>(egl->mDisplay),
                 static_cast<unsigned long>(reason));
  GeckoW10mNote(note);
}
#endif

bool RenderCompositorEGL::BeginFrame() {
#if defined(GECKO_W10M)
  if (mPrivateSyncFailed) return false;
  ++mRoutingFrame;
  TraceFrameRouting("before-bind");
#endif
  LOG("RenderCompositorEGL::BeginFrame()");
  if (kIsLinux && mEGLSurface == EGL_NO_SURFACE) {
    gfxCriticalNote
        << "We don't have EGLSurface to draw into. Called too early?";
    return false;
  }
#ifdef MOZ_WAYLAND
  if (auto* gtkWidget = mWidget->AsGTK()) {
    gtkWidget->SetEGLNativeWindowSize(GetBufferSize());
  }
#endif
  const bool madeCurrent = MakeCurrent();
#if defined(GECKO_W10M)
  TraceFrameRouting(madeCurrent ? "after-bind" : "bind-failed");
#endif
  if (!madeCurrent) {
    gfxCriticalNote << "Failed to make render context current, can't draw.";
    return false;
  }

#ifdef MOZ_WIDGET_ANDROID
  java::GeckoSurfaceTexture::DestroyUnused((int64_t)gl());
  gl()->MakeCurrent();  // DestroyUnused can change the current context!
#endif

#if defined(GECKO_W10M)
  if (mPrivateDevice && mSyncObject && RenderThread::Get()->SyncObjectNeeded() &&
      !mSyncObject->Synchronize(/* aFallible */ true)) {
    mPrivateSyncFailed = true;
    GeckoW10mNote(nsPrintfCString(
        "gfx: private-window-v1 id=%llu texture-sync failed device=0x%08lx",
        static_cast<unsigned long long>(mPrivateWindowId),
        static_cast<unsigned long>(mPrivateDevice->GetDeviceRemovedReason())).get());
    gecko_w10m_window_graphics_failed(mPrivateWindowId);
    return false;
  }
#endif
  return true;
}

RenderedFrameId RenderCompositorEGL::EndFrame(
    const nsTArray<DeviceIntRect>& aDirtyRects) {
  LOG("RenderCompositorEGL::EndFrame()");
#ifdef MOZ_WIDGET_ANDROID
  const auto& gle = gl::GLContextEGL::Cast(gl());
  const auto& egl = gle->mEgl;

  EGLSync sync = nullptr;
  if (layers::AndroidHardwareBufferManager::Get()) {
    sync = egl->fCreateSyncKHR(LOCAL_EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
  }
  if (sync) {
    int fenceFd = egl->fDupNativeFenceFDANDROID(sync);
    if (fenceFd >= 0) {
      mReleaseFence = new layers::FenceFileHandle(UniqueFileHandle(fenceFd));
    }
    egl->fDestroySync(sync);
    sync = nullptr;
  }
#endif

  RenderedFrameId frameId = GetNextRenderFrameId();
#if defined(GECKO_W10M)
  InsertFrameCompletionQuery(frameId);
  mReadFenceSignaled = false;
  if (mReadFence) {
    gl()->fFlush();
    mReadFenceSignaled = mReadFence->IncrementAndSignal();
    if (!mReadFenceSignaled) {
      GeckoW10mNote("gfx: EGL D3D11 read fence signal failed");
    }
  }
  if (mReadFenceRequired && !mReadFenceSignaled) {
    gl()->fFinish();
  }
#endif
#if defined(GECKO_W10M)
  TraceFrameRouting("after-render-before-present");
#endif
#ifdef MOZ_WIDGET_GTK
  if (mWidget->IsHidden()) {
    return frameId;
  }
#endif
#if defined(GECKO_W10M)
  if (mReadbackSurface) {
    if (mFullRender || !aDirtyRects.IsEmpty()) {
      mFullRender = !PublishReadback();
    }
    return frameId;
  }
  if (mPostSubBuffer && mEGLSurface != EGL_NO_SURFACE) {
    const auto bufferSize = GetBufferSize();
    // WebRender drew this frame whole (RequestFullRender decided it), so
    // the whole of it goes out.
    const bool whole = mWholeFrame;
    gfx::IntRect bounds;
    for (const DeviceIntRect& rect : aDirtyRects) {
      const auto left = std::clamp(rect.min.x, 0, bufferSize.width);
      const auto top = std::clamp(rect.min.y, 0, bufferSize.height);
      const auto right = std::clamp(rect.max.x, 0, bufferSize.width);
      const auto bottom = std::clamp(rect.max.y, 0, bufferSize.height);
      // EGL counts rows from the bottom.
      bounds = bounds.Union(gfx::IntRect(left, bufferSize.height - bottom,
                                         right - left, bottom - top));
    }
    if (whole) {
      bounds = gfx::IntRect(0, 0, bufferSize.width, bufferSize.height);
    }

    ++mPresentFrames;
    if (!mPrivateDevice) GeckoW10mVideoLayerEndFrame();
    if (bounds.IsEmpty()) {
      // Nothing changed on screen: WebRender returns no dirty rect at all,
      // and there is nothing to copy or present.
      ++mPresentSkipped;
    } else {
      const auto& egl = gl::GLContextEGL::Cast(gl())->mEgl;
      // How long handing the frame to the system takes, apart from drawing
      // it: with a video on the display overlay, small presents were seen
      // to cost a whole frame.
      const TimeStamp presentStart = TimeStamp::Now();
      const bool posted =
          mPostSubBuffer(egl->mDisplay, mEGLSurface, bounds.X(), bounds.Y(),
                         bounds.Width(), bounds.Height());
      const EGLint postError = posted ? LOCAL_EGL_SUCCESS : egl->mLib->fGetError();
      TraceFrameRouting(posted ? "after-present" : "present-failed");
      const double presentMs =
          (TimeStamp::Now() - presentStart).ToMilliseconds();
      mPresentMs += presentMs;
      mPresentMaxMs = std::max(mPresentMaxMs, presentMs);
      if (posted) {
        gecko_w10m_managed_frame_presented(mWidget->GetWidgetKey());
        mPresentedSize = bufferSize;
        // ANGLE resizes its buffers after a present, when it sees that the
        // panel changed size -- and moves the old picture into the new
        // offscreen texture as it does. So the frame after a resize has to
        // be drawn whole as well, and so does every frame until ANGLE's size
        // and the window's agree. Drawing only the dirty tiles into the
        // moved picture is what left a strip of new page over the old one
        // after the keyboard opened or closed.
        mFullRender = SurfaceSize() != bufferSize.ToUnknownSize();
      } else {
        // Present the next frame whole, whatever went wrong with this one.
        mFullRender = true;
        const auto surfaceSize = SurfaceSize();
        const RefPtr<ID3D11Device> device = DiagnosticDevice();
        char note[384];
        SprintfLiteral(note,
                       "gfx: diag: eglPostSubBufferNV failed EGL=0x%04x widget=%dx%d surface=%dx%d "
                       "previous=%dx%d rect=%d,%d %dx%d whole=%d dirty_count=%zu "
                       "context_lost=%d device=%s reason=0x%08lx; next frame whole",
                       postError, bufferSize.width, bufferSize.height,
                       surfaceSize.width, surfaceSize.height,
                       mPresentedSize.width, mPresentedSize.height,
                       bounds.X(), bounds.Y(), bounds.Width(), bounds.Height(),
                       whole ? 1 : 0, static_cast<size_t>(aDirtyRects.Length()),
                       gl()->IsContextLost() ? 1 : 0,
                       device ? "present" : "null",
                       static_cast<unsigned long>(device ? device->GetDeviceRemovedReason() : E_POINTER));
        GeckoW10mNote(note);
      }
      if (whole) {
        ++mPresentWhole;
      }
      if (bufferSize.width > 0 && bufferSize.height > 0) {
        mPresentArea += double(bounds.Width()) * bounds.Height() /
                        (double(bufferSize.width) * bufferSize.height);
      }
    }

    const TimeStamp now = TimeStamp::Now();
    if (mPresentStatsStart.IsNull()) {
      mPresentStatsStart = now;
    }
    const double span = (now - mPresentStatsStart).ToSeconds();
    if (span >= 10) {
      if (mPresentFrames >= 100) {
        const uint32_t presented = mPresentFrames - mPresentSkipped;
        char note[256];
        SprintfLiteral(note,
                       "present: %u frames, %u with nothing changed (not "
                       "presented), %u whole; a presented frame is %.0f%% of "
                       "the screen on average and took %.1f ms to hand over "
                       "(%.1f at most)",
                       mPresentFrames, mPresentSkipped, mPresentWhole,
                       presented ? 100.0 * mPresentArea / presented : 0.0,
                       presented ? mPresentMs / presented : 0.0,
                       mPresentMaxMs);
        GeckoW10mNote(note);
      }
      mPresentStatsStart = now;
      mPresentFrames = mPresentSkipped = mPresentWhole = 0;
      mPresentArea = 0;
      mPresentMs = mPresentMaxMs = 0;
    }
    return frameId;
  }
#endif
  if (mEGLSurface != EGL_NO_SURFACE && aDirtyRects.Length() > 0) {
    gfx::IntRegion bufferInvalid;
    const auto bufferSize = GetBufferSize();
    for (const DeviceIntRect& rect : aDirtyRects) {
      const auto left = std::clamp(rect.min.x, 0, bufferSize.width);
      const auto top = std::clamp(rect.min.y, 0, bufferSize.height);

      const auto right = std::clamp(rect.max.x, 0, bufferSize.width);
      const auto bottom = std::clamp(rect.max.y, 0, bufferSize.height);

      const auto width = right - left;
      const auto height = bottom - top;

      bufferInvalid.OrWith(
          gfx::IntRect(left, (GetBufferSize().height - bottom), width, height));
    }
    gl()->SetDamage(bufferInvalid);
  }

#ifdef MOZ_WAYLAND
  // Rendering on Wayland has to be atomic (buffer attach + commit) and
  // wayland surface is also used by main thread so lock it before
  // we paint at SwapBuffers().
  UniquePtr<widget::WaylandSurfaceLock> lock;
  if (auto* gtkWidget = mWidget->AsGTK()) {
    lock = gtkWidget->LockSurface();
  }
#endif
#if defined(GECKO_W10M)
  if (gl()->SwapBuffers()) {
    gecko_w10m_managed_frame_presented(mWidget->GetWidgetKey());
  }
#else
  gl()->SwapBuffers();
#endif
  return frameId;
}

void RenderCompositorEGL::Pause() {
  if (kIsAndroid) {
    DestroyEGLSurface();
  }
}

bool RenderCompositorEGL::Resume() {
  LOG("RenderCompositorEGL::Resume()");
  if (kIsAndroid) {
    // Destroy EGLSurface if it exists.
    DestroyEGLSurface();

    auto size = GetBufferSize();
    GLint maxTextureSize = 0;
    gl()->fGetIntegerv(LOCAL_GL_MAX_TEXTURE_SIZE, (GLint*)&maxTextureSize);

    // When window size is too big, hardware buffer allocation could fail.
    if (maxTextureSize < size.width || maxTextureSize < size.height) {
      gfxCriticalNote << "Too big ANativeWindow size(" << size.width << ", "
                      << size.height << ") MaxTextureSize " << maxTextureSize;
      return false;
    }

    mEGLSurface = CreateEGLSurface();
    if (mEGLSurface == EGL_NO_SURFACE) {
      // Often when we fail to create an EGL surface it is because the Java
      // Surface we have been provided is invalid. Therefore the on the first
      // occurence we don't raise a WebRenderError and instead just return
      // failure. This allows the widget a chance to request a new Java
      // Surface. On subsequent failures, raising the WebRenderError will
      // result in the compositor being recreated, falling back through
      // webrender configurations, and eventually crashing if we still do not
      // succeed.
      if (!mHandlingNewSurfaceError) {
        mHandlingNewSurfaceError = true;
      } else {
        RenderThread::Get()->HandleWebRenderError(WebRenderError::NEW_SURFACE);
      }
      return false;
    }
    mHandlingNewSurfaceError = false;

    gl::GLContextEGL::Cast(gl())->SetEGLSurfaceOverride(mEGLSurface);
  } else if (kIsLinux) {
#ifdef MOZ_X11
    if (widget::GdkIsX11Display()) {
      DestroyEGLSurface();
      mEGLSurface = CreateEGLSurface();
      if (mEGLSurface != EGL_NO_SURFACE) {
        // We have a new EGL surface, which on wayland needs to be configured
        // for non-blocking buffer swaps. We need MakeCurrent() to set our
        // current EGL context before we call eglSwapInterval, which is why we
        // do it here rather than where the surface was created.
        const auto& gle = gl::GLContextEGL::Cast(gl());
        const auto& egl = gle->mEgl;
        MakeCurrent();

        const int interval = gfx::gfxVars::SwapIntervalEGL() ? 1 : 0;
        egl->fSwapInterval(interval);
      } else {
        RenderThread::Get()->HandleWebRenderError(WebRenderError::NEW_SURFACE);
        return false;
      }
    }
#endif
  }
  return true;
}

bool RenderCompositorEGL::IsPaused() { return mEGLSurface == EGL_NO_SURFACE; }

bool RenderCompositorEGL::MakeCurrent() {
  const auto& gle = gl::GLContextEGL::Cast(gl());

  if (!gle->SetEGLSurfaceOverride(mEGLSurface)) return false;
  bool ok = gl()->MakeCurrent();
#if defined(GECKO_W10M)
  if (ok && mReadbackSurface) {
    if (!EnsureReadbackTarget()) return false;
    // WebRender captures the bound framebuffer as its default each frame.
    gl()->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mReadbackFramebuffer);
  }
#endif
  if (!gl()->IsGLES() && ok && mEGLSurface != EGL_NO_SURFACE) {
    // If we successfully made a surface current, set the draw buffer
    // appropriately. It's not well-defined by the EGL spec whether
    // eglMakeCurrent should do this automatically. See bug 1646135.
    gl()->fDrawBuffer(gl()->IsDoubleBuffered() ? LOCAL_GL_BACK
                                               : LOCAL_GL_FRONT);
  }
  return ok;
}

void RenderCompositorEGL::DestroyEGLSurface() {
  const auto& gle = gl::GLContextEGL::Cast(gl());
  const auto& egl = gle->mEgl;

  // Release EGLSurface of back buffer before calling ResizeBuffers().
  if (mEGLSurface) {
    gle->SetEGLSurfaceOverride(EGL_NO_SURFACE);
    gl::GLContextEGL::DestroySurface(*egl, mEGLSurface);
    mEGLSurface = nullptr;
  }
}

RefPtr<layers::Fence> RenderCompositorEGL::GetAndResetReleaseFence() {
#if defined(GECKO_W10M)
  return mReadFence && mReadFenceSignaled ? mReadFence->CloneFromHandle() : nullptr;
#endif
#ifdef MOZ_WIDGET_ANDROID
  MOZ_ASSERT(!layers::AndroidHardwareBufferManager::Get() || mReleaseFence);
  return mReleaseFence.forget();
#else
  return nullptr;
#endif
}

LayoutDeviceIntSize RenderCompositorEGL::GetBufferSize() {
#if defined(GECKO_W10M)
  if (mReadbackSurface && mEGLSurface != EGL_NO_SURFACE) {
    return LayoutDeviceIntSize(mReadbackSize.width, mReadbackSize.height);
  }
#endif
  return mWidget->GetClientSize();
}

#if defined(GECKO_W10M)
void RenderCompositorEGL::DestroyReadbackTarget() {
  gl()->fDeleteFramebuffers(1, &mReadbackFramebuffer);
  gl()->fDeleteRenderbuffers(1, &mReadbackDepth);
  gl()->fDeleteTextures(1, &mReadbackTexture);
  mReadbackFramebuffer = mReadbackDepth = mReadbackTexture = 0;
}

bool RenderCompositorEGL::EnsureReadbackTarget() {
  const auto size = mWidget->GetClientSize().ToUnknownSize();
  if (mReadbackFramebuffer && size == mReadbackSize) return true;
  GLint limit = 0;
  gl()->fGetIntegerv(LOCAL_GL_MAX_TEXTURE_SIZE, &limit);
  if (size.width <= 0 || size.height <= 0 || size.width > limit ||
      size.height > limit ||
      uint64_t(size.width) * size.height * 4 > INT32_MAX) {
    return false;
  }
  GLuint texture = 0, framebuffer = 0, depth = 0;
  GLint oldTexture = 0, oldDepth = 0;
  gl()->fGetIntegerv(LOCAL_GL_TEXTURE_BINDING_2D, &oldTexture);
  gl()->fGetIntegerv(LOCAL_GL_RENDERBUFFER_BINDING, &oldDepth);
  GLenum status = LOCAL_GL_FRAMEBUFFER_UNDEFINED;
  GLenum error = LOCAL_GL_NO_ERROR;
  {
    gl::ScopedBindFramebuffer restoreFramebuffer(gl());
    gl()->fGenTextures(1, &texture);
    gl()->fBindTexture(LOCAL_GL_TEXTURE_2D, texture);
    gl()->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MIN_FILTER,
                         LOCAL_GL_NEAREST);
    gl()->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MAG_FILTER,
                         LOCAL_GL_NEAREST);
    gl()->fTexStorage2D(LOCAL_GL_TEXTURE_2D, 1, LOCAL_GL_RGBA8, size.width,
                        size.height);
    gl()->fGenRenderbuffers(1, &depth);
    gl()->fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, depth);
    gl()->fRenderbufferStorage(LOCAL_GL_RENDERBUFFER, LOCAL_GL_DEPTH24_STENCIL8,
                               size.width, size.height);
    gl()->fGenFramebuffers(1, &framebuffer);
    gl()->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, framebuffer);
    gl()->fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER,
                                LOCAL_GL_COLOR_ATTACHMENT0, LOCAL_GL_TEXTURE_2D,
                                texture, 0);
    gl()->fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER,
                                   LOCAL_GL_DEPTH_STENCIL_ATTACHMENT,
                                   LOCAL_GL_RENDERBUFFER, depth);
    status = gl()->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER);
    error = gl()->fGetError();
    gl()->fBindTexture(LOCAL_GL_TEXTURE_2D, oldTexture);
    gl()->fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, oldDepth);
  }
  if (status != LOCAL_GL_FRAMEBUFFER_COMPLETE || error != LOCAL_GL_NO_ERROR) {
    gl()->fDeleteFramebuffers(1, &framebuffer);
    gl()->fDeleteRenderbuffers(1, &depth);
    gl()->fDeleteTextures(1, &texture);
    GeckoW10mNote(
        nsPrintfCString(
            "gfx: webapp FBO allocation failed status=0x%x error=0x%x", status,
            error)
            .get());
    return false;
  }
  DestroyReadbackTarget();
  mReadbackFramebuffer = framebuffer;
  mReadbackTexture = texture;
  mReadbackDepth = depth;
  mReadbackSize = size;
  mFullRender = true;
  GeckoW10mNote(nsPrintfCString("gfx: webapp FBO ready %dx%d framebuffer=%u",
                                size.width, size.height, framebuffer)
                    .get());
  return true;
}

bool RenderCompositorEGL::PublishReadback() {
  const int width = mReadbackSize.width;
  const int height = mReadbackSize.height;
  const size_t stride = size_t(width) * 4;
  if (!mReadbackPixels.SetLength(stride * height, fallible)) return false;

  auto* context = gl();
  gl::ScopedBindFramebuffer framebuffer(context, mReadbackFramebuffer);
  GLint packBuffer, alignment, rowLength, skipRows, skipPixels;
  context->fGetIntegerv(LOCAL_GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
  context->fGetIntegerv(LOCAL_GL_PACK_ALIGNMENT, &alignment);
  context->fGetIntegerv(LOCAL_GL_PACK_ROW_LENGTH, &rowLength);
  context->fGetIntegerv(LOCAL_GL_PACK_SKIP_ROWS, &skipRows);
  context->fGetIntegerv(LOCAL_GL_PACK_SKIP_PIXELS, &skipPixels);
  context->fBindBuffer(LOCAL_GL_PIXEL_PACK_BUFFER, 0);
  context->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 4);
  context->fPixelStorei(LOCAL_GL_PACK_ROW_LENGTH, 0);
  context->fPixelStorei(LOCAL_GL_PACK_SKIP_ROWS, 0);
  context->fPixelStorei(LOCAL_GL_PACK_SKIP_PIXELS, 0);
  const RefPtr<ID3D11Device> device = DiagnosticDevice();
  const HRESULT beforeRead = device ? device->GetDeviceRemovedReason() : E_POINTER;
  const uint64_t fenceTarget = mReadFence ? mReadFence->GetFenceValue() : 0;
  const uint64_t completedBefore = mReadFence
      ? mReadFence->mSignalFence->GetCompletedValue() : 0;
  const auto readStart = TimeStamp::Now();
  context->fReadPixels(0, 0, width, height, LOCAL_GL_RGBA,
                       LOCAL_GL_UNSIGNED_BYTE, mReadbackPixels.Elements());
  const double readMs = (TimeStamp::Now() - readStart).ToMilliseconds();
  const GLenum readError = context->fGetError();
  const HRESULT afterRead = device ? device->GetDeviceRemovedReason() : E_POINTER;
  if (FAILED(afterRead) || readError != LOCAL_GL_NO_ERROR ||
      (readMs >= 100 && mSlowReadbackNotes++ < 16)) {
    GeckoW10mNote(nsPrintfCString(
        "gfx: readback-boundary-v1 id=%llu frame=%llu size=%dx%d ms=%.2f "
        "before=0x%08lx after=0x%08lx GL=0x%x fence=%llu completed-before=%llu completed-after=%llu",
        static_cast<unsigned long long>(mPrivateWindowId),
        static_cast<unsigned long long>(mRoutingFrame), width, height, readMs,
        static_cast<unsigned long>(beforeRead), static_cast<unsigned long>(afterRead),
        unsigned(readError), static_cast<unsigned long long>(fenceTarget),
        static_cast<unsigned long long>(completedBefore),
        static_cast<unsigned long long>(mReadFence
            ? mReadFence->mSignalFence->GetCompletedValue() : 0)).get());
  }
  context->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, alignment);
  context->fPixelStorei(LOCAL_GL_PACK_ROW_LENGTH, rowLength);
  context->fPixelStorei(LOCAL_GL_PACK_SKIP_ROWS, skipRows);
  context->fPixelStorei(LOCAL_GL_PACK_SKIP_PIXELS, skipPixels);
  context->fBindBuffer(LOCAL_GL_PIXEL_PACK_BUFFER, packBuffer);
  if (context->fGetError() != LOCAL_GL_NO_ERROR || readError != LOCAL_GL_NO_ERROR || FAILED(afterRead)) return false;

  // Convert GL's bottom-up RGBA to the Shell bitmap's top-down BGRA.
  auto* data = mReadbackPixels.Elements();
  for (int row = 0; row < height / 2; ++row) {
    auto* top = data + stride * row;
    auto* bottom = data + stride * (height - row - 1);
    std::swap_ranges(top, top + stride, bottom);
  }
  for (size_t i = 0; i < stride * height; i += 4) {
    std::swap(data[i], data[i + 2]);
  }
  gecko_w10m_publish_managed_pixels(mWidget->GetWidgetKey(), width, height, data);
  return true;
}
#endif

bool RenderCompositorEGL::UsePartialPresent() {
#if defined(GECKO_W10M)
  if (mReadbackSurface) return true;
  if (mPostSubBuffer) {
    return true;
  }
#endif
  return gfx::gfxVars::WebRenderMaxPartialPresentRects() > 0;
}

bool RenderCompositorEGL::RequestFullRender() {
#if defined(GECKO_W10M)
  if (mReadbackSurface) return mFullRender;
  if (mPostSubBuffer) {
    const auto bufferSize = GetBufferSize();
    const gfx::IntSize surfaceSize = SurfaceSize();
    mWholeFrame = mFullRender || bufferSize != mPresentedSize ||
                  surfaceSize != bufferSize.ToUnknownSize() ||
                  surfaceSize != mPresentedSurfaceSize;
    mPresentedSurfaceSize = surfaceSize;
    return mWholeFrame;
  }
#endif
  return false;
}

#if defined(GECKO_W10M)
gfx::IntSize RenderCompositorEGL::SurfaceSize() const {
  EGLint width = 0;
  EGLint height = 0;
  if (mEGLSurface != EGL_NO_SURFACE) {
    const auto& egl = gl::GLContextEGL::Cast(gl())->mEgl;
    egl->fQuerySurface(mEGLSurface, LOCAL_EGL_WIDTH, &width);
    egl->fQuerySurface(mEGLSurface, LOCAL_EGL_HEIGHT, &height);
  }
  return gfx::IntSize(width, height);
}
#endif

uint32_t RenderCompositorEGL::GetMaxPartialPresentRects() {
#if defined(GECKO_W10M)
  if (mReadbackSurface) return 1;
  if (mPostSubBuffer) {
    return 1;  // eglPostSubBufferNV takes one rectangle
  }
#endif
  return gfx::gfxVars::WebRenderMaxPartialPresentRects();
}

bool RenderCompositorEGL::ShouldDrawPreviousPartialPresentRegions() {
#if defined(GECKO_W10M)
  if (mReadbackSurface) return false;
  // What was drawn before is still there: either in ANGLE's offscreen
  // texture, which is the same one every frame, or -- drawing straight into
  // the swap chain -- because a flip-model present with a dirty rect has
  // DXGI carry the rest of the previous frame into the next back buffer,
  // which is what Firefox's own ANGLE compositor relies on too.
  if (mPostSubBuffer) {
    return false;
  }
#endif
  return true;
}

size_t RenderCompositorEGL::GetBufferAge() const {
#if defined(GECKO_W10M)
  if (mReadbackSurface) return 1;
#endif
  if (!StaticPrefs::
          gfx_webrender_allow_partial_present_buffer_age_AtStartup()) {
    return 0;
  }
  return gl()->GetBufferAge();
}

void RenderCompositorEGL::SetBufferDamageRegion(const wr::DeviceIntRect* aRects,
                                                size_t aNumRects) {
#if defined(GECKO_W10M)
  if (mReadbackSurface) return;
#endif
  const auto& gle = gl::GLContextEGL::Cast(gl());
  const auto& egl = gle->mEgl;
  if (gle->HasKhrPartialUpdate() &&
      StaticPrefs::gfx_webrender_allow_partial_present_buffer_age_AtStartup()) {
    std::vector<EGLint> rects;
    rects.reserve(4 * aNumRects);
    const auto bufferSize = GetBufferSize();
    for (size_t i = 0; i < aNumRects; i++) {
      const auto left = std::clamp(aRects[i].min.x, 0, bufferSize.width);
      const auto top = std::clamp(aRects[i].min.y, 0, bufferSize.height);

      const auto right = std::clamp(aRects[i].max.x, 0, bufferSize.width);
      const auto bottom = std::clamp(aRects[i].max.y, 0, bufferSize.height);

      const auto width = right - left;
      const auto height = bottom - top;

      rects.push_back(left);
      rects.push_back(bufferSize.height - bottom);
      rects.push_back(width);
      rects.push_back(height);
    }
    const auto ret =
        egl->fSetDamageRegion(mEGLSurface, rects.data(), rects.size() / 4);
    if (ret == LOCAL_EGL_FALSE) {
      const auto err = egl->mLib->fGetError();
      gfxCriticalError() << "Error in eglSetDamageRegion: " << gfx::hexa(err);
    }
  }
}

}  // namespace mozilla::wr
