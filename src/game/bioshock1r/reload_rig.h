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
    Slide,   // 2026-10-20: a part that slides along the gun (the Tommy Gun's bolt): grabbed near the path's
             // start, it follows the hand's travel back along the gun 1:1 (Phase::travel), both ways while
             // held, and the drawn hand is locked onto it in the clip's own grip (Output::handLock)
    PumpHand,// 2026-10-20: the shotgun reload's last pump, worked EXACTLY as between shots: the free hand's
             // grip is left to the two-handed grip (Output::pumpWait), the glue's Pump drives the clip from
             // t0 to t1 (PumpConfig over this clip), and Input::pumpDone moves on
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
    // 2026-10-20: a part with a handle on BOTH sides (the Tommy Gun's bolt): the hand's sideways
    // position does not count, only along and above the gun, and it is grabbed from this much further.
    bool bothSides = false;
    float grabScale = 1.0f;
    // 2026-10-20: where the clip is held while a Fetch waits and an Insert carries (< 0: t0) - the
    // moment the clip's own hand grips the new part at the gun (tools\blender\reload_pose.py: at the
    // fetch's t0 the game's hand has tossed it, 70-78 units off the palm), so a carried part sits in
    // the hand as the game holds it, fingers and all. The latch's snap plays on from there.
    float holdT = -1.0f;
    float travel = 0.0f; // Slide: the part's full travel back along the gun (grip-frame units)
    // Slide: > 0, the stroke completes once the hand has gone this many cm along it (the clip plays the
    // rest) - the shotgun's pump, as its pump between shots (Pump, after Half-Life 2 VR); <= 0: 95% of
    // the travel (the Tommy Gun's bolt).
    float strokeCm = -1.0f;
    // 2026-10-20: skipped when the gun still had a round in it at the reload (Input::chambered) - the
    // Tommy Gun's bolt: an open-bolt gun with a round left is not racked (the tester: "more realistic").
    bool skipLoaded = false;
    // 2026-10-22 late night (the crossbow's lever: "the hand doesn't actually grip the lever"): a Pull of a handle
    // the clip's own hand holds - the hand is drawn where the clip's hand is while it drags (Output::handLock, as
    // the Tommy's bolt), riding the lever through its arc.
    bool handOnPart = false;
};
inline float hold_t(const Phase& p) { return p.holdT >= 0 ? p.holdT : p.t0; }

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
    // 2026-10-20: a spare magazine can be taken from the pouch whenever this gun is held (the Tommy Gun's
    // drum, a prop of its own until it goes into the gun - Input::spareInHand), and a thrown one can be
    // caught in the open well (Input::caught).
    bool spare = false;
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
    // Held at 0.30 s while carrying: the clip's hand closing thumb and fingers on the drum's rim to
    // pull it (reload_pose.py with the drum on its socket; at 1.113 it only pushes it in with the
    // fingertips, the headset's "held at the fingertips").
    {Kind::Fetch, 0.933f, 0.933f, nullptr, 0, "TG_AmmoClip", "take a drum from the pouch", false, false, 1.0f, 0.30f},
    {Kind::Insert, 0.933f, 1.222f, kTommyDrumIn, 4, "TG_AmmoClip", "seat the drum", false, false, 1.0f, 0.30f},
    // The bolt (tools\blender\reload_pose.py): TG_Bolt runs 16.4 units straight back between 2.10 and
    // 2.20 s, the clip's hand on it. Grabbed near the clip's hand from either side, 1.25x the reach (2x
    // buzzed the hand "all the way to the opposite shoulder"). Not racked with a round still in the gun.
    {Kind::Slide, 2.10f, 2.20f, kTommyBolt, 3, nullptr, "rack the bolt", false, true, 1.25f, -1.0f, 16.4f, -1.0f, true},
    // From where the stroke leaves the clip (2.20), not 2.244: a phase's start is SET, and the 44 ms jumped over held
    // the bolt's slam home - its sound never played (2026-10-21, "the tommy gun slide sound").
    {Kind::Play, 2.20f, 2.80f, nullptr, 0, nullptr, "the bolt runs home", false, false, 1.0f, -1.0f, 0.0f, -1.0f, true},
    {Kind::Finish, 3.20f, 3.33f, nullptr, 0, nullptr, "done"}};

// Grenade launcher ('Reload', 4.00 s) - 2026-10-22 (MANUAL_RELOAD_PLAN.md step 3, the tester's design): the gun
// stays together - its clip (the chamber's 70-deg turn, the breech sliding apart, the box off the gun) is never
// played; it is HELD at 0 while the AMMO BOX (the gun's own AmmoBoxBone, with the grenade pack on AmmoDummy inside
// it - the pistol's cylinder is the gun's own bones the same way) is pulled off the top of the gun, a new one taken
// from the pouch and seated, and then jumped to its last frames so the game completes the reload and refills.
// (The first cut, 20:50 run, took the pack alone off the box - "it gives me the ammo and drops the ammo rather than
// the whole ammo box": the box is the magazine.) The box sits at (26.0 12.4 14.8) against the grip (the grip-frame
// measure of 2026-10-22) and comes off along its own axis, up and a little back ((-0.41 0.02 0.91) - AmmoDummy
// against AmmoBoxBone). The palm's path is that line, from the box's top to 34 units out; the clip time is 0 all
// along. No mesh of its own: the spent box goes as the pistol's cylinder does (carry 2), no Havok copy.
inline constexpr Pt kLauncherBoxOut[] = {
    {0.0f, {24.4f, 12.5f, 19.0f}}, {0.0f, {20.3f, 12.7f, 28.1f}}, {0.0f, {16.2f, 12.9f, 37.2f}}, {0.0f, {12.1f, 13.1f, 46.3f}}};
inline constexpr Pt kLauncherBoxIn[] = {
    {0.0f, {12.1f, 13.1f, 46.3f}}, {0.0f, {16.2f, 12.9f, 37.2f}}, {0.0f, {20.3f, 12.7f, 28.1f}}, {0.0f, {25.2f, 12.5f, 17.2f}}};
inline constexpr Phase kLauncher[] = {
    {Kind::Pull, 0.0f, 0.0f, kLauncherBoxOut, 4, "AmmoBoxBone+AmmoDummy", "pull the box off", true},
    {Kind::Carry, 0.0f, 0.0f, nullptr, 0, "AmmoBoxBone+AmmoDummy", "drop the spent box", true},
    {Kind::Fetch, 0.0f, 0.0f, nullptr, 0, "AmmoBoxBone+AmmoDummy", "take a box from the pouch", true},
    {Kind::Insert, 0.0f, 0.0f, kLauncherBoxIn, 4, "AmmoBoxBone+AmmoDummy", "seat the box", true},
    {Kind::Finish, 3.90f, 3.95f, nullptr, 0, nullptr, "done"}};

// Crossbow ('Reload', 3.00 s) - 2026-10-22 late (MANUAL_RELOAD_PLAN.md step 4, the tester's design): the lever
// up, the bolt box from the waist, in, the lever down. The lever (X_Lever, 10 -> 72 deg over 0.45-0.75 s of the
// clip, 75 -> 0 over 2.25-2.55) is pulled along the clip's own hand path, the clip following the hand - the
// Tommy's drum pull, on a lever; the box (X_AmmoBox, 120 units of flip in the clip, never played) is the gun's
// own bone carried at the palm, as the launcher's, off the gun and back on with the clip held at 1.95 s (the
// lever up, the box on in the clip). Palm paths against the grip from reload_pose.py (2026-10-22).
// 2026-10-23 (the tester: the lever's "grab point is wrong"): each lever path's first point is the lever's TIP where
// it sits while the clip waits there, measured from the 21:21 recording (X_LeverNUB in the grip's frame), not the
// clip palm's first sample - those were 6 units under the tip for the pulls up and 11-14 off for the pulls down.
inline constexpr Pt kCrossbowLeverUp[] = {
    {0.45f, {-23.7f, -12.8f, 34.4f}}, {0.65f, {0.4f, -4.6f, 49.9f}}, {0.75f, {4.2f, -3.6f, 50.3f}}};
inline constexpr Pt kCrossbowBoxOut[] = {
    {1.95f, {4.3f, -4.7f, 30.0f}}, {1.95f, {0.0f, -5.0f, 42.0f}}, {1.95f, {-6.0f, -6.0f, 54.0f}}};
inline constexpr Pt kCrossbowBoxIn[] = {
    {1.95f, {-6.0f, -6.0f, 54.0f}}, {1.95f, {0.0f, -5.0f, 42.0f}}, {1.95f, {4.3f, -4.7f, 31.0f}}};
inline constexpr Pt kCrossbowLeverDown[] = {
    {2.25f, {11.2f, 4.6f, 56.5f}}, {2.35f, {15.6f, 4.4f, 48.0f}}, {2.45f, {-12.8f, -6.4f, 31.5f}}, {2.55f, {-10.9f, -7.7f, 27.9f}}};
// 2026-10-23 (the tester: the lever's "spot is hard to grab"; the 07:27 run logged lever grabs at reach 0.72-0.99, a
// hair inside the 12 cm radius): every lever pull is grabbed from 21 cm (grabScale 1.75), from either side of the stock.
inline constexpr Phase kCrossbow[] = {
    {Kind::Pull, 0.45f, 0.75f, kCrossbowLeverUp, 3, "X_Lever", "cock the lever up", false, true, 1.75f, -1.0f, 0.0f, -1.0f, false, true},
    {Kind::Pull, 1.95f, 1.95f, kCrossbowBoxOut, 3, "X_AmmoBox", "pull the bolt box off", true},
    {Kind::Carry, 1.95f, 1.95f, nullptr, 0, "X_AmmoBox", "drop the spent box", true},
    {Kind::Fetch, 1.95f, 1.95f, nullptr, 0, "X_AmmoBox", "take a box from the pouch", true},
    {Kind::Insert, 1.95f, 1.95f, kCrossbowBoxIn, 3, "X_AmmoBox", "seat the box", true},
    {Kind::Pull, 2.25f, 2.55f, kCrossbowLeverDown, 4, "X_Lever", "cock the lever down", false, true, 1.75f, -1.0f, 0.0f, -1.0f, false, true},
    {Kind::Finish, 2.90f, 3.00f, nullptr, 0, nullptr, "done"}};
// The crossbow's cock between shots ('Fire', 2.00 s; 2026-10-22 late night - the tester: "just supposed to be lever
// up then lever down in between shots", and a grip at the lever started a whole reload): the Fire clip re-cocks the
// crossbow itself, X_Lever up over 0.85-1.0 s and down 1.3-1.4; adopted as its own two-pull program the moment
// the clip begins, so the clip waits at 0.85 for the hand on the lever (the trigger dead until it is worked, as the
// shotgun's pump), the lever is pulled up and down along the clip's own hand path with the hand locked on it, and
// the clip runs out. Palm paths from reload_pose.py on Crossbow__FireCrossbow / Default__Fire.
inline constexpr Pt kCrossbowCockUp[] = {
    {0.85f, {-19.2f, -7.6f, 37.7f}}, {0.90f, {-4.8f, -7.3f, 48.5f}}, {0.95f, {6.0f, -2.6f, 50.9f}}, {1.00f, {7.0f, -2.4f, 51.0f}}};
inline constexpr Pt kCrossbowCockDown[] = {
    {1.30f, {0.6f, 1.8f, 54.6f}}, {1.35f, {-6.7f, -3.1f, 39.1f}}, {1.40f, {-14.0f, -7.4f, 28.6f}}};
inline constexpr Phase kCrossbowCock[] = {
    {Kind::Pull, 0.85f, 1.00f, kCrossbowCockUp, 4, "X_Lever", "cock the lever up", false, true, 1.75f, -1.0f, 0.0f, -1.0f, false, true},
    {Kind::Pull, 1.30f, 1.40f, kCrossbowCockDown, 3, "X_Lever", "cock the lever down", false, true, 1.75f, -1.0f, 0.0f, -1.0f, false, true},
    {Kind::Finish, 1.45f, 2.00f, nullptr, 0, nullptr, "cocked"}};

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
    // Held at 0.80 s while carrying: the clip's hand holds the new cylinder between thumb and fingers on
    // its way to the frame (reload_pose.py with the gun on the game's socket frame, 2026-10-20 evening).
    // The earlier 0.53 came from renders with the gun at a wrong angle: there the cylinder is still in
    // the gun and the hand only beside it - the 19:57 run's "still holding it wrong".
    {Kind::Fetch, 0.756f, 0.756f, nullptr, 0, "DrumParent+drum", "take a cylinder from the pouch", false, false, 1.0f, 0.80f},
    {Kind::Insert, 0.756f, 0.934f, kPistolCylIn, 3, "DrumParent+drum", "seat the cylinder", false, false, 1.0f, 0.80f},
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
    // The pump back 0.409-0.487, forward 0.532-0.676 - worked by the two-handed grip like every other
    // pump (the 20:35 run: "the cocking after reloading is not the same as the normal grab cocking and I
    // have to regrip it"). Its strokes are the glue's Pump over this clip (kEndPump in manual_reload.cpp).
    {Kind::PumpHand, 0.409f, 0.676f, nullptr, 0, nullptr, "pumped"},
    {Kind::Finish, 0.90f, 0.95f, nullptr, 0, nullptr, "done"}};

inline constexpr Program kPrograms[] = {
    // Eject: the Tommy Gun's drum sits seated as the clip's hand reaches it (0.267, the pull's start);
    // the pistol is folded open with its cylinder still in at 0.466 (the cylinder's pull begins).
    {L"MachineGun", L"Reload", 3.33f, kTommy, 7, false, 0.267f, "TG_AmmoClip", true},
    {L"Pistol", L"FastReload", 1.80f, kPistol, 7, false, 0.466f, "DrumParent+drum"},
    {L"Pistol", L"Reload", 1.80f, kPistol, 7, false, 0.466f, "DrumParent+drum"}, // no fast-reload upgrade: scaled
    {L"Shotgun", L"SingleFrame", 0.033f, kShotgunStart, 1, true},
    {L"Shotgun", L"Reload_Loop", 0.49f, kShotgunLoop, 4, true},
    {L"Shotgun", L"Reload", 0.95f, kShotgunEnd, 2, true},
    // 2026-10-22: the launcher's box sits on the closed gun at 0 (the clip is never played past it until the
    // finish); the box is the gun's own bones, no spare prop. Last: tests index this table.
    {L"GrenadeLauncher", L"Reload", 4.00f, kLauncher, 5, false, 0.0f, "AmmoBoxBone+AmmoDummy"},
    // 2026-10-22 late: the crossbow (no eject on X: the box comes off by hand after the lever).
    // 2026-10-23 (the tester: X should drop the empty box "like the tommy mag"): ejects at 1.95 s, the lever up and the
    // box on in the clip - the box falls, the reload waits at the pouch.
    {L"Crossbow", L"Reload", 3.00f, kCrossbow, 7, false, 1.95f, "X_AmmoBox"},
    // The crossbow's Fire clip is a program too (the lever between shots); after the Reload, which is the entry.
    {L"Crossbow", L"Fire", 2.00f, kCrossbowCock, 3, false},
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
    float seatCm = 10.0f;       // the latch: a carried part this near where it sits snaps home
    float slapCm = 20.0f;       // let go this near where it sits (a slap into the well): it is seated
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
    float partSeatCm = -1;      // the carried part's distance from where it sits in the gun (cm), -1
                                // unknown (no eject recorded the seat): the palm's path decides
    bool chambered = false;     // the gun still had a round in it when this reload began
    bool magOut = false;        // 2026-10-21: a reload left this gun with no magazine (switched away mid-reload):
                                // the reload it begins on coming back starts at its first Fetch, nothing to drop
    bool ejectNow = false;      // the reload button while a reload waits at its first pull (a dry gun's):
                                // the magazine drops, as the button's own eject
    bool pumpDone = false;      // a PumpHand phase's pump completed (the glue's Pump)
    // 2026-10-20, the spare magazine (Program::spare): one may be taken from the pouch now (the gun has
    // rounds to reload with), one is in the free hand (the glue's prop), and a thrown one reached the open
    // well this frame.
    bool spareOk = false;
    bool spareInHand = false;
    bool caught = false;
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
    // For the grip arbiter (grip_owner.h): the free hand's distance to what the reload wants next, over the
    // grab radius (<= 1 in reach) - idle, the part that starts a reload; running, the part the machine waits
    // for (a pull, the bolt) or the pouch zone (0.5) at a fetch; -1 while it wants nothing of the hand (a part
    // in the hand, the drop, the seating, the clip playing, the last pump). 2026-10-21: it was 0, an absolute
    // claim, whenever the machine claimed - the two-handed grip could never win inside the drum's reach, and
    // the drop and the seating swallowed every squeeze (HELD_GUN_MATRIX.md G1, G7).
    float reach = -1;
    float spareReach = -1;      // the spare magazine's own offer: the pouch zone (0.25) while one may be taken
    bool buzz = false;          // the hand is within reach of what it should take next
    Haptic haptic = Haptic::None;
    const char* event = nullptr;// a transition worth a log line
    bool active = false;        // a manual reload is in progress
    bool ejected = false;       // this frame the spent part starts to fall (`part` names it)
    bool handLock = false;      // the free hand is drawn where the clip's own hand is (on the bolt)
    bool chase = false;         // `t` follows the hand forward: PLAY the clip there (its sounds fire)
    bool fetched = false;       // this frame a new part was taken from the pouch (`part` names it)
    bool pumpWait = false;      // a PumpHand phase: the glue's Pump works the clip, the grip is the
                                // two-handed grip's
    bool seating = false;       // this frame the clip jumped from the carry hold onto the insert: the
                                // magazine notifies in between were skipped (the glue shows it)
    bool dropped = false;       // this frame a carried part left the hand away from the gun (the spent one
                                // pulled out, or a new one let go short of its seat): it falls or is
                                // thrown (`part` names it)
    bool spareTake = false;     // this frame a spare magazine is taken from the pouch into the free hand
    bool spareLetGo = false;    // this frame the spare in the free hand is let go: it is thrown, or falls
    bool spareUsed = false;     // this frame the spare in hand became the gun's own part (carry 1): the prop goes
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
    // The gun's magazine is out and no new one is in the hand yet: falling, carried away, or the hand at the pouch.
    bool mag_out() const {
        const int ff = first_fetch(prog_);
        if (state_ != State::Running || ff < 0) return false;
        return dropping_ || (phase_ > 0 && phase_ < ff) || (phase_ == ff && !carrying_);
    }
    // A reload waiting for the free hand, which has nothing of it: at a Pull or Slide not yet taken hold of, or at a
    // Fetch with nothing carried. The hand may grip the gun two-handed or take the gun over meanwhile.
    bool hand_free() const {
        if (state_ != State::Running || carrying_ || dropping_ || !prog_ || phase_ >= prog_->n) return false;
        switch (prog_->phases[phase_].kind) {
            case Kind::Pull:
            case Kind::Slide: return sub_ == Sub::Wait;
            case Kind::Fetch: return true;
            default: return false;
        }
    }

    Output update(const Input& in, const Config& c) {
        Output o;
        const float grabU = c.grabCm * in.unitsPerCm;
        grabU_ = grabU;
        switch (state_) {
            case State::Idle:
                if (in.adopt && in.clipLive && in.clipProgram) {
                    begin(in.clipProgram, in);
                    if (in.magOut && first_fetch(prog_) >= 0) {
                        phase_ = first_fetch(prog_);
                        enter(in);
                        running(in, c, grabU, o, /*fresh=*/true);
                        if (!o.event) o.event = "no magazine in the gun: take one from the pouch";
                        break;
                    }
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
        spare(in, o);
        o.active = state_ != State::Idle;
        if (o.part && prog_ && state_ == State::Running) o.atPalm = prog_->phases[phase_].atPalm;
        if (skipped_ && !o.event) {
            skipped_ = false;
            o.event = "a round is still in the gun: no bolt to rack";
        }
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
    bool lying_ = false;        // and lies where it fell through that Fetch
    bool snap_ = false;         // an Insert's part reached its latch: the clip seats it
    bool skipped_ = false;      // a phase was skipped (Phase::skipLoaded): say so once
    // 2026-10-20 (19:57 run, "the mag teleports to the floor and shoots up into the gun"): a carried part
    // holds the clip BEHIND its insert (Tommy 0.30, pistol 0.53 - where the clip's hand grips it), and the
    // first insert time after that was reached by PLAYING the clip there - through its own toss of the
    // drum to the waist and back. That first step is SET (`seating`: the glue shows the magazine, whose
    // hide/show notifies the jump skips); the clip plays only from there on.
    bool jumpNext_ = false;
    void from_hold(Output& o) {
        o.chase = !jumpNext_;
        if (jumpNext_) o.seating = true;
        jumpNext_ = false;
    }
    Vec grab_{};                // a Slide: where the hand took hold
    // A Slide's direction: its path's, start to end (the sideways axis out for a two-sided part).
    static Vec slide_axis(const Phase& p) {
        if (!p.path || p.n < 2) return Vec{-1, 0, 0};
        Vec d = p.path[p.n - 1].p - p.path[0].p;
        if (p.bothSides) d.y = 0;
        const float l = arm_ik::length(d);
        return l > 1e-6f ? d * (1.0f / l) : Vec{-1, 0, 0};
    }
    float dropAt_ = 0;

    // The spare magazine: free to take one when the hand has nothing else to do here - no reload, or one
    // waiting at the gun's own part (its first pull, the bolt) or playing on its own. Never at a Fetch (the
    // pouch gives the gun's own part there) or while a part rides the hand.
    bool spare_free() const {
        if (state_ == State::Idle) return true;
        if (state_ != State::Running || dropping_ || carrying_ || !prog_) return false;
        switch (ph().kind) {
            case Kind::Pull:
            case Kind::Slide: return sub_ == Sub::Wait;
            case Kind::Play:
            case Kind::Finish: return true;
            default: return false;
        }
    }
    void spare(const Input& in, Output& o) {
        if (in.spareInHand) {
            o.claim = true; // the grip holds the spare: no bumper reaches the game
            if (state_ == State::Idle) o.reach = 0.0f;
            if (!in.grip && !o.spareUsed) {
                o.spareLetGo = true;
                if (o.haptic == Haptic::None) o.haptic = Haptic::Drop;
                if (!o.event) o.event = "the spare is let go: thrown";
            }
            return;
        }
        if (!in.spareOk || !in.inPouch || !spare_free()) return;
        o.buzz = true; // the pouch has one (22:55 run: "the rumble doesn't show up")
        // A zone, not a point: well within reach - and NEARER than the gun's own drum, whose reach is a real
        // distance (00:18-00:20 run: the drum 1.7 radii off while the hand was at the pouch, the squeeze went to
        // the game and no spare came out, ever). Its own offer to the arbiter, beside the reload's.
        o.spareReach = 0.25f;
        if (state_ == State::Idle) o.claim = true;
        if (in.gripRise) {
            o.spareTake = true;
            o.claim = true;
            if (o.haptic == Haptic::None) o.haptic = Haptic::Grab;
            if (!o.event) o.event = "a spare from the pouch";
        }
    }

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
    // A phase's own reach and path, for a part with handles on both sides (Phase::bothSides): the
    // sideways axis (y, the grip frame's) does not count, and the reach is grabScale times further.
    // The hand's distance to the part over the phase's reach (<= 1 near); a large number when off the gun's side.
    static float phase_reach(const Phase& p, Vec h, float r) {
        if (!p.path || p.n <= 0 || !(r > 0.0f)) return 1e9f;
        Vec a = p.path[0].p;
        if (p.bothSides) {
            // Either side of the gun, but at the gun: sideways no farther out than the handle's own side
            // plus the reach (19:31 run: with sideways ignored, the buzz reached the other shoulder).
            if (std::fabs(h.y) > std::fabs(a.y) + r) return 1e9f;
            a.y = h.y = 0;
        }
        return arm_ik::length(h - a) / (r * p.grabScale);
    }
    static bool near_phase(const Phase& p, Vec h, float r) { return phase_reach(p, h, r) <= 1.0f; }
    static Projection project_phase(const Phase& p, Vec h) {
        if (!p.bothSides) return project(p.path, p.n, h);
        Pt flat[8];
        const int n = p.n < 8 ? p.n : 8;
        for (int i = 0; i < n; ++i) {
            flat[i] = p.path[i];
            flat[i].p.y = 0;
        }
        h.y = 0;
        return project(flat, n, h);
    }

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
        lying_ = false;
        snap_ = false;
        state_ = State::Running;
        scale_ = (p->clipLen > 1e-4f && in.clipLen > 1e-4f) ? in.clipLen / p->clipLen : 1.0f;
        enter(in);
    }

    void enter(const Input& in) {
        if (ph().skipLoaded && in.chambered && phase_ + 1 < prog_->n) {
            skipped_ = true; // a round still in: no bolt to rack
            ++phase_;
            enter(in);
            return;
        }
        sub_ = Sub::Wait;
        lastProgress_ = 0;
        lastT_ = ph().t0;
        playT_ = T(ph().t0);
        lastNow_ = in.now;
        // A pull the hand is already holding at its start - the reload's own first pull, or the next
        // stroke of the same part (the pump back, then forward) - is already being dragged.
        // 2026-10-23 (the tester: the lever down once had "no grab point"; the 21:25 run: the crossbow's lever-down
        // pull completed 9 ms after the box was seated): only the reload's first pull or the next stroke of the SAME
        // part carries the held grip over - the hand still squeezing a seated box is not holding the lever.
        const bool samePartAsBefore = phase_ > 0 && prog_->phases[phase_ - 1].kind == Kind::Pull &&
                                      ph().part && prog_->phases[phase_ - 1].part &&
                                      !strcmp(ph().part, prog_->phases[phase_ - 1].part);
        if (ph().kind == Kind::Pull && in.grip && (phase_ == 0 || samePartAsBefore) && near_phase(ph(), in.hand, grabU_))
            sub_ = Sub::Drag;
        // The next stroke of a slide the hand is still holding (the pump back, then forward) goes on
        // from where the hand is.
        if (ph().kind == Kind::Slide && in.grip && phase_ > 0 && prog_->phases[phase_ - 1].kind == Kind::Slide) {
            sub_ = Sub::Drag;
            grab_ = in.hand;
        }
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
    // The reload is over (its clip gone): idle, with NO program left behind. 2026-10-21 (the 23:55 sim run): the
    // last program and phase used to stay, so the glue's "a reload begins now" test (program() null before) was
    // true only once per weapon - the rounds at a reload's start were not read again, and the state record named
    // the old phase while the next reload was only starting.
    void ended(Output& o) {
        state_ = State::Idle;
        carrying_ = false;
        prog_ = nullptr;
        phase_ = 0;
        o.event = "the reload ended";
    }

    void running(const Input& in, const Config& c, float grabU, Output& o, bool fresh = false) {
        // The clip changed under us (the shotgun's next loop or its end): its own program.
        if (in.clipNew && !fresh) {
            if (in.clipLive && in.clipProgram) {
                begin(in.clipProgram, in);
                o.event = "next clip";
            } else if (!in.clipLive) {
                ended(o);
                return;
            }
        }
        if (!in.clipLive) {
            ended(o);
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
            o.carry = 3;
            // A spare already in the hand: the spent one has fallen clear once the latch is read (0.06 s) -
            // the new one goes in at once ("pre-pull, drop the current one, slide in the new one quickly").
            if (o.fallS >= c.dropS || (in.spareInHand && o.fallS >= 0.2f)) {
                dropping_ = false;
                lying_ = true; // the spent part lies where it fell until a new one is taken
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
                const Projection pr = project_phase(p, in.hand);
                // 20:35 run: a dry Tommy Gun's reload waited here for the hand to pull the drum, and the
                // reload button did nothing ("locked up"). The button drops the magazine, as its eject.
                // 2026-10-23: and while it waits at the pull of the ejected part itself (the crossbow's box, after
                // its lever is up) - the same drop.
                const bool atEjectPart = p.part && prog_->ejectPart && !strcmp(p.part, prog_->ejectPart);
                if (sub_ == Sub::Wait && (phase_ == 0 || atEjectPart) && in.ejectNow && start_drop(in, o)) {
                    o.event = "the reload button: the magazine drops - take a new one from the pouch";
                    o.claim = true;
                    o.hold = true;
                    o.t = T(prog_->ejectT);
                    break;
                }
                if (sub_ == Sub::Wait) {
                    const float reach = phase_reach(p, in.hand, grabU);
                    const bool near = reach <= 1.0f;
                    o.buzz = near;
                    // 2026-10-21 ("it doesn't let you swap hands on an empty gun"; "2 hand grip on empty weapons"):
                    // a reload waiting for the hand claims its grip only at the part - elsewhere the squeeze is the
                    // hand-over's or the two-handed grip's (hand_free()). Its reach is a distance, for the arbiter.
                    o.claim = near;
                    o.reach = reach < 1e8f ? reach : -1.0f;
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
                o.chase = true; // played forward: its sounds fire
                o.handLock = p.handOnPart; // a handle: the hand drawn on it, in the clip's own grip
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
            case Kind::Slide: {
                o.hold = true;
                if (sub_ == Sub::Wait) {
                    const float reach = phase_reach(p, in.hand, grabU);
                    const bool near = reach <= 1.0f;
                    o.buzz = near;
                    o.claim = near; // as the Pull's: only at the part
                    o.reach = reach < 1e8f ? reach : -1.0f;
                    o.t = T(p.t0);
                    if (near && in.gripRise) {
                        sub_ = Sub::Drag;
                        grab_ = in.hand;
                        o.haptic = Haptic::Grab;
                    }
                    break;
                }
                // Held: the part is where the hand has moved it along the stroke's own direction (its
                // path's, the sideways axis left out for a two-sided part), both ways.
                const float moved = arm_ik::dot(in.hand - grab_, slide_axis(p));
                float prog = p.travel > 1e-3f ? moved / p.travel : 1.0f;
                prog = prog < 0 ? 0 : prog > 1 ? 1 : prog;
                if (prog - lastProgress_ > 0.25f) o.haptic = Haptic::Tick;
                lastProgress_ = prog;
                o.t = T(p.t0 + (p.t1 - p.t0) * prog);
                o.handLock = true;
                o.chase = true; // played forward: its sounds fire
                const bool caught = p.strokeCm > 0.0f && moved >= p.strokeCm * in.unitsPerCm;
                if (prog >= 0.95f || caught) {
                    o.haptic = Haptic::Seat;
                    o.event = p.say;
                    o.t = T(p.t1);
                    next(in);
                } else if (!in.grip) {
                    sub_ = Sub::Wait; // let go short: it springs home
                    lastProgress_ = 0;
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
                    o.dropped = true; // the spent part leaves the hand: thrown, or dropped
                    o.event = p.say;
                    next(in);
                }
                break;
            }
            case Kind::Fetch: {
                o.hold = true;
                o.t = T(hold_t(p));
                o.part = p.part;
                const bool toInsert = phase_ + 1 < prog_->n && prog_->phases[phase_ + 1].kind == Kind::Insert;
                // The spare already in the hand IS the new part: it rides the hand from this frame (as the
                // Insert carries it), and the glue's prop goes.
                if (in.spareInHand && in.grip && toInsert) {
                    lying_ = false;
                    carrying_ = true;
                    o.fetched = true;
                    o.spareUsed = true;
                    o.event = "the spare in hand goes to the gun";
                    next(in);
                    const Phase& ins = ph();
                    o.carry = 1;
                    o.t = T(hold_t(ins));
                    o.carryFrom = ins.path[0].p;
                    jumpNext_ = hold_t(ins) < ins.t0;
                    break;
                }
                // A thrown spare reached the open well: it is seated, as a slap is (the clip plays it home).
                if (in.caught && toInsert) {
                    lying_ = false;
                    carrying_ = false;
                    next(in);
                    const Phase& ins = ph();
                    snap_ = true;
                    playT_ = T(ins.n > 1 ? ins.path[ins.n - 2].t : ins.t0);
                    jumpNext_ = hold_t(ins) < ins.t0;
                    o.haptic = Haptic::Seat;
                    o.event = "a thrown spare caught in the well: it is seated";
                    o.carry = 0;
                    o.part = ins.part;
                    o.t = playT_;
                    from_hold(o);
                    break;
                }
                o.carry = 2;
                if (lying_) {
                    o.carry = 3;
                    o.fallS = in.now - dropAt_;
                }
                o.buzz = in.inPouch;
                // 2026-10-21: a Fetch claims the free grip only at the pouch - away from it the squeeze is free for the
                // two-handed grip ("you can always grip it") and the hand-over (an empty gun changes hands).
                if (!in.inPouch && !in.spareInHand) o.claim = false;
                o.reach = in.inPouch ? 0.5f : -1.0f; // the pouch zone, as the shotgun's start offers it
                if (in.inPouch && in.gripRise) {
                    lying_ = false;
                    carrying_ = true;
                    o.haptic = Haptic::Grab;
                    o.event = p.say;
                    o.fetched = true;
                    next(in);
                }
                break;
            }
            case Kind::Insert: {
                const Projection pr = project(p.path, p.n, in.hand);
                o.part = p.part;
                o.hold = true;
                // 2026-10-20 THE LATCH: the carried PART brought within reach of where it sits in the
                // gun (in.partSeatCm, measured on the part, so the hand's turn does not matter) snaps
                // home - the clip plays its last stretch on its own.
                if (!snap_ && in.partSeatCm >= 0 && in.partSeatCm <= c.seatCm) {
                    snap_ = true;
                    playT_ = T(p.n > 1 ? p.path[p.n - 2].t : p.t0); // the insert's own last stretch
                    o.haptic = Haptic::Tick;
                }
                if (snap_) {
                    playT_ += dt;
                    o.carry = 0; // the clip has it again, sliding it in
                    o.t = playT_ < T(p.t1) ? playT_ : T(p.t1);
                    from_hold(o);
                    if (playT_ >= T(p.t1)) {
                        snap_ = false;
                        o.haptic = Haptic::Seat;
                        o.event = p.say;
                        carrying_ = false;
                        next(in);
                    }
                    break;
                }
                // Let go before it latched: the part falls; take another. 2026-10-20 (Tommy Gun, once: a drum
                // being inserted fell out and the gun still fired with no drum): a GUIDED part let go short of
                // half way used to stay guided with no hand on it - drawn where the clip had it at the guide's
                // time (the clip's own new drum, tossed in the air below the gun) and still pushed home by a
                // hand passing along the path. Now the grip decides alone: past half way it is seated (below),
                // short of it it falls, guided or not.
                // 19:31 run: a drum "slapped" into the well was let go guided short of half way - it fell,
                // looking as if it bounced off the gun. A part let go guided, or within slapCm of where it
                // sits, is seated (the clip plays it home); only one let go away from the gun falls.
                // 19:57 run: the shotgun's second shell let go 12 cm from the port fell, again and again (a
                // shell has no recorded seat - no eject - so only its path can say it is near): within
                // three quarters of slapCm of where the insert's path ends (the port), it is pressed in too.
                // (The path's END: an insert path can begin far from the gun - the Tommy Gun's at the waist.)
                const bool nearPath = in.partSeatCm < 0 && p.n > 0 &&
                                      arm_ik::length(in.hand - p.path[p.n - 1].p) <= 0.75f * c.slapCm * in.unitsPerCm;
                if (!in.grip && (sub_ == Sub::Guided || nearPath || (in.partSeatCm >= 0 && in.partSeatCm <= c.slapCm))) {
                    snap_ = true;
                    playT_ = T(p.n > 1 ? p.path[p.n - 2].t : p.t0);
                    if (sub_ == Sub::Guided && T(lastT_) > playT_) playT_ = T(lastT_);
                    o.haptic = Haptic::Tick;
                    o.event = "let go at the well: it is seated";
                    o.carry = 0;
                    o.t = playT_;
                    from_hold(o);
                    break;
                }
                if (!in.grip) {
                    o.carry = 2;
                    o.t = T(hold_t(p));
                    o.haptic = Haptic::Drop;
                    o.dropped = true;
                    o.event = "let go short of the seat: it falls - take another from the pouch";
                    carrying_ = false;
                    sub_ = Sub::Wait;
                    lastProgress_ = 0;
                    lastT_ = p.t0;
                    if (phase_ > 0 && prog_->phases[phase_ - 1].kind == Kind::Fetch) {
                        --phase_;
                        enter(in);
                    }
                    break;
                }
                if (sub_ != Sub::Guided) {
                    o.carry = 1;
                    o.t = T(hold_t(p));
                    o.carryFrom = p.path[0].p;
                    jumpNext_ = hold_t(p) < p.t0; // the clip waits behind the insert (the carry pose)
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
                    o.t = T(hold_t(p));
                    o.carryFrom = p.path[0].p;
                    jumpNext_ = hold_t(p) < p.t0;
                    break;
                }
                const float prog = pr.progress > lastProgress_ ? pr.progress : lastProgress_;
                lastProgress_ = prog;
                if (pr.t > lastT_) lastT_ = pr.t;
                o.t = T(lastT_);
                from_hold(o);
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
                o.chase = true; // played forward: its sounds fire
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
                o.chase = true; // played forward: its sounds fire
                if (playT_ >= T(p.t1)) next(in);
                break;
            }
            case Kind::PumpHand: {
                // The glue's Pump holds and plays the clip here; the free hand's grip is the two-handed
                // grip's, so the pump is grabbed and worked the same way as between shots.
                o.pumpWait = true;
                o.claim = false;
                if (in.pumpDone) {
                    o.haptic = Haptic::Seat;
                    o.event = p.say;
                    next(in);
                }
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

// ---- the shotgun's pump, by hand (2026-10-20, redesigned the same evening) ------------------------
// Every shot's 'Fire' clip pumps the gun itself: SG_Pump runs 18 units back 0.75-0.85 s, waits, and
// forward 1.05-1.15 s (tools\blender\reload_pose.py). Held two-handed - the front hand on the fore-end,
// which IS the pump - the clip WAITS at the pump for the hand to work it, and the next shot is ready
// only once it has (the game fires again when the clip ends). Not two-handed (or let go mid-way): the
// clip runs on and pumps itself, as before.
//
// THE STROKE, AFTER HALF-LIFE 2 VR (docs/bioshock1/ENGINE_NOTES.md, "Half-Life 2 VR's shotgun pump",
// concepts only). Its pump is the gun's fore-grip itself, and a stroke is a RATCHET: it counts from
// the farthest point the pump has reached the other way since the stroke began, and completes once the
// hand is a short distance (its default is 2 inches) past it - not a fixed path from where the hand
// happened to be when the clip got there. Ours: the two controllers coming 6 cm closer than the
// farthest apart they have been since the stroke began completes the back stroke; then 6 cm apart
// again from the closest. Inside a stroke the clip follows the hands (over 9 cm; the drawn hand rides
// the pump); at the catch the clip plays the rest of the stroke itself. A stroke undone
// (the hand going back the other way) is never lost: the ratchet's reference only ever moves with it.
// And as there, firing is GATED, not queued: a trigger pulled while the pump waits clicks (a buzz on
// the gun hand) and does nothing else.
// 2026-10-20 (19:33 run, "it is cocking the shots for itself"): THE STROKE IS MEASURED BETWEEN THE TWO
// CONTROLLERS, as Half-Life 2 VR measures its pump from a point on the gun. The first version read the
// front hand in the frame of the DRAWN gun, which the Fire clip itself moves (the recoil, the pump's own
// motion): the clip moved the gun, the gun moved the measured hand, the hand moved the clip - every
// pump completed itself 0.2 s after the shot. The controllers' distance knows nothing of the clip.
struct PumpConfig {
    float backT0 = 0.75f, backT1 = 0.85f, fwdT0 = 1.05f, fwdT1 = 1.15f;
    float strokeCm = 6.0f;  // controller travel past the ratchet's reference that completes a stroke
    float followCm = 9.0f;  // controller travel over which the clip follows a stroke (it finishes at the catch)
    float earlyCm = 2.5f;   // during the recoil: this much closer and the stroke has begun
    bool waitForHands = false; // the reload's last pump: not two-handed, it waits (never pumps itself)
    // 2026-10-21 (12:17 run: the reload's last pump "cocked itself" - the hand coming back from the pouch was
    // taken hold of inside the grab radius and its last 6 cm of approach, then its settling, were the two
    // strokes: grab to pumped in 0.33 s): a fresh hold counts no stroke for this long.
    float settleS = 0.30f;
    // 2026-10-21 (the tester: "it always plays the cocking sound after shooting even if you haven't cocked yet"):
    // the clip waits HERE, before the pump and its sound (< 0: at backT0), and plays on through them only once a
    // stroke has begun - played, not set, so the sound fires with the player's pump and never without it.
    float waitT = -1.0f;
};
// 2026-10-22: a stroke the gun does not need (gun_state::PumpNeed::Optional - a pump while the reload waits for a
// shell) takes a deliberate stroke, twice the quick one, so the aim's own drift while holding two-handed never
// works it (the 09:31 run: 3 cm of drift began one, 6 completed it); the clip follows the whole of it.
inline constexpr float kDeliberateStrokeCm = 12.0f;
inline PumpConfig deliberate(PumpConfig c) {
    c.strokeCm = kDeliberateStrokeCm;
    c.followCm = kDeliberateStrokeCm;
    return c;
}
// The pump's position, 0 forward .. 1 at the rear stop, at clip time t under this config (the back stroke over
// backT0..backT1, the forward one over fwdT0..fwdT1).
inline float pump_position(const PumpConfig& c, float t) {
    float back = 0.0f;
    if (t >= c.fwdT1) back = 0.0f;
    else if (t >= c.fwdT0) back = 1.0f - (t - c.fwdT0) / (c.fwdT1 - c.fwdT0);
    else if (t >= c.backT1) back = 1.0f;
    else if (t > c.backT0) back = (t - c.backT0) / (c.backT1 - c.backT0);
    return back < 0 ? 0 : back > 1 ? 1 : back;
}
struct PumpInput {
    bool fireClip = false; // the shotgun's 'Fire' clip is playing
    bool clipNew = false;  // a new 'Fire' clip (the next shot) began this frame
    float clipT = 0;       // its time (seconds)
    bool held = false;     // two hands on the gun
    // 2026-10-21 (step 5, the audit's bug 4): the front hand's position ALONG THE BARREL from the gun hand, cm,
    // forward positive (along_barrel_cm) - the pump back = smaller. It was the bare distance between the two
    // controllers, which the gun hand's own motion and any sideways motion fed as strokes (the 21:14 run: a back
    // stroke in 43 ms, then the forward stroke stuck at 1 cm while the hands closed 10 cm).
    float alongCm = 0;
    bool alongOk = false;   // both controllers tracked
    bool triggerRise = false; // the trigger was pulled this frame
    float heldS = 1e9f;    // seconds since the two-handed hold began (a fresh grab settles first)
};
// The front hand's position along the barrel from the gun hand, in cm: the free controller against the gun
// controller, projected on the barrel's direction (unit, the gun controller's aim forward). Metres in, cm out.
inline float along_barrel_cm(const float freeM[3], const float gunM[3], const float dir[3]) {
    const float d[3] = {freeM[0] - gunM[0], freeM[1] - gunM[1], freeM[2] - gunM[2]};
    return (d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2]) * 100.0f;
}

struct PumpOutput {
    bool hold = false;     // hold the clip at t
    bool run = false;      // let it run (with jump: from t)
    bool jump = false;
    float t = 0;
    bool chase = false;    // played forward to t (its sounds fire)
    Haptic haptic = Haptic::None;
    bool deny = false;     // the trigger was pulled while the pump waits: click (the gun hand)
    float strokeCm = -1;   // the stroke so far, cm past the ratchet's reference (-1 not pumping)
    const char* event = nullptr;
};
class Pump {
public:
    PumpOutput update(const PumpInput& in, const PumpConfig& c) {
        PumpOutput o;
        if (!in.fireClip) {
            stage_ = Stage::Off;
            return o;
        }
        // Each shot's clip is its own pump: the next shot's clip can begin without a frame in between
        // (the 15:37 run: a pump left Done never came back for the shot after it).
        if (stage_ == Stage::Off || in.clipNew) stage_ = Stage::Firing;
        if (stage_ == Stage::Done) return o;
        const float waitT = c.waitT >= 0.0f ? c.waitT : c.backT0;
        if (!in.held && c.waitForHands) {
            // It waits for the hands at the stroke it is on - before the pump if none has begun.
            if (stage_ == Stage::Firing && in.clipT < waitT) return o;
            o.hold = true;
            o.t = stage_ == Stage::Fwd ? fwdT_ : stage_ == Stage::Back ? c.backT0 : waitT;
            o.deny = in.triggerRise;
            rebase_ = true; // a new grab starts the ratchet afresh
            refSet_ = false;
            return o;
        }
        if (!in.held) {
            // Not (or no longer) two-handed: the clip pumps itself. (Before the pump a hand may still
            // take hold.)
            if (stage_ == Stage::Firing && in.clipT < c.backT0) return o;
            const bool wasHeld = stage_ == Stage::Back || stage_ == Stage::Fwd;
            stage_ = Stage::Done;
            if (wasHeld) {
                o.run = true;
                o.event = "let go: the shotgun pumps itself";
            }
            return o;
        }
        if (in.heldS < c.settleS) { // a fresh grab settles: its reference is taken once it has
            rebase_ = true;
            refSet_ = false;
            if (stage_ == Stage::Firing && in.clipT < waitT) return o;
            o.hold = true;
            o.t = stage_ == Stage::Fwd ? fwdT_ : stage_ == Stage::Back ? c.backT0 : waitT;
            o.deny = in.triggerRise;
            return o;
        }
        if (!in.alongOk) { // a controller lost: hold where it is, measure nothing
            if (stage_ == Stage::Firing) return o;
            o.hold = true;
            o.t = stage_ == Stage::Back ? c.backT0 : fwdT_;
            return o;
        }
        const float along = -in.alongCm; // + is back: the hands closer together
        if (rebase_) {
            ref_ = along;
            rebase_ = false;
        }
        const float strokeU = c.strokeCm;
        const float follow = c.followCm > 1e-3f ? c.followCm : 1.0f;
        switch (stage_) {
            case Stage::Firing:
                // The shot and its recoil play - unless the hand is already pumping: Half-Life 2 VR starts
                // the stroke's reference AT the shot, so a quick hand never waits out the clip's 0.75 s of
                // recoil (the clip is played on to the pump at up to 4x, its sounds and all).
                if (in.clipNew || !refSet_) {
                    ref_ = along;
                    refSet_ = true;
                }
                if (along < ref_) ref_ = along;
                if (along - ref_ < c.earlyCm) {
                    if (in.clipT < waitT) return o; // the shot and its recoil play
                    o.hold = true;                  // then it waits, before the pump and its sound
                    o.t = waitT;
                    o.deny = in.triggerRise;
                    return o;
                }
                stage_ = Stage::Back;
                refSet_ = false;
                o.event = in.clipT < waitT ? "pump it: back (begun during the recoil)" : "pump it: back";
                [[fallthrough]];
            case Stage::Back: {
                if (along < ref_) ref_ = along; // the ratchet: the farthest forward it has been
                const float d = along - ref_;
                o.hold = true;
                o.chase = true;
                o.t = c.backT0 + (c.backT1 - c.backT0) * (d / follow < 1.0f ? d / follow : 1.0f);
                o.strokeCm = d;
                o.deny = in.triggerRise;
                if (d >= strokeU) {
                    stage_ = Stage::Fwd;
                    ref_ = along;
                    fwdT_ = c.backT1;
                    o.haptic = Haptic::Tick;
                    o.t = c.backT1; // the catch: the clip finishes the stroke
                    o.event = "pump it: forward";
                }
                return o;
            }
            case Stage::Fwd: {
                if (along > ref_) ref_ = along; // the farthest back it has been
                const float d = ref_ - along;
                o.hold = true;
                o.chase = true;
                // Back at the pump's rear stop until the hand starts it forward.
                // 2026-10-22 (the 10:31 run: a hand dithering through the forward stroke ejected a shell on every
                // pass - the clip, chased forward over its shell-eject cue, was SET back to the rear stop each
                // time the hand retreated, then chased over the cue again; 20 shells in 11 s): the clip never
                // goes back along the forward stroke. The ratchet's reference still does, so the stroke's 6 cm
                // are measured from the farthest back as before.
                float t = d > 0.5f ? c.fwdT0 + (c.fwdT1 - c.fwdT0) * (d / follow < 1.0f ? d / follow : 1.0f) : c.backT1;
                if (t < fwdT_) t = fwdT_;
                fwdT_ = t;
                o.t = t;
                o.strokeCm = d;
                o.deny = in.triggerRise;
                if (d >= strokeU) {
                    stage_ = Stage::Done;
                    o.hold = false;
                    o.run = true;
                    o.jump = false; // played on from where it is: the forward stroke's sound fires
                    o.t = c.fwdT1;
                    o.haptic = Haptic::Seat;
                    o.event = "pumped: the next shell is in";
                }
                return o;
            }
            default: return o;
        }
    }
    bool working() const { return stage_ == Stage::Back || stage_ == Stage::Fwd; }
    bool backDone() const { return stage_ == Stage::Fwd; }
    bool done() const { return stage_ == Stage::Done; } // the forward stroke finished

private:
    enum class Stage : uint8_t { Off, Firing, Back, Fwd, Done };
    Stage stage_ = Stage::Off;
    float ref_ = 0;
    float fwdT_ = 0;      // the farthest the clip has been chased along the forward stroke (never set back)
    bool refSet_ = false;
    bool rebase_ = false; // the ratchet's reference restarts at the hands' next reading
};

} // namespace bvr::b1r::reload_rig
