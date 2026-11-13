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
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cmath>

#include <dxgi1_3.h>
#include <windows.ui.core.h>
#include <windows.ui.xaml.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>

namespace rx {
struct PanelMetrics {
    int32_t width = 0, height = 0;
    int32_t capacityWidth = 0, capacityHeight = 0;
    int capacityMode = 0;
    float scaleX = 1, scaleY = 1;
};
struct PanelMetricsState {
    std::mutex mutex;
    PanelMetrics value;
};
}

namespace
{
std::mutex sWindowMetricsMutex;
std::unordered_map<void *, std::shared_ptr<rx::PanelMetricsState>> sWindowMetrics;
std::shared_ptr<rx::PanelMetricsState> WindowMetrics(void *panel) {
    std::lock_guard<std::mutex> lock(sWindowMetricsMutex);
    auto entry = sWindowMetrics.find(panel);
    return entry != sWindowMetrics.end() ? entry->second : nullptr;
}
rx::PanelMetrics Snapshot(const std::shared_ptr<rx::PanelMetricsState> &state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->value;
}
// The size of the panel, which belongs to the UI thread while everything here
// runs on the renderer's. So it is handed over rather than asked for: the shell
// already measures the room it has and tells the engine, and this is the same
// number arriving by a shorter road. Written rarely, read every resize.
std::atomic<int64_t> sPanelSize{0};
std::atomic<int64_t> sPanelCapacity{0};
std::atomic<int> sCapacityMode{0};

int64_t PackPanelSize(int32_t width, int32_t height)
{
    return width > 0 && height > 0
               ? (static_cast<int64_t>(width) << 32) | static_cast<uint32_t>(height)
               : 0;
}
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

// The host publishes one complete snapshot before native-window construction.
// Renderers retain the state object, so unregistering cannot make an in-flight
// frame start reading another window's defaults or a reused pointer address.
extern "C" __declspec(dllexport) uint32_t angle_uwp_window_metrics_version() { return 1; }
extern "C" __declspec(dllexport) int32_t angle_uwp_set_window_metrics(
    void *panel, int32_t width, int32_t height, float scaleX, float scaleY,
    int32_t capacityWidth, int32_t capacityHeight, int32_t capacityMode) {
    if (!panel || width <= 0 || height <= 0 || capacityWidth < width || capacityHeight < height ||
        !std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX < 0.00001f || scaleY < 0.00001f ||
        (capacityMode != 0 && capacityMode != 1 && capacityMode != 2)) return 0;
    std::lock_guard<std::mutex> registryLock(sWindowMetricsMutex);
    auto &state = sWindowMetrics[panel];
    if (!state) state = std::make_shared<rx::PanelMetricsState>();
    std::lock_guard<std::mutex> stateLock(state->mutex);
    state->value = {width, height, capacityWidth, capacityHeight, capacityMode, scaleX, scaleY};
    return 1;
}
extern "C" __declspec(dllexport) void angle_uwp_release_window_metrics(void *panel) {
    std::lock_guard<std::mutex> lock(sWindowMetricsMutex);
    sWindowMetrics.erase(panel);
}

// Called by the shell, which owns the panel and is the only thing that knows
// how big it is. Exported because it is called from outside this library --
// xul cannot forward it, since xul does not link against ANGLE at all: EGL
// loads it at run time.
extern "C" __declspec(dllexport) void angle_uwp_set_panel_size(int32_t width,
                                                               int32_t height)
{
    sPanelSize = PackPanelSize(width, height);
}

extern "C" __declspec(dllexport) void angle_uwp_set_panel_capacity(int32_t width,
                                                                  int32_t height,
                                                                  int32_t mode)
{
    sCapacityMode = (mode == 1 || mode == 2) ? mode : 0;
    sPanelCapacity = PackPanelSize(width, height);
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

bool AngleUwpPanelCapacity(LONG *width, LONG *height, const NativeWindow11WinRT *window)
{
    if (window) return window->panelCapacity(width, height);
    const int64_t size = sPanelCapacity.load();
    const LONG capacityWidth = static_cast<LONG>(size >> 32);
    const LONG capacityHeight = static_cast<LONG>(size & 0xffffffff);
    if (width) *width = capacityWidth;
    if (height) *height = capacityHeight;
    return capacityWidth > 0 && capacityHeight > 0;
}

int AngleUwpCapacityMode(const NativeWindow11WinRT *window)
{
    if (window) return window->panelCapacityMode();
    return sPanelCapacity.load() != 0 ? sCapacityMode.load() : 0;
}

HRESULT AngleUwpSetSourceSize(IDXGISwapChain *chain, UINT width, UINT height,
                              UINT bufferWidth, UINT bufferHeight, int mode,
                              UINT *readBackWidth, UINT *readBackHeight, const NativeWindow11WinRT *window)
{
    *readBackWidth = 0;
    *readBackHeight = 0;
    if (!chain || width == 0 || height == 0 || width > bufferWidth || height > bufferHeight)
    {
        return E_INVALIDARG;
    }
    Microsoft::WRL::ComPtr<IDXGISwapChain2> chain2;
    HRESULT hr = chain->QueryInterface(IID_PPV_ARGS(&chain2));
    if (FAILED(hr))
    {
        return hr;
    }
    hr = chain2->SetSourceSize(width, height);
    if (SUCCEEDED(hr) && mode == 2)
    {
        float scaleX = sPanelScaleX.load() / 1000.0f;
        float scaleY = sPanelScaleY.load() / 1000.0f;
        if (window) window->panelScale(&scaleX, &scaleY);
        DXGI_MATRIX_3X2_F transform = {};
        transform._11 = (static_cast<float>(width) / bufferWidth) / scaleX;
        transform._22 = (static_cast<float>(height) / bufferHeight) / scaleY;
        hr = chain2->SetMatrixTransform(&transform);
        if (FAILED(hr))
        {
            char note[192];
            snprintf(note, sizeof(note),
                     "angle: diag: source-size transform %.6fx%.6f failed 0x%08lx",
                     transform._11, transform._22, static_cast<unsigned long>(hr));
            Note(note);
        }
    }
    const HRESULT readResult = chain2->GetSourceSize(readBackWidth, readBackHeight);
    if (SUCCEEDED(hr))
    {
        hr = FAILED(readResult) ? readResult
                               : (*readBackWidth == width && *readBackHeight == height ? S_OK : E_FAIL);
    }
    return hr;
}

NativeWindow11WinRT::NativeWindow11WinRT(EGLNativeWindowType window, bool hasAlpha)
    : NativeWindow11(window),
      mMetrics(WindowMetrics(reinterpret_cast<void *>(window))),
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
    const int64_t size = sPanelSize.load();
    const auto own = mMetrics ? Snapshot(mMetrics) : PanelMetrics{};
    const LONG width = mMetrics ? own.width : static_cast<LONG>(size >> 32);
    const LONG height = mMetrics ? own.height : static_cast<LONG>(size & 0xffffffff);
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

bool NativeWindow11WinRT::panelCapacity(LONG *width, LONG *height) const {
    if (!mMetrics) return AngleUwpPanelCapacity(width, height);
    const auto own = Snapshot(mMetrics);
    if (width) *width = own.capacityWidth;
    if (height) *height = own.capacityHeight;
    return own.capacityWidth > 0 && own.capacityHeight > 0;
}
int NativeWindow11WinRT::panelCapacityMode() const {
    return mMetrics ? Snapshot(mMetrics).capacityMode : AngleUwpCapacityMode();
}
void NativeWindow11WinRT::panelScale(float *x, float *y) const {
    const auto own = mMetrics ? Snapshot(mMetrics) : PanelMetrics{};
    *x = mMetrics ? own.scaleX : sPanelScaleX.load() / 1000.0f;
    *y = mMetrics ? own.scaleY : sPanelScaleY.load() / 1000.0f;
}

bool NativeWindow11WinRT::isIconic() const
{
    // A phone has no minimised window. It has a suspended app, which stops
    // drawing altogether rather than drawing into a hidden surface.
    return false;
}

HRESULT NativeWindow11WinRT::detachSwapChainForResize()
{
    if (!mPanel || !mDispatcher)
    {
        return E_INVALIDARG;
    }
    ISwapChainPanelNative *panel = mPanel;
    const HRESULT result = RunOnUIThread(
        [panel]() -> HRESULT { return panel->SetSwapChain(nullptr); }, mDispatcher);
    char note[112];
    snprintf(note, sizeof(note), "angle: diag: panel detached for resize -> 0x%08lx",
             static_cast<unsigned long>(result));
    Note(note);
    return result;
}

HRESULT NativeWindow11WinRT::onSwapChainResized(IDXGISwapChain *swapChain)
{
    if (!mPanel || !mDispatcher || !swapChain)
    {
        return E_INVALIDARG;
    }
    ISwapChainPanelNative *panel = mPanel;
    const HRESULT result = RunOnUIThread(
        [panel, swapChain]() -> HRESULT { return panel->SetSwapChain(swapChain); }, mDispatcher);
    char note[112];
    snprintf(note, sizeof(note), "angle: resized swap chain rebound to panel -> 0x%08lx",
             static_cast<unsigned long>(result));
    Note(note);
    return result;
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
    // The browser surface is an opaque XAML layer. EGL may request an alpha
    // channel for rendering, but its back-buffer alpha is not guaranteed to be
    // premultiplied (or even initialized). Using it for XAML composition can
    // hide otherwise valid RGB content behind the host's background.
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapChainDesc.Flags     = 0;

    Note(mHasAlpha ? "angle: panel alpha mode opaque (EGL alpha requested)"
                   : "angle: panel alpha mode opaque (no EGL alpha)");

    {
        char note[128];
        snprintf(note, sizeof(note),
                 "angle: making an opaque %ux%u swap chain for the panel, %u buffers", width, height,
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
        snprintf(note, sizeof(note), "angle: the swap chain was refused, 0x%08lx",
                 static_cast<unsigned long>(result));
        Note(note);
        return result;
    }
    Note("angle: the swap chain exists, giving it to the panel");
    {
        Microsoft::WRL::ComPtr<IDXGIDevice1> queueDevice;
        UINT latency = 0;
        HRESULT latencyHr = device->QueryInterface(IID_PPV_ARGS(queueDevice.GetAddressOf()));
        if (SUCCEEDED(latencyHr)) latencyHr = queueDevice->GetMaximumFrameLatency(&latency);
        DXGI_SWAP_CHAIN_DESC1 actual = {};
        const HRESULT descHr = swapChain1->GetDesc1(&actual);
        char note[320];
        snprintf(note, sizeof(note),
            "angle: diag: presentation-queue-v1 device=%p panel=%p chain=%p maxLatency=%u hr=0x%08lx desc=0x%08lx buffers=%u effect=%u flags=0x%x size=%ux%u",
            static_cast<void *>(device), static_cast<void *>(mPanel), static_cast<void *>(swapChain1),
            latency, static_cast<unsigned long>(latencyHr), static_cast<unsigned long>(descHr),
            actual.BufferCount, unsigned(actual.SwapEffect), actual.Flags, actual.Width, actual.Height);
        Note(note);
    }

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
        float scaleX, scaleY;
        panelScale(&scaleX, &scaleY);
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
