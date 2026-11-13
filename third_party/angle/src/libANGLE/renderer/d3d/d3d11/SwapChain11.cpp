//
// Copyright 2012 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// SwapChain11.cpp: Implements a back-end specific class for the D3D11 swap chain.

#ifdef UNSAFE_BUFFERS_BUILD
#    pragma allow_unsafe_buffers
#endif

#include "libANGLE/renderer/d3d/d3d11/SwapChain11.h"

#include <cstdio>

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
#    include <atomic>
#    include <mutex>
#    include <d3d11sdklayers.h>
#    include <d3d11on12.h>
#    include <dxgi1_4.h>
#    include "libANGLE/renderer/d3d/d3d11/winrt/NativeWindow11WinRT.h"
#endif

#include <EGL/eglext.h>

#include "libANGLE/features.h"
#include "libANGLE/renderer/d3d/DisplayD3D.h"
#include "libANGLE/renderer/d3d/d3d11/NativeWindow11.h"
#include "libANGLE/renderer/d3d/d3d11/Renderer11.h"
#include "libANGLE/renderer/d3d/d3d11/formatutils11.h"
#include "libANGLE/renderer/d3d/d3d11/renderer11_utils.h"
#include "libANGLE/renderer/d3d/d3d11/texture_format_table.h"
#include "libANGLE/trace.h"

// Precompiled shaders
#include "libANGLE/renderer/d3d/d3d11/shaders/compiled/passthrough2d11vs.h"
#include "libANGLE/renderer/d3d/d3d11/shaders/compiled/passthroughrgba2d11ps.h"
#include "libANGLE/renderer/d3d/d3d11/shaders/compiled/passthroughrgba2dms11ps.h"
#include "libANGLE/renderer/d3d/d3d11/shaders/compiled/resolvecolor2dps.h"

#ifdef ANGLE_ENABLE_KEYEDMUTEX
#    define ANGLE_RESOURCE_SHARE_TYPE D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
#else
#    define ANGLE_RESOURCE_SHARE_TYPE D3D11_RESOURCE_MISC_SHARED
#endif

namespace rx
{

namespace
{
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
// A shared CPU breadcrumb ring preserves ordering across different windows.
// It performs no file I/O until a failure; GPU probes below never wait.
std::mutex sCommandMutex;
char sCommands[128][448] = {};
unsigned long long sCommandSequence = 0;
std::atomic<unsigned> sChainIds{0};

void RememberCommand(const char *line, bool dump)
{
    std::lock_guard<std::mutex> lock(sCommandMutex);
    snprintf(sCommands[sCommandSequence % 128], sizeof(sCommands[0]), "%s", line);
    ++sCommandSequence;
    if (!dump) return;
    AngleUwpNote("angle: commands-v1 history begin (CPU submission order, not GPU completion)");
    const auto start = sCommandSequence > 128 ? sCommandSequence - 128 : 0;
    for (auto i = start; i < sCommandSequence; ++i)
    {
        char note[512];
        snprintf(note, sizeof(note), "angle: breadcrumb seq=%llu %s", i, sCommands[i % 128]);
        AngleUwpNote(note);
    }
    AngleUwpNote("angle: commands-v1 history end");
}

class PanelGraphicsTrace
{
  public:
    PanelGraphicsTrace(ID3D11Device *device, int oldWidth, int oldHeight, int width, int height,
                       const char *operation = "resize")
        : mDevice(device), mOldWidth(oldWidth), mOldHeight(oldHeight), mWidth(width), mHeight(height),
          mOperation(operation), mStart(::GetTickCount64()), mId(++sNextId)
    {
        mDevice->AddRef();
        Stage("begin");
        MemoryBudget();
    }

    ~PanelGraphicsTrace()
    {
        char stage[112];
        snprintf(stage, sizeof(stage), "end egl=0x%04x last=%s", mEglResult, mLastStage);
        Stage(stage);
        if (mEglResult != EGL_SUCCESS || FAILED(mDevice->GetDeviceRemovedReason()))
        {
            MemoryBudget();
            DebugMessages();
            if (FAILED(mDevice->GetDeviceRemovedReason())) DriverRemovalData();
        }
        mDevice->Release();
    }

    void Stage(const char *stage, HRESULT result = S_OK, bool hasResult = false)
    {
        char hr[20] = "n/a";
        if (hasResult)
        {
            snprintf(hr, sizeof(hr), "0x%08lx", static_cast<unsigned long>(result));
        }
        const HRESULT reason = mDevice->GetDeviceRemovedReason();
        char note[384];
        snprintf(note, sizeof(note),
                 "angle: diag: %s#%u tid=%lu +%llums %dx%d->%dx%d %s hr=%s device=0x%08lx/%s",
                 mOperation, mId, static_cast<unsigned long>(::GetCurrentThreadId()),
                 static_cast<unsigned long long>(::GetTickCount64() - mStart),
                 mOldWidth, mOldHeight, mWidth, mHeight, stage, hr,
                 static_cast<unsigned long>(reason), ReasonName(reason));
        AngleUwpNote(note);
        snprintf(mLastStage, sizeof(mLastStage), "%s", stage);
    }

    EGLint Finish(EGLint result)
    {
        mEglResult = result;
        return result;
    }

    void ProbeGpuCompletion(ID3D11DeviceContext *context)
    {
        constexpr ULONGLONG timeoutMs = 250;
        Stage("gpu.drain.begin v4 timeout=250ms");
        if (FAILED(mDevice->GetDeviceRemovedReason()))
        {
            Stage("gpu.drain.device-already-lost");
            return;
        }

        D3D11_QUERY_DESC desc = {};
        desc.Query = D3D11_QUERY_EVENT;
        ID3D11Query *query = nullptr;
        HRESULT hr = mDevice->CreateQuery(&desc, &query);
        Stage("gpu.drain.CreateQuery", hr, true);
        if (FAILED(hr))
        {
            SafeRelease(query);
            Stage("gpu.drain.unavailable; completion-unproven", hr, true);
            return;
        }

        const ULONGLONG start = ::GetTickCount64();
        context->End(query);
        context->Flush();
        BOOL completed = FALSE;
        unsigned polls = 0;
        const char *outcome = "timeout; completion-unproven";
        while (true)
        {
            ++polls;
            hr = context->GetData(query, &completed, sizeof(completed),
                                  D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (FAILED(mDevice->GetDeviceRemovedReason()))
            {
                outcome = "device-lost-before-resize";
                break;
            }
            if (FAILED(hr))
            {
                outcome = "query-failed; completion-unproven";
                break;
            }
            if (hr == S_OK && completed)
            {
                outcome = "complete";
                break;
            }
            if (::GetTickCount64() - start >= timeoutMs)
            {
                break;
            }
            ::Sleep(1);
        }
        char stage[160];
        snprintf(stage, sizeof(stage), "gpu.drain.%s elapsed=%llums polls=%u completed=%d",
                 outcome, static_cast<unsigned long long>(::GetTickCount64() - start),
                 polls, completed != FALSE);
        Stage(stage, hr, true);
        SafeRelease(query);
    }

    void MemoryBudget()
    {
        IDXGIDevice *dxgi = nullptr;
        IDXGIAdapter *adapter = nullptr;
        IDXGIAdapter3 *adapter3 = nullptr;
        HRESULT hr = mDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi));
        if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                                       reinterpret_cast<void **>(&adapter3));
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(hr)) hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        char note[256];
        snprintf(note, sizeof(note),
                 "angle: diag: %s#%u VRAM hr=0x%08lx usage=%lluMB budget=%lluMB reserved=%lluMB flags=0x%x FL=0x%x",
                 mOperation, mId, static_cast<unsigned long>(hr),
                 static_cast<unsigned long long>(info.CurrentUsage / (1024 * 1024)),
                 static_cast<unsigned long long>(info.Budget / (1024 * 1024)),
                 static_cast<unsigned long long>(info.CurrentReservation / (1024 * 1024)),
                 mDevice->GetCreationFlags(), static_cast<unsigned>(mDevice->GetFeatureLevel()));
        AngleUwpNote(note);
        SafeRelease(adapter3);
        SafeRelease(adapter);
        SafeRelease(dxgi);
    }

  private:
    static const char *ReasonName(HRESULT reason)
    {
        switch (reason)
        {
            case S_OK: return "OK";
            case DXGI_ERROR_DEVICE_REMOVED: return "REMOVED";
            case DXGI_ERROR_DEVICE_RESET: return "RESET";
            case DXGI_ERROR_DEVICE_HUNG: return "HUNG";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR";
            case DXGI_ERROR_INVALID_CALL: return "INVALID_CALL";
            case E_OUTOFMEMORY: return "OUT_OF_MEMORY";
            default: return "OTHER";
        }
    }

    void DriverRemovalData()
    {
        // Query existing interfaces only: no D3D12 DLL imports, new device,
        // resource recreation or device-recovery behavior in this observer.
        ID3D11On12Device1 *bridge = nullptr;
        ID3D12Device *underlying = nullptr;
        ID3D12DeviceRemovedExtendedData *dred = nullptr;
        const HRESULT bridgeHr = mDevice->QueryInterface(__uuidof(ID3D11On12Device1),
                                                         reinterpret_cast<void **>(&bridge));
        const HRESULT deviceHr = bridge ? bridge->GetD3D12Device(__uuidof(ID3D12Device),
                                               reinterpret_cast<void **>(&underlying)) : E_NOINTERFACE;
        const HRESULT dredHr = underlying ? underlying->QueryInterface(
            __uuidof(ID3D12DeviceRemovedExtendedData), reinterpret_cast<void **>(&dred)) : E_NOINTERFACE;
        char note[384];
        snprintf(note, sizeof(note),
            "angle: diag: driver-removal-v1 on12=0x%08lx device12=0x%08lx dred=0x%08lx reason12=0x%08lx",
            static_cast<unsigned long>(bridgeHr), static_cast<unsigned long>(deviceHr),
            static_cast<unsigned long>(dredHr),
            static_cast<unsigned long>(underlying ? underlying->GetDeviceRemovedReason() : E_NOINTERFACE));
        AngleUwpNote(note);
        if (dred)
        {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
            const HRESULT crumbsHr = dred->GetAutoBreadcrumbsOutput(&crumbs);
            D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
            const HRESULT faultHr = dred->GetPageFaultAllocationOutput(&fault);
            snprintf(note, sizeof(note),
                "angle: diag: driver-removal-v1 breadcrumbs=0x%08lx present=%d pagefault=0x%08lx gpuVA=0x%llx",
                static_cast<unsigned long>(crumbsHr), int(crumbs.pHeadAutoBreadcrumbNode != nullptr),
                static_cast<unsigned long>(faultHr), static_cast<unsigned long long>(fault.PageFaultVA));
            AngleUwpNote(note);
            auto *node = SUCCEEDED(crumbsHr) ? crumbs.pHeadAutoBreadcrumbNode : nullptr;
            for (unsigned n = 0; node && n < 16; ++n, node = node->pNext)
            {
                const UINT total = node->BreadcrumbCount;
                const UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                const UINT completed = last < total ? last : total;
                snprintf(note, sizeof(note),
                    "angle: diag: driver-removal-v1 list=%u queue=%p commandList=%p total=%u completed=%u counter=%d history=%d",
                    n, static_cast<void *>(node->pCommandQueue), static_cast<void *>(node->pCommandList),
                    total, last, int(node->pLastBreadcrumbValue != nullptr), int(node->pCommandHistory != nullptr));
                AngleUwpNote(note);
                // DRED owns these arrays for the device lifetime. Do not log
                // names, marker strings, shader contents or user data.
                if (!node->pLastBreadcrumbValue || !node->pCommandHistory) continue;
                const UINT retainedBegin = total > 65536 ? total - 65536 : 0;
                const UINT nearCompleted = completed > 3 ? completed - 3 : 0;
                const UINT begin = nearCompleted > retainedBegin ? nearCompleted : retainedBegin;
                const UINT remaining = total - completed;
                const UINT end = completed + (remaining < 5 ? remaining : 5);
                for (UINT i = begin; i < end; ++i)
                {
                    snprintf(note, sizeof(note),
                        "angle: diag: driver-removal-v1 list=%u index=%u operation=%u completed=%d",
                        n, i, unsigned(node->pCommandHistory[i % 65536]), int(i < completed));
                    AngleUwpNote(note);
                }
            }
            if (SUCCEEDED(faultHr))
            {
                const D3D12_DRED_ALLOCATION_NODE *heads[] = {
                    fault.pHeadExistingAllocationNode, fault.pHeadRecentFreedAllocationNode};
                for (unsigned kind = 0; kind < 2; ++kind)
                {
                    auto *allocation = heads[kind];
                    for (unsigned n = 0; allocation && n < 8; ++n, allocation = allocation->pNext)
                    {
                        snprintf(note, sizeof(note),
                            "angle: diag: driver-removal-v1 allocation=%s index=%u type=%u",
                            kind == 0 ? "existing" : "recently-freed", n, unsigned(allocation->AllocationType));
                        AngleUwpNote(note);
                    }
                }
            }
        }
        SafeRelease(dred);
        SafeRelease(underlying);
        SafeRelease(bridge);
    }

    void DebugMessages()
    {
        ID3D11InfoQueue *queue = nullptr;
        const HRESULT hr = mDevice->QueryInterface(__uuidof(ID3D11InfoQueue),
                                                   reinterpret_cast<void **>(&queue));
        if (FAILED(hr))
        {
            char note[128];
            snprintf(note, sizeof(note), "angle: diag: %s#%u D3D11 InfoQueue unavailable 0x%08lx",
                     mOperation, mId, static_cast<unsigned long>(hr));
            AngleUwpNote(note);
            return;
        }
        const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
        char summary[128];
        snprintf(summary, sizeof(summary), "angle: diag: %s#%u D3D11 InfoQueue messages=%llu",
                 mOperation, mId, static_cast<unsigned long long>(count));
        AngleUwpNote(summary);
        for (UINT64 i = count > 32 ? count - 32 : 0; i < count; ++i)
        {
            alignas(D3D11_MESSAGE) unsigned char storage[2048];
            SIZE_T bytes = sizeof(storage);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage);
            if (SUCCEEDED(queue->GetMessage(i, message, &bytes)))
            {
                char note[640];
                snprintf(note, sizeof(note), "angle: diag: %s#%u D3D11 id=%u severity=%u %.480s",
                         mOperation, mId, static_cast<unsigned>(message->ID),
                         static_cast<unsigned>(message->Severity),
                         message->pDescription ? message->pDescription : "(no description)");
                AngleUwpNote(note);
            }
        }
        queue->Release();
    }

    ID3D11Device *mDevice;
    int mOldWidth, mOldHeight, mWidth, mHeight;
    const char *mOperation;
    ULONGLONG mStart;
    unsigned mId;
    EGLint mEglResult = 0;
    char mLastStage[80] = "begin";
    static std::atomic<unsigned> sNextId;
};

std::atomic<unsigned> PanelGraphicsTrace::sNextId{0};
#endif

// To avoid overflow in QPC to Microseconds calculations, since we multiply
// by kMicrosecondsPerSecond, then the QPC value should not exceed
// (2^63 - 1) / 1E6. If it exceeds that threshold, we divide then multiply.
static constexpr int64_t kQPCOverflowThreshold  = 0x8637BD05AF7;
static constexpr int64_t kMicrosecondsPerSecond = 1000000;

bool NeedsOffscreenTexture(Renderer11 *renderer, NativeWindow11 *nativeWindow, EGLint orientation)
{
    // We don't need an offscreen texture if either orientation = INVERT_Y,
    // or present path fast is enabled and we're not rendering onto an offscreen surface.
    return orientation != EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE &&
           !(renderer->presentPathFastEnabled() && nativeWindow->getNativeWindow());
}
}  // anonymous namespace

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
// Called by Gecko at the first reset detection, before it releases cached GPU
// resources. Optional dynamic export, not an import required by the engine.
extern "C" __declspec(dllexport) void angle_uwp_dump_device_removed(void *device)
{
    if (!device) return;
    auto *d3d = static_cast<ID3D11Device *>(device);
    if (SUCCEEDED(d3d->GetDeviceRemovedReason())) return;
    PanelGraphicsTrace trace(d3d, 0, 0, 0, 0, "reset-before-cleanup");
    trace.Finish(EGL_CONTEXT_LOST);
}
#endif

SwapChain11::SwapChain11(Renderer11 *renderer,
                         NativeWindow11 *nativeWindow,
                         HANDLE shareHandle,
                         IUnknown *d3dTexture,
                         GLenum backBufferFormat,
                         GLenum depthBufferFormat,
                         EGLint orientation,
                         EGLint samples)
    : SwapChainD3D(shareHandle, d3dTexture, backBufferFormat, depthBufferFormat),
      mRenderer(renderer),
      mWidth(-1),
      mHeight(-1),
      mOrientation(orientation),
      mAppCreatedShareHandle(mShareHandle != nullptr),
      mSwapInterval(0),
      mPassThroughResourcesInit(false),
      mNativeWindow(nativeWindow),
      mFirstSwap(true),
      mSwapChain(nullptr),
      mSwapChain1(nullptr),
      mKeyedMutex(nullptr),
      mBackBufferTexture(),
      mBackBufferRTView(),
      mBackBufferSRView(),
      mNeedsOffscreenTexture(NeedsOffscreenTexture(renderer, nativeWindow, orientation)),
      mOffscreenTexture(),
      mOffscreenRTView(),
      mOffscreenSRView(),
      mNeedsOffscreenTextureCopy(false),
      mOffscreenTextureCopyForSRV(),
      mDepthStencilTexture(),
      mDepthStencilDSView(),
      mDepthStencilSRView(),
      mQuadVB(),
      mPassThroughSampler(),
      mPassThroughIL(),
      mPassThroughVS(),
      mPassThroughOrResolvePS(),
      mPassThroughRS(),
      mColorRenderTarget(this, renderer, false),
      mDepthStencilRenderTarget(this, renderer, true),
      mEGLSamples(samples)
{
    // Check that if present path fast is active then we're using the default orientation
    ASSERT(!mRenderer->presentPathFastEnabled() || orientation == 0);

    // Get the performance counter
    LARGE_INTEGER counterFreqency = {};
    BOOL success                  = QueryPerformanceFrequency(&counterFreqency);
    ASSERT(success);

    mQPCFrequency = counterFreqency.QuadPart;
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    mDiagnosticId = ++sChainIds;
    traceCommand("created", S_OK, true);
#endif
}

SwapChain11::~SwapChain11()
{
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    traceCommand("destroy");
    for (auto &probe : mProgress) SafeRelease(probe.query);
#endif
    release();
}

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
void SwapChain11::traceCommand(const char *stage, HRESULT result, bool resources)
{
    auto *device = mRenderer->getDevice();
    const HRESULT reason = device->GetDeviceRemovedReason();
    const bool lost = FAILED(reason) && !mDiagnosticLost;
    mDiagnosticLost |= FAILED(reason);
    char note[448];
    snprintf(note, sizeof(note),
        "tick=%llu chain=%u frame=%u tid=%lu stage=%s self=%p panel=%p swap=%p "
        "device=%p context=%p size=%dx%d buffers=%dx%d copy=%d orientation=0x%x interval=%u "
        "hr=0x%08lx reason=0x%08lx",
        static_cast<unsigned long long>(::GetTickCount64()), mDiagnosticId,
        mDiagnosticFrame, static_cast<unsigned long>(::GetCurrentThreadId()), stage,
        static_cast<void *>(this), reinterpret_cast<void *>(mNativeWindow->getNativeWindow()),
        static_cast<void *>(mSwapChain), static_cast<void *>(device),
        static_cast<void *>(mRenderer->getDeviceContext()), mWidth, mHeight,
        mBufferWidth, mBufferHeight, int(mNeedsOffscreenTexture), unsigned(mOrientation),
        mSwapInterval, static_cast<unsigned long>(result), static_cast<unsigned long>(reason));
    RememberCommand(note, lost);
    if (lost || FAILED(result) || mDiagnosticFrame <= 3)
    {
        char output[480];
        snprintf(output, sizeof(output), "angle: commands-v1 %s", note);
        AngleUwpNote(output);
    }
    if (lost || resources)
    {
        const TextureHelper11 *textures[] = {&mBackBufferTexture, &mOffscreenTexture};
        for (unsigned i = 0; i < 2; ++i)
        {
            // This is a descriptor of our own GPU resource, never file/user data.
            auto *texture = textures[i]->get();
            if (!texture) continue;
            D3D11_TEXTURE2D_DESC desc = {};
            textures[i]->getDesc(&desc);
            snprintf(note, sizeof(note),
                "angle: commands-v1 chain=%u resource=%s ptr=%p %ux%u format=%u "
                "samples=%u quality=%u mip=%u array=%u bind=0x%x misc=0x%x flags=0x%x",
                mDiagnosticId, i == 0 ? "backbuffer" : "offscreen", static_cast<void *>(texture),
                desc.Width, desc.Height, unsigned(desc.Format), desc.SampleDesc.Count,
                desc.SampleDesc.Quality, desc.MipLevels, desc.ArraySize, desc.BindFlags,
                desc.MiscFlags, device->GetCreationFlags());
            AngleUwpNote(note);
        }
    }
    if (lost)
    {
        PanelGraphicsTrace failure(device, mWidth, mHeight, mWidth, mHeight, "command-device-loss");
        failure.Stage(stage, result, true);
        failure.Finish(EGL_CONTEXT_LOST);
    }
}

void SwapChain11::probeProgress(const char *stage)
{
    auto *device = mRenderer->getDevice();
    auto *context = mRenderer->getDeviceContext();
    for (auto &probe : mProgress)
    {
        if (!probe.query) continue;
        BOOL completed = FALSE;
        const HRESULT hr = context->GetData(probe.query, &completed, sizeof(completed),
                                            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr != S_OK && !FAILED(hr)) continue;
        char label[160];
        snprintf(label, sizeof(label), "gpu-%s checkpoint=%s submitted-frame=%u completed=%d elapsed=%llums",
                 FAILED(hr) ? "query-failed" : "query-result", probe.stage, probe.frame,
                 int(completed != FALSE),
                 static_cast<unsigned long long>(::GetTickCount64() - probe.submitted));
        traceCommand(label, hr);
        SafeRelease(probe.query);
    }
    if (FAILED(device->GetDeviceRemovedReason()) ||
        (mDiagnosticFrame > 48 && mDiagnosticFrame % 120 != 0)) return;
    for (auto &probe : mProgress)
    {
        if (probe.query) continue;
        D3D11_QUERY_DESC desc = {};
        desc.Query = D3D11_QUERY_EVENT;
        const HRESULT hr = device->CreateQuery(&desc, &probe.query);
        if (FAILED(hr)) { traceCommand("CreateQuery-failed", hr); return; }
        probe.stage = stage;
        probe.frame = mDiagnosticFrame;
        probe.submitted = ::GetTickCount64();
        context->End(probe.query);
        traceCommand(stage);
        return;
    }
    traceCommand("gpu-probe-capacity-pending");
}
#endif

void SwapChain11::release()
{
    // TODO(jmadill): Should probably signal that the RenderTarget is dirty.

    SafeRelease(mSwapChain1);
    SafeRelease(mSwapChain);
    SafeRelease(mKeyedMutex);
    mBackBufferTexture.reset();
    mBackBufferRTView.reset();
    mBackBufferSRView.reset();
    mOffscreenTexture.reset();
    mOffscreenRTView.reset();
    mOffscreenSRView.reset();
    mDepthStencilTexture.reset();
    mDepthStencilDSView.reset();
    mDepthStencilSRView.reset();
    mQuadVB.reset();
    mPassThroughSampler.reset();
    mPassThroughIL.reset();
    mPassThroughVS.reset();
    mPassThroughOrResolvePS.reset();
    mPassThroughRS.reset();
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    mPassThroughResourcesInit = false;
    mBufferWidth = 0;
    mBufferHeight = 0;
    mSourceSizeDirty = false;
#endif

    if (!mAppCreatedShareHandle)
    {
        mShareHandle = nullptr;
    }
}

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
void SwapChain11::bufferSizeFor(EGLint width, EGLint height,
                                EGLint *bufferWidth, EGLint *bufferHeight) const
{
    *bufferWidth = width;
    *bufferHeight = height;
    LONG capacityWidth = 0;
    LONG capacityHeight = 0;
    if (mCapacityMode != 0 && !mSourceSizeBroken &&
        AngleUwpPanelCapacity(&capacityWidth, &capacityHeight,
                              static_cast<const NativeWindow11WinRT *>(mNativeWindow)) &&
        capacityWidth >= width && capacityHeight >= height)
    {
        *bufferWidth = capacityWidth;
        *bufferHeight = capacityHeight;
    }
}

EGLint SwapChain11::updateSourceSize(DisplayD3D *displayD3D)
{
    UINT readWidth = 0;
    UINT readHeight = 0;
    const HRESULT hr = AngleUwpSetSourceSize(mSwapChain, mWidth, mHeight,
                                             mBufferWidth, mBufferHeight, mCapacityMode,
                                             &readWidth, &readHeight,
                                             static_cast<const NativeWindow11WinRT *>(mNativeWindow));
    const HRESULT reason = mRenderer->getDevice()->GetDeviceRemovedReason();
    if (mSourceSizeNotes++ < 40 || FAILED(hr) || FAILED(reason))
    {
        char note[320];
        snprintf(note, sizeof(note),
                 "angle: diag: source-size v6 %dx%d of buffers %dx%d mode=%d hr=0x%08lx "
                 "readback=%ux%u device=0x%08lx copy=%d",
                 mWidth, mHeight, mBufferWidth, mBufferHeight, mCapacityMode,
                 static_cast<unsigned long>(hr), readWidth, readHeight,
                 static_cast<unsigned long>(reason), mNeedsOffscreenTexture ? 1 : 0);
        AngleUwpNote(note);
    }
    if (FAILED(reason) || d3d11::isDeviceLostError(hr))
    {
        return EGL_CONTEXT_LOST;
    }
    if (SUCCEEDED(hr))
    {
        mSourceSizeDirty = false;
        return EGL_SUCCESS;
    }

    // A refused crop must not leave a larger buffer stretched across the panel.
    mSourceSizeBroken = true;
    const EGLint width = mWidth;
    const EGLint height = mHeight;
    AngleUwpNote("angle: diag: source-size refused; rebuilding exact-size chain without capacity");
    const HRESULT detached =
        static_cast<NativeWindow11WinRT *>(mNativeWindow)->detachSwapChainForResize();
    if (FAILED(detached))
    {
        return d3d11::isDeviceLostError(detached) ? EGL_CONTEXT_LOST : EGL_BAD_NATIVE_WINDOW;
    }
    mCapacityMode = 0;
    auto *stateManager = mRenderer->getStateManager();
    stateManager->setRenderTarget(nullptr, nullptr);
    stateManager->setShaderResource(gl::ShaderType::Fragment, 0, nullptr);
    release();
    mRenderer->getDeviceContext()->Flush();
    mNextSourceSizeRemake = 0;
    const EGLint result = remakeAfterSourceSizeFailure(displayD3D, width, height);
    return result == EGL_SUCCESS ? EGL_BAD_ALLOC : result;
}

EGLint SwapChain11::remakeAfterSourceSizeFailure(DisplayD3D *displayD3D,
                                                EGLint width, EGLint height)
{
    if (FAILED(mRenderer->getDevice()->GetDeviceRemovedReason()))
    {
        return EGL_CONTEXT_LOST;
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now < mNextSourceSizeRemake)
    {
        return EGL_BAD_ALLOC;
    }
    mNextSourceSizeRemake = now + 1000;
    const EGLint result = reset(displayD3D, width, height, mSwapInterval);
    char note[160];
    snprintf(note, sizeof(note), "angle: diag: exact-size chain rebuild %dx%d egl=0x%04x",
             width, height, result);
    AngleUwpNote(note);
    if (result != EGL_SUCCESS)
    {
        release();
    }
    return result;
}
#endif

void SwapChain11::releaseOffscreenColorBuffer()
{
    mOffscreenTexture.reset();
    mOffscreenRTView.reset();
    mOffscreenSRView.reset();
    mNeedsOffscreenTextureCopy = false;
    mOffscreenTextureCopyForSRV.reset();
}

void SwapChain11::releaseOffscreenDepthBuffer()
{
    mDepthStencilTexture.reset();
    mDepthStencilDSView.reset();
    mDepthStencilSRView.reset();
}

EGLint SwapChain11::resetOffscreenBuffers(DisplayD3D *displayD3D,
                                          int backbufferWidth,
                                          int backbufferHeight)
{
    if (mNeedsOffscreenTexture)
    {
        EGLint result = resetOffscreenColorBuffer(displayD3D, backbufferWidth, backbufferHeight);
        if (result != EGL_SUCCESS)
        {
            return result;
        }
    }

    int depthWidth = backbufferWidth;
    int depthHeight = backbufferHeight;
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    if (!mNeedsOffscreenTexture && mBufferWidth > 0 && mBufferHeight > 0)
    {
        depthWidth = mBufferWidth;
        depthHeight = mBufferHeight;
    }
#endif
    EGLint result = resetOffscreenDepthBuffer(displayD3D, depthWidth, depthHeight);
    if (result != EGL_SUCCESS)
    {
        return result;
    }

    mWidth  = backbufferWidth;
    mHeight = backbufferHeight;

    return EGL_SUCCESS;
}

EGLint SwapChain11::resetOffscreenColorBuffer(DisplayD3D *displayD3D,
                                              int backbufferWidth,
                                              int backbufferHeight)
{
    ASSERT(mNeedsOffscreenTexture);

    ANGLE_TRACE_EVENT0("gpu.angle", "SwapChain11::resetOffscreenTexture");
    ID3D11Device *device = mRenderer->getDevice();

    ASSERT(device != nullptr);

    // D3D11 does not allow zero size textures
    ASSERT(backbufferWidth >= 1);
    ASSERT(backbufferHeight >= 1);

    // Preserve the render target content
    TextureHelper11 previousOffscreenTexture(std::move(mOffscreenTexture));
    const int previousWidth  = mWidth;
    const int previousHeight = mHeight;

    releaseOffscreenColorBuffer();

    const d3d11::Format &backbufferFormatInfo =
        d3d11::Format::Get(mOffscreenRenderTargetFormat, mRenderer->getRenderer11DeviceCaps());
    D3D11_TEXTURE2D_DESC offscreenTextureDesc = {};

    // If the app passed in a share handle or D3D texture, open the resource
    // See EGL_ANGLE_d3d_share_handle_client_buffer and EGL_ANGLE_d3d_texture_client_buffer
    if (mAppCreatedShareHandle || mD3DTexture != nullptr)
    {
        if (mAppCreatedShareHandle)
        {
            ID3D11Resource *tempResource11;
            HRESULT result = device->OpenSharedResource(mShareHandle, __uuidof(ID3D11Resource),
                                                        (void **)&tempResource11);
            if (FAILED(result) && mRenderer->getDevice1())
            {
                result = mRenderer->getDevice1()->OpenSharedResource1(
                    mShareHandle, __uuidof(ID3D11Resource), (void **)&tempResource11);
            }

            if (FAILED(result))
            {
                ERR() << "Could not open shared handle. " << gl::FmtHR(result);
                release();
                return EGL_BAD_SURFACE;
            }

            mOffscreenTexture.set(d3d11::DynamicCastComObject<ID3D11Texture2D>(tempResource11),
                                  backbufferFormatInfo);
            SafeRelease(tempResource11);
        }
        else if (mD3DTexture != nullptr)
        {
            mOffscreenTexture.set(d3d11::DynamicCastComObject<ID3D11Texture2D>(mD3DTexture),
                                  backbufferFormatInfo);
        }
        else
        {
            UNREACHABLE();
        }
        ASSERT(mOffscreenTexture.valid());
        mOffscreenTexture.getDesc(&offscreenTextureDesc);

        // Fail if the offscreen texture is not renderable.
        if ((offscreenTextureDesc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0)
        {
            ERR() << "Could not use provided offscreen texture, texture not renderable.";
            release();
            return EGL_BAD_SURFACE;
        }
    }
    else
    {
        const bool useSharedResource =
            !mNativeWindow->getNativeWindow() && mRenderer->getShareHandleSupport();

        offscreenTextureDesc.Width              = backbufferWidth;
        offscreenTextureDesc.Height             = backbufferHeight;
        offscreenTextureDesc.Format             = backbufferFormatInfo.texFormat;
        offscreenTextureDesc.MipLevels          = 1;
        offscreenTextureDesc.ArraySize          = 1;
        offscreenTextureDesc.SampleDesc.Count   = getD3DSamples();
        offscreenTextureDesc.SampleDesc.Quality = 0;
        offscreenTextureDesc.Usage              = D3D11_USAGE_DEFAULT;
        offscreenTextureDesc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        offscreenTextureDesc.CPUAccessFlags = 0;
        offscreenTextureDesc.MiscFlags      = useSharedResource ? ANGLE_RESOURCE_SHARE_TYPE : 0;

        angle::Result result = mRenderer->allocateTexture(displayD3D, offscreenTextureDesc,
                                                          backbufferFormatInfo, &mOffscreenTexture);
        if (result == angle::Result::Stop)
        {
            ERR() << "Could not create offscreen texture, " << displayD3D->getStoredErrorString();
            release();
            return EGL_BAD_ALLOC;
        }

        mOffscreenTexture.setInternalName("OffscreenBackBufferTexture");

        // EGL_ANGLE_surface_d3d_texture_2d_share_handle requires that we store a share handle for
        // the client
        if (useSharedResource)
        {
            IDXGIResource *offscreenTextureResource = nullptr;
            HRESULT hr                              = mOffscreenTexture.get()->QueryInterface(
                __uuidof(IDXGIResource), (void **)&offscreenTextureResource);

            // Fall back to no share handle on failure
            if (FAILED(hr))
            {
                ERR() << "Could not query offscreen texture resource, " << gl::FmtHR(hr);
            }
            else
            {
                hr = offscreenTextureResource->GetSharedHandle(&mShareHandle);
                SafeRelease(offscreenTextureResource);

                if (FAILED(hr))
                {
                    mShareHandle = nullptr;
                    ERR() << "Could not get offscreen texture shared handle, " << gl::FmtHR(hr);
                }
            }
        }
    }

    // This may return null if the original texture was created without a keyed mutex.
    mKeyedMutex = d3d11::DynamicCastComObject<IDXGIKeyedMutex>(mOffscreenTexture.get());

    D3D11_RENDER_TARGET_VIEW_DESC offscreenRTVDesc;
    offscreenRTVDesc.Format = backbufferFormatInfo.rtvFormat;
    offscreenRTVDesc.ViewDimension =
        (mEGLSamples <= 1) ? D3D11_RTV_DIMENSION_TEXTURE2D : D3D11_RTV_DIMENSION_TEXTURE2DMS;
    offscreenRTVDesc.Texture2D.MipSlice = 0;

    angle::Result result = mRenderer->allocateResource(displayD3D, offscreenRTVDesc,
                                                       mOffscreenTexture.get(), &mOffscreenRTView);
    if (result == angle::Result::Stop)
    {
        ERR() << "Could not create offscreen back buffer render target, "
              << displayD3D->getStoredErrorString();
        release();
        return EGL_BAD_ALLOC;
    }
    mOffscreenRTView.setInternalName("OffscreenBackBufferRenderTarget");

    D3D11_SHADER_RESOURCE_VIEW_DESC offscreenSRVDesc;
    offscreenSRVDesc.Format = backbufferFormatInfo.srvFormat;
    offscreenSRVDesc.ViewDimension =
        (mEGLSamples <= 1) ? D3D11_SRV_DIMENSION_TEXTURE2D : D3D11_SRV_DIMENSION_TEXTURE2DMS;
    offscreenSRVDesc.Texture2D.MostDetailedMip = 0;
    offscreenSRVDesc.Texture2D.MipLevels       = static_cast<UINT>(-1);

    if (offscreenTextureDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
    {
        result = mRenderer->allocateResource(displayD3D, offscreenSRVDesc, mOffscreenTexture.get(),
                                             &mOffscreenSRView);
        if (result == angle::Result::Stop)
        {
            ERR() << "Could not create offscreen back buffer shader resource, "
                  << displayD3D->getStoredErrorString();
            release();
            return EGL_BAD_ALLOC;
        }
        mOffscreenSRView.setInternalName("OffscreenBackBufferShaderResource");
    }
    else
    {
        // Special case for external textures that cannot support sampling. Since internally we
        // assume our SwapChain is always readable, we make a copy texture that is compatible.
        mNeedsOffscreenTextureCopy = true;
    }

    if (previousOffscreenTexture.valid())
    {
        D3D11_BOX sourceBox = {};
        sourceBox.left      = 0;
        sourceBox.right     = std::min(previousWidth, backbufferWidth);
        sourceBox.top       = std::max(previousHeight - backbufferHeight, 0);
        sourceBox.bottom    = previousHeight;
        sourceBox.front     = 0;
        sourceBox.back      = 1;

        ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
        const int yoffset                  = std::max(backbufferHeight - previousHeight, 0);
        deviceContext->CopySubresourceRegion(mOffscreenTexture.get(), 0, 0, yoffset, 0,
                                             previousOffscreenTexture.get(), 0, &sourceBox);

        // The panel is presented after resetOffscreenBuffers commits its new size.
#if !defined(ANGLE_ENABLE_WINDOWS_UWP)
        if (mSwapChain)
        {
            const EGLint result = swapRect(displayD3D, 0, 0, backbufferWidth, backbufferHeight);
            if (result != EGL_SUCCESS)
            {
                return result;
            }
        }
#endif
    }

    return EGL_SUCCESS;
}

EGLint SwapChain11::resetOffscreenDepthBuffer(DisplayD3D *displayD3D,
                                              int backbufferWidth,
                                              int backbufferHeight)
{
    releaseOffscreenDepthBuffer();

    if (mDepthBufferFormat != GL_NONE)
    {
        const d3d11::Format &depthBufferFormatInfo =
            d3d11::Format::Get(mDepthBufferFormat, mRenderer->getRenderer11DeviceCaps());

        D3D11_TEXTURE2D_DESC depthStencilTextureDesc;
        depthStencilTextureDesc.Width            = backbufferWidth;
        depthStencilTextureDesc.Height           = backbufferHeight;
        depthStencilTextureDesc.Format           = depthBufferFormatInfo.texFormat;
        depthStencilTextureDesc.MipLevels        = 1;
        depthStencilTextureDesc.ArraySize        = 1;
        depthStencilTextureDesc.SampleDesc.Count = getD3DSamples();
        depthStencilTextureDesc.Usage            = D3D11_USAGE_DEFAULT;
        depthStencilTextureDesc.BindFlags        = D3D11_BIND_DEPTH_STENCIL;

        // If there is a multisampled offscreen color texture, the offscreen depth-stencil texture
        // must also have the same quality value.
        if (mOffscreenTexture.valid() && getD3DSamples() > 1)
        {
            D3D11_TEXTURE2D_DESC offscreenTextureDesc = {};
            mOffscreenTexture.getDesc(&offscreenTextureDesc);
            depthStencilTextureDesc.SampleDesc.Quality = offscreenTextureDesc.SampleDesc.Quality;
        }
        else
        {
            depthStencilTextureDesc.SampleDesc.Quality = 0;
        }

        // Only create an SRV if it is supported
        bool depthStencilSRV =
            depthBufferFormatInfo.srvFormat != DXGI_FORMAT_UNKNOWN &&
            (mRenderer->getRenderer11DeviceCaps().supportsMultisampledDepthStencilSRVs ||
             depthStencilTextureDesc.SampleDesc.Count <= 1);
        if (depthStencilSRV)
        {
            depthStencilTextureDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        }

        depthStencilTextureDesc.CPUAccessFlags = 0;
        depthStencilTextureDesc.MiscFlags      = 0;

        angle::Result result = mRenderer->allocateTexture(
            displayD3D, depthStencilTextureDesc, depthBufferFormatInfo, &mDepthStencilTexture);
        if (result == angle::Result::Stop)
        {
            ERR() << "Could not create depthstencil surface for new swap chain, "
                  << displayD3D->getStoredErrorString();
            release();
            return EGL_BAD_ALLOC;
        }
        mDepthStencilTexture.setInternalName("OffscreenDepthStencilTexture");

        D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilDesc;
        depthStencilDesc.Format = depthBufferFormatInfo.dsvFormat;
        depthStencilDesc.ViewDimension =
            (mEGLSamples <= 1) ? D3D11_DSV_DIMENSION_TEXTURE2D : D3D11_DSV_DIMENSION_TEXTURE2DMS;
        depthStencilDesc.Flags              = 0;
        depthStencilDesc.Texture2D.MipSlice = 0;

        result = mRenderer->allocateResource(displayD3D, depthStencilDesc,
                                             mDepthStencilTexture.get(), &mDepthStencilDSView);
        ASSERT(result != angle::Result::Stop);
        mDepthStencilDSView.setInternalName("OffscreenDSV");

        if (depthStencilSRV)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC depthStencilSRVDesc;
            depthStencilSRVDesc.Format                    = depthBufferFormatInfo.srvFormat;
            depthStencilSRVDesc.ViewDimension             = (mEGLSamples <= 1)
                                                                ? D3D11_SRV_DIMENSION_TEXTURE2D
                                                                : D3D11_SRV_DIMENSION_TEXTURE2DMS;
            depthStencilSRVDesc.Texture2D.MostDetailedMip = 0;
            depthStencilSRVDesc.Texture2D.MipLevels       = static_cast<UINT>(-1);

            result = mRenderer->allocateResource(displayD3D, depthStencilSRVDesc,
                                                 mDepthStencilTexture.get(), &mDepthStencilSRView);
            ASSERT(result != angle::Result::Stop);
            mDepthStencilSRView.setInternalName("OffscreenDepthStencilSRV");
        }
    }

    return EGL_SUCCESS;
}

EGLint SwapChain11::resize(DisplayD3D *displayD3D, EGLint backbufferWidth, EGLint backbufferHeight)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "SwapChain11::resize");
    ID3D11Device *device = mRenderer->getDevice();

    if (device == nullptr)
    {
        return EGL_BAD_ACCESS;
    }

    // EGL allows creating a surface with 0x0 dimension, however, DXGI does not like 0x0 swapchains
    if (backbufferWidth < 1 || backbufferHeight < 1)
    {
        return EGL_SUCCESS;
    }

    // Don't resize unnecessarily
    if (mWidth == backbufferWidth && mHeight == backbufferHeight)
    {
        return EGL_SUCCESS;
    }

    // Can only call resize if we have already created our swap buffer and resources
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    if (!mSwapChain && mSourceSizeBroken)
    {
        return remakeAfterSourceSizeFailure(displayD3D, backbufferWidth, backbufferHeight);
    }
    if (mCapacityMode != 0 && !mSourceSizeBroken && mSwapChain &&
        backbufferWidth <= mBufferWidth && backbufferHeight <= mBufferHeight)
    {
        const EGLint oldWidth = mWidth;
        const EGLint oldHeight = mHeight;
        if (mNeedsOffscreenTexture)
        {
            const EGLint result = resetOffscreenBuffers(displayD3D, backbufferWidth, backbufferHeight);
            if (result != EGL_SUCCESS)
            {
                return result;
            }
        }
        else
        {
            mWidth = backbufferWidth;
            mHeight = backbufferHeight;
            if (backbufferWidth > oldWidth || backbufferHeight > oldHeight)
            {
                const float blank[4] = {};
                mRenderer->getDeviceContext()->ClearRenderTargetView(mBackBufferRTView.get(), blank);
            }
        }
        mFirstSwap = true;
        mSourceSizeDirty = true;
        char note[224];
        snprintf(note, sizeof(note),
                 "angle: diag: resize v6 %dx%d->%dx%d by source size; buffers stay %dx%d; no ResizeBuffers",
                 oldWidth, oldHeight, mWidth, mHeight, mBufferWidth, mBufferHeight);
        AngleUwpNote(note);
        return EGL_SUCCESS;
    }
#endif
    ASSERT(mSwapChain && mBackBufferTexture.valid() && mBackBufferRTView.valid() &&
           mBackBufferSRView.valid());

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    // How long the keyboard costs here: every time it opens or closes, the
    // panel changes size and the swap chain and offscreen buffers are made
    // again.
    const ULONGLONG resizeStart = ::GetTickCount64();
    PanelGraphicsTrace trace(device, mWidth, mHeight, backbufferWidth, backbufferHeight);
    const auto finish = [&trace](EGLint result) { return trace.Finish(result); };
    trace.ProbeGpuCompletion(mRenderer->getDeviceContext());
    const HRESULT beforeResizeReason = device->GetDeviceRemovedReason();
    if (FAILED(beforeResizeReason))
    {
        trace.Stage("resize.skipped; device-lost-before-buffer-release", beforeResizeReason, true);
        release();
        return finish(EGL_CONTEXT_LOST);
    }
    trace.Stage("unbind.begin");
    StateManager11 *stateManager = mRenderer->getStateManager();
    stateManager->setRenderTarget(nullptr, nullptr);
    stateManager->setShaderResource(gl::ShaderType::Fragment, 0, nullptr);
    trace.Stage("unbind.end");
#else
    const auto finish = [](EGLint result) { return result; };
#endif

    mBackBufferTexture.reset();
    mBackBufferRTView.reset();
    mBackBufferSRView.reset();

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    trace.Stage("backbuffers.released; Flush.begin");
    mRenderer->getDeviceContext()->Flush();
    trace.Stage("Flush.end; GetDesc.begin");
#endif

    // Resize swap chain
    DXGI_SWAP_CHAIN_DESC desc;
    HRESULT hr = mSwapChain->GetDesc(&desc);
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    trace.Stage("GetDesc.end", hr, true);
#endif
    if (FAILED(hr))
    {
        ERR() << "Error reading swap chain description, " << gl::FmtHR(hr);
        release();
        return finish(EGL_BAD_ALLOC);
    }

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    trace.Stage("panel.detach.begin v5");
    const HRESULT detachResult =
        static_cast<NativeWindow11WinRT *>(mNativeWindow)->detachSwapChainForResize();
    trace.Stage("panel.detach.end", detachResult, true);
    const HRESULT detachDeviceReason = device->GetDeviceRemovedReason();
    if (FAILED(detachResult) || FAILED(detachDeviceReason))
    {
        trace.Stage("resize.skipped; panel-detach-failed-or-device-lost");
        release();
        return finish(FAILED(detachDeviceReason) || d3d11::isDeviceLostError(detachResult)
                          ? EGL_CONTEXT_LOST
                          : EGL_BAD_NATIVE_WINDOW);
    }
    {
        char stage[160];
        snprintf(stage, sizeof(stage), "ResizeBuffers.begin buffers=%u format=%u usage=0x%x flags=0x%x",
                 desc.BufferCount, static_cast<unsigned>(desc.BufferDesc.Format),
                 desc.BufferUsage, desc.Flags);
        trace.Stage(stage);
    }
#endif
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    const ULONGLONG resizeBuffersStart = ::GetTickCount64();
#endif
    EGLint bufferWidth = backbufferWidth;
    EGLint bufferHeight = backbufferHeight;
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    bufferSizeFor(backbufferWidth, backbufferHeight, &bufferWidth, &bufferHeight);
    AngleUwpNote("angle: diag: native buffer resize required; requested size exceeds retained capacity or capacity disabled");
#endif
    hr = mSwapChain->ResizeBuffers(desc.BufferCount, bufferWidth, bufferHeight,
                                   getSwapChainNativeFormat(), 0);
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    const ULONGLONG resizeBuffersMs = ::GetTickCount64() - resizeBuffersStart;
    trace.Stage("ResizeBuffers.end", hr, true);
    const HRESULT afterResizeReason = device->GetDeviceRemovedReason();
    if (SUCCEEDED(hr) && FAILED(afterResizeReason))
    {
        trace.Stage("resize.returned-success-with-lost-device", afterResizeReason, true);
        release();
        return finish(EGL_CONTEXT_LOST);
    }
    if (FAILED(hr))
    {
        char note[192];
        snprintf(note, sizeof(note),
                 "angle: ResizeBuffers %dx%d -> %dx%d failed 0x%08lx, device reason 0x%08lx",
                 mWidth, mHeight, backbufferWidth, backbufferHeight,
                 static_cast<unsigned long>(hr),
                 static_cast<unsigned long>(device->GetDeviceRemovedReason()));
        AngleUwpNote(note);
    }
#endif

    if (FAILED(hr))
    {
        ERR() << "Error resizing swap chain buffers, " << gl::FmtHR(hr);
        release();

        if (d3d11::isDeviceLostError(hr))
        {
            HRESULT reason = device->GetDeviceRemovedReason();
            ERR() << "Device lost in SwapChain11::resize " << gl::FmtHR(hr)
                  << ", reason: " << gl::FmtHR(reason);
            return finish(EGL_CONTEXT_LOST);
        }
        else
        {
            return finish(EGL_BAD_ALLOC);
        }
    }

    ID3D11Texture2D *backbufferTexture = nullptr;
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    trace.Stage("panel.rebind.begin v5");
    hr = static_cast<NativeWindow11WinRT *>(mNativeWindow)->onSwapChainResized(mSwapChain);
    trace.Stage("panel.rebind.end", hr, true);
    const HRESULT rebindDeviceReason = device->GetDeviceRemovedReason();
    if (FAILED(hr) || FAILED(rebindDeviceReason))
    {
        release();
        return finish(FAILED(rebindDeviceReason) || d3d11::isDeviceLostError(hr)
                          ? EGL_CONTEXT_LOST
                          : EGL_BAD_NATIVE_WINDOW);
    }
    trace.Stage("GetBuffer.begin");
#endif
    hr                                 = mSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                               reinterpret_cast<void **>(&backbufferTexture));
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    trace.Stage("GetBuffer.end", hr, true);
#endif
    if (FAILED(hr))
    {
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        char note[160];
        snprintf(note, sizeof(note), "angle: resized GetBuffer failed 0x%08lx, device reason 0x%08lx",
                 static_cast<unsigned long>(hr),
                 static_cast<unsigned long>(device->GetDeviceRemovedReason()));
        AngleUwpNote(note);
#endif
        release();
        return finish(d3d11::isDeviceLostError(hr) ? EGL_CONTEXT_LOST : EGL_BAD_ALLOC);
    }
    {
        const auto &format =
            d3d11::Format::Get(mOffscreenRenderTargetFormat, mRenderer->getRenderer11DeviceCaps());
        mBackBufferTexture.set(backbufferTexture, format);
        mBackBufferTexture.setInternalName("BackBufferTexture");

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        D3D11_TEXTURE2D_DESC bufferDesc = {};
        backbufferTexture->GetDesc(&bufferDesc);
        char stage[160];
        snprintf(stage, sizeof(stage), "CreateRTV.begin texture=%ux%u format=%u bind=0x%x usage=%u samples=%u",
                 bufferDesc.Width, bufferDesc.Height, static_cast<unsigned>(bufferDesc.Format),
                 bufferDesc.BindFlags, static_cast<unsigned>(bufferDesc.Usage), bufferDesc.SampleDesc.Count);
        trace.Stage(stage);
#endif
        angle::Result result = mRenderer->allocateResourceNoDesc(
            displayD3D, mBackBufferTexture.get(), &mBackBufferRTView);
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        trace.Stage(result == angle::Result::Stop ? "CreateRTV.end STOP" : "CreateRTV.end OK");
#endif
        if (result == angle::Result::Stop)
        {
            const HRESULT reason = device->GetDeviceRemovedReason();
            release();
            return finish(FAILED(reason) ? EGL_CONTEXT_LOST : EGL_BAD_ALLOC);
        }
        mBackBufferRTView.setInternalName("BackBufferRTV");

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        trace.Stage("CreateSRV.begin");
#endif
        result = mRenderer->allocateResourceNoDesc(displayD3D, mBackBufferTexture.get(),
                                                   &mBackBufferSRView);
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        trace.Stage(result == angle::Result::Stop ? "CreateSRV.end STOP" : "CreateSRV.end OK");
#endif
        if (result == angle::Result::Stop)
        {
            const HRESULT reason = device->GetDeviceRemovedReason();
            release();
            return finish(FAILED(reason) ? EGL_CONTEXT_LOST : EGL_BAD_ALLOC);
        }
        mBackBufferSRView.setInternalName("BackBufferSRV");
    }

    mFirstSwap = true;

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    mBufferWidth = mBackBufferTexture.getExtents().width;
    mBufferHeight = mBackBufferTexture.getExtents().height;
    mSourceSizeDirty = mCapacityMode != 0;
    trace.Stage("offscreen.reset.begin");
    const EGLint offscreenResult =
        resetOffscreenBuffers(displayD3D, backbufferWidth, backbufferHeight);
    trace.Stage(offscreenResult == EGL_SUCCESS ? "offscreen.reset.end OK" : "offscreen.reset.end FAIL");
    if (offscreenResult != EGL_SUCCESS)
    {
        return finish(offscreenResult);
    }
    {
        char note[160];
        snprintf(note, sizeof(note),
                 "angle: swap chain resized to %dx%d in %llu ms (ResizeBuffers %llu ms), EGL 0x%04x",
                 backbufferWidth, backbufferHeight,
                 static_cast<unsigned long long>(::GetTickCount64() - resizeStart),
                 static_cast<unsigned long long>(resizeBuffersMs), offscreenResult);
        AngleUwpNote(note);
    }
    return finish(offscreenResult);
#else
    return finish(resetOffscreenBuffers(displayD3D, backbufferWidth, backbufferHeight));
#endif
}

DXGI_FORMAT SwapChain11::getSwapChainNativeFormat() const
{
    // Return a render target format for offscreen rendering is supported by IDXGISwapChain.
    // MSDN https://msdn.microsoft.com/en-us/library/windows/desktop/bb173064(v=vs.85).aspx
    switch (mOffscreenRenderTargetFormat)
    {
        case GL_RGBA8:
        case GL_RGBA4:
        case GL_RGB5_A1:
        case GL_RGB8:
        case GL_RGB565:
            return DXGI_FORMAT_R8G8B8A8_UNORM;

        case GL_BGRA8_EXT:
            return DXGI_FORMAT_B8G8R8A8_UNORM;

        case GL_RGB10_A2:
            return DXGI_FORMAT_R10G10B10A2_UNORM;

        case GL_RGBA16F:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;

        default:
            UNREACHABLE();
            return DXGI_FORMAT_UNKNOWN;
    }
}

EGLint SwapChain11::reset(DisplayD3D *displayD3D,
                          EGLint backbufferWidth,
                          EGLint backbufferHeight,
                          EGLint swapInterval)
{
    mSwapInterval = static_cast<unsigned int>(swapInterval);
    if (mSwapInterval > 4)
    {
        // IDXGISwapChain::Present documentation states that valid sync intervals are in the [0,4]
        // range
        return EGL_BAD_PARAMETER;
    }

    // If the swap chain already exists, just resize
    if (mSwapChain != nullptr)
    {
        return resize(displayD3D, backbufferWidth, backbufferHeight);
    }

    ANGLE_TRACE_EVENT0("gpu.angle", "SwapChain11::reset");
    ID3D11Device *device = mRenderer->getDevice();

    if (device == nullptr)
    {
        return EGL_BAD_ACCESS;
    }

    // Release specific resources to free up memory for the new render target, while the
    // old render target still exists for the purpose of preserving its contents.
    SafeRelease(mSwapChain1);
    SafeRelease(mSwapChain);
    mBackBufferTexture.reset();
    mBackBufferRTView.reset();

    // EGL allows creating a surface with 0x0 dimension, however, DXGI does not like 0x0 swapchains
    if (backbufferWidth < 1 || backbufferHeight < 1)
    {
        releaseOffscreenColorBuffer();
        return EGL_SUCCESS;
    }

    if (mNativeWindow->getNativeWindow())
    {
        EGLint bufferWidth = backbufferWidth;
        EGLint bufferHeight = backbufferHeight;
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        mCapacityMode = mSourceSizeBroken ? 0 : AngleUwpCapacityMode(
            static_cast<const NativeWindow11WinRT *>(mNativeWindow));
        bufferSizeFor(backbufferWidth, backbufferHeight, &bufferWidth, &bufferHeight);
        char note[192];
        snprintf(note, sizeof(note), "angle: diag: capacity v6 mode=%d surface=%dx%d buffers=%dx%d copy=%d",
                 mCapacityMode, backbufferWidth, backbufferHeight, bufferWidth, bufferHeight,
                 mNeedsOffscreenTexture ? 1 : 0);
        AngleUwpNote(note);
#endif
        HRESULT hr = mNativeWindow->createSwapChain(
            device, mRenderer->getDxgiFactory(), getSwapChainNativeFormat(), bufferWidth,
            bufferHeight, mNeedsOffscreenTexture ? 1 : getD3DSamples(), &mSwapChain);

        if (FAILED(hr))
        {
            ERR() << "Could not create additional swap chains or offscreen surfaces, "
                  << gl::FmtHR(hr);
            release();

            if (d3d11::isDeviceLostError(hr))
            {
                HRESULT reason = device->GetDeviceRemovedReason();
                ERR() << "Device lost in SwapChain11::reset " << gl::FmtHR(hr)
                      << ", reason: " << gl::FmtHR(reason);
                return EGL_CONTEXT_LOST;
            }
            else
            {
                return EGL_BAD_ALLOC;
            }
        }

        mSwapChain1 = d3d11::DynamicCastComObject<IDXGISwapChain1>(mSwapChain);

        ID3D11Texture2D *backbufferTex = nullptr;
        hr                             = mSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                               reinterpret_cast<LPVOID *>(&backbufferTex));
        ASSERT(SUCCEEDED(hr));
        if (FAILED(hr))
        {
            const HRESULT reason = device->GetDeviceRemovedReason();
            release();
            return FAILED(reason) || d3d11::isDeviceLostError(hr) ? EGL_CONTEXT_LOST : EGL_BAD_ALLOC;
        }
        const auto &format =
            d3d11::Format::Get(mOffscreenRenderTargetFormat, mRenderer->getRenderer11DeviceCaps());
        mBackBufferTexture.set(backbufferTex, format);
        mBackBufferTexture.setInternalName("BackBufferTexture");

        angle::Result result = mRenderer->allocateResourceNoDesc(
            displayD3D, mBackBufferTexture.get(), &mBackBufferRTView);
        ASSERT(result != angle::Result::Stop);
        if (result == angle::Result::Stop)
        {
            const HRESULT reason = device->GetDeviceRemovedReason();
            release();
            return FAILED(reason) ? EGL_CONTEXT_LOST : EGL_BAD_ALLOC;
        }
        mBackBufferRTView.setInternalName("BackBufferRTV");

        result = mRenderer->allocateResourceNoDesc(displayD3D, mBackBufferTexture.get(),
                                                   &mBackBufferSRView);
        ASSERT(result != angle::Result::Stop);
        if (result == angle::Result::Stop)
        {
            const HRESULT reason = device->GetDeviceRemovedReason();
            release();
            return FAILED(reason) ? EGL_CONTEXT_LOST : EGL_BAD_ALLOC;
        }
        mBackBufferSRView.setInternalName("BackBufferSRV");
    }

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    if (mSwapChain)
    {
        mBufferWidth = mBackBufferTexture.getExtents().width;
        mBufferHeight = mBackBufferTexture.getExtents().height;
        mSourceSizeDirty = mCapacityMode != 0;
    }
#endif
    mFirstSwap = true;

    return resetOffscreenBuffers(displayD3D, backbufferWidth, backbufferHeight);
}

angle::Result SwapChain11::initPassThroughResources(DisplayD3D *displayD3D)
{
    if (mPassThroughResourcesInit)
    {
        return angle::Result::Continue;
    }

    ANGLE_TRACE_EVENT0("gpu.angle", "SwapChain11::initPassThroughResources");
    ID3D11Device *device = mRenderer->getDevice();

    ASSERT(device != nullptr);

    // Make sure our resources are all not allocated, when we create
    ASSERT(!mQuadVB.valid() && !mPassThroughSampler.valid());
    ASSERT(!mPassThroughIL.valid() && !mPassThroughVS.valid() && !mPassThroughOrResolvePS.valid());

    D3D11_BUFFER_DESC vbDesc;
    vbDesc.ByteWidth           = sizeof(d3d11::PositionTexCoordVertex) * 4;
    vbDesc.Usage               = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags           = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
    vbDesc.MiscFlags           = 0;
    vbDesc.StructureByteStride = 0;

    ANGLE_TRY(mRenderer->allocateResource(displayD3D, vbDesc, &mQuadVB));
    mQuadVB.setInternalName("SwapChainQuadVB");

    D3D11_SAMPLER_DESC samplerDesc;
    samplerDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MipLODBias     = 0.0f;
    samplerDesc.MaxAnisotropy  = 0;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 0.0f;
    samplerDesc.BorderColor[1] = 0.0f;
    samplerDesc.BorderColor[2] = 0.0f;
    samplerDesc.BorderColor[3] = 0.0f;
    samplerDesc.MinLOD         = 0;
    samplerDesc.MaxLOD         = D3D11_FLOAT32_MAX;

    ANGLE_TRY(mRenderer->allocateResource(displayD3D, samplerDesc, &mPassThroughSampler));
    mPassThroughSampler.setInternalName("SwapChainPassThroughSampler");

    D3D11_INPUT_ELEMENT_DESC quadLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    InputElementArray quadElements(quadLayout);
    ShaderData vertexShaderData(g_VS_Passthrough2D);

    ANGLE_TRY(
        mRenderer->allocateResource(displayD3D, quadElements, &vertexShaderData, &mPassThroughIL));
    mPassThroughIL.setInternalName("SwapChainPassThroughIL");

    ANGLE_TRY(mRenderer->allocateResource(displayD3D, vertexShaderData, &mPassThroughVS));
    mPassThroughVS.setInternalName("SwapChainPassThroughVS");

    if (mEGLSamples <= 1)
    {
        ShaderData pixelShaderData(g_PS_PassthroughRGBA2D);
        ANGLE_TRY(
            mRenderer->allocateResource(displayD3D, pixelShaderData, &mPassThroughOrResolvePS));
    }
    else
    {
        if (mNativeWindow->getNativeWindow() && mNeedsOffscreenTexture)
        {
            ShaderData pixelShaderData(g_PS_ResolveColor2D);
            ANGLE_TRY(
                mRenderer->allocateResource(displayD3D, pixelShaderData, &mPassThroughOrResolvePS));
        }
        else
        {
            ShaderData pixelShaderData(g_PS_PassthroughRGBA2DMS);
            ANGLE_TRY(
                mRenderer->allocateResource(displayD3D, pixelShaderData, &mPassThroughOrResolvePS));
        }
    }

    mPassThroughOrResolvePS.setInternalName("SwapChainPassThroughPS");

    // Use the default rasterizer state but without culling
    D3D11_RASTERIZER_DESC rasterizerDesc;
    rasterizerDesc.FillMode              = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode              = D3D11_CULL_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias             = 0;
    rasterizerDesc.SlopeScaledDepthBias  = 0.0f;
    rasterizerDesc.DepthBiasClamp        = 0.0f;
    rasterizerDesc.DepthClipEnable       = TRUE;
    rasterizerDesc.ScissorEnable         = FALSE;
    rasterizerDesc.MultisampleEnable     = FALSE;
    rasterizerDesc.AntialiasedLineEnable = FALSE;

    ANGLE_TRY(mRenderer->allocateResource(displayD3D, rasterizerDesc, &mPassThroughRS));
    mPassThroughRS.setInternalName("SwapChainPassThroughRasterizerState");

    mPassThroughResourcesInit = true;
    return angle::Result::Continue;
}

// parameters should be validated/clamped by caller
EGLint SwapChain11::swapRect(DisplayD3D *displayD3D,
                             EGLint x,
                             EGLint y,
                             EGLint width,
                             EGLint height)
{
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    // A sub-buffer post right after the buffers were made (the first frame,
    // a resize) would copy a strip into a back buffer that holds nothing
    // else, and present() presents that frame whole. The offscreen texture
    // is complete, so copy all of it.
    if (mFirstSwap)
    {
        x      = 0;
        y      = 0;
        width  = mWidth;
        height = mHeight;
    }
#endif
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    ++mDiagnosticFrame;
    char rectangle[160];
    const bool validRect = x >= 0 && y >= 0 && width >= 0 && height >= 0 &&
        static_cast<int64_t>(x) + width <= mWidth &&
        static_cast<int64_t>(y) + height <= mHeight;
    snprintf(rectangle, sizeof(rectangle), "swapRect-before-copy rect=%d,%d %dx%d valid=%d",
             x, y, width, height, int(validRect));
    traceCommand(rectangle, validRect ? S_OK : E_INVALIDARG, mDiagnosticFrame <= 3);
    probeProgress("before-copy");
#endif
    if (mNeedsOffscreenTexture)
    {
        EGLint result = copyOffscreenToBackbuffer(displayD3D, x, y, width, height);
        if (result != EGL_SUCCESS)
        {
            return result;
        }
    }

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    traceCommand("swapRect-after-copy");
    probeProgress("after-copy");
#endif
    EGLint result = present(displayD3D, x, y, width, height);
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    traceCommand("swapRect-after-present", result == EGL_SUCCESS ? S_OK : E_FAIL);
    probeProgress("after-present");
#endif
    return result;
}

EGLint SwapChain11::copyOffscreenToBackbuffer(DisplayD3D *displayD3D,
                                              EGLint x,
                                              EGLint y,
                                              EGLint width,
                                              EGLint height)
{
    if (!mSwapChain)
    {
        return EGL_SUCCESS;
    }

    if (initPassThroughResources(displayD3D) == angle::Result::Stop)
    {
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        traceCommand("copy-resource-init-failed", E_FAIL);
#endif
        return EGL_BAD_ALLOC;
    }

    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

    // Set vertices
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT result =
        deviceContext->Map(mQuadVB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (FAILED(result))
    {
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        traceCommand("copy-vertex-Map-failed", result);
#endif
        return EGL_BAD_ACCESS;
    }

    d3d11::PositionTexCoordVertex *vertices =
        static_cast<d3d11::PositionTexCoordVertex *>(mappedResource.pData);

    // Create a quad in homogeneous coordinates
    float x1 = (x / float(mWidth)) * 2.0f - 1.0f;
    float y1 = (y / float(mHeight)) * 2.0f - 1.0f;
    float x2 = ((x + width) / float(mWidth)) * 2.0f - 1.0f;
    float y2 = ((y + height) / float(mHeight)) * 2.0f - 1.0f;

    float u1 = x / float(mWidth);
    float v1 = y / float(mHeight);
    float u2 = (x + width) / float(mWidth);
    float v2 = (y + height) / float(mHeight);

    // Invert the quad vertices depending on the surface orientation.
    if ((mOrientation & EGL_SURFACE_ORIENTATION_INVERT_X_ANGLE) != 0)
    {
        std::swap(x1, x2);
    }
    if ((mOrientation & EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE) != 0)
    {
        std::swap(y1, y2);
    }

    d3d11::SetPositionTexCoordVertex(&vertices[0], x1, y1, u1, v1);
    d3d11::SetPositionTexCoordVertex(&vertices[1], x1, y2, u1, v2);
    d3d11::SetPositionTexCoordVertex(&vertices[2], x2, y1, u2, v1);
    d3d11::SetPositionTexCoordVertex(&vertices[3], x2, y2, u2, v2);

    deviceContext->Unmap(mQuadVB.get(), 0);
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    traceCommand("copy-quad-uploaded", result);
#endif

    StateManager11 *stateManager = mRenderer->getStateManager();

    constexpr UINT stride = sizeof(d3d11::PositionTexCoordVertex);
    stateManager->setSingleVertexBuffer(&mQuadVB, stride, 0);

    // Apply state
    stateManager->setDepthStencilState(nullptr, 0xFFFFFFFF);
    stateManager->setSimpleBlendState(nullptr);
    stateManager->setRasterizerState(&mPassThroughRS);

    // Apply shaders
    stateManager->setInputLayout(&mPassThroughIL);
    stateManager->setPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    stateManager->setDrawShaders(&mPassThroughVS, nullptr, &mPassThroughOrResolvePS);

    // Apply render targets. Use the proxy context in display.
    stateManager->setRenderTarget(mBackBufferRTView.get(), nullptr);

    // Set the viewport
    stateManager->setSimpleViewport(mWidth, mHeight);

    // Apply textures
    stateManager->setSimplePixelTextureAndSampler(mOffscreenSRView, mPassThroughSampler);

    // Draw
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    traceCommand("copy-before-draw");
#endif
    deviceContext->Draw(4, 0);
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    traceCommand("copy-after-draw");
#endif

    return EGL_SUCCESS;
}

EGLint SwapChain11::present(DisplayD3D *displayD3D, EGLint x, EGLint y, EGLint width, EGLint height)
{
    if (!mSwapChain)
    {
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        if (mSourceSizeBroken && mWidth > 0 && mHeight > 0)
        {
            const EGLint result = remakeAfterSourceSizeFailure(displayD3D, mWidth, mHeight);
            return result == EGL_SUCCESS ? EGL_BAD_ALLOC : result;
        }
#endif
        return EGL_SUCCESS;
    }

    UINT swapInterval = mSwapInterval;
#if !ANGLE_VSYNC
    swapInterval = 0;
#endif

    HRESULT result = S_OK;

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    // Nobody has ever confirmed that a frame reaches this swap chain. The log
    // says the chain was made and the panel took it, and then the window is
    // hidden -- and whether a single Present happened in between has been
    // assumed both ways. Said once, the first time, and every time it fails.
    if (mFirstSwap)
    {
        AngleUwpNote("angle: first present");
    }
    if (AngleUwpMode() == 2)
    {
        if (mFirstSwap)
        {
            AngleUwpNote("angle: mode 2 -- NOT presenting, this frame or any other");
        }
        mFirstSwap = false;
        mRenderer->getStateManager()->invalidateRenderTarget();
        return EGL_SUCCESS;
    }
    if (mCapacityMode != 0 && mSourceSizeDirty)
    {
        const EGLint sourceResult = updateSourceSize(displayD3D);
        if (sourceResult != EGL_SUCCESS)
        {
            return sourceResult;
        }
    }
#endif

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    traceCommand("Present-call");
#endif
    // Use IDXGISwapChain1::Present1 with a dirty rect if DXGI 1.2 is available.
    // Dirty rect present is not supported with a multisampled swapchain.
    if (mSwapChain1 != nullptr && mEGLSamples <= 1)
    {
        if (mFirstSwap)
        {
            // Can't swap with a dirty rect if this swap chain has never swapped before
            DXGI_PRESENT_PARAMETERS params = {0, nullptr, nullptr, nullptr};
            result                         = mSwapChain1->Present1(swapInterval, 0, &params);
        }
        else
        {
            RECT rect = {static_cast<LONG>(x), static_cast<LONG>(mHeight - y - height),
                         static_cast<LONG>(x + width), static_cast<LONG>(mHeight - y)};
            DXGI_PRESENT_PARAMETERS params = {1, &rect, nullptr, nullptr};
            result                         = mSwapChain1->Present1(swapInterval, 0, &params);
        }
    }
    else
    {
        result = mSwapChain->Present(swapInterval, 0);
    }

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    traceCommand("Present-return", result);
    if (mFirstSwap)
    {
        char note[96];
        snprintf(note, sizeof(note), "angle: first present result 0x%08lx",
                 static_cast<unsigned long>(result));
        AngleUwpNote(note);
    }
    if (FAILED(result))
    {
        PanelGraphicsTrace trace(mRenderer->getDevice(), mWidth, mHeight, mWidth, mHeight,
                                 "present-failure");
        trace.Stage("Present returned", result, true);
        trace.Finish(d3d11::isDeviceLostError(result) ? EGL_CONTEXT_LOST : EGL_BAD_SURFACE);
        char note[192];
        snprintf(note, sizeof(note),
                 "angle: present %dx%d failed 0x%08lx, device reason 0x%08lx",
                 mWidth, mHeight, static_cast<unsigned long>(result),
                 static_cast<unsigned long>(mRenderer->getDevice()->GetDeviceRemovedReason()));
        AngleUwpNote(note);
    }
#endif
    mFirstSwap = false;

    // Some swapping mechanisms such as DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL unbind the current render
    // target. Mark it dirty. Use the proxy context in display since there is none available.
    mRenderer->getStateManager()->invalidateRenderTarget();

    if (result == DXGI_ERROR_DEVICE_REMOVED)
    {
        ERR() << "Present failed: the D3D11 device was removed, "
              << gl::FmtHR(mRenderer->getDevice()->GetDeviceRemovedReason());
        return EGL_CONTEXT_LOST;
    }
    else if (result == DXGI_ERROR_DEVICE_RESET)
    {
        ERR() << "Present failed: the D3D11 device was reset from a bad command.";
        return EGL_CONTEXT_LOST;
    }
    else if (FAILED(result))
    {
        ERR() << "Present failed with " << gl::FmtHR(result);
#if defined(ANGLE_ENABLE_WINDOWS_UWP)
        return d3d11::isDeviceLostError(result) ? EGL_CONTEXT_LOST : EGL_BAD_SURFACE;
#endif
    }

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
    if (result != S_OK)
    {
        // Including the S_ statuses -- DXGI_STATUS_OCCLUDED is 0x087A0001 and
        // would mean the compositor considers this chain hidden, which is the
        // very thing being asked about.
        char note[96];
        snprintf(note, sizeof(note), "angle: present returned 0x%08lx",
                 static_cast<unsigned long>(result));
        AngleUwpNote(note);
    }
#endif

    mNativeWindow->commitChange();

    return EGL_SUCCESS;
}

const TextureHelper11 &SwapChain11::getOffscreenTexture()
{
    return mNeedsOffscreenTexture ? mOffscreenTexture : mBackBufferTexture;
}

const d3d11::RenderTargetView &SwapChain11::getRenderTarget()
{
    return mNeedsOffscreenTexture ? mOffscreenRTView : mBackBufferRTView;
}

angle::Result SwapChain11::getRenderTargetShaderResource(d3d::Context *context,
                                                         const d3d11::SharedSRV **outSRV)
{
    *outSRV = nullptr;

    if (!mNeedsOffscreenTexture)
    {
        ASSERT(mBackBufferSRView.valid());
        *outSRV = &mBackBufferSRView;
        return angle::Result::Continue;
    }

    if (!mNeedsOffscreenTextureCopy)
    {
        ASSERT(mOffscreenSRView.valid());
        *outSRV = &mOffscreenSRView;
        return angle::Result::Continue;
    }

    if (!mOffscreenTextureCopyForSRV.valid())
    {
        const d3d11::Format &backbufferFormatInfo =
            d3d11::Format::Get(mOffscreenRenderTargetFormat, mRenderer->getRenderer11DeviceCaps());

        D3D11_TEXTURE2D_DESC offscreenCopyDesc;
        mOffscreenTexture.getDesc(&offscreenCopyDesc);

        offscreenCopyDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        offscreenCopyDesc.MiscFlags      = 0;
        offscreenCopyDesc.CPUAccessFlags = 0;
        TextureHelper11 offscreenTextureCopyForSRV;
        ANGLE_TRY(mRenderer->allocateTexture(context, offscreenCopyDesc, backbufferFormatInfo,
                                             &offscreenTextureCopyForSRV));
        offscreenTextureCopyForSRV.setInternalName("OffscreenBackBufferCopyForSRV");

        D3D11_SHADER_RESOURCE_VIEW_DESC offscreenSRVDesc;
        offscreenSRVDesc.Format = backbufferFormatInfo.srvFormat;
        offscreenSRVDesc.ViewDimension =
            (mEGLSamples <= 1) ? D3D11_SRV_DIMENSION_TEXTURE2D : D3D11_SRV_DIMENSION_TEXTURE2DMS;
        offscreenSRVDesc.Texture2D.MostDetailedMip = 0;
        offscreenSRVDesc.Texture2D.MipLevels       = static_cast<UINT>(-1);

        d3d11::SharedSRV offscreenSRView;
        ANGLE_TRY(mRenderer->allocateResource(context, offscreenSRVDesc,
                                              offscreenTextureCopyForSRV.get(), &offscreenSRView));
        offscreenSRView.setInternalName("OffscreenBackBufferSRV");

        // Commit created objects in one step so we don't end up with half baked member variables.
        mOffscreenTextureCopyForSRV = std::move(offscreenTextureCopyForSRV);
        mOffscreenSRView            = std::move(offscreenSRView);
    }

    // Need to copy the offscreen texture into the shader-readable copy, since it's external and
    // we don't know if the copy is up-to-date. This works around the problem we have when the app
    // passes in a texture that isn't shader-readable.
    mRenderer->getDeviceContext()->CopyResource(mOffscreenTextureCopyForSRV.get(),
                                                mOffscreenTexture.get());
    *outSRV = &mOffscreenSRView;
    return angle::Result::Continue;
}

const d3d11::DepthStencilView &SwapChain11::getDepthStencil()
{
    return mDepthStencilDSView;
}

const d3d11::SharedSRV &SwapChain11::getDepthStencilShaderResource()
{
    return mDepthStencilSRView;
}

const TextureHelper11 &SwapChain11::getDepthStencilTexture()
{
    return mDepthStencilTexture;
}

void *SwapChain11::getKeyedMutex()
{
    return mKeyedMutex;
}

void SwapChain11::recreate()
{
    // possibly should use this method instead of reset
}

RenderTargetD3D *SwapChain11::getColorRenderTarget()
{
    return &mColorRenderTarget;
}

RenderTargetD3D *SwapChain11::getDepthStencilRenderTarget()
{
    return &mDepthStencilRenderTarget;
}

egl::Error SwapChain11::getSyncValues(EGLuint64KHR *ust, EGLuint64KHR *msc, EGLuint64KHR *sbc)
{
    if (!mSwapChain)
    {
        return egl::Error(EGL_NOT_INITIALIZED, "Swap chain uninitialized");
    }

    DXGI_FRAME_STATISTICS stats = {};
    HRESULT result              = mSwapChain->GetFrameStatistics(&stats);

    if (FAILED(result))
    {
        std::ostringstream err;
        err << "Failed to get frame statistics, " << gl::FmtHR(result);
        return egl::Error(EGL_BAD_ALLOC, err.str());
    }

    // Conversion from DXGI_FRAME_STATISTICS to the output values:
    // stats.SyncRefreshCount -> msc
    // stats.PresentCount -> sbc
    // stats.SyncQPCTime -> ust with conversion to microseconds via QueryPerformanceFrequency
    *msc = stats.SyncRefreshCount;
    *sbc = stats.PresentCount;

    LONGLONG syncQPCValue = stats.SyncQPCTime.QuadPart;
    // If the QPC Value is below the overflow threshold, we proceed with
    // simple multiply and divide.
    if (syncQPCValue < kQPCOverflowThreshold)
    {
        *ust = syncQPCValue * kMicrosecondsPerSecond / mQPCFrequency;
    }
    else
    {
        // Otherwise, calculate microseconds in a round about manner to avoid
        // overflow and precision issues.
        int64_t wholeSeconds  = syncQPCValue / mQPCFrequency;
        int64_t leftoverTicks = syncQPCValue - (wholeSeconds * mQPCFrequency);
        *ust                  = wholeSeconds * kMicrosecondsPerSecond +
               leftoverTicks * kMicrosecondsPerSecond / mQPCFrequency;
    }

    return egl::NoError();
}

UINT SwapChain11::getD3DSamples() const
{
    return (mEGLSamples == 0) ? 1 : mEGLSamples;
}

}  // namespace rx
