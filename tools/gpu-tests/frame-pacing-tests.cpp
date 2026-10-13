// gpu-test-sources: src/core/gfx/frame_pacing.cpp
// This host suite uses the existing multi-source harness, but creates no GPU,
// OpenXR session or game. It checks the production recorder and its real file IO.
#include "core/gfx/frame_pacing.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fp = bvr::frame_pacing;
unsigned checks = 0;
void check(bool ok, const char* what) {
    ++checks; if (!ok) { printf("FAIL: %s\n", what); exit(1); }
}
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v; std::istringstream in(s); std::string t;
    while (std::getline(in, t, ',')) v.push_back(t);
    return v;
}
int main() {
    check(!fp::active(), "default is inert");
    { fp::Present absent(0, 0, 0); check(!fp::active(), "disabled Present is inert"); }
    check(fp::start(L"frame-pacing-test.csv", "offline-test"), "start real writer");
    check(!fp::start(L"frame-pacing-duplicate.csv", "wrong"), "second start refuses");
    {
        fp::Present frame(1, 2, 7);
        check(fp::active(), "owner thread records");
        fp::xr_context(1000000000, 8333333, 5, true);
        fp::eye(2, 2, 0);
        { fp::Scope p(fp::DesktopPresent); Sleep(7); }
        { fp::Present nested(999, 0, 0);
          check(!fp::active(), "nested Present suppressed"); fp::add(fp::PresentLog, 900000); }
        check(fp::active(), "outer resumes after nested Present");
        std::thread competitor([] {
            fp::Present frame(998, 0, 0);
            if (fp::active()) std::abort();
            fp::add(fp::PresentLog, 800000);
        });
        competitor.join();
        fp::dlss(0.3, 0.7, 0.02, 1.8);
        fp::submission(0, 2);
        fp::desktop(8, static_cast<int>(0x887a000au), 0, true, true, false);
    }
    check(!fp::active(), "outside Present cannot charge a phase");
    fp::add(fp::PresentLog, 700000);
    Sleep(7);
    { fp::Present frame(2, 0, 0); fp::eye(1, 0, 1); }
    fp::stop();
    { fp::Present absent(3, 0, 0); check(!fp::active(), "stop disables producer"); }
    std::ifstream f("frame-pacing-test.csv");
    std::vector<std::string> names; std::vector<std::vector<std::string>> rows;
    std::string line; bool build = false, footer = false;
    while (std::getline(f, line)) {
        if (line.find("# frame-pacing-v2 build=offline-test") == 0) build = true;
        if (line.find("# end rows=2 dropped=1") == 0) footer = true;
        if (line.empty() || line[0] == '#') continue;
        if (names.empty()) names = split(line); else rows.push_back(split(line));
    }
    check(build && footer, "build banner, drain count and concurrent refusal persisted");
    check(rows.size() == 2, "exactly two root frames, no disabled or nested frames");
    auto val = [&](size_t row, const char* name) -> long long {
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == name) return std::stoll(rows[row].at(i));
        printf("FAIL missing column %s\n", name); exit(1);
    };
    check(rows[0].size() == names.size() && rows[1].size() == names.size(), "CSV schema complete");
    check(val(0,"sync_interval") == 2 && val(0,"present_flags") == 7, "original desktop flags preserved");
    check(val(0,"desktop_effective_flags") == 8 && val(0,"desktop_nonblocking") == 1,
          "effective desktop policy distinct from the original arguments");
    check(val(0,"desktop_first_result") == static_cast<int>(0x887a000au) && val(0,"desktop_result") == 0 &&
          val(0,"desktop_skipped") == 1 && val(0,"desktop_rejected") == 0, "busy desktop attempt remains visible in CSV");
    check(val(1,"desktop_nonblocking") == 0 && val(1,"desktop_skipped") == 0 && val(1,"desktop_first_result") == 0,
          "desktop outcome does not leak into the next frame");
    check(val(0,"stereo") == 2 && val(0,"upscaler") == 2 && val(1,"stereo") == 1, "mode switches have per-frame tags");
    check(val(0,"period_ns") == 8333333 && val(0,"predicted_ns") == 1000000000, "XR clock recorded without treating it as QPC");
    check(val(0,"desktop_present_us") >= 5000, "known desktop stall attributed");
    check(val(0,"present_us") >= val(0,"desktop_present_us"), "parent includes child span");
    check(val(1,"outside_us") >= 5000, "known between-frame stall attributed outside Present");
    check(std::llabs(val(1,"interval_us") - val(0,"present_us") - val(1,"outside_us")) <= 2,
          "interval equals previous Present duration plus following gap, not current Present duration");
    check(val(0,"present_log_us") == 0 && val(1,"present_log_us") == 0, "nested/other-thread/outside scopes cannot contaminate owner");
    check(val(0,"dlss_upload_us") == 300 && val(0,"dlss_reply_us") == 700 && val(0,"dlss_queue_wait_us") == 20, "DLSS phases separated");
    check(val(0,"dlss_delayed_gpu_us") == 1800 && val(1,"dlss_delayed_gpu_us") == -1, "delayed GPU sample resets to unknown per Present");
    check(val(0,"submitted") == 1 && val(0,"layers") == 2 && val(1,"submitted") == 0, "submission distinct from Present");
    check(val(0,"submit_us") >= val(0,"start_us"), "submission timestamp on same CPU clock");
    // Price the diagnostic itself: a synthetic stress stream includes 21 QPC
    // scopes and every phase add. Overflow deliberately drops rather than waits.
    check(fp::start(L"frame-pacing-overhead.csv", "offline-overhead"), "restart writer");
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    std::vector<double> cost; cost.reserve(4096);
    for (unsigned n = 0; n < 4096; ++n) {
        const int64_t start = fp::stamp();
        { fp::Present frame(n + 1, 0, 0);
          for (unsigned p = 0; p < fp::Count; ++p) {
              fp::Scope scope(static_cast<fp::Phase>(p)); fp::add(static_cast<fp::Phase>(p), 1);
          }
        }
        cost.push_back((fp::stamp() - start) * 1000000.0 / freq.QuadPart);
    }
    fp::stop(); std::sort(cost.begin(), cost.end());
    printf("      diagnostic CPU overhead (synthetic 21 scopes + enqueue): median %.3f us, p99 %.3f us, max %.3f us\n",
           cost[cost.size()/2], cost[cost.size()*99/100], cost.back());
    printf("frame pacing recorder: %u checks passed; no GPU, game or simulator used\n", checks);
}
