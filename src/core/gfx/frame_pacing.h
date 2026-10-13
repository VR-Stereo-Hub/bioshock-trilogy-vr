#pragma once
#include <cstdint>

namespace bvr::frame_pacing {
// CPU spans only. Child spans overlap their parents: never sum every column.
// No GPU queries, GPU waits, file writes or thread suspension in the producer.
enum Phase : unsigned {
    Inspector, Letterbox, VrBegin, Overlay, VrEnd, Mirror, Hud, Debug, DesktopPresent,
    XrWait, XrBeginFrame, XrLocate, XrAcquire, XrCapture, XrEndFrame, XrComposite,
    Temporal, DlssUpload, DlssReply, DlssQueueWait, PresentLog, Count
};

// Startup opt-in. Only BS1 calls this, when its installed INI requests it.
// The caller supplies a unique path, an existing directory and a stable build id.
bool start(const wchar_t* path, const char* build);
// Outside DllMain, only after producers stop; intended for the standalone test.
void stop();
bool active(); // this thread owns a measured Present; nested Presents are suppressed
int64_t stamp();
void add(Phase phase, uint32_t us);
void span(Phase phase, int64_t begin);
void xr_context(int64_t predictedNs, int64_t periodNs, int state, bool shouldRender);
void submission(int result, unsigned layers);
void desktop(unsigned effectiveFlags, int firstResult, int appResult,
             bool attempted, bool skipped, bool rejected);
void eye(int stereo, int upscaler, int eyeIndex); // stereo: 0 other, 1 sequential, 2 AFW
void dlss(double uploadMs, double replyMs, double queueWaitMs, double gpuMs);
unsigned begin_present(uint64_t sequence, unsigned syncInterval, unsigned flags);
void end_present(unsigned token);

class Present {
    unsigned token_;
public:
    Present(uint64_t sequence, unsigned syncInterval, unsigned flags)
        : token_(begin_present(sequence, syncInterval, flags)) {}
    ~Present() { end_present(token_); }
    Present(const Present&) = delete;
    Present& operator=(const Present&) = delete;
};
class Scope {
    Phase phase_;
    int64_t begin_;
public:
    explicit Scope(Phase p) : phase_(p), begin_(active() ? stamp() : 0) {}
    ~Scope() { if (begin_) span(phase_, begin_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
} // namespace bvr::frame_pacing
