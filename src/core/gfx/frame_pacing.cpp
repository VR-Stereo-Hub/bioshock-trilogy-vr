#include "frame_pacing.h"
#include "frame_pacing_queue.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace bvr::frame_pacing {
namespace {
struct Sample {
    uint64_t seq = 0, startUs = 0, submitUs = 0;
    int64_t predictedNs = 0, periodNs = 0;
    uint32_t intervalUs = 0, outsideUs = 0, presentUs = 0;
    uint32_t sync = 0, flags = 0, submitted = 0, layers = 0, dropped = 0;
    int32_t state = 0, render = 0, stereo = -1, upscaler = -1, eyeIndex = -1;
    int32_t submitResult = 0, dlssGpuUs = -1;
    uint32_t desktopFlags = 0, desktopAttempted = 0, desktopSkipped = 0, desktopRejected = 0;
    int32_t desktopFirst = 0, desktopResult = 0;
    uint32_t us[Count]{};
};
const char* const kNames[Count] = {
    "inspector", "letterbox", "vr_begin", "overlay", "vr_end", "mirror", "hud", "debug", "desktop_present",
    "xr_wait", "xr_begin", "xr_locate", "xr_acquire", "xr_capture", "xr_end", "xr_composite",
    "temporal", "dlss_upload", "dlss_reply", "dlss_queue_wait", "present_log"
};
SampleQueue<Sample, 512> g_queue;
std::atomic<bool> g_enabled{false}, g_run{false};
// Serializes producers by refusal, never by waiting. The engine normally has
// exactly one render thread; an unexpected concurrent Present is counted/lost.
std::atomic_flag g_owner = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> g_dropped{0};
HANDLE g_thread = nullptr;
FILE* g_file = nullptr; // owned by the worker after start()
int64_t g_frequency = 1, g_origin = 0, g_prevStart = 0, g_prevEnd = 0;
thread_local unsigned t_depth = 0;
thread_local int64_t t_start = 0;
thread_local Sample t_sample;
constexpr uint32_t kRowCap = 120000; // bounded disk use, roughly 17 min at 120 Hz

uint64_t elapsed_long_us(int64_t start, int64_t end) {
    if (end <= start) return 0;
    const uint64_t ticks = static_cast<uint64_t>(end - start);
    const uint64_t hz = static_cast<uint64_t>(g_frequency);
    return (ticks / hz) * 1000000u + (ticks % hz) * 1000000u / hz;
}
uint32_t elapsed_us(int64_t start, int64_t end) {
    const uint64_t us = elapsed_long_us(start, end);
    return us > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(us);
}
uint32_t ms_us(double ms) {
    if (!(ms > 0.0)) return 0;
    return ms >= UINT32_MAX / 1000.0 ? UINT32_MAX : static_cast<uint32_t>(ms * 1000.0 + 0.5);
}
void write_sample(const Sample& s) {
    char line[1600];
    int n = sprintf_s(line,
        "%llu,%llu,%u,%u,%u,%llu,%lld,%lld,%d,%d,%u,%u,%d,%u,%u,%d,%d,%d,%d,%u,%u,%d,%d,%u,%u,%u",
        static_cast<unsigned long long>(s.seq), static_cast<unsigned long long>(s.startUs),
        s.intervalUs, s.outsideUs, s.presentUs, static_cast<unsigned long long>(s.submitUs),
        static_cast<long long>(s.predictedNs), static_cast<long long>(s.periodNs), s.state, s.render,
        s.submitted, s.layers, s.submitResult, s.sync, s.flags, s.stereo, s.upscaler, s.eyeIndex,
        s.dlssGpuUs, s.dropped, s.desktopFlags, s.desktopFirst, s.desktopResult,
        s.desktopAttempted, s.desktopSkipped, s.desktopRejected);
    for (unsigned i = 0; i < Count && n > 0; ++i)
        n += sprintf_s(line + n, sizeof(line) - n, ",%u", s.us[i]);
    if (n > 0) { line[n++] = '\n'; line[n] = 0; fputs(line, g_file); }
}
DWORD WINAPI writer(void*) {
    uint32_t rows = 0;
    bool failed = false;
    for (;;) {
        Sample s;
        while (rows < kRowCap && g_queue.pop(s)) { write_sample(s); ++rows; }
        // All IO is here, including flushing. Never publish a render-thread log
        // for a hitch: recording the event must not introduce the next hitch.
        if (fflush(g_file) || ferror(g_file)) { failed = true; g_enabled.store(false); break; }
        if (rows == kRowCap) { g_enabled.store(false); break; }
        if (!g_run.load(std::memory_order_acquire)) break;
        Sleep(250);
    }
    fprintf(g_file, "# end rows=%u dropped=%u cap=%u io_error=%d\n", rows,
            g_dropped.load(std::memory_order_relaxed), kRowCap, failed ? 1 : 0);
    fclose(g_file);
    g_file = nullptr;
    return 0;
}
} // namespace

int64_t stamp() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
bool active() { return t_depth == 1; }

bool start(const wchar_t* path, const char* build) {
    // Called once by the framework init thread, before hooks start presenting.
    if (g_thread || !path || !build) return false;
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return false;
    g_frequency = f.QuadPart; g_origin = stamp(); g_prevStart = g_prevEnd = 0;
    g_dropped.store(0);
    Sample stale; while (g_queue.pop(stale)) {} // restart after a bounded/failed trace
    g_file = _wfsopen(path, L"wb", _SH_DENYNO);
    if (!g_file) return false;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_file, "# frame-pacing-v2 build=%s local_start=%04u-%02u-%02uT%02u:%02u:%02u.%03u qpc_hz=%lld row_cap=%u\n",
        build, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        static_cast<long long>(g_frequency), kRowCap);
    fprintf(g_file, "# CPU durations only; nested columns overlap; outside_us belongs to the gap BEFORE this Present; submit_us marks xrEndFrame RETURN, not display.\n");
    fprintf(g_file, "seq,start_us,interval_us,outside_us,present_us,submit_us,predicted_ns,period_ns,xr_state,should_render,submitted,layers,submit_result,sync_interval,present_flags,stereo,upscaler,eye,dlss_delayed_gpu_us,dropped,desktop_effective_flags,desktop_first_result,desktop_result,desktop_nonblocking,desktop_skipped,desktop_rejected");
    for (const char* name : kNames) fprintf(g_file, ",%s_us", name);
    fputc('\n', g_file); fflush(g_file);
    g_run.store(true, std::memory_order_release);
    g_enabled.store(true, std::memory_order_release);
    g_thread = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr);
    if (!g_thread) { g_enabled.store(false); g_run.store(false); fclose(g_file); g_file = nullptr; return false; }
    return true;
}
void stop() {
    // Never call from loader lock or while a producer is inside a measured Present.
    g_enabled.store(false, std::memory_order_release);
    g_run.store(false, std::memory_order_release);
    if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = nullptr; }
}
unsigned begin_present(uint64_t sequence, unsigned syncInterval, unsigned flags) {
    if (t_depth) { ++t_depth; return 2; }
    if (!g_enabled.load(std::memory_order_acquire)) return 0;
    if (g_owner.test_and_set(std::memory_order_acquire)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed); return 0;
    }
    t_depth = 1; t_start = stamp(); t_sample = {};
    t_sample.seq = sequence; t_sample.sync = syncInterval; t_sample.flags = flags;
    t_sample.startUs = elapsed_long_us(g_origin, t_start);
    if (g_prevStart) t_sample.intervalUs = elapsed_us(g_prevStart, t_start);
    if (g_prevEnd) t_sample.outsideUs = elapsed_us(g_prevEnd, t_start);
    return 1;
}
void end_present(unsigned token) {
    if (token == 2) { --t_depth; return; }
    if (token != 1) return;
    const int64_t end = stamp();
    t_sample.presentUs = elapsed_us(t_start, end);
    t_sample.dropped = g_dropped.load(std::memory_order_relaxed);
    if (!g_queue.push(t_sample)) g_dropped.fetch_add(1, std::memory_order_relaxed);
    g_prevStart = t_start; g_prevEnd = end; t_depth = 0;
    g_owner.clear(std::memory_order_release);
}
void add(Phase phase, uint32_t us) {
    if (!active() || phase >= Count) return;
    const uint64_t total = static_cast<uint64_t>(t_sample.us[phase]) + us;
    t_sample.us[phase] = total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
}
void span(Phase phase, int64_t begin) { if (active()) add(phase, elapsed_us(begin, stamp())); }
void xr_context(int64_t predictedNs, int64_t periodNs, int state, bool shouldRender) {
    if (!active()) return;
    t_sample.predictedNs = predictedNs; t_sample.periodNs = periodNs;
    t_sample.state = state; t_sample.render = shouldRender ? 1 : 0;
}
void submission(int result, unsigned layers) {
    if (!active()) return;
    t_sample.submitResult = result; t_sample.layers = layers;
    t_sample.submitted = result >= 0 && layers ? 1u : 0u;
    t_sample.submitUs = elapsed_long_us(g_origin, stamp());
}
void desktop(unsigned flags, int first, int result, bool attempted, bool skipped, bool rejected) {
    if (!active()) return;
    t_sample.desktopFlags = flags; t_sample.desktopFirst = first; t_sample.desktopResult = result;
    t_sample.desktopAttempted = attempted ? 1u : 0u; t_sample.desktopSkipped = skipped ? 1u : 0u;
    t_sample.desktopRejected = rejected ? 1u : 0u;
}
void eye(int stereo, int upscaler, int eyeIndex) {
    if (!active()) return;
    t_sample.stereo = stereo; t_sample.upscaler = upscaler; t_sample.eyeIndex = eyeIndex;
}
void dlss(double uploadMs, double replyMs, double queueWaitMs, double gpuMs) {
    if (!active()) return;
    add(DlssUpload, ms_us(uploadMs)); add(DlssReply, ms_us(replyMs)); add(DlssQueueWait, ms_us(queueWaitMs));
    // The helper reports a retired allocator slot, NOT this frame's GPU time.
    if (gpuMs >= 0.0 && gpuMs < INT32_MAX / 1000.0) t_sample.dlssGpuUs = static_cast<int32_t>(ms_us(gpuMs));
}
} // namespace bvr::frame_pacing
