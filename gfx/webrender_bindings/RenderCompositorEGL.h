/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_RENDERCOMPOSITOR_EGL_H
#define MOZILLA_GFX_RENDERCOMPOSITOR_EGL_H

#include "GLTypes.h"
#include "nsTArray.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/webrender/RenderCompositor.h"

#if defined(GECKO_W10M)
#  include <queue>
struct ID3D11Device;
struct ID3D11Query;
#endif

namespace mozilla {

namespace layers { class FenceD3D11; }
namespace wr {

class RenderCompositorEGL : public RenderCompositor {
 public:
  static UniquePtr<RenderCompositor> Create(
      const RefPtr<widget::CompositorWidget>& aWidget, nsACString& aError);

  explicit RenderCompositorEGL(const RefPtr<widget::CompositorWidget>& aWidget,
                               RefPtr<gl::GLContext>&& aGL,
                               bool aReadbackSurface = false);
  virtual ~RenderCompositorEGL();

  bool BeginFrame() override;
  RenderedFrameId EndFrame(const nsTArray<DeviceIntRect>& aDirtyRects) final;
  void Pause() override;
  bool Resume() override;
  bool IsPaused() override;

  gl::GLContext* gl() const override { return mGL; }

  bool MakeCurrent() override;

#if defined(GECKO_W10M)
  // Here EGL *is* ANGLE, on D3D11, which is what this flag means to the rest
  // of the stack: it says the compositor can take a D3D11 texture instead of
  // a buffer of pixels. It travels to the decoder as
  // TextureFactoryIdentifier::mUseANGLE, and without it
  // KnowsCompositor::SupportsD3D11() is false and WMF turns DXVA down with
  // "Unsupported layers backend" -- which is what kept every frame on the CPU
  // even once hardware decoding was enabled and texture sharing worked.
  bool UseANGLE() const override { return true; }
  gfx::DeviceResetReason IsContextLost(bool aForce) override;
  RenderedFrameId GetLastCompletedFrameId() override;
  RenderedFrameId UpdateFrameId() override;
#else
  bool UseANGLE() const override { return false; }
#endif

  LayoutDeviceIntSize GetBufferSize() override;

#if defined(GECKO_W10M)
  bool SurfaceOriginIsTopLeft() override { return mTopLeftOrigin; }
#endif

  // Interface for partial present
  bool UsePartialPresent() override;
  bool RequestFullRender() override;
  uint32_t GetMaxPartialPresentRects() override;
  bool ShouldDrawPreviousPartialPresentRegions() override;
  size_t GetBufferAge() const override;
  void SetBufferDamageRegion(const wr::DeviceIntRect* aRects,
                             size_t aNumRects) override;

  RefPtr<layers::Fence> GetAndResetReleaseFence() override;

 protected:
  EGLSurface CreateEGLSurface();

  void DestroyEGLSurface();

  RefPtr<gl::GLContext> mGL;

  EGLSurface mEGLSurface;
#if defined(GECKO_W10M)
  bool mReadbackSurface = false;
  gfx::IntSize mReadbackSize;
  nsTArray<uint8_t> mReadbackPixels;
  uint32_t mSlowReadbackNotes = 0;
  bool PublishReadback();
  bool EnsureReadbackTarget();
  void DestroyReadbackTarget();
  GLuint mReadbackFramebuffer = 0;
  GLuint mReadbackTexture = 0;
  GLuint mReadbackDepth = 0;
  RefPtr<layers::FenceD3D11> mReadFence;
  bool mReadFenceSignaled = false;
  bool mReadFenceRequired = false;
  RefPtr<ID3D11Device> mPrivateDevice;
  uint64_t mPrivateWindowId = 0;
  bool mPrivateFailureNoted = false;
  bool mPrivateSyncFailed = false;
  RefPtr<ID3D11Device> DiagnosticDevice() const;
  void InsertFrameCompletionQuery(RenderedFrameId aFrameId);
  std::queue<std::pair<RenderedFrameId, RefPtr<ID3D11Query>>> mFrameQueries;
  RefPtr<ID3D11Query> mRecycledFrameQuery;
  RenderedFrameId mLastCompletedFrameId;
#endif

  // Whether we are in the process of handling a NEW_SURFACE error. On Android
  // this is used to allow the widget an opportunity to recover from the first
  // instance, before raising a WebRenderError on subsequent occurences.
  bool mHandlingNewSurfaceError = false;

#if defined(GECKO_W10M)
  // Partial present through ANGLE's EGL_NV_post_sub_buffer. ANGLE draws into
  // an offscreen texture of its own and keeps it from frame to frame, so
  // WebRender only has to draw what changed, and eglPostSubBufferNV copies
  // just that rectangle to the swap chain and presents it as the dirty rect.
  // Null when ANGLE does not offer it; every frame is then drawn whole.
  using PostSubBufferFn = EGLBoolean(GLAPIENTRY*)(EGLDisplay, EGLSurface,
                                                  EGLint, EGLint, EGLint,
                                                  EGLint);
  PostSubBufferFn mPostSubBuffer = nullptr;
  // Bounded diagnostics: distinguish rendering failures from Present failures
  // and verify the surface selected on the shared render-thread GL context.
  void TraceFrameRouting(const char* aStage);
  uint64_t mRoutingFrame = 0;
  uint32_t mRoutingFailures = 0;

  // A new size, or a present that failed, means the next frame is drawn
  // and presented whole.
  LayoutDeviceIntSize mPresentedSize;
  bool mFullRender = true;
  // ANGLE's own idea of the surface's size, which follows the panel and
  // changes a present later than the window's.
  gfx::IntSize SurfaceSize() const;
  gfx::IntSize mPresentedSurfaceSize;
  // What RequestFullRender decided for the frame being drawn.
  bool mWholeFrame = true;
  // The surface's rows run top down: WebRender draws into the swap chain's
  // back buffer directly, with no ANGLE offscreen texture in between.
  bool mTopLeftOrigin = false;
  // For the note every ten seconds: what presenting cost.
  TimeStamp mPresentStatsStart;
  uint32_t mPresentFrames = 0;
  uint32_t mPresentSkipped = 0;
  uint32_t mPresentWhole = 0;
  double mPresentArea = 0;
  double mPresentMs = 0;
  double mPresentMaxMs = 0;
#endif

  // FileDescriptor of release fence.
  // Release fence is a fence that is used for waiting until usage/composite of
  // AHardwareBuffer is ended. The fence is delivered to client side via
  // ImageBridge. It is used only on android.
  RefPtr<layers::Fence> mReleaseFence;
};

}  // namespace wr
}  // namespace mozilla

#endif  // MOZILLA_GFX_RENDERCOMPOSITOR_EGL_H
