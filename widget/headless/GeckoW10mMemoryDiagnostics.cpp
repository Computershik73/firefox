/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <algorithm>
#include <array>

#include "mozilla/Atomics.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Mutex.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Sprintf.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/TimeStamp.h"
#include "mozmemory.h"
#include "nsCOMPtr.h"
#include "nsIMemoryReporter.h"
#include "nsISupportsImpl.h"
#include "nsITimer.h"
#include "nsReadableUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"

extern "C" void GeckoW10mNote(const char*);
extern "C" void GeckoW10mGetWasmMemoryLedger(uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*);
extern "C" void GeckoW10mGetJitMemoryLedger(uint64_t*, uint64_t*, uint64_t*);
extern "C" void GeckoW10mGetSegmentedBufferBytes(uint64_t*, uint64_t*);
extern "C" uint64_t GeckoW10mGetPendingPipeBytes();

namespace {
constexpr uint64_t MiB = 1024 * 1024;
mozilla::StaticRefPtr<nsITimer> sTimer;
mozilla::Atomic<bool> sStopped{false};
mozilla::Atomic<bool> sReportPending{false};
mozilla::Atomic<uint32_t> sActiveReport{0};
mozilla::Atomic<bool> sCheckpointPending{false};
mozilla::TimeStamp sLastLine;
mozilla::TimeStamp sLastReport;
uint64_t sLastCommit = 0;
uint64_t sLastReportCommit = 0;
uint32_t sSequence = 0;
double sReportGapSeconds = 60;

struct ProcessSample {
  uint64_t commit = 0, resident = 0, available = 0, limit = 0;
  uint64_t appTotal = 0;
  uint64_t ownedStreams = 0, pendingStreams = 0;
  uint64_t pendingPipe = 0;
  DWORD processError = 0, appError = 0;
};

ProcessSample ReadProcess() {
  ProcessSample sample;
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  if (::GetProcessMemoryInfo(
          ::GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
          sizeof(memory))) {
    sample.commit = memory.PrivateUsage;
    sample.resident = memory.WorkingSetSize;
  } else {
    sample.processError = ::GetLastError();
  }
  using QueryFn =
      BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
  static QueryFn query = [] {
    HMODULE kernel = ::GetModuleHandleW(L"kernelbase.dll");
    auto fn = kernel ? reinterpret_cast<QueryFn>(
                           ::GetProcAddress(kernel, "GetProcessInformation"))
                     : nullptr;
    if (!fn) {
      kernel = ::GetModuleHandleW(L"kernel32.dll");
      fn = kernel ? reinterpret_cast<QueryFn>(
                        ::GetProcAddress(kernel, "GetProcessInformation"))
                  : nullptr;
    }
    return fn;
  }();
  APP_MEMORY_INFORMATION app{};
  if (query &&
      query(::GetCurrentProcess(), ProcessAppMemoryInfo, &app, sizeof(app))) {
    sample.available = app.AvailableCommit;
    sample.appTotal = app.TotalCommitUsage;
    sample.limit = app.TotalCommitUsage + app.AvailableCommit;
  } else {
    sample.appError = query ? ::GetLastError() : ERROR_PROC_NOT_FOUND;
  }
  GeckoW10mGetSegmentedBufferBytes(&sample.ownedStreams,
                                   &sample.pendingStreams);
  sample.pendingPipe = GeckoW10mGetPendingPipeBytes();
  return sample;
}

enum Bucket : size_t {
  Wasm,
  Buffers,
  Network,
  Graphics,
  JS,
  DOM,
  Other,
  Count
};
Bucket Classify(const nsACString& path) {
  if (path.Find("wasm") != kNotFound) return Wasm;
  if (path.Find("array-buffer") != kNotFound ||
      path.Find("arraybuffer") != kNotFound)
    return Buffers;
  if (StringBeginsWith(path, "explicit/network/"_ns) ||
      StringBeginsWith(path, "explicit/streams/"_ns))
    return Network;
  if (StringBeginsWith(path, "explicit/gfx/"_ns) ||
      StringBeginsWith(path, "explicit/images/"_ns) ||
      path.Find("webgl") != kNotFound)
    return Graphics;
  if (StringBeginsWith(path, "explicit/js-"_ns) ||
      path.Find("/js-") != kNotFound)
    return JS;
  if (StringBeginsWith(path, "explicit/window-objects/"_ns)) return DOM;
  return Other;
}

class Snapshot final : public nsIHandleReportCallback,
                       public nsIFinishReportingCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  Snapshot(uint32_t id, const ProcessSample& start)
      : mId(id),
        mStart(start),
        mWhen(mozilla::TimeStamp::Now()),
        mMutex("GeckoW10mMemorySnapshot") {}
  NS_IMETHOD Callback(const nsACString& process, const nsACString& path,
                      int32_t kind, int32_t units, int64_t amount,
                      const nsACString&, nsISupports*) override {
    if (!process.IsEmpty() || amount < 0 ||
        units != nsIMemoryReporter::UNITS_BYTES ||
        !StringBeginsWith(path, "explicit/"_ns) ||
        (kind != nsIMemoryReporter::KIND_HEAP &&
         kind != nsIMemoryReporter::KIND_NONHEAP))
      return NS_OK;
    mozilla::MutexAutoLock lock(mMutex);
    mBuckets[Classify(path)] += amount;
    if (kind == nsIMemoryReporter::KIND_HEAP) mHeapClassified += amount;
    return NS_OK;
  }
  NS_IMETHOD Callback(nsISupports*) override {
    MOZ_ASSERT(NS_IsMainThread());
    mozilla::MutexAutoLock lock(mMutex);
    if (sActiveReport == mId) {
      sReportPending = false;
      sActiveReport = 0;
    }
    if (sStopped) return NS_OK;
    const double duration =
        (mozilla::TimeStamp::Now() - mWhen).ToMilliseconds();
    sReportGapSeconds = duration > 250 ? 180 : 60;
    const auto end = ReadProcess();
    nsCOMPtr<nsIMemoryReporterManager> manager =
        do_GetService("@mozilla.org/memory-reporter-manager;1");
    int64_t heap = -1;
    if (manager && NS_FAILED(manager->GetHeapAllocated(&heap))) heap = -1;
    int64_t unclassified =
        heap < 0 ? -1 : std::max<int64_t>(0, heap - mHeapClassified);
    char line[768];
    SprintfLiteral(
        line,
        "memdiag: categories id=%u duration_ms=%.0f commit_before_mb=%.1f "
        "commit_after_mb=%.1f "
        "wasm_mb=%.1f array_buffers_mb=%.1f network_mb=%.1f graphics_mb=%.1f "
        "js_mb=%.1f dom_mb=%.1f other_explicit_mb=%.1f "
        "heap_unclassified_mb=%.1f "
        "streams_owned_mb=%.1f streams_pending_free_mb=%.1f "
        "pipe_pending_free_mb=%.1f",
        mId, duration, double(mStart.commit) / MiB, double(end.commit) / MiB,
        double(mBuckets[Wasm]) / MiB, double(mBuckets[Buffers]) / MiB,
        double(mBuckets[Network]) / MiB, double(mBuckets[Graphics]) / MiB,
        double(mBuckets[JS]) / MiB, double(mBuckets[DOM]) / MiB,
        double(mBuckets[Other]) / MiB,
        unclassified < 0 ? -1 : double(unclassified) / MiB,
        double(end.ownedStreams) / MiB, double(end.pendingStreams) / MiB,
        double(end.pendingPipe) / MiB);
    GeckoW10mNote(line);
    return NS_OK;
  }

 private:
  ~Snapshot() = default;
  uint32_t mId;
  ProcessSample mStart;
  mozilla::TimeStamp mWhen;
  mozilla::Mutex mMutex;
  std::array<int64_t, Count> mBuckets{};
  int64_t mHeapClassified = 0;
};
NS_IMPL_ISUPPORTS(Snapshot, nsIHandleReportCallback, nsIFinishReportingCallback)

mozilla::Atomic<uint64_t> sHeapAllocated{0}, sHeapRetained{0}, sHeapSampleTick{0};

void LogLedger(const char* reason, uint64_t requested, uint32_t error) {
  uint64_t reserved, committed, live, allocated, freed, jit, jitAlloc, jitFree;
  GeckoW10mGetWasmMemoryLedger(&reserved, &committed, &live, &allocated, &freed);
  GeckoW10mGetJitMemoryLedger(&jit, &jitAlloc, &jitFree);
  const auto process = ReadProcess();
  const uint64_t heapTick = sHeapSampleTick;
  char line[1024];
  SprintfLiteral(line,
      "memledger: reason=%s request_bytes=%llu error=%u "
      "wasm_reserved=%llu wasm_committed=%llu wasm_live=%llu "
      "wasm_alloc_total=%llu wasm_free_total=%llu jit_committed=%llu "
      "jit_alloc_total=%llu jit_free_total=%llu heap_allocated_cached=%llu "
      "heap_retained_cached=%llu heap_sample_age_ms=%llu process_commit=%llu "
      "app_total=%llu app_available=%llu process_error=%lu app_error=%lu",
      reason, (unsigned long long)requested, error,
      (unsigned long long)reserved, (unsigned long long)committed,
      (unsigned long long)live, (unsigned long long)allocated,
      (unsigned long long)freed, (unsigned long long)jit,
      (unsigned long long)jitAlloc, (unsigned long long)jitFree,
      (unsigned long long)uint64_t(sHeapAllocated),
      (unsigned long long)uint64_t(sHeapRetained),
      (unsigned long long)(heapTick ? GetTickCount64() - heapTick : UINT64_MAX),
      (unsigned long long)process.commit, (unsigned long long)process.appTotal,
      (unsigned long long)process.available, process.processError, process.appError);
  GeckoW10mNote(line);
}

void Sample(bool checkpoint) {
  MOZ_ASSERT(NS_IsMainThread());
  if (sStopped) return;
  auto now = mozilla::TimeStamp::Now();
  auto process = ReadProcess();
  if (sReportPending && !sLastReport.IsNull() &&
      (now - sLastReport).ToSeconds() > 45) {
    sReportPending = false;
    sActiveReport = 0;
    GeckoW10mNote(
        "memdiag: categories timed out or skipped by an existing report; "
        "lightweight counters continue");
  }
  bool growth =
      process.commit > sLastCommit && process.commit - sLastCommit >= 128 * MiB;
  if (!checkpoint && !growth && !sLastLine.IsNull() &&
      (now - sLastLine).ToSeconds() < 10)
    return;
  nsCOMPtr<nsIMemoryReporterManager> manager =
      do_GetService("@mozilla.org/memory-reporter-manager;1");
  int64_t heap = -1, overhead = -1, gc = -1, temporary = -1, ghosts = -1;
  if (manager) {
    if (NS_FAILED(manager->GetHeapAllocated(&heap))) heap = -1;
    if (NS_FAILED(manager->GetHeapOverheadFraction(&overhead))) overhead = -1;
    if (NS_FAILED(manager->GetJSMainRuntimeGCHeap(&gc))) gc = -1;
    if (NS_FAILED(manager->GetJSMainRuntimeTemporaryPeak(&temporary)))
      temporary = -1;
    if (NS_FAILED(manager->GetGhostWindows(&ghosts))) ghosts = -1;
  }
#ifdef MOZ_MEMORY
  jemalloc_stats_t allocator{};
  jemalloc_stats(&allocator);
  sHeapAllocated = allocator.allocated;
  sHeapRetained = allocator.waste + allocator.pages_dirty + allocator.bin_unused;
  sHeapSampleTick = GetTickCount64();
#endif
  LogLedger(checkpoint ? "checkpoint" : "sample", 0, 0);
  char line[1024];
  SprintfLiteral(
      line,
      "memdiag: sample commit_mb=%.1f delta_mb=%.1f resident_mb=%.1f "
      "app_total_mb=%.1f app_available_mb=%.1f app_limit_mb=%.1f "
      "heap_mb=%.1f heap_overhead_pct=%.2f js_gc_mb=%.1f "
      "js_temp_peak_mb=%.1f ghost_windows=%lld streams_owned_mb=%.1f "
      "streams_pending_free_mb=%.1f pipe_pending_free_mb=%.1f "
      "process_error=%lu app_error=%lu checkpoint=%d",
      double(process.commit) / MiB,
      sLastCommit ? (double(process.commit) - double(sLastCommit)) / MiB : 0,
      double(process.resident) / MiB, double(process.appTotal) / MiB,
      double(process.available) / MiB, double(process.limit) / MiB,
      heap < 0 ? -1 : double(heap) / MiB,
      overhead < 0 ? -1 : double(overhead) / 100,
      gc < 0 ? -1 : double(gc) / MiB,
      temporary < 0 ? -1 : double(temporary) / MiB,
      static_cast<long long>(ghosts), double(process.ownedStreams) / MiB,
      double(process.pendingStreams) / MiB, double(process.pendingPipe) / MiB,
      process.processError, process.appError, checkpoint);
  GeckoW10mNote(line);
  sLastLine = now;
  sLastCommit = process.commit;
  const bool rapidGrowth = process.commit > sLastReportCommit &&
                           process.commit - sLastReportCommit >= 256 * MiB;
  const double gap =
      rapidGrowth && sReportGapSeconds <= 60 ? 15 : sReportGapSeconds;
  const bool reportDue =
      sLastReport.IsNull() || (now - sLastReport).ToSeconds() >= gap;
  const bool changed = sLastReport.IsNull() ||
                       process.commit < sLastReportCommit ||
                       process.commit - sLastReportCommit >= 128 * MiB;
  const bool headroom = process.limit
                            ? process.available >= 512 * MiB &&
                                  process.available >= process.limit / 5
                            : process.commit < 3 * 1024 * MiB;
  if (!manager || !process.commit || !headroom || !reportDue || !changed ||
      sReportPending)
    return;
  sLastReport = now;
  sLastReportCommit = process.commit;
  sReportPending = true;
  uint32_t id = ++sSequence;
  sActiveReport = id;
  RefPtr<Snapshot> report = new Snapshot(id, process);
  SprintfLiteral(line,
                 "memdiag: categories begin id=%u anonymized=1 minimize=0", id);
  GeckoW10mNote(line);
  nsresult rv = manager->GetReportsForThisProcessExtended(
      report, nullptr, true, nullptr, report, nullptr);
  if (NS_FAILED(rv)) {
    sReportPending = false;
    sActiveReport = 0;
    SprintfLiteral(line, "memdiag: categories failed id=%u rv=0x%08x", id,
                   static_cast<unsigned>(rv));
    GeckoW10mNote(line);
  }
}

void Tick(nsITimer*, void*) { Sample(false); }
}  // namespace

extern "C" void GeckoW10mAllocationFailure(const char* stage,
    uint64_t requested, uint32_t error) {
  // Safe from allocation threads: allocator statistics are cached by Sample.
  const DWORD savedError = GetLastError();
  LogLedger(stage, requested, error);
  SetLastError(savedError);
}

extern "C" void GeckoW10mMemoryDiagnosticsStart() {
  MOZ_ASSERT(NS_IsMainThread());
  if (sTimer || sStopped) return;
  sTimer = NS_NewTimer();
  if (!sTimer) return;
  nsresult rv = sTimer->InitWithNamedFuncCallback(
      Tick, nullptr, 2000, nsITimer::TYPE_REPEATING_SLACK,
      "GeckoW10mMemoryDiagnostics"_ns);
  if (NS_FAILED(rv)) {
    sTimer = nullptr;
    return;
  }
  GeckoW10mNote(
      "memdiag: v1 enabled; sampled counters, anonymized categories, no URLs "
      "or input contents");
  mozilla::RunOnShutdown(
      [] {
        sStopped = true;
        if (sTimer) {
          sTimer->Cancel();
          sTimer = nullptr;
        }
      },
      mozilla::ShutdownPhase::XPCOMShutdown);
}

extern "C" void GeckoW10mMemoryDiagnosticsCheckpoint() {
  if (sStopped || sCheckpointPending.exchange(true)) return;
  nsresult rv = NS_DispatchToMainThread(
      NS_NewRunnableFunction("GeckoW10mMemoryDiagnostics::Checkpoint", [] {
        sCheckpointPending = false;
        Sample(true);
      }));
  if (NS_FAILED(rv)) sCheckpointPending = false;
}
