/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Memory pressure on Windows 10 Mobile, in one place.
//
// Two things say memory is short. The phone does, per app, against a ceiling
// it sets from the device's class -- 390 MB on a 1 GB Lumia -- and past the
// ceiling the app is simply ended; the shell hears it from MemoryManager and
// passes the level on (gecko_w10m_memory_pressure). And the process's own
// address space does, which nothing on Windows reports at all: a 32-bit
// process has 2 GB of it, the browser takes over a gigabyte before any page,
// and Discord on the HP Elite x3 left 300-400 MB free with nothing over 17 MB
// in one piece, so allocations failed with the phone nowhere near its
// ceiling. The watcher at the bottom of this file walks the address space
// every few seconds for that.
//
// Upstream's machinery was built for one notification: "low-memory" once,
// then "low-memory-ongoing" for as long as the pressure lasts, until
// something says it is over. Nothing here ever said it was over, so every
// event after the first of a run came out as "ongoing" -- and the JS engine
// does not collect on "ongoing" at all (nsJSEnvironment.cpp). A Lumia sat two
// to five times over its ceiling for minutes, told "ongoing" again and again,
// without one more shrinking GC, until the system ended it.
//
// So the engine is now told when the pressure ends, and while the pressure is
// serious it is told afresh every so often (memory.gecko-w10m.pressure-rearm-ms):
// "memory-pressure-stop" and then a new "low-memory", which collects again.
// Not more often than that, and not for Medium (RearmWanted): a shrinking GC
// and a cycle collection on a Cortex-A7 take a long time, and the cure must
// not become the illness.

#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "mozilla/Atomics.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Sprintf.h"
#include "mozilla/StaticPrefs_memory.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/TimeStamp.h"
#include "nsCOMPtr.h"
#include "nsITimer.h"
#include "nsMemoryPressure.h"
#include "nsThreadUtils.h"

extern "C" void GeckoW10mNote(const char* text);

// Delivers what NS_NotifyOfEventualMemoryPressure left pending, there and
// then. nsThread calls it before every event on the main thread and declares
// it the same way; it has no header.
void NS_DispatchMemoryPressure();

namespace {

using mozilla::Atomic;
using mozilla::StaticRefPtr;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

// ------------------------------------------------------------ the inputs
//
// These arrive from anywhere: the shell's thread, the watcher's task queue,
// whichever thread a JS allocation failed on.

// The shell's level: 0 Low, 1 Medium, 2 High, 3 over the limit.
Atomic<int32_t> sShellLevel{0};
// The largest free piece of the address space is below the threshold. Only
// the watcher writes it.
Atomic<bool> sAddressLow{false};
// A JS allocation failed since the last evaluation, and what the caller said
// it was (a string literal).
Atomic<bool> sFailed{false};
Atomic<const char*> sFailedWhy{nullptr};
// The shell said Medium or higher since the last evaluation.
Atomic<bool> sShellAsked{false};
// An evaluation is already on its way to the main thread; the next input
// rides along with it rather than sending another.
Atomic<bool> sEvaluatePending{false};
// Set just before the main loop starts (GeckoW10mMemoryStart). Until then
// there may be no main thread to evaluate on, and the shell's word goes to
// the engine the way it always did.
Atomic<bool> sStarted{false};
// Low memory went to the engine the old way before the start.
Atomic<bool> sSentEarly{false};
Atomic<bool> sShutdown{false};
// How many fresh "low-memory" notifications have been delivered, each with
// the collection it runs. The watcher compares it to say what one achieved.
Atomic<uint32_t> sLetGo{0};

// ------------------------------------------- what the engine was told
//
// Main thread only.

// The engine is under pressure as far as it knows.
bool sTold = false;
TimeStamp sToldSince;
// When the collection of the last fresh "low-memory" was over: the gaps below
// are counted from there, so that a collection which took seconds does not
// eat into the time the page gets before the next.
TimeStamp sLastFresh;
// A failed allocation holds the pressure on by itself until then.
TimeStamp sFailureUntil;
StaticRefPtr<nsITimer> sRecheckTimer;
uint32_t sPressureNotes = 0;

// How long a failed JS allocation holds the pressure on by itself: long
// enough that the engine stays in its low-memory state for what comes right
// after, short enough that one failure does not cost a collection every half
// minute for the rest of the run.
constexpr uint32_t kFailureHoldMs = 30000;
// A failed allocation within this long of the last fresh "low-memory" asks
// for nothing more: that one's collection has only just run.
constexpr uint32_t kFailureGapMs = 5000;
// Pressure that comes back within this long of the last fresh "low-memory"
// waits out the rest before it is turned on again. Otherwise the stop and the
// start would follow each other as fast as the inputs flip -- a largest free
// piece that a collection lifts over one and a half times the threshold and
// the next page pushes under it again, a phone that falls to Low and climbs
// straight back to High -- and every start is another shrinking GC and cycle
// collection.
constexpr uint32_t kTurnOnGapMs = 10000;
// A timer set for the moment a re-arm falls due, or a failure's hold runs
// out, may fire a little early; within this much, the moment has come.
constexpr double kTimerSlackMs = 100;
// Forty a run: on, a re-arm each half minute, off.
constexpr uint32_t kPressureNoteCap = 40;

// --------------------------------------------- the address-space watcher

StaticRefPtr<nsISerialEventTarget> sWatchQueue;
// Touched on the watcher's queue only, once it has been started.
StaticRefPtr<nsITimer> sWatchTimer;

// Every five seconds, and every second while the largest free piece is
// within twice the threshold: from there a page can take the rest in the
// time between two slow walks.
constexpr uint32_t kWatchSlowMs = 5000;
constexpr uint32_t kWatchFastMs = 1000;
constexpr uint32_t kWatchNoteCap = 20;

void Evaluate();

void PressureNote(const char* aText) {
  if (sPressureNotes < kPressureNoteCap) {
    ++sPressureNotes;
    GeckoW10mNote(aText);
  }
}

// Why the engine is under pressure, for the notes.
void DescribeReasons(char* aOut, size_t aLength, int32_t aLevel,
                     bool aAddressLow, bool aFailure) {
  size_t used = 0;
  aOut[0] = 0;
  auto add = [&](const char* aText) {
    if (used + 1 < aLength) {
      const int wrote = snprintf(aOut + used, aLength - used, "%s%s",
                                 used ? ", " : "", aText);
      if (wrote > 0) {
        used = std::min(aLength - 1, used + size_t(wrote));
      }
    }
  };
  if (aLevel >= 1) {
    char level[48];
    SprintfLiteral(level, "the phone says level %d", int(aLevel));
    add(level);
  }
  if (aAddressLow) {
    add("the address space is low");
  }
  if (aFailure) {
    const char* why = sFailedWhy;
    add(why ? why : "an allocation failed");
  }
  if (!used) {
    add("left over from before the start");
  }
}

// "memory-pressure-stop" and then a fresh "low-memory", each delivered here
// and now rather than left pending. nsMemoryPressure keeps one pending state
// only, so a NoPressure followed by a LowMemory, both left pending, would
// collapse into the LowMemory alone -- and that comes out as "ongoing" again.
// Whatever another thread left pending goes first, so that it cannot land in
// between. In a single process the JS engine's answer to "low-memory" -- a
// shrinking GC, a cycle collection, another GC if that freed anything -- runs
// inside the notification (CCGCScheduler::MayGCNow resolves at once), so the
// time this takes is the time the engine took to let go.
double SendFresh() {
  const TimeStamp start = TimeStamp::Now();
  NS_DispatchMemoryPressure();
  NS_NotifyOfEventualMemoryPressure(MemoryPressureState::NoPressure);
  NS_DispatchMemoryPressure();
  NS_NotifyOfEventualMemoryPressure(MemoryPressureState::LowMemory);
  NS_DispatchMemoryPressure();
  ++sLetGo;
  sLastFresh = TimeStamp::Now();
  return (sLastFresh - start).ToMilliseconds();
}

void SendStop() {
  NS_DispatchMemoryPressure();
  NS_NotifyOfEventualMemoryPressure(MemoryPressureState::NoPressure);
  NS_DispatchMemoryPressure();
}

void Recheck(nsITimer*, void*) { Evaluate(); }

// Re-arms -- a fresh low-memory every pressure-rearm-ms -- only for pressure
// that is serious: the phone at High or over its limit, the address space
// low, or an allocation that failed. Medium is where a 1 GB Lumia spends most
// of a session (about half of its 390 MB), and a shrinking GC with a cycle
// collection every 30 s there would be a pause on a Cortex-A7 for nothing
// urgent: Medium gets its fresh collection when the pressure comes on, and
// the cheap "ongoing" purges after that.
bool RearmWanted(int32_t aLevel, bool aAddressLow, bool aFailureHolds) {
  return aLevel >= 2 || aAddressLow || aFailureHolds;
}

// While the engine is under pressure something has to come back to it. The
// shell cannot be relied on for that: once the phone is over its limit there
// is no higher level to rise to and it says nothing more -- which is exactly
// when a fresh collection is wanted most. So a one-shot timer on the main
// thread, set for when the next re-arm falls due or a failed allocation's
// hold runs out -- or, for pressure held back by kTurnOnGapMs, for when it
// may be turned on -- and for nothing at all while there is no pressure.
void RecheckLater(const TimeStamp& aNow, bool aWanted, bool aRearm) {
  TimeStamp next;
  if (sTold && !sShutdown) {
    const uint32_t rearmMs =
        mozilla::StaticPrefs::memory_gecko_w10m_pressure_rearm_ms();
    if (aRearm && rearmMs && !sLastFresh.IsNull()) {
      next = sLastFresh + TimeDuration::FromMilliseconds(rearmMs);
    }
    if (!sFailureUntil.IsNull() && sFailureUntil > aNow &&
        (next.IsNull() || sFailureUntil < next)) {
      next = sFailureUntil;
    }
  } else if (aWanted && !sShutdown && !sLastFresh.IsNull()) {
    next = sLastFresh + TimeDuration::FromMilliseconds(kTurnOnGapMs);
  }
  if (next.IsNull()) {
    if (sRecheckTimer) {
      sRecheckTimer->Cancel();
    }
    return;
  }
  const double delay = next > aNow ? (next - aNow).ToMilliseconds() : 0;
  const uint32_t delayMs = delay < 1000 ? 1000 : uint32_t(delay) + 1;
  if (!sRecheckTimer) {
    sRecheckTimer = NS_NewTimer();
    if (!sRecheckTimer) {
      return;
    }
  }
  sRecheckTimer->InitWithNamedFuncCallback(
      Recheck, nullptr, delayMs, nsITimer::TYPE_ONE_SHOT,
      "GeckoW10mMemoryPressure::Recheck"_ns);
}

// The one place that decides what the engine is told. Main thread.
void Evaluate() {
  MOZ_ASSERT(NS_IsMainThread());
  // Cleared first: an input that arrives while this runs sends another.
  sEvaluatePending = false;
  if (sShutdown) {
    return;
  }
  const TimeStamp now = TimeStamp::Now();
  const bool shellAsked = sShellAsked.exchange(false);
  const bool failed = sFailed.exchange(false);
  if (failed) {
    sFailureUntil = now + TimeDuration::FromMilliseconds(kFailureHoldMs);
  }
  const int32_t level = sShellLevel;
  const bool addressLow = sAddressLow;
  const TimeDuration slack = TimeDuration::FromMilliseconds(kTimerSlackMs);
  const bool failureHolds =
      !sFailureUntil.IsNull() && now + slack < sFailureUntil;
  const bool wanted = level >= 1 || addressLow || failureHolds;
  const bool rearm = RearmWanted(level, addressLow, failureHolds);

  const uint32_t rearmMs =
      mozilla::StaticPrefs::memory_gecko_w10m_pressure_rearm_ms();
  const double sinceFresh =
      sLastFresh.IsNull() ? -1 : (now - sLastFresh).ToMilliseconds();
  // No fresh one yet under pressure the engine already holds -- sent the old
  // way before the start -- counts as due.
  const bool rearmDue =
      sinceFresh < 0 || (rearmMs && sinceFresh + kTimerSlackMs >= rearmMs);
  const bool failureDue =
      failed && (sinceFresh < 0 || sinceFresh >= kFailureGapMs);
  const bool turnOnHeld =
      sinceFresh >= 0 && sinceFresh + kTimerSlackMs < kTurnOnGapMs;

  char reasons[128];
  char note[256];
  if (wanted && !sTold) {
    if (turnOnHeld) {
      // Back moments after the last collection; the timer below brings this
      // round again when the gap is over, and it is turned on then if it is
      // still wanted. Nothing in between: with the pressure off, even the
      // cheap "low-memory" would come out as a fresh one.
      RecheckLater(now, wanted, rearm);
      return;
    }
    sTold = true;
    sToldSince = now;
    const double ms = SendFresh();
    DescribeReasons(reasons, sizeof(reasons), level, addressLow, failureHolds);
    SprintfLiteral(note,
                   "mem: pressure on -- %s; low-memory sent, the engine let "
                   "go in %.0f ms",
                   reasons, ms);
    PressureNote(note);
  } else if (!wanted && sTold) {
    sTold = false;
    SendStop();
    SprintfLiteral(note,
                   "mem: pressure over after %.0f s -- memory-pressure-stop "
                   "sent",
                   sToldSince.IsNull() ? 0.0 : (now - sToldSince).ToSeconds());
    PressureNote(note);
  } else if (wanted && ((rearm && rearmDue) || failureDue)) {
    const double ms = SendFresh();
    DescribeReasons(reasons, sizeof(reasons), level, addressLow, failureHolds);
    SprintfLiteral(note,
                   "mem: still under pressure %.0f s on -- %s; "
                   "memory-pressure-stop and a fresh low-memory sent, the "
                   "engine let go in %.0f ms",
                   sToldSince.IsNull() ? 0.0 : (now - sToldSince).ToSeconds(),
                   reasons, ms);
    PressureNote(note);
  } else if (wanted && shellAsked) {
    // The phone said so again before a re-arm was due: "low-memory-ongoing",
    // as every call from the shell used to be. The JS engine leaves it alone;
    // the caches drop what they hold again, which is cheap.
    NS_NotifyOfEventualMemoryPressure(MemoryPressureState::LowMemory);
  }
  RecheckLater(now, wanted, rearm);
}

void RequestEvaluate() {
  if (sShutdown || !sStarted) {
    return;
  }
  if (sEvaluatePending.exchange(true)) {
    return;
  }
  // Ahead of the page's own work: the old LowMemory was delivered before the
  // main thread's next event, and this should not wait much longer than that.
  nsresult rv = NS_DispatchToMainThreadQueue(
      NS_NewRunnableFunction("GeckoW10mMemoryPressure::Evaluate",
                             []() { Evaluate(); }),
      mozilla::EventQueuePriority::MediumHigh);
  if (NS_FAILED(rv)) {
    sEvaluatePending = false;
  }
}

// ------------------------------------------- the address-space watcher

struct AddressSpace {
  size_t freeBytes = 0;
  size_t largest = 0;
  size_t pieces = 0;
  size_t regions = 0;
};

// One pass over the whole address space, counting the free pieces. Nothing
// allocated and no lock but whatever VirtualQuery takes inside the kernel: a
// system call a region, a couple of thousand regions on a phone that has been
// browsing for a while.
AddressSpace WalkAddressSpace() {
  AddressSpace space;
  MEMORY_BASIC_INFORMATION info;
  uintptr_t at = 0x10000;
  while (::VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) ==
         sizeof(info)) {
    ++space.regions;
    if (info.State == MEM_FREE) {
      space.freeBytes += info.RegionSize;
      ++space.pieces;
      if (info.RegionSize > space.largest) {
        space.largest = info.RegionSize;
      }
    }
    const uintptr_t next = uintptr_t(info.BaseAddress) + info.RegionSize;
    if (next <= at) {
      break;
    }
    at = next;
  }
  return space;
}

// The watcher's own state; its task queue runs one tick at a time.
bool sLow = false;
uint32_t sLetGoNoted = 0;
uint32_t sWatchNotes = 0;
bool sTimed = false;

void WatchNote(const char* aText) {
  if (sWatchNotes < kWatchNoteCap) {
    ++sWatchNotes;
    GeckoW10mNote(aText);
  }
}

void WatchTick(nsITimer*, void*) {
  if (sShutdown) {
    return;
  }
  // Up to a gigabyte: the space is two, and twice the threshold has to fit.
  const uint32_t lowMb = std::min<uint32_t>(
      mozilla::StaticPrefs::memory_gecko_w10m_address_space_low_mb(), 1024);
  if (!lowMb) {
    // Turned off while running: the address space stops asking, and the
    // watcher stops walking.
    if (sLow) {
      sLow = false;
      sAddressLow = false;
      RequestEvaluate();
    }
    GeckoW10mNote(
        "vm watch: off -- memory.gecko-w10m.address-space.low-mb is 0");
    return;
  }
  // Read before the walk, so that a collection which finishes during it is
  // not credited with what the walk saw before it.
  const uint32_t letGo = sLetGo;

  static LARGE_INTEGER sFrequency{};
  if (!sFrequency.QuadPart) {
    ::QueryPerformanceFrequency(&sFrequency);
  }
  LARGE_INTEGER began, ended;
  ::QueryPerformanceCounter(&began);
  const AddressSpace space = WalkAddressSpace();
  ::QueryPerformanceCounter(&ended);

  constexpr size_t kMB = 1024 * 1024;
  const uint64_t threshold = uint64_t(lowMb) * kMB;
  const uint64_t largest = space.largest;
  char note[256];
  if (!sTimed) {
    sTimed = true;
    const uint64_t us =
        sFrequency.QuadPart
            ? uint64_t(ended.QuadPart - began.QuadPart) * 1000000 /
                  uint64_t(sFrequency.QuadPart)
            : 0;
    SprintfLiteral(note,
                   "vm watch: a walk takes %u us (%zu regions), every %u s; "
                   "every %u s while the largest free piece is under %u MB",
                   unsigned(us), space.regions, kWatchSlowMs / 1000,
                   kWatchFastMs / 1000, lowMb * 2);
    GeckoW10mNote(note);
  }

  if (!sLow && largest < threshold) {
    sLow = true;
    sLetGoNoted = letGo;
    sAddressLow = true;
    SprintfLiteral(note,
                   "vm watch: address space low -- largest free piece %zu MB, "
                   "%zu MB free in %zu pieces; asking the engine to let go",
                   space.largest / kMB, space.freeBytes / kMB, space.pieces);
    WatchNote(note);
    RequestEvaluate();
  } else if (sLow) {
    // Back only well clear of the threshold, so that a piece hovering at it
    // does not turn the pressure on and off at every walk.
    const bool back = largest > threshold * 3 / 2;
    const bool letGoSince = letGo != sLetGoNoted;
    if (letGoSince || back) {
      SprintfLiteral(note,
                     "vm watch: %s -- largest free piece %zu MB, %zu MB free "
                     "in %zu pieces; %s %u MB",
                     letGoSince ? "after letting go" : "address space back",
                     space.largest / kMB, space.freeBytes / kMB, space.pieces,
                     back ? "back above" : "not yet back above",
                     unsigned(lowMb * 3 / 2));
      WatchNote(note);
      sLetGoNoted = letGo;
    }
    if (back) {
      sLow = false;
      sAddressLow = false;
      RequestEvaluate();
    }
  }

  if (sWatchTimer && !sShutdown) {
    sWatchTimer->InitWithNamedFuncCallback(
        WatchTick, nullptr,
        largest < threshold * 2 ? kWatchFastMs : kWatchSlowMs,
        nsITimer::TYPE_ONE_SHOT, "GeckoW10mAddressSpaceWatch"_ns);
  }
}

// Nothing is told anything once XPCOM is going down, and no walk is left to
// fire into the middle of it. The watcher's timer is cancelled on its own
// queue, so that a tick already running cannot set it again afterwards.
void EnsureShutdownHook() {
  MOZ_ASSERT(NS_IsMainThread());
  static bool sHooked = false;
  if (sHooked) {
    return;
  }
  sHooked = true;
  mozilla::RunOnShutdown(
      [] {
        sShutdown = true;
        if (sRecheckTimer) {
          sRecheckTimer->Cancel();
          sRecheckTimer = nullptr;
        }
        if (sWatchQueue) {
          // Fallible: a queue already gone has no tick left to stop, and the
          // ticks look at sShutdown first anyway.
          sWatchQueue->Dispatch(NS_NewRunnableFunction(
                                    "GeckoW10mAddressSpaceWatch::Stop",
                                    [] {
                                      if (sWatchTimer) {
                                        sWatchTimer->Cancel();
                                        sWatchTimer = nullptr;
                                      }
                                    }),
                                NS_DISPATCH_FALLIBLE);
          sWatchQueue = nullptr;
        }
      },
      mozilla::ShutdownPhase::XPCOMShutdown);
}

}  // namespace

// From nsAppRunner, on the main thread, just before the main loop: XPCOM is
// up, the profile's prefs are read and the first window is open.
extern "C" void GeckoW10mMemoryStart() {
  MOZ_ASSERT(NS_IsMainThread());
  if (sStarted || sShutdown) {
    return;
  }
  EnsureShutdownHook();
  // Started first, so that a word from the shell from here on takes the new
  // way; the evaluation it sends cannot run before this function returns.
  sStarted = true;
  if (sSentEarly) {
    // The engine is already in its low-memory state; the first evaluation
    // re-arms it under the new rules, or ends it.
    sTold = true;
    sToldSince = TimeStamp::Now();
  }
  // Whatever the shell said before now is acted on.
  RequestEvaluate();

  if (!mozilla::StaticPrefs::memory_gecko_w10m_address_space_low_mb()) {
    GeckoW10mNote(
        "vm watch: off -- memory.gecko-w10m.address-space.low-mb is 0");
    return;
  }
  // A task queue on the background pool, not a thread of its own and not the
  // main thread: a walk is a few milliseconds of system calls every few
  // seconds, and nothing it does needs anything the main thread has.
  nsCOMPtr<nsISerialEventTarget> queue;
  if (NS_FAILED(NS_CreateBackgroundTaskQueue("GeckoW10mAddressSpaceWatch",
                                             getter_AddRefs(queue)))) {
    GeckoW10mNote(
        "vm watch: no task queue -- the address space is not watched");
    return;
  }
  nsCOMPtr<nsITimer> timer = NS_NewTimer(queue);
  if (!timer) {
    GeckoW10mNote("vm watch: no timer -- the address space is not watched");
    return;
  }
  sWatchQueue = queue;
  sWatchTimer = timer;
  if (NS_FAILED(timer->InitWithNamedFuncCallback(
          WatchTick, nullptr, kWatchSlowMs, nsITimer::TYPE_ONE_SHOT,
          "GeckoW10mAddressSpaceWatch"_ns))) {
    GeckoW10mNote("vm watch: the timer would not start -- not watching");
  }
}

// From the shell, which hears the phone's MemoryManager: 0 Low, 1 Medium,
// 2 High, 3 over the limit. The shell sends a rising level (rate-limited:
// Medium once in thirty seconds, High once in five, over the limit always,
// and the first rise after a Low at once) and 0 when usage is back down to
// Low; the engine stays under pressure from the phone from the first until
// the second.
extern "C" __declspec(dllexport) void gecko_w10m_memory_pressure(
    int32_t aLevel) {
  const int32_t level = aLevel < 0 ? 0 : aLevel > 3 ? 3 : aLevel;
  const int32_t was = sShellLevel.exchange(level);
  if (level >= 1) {
    GeckoW10mNote(level >= 2 ? "mem: the phone says memory is CRITICAL"
                             : "mem: the phone says memory is tight");
  } else if (was >= 1) {
    static Atomic<uint32_t> sBackNotes{0};
    if (sBackNotes++ < kPressureNoteCap) {
      GeckoW10mNote("mem: the phone says memory is back to low");
    }
  } else {
    return;  // Low again, and Low already: nothing changed
  }
  if (sShutdown) {
    return;
  }
  if (!sStarted) {
    // Before the main loop there may be no main thread to decide anything on.
    // Low memory goes in the way it always did, and the start takes it from
    // there.
    if (level >= 1) {
      sSentEarly = true;
      NS_NotifyOfEventualMemoryPressure(MemoryPressureState::LowMemory);
    }
    return;
  }
  if (level >= 1) {
    sShellAsked = true;
  }
  RequestEvaluate();
}

// From the JS engine, at the first allocation of the run that failed even
// after the empty GC chunks were handed back (js/src/vm/Runtime.cpp). A
// fresh "low-memory" -- unless one went out moments ago -- and the pressure
// held on for a while after it. From any thread; nothing here allocates
// beyond the one runnable that carries it to the main thread.
extern "C" void GeckoW10mMemoryPressureOnFailure(const char* aWhy) {
  if (sShutdown) {
    return;
  }
  sFailedWhy = aWhy;
  sFailed = true;
  if (!sStarted) {
    // The old way until the start, which then holds the pressure for the
    // failure as it would have from here on.
    sSentEarly = true;
    NS_NotifyOfEventualMemoryPressure(MemoryPressureState::LowMemory);
    return;
  }
  RequestEvaluate();
}
