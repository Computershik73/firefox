/* This Source Code Form is subject to the terms of the Mozilla Public
 *
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RenderCompositorD3D11SWGL.h"

#include "RenderCompositorRecordedFrame.h"
#include "RenderThread.h"
#include "gfxConfig.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/layers/Effects.h"
#include "mozilla/layers/TextureD3D11.h"
#include "mozilla/webrender/RenderD3D11TextureHost.h"
#include "mozilla/widget/CompositorWidget.h"
#if defined(GECKO_W10M)
#  include <string.h>

#  include <algorithm>

#  include "mozilla/Sprintf.h"
#  include "mozilla/StaticPrefs_gfx.h"
#  include "nsThreadUtils.h"
#endif

// swgl (gpu_d3d11.h): pixel shading on a Direct3D 11 device.
extern "C" bool SwglGpuAttach(void* aDevice);
extern "C" void SwglGpuStats(uint32_t* aOut8);
extern "C" void SwglGpuMemoryStats(uint32_t* aOut5);
extern "C" void SwglGpuDescribe(char* aOut, size_t aSize);
#if defined(GECKO_W10M)
extern "C" void GeckoW10mNote(const char* aText);
// The pixel shader probe, and what it and the draws found (gpu_d3d11.h).
typedef bool (*SwglGpuWarmupHook)(void* aData, const char* aName, int aPhase);
extern "C" bool SwglGpuWarmup(int aBudgetUs, bool aKeep,
                              SwglGpuWarmupHook aHook, void* aData);
extern "C" const char* SwglGpuWarmupPeek(uint32_t* aCount,
                                         uint32_t* aLargestBytes);
extern "C" uint32_t SwglGpuShaderReport(char* aSum, size_t aSumLen, char* aRef,
                                        size_t aRefLen);
extern "C" void SwglGpuFrameShaderStats(uint32_t* aOut4, const char** aSlowest);
#endif

namespace mozilla {
using namespace layers;

namespace wr {

extern LazyLogModule gRenderThreadLog;
#define LOG(...) MOZ_LOG(gRenderThreadLog, LogLevel::Debug, (__VA_ARGS__))

RenderCompositorD3D11SWGL::UploadMode
RenderCompositorD3D11SWGL::GetUploadMode() {
  int mode = StaticPrefs::gfx_webrender_software_d3d11_upload_mode();
  switch (mode) {
    case 1:
      return Upload_Immediate;
    case 2:
      return Upload_Staging;
    case 3:
      return Upload_StagingNoBlock;
    case 4:
      return Upload_StagingPooled;
    default:
      return Upload_Staging;
  }
}

UniquePtr<RenderCompositor> RenderCompositorD3D11SWGL::Create(
    const RefPtr<widget::CompositorWidget>& aWidget, nsACString& aError) {
  if (!aWidget->GetCompositorOptions().AllowSoftwareWebRenderD3D11() ||
      !gfx::gfxConfig::IsEnabled(gfx::Feature::D3D11_COMPOSITING)) {
    return nullptr;
  }

  void* ctx = wr_swgl_create_context();
  if (!ctx) {
    gfxCriticalNote << "Failed SWGL context creation for WebRender";
    return nullptr;
  }

  RefPtr<CompositorD3D11> compositor = MakeAndAddRef<CompositorD3D11>(aWidget);
  nsCString log;
  if (!compositor->Initialize(&log)) {
    gfxCriticalNote << "Failed to initialize CompositorD3D11 for SWGL: "
                    << log.get();
    return nullptr;
  }
  auto result = MakeUnique<RenderCompositorD3D11SWGL>(compositor, aWidget, ctx);
  if (StaticPrefs::gfx_webrender_software_d3d11_gpu_shading_AtStartup()) {
    wr_swgl_make_current(ctx);
    result->mGpuShading = SwglGpuAttach(compositor->GetDevice());
#if defined(GECKO_W10M)
    GeckoW10mNote(result->mGpuShading
                    ? "gfx: SWGL shades pixels on the GPU, tiles are render "
                      "targets"
                    : "gfx: SWGL could not take the device -- pixels stay on "
                      "the CPU");
    // A device reset makes the compositor again, and the line with it.
    static uint32_t sDeviceNotes = 0;
    if (result->mGpuShading && sDeviceNotes++ < 4) {
      char desc[400];
      SwglGpuDescribe(desc, sizeof(desc));
      char note[480];
      SprintfLiteral(note, "gfx: GPU shading device: %s", desc);
      GeckoW10mNote(note);
    }
#endif
    if (result->mGpuShading) {
      // Tiles are drawn on the GPU; the staging paths never run.
      result->mUploadMode = Upload_Staging;
    }
  }
  return result;
}

RenderCompositorD3D11SWGL::RenderCompositorD3D11SWGL(
    CompositorD3D11* aCompositor,
    const RefPtr<widget::CompositorWidget>& aWidget, void* aContext)
    : RenderCompositorLayersSWGL(aCompositor, aWidget, aContext) {
  LOG("RenderCompositorD3D11SWGL::RenderCompositorD3D11SWGL()");

  mSyncObject = GetCompositorD3D11()->GetSyncObject();
}

#if defined(GECKO_W10M)
// The compositor the pixel shader probe works for, and the serial of the
// step scheduled last; both are touched only on the render thread. A step
// runs only if it is still the last one scheduled and its compositor still
// the one probing, so a step left waiting by a compositor that is gone (a
// device reset makes a new one) does nothing.
static RenderCompositorD3D11SWGL* sWarmupOwner = nullptr;
static uint32_t sWarmupGen = 0;
// "GPU programs refused" lines, from the probe and from the 300-frame block.
static uint32_t sRefusedNotes = 0;

// A SWGL program's name without the "_gpuv" its GPU variant's name ends in.
static int ShortNameLength(const char* aName) {
  size_t length = strlen(aName);
  if (length >= 5 && !strcmp(aName + length - 5, "_gpuv")) {
    length -= 5;
  }
  return int(length);
}

// The probe's crash guard. A draw creates the pixel shaders of the programs
// its pages use; the probe creates all of them, every run. A driver that
// dies compiling some rarely used shader would then die every run, a few
// seconds in, with nothing to be done short of a new build. So while a
// create runs, swgl-gpu-probe.txt next to the note log names that program on
// its first line ("-" otherwise), and the lines after it are the programs
// to leave alone. A run that finds a name there knows the last probe died
// in that create: the program joins the list, and from then on it is left
// to its first draw, as before the probe -- never sent to the CPU, so a
// false alarm (the system ending a suspended app mid-create) costs nothing.
// The writes are not flushed: what a crashed process wrote is still in the
// system cache. All of it happens at idle. Without the note log's variable,
// or the file, the probe runs unguarded.
static bool GuardListed(const nsCString& aSkip, const char* aName) {
  size_t length = strlen(aName);
  const char* line = aSkip.get();
  while (*line) {
    const char* end = strchr(line, '\n');
    size_t n = end ? size_t(end - line) : strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) {
      n--;
    }
    if (n == length && !memcmp(line, aName, length)) {
      return true;
    }
    if (!end) {
      break;
    }
    line = end + 1;
  }
  return false;
}

static void GuardWrite(GpuWarmupGuard& aGuard, const char* aFirst) {
  if (aGuard.mFile == INVALID_HANDLE_VALUE) {
    return;
  }
  nsAutoCString text(aFirst);
  text.Append('\n');
  text.Append(aGuard.mSkip);
  ::SetFilePointer(aGuard.mFile, 0, nullptr, FILE_BEGIN);
  DWORD written = 0;
  ::WriteFile(aGuard.mFile, text.get(), static_cast<DWORD>(text.Length()),
              &written, nullptr);
  ::SetEndOfFile(aGuard.mFile);
}

static void GuardOpen(GpuWarmupGuard& aGuard) {
  if (aGuard.mFile != INVALID_HANDLE_VALUE) {
    return;
  }
  wchar_t path[MAX_PATH] = {};
  DWORD length =
      ::GetEnvironmentVariableW(L"GECKO_W10M_NOTE_LOG", path, MAX_PATH);
  if (!length || length >= MAX_PATH) {
    return;
  }
  size_t dir = length;
  while (dir > 0 && path[dir - 1] != L'\\' && path[dir - 1] != L'/') {
    dir--;
  }
  static const wchar_t kName[] = L"swgl-gpu-probe.txt";
  if (dir + sizeof(kName) / sizeof(kName[0]) > MAX_PATH) {
    return;
  }
  memcpy(path + dir, kName, sizeof(kName));
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  HANDLE file =
      ::CreateFile2(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    OPEN_ALWAYS, &params);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  aGuard.mFile = file;
  char buf[2048];
  DWORD got = 0;
  if (!::ReadFile(file, buf, sizeof(buf) - 1, &got, nullptr)) {
    got = 0;
  }
  buf[got] = 0;
  // The first line: what was being created when the last probe ended.
  const char* rest = strchr(buf, '\n');
  nsAutoCString first(buf, rest ? size_t(rest - buf) : strlen(buf));
  first.Trim(" \r\t");
  aGuard.mSkip.Assign(rest ? rest + 1 : "");
  if (!aGuard.mSkip.IsEmpty() && aGuard.mSkip.Last() != '\n') {
    aGuard.mSkip.Append('\n');
  }
  if (!first.IsEmpty() && !first.EqualsLiteral("-")) {
    if (!GuardListed(aGuard.mSkip, first.get()) &&
        aGuard.mSkip.Length() < 1900) {
      aGuard.mSkip.Append(first);
      aGuard.mSkip.Append('\n');
    }
    static uint32_t sSkipNotes = 0;
    if (sSkipNotes++ < 4) {
      char note[320];
      SprintfLiteral(note,
                     "gfx: GPU program probe skips %.200s: it was being "
                     "created when the last probe ended",
                     first.get());
      GeckoW10mNote(note);
    }
  }
  // Nothing is being created now, and the next run must not count this
  // one's line again.
  GuardWrite(aGuard, "-");
}

static void GuardClose(GpuWarmupGuard& aGuard) {
  if (aGuard.mFile != INVALID_HANDLE_VALUE) {
    ::CloseHandle(aGuard.mFile);
    aGuard.mFile = INVALID_HANDLE_VALUE;
  }
}

// SwglGpuWarmup's hook: phase 0 is just before a create, phase 1 just after.
static bool GuardHook(void* aData, const char* aName, int aPhase) {
  auto* guard = static_cast<GpuWarmupGuard*>(aData);
  if (aPhase == 0) {
    if (GuardListed(guard->mSkip, aName)) {
      return false;
    }
    GuardWrite(*guard, aName);
  } else {
    GuardWrite(*guard, "-");
  }
  return true;
}
#endif

RenderCompositorD3D11SWGL::~RenderCompositorD3D11SWGL() {
  LOG("RenderCompositorD3D11SWGL::~RenderCompositorD3D11SWGL()");
#if defined(GECKO_W10M)
  // A probe step still waiting for this compositor will do nothing.
  if (sWarmupOwner == this) {
    sWarmupOwner = nullptr;
    ++sWarmupGen;
  }
  GuardClose(mGuard);
#endif
}

#if defined(GECKO_W10M)
void RenderCompositorD3D11SWGL::ScheduleGpuWarmup(uint32_t aDelayMs) {
  sWarmupOwner = this;
  const uint32_t gen = ++sWarmupGen;
  RenderCompositorD3D11SWGL* self = this;
  (void)NS_DelayedDispatchToCurrentThread(
      NS_NewRunnableFunction("RenderCompositorD3D11SWGL::GpuWarmupStep",
                             [self, gen]() {
                               if (sWarmupOwner == self && sWarmupGen == gen) {
                                 self->GpuWarmupStep();
                               }
                             }),
      aDelayMs);
}

// One look at whether the render thread is idle, and if it is, one batch of
// the probe. It runs on that thread between WebRender's tasks, never inside
// a frame.
void RenderCompositorD3D11SWGL::GpuWarmupStep() {
  if (!mGpuShading || mGpuWarmupDone) {
    return;
  }
  if (FAILED(GetDevice()->GetDeviceRemovedReason())) {
    return;
  }
  // Idle is at most one GPU-shaded frame since the last look -- a caret's
  // blink or a slow GIF is idle, scrolling, video and animation are not --
  // and between two batches no frame at all. Busy looks again in half a
  // second, and a frame between batches waits that long too.
  const uint32_t frames = mFrameProbes - mWarmupSeenFrames;
  mWarmupSeenFrames = mFrameProbes;
  if (frames > (mWarmupBatching ? 0u : 1u)) {
    mWarmupBatching = false;
    ScheduleGpuWarmup(500);
    return;
  }
  MakeCurrent();
  const bool keep =
      StaticPrefs::
          gfx_webrender_software_d3d11_gpu_shading_warmup_AtStartup() >= 2;
  if (!mWarmupBatching && !mWarmupNoted) {
    mWarmupNoted = true;
    GuardOpen(mGuard);
    // Before the first create, so that a crash in one shows in the log.
    static uint32_t sStartNotes = 0;
    if (sStartNotes++ < 2) {
      uint32_t count = 0, largest = 0;
      const char* name = SwglGpuWarmupPeek(&count, &largest);
      char note[256];
      SprintfLiteral(note,
                     "gfx: GPU program probe starting -- %u programs, largest "
                     "%.*s (%u KB), %s",
                     count, name ? ShortNameLength(name) : 1, name ? name : "-",
                     (largest + 512) / 1024,
                     keep ? "kept once made" : "released once made (mode 1)");
      GeckoW10mNote(note);
    }
  }
  if (!SwglGpuWarmup(4000, keep, GuardHook, &mGuard)) {
    // About four milliseconds a batch, one create at the least; then the
    // thread is WebRender's again for 32 ms.
    mWarmupBatching = true;
    ScheduleGpuWarmup(32);
    return;
  }
  mGpuWarmupDone = true;
  GuardClose(mGuard);
  static uint32_t sReports = 0;
  if (sReports++ < 2) {
    char sum[400];
    char ref[400];
    const uint32_t refused =
        SwglGpuShaderReport(sum, sizeof(sum), ref, sizeof(ref));
    char note[480];
    SprintfLiteral(note,
                   "gfx: GPU programs -- %s; %u s after the first GPU frame",
                   sum, unsigned((GetTickCount64() - mGpuWarmupStart) / 1000));
    GeckoW10mNote(note);
    if (ref[0] && sRefusedNotes < 3) {
      sRefusedNotes++;
      SprintfLiteral(note, "gfx: GPU programs refused: %s", ref);
      GeckoW10mNote(note);
    }
    mRefusedNoted = refused;
  }
}
#endif

void RenderCompositorD3D11SWGL::StartCompositing(
    wr::ColorF aClearColor, const wr::DeviceIntRect* aDirtyRects,
    size_t aNumDirtyRects, const wr::DeviceIntRect* aOpaqueRects,
    size_t aNumOpaqueRects) {
  if (mGpuShading) {
    // SWGL has just drawn the tiles with the same immediate context.
    GetCompositorD3D11()->ForgetDeviceState();
#if defined(GECKO_W10M)
    if (++mGpuStatsFrames % 300 == 1) {
      uint32_t m[5];
      SwglGpuMemoryStats(m);  // before SwglGpuStats, which resets the counts
      uint32_t s[8];
      SwglGpuStats(s);
      char note[256];
      SprintfLiteral(note,
                     "gfx: GPU shading -- %u GPU draws, %u CPU draws, %u quads, "
                     "%u uploads, %u readbacks, %u clears, refused %u program "
                     "%u state",
                     s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
      GeckoW10mNote(note);
      SprintfLiteral(note,
                     "gfx: GPU-only textures -- %u CPU copies freed, %u brought "
                     "back, %u MB of CPU memory saved now; Direct3D holds %u MB "
                     "of textures and %u MB of caches",
                     m[0], m[1], m[2], m[3], m[4]);
      GeckoW10mNote(note);
      // The compositor's own tile textures, which SWGL draws into.
      size_t tiles = 0;
      uint64_t tileBytes = 0;
      for (auto& it : mSurfaces) {
        Surface* surface = it.second.get();
        tiles += surface->mTiles.size();
        tileBytes += uint64_t(surface->mTiles.size()) *
                     surface->mTileSize.width * surface->mTileSize.height * 4;
      }
      // And how much of the screen a composite covered on average since the
      // last time -- what partial redraw saves, with a video or without.
      SprintfLiteral(note,
                     "gfx: compositor tiles -- %u of them, %u MB; composited "
                     "%.2f of the screen a frame",
                     unsigned(tiles), unsigned(tileBytes >> 20),
                     mW10mCompositedFrames
                         ? mW10mCompositedSum / mW10mCompositedFrames
                         : 0.0);
      GeckoW10mNote(note);
      mW10mCompositedSum = 0;
      mW10mCompositedFrames = 0;
      // Pixel shaders the draws made inside frames since the last block --
      // what the probe is there to take out of them -- and programs refused
      // since the log last named them, the draws' refusals included, so
      // both show with the probe off as well.
      uint32_t f[4];
      const char* slowest = nullptr;
      SwglGpuFrameShaderStats(f, &slowest);
      static uint32_t sMadeNotes = 0;
      if (f[0] && sMadeNotes < 20) {
        sMadeNotes++;
        SprintfLiteral(note,
                       "gfx: pixel shaders made during frames -- %u, %u.%u ms "
                       "in all, slowest %.*s %u.%u ms",
                       f[0], f[1] / 1000, f[1] % 1000 / 100,
                       slowest ? ShortNameLength(slowest) : 1,
                       slowest ? slowest : "-", f[2] / 1000, f[2] % 1000 / 100);
        GeckoW10mNote(note);
      }
      if (f[3] > mRefusedNoted && sRefusedNotes < 3) {
        sRefusedNotes++;
        mRefusedNoted = f[3];
        char ref[400];
        SwglGpuShaderReport(nullptr, 0, ref, sizeof(ref));
        char refNote[480];
        SprintfLiteral(refNote, "gfx: GPU programs refused: %s", ref);
        GeckoW10mNote(refNote);
      }
    }
#endif
  }
#if defined(GECKO_W10M)
  if (!StaticPrefs::gfx_webrender_gecko_w10m_video_whole_window()) {
    W10mStartCompositing(aClearColor, aDirtyRects, aNumDirtyRects,
                         aOpaqueRects, aNumOpaqueRects);
    return;
  }
#endif
  if (mDrewExternalImage) {
    // A video surface changes with every frame of its own, and WebRender
    // does not count that in its dirty rects: composite the whole window,
    // or the back buffer would show the video as it was two frames ago
    // (flip model) where nothing else changed.
    gfx::IntSize size = GetBufferSize().ToUnknownSize();
    wr::DeviceIntRect whole;
    whole.min.x = 0;
    whole.min.y = 0;
    whole.max.x = size.width;
    whole.max.y = size.height;
#if defined(GECKO_W10M)
    W10mCountComposited(&whole, 1);
#endif
    RenderCompositorLayersSWGL::StartCompositing(aClearColor, &whole, 1,
                                                 aOpaqueRects, aNumOpaqueRects);
    return;
  }
#if defined(GECKO_W10M)
  W10mCountComposited(aDirtyRects, aNumDirtyRects);
#endif
  RenderCompositorLayersSWGL::StartCompositing(aClearColor, aDirtyRects,
                                               aNumDirtyRects, aOpaqueRects,
                                               aNumOpaqueRects);
}

#if defined(GECKO_W10M)
// Partial redraw with a video on the screen. The whole-window composite above
// was there for a video surface's own frames, but WebRender does count them:
// it puts an external surface into its dirty rect whole every time it
// composites one (push_compositor_surface), and invalidates where a video
// moved, shrank or went away; CompositorD3D11 copies what did not change
// forward from the other buffer. So the whole window was composited on every
// video frame -- for an inline YouTube player, three times the area that
// changed, on a GPU that has no fill to spare -- and partial redraw was off
// whenever a video played. Here the composite is WebRender's dirty area plus,
// as a second net, where the video is drawn now and where it was drawn last
// time, worked out the way HandleExternalImage draws it: the host's size
// through the surface's transform, inside its clip.
void RenderCompositorD3D11SWGL::W10mStartCompositing(
    wr::ColorF aClearColor, const wr::DeviceIntRect* aDirtyRects,
    size_t aNumDirtyRects, const wr::DeviceIntRect* aOpaqueRects,
    size_t aNumOpaqueRects) {
  if (!mInFrame || aNumDirtyRects == 0) {
    // Nothing changed, a video included: there is no composite to make.
    RenderCompositorLayersSWGL::StartCompositing(aClearColor, aDirtyRects,
                                                 aNumDirtyRects, aOpaqueRects,
                                                 aNumOpaqueRects);
    return;
  }

  const gfx::IntSize bufferSize = GetBufferSize().ToUnknownSize();
  const gfx::Rect bounds(0, 0, bufferSize.width, bufferSize.height);
  gfx::IntRect videoNow;
  for (const auto& frameSurface : mFrameSurfaces) {
    auto cursor = mSurfaces.find(frameSurface.mId);
    if (cursor == mSurfaces.end()) {
      continue;
    }
    Surface* surface = cursor->second.get();
    if (!surface->mIsExternal || !surface->mExternalImage) {
      continue;
    }
    gfx::IntSize size;
    if (auto* host = surface->mExternalImage->AsRenderDXGITextureHost()) {
      size = host->GetSize(0);
    } else if (auto* host =
                   surface->mExternalImage->AsRenderDXGIYCbCrTextureHost()) {
      size = host->GetSize(0);
    } else {
      continue;
    }
    gfx::Rect drawn = frameSurface.mTransform.TransformBounds(
        gfx::Rect(0, 0, size.width, size.height));
    drawn = drawn.Intersect(bounds);
    drawn.RoundOut();
    gfx::IntRect drawnInt;
    if (!drawn.ToIntRect(&drawnInt)) {
      continue;
    }
    videoNow = videoNow.Union(drawnInt.Intersect(frameSurface.mClipRect));
  }

  if (!videoNow.IsEmpty()) {
    static bool sModeNoted = false;
    if (!sModeNoted) {
      sModeNoted = true;
      GeckoW10mNote(
          "gfx: video frames redraw the dirty area and the video, not the "
          "whole window");
    }
    // WebRender rounds its rect and this rounds out: a pixel's difference at
    // an edge is not a video left out.
    nsIntRegion webrender;
    for (size_t i = 0; i < aNumDirtyRects; i++) {
      const auto& rect = aDirtyRects[i];
      webrender.OrWith(
          gfx::IntRect(rect.min.x, rect.min.y, rect.width(), rect.height()));
    }
    gfx::IntRect inner = videoNow;
    inner.Deflate(1);
    static uint32_t sLeftOutNotes = 0;
    if (!inner.IsEmpty() && !webrender.Contains(inner) && sLeftOutNotes < 5) {
      sLeftOutNotes++;
      char note[200];
      SprintfLiteral(note,
                     "gfx: WebRender's dirty rect left out a video surface "
                     "(%d,%d %dx%d) -- added to the redraw",
                     videoNow.x, videoNow.y, videoNow.width, videoNow.height);
      GeckoW10mNote(note);
    }
  }

  if (videoNow.IsEmpty() && mW10mPrevVideo.IsEmpty()) {
    W10mCountComposited(aDirtyRects, aNumDirtyRects);
    RenderCompositorLayersSWGL::StartCompositing(aClearColor, aDirtyRects,
                                                 aNumDirtyRects, aOpaqueRects,
                                                 aNumOpaqueRects);
    return;
  }

  AutoTArray<wr::DeviceIntRect, 8> rects;
  rects.AppendElements(aDirtyRects, aNumDirtyRects);
  for (const gfx::IntRect* video : {&videoNow, &mW10mPrevVideo}) {
    if (video->IsEmpty()) {
      continue;
    }
    wr::DeviceIntRect rect;
    rect.min.x = video->X();
    rect.min.y = video->Y();
    rect.max.x = video->XMost();
    rect.max.y = video->YMost();
    rects.AppendElement(rect);
  }
  if (rects.Length() > 4) {
    wr::DeviceIntRect all = rects[0];
    for (const auto& rect : rects) {
      all.min.x = std::min(all.min.x, rect.min.x);
      all.min.y = std::min(all.min.y, rect.min.y);
      all.max.x = std::max(all.max.x, rect.max.x);
      all.max.y = std::max(all.max.y, rect.max.y);
    }
    rects.SetLength(1);
    rects[0] = all;
  }
  mW10mPrevVideo = videoNow;
  W10mCountComposited(rects.Elements(), rects.Length());
  RenderCompositorLayersSWGL::StartCompositing(
      aClearColor, rects.Elements(), rects.Length(), aOpaqueRects,
      aNumOpaqueRects);
}

// How much of the screen this composite covers, for the 300-frame note --
// which only GPU shading writes, so only then.
void RenderCompositorD3D11SWGL::W10mCountComposited(
    const wr::DeviceIntRect* aRects, size_t aNumRects) {
  if (!mGpuShading || !mInFrame || !aNumRects) {
    return;
  }
  const gfx::IntSize size = GetBufferSize().ToUnknownSize();
  if (size.width <= 0 || size.height <= 0) {
    return;
  }
  nsIntRegion covered;
  for (size_t i = 0; i < aNumRects; i++) {
    const auto& rect = aRects[i];
    covered.OrWith(
        gfx::IntRect(rect.min.x, rect.min.y, rect.width(), rect.height()));
  }
  covered.AndWith(gfx::IntRect(gfx::IntPoint(0, 0), size));
  mW10mCompositedSum +=
      double(covered.Area()) / (double(size.width) * size.height);
  mW10mCompositedFrames++;
}
#endif

// Reads one pixel of a texture on the compositor's device (BGRA as stored).
bool RenderCompositorD3D11SWGL::ProbePixel(ID3D11Texture2D* aTexture, int aX,
                                           int aY, uint32_t* aOut) {
  if (!aTexture) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc;
  aTexture->GetDesc(&desc);
  if (aX < 0 || aY < 0 || UINT(aX) >= desc.Width || UINT(aY) >= desc.Height) {
    return false;
  }
  CD3D11_TEXTURE2D_DESC sd(desc.Format, 1, 1, 1, 1, 0, D3D11_USAGE_STAGING,
                           D3D11_CPU_ACCESS_READ);
  RefPtr<ID3D11Texture2D> staging;
  if (FAILED(GetDevice()->CreateTexture2D(&sd, nullptr,
                                          getter_AddRefs(staging)))) {
    return false;
  }
  RefPtr<ID3D11DeviceContext> context;
  GetDevice()->GetImmediateContext(getter_AddRefs(context));
  D3D11_BOX box = {UINT(aX), UINT(aY), 0, UINT(aX + 1), UINT(aY + 1), 1};
  context->CopySubresourceRegion(staging, 0, 0, 0, 0, aTexture, 0, &box);
  D3D11_MAPPED_SUBRESOURCE map;
  if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
    return false;
  }
  *aOut = *(const uint32_t*)map.pData;
  context->Unmap(staging, 0);
  return true;
}

void RenderCompositorD3D11SWGL::CompositorEndFrame() {
  mDrewExternalImage = false;
  if (mGpuShading) {
    // WebRender started compositing before it drew the tiles, and SWGL drew
    // them with this context: the composite needs its own state back.
    GetCompositorD3D11()->RestoreDeviceState();
  }
  RenderCompositorLayersSWGL::CompositorEndFrame();
}

RenderedFrameId RenderCompositorD3D11SWGL::EndFrame(
    const nsTArray<DeviceIntRect>& aDirtyRects) {
#if defined(GECKO_W10M)
  if (mGpuShading && (mFrameProbes < 8 || mFrameProbes % 300 == 0)) {
    // What the composite put on screen, before it is presented.
    RefPtr<CompositingRenderTarget> rt =
        GetCompositorD3D11()->GetCurrentRenderTarget();
    auto* rt11 = static_cast<CompositingRenderTargetD3D11*>(rt.get());
    char note[256];
    HRESULT removed = GetDevice()->GetDeviceRemovedReason();
    if (rt11 && rt11->GetD3D11Texture()) {
      gfx::IntSize size = rt11->GetSize();
      uint32_t c = 0, l = 0, t = 0;
      bool ok = ProbePixel(rt11->GetD3D11Texture(), size.width / 2,
                           size.height / 2, &c) &&
                ProbePixel(rt11->GetD3D11Texture(), size.width / 4,
                           size.height / 2, &l) &&
                ProbePixel(rt11->GetD3D11Texture(), size.width / 2,
                           size.height / 8, &t);
      SprintfLiteral(note,
                     "gfx: frame probe %u: screen %dx%d centre %08x left %08x "
                     "top %08x%s, device %08lx",
                     mFrameProbes, size.width, size.height, c, l, t,
                     ok ? "" : " (probe failed)", (unsigned long)removed);
    } else {
      SprintfLiteral(note,
                     "gfx: frame probe %u: no composite this frame, device "
                     "%08lx",
                     mFrameProbes, (unsigned long)removed);
    }
    GeckoW10mNote(note);
  }
  if (mGpuShading) {
    mFrameProbes++;
    // The pixel shader probe counts from the first GPU-shaded frame; after
    // that this is the one test a frame it costs.
    if (!mGpuWarmupScheduled) {
      mGpuWarmupScheduled = true;
      mGpuWarmupStart = GetTickCount64();
      mWarmupSeenFrames = mFrameProbes;
      if (StaticPrefs::
              gfx_webrender_software_d3d11_gpu_shading_warmup_AtStartup() >
          0) {
        ScheduleGpuWarmup(
            StaticPrefs::
                gfx_webrender_software_d3d11_gpu_shading_warmup_delay_ms_AtStartup());
      }
    }
  }
#endif
  return RenderCompositorLayersSWGL::EndFrame(aDirtyRects);
}

bool RenderCompositorD3D11SWGL::BeginFrame() {
  if (!RenderCompositorLayersSWGL::BeginFrame()) {
    return false;
  }

  // With GPU shading the mode stays the Upload_Staging that Create() set:
  // upload-mode 1 would make new tiles Immediate ones, whose texture source
  // has no Direct3D texture, and Map would hand SWGL a null one to draw into.
  if (!mGpuShading) {
    mUploadMode = GetUploadMode();
  }
  return true;
}

void RenderCompositorD3D11SWGL::HandleExternalImage(
    RenderTextureHost* aExternalImage, FrameSurface& aFrameSurface) {
  // We need to hold the texture source separately from the effect,
  // since the effect doesn't hold a strong reference.
  RefPtr<DataTextureSourceD3D11> layer;
  RefPtr<TexturedEffect> texturedEffect;
  gfx::IntSize size;
  if (auto* host = aExternalImage->AsRenderDXGITextureHost()) {
    if (!host->EnsureD3D11Texture2D(GetDevice())) {
      return;
    }

#if defined(GECKO_W10M)
    // The frame order check. At feature level 9_3 every conversion leaves a
    // rising number on its texture (DXVA2Manager.cpp); textures without one
    // (feature level 11, NV12, software frames) are not counted. A number
    // lower than the one this surface drew before is a frame older than the
    // last -- the decoder's order, the compositor going back, or WebRender
    // mixing up keys. The same texture, with no new image attached since it
    // was drawn but a different number, was written while still on the
    // screen: texture reuse, which the code says cannot happen.
    if (ID3D11Texture2D* tex = host->GetD3D11Texture2D()) {
      uint32_t seq = 0;
      UINT seqSize = sizeof(seq);
      if (FAILED(tex->GetPrivateData(kGeckoW10mFrameSeq, &seqSize, &seq)) ||
          seqSize != sizeof(seq)) {
        seq = 0;
      }
      auto cursor = mSurfaces.find(aFrameSurface.mId);
      if (seq && cursor != mSurfaces.end()) {
        static uint32_t sDraws = 0;
        static uint32_t sOlder = 0;
        static uint32_t sRewritten = 0;
        static uint32_t sNotes = 0;
        Surface* surface = cursor->second.get();
        if (seq < surface->mW10mDrawnSeq) {
          sOlder++;
        }
        if (tex == surface->mW10mDrawnTex &&
            surface->mW10mAttaches == surface->mW10mDrawnAttach &&
            seq != surface->mW10mDrawnSeq) {
          sRewritten++;
        }
        surface->mW10mDrawnTex = tex;
        surface->mW10mDrawnAttach = surface->mW10mAttaches;
        surface->mW10mDrawnSeq = seq;
        if (++sDraws == 900) {
          if (sNotes < 20) {
            sNotes++;
            char note[256];
            SprintfLiteral(note,
                           "video: %u composites drew the video; %u drew an "
                           "older conversion than the one before, %u drew a "
                           "texture rewritten since it was attached",
                           sDraws, sOlder, sRewritten);
            GeckoW10mNote(note);
          }
          sDraws = sOlder = sRewritten = 0;
        }
      }
    }
#endif

    layer = new DataTextureSourceD3D11(GetDevice(), host->GetFormat(),
                                       host->GetD3D11Texture2D());
    if (host->GetFormat() == gfx::SurfaceFormat::NV12 ||
        host->GetFormat() == gfx::SurfaceFormat::P010 ||
        host->GetFormat() == gfx::SurfaceFormat::P016) {
      const auto yuv = FromYUVRangedColorSpace(host->GetYUVColorSpace());
      texturedEffect =
          new EffectNV12(layer, yuv.space, yuv.range, host->GetColorDepth(),
                         aFrameSurface.mFilter);
    } else {
      MOZ_ASSERT(host->GetFormat() == gfx::SurfaceFormat::B8G8R8X8 ||
                 host->GetFormat() == gfx::SurfaceFormat::B8G8R8A8);
      texturedEffect = CreateTexturedEffect(host->GetFormat(), layer,
                                            aFrameSurface.mFilter, true);
    }
    size = host->GetSize(0);
    host->LockInternal();
  } else if (auto* host = aExternalImage->AsRenderDXGIYCbCrTextureHost()) {
    if (!host->EnsureD3D11Texture2D(GetDevice())) {
      return;
    }

    layer = new DataTextureSourceD3D11(GetDevice(), gfx::SurfaceFormat::A8,
                                       host->GetD3D11Texture2D(0));
    RefPtr<DataTextureSourceD3D11> u = new DataTextureSourceD3D11(
        GetDevice(), gfx::SurfaceFormat::A8, host->GetD3D11Texture2D(1));
    layer->SetNextSibling(u);
    RefPtr<DataTextureSourceD3D11> v = new DataTextureSourceD3D11(
        GetDevice(), gfx::SurfaceFormat::A8, host->GetD3D11Texture2D(2));
    u->SetNextSibling(v);

    const auto yuv = FromYUVRangedColorSpace(host->GetYUVColorSpace());
    texturedEffect =
        new EffectYCbCr(layer, yuv.space, yuv.range, host->GetColorDepth(),
                        aFrameSurface.mFilter);
    size = host->GetSize(0);
    host->LockInternal();
  }

  gfx::Rect drawRect(0, 0, size.width, size.height);
  mDrewExternalImage = true;

#if defined(GECKO_W10M)
  static bool sNoted = false;
  if (!sNoted && texturedEffect) {
    sNoted = true;
    GeckoW10mNote("gfx: a video frame drawn by the D3D11 compositor as a surface "
                "of its own");
  }
#endif

  EffectChain effect;
  effect.mPrimaryEffect = texturedEffect;
  mCompositor->DrawQuad(drawRect, aFrameSurface.mClipRect, effect, 1.0,
                        aFrameSurface.mTransform, drawRect);

  if (auto* host = aExternalImage->AsRenderDXGITextureHost()) {
    host->Unlock();
  } else if (auto* host = aExternalImage->AsRenderDXGIYCbCrTextureHost()) {
    host->Unlock();
  }
}

void RenderCompositorD3D11SWGL::Pause() {}

bool RenderCompositorD3D11SWGL::Resume() { return true; }

gfx::DeviceResetReason RenderCompositorD3D11SWGL::IsContextLost(bool aForce) {
  // CompositorD3D11 uses ID3D11Device for composite. The device status needs to
  // be checked.
  auto reason = GetDevice()->GetDeviceRemovedReason();
  return layers::DXGIErrorToDeviceResetReason(reason);
}

UniquePtr<RenderCompositorLayersSWGL::Surface>
RenderCompositorD3D11SWGL::DoCreateSurface(wr::DeviceIntSize aTileSize,
                                           bool aIsOpaque) {
  return MakeUnique<SurfaceD3D11SWGL>(aTileSize, aIsOpaque);
}

SurfaceD3D11SWGL::SurfaceD3D11SWGL(wr::DeviceIntSize aTileSize, bool aIsOpaque)
    : Surface(aTileSize, aIsOpaque) {}

RenderCompositorD3D11SWGL::TileD3D11::TileD3D11(
    layers::DataTextureSourceD3D11* aTexture, ID3D11Texture2D* aStagingTexture,
    gfx::DataSourceSurface* aDataSourceSurface, Surface* aOwner,
    RenderCompositorD3D11SWGL* aRenderCompositor)
    : Tile(),
      mTexture(aTexture),
      mStagingTexture(aStagingTexture),
      mSurface(aDataSourceSurface),
      mOwner(aOwner->AsSurfaceD3D11SWGL()),
      mRenderCompositor(aRenderCompositor) {}

bool RenderCompositorD3D11SWGL::TileD3D11::Map(wr::DeviceIntRect aDirtyRect,
                                               wr::DeviceIntRect aValidRect,
                                               void** aData, int32_t* aStride) {
  const UploadMode uploadMode = mRenderCompositor->GetUploadMode();
  const gfx::IntSize tileSize = mOwner->TileSize();

  if (!IsValid()) {
    return false;
  }

  if (mRenderCompositor->mGpuShading) {
    // SWGL draws into the tile texture itself, on the GPU: the texture goes
    // over instead of memory, flagged by a negative stride (sw_compositor.rs).
    *aData = mTexture->GetD3D11Texture();
    *aStride = -1;
    mValidRect = gfx::Rect(aValidRect.min.x, aValidRect.min.y,
                           aValidRect.width(), aValidRect.height());
    return true;
  }

  // Check if this tile's upload method matches what we're using for this frame,
  // and if not then reallocate to fix it. Do this before we copy the struct
  // into mCurrentTile.
  if (uploadMode == Upload_Immediate) {
    if (mStagingTexture) {
      MOZ_ASSERT(!mSurface);
      mStagingTexture = nullptr;
      mSurface = mRenderCompositor->CreateStagingSurface(tileSize);
    }
  } else {
    if (mSurface) {
      MOZ_ASSERT(!mStagingTexture);
      mSurface = nullptr;
      mStagingTexture = mRenderCompositor->CreateStagingTexture(tileSize);
    }
  }

  mRenderCompositor->mCurrentStagingTexture = mStagingTexture;
  mRenderCompositor->mCurrentStagingTextureIsTemp = false;

  if (uploadMode == Upload_Immediate) {
    gfx::DataSourceSurface::MappedSurface map;
    if (!mSurface->Map(gfx::DataSourceSurface::READ_WRITE, &map)) {
      return false;
    }

    *aData = map.mData + aValidRect.min.y * map.mStride + aValidRect.min.x * 4;
    *aStride = map.mStride;
    // Ensure our mapped data is accessible by writing to the beginning and end
    // of the dirty region. See bug 1717519
    if (aDirtyRect.width() > 0 && aDirtyRect.height() > 0) {
      uint32_t* probeData = (uint32_t*)map.mData +
                            aDirtyRect.min.y * (map.mStride / 4) +
                            aDirtyRect.min.x;
      *probeData = 0;
      uint32_t* probeDataEnd = (uint32_t*)map.mData +
                               (aDirtyRect.max.y - 1) * (map.mStride / 4) +
                               (aDirtyRect.max.x - 1);
      *probeDataEnd = 0;
    }

    mValidRect = gfx::Rect(aValidRect.min.x, aValidRect.min.y,
                           aValidRect.width(), aValidRect.height());
    return true;
  }

  if (!mRenderCompositor->mCurrentStagingTexture) {
    return false;
  }

  RefPtr<ID3D11DeviceContext> context;
  mRenderCompositor->GetDevice()->GetImmediateContext(getter_AddRefs(context));

  D3D11_MAPPED_SUBRESOURCE mappedSubresource;

  bool shouldBlock = uploadMode == Upload_Staging;

  HRESULT hr = context->Map(
      mRenderCompositor->mCurrentStagingTexture, 0, D3D11_MAP_READ_WRITE,
      shouldBlock ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mappedSubresource);
  if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
    // mCurrentTile is a copy of the real tile data, so we can just replace the
    // staging one with a temporary for this draw. The staging texture for the
    // real tile remains untouched, so we'll go back to using that for future
    // frames and discard the new one. In the future we could improve this by
    // having a pool of shared staging textures for all the tiles.

    // Mark the tile as having a temporary staging texture.
    mRenderCompositor->mCurrentStagingTextureIsTemp = true;

    // Try grabbing a texture from the staging pool and see if we can use that.
    if (uploadMode == Upload_StagingPooled && mOwner->mStagingPool.Length()) {
      mRenderCompositor->mCurrentStagingTexture =
          mOwner->mStagingPool.ElementAt(0);
      mOwner->mStagingPool.RemoveElementAt(0);
      hr = context->Map(mRenderCompositor->mCurrentStagingTexture, 0,
                        D3D11_MAP_READ_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT,
                        &mappedSubresource);

      // If that failed, put it back into the pool (but at the end).
      if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        mOwner->mStagingPool.AppendElement(
            mRenderCompositor->mCurrentStagingTexture);
      }
    }

    // No staging textures, or we tried one and it was busy. Allocate a brand
    // new one instead.
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      mRenderCompositor->mCurrentStagingTexture =
          mRenderCompositor->CreateStagingTexture(tileSize);
      if (!mRenderCompositor->mCurrentStagingTexture) {
        return false;
      }
      hr = context->Map(mRenderCompositor->mCurrentStagingTexture, 0,
                        D3D11_MAP_READ_WRITE, 0, &mappedSubresource);
    }
  }
  if (!SUCCEEDED(hr)) {
    gfxCriticalError() << "Failed to map tile: " << gfx::hexa(hr);
    // This is only expected to fail if we hit a device reset.
    MOZ_RELEASE_ASSERT(
        mRenderCompositor->GetDevice()->GetDeviceRemovedReason() != S_OK);
    return false;
  }

  // aData is expected to contain a pointer to the first pixel within the valid
  // rect, so take the mapped resource's data (which covers the full tile size)
  // and offset it by the top/left of the valid rect.
  *aData = (uint8_t*)mappedSubresource.pData +
           aValidRect.min.y * mappedSubresource.RowPitch + aValidRect.min.x * 4;
  *aStride = mappedSubresource.RowPitch;

  // Ensure our mapped data is accessible by writing to the beginning and end
  // of the dirty region. See bug 1717519
  if (aDirtyRect.width() > 0 && aDirtyRect.height() > 0) {
    uint32_t* probeData = (uint32_t*)mappedSubresource.pData +
                          aDirtyRect.min.y * (mappedSubresource.RowPitch / 4) +
                          aDirtyRect.min.x;
    *probeData = 0;
    uint32_t* probeDataEnd =
        (uint32_t*)mappedSubresource.pData +
        (aDirtyRect.max.y - 1) * (mappedSubresource.RowPitch / 4) +
        (aDirtyRect.max.x - 1);
    *probeDataEnd = 0;
  }

  // Store the new valid rect, so that we can composite only those pixels
  mValidRect = gfx::Rect(aValidRect.min.x, aValidRect.min.y, aValidRect.width(),
                         aValidRect.height());

  return true;
}

void RenderCompositorD3D11SWGL::TileD3D11::Unmap(
    const gfx::IntRect& aDirtyRect) {
  const UploadMode uploadMode = mRenderCompositor->GetUploadMode();

  if (!IsValid()) {
    return;
  }

  if (mRenderCompositor->mGpuShading) {
    // Already there: nothing to upload.
#if defined(GECKO_W10M)
    uint32_t n = mRenderCompositor->mTileProbes++;
    if ((n < 16 || n % 500 == 0) && !aDirtyRect.IsEmpty()) {
      gfx::IntPoint mid = aDirtyRect.Center();
      uint32_t c = 0;
      bool ok = mRenderCompositor->ProbePixel(mTexture->GetD3D11Texture(),
                                              mid.x, mid.y, &c);
      char note[256];
      SprintfLiteral(note,
                     "gfx: tile probe %u: valid %d,%d %dx%d dirty %d,%d %dx%d "
                     "centre %08x%s",
                     n, int(mValidRect.x), int(mValidRect.y),
                     int(mValidRect.width), int(mValidRect.height),
                     aDirtyRect.x, aDirtyRect.y, aDirtyRect.width,
                     aDirtyRect.height, c, ok ? "" : " (probe failed)");
      GeckoW10mNote(note);
    }
#endif
    return;
  }

  if (mSurface) {
    MOZ_ASSERT(uploadMode == Upload_Immediate);
    mSurface->Unmap();
    nsIntRegion dirty(aDirtyRect);
    // This uses UpdateSubresource, which blocks, so is likely implemented as a
    // memcpy into driver owned memory, followed by an async upload. The staging
    // method should avoid this extra copy, and is likely to be faster usually.
    // We could possible do this call on a background thread so that sw-wr can
    // start drawing the next tile while the memcpy is in progress.
    mTexture->Update(mSurface, &dirty);
    return;
  }

  if (!mRenderCompositor->mCurrentStagingTexture) {
    return;
  }

  RefPtr<ID3D11DeviceContext> context;
  mRenderCompositor->GetDevice()->GetImmediateContext(getter_AddRefs(context));

  context->Unmap(mRenderCompositor->mCurrentStagingTexture, 0);

  D3D11_BOX box;
  box.front = 0;
  box.back = 1;
  box.left = aDirtyRect.X();
  box.top = aDirtyRect.Y();
  box.right = aDirtyRect.XMost();
  box.bottom = aDirtyRect.YMost();

  context->CopySubresourceRegion(
      mTexture->GetD3D11Texture(), 0, aDirtyRect.x, aDirtyRect.y, 0,
      mRenderCompositor->mCurrentStagingTexture, 0, &box);

  // If we allocated a temp staging texture for this tile, and we're running
  // in pooled mode, then consider adding it to the pool for later.
  if (mRenderCompositor->mCurrentStagingTextureIsTemp &&
      uploadMode == Upload_StagingPooled) {
    static const uint32_t kMaxPoolSize = 5;
    if (mOwner->mStagingPool.Length() < kMaxPoolSize) {
      mOwner->mStagingPool.AppendElement(
          mRenderCompositor->mCurrentStagingTexture);
    }
  }

  mRenderCompositor->mCurrentStagingTexture = nullptr;
  mRenderCompositor->mCurrentStagingTextureIsTemp = false;
}

bool RenderCompositorD3D11SWGL::TileD3D11::IsValid() { return !!mTexture; }

already_AddRefed<ID3D11Texture2D>
RenderCompositorD3D11SWGL::CreateStagingTexture(const gfx::IntSize aSize) {
  CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_B8G8R8A8_UNORM, aSize.width,
                             aSize.height, 1, 1);

  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;

  RefPtr<ID3D11Texture2D> cpuTexture;
  DebugOnly<HRESULT> hr =
      GetDevice()->CreateTexture2D(&desc, nullptr, getter_AddRefs(cpuTexture));
  MOZ_ASSERT(SUCCEEDED(hr));
  if (!cpuTexture) {
    gfxCriticalNote << "Failed to create StagingTexture: " << aSize;
    RenderThread::Get()->HandleWebRenderError(WebRenderError::NEW_SURFACE);
  }
  return cpuTexture.forget();
}

already_AddRefed<gfx::DataSourceSurface>
RenderCompositorD3D11SWGL::CreateStagingSurface(const gfx::IntSize aSize) {
  return gfx::Factory::CreateDataSourceSurface(aSize,
                                               gfx::SurfaceFormat::B8G8R8A8);
}

UniquePtr<RenderCompositorLayersSWGL::Tile>
RenderCompositorD3D11SWGL::DoCreateTile(Surface* aSurface) {
  MOZ_RELEASE_ASSERT(aSurface);

  const auto tileSize = aSurface->TileSize();

  if (mUploadMode == Upload_Immediate) {
    RefPtr<DataTextureSourceD3D11> source =
        new DataTextureSourceD3D11(gfx::SurfaceFormat::B8G8R8A8, mCompositor,
                                   layers::TextureFlags::NO_FLAGS);
    RefPtr<gfx::DataSourceSurface> surf = CreateStagingSurface(tileSize);
    return MakeUnique<TileD3D11>(source, nullptr, surf, aSurface, this);
  }

  MOZ_ASSERT(mUploadMode == Upload_Staging ||
             mUploadMode == Upload_StagingNoBlock ||
             mUploadMode == Upload_StagingPooled);

  CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_B8G8R8A8_UNORM, tileSize.width,
                             tileSize.height, 1, 1);
  if (mGpuShading) {
    // SWGL renders into it.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  }

  RefPtr<ID3D11Texture2D> texture;
  DebugOnly<HRESULT> hr =
      GetDevice()->CreateTexture2D(&desc, nullptr, getter_AddRefs(texture));
  MOZ_ASSERT(SUCCEEDED(hr));
  if (!texture) {
    gfxCriticalNote << "Failed to allocate Texture2D: " << aSurface->TileSize();
    RenderThread::Get()->HandleWebRenderError(WebRenderError::NEW_SURFACE);
    return MakeUnique<TileD3D11>(nullptr, nullptr, nullptr, aSurface, this);
  }

  RefPtr<DataTextureSourceD3D11> source = new DataTextureSourceD3D11(
      GetDevice(), gfx::SurfaceFormat::B8G8R8A8, texture);

  // No staging texture when the GPU draws the tile: a megabyte a tile saved.
  RefPtr<ID3D11Texture2D> cpuTexture =
      mGpuShading ? nullptr : CreateStagingTexture(tileSize);
  return MakeUnique<TileD3D11>(source, cpuTexture, nullptr, aSurface, this);
}

bool RenderCompositorD3D11SWGL::MaybeReadback(
    const gfx::IntSize& aReadbackSize, const wr::ImageFormat& aReadbackFormat,
    const Range<uint8_t>& aReadbackBuffer, bool* aNeedsYFlip) {
  MOZ_ASSERT(aReadbackFormat == wr::ImageFormat::BGRA8);

  auto stride =
      aReadbackSize.width * gfx::BytesPerPixel(gfx::SurfaceFormat::B8G8R8A8);
  RefPtr<gfx::DrawTarget> dt = gfx::Factory::CreateDrawTargetForData(
      gfx::BackendType::SKIA, &aReadbackBuffer[0], aReadbackSize, stride,
      gfx::SurfaceFormat::B8G8R8A8, false);
  if (!dt) {
    return false;
  }

  GetCompositorD3D11()->Readback(dt);
  return true;
}

}  // namespace wr
}  // namespace mozilla
