#pragma once
// The manual reload's programs and its state machine, pure (2026-10-20). Host-tested by
// tools\tests\reload-rig-tests.cpp; manual_reload.cpp feeds it the live numbers and carries out
// what it asks for.
//
// THE IDEA. A reload is the game's OWN reload clip, held and driven by the player's hands. The game
// starts its reload as always (PlayerWeapon.Reload), and its script waits on the clip
// (Weapon.FinishAnimations) - so while the mod holds the clip's time, the game waits, and when the
// clip ends the game refills the gun exactly as it would have: ammo, sounds and timing stay the
// game's. Each step of a reload is a SEGMENT of the clip, cut where the clip's own off hand works a
// part of the gun - pulls the drum, folds the barrel, presses a shell in, racks the bolt. The clip
// time inside a segment follows the player's hand along the path the clip's hand takes there (in
// the frame of the gun's grip), so pulling the drum down IS the clip pulling it down, at the
// player's pace, and the part never leaves the hand that moves it.
//
// Between segments the part a hand carries rides that hand: the spent drum or cylinder until the
// grip lets go (then it is gone), a new one from the pouch at the waist, a shell for the shotgun.
// Brought near where it goes, it is guided home: the clip takes over again along the clip's path.
//
// The paths and times are the clips' own, measured with `vrbones reloadrec` (bones.cpp): palm
// positions in the grip's frame, rig units, and clip seconds; a variant clip of a different
// length plays the same program scaled in time.

#include "arm_ik.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace bvr::b1r::reload_rig {

using arm_ik::Vec;

struct Pt {
    float t;   // clip seconds (the recorded clip)
    Vec p;     // where the clip has the off hand's palm then, in the grip's frame (rig units)
};

enum class Kind : uint8_t {
    Pull,    // grab the part at the path's start and drag it along the path; the clip follows
    Carry,   // the part just pulled rides the hand until the grip lets go (then it is dropped)
    Fetch,   // a new part from the pouch at the waist (the grip there)
    Insert,  // the carried part is guided home along the path when brought near it
    Flick,   // a flick of the gun hand (or pushing the part along the path) - the pistol's close
    Play,    // the clip runs on its own from t0 to t1
    Finish,  // let the clip run to its end: the game completes the reload
};

struct Phase {
    Kind kind;
    float t0, t1;      // clip seconds, recorded clip
    const Pt* path;    // Pull / Insert / Flick (the push alternative); null otherwise
    int n;
    const char* part;  // the gun part(s) a Carry/Fetch/Insert moves, '+'-separated bone names
    const char* say;   // for the log
    bool atPalm = false; // carried at the palm (the clip's hand does not hold it at this time: the
                         // shotgun's shell is by the port when its loop begins)
};

struct Program {
    const wchar_t* weaponClass;
    const wchar_t* clip;   // the weapon's clip name (Weapon.AnimationHandle)
    float clipLen;         // the recorded clip's length
    const Phase* phases;
    int n;
    bool entryAtPouch;     // the reload begins with a part from the pouch (the shotgun's shell)
    // 2026-10-20, the eject: the clip time where the gun is open and the spent part still sits in it
    // (held while that part falls), and the part. ejectT < 0: this clip does not eject.
    float ejectT = -1.0f;
    const char* ejectPart = nullptr;
};

// ---- the programs ------------------------------------------------------------------------------
// Tommy Gun ('Reload', 3.33 s): the drum down and out, a new drum up and in, the bolt racked.
inline constexpr Pt kTommyDrumOut[] = {
    {0.267f, {19.3f, 3.3f, -11.8f}}, {0.369f, {17.0f, 4.6f, -28.5f}},
    {0.466f, {15.8f, 2.9f, -46.4f}}, {0.567f, {15.9f, 4.9f, -66.2f}}};
inline constexpr Pt kTommyDrumIn[] = {
    {0.933f, {2.8f, -1.4f, -55.3f}}, {1.023f, {14.8f, 8.9f, -41.4f}},
    {1.113f, {16.7f, 15.1f, -11.4f}}, {1.222f, {19.6f, 7.5f, -8.7f}}};
inline constexpr Pt kTommyBolt[] = {
    {2.058f, {8.9f, 12.6f, 9.5f}}, {2.144f, {-3.6f, 4.2f, 19.0f}}, {2.244f, {-4.6f, 1.5f, 13.0f}}};
inline constexpr Phase kTommy[] = {
    {Kind::Pull, 0.267f, 0.567f, kTommyDrumOut, 4, "TG_AmmoClip", "pull the drum out"},
    {Kind::Carry, 0.567f, 0.567f, nullptr, 0, "TG_AmmoClip", "drop the spent drum"},
    {Kind::Fetch, 0.933f, 0.933f, nullptr, 0, "TG_AmmoClip", "take a drum from the pouch"},
    {Kind::Insert, 0.933f, 1.222f, kTommyDrumIn, 4, "TG_AmmoClip", "seat the drum"},
    {Kind::Pull, 2.058f, 2.244f, kTommyBolt, 3, nullptr, "rack the bolt"},
    {Kind::Play, 2.244f, 2.80f, nullptr, 0, nullptr, "the bolt runs home"},
    {Kind::Finish, 3.20f, 3.33f, nullptr, 0, nullptr, "done"}};

// Pistol ('FastReload', 1.80 s): a top-break revolver - fold the barrel down, the cylinder out, a
// new cylinder in, flick it shut.
inline constexpr Pt kPistolOpen[] = {
    {0.189f, {25.7f, -4.6f, 24.3f}}, {0.277f, {29.6f, -2.7f, 15.4f}}, {0.377f, {30.3f, 1.4f, 5.3f}}};
inline constexpr Pt kPistolCylOut[] = {
    {0.466f, {36.9f, -4.2f, 5.6f}}, {0.566f, {34.4f, -8.4f, 2.2f}}, {0.657f, {25.6f, -51.4f, 42.2f}}};
inline constexpr Pt kPistolCylIn[] = {
    {0.756f, {7.1f, -20.5f, 47.8f}}, {0.847f, {16.2f, -2.8f, 25.4f}}, {0.934f, {24.1f, -4.0f, 21.4f}}};
// Closing has no hand in the clip (it is flicked shut); pushing the barrel back up the way it came
// down is the other way to do it.
inline constexpr Pt kPistolClose[] = {
    {1.40f, {30.3f, 1.4f, 5.3f}}, {1.47f, {29.6f, -2.7f, 15.4f}}, {1.55f, {25.7f, -4.6f, 24.3f}}};
inline constexpr Phase kPistol[] = {
    {Kind::Pull, 0.189f, 0.377f, kPistolOpen, 3, nullptr, "fold the barrel down"},
    {Kind::Pull, 0.466f, 0.657f, kPistolCylOut, 3, nullptr, "pull the cylinder out"},
    {Kind::Carry, 0.657f, 0.657f, nullptr, 0, "DrumParent+drum", "drop the spent cylinder"},
    {Kind::Fetch, 0.756f, 0.756f, nullptr, 0, "DrumParent+drum", "take a cylinder from the pouch"},
    {Kind::Insert, 0.756f, 0.934f, kPistolCylIn, 3, "DrumParent+drum", "seat the cylinder"},
    {Kind::Flick, 1.40f, 1.55f, kPistolClose, 3, nullptr, "flick it shut"},
    {Kind::Finish, 1.70f, 1.80f, nullptr, 0, nullptr, "done"}};

// Shotgun: three clips. The start ('SingleFrame') runs on; each loop ('Reload_Loop', 0.49 s) is one
// shell from the pouch pressed into the port; the end ('Reload', 0.95 s) is the pump, back and
// forward. The game decides loop or end by the shells it still needs.
inline constexpr Phase kShotgunStart[] = {{Kind::Finish, 0.0f, 0.033f, nullptr, 0, nullptr, "start"}};
inline constexpr Pt kShotgunShell[] = {
    {0.034f, {16.0f, 6.8f, -23.2f}}, {0.079f, {26.1f, 5.9f, -2.7f}}, {0.122f, {27.3f, 5.2f, 0.7f}},
    {0.168f, {28.1f, 5.3f, 2.7f}},   {0.222f, {29.3f, 6.3f, 3.6f}},  {0.267f, {29.8f, 6.9f, 4.1f}}};
inline constexpr Phase kShotgunLoop[] = {
    {Kind::Fetch, 0.034f, 0.034f, nullptr, 0, "SG_Shell", "take a shell from the pouch", true},
    {Kind::Insert, 0.034f, 0.267f, kShotgunShell, 6, "SG_Shell", "press the shell in", true},
    {Kind::Play, 0.267f, 0.49f, nullptr, 0, nullptr, "the hand leaves the port"},
    {Kind::Finish, 0.49f, 0.49f, nullptr, 0, nullptr, "next"}};
inline constexpr Pt kShotgunPumpBack[] = {
    {0.409f, {52.7f, 18.8f, 16.9f}}, {0.445f, {42.6f, 14.5f, 12.8f}}, {0.487f, {38.4f, 12.9f, 10.9f}}};
inline constexpr Pt kShotgunPumpFwd[] = {
    {0.532f, {38.4f, 13.0f, 10.9f}}, {0.588f, {39.2f, 13.2f, 11.2f}}, {0.634f, {44.1f, 15.1f, 13.3f}},
    {0.676f, {51.1f, 17.7f, 16.4f}}};
inline constexpr Phase kShotgunEnd[] = {
    {Kind::Pull, 0.409f, 0.487f, kShotgunPumpBack, 3, nullptr, "pump back"},
    {Kind::Pull, 0.532f, 0.676f, kShotgunPumpFwd, 4, nullptr, "pump forward"},
    {Kind::Finish, 0.90f, 0.95f, nullptr, 0, nullptr, "done"}};

inline constexpr Program kPrograms[] = {
    // Eject: the Tommy Gun's drum sits seated as the clip's hand reaches it (0.267, the pull's start);
    // the pistol is folded open with its cylinder still in at 0.466 (the cylinder's pull begins).
    {L"MachineGun", L"Reload", 3.33f, kTommy, 7, false, 0.267f, "TG_AmmoClip"},
    {L"Pistol", L"FastReload", 1.80f, kPistol, 7, false, 0.466f, "DrumParent+drum"},
    {L"Pistol", L"Reload", 1.80f, kPistol, 7, false, 0.466f, "DrumParent+drum"}, // no fast-reload upgrade: scaled
    {L"Shotgun", L"SingleFrame", 0.033f, kShotgunStart, 1, true},
    {L"Shotgun", L"Reload_Loop", 0.49f, kShotgunLoop, 4, true},
    {L"Shotgun", L"Reload", 0.95f, kShotgunEnd, 3, true},
};

inline bool same(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) ++a, ++b;
    return *a == *b;
}
// The program for a weapon's clip; null when the clip is not one of ours.
inline const Program* find(const wchar_t* weaponClass, const wchar_t* clip) {
    for (const Program& p : kPrograms)
        if (same(p.weaponClass, weaponClass) && same(p.clip, clip)) return &p;
    return nullptr;
}
// The program a reload of this weapon begins with (its first-listed), or null: no manual reload.
inline const Program* entry(const wchar_t* weaponClass) {
    for (const Program& p : kPrograms)
        if (same(p.weaponClass, weaponClass)) return &p;
    return nullptr;
}

// ---- the path ----------------------------------------------------------------------------------
struct Projection {
    float t = 0;        // clip seconds at the nearest point
    float progress = 0; // 0..1 along the path's length
    float dist = 1e9f;  // distance from the path (rig units)
};
inline Projection project(const Pt* path, int n, Vec p) {
    Projection best;
    if (!path || n < 2) return best;
    float total = 0;
    for (int i = 0; i + 1 < n; ++i) total += arm_ik::length(path[i + 1].p - path[i].p);
    float along = 0;
    for (int i = 0; i + 1 < n; ++i) {
        const Vec a = path[i].p, d = path[i + 1].p - path[i].p;
        const float L2 = arm_ik::dot(d, d), L = std::sqrt(L2);
        float u = L2 > 1e-9f ? arm_ik::dot(p - a, d) / L2 : 0.0f;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        const float dist = arm_ik::length(p - (a + d * u));
        if (dist < best.dist) {
            best.dist = dist;
            best.t = path[i].t + (path[i + 1].t - path[i].t) * u;
            best.progress = total > 1e-6f ? (along + L * u) / total : 0.0f;
        }
        along += L;
    }
    return best;
}

// ---- the machine -------------------------------------------------------------------------------
struct Config {
    float grabCm = 12.0f;       // reach to grab a part (the grab radius the two-handed grip uses)
    float insertCm = 6.0f;      // a carried part within this of its path is guided home
    float insertLeaveCm = 10.0f;// and leaves the guide past this
    float flickDegPerSec = 320; // a flick of the gun hand
    float startTimeoutS = 0.6f; // the game must begin its reload within this, or there is none
    float settleProgress = 0.85f;// let go past this and a pull completes itself
    float dropS = 0.7f;         // the eject: how long the spent part falls before it is gone
};

struct Input {
    float now = 0;              // seconds
    // The weapon's reload clip, when the game is playing one.
    bool clipLive = false;
    const wchar_t* clip = nullptr;
    float clipLen = 0;
    bool clipNew = false;       // a different clip (handle) than last frame
    const Program* clipProgram = nullptr; // find(weaponClass, clip)
    const Program* entryProgram = nullptr;// entry(weaponClass), null: no manual reload for this gun
    bool canReload = true;      // the gun is not full
    Vec hand{};                 // the free hand's palm, grip frame, rig units (the recorded frame)
    float unitsPerCm = 1;       // rig units per centimetre
    bool grip = false, gripRise = false; // the free hand's grip
    bool inPouch = false;       // the free hand is at the waist
    float gunDegPerSec = 0;     // the gun hand's turn rate
    bool adopt = false;         // a reload the GAME began (the gun ran dry), not the reload button:
                                // it waits for the hands like one they began
    bool eject = false;         // 2026-10-20: the reload button with the manual reload on - the gun
                                // opens and its magazine drops, then the reload waits at the
                                // program's first Fetch (a new one from the pouch)
    bool ejectAdopted = false;  // and a reload the game began (the gun ran dry) ejects the same way
};

enum class Act : uint8_t { None, StartReload, Abort };
enum class Haptic : uint8_t { None, Grab, Tick, Seat, Deny, Drop };

struct Output {
    Act act = Act::None;
    bool hold = false;          // hold the clip at `t` (paused)
    float t = 0;                // clip seconds, ACTUAL clip (scaled)
    bool run = false;           // let the clip run (resume)
    bool jump = false;          // with run: set the clip to `t` first (once)
    int carry = 0;              // 0 the clip has the part; 1 the part rides the hand; 2 it is gone;
                                // 3 it falls out of the gun (`fallS` seconds into the fall)
    float fallS = 0;
    const char* part = nullptr; // the part(s) `carry` applies to
    bool atPalm = false;        // carried at the palm, not where the clip's hand holds it
    Vec carryFrom{};            // the clip's palm where the carried part sits on it (grip frame)
    bool claim = false;         // the free hand's grip is the reload's (no bumper reaches the game)
    float reach = -1;           // idle: the free hand's distance to what starts a reload, over the grab
                                // radius (<= 1 in reach; the grip arbiter compares it with the other
                                // grabs'); -1 nothing to start
    bool buzz = false;          // the hand is within reach of what it should take next
    Haptic haptic = Haptic::None;
    const char* event = nullptr;// a transition worth a log line
    bool active = false;        // a manual reload is in progress
    bool ejected = false;       // this frame the spent part starts to fall (`part` names it)
};

// The first Fetch of a program (where an eject starts), -1 when it has none.
inline int first_fetch(const Program* p) {
    if (!p) return -1;
    for (int i = 0; i < p->n; ++i)
        if (p->phases[i].kind == Kind::Fetch) return i;
    return -1;
}

class Machine {
public:
    void reset() { *this = Machine{}; }
    bool active() const { return state_ != State::Idle; }
    const Program* program() const { return prog_; }
    int phase() const { return phase_; }

    Output update(const Input& in, const Config& c) {
        Output o;
        const float grabU = c.grabCm * in.unitsPerCm;
        grabU_ = grabU;
        switch (state_) {
            case State::Idle:
                if (in.adopt && in.clipLive && in.clipProgram) {
                    begin(in.clipProgram, in);
                    if (in.ejectAdopted && start_drop(in, o)) {
                        running(in, c, grabU, o, /*fresh=*/true);
                        o.event = "the gun ran dry: it opens and the magazine drops - take a new one from the pouch";
                        break;
                    }
                    running(in, c, grabU, o, /*fresh=*/true);
                    if (!o.event) o.event = "the game's own reload waits for the hands";
                    break;
                }
                idle(in, c, grabU, o);
                break;
            case State::Starting:
                starting(in, c, o);
                // The clip it was waiting for is held from this very frame.
                if (state_ == State::Running) {
                    const char* ev = o.event;
                    running(in, c, grabU, o, /*fresh=*/true);
                    if (!o.event) o.event = ev;
                }
                break;
            case State::Running: running(in, c, grabU, o); break;
        }
        o.active = state_ != State::Idle;
        if (o.part && prog_ && state_ == State::Running) o.atPalm = prog_->phases[phase_].atPalm;
        return o;
    }

private:
    enum class State : uint8_t { Idle, Starting, Running };
    enum class Sub : uint8_t { Wait, Drag, Guided };
    State state_ = State::Idle;
    Sub sub_ = Sub::Wait;
    const Program* prog_ = nullptr;
    int phase_ = 0;
    float startAt_ = 0, playT_ = 0, lastNow_ = 0, scale_ = 1;
    float lastProgress_ = 0, lastT_ = 0, grabU_ = 0;
    bool carrying_ = false;     // a part rides the hand
    bool carriedFromEntry_ = false;
    bool ejecting_ = false;     // the reload button started this one: it ejects when the clip starts
    bool dropping_ = false;     // the spent part is falling; then the first Fetch
    float dropAt_ = 0;

    // The eject: hold the clip open with the spent part seated while it falls, then wait at the first
    // Fetch. False (nothing changes) for a program that does not eject.
    bool start_drop(const Input& in, Output& o) {
        if (!prog_ || prog_->ejectT < 0 || first_fetch(prog_) < 0) return false;
        dropping_ = true;
        dropAt_ = in.now;
        o.ejected = true;
        o.part = prog_->ejectPart;
        return true;
    }

    const Phase& ph() const { return prog_->phases[phase_]; }
    float T(float recorded) const { return recorded * scale_; }

    static bool within(const Pt* path, int n, Vec p, float r) { return n > 0 && arm_ik::length(p - path[0].p) <= r; }

    void idle(const Input& in, const Config&, float grabU, Output& o) {
        const Program* e = in.entryProgram;
        if (in.eject && e && e->ejectT >= 0 && !in.canReload) {
            o.haptic = Haptic::Deny; // full, or nothing to reload with
            o.event = "eject: nothing to reload (full, or no spare rounds)";
            return;
        }
        if (!e || !in.canReload || in.clipLive) return;
        // The reload button: the gun opens and its magazine drops (a program that ejects only).
        if (in.eject) {
            if (e->ejectT < 0 || first_fetch(e) < 0) return;
            state_ = State::Starting;
            startAt_ = in.now;
            ejecting_ = true;
            o.act = Act::StartReload;
            o.claim = true;
            o.event = "eject: the game is asked to reload";
            return;
        }
        const Phase& first = e->phases[0];
        bool go = false;
        if (e->entryAtPouch) {
            o.claim = in.inPouch;
            o.buzz = in.inPouch;
            o.reach = in.inPouch ? 0.5f : -1.0f; // a zone, not a point: mid-reach
            go = in.inPouch && in.gripRise;
            carriedFromEntry_ = go;
        } else if (first.kind == Kind::Pull) {
            const bool near = within(first.path, first.n, in.hand, grabU);
            if (first.n > 0 && grabU > 0) o.reach = arm_ik::length(in.hand - first.path[0].p) / grabU;
            o.claim = near;
            o.buzz = near;
            go = near && in.gripRise;
        }
        if (!go) return;
        state_ = State::Starting;
        startAt_ = in.now;
        o.act = Act::StartReload;
        o.claim = true;
        o.haptic = Haptic::Grab;
        o.event = "start: the game is asked to reload";
    }

    void starting(const Input& in, const Config& c, Output& o) {
        o.claim = true;
        if (in.clipLive && in.clipProgram) {
            begin(in.clipProgram, in);
            o.event = "the game's reload clip is held";
            if (ejecting_) {
                ejecting_ = false;
                if (start_drop(in, o)) o.event = "eject: the gun opens, the magazine drops - take a new one from the pouch";
            }
            return;
        }
        if (in.now - startAt_ > c.startTimeoutS) {
            ejecting_ = false;
            state_ = State::Idle;
            o.act = Act::Abort;
            o.haptic = Haptic::Deny;
            o.event = "no reload (full, or no ammunition)";
        }
    }

    void begin(const Program* p, const Input& in) {
        prog_ = p;
        phase_ = 0;
        dropping_ = false;
        state_ = State::Running;
        scale_ = (p->clipLen > 1e-4f && in.clipLen > 1e-4f) ? in.clipLen / p->clipLen : 1.0f;
        enter(in);
    }

    void enter(const Input& in) {
        sub_ = Sub::Wait;
        lastProgress_ = 0;
        lastT_ = ph().t0;
        playT_ = T(ph().t0);
        lastNow_ = in.now;
        // A pull the hand is already holding at its start - the reload's own first pull, or the next
        // stroke of the same part (the pump back, then forward) - is already being dragged.
        if (ph().kind == Kind::Pull && in.grip && within(ph().path, ph().n, in.hand, grabU_)) sub_ = Sub::Drag;
        if (ph().kind == Kind::Fetch && carriedFromEntry_ && in.grip) {
            carrying_ = true; // the shell taken at the pouch to start the reload
            carriedFromEntry_ = false;
            next(in);
        }
    }

    void next(const Input& in) {
        if (phase_ + 1 < prog_->n) {
            ++phase_;
            enter(in);
        }
    }

    void running(const Input& in, const Config& c, float grabU, Output& o, bool fresh = false) {
        // The clip changed under us (the shotgun's next loop or its end): its own program.
        if (in.clipNew && !fresh) {
            if (in.clipLive && in.clipProgram) {
                begin(in.clipProgram, in);
                o.event = "next clip";
            } else if (!in.clipLive) {
                state_ = State::Idle;
                carrying_ = false;
                o.event = "the reload ended";
                return;
            }
        }
        if (!in.clipLive) {
            state_ = State::Idle;
            carrying_ = false;
            o.event = "the reload ended";
            return;
        }
        // The eject: the gun held open, the spent part falling out of it; then the first Fetch.
        if (dropping_) {
            lastNow_ = in.now;
            o.claim = true;
            o.hold = true;
            o.t = T(prog_->ejectT);
            o.part = prog_->ejectPart;
            o.fallS = in.now - dropAt_;
            o.carry = o.fallS < c.dropS ? 3 : 2;
            if (o.fallS >= c.dropS) {
                dropping_ = false;
                phase_ = first_fetch(prog_);
                enter(in);
            }
            return;
        }
        const Phase& p = ph();
        const float dt = in.now - lastNow_;
        lastNow_ = in.now;
        o.claim = true;
        switch (p.kind) {
            case Kind::Pull: {
                const Projection pr = project(p.path, p.n, in.hand);
                if (sub_ == Sub::Wait) {
                    const bool near = within(p.path, p.n, in.hand, grabU);
                    o.buzz = near;
                    o.hold = true;
                    o.t = T(p.t0);
                    if (near && in.gripRise) {
                        sub_ = Sub::Drag;
                        o.haptic = Haptic::Grab;
                    }
                    break;
                }
                // Dragging: only forward along the path, never back past where it got to. The clip
                // time is the path's own at the nearest point (the clip moves unevenly).
                const float prog = pr.progress > lastProgress_ ? pr.progress : lastProgress_;
                if (prog - lastProgress_ > 0.2f) o.haptic = Haptic::Tick;
                lastProgress_ = prog;
                if (pr.t > lastT_) lastT_ = pr.t;
                o.hold = true;
                o.t = T(lastT_);
                if (prog >= 0.98f || (!in.grip && prog >= c.settleProgress)) {
                    o.haptic = Haptic::Seat;
                    o.event = p.say;
                    o.t = T(p.t1);
                    next(in);
                } else if (!in.grip) {
                    sub_ = Sub::Wait; // let go short: it springs back to where it began
                    lastProgress_ = 0;
                    lastT_ = p.t0;
                }
                break;
            }
            case Kind::Carry: {
                o.hold = true;
                o.t = T(p.t0);
                o.part = p.part;
                o.carry = 1;
                o.carryFrom = prog_->phases[phase_ - 1].path[prog_->phases[phase_ - 1].n - 1].p;
                if (!in.grip) {
                    o.carry = 2;
                    o.haptic = Haptic::Drop;
                    o.event = p.say;
                    next(in);
                }
                break;
            }
            case Kind::Fetch: {
                o.hold = true;
                o.t = T(p.t0);
                o.part = p.part;
                o.carry = 2;
                o.buzz = in.inPouch;
                if (in.inPouch && in.gripRise) {
                    carrying_ = true;
                    o.haptic = Haptic::Grab;
                    o.event = p.say;
                    next(in);
                }
                break;
            }
            case Kind::Insert: {
                const Projection pr = project(p.path, p.n, in.hand);
                o.part = p.part;
                o.hold = true;
                if (!in.grip && sub_ != Sub::Guided) {
                    // Let go away from the gun: the part falls; take another.
                    o.carry = 2;
                    o.t = T(p.t0);
                    o.haptic = Haptic::Drop;
                    carrying_ = false;
                    if (phase_ > 0 && prog_->phases[phase_ - 1].kind == Kind::Fetch) {
                        --phase_;
                        enter(in);
                    }
                    break;
                }
                if (sub_ != Sub::Guided) {
                    o.carry = 1;
                    o.t = T(p.t0);
                    o.carryFrom = p.path[0].p;
                    if (pr.dist <= c.insertCm * in.unitsPerCm) {
                        sub_ = Sub::Guided;
                        o.haptic = Haptic::Tick;
                    }
                    break;
                }
                // Guided home by the clip.
                if (pr.dist > c.insertLeaveCm * in.unitsPerCm && pr.progress < 0.5f) {
                    sub_ = Sub::Wait;
                    lastProgress_ = 0;
                    lastT_ = p.t0;
                    o.carry = 1;
                    o.t = T(p.t0);
                    o.carryFrom = p.path[0].p;
                    break;
                }
                const float prog = pr.progress > lastProgress_ ? pr.progress : lastProgress_;
                lastProgress_ = prog;
                if (pr.t > lastT_) lastT_ = pr.t;
                o.t = T(lastT_);
                if (prog >= 0.97f || (!in.grip && prog >= 0.5f)) {
                    o.t = T(p.t1);
                    o.haptic = Haptic::Seat;
                    o.event = p.say;
                    carrying_ = false;
                    next(in);
                }
                break;
            }
            case Kind::Flick: {
                o.hold = true;
                o.t = T(p.t0);
                bool done = in.gunDegPerSec >= c.flickDegPerSec;
                if (!done && p.path) {
                    // Or push it shut along the path.
                    const Projection pr = project(p.path, p.n, in.hand);
                    if (sub_ == Sub::Wait && within(p.path, p.n, in.hand, grabU)) {
                        o.buzz = true;
                        if (in.gripRise) sub_ = Sub::Drag;
                    }
                    if (sub_ == Sub::Drag) {
                        lastProgress_ = pr.progress > lastProgress_ ? pr.progress : lastProgress_;
                        if (pr.t > lastT_) lastT_ = pr.t;
                        o.t = T(lastT_);
                        done = lastProgress_ >= 0.95f;
                        if (!in.grip && !done) sub_ = Sub::Wait;
                    }
                }
                if (done) {
                    o.haptic = Haptic::Seat;
                    o.event = p.say;
                    o.t = T(p.t1);
                    next(in);
                }
                break;
            }
            case Kind::Play: {
                playT_ += dt;
                o.hold = true;
                o.t = playT_ < T(p.t1) ? playT_ : T(p.t1);
                if (playT_ >= T(p.t1)) next(in);
                break;
            }
            case Kind::Finish: {
                // Jump to the clip's last moment once, then let it run out: the game completes.
                o.run = true;
                o.jump = sub_ == Sub::Wait;
                o.t = T(p.t0);
                sub_ = Sub::Drag;
                o.claim = false;
                break;
            }
        }
    }
};

} // namespace bvr::b1r::reload_rig
