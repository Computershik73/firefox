/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsSegmentedBuffer.h"

#include "nsNetCID.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#ifdef GECKO_W10M
#  include "mozilla/Atomics.h"
#  include "nsIMemoryReporter.h"
namespace {
mozilla::Atomic<uint64_t> sSegmentOwnedBytes{0};
mozilla::Atomic<uint64_t> sSegmentPendingFreeBytes{0};
MOZ_DEFINE_MALLOC_SIZE_OF(GeckoW10mSegmentSizeOf)
}  // namespace
extern "C" void GeckoW10mGetSegmentedBufferBytes(uint64_t* owned,
                                                 uint64_t* pending) {
  *owned = sSegmentOwnedBytes;
  *pending = sSegmentPendingFreeBytes;
}
#endif

static constexpr uint32_t kSegmentedBufferFreeOMTThreshold = 128;

nsresult nsSegmentedBuffer::Init(uint32_t aSegmentSize) {
  if (mSegmentArrayCount != 0) {
    return NS_ERROR_FAILURE;  // initialized more than once
  }
  mSegmentSize = aSegmentSize;
  mSegmentArrayCount = NS_SEGMENTARRAY_INITIAL_COUNT;
  return NS_OK;
}

char* nsSegmentedBuffer::AppendNewSegment(
    mozilla::UniqueFreePtr<char> aSegment) {
  if (!mSegmentArray) {
    uint32_t bytes = mSegmentArrayCount * sizeof(char*);
    mSegmentArray = (char**)moz_xmalloc(bytes);
    memset(mSegmentArray, 0, bytes);
  }

  if (IsFull()) {
    mozilla::CheckedInt<uint32_t> newArraySize =
        mozilla::CheckedInt<uint32_t>(mSegmentArrayCount) * 2;
    mozilla::CheckedInt<uint32_t> bytes = newArraySize * sizeof(char*);
    if (!bytes.isValid()) {
      return nullptr;
    }

    mSegmentArray = (char**)moz_xrealloc(mSegmentArray, bytes.value());
    // copy wrapped content to new extension
    if (mFirstSegmentIndex > mLastSegmentIndex) {
      // deal with wrap around case
      memcpy(&mSegmentArray[mSegmentArrayCount], mSegmentArray,
             mLastSegmentIndex * sizeof(char*));
      memset(mSegmentArray, 0, mLastSegmentIndex * sizeof(char*));
      mLastSegmentIndex += mSegmentArrayCount;
      memset(&mSegmentArray[mLastSegmentIndex], 0,
             (newArraySize.value() - mLastSegmentIndex) * sizeof(char*));
    } else {
      memset(&mSegmentArray[mLastSegmentIndex], 0,
             (newArraySize.value() - mLastSegmentIndex) * sizeof(char*));
    }
    mSegmentArrayCount = newArraySize.value();
  }

  char* seg = aSegment ? aSegment.release() : (char*)malloc(mSegmentSize);
  if (!seg) {
    return nullptr;
  }
  mSegmentArray[mLastSegmentIndex] = seg;
#ifdef GECKO_W10M
  const uint64_t bytes = GeckoW10mSegmentSizeOf(seg);
  mDiagnosticOwnedBytes += bytes;
  sSegmentOwnedBytes += bytes;
#endif
  mLastSegmentIndex = ModSegArraySize(mLastSegmentIndex + 1);
  return seg;
}

mozilla::UniqueFreePtr<char> nsSegmentedBuffer::PopFirstSegment() {
  NS_ASSERTION(mSegmentArray[mFirstSegmentIndex] != nullptr,
               "deleting bad segment");
  mozilla::UniqueFreePtr<char> segment(mSegmentArray[mFirstSegmentIndex]);
#ifdef GECKO_W10M
  const uint64_t bytes = GeckoW10mSegmentSizeOf(segment.get());
  mDiagnosticOwnedBytes -= bytes;
  sSegmentOwnedBytes -= bytes;
#endif
  mSegmentArray[mFirstSegmentIndex] = nullptr;
  int32_t last = ModSegArraySize(mLastSegmentIndex - 1);
  if (mFirstSegmentIndex == last) {
    mLastSegmentIndex = last;
  } else {
    mFirstSegmentIndex = ModSegArraySize(mFirstSegmentIndex + 1);
  }
  return segment;
}

mozilla::UniqueFreePtr<char> nsSegmentedBuffer::PopLastSegment() {
  int32_t last = ModSegArraySize(mLastSegmentIndex - 1);
  NS_ASSERTION(mSegmentArray[last] != nullptr, "deleting bad segment");
  mozilla::UniqueFreePtr<char> segment(mSegmentArray[last]);
#ifdef GECKO_W10M
  const uint64_t bytes = GeckoW10mSegmentSizeOf(segment.get());
  mDiagnosticOwnedBytes -= bytes;
  sSegmentOwnedBytes -= bytes;
#endif
  mSegmentArray[last] = nullptr;
  mLastSegmentIndex = last;
  return segment;
}

bool nsSegmentedBuffer::ReallocLastSegment(size_t aNewSize) {
  int32_t last = ModSegArraySize(mLastSegmentIndex - 1);
  NS_ASSERTION(mSegmentArray[last] != nullptr, "realloc'ing bad segment");
#ifdef GECKO_W10M
  const uint64_t oldBytes = GeckoW10mSegmentSizeOf(mSegmentArray[last]);
#endif
  char* newSegment = (char*)realloc(mSegmentArray[last], aNewSize);
  if (newSegment) {
#ifdef GECKO_W10M
    const uint64_t newBytes = GeckoW10mSegmentSizeOf(newSegment);
    mDiagnosticOwnedBytes = mDiagnosticOwnedBytes - oldBytes + newBytes;
    sSegmentOwnedBytes -= oldBytes;
    sSegmentOwnedBytes += newBytes;
#endif
    mSegmentArray[last] = newSegment;
    return true;
  }
  return false;
}

void nsSegmentedBuffer::Clear() {
#ifdef GECKO_W10M
  const uint64_t pendingBytes = std::exchange(mDiagnosticOwnedBytes, 0);
  sSegmentOwnedBytes -= pendingBytes;
  sSegmentPendingFreeBytes += pendingBytes;
#endif
  // Clear out the buffer's members back to their initial state.
  uint32_t arrayCount =
      std::exchange(mSegmentArrayCount, NS_SEGMENTARRAY_INITIAL_COUNT);
  char** segmentArray = std::exchange(mSegmentArray, nullptr);
  mFirstSegmentIndex = mLastSegmentIndex = 0;

  auto freeSegmentArray = [arrayCount, segmentArray
#ifdef GECKO_W10M
                           ,
                           pendingBytes
#endif
  ]() {
    for (uint32_t i = 0; i < arrayCount; ++i) {
      if (segmentArray[i]) {
        free(segmentArray[i]);
      }
    }
    free(segmentArray);
#ifdef GECKO_W10M
    sSegmentPendingFreeBytes -= pendingBytes;
#endif
  };

  if (segmentArray) {
    // If we have a small number of entries, free them synchronously. In some
    // rare cases, `nsSegmentedBuffer` may be gigantic, in which case it should
    // be freed async in a background task to avoid janking this thread.
    if (arrayCount < kSegmentedBufferFreeOMTThreshold ||
        NS_FAILED(NS_DispatchBackgroundTask(NS_NewRunnableFunction(
            "nsSegmentedBuffer::Clear", freeSegmentArray)))) {
      freeSegmentArray();
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
