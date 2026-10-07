/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HeadlessClipboard.h"

#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsISupportsPrimitives.h"

#if defined(GECKO_W10M)
#  include <atomic>

#  include "mozilla/StaticMutex.h"
#  include "mozilla/StaticPtr.h"
#  include "nsLinebreakConverter.h"
#  include "nsPrintfCString.h"

extern "C" void GeckoW10mNote(const char* aText);

// Windows 10 Mobile: the engine runs headless, so nsWidgetFactory.cpp hands
// out this clipboard, and it kept what was copied in memory and nowhere else.
// Copy and paste worked inside the browser, but nothing copied here reached
// another app, and nothing copied there could be pasted here. The phone's
// clipboard may only be used on the shell's UI thread, while a paste asks
// synchronously on the engine's main thread, so neither side waits:
//  - a copy here is handed to the shell (the sink), which puts it on the
//    phone's clipboard on its UI thread a moment later;
//  - the shell reads the phone's clipboard when it changes and whenever the
//    app comes to the front, and leaves the text here
//    (gecko_w10m_clipboard_phone_text); the clipboard's next question picks
//    it up.
// Each copy handed over is numbered. A reading carries the number of the last
// copy the shell had put on the phone when it began, and one begun before the
// browser's latest copy got there is dropped -- it would otherwise put the
// older text back over that copy. UTF-16 both ways; HTML goes out with text.
using GeckoW10mClipboardSink = void (*)(uint32_t aCopy, const wchar_t* aText,
                                        const wchar_t* aHTML);
static std::atomic<GeckoW10mClipboardSink> sGeckoW10mClipboardSink{nullptr};

static mozilla::StaticMutex sGeckoW10mClipboardLock MOZ_UNANNOTATED;
// The shell's last reading taken: the phone's text, or null for none.
static mozilla::StaticAutoPtr<nsString> sGeckoW10mPhoneText;
// One more for every reading taken.
static uint32_t sGeckoW10mPhoneSerial = 0;
// One more for every copy handed to the shell.
static uint32_t sGeckoW10mCopy = 0;

extern "C" __declspec(dllexport) void gecko_w10m_set_clipboard_sink(
    GeckoW10mClipboardSink aSink) {
  sGeckoW10mClipboardSink.store(aSink);
  GeckoW10mNote(aSink ? "clipboard: the phone's, through the shell"
                      : "clipboard: in memory only");
}

// From the shell, any thread. aAfterCopy: the copy the shell had last put on
// the phone when it began this read. aText null: no text there.
extern "C" __declspec(dllexport) void gecko_w10m_clipboard_phone_text(
    uint32_t aAfterCopy, const wchar_t* aText) {
  nsString* text = nullptr;
  if (aText) {
    text = new nsString(reinterpret_cast<const char16_t*>(aText));
    // Windows ends lines in CR LF (and XAML's text boxes in a lone CR), the
    // DOM in LF; nsClipboard.cpp converts the same way on the desktop.
    nsLinebreakConverter::ConvertStringLineBreaks(
        *text, nsLinebreakConverter::eLinebreakAny,
        nsLinebreakConverter::eLinebreakContent);
  }
  mozilla::StaticMutexAutoLock lock(sGeckoW10mClipboardLock);
  if (aAfterCopy != sGeckoW10mCopy) {
    delete text;  // read before the browser's latest copy reached the phone
    return;
  }
  sGeckoW10mPhoneText = text;
  ++sGeckoW10mPhoneSerial;
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
    if (!flavor.EqualsLiteral(kTextMime) && !flavor.EqualsLiteral(kHTMLMime)) {
      continue;
    }

    nsCOMPtr<nsISupports> data;
    rv = aTransferable->GetTransferData(flavor.get(), getter_AddRefs(data));
    if (NS_FAILED(rv)) {
      continue;
    }

    nsCOMPtr<nsISupportsString> wideString = do_QueryInterface(data);
    if (!wideString) {
      continue;
    }

    nsAutoString utf16string;
    wideString->GetData(utf16string);
    flavor.EqualsLiteral(kTextMime) ? clipboard->SetText(utf16string)
                                    : clipboard->SetHTML(utf16string);
  }

#if defined(GECKO_W10M)
  if (aWhichClipboard == nsIClipboard::kGlobalClipboard) {
    GeckoW10mClipboardSink sink = sGeckoW10mClipboardSink.load();
    const bool any = clipboard->HasText() || clipboard->HasHTML();
    uint32_t copy = 0;
    {
      mozilla::StaticMutexAutoLock lock(sGeckoW10mClipboardLock);
      // Whatever the shell read before this copy is older than it.
      mGeckoW10mSeenSerial = sGeckoW10mPhoneSerial;
      if (sink && any) {
        copy = ++sGeckoW10mCopy;
      }
    }
    if (copy) {
      // nsStrings, so NUL-ended; the shell copies them before returning.
      nsString text(clipboard->GetText()), html(clipboard->GetHTML());
      sink(copy,
           clipboard->HasText() ? static_cast<const wchar_t*>(text.get())
                                : nullptr,
           clipboard->HasHTML() ? static_cast<const wchar_t*>(html.get())
                                : nullptr);
      nsPrintfCString note("clipboard: copy %u, %u characters%s, to the phone's",
                           copy, clipboard->HasText() ? text.Length() : 0u,
                           clipboard->HasHTML() ? " with HTML" : "");
      GeckoW10mNote(note.get());
    }
  }
#endif

  return NS_OK;
}

#if defined(GECKO_W10M)
void HeadlessClipboard::GeckoW10mTakePhoneText(ClipboardType aWhichClipboard) {
  if (aWhichClipboard != nsIClipboard::kGlobalClipboard ||
      !sGeckoW10mClipboardSink.load()) {
    return;
  }
  bool hasText = false;
  nsString text;
  {
    mozilla::StaticMutexAutoLock lock(sGeckoW10mClipboardLock);
    if (sGeckoW10mPhoneSerial == mGeckoW10mSeenSerial) {
      return;
    }
    mGeckoW10mSeenSerial = sGeckoW10mPhoneSerial;
    if (sGeckoW10mPhoneText) {
      hasText = true;
      text = *sGeckoW10mPhoneText;
    }
  }
  auto& clipboard = mClipboards[aWhichClipboard];
  if (!hasText && !clipboard->HasText()) {
    // Nothing on the phone and nothing here; or a copy of only HTML made
    // here, which the phone shows as no text -- ours, so it stays.
    return;
  }
  if (hasText && clipboard->HasText()) {
    // What was copied here, back from the phone: keep it, and the HTML
    // that went with it. Line ends aside.
    nsAutoString ours(clipboard->GetText()), theirs(text);
    ours.StripChar(u'\r');
    theirs.StripChar(u'\r');
    if (ours == theirs) {
      return;
    }
  }
  // Another app copied something since: that is what Paste gives now. The
  // change count moves, so nsBaseClipboard drops what it knew of our copy.
  clipboard->Clear();
  if (hasText) {
    clipboard->SetText(text);
  }
  nsPrintfCString note("clipboard: the phone's changed, %u characters taken",
                       hasText ? text.Length() : 0u);
  GeckoW10mNote(note.get());
}
#endif

mozilla::Result<nsCOMPtr<nsISupports>, nsresult>
HeadlessClipboard::GetNativeClipboardData(const nsACString& aFlavor,
                                          ClipboardType aWhichClipboard,
                                          uint64_t aThreshold) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));
#if defined(GECKO_W10M)
  GeckoW10mTakePhoneText(aWhichClipboard);
#endif

  auto& clipboard = mClipboards[aWhichClipboard];
  MOZ_ASSERT(clipboard);

  if (!aFlavor.EqualsLiteral(kTextMime) && !aFlavor.EqualsLiteral(kHTMLMime)) {
    return nsCOMPtr<nsISupports>{};
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
  return NS_OK;
}

mozilla::Result<int32_t, nsresult>
HeadlessClipboard::GetNativeClipboardSequenceNumber(
    ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));
#if defined(GECKO_W10M)
  GeckoW10mTakePhoneText(aWhichClipboard);
#endif
  auto& clipboard = mClipboards[aWhichClipboard];
  MOZ_ASSERT(clipboard);
  return clipboard->GetChangeCount();
  ;
}

mozilla::Result<bool, nsresult>
HeadlessClipboard::HasNativeClipboardDataMatchingFlavors(
    const nsTArray<nsCString>& aFlavorList, ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(
      nsIClipboard::IsClipboardTypeSupported(aWhichClipboard));
#if defined(GECKO_W10M)
  GeckoW10mTakePhoneText(aWhichClipboard);
#endif

  auto& clipboard = mClipboards[aWhichClipboard];
  MOZ_ASSERT(clipboard);

  // Retrieve the union of all aHasType in aFlavorList
  for (auto& flavor : aFlavorList) {
    if ((flavor.EqualsLiteral(kTextMime) && clipboard->HasText()) ||
        (flavor.EqualsLiteral(kHTMLMime) && clipboard->HasHTML())) {
      return true;
    }
  }
  return false;
}

}  // namespace mozilla::widget
