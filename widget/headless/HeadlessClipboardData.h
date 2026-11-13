/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_HeadlessClipboardData_h
#define mozilla_widget_HeadlessClipboardData_h

#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsTArray.h"

class nsISupports;

namespace mozilla {
namespace widget {

class HeadlessClipboardData final {
 public:
  HeadlessClipboardData() : mPlain(VoidString()), mHTML(VoidString()) {}
  ~HeadlessClipboardData() = default;

  // For text/plain
  void SetText(const nsAString& aText);
  bool HasText() const;
  const nsAString& GetText() const;

  // For text/html
  void SetHTML(const nsAString& aHTML);
  bool HasHTML() const;
  const nsAString& GetHTML() const;

  // Preserve native images and the other transferable flavors too.  The
  // original headless clipboard discarded everything except text/HTML,
  // which made an image copied in one Gecko document unavailable even to
  // another Gecko document in the same process.
  void SetData(const nsACString& aFlavor, nsISupports* aData);
  bool HasData(const nsACString& aFlavor) const;
  already_AddRefed<nsISupports> GetData(const nsACString& aFlavor) const;

  int32_t GetChangeCount() const;

  // For other APIs
  void Clear();

 private:
  nsString mPlain;
  nsString mHTML;

  struct Entry {
    nsCString mFlavor;
    nsCOMPtr<nsISupports> mData;
  };
  nsTArray<Entry> mOtherData;

  int32_t mChangeCount = 0;
};

}  // namespace widget
}  // namespace mozilla

#endif  // mozilla_widget_HeadlessClipboardData_h
