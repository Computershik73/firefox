/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RenderCompositor.h"

#include "GLContext.h"
#include "gfxConfig.h"
#include "gfxPlatform.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/layers/SyncObject.h"
#include "mozilla/webrender/RenderCompositorLayerNative.h"
#include "mozilla/webrender/RenderCompositorLayersSWGL.h"
#include "mozilla/webrender/RenderCompositorOGL.h"
#include "mozilla/webrender/RenderCompositorSWGL.h"
#include "mozilla/webrender/RenderThread.h"
#include "mozilla/webrender/webrender_ffi.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/widget/CompositorWidget.h"

#if defined(GECKO_W10M)
#  include "mozilla/Sprintf.h"
#  include "nsString.h"
extern "C" void GeckoW10mNote(const char* text);
extern "C" bool gecko_w10m_widget_is_overlay(mozilla::widget::CompositorWidget* aWidget);
#endif

#ifdef XP_WIN
#  include "mozilla/webrender/RenderCompositorANGLE.h"
#  include "mozilla/widget/WinCompositorWidget.h"
#endif

#if defined(MOZ_WIDGET_ANDROID) || defined(MOZ_WIDGET_GTK) ||     defined(GECKO_W10M)
#  include "mozilla/webrender/RenderCompositorEGL.h"
#endif

#ifdef MOZ_WAYLAND
#  include "mozilla/webrender/RenderCompositorNative.h"
#endif

#ifdef XP_DARWIN
#  include "mozilla/webrender/RenderCompositorNative.h"
#endif

namespace mozilla::wr {

void wr_compositor_add_surface(void* aCompositor, wr::NativeSurfaceId aId,
                               const wr::CompositorSurfaceTransform* aTransform,
                               wr::DeviceIntRect aClipRect,
                               wr::ImageRendering aImageRendering,
                               wr::DeviceIntRect aRoundedClipRect,
                               wr::ClipRadius aRoundedClipRadius) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->AddSurface(aId, *aTransform, aClipRect, aImageRendering,
                         aRoundedClipRect, aRoundedClipRadius);
}

void wr_compositor_begin_frame(void* aCompositor) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->CompositorBeginFrame();
}

void wr_compositor_bind(void* aCompositor, wr::NativeTileId aId,
                        wr::DeviceIntPoint* aOffset, uint32_t* aFboId,
                        wr::DeviceIntRect aDirtyRect,
                        wr::DeviceIntRect aValidRect) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->Bind(aId, aOffset, aFboId, aDirtyRect, aValidRect);
}

void wr_compositor_create_surface(void* aCompositor, wr::NativeSurfaceId aId,
                                  wr::DeviceIntPoint aVirtualOffset,
                                  wr::DeviceIntSize aTileSize, bool aIsOpaque) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->CreateSurface(aId, aVirtualOffset, aTileSize, aIsOpaque);
}

void wr_compositor_create_external_surface(void* aCompositor,
                                           wr::NativeSurfaceId aId,
                                           bool aIsOpaque) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->CreateExternalSurface(aId, aIsOpaque);
}

void wr_compositor_create_swapchain_surface(void* aCompositor,
                                            wr::NativeSurfaceId aId,
                                            wr::DeviceIntSize aSize,
                                            bool aIsOpaque,
                                            bool aNeedsSyncDcompCommit) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->CreateSwapChainSurface(aId, aSize, aIsOpaque,
                                     aNeedsSyncDcompCommit);
}

void wr_compositor_resize_swapchain(void* aCompositor, wr::NativeSurfaceId aId,
                                    wr::DeviceIntSize aSize) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->ResizeSwapChainSurface(aId, aSize);
}

void wr_compositor_create_backdrop_surface(void* aCompositor,
                                           wr::NativeSurfaceId aId,
                                           wr::ColorF aColor) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->CreateBackdropSurface(aId, aColor);
}

void wr_compositor_create_tile(void* aCompositor, wr::NativeSurfaceId aId,
                               int32_t aX, int32_t aY) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->CreateTile(aId, aX, aY);
}

void wr_compositor_destroy_tile(void* aCompositor, wr::NativeSurfaceId aId,
                                int32_t aX, int32_t aY) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->DestroyTile(aId, aX, aY);
}

void wr_compositor_bind_swapchain(void* aCompositor, wr::NativeSurfaceId aId,
                                  const wr::DeviceIntRect* aDirtyRects,
                                  size_t aNumDirtyRects) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->BindSwapChain(aId, aDirtyRects, aNumDirtyRects);
}

void wr_compositor_present_swapchain(void* aCompositor, wr::NativeSurfaceId aId,
                                     const wr::DeviceIntRect* aDirtyRects,
                                     size_t aNumDirtyRects) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->PresentSwapChain(aId, aDirtyRects, aNumDirtyRects);
}

void wr_compositor_destroy_surface(void* aCompositor, NativeSurfaceId aId) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->DestroySurface(aId);
}

void wr_compositor_attach_external_image(void* aCompositor,
                                         wr::NativeSurfaceId aId,
                                         wr::ExternalImageId aExternalImage) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->AttachExternalImage(aId, aExternalImage);
}

void wr_compositor_start_compositing(void* aCompositor, wr::ColorF aClearColor,
                                     const wr::DeviceIntRect* aDirtyRects,
                                     size_t aNumDirtyRects,
                                     const wr::DeviceIntRect* aOpaqueRects,
                                     size_t aNumOpaqueRects) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->StartCompositing(aClearColor, aDirtyRects, aNumDirtyRects,
                               aOpaqueRects, aNumOpaqueRects);
}

void wr_compositor_end_frame(void* aCompositor) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->CompositorEndFrame();
}

void wr_compositor_get_capabilities(void* aCompositor,
                                    CompositorCapabilities* aCaps) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->GetCompositorCapabilities(aCaps);
}

void wr_compositor_get_window_visibility(void* aCompositor,
                                         WindowVisibility* aVisibility) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->GetWindowVisibility(aVisibility);
}

void wr_compositor_get_window_properties(void* aCompositor,
                                         WindowProperties* aProperties) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->GetWindowProperties(aProperties);
}

void wr_compositor_unbind(void* aCompositor) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->Unbind();
}

void wr_compositor_deinit(void* aCompositor) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->DeInit();
}

void wr_compositor_map_tile(void* aCompositor, wr::NativeTileId aId,
                            wr::DeviceIntRect aDirtyRect,
                            wr::DeviceIntRect aValidRect, void** aData,
                            int32_t* aStride) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->MapTile(aId, aDirtyRect, aValidRect, aData, aStride);
}

void wr_compositor_unmap_tile(void* aCompositor) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->UnmapTile();
}

void wr_partial_present_compositor_set_buffer_damage_region(
    void* aCompositor, const wr::DeviceIntRect* aRects, size_t aNRects) {
  RenderCompositor* compositor = static_cast<RenderCompositor*>(aCompositor);
  compositor->SetBufferDamageRegion(aRects, aNRects);
}

/* static */
UniquePtr<RenderCompositor> RenderCompositor::Create(
    const RefPtr<widget::CompositorWidget>& aWidget, nsACString& aError) {
  if (aWidget->GetCompositorOptions().UseSoftwareWebRender()) {
#ifdef XP_DARWIN
    // Mac uses NativeLayerCA
    if (!gfxPlatform::IsHeadless()) {
      return RenderCompositorNativeSWGL::Create(aWidget, aError);
    }
#elif defined(MOZ_WAYLAND)
    // Some widgets on Wayland (D&D popups for instance) can't use native
    // compositor due to system limitations.
    if (gfx::gfxVars::UseWebRenderCompositor() &&
        aWidget->GetCompositorOptions().AllowNativeCompositor()) {
      return RenderCompositorNativeSWGL::Create(aWidget, aError);
    }
#endif
#if defined(GECKO_W10M)
    // One XAML SwapChainPanel, and it belongs to the browser window. Menus,
    // panels and dialogs are drawn in software into the widget's own buffer
    // and laid over it by the shell; only the browser window is composited by
    // D3D11, which presents to the panel. That is the path for GPUs that stop
    // at feature level 9_3, where hardware WebRender cannot run.
    if (gecko_w10m_widget_is_overlay(aWidget.get())) {
      GeckoW10mNote("gfx: an overlay window -- software compositor over the panel");
      return RenderCompositorSWGL::Create(aWidget, aError);
    }
    GeckoW10mNote("gfx: software WebRender, trying the D3D11 compositor");
#endif
    UniquePtr<RenderCompositor> comp =
        RenderCompositorLayersSWGL::Create(aWidget, aError);
    if (comp) {
#if defined(GECKO_W10M)
      GeckoW10mNote("gfx: the D3D11 compositor is up -- tiles go to the GPU");
#endif
      return comp;
    }
#if defined(GECKO_W10M)
    {
      char note[192];
      SprintfLiteral(note,
                     "gfx: no D3D11 compositor (%s) -- software all the way",
                     nsPromiseFlatCString(aError).get());
      GeckoW10mNote(note);
    }
#endif
#if defined(MOZ_WIDGET_ANDROID)
    // On Android, we do not want to fallback from RenderCompositorOGLSWGL to
    // RenderCompositorSWGL.
    if (aWidget->GetCompositorOptions().AllowSoftwareWebRenderOGL()) {
      return nullptr;
    }
#endif
    return RenderCompositorSWGL::Create(aWidget, aError);
  }

#if defined(GECKO_W10M)
  // RenderCompositorANGLE is written around mWidget->AsWindows() -- GetHwnd,
  // GetCompositorHwnd, MakeWindowAssociation, DestroyCompositorWindow. An app
  // container is given no HWND and this widget is headless, so there is
  // nothing for it to hold on to.
  //
  // RenderCompositorEGL is the one that asks the widget for a native window
  // and leaves the rest to EGL, which is how Android and Wayland present. Here
  // EGL is ANGLE, the native window is the XAML SwapChainPanel, and ANGLE
  // makes a composition swap chain for it -- so the frame is handed to the
  // system compositor as a texture instead of being copied out through main
  // memory.
  // Menus, panels and dialogs are windows of their own. There is one panel
  // and it belongs to the browser window, so these are drawn in software --
  // into the compositor widget's own buffer, as on the software path -- and
  // the shell lays that buffer over the GPU picture.
  if (gecko_w10m_widget_is_overlay(aWidget.get())) {
    GeckoW10mNote("gfx: an overlay window -- software compositor over the panel");
    return RenderCompositorSWGL::Create(aWidget, aError);
  }
  GeckoW10mNote("gfx: making the EGL compositor");
  auto compositor = RenderCompositorEGL::Create(aWidget, aError);
  if (compositor) {
    GeckoW10mNote("gfx: the EGL compositor is up");
  } else {
    char note[192];
    SprintfLiteral(note, "gfx: the EGL compositor failed -- %s",
                   nsPromiseFlatCString(aError).get());
    GeckoW10mNote(note);
  }
  return compositor;
#elif defined(XP_WIN)
  if (gfx::gfxVars::UseWebRenderANGLE()) {
    return RenderCompositorANGLE::Create(aWidget, aError);
  }
#endif

#if defined(MOZ_WAYLAND)
  if (gfx::gfxVars::UseWebRenderCompositor() &&
      aWidget->GetCompositorOptions().AllowNativeCompositor()) {
    if (StaticPrefs::gfx_webrender_layer_compositor()) {
      return RenderCompositorLayerNativeOGL::Create(aWidget, aError);
    } else {
      return RenderCompositorNativeOGL::Create(aWidget, aError);
    }
  }
#endif

#if defined(MOZ_WIDGET_ANDROID) || defined(MOZ_WIDGET_GTK)
  UniquePtr<RenderCompositor> eglCompositor =
      RenderCompositorEGL::Create(aWidget, aError);
  if (eglCompositor) {
    return eglCompositor;
  }
#endif

#if defined(MOZ_WIDGET_ANDROID)
  // RenderCompositorOGL is not used on android
  return nullptr;
#elif defined(XP_DARWIN)
  if (gfx::gfxVars::UseWebRenderCompositor() &&
      aWidget->GetCompositorOptions().AllowNativeCompositor()) {
    if (StaticPrefs::gfx_webrender_layer_compositor()) {
      UniquePtr<RenderCompositor> compositor =
          RenderCompositorLayerNativeOGL::Create(aWidget, aError);
      if (compositor) {
        return compositor;
      }
    }
  }
  // Mac uses NativeLayerCA
  return RenderCompositorNativeOGL::Create(aWidget, aError);
#else
  return RenderCompositorOGL::Create(aWidget, aError);
#endif
}

RenderCompositor::RenderCompositor(
    const RefPtr<widget::CompositorWidget>& aWidget)
    : mWidget(aWidget) {}

RenderCompositor::~RenderCompositor() = default;

bool RenderCompositor::MakeCurrent() { return gl()->MakeCurrent(); }

void RenderCompositor::GetCompositorCapabilities(
    CompositorCapabilities* aCaps) {
  if (StaticPrefs::gfx_webrender_compositor_max_update_rects_AtStartup() > 0) {
    aCaps->max_update_rects = 1;
  } else {
    aCaps->max_update_rects = 0;
  }
}

void RenderCompositor::GetWindowVisibility(WindowVisibility* aVisibility) {
#ifdef XP_WIN
  auto* widget = mWidget->AsWindows();
  if (!widget) {
    return;
  }
  aVisibility->is_fully_occluded = widget->GetWindowIsFullyOccluded();
#endif
}

void RenderCompositor::GetWindowProperties(WindowProperties* aProperties) {
  aProperties->is_opaque = true;
}

gfx::DeviceResetReason RenderCompositor::IsContextLost(bool aForce) {
  auto* glc = gl();
  // GetGraphicsResetStatus may trigger an implicit MakeCurrent if robustness
  // is not supported, so unless we are forcing, pass on the check.
  if (!glc || (!aForce && !glc->IsSupported(gl::GLFeature::robustness))) {
    return gfx::DeviceResetReason::OK;
  }
  auto resetStatus = glc->fGetGraphicsResetStatus();
  switch (resetStatus) {
    case LOCAL_GL_NO_ERROR:
      return gfx::DeviceResetReason::OK;
    case LOCAL_GL_INNOCENT_CONTEXT_RESET_ARB:
      return gfx::DeviceResetReason::DRIVER_ERROR;
    case LOCAL_GL_PURGED_CONTEXT_RESET_NV:
      return gfx::DeviceResetReason::NVIDIA_VIDEO;
    case LOCAL_GL_GUILTY_CONTEXT_RESET_ARB:
      return gfx::DeviceResetReason::RESET;
    case LOCAL_GL_UNKNOWN_CONTEXT_RESET_ARB:
      return gfx::DeviceResetReason::UNKNOWN;
    default:
      gfxCriticalError() << "Device reset with WR context unexpected status: "
                         << gfx::hexa(resetStatus);
      break;
  }
  return gfx::DeviceResetReason::OTHER;
}

}  // namespace mozilla::wr
