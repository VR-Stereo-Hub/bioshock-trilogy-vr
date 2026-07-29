#pragma once
// Safe, bounded search of the process's private committed memory for a live
// engine object identified by its class vtable. Game-agnostic: no game names,
// no addresses. Per-game accept filters live in game/<title>/patterns.cpp.
//
// WHY THIS MODULE EXISTS (session 27 crash post-mortem). The per-game scanner
// it replaces walked the whole 4 GB address space at a 4-byte stride in one
// blocking call on the game thread, accepting any MEM_PRIVATE + PAGE_READWRITE
// region. Three consequences, all observed in the field:
//
//   1. Thread stacks, TEBs and PEBs ARE MEM_PRIVATE + PAGE_READWRITE, so they
//      were swept - the filter excluded MEM_IMAGE/MEM_MAPPED, not stacks. A
//      stack slot that passed the accept filter would then be written to every
//      frame by the consumer.
//   2. The needle was an ordinary function argument, so it was spilled onto the
//      scanning thread's own stack and the sweep matched its own argument. That
//      is the two "vtable match" lines 0x40 apart just below esp in the
//      reporter's log.
//   3. One pass cost 1-4 s of blocked game thread, felt as "the game freezes
//      every couple of seconds".
//
// All three are addressed here: stacks and TEBs are excluded exactly (plus a
// guard-page heuristic as a second line), the needle only ever exists XOR-
// masked inside the inner loop, and a pass is sliced across frames under a
// wall-clock budget so no single call can stall a frame.
//
// THREADING: one Sweep belongs to one caller on one thread. Nothing here is
// internally synchronized.

#include <cstdint>

namespace bvr::heap_scan {

// Regions the sweep must never read: every thread's stack allocation and TEB.
// Refreshed at the start of each pass, since threads come and go.
constexpr int kMaxExcludes = 128;

struct ExcludeSet {
    struct Span {
        uintptr_t lo;
        uintptr_t hi; // exclusive
    };
    Span spans[kMaxExcludes];
    int count = 0;
    bool truncated = false; // more threads than kMaxExcludes: fail loud, not silent

    bool overlaps(uintptr_t lo, uintptr_t hi) const;
};

// Every thread stack allocation and TEB page in this process. Best-effort: a
// thread we cannot open is reported via `missed` so the caller can log it
// rather than silently sweeping that stack.
void snapshot_excludes(ExcludeSet& out, int* missed = nullptr);

// Candidates recorded for the caller to log AFTER the guarded region. Logging
// from inside an SEH __try is a deadlock hazard: MSVC under /EHsc does not run
// C++ destructors during SEH unwinding, so a fault taken while the log mutex
// is held leaves it held for the life of the process.
constexpr int kMaxRecorded = 12;

// Diagnostic integers an accept filter may hand back per candidate. Integers
// rather than a formatted string on purpose: formatting inside the guarded
// region is the thing this mechanism exists to avoid. Each callsite knows what
// its own slots mean and formats them after the search returns.
constexpr int kProbeSlots = 4;

enum class Outcome {
    Working,  // budget spent, cursor advanced, call again
    Complete, // a full pass finished; read `accepted` / `first`
};

// Per-callsite sweep state. Zero-initialize once and keep it; the sweep owns
// every field. `reset()` starts a fresh pass.
//
// The exclusion snapshot is taken once per PASS and cached here, not per slice:
// enumerating threads costs more than a slice's worth of comparing, and a pass
// is short enough that a thread created mid-pass cannot move an existing stack.
struct Sweep {
    ExcludeSet excludes;    // refreshed at pass start
    uintptr_t cursor = 0;   // next address to examine (0 = start a new pass)
    uint32_t passes = 0;    // completed passes
    uint32_t slices = 0;    // budget slices spent on the current pass
    uint64_t elapsedUs = 0; // wall clock spent on the current pass
    uint64_t bytes = 0;     // bytes examined this pass

    int matches = 0;  // vtable hits this pass
    int accepted = 0; // accept() said yes this pass (>1 means ambiguous)
    void* first = nullptr;

    int recorded = 0;
    void* addr[kMaxRecorded] = {};
    int32_t probe[kMaxRecorded][kProbeSlots] = {};
    bool ok[kMaxRecorded] = {};

    int excludeSpans = 0;  // spans in force this pass, for the log line
    int excludeMissed = 0; // threads we could not query (-1 = none at all)

    uint32_t blocks = 0; // busy heap blocks examined (heap_blocks only)
    uint32_t heaps = 0;  // heaps walked (heap_blocks only)

    void reset();
};

// Called for every object whose first dword is the wanted vtable. Decides
// whether this instance is the one wanted - every UClass also has a default
// object carrying the same vtable, so a plausibility test on the object's own
// fields is mandatory. Runs INSIDE the sweep's SEH guard: it may read up to
// `needBytes` from `obj`, must not throw, and must not log or allocate.
// `probe` receives kProbeSlots diagnostic integers recorded for the caller.
using Accept = bool (*)(void* obj, void* user, int32_t probe[kProbeSlots]);

// Advance `s` by at most `budgetUs` microseconds of wall clock. Returns
// Working while the pass is unfinished. `wantVtable` is the absolute address of
// the class vtable; `needBytes` is how much of the object the accept filter
// reads.
Outcome sweep(Sweep& s, const void* wantVtable, uint32_t needBytes, Accept accept, void* user,
              uint32_t budgetUs);

// FAST PATH, and the one to prefer. Enumerates the BUSY blocks of every heap in
// the process and tests each block large enough to hold the object, instead of
// every 4-byte-aligned address in every private region. Two wins over sweep():
//
//   - Cost. A sweep reads 1-2 GB per pass and measured 1-4 s of blocked game
//     thread; this touches a handful of dwords per allocation and completes in
//     tens of milliseconds, so it needs no slicing at all.
//   - Liveness. HeapWalk only reports allocated blocks, so a freed-but-not-yet-
//     reused block cannot be returned. That matters because the consumer WRITES
//     to what it gets back, and writing an int into a freed block smashes the
//     allocator's bookkeeping - the leading explanation for the field
//     0xC0000374 reports.
//
// Returns false if the object was not found, in which case the caller should
// fall back to sweep(): an engine that allocates objects out of its own
// VirtualAlloc'd pools rather than a Win32 heap is invisible here. `s` is
// filled the same way sweep() fills it, plus `blocks`/`heaps` for the log line.
// Never logs or allocates while a heap lock is held.
bool heap_blocks(Sweep& s, const void* wantVtable, uint32_t needBytes, Accept accept, void* user);

} // namespace bvr::heap_scan
