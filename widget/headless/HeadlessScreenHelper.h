/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_HeadlessScreenHelper_h
#define mozilla_widget_HeadlessScreenHelper_h

#include "mozilla/widget/ScreenManager.h"

namespace mozilla {
namespace widget {

class HeadlessScreenHelper final : public ScreenManager::Helper {
 public:
  HeadlessScreenHelper();
  ~HeadlessScreenHelper() override = default;

 private:
  static LayoutDeviceIntRect GetScreenRect();
#if defined(GECKO_W10M)
 public:
  // Main thread only. Replaces the one screen with one of this size.
  static void GeckoW10mSetScreen(int32_t aWidth, int32_t aHeight);
#endif
};

}  // namespace widget
}  // namespace mozilla

#endif  // mozilla_widget_HeadlessScreenHelper_h
