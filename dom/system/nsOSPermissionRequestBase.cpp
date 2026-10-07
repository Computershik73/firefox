/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsOSPermissionRequestBase.h"

#include "mozilla/dom/Promise.h"

using namespace mozilla;

using mozilla::dom::Promise;

#if defined(GECKO_W10M)
#  include <atomic>

#  include "nsPrintfCString.h"
#  include "nsTHashMap.h"
#  include "nsThreadUtils.h"

extern "C" void GeckoW10mNote(const char*);

// Windows 10 Mobile: the microphone sits behind the phone's own consent
// (Settings > Privacy > Microphone). Until the app has it the audio service
// refuses the capture endpoint, and nothing on that path ever asks the user,
// so a call in Discord or WhatsApp started without a microphone. The shell
// knows the consent and can ask for it (MediaCapture, on its UI thread);
// Firefox asks right after the user allowed the site, the same place macOS
// asks (WebRTCParent.checkOSPermission).
//   state(kind): a PERMISSION_STATE_* value, at once. kind 0 is the
//                microphone, 1 the camera.
//   request(id, kind): ask; answered later with
//                gecko_w10m_capture_consent_result(id, granted).
using GeckoW10mConsentState = int32_t (*)(int32_t aKind);
using GeckoW10mConsentRequest = void (*)(uint32_t aId, int32_t aKind);
static std::atomic<GeckoW10mConsentState> sGeckoW10mConsentState{nullptr};
static std::atomic<GeckoW10mConsentRequest> sGeckoW10mConsentRequest{nullptr};

// Main thread only.
static nsTHashMap<uint32_t, RefPtr<Promise>>& GeckoW10mPendingConsents() {
  static nsTHashMap<uint32_t, RefPtr<Promise>> sMap;
  return sMap;
}
static uint32_t sGeckoW10mNextConsent = 1;

extern "C" __declspec(dllexport) void gecko_w10m_set_capture_consent_sink(
    GeckoW10mConsentState aState, GeckoW10mConsentRequest aRequest) {
  sGeckoW10mConsentState.store(aState);
  sGeckoW10mConsentRequest.store(aRequest);
  GeckoW10mNote(aState ? "capture: the shell asks for microphone consent"
                       : "capture: consent sink cleared");
}

// From the shell, on any thread.
extern "C" __declspec(dllexport) void gecko_w10m_capture_consent_result(
    uint32_t aId, int32_t aGranted) {
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "gecko_w10m_capture_consent_result", [aId, aGranted]() {
        auto pending = GeckoW10mPendingConsents().Extract(aId);
        if (!pending || !*pending) {
          return;
        }
        RefPtr<Promise> promise = pending.extract();
        nsPrintfCString note("capture: consent %u %s", aId,
                             aGranted ? "given" : "refused");
        GeckoW10mNote(note.get());
        promise->MaybeResolve(aGranted != 0);
      }));
}

// Hands the promise to the shell; false if there is no shell to ask.
static bool GeckoW10mAskForConsent(int32_t aKind, Promise* aPromise) {
  GeckoW10mConsentRequest request = sGeckoW10mConsentRequest.load();
  if (!request) {
    return false;
  }
  const uint32_t id = sGeckoW10mNextConsent++;
  GeckoW10mPendingConsents().InsertOrUpdate(id, RefPtr<Promise>(aPromise));
  request(id, aKind);
  return true;
}
#endif

NS_IMPL_ISUPPORTS(nsOSPermissionRequestBase, nsIOSPermissionRequest,
                  nsISupportsWeakReference)

NS_IMETHODIMP
nsOSPermissionRequestBase::GetMediaCapturePermissionState(
    uint16_t* aCamera, uint16_t* aMicrophone) {
  nsresult rv = GetVideoCapturePermissionState(aCamera);
  if (NS_FAILED(rv)) {
    return rv;
  }
  return GetAudioCapturePermissionState(aMicrophone);
}

NS_IMETHODIMP
nsOSPermissionRequestBase::GetAudioCapturePermissionState(uint16_t* aAudio) {
  MOZ_ASSERT(aAudio);
#if defined(GECKO_W10M)
  if (GeckoW10mConsentState state = sGeckoW10mConsentState.load()) {
    *aAudio = static_cast<uint16_t>(state(0));
    return NS_OK;
  }
#endif
  *aAudio = PERMISSION_STATE_AUTHORIZED;
  return NS_OK;
}

NS_IMETHODIMP
nsOSPermissionRequestBase::GetVideoCapturePermissionState(uint16_t* aVideo) {
  MOZ_ASSERT(aVideo);
  *aVideo = PERMISSION_STATE_AUTHORIZED;
  return NS_OK;
}

NS_IMETHODIMP
nsOSPermissionRequestBase::GetScreenCapturePermissionState(uint16_t* aScreen) {
  MOZ_ASSERT(aScreen);
  *aScreen = PERMISSION_STATE_AUTHORIZED;
  return NS_OK;
}

nsresult nsOSPermissionRequestBase::GetPromise(JSContext* aCx,
                                               RefPtr<Promise>& aPromiseOut) {
  nsIGlobalObject* globalObject = xpc::CurrentNativeGlobal(aCx);
  if (NS_WARN_IF(!globalObject)) {
    return NS_ERROR_UNEXPECTED;
  }

  ErrorResult result;
  aPromiseOut = Promise::Create(globalObject, result);
  if (NS_WARN_IF(result.Failed())) {
    return result.StealNSResult();
  }

  return NS_OK;
}

NS_IMETHODIMP
nsOSPermissionRequestBase::RequestVideoCapturePermission(
    JSContext* aCx, Promise** aPromiseOut) {
  RefPtr<Promise> promiseHandle;
  nsresult rv = GetPromise(aCx, promiseHandle);
  if (NS_FAILED(rv)) {
    return rv;
  }

  promiseHandle->MaybeResolve(true /* access authorized */);
  promiseHandle.forget(aPromiseOut);
  return NS_OK;
}

NS_IMETHODIMP
nsOSPermissionRequestBase::RequestAudioCapturePermission(
    JSContext* aCx, Promise** aPromiseOut) {
  RefPtr<Promise> promiseHandle;
  nsresult rv = GetPromise(aCx, promiseHandle);
  if (NS_FAILED(rv)) {
    return rv;
  }

#if defined(GECKO_W10M)
  if (GeckoW10mAskForConsent(0, promiseHandle)) {
    promiseHandle.forget(aPromiseOut);
    return NS_OK;
  }
#endif
  promiseHandle->MaybeResolve(true /* access authorized */);
  promiseHandle.forget(aPromiseOut);
  return NS_OK;
}

NS_IMETHODIMP
nsOSPermissionRequestBase::MaybeRequestScreenCapturePermission() {
  return NS_OK;
}
