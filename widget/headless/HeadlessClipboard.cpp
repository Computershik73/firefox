/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HeadlessClipboard.h"

#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsISupportsPrimitives.h"

#if defined(GECKO_W10M)
#  include <atomic>
#  include <cstdint>

namespace {

struct GeckoW10mClipboardBroker {
  int32_t (*writeText)(const uint16_t*, int32_t);
  int32_t (*readText)(uint16_t*, int32_t);
  void (*clear)();
  uint32_t (*sequence)();
};

std::atomic<const GeckoW10mClipboardBroker*> sGeckoW10mClipboardBroker{
    nullptr};

bool GeckoW10mWriteClipboardText(const nsAString& aText) {
  const auto* broker = sGeckoW10mClipboardBroker.load();
  return broker && broker->writeText &&
         broker->writeText(
             reinterpret_cast<const uint16_t*>(aText.BeginReading()),
             static_cast<int32_t>(aText.Length())) == 1;
}

bool GeckoW10mReadClipboardText(nsAString& aText) {
  const auto* broker = sGeckoW10mClipboardBroker.load();
  if (!broker || !broker->readText) {
    return false;
  }
  const int32_t length = broker->readText(nullptr, 0);
  if (length < 0) {
    return false;
  }
  aText.SetLength(static_cast<uint32_t>(length));
  if (!length) {
    return true;
  }
  const int32_t copied = broker->readText(
      reinterpret_cast<uint16_t*>(aText.BeginWriting()), length);
  if (copied < 0) {
    aText.Truncate();
    return false;
  }
  aText.Truncate(static_cast<uint32_t>(copied));
  return true;
}

}  // namespace

extern "C" __declspec(dllexport) void gecko_w10m_set_clipboard_broker(
    const GeckoW10mClipboardBroker* aBroker) {
  sGeckoW10mClipboardBroker.store(aBroker);
}
#endif

namespace mozilla::widget {

NS_IMPL_ISUPPORTS_INHERITED0(HeadlessClipboard, nsBaseClipboard)

HeadlessClipboard::HeadlessClipboard()
    : nsBaseClipboard(mozilla::dom::ClipboardCapabilities(
          true /* supportsSelectionClipboard */,
          true /* supportsFindClipboard */,
          true /* supportsSelectionCache */)) {
  for (auto& clipboard : mClipboards) {
    clipboard = MakeUnique<HeadlessClipboardData>();
  }
}

NS_IMETHODIMP
HeadlessClipboard::SetNativeClipboardData(nsITransferable* aTransferable,
                                          ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(aTransferable);
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));

  // Clear out the clipboard in order to set the new data.
  EmptyNativeClipboardData(aWhichClipboard);

  nsTArray<nsCString> flavors;
  nsresult rv = aTransferable->FlavorsTransferableCanExport(flavors);
  if (NS_FAILED(rv)) {
    return rv;
  }

  auto& clipboard = mClipboards[aWhichClipboard];
  MOZ_ASSERT(clipboard);

  for (const auto& flavor : flavors) {
    nsCOMPtr<nsISupports> data;
    rv = aTransferable->GetTransferData(flavor.get(), getter_AddRefs(data));
    if (NS_FAILED(rv)) {
      continue;
    }

    if (flavor.EqualsLiteral(kTextMime) || flavor.EqualsLiteral(kHTMLMime)) {
      nsCOMPtr<nsISupportsString> wideString = do_QueryInterface(data);
      if (!wideString) {
        continue;
      }
      nsAutoString utf16string;
      wideString->GetData(utf16string);
      if (flavor.EqualsLiteral(kTextMime)) {
        clipboard->SetText(utf16string);
#if defined(GECKO_W10M)
        GeckoW10mWriteClipboardText(utf16string);
#endif
      } else {
        clipboard->SetHTML(utf16string);
      }
    } else {
      clipboard->SetData(flavor, data);
    }
  }

  return NS_OK;
}

mozilla::Result<nsCOMPtr<nsISupports>, nsresult>
HeadlessClipboard::GetNativeClipboardData(const nsACString& aFlavor,
                                          ClipboardType aWhichClipboard,
                                          uint64_t aThreshold) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));

  auto& clipboard = mClipboards[aWhichClipboard];
  MOZ_ASSERT(clipboard);

#if defined(GECKO_W10M)
  if (aWhichClipboard == kGlobalClipboard &&
      aFlavor.EqualsLiteral(kTextMime)) {
    nsAutoString systemText;
    if (GeckoW10mReadClipboardText(systemText)) {
      clipboard->SetText(systemText);
    }
  }
#endif

  if (!aFlavor.EqualsLiteral(kTextMime) && !aFlavor.EqualsLiteral(kHTMLMime)) {
    nsCOMPtr<nsISupports> data = clipboard->GetData(aFlavor);
    return data;
  }

  bool isText = aFlavor.EqualsLiteral(kTextMime);
  if (!(isText ? clipboard->HasText() : clipboard->HasHTML())) {
    return nsCOMPtr<nsISupports>{};
  }

  if (aThreshold && isText && clipboard->GetText().Length() > aThreshold) {
    return mozilla::Err(NS_ERROR_CLIPBOARD_TOO_BIG);
  }

  nsresult rv;
  nsCOMPtr<nsISupportsString> dataWrapper =
      do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID, &rv);
  rv = dataWrapper->SetData(isText ? clipboard->GetText()
                                   : clipboard->GetHTML());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nsCOMPtr<nsISupports>{};
  }

  return nsCOMPtr<nsISupports>(std::move(dataWrapper));
}

nsresult HeadlessClipboard::EmptyNativeClipboardData(
    ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));
  auto& clipboard = mClipboards[aWhichClipboard];
  MOZ_ASSERT(clipboard);
  clipboard->Clear();
#if defined(GECKO_W10M)
  const auto* broker = sGeckoW10mClipboardBroker.load();
  if (aWhichClipboard == kGlobalClipboard && broker && broker->clear) {
    broker->clear();
  }
#endif
  return NS_OK;
}

mozilla::Result<int32_t, nsresult>
HeadlessClipboard::GetNativeClipboardSequenceNumber(
    ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));
  auto& clipboard = mClipboards[aWhichClipboard];
  MOZ_ASSERT(clipboard);
#if defined(GECKO_W10M)
  const auto* broker = sGeckoW10mClipboardBroker.load();
  if (aWhichClipboard == kGlobalClipboard && broker && broker->sequence) {
    return static_cast<int32_t>(broker->sequence());
  }
#endif
  return clipboard->GetChangeCount();
  ;
}

mozilla::Result<bool, nsresult>
HeadlessClipboard::HasNativeClipboardDataMatchingFlavors(
    const nsTArray<nsCString>& aFlavorList, ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));

  auto& clipboard = mClipboards[aWhichClipboard];
  MOZ_ASSERT(clipboard);

#if defined(GECKO_W10M)
  if (aWhichClipboard == kGlobalClipboard) {
    for (const auto& flavor : aFlavorList) {
      if (flavor.EqualsLiteral(kTextMime)) {
        nsAutoString systemText;
        if (GeckoW10mReadClipboardText(systemText)) {
          clipboard->SetText(systemText);
        }
        break;
      }
    }
  }
#endif

  // Retrieve the union of all aHasType in aFlavorList
  for (auto& flavor : aFlavorList) {
    if ((flavor.EqualsLiteral(kTextMime) && clipboard->HasText()) ||
        (flavor.EqualsLiteral(kHTMLMime) && clipboard->HasHTML())) {
      return true;
    }
    if (clipboard->HasData(flavor)) {
      return true;
    }
  }
  return false;
}

}  // namespace mozilla::widget
