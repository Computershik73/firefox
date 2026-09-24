//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// NativeWindow11WinRT.cpp: Implementation of NativeWindow11 for a XAML
// SwapChainPanel.

#include "libANGLE/renderer/d3d/d3d11/winrt/NativeWindow11WinRT.h"

#include "libANGLE/renderer/d3d/d3d11/renderer11_utils.h"

#include "common/debug.h"

#include <atomic>
#include <cstdio>
#include <cstdint>

#include <dxgi1_3.h>
#include <windows.ui.core.h>
#include <windows.ui.xaml.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>

namespace
{
// The size of the panel, which belongs to the UI thread while everything here
// runs on the renderer's. So it is handed over rather than asked for: the shell
// already measures the room it has and tells the engine, and this is the same
// number arriving by a shorter road. Written rarely, read every resize.
std::atomic<LONG> sPanelWidth{0};
std::atomic<LONG> sPanelHeight{0};
// The panel's composition scale, times 1000. XAML shows a swap chain's pixels
// as logical units unless told otherwise; on a 350% phone that is a picture
// three and a half times too big. The shell reads the scale from the panel
// and it goes onto the chain as an inverse matrix transform.
std::atomic<int> sPanelScaleX{1000};
std::atomic<int> sPanelScaleY{1000};

// Where this says what it did. The notes Gecko writes live in xul, which this
// library is not linked against, so the shell hands over somewhere to write --
// its own log, which is also where the crash trace goes, so the two can be
// read against each other.
std::atomic<void (*)(const char *)> sLogger{nullptr};
std::atomic<int> sMode{0};

void Note(const char *text)
{
    auto logger = sLogger.load();
    if (logger)
    {
        logger(text);
    }
}

// A SwapChainPanel may only be touched from the thread that owns it. Calling
// SetSwapChain from the render thread does not fail -- it corrupts the system
// compositor, and the process dies a moment later writing to address zero
// inside CoreUIComponents, several frames below dcomp. So the call is handed
// to the panel's own dispatcher and waited on.
HRESULT RunOnUIThread(std::function<HRESULT()> &&work,
                      ABI::Windows::UI::Core::ICoreDispatcher *dispatcher)
{
    if (!dispatcher)
    {
        return E_INVALIDARG;
    }

    boolean hasAccess = FALSE;
    HRESULT result    = dispatcher->get_HasThreadAccess(&hasAccess);
    if (FAILED(result))
    {
        return result;
    }
    if (hasAccess)
    {
        return work();
    }

    Microsoft::WRL::Wrappers::Event done(
        CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
    if (!done.IsValid())
    {
        return E_FAIL;
    }

    HRESULT inner = E_FAIL;
    Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncAction> action;
    result = dispatcher->RunAsync(
        ABI::Windows::UI::Core::CoreDispatcherPriority_Normal,
        Microsoft::WRL::Callback<Microsoft::WRL::Implements<
            Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
            ABI::Windows::UI::Core::IDispatchedHandler, Microsoft::WRL::FtmBase>>(
            [&work, &inner, &done]() -> HRESULT {
                inner = work();
                SetEvent(done.Get());
                return S_OK;
            })
            .Get(),
        &action);
    if (FAILED(result))
    {
        return result;
    }

    WaitForSingleObjectEx(done.Get(), INFINITE, FALSE);
    return inner;
}
}  // namespace

// Called by the shell, which owns the panel and is the only thing that knows
// how big it is. Exported because it is called from outside this library --
// xul cannot forward it, since xul does not link against ANGLE at all: EGL
// loads it at run time.
extern "C" __declspec(dllexport) void angle_uwp_set_panel_size(int32_t width,
                                                               int32_t height)
{
    sPanelWidth  = width;
    sPanelHeight = height;
}

extern "C" __declspec(dllexport) void angle_uwp_set_panel_scale(float x, float y)
{
    sPanelScaleX = static_cast<int>((x > 0 ? x : 1.0f) * 1000.0f + 0.5f);
    sPanelScaleY = static_cast<int>((y > 0 ? y : 1.0f) * 1000.0f + 0.5f);
}

extern "C" __declspec(dllexport) void angle_uwp_set_mode(int32_t mode)
{
    sMode = mode;
    char note[96];
    snprintf(note, sizeof(note), "angle: mode %d -- %s", mode,
             mode == 1   ? "the swap chain will never be handed to the panel"
             : mode == 2 ? "the swap chain will never be presented"
                         : "the real thing");
    Note(note);
}

extern "C" __declspec(dllexport) void angle_uwp_set_logger(void (*logger)(const char *))
{
    sLogger = logger;
    // Proof the channel works. Everything else this file says has failed to
    // appear, and there are two reasons that could be: the code did not run,
    // or it cannot be heard. This line separates them.
    Note("angle: logging is on");
}

namespace rx
{

int AngleUwpMode()
{
    return sMode.load();
}

void AngleUwpNote(const char *text)
{
    Note(text);
}

NativeWindow11WinRT::NativeWindow11WinRT(EGLNativeWindowType window, bool hasAlpha)
    : NativeWindow11(window),
      mPanel(nullptr),
      mDispatcher(nullptr),
      mHasAlpha(hasAlpha),
      mWidth(0),
      mHeight(0)
{}

NativeWindow11WinRT::~NativeWindow11WinRT()
{
    SafeRelease(mPanel);
    SafeRelease(mDispatcher);
}

bool NativeWindow11WinRT::initialize()
{
    if (mPanel)
    {
        return true;
    }
    IInspectable *window = reinterpret_cast<IInspectable *>(getNativeWindow());
    if (!window)
    {
        return false;
    }
    // A SwapChainPanel presents through this one interface and nothing else;
    // if the object handed over does not have it, it is not a panel and there
    // is nothing this class can do with it.
    if (FAILED(window->QueryInterface(__uuidof(ISwapChainPanelNative),
                                      reinterpret_cast<void **>(&mPanel))))
    {
        Note("angle: what was handed over is not a SwapChainPanel");
        return false;
    }

    // Every XAML object knows the thread it belongs to, and that is the only
    // thread allowed to give it a swap chain.
    Microsoft::WRL::ComPtr<ABI::Windows::UI::Xaml::IDependencyObject> dependency;
    if (FAILED(window->QueryInterface(
            __uuidof(ABI::Windows::UI::Xaml::IDependencyObject),
            reinterpret_cast<void **>(dependency.GetAddressOf()))))
    {
        SafeRelease(mPanel);
        return false;
    }
    if (FAILED(dependency->get_Dispatcher(&mDispatcher)) || !mDispatcher)
    {
        Note("angle: the panel would not say which thread it belongs to");
        SafeRelease(mPanel);
        return false;
    }
    Note("angle: the panel is ready");
    return true;
}

bool NativeWindow11WinRT::getClientRect(LPRECT rect) const
{
    if (!rect)
    {
        return false;
    }
    const LONG width  = sPanelWidth.load();
    const LONG height = sPanelHeight.load();
    if (width <= 0 || height <= 0)
    {
        return false;
    }
    rect->left   = 0;
    rect->top    = 0;
    rect->right  = width;
    rect->bottom = height;
    return true;
}

bool NativeWindow11WinRT::isIconic() const
{
    // A phone has no minimised window. It has a suspended app, which stops
    // drawing altogether rather than drawing into a hidden surface.
    return false;
}

HRESULT NativeWindow11WinRT::createSwapChain(ID3D11Device *device,
                                             IDXGIFactory *factory,
                                             DXGI_FORMAT format,
                                             UINT width,
                                             UINT height,
                                             UINT samples,
                                             IDXGISwapChain **swapChain)
{
    if (device == nullptr || factory == nullptr || swapChain == nullptr || width == 0 ||
        height == 0)
    {
        return E_INVALIDARG;
    }
    if (!mPanel && !initialize())
    {
        return E_INVALIDARG;
    }

    // There is no window to present to, so the chain is made for composition
    // and then given to the panel, which is what puts it on the screen. This is
    // the whole point of the exercise: the frame stops being copied through
    // main memory and is handed to the compositor as a texture.
    IDXGIFactory2 *factory2 = d3d11::DynamicCastComObject<IDXGIFactory2>(factory);
    if (factory2 == nullptr)
    {
        return E_INVALIDARG;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width                 = width;
    swapChainDesc.Height                = height;
    swapChainDesc.Format                = format;
    swapChainDesc.Stereo                = FALSE;
    // Composition swap chains take no multisampling; the renderer resolves
    // before presenting.
    swapChainDesc.SampleDesc.Count   = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage =
        DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_BACK_BUFFER | DXGI_USAGE_SHADER_INPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Scaling     = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.AlphaMode = mHasAlpha ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
    swapChainDesc.Flags     = 0;

    {
        char note[128];
        snprintf(note, sizeof(note), "angle: making a %ux%u swap chain for the panel", width,
                 height);
        Note(note);
    }

    IDXGISwapChain1 *swapChain1 = nullptr;
    HRESULT result =
        factory2->CreateSwapChainForComposition(device, &swapChainDesc, nullptr, &swapChain1);
    SafeRelease(factory2);
    if (FAILED(result))
    {
        char note[128];
        snprintf(note, sizeof(note), "angle: the swap chain was refused, 0x%08lx",
                 static_cast<unsigned long>(result));
        Note(note);
        return result;
    }
    Note("angle: the swap chain exists, giving it to the panel");

    if (sMode.load() == 1)
    {
        // The experiment: everything but the hand-over. The chain exists and
        // will be presented to; the panel never hears of it.
        Note("angle: mode 1 -- NOT giving it to the panel");
        mWidth     = static_cast<LONG>(width);
        mHeight    = static_cast<LONG>(height);
        *swapChain = static_cast<IDXGISwapChain *>(swapChain1);
        return S_OK;
    }

    ISwapChainPanelNative *panel = mPanel;
    result                       = RunOnUIThread(
        [panel, swapChain1]() -> HRESULT { return panel->SetSwapChain(swapChain1); },
        mDispatcher);
    if (FAILED(result))
    {
        char note[128];
        snprintf(note, sizeof(note), "angle: the panel would not take it, 0x%08lx",
                 static_cast<unsigned long>(result));
        Note(note);
        SafeRelease(swapChain1);
        return result;
    }
    Note("angle: the panel took the swap chain");

    {
        const float scaleX = sPanelScaleX.load() / 1000.0f;
        const float scaleY = sPanelScaleY.load() / 1000.0f;
        IDXGISwapChain2 *swapChain2 = nullptr;
        if (SUCCEEDED(swapChain1->QueryInterface(__uuidof(IDXGISwapChain2),
                                                 reinterpret_cast<void **>(&swapChain2))) &&
            swapChain2)
        {
            DXGI_MATRIX_3X2_F inverse = {};
            inverse._11               = 1.0f / scaleX;
            inverse._22               = 1.0f / scaleY;
            const HRESULT scaled      = swapChain2->SetMatrixTransform(&inverse);
            char note[160];
            snprintf(note, sizeof(note),
                     "angle: composition scale %.3fx%.3f, inverse transform set -> 0x%08lx", scaleX,
                     scaleY, static_cast<unsigned long>(scaled));
            Note(note);
            swapChain2->Release();
        }
        else
        {
            Note("angle: no IDXGISwapChain2 -- the composition scale cannot be undone");
        }
    }

    mWidth     = static_cast<LONG>(width);
    mHeight    = static_cast<LONG>(height);
    *swapChain = static_cast<IDXGISwapChain *>(swapChain1);
    return S_OK;
}

void NativeWindow11WinRT::commitChange() {}

// static
bool NativeWindow11WinRT::IsValidNativeWindow(EGLNativeWindowType window)
{
    // This is where the refusal happens, and until now it happened in silence:
    // eglCreateWindowSurface asks this first, and if it says no the surface is
    // never attempted and nothing else in this file runs. It says why now.
    IInspectable *inspectable = reinterpret_cast<IInspectable *>(window);
    if (!inspectable)
    {
        Note("angle: there is no panel to make a surface for");
        return false;
    }
    ISwapChainPanelNative *panel = nullptr;
    const HRESULT result = inspectable->QueryInterface(
        __uuidof(ISwapChainPanelNative), reinterpret_cast<void **>(&panel));
    if (FAILED(result))
    {
        // RPC_E_WRONG_THREAD (0x8001010e) here would mean the panel is bound
        // to the UI thread's apartment and this is not that thread -- which is
        // a marshalling problem, not a bad pointer.
        char note[128];
        snprintf(note, sizeof(note),
                 "angle: the panel refused ISwapChainPanelNative, hr 0x%08lx",
                 static_cast<unsigned long>(result));
        Note(note);
        return false;
    }
    SafeRelease(panel);
    Note("angle: the panel is a real SwapChainPanel");
    return true;
}

}  // namespace rx
