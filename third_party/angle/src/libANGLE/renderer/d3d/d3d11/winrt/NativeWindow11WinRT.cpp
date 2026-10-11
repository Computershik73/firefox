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
// number arriving by a shorter road. Written rarely, read every resize. One
// value, the width in its upper half: as two, a read between the two stores
// when the phone is turned gave a size as wide as the new one and as tall as
// the old, larger than the window either way up, and buffers made that size
// hold every size after it and are kept (SwapChain11::resize).
std::atomic<int64_t> sPanelSize{0};
// The panel's composition scale, times 1000. XAML shows a swap chain's pixels
// as logical units unless told otherwise; on a 350% phone that is a picture
// three and a half times too big. The shell reads the scale from the panel
// and it goes onto the chain as an inverse matrix transform.
std::atomic<int> sPanelScaleX{1000};
std::atomic<int> sPanelScaleY{1000};
// The largest size the panel can have while the phone is held as it is, which
// is the window's, and what the shell wants done with it (AngleUwpCapacityMode
// in the header). One value like the panel's above: read between two stores
// when the phone turns it would be 2560x2560, which covers every panel and
// would be taken for the capacity -- three buffers of 25 MB.
std::atomic<int64_t> sPanelCapacity{0};
std::atomic<int> sCapacityMode{0};

// A size as one value, and 0 for a size that is not one.
int64_t PackSize(int32_t width, int32_t height)
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    return (static_cast<int64_t>(width) << 32) | static_cast<uint32_t>(height);
}

bool UnpackSize(int64_t size, LONG *width, LONG *height)
{
    *width  = static_cast<LONG>(size >> 32);
    *height = static_cast<LONG>(size & 0xffffffff);
    return *width > 0 && *height > 0;
}

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
    sPanelSize = PackSize(width, height);
}

extern "C" __declspec(dllexport) void angle_uwp_set_panel_scale(float x, float y)
{
    sPanelScaleX = static_cast<int>((x > 0 ? x : 1.0f) * 1000.0f + 0.5f);
    sPanelScaleY = static_cast<int>((y > 0 ? y : 1.0f) * 1000.0f + 0.5f);
}

// The window's size, at the start and each time the phone is turned, and the
// mode with it. A shell that does not know of this never calls it, and then
// there is no capacity and nothing here differs from what it was.
extern "C" __declspec(dllexport) void angle_uwp_set_panel_capacity(int32_t width,
                                                                   int32_t height,
                                                                   int32_t mode)
{
    sPanelCapacity = PackSize(width, height);
    sCapacityMode  = (mode == 1 || mode == 2) ? mode : 0;
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

bool AngleUwpPanelCapacity(LONG *width, LONG *height)
{
    LONG capacityWidth  = 0;
    LONG capacityHeight = 0;
    if (!UnpackSize(sPanelCapacity.load(), &capacityWidth, &capacityHeight))
    {
        return false;
    }
    if (width)
    {
        *width = capacityWidth;
    }
    if (height)
    {
        *height = capacityHeight;
    }
    return true;
}

int AngleUwpCapacityMode()
{
    // Without a capacity there is nothing for a mode to act on, and a chain
    // made then has to be exactly the chain it always was.
    return AngleUwpPanelCapacity(nullptr, nullptr) ? sCapacityMode.load() : 0;
}

HRESULT AngleUwpSetSourceSize(IDXGISwapChain *chain,
                              UINT width,
                              UINT height,
                              UINT bufferWidth,
                              UINT bufferHeight,
                              int mode,
                              UINT *readBackWidth,
                              UINT *readBackHeight)
{
    if (readBackWidth)
    {
        *readBackWidth = 0;
    }
    if (readBackHeight)
    {
        *readBackHeight = 0;
    }
    if (chain == nullptr || width == 0 || height == 0 || bufferWidth == 0 || bufferHeight == 0)
    {
        return E_INVALIDARG;
    }

    IDXGISwapChain2 *chain2 = nullptr;
    if (FAILED(chain->QueryInterface(__uuidof(IDXGISwapChain2),
                                     reinterpret_cast<void **>(&chain2))) ||
        !chain2)
    {
        return E_NOINTERFACE;
    }

    // Like the matrix in createSwapChain, this is a call on the chain and not
    // on the panel, so it is made on the renderer's thread. Whether Windows
    // takes it there is one of the things the result is noted for.
    HRESULT result = chain2->SetSourceSize(width, height);
    if (SUCCEEDED(result) && mode == 2)
    {
        // A swap chain made with DXGI_SCALING_STRETCH may have its shown part
        // stretched to the size of the buffers; a composition chain has no
        // other size to stretch to. If that is what Windows does, the picture
        // is as much too large as the buffers are larger than the part, and
        // this takes that off again, on top of the composition scale. The
        // matrix replaces the one set when the chain was made; where the part
        // is all of the buffers it is the same matrix.
        const float scaleX          = sPanelScaleX.load() / 1000.0f;
        const float scaleY          = sPanelScaleY.load() / 1000.0f;
        DXGI_MATRIX_3X2_F transform = {};
        transform._11               = (static_cast<float>(width) / bufferWidth) / scaleX;
        transform._22               = (static_cast<float>(height) / bufferHeight) / scaleY;
        result                      = chain2->SetMatrixTransform(&transform);
        if (FAILED(result))
        {
            // The caller's note has only the one result, so which of the two
            // calls it came from is said here.
            char note[256];
            snprintf(note, sizeof(note),
                     "angle: source size %ux%u was taken, its transform %.4fx%.4f failed -> "
                     "0x%08lx",
                     width, height, transform._11, transform._22,
                     static_cast<unsigned long>(result));
            Note(note);
        }
    }

    UINT sourceWidth  = 0;
    UINT sourceHeight = 0;
    if (SUCCEEDED(chain2->GetSourceSize(&sourceWidth, &sourceHeight)))
    {
        if (readBackWidth)
        {
            *readBackWidth = sourceWidth;
        }
        if (readBackHeight)
        {
            *readBackHeight = sourceHeight;
        }
    }
    chain2->Release();
    return result;
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
    LONG width  = 0;
    LONG height = 0;
    if (!UnpackSize(sPanelSize.load(), &width, &height))
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
    // Three, not two. With two, a frame that takes a little longer than one
    // refresh (16.7 ms) has nowhere to go while the previous one waits for
    // the display, so the renderer stands until the next refresh and every
    // frame costs two: fullscreen video ran at exactly 30 frames a second,
    // 30-33 ms each, on frames that needed perhaps 18. The third buffer lets
    // the next frame be drawn while one waits -- Firefox's triple buffering
    // on Windows, for the same reason.
    swapChainDesc.BufferCount = 3;
    swapChainDesc.Scaling     = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.AlphaMode = mHasAlpha ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
    swapChainDesc.Flags     = 0;

    {
        char note[128];
        snprintf(note, sizeof(note),
                 "angle: making a %ux%u swap chain for the panel, %u buffers", width, height,
                 swapChainDesc.BufferCount);
        Note(note);
    }

    IDXGISwapChain1 *swapChain1 = nullptr;
    HRESULT result =
        factory2->CreateSwapChainForComposition(device, &swapChainDesc, nullptr, &swapChain1);
    SafeRelease(factory2);
    if (FAILED(result))
    {
        char note[128];
        snprintf(note, sizeof(note), "angle: making the swap chain failed, 0x%08lx",
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
        snprintf(note, sizeof(note), "angle: giving the swap chain to the panel failed, 0x%08lx",
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
