// reload-rig-tests.cpp - host tests for src/game/bioshock1r/reload_rig.h, the BS1 manual reload's
// programs and state machine. Run `.\tools\host-test.ps1 reload-rig`.
//
// What has to hold:
//   project - the nearest point of a path gives the clip time between its samples and the
//             progress along its length; far from the path the distance says so
//   tommy   - a grip away from the drum starts nothing; at the drum it asks the game to reload;
//             the held clip time follows the hand down the drum's path, only forward; past the
//             end the drum rides the hand; letting go drops it; a grip at the pouch takes a new one;
//             near the well it is guided home and seats; the bolt is pulled the same way; the clip
//             then plays on its own and is let run to its end; the reload ending returns to idle
//   timeout - no clip within the start window aborts (a full gun, no ammunition)
//   shotgun - a grip at the pouch starts the reload with the shell already in hand; the start clip
//             runs; the loop seats the shell; the next loop wants a new shell from the pouch;
//             the end clip is the pump, back then forward
//   scale   - a variant clip of a different length scales every held time
#include "game/bioshock1r/reload_rig.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace bvr::b1r::reload_rig;
using bvr::b1r::arm_ik::Vec;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
    }
}

struct Sim {
    Machine m;
    Config c;
    Input in;
    float now = 0;
    const wchar_t* cls = L"MachineGun";
    Output step(Vec hand, bool grip, bool pouch = false) {
        now += 0.011f;
        in.now = now;
        in.hand = hand;
        in.gripRise = grip && !in.grip;
        in.grip = grip;
        in.inPouch = pouch;
        in.entryProgram = entry(cls);
        in.clipProgram = in.clipLive ? find(cls, in.clip) : nullptr;
        Output o = m.update(in, c);
        in.clipNew = false;
        return o;
    }
    void clip(const wchar_t* name, float len) {
        in.clipLive = true;
        in.clip = name;
        in.clipLen = len;
        in.clipNew = true;
    }
};

int main() {
    // ---- project ----
    {
        const Projection a = project(kTommyDrumOut, 4, kTommyDrumOut[0].p);
        check(std::fabs(a.t - 0.267f) < 1e-4f && a.progress < 1e-4f && a.dist < 1e-4f, "path start", a.t, 0.267);
        const Projection b = project(kTommyDrumOut, 4, kTommyDrumOut[3].p);
        check(std::fabs(b.t - 0.567f) < 1e-4f && b.progress > 0.9999f, "path end", b.t, 0.567);
        const Vec mid = (kTommyDrumOut[1].p + kTommyDrumOut[2].p) * 0.5f;
        const Projection m = project(kTommyDrumOut, 4, mid + Vec{0, 3, 0});
        check(m.t > 0.369f && m.t < 0.466f && std::fabs(m.dist - 3.0f) < 0.6f, "mid segment, 3 off", m.t, 0.42);
        const Projection far = project(kTommyDrumOut, 4, Vec{200, 0, 0});
        check(far.dist > 150.0f, "far from the path (negative control)", far.dist, 150);
    }
    // ---- tommy, the whole reload ----
    {
        Sim s;
        Output o = s.step(Vec{40, 7, 2}, false); // at the fore-grip, not the drum
        o = s.step(Vec{40, 7, 2}, true);
        check(o.act == Act::None && !s.m.active(), "a grip away from the drum starts nothing");
        o = s.step(Vec{40, 7, 2}, false);
        o = s.step(kTommyDrumOut[0].p + Vec{2, 0, 0}, false);
        check(o.buzz && o.claim, "at the drum: buzz, and the grip is the reload's");
        o = s.step(kTommyDrumOut[0].p + Vec{2, 0, 0}, true);
        check(o.act == Act::StartReload && s.m.active(), "a grip at the drum asks the game to reload");
        s.clip(L"Reload", 3.33f);
        o = s.step(Vec{40, 7, 2}, false);
        check(!o.claim && s.m.hand_free(), "waiting, the hand away from the drum: its grip is free (hand-over, two hands)");
        o = s.step(kTommyDrumOut[0].p, false);
        check(o.claim && s.m.hand_free(), "waiting at the drum: the grip is the reload's");
        o = s.step(kTommyDrumOut[0].p, true);
        check(o.hold && std::fabs(o.t - 0.267f) < 0.01f, "held at the drum's segment start", o.t, 0.267);
        check(!s.m.hand_free(), "pulling the drum: the hand is the reload's (negative control)");
        o = s.step(kTommyDrumOut[2].p, true);
        check(o.hold && std::fabs(o.t - 0.466f) < 0.01f, "the clip follows the hand down", o.t, 0.466);
        o = s.step(kTommyDrumOut[1].p, true);
        check(o.t >= 0.46f, "never back up the path", o.t, 0.466);
        o = s.step(kTommyDrumOut[3].p + Vec{0, 0, -5}, true);
        check(s.m.phase() == 1, "past the end: the next phase (carry)", s.m.phase(), 1);
        o = s.step(Vec{10, 0, -90}, true);
        check(o.carry == 1 && o.part && std::fabs(o.t - 0.567f) < 0.01f, "the drum rides the hand", o.carry, 1);
        o = s.step(Vec{10, 0, -90}, false);
        check(o.carry == 2 && s.m.phase() == 2, "let go: dropped, fetch next", s.m.phase(), 2);
        o = s.step(Vec{10, 0, -90}, false, true);
        check(o.buzz && o.carry == 2 && std::fabs(o.t - 0.30f) < 0.01f, "at the pouch: buzz, clip where its hand holds the drum", o.t,
              0.30);
        o = s.step(Vec{10, 0, -90}, true, true);
        check(s.m.phase() == 3, "a grip at the pouch takes a drum", s.m.phase(), 3);
        o = s.step(Vec{10, 0, -90}, true);
        check(o.carry == 1, "the new drum rides the hand", o.carry, 1);
        o = s.step(kTommyDrumIn[0].p + Vec{0, 2, 0}, true);
        o = s.step(kTommyDrumIn[1].p, true);
        check(o.carry == 0 && o.t > 0.95f, "near the well: guided, the clip carries it", o.t, 1.0);
        o = s.step(kTommyDrumIn[3].p, true);
        check(s.m.phase() == 4, "seated: the bolt next", s.m.phase(), 4);
        o = s.step(kTommyDrumIn[3].p, false);
        o = s.step(Vec{kTommyBolt[0].p.x, -60.0f, kTommyBolt[0].p.z}, false);
        check(!o.buzz, "the bolt's reach stops at the gun: the other shoulder does not buzz");
        o = s.step(Vec{kTommyBolt[0].p.x, -kTommyBolt[0].p.y, kTommyBolt[0].p.z}, false);
        check(o.buzz, "the bolt from the gun's other side does");
        o = s.step(kTommyBolt[0].p, false);
        check(o.buzz && std::fabs(o.t - 2.10f) < 0.01f, "the clip waits at the bolt", o.t, 2.10);
        o = s.step(kTommyBolt[0].p, true);
        o = s.step(kTommyBolt[0].p - Vec{8.2f, 0, 0}, true); // half its 16.4 travel back
        check(o.handLock && std::fabs(o.t - 2.15f) < 0.01f, "held: the bolt follows the hand, the hand on it",
              o.t, 2.15);
        o = s.step(kTommyBolt[0].p - Vec{4.1f, 0, 0}, true); // and forward again
        check(std::fabs(o.t - 2.125f) < 0.01f, "both ways while held", o.t, 2.125);
        o = s.step(kTommyBolt[0].p - Vec{16.4f, 0, 0}, true);
        check(s.m.phase() == 5, "racked: the bolt runs home on its own", s.m.phase(), 5);
        for (int i = 0; i < 80; ++i) o = s.step(Vec{40, 7, 2}, false);
        check(s.m.phase() == 6 && o.run, "then the clip is let run to its end", s.m.phase(), 6);
        s.in.clipLive = false;
        o = s.step(Vec{40, 7, 2}, false);
        check(!s.m.active(), "the reload ended: idle");
    }
    // ---- the new drum let go short of its seat (2026-10-20: it fell out, the gun still fired) ----
    {
        Sim s;
        s.in.eject = true;
        Output o = s.step(Vec{40, 7, 2}, false);
        s.in.eject = false;
        check(o.act == Act::StartReload, "eject: the game is asked to reload");
        s.clip(L"Reload", 3.33f);
        o = s.step(Vec{10, 0, -90}, false);
        check(o.ejected, "the clip starts: the drum drops");
        o = s.step(Vec{40, 7, 2}, false);
        check(o.carry == 3 && o.reach < 0.0f, "dropping: the reload wants nothing of the hand (G1: a squeeze is the grip's)", o.reach, -1);
        for (int i = 0; i < 80; ++i) o = s.step(Vec{10, 0, -90}, false);
        check(s.m.phase() == 2, "then the reload waits at the pouch", s.m.phase(), 2);
        check(o.reach < 0.0f, "at the fetch, away from the pouch: nothing offered", o.reach, -1);
        o = s.step(Vec{10, 0, -90}, false, true);
        check(std::fabs(o.reach - 0.5f) < 1e-6f, "at the pouch: the pouch zone", o.reach, 0.5);
        o = s.step(Vec{10, 0, -90}, true, true);
        check(o.fetched && s.m.phase() == 3, "a drum from the pouch: fetched", s.m.phase(), 3);
        o = s.step(Vec{10, 0, -60}, true);
        check(o.carry == 1 && o.reach < 0.0f, "carrying: nothing offered (the grip is down anyway)", o.reach, -1);
        o = s.step(Vec{10, 0, -60}, true);
        o = s.step(Vec{10, 0, -60}, false); // let go away from the gun
        check(o.dropped && o.carry == 2 && s.m.phase() == 2, "let go away from the gun: it falls, back to the pouch",
              s.m.phase(), 2);
        o = s.step(kTommyDrumIn[2].p, false);
        o = s.step(kTommyDrumIn[3].p, false);
        check(s.m.phase() == 2 && !o.dropped, "no part, no seat: a bare hand along the path does nothing",
              s.m.phase(), 2);
        o = s.step(Vec{10, 0, -90}, true, true);
        o = s.step(kTommyDrumIn[0].p + Vec{0, 2, 0}, true);
        o = s.step(kTommyDrumIn[0].p, true);
        check(o.carry == 0 && o.seating && !o.chase,
              "guided: the clip is SET from the carry hold onto the insert, never played through its toss");
        o = s.step(kTommyDrumIn[0].p, true);
        check(o.carry == 0 && !o.seating, "at the well's mouth: guided, played from there");
        o = s.step(kTommyDrumIn[0].p, false); // a slap: let go at the mouth, short of half way
        check(!o.dropped && o.carry == 0, "slapped in: let go guided, it is seated, not dropped");
        for (int i = 0; i < 30 && s.m.phase() == 3; ++i) o = s.step(Vec{10, 0, -60}, false);
        check(s.m.phase() == 4, "the clip plays it home: the bolt next", s.m.phase(), 4);
    }
    // ---- a dry Tommy Gun waits for the hand to pull the drum; X drops it instead (20:35: "locked up") ----
    {
        Sim s;
        s.in.adopt = true;
        s.clip(L"Reload", 3.33f);
        Output o = s.step(Vec{40, 7, 2}, false);
        s.in.adopt = false;
        check(s.m.active() && s.m.phase() == 0 && !o.ejected, "dry: the reload waits at the drum, nothing dropped");
        // 2026-10-21 (HELD_GUN_MATRIX.md G7): a waiting reload's reach is a DISTANCE for the arbiter, never 0.
        check(o.reach > 1.0f && !o.claim, "waiting, the hand at the fore-grip: the drum is out of reach, a distance (not 0)", o.reach, 2);
        o = s.step(kTommyDrumOut[0].p + Vec{3, 0, 0}, false);
        check(o.reach > 0.0f && o.reach <= 1.0f && o.claim, "the hand near the drum: in reach, still a distance", o.reach, 0.25);
        o = s.step(Vec{40, 7, 2}, false);
        s.in.ejectNow = true;
        o = s.step(Vec{40, 7, 2}, false);
        s.in.ejectNow = false;
        check(o.ejected, "the reload button there: the drum drops");
        for (int i = 0; i < 80; ++i) o = s.step(Vec{10, 0, -90}, false);
        check(s.m.phase() == 2, "then the pouch", s.m.phase(), 2);
    }
    // ---- 2026-10-21 a Tommy Gun left with no drum: its next reload begins at the pouch, nothing dropped ----
    {
        Sim s;
        s.in.adopt = true;
        s.in.ejectAdopted = true;
        s.in.magOut = true;
        s.clip(L"Reload", 3.33f);
        Output o = s.step(Vec{40, 7, 2}, false);
        s.in.adopt = false;
        check(s.m.active() && s.m.phase() == 2 && !o.ejected && s.m.mag_out(),
              "no drum: the reload waits at the pouch, nothing falls", s.m.phase(), 2);
        o = s.step(Vec{10, 0, -90}, false, true);
        o = s.step(Vec{10, 0, -90}, true, true);
        check(o.fetched && !s.m.mag_out(), "a drum from the pouch: the gun has one again (negative control)");
    }
    // ---- 2026-10-20 the spare drum: pulled any time the Tommy Gun is held, X swaps it in quickly ----
    {
        Sim s;
        s.in.spareOk = true;
        Output o = s.step(Vec{10, 0, -90}, false, true);
        check(!s.m.active() && o.claim && o.spareReach > 0 && o.buzz, "idle, at the pouch: the grip would take a spare (it buzzes)");
        check(o.spareReach <= 0.25f && o.reach > 1.0f, "the spare's pouch zone is its own offer; the drum's reach is a real distance",
              o.spareReach, 0.25);
        o = s.step(Vec{10, 0, -90}, true, true);
        check(o.spareTake && !s.m.active() && o.act == Act::None, "a grip at the pouch takes a spare - no reload starts");
        s.in.spareInHand = true;
        o = s.step(Vec{20, 0, -40}, true);
        check(o.claim && !o.spareTake && !o.spareLetGo, "held: the grip is the spare's");
        s.in.eject = true;
        o = s.step(Vec{20, 0, -40}, true);
        s.in.eject = false;
        check(o.act == Act::StartReload, "X with a spare in hand: the game is asked to reload");
        s.clip(L"Reload", 3.33f);
        o = s.step(Vec{20, 0, -40}, true);
        check(o.ejected, "the drum drops");
        int frames = 0;
        for (; frames < 80 && s.m.phase() != 3; ++frames) o = s.step(Vec{20, 0, -40}, true);
        check(s.m.phase() == 3 && frames * 0.011f < 0.3f, "the spare goes in at once (0.2 s, not the 0.7 s fall)",
              frames * 0.011f, 0.2);
        check(o.spareUsed && o.fetched && o.carry == 1 && std::fabs(o.t - 0.30f) < 0.01f,
              "the spare becomes the gun's own drum, riding the hand where the clip holds it", o.carry, 1);
        s.in.spareInHand = false;
        o = s.step(kTommyDrumIn[0].p, true);
        o = s.step(kTommyDrumIn[3].p, true);
        check(s.m.phase() >= 4, "and seats as any drum does", s.m.phase(), 4);
    }
    // ---- the spare during a dry gun's wait; let go it is thrown; never at the bolt mid-stroke ----
    {
        Sim s;
        s.in.spareOk = true;
        s.in.adopt = true;
        s.clip(L"Reload", 3.33f);
        Output o = s.step(Vec{40, 7, 2}, false);
        s.in.adopt = false;
        o = s.step(Vec{10, 0, -90}, true, true);
        check(o.spareTake && s.m.phase() == 0, "a dry gun waiting at its drum: a spare can still be pulled");
        s.in.spareInHand = true;
        o = s.step(Vec{10, 0, -60}, false);
        check(o.spareLetGo && s.m.phase() == 0, "let go: thrown, the reload still waits");
        s.in.spareInHand = false;
        o = s.step(Vec{10, 0, -60}, false);
        check(!o.spareLetGo && !o.spareTake, "once only");
    }
    {
        Sim s; // at the Fetch, the pouch gives the gun's own drum, never a spare
        s.in.spareOk = true;
        s.in.eject = true;
        Output o = s.step(Vec{40, 7, 2}, false);
        s.in.eject = false;
        s.clip(L"Reload", 3.33f);
        for (int i = 0; i < 80; ++i) o = s.step(Vec{10, 0, -90}, false);
        o = s.step(Vec{10, 0, -90}, true, true);
        check(o.fetched && !o.spareTake, "at the Fetch: the gun's own drum, not a spare");
        o = s.step(Vec{10, 0, -60}, true, true);
        check(!o.spareTake, "carrying: no spare");
    }
    // ---- a thrown spare caught in the open well is seated ----
    {
        Sim s;
        s.in.eject = true;
        Output o = s.step(Vec{40, 7, 2}, false);
        s.in.eject = false;
        s.clip(L"Reload", 3.33f);
        for (int i = 0; i < 80; ++i) o = s.step(Vec{40, 7, 2}, false);
        check(s.m.phase() == 2, "waiting at the pouch", s.m.phase(), 2);
        s.in.caught = true;
        o = s.step(Vec{40, 7, 2}, false);
        s.in.caught = false;
        check(s.m.phase() == 3 && o.carry == 0 && o.seating && o.haptic == Haptic::Seat,
              "caught: seated, the clip set onto the insert's last stretch", s.m.phase(), 3);
        for (int i = 0; i < 30 && s.m.phase() == 3; ++i) o = s.step(Vec{40, 7, 2}, false);
        check(s.m.phase() == 4, "the clip plays it home: the bolt next", s.m.phase(), 4);
        Sim n; // and not caught while the gun still has its drum
        n.in.caught = true;
        o = n.step(Vec{40, 7, 2}, false);
        check(!n.m.active() && o.act == Act::None, "NEGATIVE: a caught drum with no open well does nothing");
    }
    // ---- a round still in the Tommy Gun: no bolt to rack ----
    {
        Sim s;
        s.in.chambered = true;
        s.in.eject = true;
        Output o = s.step(Vec{40, 7, 2}, false);
        s.in.eject = false;
        s.clip(L"Reload", 3.33f);
        for (int i = 0; i < 80; ++i) o = s.step(Vec{10, 0, -90}, false);
        o = s.step(Vec{10, 0, -90}, true, true);
        o = s.step(kTommyDrumIn[0].p, true);
        o = s.step(kTommyDrumIn[3].p, true);
        o = s.step(kTommyDrumIn[3].p, false);
        check(s.m.phase() == 6 && o.run, "seated with a round in: straight to the end, no bolt", s.m.phase(), 6);
    }
    // ---- a shell let go near the port is pressed in (19:57: the second shell kept falling) ----
    {
        Sim s;
        s.cls = L"Shotgun";
        s.in.adopt = true;
        s.clip(L"Reload_Loop", 0.49f);
        Output o = s.step(Vec{10, 0, -90}, false);
        s.in.adopt = false;
        o = s.step(Vec{10, 0, -90}, true, true);
        check(s.m.phase() == 1, "a shell from the pouch", s.m.phase(), 1);
        const Vec port = kShotgunShell[5].p;
        o = s.step(port + Vec{0, 12, 0}, true);
        o = s.step(port + Vec{0, 12, 0}, false);
        check(!o.dropped && o.carry == 0, "let go 12 cm from the port: pressed in, not dropped");
        Sim f;
        f.cls = L"Shotgun";
        f.in.adopt = true;
        f.clip(L"Reload_Loop", 0.49f);
        o = f.step(Vec{10, 0, -90}, false);
        f.in.adopt = false;
        o = f.step(Vec{10, 0, -90}, true, true);
        o = f.step(Vec{10, 0, -60}, true);
        o = f.step(Vec{10, 0, -60}, false);
        check(o.dropped, "let go far from the port: it falls");
    }
    // ---- timeout ----
    {
        Sim s;
        s.step(kTommyDrumOut[0].p, false);
        Output o = s.step(kTommyDrumOut[0].p, true);
        for (int i = 0; i < 70 && o.act != Act::Abort; ++i) o = s.step(kTommyDrumOut[0].p, true);
        check(o.act == Act::Abort && !s.m.active(), "no clip: abort");
    }
    // ---- shotgun ----
    {
        Sim s;
        s.cls = L"Shotgun";
        Output o = s.step(Vec{10, 0, -60}, false, true);
        check(o.claim && o.buzz, "the shotgun: the pouch is where a reload begins");
        o = s.step(Vec{10, 0, -60}, true, true);
        check(o.act == Act::StartReload, "a shell from the pouch starts it");
        s.clip(L"SingleFrame", 0.033f);
        o = s.step(Vec{10, 0, -60}, true);
        check(o.run, "the start clip runs on");
        s.clip(L"Reload_Loop", 0.49f);
        o = s.step(Vec{10, 0, -60}, true);
        check(s.m.phase() == 1 && o.carry == 1, "the loop: the shell is already in hand", s.m.phase(), 1);
        o = s.step(kShotgunShell[0].p, true);
        o = s.step(kShotgunShell[5].p, true);
        check(s.m.phase() == 2, "pressed in", s.m.phase(), 2);
        for (int i = 0; i < 40; ++i) o = s.step(Vec{30, 0, -20}, false);
        check(o.run, "the loop runs out");
        s.clip(L"Reload_Loop", 0.49f);
        o = s.step(Vec{30, 0, -20}, false);
        check(s.m.phase() == 0 && o.carry == 2, "the next loop wants a new shell", s.m.phase(), 0);
        o = s.step(Vec{10, 0, -60}, true, true);
        check(s.m.phase() == 1, "taken at the pouch", s.m.phase(), 1);
        s.clip(L"Reload", 0.95f);
        o = s.step(Vec{52, 18, 16}, false);
        check(s.m.program() && s.m.program()->clip == kPrograms[5].clip, "the end clip is the pump");
        o = s.step(kShotgunPumpBack[0].p, true);
        check(o.pumpWait && !o.claim && !o.hold && !o.run,
              "the last pump waits for the two-handed pump: the grip is not the reload's");
        s.in.pumpDone = true;
        o = s.step(kShotgunPumpBack[0].p, true);
        s.in.pumpDone = false;
        o = s.step(kShotgunPumpBack[0].p, false);
        check(s.m.phase() == 1 && o.run && o.jump, "pumped: the clip let run to its end", s.m.phase(), 1);
    }
    // ---- adopt: the game reloads by itself (the gun ran dry) ----
    {
        Sim s;
        s.clip(L"Reload", 3.33f);
        s.in.adopt = true;
        Output o = s.step(Vec{40, 7, 2}, false);
        check(s.m.active() && o.hold && std::fabs(o.t - 0.267f) < 0.01f, "adopted: held at the drum, waiting", o.t,
              0.267);
        s.in.adopt = false;
        o = s.step(kTommyDrumOut[0].p, true);
        o = s.step(kTommyDrumOut[3].p, true);
        check(s.m.phase() == 1, "and the hands carry it on", s.m.phase(), 1);
        Sim b; // the reload button: not adopted - it plays as always
        b.clip(L"Reload", 3.33f);
        o = b.step(Vec{40, 7, 2}, false);
        check(!b.m.active() && !o.hold, "a reload the button began is never held");
    }
    // ---- eject: the reload button with the manual reload on ----
    {
        Sim s;
        s.in.eject = true;
        Output o = s.step(Vec{40, 7, 2}, false);
        check(o.act == Act::StartReload && o.carry == 0, "eject: the reload is asked for, the drum still in");
        s.in.eject = false;
        o = s.step(Vec{40, 7, 2}, false);
        check(s.m.active() && o.carry == 0, "waiting for the game: the drum stays in");
        s.clip(L"Reload", 3.33f);
        o = s.step(Vec{40, 7, 2}, false);
        check(o.ejected && o.part && !strcmp(o.part, "TG_AmmoClip"), "the clip starts: the drum is to fall");
        check(o.carry == 3 && o.hold && std::fabs(o.t - 0.267f) < 0.01f,
              "held where the drum still sits, falling", o.t, 0.267);
        o = s.step(Vec{40, 7, 2}, false);
        check(!o.ejected && o.carry == 3 && o.fallS > 0.0f, "the drop is once; the fall goes on", o.fallS, 0.011);
        for (int i = 0; i < 70; ++i) o = s.step(Vec{40, 7, 2}, false);
        check(s.m.phase() == 2 && o.hold && std::fabs(o.t - 0.30f) < 0.01f && o.carry == 3,
              "after the fall: the fetch, the gun open, the old drum lying where it fell", o.t, 0.30);
        o = s.step(Vec{0, 0, -40}, true, true);
        check(s.m.phase() == 3, "a new drum from the pouch", s.m.phase(), 3);

        Sim p;
        p.cls = L"Pistol";
        p.in.eject = true;
        p.step(Vec{40, 7, 2}, false);
        p.in.eject = false;
        p.clip(L"FastReload", 1.80f);
        o = p.step(Vec{40, 7, 2}, false);
        check(o.ejected && o.carry == 3 && std::fabs(o.t - 0.466f) < 0.01f, "pistol: open, the cylinder falls", o.t,
              0.466);
        for (int i = 0; i < 70; ++i) o = p.step(Vec{40, 7, 2}, false);
        check(p.m.phase() == 3 && std::fabs(o.t - 0.80f) < 0.01f, "then take a new one (held where the clip's hand holds it)",
              o.t, 0.80);

        // The latch: the carried drum within 10 cm of its seat snaps home, wherever the palm is.
        s.in.partSeatCm = 30.0f;
        o = s.step(Vec{90, -40, 60}, true); // far off the recorded palm path
        check(s.m.phase() == 3, "far from the latch: still carrying", s.m.phase(), 3);
        s.in.partSeatCm = 8.0f;
        o = s.step(Vec{90, -40, 60}, true);
        for (int i = 0; i < 40 && s.m.phase() == 3; ++i) o = s.step(Vec{90, -40, 60}, true);
        check(s.m.phase() == 4, "within 10 cm of the latch: seated by the clip", s.m.phase(), 4);
        s.in.partSeatCm = -1.0f;
        // The bolt (phase 4) from the OTHER side of the gun: the sideways axis does not count.
        Vec boltOther = kTommyBolt[0].p;
        boltOther.y = -boltOther.y - 10.0f;
        o = s.step(boltOther, false);
        o = s.step(boltOther, true);
        Vec boltEnd = boltOther - Vec{17.0f, 0, 0};
        for (int i = 0; i < 5; ++i) o = s.step(boltEnd, true);
        check(s.m.phase() == 5, "the bolt racked from the other side", s.m.phase(), 5);

        Sim d; // the gun ran dry: the game's own reload ejects the same way
        d.in.ejectAdopted = true;
        d.clip(L"Reload", 3.33f);
        d.in.adopt = true;
        o = d.step(Vec{40, 7, 2}, false);
        check(d.m.active() && o.ejected && o.carry == 3, "a dry gun's reload drops the drum too");
        Sim d0; // ejectAdopted off (the manual reload's old way): it waits at the drum
        d0.clip(L"Reload", 3.33f);
        d0.in.adopt = true;
        o = d0.step(Vec{40, 7, 2}, false);
        check(d0.m.active() && !o.ejected && std::fabs(o.t - 0.267f) < 0.01f, "without it: waits for the hands");

        Sim a; // the game will not reload: nothing is dropped
        a.in.eject = true;
        a.step(Vec{40, 7, 2}, false);
        a.in.eject = false;
        for (int i = 0; i < 80; ++i) o = a.step(Vec{40, 7, 2}, false);
        check(!a.m.active() && o.carry == 0 && !o.ejected, "no reload: the drum stays");

        Sim g; // the shotgun starts at the pouch: no eject
        g.cls = L"Shotgun";
        g.in.eject = true;
        o = g.step(Vec{40, 7, 2}, false);
        check(o.act != Act::StartReload && !g.m.active(), "the shotgun does not eject");

        Sim f; // full: the button does nothing
        f.in.eject = true;
        f.in.canReload = false;
        o = f.step(Vec{40, 7, 2}, false);
        check(o.act != Act::StartReload && o.haptic == Haptic::Deny, "a full gun does not eject: it buzzes no");
    }
    // ---- the shotgun reload's last pump is the pump between shots (PumpHand + the waiting Pump) ----
    {
        Sim s;
        s.cls = L"Shotgun";
        s.in.adopt = true;
        s.clip(L"Reload", 0.95f);
        Output o = s.step(kShotgunPumpBack[0].p, false);
        s.in.adopt = false;
        check(s.m.active() && s.m.phase() == 0 && o.pumpWait, "the end clip: waiting at the pump", s.m.phase(), 0);
        // The Pump over the end clip, waiting for the hands: one-handed it holds, it never pumps itself.
        PumpConfig c;
        c.backT0 = 0.409f;
        c.backT1 = 0.487f;
        c.fwdT0 = 0.532f;
        c.fwdT1 = 0.676f;
        c.earlyCm = 2.5f;
        c.waitForHands = true;
        Pump p;
        PumpInput in;
        in.fireClip = in.clipNew = in.alongOk = true;
        in.clipT = 0.41f;
        in.alongCm = 40.0f;
        PumpOutput po = p.update(in, c);
        in.clipNew = false;
        check(po.hold && !po.run && std::fabs(po.t - 0.409f) < 0.01f, "not two-handed: it waits at the pump", po.t, 0.409);
        po = p.update(in, c);
        check(po.hold && !po.run, "and keeps waiting (never pumps itself)");
        in.held = true;
        po = p.update(in, c);
        in.alongCm = 33.5f;
        po = p.update(in, c);
        check(p.backDone(), "two-handed: 6 cm closer, the back stroke");
        in.alongCm = 40.0f;
        po = p.update(in, c);
        check(po.run && !p.working(), "6 cm apart: pumped, the clip plays out");
        // 12:17 run: the hand back from the pouch, taken hold of as it came, cocked it in 0.33 s. A fresh grab settles.
        Pump p2;
        in.held = false;
        in.clipNew = true;
        in.alongCm = 45.0f;
        po = p2.update(in, c);
        in.clipNew = false;
        in.held = true;
        in.heldS = 0.05f;
        in.alongCm = 38.0f; // still coming in: 7 cm
        po = p2.update(in, c);
        in.heldS = 0.15f;
        in.alongCm = 46.0f; // and settling back out: 8 cm
        po = p2.update(in, c);
        check(po.hold && !po.run && !p2.backDone(), "a fresh grab's approach and settle count no stroke");
        in.heldS = 0.5f;
        po = p2.update(in, c);
        in.alongCm = 39.5f;
        po = p2.update(in, c);
        check(p2.backDone(), "settled: a real 6 cm stroke counts");
    }
    // ---- the shotgun's pump by hand: Half-Life 2 VR's ratchet strokes ----
    {
        Pump pump;
        const PumpConfig c; // 6 cm strokes, measured between the two controllers
        PumpInput in;
        in.fireClip = true;
        in.clipNew = true;
        in.held = in.alongOk = true;
        const float h = 40.0f; // the controllers 40 cm apart
        in.alongCm = h;
        in.clipT = 0.3f;
        PumpOutput o = pump.update(in, c);
        in.clipNew = false;
        check(!o.hold && !o.run, "the shot plays first");
        in.clipT = 0.76f;
        o = pump.update(in, c);
        check(o.hold && std::fabs(o.t - 0.75f) < 0.01f && !pump.working(), "at the pump: the clip waits for the hand",
              o.t, 0.75);
        in.triggerRise = true;
        o = pump.update(in, c);
        in.triggerRise = false;
        check(o.deny && o.hold, "the trigger while it waits: a click, nothing fires");
        in.alongCm = h + 3.0f; // forward first: the ratchet's reference follows it
        o = pump.update(in, c);
        in.alongCm = h - 2.0f; // 5 cm back from there
        o = pump.update(in, c);
        check(o.hold && std::fabs(o.strokeCm - 5.0f) < 0.01f && !pump.backDone(),
              "measured from the farthest forward: 5 cm, not yet", o.strokeCm, 5.0);
        check(std::fabs(o.t - (0.75f + 0.1f * 5.0f / 9.0f)) < 0.005f, "the clip follows the hands", o.t, 0.806);
        in.alongCm = h - 3.2f;
        o = pump.update(in, c);
        check(pump.backDone() && std::fabs(o.t - 0.85f) < 0.01f, "6 cm: caught, the clip finishes the stroke", o.t, 0.85);
        const float back = in.alongCm;
        in.alongCm = back - 2.0f; // further back first
        o = pump.update(in, c);
        check(std::fabs(o.t - 0.85f) < 0.01f, "held at the rear stop", o.t, 0.85);
        in.alongCm = back + 3.0f; // 5 cm forward of the farthest back
        o = pump.update(in, c);
        check(o.hold && std::fabs(o.strokeCm - 5.0f) < 0.01f, "forward from the farthest back: 5 cm", o.strokeCm, 5.0);
        in.alongCm = back + 4.1f;
        o = pump.update(in, c);
        check(o.run && !o.jump && !pump.working(), "6 cm forward: pumped, the clip plays out");
        o = pump.update(in, c);
        check(!o.hold && !o.run, "then nothing more this shot");
        in.clipNew = true; // the next shot's clip, no frame between
        in.clipT = 0.0f;
        o = pump.update(in, c);
        in.clipNew = false;
        in.clipT = 0.76f;
        o = pump.update(in, c);
        check(o.hold && std::fabs(o.t - 0.75f) < 0.01f, "the next shot waits for its own pump");
        in.fireClip = false;
        pump.update(in, c);

        Pump e; // a quick hand: the stroke begins during the recoil
        in = PumpInput{};
        in.fireClip = in.clipNew = in.held = in.alongOk = true;
        in.alongCm = h;
        in.clipT = 0.1f;
        e.update(in, c);
        in.clipNew = false;
        in.alongCm = h - 3.0f;
        o = e.update(in, c);
        check(e.working() && o.hold && o.t >= 0.75f, "3 cm closer during the recoil: the pump begins", o.t, 0.75);

        Pump q; // not two-handed: the clip pumps itself
        in = PumpInput{};
        in.fireClip = true;
        in.clipT = 0.8f;
        o = q.update(in, c);
        check(!o.hold, "one-handed: never held");
        Pump r; // let go mid-pump: it runs on
        in.held = in.alongOk = true;
        in.alongCm = 40.0f;
        in.clipT = 0.76f;
        r.update(in, c);
        in.alongCm = 36.0f; // a stroke begun
        r.update(in, c);
        in.held = false;
        o = r.update(in, c);
        check(o.run && !o.hold, "let go mid-pump: the shotgun pumps itself (the default, no waitForHands)");
        // 2026-10-21: the shot pump the game uses - waits after the recoil (0.45), for the hands, held or not.
        PumpConfig sc;
        sc.waitT = 0.45f;
        sc.waitForHands = true;
        Pump w;
        in = PumpInput{};
        in.fireClip = in.clipNew = in.alongOk = true;
        in.alongCm = 40.0f;
        in.clipT = 0.3f;
        o = w.update(in, sc);
        in.clipNew = false;
        check(!o.hold && !o.run, "the recoil plays");
        in.clipT = 0.46f;
        o = w.update(in, sc);
        check(o.hold && std::fabs(o.t - 0.45f) < 0.01f, "one-handed: it waits after the recoil, before the pump", o.t, 0.45);
        for (int i = 0; i < 5; ++i) o = w.update(in, sc);
        check(o.hold && !o.run, "and never pumps itself");
        in.held = true;
        o = w.update(in, sc);
        in.alongCm = 36.0f;
        o = w.update(in, sc);
        check(o.chase && o.t > 0.75f, "a stroke begun: played on from the wait through the pump (its sound with it)", o.t, 0.79);
    }
    // ---- the stroke along the barrel (2026-10-21, step 5) ----
    {
        const float dir[3] = {0, 0, -1}; // the gun controller's aim forward (XR: -Z)
        const float gun[3] = {0.1f, 1.2f, -0.3f};
        float fr[3] = {0.1f, 1.2f, -0.7f}; // 40 cm ahead along the barrel
        check(std::fabs(along_barrel_cm(fr, gun, dir) - 40.0f) < 1e-4f, "40 cm ahead along the barrel", along_barrel_cm(fr, gun, dir), 40);
        fr[0] += 0.15f; // sideways 15 cm: no change along
        check(std::fabs(along_barrel_cm(fr, gun, dir) - 40.0f) < 1e-4f, "sideways motion is no stroke", along_barrel_cm(fr, gun, dir), 40);
        fr[1] -= 0.1f; // down 10 cm: no change along
        check(std::fabs(along_barrel_cm(fr, gun, dir) - 40.0f) < 1e-4f, "up or down is no stroke", along_barrel_cm(fr, gun, dir), 40);
        const float gun2[3] = {0.1f, 1.2f, -0.5f}; // the gun hand moved 20 cm forward with the gun...
        const float fr2[3] = {0.25f, 1.1f, -0.9f};  // ...and the front hand with it: still 40 along
        check(std::fabs(along_barrel_cm(fr2, gun2, dir) - 40.0f) < 1e-4f, "the gun hand moving with the gun is no stroke", along_barrel_cm(fr2, gun2, dir), 40);
        const float back[3] = {0.25f, 1.1f, -0.64f};
        check(std::fabs(along_barrel_cm(back, gun, dir) - 34.0f) < 1e-4f, "6 cm back along the barrel: the stroke", along_barrel_cm(back, gun, dir), 34);
        // And through the ratchet: a back stroke by along, not by span.
        Pump p;
        const PumpConfig c;
        PumpInput in;
        in.fireClip = in.clipNew = in.held = in.alongOk = true;
        in.clipT = 0.76f;
        in.alongCm = 40.0f;
        p.update(in, c);
        in.clipNew = false;
        in.alongCm = 33.9f;
        PumpOutput o = p.update(in, c);
        check(p.backDone() && std::fabs(o.t - 0.85f) < 0.01f, "6.1 cm back along the barrel: the back stroke", o.t, 0.85);
    }
    // ---- scale ----
    {
        Sim s;
        s.cls = L"Pistol";
        s.step(kPistolOpen[0].p, false);
        Output o = s.step(kPistolOpen[0].p, true);
        s.clip(L"Reload", 3.6f); // twice the recorded 1.80 s
        o = s.step(kPistolOpen[0].p, true);
        check(std::fabs(o.t - 2.0f * 0.189f) < 0.01f, "a clip twice as long holds at twice the time", o.t, 0.378);
    }

    printf("reload-rig: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
