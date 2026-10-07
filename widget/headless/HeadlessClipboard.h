/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_HeadlessClipboard_h
#define mozilla_widget_HeadlessClipboard_h

#include "HeadlessClipboardData.h"
#include "mozilla/UniquePtr.h"
#include "nsBaseClipboard.h"
#include "nsIClipboard.h"

namespace mozilla {
namespace widget {

class HeadlessClipboard final : public nsBaseClipboard {
 public:
  HeadlessClipboard();

  NS_DECL_ISUPPORTS_INHERITED
  mozilla::Result<int32_t, nsresult> GetNativeClipboardSequenceNumber(
      ClipboardType aWhichClipboard) override;

 protected:
  ~HeadlessClipboard() = default;

  // Implement the native clipboard behavior.
  NS_IMETHOD SetNativeClipboardData(nsITransferable* aTransferable,
                                    ClipboardType aWhichClipboard) override;
  mozilla::Result<nsCOMPtr<nsISupports>, nsresult> GetNativeClipboardData(
      const nsACString& aFlavor, ClipboardType aWhichClipboard,
      uint64_t aThreshold) override;
  nsresult EmptyNativeClipboardData(ClipboardType aWhichClipboard) override;
  mozilla::Result<bool, nsresult> HasNativeClipboardDataMatchingFlavors(
      const nsTArray<nsCString>& aFlavorList,
      ClipboardType aWhichClipboard) override;

 private:
#if defined(GECKO_W10M)
  // Takes what the shell last read from the phone's clipboard, if that
  // changed since the last look. Main thread; the global clipboard only.
  void GeckoW10mTakePhoneText(ClipboardType aWhichClipboard);
  // The shell's reading this clipboard last took (or set aside as older
  // than a copy made here).
  uint32_t mGeckoW10mSeenSerial = 0;
#endif
  UniquePtr<HeadlessClipboardData>
      mClipboards[nsIClipboard::kClipboardTypeCount];
};

}  // namespace widget
}  // namespace mozilla

#endif
