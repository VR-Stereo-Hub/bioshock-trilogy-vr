#include "game/bioshockinf/frame_context.h"

#include "core/util/log.h"

#include <windows.h>

#include <atomic>

namespace bvr::bsi::frame_context {
namespace {

// A SEQLOCK, not a mutex: the game-thread writer must never block on a reader,
// and it runs 1400-9600 times a second. A seqlock rather than a published-index
// double buffer, because a reader descheduled mid-copy must be able to DETECT
// that it was lapped rather than silently hand back half of one frame and half
// of the next. The retry loop makes a torn snapshot impossible to CONSUME; the
// bounded spin makes it impossible to hang.
//
// THE FORMAL CAVEAT, stated rather than hidden: the reader's copy of g_ctx races
// with the writer's store by the letter of the standard. Four things make that
// safe here. (a) FrameContext is a trivially-copyable POD of scalars - no
// vtable, no allocation, no invariant spanning fields. (b) A torn value is never
// dereferenced: the only pointer, `pc`, is is_memory_valid-gated at every
// consumer regardless. (c) The retry loop discards any torn snapshot before use.
// (d) On Win32/MSVC these are plain aligned loads and stores. If a sanitizer
// ever flags it the escape hatch is a memcpy through volatile unsigned char*,
// which costs nothing at this size.
//
// The fences are free on x86 - acquire/release atomic_thread_fence compiles to a
// compiler barrier with no mfence. Their job is to stop the compiler sinking the
// odd tag below the body write, or hoisting the body read above the tag read.
alignas(64) std::atomic<uint32_t> g_seq{0};
FrameContext g_ctx{}; // plain POD; the seqlock is its only guard
std::atomic<uint32_t> g_writerTid{0};
std::atomic<uint32_t> g_publishes{0};
std::atomic<uint32_t> g_retries{0};
std::atomic<uint32_t> g_refusals{0};
std::atomic<uint32_t> g_foreignWrites{0};

} // namespace

void publish(const FrameContext& c) {
    const uint32_t tid = GetCurrentThreadId();
    uint32_t expect = 0;
    if (!g_writerTid.compare_exchange_strong(expect, tid, std::memory_order_relaxed) &&
        expect != tid) {
        // A second writer breaks the even/odd scheme outright. This must never
        // happen; if it does, the counter names it rather than the data going
        // quietly wrong.
        g_foreignWrites.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t s = g_seq.load(std::memory_order_relaxed);
    g_seq.store(s + 1, std::memory_order_relaxed); // -> ODD: writer inside
    std::atomic_thread_fence(std::memory_order_release);
    g_ctx = c;
    std::atomic_thread_fence(std::memory_order_release);
    g_seq.store(s + 2, std::memory_order_relaxed); // -> EVEN: published
    g_publishes.fetch_add(1, std::memory_order_relaxed);
}

bool read(FrameContext* out, uint32_t maxAgeMs) {
    if (!out) return false;
    for (int spin = 0; spin < 8; ++spin) {
        const uint32_t s0 = g_seq.load(std::memory_order_acquire);
        if (s0 & 1u) { // writer is inside
            g_retries.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const FrameContext tmp = g_ctx; // MAY BE TORN - not trusted yet
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_seq.load(std::memory_order_relaxed) != s0) { // lapped mid-copy
            g_retries.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // From here the snapshot is COHERENT. Only now may anything be believed.
        if (!tmp.vrDriving || !tmp.haveRecenter) {
            g_refusals.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (maxAgeMs != 0 && GetTickCount64() - tmp.stamp > maxAgeMs) {
            g_refusals.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        *out = tmp;
        return true;
    }
    // The writer is hot. REFUSE rather than hand back a guess.
    g_refusals.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void stats(uint32_t* publishes, uint32_t* retries, uint32_t* refusals,
           uint32_t* foreignWrites) {
    if (publishes) *publishes = g_publishes.load(std::memory_order_relaxed);
    if (retries) *retries = g_retries.load(std::memory_order_relaxed);
    if (refusals) *refusals = g_refusals.load(std::memory_order_relaxed);
    if (foreignWrites) *foreignWrites = g_foreignWrites.load(std::memory_order_relaxed);
}

} // namespace bvr::bsi::frame_context
