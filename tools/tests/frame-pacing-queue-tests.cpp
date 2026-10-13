#include "core/gfx/frame_pacing_queue.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

void check(bool ok, const char* what) { if (!ok) { printf("FAIL: %s\n", what); exit(1); } }
struct Item { uint32_t seq = 0, inverse = 0; };
int main() {
    bvr::frame_pacing::SampleQueue<Item, 4> q;
    Item out{99, 0};
    check(!q.pop(out) && out.seq == 99, "empty pop must fail without modifying output");
    for (uint32_t i = 0; i < 4; ++i) check(q.push({i, ~i}), "fill exact capacity");
    check(!q.push({50, 50}), "negative control: full queue refuses, no overwrite or wait");
    for (uint32_t i = 0; i < 4; ++i)
        check(q.pop(out) && out.seq == i && out.inverse == ~i, "full refusal preserved FIFO and payload");
    check(!q.pop(out), "drained queue is empty");
    constexpr uint32_t count = 250000;
    std::thread producer([&] {
        for (uint32_t i = 0; i < count; ++i) while (!q.push({i, ~i})) std::this_thread::yield();
    });
    for (uint32_t i = 0; i < count; ++i) {
        while (!q.pop(out)) std::this_thread::yield();
        check(out.seq == i && out.inverse == ~i, "concurrent wraparound: ordered complete payload");
    }
    producer.join();
    check(!q.pop(out), "concurrent queue drained exactly once per sample");
    puts("frame pacing queue: full/empty/refusal and 250000 concurrent ordered samples passed");
}
