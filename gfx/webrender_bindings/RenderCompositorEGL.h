/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_RENDERCOMPOSITOR_EGL_H
#define MOZILLA_GFX_RENDERCOMPOSITOR_EGL_H

#include "GLTypes.h"
#include "mozilla/webrender/RenderCompositor.h"

namespace mozilla {

namespace wr {

class RenderCompositorEGL : public RenderCompositor {
 public:
  static UniquePtr<RenderCompositor> Create(
      const RefPtr<widget::CompositorWidget>& aWidget, nsACString& aError);

  explicit RenderCompositorEGL(const RefPtr<widget::CompositorWidget>& aWidget,
                               RefPtr<gl::GLContext>&& aGL);
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
#else
  bool UseANGLE() const override { return false; }
#endif

  LayoutDeviceIntSize GetBufferSize() override;

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

  // Whether we are in the process of handling a NEW_SURFACE error. On Android
  // this is used to allow the widget an opportunity to recover from the first
  // instance, before raising a WebRenderError on subsequent occurences.
  bool mHandlingNewSurfaceError = false;

  // FileDescriptor of release fence.
  // Release fence is a fence that is used for waiting until usage/composite of
  // AHardwareBuffer is ended. The fence is delivered to client side via
  // ImageBridge. It is used only on android.
  RefPtr<layers::Fence> mReleaseFence;
};

}  // namespace wr
}  // namespace mozilla

#endif  // MOZILLA_GFX_RENDERCOMPOSITOR_EGL_H
