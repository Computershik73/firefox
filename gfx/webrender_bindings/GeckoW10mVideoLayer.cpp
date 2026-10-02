/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// GeckoW10m: a playing video as a hardware overlay of its own.
//
// Fullscreen 1080p60 cost the compositor 22-29 ms a frame on an Elite X3,
// with no overdraw to speak of: the cost was the video itself, sampled from
// NV12 and scaled to 2560x1440 by the 3D engine, a 60 fps video wants 16.7.
// The phone's display can do that work itself -- it lays an NV12 layer over
// the picture and scales it as it scans out (CheckOverlaySupport: NV12 with
// scaling), and an NV12 swap chain given to a XAML panel was shown that way
// in every test, including under a translucent browser layer.
//
// So when WebRender is about to composite an opaque NV12 video surface, it
// offers the frame here first. The frame is copied into an NV12 swap chain
// whose composition surface sits in a panel under the browser's own, the
// shell is told where the video is, and once the shell says the panel is in
// place WebRender draws a transparent hole instead of the video. Everything
// drawn over the video -- controls, subtitles -- is still drawn by WebRender,
// over the hole. Any time the video moves, the hole closes and WebRender draws
// the video again until the panel has caught up, so a scrolling page never
// shows the panel out of place.
//
// gfx.webrender.gecko-w10m.video-overlay = false turns it off.

#include <d3d11.h>
#include <dxgi1_3.h>

#include <atomic>

#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/Sprintf.h"
#include "mozilla/webrender/RenderD3D11TextureHost.h"
#include "mozilla/webrender/RenderThread.h"
#include "mozilla/webrender/WebRenderTypes.h"

extern "C" void GeckoW10mNote(const char* aText);

namespace {

// The shell's side; the layout matches EngineView::VideoLayerSink.
struct GeckoW10mVideoLayerSink {
  void (*attach)(void* aSurface);
  void (*place)(uint32_t aGeneration, int32_t aX, int32_t aY, int32_t aWidth,
                int32_t aHeight, int32_t aClipX, int32_t aClipY,
                int32_t aClipWidth, int32_t aClipHeight, int32_t aChainWidth,
                int32_t aChainHeight);
  void (*show)(int32_t aVisible);
};

std::atomic<const GeckoW10mVideoLayerSink*> sSink{nullptr};
std::atomic<uint32_t> sAckedGeneration{0};

constexpr DWORD kCompositionObjectAllAccess = 0x0003;
constexpr UINT kSwapChainFlagYuvVideo = DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO;

struct Rect {
  int32_t x = 0, y = 0, w = 0, h = 0;
  bool operator==(const Rect& aOther) const {
    return x == aOther.x && y == aOther.y && w == aOther.w && h == aOther.h;
  }
  bool operator!=(const Rect& aOther) const { return !(*this == aOther); }
};

// All of it lives on the render thread.
struct VideoLayer {
  RefPtr<ID3D11Device> mDevice;
  RefPtr<ID3D11DeviceContext> mContext;
  RefPtr<IDXGISwapChain1> mChain;
  HANDLE mSurface = nullptr;
  int32_t mChainWidth = 0;
  int32_t mChainHeight = 0;
  bool mBroken = false;  // something refused; WebRender keeps the video

  uint32_t mGeneration = 0;
  Rect mRect;
  Rect mClip;
  uint32_t mStableFrames = 0;
  bool mShown = false;
  bool mPresented = false;

  // The last frame copied in, so a composite for another reason does not
  // present the same picture again.
  ID3D11Texture2D* mLastTexture = nullptr;
  uint32_t mLastIndex = UINT32_MAX;
  void* mLastHost = nullptr;

  bool mOfferedThisFrame = false;
  uint32_t mFramesWithout = 0;

  uint32_t mStatFrames = 0;
  uint32_t mStatHoles = 0;
  bool mSaidHole = false;

  void Note(const char* aText) { GeckoW10mNote(aText); }

  // Each reason a video is left to WebRender, said once.
  uint32_t mDeclinedReasons = 0;
  void Decline(uint32_t aReason, const char* aWhy) {
    if (!(mDeclinedReasons & aReason)) {
      mDeclinedReasons |= aReason;
      char note[160];
      SprintfLiteral(note, "video layer: a video left to WebRender -- %s", aWhy);
      Note(note);
    }
  }

  void Break(const char* aWhy, HRESULT aHr) {
    char note[160];
    SprintfLiteral(note, "video layer: %s (0x%08lx) -- WebRender keeps drawing videos",
                   aWhy, static_cast<unsigned long>(aHr));
    Note(note);
    mBroken = true;
    Hide();
  }

  void Hide() {
    if (mShown) {
      if (auto* sink = sSink.load()) {
        sink->show(0);
      }
      mShown = false;
    }
    mStableFrames = 0;
    mRect = Rect();
  }

  bool EnsureChain(ID3D11Device* aDevice, int32_t aWidth, int32_t aHeight) {
    if (mChain && mDevice == aDevice && mChainWidth == aWidth &&
        mChainHeight == aHeight) {
      return true;
    }
    if (mChain && mDevice == aDevice) {
      // A new size, as when YouTube switches quality.
      HRESULT hr = mChain->ResizeBuffers(0, aWidth, aHeight, DXGI_FORMAT_NV12,
                                         kSwapChainFlagYuvVideo);
      if (FAILED(hr)) {
        Break("the swap chain would not resize", hr);
        return false;
      }
      mChainWidth = aWidth;
      mChainHeight = aHeight;
      mLastTexture = nullptr;
      mPresented = false;
      ++mGeneration;  // the panel's scale depends on the chain's size
      mStableFrames = 0;
      return true;
    }

    mDevice = aDevice;
    mDevice->GetImmediateContext(getter_AddRefs(mContext));

    HMODULE dcomp =
        ::LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using HandleFn = HRESULT(WINAPI*)(DWORD, SECURITY_ATTRIBUTES*, HANDLE*);
    auto createHandle =
        dcomp ? reinterpret_cast<HandleFn>(
                    ::GetProcAddress(dcomp, "DCompositionCreateSurfaceHandle"))
              : nullptr;
    if (!createHandle) {
      Break("no composition surface handles", E_NOTIMPL);
      return false;
    }

    RefPtr<IDXGIDevice> dxgi;
    RefPtr<IDXGIAdapter> adapter;
    RefPtr<IDXGIFactoryMedia> media;
    HRESULT hr = mDevice->QueryInterface(__uuidof(IDXGIDevice),
                                         getter_AddRefs(dxgi));
    if (SUCCEEDED(hr)) {
      hr = dxgi->GetAdapter(getter_AddRefs(adapter));
    }
    if (SUCCEEDED(hr)) {
      hr = adapter->GetParent(__uuidof(IDXGIFactoryMedia),
                              getter_AddRefs(media));
    }
    if (FAILED(hr) || !media) {
      Break("no media swap chains", hr);
      return false;
    }

    hr = createHandle(kCompositionObjectAllAccess, nullptr, &mSurface);
    if (FAILED(hr) || !mSurface) {
      Break("no composition surface", hr);
      return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = aWidth;
    desc.Height = aHeight;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = kSwapChainFlagYuvVideo;
    hr = media->CreateSwapChainForCompositionSurfaceHandle(
        mDevice, mSurface, &desc, nullptr, getter_AddRefs(mChain));
    if (FAILED(hr)) {
      Break("no NV12 swap chain", hr);
      return false;
    }
    mChainWidth = aWidth;
    mChainHeight = aHeight;
    if (auto* sink = sSink.load()) {
      sink->attach(mSurface);
    }
    char note[128];
    SprintfLiteral(note, "video layer: an NV12 swap chain of %dx%d for the display",
                   aWidth, aHeight);
    Note(note);
    return true;
  }

  // Copies the frame in and presents it, if it is a new one.
  bool Present(mozilla::wr::RenderDXGITextureHost* aHost,
               ID3D11Texture2D* aTexture) {
    const uint32_t index = aHost->ArrayIndex();
    if (mPresented && aTexture == mLastTexture && index == mLastIndex &&
        aHost == mLastHost) {
      return true;
    }
    RefPtr<ID3D11Texture2D> buffer;
    HRESULT hr = mChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                   getter_AddRefs(buffer));
    if (FAILED(hr)) {
      Break("no back buffer", hr);
      return false;
    }
    D3D11_BOX box = {0, 0, 0, UINT(mChainWidth), UINT(mChainHeight), 1};
    mContext->CopySubresourceRegion(buffer, 0, 0, 0, 0, aTexture, index, &box);
    hr = mChain->Present(0, 0);
    if (FAILED(hr)) {
      Break("present failed", hr);
      return false;
    }
    mLastTexture = aTexture;
    mLastIndex = index;
    mLastHost = aHost;
    mPresented = true;
    return true;
  }

  bool Offer(const mozilla::wr::ExternalImageId& aId, const float* aRects) {
    if (mBroken) {
      return false;
    }
    if (!mozilla::StaticPrefs::gfx_webrender_gecko_w10m_video_overlay_AtStartup()) {
      Decline(16, "turned off (gfx.webrender.gecko-w10m.video-overlay)");
      return false;
    }
    if (!sSink.load()) {
      Decline(32, "the shell has no panel for it");
      return false;
    }
    mOfferedThisFrame = true;

    auto* texture = mozilla::wr::RenderThread::Get()->GetRenderTexture(aId);
    auto* host = texture ? texture->AsRenderDXGITextureHost() : nullptr;
    if (!host) {
      Decline(1, texture ? "not a D3D11 texture" : "no texture for its id");
      Hide();
      return false;
    }
    if (host->GetFormat() != mozilla::gfx::SurfaceFormat::NV12) {
      Decline(2, "not NV12");
      Hide();
      return false;
    }
    if (host->mHasKeyedMutex) {
      Decline(4, "the frame is behind a keyed mutex");
      Hide();
      return false;
    }
    ID3D11Texture2D* d3dTexture = host->GetD3D11Texture2DWithGL();
    if (!d3dTexture) {
      Decline(8, "the frame's texture could not be opened");
      Hide();
      return false;
    }
    RefPtr<ID3D11Device> device;
    d3dTexture->GetDevice(getter_AddRefs(device));
    const mozilla::gfx::IntSize size = host->GetSize(0);
    const int32_t width = size.width & ~1;
    const int32_t height = size.height & ~1;
    if (width <= 0 || height <= 0 || !EnsureChain(device, width, height)) {
      return false;
    }
    if (!Present(host, d3dTexture)) {
      return false;
    }

    // Where the video is, and the part of it the page shows, in the
    // browser surface's pixels.
    Rect rect{int32_t(aRects[0] + 0.5f), int32_t(aRects[1] + 0.5f),
              int32_t(aRects[2] - aRects[0] + 0.5f),
              int32_t(aRects[3] - aRects[1] + 0.5f)};
    Rect clip{int32_t(aRects[4] + 0.5f), int32_t(aRects[5] + 0.5f),
              int32_t(aRects[6] - aRects[4] + 0.5f),
              int32_t(aRects[7] - aRects[5] + 0.5f)};
    if (rect.w <= 0 || rect.h <= 0) {
      return false;
    }
    if (rect != mRect || clip != mClip) {
      mRect = rect;
      mClip = clip;
      ++mGeneration;
      mStableFrames = 0;
      if (auto* sink = sSink.load()) {
        sink->place(mGeneration, rect.x, rect.y, rect.w, rect.h, clip.x,
                    clip.y, clip.w, clip.h, mChainWidth, mChainHeight);
        if (!mShown) {
          sink->show(1);
          mShown = true;
        }
      }
      return false;
    }
    ++mStableFrames;
    // The hole only once the shell has put the panel where this frame's
    // video is, and the video has stood still for a frame.
    const bool hole =
        sAckedGeneration.load() == mGeneration && mStableFrames >= 2;
    ++mStatFrames;
    if (hole) {
      ++mStatHoles;
      if (!mSaidHole) {
        mSaidHole = true;
        Note("video layer: the display shows the video; WebRender leaves a hole");
      }
    }
    return hole;
  }

  void EndFrame() {
    if (mOfferedThisFrame) {
      mFramesWithout = 0;
    } else if (mShown && ++mFramesWithout >= 2) {
      // The video is gone from the screen.
      Hide();
    }
    mOfferedThisFrame = false;
    if (mStatFrames >= 300) {
      char note[128];
      SprintfLiteral(note,
                     "video layer: %u of the last %u video frames shown by the "
                     "display as an overlay",
                     mStatHoles, mStatFrames);
      Note(note);
      mStatFrames = mStatHoles = 0;
    }
  }
};

VideoLayer& Layer() {
  static VideoLayer sLayer;
  return sLayer;
}

}  // namespace

// The shell hands over its panel's three entry points (EngineView).
extern "C" __declspec(dllexport) void gecko_w10m_set_video_layer_sink(
    const GeckoW10mVideoLayerSink* aSink) {
  sSink.store(aSink);
}

// The shell has applied the placement with this generation.
extern "C" __declspec(dllexport) void gecko_w10m_video_layer_ack(
    uint32_t aGeneration) {
  sAckedGeneration.store(aGeneration);
}

// From WebRender's composite (renderer/composite.rs), for an opaque NV12
// video about to be drawn: aRects is the video's rect and its visible part,
// x0 y0 x1 y1 each, in device pixels. True: the display shows it, draw a
// transparent hole.
extern "C" bool gecko_w10m_video_layer_frame(uint64_t aExternalImageId,
                                             const float* aRects) {
  return Layer().Offer(mozilla::wr::ToExternalImageId(aExternalImageId), aRects);
}

// From RenderCompositorEGL::EndFrame, once per presented frame.
void GeckoW10mVideoLayerEndFrame() { Layer().EndFrame(); }
