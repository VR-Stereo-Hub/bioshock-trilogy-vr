#include "game/bioshockinf/camera.h"

#include "game/bioshockinf/aim.h"

#include "core/framework/command.h"
#include "core/gfx/hud_capture.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "game/bioshockinf/config.h"
#include "game/bioshockinf/frame_context.h"
#include "game/bioshockinf/game_ini.h"
#include "game/bioshockinf/hands.h"
#include "game/bioshockinf/inf_math.h"
#include "game/bioshockinf/input_drive.h"
#include "game/bioshockinf/lens.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/recorder.h"
#include "game/bioshockinf/reflect.h"
#include "game/bioshockinf/scenedraw.h"

#include <MinHook.h>
#include <imgui.h>
#include <intrin.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <share.h>
#include <string>

namespace bvr::bsi::camera {
namespace {

// The UE3 PODs (FVector/FRotator), rotator constants and the XR<->UE mapping
// live in inf_math.h - adapter-local on purpose, NOT game/shared/ue_math.h:
// that header states it is the Vengeance/UE2.5 family's convention, and this
// game's patterns.h states that on UE3 even shapes are suspect. Same layout by
// coincidence is not the same layout by contract.

// APlayerController::GetPlayerViewPoint is __thiscall. __fastcall with a dummy
// EDX slot is register-, stack- and cleanup-identical, and works as a plain
// free function.
//
// EXACTLY TWO STACK ARGS. The target is `ret 8`, and `ret imm / 4 == 2` is a
// hard requirement: a mismatch returns on a misaligned stack and pops a
// "Run-Time Check Failure #0 - ESP was not properly saved" dialog which writes
// NO crash dump (RTC is a Debug compiler check, not an SEH fault). ONE typedef
// serves both the trampoline pointer and the detour so the two cannot disagree.
using GetViewPointFn = void(__fastcall*)(void* self, void* edx, FVector* loc, FRotator* rot);

GetViewPointFn g_original = nullptr;
void* g_target = nullptr;
std::atomic<bool> g_hookLive{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_fired{false};
std::atomic<bool> g_loggedFirstFire{false};
std::atomic<uint32_t> g_callCount{0};
std::atomic<uint64_t> g_lastCallMs{0};

// Heartbeat state. Game thread only - it is only ever touched inside the
// throttled block, which the tid latch confines to one thread.
std::atomic<bool> g_heartbeat{true};
int g_beatsLeft = 10; // self-expiring burst; `bsicam heartbeat on` re-arms it
uint64_t g_lastBeatMs = 0;
uint32_t g_beatBaseCount = 0;

// ---------------------------------------------------------------------------
// I4 drive state (session 39). The drive writes ONLY the detour's out-params -
// see camera.h. Atomics are the overlay/command edges; everything else is
// game thread only (the tid latch confines the drive to one thread).
// ---------------------------------------------------------------------------
std::atomic<bool> g_driveEnabled{false}; // every new render lever ships OFF
// Unreal units per meter. 50 is UE3's CANONICAL scale (1 uu = 2 cm), NOT
// BS1/BS2's calibrated 100 (Vengeance is a different engine; never copy a
// number between games). The headset session calibrates the real value via
// the F10 slider; the flat battery only proves the code applies whatever is
// set here.
std::atomic<float> g_worldScale{50.0f};
std::atomic<bool> g_recenterRequested{true}; // auto-recenter on first drive
std::atomic<bool> g_vrDriving{false};        // telemetry for the UI
// Head-offset telemetry: the recenter-relative offset applied to loc this
// frame, in UU - makes the world-scale value's effect a number in the log,
// which is what the flat 6DoF check measures.
std::atomic<float> g_headOffX{0.0f}, g_headOffY{0.0f}, g_headOffZ{0.0f};

// Game thread only.
bool g_haveRecenter = false;
bvr::vr::HeadPose g_recenterPose{};
// The seated frame's yaw zero, in ROTATOR UNITS (65536/turn), integer for
// exactness - wrap_rot subtraction on integers is exact, floats drift.
int32_t g_recenterYawUnits = 0;
float recenter_yaw_rad() { return g_recenterYawUnits / kRotUnitsPerRadian; }

// The head-vs-engine pitch error, computed against the engine's own pitch
// BEFORE the drive overwrites the out-param. It exists because the drive
// heartbeat reports the FINAL rot by design - so it shows the head's pitch,
// never the engine's, and could not by itself show the engine's pitch stuck
// (the BS1 -88.9 lesson; the engine base itself is on the heartbeat as
// engineRot, from the pre-drive snapshot). NOTE: unlike BS2, the error is
// LOGGED ONLY - it is never published to the input bridge in I4
// (publish_vr_gameplay would arm the shared pitch kill and seize right-stick
// Y; that lane is I7's).
float g_pitchErrDeg = 0.0f;

// What the last drive actually did, for the heartbeat. Game thread only.
const char* g_driveLane = "off";
FVector g_finalLoc{};
FRotator g_finalRot{};
bool g_finalValid = false;

// ---------------------------------------------------------------------------
// I5 stereo state (session 40). Rung 1 is the projection flip: the adapter
// finally feeds core an HONEST fov claim and asks for camera mode, and core
// swaps the quad for a projection layer (openxr_runtime projectionMode).
// ---------------------------------------------------------------------------
// The claimed VERTICAL half-tangent. The I2/I37 law: the game's FOV option is
// vertical-referenced and tanH = tanV x aspect at any aspect (measured at two
// aspects, ENGINE_NOTES "The FOV law"). Default = the slider at MINIMUM (the
// shipped default position, tanV pinned 0.4317). There is no live option
// reader yet (that lever is I6's); if the user moves the in-game slider the
// claim goes stale - correct it with `bsifov tanv <v>` and verify against a
// `dumpframe cb` decode. A WRONG CLAIM POISONS EVERY STEREO JUDGMENT (BS1's
// M4), which is why this is loud in the log and visible in the overlay.
std::atomic<float> g_claimTanV{patterns::kTanVSliderMin};
// I6 rung 1: THE FOV LEVER. Horizontal degrees at the current aspect; 0 = off.
// The engine recomputes its FOV caches from the option every tick (patterns.h
// "the live FOV chain"), so the lever ENFORCES per detour dispatch - writing
// [cam+0x214] and [cam+0x3D0] each call outruns the refresh, and disarming
// self-restores (the recompute is the undo). While armed the claim DERIVES
// from the lever (tanV = tan(deg/2)/aspect, so the published hfov == the
// commanded degrees) - the audit line can never go stale against this lever.
std::atomic<float> g_fovLeverDeg{0.0f};
std::atomic<uint32_t> g_fovLeverWrites{0};
std::atomic<uint32_t> g_fovLeverFaults{0};

// I6 rung 3: the resolution picker. Named modes are FIXED pixel sizes (a
// preset must mean the same pixels on every rig and boot - BS2's rule); the
// eye-shaped rungs are 0.93 aspect (the Quest 3 render-target shape). THE LAW
// CAVEAT on every non-flat entry: the FOV option is vertical-referenced
// (tanH = tanV x aspect), so a squarer render ALONE just narrows the
// horizontal - these modes pay only combined with the FOV lever (wide tanV +
// near-square target = the filled eye).
struct ResMode {
    const char* cmdName;
    const char* label;
    int w, h;
};
constexpr ResMode kResModes[] = {
    {"flat", "2560 x 1440 (16:9 desktop native, flat play)", 2560, 1440},
    {"squareperf", "1440 x 1440 (1:1, 2.1 MPx, perf / flat-test)", 1440, 1440},
    {"eye", "1600 x 1712 (0.93, 2.7 MPx, eye-shaped)", 1600, 1712},
    {"native", "2064 x 2208 (0.93, 4.6 MPx, Quest 3 native)", 2064, 2208},
    {"sharp", "2480 x 2648 (0.93, 6.6 MPx, demanding)", 2480, 2648},
};
// F10/seam -> game thread: (w<<32)|h, 0 = none pending. Consumed in the
// detour tail; the apply dispatches setres INTO the engine (game-thread-only
// per the reflect gates) and then persists the ini so the next boot agrees.
std::atomic<uint64_t> g_resApplyPending{0};
// The vrstereo latch (what the toggle last applied), telemetry for UI/status.
std::atomic<bool> g_stereoArmed{false};
// F10 -> game thread: -1 none, 0 pending off, 1 pending on. The overlay draws
// on the render thread and must never touch engine state or (later, rung 3)
// install hooks; the detour consumes this on the next call - BS2's
// request_vrstereo shape.
std::atomic<int> g_vrstereoPending{-1};
// I7 (session 44): the synthetic pad's armed state, and its posting lane. ON by
// default on this game - see cfg_set_input for why a boot must come up with
// controls live. The adapter applies the boot value directly at init (so a
// refused camera hook cannot leave the player with no controller); after that
// the detour drains the pending slot exactly like the stereo toggle.
std::atomic<bool> g_inputOn{true};
std::atomic<int> g_inputPending{-1};
// What publish_projection_claim last computed, for the overlay/heartbeat.
std::atomic<float> g_lastClaimHfovDeg{0.0f};
std::atomic<float> g_lastClaimAspect{0.0f};

// Rung 2: the inter-pupillary distance, adapter-local (core has no IPD API -
// the parallax the headset shows is produced entirely here). 63 mm default,
// F10 slider, persisted in vrpreset.ini.
std::atomic<float> g_ipdMm{63.0f};
// Per-eye telemetry for the heartbeat's inter-eye check: the last FINAL loc
// published for each eye (index 0 = left/sign -1). Game thread only.
FVector g_eyeLoc[2] = {};
bool g_eyeLocValid[2] = {false, false};
int g_lastEyeSign = 0;

// Rung 3c: SequentialReentry's cached base. Pass 1 caches the fully-driven
// PRE-EYE camera here (game thread only); pass 2 replays it ABSOLUTELY and
// applies the +1 eye - it never re-samples the head, because a Present lands
// between the passes and a re-sample skews the pair into vertical disparity
// (the ROADMAP box, BS1's proven rule). The 100 ms staleness guard leaves
// the camera alone rather than replay a dead base.
FVector g_srBaseLoc{};
FRotator g_srBaseRot{};
uint64_t g_srBaseStampMs = 0;
bool g_srBaseValid = false;
std::atomic<uint32_t> g_srReplayBursts{0}; // one per doubled draw (seq edge)
std::atomic<uint32_t> g_srReplayCalls{0};  // raw pass-2 camera dispatches
uint32_t g_srLastSeq = 0;                  // game thread only

// Synthetic HMD lane (BS2's shape, WITH the position triple): `simhead <yaw>
// <pitch> <roll> [px py pz] [holdMs]` feeds a scripted head pose through the
// real drive, so the full 6DoF xr-to-ue mapping is provable flat from the
// log. Self-expiring; ungated by session state so it works with no XR
// session at all.
struct SimHead {
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    float px = 0.0f, py = 0.0f, pz = 0.0f; // meters, XR local space
    uint64_t deadline = 0;
};
SimHead g_simHead;

// vrrec cadence: the detour fires many times per rendered frame, so the
// recorder tap advances on a present-count edge (see recorder.h). Game
// thread only.
uint64_t g_lastPresentSeen = 0;

// Thread identity. The camera hook installs at ~T+0.4 s and the first Present
// lands at ~T+8.5 s, so the FIRST detour fire can precede any Present and
// d3d11_hook::last_present_tid() may still be 0. The comparison therefore lives
// on the throttled path, not in the first-fire line.
std::atomic<uint32_t> g_cameraTid{0};
std::atomic<uint32_t> g_foreignTidCalls{0};
std::atomic<bool> g_loggedThreadSplit{false};

// Path census. GetPlayerViewPoint has four internal paths and only the first is
// a cheap cached read; which one runs tells us whether +0x248 and +0x240 mean
// what session 34 inferred from shape.
std::atomic<uint32_t> g_pathCached{0};   // [this+0x248] bit 0 set
std::atomic<uint32_t> g_pathCamera{0};   // clear, and [this+0x240] non-null
std::atomic<uint32_t> g_pathTarget{0};   // clear, and [this+0x240] null
std::atomic<uint32_t> g_pathUnknown{0};  // `this` unreadable
std::atomic<bool> g_loggedMatrix{false};
std::atomic<void*> g_lastSelf{nullptr};

// ---------------------------------------------------------------------------
// Rung 3a: caller census. This seam has 14 static callers and fires
// 1400-9600/s while the game presents ~90/s - the caller whose rate tracks
// presents 1:1 marks the once-per-frame scene-build path, which is where the
// SequentialReentry root is derived from (walk UP from that return RVA with
// pe-xref + capstone, offline). COUNTS per return RVA, not just distinct
// RVAs (BS1's note_caller shape, extended - rate is the discriminator here).
// Game thread only (called after the tid latch). `bsicam callers` dumps
// counts + deltas against the present counter; two dumps give rates.
// ---------------------------------------------------------------------------
constexpr size_t kCallerSlots = 24;
struct CallerSlot {
    uint32_t rva = 0;
    uint64_t count = 0;
    uint64_t lastDump = 0;
};
CallerSlot g_callers[kCallerSlots];
uint64_t g_callerOverflow = 0;
uint64_t g_callersLastPresent = 0;
uint64_t g_callersLastMs = 0;

void note_caller(uint32_t rva) {
    for (auto& slot : g_callers) {
        if (slot.rva == rva) {
            ++slot.count;
            return;
        }
        if (slot.rva == 0) {
            slot.rva = rva;
            slot.count = 1;
            return;
        }
    }
    ++g_callerOverflow;
}

uint32_t to_rva(const void* p) {
    const patterns::Symbols& s = patterns::symbols();
    if (!s.imageBase) return 0xFFFFFFFFu;
    const uintptr_t d =
        reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(s.imageBase);
    return d < s.imageSize ? static_cast<uint32_t>(d) : 0xFFFFFFFFu;
}

// One-shot stack backtrace, armed per caller return RVA (`bsicam stack
// <hexRva>`): the next detour call whose immediate caller matches logs the
// whole return chain as RVAs. This is how the scene-build ROOT is derived -
// the census names the once-per-frame immediate caller, the backtrace names
// everything above it, and pe-xref/capstone then only have to CONFIRM
// entries, not guess them. RtlCaptureStackBackTrace is cheap and read-only;
// with FPO frames it can come up short, which is a visible result (fewer
// frames), never a wrong one.
std::atomic<uint32_t> g_stackWantCaller{0};

// FPO cuts RtlCaptureStackBackTrace short, so the one-shot ALSO scrapes the
// raw stack: any dword that is an image VA whose preceding bytes decode as a
// call instruction is a plausible return address. Heuristic (a data dword
// can false-positive), which is fine for a derivation instrument - every hit
// gets confirmed offline before anything hooks it.
struct ScrapeHit {
    uint32_t rva;
    uint32_t kind; // call opcode class, for the log
};

int scrape_stack(void* stackAnchor, ScrapeHit* out, int maxOut) {
    int n = 0;
    const patterns::Symbols& s = patterns::symbols();
    if (!s.imageBase) return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(s.imageBase);
    __try {
        const uintptr_t* sp = static_cast<const uintptr_t*>(stackAnchor);
        for (int i = 0; i < 0x800 && n < maxOut; ++i) {
            const uintptr_t v = sp[i];
            if (v < base + 0x1000 || v >= base + s.imageSize) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(v);
            uint32_t kind = 0;
            if (p[-5] == 0xE8) kind = 0xE8;                       // call rel32
            else if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) kind = 0xFFD0; // call reg
            else if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) kind = 0xFF50; // call [reg+d8]
            else if (p[-6] == 0xFF && (p[-5] & 0xF8) == 0x90) kind = 0xFF90; // call [reg+d32]
            else if (p[-7] == 0xFF && p[-6] == 0x14) kind = 0xFF14;          // call [sib]
            if (!kind) continue;
            out[n].rva = static_cast<uint32_t>(v - base);
            out[n].kind = kind;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void maybe_backtrace(uint32_t callerRva, void* stackAnchor) {
    if (g_stackWantCaller.load(std::memory_order_relaxed) != callerRva) return;
    if (g_stackWantCaller.exchange(0, std::memory_order_relaxed) != callerRva) return;
    void* frames[24] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    BVR_LOG("[bsi] stack: one-shot for caller 0x%X - %u frames (RVAs, 0xFFFFFFFF = outside "
            "the image):",
            callerRva, n);
    for (USHORT i = 0; i < n; ++i)
        BVR_LOG("[bsi] stack:   #%u 0x%08X", i, to_rva(frames[i]));
    ScrapeHit hits[24];
    const int m = scrape_stack(stackAnchor, hits, 24);
    BVR_LOG("[bsi] stack: scrape (%d call-preceded stack dwords, walk-up order, heuristic):",
            m);
    for (int i = 0; i < m; ++i)
        BVR_LOG("[bsi] stack:   ret 0x%08X (call form %X)", hits[i].rva, hits[i].kind);
}

// One-shot LIVE resolution of the vtable-dispatched scene-draw entry
// (`bsicam scenedraw`). The static walk dead-ends: the call at 0x1FE05D is
// `mov edx,[ecx]; mov edx,[edx+8]; call edx` with ecx = [viewport+0x1C], so
// the entry lives in a vtable the census cannot name. Live it is trivial:
// when this detour runs from the scene path, the outer frame's return
// address (0x1FE05F) is on the stack with the two pushed args right above
// it - [ret][viewport][canvas] - so scan up from our own frame for that
// return VA, take the next dword as the viewport, and follow
// [viewport+0x1C] -> [client] -> [vtable+8]. Every read SEH-guarded,
// POD-only inside the guard.
std::atomic<bool> g_sceneProbeArmed{false};

struct SceneProbeResult {
    uint32_t viewportRva = 0; // heap - expect 0xFFFFFFFF, logged for shape
    void* viewport = nullptr;
    void* client = nullptr;
    uint32_t vtableRva = 0;
    uint32_t entryRva = 0;
    uint32_t slots[6] = {};
    bool ok = false;
};

SceneProbeResult scene_probe(void* stackAnchor) {
    SceneProbeResult r{};
    const patterns::Symbols& s = patterns::symbols();
    if (!s.imageBase) return r;
    const uintptr_t wantRet =
        reinterpret_cast<uintptr_t>(s.imageBase) + patterns::kSceneDispatchRetRva;
    __try {
        const uintptr_t* sp = static_cast<const uintptr_t*>(stackAnchor);
        for (int i = 0; i < 0x600; ++i) {
            if (sp[i] != wantRet) continue;
            void* viewport = reinterpret_cast<void*>(sp[i + 1]);
            if (!viewport) return r;
            r.viewport = viewport;
            const uint8_t* vp = static_cast<const uint8_t*>(viewport);
            void* client = *reinterpret_cast<void* const*>(vp + 0x1C);
            if (!client) return r;
            r.client = client;
            const uintptr_t* vt = *reinterpret_cast<const uintptr_t* const*>(client);
            if (!vt) return r;
            r.vtableRva = to_rva(vt);
            for (int k = 0; k < 6; ++k) r.slots[k] = to_rva(reinterpret_cast<void*>(vt[k]));
            r.entryRva = r.slots[2];
            r.ok = true;
            return r;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
    return r;
}

void maybe_scene_probe(void* stackAnchor) {
    if (!g_sceneProbeArmed.load(std::memory_order_relaxed)) return;
    if (!g_sceneProbeArmed.exchange(false, std::memory_order_relaxed)) return;
    const SceneProbeResult r = scene_probe(stackAnchor);
    if (!r.ok) {
        BVR_LOG("[bsi] sceneprobe: dispatch ret 0x%X not found on the stack this call (or a "
                "read faulted) - re-arm with `bsicam scenedraw` and make sure the scene "
                "path is firing",
                patterns::kSceneDispatchRetRva);
        return;
    }
    BVR_LOG("[bsi] sceneprobe: viewport=%p client=%p vtable RVA 0x%08X", r.viewport, r.client,
            r.vtableRva);
    BVR_LOG("[bsi] sceneprobe: slots [0]=0x%08X [1]=0x%08X [2]=0x%08X <- SCENE DRAW ENTRY "
            "[3]=0x%08X [4]=0x%08X [5]=0x%08X",
            r.slots[0], r.slots[1], r.slots[2], r.slots[3], r.slots[4], r.slots[5]);
}

// Generic one-shot vtable read (`bsicam vtprobe <globalVaHex> <slotHex>`):
// obj = *[globalVa], vt = [obj], entry = [vt+slot], all RVAs logged. Runs on
// the game thread at the next detour call, SEH-guarded - the derivation
// instrument for virtually-dispatched roots.
std::atomic<uint64_t> g_vtProbe{0}; // (globalVa << 16) | slot; 0 = disarmed

void maybe_vt_probe() {
    const uint64_t req = g_vtProbe.exchange(0, std::memory_order_relaxed);
    if (!req) return;
    const uint32_t globalVa = static_cast<uint32_t>(req >> 16);
    const uint32_t slot = static_cast<uint32_t>(req & 0xFFFF);
    void* obj = nullptr;
    void* vt = nullptr;
    void* entry = nullptr;
    bool ok = false;
    __try {
        obj = *reinterpret_cast<void* const*>(static_cast<uintptr_t>(globalVa));
        if (obj) {
            vt = *static_cast<void* const*>(obj);
            if (vt)
                entry = *reinterpret_cast<void* const*>(static_cast<uint8_t*>(vt) + slot);
            ok = entry != nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (ok)
        BVR_LOG("[bsi] vtprobe: [*0x%08X] obj=%p vt RVA 0x%08X slot +0x%X -> entry RVA "
                "0x%08X",
                globalVa, obj, to_rva(vt), slot, to_rva(entry));
    else
        BVR_LOG("[bsi] vtprobe: FAILED at 0x%08X (+0x%X) - obj=%p vt=%p", globalVa, slot,
                obj, vt);
}

void dump_callers() {
    const uint64_t present = bvr::d3d11_hook::present_count();
    const uint64_t now = GetTickCount64();
    const uint64_t presentDelta = present - g_callersLastPresent;
    const uint64_t msDelta = g_callersLastMs ? now - g_callersLastMs : 0;
    BVR_LOG("[bsi] callers: presents +%llu in %llu ms (a caller whose delta matches is the "
            "once-per-frame scene path)",
            static_cast<unsigned long long>(presentDelta),
            static_cast<unsigned long long>(msDelta));
    for (auto& slot : g_callers) {
        if (slot.rva == 0) break;
        BVR_LOG("[bsi] callers: ret 0x%08X count=%llu delta=%llu", slot.rva,
                static_cast<unsigned long long>(slot.count),
                static_cast<unsigned long long>(slot.count - slot.lastDump));
        slot.lastDump = slot.count;
    }
    if (g_callerOverflow)
        BVR_LOG("[bsi] callers: overflow=%llu (table full - widen kCallerSlots)",
                static_cast<unsigned long long>(g_callerOverflow));
    g_callersLastPresent = present;
    g_callersLastMs = now;
}

// Silence detection. The game-thread pump rides on this hook, so a camera that
// stops is a command surface that stops - and that must be a timestamped fact
// in the log, not a mystery. Detected on RESUME by comparing against the
// previous call's timestamp, which needs no extra state and no second thread:
// the only observer that can be sure a gap ended is the call that ends it.
constexpr uint64_t kSilenceReportMs = 2000;

struct Snapshot {
    FVector loc{};
    FRotator rot{};
    // The PRE-TRANSFORM source for whichever internal path actually ran, and
    // its name. Session 36 shipped this comparing only against [this+0x24C],
    // which is path 1's source - while every observed sample took path 2 and
    // read [cam+0x3B8]. It compared the wrong field and its "raw copy" verdict
    // was worthless. Now the source follows the path.
    FVector sourceLoc{};
    const char* sourceName = "";
    bool sourceValid = false;
    bool valid = false;
};
Snapshot g_last;

// Session 44 (I7 controls): the ENGINE's own view Z, windowed between
// heartbeats. The 1 Hz beat is a POINT sample and a jump's whole airborne arc
// fits between two of them, so "did A jump" was unanswerable; crouch and sprint
// want a STEP rather than an instant too. This accumulates the pre-drive value,
// so the head offset never enters it - what moves this number is the pawn.
// Written only on the latched camera thread (the detour turns foreign tids away
// before the snapshot), consumed and reset by the heartbeat.
struct ZWindow {
    float min = 0.0f, max = 0.0f, last = 0.0f;
    float prevX = 0.0f, prevY = 0.0f, travel = 0.0f; // horizontal ground travel
    bool valid = false;
};
ZWindow g_zWin;

// ---------------------------------------------------------------------------
// The 1 Hz probe. `this` is an engine pointer we did not create, so every read
// is SEH-guarded and the guarded body is POD-only: no logging and no allocation
// inside the guard, because MSVC under /EHsc does not run destructors during
// SEH unwinding and a fault taken while the log mutex was held would leave it
// held for the life of the process (BioShock 1 learned that expensively).
//
// This runs at 1 Hz and never per call: is_memory_valid is a VirtualQuery, and
// this detour can run 4000+ times a second.
// ---------------------------------------------------------------------------
struct Probe {
    bool ok = false;
    bool cachedFlag = false;
    bool cameraNonNull = false;
    FVector cachedLoc{};  // [this+0x24C], path 1's source
    FVector camLoc{};     // [cam+0x3B8],  path 2's source
    bool camLocRead = false;
    float matrix[16] = {};
    bool matrixOk = false;
};

Probe probe_self(void* self) {
    Probe p{};
    if (!self) return p;
    __try {
        const uint8_t* base = static_cast<const uint8_t*>(self);
        p.cachedFlag =
            (*reinterpret_cast<const uint8_t*>(base + patterns::kPcCachedPovFlagOffset) & 1u) != 0;
        const uint8_t* cam =
            *reinterpret_cast<const uint8_t* const*>(base + patterns::kPcCameraOffset);
        p.cameraNonNull = cam != nullptr;
        memcpy(&p.cachedLoc, base + patterns::kPcCachedLocOffset, sizeof p.cachedLoc);
        memcpy(p.matrix, base + patterns::kPcViewTransformOffset, sizeof p.matrix);
        p.matrixOk = true;
        // Second-level dereference, and the reason the whole probe is SEH
        // guarded: `cam` is an engine pointer we neither created nor own.
        if (cam) {
            memcpy(&p.camLoc, cam + patterns::kCameraPovLocOffset, sizeof p.camLoc);
            p.camLocRead = true;
        }
        p.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p.ok = false;
    }
    return p;
}

void log_matrix_once(const Probe& p) {
    if (!p.matrixOk) return;
    if (g_loggedMatrix.exchange(true)) return;
    // All four internal paths converge on a 4x4 SSE transform fed from
    // [this+0x430]. Whether the returned view is TRANSFORMED or merely copied
    // decides where an I4 HMD pose has to be injected, so it is measured here
    // rather than assumed.
    char line[320];
    int n = 0;
    line[0] = '\0';
    for (int i = 0; i < 16 && n >= 0 && static_cast<size_t>(n) + 12 < sizeof line; ++i)
        n += _snprintf_s(line + n, sizeof line - n, _TRUNCATE, "%.4f%s", p.matrix[i],
                         (i % 4 == 3) ? " | " : " ");
    BVR_LOG("[bsi] camera: [this+0x430] 4x4 = %s", line);
}

// ---------------------------------------------------------------------------
// The I4 drive (session 39). Writes ONLY through the out-params, after the
// original has filled them - the engine's camera state is never touched, so
// drive-off is a byte-identical passthrough and the engine's own view keeps
// moving under mouse/pad (read back every beat as engineRot on the drive
// heartbeat). Runs on EVERY detour call so all of a frame's callers see one
// consistent substituted view; the math is a handful of trig calls, far below
// this seam's 9681/s ceiling.
//
// Lane priority is BS1's proven order: vrrec replay -> simhead -> live. While
// a replay is loaded the live/sim lanes are never consulted (a replayed frame
// must never recenter or read the live head); an entry recorded with the
// camera not driven leaves the camera to the game, faithfully.
// ---------------------------------------------------------------------------
// Half-IPD displacement along the FINAL rotator's full-rotation right axis
// (BS1's session-22 lesson kept: a yaw-only right vector makes the virtual
// eyes stay horizontal while the head rolls, so the world's stereo collapses
// exactly when the horizon tilts; the full basis stacks the eyes the way real
// eyes stack, and reduces to the yaw-only formula bit-for-bit at roll 0).
// sign: -1 left, +1 right, in UU via ipd * worldScale.
void apply_eye_offset(FVector* loc, const FRotator& rot, int sign) {
    float fwd[3], right[3], up[3];
    ue_rot_basis(rot, fwd, right, up);
    const float halfIpdUu = static_cast<float>(sign) *
                            (g_ipdMm.load(std::memory_order_relaxed) / 2000.0f) *
                            g_worldScale.load(std::memory_order_relaxed);
    loc->x += right[0] * halfIpdUu;
    loc->y += right[1] * halfIpdUu;
    loc->z += right[2] * halfIpdUu;
}

void drive_view(FVector* loc, FRotator* rot, uint64_t now) {
    if (!loc || !rot) return;

    // Captured BEFORE any write, for the frame context published at the tail.
    // entryLoc is the camera location PRE-head-offset AND PRE-eye-offset, and
    // entryYawUnits is the engine's OWN yaw - the two quantities every consumer
    // that places a controller in the world has to share with this drive.
    const FVector entryLoc = *loc;
    const int32_t entryYawUnits = rot->yaw;

    bvr::vr::HeadPose hp{};
    bool driveHead = false;
    bool liveHead = false;
    const char* lane = "off";
    if (recorder::playing()) {
        if (recorder::replay_head(hp)) {
            driveHead = true;
            lane = "replay";
        }
    } else if (now < g_simHead.deadline) {
        float q[4];
        xr_local_trim_quat(g_simHead.pitchDeg / kRadToDeg, g_simHead.yawDeg / kRadToDeg,
                           g_simHead.rollDeg / kRadToDeg, q);
        hp = {};
        hp.px = g_simHead.px;
        hp.py = g_simHead.py;
        hp.pz = g_simHead.pz;
        hp.qx = q[0];
        hp.qy = q[1];
        hp.qz = q[2];
        hp.qw = q[3];
        driveHead = true; // sim lane stays ungated for flat tests
        lane = "sim";
    } else if (g_driveEnabled.load(std::memory_order_relaxed) && bvr::vr::get_head_pose(hp)) {
        // Live lane. Gated on the adapter's own flag plus a valid located
        // pose - NOT on vr_camera_mode(): camera mode flips core's submission
        // from the quad to a projection layer, and I4 is the MonoTracked rung
        // (head-driven camera UNDER the quad). set_camera_mode is never
        // called anywhere in this adapter until I5.
        driveHead = true;
        liveHead = true;
        lane = "live";
    }

    if (driveHead) {
        const UeAngles a = ue_angles_from_xr_quat(hp.qx, hp.qy, hp.qz, hp.qw);
        if (g_recenterRequested.exchange(false, std::memory_order_relaxed) || !g_haveRecenter) {
            g_recenterPose = hp;
            g_recenterYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
            g_haveRecenter = true;
            BVR_LOG("[bsi] vr camera recentered (yaw %.1f deg)", a.yawRad * kRadToDeg);
        }

        // Rotation: yaw ADDITIVE (the game's own yaw plus the head-look
        // residual, so stick/mouse turning and game scripting keep working);
        // pitch and roll ABSOLUTE from the head - on the out-param only, the
        // engine's own values stay engine-owned underneath.
        const int32_t gameYawUnits = rot->yaw;
        const int32_t headYawUnits =
            static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
        const int32_t residualUnits = wrap_rot(headYawUnits - g_recenterYawUnits);
        const float gameYawRad = static_cast<float>(gameYawUnits) / kRotUnitsPerRadian;
        {
            const int32_t headPitchUnits =
                static_cast<int32_t>(lroundf(a.pitchRad * kRotUnitsPerRadian));
            g_pitchErrDeg =
                static_cast<float>(wrap_rot(headPitchUnits - rot->pitch)) / kRotUnitsPerDegree;
        }
        rot->pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
        rot->roll = static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian);
        rot->yaw = gameYawUnits + residualUnits;

        // Position: recenter-relative head offset, mapped XR->UE, rotated
        // into the recenter-local frame and back out by the game yaw (which
        // is where recenter-forward points now, since our yaw is purely
        // additive), scaled UU-per-meter, ADDED to the game's own location.
        // Z is world-up, unrotated.
        const float dxr[3] = {hp.px - g_recenterPose.px, hp.py - g_recenterPose.py,
                              hp.pz - g_recenterPose.pz};
        float d[3];
        xr_to_ue(dxr, d);
        const float scale = g_worldScale.load(std::memory_order_relaxed);
        const float recenterYawRad = recenter_yaw_rad();
        const float c = cosf(-recenterYawRad), s = sinf(-recenterYawRad);
        const float lx = d[0] * c - d[1] * s;
        const float ly = d[0] * s + d[1] * c;
        const float cg = cosf(gameYawRad), sg = sinf(gameYawRad);
        const float ox = (lx * cg - ly * sg) * scale;
        const float oy = (lx * sg + ly * cg) * scale;
        const float oz = d[2] * scale;
        loc->x += ox;
        loc->y += oy;
        loc->z += oz;
        g_headOffX.store(ox, std::memory_order_relaxed);
        g_headOffY.store(oy, std::memory_order_relaxed);
        g_headOffZ.store(oz, std::memory_order_relaxed);
    } else {
        g_pitchErrDeg = 0.0f;
    }

    // The eye offset, applied LAST so the final camera is base + half-IPD
    // along its own right axis. Two mutually exclusive sign sources:
    //  - SR armed (rung 3): this whole pass is the LEFT eye - cache the
    //    PRE-EYE base for pass 2's replay, then apply -1. Pass 2 never runs
    //    through here (the detour forks to the replay instead).
    //  - AER (rung 2): core's current_eye_sign(), flipped after each submit
    //    so this renders exactly the eye the next Present will capture.
    // Applied with or without a head drive (a stereo camera does not need a
    // driven one), and to every caller in the frame - this seam is the
    // engine's single camera path, same as BS1/BS2's CalcView.
    if (scenedraw::stereo_active()) {
        g_srBaseLoc = *loc;
        g_srBaseRot = *rot;
        g_srBaseStampMs = now;
        g_srBaseValid = true;
        apply_eye_offset(loc, *rot, -1);
        g_lastEyeSign = -1;
        g_eyeLoc[0] = *loc;
        g_eyeLocValid[0] = true;
    } else {
        g_srBaseValid = false;
        const int eyeSign = bvr::vr::current_eye_sign();
        if (eyeSign != 0) apply_eye_offset(loc, *rot, eyeSign);
        g_lastEyeSign = eyeSign;
        if (eyeSign != 0) {
            g_eyeLoc[eyeSign < 0 ? 0 : 1] = *loc;
            g_eyeLocValid[eyeSign < 0 ? 0 : 1] = true;
        }
    }

    g_driveLane = lane;
    g_finalLoc = *loc;
    g_finalRot = *rot;
    g_finalValid = true;
    g_vrDriving.store(driveHead, std::memory_order_relaxed);

    // Publish the frame context: EVERY dispatch, not on the present edge. The
    // aim seam fires on the engine's FIRE path, which is not synchronized with
    // presents, so a context one present old would be up to 11 ms stale on a
    // moving hand. A publish is ~64 bytes of memcpy and two relaxed stores.
    {
        FrameContext fc{};
        fc.vrDriving = driveHead;
        fc.haveRecenter = g_haveRecenter;
        fc.gameYawUnits = entryYawUnits;
        fc.recenterYawUnits = g_recenterYawUnits;
        fc.baseX = entryLoc.x;
        fc.baseY = entryLoc.y;
        fc.baseZ = entryLoc.z;
        fc.recenterPx = g_recenterPose.px;
        fc.recenterPy = g_recenterPose.py;
        fc.recenterPz = g_recenterPose.pz;
        fc.worldScale = g_worldScale.load(std::memory_order_relaxed);
        fc.pc = g_lastSelf.load(std::memory_order_relaxed);
        fc.stamp = now;
        frame_context::publish(fc);
    }

    // The vrrec tap, once per rendered frame (present-count edge - the seam
    // itself fires many times per frame). Both record and replay advance on
    // the same edge, so the cadences match by construction.
    const uint64_t present = bvr::d3d11_hook::present_count();
    if (present != g_lastPresentSeen) {
        g_lastPresentSeen = present;
        recorder::on_tick(driveHead ? hp : bvr::vr::HeadPose{}, driveHead, liveHead, *loc,
                          *rot);
        // I8 write point 3: exactly once per rendered frame, game thread, after
        // loc/rot are final and the frame context has just been published.
        hands::on_camera_tail();
    }
}

// --- vrpreset: the config registry (I6 rung 5) ------------------------------
// The hand-rolled 3-key writer/reader this replaces used the same file and
// the same key names, so legacy vrpreset.ini files keep loading through the
// registry unchanged. resW/resH getters read the game ini through a 1 Hz
// cache (the F10 readout loop calls get() every frame - no file IO there).
game_ini::Resolution cached_ini_resolution() {
    static game_ini::Resolution s_cached;
    static uint64_t s_stampMs = 0;
    const uint64_t now = GetTickCount64();
    if (now - s_stampMs >= 1000) {
        s_stampMs = now;
        s_cached = game_ini::read_resolution();
    }
    return s_cached;
}

float cfg_get_world_scale() { return g_worldScale.load(std::memory_order_relaxed); }
void cfg_set_world_scale(float v) { g_worldScale.store(v, std::memory_order_relaxed); }
float cfg_get_claim_tanv() { return g_claimTanV.load(std::memory_order_relaxed); }
void cfg_set_claim_tanv(float v) { g_claimTanV.store(v, std::memory_order_relaxed); }
float cfg_get_ipd() { return g_ipdMm.load(std::memory_order_relaxed); }
void cfg_set_ipd(float v) { g_ipdMm.store(v, std::memory_order_relaxed); }
float cfg_get_fov_lever() { return g_fovLeverDeg.load(std::memory_order_relaxed); }
void cfg_set_fov_lever(float v) {
    // 0 = off is a legal stored state; anything else clamps to the verb range.
    g_fovLeverDeg.store(v >= 20.0f && v <= 170.0f ? v : 0.0f, std::memory_order_relaxed);
}
float cfg_get_res_w() { return static_cast<float>(cached_ini_resolution().x); }
float cfg_get_res_h() { return static_cast<float>(cached_ini_resolution().y); }
// Session 41 headset feedback: the preset carries the WHOLE session shape -
// stereo armed and the HMD drive too, so one Load restores everything. Both
// setters go through the same posting/atomic lanes the F10 checkboxes use.
float cfg_get_vrstereo() { return g_stereoArmed.load(std::memory_order_relaxed) ? 1.0f : 0.0f; }
void cfg_set_vrstereo(float v) {
    g_vrstereoPending.store(v != 0.0f ? 1 : 0, std::memory_order_relaxed);
}
float cfg_get_drive() { return g_driveEnabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f; }
void cfg_set_drive(float v) { g_driveEnabled.store(v != 0.0f, std::memory_order_relaxed); }
// Session 44 (I7): the synthetic pad's armed state joins the preset. It has to:
// `bsiinput` shipped OFF, so a headset boot came up with no controller AND no
// way to fix that, because anything judged in the headset must be a control -
// alt-tabbing to type destabilises the XR session. Default ON for this game.
// The setter POSTS (an F10 preset Load runs on the render thread); the game
// thread applies it, same lane as the stereo toggle.
float cfg_get_input() { return g_inputOn.load(std::memory_order_relaxed) ? 1.0f : 0.0f; }
void cfg_set_input(float v) {
    const bool on = v != 0.0f;
    g_inputOn.store(on, std::memory_order_relaxed);
    g_inputPending.store(on ? 1 : 0, std::memory_order_relaxed);
}

constexpr config::KeyDesc kConfigKeys[] = {
    {"worldScale", cfg_get_world_scale, cfg_set_world_scale, 1.0f, 500.0f},
    {"claimTanV", cfg_get_claim_tanv, cfg_set_claim_tanv, 0.05f, 4.0f},
    {"ipdMm", cfg_get_ipd, cfg_set_ipd, 40.0f, 80.0f},
    {"fovLeverDeg", cfg_get_fov_lever, cfg_set_fov_lever, 0.0f, 170.0f},
    {"resW", cfg_get_res_w, config::detail::latch_wanted_res_w, 640.0f, 16384.0f},
    {"resH", cfg_get_res_h, config::detail::latch_wanted_res_h, 480.0f, 16384.0f},
    {"vrstereoOn", cfg_get_vrstereo, cfg_set_vrstereo, 0.0f, 1.0f},
    {"driveHmd", cfg_get_drive, cfg_set_drive, 0.0f, 1.0f},
    {"inputOn", cfg_get_input, cfg_set_input, 0.0f, 1.0f},
};

// ---------------------------------------------------------------------------
// I6 rung 1: the per-dispatch FOV enforcement. Game thread only, SEH-guarded
// C frame (same discipline as probe_self: `cam` is an engine pointer we
// neither created nor own, and a torn camera at a load must fault soft, not
// crash). Writes BOTH known copies - the DefaultFOV field the per-tick
// refresh reads and the cached POV the projection consumes - so whichever
// side of the refresh this dispatch lands on, the enforced value is the one
// downstream. Idempotent across the SR pass-2 replay dispatches.
// ---------------------------------------------------------------------------
void apply_fov_lever(void* self) {
    const float deg = g_fovLeverDeg.load(std::memory_order_relaxed);
    if (deg <= 0.0f || !self) return;
    __try {
        uint8_t* base = static_cast<uint8_t*>(self);
        uint8_t* cam = *reinterpret_cast<uint8_t* const*>(base + patterns::kPcCameraOffset);
        if (!cam) return;
        *reinterpret_cast<float*>(cam + patterns::kCameraDefaultFovOffset) = deg;
        *reinterpret_cast<float*>(cam + patterns::kCameraPovFovOffset) = deg;
        g_fovLeverWrites.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fovLeverFaults.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// The projection claim, published on every detour call (BS2 publishes per
// CalcView - same seam role here; the math is one atan). Two core inputs:
//  - set_rendered_hfov: the horizontal FOV the game actually renders, from
//    the law hfov = 2*atan(tanV x aspect). hfovSrc=0, the honest claim - the
//    projection layer is tagged with it, and a mismatch reads as fisheye.
//  - publish_gameplay_view(true): core's cinematic fallback (g_cineEnabled
//    defaults ON) drops projection to the quad whenever this publish goes
//    STALE, so it must tick every dispatch. Strict stays constant true for
//    I5 - Infinite has no cinematic classifier until I9, and the stale leg
//    already quads load screens for free (this seam goes silent there).
// ---------------------------------------------------------------------------
void publish_projection_claim() {
    unsigned w = 0, h = 0;
    float aspect = 16.0f / 9.0f; // pre-first-Present fallback, matches the law's 16:9 row
    if (bvr::hud::backbuffer_dims(&w, &h) && w > 0 && h > 0)
        aspect = static_cast<float>(w) / static_cast<float>(h);
    // Lever armed: the claim derives from the ENFORCED value, not the manual
    // tanV. The engine reads the camera degrees as horizontal AT THE FIXED
    // 16:9 REFERENCE (patterns.h kFovRefAspect - measured at 1:1, where the
    // claim audit caught the current-aspect reading 43.7% wrong): tanV =
    // tan(deg/2)/(16/9), pinned across aspects; the publish below re-derives
    // tanH from the live aspect. The manual claim (bsifov tanv) is mirrored
    // so the heartbeat, overlay and preset all read the live truth.
    const float leverDeg = g_fovLeverDeg.load(std::memory_order_relaxed);
    if (leverDeg > 0.0f) {
        const float leverTanV = tanf(leverDeg * 0.5f / kRadToDeg) / patterns::kFovRefAspect;
        g_claimTanV.store(leverTanV, std::memory_order_relaxed);
    }
    const float tanV = g_claimTanV.load(std::memory_order_relaxed);
    const float hfovDeg = 2.0f * atanf(tanV * aspect) * kRadToDeg;
    bvr::vr::set_rendered_hfov(hfovDeg);
    bvr::vr::publish_gameplay_view(true);
    g_lastClaimHfovDeg.store(hfovDeg, std::memory_order_relaxed);
    g_lastClaimAspect.store(aspect, std::memory_order_relaxed);
}

// The vrstereo one-toggle. Runs on the GAME thread only (seam commands run
// there via the pump handover; the overlay posts via g_vrstereoPending -
// load-bearing at rung 3, where ON installs a hook). NO 1t rung on this game
// by design - DR-I5 measured a threaded ring-buffered substrate (BS2's
// shape, not BS1's kick-and-wait), so the ladder is BS2's minus 1t:
//   ON  : set_enabled -> set_camera_mode -> set_sr_pair_pacing ->
//         scenedraw install + arm  (monoOnly stops before pair pacing -
//         the rung-1 grade, projection with both eyes the same image)
//   OFF : scenedraw disarm -> set_alternate_eye(false) -> set_camera_mode
//         (exact reverse, symmetric across every backend so an off can
//         never strand one armed - BS2's asymmetric-off trap. Session
//         stays live: OFF returns to the mono quad, not to flat.)
void apply_vrstereo(bool on, bool monoOnly = false) {
    if (on) {
        bvr::vr::set_enabled(true);
        bvr::vr::set_camera_mode(true);
        g_stereoArmed.store(true, std::memory_order_relaxed);
        bool srArmed = false;
        if (!monoOnly) {
            bvr::vr::set_sr_pair_pacing(true);
            // Session 42 (the judder investigation): even pair-open cadence.
            // On a runtime that strictly gates xrWaitFrame this is measured
            // near-inert (sim: pairs already lock to refresh); on one that
            // pipelines, it stops the pair rate free-running against the
            // display (the ~5 Hz beat suspect). F10 checkbox + `vrpace sync`
            // are the in-headset A/B - default ON with stereo on this game.
            bvr::vr::set_pace_sync(true);
            // Session 43 (the stutter hunt): with stereo comes the spike
            // instrument - any pair interval > 2x period snapshots its
            // per-phase attribution to pacetrace.log, and the TRACE pairs
            // line carries spikes/s as the lever-A/B metric. Off with stereo
            // below; `vrpace spike` is the live seam.
            bvr::vr::set_spike_trace(true);
            srArmed = scenedraw::install() && scenedraw::set_stereo(true);
        }
        BVR_LOG("[bsi] VRSTEREO ON (%s): claim tanV=%.4f hfov=%.1f deg aspect=%.4f%s",
                monoOnly     ? "mono projection - both eyes the same image"
                : srArmed    ? "SequentialReentry - doubled scene build, pair-paced"
                             : "projection only - SR REFUSED, see [reentry] above",
                g_claimTanV.load(std::memory_order_relaxed),
                g_lastClaimHfovDeg.load(std::memory_order_relaxed),
                g_lastClaimAspect.load(std::memory_order_relaxed),
                srArmed ? " (inter-eye + reentry beats are the acceptance instruments)" : "");
    } else {
        scenedraw::set_stereo(false);
        // Symmetric off (the BS2 asymmetric-off trap): pair pacing stays set
        // after OFF, and without eye tags every present walks the wait path -
        // a still-armed sync would then pace the MONO quad to refresh, which
        // is not the state the user toggled back to.
        bvr::vr::set_spike_trace(false);
        bvr::vr::set_pace_sync(false);
        bvr::vr::set_alternate_eye(false);
        bvr::vr::set_camera_mode(false);
        g_stereoArmed.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] vrstereo off - back to the mono quad (session stays live)");
    }
}

void apply_pending_vrstereo() {
    const int pending = g_vrstereoPending.exchange(-1, std::memory_order_relaxed);
    if (pending >= 0) apply_vrstereo(pending != 0);
}

// I7: a preset Load (render thread) posts the pad's armed state; this drains it
// on the game thread, next to the stereo one.
void apply_pending_input() {
    const int pending = g_inputPending.exchange(-1, std::memory_order_relaxed);
    if (pending >= 0) input_drive::set_enabled_from_config(pending != 0);
}

// I6 rung 3: consume a posted resolution change on the game thread. Both
// lanes, in order: (1) live - setres through the proven ConsoleCommand seam
// (resizes the backbuffer within 20 ms, XR swapchain rebuild survives it -
// s38); (2) persist - the XUserOptions.ini write so the next boot agrees.
// The ini write is single-digit milliseconds, once, on explicit request -
// fine on the game thread (BS2 does the same).
void apply_pending_resolution() {
    const uint64_t packed = g_resApplyPending.exchange(0, std::memory_order_relaxed);
    if (!packed) return;
    const int w = static_cast<int>(packed >> 32);
    const int h = static_cast<int>(packed & 0xFFFFFFFFu);
    char cmd[48];
    _snprintf_s(cmd, sizeof cmd, _TRUNCATE, "setres %dx%d", w, h);
    BVR_LOG("[bsi] resolution: applying %dx%d (live setres + ini persist)", w, h);
    reflect::exec_console(cmd);
    game_ini::write_resolution(w, h);
}

void throttled(void* self, uint64_t now) {
    const uint32_t count = g_callCount.load(std::memory_order_relaxed);

    const Probe p = probe_self(self);
    if (!p.ok) {
        g_pathUnknown.fetch_add(1, std::memory_order_relaxed);
    } else if (p.cachedFlag) {
        g_pathCached.fetch_add(1, std::memory_order_relaxed);
    } else if (p.cameraNonNull) {
        g_pathCamera.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Paths 3 and 4 are ONE honest bucket. Separating them means calling
        // vtable slot +0x2C0 (GetViewTarget), and a virtual call out of a
        // detour can lazily create engine objects - the guard would be worse
        // than the thing it measures.
        g_pathTarget.fetch_add(1, std::memory_order_relaxed);
    }
    if (p.ok) {
        // Bind the comparison to the path that ACTUALLY RAN. Getting this wrong
        // is not a cosmetic bug: it is the difference between "the returned
        // view is a raw copy" and "the returned view is transformed", which is
        // what decides where I4 injects an HMD pose.
        if (p.cachedFlag) {
            g_last.sourceLoc = p.cachedLoc;
            g_last.sourceName = "cached POV [this+0x24C]";
            g_last.sourceValid = true;
        } else if (p.cameraNonNull && p.camLocRead) {
            g_last.sourceLoc = p.camLoc;
            g_last.sourceName = "camera POV [cam+0x3B8]";
            g_last.sourceValid = true;
        } else {
            // Paths 3 and 4 read off the view target, which we refuse to
            // resolve from inside a detour. No source, so no claim.
            g_last.sourceValid = false;
            g_last.sourceName = "view target (not resolved - no claim made)";
        }
        log_matrix_once(p);
    }

    // Thread split, once. Feeds DR-I5: if UE3's game thread and render thread
    // are the same here, the whole substrate question changes shape.
    const uint32_t presentTid = bvr::d3d11_hook::last_present_tid();
    if (presentTid != 0 && !g_loggedThreadSplit.exchange(true)) {
        const uint32_t camTid = g_cameraTid.load(std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: thread split - camera tid %u, present tid %u -> %s", camTid,
                presentTid,
                camTid == presentTid ? "SAME THREAD (game and render are one)"
                                     : "separate game and render threads");
    }

    if (!g_heartbeat.load(std::memory_order_relaxed) || g_beatsLeft <= 0) {
        g_lastBeatMs = 0;
        return;
    }
    if (g_lastBeatMs == 0) {
        g_lastBeatMs = now;
        g_beatBaseCount = count;
        return;
    }
    if (now - g_lastBeatMs < 1000) return;

    const uint32_t perSec =
        static_cast<uint32_t>((count - g_beatBaseCount) * 1000ull / (now - g_lastBeatMs));
    // Rotator components as %d ALWAYS, with degrees alongside. Never %f.
    BVR_LOG("[bsi] camera: loc=(%.1f %.1f %.1f) rot=(%d %d %d) = (%.1f %.1f %.1f)deg "
            "(%u calls/s, %u total)",
            g_last.loc.x, g_last.loc.y, g_last.loc.z, g_last.rot.pitch, g_last.rot.yaw,
            g_last.rot.roll, g_last.rot.pitch * kRotToDeg, g_last.rot.yaw * kRotToDeg,
            g_last.rot.roll * kRotToDeg, perSec, count);
    if (g_zWin.valid) {
        // The I7 controls instrument. Read it as: zSpan > ~30 UU with last back
        // near min = a JUMP happened inside this second; last sitting a step
        // BELOW min-of-the-previous-beat and staying there = CROUCH; travel/s
        // stepping up mid-walk with the stick unchanged = SPRINT.
        BVR_LOG("[bsi] camera: z window min=%.1f max=%.1f last=%.1f span=%.1f | "
                "ground travel %.1f UU this beat",
                g_zWin.min, g_zWin.max, g_zWin.last, g_zWin.max - g_zWin.min, g_zWin.travel);
        g_zWin.valid = false; // re-seed from the next dispatch
        g_zWin.travel = 0.0f;
    }
    if (g_last.sourceValid) {
        // The delta between the source the ACTIVE path read and the value
        // actually handed back. Non-zero means the documented 4x4 transform is
        // really applied on the way out - measured, not assumed, and now
        // measured against the right field.
        const float dx = g_last.loc.x - g_last.sourceLoc.x;
        const float dy = g_last.loc.y - g_last.sourceLoc.y;
        const float dz = g_last.loc.z - g_last.sourceLoc.z;
        BVR_LOG("[bsi] camera: returned-minus-source d=(%.3f %.3f %.3f) vs %s%s", dx, dy, dz,
                g_last.sourceName,
                (dx == 0.0f && dy == 0.0f && dz == 0.0f)
                    ? "  <- identical, this path hands back its source unchanged"
                    : "  <- TRANSFORMED on the way out");
    } else {
        BVR_LOG("[bsi] camera: returned-minus-source SKIPPED - %s", g_last.sourceName);
    }
    if (g_finalValid) {
        // The FINAL camera handed back to the game - drive and offsets and
        // all - which is what the flat 6DoF checks measure (simhead -> exact
        // degrees; sim position -> headOff in UU). engineRot is the pre-drive
        // snapshot: the engine's own view, which must keep moving under
        // mouse/pad while the drive is on - a frozen engineRot next to a
        // moving final rot is the BS1 pitch-freeze bug showing itself.
        // xr=<state> because a running session that never reached FOCUSED
        // still paces the game at the runtime's not-visible cadence.
        BVR_LOG("[bsi] drive: lane=%s final loc=(%.1f %.1f %.1f) rot=(%d %d %d) = "
                "(%.1f %.1f %.1f)deg engineRot=(%d %d %d) pitchErr=%.1f "
                "headOff=(%.1f %.1f %.1f) scale=%.0f xr=%s",
                g_driveLane, g_finalLoc.x, g_finalLoc.y, g_finalLoc.z, g_finalRot.pitch,
                g_finalRot.yaw, g_finalRot.roll, g_finalRot.pitch * kRotToDeg,
                g_finalRot.yaw * kRotToDeg, g_finalRot.roll * kRotToDeg, g_last.rot.pitch,
                g_last.rot.yaw, g_last.rot.roll, g_pitchErrDeg,
                g_headOffX.load(std::memory_order_relaxed),
                g_headOffY.load(std::memory_order_relaxed),
                g_headOffZ.load(std::memory_order_relaxed),
                g_worldScale.load(std::memory_order_relaxed), bvr::vr::session_state_name());
    }
    if (g_stereoArmed.load(std::memory_order_relaxed)) {
        // The I5 stereo heartbeat: what the flat battery asserts on. camMode
        // is core's composite (requested AND session AND projection-ready);
        // the audit tangents are what the last projection layer was actually
        // TAGGED with (src 0 = our claim - anything else means core fell back
        // and the claim is not being consumed).
        float auditTanH = 0.0f, auditTanV = 0.0f;
        int auditSrc = -1;
        unsigned swapW = 0, swapH = 0;
        bvr::vr::fov_audit(&auditTanH, &auditTanV, &auditSrc, &swapW, &swapH);
        BVR_LOG("[bsi] stereo: armed=1 camMode=%d claim tanV=%.4f aspect=%.4f hfov=%.1f | "
                "audit tanH=%.4f tanV=%.4f src=%d swap=%ux%u",
                bvr::vr::vr_camera_mode() ? 1 : 0,
                g_claimTanV.load(std::memory_order_relaxed),
                g_lastClaimAspect.load(std::memory_order_relaxed),
                g_lastClaimHfovDeg.load(std::memory_order_relaxed), auditTanH, auditTanV,
                auditSrc, swapW, swapH);
        if (g_eyeLocValid[0] && g_eyeLocValid[1]) {
            // The inter-eye check the flat battery asserts: |L-R| must equal
            // ipd (m) x worldScale UU exactly - the L and R finals are the
            // same base displaced -/+ half an IPD along one right vector.
            // Under AER the two finals are a frame apart, so compare only
            // while the base holds still (the battery parks the head).
            const float dx = g_eyeLoc[1].x - g_eyeLoc[0].x;
            const float dy = g_eyeLoc[1].y - g_eyeLoc[0].y;
            const float dz = g_eyeLoc[1].z - g_eyeLoc[0].z;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            const float expect = (g_ipdMm.load(std::memory_order_relaxed) / 1000.0f) *
                                 g_worldScale.load(std::memory_order_relaxed);
            BVR_LOG("[bsi] stereo: inter-eye |d|=%.3f UU expect=%.3f (d=(%.3f %.3f %.3f) "
                    "ipd=%.1fmm scale=%.0f lastSign=%+d)",
                    d, expect, dx, dy, dz, g_ipdMm.load(std::memory_order_relaxed),
                    g_worldScale.load(std::memory_order_relaxed), g_lastEyeSign);
        }
    }
    g_lastBeatMs = now;
    g_beatBaseCount = count;
    --g_beatsLeft;
}

// ---------------------------------------------------------------------------
// The detour. The probe/observation machinery is read-only (the snapshot is
// copied into const locals BEFORE the drive, so the heartbeat's
// returned-minus-source instrument keeps measuring the ORIGINAL's output);
// the ONLY writes through loc/rot in this translation unit are in drive_view,
// which substitutes the out-params after the original has filled them and
// never touches engine memory.
// ---------------------------------------------------------------------------
void __fastcall GetViewPointDetour(void* self, void* edx, FVector* loc, FRotator* rot) {
    // Original FIRST. It writes both out-params on all four internal paths, so
    // after this call their writability is proven rather than assumed.
    g_original(self, edx, loc, rot);

    if (!g_enabled.load(std::memory_order_relaxed)) return;

    // Rung 3c: the pass-2 fork. Inside the RE-ENTERED scene draw this seam
    // must replay pass 1's cached base (right-eyed) and do NOTHING else - no
    // census, no snapshot, no pump poll, no drive, no heartbeat, and above
    // all no g_lastCallMs update, so "camera silent" keeps meaning the
    // NORMAL pass went silent (BS2's discipline). Absolute writes make the
    // several pass-2 dispatches per draw idempotent; the burst counter
    // advances once per doubled draw via the seq edge, which is what makes
    // "bursts == second draws" an exact gate.
    if (scenedraw::second_pass_for_current_thread()) {
        if (loc && rot && g_srBaseValid &&
            GetTickCount64() - g_srBaseStampMs <= 100) {
            *loc = g_srBaseLoc;
            *rot = g_srBaseRot;
            apply_eye_offset(loc, *rot, +1);
            g_eyeLoc[1] = *loc;
            g_eyeLocValid[1] = true;
            g_srReplayCalls.fetch_add(1, std::memory_order_relaxed);
            // The doubled draw may re-evaluate the viewmodel over pass 1's rig
            // write; repaint it or the two eyes render different positions.
            hands::on_second_pass();
            const uint32_t seq = scenedraw::second_pass_seq();
            if (seq != g_srLastSeq) {
                g_srLastSeq = seq;
                g_srReplayBursts.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }

    const uint32_t count = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t now = GetTickCount64();
    const uint64_t prevCallMs = g_lastCallMs.exchange(now, std::memory_order_relaxed);
    g_fired.store(true, std::memory_order_relaxed);

    const uint32_t tid = GetCurrentThreadId();
    uint32_t expectedTid = 0;
    if (!g_cameraTid.compare_exchange_strong(expectedTid, tid, std::memory_order_relaxed) &&
        expectedTid != tid) {
        // A second thread dispatching this is DR-I5 evidence, not a bug - but
        // the throttled block is not thread-safe, so foreign threads are
        // counted and then leave.
        g_foreignTidCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Rung 3a census: which return address is calling, at what rate.
    {
        const uint32_t callerRva = to_rva(_ReturnAddress());
        note_caller(callerRva);
        maybe_backtrace(callerRva, _AddressOfReturnAddress());
        maybe_scene_probe(_AddressOfReturnAddress());
        maybe_vt_probe();
    }

    // Snapshot for the heartbeat. const locals: nothing here can write back.
    const FVector outLoc = loc ? *loc : FVector{};
    const FRotator outRot = rot ? *rot : FRotator{};
    g_last.loc = outLoc;
    g_last.rot = outRot;
    g_last.valid = true;
    g_lastSelf.store(self, std::memory_order_relaxed);

    // Z window + ground travel (I7 controls instrument): every dispatch, not
    // every beat. Travel is summed per dispatch rather than measured end-to-end
    // so a there-and-back walk still reads as motion.
    if (!g_zWin.valid) {
        g_zWin.min = g_zWin.max = outLoc.z;
        g_zWin.prevX = outLoc.x;
        g_zWin.prevY = outLoc.y;
        g_zWin.valid = true;
    } else {
        if (outLoc.z < g_zWin.min) g_zWin.min = outLoc.z;
        if (outLoc.z > g_zWin.max) g_zWin.max = outLoc.z;
        const float dx = outLoc.x - g_zWin.prevX;
        const float dy = outLoc.y - g_zWin.prevY;
        g_zWin.travel += sqrtf(dx * dx + dy * dy);
        g_zWin.prevX = outLoc.x;
        g_zWin.prevY = outLoc.y;
    }
    g_zWin.last = outLoc.z;

    if (!g_loggedFirstFire.exchange(true)) {
        BVR_LOG("[bsi] camera: FIRST FIRE - GetPlayerViewPoint detour live. this=%p tid=%u "
                "loc=(%.1f %.1f %.1f) rot=(%d %d %d)",
                self, tid, outLoc.x, outLoc.y, outLoc.z, outRot.pitch, outRot.yaw, outRot.roll);
    }

    // THE HANDOVER. This is the whole reason the command seam was built to be
    // pump-agnostic: from here commands run on the GAME thread, where anything
    // that touches the engine belongs. Safe to call every frame - it is 1 Hz
    // internally - and it latches on the first call, so the takeover happens
    // the moment the hook is proven live rather than on a timer.
    bvr::command::poll_from_game_thread(now);

    // A gap that just ended. During it the game-thread command pump was silent
    // too, so this line is what turns "the mod stopped responding" into a
    // timestamped fact. Level loads and cinematics are the expected causes.
    if (prevCallMs != 0 && now - prevCallMs >= kSilenceReportMs) {
        BVR_LOG("[bsi] camera: RESUMED after %llu ms silent - the game-thread command pump was "
                "silent with it for that whole window",
                static_cast<unsigned long long>(now - prevCallMs));
        g_lastBeatMs = 0; // reseed the base rather than report a fake calls/s spike
    }

    // I5: consume a posted vrstereo toggle (game thread, before the drive so
    // it takes effect this call), then publish the projection claim - the
    // fov the layer is tagged with plus the gameplay-view liveness that keeps
    // core's cinematic fallback from quadding a live gameplay projection.
    apply_pending_vrstereo();
    apply_pending_input();
    apply_pending_resolution();
    config::tick(); // F10-posted preset save/load ops (file IO on this thread)
    // Session-41 headset feedback: a loaded preset's resolution APPLIES (one
    // Load restores the whole session shape - the user's call, overriding the
    // earlier latch-then-click design). Same game-thread lane as the picker.
    {
        int pw = 0, ph = 0;
        bool fresh = false;
        if (config::wanted_resolution(&pw, &ph, &fresh) && fresh && pw >= 640 && ph >= 480)
            g_resApplyPending.store(
                (static_cast<uint64_t>(static_cast<uint32_t>(pw)) << 32) |
                    static_cast<uint32_t>(ph),
                std::memory_order_relaxed);
    }
    // I6: the lens decoder's round tick runs BEFORE the lever and the claim
    // publish - a track-mode write is deliberately overridden by an armed
    // lever, and the audit compares against the claim the previous dispatch
    // published. Then enforce the lever, then publish the claim it implies.
    lens::tick(now);
    apply_fov_lever(self);
    publish_projection_claim();

    // The I4 drive, AFTER the snapshot (so the observation instruments keep
    // measuring the original) and after the command poll (so a just-dispatched
    // simhead/recenter takes effect on this very call).
    drive_view(loc, rot, now);

    static uint64_t s_lastThrottle = 0;
    if (now - s_lastThrottle >= 1000) {
        s_lastThrottle = now;
        throttled(self, now);
        // I7 aim: the seam is a VIRTUAL read off the live pawn, so it can only
        // be installed once a pawn exists - which is a load-time race no init
        // ordering can win. Retried at 1 Hz while armed, one relaxed load when
        // not; a refusal logs its own gate.
        if (aim::wants_install()) aim::try_install();
    }
}

const char* path_summary(char* buf, size_t n) {
    _snprintf_s(buf, n, _TRUNCATE, "cached=%u camera=%u viewtarget-or-self=%u unreadable=%u",
                g_pathCached.load(), g_pathCamera.load(), g_pathTarget.load(),
                g_pathUnknown.load());
    return buf;
}

void log_status() {
    char paths[160];
    BVR_LOG("[bsi] camera: hook=%s enabled=%s fired=%s calls=%u silent=%llu ms tid=%u "
            "foreign-tid-calls=%u",
            g_hookLive.load() ? "installed" : "NOT installed",
            g_enabled.load() ? "yes" : "no", g_fired.load() ? "YES" : "no",
            g_callCount.load(), static_cast<unsigned long long>(silent_ms()),
            g_cameraTid.load(), g_foreignTidCalls.load());
    BVR_LOG("[bsi] camera: paths %s", path_summary(paths, sizeof paths));
}

} // namespace

bool install(const bvr::pattern_scan::ProcessImage& image) {
    (void)image;
    if (g_hookLive.load()) return true;

    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] camera: hook NOT installed - build gate closed. The game runs flat.");
        return false;
    }

    const uint8_t* target =
        patterns::rva_to_address(patterns::kGetPlayerViewPointRva, 64);
    if (!target) {
        BVR_LOG("[bsi] camera: hook NOT installed - RVA 0x%X is not readable",
                patterns::kGetPlayerViewPointRva);
        return false;
    }

    // Prologue gate. A hardcoded RVA on the wrong build does not fail to work,
    // it detours whatever happens to live there - which is how a mod corrupts a
    // game rather than merely not helping. REFUSE on any mismatch.
    if (memcmp(target, patterns::kGetPlayerViewPointPrologue,
               sizeof patterns::kGetPlayerViewPointPrologue) != 0) {
        BVR_LOG("[bsi] camera: prologue MISMATCH at RVA 0x%X (got %02X %02X %02X %02X ...) - "
                "build changed? REFUSING hook, the game runs flat",
                patterns::kGetPlayerViewPointRva, target[0], target[1], target[2], target[3]);
        return false;
    }

    // Independently confirm the argument count the typedef declares. `ret 8`
    // is C2 08 00; if it is not in the body, the 2-stack-arg assumption is
    // wrong and hooking would pop the RTC dialog that writes no crash dump.
    constexpr size_t kRetScanBytes = 0x400;
    const uint8_t* body = patterns::rva_to_address(patterns::kGetPlayerViewPointRva,
                                                   kRetScanBytes);
    bool retFound = false;
    if (body) {
        for (size_t i = 0; i + 3 <= kRetScanBytes; ++i) {
            if (body[i] == 0xC2 && body[i + 1] == patterns::kGetPlayerViewPointRetImm &&
                body[i + 2] == 0x00) {
                retFound = true;
                break;
            }
        }
    }
    if (!retFound) {
        BVR_LOG("[bsi] camera: no `ret %u` (C2 %02X 00) found in the first 0x400 bytes of "
                "RVA 0x%X - the 2-stack-arg assumption is UNCONFIRMED. REFUSING hook rather "
                "than risking a misaligned return (the RTC dialog writes no crash dump).",
                patterns::kGetPlayerViewPointRetImm, patterns::kGetPlayerViewPointRetImm,
                patterns::kGetPlayerViewPointRva);
        return false;
    }

    void* addr = const_cast<uint8_t*>(target);
    MH_STATUS status = MH_CreateHook(addr, reinterpret_cast<void*>(&GetViewPointDetour),
                                     reinterpret_cast<void**>(&g_original));
    if (status != MH_OK) {
        BVR_LOG("[bsi] camera: MH_CreateHook failed: %s", MH_StatusToString(status));
        return false;
    }
    // Self-enabling so this hook's activation never rides on another module's
    // MH_EnableHook(MH_ALL_HOOKS).
    status = MH_EnableHook(addr);
    if (status != MH_OK) {
        BVR_LOG("[bsi] camera: MH_EnableHook failed: %s", MH_StatusToString(status));
        MH_RemoveHook(addr);
        g_original = nullptr;
        return false;
    }

    g_target = addr;
    g_hookLive.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] camera: hook installed on GetPlayerViewPoint (target %p, RVA 0x%X, "
            "prologue and `ret %u` both verified). I4 drive present but OFF by default - "
            "the write target is the detour's out-params ONLY, never engine memory; with "
            "the drive off the hook observes only.",
            addr, patterns::kGetPlayerViewPointRva, patterns::kGetPlayerViewPointRetImm);
    return true;
}

bool has_fired() {
    return g_fired.load(std::memory_order_relaxed);
}

bool hook_live() {
    return g_hookLive.load(std::memory_order_relaxed);
}

void* last_player_controller() {
    return g_lastSelf.load(std::memory_order_relaxed);
}

bool aim_basis(int32_t* gameYawUnits, int32_t* recenterYawUnits) {
    // The drive must be live and a recenter must exist, or "the residual off
    // the recenter" has no meaning and a substitution would be a guess. Both
    // reads are of game-thread state, and the aim seam runs on the game thread
    // (its own interlock checks that), so no lock is needed.
    if (!g_driveEnabled.load(std::memory_order_relaxed) || !g_haveRecenter || !g_last.valid)
        return false;
    if (gameYawUnits) *gameYawUnits = g_last.rot.yaw;
    if (recenterYawUnits) *recenterYawUnits = g_recenterYawUnits;
    return true;
}

uint32_t second_pass_replays() {
    return g_srReplayBursts.load(std::memory_order_relaxed);
}

uint32_t camera_tid() {
    return g_cameraTid.load(std::memory_order_relaxed);
}

uint64_t silent_ms() {
    const uint64_t last = g_lastCallMs.load(std::memory_order_relaxed);
    if (last == 0) return 0;
    const uint64_t now = GetTickCount64();
    return now > last ? now - last : 0;
}

float claim_tan_v() {
    return g_claimTanV.load(std::memory_order_relaxed);
}

void set_claim_tan_v(float v) {
    if (v > 0.05f && v < 4.0f) g_claimTanV.store(v, std::memory_order_relaxed);
}

void get_recenter_state(bvr::vr::HeadPose* pose, int32_t* yawUnits, float* worldScale) {
    if (pose) *pose = g_recenterPose;
    if (yawUnits) *yawUnits = g_recenterYawUnits;
    if (worldScale) *worldScale = g_worldScale.load(std::memory_order_relaxed);
}

void set_recenter_state(const bvr::vr::HeadPose& pose, int32_t yawUnits, float worldScale) {
    g_recenterPose = pose;
    g_recenterYawUnits = yawUnits;
    g_worldScale.store(worldScale, std::memory_order_relaxed);
    g_haveRecenter = true;
    // A pending auto-recenter would re-reference the mapping onto the first
    // replayed head pose, throwing away the state just restored.
    g_recenterRequested.store(false, std::memory_order_relaxed);
    BVR_LOG("[bsi] recenter state SET (yaw %d units, worldScale %.1f) - vrrec play restore",
            yawUnits, worldScale);
}

void load_vr_preset() {
    // Adapter init: register the config table once (before the first load so
    // the applied values go through the registry's range guards), then load
    // the current store. Legacy 3-key files load unchanged - same file, same
    // key names.
    config::init(kConfigKeys, std::size(kConfigKeys));
    config::load_current();
}

bool input_armed_at_boot() {
    g_inputPending.store(-1, std::memory_order_relaxed); // the adapter applies it itself
    return g_inputOn.load(std::memory_order_relaxed);
}

bool handle_drive_verb(const char* cmd, const char* args) {
    if (!args) args = "";
    while (*args == ' ') ++args;

    if (strcmp(cmd, "recenter") == 0) {
        g_recenterRequested.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] command: recenter");
        return true;
    }
    if (strcmp(cmd, "worldscale") == 0) {
        float v = 0.0f;
        if (sscanf_s(args, "%f", &v) == 1 && v > 0.0f) {
            g_worldScale.store(v, std::memory_order_relaxed);
            BVR_LOG("[bsi] command: worldscale %.1f", v);
        } else {
            BVR_LOG("[bsi] usage: worldscale <uuPerMeter> (current %.1f)",
                    g_worldScale.load(std::memory_order_relaxed));
        }
        return true;
    }
    if (strcmp(cmd, "vrpreset") == 0) {
        // The whole verb family lives in the config registry now:
        // save | saveas <name> | load [<name>] | list (bare = load current).
        config::handle_vrpreset(args);
        return true;
    }
    if (strcmp(cmd, "vrstereo") == 0) {
        // Seam commands already run on the game thread (the pump handover), so
        // this applies directly; the overlay checkbox posts instead.
        if (strncmp(args, "mono", 4) == 0) {
            apply_vrstereo(true, /*monoOnly=*/true);
        } else if (strncmp(args, "on", 2) == 0) {
            apply_vrstereo(true);
        } else if (strncmp(args, "off", 3) == 0) {
            apply_vrstereo(false);
        } else {
            BVR_LOG("[bsi] vrstereo: armed=%d sr=%d camMode=%d session=%s | usage: vrstereo "
                    "on|mono|off",
                    g_stereoArmed.load(std::memory_order_relaxed) ? 1 : 0,
                    scenedraw::stereo_active() ? 1 : 0, bvr::vr::vr_camera_mode() ? 1 : 0,
                    bvr::vr::session_state_name());
        }
        return true;
    }
    if (strcmp(cmd, "vraer") == 0) {
        // AlternateEye one-toggle: real geometric stereo, one eye per frame,
        // the compositor reprojecting the other. Judders by design - it is
        // the de-risking rung, no engine re-entrancy. Rides on the vrstereo
        // arming (enable + camera mode) plus core's AER flag; the eye offset
        // itself is applied in drive_view off current_eye_sign().
        if (strncmp(args, "on", 2) == 0) {
            apply_vrstereo(true);
            bvr::vr::set_alternate_eye(true);
            BVR_LOG("[bsi] VRAER ON: AlternateEye armed (ipd %.1f mm, worldScale %.0f -> "
                    "inter-eye %.3f UU on the heartbeat)",
                    g_ipdMm.load(std::memory_order_relaxed),
                    g_worldScale.load(std::memory_order_relaxed),
                    (g_ipdMm.load(std::memory_order_relaxed) / 1000.0f) *
                        g_worldScale.load(std::memory_order_relaxed));
        } else if (strncmp(args, "off", 3) == 0) {
            bvr::vr::set_alternate_eye(false);
            BVR_LOG("[bsi] vraer off (camera mode stays as vrstereo left it)");
        } else {
            BVR_LOG("[bsi] vraer: eyeSign=%+d ipd=%.1fmm | usage: vraer on|off",
                    bvr::vr::current_eye_sign(), g_ipdMm.load(std::memory_order_relaxed));
        }
        return true;
    }
    if (strcmp(cmd, "ipd") == 0) {
        float v = 0.0f;
        if (sscanf_s(args, "%f", &v) == 1 && v >= 40.0f && v <= 80.0f) {
            g_ipdMm.store(v, std::memory_order_relaxed);
            BVR_LOG("[bsi] ipd %.1f mm (vrpreset save persists)", v);
        } else {
            BVR_LOG("[bsi] usage: ipd <40..80 mm> (current %.1f)",
                    g_ipdMm.load(std::memory_order_relaxed));
        }
        return true;
    }
    if (strcmp(cmd, "bsires") == 0) {
        // Accepts a mode name, "W H", "WxH", "list" or "status". Posts the
        // same atomic the F10 Apply button posts, so the harness and the UI
        // share one lane. Token-match, not whole-string (trailing newline).
        int w = 0, h = 0;
        char tok[24] = "";
        sscanf_s(args, "%23s", tok, static_cast<unsigned>(sizeof tok));
        if (!tok[0] || strcmp(tok, "status") == 0) {
            unsigned lw = 0, lh = 0;
            bvr::hud::backbuffer_dims(&lw, &lh);
            game_ini::log_status(lw, lh);
            return true;
        }
        if (strcmp(tok, "list") == 0) {
            for (const ResMode& m : kResModes)
                BVR_LOG("[bsi] bsires %-10s = %s", m.cmdName, m.label);
            BVR_LOG("[bsi] bsires <w> <h> | <WxH> | <mode> | status | list");
            return true;
        }
        for (const ResMode& m : kResModes) {
            if (strcmp(tok, m.cmdName) == 0) {
                w = m.w;
                h = m.h;
                break;
            }
        }
        if (!w && (sscanf_s(args, "%d %d", &w, &h) == 2 ||
                   sscanf_s(args, "%dx%d", &w, &h) == 2)) {
            // parsed
        }
        if (w >= 640 && h >= 480 && w <= 16384 && h <= 16384) {
            g_resApplyPending.store((static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32) |
                                        static_cast<uint32_t>(h),
                                    std::memory_order_relaxed);
            BVR_LOG("[bsi] resolution %dx%d posted (applies on the next camera dispatch)", w,
                    h);
        } else {
            BVR_LOG("[bsi] usage: bsires <w> <h> | <WxH> | <mode> | status | list");
        }
        return true;
    }
    if (strcmp(cmd, "bsifov") == 0) {
        if (strncmp(args, "set", 3) == 0) {
            float deg = 0.0f;
            if (sscanf_s(args + 3, "%f", &deg) == 1 && deg >= 20.0f && deg <= 170.0f) {
                g_fovLeverDeg.store(deg, std::memory_order_relaxed);
                const float tanV = tanf(deg * 0.5f / kRadToDeg) / patterns::kFovRefAspect;
                BVR_LOG("[bsi] FOV LEVER ARMED: %.1f deg (horizontal at the 16:9 reference), "
                        "enforced per dispatch. Engine law: tanV=%.4f pinned, tanH = tanV x "
                        "aspect (expect that in a `dumpframe cb` decode; the claim derives "
                        "from the lever)",
                        deg, tanV);
            } else {
                BVR_LOG("[bsi] usage: bsifov set <20..170 deg> (current %.1f, 0=off)",
                        g_fovLeverDeg.load(std::memory_order_relaxed));
            }
        } else if (strncmp(args, "off", 3) == 0) {
            g_fovLeverDeg.store(0.0f, std::memory_order_relaxed);
            BVR_LOG("[bsi] fov lever OFF - the engine's per-tick recompute restores the "
                    "native FOV within a tick (writes=%u faults=%u); the claim keeps the "
                    "last derived tanV until bsifov tanv corrects it",
                    g_fovLeverWrites.load(std::memory_order_relaxed),
                    g_fovLeverFaults.load(std::memory_order_relaxed));
        } else if (strncmp(args, "tanv", 4) == 0) {
            float v = 0.0f;
            if (sscanf_s(args + 4, "%f", &v) == 1 && v > 0.1f && v < 2.0f) {
                g_claimTanV.store(v, std::memory_order_relaxed);
                BVR_LOG("[bsi] fov claim: tanV=%.4f (law: slider min %.4f .. max %.4f; verify "
                        "against a `dumpframe cb` decode)",
                        v, patterns::kTanVSliderMin, patterns::kTanVSliderMax);
            } else {
                BVR_LOG("[bsi] usage: bsifov tanv <0.1..2.0> (current %.4f)",
                        g_claimTanV.load(std::memory_order_relaxed));
            }
        } else {
            float auditTanH = 0.0f, auditTanV = 0.0f;
            int auditSrc = -1;
            unsigned swapW = 0, swapH = 0;
            bvr::vr::fov_audit(&auditTanH, &auditTanV, &auditSrc, &swapW, &swapH);
            BVR_LOG("[bsi] fov claim: tanV=%.4f aspect=%.4f hfov=%.1f deg | lever=%.1f deg "
                    "(writes=%u faults=%u) | audit tanH=%.4f tanV=%.4f src=%d swap=%ux%u | "
                    "usage: bsifov [set <deg>|off|tanv <v>]",
                    g_claimTanV.load(std::memory_order_relaxed),
                    g_lastClaimAspect.load(std::memory_order_relaxed),
                    g_lastClaimHfovDeg.load(std::memory_order_relaxed),
                    g_fovLeverDeg.load(std::memory_order_relaxed),
                    g_fovLeverWrites.load(std::memory_order_relaxed),
                    g_fovLeverFaults.load(std::memory_order_relaxed), auditTanH, auditTanV,
                    auditSrc, swapW, swapH);
        }
        return true;
    }
    if (strcmp(cmd, "bsipose") == 0) {
        // s43b: the pose-attribution lag A/B (core selector, doc at
        // set_pose_lag). The jumpy-camera hypothesis: Infinite's threaded
        // one-frame-lag renderer makes the historical one-back attribution
        // wrong by a generation; whichever lag feels smooth in the headset
        // names the pipeline depth.
        int lag = -1;
        if (sscanf_s(args, "%d", &lag) == 1 && lag >= 0 && lag <= 2) {
            bvr::vr::set_pose_lag(lag);
        } else {
            BVR_LOG("[bsi] pose attribution lag=%d (0=fresh 1=one-back[default] "
                    "2=two-back) | inter-generation delta %.2f deg/pair | usage: "
                    "bsipose 0|1|2",
                    bvr::vr::get_pose_lag(), bvr::vr::get_pose_gen_delta_deg());
        }
        return true;
    }
    if (strcmp(cmd, "simhead") == 0) {
        if (strncmp(args, "off", 3) == 0) {
            g_simHead.deadline = 0;
            BVR_LOG("[bsi] command: simhead off");
        } else {
            // 3 args = angles; 4 = angles + holdMs (BS1-compatible); 6 =
            // angles + position; 7 = angles + position + holdMs.
            float v[7] = {};
            int n = sscanf_s(args, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4],
                             &v[5], &v[6]);
            if (n == 3 || n == 4 || n == 6 || n == 7) {
                const bool wasIdle = GetTickCount64() >= g_simHead.deadline;
                g_simHead.yawDeg = v[0];
                g_simHead.pitchDeg = v[1];
                g_simHead.rollDeg = v[2];
                g_simHead.px = n >= 6 ? v[3] : 0.0f;
                g_simHead.py = n >= 6 ? v[4] : 0.0f;
                g_simHead.pz = n >= 6 ? v[5] : 0.0f;
                int hold = n == 4   ? static_cast<int>(v[3])
                           : n == 7 ? static_cast<int>(v[6])
                                    : 0;
                if (hold <= 0) hold = 120000;
                g_simHead.deadline = GetTickCount64() + static_cast<uint64_t>(hold);
                if (wasIdle) g_recenterRequested.store(true, std::memory_order_relaxed);
                BVR_LOG("[bsi] command: simhead yaw %.1f pitch %.1f roll %.1f "
                        "pos (%.2f %.2f %.2f) for %d ms%s",
                        v[0], v[1], v[2], g_simHead.px, g_simHead.py, g_simHead.pz, hold,
                        wasIdle ? " (recentering onto first sim pose)" : "");
            } else {
                BVR_LOG("[bsi] usage: simhead <yaw> <pitch> <roll> [px py pz] [holdMs] | "
                        "simhead off");
            }
        }
        return true;
    }
    return false;
}

bool handle_command(const char* args) {
    if (!args) args = "";
    while (*args == ' ') ++args;

    if (strncmp(args, "status", 6) == 0 || *args == '\0') {
        log_status();
        return true;
    }
    if (strncmp(args, "paths", 5) == 0) {
        char buf[160];
        BVR_LOG("[bsi] camera: path census %s", path_summary(buf, sizeof buf));
        BVR_LOG("[bsi] camera: 'cached' = [this+0x248] bit 0 set (fast path, +0x24C/+0x258); "
                "'camera' = clear with [this+0x240] non-null; the third bucket is the view "
                "target and the controller's own fields, deliberately NOT separated because "
                "that needs a virtual call out of a detour.");
        return true;
    }
    if (strncmp(args, "callers", 7) == 0) {
        dump_callers();
        return true;
    }
    if (strncmp(args, "vtprobe", 7) == 0) {
        unsigned va = 0, slot = 0;
        if (sscanf_s(args + 7, "%x %x", &va, &slot) == 2 && va != 0 && slot < 0x10000) {
            g_vtProbe.store((static_cast<uint64_t>(va) << 16) | slot,
                            std::memory_order_relaxed);
            BVR_LOG("[bsi] camera: vtprobe armed for [*0x%08X] slot +0x%X", va, slot);
        } else {
            BVR_LOG("[bsi] usage: bsicam vtprobe <globalVaHex> <slotHex>");
        }
        return true;
    }
    if (strncmp(args, "scenedraw", 9) == 0) {
        g_sceneProbeArmed.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: scene-draw probe armed (one-shot)");
        return true;
    }
    if (strncmp(args, "stack", 5) == 0) {
        const char* v = args + 5;
        while (*v == ' ') ++v;
        unsigned rva = 0;
        if (sscanf_s(v, "%x", &rva) == 1 && rva != 0) {
            g_stackWantCaller.store(rva, std::memory_order_relaxed);
            BVR_LOG("[bsi] camera: one-shot backtrace armed for caller 0x%X", rva);
        } else {
            BVR_LOG("[bsi] usage: bsicam stack <callerRetRvaHex>");
        }
        return true;
    }
    if (strncmp(args, "tid", 3) == 0) {
        BVR_LOG("[bsi] camera: camera tid=%u present tid=%u foreign-tid calls=%u",
                g_cameraTid.load(), bvr::d3d11_hook::last_present_tid(),
                g_foreignTidCalls.load());
        return true;
    }
    if (strncmp(args, "matrix", 6) == 0) {
        g_loggedMatrix.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: re-arming the one-shot [this+0x430] dump for the next beat");
        return true;
    }
    if (strncmp(args, "heartbeat", 9) == 0) {
        const char* v = args + 9;
        while (*v == ' ') ++v;
        const bool on = strncmp(v, "off", 3) != 0;
        g_heartbeat.store(on, std::memory_order_relaxed);
        if (on) g_beatsLeft = 10;
        BVR_LOG("[bsi] camera: heartbeat %s%s", on ? "ON" : "off",
                on ? " (10-beat burst)" : "");
        return true;
    }
    if (strncmp(args, "drive", 5) == 0) {
        const char* v = args + 5;
        while (*v == ' ') ++v;
        if (strncmp(v, "on", 2) == 0) {
            g_driveEnabled.store(true, std::memory_order_relaxed);
            BVR_LOG("[bsi] camera: DRIVE ON (live lane; needs a located head pose - simhead "
                    "and vrrec replay drive regardless). Out-param substitution only; the "
                    "quad stays the submit path.");
        } else if (strncmp(v, "off", 3) == 0) {
            g_driveEnabled.store(false, std::memory_order_relaxed);
            BVR_LOG("[bsi] camera: drive off (passthrough)");
        } else {
            BVR_LOG("[bsi] camera: drive=%s lane=%s driving=%d scale=%.1f recenter=%s "
                    "(bsicam drive on|off)",
                    g_driveEnabled.load(std::memory_order_relaxed) ? "ON" : "off", g_driveLane,
                    g_vrDriving.load(std::memory_order_relaxed) ? 1 : 0,
                    g_worldScale.load(std::memory_order_relaxed),
                    g_haveRecenter ? "captured" : "pending");
        }
        return true;
    }
    if (strncmp(args, "off", 3) == 0) {
        // Deliberately does NOT uninstall. This stops the detour's body, which
        // stops the game-thread command pump, which is exactly the positive
        // control for the pump lease in core/framework/command: four seconds
        // later `vrcmd` must report the Present pump has resumed degraded.
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: observation DISABLED. The game-thread command pump goes silent "
                "with it - the Present pump should take back over within the lease window, "
                "degraded. `bsicam on` restores.");
        return true;
    }
    if (strncmp(args, "on", 2) == 0) {
        g_enabled.store(true, std::memory_order_relaxed);
        g_lastBeatMs = 0;
        BVR_LOG("[bsi] camera: observation enabled");
        return true;
    }
    return false;
}

void draw_debug_ui() {
    // The I4 in-headset surface FIRST and default-open: anything judged by
    // eye gets a control here, never a typed command (alt-tabbing to type
    // destabilises the XR session). Sliders and buttons write atomics only;
    // the game thread consumes them at the next detour call.
    if (ImGui::CollapsingHeader("VR camera (I4)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text(g_vrDriving.load(std::memory_order_relaxed)
                        ? "camera: driven by HMD pose (lane on the drive heartbeat)"
                        : "camera: game (enable the drive; live lane needs an XR session)");
        {
            bool drive = g_driveEnabled.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Drive camera from HMD (quad stays the screen)", &drive))
                g_driveEnabled.store(drive, std::memory_order_relaxed);
        }
        if (ImGui::Button("Recenter (seated pose + view yaw)"))
            g_recenterRequested.store(true, std::memory_order_relaxed);
        {
            float ws = g_worldScale.load(std::memory_order_relaxed);
            if (ImGui::SliderFloat("World scale (UU per m)", &ws, 10.0f, 200.0f, "%.0f"))
                g_worldScale.store(ws, std::memory_order_relaxed);
        }
        ImGui::Text("head offset: (%.1f %.1f %.1f) UU",
                    g_headOffX.load(std::memory_order_relaxed),
                    g_headOffY.load(std::memory_order_relaxed),
                    g_headOffZ.load(std::memory_order_relaxed));
        ImGui::TextDisabled("persist tuning: vrpreset save (worldScale)");
    }

    // I5 stereo. The checkbox POSTS - the game thread applies at the next
    // detour call (rung 3 makes the toggle install a hook, which must never
    // happen on the render thread this UI draws on).
    if (ImGui::CollapsingHeader("VR stereo (I5)", ImGuiTreeNodeFlags_DefaultOpen)) {
        {
            bool armed = g_stereoArmed.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("VR stereo (projection layer)", &armed))
                g_vrstereoPending.store(armed ? 1 : 0, std::memory_order_relaxed);
        }
        ImGui::Text("camera mode: %s (core: requested AND session AND projection-ready)",
                    bvr::vr::vr_camera_mode() ? "LIVE" : "off");
        {
            float ipd = g_ipdMm.load(std::memory_order_relaxed);
            if (ImGui::SliderFloat("IPD (mm)", &ipd, 50.0f, 75.0f, "%.1f"))
                g_ipdMm.store(ipd, std::memory_order_relaxed);
        }
        // I6: the FOV lever. Checkbox + slider write the atomic only; the
        // engine write happens per dispatch in the detour (apply_fov_lever).
        {
            float lever = g_fovLeverDeg.load(std::memory_order_relaxed);
            bool armed = lever > 0.0f;
            if (ImGui::Checkbox("FOV lever (fill the eye)", &armed))
                g_fovLeverDeg.store(armed ? (lever > 0.0f ? lever : 100.0f) : 0.0f,
                                    std::memory_order_relaxed);
            if (armed) {
                float deg = lever > 0.0f ? lever : 100.0f;
                if (ImGui::SliderFloat("horizontal FOV (deg)", &deg, 60.0f, 140.0f, "%.0f"))
                    g_fovLeverDeg.store(deg, std::memory_order_relaxed);
                ImGui::Text("lever: writes %u  faults %u",
                            g_fovLeverWrites.load(std::memory_order_relaxed),
                            g_fovLeverFaults.load(std::memory_order_relaxed));
            }
        }
        ImGui::Text("claim: tanV %.4f  aspect %.4f  hfov %.1f deg",
                    g_claimTanV.load(std::memory_order_relaxed),
                    g_lastClaimAspect.load(std::memory_order_relaxed),
                    g_lastClaimHfovDeg.load(std::memory_order_relaxed));
        ImGui::TextDisabled("lever OFF: claim tracks bsifov tanv (fix manually if the in-game "
                            "slider moved). Lever ON: claim tracks the lever.");
        ImGui::TextDisabled("persist tuning: vrpreset save (worldScale, ipd, claim, lever)");
        // s43b: the jumpy-camera A/B. Which locate generation the submitted
        // eyes are attributed to - mis-attribution on this threaded renderer
        // is the reprojection-wobble suspect. Radio writes the core selector
        // directly (atomic int, safe from the render thread).
        ImGui::Separator();
        ImGui::Text("POSE ATTRIBUTION (jumpy-camera A/B, s43b)");
        {
            int lag = bvr::vr::get_pose_lag();
            bool changed = false;
            changed |= ImGui::RadioButton("fresh (0)", &lag, 0);
            ImGui::SameLine();
            changed |= ImGui::RadioButton("1 back (default)", &lag, 1);
            ImGui::SameLine();
            changed |= ImGui::RadioButton("2 back (threaded)", &lag, 2);
            if (changed) bvr::vr::set_pose_lag(lag);
            ImGui::Text("head delta between generations: %.2f deg/pair",
                        bvr::vr::get_pose_gen_delta_deg());
            ImGui::TextDisabled("turn your head steadily; pick the one with no wobble/drag. "
                                "Wrong lag = the world bounces in proportion to head speed.");
        }
    }

    if (ImGui::CollapsingHeader("RENDER RESOLUTION (I6, applies live + persists)")) {
        // Render thread: reads and one posted atomic only. Ini re-read is
        // throttled to 1 Hz (BS2's rule - BS1 re-read every frame; don't).
        static game_ini::Resolution s_ini;
        static uint64_t s_lastIniReadMs = 0;
        const uint64_t nowMs = GetTickCount64();
        if (nowMs - s_lastIniReadMs >= 1000) {
            s_lastIniReadMs = nowMs;
            s_ini = game_ini::read_resolution();
        }
        unsigned liveW = 0, liveH = 0;
        bvr::hud::backbuffer_dims(&liveW, &liveH);
        if (s_ini.valid)
            ImGui::Text("ini: %dx%d   live backbuffer: %ux%u", s_ini.x, s_ini.y, liveW, liveH);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "XUserOptions.ini not found - boot-persist lane dead");
        if (s_ini.valid && liveW && (static_cast<unsigned>(s_ini.x) != liveW ||
                                     static_cast<unsigned>(s_ini.y) != liveH))
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "ini and live differ - Apply persists both lanes");
        {
            uint32_t rw = 0, rh = 0;
            if (bvr::vr::recommended_eye_size(&rw, &rh))
                ImGui::Text("headset recommends %ux%u per eye", rw, rh);
            else
                ImGui::TextDisabled("headset recommendation needs a live XR session");
        }

        constexpr int kCustom = static_cast<int>(std::size(kResModes));
        static int s_sel = -1;
        static int s_customW = 2560, s_customH = 1440;
        if (s_sel < 0) {
            // First draw: preselect from the ini so the combo opens on truth.
            s_sel = kCustom;
            for (int i = 0; i < kCustom; ++i)
                if (s_ini.valid && kResModes[i].w == s_ini.x && kResModes[i].h == s_ini.y)
                    s_sel = i;
            if (s_ini.valid) {
                s_customW = s_ini.x;
                s_customH = s_ini.y;
            }
        }
        auto modeLabel = [](int i) {
            return i < kCustom ? kResModes[i].label : "custom";
        };
        if (ImGui::BeginCombo("mode", modeLabel(s_sel))) {
            for (int i = 0; i <= kCustom; ++i)
                if (ImGui::Selectable(modeLabel(i), s_sel == i)) s_sel = i;
            ImGui::EndCombo();
        }
        int selW = s_sel < kCustom ? kResModes[s_sel].w : s_customW;
        int selH = s_sel < kCustom ? kResModes[s_sel].h : s_customH;
        if (s_sel == kCustom) {
            ImGui::InputInt("width", &s_customW);
            ImGui::InputInt("height", &s_customH);
            if (s_customW < 640) s_customW = 640;
            if (s_customW > 8192) s_customW = 8192;
            if (s_customH < 480) s_customH = 480;
            if (s_customH > 8192) s_customH = 8192;
            selW = s_customW;
            selH = s_customH;
        }
        const float selAspect = selH > 0 ? static_cast<float>(selW) / selH : 0.0f;
        ImGui::Text("selected: %dx%d  (%.2f MPx, aspect %.3f)", selW, selH,
                    selW * selH / 1.0e6f, selAspect);
        if (selAspect > 1.25f || (selAspect > 0.0f && selAspect < 0.8f))
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "aspect %.3f is far from the eye's ~0.93 - much of this render "
                               "falls outside the lenses",
                               selAspect);
        ImGui::TextDisabled("the FOV law is vertical-referenced: a squarer render pays only "
                            "combined with the FOV lever");
        if (ImGui::Button("Apply (live setres + write ini)"))
            g_resApplyPending.store(
                (static_cast<uint64_t>(static_cast<uint32_t>(selW)) << 32) |
                    static_cast<uint32_t>(selH),
                std::memory_order_relaxed);
    }

    if (!ImGui::CollapsingHeader("Camera seam (DR-I2 observation)")) return;
    ImGui::Text("hook: %s   fired: %s", hook_live() ? "installed" : "not installed",
                has_fired() ? "YES" : "no");
    ImGui::Text("calls: %u   silent: %llu ms", g_callCount.load(),
                static_cast<unsigned long long>(silent_ms()));
    ImGui::Text("tid: camera %u / present %u", g_cameraTid.load(),
                bvr::d3d11_hook::last_present_tid());
    ImGui::Text("paths: cached %u | camera %u | target %u | unreadable %u", g_pathCached.load(),
                g_pathCamera.load(), g_pathTarget.load(), g_pathUnknown.load());
    if (g_last.valid) {
        ImGui::Text("engine loc (%.1f %.1f %.1f)", g_last.loc.x, g_last.loc.y, g_last.loc.z);
        ImGui::Text("engine rot (%d %d %d) = (%.1f %.1f %.1f) deg", g_last.rot.pitch,
                    g_last.rot.yaw, g_last.rot.roll, g_last.rot.pitch * kRotToDeg,
                    g_last.rot.yaw * kRotToDeg, g_last.rot.roll * kRotToDeg);
    }
    ImGui::TextDisabled("snapshot is pre-drive: this is the engine's own view");
}

} // namespace bvr::bsi::camera
