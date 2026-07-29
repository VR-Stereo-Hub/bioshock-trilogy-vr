#include "core/hooks/heap_scan.h"

#include <windows.h>
#include <tlhelp32.h>
#include <winnt.h>

#include <iterator>

namespace bvr::heap_scan {
namespace {

// The sweep's search space. The game is Large-Address-Aware and after a long
// session the engine allocates actors above 2 GB, so a 0x7FFF0000 cap made late
// objects invisible (session 18 part 4). VirtualQuery simply fails past the top
// on a non-LAA process, so the walk still terminates there.
constexpr uintptr_t kScanLow = 0x10000;
constexpr uintptr_t kScanHigh = 0xFFFE0000u;

// The needle never exists un-masked in this frame; see the header. Any non-zero
// constant works - this one is only a bit pattern, not a secret.
constexpr uintptr_t kNeedleMask = 0xA5C3F00Du;

// Deadline granularity. Checking the clock every dword would cost more than the
// compare; 16 KB of dwords between checks bounds the overshoot to a few
// microseconds while keeping the inner loop tight.
constexpr uintptr_t kDeadlineChunk = 16 * 1024;

int64_t qpc_freq() {
    static int64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? f.QuadPart : 1;
    }();
    return freq;
}

int64_t qpc_now() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

uint64_t qpc_to_us(int64_t ticks) {
    return static_cast<uint64_t>((ticks * 1000000LL) / qpc_freq());
}

// ---- thread stack / TEB exclusion -------------------------------------------

using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

NtQueryInformationThreadFn nt_query_thread() {
    static NtQueryInformationThreadFn fn = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtQueryInformationThreadFn>(
                           GetProcAddress(ntdll, "NtQueryInformationThread"))
                     : nullptr;
    }();
    return fn;
}

// ThreadBasicInformation. Declared locally rather than pulled from a DDK header
// - only TebBaseAddress is used and the layout has been stable since NT 3.1.
struct ThreadBasicInfo {
    LONG ExitStatus;
    PVOID TebBaseAddress;
    struct {
        HANDLE UniqueProcess;
        HANDLE UniqueThread;
    } ClientId;
    ULONG_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
};

void add_span(ExcludeSet& set, uintptr_t lo, uintptr_t hi) {
    if (hi <= lo) return;
    if (set.count >= kMaxExcludes) {
        set.truncated = true;
        return;
    }
    set.spans[set.count].lo = lo;
    set.spans[set.count].hi = hi;
    ++set.count;
}

// The whole stack ALLOCATION, not just its committed part: the reserved pages
// below StackLimit hold the guard page and grow into the same allocation, and a
// region-granular sweep would otherwise clip the boundary.
void add_stack_of_teb(ExcludeSet& set, const void* teb) {
    if (!teb) return;
    const auto* tib = static_cast<const NT_TIB*>(teb);
    // Reading another thread's TEB is an ordinary same-process read, but the
    // thread can exit underneath us, so guard it.
    uintptr_t stackBase = 0, stackLimit = 0;
    __try {
        stackBase = reinterpret_cast<uintptr_t>(tib->StackBase);
        stackLimit = reinterpret_cast<uintptr_t>(tib->StackLimit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!stackBase || !stackLimit || stackLimit >= stackBase) return;

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t lo = stackLimit;
    if (VirtualQuery(reinterpret_cast<void*>(stackLimit), &mbi, sizeof(mbi)) == sizeof(mbi) &&
        mbi.AllocationBase)
        lo = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    add_span(set, lo, stackBase);

    // The TEB itself is MEM_PRIVATE + PAGE_READWRITE and holds plenty of
    // pointers into the image.
    uintptr_t tebAddr = reinterpret_cast<uintptr_t>(teb);
    add_span(set, tebAddr, tebAddr + 0x2000);
}

// A stack's committed area always has a PAGE_GUARD page directly below it (the
// guard moves down as the stack grows), so a region whose predecessor page is
// guarded is a stack body. Second line of defence for a thread we could not
// open, and it needs no ntdll at all.
bool preceded_by_guard_page(uintptr_t base) {
    if (base <= 0x1000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(base - 0x1000), &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    return (mbi.Protect & PAGE_GUARD) != 0;
}

bool region_scannable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    // Engine objects live in private heap memory. MEM_IMAGE would match the
    // module's own .data copies of the vtable pointer; MEM_MAPPED is file and
    // section views.
    if (mbi.Type != MEM_PRIVATE) return false;
    switch (mbi.Protect & 0xFF) {
        case PAGE_READWRITE:
        case PAGE_EXECUTE_READWRITE:
            return true;
        default:
            return false;
    }
}

// The guarded inner sweep. No C++ objects, no logging, no allocation in this
// frame or anything it calls: a fault here unwinds without running destructors.
// Returns the address it stopped at (== end when the region is finished).
uintptr_t sweep_region(uintptr_t begin, uintptr_t end, uintptr_t xorNeedle, uint32_t needBytes,
                       Accept accept, void* user, Sweep* s, int64_t deadline) {
    uintptr_t a = begin;
    __try {
        while (a + needBytes <= end) {
            uintptr_t chunkEnd = a + kDeadlineChunk;
            if (chunkEnd > end - needBytes + 1) chunkEnd = end - needBytes + 1;
            for (; a < chunkEnd; a += 4) {
                if ((*reinterpret_cast<const uintptr_t*>(a) ^ kNeedleMask) != xorNeedle) continue;
                ++s->matches;
                void* obj = reinterpret_cast<void*>(a);
                int32_t probe[kProbeSlots] = {};
                bool good = accept(obj, user, probe);
                if (s->recorded < kMaxRecorded) {
                    s->addr[s->recorded] = obj;
                    for (int k = 0; k < kProbeSlots; ++k) s->probe[s->recorded][k] = probe[k];
                    s->ok[s->recorded] = good;
                    ++s->recorded;
                }
                if (good) {
                    ++s->accepted;
                    if (!s->first) s->first = obj;
                }
            }
            if (qpc_now() >= deadline) return a;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The region decommitted under us, or the accept filter read off the
        // end of a candidate. Abandon the rest of this region and move on.
        return end;
    }
    return end;
}

// A pooled allocator can put a fixed header in front of the object, so the
// object need not sit exactly at the block start. Probing a few aligned offsets
// costs a handful of compares per block and covers that; every hit still has to
// survive the accept filter, so a wider probe cannot loosen identity.
constexpr uint32_t kBlockProbeSpan = 64;

// Guarded per-heap walk. The __except swallows and falls through to HeapUnlock,
// so the unlock runs on both paths: a fault that escaped with the lock held
// would wedge that heap for the life of the process, blocking every thread that
// allocates from it. HeapLock is re-entrant for the calling thread, so an accept
// filter that allocated (none may) could not self-deadlock either.
void walk_one_heap(HANDLE heap, uintptr_t xorNeedle, uint32_t needBytes, Accept accept, void* user,
                   Sweep* s) {
    if (!HeapLock(heap)) return;
    __try {
        PROCESS_HEAP_ENTRY e{};
        while (HeapWalk(heap, &e)) {
            if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
            if (e.cbData < needBytes) continue;
            ++s->blocks;
            const uintptr_t blockLo = reinterpret_cast<uintptr_t>(e.lpData);
            const uintptr_t blockHi = blockLo + e.cbData;
            uintptr_t probeEnd = blockLo + kBlockProbeSpan;
            if (probeEnd > blockHi) probeEnd = blockHi;
            for (uintptr_t a = blockLo; a + needBytes <= blockHi && a < probeEnd; a += 4) {
                if ((*reinterpret_cast<const uintptr_t*>(a) ^ kNeedleMask) != xorNeedle) continue;
                ++s->matches;
                void* obj = reinterpret_cast<void*>(a);
                int32_t probe[kProbeSlots] = {};
                bool good = accept(obj, user, probe);
                if (s->recorded < kMaxRecorded) {
                    s->addr[s->recorded] = obj;
                    for (int k = 0; k < kProbeSlots; ++k) s->probe[s->recorded][k] = probe[k];
                    s->ok[s->recorded] = good;
                    ++s->recorded;
                }
                if (good) {
                    ++s->accepted;
                    if (!s->first) s->first = obj;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Fall through to __finally; the rest of this heap is abandoned.
    }
    HeapUnlock(heap);
}

} // namespace

bool ExcludeSet::overlaps(uintptr_t lo, uintptr_t hi) const {
    for (int i = 0; i < count; ++i)
        if (lo < spans[i].hi && spans[i].lo < hi) return true;
    return false;
}

void snapshot_excludes(ExcludeSet& out, int* missed) {
    out.count = 0;
    out.truncated = false;
    int miss = 0;

    // The current thread first, unconditionally: it is the one whose frames
    // hold the needle, and it needs no handle to find.
    add_stack_of_teb(out, NtCurrentTeb());

    auto query = nt_query_thread();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE || !query) {
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        if (missed) *missed = -1; // "could not enumerate at all"
        return;
    }

    const DWORD self = GetCurrentProcessId();
    const DWORD selfTid = GetCurrentThreadId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) +
                                sizeof(te.th32OwnerProcessID))
                continue;
            if (te.th32OwnerProcessID != self) continue;
            if (te.th32ThreadID == selfTid) continue; // already added

            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) {
                ++miss;
                continue;
            }
            ThreadBasicInfo tbi{};
            ULONG len = 0;
            if (query(th, 0 /*ThreadBasicInformation*/, &tbi, sizeof(tbi), &len) >= 0)
                add_stack_of_teb(out, tbi.TebBaseAddress);
            else
                ++miss;
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    if (missed) *missed = miss;
}

void Sweep::reset() {
    cursor = kScanLow;
    slices = 0;
    elapsedUs = 0;
    bytes = 0;
    matches = 0;
    accepted = 0;
    first = nullptr;
    recorded = 0;
    snapshot_excludes(excludes, &excludeMissed);
    excludeSpans = excludes.count;
}

Outcome sweep(Sweep& s, const void* wantVtable, uint32_t needBytes, Accept accept, void* user,
              uint32_t budgetUs) {
    if (!accept || !wantVtable || needBytes < sizeof(void*)) {
        s.reset();
        ++s.passes;
        return Outcome::Complete;
    }

    // A fresh pass re-snapshots the exclusions (inside reset): threads come and
    // go, and a stale snapshot is exactly how a stack gets swept.
    if (s.cursor < kScanLow) s.reset();

    // Masked here and compared masked in the inner loop, so the raw vtable
    // address is never a live value inside the sweep.
    const uintptr_t xorNeedle = reinterpret_cast<uintptr_t>(wantVtable) ^ kNeedleMask;

    const int64_t start = qpc_now();
    const int64_t deadline = start + (static_cast<int64_t>(budgetUs) * qpc_freq()) / 1000000LL;
    ++s.slices;

    MEMORY_BASIC_INFORMATION mbi{};
    while (s.cursor < kScanHigh) {
        if (VirtualQuery(reinterpret_cast<void*>(s.cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
            break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + mbi.RegionSize;
        if (end <= base) break; // wrapped: nothing sane left above

        const bool skip = !region_scannable(mbi) || s.excludes.overlaps(base, end) ||
                          preceded_by_guard_page(base);
        if (!skip) {
            const uintptr_t from = s.cursor > base ? s.cursor : base;
            const uintptr_t stopped =
                sweep_region(from, end, xorNeedle, needBytes, accept, user, &s, deadline);
            s.bytes += stopped - from;
            if (stopped < end) {
                s.cursor = stopped;
                s.elapsedUs += qpc_to_us(qpc_now() - start);
                return Outcome::Working;
            }
        }
        s.cursor = end;
        if (qpc_now() >= deadline && s.cursor < kScanHigh) {
            s.elapsedUs += qpc_to_us(qpc_now() - start);
            return Outcome::Working;
        }
    }

    s.elapsedUs += qpc_to_us(qpc_now() - start);
    s.cursor = 0; // next call starts a new pass
    ++s.passes;
    return Outcome::Complete;
}

bool heap_blocks(Sweep& s, const void* wantVtable, uint32_t needBytes, Accept accept, void* user) {
    s.reset();
    s.cursor = 0; // this path does not slice; a following sweep() starts clean
    if (!accept || !wantVtable || needBytes < sizeof(void*)) return false;

    const int64_t start = qpc_now();
    const uintptr_t xorNeedle = reinterpret_cast<uintptr_t>(wantVtable) ^ kNeedleMask;

    HANDLE heaps[96];
    DWORD n = GetProcessHeaps(static_cast<DWORD>(std::size(heaps)), heaps);
    if (n > std::size(heaps)) n = static_cast<DWORD>(std::size(heaps));
    for (DWORD i = 0; i < n; ++i) {
        if (!heaps[i]) continue;
        ++s.heaps;
        walk_one_heap(heaps[i], xorNeedle, needBytes, accept, user, &s);
    }

    s.elapsedUs = qpc_to_us(qpc_now() - start);
    ++s.passes;
    return s.first != nullptr;
}

} // namespace bvr::heap_scan
