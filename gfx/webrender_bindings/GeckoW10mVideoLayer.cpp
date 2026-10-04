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

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <math.h>

#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/TimeStamp.h"
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
  // Above the browser's panel, or under it. The display takes the layer as
  // a hardware overlay only above (the test in build 132: under the
  // browser it was composed by the GPU whatever else was done), and above
  // it hides whatever the page draws over the video.
  // aTrim*: rows and columns of the visible part, in the browser surface's
  // pixels, cut off the panel while it is above, so that a thin band the page
  // draws over the video's edge (a player's progress bar) still shows.
  void (*stack)(int32_t aAbove, int32_t aTrimTop, int32_t aTrimBottom,
                int32_t aTrimLeft, int32_t aTrimRight);
};

std::atomic<const GeckoW10mVideoLayerSink*> sSink{nullptr};
std::atomic<uint32_t> sAckedGeneration{0};

constexpr DWORD kCompositionObjectAllAccess = 0x0003;
constexpr UINT kSwapChainFlagYuvVideo = DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO;

// Rows and columns cut off each edge of the video's visible part.
struct Trim {
  int32_t top = 0, bottom = 0, left = 0, right = 0;
  // A band that only moves by a pixel or two is the same band.
  bool Near(const Trim& aOther) const {
    return std::abs(top - aOther.top) <= 2 &&
           std::abs(bottom - aOther.bottom) <= 2 &&
           std::abs(left - aOther.left) <= 2 &&
           std::abs(right - aOther.right) <= 2;
  }
};

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
  // Whether the panel lies above the browser's, and for how many composites
  // in a row nothing of the page has been drawn in front of the video.
  bool mAbove = false;
  Trim mTrim;
  uint32_t mUncoveredFrames = 0;
  uint32_t mStatAbove = 0;
  uint32_t mStackNotes = 0;
  // What the page drew in front of the video, for the notes.
  uint32_t mStatCovered = 0;
  uint32_t mCoverTiles = 0;
  Rect mCover;
  bool mSaidCover = false;
  bool mPresented = false;
  uint32_t mPlacementNotes = 0;

  // The last frame copied in, so a composite for another reason does not
  // present the same picture again.
  ID3D11Texture2D* mLastTexture = nullptr;
  uint32_t mLastIndex = UINT32_MAX;
  void* mLastHost = nullptr;

  bool mOfferedThisFrame = false;
  uint32_t mFramesWithout = 0;

  uint32_t mStatFrames = 0;
  uint32_t mStatHoles = 0;
  uint32_t mStatOverlay = 0;
  uint32_t mStatComposed = 0;
  uint32_t mStatOther = 0;
  // The display's last answer, to note each time it changes its mind.
  int mLastMode = -1;
  uint32_t mStatPresents = 0;
  uint32_t mStatBusy = 0;
  double mStatPresentMs = 0;
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

  void Stack(bool aAbove, Trim aTrim = Trim()) {
    if (!aAbove) {
      aTrim = Trim();
    }
    if (aAbove == mAbove && aTrim.Near(mTrim)) {
      return;
    }
    mAbove = aAbove;
    mTrim = aTrim;
    if (auto* sink = sSink.load()) {
      sink->stack(aAbove ? 1 : 0, aTrim.top, aTrim.bottom, aTrim.left,
                  aTrim.right);
    }
    if (mStackNotes < 40) {
      ++mStackNotes;
      char note[224];
      if (aAbove) {
        SprintfLiteral(note,
                       "video layer: above the browser, cut off for what the "
                       "page draws there: %d top, %d bottom, %d left, %d right",
                       aTrim.top, aTrim.bottom, aTrim.left, aTrim.right);
      } else {
        SprintfLiteral(note,
                       "video layer: under the browser -- the page draws in "
                       "front of the video, or it moved");
      }
      Note(note);
    }
  }

  // Whether the panel can go above the browser though the page draws over
  // the video: only when all of it lies in a band along one edge, at most a
  // twelfth of the visible height or width, which the panel then leaves out.
  // (A progress bar along the bottom; in landscape fullscreen YouTube keeps
  // something 84 pixels wide down the left edge.)
  bool Trims(int32_t aTiles, const Rect& aCover, Trim* aTrim) const {
    *aTrim = Trim();
    if (aTiles <= 0) {
      return true;
    }
    const int32_t bandY = std::max(4, mClip.h / 12);
    const int32_t bandX = std::max(4, mClip.w / 12);
    const int32_t clipBottom = mClip.y + mClip.h;
    const int32_t clipRight = mClip.x + mClip.w;
    if (aCover.y >= clipBottom - bandY) {
      aTrim->bottom = clipBottom - aCover.y;
      return true;
    }
    if (aCover.y + aCover.h <= mClip.y + bandY) {
      aTrim->top = aCover.y + aCover.h - mClip.y;
      return true;
    }
    if (aCover.x + aCover.w <= mClip.x + bandX) {
      aTrim->left = aCover.x + aCover.w - mClip.x;
      return true;
    }
    if (aCover.x >= clipRight - bandX) {
      aTrim->right = clipRight - aCover.x;
      return true;
    }
    return false;
  }

  void Hide() {
    mLastHole = false;
    mUncoveredFrames = 0;
    Stack(false);
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
      // The panel's scale depends on the chain's size: forget the placement
      // so the next frame sends a new one, and keep the hole closed until
      // the shell has applied it. Without this a quality change kept the old
      // scale, and the new, larger picture showed magnified.
      mRect = Rect();
      mClip = Rect();
      mStableFrames = 0;
      mPlacementNotes = 0;
      char note[128];
      SprintfLiteral(note, "video layer: the swap chain is now %dx%d", aWidth,
                     aHeight);
      Note(note);
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
    const mozilla::TimeStamp presentStart = mozilla::TimeStamp::Now();
    // Tied to the display's refresh, as a video player presents. The overlay
    // test, which the display took as a hardware overlay, presented this
    // way; this layer's presents with no sync interval were composed by the
    // GPU every time. If the display still has frames queued the present is
    // turned down instead of waiting, and the next composite tries again.
    hr = mChain->Present(1, DXGI_PRESENT_DO_NOT_WAIT);
    mStatPresentMs +=
        (mozilla::TimeStamp::Now() - presentStart).ToMilliseconds();
    ++mStatPresents;
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      ++mStatBusy;
      return true;
    }
    if (FAILED(hr)) {
      Break("present failed", hr);
      return false;
    }
    // How the system really showed it: as a hardware overlay, or composed
    // by the GPU after all. Asked four times a second.
    if (mStatPresents % 15 == 0) {
      RefPtr<IDXGISwapChainMedia> media;
      DXGI_FRAME_STATISTICS_MEDIA stats = {};
      if (SUCCEEDED(mChain->QueryInterface(__uuidof(IDXGISwapChainMedia),
                                           getter_AddRefs(media))) &&
          SUCCEEDED(media->GetFrameStatisticsMedia(&stats))) {
        if (stats.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_OVERLAY) {
          ++mStatOverlay;
        } else if (stats.CompositionMode ==
                   DXGI_FRAME_PRESENTATION_MODE_COMPOSED) {
          ++mStatComposed;
        } else {
          ++mStatOther;
        }
        const int mode = int(stats.CompositionMode);
        if (mode != mLastMode) {
          mLastMode = mode;
          Note(mode == DXGI_FRAME_PRESENTATION_MODE_OVERLAY
                   ? "video layer: the system shows it as a HARDWARE OVERLAY "
                     "now"
                   : mode == DXGI_FRAME_PRESENTATION_MODE_COMPOSED
                         ? "video layer: the system composes it on the GPU now"
                         : "video layer: the system shows it some other way "
                           "now");
        }
      }
    }
    mLastTexture = aTexture;
    mLastIndex = index;
    mLastHost = aHost;
    mPresented = true;
    return true;
  }

  // 0: WebRender draws the video; 1: a hole, new this frame; 2: a hole that
  // was already there.
  int32_t Offer(const mozilla::wr::ExternalImageId& aId, const float* aRects,
                int32_t aCoveredTiles) {
    const bool hole = OfferForHole(aId, aRects);
    Rect cover;
    if (aCoveredTiles > 0) {
      cover.x = int32_t(floorf(aRects[8]));
      cover.y = int32_t(floorf(aRects[9]));
      cover.w = int32_t(ceilf(aRects[10])) - cover.x;
      cover.h = int32_t(ceilf(aRects[11])) - cover.y;
      ++mStatCovered;
      mCoverTiles = uint32_t(aCoveredTiles);
      mCover = cover;
      if (!mSaidCover && hole) {
        mSaidCover = true;
        NoteCover("first seen");
      }
    }
    // Above the browser only while the hole is there -- the panel is where
    // this frame's video is -- and nothing of the page is in front of it but
    // a band along an edge, which the panel leaves out; under it at once
    // otherwise. Raised only after a tenth of a second like that, so a
    // player's controls fading in and out do not make the panel jump back
    // and forth.
    Trim trim;
    if (hole && Trims(aCoveredTiles, cover, &trim)) {
      if (++mUncoveredFrames >= 6) {
        Stack(true, trim);
      }
    } else {
      mUncoveredFrames = 0;
      Stack(false);
    }
    if (mAbove) {
      ++mStatAbove;
    }
    const int32_t answer = hole ? (mLastHole ? 2 : 1) : 0;
    mLastHole = hole;
    return answer;
  }
  bool mLastHole = false;

  bool OfferForHole(const mozilla::wr::ExternalImageId& aId,
                    const float* aRects) {
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
    // Edges rounded each on its own: a width rounded on its own came out a
    // pixel wider than the screen in fullscreen, which made a clip of the
    // pixel and a clipped panel.
    auto edge = [](float aValue) { return int32_t(floorf(aValue + 0.5f)); };
    const int32_t x0 = edge(aRects[0]), y0 = edge(aRects[1]);
    const int32_t x1 = edge(aRects[2]), y1 = edge(aRects[3]);
    const int32_t cx0 = std::max(x0, edge(aRects[4]));
    const int32_t cy0 = std::max(y0, edge(aRects[5]));
    const int32_t cx1 = std::min(x1, edge(aRects[6]));
    const int32_t cy1 = std::min(y1, edge(aRects[7]));
    Rect rect{x0, y0, x1 - x0, y1 - y0};
    Rect clip{cx0, cy0, cx1 - cx0, cy1 - cy0};
    if (rect.w <= 0 || rect.h <= 0 || clip.w <= 0 || clip.h <= 0) {
      return false;
    }
    // A fullscreen video's rect runs a pixel past the screen's edge (2561
    // wide on a 2560 screen), and the clip that cuts the pixel off made the
    // panel a clipped one for nothing: what lies off the screen is not seen
    // anyway. Two pixels or less on each side is no clip.
    if (cx0 - x0 <= 2 && cy0 - y0 <= 2 && x1 - cx1 <= 2 && y1 - cy1 <= 2) {
      clip = rect;
    }
    if (rect != mRect || clip != mClip) {
      mRect = rect;
      mClip = clip;
      ++mGeneration;
      mStableFrames = 0;
      if (mPlacementNotes < 40) {
        ++mPlacementNotes;
        char note[192];
        SprintfLiteral(note,
                       "video layer: placed %u -- %dx%d at %d,%d, visible "
                       "%dx%d at %d,%d, chain %dx%d",
                       mGeneration, rect.w, rect.h, rect.x, rect.y, clip.w,
                       clip.h, clip.x, clip.y, mChainWidth, mChainHeight);
        Note(note);
      }
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

  void NoteCover(const char* aWhen) {
    char note[256];
    const double share =
        mClip.w > 0 && mClip.h > 0
            ? 100.0 * double(mCover.w) * mCover.h / (double(mClip.w) * mClip.h)
            : 0.0;
    SprintfLiteral(note,
                   "video layer: in front of the video (%s): %u tiles of the "
                   "page over %dx%d at %d,%d, %.1f%% of what shows of it "
                   "(%dx%d at %d,%d)",
                   aWhen, mCoverTiles, mCover.w, mCover.h, mCover.x, mCover.y,
                   share, mClip.w, mClip.h, mClip.x, mClip.y);
    Note(note);
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
      char note[384];
      SprintfLiteral(note,
                     "video layer: a hole in %u of the last %u composites; the "
                     "system says overlay %u, composed by the GPU %u, other "
                     "%u; a video present took %.1f ms, %u of %u turned "
                     "down as the display was still busy; above the browser "
                     "in %u composites, something in front of it in %u%s",
                     mStatHoles, mStatFrames, mStatOverlay, mStatComposed,
                     mStatOther,
                     mStatPresents ? mStatPresentMs / mStatPresents : 0.0,
                     mStatBusy, mStatPresents, mStatAbove, mStatCovered,
                     mClip == mRect ? "" : "; the panel is clipped");
      Note(note);
      mStatFrames = mStatHoles = 0;
      mStatOverlay = mStatComposed = mStatOther = 0;
      mStatPresents = 0;
      mStatBusy = 0;
      if (mStatCovered) {
        NoteCover("the last of them");
      }
      mStatAbove = 0;
      mStatCovered = 0;
      mStatPresentMs = 0;
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
// then the box the page draws over in front of it, x0 y0 x1 y1 each, in
// device pixels; aCovered, how many of the page's tiles do. True: the display
// shows it, draw a transparent hole.
extern "C" int32_t gecko_w10m_video_layer_frame(uint64_t aExternalImageId,
                                                const float* aRects,
                                                int32_t aCovered) {
  return Layer().Offer(mozilla::wr::ToExternalImageId(aExternalImageId), aRects,
                       aCovered);
}

// From RenderCompositorEGL::EndFrame, once per presented frame.
void GeckoW10mVideoLayerEndFrame() { Layer().EndFrame(); }
