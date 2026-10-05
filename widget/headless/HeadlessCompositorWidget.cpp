/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HeadlessCompositorWidget.h"
#include "HeadlessScreenHelper.h"
#include "nsPrintfCString.h"
#if defined(GECKO_W10M)
#  include "nsCommandLine.h"
#  include "nsICommandLine.h"
#  include "nsICommandLineRunner.h"
#  include "nsIWindowMediator.h"
#  include "mozIDOMWindow.h"
#endif

#include "VsyncDispatcher.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "mozilla/gfx/DeviceManagerDx.h"
#include "mozilla/Services.h"
#include "nsIObserverService.h"
#include "nsIObserver.h"
#include "nsMemoryPressure.h"

#if defined(GECKO_W10M)
#  include <string.h>

#  include <algorithm>
#  include <vector>

#  include "mozilla/Atomics.h"
#  include "mozilla/StaticMutex.h"
#  include "mozilla/gfx/2D.h"
#  include "mozilla/gfx/Types.h"
#  include "mozilla/ClearOnShutdown.h"
#  include "mozilla/Sprintf.h"
#  include "mozilla/StaticPtr.h"
#  include "mozilla/TextEventDispatcherListener.h"
#  include "mozilla/TextEventDispatcher.h"
#  include "mozilla/TextEvents.h"
#  include "nsIWidget.h"
#  include "nsString.h"
#  include "nsThreadUtils.h"

extern "C" void GeckoW10mNote(const char* text);

namespace {

// The reason typed characters never arrived. A transaction has to be opened on
// the dispatcher before anything can be sent through it, and the native kind
// asks the widget for a listener -- which every real platform provides and a
// headless window does not, so BeginNativeInputTransaction refused every time
// and the text was dropped in silence.
//
// This is that listener, modelled on the Windows one, which is also a
// singleton because there is one input context in the process. There is no
// native input method here to be told anything, so the notifications are
// accepted and dropped; what it exists for is to let a transaction begin.
//
// The dispatcher holds it weakly, so something else has to hold it strongly,
// which is what the static reference is for.
class GeckoW10mTextListener final
    : public mozilla::widget::TextEventDispatcherListener {
 public:
  NS_DECL_ISUPPORTS

  NS_IMETHOD NotifyIME(mozilla::widget::TextEventDispatcher*,
                       const mozilla::widget::IMENotification&) override {
    return NS_OK;
  }
  NS_IMETHOD_(mozilla::widget::IMENotificationRequests)
  GetIMENotificationRequests() override {
    return mozilla::widget::IMENotificationRequests();
  }
  NS_IMETHOD_(void)
  OnRemovedFrom(mozilla::widget::TextEventDispatcher*) override {}
  NS_IMETHOD_(void)
  WillDispatchKeyboardEvent(mozilla::widget::TextEventDispatcher*,
                            mozilla::WidgetKeyboardEvent&, uint32_t,
                            void*) override {}

 private:
  ~GeckoW10mTextListener() = default;
};

NS_IMPL_ISUPPORTS(GeckoW10mTextListener,
                  mozilla::widget::TextEventDispatcherListener,
                  nsISupportsWeakReference)

mozilla::StaticRefPtr<GeckoW10mTextListener> sTextListener;

GeckoW10mTextListener* TextListener() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!sTextListener) {
    sTextListener = new GeckoW10mTextListener();
    mozilla::ClearOnShutdown(&sTextListener);
  }
  return sTextListener.get();
}


// What the shell reads. The windows paint into their own buffers and the
// finished picture is assembled here, so the shell never sees a half-drawn
// frame and the lock is never held for the length of a paint.
mozilla::StaticMutex sFrameLock MOZ_UNANNOTATED;
mozilla::UniquePtr<uint8_t[]> sFront;
size_t sFrontCapacity = 0;
int32_t sFrontWidth = 0;
int32_t sFrontHeight = 0;
uint64_t sSerial = 0;

// Every live window, and which of them is the one being shown. Guarded by the
// same lock; the compositor is a single thread, so this is only ever contended
// with the shell reading a frame.
std::vector<mozilla::widget::HeadlessCompositorWidget*> sWindows;
mozilla::widget::HeadlessCompositorWidget* sMain = nullptr;
// The browser window's compositor widget whether or not it ever painted in
// software. On the hardware path it never does -- WebRender presents it
// through the panel -- but the overlays still need its size and origin to be
// composited over it.
mozilla::widget::HeadlessCompositorWidget* sMainWidget = nullptr;

}  // namespace
#endif

namespace mozilla {
namespace widget {

HeadlessCompositorWidget::HeadlessCompositorWidget(
    const HeadlessCompositorWidgetInitData& aInitData,
    const layers::CompositorOptions& aOptions, HeadlessWidget* aWindow)
    : CompositorWidget(aOptions),
      mWidget(aWindow),
      mClientSize(LayoutDeviceIntSize(aInitData.InitialClientSize()),
                  "HeadlessCompositorWidget::mClientSize")
#if defined(GECKO_W10M)
      ,
      mOrigin(aWindow->GetBounds().TopLeft(),
              "HeadlessCompositorWidget::mOrigin")
#endif
{
#if defined(GECKO_W10M)
  mIsOverlay = aWindow->GetWindowType() != WindowType::TopLevel;
  StaticMutexAutoLock lock(sFrameLock);
  sWindows.push_back(this);
  if (!mIsOverlay) {
    sMainWidget = this;
  }
#endif
}

#if defined(GECKO_W10M)
HeadlessCompositorWidget::~HeadlessCompositorWidget() {
  StaticMutexAutoLock lock(sFrameLock);
  if (sMainWidget == this) {
    sMainWidget = nullptr;
  }
  for (auto it = sWindows.begin(); it != sWindows.end(); ++it) {
    if (*it == this) {
      sWindows.erase(it);
      break;
    }
  }
  if (sMain == this) {
    sMain = nullptr;
  }
}
#endif

void HeadlessCompositorWidget::ObserveVsync(VsyncObserver* aObserver) {
  if (RefPtr<CompositorVsyncDispatcher> cvd =
          mWidget->GetCompositorVsyncDispatcher()) {
    cvd->SetCompositorVsyncObserver(aObserver);
  }
}

nsIWidget* HeadlessCompositorWidget::RealWidget() { return mWidget; }

void HeadlessCompositorWidget::NotifyClientSizeChanged(
    const LayoutDeviceIntSize& aClientSize) {
  auto size = mClientSize.Lock();
  *size = aClientSize;
}

#if defined(GECKO_W10M)
void HeadlessCompositorWidget::NotifyOriginChanged(
    const LayoutDeviceIntPoint& aOrigin) {
  auto origin = mOrigin.Lock();
  *origin = aOrigin;
}

void HeadlessCompositorWidget::NotifyVisibilityChanged(bool aVisible) {
  mVisible = aVisible;
  StaticMutexAutoLock lock(sFrameLock);
  if (!mIsOverlay) {
    if (aVisible) {
      sMainWidget = this;
    }
    return;
  }
  // A menu that closed has to come off the picture, and on the hardware path
  // nobody repaints the window underneath to take it off -- so the overlay
  // layer is rebuilt here, empty if this was the last one.
  if (!sMain) {
    ComposeOverlaysOnly();
  }
}

// The hardware path: the browser window is on the panel, the overlays are
// painted in software, and the shell draws this layer -- transparent wherever
// there is no overlay -- over the panel. Same blend as the software path.
void HeadlessCompositorWidget::ComposeOverlaysOnly() {
  if (!sMainWidget) {
    return;
  }
  const LayoutDeviceIntSize size = [] {
    auto s = sMainWidget->mClientSize.Lock();
    return *s;
  }();
  if (size.width <= 0 || size.height <= 0) {
    return;
  }
  const size_t stride = size_t(size.width) * 4;
  const size_t bytes = stride * size_t(size.height);
  if (sFrontCapacity < bytes) {
    sFront = MakeUnique<uint8_t[]>(bytes);
    sFrontCapacity = sFront ? bytes : 0;
    if (!sFront) {
      return;
    }
  }
  sFrontWidth = size.width;
  sFrontHeight = size.height;
  memset(sFront.get(), 0, bytes);

  LayoutDeviceIntPoint base;
  {
    auto origin = sMainWidget->mOrigin.Lock();
    base = *origin;
  }
  for (auto* window : sWindows) {
    if (!window->mIsOverlay || !window->mVisible || window->mWidth <= 0 ||
        !window->mPixels) {
      continue;
    }
    if (window->mPaintedWidth != window->mWidth ||
        window->mPaintedHeight != window->mHeight) {
      continue;
    }
    LayoutDeviceIntPoint at;
    {
      auto origin = window->mOrigin.Lock();
      at = *origin;
    }
    const int32_t dx = at.x - base.x;
    const int32_t dy = at.y - base.y;
    const size_t sourceStride = size_t(window->mWidth) * 4;
    for (int32_t row = 0; row < window->mHeight; ++row) {
      const int32_t y = dy + row;
      if (y < 0 || y >= sFrontHeight) {
        continue;
      }
      const int32_t from = dx < 0 ? -dx : 0;
      const int32_t count =
          std::min(window->mWidth - from, sFrontWidth - (dx + from));
      if (count <= 0) {
        continue;
      }
      uint8_t* dest = sFront.get() + size_t(y) * stride + size_t(dx + from) * 4;
      const uint8_t* src = window->mPixels.get() + size_t(row) * sourceStride +
                           size_t(from) * 4;
      for (int32_t i = 0; i < count; ++i, dest += 4, src += 4) {
        const uint32_t alpha = src[3];
        if (alpha == 255) {
          memcpy(dest, src, 4);
          continue;
        }
        if (alpha == 0) {
          continue;
        }
        const uint32_t inverse = 255 - alpha;
        for (int c = 0; c < 4; ++c) {
          dest[c] = uint8_t(src[c] + ((dest[c] * inverse + 127) / 255));
        }
      }
    }
  }
  ++sSerial;
}
#endif

LayoutDeviceIntSize HeadlessCompositorWidget::GetClientSize() {
  auto size = mClientSize.Lock();
  return *size;
}

uintptr_t HeadlessCompositorWidget::GetWidgetKey() {
  return reinterpret_cast<uintptr_t>(mWidget);
}

#if defined(GECKO_W10M)
already_AddRefed<gfx::DrawTarget>
HeadlessCompositorWidget::StartRemoteDrawingInRegion(
    const LayoutDeviceIntRegion& aInvalidRegion) {
  const LayoutDeviceIntSize size = GetClientSize();
  if (size.width <= 0 || size.height <= 0) {
    return nullptr;
  }

  const size_t stride = size_t(size.width) * 4;
  const size_t bytes = stride * size_t(size.height);

  if (mCapacity < bytes) {
    // Zeroed, so that a buffer which is never painted is transparent rather
    // than whatever the allocator happened to hand over.
    mPixels = MakeUnique<uint8_t[]>(bytes);
    mCapacity = mPixels ? bytes : 0;
    if (!mPixels) {
      return nullptr;
    }
    memset(mPixels.get(), 0, bytes);
  }

  // The size becomes visible to the compositing loop here, before a single
  // pixel of it has been drawn -- so the loop is told to leave this window
  // alone until a paint at this size has finished. Without that, a menu caught
  // between starting and finishing was composited from a buffer still holding
  // the last paint, at the last paint's row length, and came out as a black
  // rectangle over the page. It only happened when the timing fell that way,
  // which is why it happened every other time.
  {
    StaticMutexAutoLock lock(sFrameLock);
    mWidth = size.width;
    mHeight = size.height;
  }

  static bool sSaidSo = false;
  if (!sSaidSo) {
    sSaidSo = true;
    GeckoW10mNote("frames: the compositor is drawing into the shell buffer");
  }

  RefPtr<gfx::DrawTarget> dt = gfx::Factory::CreateDrawTargetForData(
      gfx::BackendType::SKIA, mPixels.get(),
      gfx::IntSize(size.width, size.height), int32_t(stride),
      gfx::SurfaceFormat::B8G8R8A8);
  return dt.forget();
}

void HeadlessCompositorWidget::EndRemoteDrawingInRegion(
    gfx::DrawTarget* aDrawTarget, const LayoutDeviceIntRegion& aInvalidRegion) {
  if (aDrawTarget) {
    aDrawTarget->Flush();
  }

  StaticMutexAutoLock lock(sFrameLock);

  // This window now holds a complete picture of itself at its current size.
  mPaintedWidth = mWidth;
  mPaintedHeight = mHeight;

  if (!mIsOverlay) {
    sMain = this;
  }
  if (!sMain && mIsOverlay) {
    // Hardware path: the window underneath is on the panel, not in here.
    ComposeOverlaysOnly();
    return;
  }
  if (!sMain || sMain->mWidth <= 0 || sMain->mHeight <= 0 || !sMain->mPixels) {
    return;
  }

  // Assemble the whole picture every time, whichever window painted. A menu
  // that painted on its own would otherwise be erased the next time the window
  // beneath it painted, and a window that painted on its own would leave the
  // menu behind as a stale patch.
  const size_t stride = size_t(sMain->mWidth) * 4;
  const size_t bytes = stride * size_t(sMain->mHeight);
  if (sFrontCapacity < bytes) {
    sFront = MakeUnique<uint8_t[]>(bytes);
    sFrontCapacity = sFront ? bytes : 0;
    if (!sFront) {
      return;
    }
  }
  sFrontWidth = sMain->mWidth;
  sFrontHeight = sMain->mHeight;
  memcpy(sFront.get(), sMain->mPixels.get(), bytes);

  LayoutDeviceIntPoint base;
  {
    auto origin = sMain->mOrigin.Lock();
    base = *origin;
  }

  for (auto* window : sWindows) {
    if (window == sMain || !window->mIsOverlay || !window->mVisible ||
        window->mWidth <= 0 || !window->mPixels) {
      continue;
    }
    // Resized but not yet redrawn: what is in the buffer belongs to the old
    // size and would be read with the wrong row length.
    if (window->mPaintedWidth != window->mWidth ||
        window->mPaintedHeight != window->mHeight) {
      continue;
    }

    LayoutDeviceIntPoint at;
    {
      auto origin = window->mOrigin.Lock();
      at = *origin;
    }
    const int32_t dx = at.x - base.x;
    const int32_t dy = at.y - base.y;
    const size_t sourceStride = size_t(window->mWidth) * 4;

    // Blended, not copied. An overlay's buffer is only as painted as its
    // window is: a menu paints its panel and leaves the rest of the buffer
    // alone, and those bytes are zeros. Copied straight down, a zero pixel is
    // opaque black, which is what the address bar's drop-down turned into --
    // a black rectangle over the page. Composited over, a zero pixel is
    // nothing at all and the window beneath shows through.
    //
    // WebRender hands these over premultiplied, so this is the plain
    // source-over: dest = src + dest * (1 - alpha).
    for (int32_t row = 0; row < window->mHeight; ++row) {
      const int32_t y = dy + row;
      if (y < 0 || y >= sFrontHeight) {
        continue;
      }
      const int32_t from = dx < 0 ? -dx : 0;
      const int32_t count =
          std::min(window->mWidth - from, sFrontWidth - (dx + from));
      if (count <= 0) {
        continue;
      }
      uint8_t* dest = sFront.get() + size_t(y) * stride + size_t(dx + from) * 4;
      const uint8_t* src = window->mPixels.get() + size_t(row) * sourceStride +
                           size_t(from) * 4;
      for (int32_t i = 0; i < count; ++i, dest += 4, src += 4) {
        const uint32_t alpha = src[3];
        if (alpha == 255) {
          memcpy(dest, src, 4);
          continue;
        }
        if (alpha == 0) {
          continue;
        }
        const uint32_t inverse = 255 - alpha;
        for (int c = 0; c < 4; ++c) {
          dest[c] = uint8_t(src[c] + ((dest[c] * inverse + 127) / 255));
        }
      }
    }
  }

  ++sSerial;
}
#endif

}  // namespace widget
}  // namespace mozilla

#if defined(GECKO_W10M)
// Read by the shell, which has no other way to see what Gecko drew. Frames are
// BGRA, top row first, tightly packed. A caller that already holds the serial
// it last saw gets nothing copied and a zero back, so an idle engine costs the
// UI thread a lock and a comparison.
extern "C" __declspec(dllexport) uint64_t gecko_w10m_frame_serial() {
  mozilla::StaticMutexAutoLock lock(sFrameLock);
  return sSerial;
}

// Input goes the other way, and takes the same route a synthesized event takes
// Input goes on the input queue, not the ordinary one.
//
// Gecko's own input events are dispatched at InputHigh so they overtake
// timers, page script and everything else the main thread has queued. These
// were going through NS_DispatchToMainThread, which puts them behind all of
// it -- which is how taps came to be swallowed while a video was playing, and
// why a burst of them used to arrive at once when a page finished loading.
static void GeckoW10mDispatchInput(already_AddRefed<nsIRunnable> aRunnable) {
  NS_DispatchToMainThreadQueue(std::move(aRunnable),
                               mozilla::EventQueuePriority::InputHigh);
}

// in a test: HeadlessWidget builds a real WidgetMouseEvent or WidgetWheelEvent
// and puts it through DispatchInputEvent. The shell calls from its UI thread,
// so each one is handed to the main thread rather than dispatched in place.
//
// Message: 0 move, 1 button down, 2 button up. Coordinates are in the pixels of
// the frame the shell last drew, which is the window.
extern "C" __declspec(dllexport) void gecko_w10m_input_mouse(int32_t aMessage,
                                                          int32_t aX,
                                                          int32_t aY) {
  GeckoW10mDispatchInput(
      NS_NewRunnableFunction("gecko_w10m_input_mouse", [aMessage, aX, aY]() {
        auto* widget = mozilla::widget::HeadlessWidget::ShownWidget();
        if (!widget) {
          return;
        }
        using NativeMouseMessage = nsIWidget::NativeMouseMessage;
        NativeMouseMessage message = NativeMouseMessage::Move;
        if (aMessage == 1) {
          message = NativeMouseMessage::ButtonDown;
        } else if (aMessage == 2) {
          message = NativeMouseMessage::ButtonUp;
        }
        widget->SynthesizeNativeMouseEvent(
            mozilla::LayoutDeviceIntPoint(aX, aY), message,
            aMessage == 0 ? mozilla::MouseButton::eNotPressed
                          : mozilla::MouseButton::ePrimary,
            nsIWidget::NativeModifiers::NO_MODIFIERS, nullptr);
      }));
}

// Anything the browser itself cannot open -- mailto:, tel:, ms-settings: --
// belongs to the system, and in an app container only the shell can ask for
// it. The shell leaves a function here; the external protocol handler uses
// it instead of ShellExecute, which the container refuses.
static void (*gGeckoW10mUriLauncher)(const char*) = nullptr;

extern "C" __declspec(dllexport) void gecko_w10m_set_uri_launcher(
    void (*aLauncher)(const char*)) {
  gGeckoW10mUriLauncher = aLauncher;
  GeckoW10mNote(aLauncher ? "launcher: the shell can open system URIs"
                        : "launcher: cleared");
}

// Called from the external protocol handler. True when the shell took it.
extern "C" bool gecko_w10m_launch_uri(const char* aUri) {
  if (!gGeckoW10mUriLauncher || !aUri || !*aUri) {
    return false;
  }
  nsPrintfCString note("launcher: handing %s to the system", aUri);
  GeckoW10mNote(note.get());
  gGeckoW10mUriLauncher(aUri);
  return true;
}

extern "C" bool gecko_w10m_has_uri_launcher() {
  return gGeckoW10mUriLauncher != nullptr;
}

// Fullscreen. The window's size belongs to the shell -- it is a XAML panel,
// and the swap chain behind it is resized by the shell and nobody else. So
// the engine does not resize itself when a page asks for fullscreen; it says
// so here, and the shell hides the status bar and the navigation bar. The
// room that frees up arrives back through gecko_w10m_resize like any other
// change of size, and the window and the surface stay the same shape all the
// way through.
static void (*gGeckoW10mFullscreenSink)(int) = nullptr;

extern "C" __declspec(dllexport) void gecko_w10m_set_fullscreen_sink(
    void (*aSink)(int)) {
  gGeckoW10mFullscreenSink = aSink;
  GeckoW10mNote(aSink ? "fullscreen: the shell is listening"
                    : "fullscreen: sink cleared");
}

extern "C" void GeckoW10mFullscreenChanged(bool aFullscreen) {
  GeckoW10mNote(aFullscreen ? "fullscreen: on -- asking the shell for the whole "
                            "screen"
                          : "fullscreen: off -- the shell can have its bars "
                            "back");
  if (gGeckoW10mFullscreenSink) {
    gGeckoW10mFullscreenSink(aFullscreen ? 1 : 0);
  }
}

// Opens a URL the way another instance of Firefox would hand one over: a
// remote command line, which the browser's own handler turns into a new tab
// in the window that is already open. This is what makes GeckoW10m usable as
// the system's browser -- the shell is handed a URL by the phone and passes
// it here.
//
// Returns 1 when the URL was taken, 0 when the browser is not ready for it
// yet (no window), so the shell can ask again.
extern "C" __declspec(dllexport) int32_t gecko_w10m_open_url(const char* aUrl) {
  if (!aUrl || !*aUrl) {
    return 1;  // nothing to do, and nothing to retry
  }
  if (!NS_IsMainThread()) {
    // Only the main thread may touch the command line, and the answer is
    // wanted synchronously, so the caller is told to come back.
    nsCString url(aUrl);
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "gecko_w10m_open_url", [url]() { gecko_w10m_open_url(url.get()); }));
    return 1;
  }

  // Until a browser window exists there is nothing to open a tab in, and the
  // handler would make a second window rather than a tab.
  nsCOMPtr<nsIWindowMediator> mediator =
      do_GetService(NS_WINDOWMEDIATOR_CONTRACTID);
  if (!mediator) {
    return 0;
  }
  nsCOMPtr<mozIDOMWindowProxy> window;
  mediator->GetMostRecentWindow(u"navigator:browser",
                                getter_AddRefs(window));
  if (!window) {
    return 0;
  }

  nsCString url(aUrl);
  const char* argv[] = {"", "-url", url.get()};
  nsCOMPtr<nsICommandLineRunner> cmdline(new nsCommandLine());
  nsresult rv = cmdline->Init(3, argv, nullptr,
                              nsICommandLine::STATE_REMOTE_AUTO);
  if (NS_SUCCEEDED(rv)) {
    rv = cmdline->Run();
  }
  nsPrintfCString note("open: %s -> 0x%08x", url.get(),
                       static_cast<uint32_t>(rv));
  GeckoW10mNote(note.get());
  return NS_SUCCEEDED(rv) ? 1 : 0;
}

// The display, in device pixels, whichever way the phone is held.
// The display's density, which decides how far a finger may wander and still
// be a tap. Without it APZ measures against 96 dpi.
extern "C" __declspec(dllexport) void gecko_w10m_set_dpi(int32_t aDpi) {
  // An integer. The first version took a float and the engine never saw the
  // call at all -- the shell is built with MSVC and xul with clang, and a
  // float across that boundary on ARM32 is the one thing in this port that has
  // never worked. Whole dots per inch is precision enough for a finger.
  NS_DispatchToMainThread(NS_NewRunnableFunction("gecko_w10m_set_dpi", [aDpi]() {
    mozilla::widget::HeadlessWidget::GeckoW10mSetDpi(aDpi);
  }));
}

// The shell's life on a phone, told to the engine the way Android tells
// GeckoView: "application-background" when the system is about to suspend
// the app -- which on this platform is also how it is ended, so this is the
// last chance to write anything down -- and "application-foreground" when
// it comes back. Chrome script listens and flushes what Firefox otherwise
// saves on a shutdown that never comes here.
extern "C" __declspec(dllexport) void gecko_w10m_app_state(int32_t aForeground) {
  NS_DispatchToMainThread(
      NS_NewRunnableFunction("gecko_w10m_app_state", [aForeground]() {
        const char* topic =
            aForeground ? "application-foreground" : "application-background";
        GeckoW10mNote(aForeground ? "app: foreground -- telling chrome"
                                : "app: background -- telling chrome to flush");
        if (nsCOMPtr<nsIObserverService> obs =
                mozilla::services::GetObserverService()) {
          obs->NotifyObservers(nullptr, topic, nullptr);
        }
      }));
}

// The phone says memory is tight. It says so per app, against a ceiling it
// set from the device's class, and past the ceiling the app is simply gone;
// the engine's own low-memory machinery is what drops caches, minimises the
// heap and unloads background tabs, and this is how it hears.
// A message channel between chrome script and the shell, for what the
// engine cannot do and the shell can: the phone's PlayReady, for one.
//
// Chrome script sends by notifying "gecko-w10m-bridge" with a JSON string as
// the data; the observer below hands it to the shell as UTF-8. The shell
// answers through gecko_w10m_bridge_reply, which notifies "gecko-w10m-bridge-reply"
// on the main thread with the JSON it was given. Both directions are plain
// strings, so no IDL, no component, no build change beyond this file; binary
// payloads travel as base64 inside the JSON.
static void (*gGeckoW10mBridgeSink)(const char*) = nullptr;

namespace {
class GeckoW10mBridgeObserver final : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_IMETHOD Observe(nsISupports*, const char* aTopic,
                     const char16_t* aData) override {
    if (gGeckoW10mBridgeSink && aData) {
      NS_ConvertUTF16toUTF8 utf8(aData);
      gGeckoW10mBridgeSink(utf8.get());
    }
    return NS_OK;
  }

 private:
  ~GeckoW10mBridgeObserver() = default;
};
NS_IMPL_ISUPPORTS(GeckoW10mBridgeObserver, nsIObserver)
}  // namespace

// Returns 1 once the observer is armed on the main thread, 0 if that could
// not even be scheduled yet (XPCOM not up); the shell retries.
extern "C" __declspec(dllexport) int32_t gecko_w10m_set_bridge_sink(
    void (*aSink)(const char*)) {
  gGeckoW10mBridgeSink = aSink;
  static bool sArmed = false;
  if (sArmed) {
    return 1;
  }
  nsresult rv = NS_DispatchToMainThread(
      NS_NewRunnableFunction("gecko_w10m_set_bridge_sink", []() {
        if (nsCOMPtr<nsIObserverService> obs =
                mozilla::services::GetObserverService()) {
          RefPtr<GeckoW10mBridgeObserver> observer = new GeckoW10mBridgeObserver();
          obs->AddObserver(observer, "gecko-w10m-bridge", false);
          GeckoW10mNote("bridge: chrome can now reach the shell");
        }
      }));
  if (NS_FAILED(rv)) {
    return 0;
  }
  sArmed = true;
  return 1;
}

extern "C" __declspec(dllexport) void gecko_w10m_bridge_reply(const char* aJson) {
  if (!aJson) {
    return;
  }
  nsString data;
  CopyUTF8toUTF16(mozilla::MakeStringSpan(aJson), data);
  NS_DispatchToMainThread(
      NS_NewRunnableFunction("gecko_w10m_bridge_reply", [data]() {
        if (nsCOMPtr<nsIObserverService> obs =
                mozilla::services::GetObserverService()) {
          obs->NotifyObservers(nullptr, "gecko-w10m-bridge-reply", data.get());
        }
      }));
}

extern "C" __declspec(dllexport) void gecko_w10m_memory_pressure(int32_t aLevel) {
  GeckoW10mNote(aLevel >= 2 ? "mem: the phone says memory is CRITICAL"
                          : "mem: the phone says memory is tight");
  NS_NotifyOfEventualMemoryPressure(MemoryPressureState::LowMemory);
}

extern "C" __declspec(dllexport) void gecko_w10m_set_screen(int32_t aWidth,
                                                         int32_t aHeight) {
  if (aWidth <= 0 || aHeight <= 0) {
    return;
  }
  NS_DispatchToMainThread(
      NS_NewRunnableFunction("gecko_w10m_set_screen", [aWidth, aHeight]() {
        mozilla::widget::HeadlessScreenHelper::GeckoW10mSetScreen(aWidth, aHeight);
      }));
}

// A finger: 0 down, 1 move, 2 up, 3 cancelled. Goes through the widget's
// synthesized-touch path, which feeds APZ, so panning and zooming happen on
// the compositor and a tap becomes a click the way it does on any phone.
extern "C" __declspec(dllexport) void gecko_w10m_input_touch(int32_t aPointerId,
                                                          int32_t aState,
                                                          int32_t aX,
                                                          int32_t aY) {
  GeckoW10mDispatchInput(NS_NewRunnableFunction(
      "gecko_w10m_input_touch", [aPointerId, aState, aX, aY]() {

        auto* widget = mozilla::widget::HeadlessWidget::ShownWidget();
        if (!widget) {
          return;
        }
        auto state = TOUCH_CONTACT;
        if (aState == 2) {
          state = TOUCH_REMOVE;
        } else if (aState == 3) {
          state = TOUCH_CANCEL;
        }
        widget->SynthesizeNativeTouchPoint(
            static_cast<uint32_t>(aPointerId), state,
            mozilla::LayoutDeviceIntPoint(aX, aY), 1.0, 90, nullptr);
      }));
}

extern "C" __declspec(dllexport) void gecko_w10m_input_wheel(int32_t aX, int32_t aY,
                                                          double aDeltaX,
                                                          double aDeltaY) {
  GeckoW10mDispatchInput(
      NS_NewRunnableFunction("gecko_w10m_input_wheel", [aX, aY, aDeltaX, aDeltaY]() {
        auto* widget = mozilla::widget::HeadlessWidget::ShownWidget();
        if (!widget) {
          return;
        }
        widget->SynthesizeNativeMouseScrollEvent(
            mozilla::LayoutDeviceIntPoint(aX, aY), 0, aDeltaX, aDeltaY, 0,
            nsIWidget::NativeModifiers::NO_MODIFIERS, 0, nullptr);
      }));
}

// For the shell's heartbeat: the engine's own D3D11 device, so its
// GetDeviceRemovedReason can be polled from outside the renderer thread, and
// the renderer thread's pulse/where counters from WebRender's device code.
extern "C" uint32_t gecko_w10m_wr_pulse_rust();
extern "C" uint32_t gecko_w10m_wr_where_rust();
extern "C" uint32_t gecko_w10m_wr_thread_rust();
extern "C" __declspec(dllexport) uint32_t gecko_w10m_wr_thread() {
  return gecko_w10m_wr_thread_rust();
}
extern "C" __declspec(dllexport) void* gecko_w10m_gecko_device() {
  auto* dm = mozilla::gfx::DeviceManagerDx::Get();
  if (!dm) {
    return nullptr;
  }
  RefPtr<ID3D11Device> device = dm->GetCompositorDevice();
  return device.get();  // the manager keeps its own reference
}
extern "C" __declspec(dllexport) uint32_t gecko_w10m_wr_pulse() {
  return gecko_w10m_wr_pulse_rust();
}
extern "C" __declspec(dllexport) uint32_t gecko_w10m_wr_where() {
  return gecko_w10m_wr_where_rust();
}

extern "C" __declspec(dllexport) int32_t gecko_w10m_frame_copy(
    void* aDest, int32_t aCapacity, int32_t* aWidth, int32_t* aHeight,
    uint64_t* aSerial) {
  mozilla::StaticMutexAutoLock lock(sFrameLock);
  if (!sFront || sFrontWidth <= 0 || sFrontHeight <= 0) {
    return 0;
  }
  if (aSerial && *aSerial == sSerial) {
    return 0;
  }

  // aWidth and aHeight come in holding the size of the caller's bitmap and go
  // out holding the size of the frame. The shape has to match exactly, not
  // merely fit: a 1440-wide bitmap has room for a 1298x2308 frame, but the
  // rows are 1298 pixels long and the bitmap's are 1440, so each row lands a
  // little further along than the last and the picture shears into diagonal
  // stripes. Nor is an equal pixel count enough -- 1440x2560 and 2560x1440 are
  // the same number of pixels and nothing alike -- so both are compared.
  const size_t bytes = size_t(sFrontWidth) * size_t(sFrontHeight) * 4;
  const bool fits = aDest && size_t(aCapacity) >= bytes && aWidth && aHeight &&
                    *aWidth == sFrontWidth && *aHeight == sFrontHeight;
  if (!fits) {
    // Report the size anyway, so the caller can build the right bitmap and
    // come back for the frame.
    if (aWidth) *aWidth = sFrontWidth;
    if (aHeight) *aHeight = sFrontHeight;
    return 0;
  }

  memcpy(aDest, sFront.get(), bytes);
  if (aWidth) *aWidth = sFrontWidth;
  if (aHeight) *aHeight = sFrontHeight;
  if (aSerial) *aSerial = sSerial;
  return 1;
}
#endif

#if defined(GECKO_W10M)
// ---------------------------------------------------------------- text input
//
// A phone has no keyboard until something asks for one, and the only thing
// that knows to ask is Gecko: SetInputContext is called when focus moves onto
// or off an editable field. The shell reads that and raises the on-screen
// keyboard, which is the only reason any of the rest of this is reachable.
//
// Characters are committed as a composition rather than sent as keystrokes.
// That is what an on-screen keyboard is -- it produces text, not scancodes --
// and it is the path TextEventDispatcher exists to serve. Keys that are not
// text, like Backspace and Enter, still have to be keys.

// Whether anything is open over the browser window: a dialog, a menu, a popup.
// These have no title bar here, because a headless window has no frame for the
// system to draw one in, so there is nothing on them to press to make them go
// away. The shell gives them the phone's back button instead, and asks this
// first so that back still means back when nothing is open.
// ---------------------------------------------------------------- the panel
//
// The one XAML SwapChainPanel the shell is showing. EGL asks for it by name
// when it makes a surface, and ANGLE turns it into a composition swap chain;
// from then on the frame is handed to the system compositor as a texture
// rather than copied out through this buffer.
//
// One panel per process, so a static is the honest shape. Written once by the
// shell before the engine starts, read by the render thread afterwards.
namespace {
mozilla::Atomic<void*> sPanel{nullptr};
}

// Called by the shell. Only the panel: its size goes straight to ANGLE, which
// the shell can reach and this cannot -- xul does not link against ANGLE at
// all, because EGL loads it at run time.
extern "C" __declspec(dllexport) void gecko_w10m_set_panel(void* aPanel) {
  const bool first = !sPanel && aPanel;
  sPanel = aPanel;
  if (first) {
    GeckoW10mNote("panel: the engine has somewhere to present");
  }
}

// Read by GLContextProviderEGL, which declares it rather than including this.
extern "C" void* gecko_w10m_egl_native_window() { return sPanel; }

// Read by RenderCompositor::Create, which declares it rather than including
// this: menus, panels and dialogs are composited in software over the panel.
extern "C" bool gecko_w10m_widget_is_overlay(mozilla::widget::CompositorWidget* aWidget) {
  auto* delegate = aWidget ? aWidget->AsDelegate() : nullptr;
  auto* headless = delegate ? delegate->AsHeadlessCompositorWidget() : nullptr;
  return headless && headless->IsOverlay();
}

extern "C" __declspec(dllexport) int32_t gecko_w10m_overlay_open() {
  mozilla::StaticMutexAutoLock lock(sFrameLock);
  for (auto* window : sWindows) {
    if (window->IsOverlayShowing()) {
      return 1;
    }
  }
  return 0;
}

extern "C" __declspec(dllexport) int32_t gecko_w10m_text_input_wanted() {
  return mozilla::widget::HeadlessWidget::TextInputWanted() ? 1 : 0;
}

// The focused field's kind (see HeadlessWidget::TextInputKind) in the low
// byte, and the focus serial above it, so one call tells the shell both which
// keyboard to raise and whether a field has just been focused or tapped.
extern "C" __declspec(dllexport) uint32_t gecko_w10m_text_input_state() {
  return (mozilla::widget::HeadlessWidget::TextInputSerial() << 8) |
         (uint32_t(mozilla::widget::HeadlessWidget::TextInputKind()) & 0xff);
}

extern "C" __declspec(dllexport) void gecko_w10m_input_text(
    const uint16_t* aText, int32_t aLength) {
  if (!aText || aLength <= 0) {
    return;
  }
  nsString text(reinterpret_cast<const char16_t*>(aText), aLength);
  GeckoW10mDispatchInput(NS_NewRunnableFunction(
      "gecko_w10m_input_text", [text = std::move(text)]() mutable {
        // Three ways for this to come to nothing, and none of them said so.
        // Typing has now failed twice for reasons that were in different
        // halves of the path, so each step reports itself and the next log
        // names the one that is silent.
        // An open dialog takes the text (a prompt's field, a sign-in box).
        auto* widget = mozilla::widget::HeadlessWidget::KeyWidget();
        if (!widget) {
          GeckoW10mNote("text: arrived, but there is no shown widget");
          return;
        }
        mozilla::widget::TextEventDispatcher* dispatcher =
            widget->GetTextEventDispatcher();
        if (!dispatcher) {
          GeckoW10mNote("text: arrived, but the widget has no dispatcher");
          return;
        }
        if (NS_FAILED(dispatcher->BeginInputTransaction(TextListener()))) {
          GeckoW10mNote("text: arrived, but the transaction was refused");
          return;
        }
        nsEventStatus status = nsEventStatus_eIgnore;
        dispatcher->CommitComposition(status, &text);
        {
          char note[96];
          SprintfLiteral(note, "text: committed %d characters, status %d",
                         int(text.Length()), int(status));
          GeckoW10mNote(note);
        }
      }));
}

extern "C" __declspec(dllexport) void gecko_w10m_input_key(int32_t aKeyCode) {
  GeckoW10mDispatchInput(
      NS_NewRunnableFunction("gecko_w10m_input_key", [aKeyCode]() {
        // An open dialog takes the keys: Escape from the phone's back button
        // closes it, Enter answers it.
        auto* widget = mozilla::widget::HeadlessWidget::KeyWidget();
        if (!widget) {
          return;
        }
        mozilla::widget::TextEventDispatcher* dispatcher =
            widget->GetTextEventDispatcher();
        // Same refusal as the text path, and so the same cause: Backspace,
        // Enter and the arrows never worked either.
        if (!dispatcher ||
            NS_FAILED(dispatcher->BeginInputTransaction(TextListener()))) {
          GeckoW10mNote("key: arrived, but the transaction was refused");
          return;
        }

        // The shell sends Windows virtual key codes, which is also what
        // NS_VK_* are, so the code needs no translation -- only the name,
        // which is what the DOM actually reports as KeyboardEvent.key.
        mozilla::KeyNameIndex name = mozilla::KEY_NAME_INDEX_Unidentified;
        switch (aKeyCode) {
          case 8:
            name = mozilla::KEY_NAME_INDEX_Backspace;
            break;
          case 9:
            name = mozilla::KEY_NAME_INDEX_Tab;
            break;
          case 13:
            name = mozilla::KEY_NAME_INDEX_Enter;
            break;
          case 27:
            name = mozilla::KEY_NAME_INDEX_Escape;
            break;
          case 33:
            name = mozilla::KEY_NAME_INDEX_PageUp;
            break;
          case 34:
            name = mozilla::KEY_NAME_INDEX_PageDown;
            break;
          case 35:
            name = mozilla::KEY_NAME_INDEX_End;
            break;
          case 36:
            name = mozilla::KEY_NAME_INDEX_Home;
            break;
          case 37:
            name = mozilla::KEY_NAME_INDEX_ArrowLeft;
            break;
          case 38:
            name = mozilla::KEY_NAME_INDEX_ArrowUp;
            break;
          case 39:
            name = mozilla::KEY_NAME_INDEX_ArrowRight;
            break;
          case 40:
            name = mozilla::KEY_NAME_INDEX_ArrowDown;
            break;
          case 46:
            name = mozilla::KEY_NAME_INDEX_Delete;
            break;
          default:
            return;
        }

        mozilla::WidgetKeyboardEvent event(true, mozilla::eKeyDown, widget);
        event.mKeyCode = uint32_t(aKeyCode);
        event.mKeyNameIndex = name;
        event.mCodeNameIndex = mozilla::CODE_NAME_INDEX_UNKNOWN;
        event.mIsRepeat = false;

        nsEventStatus status = nsEventStatus_eIgnore;
        const bool down =
            dispatcher->DispatchKeyboardEvent(mozilla::eKeyDown, event, status);
        const nsEventStatus downStatus = status;
        if (down) {
          dispatcher->MaybeDispatchKeypressEvents(event, status);
        }
        status = nsEventStatus_eIgnore;
        dispatcher->DispatchKeyboardEvent(mozilla::eKeyUp, event, status);
      }));
}

// The on-screen keyboard covers the bottom of the screen, and a window that
// does not know that puts the field being typed into underneath it.
namespace mozilla::widget {
extern int32_t gGeckoW10mPendingWidth;
extern int32_t gGeckoW10mPendingHeight;
}  // namespace mozilla::widget
extern "C" __declspec(dllexport) void gecko_w10m_resize(int32_t aWidth,
                                                     int32_t aHeight) {
  if (aWidth <= 0 || aHeight <= 0) {
    return;
  }
  mozilla::widget::gGeckoW10mPendingWidth = aWidth;
  mozilla::widget::gGeckoW10mPendingHeight = aHeight;
  NS_DispatchToMainThread(
      NS_NewRunnableFunction("gecko_w10m_resize", [aWidth, aHeight]() {
        auto* widget = mozilla::widget::HeadlessWidget::ShownWidget();
        if (!widget) {
          return;
        }
        widget->Resize(mozilla::DesktopSize(aWidth, aHeight), true);
      }));
}
#endif
