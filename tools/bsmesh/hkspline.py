"""hkspline.py - decompress hkaSplineCompressedAnimation (Havok 2012) into per-frame
local bone transforms.

The member layout comes from the exe (hkclass_dump.py); this file is the meaning of
the `data` blob. Per block (blockOffsets[b] into data), measured on BS1's hands:

  masks      numberOfTransformTracks x 4 bytes, then the float-track masks; the
             region is maskAndQuantizationSize bytes
             [0] quantization: bits 0-1 position (0 = 8-bit, 1 = 16-bit), bits 2-5
                 rotation (0 POLAR32, 1 THREECOMP40, 2 THREECOMP48, 3 THREECOMP24,
                 4 STRAIGHT16, 5 UNCOMPRESSED), bits 6-7 scale (as position)
             [1] position sub-tracks: bit i (0-2) = component i STATIC,
                 bit i+4 = component i SPLINE, neither = identity (0)
             [2] rotation: any of bits 4-7 = SPLINE, else any of bits 0-3 = STATIC,
                 else identity
             [3] scale: as position, identity 1
  then, per track: position, rotation, scale, each followed by padding to 4.
  A vector sub-track with any SPLINE component:
      u16 numItems, u8 degree, (numItems + degree + 2) knot bytes, pad to 4,
      per component in x,y,z order: SPLINE -> f32 min, f32 max; STATIC -> f32 value,
      then numItems + 1 control points, each the SPLINE components quantised
      (8/16-bit, min + (max - min) * q / qmax)
  without one: the STATIC components' f32 values.
  A rotation sub-track: SPLINE -> u16 numItems, u8 degree, knots, pad to the
  quantisation's alignment, numItems + 1 quaternions; STATIC -> one quaternion.

Evaluation is a standard B-spline (Cox-de Boor) at the block-local frame; quaternion
control points are blended component-wise and renormalised.

How the decoder was checked (rendered poses, and left/right measured numerically) is
docs/bioshock1/HAVOK_AND_PACKAGES.md, "Validation".
"""
import math
import struct

ROT_SIZE = {0: 4, 1: 5, 2: 6, 3: 3, 4: 2, 5: 16}
ROT_ALIGN = {0: 4, 1: 1, 2: 2, 3: 1, 4: 2, 5: 4}


def _align(o, a):
    return (o + a - 1) & ~(a - 1)


def _rot_3comp40(b):
    v = int.from_bytes(b[:5], "little")
    mask = (1 << 12) - 1
    frac = 1.0 / (2047.0 * math.sqrt(2.0))
    c = [((v >> s) & mask) - 2047 for s in (0, 12, 24)]
    c = [x * frac for x in c]
    shift = (v >> 36) & 3
    invert = (v >> 38) & 1
    w2 = 1.0 - (c[0] * c[0] + c[1] * c[1] + c[2] * c[2])
    w = math.sqrt(w2) if w2 > 0 else 0.0
    if invert:
        w = -w
    c.insert(shift, w)
    return tuple(c)        # x, y, z, w


def _rot_3comp48(b):
    x, y, z = struct.unpack("<hhh", b[:6])
    shift = ((y >> 14) & 2) | ((x >> 15) & 1)
    invert = (z >> 15) & 1
    mask = 0x7FFF
    frac = 1.0 / (16383.0 * math.sqrt(2.0))
    c = [((t & mask) - 16383) * frac for t in (x, y, z)]
    w2 = 1.0 - sum(t * t for t in c)
    w = math.sqrt(w2) if w2 > 0 else 0.0
    if invert:
        w = -w
    c.insert(shift, w)
    return tuple(c)


def _rot(qtype, b):
    if qtype == 1:
        return _rot_3comp40(b)
    if qtype == 2:
        return _rot_3comp48(b)
    if qtype == 5:
        return struct.unpack("<4f", b[:16])
    raise NotImplementedError("rotation quantisation %d not implemented (never seen on BS1)" % qtype)


def _find_span(n, p, u, U):
    if u >= U[n + 1]:
        return n
    lo, hi = p, n + 1
    mid = (lo + hi) // 2
    while u < U[mid] or u >= U[mid + 1]:
        if u < U[mid]:
            hi = mid
        else:
            lo = mid
        mid = (lo + hi) // 2
    return mid


def _basis(span, u, p, U):
    N = [1.0] + [0.0] * p
    left = [0.0] * (p + 1)
    right = [0.0] * (p + 1)
    for j in range(1, p + 1):
        left[j] = u - U[span + 1 - j]
        right[j] = U[span + j] - u
        saved = 0.0
        for r in range(j):
            den = right[r + 1] + left[j - r]
            temp = N[r] / den if den else 0.0
            N[r] = saved + right[r + 1] * temp
            saved = left[j - r] * temp
        N[j] = saved
    return N


class Spline:
    def __init__(self, degree, knots, points):
        self.p, self.U, self.P = degree, list(knots), points

    def eval(self, u):
        n = len(self.P) - 1
        span = _find_span(n, self.p, u, self.U)
        N = _basis(span, u, self.p, self.U)
        dim = len(self.P[0])
        out = [0.0] * dim
        for i in range(self.p + 1):
            cp = self.P[span - self.p + i]
            for k in range(dim):
                out[k] += N[i] * cp[k]
        return out


def _read_vec(d, o, qtype, flags, identity):
    """Returns (callable(frame) -> 3-tuple, new offset)."""
    static = [(flags >> i) & 1 for i in range(3)]
    spline = [(flags >> (i + 4)) & 1 for i in range(3)]
    if any(spline):
        num_items, degree = struct.unpack_from("<HB", d, o); o += 3
        knots = d[o:o + num_items + degree + 2]; o += num_items + degree + 2
        o = _align(o, 4)
        ranges = [None] * 3
        values = [identity] * 3
        for i in range(3):
            if spline[i]:
                ranges[i] = struct.unpack_from("<ff", d, o); o += 8
            elif static[i]:
                values[i] = struct.unpack_from("<f", d, o)[0]; o += 4
        qmax = 255.0 if qtype == 0 else 65535.0
        qsize = 1 if qtype == 0 else 2
        pts = []
        for _ in range(num_items + 1):
            cp = []
            for i in range(3):
                if spline[i]:
                    q = d[o] if qsize == 1 else struct.unpack_from("<H", d, o)[0]
                    o += qsize
                    lo, hi = ranges[i]
                    cp.append(lo + (hi - lo) * q / qmax)
            pts.append(cp)
        o = _align(o, 4)
        sp = Spline(degree, knots, pts)
        idx = [i for i in range(3) if spline[i]]

        def f(u, sp=sp, idx=idx, values=values):
            v = list(values)
            r = sp.eval(u)
            for k, i in enumerate(idx):
                v[i] = r[k]
            return tuple(v)
        return f, o
    values = [identity] * 3
    for i in range(3):
        if static[i]:
            values[i] = struct.unpack_from("<f", d, o)[0]; o += 4
    o = _align(o, 4)
    vt = tuple(values)
    return (lambda u, vt=vt: vt), o


def _read_rot(d, o, qtype, flags):
    if flags & 0xF0:
        num_items, degree = struct.unpack_from("<HB", d, o); o += 3
        knots = d[o:o + num_items + degree + 2]; o += num_items + degree + 2
        o = _align(o, ROT_ALIGN[qtype])
        pts = []
        for _ in range(num_items + 1):
            pts.append(list(_rot(qtype, d[o:o + ROT_SIZE[qtype]]))); o += ROT_SIZE[qtype]
        # Keep neighbouring control points in one hemisphere so the blend is short.
        for i in range(1, len(pts)):
            if sum(a * b for a, b in zip(pts[i - 1], pts[i])) < 0:
                pts[i] = [-x for x in pts[i]]
        o = _align(o, 4)
        sp = Spline(degree, knots, pts)

        def f(u, sp=sp):
            q = sp.eval(u)
            n = math.sqrt(sum(x * x for x in q)) or 1.0
            return tuple(x / n for x in q)
        return f, o
    if flags & 0x0F:
        o = _align(o, ROT_ALIGN[qtype])
        q = _rot(qtype, d[o:o + ROT_SIZE[qtype]]); o += ROT_SIZE[qtype]
        o = _align(o, 4)
        return (lambda u, q=q: q), o
    o = _align(o, 4)
    return (lambda u: (0.0, 0.0, 0.0, 1.0)), o


def decode(anim):
    """anim: an hkread-decoded hkaSplineCompressedAnimation. Returns a list of frames,
    each a list (one per transform track) of (t, q, s) local transforms."""
    d = anim["data"]
    ntracks = anim["numberOfTransformTracks"]
    nframes = anim["numFrames"]
    fpb = anim["maxFramesPerBlock"]
    if anim.get("endian", 0) != 0:
        raise NotImplementedError("big-endian spline data")
    frames = [[None] * ntracks for _ in range(nframes)]
    for b, boff in enumerate(anim["blockOffsets"]):
        masks = d[boff:boff + 4 * ntracks]
        o = boff + anim["maskAndQuantizationSize"]
        first = b * (fpb - 1)
        last = min(nframes, first + fpb)
        for t in range(ntracks):
            qt, pf, rf, sf = masks[4 * t:4 * t + 4]
            pos, o = _read_vec(d, o, qt & 3, pf, 0.0)
            rot, o = _read_rot(d, o, (qt >> 2) & 15, rf)
            scl, o = _read_vec(d, o, (qt >> 6) & 3, sf, 1.0)
            for fr in range(first, last):
                u = float(fr - first)
                frames[fr][t] = (pos(u), rot(u), scl(u))
    return frames
