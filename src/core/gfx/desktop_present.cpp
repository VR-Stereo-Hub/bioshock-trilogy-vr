#include "desktop_present.h"
#include <atomic>

namespace bvr::desktop_present {
namespace {
std::atomic<bool> g_enabled{false}, g_rejected{false};
thread_local unsigned t_depth = 0;
thread_local bool t_submitted = false;
}

void configure(bool enabled) {
    g_rejected.store(false, std::memory_order_relaxed);
    g_enabled.store(enabled, std::memory_order_release);
}
Frame::Frame() {
    if (t_depth || g_enabled.load(std::memory_order_acquire)) {
        entered_ = true;
        if (++t_depth == 1) t_submitted = false;
    }
}
Frame::~Frame() {
    if (entered_ && --t_depth == 0) t_submitted = false;
}
void mark_xr_submission(bool submitted) {
    if (t_depth == 1) t_submitted = submitted;
}
Result present(PresentFn original, IDXGISwapChain* swapchain, UINT sync, UINT flags) {
    Result r;
    r.attempted = g_enabled.load(std::memory_order_acquire) &&
        !g_rejected.load(std::memory_order_relaxed) && t_depth == 1 &&
        t_submitted && sync == 0 && flags == 0;
    // Consume once, and do not let an unexpected nested call consume its parent.
    if (t_depth == 1) t_submitted = false;
    r.flags = r.attempted ? flags | DXGI_PRESENT_DO_NOT_WAIT : flags;
    r.first = r.value = original(swapchain, sync, r.flags);
    if (r.attempted && r.first == DXGI_ERROR_WAS_STILL_DRAWING) {
        r.skipped = true;
        r.value = S_OK; // the headset frame has already been submitted
    } else if (r.attempted && (r.first == DXGI_ERROR_INVALID_CALL ||
               r.first == DXGI_ERROR_UNSUPPORTED || r.first == E_INVALIDARG)) {
        g_rejected.store(true, std::memory_order_relaxed);
        r.rejected = true;
        r.value = original(swapchain, sync, flags); // original contract, one fallback
    }
    return r;
}
} // namespace bvr::desktop_present
