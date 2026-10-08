/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HeadlessScreenHelper.h"

#include "mozilla/RefPtr.h"
#include "mozilla/dom/DOMTypes.h"
#include "nsTArray.h"
#include "prenv.h"

#if defined(GECKO_W10M)
#  include "mozilla/Sprintf.h"
extern "C" void GeckoW10mNote(const char* text);
#endif

namespace mozilla {
namespace widget {

#if defined(GECKO_W10M)
// The shell says what the display is right now; a phone turns over.
static int32_t sGeckoW10mScreenWidth = 0;
static int32_t sGeckoW10mScreenHeight = 0;
// And how much of it the window has. That is the screen's available area:
// menus and panels are kept inside it. Left as the whole display, a list
// opened near the bottom -- the microphone choice in the permission prompt
// -- "fitted" below its field, into the part under the navigation bar, so it
// neither opened upwards nor got a scroll bar, and its end could not be
// reached.
static int32_t sGeckoW10mRoomWidth = 0;
static int32_t sGeckoW10mRoomHeight = 0;

static void GeckoW10mRefreshScreen(const LayoutDeviceIntRect& aRect) {
  LayoutDeviceIntRect avail = aRect;
  if (sGeckoW10mRoomWidth > 0 && sGeckoW10mRoomHeight > 0) {
    // The window sits at the screen's origin in the engine's coordinates.
    avail = aRect.Intersect(LayoutDeviceIntRect(0, 0, sGeckoW10mRoomWidth,
                                                sGeckoW10mRoomHeight));
    if (avail.IsEmpty()) {
      avail = aRect;
    }
  }
  AutoTArray<RefPtr<Screen>, 1> screenList;
  auto screen = MakeRefPtr<Screen>(
      aRect, avail, 24, 24, 0, DesktopToLayoutDeviceScale(),
      CSSToLayoutDeviceScale(), 96.0f, Screen::IsPseudoDisplay::No,
      Screen::IsHDR::No, 80.0f, 1000.0f);
  screenList.AppendElement(screen.forget());
  ScreenManager::Refresh(std::move(screenList));
}
#endif

/* static */
LayoutDeviceIntRect HeadlessScreenHelper::GetScreenRect() {
#if defined(GECKO_W10M)
  if (sGeckoW10mScreenWidth > 0 && sGeckoW10mScreenHeight > 0) {
    return LayoutDeviceIntRect(0, 0, sGeckoW10mScreenWidth, sGeckoW10mScreenHeight);
  }
#endif
  char* ev = PR_GetEnv("MOZ_HEADLESS_WIDTH");
  int width = 1366;
  if (ev) {
    width = atoi(ev);
  }
  ev = PR_GetEnv("MOZ_HEADLESS_HEIGHT");
  int height = 768;
  if (ev) {
    height = atoi(ev);
  }
  return LayoutDeviceIntRect(0, 0, width, height);
}

#if defined(GECKO_W10M)
/* static */
void HeadlessScreenHelper::GeckoW10mSetScreen(int32_t aWidth, int32_t aHeight) {
  if (aWidth <= 0 || aHeight <= 0 ||
      (aWidth == sGeckoW10mScreenWidth && aHeight == sGeckoW10mScreenHeight)) {
    return;
  }
  sGeckoW10mScreenWidth = aWidth;
  sGeckoW10mScreenHeight = aHeight;
  GeckoW10mRefreshScreen(LayoutDeviceIntRect(0, 0, aWidth, aHeight));
  char note[96];
  SprintfLiteral(note, "screen: now %dx%d", aWidth, aHeight);
  GeckoW10mNote(note);
}

/* static */
void HeadlessScreenHelper::GeckoW10mSetRoom(int32_t aWidth, int32_t aHeight) {
  if (aWidth <= 0 || aHeight <= 0 ||
      (aWidth == sGeckoW10mRoomWidth && aHeight == sGeckoW10mRoomHeight)) {
    return;
  }
  sGeckoW10mRoomWidth = aWidth;
  sGeckoW10mRoomHeight = aHeight;
  GeckoW10mRefreshScreen(GetScreenRect());
}
#endif

HeadlessScreenHelper::HeadlessScreenHelper() {
  AutoTArray<RefPtr<Screen>, 1> screenList;
  LayoutDeviceIntRect rect = GetScreenRect();
#if defined(GECKO_W10M)
  {
    // The screen is built with a scale of one in every coordinate system, so
    // what a window makes of it depends entirely on its own scale. Worth
    // having on the record next to the sizes the windows end up with.
    char note[128];
    SprintfLiteral(note, "screen: %dx%d, all scales 1", rect.Width(),
                   rect.Height());
    GeckoW10mNote(note);
  }
#endif
  auto ret = MakeRefPtr<Screen>(
      rect, rect, 24, 24, 0, DesktopToLayoutDeviceScale(),
      CSSToLayoutDeviceScale(), 96.0f, Screen::IsPseudoDisplay::No,
      Screen::IsHDR::No, 80.0f, 1000.0f);
  screenList.AppendElement(ret.forget());
  ScreenManager::Refresh(std::move(screenList));
}

}  // namespace widget
}  // namespace mozilla
