/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Handing CompositorD3D11's swap chain to the shell's XAML SwapChainPanel.
//
// The phones that matter most -- the Lumia 550, 650, 950 and their like --
// have Adrenos that stop at Direct3D feature level 9_3. ANGLE there offers
// only GLES2, which hardware WebRender will not run on, so the page is
// rasterised in software. It used to be composited in software as well and
// copied out through main memory into a XAML Image, every frame. This is the
// other half: software WebRender draws the tiles, and the old D3D11 layers
// compositor -- whose shaders are built for 9_3 -- puts them together on the
// GPU and presents them to the same panel ANGLE uses on the bigger phones.
//
// CompositorD3D11 was written for a window. There is no window in an app
// container, so its swap chain is made for composition and given to the
// panel, exactly as NativeWindow11WinRT does for ANGLE. That file cannot be
// reused: ANGLE is a separate library that xul does not link against. It is
// in a file of its own because the WinRT headers do not belong anywhere near
// a unified build.

#include "GeckoW10mPanelSwapChain.h"

#include <atomic>
#include <cstdio>
#include <functional>

#include <dxgi1_3.h>
#include <windows.ui.core.h>
#include <windows.ui.xaml.h>
#include <windows.ui.xaml.controls.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>

extern "C" void GeckoW10mNote(const char* text);

// The panel the shell handed over; HeadlessCompositorWidget keeps it.
extern "C" void* gecko_w10m_egl_native_window();

namespace {

std::atomic<bool> sPresenting{false};

// A SwapChainPanel may only be touched from the thread that owns it. Calling
// SetSwapChain from the render thread does not fail -- it corrupts the system
// compositor, and the process dies a moment later inside CoreUIComponents.
// So the call is handed to the panel's own dispatcher and waited on.
HRESULT RunOnUIThread(std::function<HRESULT()>&& aWork,
                      ABI::Windows::UI::Core::ICoreDispatcher* aDispatcher) {
  boolean hasAccess = FALSE;
  HRESULT hr = aDispatcher->get_HasThreadAccess(&hasAccess);
  if (FAILED(hr)) {
    return hr;
  }
  if (hasAccess) {
    return aWork();
  }

  Microsoft::WRL::Wrappers::Event done(CreateEventExW(
      nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
  if (!done.IsValid()) {
    return E_FAIL;
  }

  HRESULT inner = E_FAIL;
  Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncAction> action;
  hr = aDispatcher->RunAsync(
      ABI::Windows::UI::Core::CoreDispatcherPriority_Normal,
      Microsoft::WRL::Callback<Microsoft::WRL::Implements<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ABI::Windows::UI::Core::IDispatchedHandler,
          Microsoft::WRL::FtmBase>>([&aWork, &inner, &done]() -> HRESULT {
        inner = aWork();
        SetEvent(done.Get());
        return S_OK;
      })
          .Get(),
      &action);
  if (FAILED(hr)) {
    return hr;
  }
  WaitForSingleObjectEx(done.Get(), INFINITE, FALSE);
  return inner;
}

void NoteHr(const char* aWhat, HRESULT aHr) {
  char note[160];
  snprintf(note, sizeof(note), "d3d11: %s, 0x%08lx", aWhat,
           static_cast<unsigned long>(aHr));
  GeckoW10mNote(note);
}

}  // namespace

namespace mozilla::layers {

bool GeckoW10mHasPanel() { return gecko_w10m_egl_native_window() != nullptr; }

HRESULT GeckoW10mAttachSwapChainToPanel(IDXGISwapChain1* aSwapChain) {
  using namespace ABI::Windows::UI;
  auto* window = static_cast<IInspectable*>(gecko_w10m_egl_native_window());
  if (!window || !aSwapChain) {
    GeckoW10mNote("d3d11: there is no panel to present to");
    return E_INVALIDARG;
  }

  Microsoft::WRL::ComPtr<ISwapChainPanelNative> native;
  HRESULT hr = window->QueryInterface(IID_PPV_ARGS(native.GetAddressOf()));
  if (FAILED(hr)) {
    NoteHr("what was handed over is not a SwapChainPanel", hr);
    return hr;
  }
  Microsoft::WRL::ComPtr<Xaml::IDependencyObject> dependency;
  hr = window->QueryInterface(IID_PPV_ARGS(dependency.GetAddressOf()));
  if (FAILED(hr)) {
    NoteHr("the panel is not a XAML object", hr);
    return hr;
  }
  Microsoft::WRL::ComPtr<Core::ICoreDispatcher> dispatcher;
  hr = dependency->get_Dispatcher(dispatcher.GetAddressOf());
  if (FAILED(hr) || !dispatcher) {
    NoteHr("the panel would not say which thread it belongs to", hr);
    return FAILED(hr) ? hr : E_FAIL;
  }
  // The scale is a property of the panel, so it is read on the panel's thread
  // in the same trip that hands the chain over.
  Microsoft::WRL::ComPtr<Xaml::Controls::ISwapChainPanel> panel;
  window->QueryInterface(IID_PPV_ARGS(panel.GetAddressOf()));

  float scaleX = 1.0f;
  float scaleY = 1.0f;
  ISwapChainPanelNative* rawNative = native.Get();
  Xaml::Controls::ISwapChainPanel* rawPanel = panel.Get();
  hr = RunOnUIThread(
      [rawNative, rawPanel, aSwapChain, &scaleX, &scaleY]() -> HRESULT {
        if (rawPanel) {
          rawPanel->get_CompositionScaleX(&scaleX);
          rawPanel->get_CompositionScaleY(&scaleY);
        }
        return rawNative->SetSwapChain(aSwapChain);
      },
      dispatcher.Get());
  if (FAILED(hr)) {
    NoteHr("the panel would not take the swap chain", hr);
    return hr;
  }

  // XAML shows a swap chain's pixels as logical units; on a 200% phone that
  // is a picture twice too big. The inverse transform makes it one to one.
  if (scaleX <= 0) scaleX = 1.0f;
  if (scaleY <= 0) scaleY = 1.0f;
  Microsoft::WRL::ComPtr<IDXGISwapChain2> chain2;
  HRESULT scaled = aSwapChain->QueryInterface(IID_PPV_ARGS(chain2.GetAddressOf()));
  if (SUCCEEDED(scaled)) {
    DXGI_MATRIX_3X2_F inverse = {};
    inverse._11 = 1.0f / scaleX;
    inverse._22 = 1.0f / scaleY;
    scaled = chain2->SetMatrixTransform(&inverse);
  }
  char note[160];
  snprintf(note, sizeof(note),
           "d3d11: the panel took the swap chain, scale %.3fx%.3f, inverse "
           "transform -> 0x%08lx",
           scaleX, scaleY, static_cast<unsigned long>(scaled));
  GeckoW10mNote(note);

  sPresenting = true;
  return S_OK;
}

}  // namespace mozilla::layers

// Polled by the shell. The software path fills the buffer the splash waits
// for; this one presents on its own, so the shell has to be told.
extern "C" __declspec(dllexport) int32_t gecko_w10m_panel_presenting() {
  return sPresenting ? 1 : 0;
}
