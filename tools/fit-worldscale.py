#!/usr/bin/env python3
"""Measure worldScale (Unreal units per real metre) from a `vrscale` capture.

WHY THIS IS NOT CIRCULAR. Almost every way of checking worldScale feeds the
setting back into its own answer: ask the mod how far your hand moved and it
replies `metres * worldScale` by construction. Gravity does not have that
problem - it is the engine's own physics, measured in UU/s^2, and real gravity
is 9.81 m/s^2 whatever the mod thinks. So:

    UU per metre = |measured gravity| / 9.81

Unreal's default zone gravity of -950 UU/s^2 implies ~97 UU/m, which is the
same ballpark as the ~100 upstream measured by a different method (a known-
distance hand sweep read off an engine-owned coordinate) - see
docs/bioshock2/ENGINE_NOTES.md.

HOW TO CAPTURE
    tools\\game-cmd.ps1 "vrscale 20"
  then, in game, JUMP five or six times on flat ground, or drop off a ledge -
  a long fall is the best signal there is. Then:
    python tools\\fit-worldscale.py

METHOD. The capture logs the ENGINE's camera z per CalcView with a QPC
timestamp. This fits z(t) = z0 + v0*t + 0.5*a*t^2 by least squares over each
airborne arc - a whole-arc fit, NOT a double difference of position, because
differentiating frame-to-frame samples twice amplifies noise so badly that a
lift decelerating reads as gravity (it did, on the first attempt: 12 noisy
samples claimed 286 UU/m).

Arcs are only accepted when the fit is actually parabolic (low RMS residual),
which is what rejects lifts, stairs, slopes and view bob.
"""

import argparse
import os
import re
import statistics
import sys

SAMPLE = re.compile(r"\[vrscale\] t=([\d.]+) z=([-\d.]+)")

# An arc has to be airborne (moving vertically), long enough to fit, and clean.
MIN_SPEED_UU_S = 40.0   # below this the player is walking, bobbing or still
MIN_POINTS = 10
MAX_RMS_UU = 1.5        # a real ballistic arc fits far tighter than this
GRAVITY_MS2 = 9.81


def load(path):
    with open(path, errors="ignore") as f:
        pts = [(float(t), float(z)) for t, z in SAMPLE.findall(f.read())]
    if not pts:
        sys.exit(f"no [vrscale] samples in {path}\n"
                 "Arm one with:  tools\\game-cmd.ps1 \"vrscale 20\"")
    t0 = pts[0][0]
    return [(t - t0, z) for t, z in pts]


def fit_quadratic(pts):
    """Least-squares z = a*t^2 + b*t + c. Returns (a, b, c, rms)."""
    n = len(pts)
    sx = sx2 = sx3 = sx4 = sy = sxy = sx2y = 0.0
    for x, y in pts:
        x2 = x * x
        sx += x; sx2 += x2; sx3 += x2 * x; sx4 += x2 * x2
        sy += y; sxy += x * y; sx2y += x2 * y
    # 3x3 solve by Cramer's rule - no numpy dependency for one small system.
    m = [[sx4, sx3, sx2], [sx3, sx2, sx], [sx2, sx, float(n)]]
    v = [sx2y, sxy, sy]

    def det3(a):
        return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))

    d = det3(m)
    if abs(d) < 1e-9:
        return None
    coef = []
    for i in range(3):
        mi = [row[:] for row in m]
        for r in range(3):
            mi[r][i] = v[r]
        coef.append(det3(mi) / d)
    a, b, c = coef
    rms = (sum((y - (a * x * x + b * x + c)) ** 2 for x, y in pts) / n) ** 0.5
    return a, b, c, rms


BIN_S = 0.010   # resample step: 10 ms


def rebin(samples):
    """Average samples into fixed 10 ms bins before any differencing.

    Necessary because the capture rate is wildly variable: paced in VR it is
    ~90-190 Hz, but flat and unpaced the engine calls CalcView ~1750 times a
    second (median gap 0.57 ms, measured). Differencing adjacent samples at
    that spacing is dominated by the 0.001 UU log quantisation, which is how a
    first pass reported |dz/dt| peaks of 38000 UU/s. Binning first makes the
    velocity estimate independent of frame rate.
    """
    out, bucket, edge = [], [], None
    for t, z in samples:
        if edge is None:
            edge = t
        if t >= edge + BIN_S:
            if bucket:
                out.append((edge + BIN_S / 2.0, sum(bucket) / len(bucket)))
            # skip empty bins rather than interpolating across capture gaps
            while t >= edge + BIN_S:
                edge += BIN_S
            bucket = []
        bucket.append(z)
    if bucket:
        out.append((edge + BIN_S / 2.0, sum(bucket) / len(bucket)))
    return out


def arcs(samples):
    """Contiguous runs where the camera is genuinely moving vertically."""
    vel = []
    for (ta, za), (tb, zb) in zip(samples, samples[1:]):
        dt = tb - ta
        # a gap larger than a few bins means the capture paused - not motion
        vel.append(abs((zb - za) / dt) if 1e-6 < dt < 0.05 else 0.0)
    out, cur = [], []
    for i, v in enumerate(vel):
        if v > MIN_SPEED_UU_S:
            cur.append(i)
        else:
            if len(cur) >= MIN_POINTS:
                out.append((cur[0], cur[-1] + 2))
            cur = []
    if len(cur) >= MIN_POINTS:
        out.append((cur[0], cur[-1] + 2))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", default=os.path.join(
        os.environ.get("LOCALAPPDATA", ""), "BioshockVR", "bioshockvr.log"))
    ap.add_argument("--worldscale", type=float, default=100.0,
                    help="the value currently configured, for comparison")
    a = ap.parse_args()

    raw = load(a.log)
    s = rebin(raw)
    print(f"{len(raw)} samples over {raw[-1][0]:.1f}s "
          f"-> {len(s)} bins of {BIN_S*1000:.0f} ms, "
          f"z {min(z for _, z in raw):.1f}..{max(z for _, z in raw):.1f} UU")

    good = []
    for lo, hi in arcs(s):
        seg = s[lo:hi]
        if len(seg) < MIN_POINTS:
            continue
        mid = seg[len(seg) // 2][0]
        f = fit_quadratic([(t - mid, z) for t, z in seg])
        if not f:
            continue
        accel = 2.0 * f[0]
        if accel < -100.0 and f[3] <= MAX_RMS_UU:
            good.append((accel, len(seg), f[3], seg[0][0]))

    if not good:
        print("\nNo clean ballistic arcs found.")
        print("The capture needs the player actually airborne - jump on flat")
        print("ground, or drop off a ledge. Lifts, stairs and slopes are")
        print("rejected on purpose: they are not parabolic.")
        return

    print(f"\n{len(good)} clean arc(s):")
    for accel, n, rms, t in sorted(good, key=lambda g: g[3]):
        print(f"  t={t:6.2f}s  n={n:3d}  rms={rms:4.2f} UU  "
              f"accel={accel:9.1f} UU/s^2  -> {abs(accel)/GRAVITY_MS2:6.1f} UU/m")

    med = statistics.median([g[0] for g in good])
    uu_per_m = abs(med) / GRAVITY_MS2
    print(f"\nmedian gravity : {med:.1f} UU/s^2")
    print(f"UU per metre   : {uu_per_m:.1f}")
    print(f"worldScale set : {a.worldscale:.0f}")
    ratio = uu_per_m / a.worldscale if a.worldscale else 0.0
    if 0.9 <= ratio <= 1.1:
        print(f"VERDICT: consistent (within {abs(1-ratio)*100:.0f}%) - leave it alone.")
    else:
        print(f"VERDICT: MISMATCH by {ratio:.2f}x - worldScale would want ~{uu_per_m:.0f}.")
        print("Before acting on this, sanity-check it against the game's own")
        print("gravity if you can read it: a game tuned to non-real gravity")
        print("breaks the 9.81 assumption this whole method rests on.")


if __name__ == "__main__":
    main()
