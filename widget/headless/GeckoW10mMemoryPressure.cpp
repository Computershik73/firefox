/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Memory pressure on Windows 10 Mobile, in one place.
//
// Two things say memory is short. The phone does, per app, against a ceiling
// it sets from the device's class -- 390 MB on a 1 GB Lumia -- and past the
// ceiling the app may be ended; the shell hears it from MemoryManager and
// passes the level on (gecko_w10m_memory_pressure). And the process's own
// address space does, which nothing on Windows reports at all: a 32-bit
// process has 2 GB of it, the browser takes over a gigabyte before any page,
// and Discord on the HP Elite x3 left 300-400 MB free with nothing over 17 MB
// in one piece, so allocations failed with the phone nowhere near its
// ceiling. The watcher at the bottom of this file keeps an eye on that.
//
// Upstream's machinery was built for one notification: "low-memory" once,
// then "low-memory-ongoing" for as long as the pressure lasts, until
// something says it is over. Nothing here ever said it was over, so every
// event after the first of a run came out as "ongoing", and the JS engine
// does not collect on "ongoing" at all (nsJSEnvironment.cpp). So the engine
// is now told when the pressure ends ("memory-pressure-stop"), and the next
// spell of pressure starts with a fresh "low-memory" again. This file is the
// only thing that tells it either: upstream's own watcher of the system's
// memory is not started on this port (AvailableMemoryWatcherWin.cpp). All it
// could do here was tell the engine behind this file's back -- its stop would
// turn the next cheap "ongoing" sent from here into a fresh "low-memory",
// collection and all -- because the one other thing it does, unloading a
// background tab, needs tabs in processes of their own.
//
// What a fresh "low-memory" costs was measured in build 172, which re-sent it
// every half minute while the pressure was serious: the JS engine answers it
// with a full, non-incremental, shrinking GC, a cycle collection and usually
// a second GC, all on the one main thread there is -- in a single process
// CCGCScheduler::MayGCNow resolves at once, so it all runs inside the
// notification. On a Lumia 650 with Discord seven of them took 1.0, 2.4,
// 1.7, 5.4, 6.9, 12.2 and 21.9 s, each slower as the heap grew and more of
// it was paged out, and only one freed much: the page's heap was live. So a
// fresh one is sent again only when it is predicted to be short, takes no
// more than a tenth of the time, and the last ones paid for themselves in
// the process's commit -- what the phone counts against its ceiling.
// Otherwise the engine gets the cheap "ongoing", which drops the caches and
// leaves the JS heap alone. The cure must not become the illness.

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>

#include "js/GCAPI.h"
#include "mozilla/Atomics.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/CycleCollectedJSContext.h"
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
// Not zero while the app has been told it is going to the background and
// has not come back yet: suspended in between, or about to be. The clocks run
// on through that and nothing else does (see Evaluate). A number rather than
// a flag, a new one each time it goes: the call that says it is back names
// the going it answers, and one that arrives late, after the app has gone
// again, clears nothing.
Atomic<uint32_t> sAwayToken{0};
Atomic<uint32_t> sAwayCount{0};
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
// When the collection of the last fresh "low-memory" was over, what it cost
// and how big the JS heap was just before it. The gaps below are counted from
// its end, so that a collection which took seconds does not eat into the time
// the page gets before the next; the cost of the next is predicted from it.
TimeStamp sLastFresh;
double sLastFreshMs = 0;
uint32_t sLastFreshHeapMb = 0;
// Where the fresh re-arm's clock counts from: the same moment, unless the
// clocks were started again since (RestartClocks). Kept apart from
// sLastFresh, which also says "a collection has only just run" to the failed
// allocation and to pressure coming back, and has to stay true for them.
TimeStamp sFreshClockFrom;
// When the engine was last told again under pressure that is serious, fresh
// or "ongoing": the re-arm clock.
TimeStamp sLastRearm;
// When "ongoing" was last delivered, or a fresh one, which drops the same
// caches.
TimeStamp sLastOngoing;
// A failed allocation holds the pressure on by itself until then.
TimeStamp sFailureUntil;
// So does an address space that has just gone low, once (see Evaluate).
TimeStamp sAddressUntil;
bool sAddressSeen = false;
// Since when nothing has wanted the pressure that is still on.
TimeStamp sUnwantedSince;
StaticRefPtr<nsITimer> sRecheckTimer;
uint32_t sPressureNotes = 0;
uint32_t sSummaryNotes = 0;

// One spell of pressure: from the fresh "low-memory" that starts it to the
// "memory-pressure-stop" that ends it. What it is allowed, and what it did.
struct Spell {
  uint32_t id = 0;
  // Fresh re-arms in a row that freed too little, and whether that stopped
  // them for the rest of the spell.
  uint32_t unproductive = 0;
  bool fullStopped = false;
  // The same for the cheap re-arms, which only space themselves out.
  uint32_t cheapUnproductive = 0;
  // What happened, for the summary.
  uint32_t fullOn = 0, fullRearm = 0, fullFailure = 0;
  double fullMs = 0, fullFreedMb = 0;
  uint32_t skippedCost = 0, skippedStopped = 0, skippedGap = 0;
  uint32_t cheapTimer = 0, cheapAsked = 0;
  double cheapMs = 0, cheapFreedMb = 0;
  uint32_t dropped = 0;
  TimeStamp lastSummary;
};
Spell sSpell;
// Every fresh one and every cheap one gets a number, so that a reading which
// comes in after a newer one was sent can tell it is out of date.
uint32_t sFullSeq = 0;
uint32_t sCheapSeq = 0;

// How long a failed JS allocation holds the pressure on by itself: long
// enough that the engine stays in its low-memory state for what comes right
// after, short enough that one failure does not keep it there for the rest of
// the run.
constexpr uint32_t kFailureHoldMs = 30000;
// A failed allocation within this long of the last fresh "low-memory" asks
// for nothing more: that one's collection has only just run.
constexpr uint32_t kFailureGapMs = 5000;
// Pressure that comes back within this long of the last fresh "low-memory"
// -- or within kDutyFactor times what that one cost, if longer, up to the
// maximum -- waits out the rest before it is turned on again. Otherwise the
// stop and the start would follow each other as fast as the inputs flip, and
// every start is another shrinking GC and cycle collection. Not longer than
// the maximum: while it waits the engine is told nothing at all.
constexpr uint32_t kTurnOnGapMs = 10000;
constexpr uint32_t kTurnOnGapMaxMs = 60000;
// A timer set for the moment something falls due may fire a little early;
// within this much, the moment has come.
constexpr double kTimerSlackMs = 100;
// A fresh re-arm waits at least this many times what the last fresh one cost
// after it ended, and at least pressure-rearm-ms: at most a tenth of the main
// thread's time goes to them, and only while they keep paying for
// themselves. Nine, not more: at nineteen the one collection of 172's run
// that freed 142 MB in 1.7 s would have had to wait 46 s after the 2.4 s one
// before it, and by then the heap had grown past what the cap allows.
constexpr double kDutyFactor = 9;
// And is not sent at all when it is predicted to take longer than this. The
// prediction is the last one's cost, scaled up by how much the JS heap has
// grown since (never down): 1020 ms at 24 MB predicted 5440 ms at 128 MB, and
// it took 5360.
constexpr double kFreshCostCapMs = 3000;
constexpr uint32_t kHeapFloorMb = 8;
// A fresh one paid for itself when the commit it freed, from before it to
// after the purge that follows it, was at least this much, and at least this
// much a second of the time it took. By the phone's own count in 172, 142 MB
// in 1.7 s did; 0 MB in 6.9 and in 21.9 s did not.
constexpr double kMinFreedMb = 16;
constexpr double kFreedMbPerSecond = 10;
// Two that did not, and the rest of the spell gets cheap ones only.
constexpr uint32_t kMaxUnproductive = 2;
// The cheap ones under serious pressure come every pressure-rearm-ms, twice
// and four times that once they stop freeing this much. Their clock is their
// own: what a cache purge frees says nothing of what a collection would.
constexpr double kMinCheapFreedMb = 4;
constexpr uint32_t kCheapBackoffMax = 2;
// No gap ever grows past this.
constexpr uint32_t kRearmGapMaxMs = 300000;
// The phone repeats its level many times a second near the ceiling (level 3
// was entered seven times in 1.6 s); each "ongoing" flushes WebRender's
// caches, so the phone gets one at most this often -- twice and four times
// this while they free nothing, like the clock's own.
constexpr uint32_t kOngoingGapMs = 10000;
// An address space that has just gone low holds the pressure on this long:
// one fresh collection, and the caches dropped meanwhile. Not more: no
// collection in 172 ever made the largest free piece bigger (62 MB before and
// after, 59 and 59) -- the big pieces went to codec libraries, a second
// renderer and the microphone, not to the JS heap.
constexpr uint32_t kAddressHoldMs = 30000;
// Pressure nothing wants any more is ended only after this long of that. The
// phone's levels have no slack in them: usage sitting on the line between
// Low and Medium crosses it every few seconds, and a stop at every fall would
// make every rise a new spell -- a full collection every ten seconds, the one
// kind no cap applies to.
constexpr uint32_t kStopDwellMs = 30000;
// A summary of the spell this often while it lasts, and at its end.
constexpr uint32_t kSummaryMs = 120000;
constexpr uint32_t kPressureNoteCap = 60;
constexpr uint32_t kSummaryNoteCap = 40;

// --------------------------------------------- the address-space watcher

StaticRefPtr<nsISerialEventTarget> sWatchQueue;
// Touched on the watcher's queue only, once it has been started.
StaticRefPtr<nsITimer> sWatchTimer;

// A walk of the whole address space took 49 ms on a Lumia 650 (1668
// regions, a system call each), and 172 made one every second once the
// largest free piece was within twice the threshold -- five per cent of a
// core. Now a tick every second looks only at the few largest free pieces
// the last walk found, which proves the space is not low as long as one of
// them is still big enough; a whole walk is made only when that proof fails,
// and never more than one per hundred times what the last one took.
constexpr uint32_t kFirstTickMs = 5000;
constexpr uint32_t kCheckMs = 1000;
constexpr int kKeptPieces = 8;
// A piece something was taken from is stepped through this far and no
// further, so that one piece being filled from its bottom a megabyte at a
// time does not use up the look at the others.
constexpr uint32_t kCheckCallsPerPiece = 3;
constexpr uint32_t kWalkMinGapMs = 2000;
constexpr double kWalkCostFactor = 100;
// The cost that factor multiplies is wall-clock time on a pool thread, which
// a busy phone stretches; past this much it is the phone, not the walk.
constexpr double kWalkCostMaxMs = 100;
// While the space is low: a walk after each fresh collection, to say what it
// did, and otherwise one this often.
constexpr uint32_t kWalkLowGapMs = 30000;
constexpr uint32_t kWatchSummaryMs = 120000;
constexpr uint32_t kWatchNoteCap = 40;

void Evaluate();
void RequestEvaluate();

void PressureNote(const char* aText) {
  if (sPressureNotes < kPressureNoteCap) {
    ++sPressureNotes;
    GeckoW10mNote(aText);
  }
}

// The process's commit charge, in bytes: what the phone holds the app to
// (its usage moves one for one with the system's commit), not the working
// set the memory reporters call resident. One system call, any thread;
// 0 when it cannot be read.
uint64_t ReadCommit() {
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  if (!::GetProcessMemoryInfo(
          ::GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
          sizeof(counters))) {
    return 0;
  }
  return counters.PrivateUsage;
}

double ToMb(uint64_t aBytes) { return double(aBytes) / (1024.0 * 1024.0); }

// The JS GC heap in MB, a counter the GC keeps. Main thread; 0 when there is
// no JS context (yet, or any more).
uint32_t ReadJsHeapMb() {
  mozilla::CycleCollectedJSContext* ccjs =
      mozilla::CycleCollectedJSContext::Get();
  JSContext* cx = ccjs ? ccjs->Context() : nullptr;
  return cx ? JS_GetGCParameter(cx, JSGC_BYTES) >> 20 : 0;
}

bool HoldsAt(const TimeStamp& aUntil, const TimeStamp& aNow) {
  return !aUntil.IsNull() &&
         aNow + TimeDuration::FromMilliseconds(kTimerSlackMs) < aUntil;
}

double MsSince(const TimeStamp& aThen, const TimeStamp& aNow) {
  return aThen.IsNull() ? -1 : (aNow - aThen).ToMilliseconds();
}

uint32_t RearmMs() {
  return mozilla::StaticPrefs::memory_gecko_w10m_pressure_rearm_ms();
}

// How long pressure that ended stays off at least, from the end of the last
// fresh collection.
double TurnOnHoldMs() {
  return std::min<double>(
      kTurnOnGapMaxMs,
      std::max<double>(kTurnOnGapMs, kDutyFactor * sLastFreshMs));
}

// How long after the end of the last fresh collection the next fresh re-arm
// waits at least.
double FreshGapMs(uint32_t aRearmMs) {
  return std::min<double>(
      kRearmGapMaxMs, std::max<double>(aRearmMs, kDutyFactor * sLastFreshMs));
}

// The re-arm clock's period: pressure-rearm-ms, doubled and doubled again
// while the cheap ones free nothing.
double CheapGapMs(uint32_t aRearmMs) {
  const uint32_t shift = std::min(sSpell.cheapUnproductive, kCheapBackoffMax);
  return std::min<double>(kRearmGapMaxMs, double(aRearmMs) * (1u << shift));
}

// And how far apart the ones the phone asks for are kept.
double AskedGapMs() {
  const uint32_t shift = std::min(sSpell.cheapUnproductive, kCheapBackoffMax);
  return double(kOngoingGapMs) * (1u << shift);
}

// Why the engine is under pressure, for the notes.
void DescribeReasons(char* aOut, size_t aLength, int32_t aLevel,
                     bool aAddressHolds, bool aFailure) {
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
  if (aAddressHolds) {
    add("the address space went low");
  }
  if (aFailure) {
    const char* why = sFailedWhy;
    add(why ? why : "an allocation failed");
  }
  if (!used) {
    add("left over from before the start");
  }
}

// ------------------------------------------------- telling the engine

enum class Kind : uint8_t { On, Rearm, Failure, Timer, Asked };

const char* KindName(Kind aKind) {
  switch (aKind) {
    case Kind::On:
      return "pressure on";
    case Kind::Rearm:
      return "re-arm";
    case Kind::Failure:
      return "an allocation failed";
    case Kind::Timer:
      return "re-arm";
    case Kind::Asked:
      return "asked";
  }
  return "?";
}

// What one notification cost and what it freed. The commit is read before,
// right after -- the JS engine's part is over by then -- and once more from a
// runnable queued behind the jemalloc purge that every memory-pressure
// notification queues on the main thread at normal priority
// (AvailableMemoryTracker.cpp), which frees the dirty pages the collection
// left behind.
struct Record {
  Kind kind = Kind::On;
  uint32_t spell = 0;
  uint32_t seq = 0;
  double ms = 0;
  double predictedMs = 0;
  uint32_t heapBeforeMb = 0;
  uint32_t heapAfterMb = 0;
  uint64_t commitBefore = 0;
  uint64_t commitSync = 0;
  // False when the app may have been suspended while it ran (SendFresh).
  bool timed = true;
  char reasons[112] = {};
};

bool IsFull(Kind aKind) {
  return aKind == Kind::On || aKind == Kind::Rearm || aKind == Kind::Failure;
}

void AfterLetGo(const Record& aRecord) {
  if (sShutdown) {
    return;
  }
  const uint64_t commitAfter = ReadCommit();
  double freedMb = -1;
  if (aRecord.commitBefore && aRecord.commitSync && commitAfter) {
    const double before = ToMb(aRecord.commitBefore);
    freedMb = std::max(0.0, std::max(before - ToMb(aRecord.commitSync),
                                     before - ToMb(commitAfter)));
  }
  const bool current = aRecord.spell == sSpell.id;
  char note[384];

  if (!IsFull(aRecord.kind)) {
    // A cheap one: what it freed sets how far apart the next ones are.
    if (current && aRecord.seq == sCheapSeq) {
      sSpell.cheapFreedMb += std::max(0.0, freedMb);
      const uint32_t was = sSpell.cheapUnproductive;
      if (freedMb >= kMinCheapFreedMb) {
        sSpell.cheapUnproductive = 0;
      } else if (sSpell.cheapUnproductive < kCheapBackoffMax) {
        ++sSpell.cheapUnproductive;
      }
      if (sSpell.cheapUnproductive != was) {
        // The clock's period changed; the timer is set again for it.
        RequestEvaluate();
      }
    }
    return;
  }

  // A commit that could not be read counts as nothing freed, which stops
  // the re-arms sooner -- the cautious side for the page.
  const double wantMb =
      std::max(kMinFreedMb, aRecord.ms * kFreedMbPerSecond / 1000.0);
  const bool productive = freedMb >= wantMb;
  const char* verdict = "";
  if (!aRecord.timed) {
    verdict = "the app was on its way to the background, not timed or counted";
  } else if (current) {
    sSpell.fullFreedMb += std::max(0.0, freedMb);
    if (aRecord.seq == sFullSeq) {
      if (productive) {
        sSpell.unproductive = 0;
        verdict = "paid for itself";
      } else if (aRecord.kind == Kind::Rearm) {
        // Only a re-arm counts against the next: the one that starts the
        // spell is upstream's own, and a failure's comes once a run.
        if (++sSpell.unproductive >= kMaxUnproductive) {
          sSpell.fullStopped = true;
          verdict = "did not pay for itself, no more fresh ones this spell";
        } else {
          verdict = "did not pay for itself";
        }
      } else {
        verdict = "freed little (not counted)";
      }
    } else {
      verdict = "a newer one went out meanwhile";
    }
  } else {
    verdict = "the spell is over";
  }

  char predicted[48] = {};
  if (aRecord.predictedMs > 0) {
    SprintfLiteral(predicted, " (predicted %.0f)", aRecord.predictedMs);
  }
  char freed[96];
  if (freedMb < 0) {
    SprintfLiteral(freed, "commit not readable");
  } else {
    SprintfLiteral(freed, "commit %.0f -> %.0f -> %.0f MB: freed %.0f MB",
                   ToMb(aRecord.commitBefore), ToMb(aRecord.commitSync),
                   ToMb(commitAfter), freedMb);
  }
  SprintfLiteral(note,
                 "mem: let go #%u (%s; %s) -- %.0f ms%s, js gc heap %u -> "
                 "%u MB, %s, %s",
                 aRecord.seq, KindName(aRecord.kind), aRecord.reasons,
                 aRecord.ms, predicted, aRecord.heapBeforeMb,
                 aRecord.heapAfterMb, freed, verdict);
  PressureNote(note);
}

void QueueAfter(const Record& aRecord) {
  // Plain priority, so that it lands behind the purge (see Record).
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "GeckoW10mMemoryPressure::AfterLetGo",
      [record = aRecord]() { AfterLetGo(record); }));
}

void NoteCommitReadOnce() {
  static bool sNoted = false;
  if (sNoted) {
    return;
  }
  sNoted = true;
  LARGE_INTEGER frequency, began, ended;
  ::QueryPerformanceFrequency(&frequency);
  ::QueryPerformanceCounter(&began);
  const uint64_t commit = ReadCommit();
  ::QueryPerformanceCounter(&ended);
  char note[128];
  SprintfLiteral(note, "mem: a commit read takes %u us (commit %.0f MB)",
                 frequency.QuadPart
                     ? unsigned(uint64_t(ended.QuadPart - began.QuadPart) *
                                1000000 / uint64_t(frequency.QuadPart))
                     : 0u,
                 ToMb(commit));
  GeckoW10mNote(note);
}

// "memory-pressure-stop" and then a fresh "low-memory", each delivered here
// and now rather than left pending. nsMemoryPressure keeps one pending state
// only, so a NoPressure followed by a LowMemory, both left pending, would
// collapse into the LowMemory alone -- and that comes out as "ongoing" again.
// Whatever another thread left pending goes first, so that it cannot land in
// between.
void SendFresh(Kind aKind, double aPredictedMs, const char* aReasons) {
  NoteCommitReadOnce();
  Record record;
  record.kind = aKind;
  record.spell = sSpell.id;
  record.seq = ++sFullSeq;
  record.predictedMs = aPredictedMs;
  SprintfLiteral(record.reasons, "%s", aReasons);
  record.heapBeforeMb = ReadJsHeapMb();
  record.commitBefore = ReadCommit();

  const bool awayBefore = sAwayToken != 0;
  const TimeStamp start = TimeStamp::Now();
  NS_DispatchMemoryPressure();
  NS_NotifyOfEventualMemoryPressure(MemoryPressureState::NoPressure);
  NS_DispatchMemoryPressure();
  NS_NotifyOfEventualMemoryPressure(MemoryPressureState::LowMemory);
  NS_DispatchMemoryPressure();
  const TimeStamp end = TimeStamp::Now();

  record.ms = (end - start).ToMilliseconds();
  // The app on its way to the background before this or during it may have
  // been suspended in the middle, and the stopwatch ran on through that: the
  // time says nothing of what the collection cost.
  record.timed = !awayBefore && !sAwayToken;
  record.commitSync = ReadCommit();
  record.heapAfterMb = ReadJsHeapMb();
  ++sLetGo;
  sLastFresh = end;
  sFreshClockFrom = end;
  if (record.timed) {
    sLastFreshMs = record.ms;
    sLastFreshHeapMb = record.heapBeforeMb;
    sSpell.fullMs += record.ms;
  }
  sLastRearm = end;
  sLastOngoing = end;
  switch (aKind) {
    case Kind::On:
      ++sSpell.fullOn;
      break;
    case Kind::Rearm:
      ++sSpell.fullRearm;
      break;
    default:
      ++sSpell.fullFailure;
      break;
  }
  QueueAfter(record);
}

// "low-memory-ongoing": the caches dropped, the JS heap left alone. The
// engine's state is LowMemory by now, so a LowMemory comes out as that.
void SendOngoing(Kind aKind) {
  Record record;
  record.kind = aKind;
  record.spell = sSpell.id;
  record.seq = ++sCheapSeq;
  record.commitBefore = ReadCommit();
  const TimeStamp start = TimeStamp::Now();
  NS_DispatchMemoryPressure();
  NS_NotifyOfEventualMemoryPressure(MemoryPressureState::LowMemory);
  NS_DispatchMemoryPressure();
  const TimeStamp end = TimeStamp::Now();
  record.ms = (end - start).ToMilliseconds();
  record.timed = !sAwayToken;
  record.commitSync = ReadCommit();
  sLastOngoing = end;
  // The clock's own or one that was asked for, the caches are dropped all the
  // same: the clock starts again from either, so that its tick does not flush
  // them a second time half a second after the phone's.
  sLastRearm = end;
  if (aKind == Kind::Timer) {
    ++sSpell.cheapTimer;
  } else {
    ++sSpell.cheapAsked;
  }
  if (record.timed) {
    sSpell.cheapMs += record.ms;
  }
  QueueAfter(record);
}

void SendStop() {
  NS_DispatchMemoryPressure();
  NS_NotifyOfEventualMemoryPressure(MemoryPressureState::NoPressure);
  NS_DispatchMemoryPressure();
}

void NoteSummary(const TimeStamp& aNow, const char* aWhen) {
  if (sSummaryNotes >= kSummaryNoteCap) {
    return;
  }
  ++sSummaryNotes;
  sSpell.lastSummary = aNow;
  const double spellMs = std::max(1.0, MsSince(sToldSince, aNow));
  char note[448];
  SprintfLiteral(
      note,
      "mem: pressure %s %.0f s -- fresh let-gos %u (on %u, re-arm %u, "
      "failure %u) %.0f ms, freed %.0f MB; fresh re-arms skipped %u (cost %u, "
      "stopped %u, gap %u); ongoing %u (timer %u, asked %u) %.0f ms, freed "
      "%.0f MB; dropped by the throttle (%u s and up) %u; the main thread gave %.1f%% "
      "of the spell to them",
      aWhen, spellMs / 1000.0,
      sSpell.fullOn + sSpell.fullRearm + sSpell.fullFailure, sSpell.fullOn,
      sSpell.fullRearm, sSpell.fullFailure, sSpell.fullMs, sSpell.fullFreedMb,
      sSpell.skippedCost + sSpell.skippedStopped + sSpell.skippedGap,
      sSpell.skippedCost, sSpell.skippedStopped, sSpell.skippedGap,
      sSpell.cheapTimer + sSpell.cheapAsked, sSpell.cheapTimer,
      sSpell.cheapAsked, sSpell.cheapMs, sSpell.cheapFreedMb,
      kOngoingGapMs / 1000, sSpell.dropped,
      100.0 * (sSpell.fullMs + sSpell.cheapMs) / spellMs);
  GeckoW10mNote(note);
}

// The re-arm clocks start again from now, for pressure that is on: while the
// app is away in the background, where they run on and nothing else does.
// Otherwise everything would fall due in the first moment back -- a
// collection on top of the one chrome runs on the way out (heap-minimize).
void RestartClocks() {
  if (!sTold) {
    return;
  }
  const TimeStamp now = TimeStamp::Now();
  sFreshClockFrom = now;
  sLastRearm = now;
  sLastOngoing = now;
  // The wait before the stop as well: thirty seconds of the page running
  // with nothing wanting the pressure, not thirty seconds of the app asleep.
  if (!sUnwantedSince.IsNull()) {
    sUnwantedSince = now;
  }
}

void Recheck(nsITimer*, void*) { Evaluate(); }

// While the engine is under pressure something has to come back to it. The
// shell cannot be relied on for that: once the phone is over its limit there
// is no higher level to rise to and it says nothing more. So a one-shot timer
// on the main thread, set for the next tick of the re-arm clock, or the end
// of a hold -- or, for pressure held back by TurnOnHoldMs, for when it may be
// turned on -- and for nothing at all while there is no pressure. Counted
// from now, not from when the evaluation began: a collection in between
// would stretch every gap by its own length.
void RecheckLater(bool aWanted, bool aSerious) {
  const TimeStamp now = TimeStamp::Now();
  TimeStamp next;
  auto consider = [&](const TimeStamp& aWhen) {
    if (!aWhen.IsNull() && aWhen > now && (next.IsNull() || aWhen < next)) {
      next = aWhen;
    }
  };
  if (sTold && !sShutdown) {
    const uint32_t rearmMs = RearmMs();
    if (aSerious && rearmMs) {
      // The cheap clock; due already, or never told again yet, means as soon
      // as the timer's minimum allows.
      const TimeStamp due =
          sLastRearm.IsNull()
              ? now
              : sLastRearm +
                    TimeDuration::FromMilliseconds(CheapGapMs(rearmMs));
      next = due > now ? due : now;
      // And the fresh one's, when that comes sooner and can still come to
      // anything. One that is already past was refused for its cost, and is
      // looked at again at the cheap clock's next tick.
      if (!sSpell.fullStopped && !sFreshClockFrom.IsNull()) {
        consider(sFreshClockFrom +
                 TimeDuration::FromMilliseconds(FreshGapMs(rearmMs)));
      }
    }
    // The end of a hold, or of the wait before the stop. One that has passed
    // since the evaluation looked -- a collection can outlast it -- is due
    // now, not forgotten: nothing else would come back to end the pressure.
    // Evaluate clears each as it sees it over, so this does not go round.
    auto considerEnd = [&](const TimeStamp& aWhen) {
      if (!aWhen.IsNull()) {
        const TimeStamp when = aWhen > now ? aWhen : now;
        if (next.IsNull() || when < next) {
          next = when;
        }
      }
    };
    considerEnd(sFailureUntil);
    considerEnd(sAddressUntil);
    if (!aWanted && !sUnwantedSince.IsNull()) {
      considerEnd(sUnwantedSince +
                  TimeDuration::FromMilliseconds(kStopDwellMs));
    }
  } else if (aWanted && !sShutdown && !sLastFresh.IsNull()) {
    next = sLastFresh + TimeDuration::FromMilliseconds(TurnOnHoldMs());
  }
  if (next.IsNull()) {
    if (sRecheckTimer) {
      sRecheckTimer->Cancel();
    }
    return;
  }
  const double delay = next > now ? (next - now).ToMilliseconds() : 0;
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

enum class Skip : uint8_t { None, Stopped, Cost, Gap };

// Whether a fresh re-arm whose time has come is refused all the same, and
// what it is predicted to cost.
Skip FreshBlocked(double* aPredicted) {
  const uint32_t heapMb = ReadJsHeapMb();
  const double grown =
      double(std::max(heapMb, kHeapFloorMb)) /
      double(std::max(sLastFreshHeapMb, kHeapFloorMb));
  *aPredicted = sLastFreshMs * std::max(1.0, grown);
  if (sSpell.fullStopped) {
    return Skip::Stopped;
  }
  if (*aPredicted > kFreshCostCapMs) {
    return Skip::Cost;
  }
  return Skip::None;
}

void NoteSkip(Skip aWhy, double aPredicted, double aSinceFresh) {
  uint32_t* count = aWhy == Skip::Stopped ? &sSpell.skippedStopped
                    : aWhy == Skip::Cost  ? &sSpell.skippedCost
                                          : &sSpell.skippedGap;
  // The first for each reason in a spell, then every fifth.
  if ((*count)++ % 5) {
    return;
  }
  char note[256];
  switch (aWhy) {
    case Skip::Stopped:
      SprintfLiteral(note,
                     "mem: fresh re-arm skipped -- %u in a row did not pay "
                     "for themselves; ongoing instead",
                     kMaxUnproductive);
      break;
    case Skip::Cost:
      SprintfLiteral(note,
                     "mem: fresh re-arm skipped -- predicted %.0f ms > %.0f "
                     "(last %.0f ms at %u MB of js gc heap); ongoing instead",
                     aPredicted, kFreshCostCapMs, sLastFreshMs,
                     sLastFreshHeapMb);
      break;
    default:
      SprintfLiteral(note,
                     "mem: fresh re-arm skipped -- its clock has run %.0f s, "
                     "after one that took %.0f ms; ongoing instead",
                     aSinceFresh / 1000.0, sLastFreshMs);
      break;
  }
  PressureNote(note);
}

// The one place that decides what the engine is told. Main thread.
void Evaluate() {
  MOZ_ASSERT(NS_IsMainThread());
  // Cleared first: an input that arrives while this runs sends another.
  sEvaluatePending = false;
  if (sShutdown) {
    return;
  }
  if (sAwayToken) {
    // On the way to the background or just back from it, whichever of the
    // timer, an input and the foreground's own call gets here first: nothing
    // falls due by time alone. Pressure can still come on and hold.
    RestartClocks();
  }
  const TimeStamp now = TimeStamp::Now();
  const bool shellAsked = sShellAsked.exchange(false);
  const bool failed = sFailed.exchange(false);
  const int32_t level = sShellLevel;
  const double sinceFresh = MsSince(sLastFresh, now);
  const double turnOnHoldMs = TurnOnHoldMs();
  const bool turnOnHeld =
      sinceFresh >= 0 && sinceFresh + kTimerSlackMs < turnOnHoldMs;
  // A hold that begins while pressure is held back by the turn-on hold
  // begins when that ends, so that it is still there to turn it on.
  const TimeStamp holdFrom =
      turnOnHeld && !sTold
          ? sLastFresh + TimeDuration::FromMilliseconds(turnOnHoldMs)
          : now;
  const bool failureDue =
      failed && (sinceFresh < 0 || sinceFresh >= kFailureGapMs);
  if (failed) {
    // One that is due does not wait for the turn-on hold (below).
    sFailureUntil = (failureDue ? now : holdFrom) +
                    TimeDuration::FromMilliseconds(kFailureHoldMs);
  }

  // The address space going low is an edge, not a state: on the HP it is low
  // for the whole session, and pressure held on for that alone would keep
  // the engine in its low-memory state -- the nursery at its smallest -- for
  // nothing a collection can fix. The hold starts when the pressure may be
  // turned on, so that one held back still gets its collection.
  const bool addressLow = sAddressLow;
  const bool addressEdge = addressLow && !sAddressSeen;
  if (addressEdge) {
    sAddressUntil = holdFrom + TimeDuration::FromMilliseconds(kAddressHoldMs);
  }
  sAddressSeen = addressLow;

  // A hold seen over is cleared: RecheckLater takes one that is still set
  // and in the past for one that ran out unseen.
  auto holds = [&](TimeStamp& aUntil) {
    if (HoldsAt(aUntil, now)) {
      return true;
    }
    aUntil = TimeStamp();
    return false;
  };
  const bool failureHolds = holds(sFailureUntil);
  const bool addressHolds = holds(sAddressUntil);
  const bool wanted = level >= 1 || addressHolds || failureHolds;
  // Serious enough for the re-arm clock: the phone at High or over its
  // limit, or an allocation that failed. Medium is where a 1 GB Lumia spends
  // much of a session, about half of its 390 MB, and gets one fresh
  // collection when the pressure comes on and the phone's "ongoing" after.
  const bool serious = level >= 2 || failureHolds;
  const uint32_t rearmMs = RearmMs();
  if (wanted || shellAsked) {
    // Wanted, or wanted a moment ago: a rise that fell back before this ran
    // left only its asking behind, and still starts the wait before the stop
    // again.
    sUnwantedSince = TimeStamp();
  }

  char reasons[112];
  DescribeReasons(reasons, sizeof(reasons), level, addressHolds,
                  failureHolds);
  if (wanted && !sTold) {
    if (turnOnHeld && !failureDue) {
      // Back moments after the last collection; the timer brings this round
      // again when the hold is over, and it is turned on then if it is still
      // wanted. Nothing in between: with the pressure off, even the cheap
      // "low-memory" would come out as a fresh one. A failed allocation does
      // not wait; it happens once a run.
      RecheckLater(wanted, serious);
      return;
    }
    sSpell = Spell();
    sSpell.id = sFullSeq + 1;
    sSpell.lastSummary = now;
    sTold = true;
    sToldSince = now;
    SendFresh(failureDue ? Kind::Failure : Kind::On, 0, reasons);
  } else if (!wanted && sTold) {
    if (sUnwantedSince.IsNull()) {
      sUnwantedSince = now;
    }
    if (MsSince(sUnwantedSince, now) + kTimerSlackMs >= kStopDwellMs) {
      sTold = false;
      sUnwantedSince = TimeStamp();
      SendStop();
      NoteSummary(now, "over after");
    }
  } else if (wanted && sTold) {
    bool told = false;
    if (failureDue) {
      SendFresh(Kind::Failure, 0, reasons);
      told = true;
    } else if (serious && rearmMs) {
      // Two clocks. The cheap one ticks every pressure-rearm-ms, less often
      // while it frees nothing. The fresh one's time comes FreshGapMs after
      // the last fresh collection ended, whenever that is -- the cheap clock
      // does not hold it back -- and it is then sent unless it is refused
      // for its cost or its record. Never told yet counts as due for both:
      // pressure the engine was put under the old way, before the start.
      const double sinceRearm = MsSince(sLastRearm, now);
      const bool cheapDue =
          sinceRearm < 0 || sinceRearm + kTimerSlackMs >= CheapGapMs(rearmMs);
      const double sinceClock = MsSince(sFreshClockFrom, now);
      const bool freshDue =
          sinceClock < 0 || sinceClock + kTimerSlackMs >= FreshGapMs(rearmMs);
      if (freshDue || cheapDue) {
        double predicted = 0;
        const Skip skip = freshDue ? FreshBlocked(&predicted) : Skip::Gap;
        if (skip == Skip::None) {
          SendFresh(Kind::Rearm, predicted, reasons);
          told = true;
        } else if (cheapDue) {
          NoteSkip(skip, predicted, sinceClock);
          SendOngoing(Kind::Timer);
          told = true;
        }
      }
    }
    if (!told && (shellAsked || addressEdge)) {
      // The phone said so again, or the address space went low while the
      // pressure was already on: the caches go, WebRender's big textures
      // among them, which is all a collection could do for the address
      // space anyway. The phone's is throttled; the address space's comes
      // once per time it goes low, and is not.
      const double sinceOngoing = MsSince(sLastOngoing, now);
      if (addressEdge || sinceOngoing < 0 ||
          sinceOngoing + kTimerSlackMs >= AskedGapMs()) {
        SendOngoing(Kind::Asked);
      } else {
        ++sSpell.dropped;
      }
    }
    if (MsSince(sSpell.lastSummary, now) >= kSummaryMs) {
      NoteSummary(now, "on for");
    }
  }
  RecheckLater(wanted, serious);
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

struct Piece {
  uintptr_t base = 0;
  size_t size = 0;
};

struct AddressSpace {
  size_t freeBytes = 0;
  size_t largest = 0;
  size_t pieces = 0;
  size_t regions = 0;
  // The largest free pieces, largest first.
  Piece kept[kKeptPieces];
  int keptCount = 0;
};

void Keep(AddressSpace& aSpace, uintptr_t aBase, size_t aSize) {
  int at = aSpace.keptCount;
  while (at > 0 && aSpace.kept[at - 1].size < aSize) {
    --at;
  }
  if (at >= kKeptPieces) {
    return;
  }
  const int last = std::min(aSpace.keptCount, kKeptPieces - 1);
  for (int i = last; i > at; --i) {
    aSpace.kept[i] = aSpace.kept[i - 1];
  }
  aSpace.kept[at] = Piece{aBase, aSize};
  aSpace.keptCount = std::min(aSpace.keptCount + 1, kKeptPieces);
}

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
      Keep(space, uintptr_t(info.BaseAddress), info.RegionSize);
    }
    const uintptr_t next = uintptr_t(info.BaseAddress) + info.RegionSize;
    if (next <= at) {
      break;
    }
    at = next;
  }
  return space;
}

// The largest free run that starts inside one of the kept pieces now: a
// lower bound on the largest free piece, since every run it finds is real.
// A piece nothing was taken from answers in one call; one that something was
// taken from is stepped through for kCheckCallsPerPiece calls.
size_t CheckPieces(const AddressSpace& aLast, uint32_t* aCalls) {
  size_t best = 0;
  uint32_t calls = 0;
  for (int i = 0; i < aLast.keptCount; ++i) {
    uintptr_t at = aLast.kept[i].base;
    const uintptr_t end = at + aLast.kept[i].size;
    for (uint32_t n = 0; at < end && n < kCheckCallsPerPiece; ++n) {
      MEMORY_BASIC_INFORMATION info;
      if (::VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) !=
          sizeof(info)) {
        break;
      }
      ++calls;
      if (info.State == MEM_FREE && info.RegionSize > best) {
        best = info.RegionSize;
      }
      const uintptr_t next = uintptr_t(info.BaseAddress) + info.RegionSize;
      if (next <= at) {
        break;
      }
      at = next;
    }
  }
  *aCalls = calls;
  return best;
}

// The watcher's own state; its task queue runs one tick at a time.
AddressSpace sLastWalk;
bool sWalked = false;
TimeStamp sLastWalkAt;
double sLastWalkMs = 0;
bool sLow = false;
uint32_t sLetGoNoted = 0;
uint32_t sWatchNotes = 0;
uint32_t sWatchSummaries = 0;
LARGE_INTEGER sFrequency{};
// Since the last summary.
struct WatchStats {
  uint32_t walks = 0;
  double walkMs = 0, walkMaxMs = 0;
  size_t regionsMin = 0, regionsMax = 0;
  uint32_t checks = 0, checkCalls = 0;
  double checkMs = 0;
  size_t lastHint = 0;
  TimeStamp since;
} sWatchStats;

void WatchNote(const char* aText) {
  if (sWatchNotes < kWatchNoteCap) {
    ++sWatchNotes;
    GeckoW10mNote(aText);
  }
}

double ElapsedMs(const LARGE_INTEGER& aBegan, const LARGE_INTEGER& aEnded) {
  return sFrequency.QuadPart
             ? double(aEnded.QuadPart - aBegan.QuadPart) * 1000.0 /
                   double(sFrequency.QuadPart)
             : 0;
}

void MaybeWatchSummary(const TimeStamp& aNow) {
  constexpr size_t kMB = 1024 * 1024;
  if (sWatchStats.since.IsNull()) {
    sWatchStats.since = aNow;
    return;
  }
  if ((aNow - sWatchStats.since).ToMilliseconds() < kWatchSummaryMs ||
      sWatchSummaries >= kWatchNoteCap) {
    return;
  }
  ++sWatchSummaries;
  char note[320];
  SprintfLiteral(
      note,
      "vm watch: last %.0f s -- %u walks (avg %.0f ms, max %.0f ms, %zu-%zu "
      "regions), %u checks (avg %.2f ms, %.1f calls); largest free piece %zu "
      "MB (the check sees %zu MB), %zu MB free in %zu pieces; %s",
      (aNow - sWatchStats.since).ToSeconds(), sWatchStats.walks,
      sWatchStats.walks ? sWatchStats.walkMs / sWatchStats.walks : 0.0,
      sWatchStats.walkMaxMs, sWatchStats.regionsMin, sWatchStats.regionsMax,
      sWatchStats.checks,
      sWatchStats.checks ? sWatchStats.checkMs / sWatchStats.checks : 0.0,
      sWatchStats.checks ? double(sWatchStats.checkCalls) / sWatchStats.checks
                         : 0.0,
      sLastWalk.largest / kMB, sWatchStats.lastHint / kMB,
      sLastWalk.freeBytes / kMB, sLastWalk.pieces, sLow ? "low" : "clear");
  GeckoW10mNote(note);
  sWatchStats = WatchStats();
  sWatchStats.since = aNow;
}

void WatchTick(nsITimer*, void*) {
  if (sShutdown) {
    return;
  }
  // Up to a gigabyte: the space is two, and one and a half times the
  // threshold has to fit.
  const uint32_t lowMb = std::min<uint32_t>(
      mozilla::StaticPrefs::memory_gecko_w10m_address_space_low_mb(), 1024);
  if (!lowMb) {
    // Turned off while running: the address space stops asking, and the
    // watcher stops looking.
    if (sLow) {
      sLow = false;
      sAddressLow = false;
      RequestEvaluate();
    }
    GeckoW10mNote(
        "vm watch: off -- memory.gecko-w10m.address-space.low-mb is 0");
    return;
  }
  if (!sFrequency.QuadPart) {
    ::QueryPerformanceFrequency(&sFrequency);
  }
  constexpr size_t kMB = 1024 * 1024;
  const uint64_t threshold = uint64_t(lowMb) * kMB;
  const uint64_t backAbove = threshold * 3 / 2;
  // Read before looking, so that a collection which finishes meanwhile is
  // not credited with what was seen before it.
  const uint32_t letGo = sLetGo;
  const TimeStamp now = TimeStamp::Now();
  char note[256];

  bool walk = !sWalked;
  if (sWalked) {
    LARGE_INTEGER began, ended;
    ::QueryPerformanceCounter(&began);
    uint32_t calls = 0;
    const uint64_t hint = CheckPieces(sLastWalk, &calls);
    ::QueryPerformanceCounter(&ended);
    ++sWatchStats.checks;
    sWatchStats.checkCalls += calls;
    sWatchStats.checkMs += ElapsedMs(began, ended);
    sWatchStats.lastHint = size_t(hint);
    if (!sLow) {
      // One of the pieces still big enough proves the space is not low.
      walk = hint < threshold;
    } else if (hint > backAbove) {
      sLow = false;
      sAddressLow = false;
      SprintfLiteral(note,
                     "vm watch: address space back -- a free piece of %zu MB "
                     "(from the check), above %u MB",
                     size_t(hint / kMB), unsigned(lowMb * 3 / 2));
      WatchNote(note);
      RequestEvaluate();
    } else {
      walk = letGo != sLetGoNoted ||
             (now - sLastWalkAt).ToMilliseconds() >= kWalkLowGapMs;
    }
    // At most one per hundred times what the last one took: one per cent
    // of a core, whatever the phone.
    if (walk &&
        (now - sLastWalkAt).ToMilliseconds() <
            std::max<double>(kWalkMinGapMs,
                             kWalkCostFactor *
                                 std::min(sLastWalkMs, kWalkCostMaxMs))) {
      walk = false;
    }
  }

  if (walk) {
    LARGE_INTEGER began, ended;
    ::QueryPerformanceCounter(&began);
    sLastWalk = WalkAddressSpace();
    ::QueryPerformanceCounter(&ended);
    sLastWalkMs = ElapsedMs(began, ended);
    sLastWalkAt = TimeStamp::Now();
    ++sWatchStats.walks;
    sWatchStats.walkMs += sLastWalkMs;
    sWatchStats.walkMaxMs = std::max(sWatchStats.walkMaxMs, sLastWalkMs);
    sWatchStats.regionsMin = sWatchStats.regionsMin
                                 ? std::min(sWatchStats.regionsMin,
                                            sLastWalk.regions)
                                 : sLastWalk.regions;
    sWatchStats.regionsMax =
        std::max(sWatchStats.regionsMax, sLastWalk.regions);
    if (!sWalked) {
      sWalked = true;
      SprintfLiteral(note,
                     "vm watch: first walk %.1f ms (%zu regions), largest "
                     "free piece %zu MB; then a check of the %d largest "
                     "pieces every %u s, and a whole walk only when it "
                     "cannot tell, at most one per %.0f times its cost",
                     sLastWalkMs, sLastWalk.regions, sLastWalk.largest / kMB,
                     kKeptPieces, kCheckMs / 1000, kWalkCostFactor);
      GeckoW10mNote(note);
    }

    const uint64_t largest = sLastWalk.largest;
    if (!sLow && largest < threshold) {
      sLow = true;
      sLetGoNoted = letGo;
      sAddressLow = true;
      SprintfLiteral(note,
                     "vm watch: address space low -- largest free piece %zu "
                     "MB, %zu MB free in %zu pieces (walk %.0f ms); asking "
                     "the engine to let go",
                     sLastWalk.largest / kMB, sLastWalk.freeBytes / kMB,
                     sLastWalk.pieces, sLastWalkMs);
      WatchNote(note);
      RequestEvaluate();
    } else if (sLow) {
      // Back only well clear of the threshold, so that a piece hovering at
      // it does not turn the pressure on and off at every walk.
      const bool back = largest > backAbove;
      const bool letGoSince = letGo != sLetGoNoted;
      if (letGoSince || back) {
        SprintfLiteral(note,
                       "vm watch: %s -- largest free piece %zu MB, %zu MB "
                       "free in %zu pieces (walk %.0f ms); %s %u MB",
                       letGoSince ? "after letting go" : "address space back",
                       sLastWalk.largest / kMB, sLastWalk.freeBytes / kMB,
                       sLastWalk.pieces, sLastWalkMs,
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
  }

  MaybeWatchSummary(now);
  if (sWatchTimer && !sShutdown) {
    sWatchTimer->InitWithNamedFuncCallback(WatchTick, nullptr, kCheckMs,
                                           nsITimer::TYPE_ONE_SHOT,
                                           "GeckoW10mAddressSpaceWatch"_ns);
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
    // The engine is already in its low-memory state, with the collection a
    // first "low-memory" runs only just behind it; the first evaluation
    // either keeps it there under the new rules, or ends it. The clocks start
    // from here, so that it is not at once told afresh.
    sSpell = Spell();
    sSpell.id = sFullSeq + 1;
    sTold = true;
    sToldSince = TimeStamp::Now();
    sSpell.lastSummary = sToldSince;
    sLastFresh = sToldSince;
    sFreshClockFrom = sToldSince;
    sLastRearm = sToldSince;
    sLastOngoing = sToldSince;
  }
  // Whatever the shell said before now is acted on.
  RequestEvaluate();

  if (!mozilla::StaticPrefs::memory_gecko_w10m_address_space_low_mb()) {
    GeckoW10mNote(
        "vm watch: off -- memory.gecko-w10m.address-space.low-mb is 0");
    return;
  }
  // A task queue on the background pool, not a thread of its own and not the
  // main thread: nothing it does needs anything the main thread has.
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
          WatchTick, nullptr, kFirstTickMs, nsITimer::TYPE_ONE_SHOT,
          "GeckoW10mAddressSpaceWatch"_ns))) {
    GeckoW10mNote("vm watch: the timer would not start -- not watching");
  }
}

// From the shell, which hears the phone's MemoryManager: 0 Low, 1 Medium,
// 2 High, 3 over the limit. The shell sends every change of level, up and
// down, as the phone reports it; the engine stays under pressure from the
// phone from the first Medium until Low, and the pressure is serious while
// the level is High or above. What it costs to be told often is bounded
// here, not in the shell.
extern "C" __declspec(dllexport) void gecko_w10m_memory_pressure(
    int32_t aLevel) {
  const int32_t level = aLevel < 0 ? 0 : aLevel > 3 ? 3 : aLevel;
  const int32_t was = sShellLevel.exchange(level);
  if (level >= 1) {
    // Only when it changes between tight and critical: near the ceiling the
    // phone goes between High and over the limit many times a second. And
    // not for ever: a session can sit on the line between Medium and High.
    static Atomic<uint32_t> sLevelNotes{0};
    if ((was == 0 || (level >= 2) != (was >= 2)) &&
        sLevelNotes++ < kPressureNoteCap) {
      GeckoW10mNote(level >= 2 ? "mem: the phone says memory is CRITICAL"
                               : "mem: the phone says memory is tight");
    }
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
  // A fall only changes what the pressure is; a rise, or the same level said
  // again, asks for the caches to go.
  if (level >= 1 && level >= was) {
    sShellAsked = true;
  }
  RequestEvaluate();
}

// From gecko_w10m_app_state, on the shell's thread, the moment the app is
// told it is going to the background.
extern "C" void GeckoW10mMemoryBackground() {
  uint32_t token = ++sAwayCount;
  if (!token) {
    token = ++sAwayCount;
  }
  sAwayToken = token;
}

// On the shell's thread again, the moment it is told it is back: which going
// that answers.
extern "C" uint32_t GeckoW10mMemoryAwayToken() { return sAwayToken; }

// And on the main thread, some time after that, with that number. The clocks
// start again from here, and the timer with them -- unless the app has gone
// to the background again meanwhile, in which case it is still away.
extern "C" void GeckoW10mMemoryForeground(uint32_t aToken) {
  MOZ_ASSERT(NS_IsMainThread());
  if (sShutdown || !sStarted) {
    sAwayToken.compareExchange(aToken, 0);
    return;
  }
  // Before it stops being away, so that an evaluation cannot run with the old
  // clocks in between.
  RestartClocks();
  if (aToken && sAwayToken.compareExchange(aToken, 0)) {
    Evaluate();
  }
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
