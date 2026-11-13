/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if defined(GECKO_W10M)
#include "HeadlessWidget.h"
#include "HeadlessCompositorWidget.h"
#include "mozilla/StaticMutex.h"
#include "nsComponentManagerUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsIAppShellService.h"
#include "nsIAppWindow.h"
#include "nsIBaseWindow.h"
#include "nsIWebBrowserChrome.h"
#include "nsIMutableArray.h"
#include "nsIWritablePropertyBag2.h"
#include "nsIDocShell.h"
#include "nsGlobalWindowOuter.h"
#include "nsIWidgetListener.h"
#include "nsISupportsPrimitives.h"
#include "mozIDOMWindow.h"
#include "nsIURI.h"
#include "nsNetUtil.h"
#include "nsPrintfCString.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include <unordered_map>
#include <deque>
#include <cstring>
#include "mozilla/Services.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPtr.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"

extern "C" void GeckoW10mNote(const char* text);

namespace {
using mozilla::widget::HeadlessWidget;
using mozilla::widget::HeadlessCompositorWidget;
// Status is polled across the ABI; pending/ready are not acknowledgements of
// rendered frames. Closed IDs never refer to another window.
enum : uint32_t { Pending = 1, Ready = 2, Closed = 3, Failed = 4 };
struct Request {
  // Non-owning, main-thread-only registration during CreateTopLevelWindow.
  // Match the URI object, not its canonical string shared by every window.
  nsIURI* creationURI = nullptr;
  bool directPresent = false;
  int32_t viewportWidth = 0;
  int32_t viewportHeight = 0;
  uint32_t state = Pending;
  nsresult error = NS_OK;
  bool claimed = false;
  bool nativeAlive = false;
  bool navigationReady = false;
  bool textWanted = false;
  uint32_t textSerial = 0;
  bool cancelled = false;
  std::deque<int32_t> commands;
};
mozilla::StaticMutex sRequestsLock;
std::unordered_map<uint64_t, Request> sRequests;

class ManagedCommandObserver final : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_IMETHOD Observe(nsISupports* subject, const char* topic, const char16_t* data) override {
    nsCOMPtr<nsISupportsString> value = do_QueryInterface(subject);
    if (!value || !data) return NS_OK;
    nsString idText;
    value->GetData(idText);
    nsresult rv;
    int64_t id = idText.ToInteger64(&rv);
    if (NS_FAILED(rv) || id <= 0) return NS_OK;
    if (strcmp(topic, "gecko-w10m-managed-navigation-ready") == 0) {
      GeckoW10mNote(nsPrintfCString("window-session: navigation ready id=%llu",
          static_cast<unsigned long long>(id)).get());
      mozilla::StaticMutexAutoLock lock(sRequestsLock);
      auto found = sRequests.find(uint64_t(id));
      if (found != sRequests.end()) found->second.navigationReady = true;
      return NS_OK;
    }
    int32_t command = nsDependentString(data).ToInteger(&rv);
    if (NS_FAILED(rv) || command < 1 || (command > 4 && (command < 100 || command > 107))) return NS_OK;
    mozilla::StaticMutexAutoLock lock(sRequestsLock);
    auto found = sRequests.find(uint64_t(id));
    if (found != sRequests.end() && found->second.state == Ready &&
        found->second.commands.size() < 64) found->second.commands.push_back(command);
    return NS_OK;
  }
 private:
  ~ManagedCommandObserver() = default;
};
NS_IMPL_ISUPPORTS(ManagedCommandObserver, nsIObserver)
mozilla::StaticRefPtr<ManagedCommandObserver> sCommandObserver;
void EnsureCommandObserver() {
  MOZ_ASSERT(NS_IsMainThread());
  if (sCommandObserver) return;
  nsCOMPtr<nsIObserverService> observers = mozilla::services::GetObserverService();
  if (!observers) return;
  sCommandObserver = new ManagedCommandObserver();
  observers->AddObserver(sCommandObserver, "gecko-w10m-managed-window-command", false);
  observers->AddObserver(sCommandObserver, "gecko-w10m-managed-navigation-ready", false);
  mozilla::ClearOnShutdown(&sCommandObserver);
}

void FailRequest(uint64_t id, nsresult error) {
  GeckoW10mNote(nsPrintfCString("window-session: failed id=%llu rv=0x%08x",
      static_cast<unsigned long long>(id), uint32_t(error)).get());
  {
    mozilla::StaticMutexAutoLock lock(sRequestsLock);
    auto found = sRequests.find(id);
    if (found == sRequests.end()) return;
    found->second.state = Failed;
    found->second.error = error;
  }
  // A window that already exists owns the binding until its destruction.
  // Never drop its COM panel while an EGL surface may be starting.
  RefPtr<HeadlessWidget> widget = HeadlessWidget::WindowById(id);
  if (!widget) {
    HeadlessCompositorWidget::ForgetWindowPanel(id);
  }
}
void RequestClose(uint64_t id) {
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<HeadlessWidget> widget = HeadlessWidget::WindowById(id);
  if (widget && widget->GetWidgetListener()) {
    widget->GetWidgetListener()->RequestWindowClose(widget);
  }
}
}  // namespace

// nsAppShellService asks using the exact URI it is creating. This carries the
// request through InitData; no 'next widget created' or active-window inference.
extern "C" uint64_t gecko_w10m_claim_window_uri(nsIURI* uri) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!uri) return 0;
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  for (auto& entry : sRequests) {
    auto& request = entry.second;
    if (request.state == Pending && !request.claimed &&
        request.creationURI == uri) {
      request.claimed = true;
      return entry.first;
    }
  }
  return 0;
}

extern "C" void gecko_w10m_window_created(uint64_t id) {
  MOZ_ASSERT(NS_IsMainThread());
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  if (found != sRequests.end()) found->second.nativeAlive = true;
}

extern "C" void gecko_w10m_window_destroyed(uint64_t id) {
  MOZ_ASSERT(NS_IsMainThread());
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  if (found != sRequests.end()) {
    found->second.nativeAlive = false;
    if (found->second.state != Failed) found->second.state = Closed;
  }
}

// A private renderer fails only its own request; never ask the process-wide
// gfx fallback machinery to disable the primary browser's healthy device.
extern "C" void gecko_w10m_window_graphics_failed(uint64_t id) {
  if (!id) return;
  NS_DispatchToMainThread(NS_NewRunnableFunction("ManagedWindowGraphicsFailed", [id]() {
    {
      mozilla::StaticMutexAutoLock lock(sRequestsLock);
      auto found = sRequests.find(id);
      if (found == sRequests.end() || found->second.state == Failed ||
          found->second.state == Closed) return;
    }
    FailRequest(id, NS_ERROR_FAILURE);
    RequestClose(id);
  }));
}

extern "C" bool gecko_w10m_managed_direct_present(uint64_t id) {
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  return found != sRequests.end() && found->second.directPresent;
}

// Shell-owned physical viewport, available before native widget creation.
// Browser chrome/session persistence must not resize a managed native surface.
extern "C" bool gecko_w10m_managed_viewport(uint64_t id, int32_t* width, int32_t* height) {
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  if (found == sRequests.end() || found->second.viewportWidth <= 0 ||
      found->second.viewportHeight <= 0) return false;
  *width = found->second.viewportWidth;
  *height = found->second.viewportHeight;
  return true;
}
extern "C" void gecko_w10m_set_managed_viewport(uint64_t id, int32_t width, int32_t height) {
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  if (found == sRequests.end() || width <= 0 || height <= 0) return;
  found->second.viewportWidth = width;
  found->second.viewportHeight = height;
}

extern "C" __declspec(dllexport) uint64_t gecko_w10m_create_browser_window(
    void* panel, const char* uri, int32_t width, int32_t height, int32_t appMode) {
  if (!panel || !uri || !*uri || width <= 0 || height <= 0) {
    GeckoW10mNote("window-session: request rejected (invalid arguments)");
    return 0;
  }
  const uint64_t id = HeadlessWidget::AllocateWindowId();
  nsCString chrome("chrome://browser/content/browser.xhtml");
  {
    mozilla::StaticMutexAutoLock lock(sRequestsLock);
    if (sRequests.size() >= 64) {
      GeckoW10mNote("window-session: request rejected (session limit)");
      return 0;
    }
    Request request;
    request.viewportWidth = width;
    request.viewportHeight = height;

    sRequests.emplace(id, std::move(request));
  }
  if (!HeadlessCompositorWidget::PrepareWindowPanel(id, panel)) {
    GeckoW10mNote("window-session: request rejected (panel binding)");
    mozilla::StaticMutexAutoLock lock(sRequestsLock);
    sRequests.erase(id);
    return 0;
  }
  nsCString target(uri);
  nsresult queued = NS_DispatchToMainThread(NS_NewRunnableFunction(
      "gecko_w10m_create_browser_window", [id, chrome, target, width, height, appMode]() {
        bool cancelled;
        {
          mozilla::StaticMutexAutoLock lock(sRequestsLock);
          auto found = sRequests.find(id);
          if (found == sRequests.end()) return;
          found->second.directPresent = mozilla::Preferences::GetBool(
              "widget.gecko-w10m.webapp-direct-present", false);
          cancelled = found->second.cancelled;
          if (cancelled) found->second.state = Closed;
        }
        if (cancelled) {
          HeadlessCompositorWidget::ForgetWindowPanel(id);
          return;
        }
        EnsureCommandObserver();
        nsCOMPtr<nsIURI> parsed;
        nsresult rv = NS_NewURI(getter_AddRefs(parsed), target);
        if (NS_FAILED(rv) || !(parsed->SchemeIs("https") || parsed->SchemeIs("http") ||
                              target.EqualsLiteral("about:home") || target.EqualsLiteral("about:blank"))) {
          FailRequest(id, NS_ERROR_INVALID_ARG);
          return;
        }
        // WindowWatcher first creates a top level with a null URI and only
        // later navigates it. Use AppShell directly so the explicit URI claim
        // reaches InitData before widget/compositor construction.
        nsCOMPtr<nsIAppShellService> shell =
            do_GetService("@mozilla.org/appshell/appShellService;1", &rv);
        nsCOMPtr<nsISupportsString> argument =
            do_CreateInstance("@mozilla.org/supports-string;1");
        nsCOMPtr<nsIMutableArray> arguments =
            do_CreateInstance("@mozilla.org/array;1");
        nsCOMPtr<nsIURI> chromeURI;
        if (!shell || !argument || !arguments ||
            NS_FAILED(NS_NewURI(getter_AddRefs(chromeURI), chrome))) {
          FailRequest(id, NS_ERROR_NOT_AVAILABLE);
          return;
        }
        argument->SetData(NS_ConvertUTF8toUTF16(target));
        rv = arguments->AppendElement(argument);
        if (NS_FAILED(rv)) { FailRequest(id, rv); return; }
        nsCOMPtr<nsIWritablePropertyBag2> options =
            do_CreateInstance("@mozilla.org/hash-property-bag;1");
        if (!options) { FailRequest(id, NS_ERROR_NOT_AVAILABLE); return; }
        rv = options->SetPropertyAsAString(u"desktopmode-window"_ns,
            NS_ConvertUTF8toUTF16(nsPrintfCString("%llu", static_cast<unsigned long long>(id))));
        if (NS_SUCCEEDED(rv)) rv = options->SetPropertyAsBool(u"desktopmode-app"_ns, appMode != 0);
        if (NS_SUCCEEDED(rv)) rv = arguments->AppendElement(options);
        if (NS_FAILED(rv)) { FailRequest(id, rv); return; }
        uint32_t flags = nsIWebBrowserChrome::CHROME_ALL |
                         nsIWebBrowserChrome::CHROME_OPENAS_CHROME;
        if (auto* main = HeadlessWidget::MainWidget()) {
          if (auto* listener = main->GetWidgetListener()) {
            if (auto* primary = listener->GetAppWindow()) {
              uint32_t inherited = 0;
              if (NS_SUCCEEDED(primary->GetChromeFlags(&inherited))) {
                flags |= inherited & (nsIWebBrowserChrome::CHROME_REMOTE_WINDOW |
                                      nsIWebBrowserChrome::CHROME_FISSION_WINDOW);
              }
            }
          }
        }
        nsCOMPtr<nsIAppWindow> created;
        {
          mozilla::StaticMutexAutoLock lock(sRequestsLock);
          auto found = sRequests.find(id);
          if (found == sRequests.end()) return;
          found->second.creationURI = chromeURI;
        }
        rv = shell->CreateTopLevelWindow(nullptr, chromeURI, flags, width, height,
                                         getter_AddRefs(created));
        {
          mozilla::StaticMutexAutoLock lock(sRequestsLock);
          auto found = sRequests.find(id);
          if (found != sRequests.end()) found->second.creationURI = nullptr;
        }
        RefPtr<HeadlessWidget> widget = HeadlessWidget::WindowById(id);
        GeckoW10mNote(nsPrintfCString(
            "window-session: top-level id=%llu rv=0x%08x created=%d claimed=%d",
            static_cast<unsigned long long>(id), uint32_t(rv),
            int(bool(created)), int(bool(widget))).get());
        if (NS_FAILED(rv) || !created || !widget) {
          FailRequest(id, NS_FAILED(rv) ? rv : NS_ERROR_UNEXPECTED);
          if (widget) RequestClose(id);
          else if (created) {
            nsCOMPtr<nsIBaseWindow> base = do_QueryInterface(created);
            if (base) base->Destroy();
          }
          return;
        }
        nsCOMPtr<nsIDocShell> docShell;
        rv = created->GetDocShell(getter_AddRefs(docShell));
        auto* outer = docShell ? docShell->GetWindow() : nullptr;
        if (NS_SUCCEEDED(rv) && outer) {
          rv = nsGlobalWindowOuter::Cast(outer)->SetArguments(arguments);
        } else if (NS_SUCCEEDED(rv)) {
          rv = NS_ERROR_NOT_AVAILABLE;
        }
        if (NS_FAILED(rv)) { FailRequest(id, rv); RequestClose(id); return; }
        // CreateTopLevelWindow loads chrome but does not show the window.
        // WindowWatcher normally does this in SizeOpenedWindow. Without it
        // mShowAfterLoad stays false, so there is no initial MozAfterPaint and
        // browser delayed startup can never acknowledge navigation readiness.
        nsCOMPtr<nsIBaseWindow> base = do_QueryInterface(created);
        rv = base ? base->SetVisibility(true) : NS_ERROR_NO_INTERFACE;
        GeckoW10mNote(nsPrintfCString("window-session: show id=%llu rv=0x%08x",
            static_cast<unsigned long long>(id), uint32_t(rv)).get());
        if (NS_FAILED(rv)) { FailRequest(id, rv); RequestClose(id); return; }
        {
          mozilla::StaticMutexAutoLock lock(sRequestsLock);
          auto found = sRequests.find(id);
          if (found == sRequests.end()) return;
          cancelled = found->second.cancelled;
          if (found->second.state == Pending) found->second.state = Ready;
        }
        if (cancelled) RequestClose(id);
      }));
  if (NS_FAILED(queued)) {
    // Preserve the precise error for status polling; do not turn every
    // startup/dispatch failure into an anonymous zero return.
    {
      mozilla::StaticMutexAutoLock lock(sRequestsLock);
      auto& request = sRequests.find(id)->second;
      request.state = Failed;
      request.error = queued;
    }
    GeckoW10mNote(nsPrintfCString("window-session: dispatch failed id=%llu rv=0x%08x",
        static_cast<unsigned long long>(id), uint32_t(queued)).get());
    HeadlessCompositorWidget::ForgetWindowPanel(id);
  }
  return id;
}

extern "C" __declspec(dllexport) uint32_t gecko_w10m_managed_window_count() {
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  uint32_t count = 0;
  for (const auto& entry : sRequests) {
    if (entry.second.state == Pending || entry.second.nativeAlive) ++count;
  }
  return count;
}

extern "C" __declspec(dllexport) uint32_t gecko_w10m_window_request_status(
    uint64_t id, uint32_t* error) {
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  if (error) *error = found != sRequests.end() ? uint32_t(found->second.error) : 0;
  if (found == sRequests.end()) return 0;
  // Failed creation can still be unwinding a native window. Retain its UI
  // panel until Destroy confirms that the renderer has released ownership.
  if (found->second.state == Failed && found->second.nativeAlive) return 5;
  if (found->second.state == Ready && !found->second.navigationReady) return Pending;
  return found->second.state;
}
extern "C" __declspec(dllexport) void gecko_w10m_cancel_window_creation(uint64_t id) {
  {
    mozilla::StaticMutexAutoLock lock(sRequestsLock);
    auto found = sRequests.find(id);
    if (found == sRequests.end() || found->second.state == Closed || found->second.state == Failed) return;
    found->second.cancelled = true;
  }
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "gecko_w10m_cancel_window_creation", [id]() { RequestClose(id); }));
}
extern "C" void gecko_w10m_window_text_focus_changed(uint64_t id, bool wanted, bool fresh) {
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  if (found == sRequests.end()) return;
  auto& request = found->second;
  if (wanted && (fresh || !request.textWanted)) ++request.textSerial;
  request.textWanted = wanted;
}
extern "C" __declspec(dllexport) uint32_t gecko_w10m_window_text_state(uint64_t id) {
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  if (found == sRequests.end()) return 0;
  return (found->second.textSerial << 1) | (found->second.textWanted ? 1u : 0u);
}

extern "C" __declspec(dllexport) int32_t gecko_w10m_window_open_url(uint64_t id, const char* uri) {
  if (!uri || !*uri) return 0;
  {
    mozilla::StaticMutexAutoLock lock(sRequestsLock);
    auto found = sRequests.find(id);
    if (found == sRequests.end() || found->second.state != Ready ||
        !found->second.navigationReady || found->second.cancelled) return 0;
  }
  nsCString url(uri);
  return NS_SUCCEEDED(NS_DispatchToMainThread(NS_NewRunnableFunction(
      "gecko_w10m_window_open_url", [id, url]() {
        {
          mozilla::StaticMutexAutoLock lock(sRequestsLock);
          auto found = sRequests.find(id);
          if (found == sRequests.end() || found->second.state != Ready || found->second.cancelled) return;
        }
        if (StringBeginsWith(url, "desktopmode-context:"_ns)) {
          const nsCString action(Substring(url, 20));
          if (!action.EqualsLiteral("copy") && !action.EqualsLiteral("paste") &&
              !action.EqualsLiteral("saveimage") && !action.EqualsLiteral("dismiss")) return;
          nsCOMPtr<nsISupportsString> subject = do_CreateInstance("@mozilla.org/supports-string;1");
          nsCOMPtr<nsIObserverService> observers = mozilla::services::GetObserverService();
          if (!subject || !observers) return;
          subject->SetData(NS_ConvertUTF8toUTF16(nsPrintfCString("%llu", static_cast<unsigned long long>(id))));
          observers->NotifyObservers(subject, "gecko-w10m-managed-context-action",
              NS_ConvertUTF8toUTF16(action).get());
          return;
        }
        nsCOMPtr<nsIURI> parsed;
        if (NS_FAILED(NS_NewURI(getter_AddRefs(parsed), url)) ||
            !(parsed->SchemeIs("https") || parsed->SchemeIs("http"))) return;
        nsCOMPtr<nsISupportsString> subject = do_CreateInstance("@mozilla.org/supports-string;1");
        nsCOMPtr<nsIObserverService> observers = mozilla::services::GetObserverService();
        if (!subject || !observers) return;
        subject->SetData(NS_ConvertUTF8toUTF16(nsPrintfCString("%llu", static_cast<unsigned long long>(id))));
        observers->NotifyObservers(subject, "gecko-w10m-managed-open-url", NS_ConvertUTF8toUTF16(url).get());
      }))) ? 1 : 0;
}

extern "C" __declspec(dllexport) int32_t gecko_w10m_take_managed_window_command(uint64_t id) {
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  if (found == sRequests.end() || found->second.commands.empty()) return 0;
  int32_t command = found->second.commands.front();
  found->second.commands.pop_front();
  return command;
}

extern "C" __declspec(dllexport) void gecko_w10m_release_window_request(uint64_t id) {
  mozilla::StaticMutexAutoLock lock(sRequestsLock);
  auto found = sRequests.find(id);
  if (found != sRequests.end() &&
      !found->second.nativeAlive &&
      (found->second.state == Closed || found->second.state == Failed)) sRequests.erase(found);
}
#endif