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
#if defined(GECKO_W10M)
#  include "mozilla/layers/CompositorBridgeParent.h"
#  include "mozilla/layers/CompositorThread.h"
#endif
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
#  include "mozilla/Sprintf.h"
extern "C" void GeckoW10mNote(const char* text);
// GeckoW10mVideoLayer.cpp: hides the video layer once its video is gone.
void GeckoW10mVideoLayerEndFrame();
extern "C" void* gecko_w10m_egl_native_window();
#endif

namespace mozilla::wr {

extern LazyLogModule gRenderThreadLog;
#define LOG(...) MOZ_LOG(gRenderThreadLog, LogLevel::Debug, (__VA_ARGS__))

/* static */
UniquePtr<RenderCompositor> RenderCompositorEGL::Create(
    const RefPtr<widget::CompositorWidget>& aWidget, nsACString& aError) {
  if (kIsLinux && !gfx::gfxVars::UseEGL()) {
    return nullptr;
  }
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
    void* const window = gecko_w10m_egl_native_window();
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
    RefPtr<gl::GLContext>&& aGL)
    : RenderCompositor(aWidget), mGL(aGL), mEGLSurface(EGL_NO_SURFACE) {
  MOZ_ASSERT(mGL);
  LOG("RenderCompositorEGL::RenderCompositorEGL()");
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
    // The preference was set to 0 once and then never read on this path, so
    // partial present ran regardless of it. Now 0 means whole frames, for a
    // comparison on the phone, and the build ships with partial present on.
    mWholeFrames =
        mPostSubBuffer &&
        StaticPrefs::gfx_webrender_max_partial_present_rects_AtStartup() == 0;
    {
      char note[256];
      SprintfLiteral(
          note, "gfx: %s (max-partial-present-rects: pref %u, in use %d)",
          !mPostSubBuffer ? "no EGL_NV_post_sub_buffer -- every frame is "
                            "drawn whole"
          : mWholeFrames  ? "whole frames -- a frame that changed anything is "
                            "drawn and presented whole, one that changed "
                            "nothing is skipped"
                          : "partial present -- only what changed is drawn "
                            "and presented",
          StaticPrefs::gfx_webrender_max_partial_present_rects_AtStartup(),
          int(gfx::gfxVars::WebRenderMaxPartialPresentRects()));
      GeckoW10mNote(note);
    }
    // WebRender reads this when it sets its device up, which is after the
    // compositor is made (WebRenderAPI.cpp, NewRenderer).
    if (!StaticPrefs::gfx_webrender_gecko_w10m_tile_partial_update_AtStartup()) {
      ::SetEnvironmentVariableW(L"GECKO_W10M_NO_TILE_PARTIAL_UPDATE", L"1");
      GeckoW10mNote("gfx: a changed picture tile is redrawn whole");
    }
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

#if defined(GECKO_W10M)
// Render thread only.
static RenderCompositorEGL* sRetryOwner = nullptr;
static uint32_t sRetryGen = 0;

// ANGLE looks at the panel's size, and makes again a swap chain it had to
// give up, only inside a present. When it could not give the surface the size
// the window has -- refused for want of address space -- the next try needs a
// frame to carry it, and a page that is standing still sends none: the
// picture would stay cut to the old size until the user touched something.
// So one frame is asked for a while later, and later still each time the
// sizes go on disagreeing. A post that failed outright -- no chain at all --
// asks again in half a second every time: ANGLE spaces its own tries at
// making one, a frame that finds it not yet due costs it nothing, and the
// frame that does make the chain shows nothing itself, so the one after it
// should not be long.
void RenderCompositorEGL::ScheduleRetryFrame() {
  // One at a time -- but a post that failed does not wait behind a retry set
  // for sizes that disagreed, which can be half a minute away: that one is
  // put aside (its generation is no longer the current one) for the quick
  // one.
  if (mRetryPending && (!mPostFailed || mRetryQuick)) {
    return;
  }
  mRetryPending = true;
  mRetryQuick = mPostFailed;
  sRetryOwner = this;
  const uint32_t gen = ++sRetryGen;
  RenderCompositorEGL* self = this;
  uint32_t delayMs = 500;
  if (!mPostFailed) {
    delayMs = mRetryDelayMs;
    mRetryDelayMs = std::min<uint32_t>(mRetryDelayMs * 2, 30000);
  }
  (void)NS_DelayedDispatchToCurrentThread(
      NS_NewRunnableFunction(
          "RenderCompositorEGL::RetryFrame",
          [self, gen]() {
            if (sRetryOwner != self || sRetryGen != gen) {
              return;
            }
            self->mRetryPending = false;
            if (self->mEGLSurface == EGL_NO_SURFACE) {
              return;
            }
            if (!self->mPostFailed &&
                self->SurfaceSize() == self->GetBufferSize().ToUnknownSize()) {
              // They agree by now -- the usual case, the window's own resize
              // having caught up with the panel's.
              self->mRetryDelayMs = 1000;
              return;
            }
            if (nsCOMPtr<nsISerialEventTarget> thread =
                    layers::CompositorThread()) {
              thread->Dispatch(NS_NewRunnableFunction(
                  "GeckoW10mForceComposite",
                  []() { layers::CompositorBridgeParent::GeckoW10mForceComposite(); }));
            }
          }),
      delayMs);
}
#endif

RenderCompositorEGL::~RenderCompositorEGL() {
  LOG("RenderCompositorEGL::~RenderCompositorEGL()");
#if defined(GECKO_W10M)
  if (sRetryOwner == this) {
    sRetryOwner = nullptr;
  }
#endif
#ifdef MOZ_WIDGET_ANDROID
  java::GeckoSurfaceTexture::DestroyUnused((int64_t)gl());
#endif
  DestroyEGLSurface();
}

bool RenderCompositorEGL::BeginFrame() {
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
  if (!MakeCurrent()) {
    gfxCriticalNote << "Failed to make render context current, can't draw.";
    return false;
  }

#ifdef MOZ_WIDGET_ANDROID
  java::GeckoSurfaceTexture::DestroyUnused((int64_t)gl());
  gl()->MakeCurrent();  // DestroyUnused can change the current context!
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
#ifdef MOZ_WIDGET_GTK
  if (mWidget->IsHidden()) {
    return frameId;
  }
#endif
#if defined(GECKO_W10M)
  if (mPostSubBuffer && mEGLSurface != EGL_NO_SURFACE) {
    const auto bufferSize = GetBufferSize();
    // WebRender drew this frame whole (RequestFullRender decided it, or
    // whole frames are asked for and it changed something), so the whole of
    // it goes out.
    const bool whole = mWholeFrame || (mWholeFrames && !aDirtyRects.IsEmpty());
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
      // The surface's own rectangle, not the window's. The two differ while
      // ANGLE has not (or not yet) given the surface the window's size, and
      // ANGLE counts this rectangle's rows from the bottom of its own height:
      // the window's, on a taller surface, named the rows at the wrong edge
      // as changed and left the ones that were drawn as stale.
      const gfx::IntSize surfaceSize = SurfaceSize();
      bounds = surfaceSize.IsEmpty()
                   ? gfx::IntRect(0, 0, bufferSize.width, bufferSize.height)
                   : gfx::IntRect(0, 0, surfaceSize.width, surfaceSize.height);
    }

    ++mPresentFrames;
    GeckoW10mVideoLayerEndFrame();
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
      const double presentMs =
          (TimeStamp::Now() - presentStart).ToMilliseconds();
      mPresentMs += presentMs;
      mPresentMaxMs = std::max(mPresentMaxMs, presentMs);
      mPostFailed = !posted;
      if (!posted || SurfaceSize() != bufferSize.ToUnknownSize()) {
        ScheduleRetryFrame();
      } else {
        // All is well again. A retry still waiting -- up to half a minute
        // away, after a long refusal -- is dropped with it, so that the next
        // trouble starts from one second and not from behind that one.
        mRetryDelayMs = 1000;
        if (mRetryPending) {
          mRetryPending = false;
          ++sRetryGen;
        }
      }
      if (posted) {
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
        // Said at most once a second: a surface whose buffers cannot be made
        // (ANGLE's SwapChain11, out of address space) fails at every frame
        // until they can.
        static TimeStamp sLastSaid;
        static uint32_t sFailedSince = 0;
        ++sFailedSince;
        const TimeStamp failedAt = TimeStamp::Now();
        if (sLastSaid.IsNull() ||
            (failedAt - sLastSaid).ToMilliseconds() >= 1000) {
          char note[112];
          SprintfLiteral(note,
                         "gfx: eglPostSubBufferNV failed (%u since last said), "
                         "next frame whole",
                         sFailedSince);
          GeckoW10mNote(note);
          sLastSaid = failedAt;
          sFailedSince = 0;
        }
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
  gl()->SwapBuffers();
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

  gle->SetEGLSurfaceOverride(mEGLSurface);
  bool ok = gl()->MakeCurrent();
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
#ifdef MOZ_WIDGET_ANDROID
  MOZ_ASSERT(!layers::AndroidHardwareBufferManager::Get() || mReleaseFence);
  return mReleaseFence.forget();
#else
  return nullptr;
#endif
}

LayoutDeviceIntSize RenderCompositorEGL::GetBufferSize() {
  return mWidget->GetClientSize();
}

bool RenderCompositorEGL::UsePartialPresent() {
#if defined(GECKO_W10M)
  if (mPostSubBuffer) {
    return true;
  }
#endif
  return gfx::gfxVars::WebRenderMaxPartialPresentRects() > 0;
}

bool RenderCompositorEGL::RequestFullRender() {
#if defined(GECKO_W10M)
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
  if (mPostSubBuffer) {
    return 1;  // eglPostSubBufferNV takes one rectangle
  }
#endif
  return gfx::gfxVars::WebRenderMaxPartialPresentRects();
}

bool RenderCompositorEGL::ShouldDrawPreviousPartialPresentRegions() {
#if defined(GECKO_W10M)
  // What was drawn before is still there: either in ANGLE's offscreen
  // texture, which is the same one every frame, or -- drawing straight into
  // the swap chain -- because a flip-model present with a dirty rect has
  // DXGI carry the rest of the previous frame into the next back buffer,
  // which is what Firefox's own ANGLE compositor relies on too.
  if (mPostSubBuffer) {
    // Whole frames: nothing of it is trusted, and with a buffer age of 0
    // WebRender redraws all of any frame that changed something.
    return mWholeFrames;
  }
#endif
  return true;
}

size_t RenderCompositorEGL::GetBufferAge() const {
  if (!StaticPrefs::
          gfx_webrender_allow_partial_present_buffer_age_AtStartup()) {
    return 0;
  }
  return gl()->GetBufferAge();
}

void RenderCompositorEGL::SetBufferDamageRegion(const wr::DeviceIntRect* aRects,
                                                size_t aNumRects) {
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
