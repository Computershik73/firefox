#include <cstdio>
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Bootstrap.h"
#include "nsXPCOM.h"

#include "AutoSQLiteLifetime.h"

#if defined(XP_WIN) && defined(_M_X64) && defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED)
#  include <windows.h>
#endif  // XP_WIN && _M_X64 && MOZ_DIAGNOSTIC_ASSERT_ENABLED

#ifdef MOZ_WIDGET_ANDROID
#  include "mozilla/jni/Utils.h"
#  ifdef MOZ_PROFILE_GENERATE
extern "C" int __llvm_profile_dump(void);
#  endif
#endif

#if defined(GECKO_W10M)
#  include <windows.h>

#  include <cstring>

#  include <delayimp.h>

namespace {

// Windows 10 Mobile ships a reduced version of the desktop libraries: a
// library may be absent entirely, or present and missing most of its
// functions. Of the 470 functions Gecko delay-loads, this device has 221 of
// them missing. Gecko reaches them through delay-load thunks, and a thunk that
// cannot resolve reports it by raising -- which on 32-bit ARM, where clang
// emits no exception handling at all, nothing catches. One absent function
// ends the thread and takes the process with it.
//
// The delay-load machinery offers a failure hook for exactly this. Answering
// with a stub is only safe because of the architecture: 32-bit ARM has one
// calling convention and the caller removes its own arguments, so a single
// nullary function can stand in for any signature at all. The same trick on
// x86 would corrupt the stack on the first call, every stdcall stub having to
// match its victim's argument count exactly.
//
// What a stub cannot do is be correct. Returning zero reads as failure to most
// callers and as "none" to the rest, which is the truth here -- there is no
// window to find, no theme to open, no MIDI device to enumerate -- but a
// caller that expected success will misbehave rather than crash. So every
// substitution is written down, and GECKO_W10M_DELAYLOAD_LOG says where.

// One open handle per log, and the flush on a leash.
//
// Every line used to cost a CreateFile2, a seek, two writes, a
// FlushFileBuffers -- which makes the eMMC commit, and on this phone that is
// milliseconds -- and a CloseHandle. Notes are written from the renderer
// thread between frames and from the main thread on every console message, so
// the log was quietly setting the frame rate: compositing sat at about thirty
// frames a second, which a sixty-frame video notices as every second frame
// dropped. The handle stays open now and the flush happens at most four times
// a second. A quarter second of notes is what a hard hang can lose; anything
// short of that the kernel still has when the process dies.
struct GeckoW10mLogFile {
  const wchar_t* variable = nullptr;
  HANDLE handle = INVALID_HANDLE_VALUE;
  ULONGLONG flushed = 0;
  ULONGLONG lastTry = 0;
  unsigned attempts = 0;
  unsigned lost = 0;
};

static GeckoW10mLogFile gGeckoW10mLogs[4];
static CRITICAL_SECTION gGeckoW10mLogLock;
static INIT_ONCE gGeckoW10mLogLockOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK GeckoW10mInitLogLock(PINIT_ONCE, PVOID, PVOID*) {
  ::InitializeCriticalSectionEx(&gGeckoW10mLogLock, 0, 0);
  return TRUE;
}

void AppendLine(const wchar_t* variable, const char* text) {
  ::InitOnceExecuteOnce(&gGeckoW10mLogLockOnce, GeckoW10mInitLogLock, nullptr,
                        nullptr);
  ::EnterCriticalSection(&gGeckoW10mLogLock);

  GeckoW10mLogFile* slot = nullptr;
  for (auto& candidate : gGeckoW10mLogs) {
    if (candidate.variable == variable) {
      slot = &candidate;
      break;
    }
    if (!candidate.variable) {
      candidate.variable = variable;
      slot = &candidate;
      break;
    }
  }

  // The open used to be tried once, with the file shared for reading only,
  // and every device log had exactly one launch in it. The launch before is
  // why: a closed app on this platform is suspended first and ended some
  // time later, and until then its handle is still on the file. The next
  // process got a sharing violation, gave up for good, and the second launch
  // -- the one every startup question hinged on -- never left a line.
  // Shared for everything now, and tried again every two seconds until it
  // opens; the lines lost meanwhile are counted, and the count is the first
  // thing written.
  if (slot && slot->handle == INVALID_HANDLE_VALUE) {
    const ULONGLONG now = ::GetTickCount64();
    if (!slot->attempts || now - slot->lastTry >= 2000) {
      slot->lastTry = now;
      ++slot->attempts;
      wchar_t path[MAX_PATH] = {};
      if (::GetEnvironmentVariableW(variable, path, MAX_PATH)) {
        CREATEFILE2_EXTENDED_PARAMETERS params{};
        params.dwSize = sizeof(params);
        params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        slot->handle = ::CreateFile2(
            path, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            OPEN_ALWAYS, &params);
        if (slot->handle != INVALID_HANDLE_VALUE && slot->attempts > 1) {
          char opened[128];
          ::snprintf(opened, sizeof(opened),
                     "note log: opened on attempt %u, %u lines lost before it",
                     slot->attempts, slot->lost);
          DWORD written = 0;
          ::SetFilePointer(slot->handle, 0, nullptr, FILE_END);
          ::WriteFile(slot->handle, opened,
                      static_cast<DWORD>(std::strlen(opened)), &written,
                      nullptr);
          ::WriteFile(slot->handle, "\r\n", 2, &written, nullptr);
        }
      }
    }
    if (slot->handle == INVALID_HANDLE_VALUE) {
      ++slot->lost;
    }
  }

  if (slot && slot->handle != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ::SetFilePointer(slot->handle, 0, nullptr, FILE_END);
    ::WriteFile(slot->handle, text, static_cast<DWORD>(std::strlen(text)),
                &written, nullptr);
    ::WriteFile(slot->handle, "\r\n", 2, &written, nullptr);
    const ULONGLONG now = ::GetTickCount64();
    if (now - slot->flushed >= 250) {
      slot->flushed = now;
      ::FlushFileBuffers(slot->handle);
    }
  }

  ::LeaveCriticalSection(&gGeckoW10mLogLock);
}

void RecordSubstitution(const char* dll, const char* proc) {
  char line[256];
  int length = 0;
  auto append = [&](const char* text) {
    while (*text && length < static_cast<int>(sizeof(line)) - 1) {
      line[length++] = *text++;
    }
  };
  append(dll ? dll : "?");
  append("!");
  append(proc ? proc : "(ordinal)");
  line[length] = 0;
  AppendLine(L"GECKO_W10M_DELAYLOAD_LOG", line);
}

// Zero in r0. Any caller's arguments are its own to clean up, so this is a
// valid body for every signature the thunks can ask for.
int WINAPI GeckoW10mMissingProc() { return 0; }

// Zero is the wrong answer for some of them, and the substitution log says
// which -- it records what the engine actually reached for rather than what it
// might. Two of those were being answered with something worse than nothing.
//
// MsgWaitForMultipleObjectsEx was returning zero, which reads as "the first
// handle is signalled", so the event loop stopped waiting and spun at full
// speed: hundreds of consecutive calls in the log. There is no message queue
// here to wait on, but the handles are real, and waiting on those is the whole
// of what this call means on a device with no messages.
DWORD WINAPI GeckoW10mMsgWaitForMultipleObjectsEx(DWORD aCount,
                                                const HANDLE* aHandles,
                                                DWORD aMilliseconds,
                                                DWORD aWakeMask, DWORD aFlags) {
  constexpr DWORD kWaitAll = 0x0001;    // MWMO_WAITALL
  constexpr DWORD kAlertable = 0x0002;  // MWMO_ALERTABLE

  if (!aCount) {
    // WaitForMultipleObjectsEx rejects an empty set, and a caller with no
    // handles was only ever waiting for messages.
    ::SleepEx(aMilliseconds, (aFlags & kAlertable) != 0);
    return WAIT_TIMEOUT;
  }
  return ::WaitForMultipleObjectsEx(aCount, aHandles, (aFlags & kWaitAll) != 0,
                                    aMilliseconds, (aFlags & kAlertable) != 0);
}

// And zero is S_OK. A function returning an HRESULT that claims success while
// leaving its out parameter untouched hands the caller whatever was already
// there -- for DwmGetCompositionTimingInfo, a composition clock made of noise,
// feeding the refresh driver. Failing is both true and useful: Gecko has a
// fallback for a machine with no compositor, and this is one.
HRESULT WINAPI GeckoW10mFailingHresult() { return E_FAIL; }

// And zero is "it worked" for the raw input calls, which report failure as
// (UINT)-1 and their result through an out parameter they never wrote. The
// gamepad service asked GetRawInputDeviceList how many devices there are,
// was told "none of my business went wrong", read an uninitialised count of
// several hundred million, and tried to allocate an array for it -- which
// ends in NS_ABORT_OOM and takes the browser with it. A page that touches
// the Gamepad API, such as the player on vkvideo.ru, was enough.
UINT WINAPI GeckoW10mFailingUint() { return static_cast<UINT>(-1); }

// GetSysColor is not on the phone either, and a stub answering zero paints
// every system colour black: the menus, the address bar's drop-down and the
// autocomplete panel over a page all came out as solid black rectangles,
// because their backgrounds are -moz-Dialog, Menu and Window, which the
// Windows look-and-feel reads straight from here. These are the Windows 10
// light-theme defaults, COLORREF byte order (0x00BBGGRR).
DWORD WINAPI GeckoW10mGetSysColor(int aIndex) {
  static const DWORD kColors[] = {
      0x00C8C8C8,  // 0  COLOR_SCROLLBAR
      0x00000000,  // 1  COLOR_BACKGROUND
      0x00D1B499,  // 2  COLOR_ACTIVECAPTION
      0x00DBCDBF,  // 3  COLOR_INACTIVECAPTION
      0x00F0F0F0,  // 4  COLOR_MENU
      0x00FFFFFF,  // 5  COLOR_WINDOW
      0x00646464,  // 6  COLOR_WINDOWFRAME
      0x00000000,  // 7  COLOR_MENUTEXT
      0x00000000,  // 8  COLOR_WINDOWTEXT
      0x00000000,  // 9  COLOR_CAPTIONTEXT
      0x00B4B4B4,  // 10 COLOR_ACTIVEBORDER
      0x00FCF7F4,  // 11 COLOR_INACTIVEBORDER
      0x00ABABAB,  // 12 COLOR_APPWORKSPACE
      0x00D77800,  // 13 COLOR_HIGHLIGHT
      0x00FFFFFF,  // 14 COLOR_HIGHLIGHTTEXT
      0x00F0F0F0,  // 15 COLOR_BTNFACE
      0x00A0A0A0,  // 16 COLOR_BTNSHADOW
      0x006D6D6D,  // 17 COLOR_GRAYTEXT
      0x00000000,  // 18 COLOR_BTNTEXT
      0x00000000,  // 19 COLOR_INACTIVECAPTIONTEXT
      0x00FFFFFF,  // 20 COLOR_BTNHIGHLIGHT
      0x00696969,  // 21 COLOR_3DDKSHADOW
      0x00E3E3E3,  // 22 COLOR_3DLIGHT
      0x00000000,  // 23 COLOR_INFOTEXT
      0x00E1FFFF,  // 24 COLOR_INFOBK
      0x00000000,  // 25 (unused)
      0x00CC6600,  // 26 COLOR_HOTLIGHT
      0x00EAD1B9,  // 27 COLOR_GRADIENTACTIVECAPTION
      0x00F2E4D7,  // 28 COLOR_GRADIENTINACTIVECAPTION
      0x00D77800,  // 29 COLOR_MENUHILIGHT
      0x00F0F0F0,  // 30 COLOR_MENUBAR
  };
  if (aIndex < 0 || aIndex >= int(sizeof(kColors) / sizeof(kColors[0]))) {
    return 0;
  }
  return kColors[aIndex];
}

// A caret that blinks every half second, as on the desktop; zero here means
// "never blink", which reads as a dead field.
UINT WINAPI GeckoW10mGetCaretBlinkTime() { return 530; }

struct Substitute {
  const char* name;
  FARPROC stub;
};

const Substitute kSubstitutes[] = {
    {"MsgWaitForMultipleObjectsEx",
     reinterpret_cast<FARPROC>(&GeckoW10mMsgWaitForMultipleObjectsEx)},
    {"DwmGetCompositionTimingInfo",
     reinterpret_cast<FARPROC>(&GeckoW10mFailingHresult)},
    {"DwmIsCompositionEnabled",
     reinterpret_cast<FARPROC>(&GeckoW10mFailingHresult)},
    {"DwmFlush", reinterpret_cast<FARPROC>(&GeckoW10mFailingHresult)},
    {"GetThemeColor", reinterpret_cast<FARPROC>(&GeckoW10mFailingHresult)},
    {"GetThemeMetric", reinterpret_cast<FARPROC>(&GeckoW10mFailingHresult)},
    {"GetThemePartSize", reinterpret_cast<FARPROC>(&GeckoW10mFailingHresult)},
    {"GetThemeSysFont", reinterpret_cast<FARPROC>(&GeckoW10mFailingHresult)},
    {"CloseThemeData", reinterpret_cast<FARPROC>(&GeckoW10mFailingHresult)},
    {"GetRawInputDeviceList", reinterpret_cast<FARPROC>(&GeckoW10mFailingUint)},
    {"GetRawInputDeviceInfoW", reinterpret_cast<FARPROC>(&GeckoW10mFailingUint)},
    {"GetRawInputDeviceInfoA", reinterpret_cast<FARPROC>(&GeckoW10mFailingUint)},
    {"GetRawInputData", reinterpret_cast<FARPROC>(&GeckoW10mFailingUint)},
    {"GetRawInputBuffer", reinterpret_cast<FARPROC>(&GeckoW10mFailingUint)},
    {"GetSysColor", reinterpret_cast<FARPROC>(&GeckoW10mGetSysColor)},
    {"GetCaretBlinkTime", reinterpret_cast<FARPROC>(&GeckoW10mGetCaretBlinkTime)},
};

FARPROC WINAPI GeckoW10mDelayLoadFailure(unsigned notification,
                                       PDelayLoadInfo info) {
  if (!info) return nullptr;

  if (notification == dliFailLoadLib) {
    // The library is not on the device at all. Handing back a module that is
    // -- this executable will do -- sends every function in it down the path
    // below, where each gets a stub of its own, rather than failing the whole
    // group here.
    RecordSubstitution(info->szDll, "(entire library)");
    return reinterpret_cast<FARPROC>(::GetModuleHandleW(nullptr));
  }

  if (notification != dliFailGetProc) return nullptr;

  RecordSubstitution(info->szDll, info->dlp.fImportByName ? info->dlp.szProcName
                                                          : nullptr);

  if (info->dlp.fImportByName) {
    for (const Substitute& substitute : kSubstitutes) {
      if (std::strcmp(info->dlp.szProcName, substitute.name) == 0) {
        return substitute.stub;
      }
    }
  }
  return reinterpret_cast<FARPROC>(&GeckoW10mMissingProc);
}

}  // namespace

// Bringing an engine up on a device with no debugger means the engine has to
// be able to say things. printf_stderr goes nowhere in a packaged app -- the
// process has no console, and ucrtbase captured the standard handles long
// before anything could redirect them -- and MOZ_LOG is not up yet during the
// startup this port keeps dying in. This writes one line to a file the shell
// names, immediately and flushed, which is enough to instrument a suspect
// function and read the answer off the device afterwards.
extern "C" void GeckoW10mNote(const char* text) {
  // Stamped with the wall clock to the millisecond, in the same form as the
  // shell's log, so the two can be laid side by side. The notes have said
  // "the EGL compositor is up" for a month without ever saying when.
  SYSTEMTIME now;
  ::GetLocalTime(&now);
  char line[512];
  snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u  %s", now.wHour, now.wMinute,
           now.wSecond, now.wMilliseconds, text ? text : "");
  AppendLine(L"GECKO_W10M_NOTE_LOG", line);
}

// What the process's address space is made of. A 32-bit process on the phone
// has 2 GB of it, the browser takes about 1.2 GB before any page, and Discord
// after login could not get 64 MB in one piece with 450 MB free in total: the
// rest was in hundreds of pieces, the largest 18 MB. This says who holds what:
// totals by kind, private allocations by size, the largest allocations with
// the module or file behind them, and a picture of the whole space in 16 MB
// cells. Twelve a run; from the shell a minute after start and when the
// system says memory use went up, and from the engine at the first failed JS
// allocation.
namespace {
struct GeckoW10mAlloc {
  uintptr_t base = 0;
  size_t size = 0;
  size_t committed = 0;
  DWORD type = 0;
  DWORD protect = 0;
};

const char* GeckoW10mBaseName(const wchar_t* aPath, char* aOut, size_t aLen) {
  const wchar_t* name = aPath;
  for (const wchar_t* c = aPath; *c; ++c) {
    if (*c == L'\\' || *c == L'/') {
      name = c + 1;
    }
  }
  size_t i = 0;
  for (; name[i] && i + 1 < aLen; ++i) {
    aOut[i] = name[i] < 128 ? static_cast<char>(name[i]) : '?';
  }
  aOut[i] = 0;
  return aOut;
}
}  // namespace

static void GeckoW10mNoteAddressSpace(const char* aWhy);

// From the shell: a minute after start, and when memory use goes up. Eight a
// run.
extern "C" __declspec(dllexport) void gecko_w10m_note_address_space(
    const char* aWhy) {
  static LONG sCalls = 0;
  if (::InterlockedIncrement(&sCalls) > 8) {
    return;
  }
  GeckoW10mNoteAddressSpace(aWhy);
}

// From the engine, when an allocation has failed: a budget of its own, so the
// map of the moment that matters is never the one left out.
extern "C" void GeckoW10mNoteAddressSpaceOnFailure(const char* aWhy) {
  static LONG sCalls = 0;
  if (::InterlockedIncrement(&sCalls) > 3) {
    return;
  }
  GeckoW10mNoteAddressSpace(aWhy);
}

static void GeckoW10mNoteAddressSpace(const char* aWhy) {
  // One at a time, each line marked with its map's number: two maps taken at
  // once would otherwise interleave line by line. Nothing in here allocates,
  // and the log's own lock is taken and let go inside, line by line.
  static SRWLOCK sLock = SRWLOCK_INIT;
  static LONG sSerial = 0;
  ::AcquireSRWLockExclusive(&sLock);
  const int serial = int(::InterlockedIncrement(&sSerial));
  constexpr size_t kMB = 1024 * 1024;
  constexpr size_t kCell = 16 * kMB;
  constexpr int kTop = 20;
  constexpr int kCells = 128;  // 2 GB in 16 MB cells
  GeckoW10mAlloc top[kTop];
  int topCount = 0;
  // image, mapped, private
  size_t kindSize[3] = {}, kindCommit[3] = {}, kindCount[3] = {};
  // private allocations: <1, 1-4, 4-16, 16-64, 64+ MB
  size_t bucketCount[5] = {}, bucketSize[5] = {};
  size_t freeTotal = 0, freePieces = 0, largest = 0;
  size_t freeBySize[4] = {};  // 64+, 16-64, 4-16, smaller
  // Per cell: free, image, mapped, private bytes.
  size_t cells[kCells][4] = {};

  auto kindOf = [](DWORD aType) {
    return aType == MEM_IMAGE ? 0 : aType == MEM_MAPPED ? 1 : 2;
  };
  auto paint = [&](uintptr_t aStart, size_t aSize, int aSlot) {
    uintptr_t at = aStart;
    const uintptr_t end = aStart + aSize;
    while (at < end) {
      const size_t cell = at / kCell;
      if (cell >= size_t(kCells)) {
        break;
      }
      const uintptr_t cellEnd = (cell + 1) * kCell;
      const uintptr_t stop = end < cellEnd ? end : cellEnd;
      cells[cell][aSlot] += stop - at;
      at = stop;
    }
  };
  auto flush = [&](const GeckoW10mAlloc& a) {
    const int kind = kindOf(a.type);
    kindSize[kind] += a.size;
    kindCommit[kind] += a.committed;
    ++kindCount[kind];
    if (kind == 2) {
      const int b = a.size < kMB ? 0 : a.size < 4 * kMB ? 1 : a.size < 16 * kMB ? 2
                  : a.size < 64 * kMB ? 3 : 4;
      ++bucketCount[b];
      bucketSize[b] += a.size;
    }
    // Keep the largest, sorted.
    int slot = topCount < kTop ? topCount : kTop - 1;
    if (topCount == kTop && a.size <= top[kTop - 1].size) {
      return;
    }
    while (slot > 0 && top[slot - 1].size < a.size) {
      top[slot] = top[slot - 1];
      --slot;
    }
    top[slot] = a;
    if (topCount < kTop) {
      ++topCount;
    }
  };

  GeckoW10mAlloc cur;
  bool haveCur = false;
  MEMORY_BASIC_INFORMATION info;
  uintptr_t at = 0x10000;
  while (::VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) ==
         sizeof(info)) {
    const uintptr_t base = uintptr_t(info.BaseAddress);
    if (info.State == MEM_FREE) {
      if (haveCur) {
        flush(cur);
        haveCur = false;
      }
      freeTotal += info.RegionSize;
      ++freePieces;
      if (info.RegionSize > largest) {
        largest = info.RegionSize;
      }
      ++freeBySize[info.RegionSize >= 64 * kMB   ? 0
                   : info.RegionSize >= 16 * kMB ? 1
                   : info.RegionSize >= 4 * kMB  ? 2
                                                 : 3];
      paint(base, info.RegionSize, 0);
    } else {
      const uintptr_t allocation = uintptr_t(info.AllocationBase);
      if (!haveCur || allocation != cur.base) {
        if (haveCur) {
          flush(cur);
        }
        cur = GeckoW10mAlloc();
        cur.base = allocation;
        cur.type = info.Type;
        cur.protect = info.AllocationProtect;
        haveCur = true;
      }
      cur.size += info.RegionSize;
      if (info.State == MEM_COMMIT) {
        cur.committed += info.RegionSize;
      }
      paint(base, info.RegionSize, 1 + kindOf(info.Type));
    }
    const uintptr_t next = base + info.RegionSize;
    if (next <= at) {
      break;
    }
    at = next;
  }
  if (haveCur) {
    flush(cur);
  }

  char note[512];
  snprintf(note, sizeof(note),
           "vm%d (%s): images %zu MB in %zu, mapped files %zu MB in %zu, private "
           "%zu MB in %zu (%zu MB of it committed); free %zu MB in %zu pieces, "
           "the largest %zu MB -- %zu of 64 MB or more, %zu of 16-64, %zu of "
           "4-16, %zu smaller",
           serial, aWhy ? aWhy : "asked", kindSize[0] / kMB, kindCount[0],
           kindSize[1] / kMB, kindCount[1], kindSize[2] / kMB, kindCount[2],
           kindCommit[2] / kMB, freeTotal / kMB, freePieces, largest / kMB,
           freeBySize[0], freeBySize[1], freeBySize[2], freeBySize[3]);
  GeckoW10mNote(note);
  snprintf(note, sizeof(note),
           "vm%d: private allocations -- under 1 MB: %zu (%zu MB), 1-4 MB: %zu "
           "(%zu MB), 4-16 MB: %zu (%zu MB), 16-64 MB: %zu (%zu MB), 64 MB or "
           "more: %zu (%zu MB)",
           serial, bucketCount[0], bucketSize[0] / kMB, bucketCount[1],
           bucketSize[1] / kMB, bucketCount[2], bucketSize[2] / kMB,
           bucketCount[3], bucketSize[3] / kMB, bucketCount[4],
           bucketSize[4] / kMB);
  GeckoW10mNote(note);

  using MappedNameFn = DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD);
  static const auto mappedName = [] {
    for (const wchar_t* dll : {L"kernelbase.dll", L"kernel32.dll"}) {
      if (HMODULE module = ::GetModuleHandleW(dll)) {
        if (FARPROC fn = ::GetProcAddress(module, "K32GetMappedFileNameW")) {
          return reinterpret_cast<MappedNameFn>(fn);
        }
      }
    }
    return MappedNameFn(nullptr);
  }();
  for (int i = 0; i < topCount; ++i) {
    const GeckoW10mAlloc& a = top[i];
    wchar_t path[MAX_PATH] = L"";
    char name[64] = "";
    if (a.type == MEM_IMAGE) {
      if (::GetModuleFileNameW(reinterpret_cast<HMODULE>(a.base), path,
                               MAX_PATH)) {
        GeckoW10mBaseName(path, name, sizeof(name));
      }
    } else if (a.type == MEM_MAPPED && mappedName) {
      if (mappedName(::GetCurrentProcess(), reinterpret_cast<void*>(a.base),
                     path, MAX_PATH)) {
        GeckoW10mBaseName(path, name, sizeof(name));
      }
    }
    const DWORD p = a.protect & 0xff;
    snprintf(note, sizeof(note),
             "vm%d: %2d. 0x%08x %6.1f MB %s, %.1f MB committed, %s%s%s",
             serial, i + 1,
             unsigned(a.base), double(a.size) / kMB,
             a.type == MEM_IMAGE ? "image" : a.type == MEM_MAPPED ? "mapped"
                                                                  : "private",
             double(a.committed) / kMB,
             p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY ? "rwx"
             : p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE              ? "rx"
             : p == PAGE_READWRITE || p == PAGE_WRITECOPY               ? "rw"
             : p == PAGE_READONLY                                       ? "r"
             : p == PAGE_NOACCESS                                       ? "reserved"
                                                                        : "other",
             name[0] ? " " : "", name);
    GeckoW10mNote(note);
  }

  // The picture: '.' a free cell, ',' half free or more, else who holds most
  // of it -- I an image, M a mapped file, P private memory. 64 cells a line,
  // a gigabyte.
  for (int half = 0; half < 2; ++half) {
    char picture[kCells / 2 + 1];
    for (int c = 0; c < kCells / 2; ++c) {
      const size_t* cell = cells[half * (kCells / 2) + c];
      char mark;
      if (cell[0] >= kCell) {
        mark = '.';
      } else if (cell[0] * 2 >= kCell) {
        mark = ',';
      } else if (cell[1] >= cell[2] && cell[1] >= cell[3]) {
        mark = 'I';
      } else if (cell[2] >= cell[3]) {
        mark = 'M';
      } else {
        mark = 'P';
      }
      picture[c] = mark;
    }
    picture[kCells / 2] = 0;
    snprintf(note, sizeof(note), "vm%d: %s GB |%s|", serial,
             half ? "1-2" : "0-1", picture);
    GeckoW10mNote(note);
  }
  ::ReleaseSRWLockExclusive(&sLock);
}

// The hook is const -- it was made so deliberately, a writable global function
// pointer being an obvious thing to attack -- so it is defined here rather than
// assigned at run time. delayimp.lib supplies a null default only when nothing
// else defines it.
extern "C" const PfnDliHook __pfnDliFailureHook2 = &GeckoW10mDelayLoadFailure;
#endif  // GECKO_W10M

namespace mozilla {

class BootstrapImpl final : public Bootstrap {
 protected:
  AutoSQLiteLifetime mSQLLT;

  virtual void Dispose() override { delete this; }

 public:
  BootstrapImpl() = default;

  ~BootstrapImpl() = default;

  virtual void NS_LogInit() override { ::NS_LogInit(); }

  virtual void NS_LogTerm() override { ::NS_LogTerm(); }

  virtual void XRE_StartupTimelineRecord(int aEvent,
                                         mozilla::TimeStamp aWhen) override {
    ::XRE_StartupTimelineRecord(aEvent, aWhen);
  }

  virtual int XRE_main(int argc, char* argv[],
                       const BootstrapConfig& aConfig) override {
#if defined(GECKO_W10M)
    // First line in the file, so its absence means the channel is broken
    // rather than the code being instrumented never running.
    GeckoW10mNote("XRE_main entered");
#endif
    return ::XRE_main(argc, argv, aConfig);
  }

  virtual void XRE_StopLateWriteChecks() override {
    ::XRE_StopLateWriteChecks();
  }

  virtual int XRE_XPCShellMain(int argc, char** argv, char** envp,
                               const XREShellData* aShellData) override {
    return ::XRE_XPCShellMain(argc, argv, envp, aShellData);
  }

  virtual nsresult XRE_InitChildProcess(
      int argc, char* argv[], const XREChildData* aChildData) override {
    return ::XRE_InitChildProcess(argc, argv, aChildData);
  }

  virtual void XRE_EnableSameExecutableForContentProc() override {
    ::XRE_EnableSameExecutableForContentProc();
  }

#ifdef MOZ_WIDGET_ANDROID
  virtual void XRE_SetGeckoThreadEnv(JNIEnv* aEnv) override {
    mozilla::jni::SetGeckoThreadEnv(aEnv);
  }

  virtual void XRE_SetAndroidChildFds(JNIEnv* aEnv, jintArray aFds) override {
    ::XRE_SetAndroidChildFds(aEnv, aFds);
  }

#  ifdef MOZ_PROFILE_GENERATE
  virtual void XRE_WriteLLVMProfData() override {
    __android_log_print(ANDROID_LOG_INFO, "GeckoLibLoad",
                        "Calling __llvm_profile_dump()");
    __llvm_profile_dump();
  }
#  endif
#endif

#ifdef LIBFUZZER
  virtual void XRE_LibFuzzerSetDriver(LibFuzzerDriver aDriver) override {
    ::XRE_LibFuzzerSetDriver(aDriver);
  }
#endif

#ifdef MOZ_ENABLE_FORKSERVER
  virtual int XRE_ForkServer(int* argc, char*** argv) override {
    return ::XRE_ForkServer(argc, argv);
  }
#endif
};

#if defined(XP_WIN) && defined(_M_X64) && defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED)
extern "C" uint32_t _tls_index;

extern "C" NS_EXPORT bool XRE_CheckBlockScopeStaticVarInit(
    uint32_t* aTlsIndex) {
  // Copy the value of xul's _tls_index for diagnostics.
  if (aTlsIndex) {
    *aTlsIndex = _tls_index;
  }

  // Check that block-scope static variable initialization works. We use
  // volatile here to keep the compiler honest - we want it to generate the code
  // that will ensure that only a single thread goes through the lambda.
  static bool sItWorks = []() -> bool {
    bool const volatile value = true;
    return value;
  }();
  return sItWorks;
}
#endif  // XP_WIN && _M_X64 && MOZ_DIAGNOSTIC_ASSERT_ENABLED

extern "C" NS_EXPORT void NS_FROZENCALL
XRE_GetBootstrap(Bootstrap::UniquePtr& b) {
  static bool sBootstrapInitialized = false;
  MOZ_RELEASE_ASSERT(!sBootstrapInitialized);

  sBootstrapInitialized = true;
  b.reset(new BootstrapImpl());
}

}  // namespace mozilla
