#pragma once
#include <dxgi.h>

namespace bvr::desktop_present {
using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);

// Default off. A game adapter opts in once, before Present hooks are installed.
void configure(bool enabled);

// A submission is evidence for this root Present on this thread only. It must
// never survive a menu, a lost session, another thread or a nested Present.
class Frame {
    bool entered_ = false;
public:
    Frame();
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};
// Caller has just successfully submitted a focused projection frame to XR.
// Empty frames, cinematic/flat quads, held half-pairs and failures do not qualify.
void mark_xr_submission(bool focusedProjectionSubmitted);

struct Result {
    HRESULT value = S_OK;  // result returned to the application
    HRESULT first = S_OK;  // raw result of the first desktop attempt
    UINT flags = 0;        // flags used for that attempt
    bool attempted = false, skipped = false, rejected = false;
};
// Only augments ordinary Present(0, 0), after a current XR submission. A busy
// desktop update is skipped, without retrying or delaying the next VR frame.
// Existing nonzero flags/intervals and genuine device errors retain their API
// contract. Unsupported flags disable this opt-in for the rest of the process.
Result present(PresentFn original, IDXGISwapChain* swapchain, UINT sync, UINT flags);
} // namespace bvr::desktop_present
