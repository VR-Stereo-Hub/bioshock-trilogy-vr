#pragma once
// One-shot D3D11 frame inspector (DR-3): hooks the immediate context's draw
// and clear vtable slots; when armed, records one full frame of draw events -
// render targets + formats, viewports, constant-buffer sizes/contents, SRV0,
// and return-address callstacks mapped to module+RVA - then writes a text
// dump next to the log and disarms. Costs one atomic load per draw call when
// idle. In-tree replacement for a RenderDoc frame map: the callstack RVAs are
// exactly what the SequentialReentry scene-draw hook needs.

#include <cstdint>

struct IDXGISwapChain;
struct ID3D11DeviceContext;
struct ID3D11DepthStencilView;
struct ID3D11RenderTargetView;
struct ID3D11Resource;
struct D3D11_BOX;

namespace bvr::frame_inspector {

// Hook the immediate-context vtable slots (fail-soft per slot). `ctxVtable`
// is the vtable pointer harvested from the game's context; `gameContext` is
// the game's immediate context itself. THE VTABLE IS SHARED BY EVERY D3D11
// CONTEXT IN THE PROCESS - the OpenXR runtime's own device's contexts included
// (Virtual Desktop's compositor threads were found inside our Unmap detour,
// 2026-10-10, six driver crashes) - so every detour does its work only for
// `gameContext` and passes any other context straight through. Null = no
// filter (the pre-fix behaviour; only for tests).
bool install(void** ctxVtable, ID3D11DeviceContext* gameContext);

// Arm a one-shot dump of the NEXT full Present-to-Present frame.
//   mode 1 = lite  - events and resources only, no constant-buffer readback
//   mode 2 = full  - plus a staging readback of the bound VS b0, ONCE PER
//                    DISTINCT BUFFER OBJECT. Correct for an engine that renames
//                    its constant buffer on every upload (Map/WRITE_DISCARD).
//   mode 3 = cb    - full, plus every UpdateSubresource into a constant buffer
//                    captured at its real size, plus the per-draw VS/PS
//                    constant-buffer identities. For engines that REUSE one
//                    buffer object and rewrite it, where mode 2 both
//                    under-samples and misattributes what it does capture.
//                    Added session 36 for BioShock Infinite (UE3, deferred).
// count > 1 records that many CONSECUTIVE present windows (files suffixed _qN)
// - required to see both halves of a stereo pair, since a game-thread arm
// always opens on the same pair phase.
void arm(int mode, int count = 1);

// Frame boundary, called at the head of the Present detour: finalizes and
// writes a recording frame, or begins a pending one.
void on_present(IDXGISwapChain* swapchain);

// ---- Session 34: an adapter-supplied per-DrawIndexed veto -------------------
// Return true to DROP the draw. Null by default, so a game that never
// registers one is on exactly the code path it was on before - which is the
// point: this is core code that only BioShock 2 turns on.
//
// It exists because a mesh cannot be identified any other way inside a single
// pass. BioShock 2's foreground pass is ~17 draws carrying the weapon AND the
// Big Daddy helmet; they share a lens, a render target and a callstack, so
// neither the fov watch nor a draw count can separate them. The index count
// can, and it is stable for a given mesh.
//
// Runs on the render thread, once per DrawIndexed, guarded by the same
// re-entrancy suppression as the recorder - so keep it to a couple of integer
// compares and no device calls.
using MeshSkipFn = bool (*)(unsigned indexCount);
void set_mesh_skip(MeshSkipFn fn);
unsigned mesh_skips();

// ---- Temporal seam: a draw tap for the scene depth ---------------------------
// The adapter's DLSS/DLAA and alternate-frame-warp passes need the game's scene depth,
// which only the draw stream can identify (the depth surface the world's DrawIndexed
// calls are made against). Null by default - a game that never sets one runs exactly the
// detours it ran before. Only the game's own calls are reported (never the mod's, which
// run under ScopedSuppress or through the original functions). Render thread; keep the
// callbacks cheap.
struct DrawTap {
    // AFTER the game's OMSetRenderTargets (the new depth binding is live).
    void (*on_setrt)(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv) = nullptr;
    // BEFORE each game DrawIndexed (the world renders through DrawIndexed only).
    void (*on_draw_indexed)(ID3D11DeviceContext* ctx) = nullptr;
    // BEFORE the game's ClearDepthStencilView: the last chance to keep what it clears.
    void (*on_clear_dsv)(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, unsigned flags) = nullptr;
};
void set_draw_tap(const DrawTap* tap);

// Independent read-only diagnostic observer. All callbacks run under suppression,
// on the game's exact immediate context only. Default null for every adapter.
// Kept separate from DrawTap so an audit cannot replace the temporal depth tap.
enum class DrawKind { Direct, Indexed, Instanced, IndexedInstanced };
struct DrawArgs {
    DrawKind kind;
    unsigned count, start;
    int baseVertex;
    unsigned instances = 1, startInstance = 0;
};
struct DrawObserver {
    void (*on_draw)(ID3D11DeviceContext*, const DrawArgs&) = nullptr;
    void (*on_present)(ID3D11DeviceContext*) = nullptr;
    void (*on_other)(ID3D11DeviceContext*, const char*) = nullptr;
};
void set_draw_observer(const DrawObserver* observer);

// Opt-in resource events for an adapter-owned immediate replay stream. Null by
// default. Callbacks run suppressed on the exact game context; Map is reported
// after success, Unmap before its pointer expires, other writes before execution.
// Callback lifetime is static. No callback may throw or change game bindings.
struct ResourceObserver {
    void (*on_map)(ID3D11DeviceContext*, ID3D11Resource*, unsigned sub, unsigned mapType, void* data) = nullptr;
    void (*on_unmap)(ID3D11DeviceContext*, ID3D11Resource*, unsigned sub) = nullptr;
    void (*on_update)(ID3D11DeviceContext*, ID3D11Resource*, unsigned sub, const D3D11_BOX*,
                      const void* data, unsigned rowPitch, unsigned depthPitch) = nullptr;
    void (*on_copy)(ID3D11DeviceContext*, ID3D11Resource* dst, unsigned dstSub, unsigned x, unsigned y, unsigned z,
                    ID3D11Resource* src, unsigned srcSub, const D3D11_BOX*, bool whole) = nullptr;
    void (*on_clear_rtv)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const float[4]) = nullptr;
    void (*on_clear_dsv)(ID3D11DeviceContext*, ID3D11DepthStencilView*, unsigned, float, unsigned char) = nullptr;
    void (*on_unknown)(ID3D11DeviceContext*, const char*) = nullptr;
    void (*on_reset)(ID3D11DeviceContext*) = nullptr;
};
void set_resource_observer(const ResourceObserver* observer);
bool resource_observer_hooks_ready();
// Release adapter-owned references before ResizeBuffers. Exact-context filter.
void on_resource_reset(ID3D11DeviceContext* ctx);


// Independent opt-in scratch-render diagnostic. Static callback lifetime is
// required: a successful before_draw is paired with that SAME object's after_draw,
// even if registration changes during the original game draw. Both run suppressed
// on the exact game context, after HUD redirects/vetoes. Default null.
// Callbacks must restore any bindings they change and must not throw.
struct DrawReplayProbe {
    bool (*before_draw)(ID3D11DeviceContext*, const DrawArgs&) = nullptr;
    void (*after_draw)(ID3D11DeviceContext*, const DrawArgs&) = nullptr;
    void (*on_present)(ID3D11DeviceContext*) = nullptr;
};
void set_draw_replay_probe(const DrawReplayProbe* probe);
// Issue through the originals, without re-entering the HUD, tap, census or probe.
// Accepted only inside a suppressed callback on the installed game context.
bool replay_draw(ID3D11DeviceContext* ctx, const DrawArgs& args);
// Query ranges observed since hook installation: replay would otherwise change
// an engine occlusion/statistics query. False if optional query hooks failed.
bool replay_queries_idle(ID3D11DeviceContext* ctx);



// Suppress capture of our own rendering (ImGui, VR copies) on this thread
// for the current scope. Used by the Present detour after on_present().
struct ScopedSuppress {
    ScopedSuppress();
    ~ScopedSuppress();
};

// Always-on constant-buffer watch (session 13): hooks Map/Unmap and, on every
// WRITE-mapped buffer unmap, matches `pattern` (patCount floats compared at
// float offset patFirst with tolerance) against the staged bytes; on a match
// the floats [capFirst, capFirst+capCount) are copied into a seqlocked slot.
// Cost per unmap on a miss is one float compare. Built for the BioShock
// foreground-scene transform capture (the vm draws' cb0 carries a constant
// tan-block fingerprint), but game-agnostic: the adapter supplies the floats.
// capCount is capped at 32.
// requiredBytes: exact buffer ByteWidth to accept (0 = any) - constant-buffer
// tiers reuse layouts, so the same fingerprint can select different content.
void set_cb_watch(const float* pattern, uint32_t patFirst, uint32_t patCount,
                  uint32_t capFirst, uint32_t capCount, uint32_t requiredBytes);

// Copy the latest watch capture into `out` (capCount floats). Returns false
// if nothing was captured yet. `ageMs` (optional) gets the capture age.
bool latest_cb_watch(float* out, uint32_t count, uint64_t* ageMs);

// Lifetime accepted-capture count (diagnostic: is the watch firing at all?).
uint32_t cb_watch_hits();

// s87: log the next n WRITE-mapped VERTEX-buffer unmaps (ByteWidth, map type,
// Unmap callstack as exe RVAs). Opt-in, off by default; a game that never
// calls it is on exactly the code path it was on before. BioShock 1 uses it
// to find its CPU skinner's upload (the hands draws bind no bone palette).
void vb_tap_log(int n);

// Log the Unmap-time callstack (game-exe RVAs) for the next n fingerprint
// matches - the discovery instrument for whoever BUILDS the watched values.
void cb_watch_log_stacks(int n);

// ---- Opt-in UpdateSubresource constant-buffer tap (session 41, BSI I6) ------
// The always-on cb watch above taps Map/Unmap - right for the Vengeance
// remasters, which stage per-draw constants through WRITE_DISCARD maps.
// BioShock Infinite (UE3 6829) uploads constant buffers through
// UpdateSubresource instead (the whole reason dumpframe mode 3 exists), so a
// LIVE lens decoder there needs this tap: when armed, every strideN-th
// UpdateSubresource has its destination checked (constant buffer of exactly
// requiredBytes, whole-buffer or offset-0 update) and the first kCbTapFloats
// floats of the payload go into a small seqlock-stamped ring the game thread
// drains. The tap captures RAW floats only - no decode, no policy, no game
// knowledge; all of that lives in the consuming adapter.
// DISARMED COST IS ONE RELAXED ATOMIC LOAD per UpdateSubresource. No game
// arms it by default and only the bioshockinf adapter calls the setter, so
// BS1/BS2 behaviour is untouched (observable: cb_upload_tap_count() stays 0
// for the whole life of an unarmed session).
inline constexpr uint32_t kCbTapFloats = 20; // 80 B: a row-major 4x4 + spare
inline constexpr uint32_t kCbTapSlots = 64;
void set_cb_upload_tap(uint32_t requiredBytes, uint32_t strideN); // bytes 0 = disarm
// Drain samples newer than *cursor (opaque, start at 0) into `out`
// (maxSamples x kCbTapFloats floats). Returns samples written; advances
// *cursor. Overwritten ring slots are skipped, not invented.
uint32_t drain_cb_upload_samples(float* out, uint32_t maxSamples, uint32_t* cursor);
uint32_t cb_upload_tap_count(); // lifetime accepted samples (diagnostic)

// Overlay section: dump buttons + last-dump status.
void draw_debug_ui();

// Lifetime draw-call census across all four draw slots. The reentry probe
// (DR-5) snapshots this around a re-entered frame-root call: a second-call
// delta comparable to a full frame's draws proves a real second scene render.
uint64_t draw_call_census();

} // namespace bvr::frame_inspector
