/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Runtime.h"

#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/ThreadLocal.h"

#include "jsfriendapi.h"

#include "builtin/String.h"
#include "frontend/CompilationStencil.h"
#include "frontend/ParserAtom.h"  // frontend::WellKnownParserAtoms
#include "gc/GC.h"
#include "gc/PublicIterators.h"
#include "jit/IonCompileTask.h"
#include "jit/JitOptions.h"  // js::fuzzingSafe
#include "jit/JitRuntime.h"
#include "jit/Simulator.h"
#include "js/AllocationLogging.h"  // JS_COUNT_CTOR, JS_COUNT_DTOR
#include "js/experimental/JSStencil.h"
#include "js/experimental/SourceHook.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/Interrupt.h"
#include "js/MemoryMetrics.h"
#include "js/Stack.h"  // JS::NativeStackLimitMin
#include "js/Wrapper.h"
#include "js/WrapperCallbacks.h"
#include "util/DefaultLocale.h"
#include "util/RandomSeed.h"
#include "vm/DateTime.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/PromiseObject.h"  // js::PromiseObject
#include "vm/SharedImmutableStringsCache.h"
#include "vm/Warnings.h"  // js::WarnNumberUC
#include "wasm/WasmPI.h"
#include "wasm/WasmSignalHandlers.h"

#include "debugger/DebugAPI-inl.h"
#include "gc/ArenaList-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/Realm-inl.h"

using namespace js;

using mozilla::Atomic;
using mozilla::DebugOnly;

/* static */ MOZ_THREAD_LOCAL(JSContext*) js::TlsContext;
/* static */
Atomic<size_t> JSRuntime::liveRuntimesCount;
Atomic<JS::LargeAllocationFailureCallback> js::OnLargeAllocationFailure;

JS::FilenameValidationCallback js::gFilenameValidationCallback = nullptr;

namespace js {

#ifndef __wasi__
bool gCanUseExtraThreads = true;
#else
bool gCanUseExtraThreads = false;
#endif
}  // namespace js

void js::DisableExtraThreads() { gCanUseExtraThreads = false; }

const JSSecurityCallbacks js::NullSecurityCallbacks = {};

static const JSWrapObjectCallbacks DefaultWrapObjectCallbacks = {
    TransparentObjectWrapper, nullptr};

extern bool DefaultHostEnsureCanAddPrivateElementCallback(JSContext* cx,
                                                          HandleValue val);

static size_t ReturnZeroSize(const void* p) { return 0; }

JSRuntime::JSRuntime(JSRuntime* parentRuntime)
    : parentRuntime(parentRuntime),
#ifdef DEBUG
      updateChildRuntimeCount(parentRuntime),
      initialized_(false),
#endif
      mainContext_(nullptr),
      profilerSampleBufferRangeStart_(0),
      telemetryCallback(nullptr),
      consumeStreamCallback(nullptr),
      reportStreamErrorCallback(nullptr),
      hadOutOfMemory(false),
      allowRelazificationForTesting(false),
      sizeOfIncludingThisCompartmentCallback(nullptr),
      realmNameCallback(nullptr),
      securityCallbacks(&NullSecurityCallbacks),
      DOMcallbacks(nullptr),
      destroyPrincipals(nullptr),
      readPrincipals(nullptr),
      canAddPrivateElement(&DefaultHostEnsureCanAddPrivateElementCallback),
      warningReporter(nullptr),
      geckoProfiler_(thisFromCtor()),
      trustedPrincipals_(nullptr),
      wrapObjectCallbacks(&DefaultWrapObjectCallbacks),
      preserveWrapperCallback(nullptr),
      scriptEnvironmentPreparer(nullptr),
      ctypesActivityCallback(nullptr),
      windowProxyClass_(nullptr),
      numRealms(0),
      numDebuggeeRealms_(0),
      numDebuggeeRealmsObservingCoverage_(0),
      localeCallbacks(nullptr),
      defaultLocale(LanguageId::und()),
      profilingScripts(false),
      scriptAndCountsVector(nullptr),
      watchtowerTestingLog(nullptr),
      jitRuntime_(nullptr),
      gc(thisFromCtor()),
      emptyString(nullptr),
#if !JS_HAS_INTL_API
      thousandsSeparator(nullptr),
      decimalSeparator(nullptr),
      numGrouping(nullptr),
#endif
      beingDestroyed_(false),
      allowContentJS_(true),
      atoms_(nullptr),
      permanentAtoms_(nullptr),
      staticStrings(nullptr),
      commonNames(nullptr),
      wellKnownSymbols(nullptr),
      scriptDataTableHolder_(SharedScriptDataTableHolder::NeedsLock::No),
      liveSABs(0),
      beforeWaitCallback(nullptr),
      afterWaitCallback(nullptr),
      offthreadBaselineCompilationEnabled_(false),
      offthreadIonCompilationEnabled_(true),
      autoWritableJitCodeActive_(false),
      oomCallback(nullptr),
      debuggerMallocSizeOf(ReturnZeroSize),
      stackFormat_(parentRuntime ? js::StackFormat::Default
                                 : js::StackFormat::SpiderMonkey),
      wasmInstances(mutexid::WasmRuntimeInstances),
      moduleAsyncEvaluatingPostOrder(0),
      pendingAsyncModuleEvaluations(0) {
  JS_COUNT_CTOR(JSRuntime);
  liveRuntimesCount++;

#ifndef __wasi__
  // See function comment for why we call this now, not in JS_Init().
  wasm::EnsureEagerProcessSignalHandlers();
#endif  // __wasi__
}

JSRuntime::~JSRuntime() {
  JS_COUNT_DTOR(JSRuntime);
  MOZ_ASSERT(!initialized_);

  DebugOnly<size_t> oldCount = liveRuntimesCount--;
  MOZ_ASSERT(oldCount > 0);

  MOZ_ASSERT(wasmInstances.lock()->empty());

  MOZ_ASSERT(numRealms == 0);
  MOZ_ASSERT(numDebuggeeRealms_ == 0);
  MOZ_ASSERT(numDebuggeeRealmsObservingCoverage_ == 0);
}

bool JSRuntime::init(JSContext* cx, uint32_t maxbytes) {
#ifdef DEBUG
  MOZ_ASSERT(!initialized_);
  initialized_ = true;
#endif

  if (CanUseExtraThreads() && !EnsureHelperThreadsInitialized()) {
    return false;
  }

  mainContext_ = cx;

  if (!gc.init(maxbytes)) {
    return false;
  }

  if (!InitRuntimeNumberState(this)) {
    return false;
  }

  // As a hack, we clear our timezone cache every time we create a new runtime.
  // Also see the comment in JS::Realm::init().
  js::ResetTimeZoneInternal(ResetTimeZoneMode::DontResetIfOffsetUnchanged);

  caches().megamorphicSetPropCache = MakeUnique<MegamorphicSetPropCache>();
  if (!caches().megamorphicSetPropCache) {
    return false;
  }

  return true;
}

void JSRuntime::destroyRuntime() {
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy());
  MOZ_ASSERT(childRuntimeCount == 0);
  MOZ_ASSERT(initialized_);

#ifdef JS_HAS_INTL_API
  sharedIntlData.ref().destroyInstance();
#endif

  watchtowerTestingLog.ref().reset();

  if (gc.wasInitialized()) {
    /*
     * Finish any in-progress GCs first.
     */
    JSContext* cx = mainContextFromOwnThread();
    if (JS::IsIncrementalGCInProgress(cx)) {
      gc::FinishGC(cx);
    }

    /* Free source hook early, as its destructor may want to delete roots. */
    sourceHook = nullptr;

    /*
     * Cancel any pending, in progress or completed baseline/Ion compilations
     * and parse tasks. Waiting for wasm and compression tasks is done
     * synchronously (on the main thread or during parse tasks), so no
     * explicit canceling is needed for these.
     */
    CancelOffThreadCompile(this);
    CancelOffThreadDelazify(this);
    CancelOffThreadCompressions(this);

    /* Wait for GC tasks to finish, including background allocation. */
    gc.waitForBackgroundTasks();

    /*
     * Flag us as being destroyed. This allows the GC to free things like
     * interned atoms and Ion trampolines.
     */
    beingDestroyed_ = true;

    /* Remove persistent GC roots. */
    gc.finishRoots();

    /* Allow the GC to release scripts that were being profiled. */
    profilingScripts = false;

    JS::PrepareForFullGC(cx);
    gc.gc(JS::GCOptions::Shutdown, JS::GCReason::DESTROY_RUNTIME);
  }

  AutoNoteSingleThreadedRegion anstr;

  MOZ_ASSERT(scriptDataTableHolder().getWithoutLock().empty());

#if !JS_HAS_INTL_API
  FinishRuntimeNumberState(this);
#endif

  gc.finish();

  for (auto [f, data] : cleanupClosures.ref()) {
    f(data);
  }
  cleanupClosures.ref().clear();

  js_delete(jitRuntime_.ref());

#ifdef DEBUG
  initialized_ = false;
#endif
}

void JSRuntime::addTelemetry(JSMetric id, const JSTelemetryData& sample) {
  if (telemetryCallback) {
    (*telemetryCallback)(id, sample);
  }
}

void JSRuntime::setTelemetryCallback(
    JSRuntime* rt, JSAccumulateTelemetryDataCallback callback) {
  rt->telemetryCallback = callback;
}

void JSRuntime::setUseCounter(JSObject* obj, JSUseCounter counter) {
  if (useCounterCallback) {
    // A use counter callback cannot GC.
    JS::AutoSuppressGCAnalysis suppress;
    (*useCounterCallback)(obj, counter);
  }
}

void JSRuntime::setUseCounterCallback(JSRuntime* rt,
                                      JSSetUseCounterCallback callback) {
  rt->useCounterCallback = callback;
}

void JSRuntime::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                       JS::RuntimeSizes* rtSizes) {
  rtSizes->object += mallocSizeOf(this);

  rtSizes->atomsTable += atoms().sizeOfIncludingThis(mallocSizeOf);
  rtSizes->gc.marker += gc.markers.sizeOfExcludingThis(mallocSizeOf);
  for (auto& marker : gc.markers) {
    rtSizes->gc.marker += marker->sizeOfIncludingThis(mallocSizeOf);
  }

  if (!parentRuntime) {
    rtSizes->atomsTable += mallocSizeOf(staticStrings);
    rtSizes->atomsTable += mallocSizeOf(commonNames);
    rtSizes->atomsTable += permanentAtoms()->sizeOfIncludingThis(mallocSizeOf);

    rtSizes->selfHostStencil =
        selfHostStencilInput_->sizeOfIncludingThis(mallocSizeOf) +
        selfHostStencil_->sizeOfIncludingThis(mallocSizeOf) +
        selfHostScriptMap.ref().shallowSizeOfExcludingThis(mallocSizeOf);
  }

  JSContext* cx = mainContextFromAnyThread();
  rtSizes->contexts += cx->sizeOfIncludingThis(mallocSizeOf);
  rtSizes->temporary += cx->tempLifoAlloc().sizeOfExcludingThis(mallocSizeOf);
  rtSizes->interpreterStack +=
      cx->interpreterStack().sizeOfExcludingThis(mallocSizeOf);
  rtSizes->uncompressedSourceCache +=
      caches().uncompressedSourceCache.sizeOfExcludingThis(mallocSizeOf);

  rtSizes->gc.nurseryCommitted += gc.nursery().totalCommitted();
  rtSizes->gc.nurseryMallocedBuffers +=
      gc.nursery().sizeOfMallocedBuffers(mallocSizeOf);
  gc.storeBuffer().addSizeOfExcludingThis(mallocSizeOf, &rtSizes->gc);

  if (isMainRuntime()) {
    rtSizes->sharedImmutableStringsCache +=
        js::SharedImmutableStringsCache::getSingleton().sizeOfExcludingThis(
            mallocSizeOf);
    rtSizes->atomsTable +=
        js::frontend::WellKnownParserAtoms::getSingleton().sizeOfExcludingThis(
            mallocSizeOf);
  }

#ifdef JS_HAS_INTL_API
  rtSizes->sharedIntlData +=
      sharedIntlData.ref().sizeOfExcludingThis(mallocSizeOf);
#endif

  {
    auto& table = scriptDataTableHolder().getWithoutLock();

    rtSizes->scriptData += table.shallowSizeOfExcludingThis(mallocSizeOf);
    for (auto iter = table.iter(); !iter.done(); iter.next()) {
      rtSizes->scriptData += iter.get()->sizeOfIncludingThis(mallocSizeOf);
    }
  }

  if (isMainRuntime()) {
    AutoLockGlobalScriptData lock;

    auto& table = js::globalSharedScriptDataTableHolder.get(lock);

    rtSizes->scriptData += table.shallowSizeOfExcludingThis(mallocSizeOf);
    for (auto iter = table.iter(); !iter.done(); iter.next()) {
      rtSizes->scriptData += iter.get()->sizeOfIncludingThis(mallocSizeOf);
    }
  }

  if (jitRuntime_) {
    // Sizes of the IonCompileTasks we are holding for lazy linking
    for (auto* task : jitRuntime_->ionLazyLinkList(this)) {
      rtSizes->jitLazyLink += task->sizeOfExcludingThis(mallocSizeOf);
    }
  }

  rtSizes->wasmRuntime +=
      wasmInstances.lock()->shallowSizeOfExcludingThis(mallocSizeOf);

#ifdef ENABLE_WASM_JSPI
  rtSizes->wasmContStacks +=
      mainContextFromAnyThread()->wasm().contStacks().sizeOfNonHeap();
#endif
}

static bool InvokeInterruptCallbacks(JSContext* cx) {
  bool stop = false;
  for (JSInterruptCallback cb : cx->interruptCallbacks()) {
    if (!cb(cx)) {
      stop = true;
    }
  }
  return stop;
}

static bool HandleInterrupt(JSContext* cx, bool invokeCallback,
                            bool oomStackTrace) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  cx->runtime()->gc.gcIfRequested();

  if (oomStackTrace) {
    // Capture OOM stack trace this way because we don't have memory to handle
    // it the way ComputeStackString does.
    cx->captureOOMStackTrace();
  } else {
    // We can handle OOM interrupts while handling exceptions, when it isn't
    // safe to attach finished compilations

    // A worker thread may have requested an interrupt after finishing an
    // offthread compilation.
    jit::AttachFinishedCompilations(cx);
  }

  // Don't call the interrupt callback if we only interrupted for GC, Ion, or
  // OOM.
  if (!invokeCallback) {
    return true;
  }

  // Important: Additional callbacks can occur inside the callback handler
  // if it re-enters the JS engine. The embedding must ensure that the
  // callback is disconnected before attempting such re-entry.
  if (cx->interruptCallbackDisabled) {
    return true;
  }

  if (!InvokeInterruptCallbacks(cx)) {
    // Debugger treats invoking the interrupt callback as a "step", so
    // invoke the onStep handler. Skip this in --fuzzing-safe mode because
    // fuzzer-generated onStep handlers can mutate state that native/builtin
    // code (e.g. TypedArrayJoinKernel) assumes is stable.
    if (cx->realm()->isDebuggee() && !fuzzingSafe) {
      ScriptFrameIter iter(cx);
      MOZ_ASSERT_IF(!iter.done(), !iter.isResumingGenerator());
      if (!iter.done() && cx->compartment() == iter.compartment() &&
          DebugAPI::stepModeEnabled(iter.script())) {
        if (!DebugAPI::onSingleStep(cx)) {
          return false;
        }
      }
    }

    return true;
  }

  // No need to set aside any pending exception here: ComputeStackString
  // already does that.
  JSString* stack = ComputeStackString(cx);

  UniqueTwoByteChars stringChars;
  if (stack) {
    stringChars = JS_CopyStringCharsZ(cx, stack);
    if (!stringChars) {
      cx->recoverFromOutOfMemory();
    }
  }

  const char16_t* chars;
  if (stringChars) {
    chars = stringChars.get();
  } else {
    chars = u"(stack not available)";
  }
  WarnNumberUC(cx, JSMSG_TERMINATED, chars);
  cx->reportUncatchableException();
  return false;
}

void JSContext::requestInterrupt(InterruptReason reason) {
  interruptBits_ |= uint32_t(reason);
  jitStackLimit = JS::NativeStackLimitMin;

  if (reason == InterruptReason::CallbackUrgent) {
    // If this interrupt is urgent (slow script dialog for instance), take
    // additional steps to interrupt corner cases where the above fields are
    // not regularly polled.
    FutexThread::lock();
    if (fx.isWaiting()) {
      fx.notify(FutexThread::NotifyForJSInterrupt);
    }
    fx.unlock();
  }

  if (reason == InterruptReason::CallbackUrgent ||
      reason == InterruptReason::MajorGC ||
      reason == InterruptReason::MinorGC) {
    wasm::InterruptRunningCode(this);
  }
}

bool JSContext::handleInterrupt() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));
  if (hasAnyPendingInterrupt() || jitStackLimit == JS::NativeStackLimitMin) {
    bool invokeCallback =
        hasPendingInterrupt(InterruptReason::CallbackUrgent) ||
        hasPendingInterrupt(InterruptReason::CallbackCanWait);
    bool oomStackTrace = hasPendingInterrupt(InterruptReason::OOMStackTrace);
    interruptBits_ = 0;
    resetJitStackLimit();
    return HandleInterrupt(this, invokeCallback, oomStackTrace);
  }
  return true;
}

bool JSContext::handleInterruptNoCallbacks() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));
  if (hasAnyPendingInterrupt() || jitStackLimit == JS::NativeStackLimitMin) {
    bool oomStackTrace = hasPendingInterrupt(InterruptReason::OOMStackTrace);
    clearPendingInterrupt(js::InterruptReason::OOMStackTrace);
    if (!hasAnyPendingInterrupt()) {
      resetJitStackLimit();
    }
    return HandleInterrupt(this, false, oomStackTrace);
  }
  return true;
}

void JSContext::clearPendingInterrupt(js::InterruptReason reason) {
  // Interrupt bit have already been cleared.
  interruptBits_ &= ~uint32_t(reason);
}

void JSRuntime::setDefaultLocale(LanguageId locale) {
  // Replace "und" with "und-Zzzz-ZZ" to mark the locale as resolved.
  //
  // "und-Zzzz-ZZ" is an undetermined language with unknown script and region.
  if (locale == LanguageId::und()) {
    locale = LanguageId::fromValidBcp49("und-Zzzz-ZZ");
  }

#if JS_HAS_INTL_API
  if (!LocaleHasDefaultCaseMapping(locale)) {
    runtimeFuses.ref().defaultLocaleHasDefaultCaseMappingFuse.popFuse(
        mainContextFromOwnThread());
  }
#endif

  defaultLocale = locale;
}

bool JSRuntime::setDefaultLocale(const char* locale) {
  if (!locale) {
    return false;
  }

  setDefaultLocale(DefaultLocaleFrom(locale));
  return true;
}

void JSRuntime::resetDefaultLocale() { defaultLocale = LanguageId::und(); }

LanguageId JSRuntime::getDefaultLocale() {
  auto locale = defaultLocale.ref();
  if (locale == LanguageId::und()) {
    locale = SystemDefaultLocale();
    setDefaultLocale(locale);
  }
  return locale;
}

#ifdef JS_HAS_INTL_API
void JSRuntime::traceSharedIntlData(JSTracer* trc) {
  sharedIntlData.ref().trace(trc);
}
#endif

SharedScriptDataTableHolder& JSRuntime::scriptDataTableHolder() {
  return scriptDataTableHolder_;
}

bool JSRuntime::getHostDefinedData(
    JSContext* cx, JS::MutableHandle<JSObject*> incumbentGlobal,
    JS::MutableHandle<JSObject*> optionalHostDefinedData) const {
  MOZ_ASSERT(cx->jobQueue);

  if (!cx->jobQueue->getHostDefinedData(cx, incumbentGlobal,
                                        optionalHostDefinedData)) {
    return false;
  }

  // incumbentGlobal is returned unwrapped. Callers will convert this to its
  // Object.prototype representative and then wrap that into cx's compartment.
  // It must therefore be a raw GlobalObject (or null), not a CCW.
  //
  // The intuition here would be that you would want to instead have a
  // MutableHandle<GlobalObject*> outparam, however to reduce rooting
  // costs, having two type-incompatible roots is probably the worse
  // performance choice, and so instead we keep JSObject, as the
  // final wrapped representative will have type JSObject.
  MOZ_ASSERT_IF(incumbentGlobal, incumbentGlobal->is<GlobalObject>());

  // optionalHostDefined data on the other hand should be same compartment,
  // but will be wrapped as necessary.
  cx->check(optionalHostDefinedData);
  return true;
}

JS_PUBLIC_API JSObject*
JS::MaybeGetPromiseAllocationSiteFromPossiblyWrappedPromise(
    HandleObject promise) {
  if (!promise) {
    return nullptr;
  }

  JSObject* unwrappedPromise = promise;
  // While the job object is guaranteed to be unwrapped, the promise
  // might be wrapped. See the comments in EnqueuePromiseReactionJob in
  // builtin/Promise.cpp for details.
  if (IsWrapper(promise)) {
    unwrappedPromise = UncheckedUnwrap(promise);
  }
  if (unwrappedPromise->is<PromiseObject>()) {
    return unwrappedPromise->as<PromiseObject>().allocationSite();
  }
  return nullptr;
}

void JSRuntime::addUnhandledRejectedPromise(JSContext* cx,
                                            js::HandleObject promise) {
  MOZ_ASSERT(promise->is<PromiseObject>());
  if (!cx->promiseRejectionTrackerCallback) {
    return;
  }

  bool mutedErrors = false;
  if (JSScript* script = cx->currentScript()) {
    mutedErrors = script->mutedErrors();
  }

  void* data = cx->promiseRejectionTrackerCallbackData;
  cx->promiseRejectionTrackerCallback(
      cx, mutedErrors, promise, JS::PromiseRejectionHandlingState::Unhandled,
      data);
}

void JSRuntime::removeUnhandledRejectedPromise(JSContext* cx,
                                               js::HandleObject promise) {
  MOZ_ASSERT(promise->is<PromiseObject>());
  if (!cx->promiseRejectionTrackerCallback) {
    return;
  }

  bool mutedErrors = false;
  if (JSScript* script = cx->currentScript()) {
    mutedErrors = script->mutedErrors();
  }

  void* data = cx->promiseRejectionTrackerCallbackData;
  cx->promiseRejectionTrackerCallback(
      cx, mutedErrors, promise, JS::PromiseRejectionHandlingState::Handled,
      data);
}

mozilla::non_crypto::XorShift128PlusRNG& JSRuntime::randomKeyGenerator() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(this));
  if (randomKeyGenerator_.isNothing()) {
    mozilla::Array<uint64_t, 2> seed;
    GenerateXorShift128PlusSeed(seed);
    randomKeyGenerator_.emplace(seed[0], seed[1]);
  }
  return randomKeyGenerator_.ref();
}

mozilla::non_crypto::XorShift128PlusRNG JSRuntime::forkRandomKeyGenerator() {
  auto& rng = randomKeyGenerator();
  return mozilla::non_crypto::XorShift128PlusRNG(rng.next(), rng.next());
}

js::HashNumber JSRuntime::randomHashCode() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(this));

  if (randomHashCodeGenerator_.isNothing()) {
    mozilla::Array<uint64_t, 2> seed;
    GenerateXorShift128PlusSeed(seed);
    randomHashCodeGenerator_.emplace(seed[0], seed[1]);
  }

  return HashNumber(randomHashCodeGenerator_->next());
}

#if defined(GECKO_W10M)
#  include <algorithm>

#  include "jsapi.h"
#  include "mozilla/Sprintf.h"
#  include "util/WindowsWrapper.h"
extern "C" void GeckoW10mNoteAddressSpaceOnFailure(const char* aWhy);
extern "C" void GeckoW10mMemoryPressureOnFailure(const char* aWhy);
extern "C" void GeckoW10mNote(const char* text);
extern "C" IMAGE_DOS_HEADER __ImageBase;

// An allocation the engine needed failed, and failed again after empty GC
// chunks were given back. On the 32-bit phone that is the address space in
// pieces more often than memory used up -- one log had 890 MB free and a
// failure all the same. The size, the free address space and its largest
// piece say which; the caller's address, run through tools/symbolize.py,
// says who asked.
static void GeckoW10mNoteMallocFailure(JSRuntime* aRuntime,
                                       AllocFunction aFunc, size_t aBytes,
                                       void* aCaller, JSContext* aCx) {
  static mozilla::Atomic<uint32_t> sNotes{0};
  const uint32_t drawn = sNotes++;
  if (drawn >= 8) {
    return;
  }
  size_t freeBytes = 0, largest = 0, pieces = 0;
  MEMORY_BASIC_INFORMATION info;
  uintptr_t at = 0x10000;
  while (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) ==
         sizeof(info)) {
    if (info.State == MEM_FREE) {
      freeBytes += info.RegionSize;
      largest = std::max(largest, size_t(info.RegionSize));
      ++pieces;
    }
    const uintptr_t next = uintptr_t(info.BaseAddress) + info.RegionSize;
    if (next <= at) {
      break;
    }
    at = next;
  }
  char note[256];
  SprintfLiteral(note,
                 "js: %s of %zu bytes failed (%s runtime); address space %zu "
                 "MB free in %zu pieces, the largest %zu MB; asked from "
                 "xul.dll+0x%08x",
                 aFunc == AllocFunction::Malloc   ? "malloc"
                 : aFunc == AllocFunction::Calloc ? "calloc"
                                                  : "realloc",
                 aBytes, aRuntime->isMainRuntime() ? "main" : "worker",
                 freeBytes >> 20, pieces, largest >> 20,
                 unsigned((uintptr_t(aCaller) & ~uintptr_t(1)) -
                          uintptr_t(&__ImageBase)));
  GeckoW10mNote(note);
  // The first one: what the address space was made of when it happened. And
  // then the engine is asked to let go -- a fresh "low-memory", a shrinking
  // GC and a cycle collection -- which nothing asked for before: a run that
  // had been told about memory once already only ever heard "ongoing" again,
  // and the JS engine ignores that
  // (widget/headless/GeckoW10mMemoryPressure.cpp). After the map, so that
  // the map is of the moment it failed.
  if (drawn == 0) {
    GeckoW10mNoteAddressSpaceOnFailure("a JS allocation failed");
    GeckoW10mMemoryPressureOnFailure("a JS allocation failed");
  }

  // The direct caller is a vector's allocation policy, which says nothing.
  // The script running at the time does -- a page's own, or the browser's --
  // and so do the native callers further up, for tools/symbolize.py. The
  // stack walker is looked up, not imported: an import the phone lacks
  // would stop xul.dll from loading at all.
  if (aCx && js::CurrentThreadCanAccessRuntime(aRuntime) && !aCx->realm()) {
    // Atoms are made outside any realm, and then no caller can be named.
    GeckoW10mNote("js: it was in the atoms zone, no script can be named");
  } else if (aCx && js::CurrentThreadCanAccessRuntime(aRuntime)) {
    JS::AutoFilename file;
    uint32_t line = 0;
    JS::ColumnNumberOneOrigin column;
    if (JS::DescribeScriptedCaller(&file, aCx, &line, &column) && file.get()) {
      char where[200];
      SprintfLiteral(where, "%s", file.get());
      // No query or fragment: the log is a file people send.
      for (char* c = where; *c; ++c) {
        if (*c == '?' || *c == '#') {
          *c = 0;
          break;
        }
      }
      SprintfLiteral(note, "js: the script running then: %s:%u:%u", where,
                     line, column.oneOriginValue());
      GeckoW10mNote(note);
    } else {
      GeckoW10mNote("js: no script was running then");
    }
  }
  using CaptureFn = USHORT(WINAPI*)(ULONG, ULONG, PVOID*, PULONG);
  static const auto capture = reinterpret_cast<CaptureFn>(::GetProcAddress(
      ::GetModuleHandleW(L"ntdll.dll"), "RtlCaptureStackBackTrace"));
  if (capture) {
    void* frames[24];
    const USHORT count = capture(1, 24, frames, nullptr);
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(&__ImageBase) + __ImageBase.e_lfanew);
    const uintptr_t begin = uintptr_t(&__ImageBase);
    const uintptr_t end = begin + headers->OptionalHeader.SizeOfImage;
    int length = SprintfLiteral(note, "js: native callers (%u frames):", count);
    int listed = 0;
    for (USHORT i = 0; i < count && listed < 10 && length > 0 &&
                       length < int(sizeof(note)) - 16;
         ++i) {
      const uintptr_t pc = uintptr_t(frames[i]) & ~uintptr_t(1);
      if (pc >= begin && pc < end) {
        length += snprintf(note + length, sizeof(note) - length, " +0x%x",
                           unsigned(pc - begin));
        ++listed;
      }
    }
    GeckoW10mNote(note);
  }
}
#endif

JS_PUBLIC_API void* JSRuntime::onOutOfMemory(AllocFunction allocFunc,
                                             arena_id_t arena, size_t nbytes,
                                             void* reallocPtr,
                                             JSContext* maybecx) {
  MOZ_ASSERT_IF(allocFunc != AllocFunction::Realloc, !reallocPtr);

  if (JS::RuntimeHeapIsBusy()) {
    return nullptr;
  }

  if (!oom::IsSimulatedOOMAllocation()) {
    /*
     * Retry when we are done with the background sweeping and have stopped
     * all the allocations and released the empty GC chunks.
     */
    gc.onOutOfMallocMemory();
    void* p;
    switch (allocFunc) {
      case AllocFunction::Malloc:
        p = js_arena_malloc(arena, nbytes);
        break;
      case AllocFunction::Calloc:
        p = js_arena_calloc(arena, nbytes, 1);
        break;
      case AllocFunction::Realloc:
        p = js_arena_realloc(arena, reallocPtr, nbytes);
        break;
      default:
        MOZ_CRASH();
    }
    if (p) {
      return p;
    }
#if defined(GECKO_W10M)
    GeckoW10mNoteMallocFailure(this, allocFunc, nbytes,
                               __builtin_return_address(0), maybecx);
#endif
  }

  if (maybecx) {
    ReportOutOfMemory(maybecx);
  }
  return nullptr;
}

void* JSRuntime::onOutOfMemoryCanGC(AllocFunction allocFunc, arena_id_t arena,
                                    size_t bytes, void* reallocPtr) {
  if (OnLargeAllocationFailure && bytes >= LARGE_ALLOCATION) {
    OnLargeAllocationFailure();
  }
  return onOutOfMemory(allocFunc, arena, bytes, reallocPtr);
}

bool JSRuntime::activeGCInAtomsZone() {
  Zone* zone = unsafeAtomsZone();
  return (zone->needsMarkingBarrier() && !gc.isVerifyPreBarriersEnabled()) ||
         zone->wasGCStarted();
}

void JSRuntime::commitPendingWrapperPreservations() {
  for (NonAtomZonesIter zone(this); !zone.done(); zone.next()) {
    commitPendingWrapperPreservations(zone);
  }
}

void JSRuntime::commitPendingWrapperPreservations(JS::Zone* zone) {
  for (JSObject* wrapper : zone->slurpPendingWrapperPreservations()) {
    MOZ_RELEASE_ASSERT(!IsWrapper(wrapper));

    JS::Value objectWrapperSlot =
        JS::GetReservedSlot(wrapper, JS_OBJECT_WRAPPER_SLOT);
    // This mirrors logic in MaybePreserveDOMWrapper, and should be kept in
    // sync with that.
    if (objectWrapperSlot.isUndefined() || !objectWrapperSlot.toPrivate()) {
      continue;
    }

    Rooted<JSObject*> rooted(mainContextFromOwnThread(), wrapper);
    preserveWrapperCallback(mainContextFromOwnThread(), rooted);
  }

  // The callback must not cause more wrappers to be preserved or they will
  // overwrite the buffer we're processing in the loop above.
  MOZ_ASSERT(!zone->hasPendingWrapperPreservations());
}

void JSRuntime::incrementNumDebuggeeRealms() {
  if (numDebuggeeRealms_ == 0) {
    jitRuntime()->baselineInterpreter().toggleDebuggerInstrumentation(true);
  }

  numDebuggeeRealms_++;
  MOZ_ASSERT(numDebuggeeRealms_ <= numRealms);
}

void JSRuntime::decrementNumDebuggeeRealms() {
  MOZ_ASSERT(numDebuggeeRealms_ > 0);
  numDebuggeeRealms_--;

  // Note: if we had shutdown leaks we can end up here while destroying the
  // runtime. It's not safe to access JitRuntime trampolines because they're no
  // longer traced.
  if (numDebuggeeRealms_ == 0 && !isBeingDestroyed()) {
    jitRuntime()->baselineInterpreter().toggleDebuggerInstrumentation(false);
  }
}

void JSRuntime::incrementNumDebuggeeRealmsObservingCoverage() {
  if (numDebuggeeRealmsObservingCoverage_ == 0) {
    jit::BaselineInterpreter& interp = jitRuntime()->baselineInterpreter();
    interp.toggleCodeCoverageInstrumentation(true);
  }

  numDebuggeeRealmsObservingCoverage_++;
  MOZ_ASSERT(numDebuggeeRealmsObservingCoverage_ <= numRealms);
}

void JSRuntime::decrementNumDebuggeeRealmsObservingCoverage() {
  MOZ_ASSERT(numDebuggeeRealmsObservingCoverage_ > 0);
  numDebuggeeRealmsObservingCoverage_--;

  // Note: if we had shutdown leaks we can end up here while destroying the
  // runtime. It's not safe to access JitRuntime trampolines because they're no
  // longer traced.
  if (numDebuggeeRealmsObservingCoverage_ == 0 && !isBeingDestroyed()) {
    jit::BaselineInterpreter& interp = jitRuntime()->baselineInterpreter();
    interp.toggleCodeCoverageInstrumentation(false);
  }
}

bool js::CurrentThreadCanAccessRuntime(const JSRuntime* rt) {
  return rt->mainContextFromAnyThread() == TlsContext.get();
}

bool js::CurrentThreadCanAccessZone(Zone* zone) {
  return CurrentThreadCanAccessRuntime(zone->runtime_);
}

#ifdef DEBUG
bool js::CurrentThreadIsMainThread() { return !!TlsContext.get(); }
#endif

JS_PUBLIC_API void JS::SetJSContextProfilerSampleBufferRangeStart(
    JSContext* cx, uint64_t rangeStart) {
  cx->runtime()->setProfilerSampleBufferRangeStart(rangeStart);
}

JS_PUBLIC_API bool JS::IsProfilingEnabledForContext(JSContext* cx) {
  MOZ_ASSERT(cx);
  return cx->runtime()->geckoProfiler().enabled();
}

JS_PUBLIC_API void JS::EnableRecordingAllocations(
    JSContext* cx, JS::RecordAllocationsCallback callback, double probability) {
  MOZ_ASSERT(cx);
  cx->runtime()->startRecordingAllocations(probability, callback);
}

JS_PUBLIC_API void JS::DisableRecordingAllocations(JSContext* cx) {
  MOZ_ASSERT(cx);
  cx->runtime()->stopRecordingAllocations();
}

JS_PUBLIC_API void JS::shadow::RegisterWeakCache(
    JSRuntime* rt, detail::WeakCacheBase* cachep) {
  rt->gc.registerWeakCache(cachep);
}

void JSRuntime::startRecordingAllocations(
    double probability, JS::RecordAllocationsCallback callback) {
  allocationSamplingProbability = probability;
  recordAllocationCallback = callback;

  // Go through all of the existing realms, and turn on allocation tracking.
  for (RealmsIter realm(this); !realm.done(); realm.next()) {
    realm->setAllocationMetadataBuilder(&SavedStacks::metadataBuilder);
    realm->chooseAllocationSamplingProbability();
  }
}

void JSRuntime::stopRecordingAllocations() {
  recordAllocationCallback = nullptr;
  // Go through all of the existing realms, and turn on allocation tracking.
  for (RealmsIter realm(this); !realm.done(); realm.next()) {
    js::GlobalObject* global = realm->maybeGlobal();
    if (!realm->isDebuggee() || !global ||
        !DebugAPI::isObservedByDebuggerTrackingAllocations(*global)) {
      // Only remove the allocation metadata builder if no Debuggers are
      // tracking allocations.
      realm->forgetAllocationMetadataBuilder();
    }
  }
}

// This function can run to ensure that when new realms are created
// they have allocation logging turned on.
void JSRuntime::ensureRealmIsRecordingAllocations(
    Handle<GlobalObject*> global) {
  if (recordAllocationCallback) {
    if (!global->realm()->isRecordingAllocations()) {
      // This is a new realm, turn on allocations for it.
      global->realm()->setAllocationMetadataBuilder(
          &SavedStacks::metadataBuilder);
    }
    // Ensure the probability is up to date with the current combination of
    // debuggers and runtime profiling.
    global->realm()->chooseAllocationSamplingProbability();
  }
}
