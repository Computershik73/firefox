/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// This ensures the right configuration for e.g. MOZ_GLUE_IN_PROGRAM,
// used in the MFBT headers included further below. We use js-confdefs.h
// instead of mozilla-config.h because the latter is not present in
// spidermonkey standalone builds while the former is always present.
#include "js-confdefs.h"
#include "mozilla/Assertions.h"
#include "mozilla/Types.h"
#include "mozilla/mozalloc_oom.h"

#if defined(GECKO_W10M)
#  include "../../../memory/mozalloc/GeckoW10mNote.h"
#endif

// MOZ_Crash wrapper for use by rust, since MOZ_Crash is an inline function.
extern "C" void RustMozCrash(const char* aFilename, int aLine,
                             const char* aReason) {
#if defined(GECKO_W10M)
  // UWP has no usable stderr at this point. Preserve the Rust panic payload
  // before MOZ_Crash deliberately breaks and writes through null.
  gecko_w10m::Note("Rust MOZ_CRASH:");
  gecko_w10m::Note(aReason ? aReason : "(no reason)");
  gecko_w10m::Note(aFilename ? aFilename : "(no filename)");
#endif
  MOZ_Crash(aFilename, aLine, aReason);
}

// mozalloc_handle_oom wrapper for use by rust, because mozalloc_handle_oom is
// MFBT_API, that rust can't respect.
extern "C" void RustHandleOOM(size_t size) { mozalloc_handle_oom(size); }
