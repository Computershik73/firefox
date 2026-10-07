/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "HeadlessWidget.h"
#if defined(GECKO_W10M)
#  include "nsIRollupListener.h"
#endif

#include <algorithm>

#if defined(GECKO_W10M)
#  include "mozilla/Atomics.h"
#  include "mozilla/Sprintf.h"
extern "C" void GeckoW10mNote(const char* text);
#endif

#include "BasicEvents.h"
#include "ErrorList.h"
#include "HeadlessCompositorWidget.h"
#include "HeadlessKeyBindings.h"
#include "InputData.h"
#include "MouseEvents.h"
#include "UnitTransforms.h"
#include <vector>

#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Maybe.h"
#include "mozilla/NativeKeyBindingsType.h"
#include "mozilla/Preferences.h"
#include "mozilla/TextEventDispatcher.h"
#include "mozilla/TextEvents.h"
#include "mozilla/WritingModes.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/widget/HeadlessWidgetTypes.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "mozilla/widget/Screen.h"
#include "nsIScreen.h"

using namespace mozilla;
using namespace mozilla::gfx;
using namespace mozilla::layers;

using mozilla::LogLevel;

#ifdef MOZ_LOGGING

#  include "mozilla/Logging.h"
static mozilla::LazyLogModule sWidgetLog("Widget");
static mozilla::LazyLogModule sWidgetFocusLog("WidgetFocus");
#  define LOG(args) MOZ_LOG(sWidgetLog, mozilla::LogLevel::Debug, args)
#  define LOGFOCUS(args) \
    MOZ_LOG(sWidgetFocusLog, mozilla::LogLevel::Debug, args)

#else

#  define LOG(args)
#  define LOGFOCUS(args)

#endif /* MOZ_LOGGING */

/*static*/
already_AddRefed<nsIWidget> nsIWidget::CreateHeadlessWidget() {
  nsCOMPtr<nsIWidget> widget = new mozilla::widget::HeadlessWidget();
  return widget.forget();
}

#if defined(GECKO_W10M)
#  include "mozilla/Sprintf.h"
extern "C" void GeckoW10mNote(const char* aText);
extern "C" void GeckoW10mFullscreenChanged(bool aFullscreen);
#endif

namespace mozilla {
namespace widget {

StaticAutoPtr<nsTArray<HeadlessWidget*>> HeadlessWidget::sActiveWindows;

already_AddRefed<HeadlessWidget> HeadlessWidget::GetActiveWindow() {
  if (!sActiveWindows) {
    return nullptr;
  }
  auto length = sActiveWindows->Length();
  if (length == 0) {
    return nullptr;
  }
  RefPtr<HeadlessWidget> widget = sActiveWindows->ElementAt(length - 1);
  return widget.forget();
}

HeadlessWidget::HeadlessWidget()
    : mEnabled(true),
      mVisible(false),
      mDestroyed(false),
      mAlwaysOnTop(false),
      mCompositorWidget(nullptr),
      mSizeMode(nsSizeMode_Normal),
      mLastSizeMode(nsSizeMode_Normal),
      mEffectiveSizeMode(nsSizeMode_Normal),
      mRestoreBounds(0, 0, 0, 0) {
  mWidgetType = WidgetType::Headless;
  if (!sActiveWindows) {
    sActiveWindows = new nsTArray<HeadlessWidget*>();
    ClearOnShutdown(&sActiveWindows);
  }
}

HeadlessWidget::~HeadlessWidget() {
  LOG(("HeadlessWidget::~HeadlessWidget() [%p]\n", (void*)this));

  Destroy();
}

#if defined(GECKO_W10M)
namespace {
// Raw, and main thread only. A strong reference here would keep a window
// alive past its own teardown, which is worse than the discipline.
HeadlessWidget* sShownWidget = nullptr;
// Popups and dialogs in creation order; the last visible one under a point
// is on top. The shell delivers every tap to the browser window, and an
// operating system would have delivered it to the window under the finger
// instead, so that is done here. Dialogs were left out, and their buttons
// never got a tap: it went through to the page under them.
std::vector<HeadlessWidget*> sGeckoW10mPopups;
// A touch that closed a popup, kept from the page until the finger lifts.
int64_t sGeckoW10mSwallowedPointer = -1;

// The window each finger went down in, which keeps the finger until it
// lifts -- what an operating system does for a window under a finger.
// Routing every event of a touch by where that one event landed split a
// touch whose finger slid onto a popup, or under which a popup opened or
// closed: the down went to one window and the up to another, and the window
// that never heard the up held that finger for good. Null once the window
// is gone: the rest of that finger goes nowhere.
struct GeckoW10mFinger {
  uint32_t mId;
  HeadlessWidget* mWindow;
};
std::vector<GeckoW10mFinger> sGeckoW10mFingers;

// A finger or a click outside every open popup closes them, as the operating
// system's window manager tells Firefox to on the desktop (nsWindow's
// DealWithPopups). Headless, nothing did, and a panel -- an extension's,
// the downloads panel -- stayed over the page for good. True when the event
// that closed them is to go no further.
bool GeckoW10mRollupOutside(const mozilla::LayoutDeviceIntPoint& aPoint) {
  nsIRollupListener* listener = nsIWidget::GetActiveRollupListener();
  if (!listener) {
    return false;
  }
  nsCOMPtr<nsIWidget> rollupWidget = listener->GetRollupWidget();
  if (!rollupWidget) {
    return false;
  }
  nsIRollupListener::RollupOptions options;
  options.mPoint = &aPoint;
  return listener->Rollup(options);
}
}  // namespace

HeadlessWidget* HeadlessWidget::ShownWidget() {
  MOZ_ASSERT(NS_IsMainThread());
  return sShownWidget;
}

/* static */
HeadlessWidget* HeadlessWidget::GeckoW10mPopupUnder(
    const LayoutDeviceIntPoint& aPoint) {
  for (auto it = sGeckoW10mPopups.rbegin(); it != sGeckoW10mPopups.rend(); ++it) {
    HeadlessWidget* popup = *it;
    if (!popup->mVisible || popup->mDestroyed ||
        popup->mGeckoW10mInputRegion.mFullyTransparent) {
      continue;
    }
    // The margin is outside the popup for input, as on the desktop
    // (nsWindow's WM_NCHITTEST): a tap in a panel's shadow closes it.
    LayoutDeviceIntRect area = popup->mBounds;
    area.Deflate(popup->mGeckoW10mInputRegion.mMargin);
    if (area.Contains(aPoint.x, aPoint.y)) {
      return popup;
    }
  }
  return nullptr;
}

/* static */
void HeadlessWidget::GeckoW10mPress(uint32_t aPointerId) {
  MOZ_ASSERT(NS_IsMainThread());
  std::erase_if(sGeckoW10mFingers, [aPointerId](const GeckoW10mFinger& aFinger) {
    return aFinger.mId == aPointerId;
  });
  // A finger a rollup swallowed whose release was lost: the new one with its
  // id is a new touch, not the rest of that one.
  if (sGeckoW10mSwallowedPointer == int64_t(aPointerId)) {
    sGeckoW10mSwallowedPointer = -1;
  }
  // Strong references: the cancel runs page script and APZ, which may hide
  // or destroy a popup, and Destroy takes it out of sGeckoW10mPopups.
  AutoTArray<RefPtr<HeadlessWidget>, 8> windows;
  if (sShownWidget) {
    windows.AppendElement(sShownWidget);
  }
  for (HeadlessWidget* popup : sGeckoW10mPopups) {
    windows.AppendElement(popup);
  }
  for (const RefPtr<HeadlessWidget>& window : windows) {
    if (window->mDestroyed || !window->mSynthesizedTouchInput) {
      continue;
    }
    const int32_t index =
        window->mSynthesizedTouchInput->IndexOfTouch(int32_t(aPointerId));
    if (index < 0) {
      continue;
    }
    const LayoutDeviceIntPoint at = ViewAs<LayoutDevicePixel>(
        window->mSynthesizedTouchInput->mTouches[index].mScreenPoint,
        PixelCastJustification::LayoutDeviceIsScreenForUntransformedEvent);
    char note[160];
    SprintfLiteral(note,
                   "touch: finger %u went down again in a window (type %d) "
                   "still holding it -- its release was lost; cancelled",
                   aPointerId, int(window->mWindowType));
    GeckoW10mNote(note);
    MultiTouchInput cancel = UpdateSynthesizedTouchState(
        window->mSynthesizedTouchInput.get(), TimeStamp::Now(), aPointerId,
        TOUCH_CANCEL, at, 1.0, 90);
    window->DispatchTouchInput(cancel);
  }
}

HeadlessWidget* HeadlessWidget::TopDialog() {
  MOZ_ASSERT(NS_IsMainThread());
  for (auto it = sGeckoW10mPopups.rbegin(); it != sGeckoW10mPopups.rend();
       ++it) {
    HeadlessWidget* window = *it;
    if (window->mWindowType == WindowType::Dialog && window->mVisible &&
        !window->mDestroyed) {
      return window;
    }
  }
  return nullptr;
}

HeadlessWidget* HeadlessWidget::KeyWidget() {
  HeadlessWidget* dialog = TopDialog();
  return dialog ? dialog : ShownWidget();
}

namespace {
// Written on the main thread when focus moves, read by the shell from its own.
// A bool either side of a torn read is still a bool.
mozilla::Atomic<bool> sTextInputWanted{false};
mozilla::Atomic<int32_t> sTextInputKind{0};
mozilla::Atomic<uint32_t> sTextInputSerial{0};

int32_t KindOf(const InputContext& aContext) {
  if (aContext.mIMEState.mEnabled == IMEEnabled::Password ||
      aContext.IsPasswordEditor()) {
    return 1;
  }
  // inputmode says what the page wants typed, whatever the element is.
  const nsString& mode = aContext.mHTMLInputMode;
  if (mode.EqualsLiteral("numeric") || mode.EqualsLiteral("decimal")) {
    return 5;
  }
  if (mode.EqualsLiteral("tel")) {
    return 4;
  }
  if (mode.EqualsLiteral("email")) {
    return 2;
  }
  if (mode.EqualsLiteral("url")) {
    return 3;
  }
  if (mode.EqualsLiteral("search")) {
    return 6;
  }
  if (!mode.IsEmpty()) {
    return 0;
  }
  const nsString& type = aContext.mHTMLInputType;
  if (type.EqualsLiteral("email")) {
    return 2;
  }
  if (type.EqualsLiteral("url")) {
    return 3;
  }
  if (type.EqualsLiteral("tel")) {
    return 4;
  }
  if (type.EqualsLiteral("number")) {
    return 5;
  }
  if (type.EqualsLiteral("search") || aContext.mActionHint.EqualsLiteral("search")) {
    return 6;
  }
  return 0;
}
}  // namespace

bool HeadlessWidget::TextInputWanted() { return sTextInputWanted; }
int32_t HeadlessWidget::TextInputKind() { return sTextInputKind; }
uint32_t HeadlessWidget::TextInputSerial() { return sTextInputSerial; }

void HeadlessWidget::SetInputContext(const InputContext& aContext,
                                     const InputContextAction& aAction) {
  mInputContext = aContext;
  // Gecko says this whenever focus moves onto or off an editable field, which
  // is exactly when a phone should raise or drop its keyboard.
  const bool wanted =
      aContext.mIMEState.mEnabled == IMEEnabled::Enabled ||
      aContext.mIMEState.mEnabled == IMEEnabled::Password;
  if (wanted) {
    sTextInputKind = KindOf(aContext);
    // Focus moving onto a field, or a field being clicked while it has the
    // focus: both are someone about to type.
    if (aAction.mFocusChange != InputContextAction::FOCUS_NOT_CHANGED ||
        aAction.mCause == InputContextAction::CAUSE_MOUSE ||
        aAction.mCause == InputContextAction::CAUSE_TOUCH) {
      ++sTextInputSerial;
    }
  }
  sTextInputWanted = wanted;
}
#endif

void HeadlessWidget::Destroy() {
#if defined(GECKO_W10M)
  if (sShownWidget == this) {
    sShownWidget = nullptr;
  }
  for (GeckoW10mFinger& finger : sGeckoW10mFingers) {
    if (finger.mWindow == this) {
      finger.mWindow = nullptr;
    }
  }
  for (auto it = sGeckoW10mPopups.begin(); it != sGeckoW10mPopups.end(); ++it) {
    if (*it == this) {
      sGeckoW10mPopups.erase(it);
      break;
    }
  }
#endif
  if (mDestroyed) {
    return;
  }
  LOG(("HeadlessWidget::Destroy [%p]\n", (void*)this));
  mDestroyed = true;

  if (sActiveWindows) {
    int32_t index = sActiveWindows->IndexOf(this);
    if (index != -1) {
      RefPtr<HeadlessWidget> activeWindow = GetActiveWindow();
      sActiveWindows->RemoveElementAt(index);
      // If this is the currently active widget and there's a previously active
      // widget, activate the previous widget.
      RefPtr<HeadlessWidget> previousActiveWindow = GetActiveWindow();
      if (this == activeWindow && previousActiveWindow &&
          previousActiveWindow->mWidgetListener) {
        previousActiveWindow->mWidgetListener->WindowActivated();
      }
    }
  }

  nsIWidget::OnDestroy();

  nsIWidget::Destroy();
}

nsresult HeadlessWidget::Create(nsIWidget* aParent,
                                const LayoutDeviceIntRect& aRect,
                                const widget::InitData& aInitData) {
  BaseCreate(aParent, aInitData);
  mBounds = aRect;
  mRestoreBounds = aRect;
  mAlwaysOnTop = aInitData.mAlwaysOnTop;
#if defined(GECKO_W10M)
  if (mWindowType == WindowType::Popup || mWindowType == WindowType::Dialog) {
    sGeckoW10mPopups.push_back(this);
  }
#endif
  return NS_OK;
}

void HeadlessWidget::GetCompositorWidgetInitData(
    mozilla::widget::CompositorWidgetInitData* aInitData) {
  *aInitData =
      mozilla::widget::HeadlessCompositorWidgetInitData(GetClientSize());
}

void HeadlessWidget::RaiseWindow() {
  MOZ_ASSERT(
      mWindowType == WindowType::TopLevel || mWindowType == WindowType::Dialog,
      "Raising a non-toplevel window.");

  // Do nothing if this is the currently active window.
  RefPtr<HeadlessWidget> activeWindow = GetActiveWindow();
  if (activeWindow == this) {
    return;
  }

  // Deactivate the last active window.
  if (activeWindow && activeWindow->mWidgetListener) {
    activeWindow->mWidgetListener->WindowDeactivated();
  }

  // Remove this window if it's already tracked.
  int32_t index = sActiveWindows->IndexOf(this);
  if (index != -1) {
    sActiveWindows->RemoveElementAt(index);
  }

  // Activate this window.
  sActiveWindows->AppendElement(this);
  if (mWidgetListener) mWidgetListener->WindowActivated();
}

#if defined(GECKO_W10M)
// The shell reports the room before the browser window exists, and the resize
// it sent was dropped for want of a widget -- so the window kept the whole
// screen it was first told about while the panel was shorter, and the
// hardware frame was drawn 2560 tall into a 2308 surface. The last size seen
// is kept and applied the moment a top-level window shows.
int32_t gGeckoW10mPendingWidth = 0;
int32_t gGeckoW10mPendingHeight = 0;
#endif

void HeadlessWidget::Show(bool aState) {
#if defined(GECKO_W10M)
  if (mCompositorWidget) {
    mCompositorWidget->NotifyVisibilityChanged(aState);
  }
  if (aState && mWindowType == WindowType::TopLevel) {
    // Last one shown wins, which after startup is the browser window: the
    // graphics sanity test shows and hides one of its own before it.
    sShownWidget = this;
    if (gGeckoW10mPendingWidth > 0 && gGeckoW10mPendingHeight > 0) {
      const int32_t width = gGeckoW10mPendingWidth;
      const int32_t height = gGeckoW10mPendingHeight;
      NS_DispatchToMainThread(NS_NewRunnableFunction(
          "HeadlessWidget::ApplyPendingRoom", [width, height]() {
            if (auto* widget = HeadlessWidget::ShownWidget()) {
              widget->Resize(mozilla::DesktopSize(width, height), true);
            }
          }));
    }
  }
#endif
  mVisible = aState;

  LOG(("HeadlessWidget::Show [%p] state %d\n", (void*)this, aState));

  // Top-level window and dialogs are activated/raised when shown.
  // NB: alwaysontop windows are generally used for peripheral indicators.
  //     So we don't focus them by default unless they are a Document
  //     Picture-in-Picture.
  if (aState && (!mAlwaysOnTop || mPiPType == PiPType::DocumentPiP) &&
      (mWindowType == WindowType::TopLevel ||
       mWindowType == WindowType::Dialog)) {
    RaiseWindow();
  }

  ApplySizeModeSideEffects();
}

bool HeadlessWidget::IsVisible() const { return mVisible; }

void HeadlessWidget::SetFocus(Raise aRaise,
                              mozilla::dom::CallerType aCallerType) {
  LOGFOCUS(("  SetFocus %d [%p]\n", aRaise == Raise::Yes, (void*)this));

  // This means we request activation of our toplevel window.
  if (aRaise == Raise::Yes) {
    HeadlessWidget* topLevel = (HeadlessWidget*)GetTopLevelWidget();

    // The toplevel only becomes active if it's currently visible; otherwise, it
    // will be activated anyway when it's shown.
    if (topLevel->IsVisible()) {
      topLevel->RaiseWindow();
    }
  }
}

void HeadlessWidget::Enable(bool aState) { mEnabled = aState; }

bool HeadlessWidget::IsEnabled() const { return mEnabled; }

void HeadlessWidget::Move(const DesktopPoint& aPoint) {
  LOG(("HeadlessWidget::Move [%p] %f %f\n", this, aPoint.x.value,
       aPoint.y.value));

  auto topLeft =
      LayoutDeviceIntPoint::Round(aPoint * GetDesktopToDeviceScale());
  if (mWindowType == WindowType::TopLevel ||
      mWindowType == WindowType::Dialog) {
    SetSizeMode(nsSizeMode_Normal);
  }
  MoveInternal(topLeft.x, topLeft.y);
}

void HeadlessWidget::MoveInternal(int32_t aX, int32_t aY) {
  // Since a popup window's x/y coordinates are in relation to
  // the parent, the parent might have moved so we always move a
  // popup window.
  if (mBounds.IsEqualXY(aX, aY) && mWindowType != WindowType::Popup) {
    return;
  }

  mBounds.MoveTo(aX, aY);
#if defined(GECKO_W10M)
  // And having been made to fit, it has to be put somewhere it fits. A window
  // placed for a screen it is no longer the size of would hang off an edge,
  // and the part that hangs off is simply not drawn.
  if (mWindowType != WindowType::TopLevel) {
    if (nsCOMPtr<nsIScreen> screen = GetWidgetScreen()) {
      int32_t left, top, screenWidth, screenHeight;
      if (NS_SUCCEEDED(screen->GetRectDisplayPix(&left, &top, &screenWidth,
                                                 &screenHeight))) {
        const int32_t x = std::clamp(
            mBounds.X(), left, std::max(left, left + screenWidth - mBounds.Width()));
        const int32_t y = std::clamp(
            mBounds.Y(), top, std::max(top, top + screenHeight - mBounds.Height()));
        mBounds.MoveTo(x, y);
      }
    }
  }
  // A popup is a window of its own and the shell has to be told where to put
  // it over the one underneath.
  if (mCompositorWidget) {
    mCompositorWidget->NotifyOriginChanged(mBounds.TopLeft());
  }
#endif
  NotifyWindowMoved(mBounds.TopLeft());
}

LayoutDeviceIntPoint HeadlessWidget::WidgetToScreenOffset() {
  if (mWindowType == WindowType::Popup) {
    return mBounds.TopLeft();
  }
  return GetTopLevelWidget()->GetBounds().TopLeft();
}

WindowRenderer* HeadlessWidget::GetWindowRenderer() {
  return nsIWidget::GetWindowRenderer();
}

void HeadlessWidget::SetCompositorWidgetDelegate(
    CompositorWidgetDelegate* delegate) {
  if (delegate) {
    mCompositorWidget = delegate->AsHeadlessCompositorWidget();
    MOZ_ASSERT(mCompositorWidget,
               "HeadlessWidget::SetCompositorWidgetDelegate called with a "
               "non-HeadlessCompositorWidget");
  } else {
    mCompositorWidget = nullptr;
  }
}

void HeadlessWidget::Resize(const DesktopSize& aSize, bool aRepaint) {
  int32_t width = NSToIntRound(aSize.width);
  int32_t height = NSToIntRound(aSize.height);
  ResizeInternal(width, height, aRepaint);
}

void HeadlessWidget::ResizeInternal(int32_t aWidth, int32_t aHeight,
                                    bool aRepaint) {
#if defined(GECKO_W10M)
  // What was asked for, what the constraints allowed, and what kind of window
  // it was. This found the window that came out wider than the screen, and it
  // stays because it is the only place the two numbers can be compared.
  const int32_t asked = aWidth;
  const int32_t askedHeight = aHeight;
#endif
  ConstrainSize(&aWidth, &aHeight);
#if defined(GECKO_W10M)
  // ConstrainSize knows only the minimum and maximum the front end asked for;
  // nothing in the path brings a window down to the size of the display. On a
  // desktop a dialog wider than the screen is merely a window wider than the
  // screen. Here there is nowhere for it to hang off to, so the About dialog
  // arrived 1991 device pixels wide on a screen of 1440 and lost its right
  // hand side. The screen wins, and layout is told the real size so the
  // contents reflow into it rather than being cut.
  //
  // Only for windows that sit over the browser. The browser window itself is
  // sized by the shell and has a minimum of its own that must not be fought.
  if (mWindowType != WindowType::TopLevel) {
    if (nsCOMPtr<nsIScreen> screen = GetWidgetScreen()) {
      int32_t left, top, screenWidth, screenHeight;
      if (NS_SUCCEEDED(screen->GetRectDisplayPix(&left, &top, &screenWidth,
                                                 &screenHeight))) {
        aWidth = std::min(aWidth, screenWidth);
        aHeight = std::min(aHeight, screenHeight);
      }
    }
  }
#endif
  mBounds.SizeTo(LayoutDeviceIntSize(aWidth, aHeight));
#if defined(GECKO_W10M)
  {
    char note[160];
    SprintfLiteral(note,
                   "resize: type %d asked %dx%d got %dx%d, scale %.3f",
                   int(mWindowType), asked, askedHeight, aWidth, aHeight,
                   double(GetDefaultScale().scale));
    GeckoW10mNote(note);
  }
#endif

  if (mCompositorWidget) {
    mCompositorWidget->NotifyClientSizeChanged(mBounds.Size());
  }
  if (mWidgetListener) {
    mWidgetListener->WindowResized(this, mBounds.Size());
  }
  if (mAttachedWidgetListener) {
    mAttachedWidgetListener->WindowResized(this, mBounds.Size());
  }
}

void HeadlessWidget::Resize(const DesktopRect& aRect, bool aRepaint) {
  auto tl =
      LayoutDeviceIntPoint::Round(aRect.TopLeft() * GetDesktopToDeviceScale());
  // Intentionally to avoid SetSizeMode() call in Move().
  MoveInternal(tl.x, tl.y);
  Resize(aRect.Size(), aRepaint);
}

void HeadlessWidget::SetSizeMode(nsSizeMode aMode) {
  LOG(("HeadlessWidget::SetSizeMode [%p] %d\n", (void*)this, aMode));

  if (aMode == mSizeMode) {
    return;
  }

  if (aMode == nsSizeMode_Normal && mSizeMode == nsSizeMode_Fullscreen) {
    MakeFullScreen(false);
    return;
  }

  mSizeMode = aMode;

  // Normally in real widget backends a window event would be triggered that
  // would cause the window manager to handle resizing the window. In headless
  // the window must manually be resized.
  ApplySizeModeSideEffects();
}

void HeadlessWidget::ApplySizeModeSideEffects() {
  if (!mVisible || mEffectiveSizeMode == mSizeMode) {
    return;
  }

#if defined(GECKO_W10M)
  // Every branch below resizes or moves the window, and on this port neither
  // is the window's to do: the shell measures the room and the swap chain and
  // hands both over together through gecko_w10m_resize. Record the mode and say
  // so; the size stays what the shell last asked for.
  mEffectiveSizeMode = mSizeMode;
  if (mWidgetListener) {
    mWidgetListener->SizeModeChanged(mSizeMode);
  }
  return;
#else

  if (mEffectiveSizeMode == nsSizeMode_Normal) {
    // Store the last normal size bounds so it can be restored when entering
    // normal mode again.
    mRestoreBounds = mBounds;
  }

  switch (mSizeMode) {
    case nsSizeMode_Normal: {
      MoveInternal(mRestoreBounds.X(), mRestoreBounds.Y());
      ResizeInternal(mRestoreBounds.Width(), mRestoreBounds.Height(), false);
      break;
    }
    case nsSizeMode_Minimized:
      break;
    case nsSizeMode_Maximized: {
      nsCOMPtr<nsIScreen> screen = GetWidgetScreen();
      if (screen) {
        int32_t left, top, width, height;
        if (NS_SUCCEEDED(
                screen->GetRectDisplayPix(&left, &top, &width, &height))) {
          MoveInternal(0, 0);
          ResizeInternal(width, height, true);
        }
      }
      break;
    }
    case nsSizeMode_Fullscreen:
      // This will take care of resizing the window.
      nsIWidget::InfallibleMakeFullScreen(true);
      break;
    default:
      break;
  }

  mEffectiveSizeMode = mSizeMode;
  if (mWidgetListener) {
    mWidgetListener->SizeModeChanged(mSizeMode);
  }
#endif
}

#if defined(GECKO_W10M)
// Set by the shell from the display's own numbers, once at startup and again
// whenever the phone reports a change.
static float sGeckoW10mDpi = 96.0f;

/* static */
void HeadlessWidget::GeckoW10mSetDpi(int32_t aDpi) {
  // A phone that reports something absurd is a phone we ignore.
  if (aDpi < 50 || aDpi > 1200 || float(aDpi) == sGeckoW10mDpi) {
    return;
  }
  sGeckoW10mDpi = float(aDpi);
  char note[96];
  SprintfLiteral(note, "screen: %d dpi -- a tap may wander %d px", aDpi,
                 aDpi / 10);
  GeckoW10mNote(note);
  // APZ took a copy when the tree manager was configured; it needs the new one.
  if (HeadlessWidget* shown = HeadlessWidget::ShownWidget()) {
    shown->NotifyAPZOfDPIChange();
  }
}

float HeadlessWidget::GetDPI() { return sGeckoW10mDpi; }
#endif

nsresult HeadlessWidget::MakeFullScreen(bool aFullScreen) {
  // Directly update the size mode here so a later call SetSizeMode does
  // nothing.
  if (aFullScreen) {
    if (mSizeMode != nsSizeMode_Fullscreen) {
      mLastSizeMode = mSizeMode;
    }
    mSizeMode = nsSizeMode_Fullscreen;
  } else {
    mSizeMode = mLastSizeMode;
  }

#if defined(GECKO_W10M)
  // Order is the whole fix here.
  //
  // The window is resized first, synchronously, to the rectangle the shell
  // reports as the screen -- which is the whole display, the size this window
  // will have once the status bar and the navigation bar are out of the way.
  // Only then does the size-mode change go out, and only then does the page
  // hear "fullscreenchange". So the page builds its fullscreen player once,
  // already at the final size.
  //
  // Doing it the other way round is what kept throwing the player back into a
  // window: the shell hid the bars, the room grew a moment later, and the page
  // was handed a resize in the middle of its own transition. vkvideo's player
  // answers a resize by rebuilding itself, which removes the element that
  // asked for fullscreen, and Gecko has no choice but to leave.
  //
  // The shell is told as well, because hiding the bars is its job; by the time
  // it has, the window is already the size it will measure, so nothing resizes
  // twice.
  GeckoW10mFullscreenChanged(aFullScreen);
  nsIWidget::InfallibleMakeFullScreen(aFullScreen);
  if (mWidgetListener) {
    mWidgetListener->SizeModeChanged(mSizeMode);
  }
  return NS_OK;
#else
  // Notify the listener first so size mode change events are triggered before
  // resize events.
  if (mWidgetListener) {
    mWidgetListener->SizeModeChanged(mSizeMode);
  }

  // Real widget backends don't seem to follow a common approach for
  // when and how many resize events are triggered during fullscreen
  // transitions. InfallibleMakeFullScreen will trigger a resize, but it
  // will be ignored if still transitioning to fullscreen, so it must be
  // triggered on the next tick.
  RefPtr<HeadlessWidget> self(this);
  NS_DispatchToCurrentThread(NS_NewRunnableFunction(
      "HeadlessWidget::MakeFullScreen", [self, aFullScreen]() -> void {
        self->InfallibleMakeFullScreen(aFullScreen);
      }));

  return NS_OK;
#endif
}

nsresult HeadlessWidget::AttachNativeKeyEvent(WidgetKeyboardEvent& aEvent) {
  HeadlessKeyBindings& bindings = HeadlessKeyBindings::GetInstance();
  return bindings.AttachNativeKeyEvent(aEvent);
}

bool HeadlessWidget::GetEditCommands(NativeKeyBindingsType aType,
                                     const WidgetKeyboardEvent& aEvent,
                                     nsTArray<CommandInt>& aCommands) {
  // Validate the arguments.
  if (NS_WARN_IF(!nsIWidget::GetEditCommands(aType, aEvent, aCommands))) {
    return false;
  }

  Maybe<WritingMode> writingMode;
  if (aEvent.NeedsToRemapNavigationKey()) {
    if (RefPtr<TextEventDispatcher> dispatcher = GetTextEventDispatcher()) {
      writingMode = dispatcher->MaybeQueryWritingModeAtSelection();
    }
  }

  HeadlessKeyBindings& bindings = HeadlessKeyBindings::GetInstance();
  bindings.GetEditCommands(aType, aEvent, writingMode, aCommands);
  return true;
}

nsresult HeadlessWidget::SynthesizeNativeMouseEvent(
    LayoutDeviceIntPoint aPoint, NativeMouseMessage aNativeMessage,
    MouseButton aButton, nsIWidget::NativeModifiers aModifierFlags,
    nsISynthesizedEventCallback* aCallback) {
  AutoSynthesizedEventCallbackNotifier notifier(aCallback);
#if defined(GECKO_W10M)
  if (mWindowType == WindowType::TopLevel) {
    if (HeadlessWidget* popup = GeckoW10mPopupUnder(aPoint)) {
      return popup->SynthesizeNativeMouseEvent(aPoint, aNativeMessage, aButton,
                                               aModifierFlags, nullptr);
    }
    // A dialog is modal: what is under it waits for its answer. A tap
    // beside it reaching the page would act on a page that is not done
    // asking.
    if (TopDialog()) {
      return NS_OK;
    }
    if (aNativeMessage == NativeMouseMessage::ButtonDown &&
        GeckoW10mRollupOutside(aPoint)) {
      return NS_OK;
    }
  }
#endif
  EventMessage msg;
  switch (aNativeMessage) {
    case NativeMouseMessage::Move:
      msg = eMouseMove;
      break;
    case NativeMouseMessage::ButtonDown:
      msg = eMouseDown;
      break;
    case NativeMouseMessage::ButtonUp:
      msg = eMouseUp;
      break;
    case NativeMouseMessage::EnterWindow:
    case NativeMouseMessage::LeaveWindow:
      MOZ_ASSERT_UNREACHABLE("Unsupported synthesized mouse event");
      return NS_ERROR_UNEXPECTED;
  }
  WidgetMouseEvent event(true, msg, this, WidgetMouseEvent::eReal);
  event.mRefPoint = aPoint - WidgetToScreenOffset();
  if (msg == eMouseDown || msg == eMouseUp) {
    event.mButton = aButton;
  }
  if (msg == eMouseDown) {
    event.mClickCount = 1;
  }
  event.AssignEventTime(WidgetEventTime());
  DispatchInputEvent(&event);
  return NS_OK;
}

nsresult HeadlessWidget::SynthesizeNativeMouseScrollEvent(
    mozilla::LayoutDeviceIntPoint aPoint, uint32_t aNativeMessage,
    double aDeltaX, double aDeltaY, double aDeltaZ,
    nsIWidget::NativeModifiers aModifierFlags, uint32_t aAdditionalFlags,
    nsISynthesizedEventCallback* aCallback) {
  AutoSynthesizedEventCallbackNotifier notifier(aCallback);
  // The various platforms seem to handle scrolling deltas differently,
  // but the following seems to emulate it well enough.
  WidgetWheelEvent event(true, eWheel, this);
  event.mDeltaMode = MOZ_HEADLESS_SCROLL_DELTA_MODE;
  event.mIsNoLineOrPageDelta = true;
  event.mDeltaX = -aDeltaX * MOZ_HEADLESS_SCROLL_MULTIPLIER;
  event.mDeltaY = -aDeltaY * MOZ_HEADLESS_SCROLL_MULTIPLIER;
  event.mDeltaZ = -aDeltaZ * MOZ_HEADLESS_SCROLL_MULTIPLIER;
  event.mRefPoint = aPoint - WidgetToScreenOffset();
  event.AssignEventTime(WidgetEventTime());
  DispatchInputEvent(&event);
  return NS_OK;
}

nsresult HeadlessWidget::SynthesizeNativeTouchPoint(
    uint32_t aPointerId, TouchPointerState aPointerState,
    LayoutDeviceIntPoint aPoint, double aPointerPressure,
    uint32_t aPointerOrientation, nsISynthesizedEventCallback* aCallback) {
  AutoSynthesizedEventCallbackNotifier notifier(aCallback);
#if defined(GECKO_W10M)
  return GeckoW10mSynthesizeTouch(aPointerId, aPointerState, aPoint,
                                  aPointerPressure, aPointerOrientation,
                                  TimeStamp::Now());
}

nsresult HeadlessWidget::GeckoW10mSynthesizeTouch(
    uint32_t aPointerId, TouchPointerState aPointerState,
    LayoutDeviceIntPoint aPoint, double aPointerPressure,
    uint32_t aPointerOrientation, const TimeStamp& aTime) {
#endif

  MOZ_ASSERT(NS_IsMainThread());
  if (aPointerState == TOUCH_HOVER) {
    return NS_ERROR_UNEXPECTED;
  }
#if defined(GECKO_W10M)
  // As with the mouse: a finger that goes down over a popup or a dialog
  // belongs to its window until it lifts, and one beside an open dialog to
  // nobody.
  if (mWindowType == WindowType::TopLevel) {
    HeadlessWidget* owner = this;
    bool fresh = false;
    auto finger = std::find_if(
        sGeckoW10mFingers.begin(), sGeckoW10mFingers.end(),
        [aPointerId](const GeckoW10mFinger& aFinger) {
          return aFinger.mId == aPointerId;
        });
    if (finger != sGeckoW10mFingers.end()) {
      owner = finger->mWindow;
      if (aPointerState & (TOUCH_REMOVE | TOUCH_CANCEL)) {
        sGeckoW10mFingers.erase(finger);
      }
      if (!owner) {
        return NS_OK;  // its window is gone
      }
    } else if (aPointerState & TOUCH_CONTACT) {
      if (HeadlessWidget* popup = GeckoW10mPopupUnder(aPoint)) {
        owner = popup;
      }
      sGeckoW10mFingers.push_back({aPointerId, owner});
      fresh = true;
    }
    if (owner != this) {
      return owner->GeckoW10mSynthesizeTouch(aPointerId, aPointerState, aPoint,
                                             aPointerPressure,
                                             aPointerOrientation, aTime);
    }
    // The whole touch, unless one the page already has under way is ending.
    if (TopDialog() && (!mSynthesizedTouchInput ||
                        mSynthesizedTouchInput->mTouches.IsEmpty())) {
      // A finger that went down beside the dialog stays nobody's until it
      // lifts, even if the dialog closes under it meanwhile.
      if (fresh) {
        sGeckoW10mFingers.back().mWindow = nullptr;
      }
      return NS_OK;
    }
    // A touch outside the open popups closes them; if Firefox says so, the
    // whole touch stops there.
    if (sGeckoW10mSwallowedPointer == int64_t(aPointerId)) {
      if (aPointerState & (TOUCH_REMOVE | TOUCH_CANCEL)) {
        sGeckoW10mSwallowedPointer = -1;
      }
      return NS_OK;
    }
    if ((aPointerState & TOUCH_CONTACT) &&
        (!mSynthesizedTouchInput ||
         mSynthesizedTouchInput->mTouches.IsEmpty()) &&
        GeckoW10mRollupOutside(aPoint)) {
      sGeckoW10mSwallowedPointer = int64_t(aPointerId);
      return NS_OK;
    }
  }
#endif

  if (!mSynthesizedTouchInput) {
    mSynthesizedTouchInput = MakeUnique<MultiTouchInput>();
  }

  LayoutDeviceIntPoint pointInWindow = aPoint - WidgetToScreenOffset();
#if defined(GECKO_W10M)
  // The finger's time, not the main thread's (GeckoW10mSynthesizeTouch).
  const TimeStamp when = aTime;
#else
  const TimeStamp when = TimeStamp::Now();
#endif
  MultiTouchInput inputToDispatch = UpdateSynthesizedTouchState(
      mSynthesizedTouchInput.get(), when, aPointerId, aPointerState,
      pointInWindow, aPointerPressure, aPointerOrientation);
  DispatchTouchInput(inputToDispatch);
  return NS_OK;
}

nsresult HeadlessWidget::SynthesizeNativeTouchPadPinch(
    TouchpadGesturePhase aEventPhase, float aScale, LayoutDeviceIntPoint aPoint,
    int32_t aModifierFlags) {
  MOZ_ASSERT(NS_IsMainThread());

  PinchGestureInput::PinchGestureType pinchGestureType =
      PinchGestureInput::PINCHGESTURE_SCALE;
  ScreenCoord CurrentSpan;
  ScreenCoord PreviousSpan;
  switch (aEventPhase) {
    case PHASE_BEGIN:
      pinchGestureType = PinchGestureInput::PINCHGESTURE_START;
      CurrentSpan = aScale;
      PreviousSpan = 0.999;
      break;

    case PHASE_UPDATE:
      pinchGestureType = PinchGestureInput::PINCHGESTURE_SCALE;
      if (aScale == mLastPinchSpan) {
        return NS_ERROR_INVALID_ARG;
      }
      CurrentSpan = aScale;
      PreviousSpan = mLastPinchSpan;
      break;

    case PHASE_END:
      pinchGestureType = PinchGestureInput::PINCHGESTURE_END;
      CurrentSpan = aScale;
      PreviousSpan = mLastPinchSpan;
      break;

    default:
      return NS_ERROR_INVALID_ARG;
  }

  ScreenPoint touchpadPoint = ViewAs<ScreenPixel>(
      aPoint - WidgetToScreenOffset(),
      PixelCastJustification::LayoutDeviceIsScreenForUntransformedEvent);
  // The headless widget does not support modifiers.
  // Do not pass `aModifierFlags` because it contains native modifier values.
  PinchGestureInput inputToDispatch(
      pinchGestureType, PinchGestureInput::TRACKPAD, TimeStamp::Now(),
      ExternalPoint(0, 0), touchpadPoint,
      100.0 * ((aEventPhase == PHASE_END) ? ScreenCoord(1.f) : CurrentSpan),
      100.0 * ((aEventPhase == PHASE_END) ? ScreenCoord(1.f) : PreviousSpan),
      0);

  if (!inputToDispatch.SetLineOrPageDeltaY(this)) {
    return NS_ERROR_INVALID_ARG;
  }

  mLastPinchSpan = aScale;
  DispatchPinchGestureInput(inputToDispatch);
  return NS_OK;
}

nsresult HeadlessWidget::SynthesizeNativeTouchpadPan(
    TouchpadGesturePhase aEventPhase, LayoutDeviceIntPoint aPoint,
    double aDeltaX, double aDeltaY, int32_t aModifierFlags,
    nsISynthesizedEventCallback* aCallback) {
  AutoSynthesizedEventCallbackNotifier notifier(aCallback);

  MOZ_ASSERT(NS_IsMainThread());

  PanGestureInput::PanGestureType eventType = PanGestureInput::PANGESTURE_PAN;
  switch (aEventPhase) {
    case PHASE_BEGIN:
      eventType = PanGestureInput::PANGESTURE_START;
      break;
    case PHASE_UPDATE:
      eventType = PanGestureInput::PANGESTURE_PAN;
      break;
    case PHASE_END:
      eventType = PanGestureInput::PANGESTURE_END;
      break;
    default:
      return NS_ERROR_INVALID_ARG;
  }

  ScreenPoint touchpadPoint = ViewAs<ScreenPixel>(
      aPoint - WidgetToScreenOffset(),
      PixelCastJustification::LayoutDeviceIsScreenForUntransformedEvent);
  PanGestureInput input(eventType, TimeStamp::Now(), touchpadPoint,
                        ScreenPoint(float(aDeltaX), float(aDeltaY)),
                        // Same as SynthesizeNativeTouchPadPinch case we ignore
                        // aModifierFlags.
                        0);

  input.mSimulateMomentum =
      Preferences::GetBool("apz.test.headless.simulate_momentum");

  DispatchPanGestureInput(input);

  return NS_OK;
}

}  // namespace widget
}  // namespace mozilla
