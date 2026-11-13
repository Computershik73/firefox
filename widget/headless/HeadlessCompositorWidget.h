/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_headless_HeadlessCompositorWidget_h
#define widget_headless_HeadlessCompositorWidget_h

#include "HeadlessWidget.h"
#include "mozilla/widget/CompositorWidget.h"

#if defined(GECKO_W10M)
#  include "mozilla/Atomics.h"
#  include "mozilla/UniquePtr.h"
#endif

namespace mozilla {
namespace widget {

class HeadlessCompositorWidgetInitData;

class HeadlessCompositorWidget final : public CompositorWidget,
                                       public CompositorWidgetDelegate {
 public:
  HeadlessCompositorWidget(const HeadlessCompositorWidgetInitData& aInitData,
                           const layers::CompositorOptions& aOptions,
                           HeadlessWidget* aWindow);
#if defined(GECKO_W10M)
  ~HeadlessCompositorWidget();
#endif

  void NotifyClientSizeChanged(const LayoutDeviceIntSize& aClientSize);
#if defined(GECKO_W10M)
  // Where this window sits on the headless screen. A menu is a window of its
  // own and knows only its own corner, so this is what says where to put it.
  void NotifyOriginChanged(const LayoutDeviceIntPoint& aOrigin);
  // A menu that has been dismissed still has its last frame; without this it
  // would keep being drawn over the window it was dismissed from.
  void NotifyVisibilityChanged(bool aVisible);
  static void ComposeOverlaysOnly();
  // Something is standing over the browser window right now.
  bool IsOverlayShowing() const { return mIsOverlay && mVisible; }
  // Anything that is not the browser window: menus, panels, dialogs. On the
  // hardware path these are drawn in software and composited by the shell
  // over the GPU picture, so RenderCompositor::Create asks.
  bool IsOverlay() const { return mIsOverlay; }
  bool IsExternalWindow() const { return mIsExternalWindow; }
  bool RequiresSoftwareComposition() const;
  void SetExternalWindow(bool aExternal);
  static int32_t CopyExternalFrame(void* aDest, int32_t aCapacity,
                                   int32_t* aWidth, int32_t* aHeight,
                                   uint64_t* aSerial);
  static bool HasExternalWindow();
  static bool IsSoftwareMainPresenting();
  // Registry ABI v1: IDs are never reused. Binding is immutable for the
  // compositor lifetime so an EGL context cannot accidentally change panels.
  static uint32_t ListWindowIds(uint64_t* aIds, uint32_t aCapacity);
  static bool BindWindowPanel(uint64_t aId, void* aPanel);
  static bool PrepareWindowPanel(uint64_t aId, void* aPanel);
  static void* PanelForWidgetKey(uintptr_t aKey, bool* aManaged = nullptr);
  static void ForgetWindowPanel(uint64_t aId);
  static void ManagedFramePresented(uintptr_t aKey);
  static void PublishManagedPixels(uintptr_t aKey, int32_t aWidth,
                                   int32_t aHeight, const uint8_t* aPixels);
  static int32_t CopyManagedFrame(uint64_t aId, void* aDest, int32_t aCapacity,
      int32_t* aWidth, int32_t* aHeight, uint64_t* aSerial, int32_t* aSoftware);

#endif

  // CompositorWidget Overrides

  uintptr_t GetWidgetKey() override;

  LayoutDeviceIntSize GetClientSize() override;

  nsIWidget* RealWidget() override;
  CompositorWidgetDelegate* AsDelegate() override { return this; }

  void ObserveVsync(VsyncObserver* aObserver) override;

  // CompositorWidgetDelegate Overrides

  HeadlessCompositorWidget* AsHeadlessCompositorWidget() override {
    return this;
  }

#if defined(GECKO_W10M)
  // A headless compositor widget hands the software WebRender compositor no
  // surface at all, so everything it paints goes nowhere. On Windows 10 Mobile
  // there is no HWND for app content and headless is the only widget this port
  // can create, which makes these two methods the whole of the path from Gecko
  // to the screen.
  already_AddRefed<gfx::DrawTarget> StartRemoteDrawingInRegion(
      const LayoutDeviceIntRegion& aInvalidRegion) override;
  void EndRemoteDrawingInRegion(
      gfx::DrawTarget* aDrawTarget,
      const LayoutDeviceIntRegion& aInvalidRegion) override;
#endif

 private:
  HeadlessWidget* mWidget;

  // See GtkCompositorWidget for the justification for this mutex.
  DataMutex<LayoutDeviceIntSize> mClientSize;

#if defined(GECKO_W10M)
  // Every window paints into its own buffer. With one buffer between them, a
  // menu and the window underneath it took turns writing the same pixels and
  // the shell showed whatever was there when it looked -- which is what the
  // streaks across the screen were.
  uint64_t mWindowId = 0;
  UniquePtr<uint8_t[]> mPixels;
  size_t mCapacity = 0;
  int32_t mWidth = 0;
  int32_t mHeight = 0;

  // Popups are drawn over the window rather than instead of it.
  // Anything that is not the browser window itself -- menus, dialogs, the
  // graphics sanity test's window -- is drawn over it rather than instead
  // of it. About Nightly is a dialog, and treating it as a window of its
  // own is what put it and the browser in a fight over the same frame.
  bool mIsOverlay = false;
  bool mIsExternalWindow = false;
  bool mIsManagedWindow = false;
  int32_t mManagedFrameSource = 0;  // 0 pending, 1 software, 2 GPU
  uint64_t mExternalSerial = 0;
  // The size this window's buffer was last fully painted at. Until it matches
  // the current size, the buffer holds the previous picture at the previous
  // row length and must not be composited.
  int32_t mPaintedWidth = 0;
  int32_t mPaintedHeight = 0;
  // StartRemoteDrawing exposes mPixels directly to Skia. Keep the shared
  // frame lock for the complete paint, not just while dimensions are changed,
  // so the UWP host can never copy or observe a buffer while it is being
  // reallocated/written.
  bool mFrameLockHeldForDrawing = false;
  mozilla::Atomic<bool> mVisible{true};
  DataMutex<LayoutDeviceIntPoint> mOrigin;
#endif
};

}  // namespace widget
}  // namespace mozilla

#endif  // widget_headless_HeadlessCompositor_h
