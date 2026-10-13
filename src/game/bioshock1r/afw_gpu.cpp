// See afw_gpu.h.
#include "game/bioshock1r/afw_gpu.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace bvr::b1r::afw {
namespace {

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
void put(char* why, size_t cap, const char* fmt, ...) {
    if (!why || !cap) return;
    va_list a; va_start(a, fmt);
    _vsnprintf_s(why, cap, _TRUNCATE, fmt, a);
    va_end(a);
}

const char* kSrc = R"(
cbuffer W : register(b0) {
    float4 srcLocSky;       // xyz source camera, w sky distance (uu)
    float4 srcFwdTanH;
    float4 srcRightTanV;
    float4 srcUpGrid;       // w mesh cell (texels)
    float4 tgtLocFar;       // w z-buffer far (uu)
    float4 tgtFwdTanH;
    float4 tgtRightTanV;
    float4 tgtUpEdgeRel;    // w discontinuity ratio
    float4 othLocStaleRel;  // the other source, for the stale test; w stale ratio
    float4 othFwdTanH;
    float4 othRightTanV;
    float4 othUpStaleAbs;   // w stale absolute (uu)
    float4 dims;            // source depth w, h, cells x, cells y
    float4 othDims;         // other depth w, h, stale test on, discontinuity absolute (uu)
    float4 compose;         // nearer ratio, tint, use A, use B
    float4 outDims;         // w, h
    float4 fg;              // hand depth gain, band (uu): nearer depth is read as depth / gain;
                            // z: the first vertex of this draw band (SV_VertexID restarts at 0);
                            // w: the hands' tangent gain (their lens over the world's)
    float4 maskFlags;       // x: this source's hands mask bound; y: this source is A (its hands
                            // are dropped); z: the other source's mask bound; w: hands from the
                            // fresh eye (compose)
    float4 refineFlags;     // x: refine; y: stale test; z/w: A/B mask present (compose)
    float4 fgSrcLoc;
    float4 fgSrcFwd;
    float4 fgSrcRight;
    float4 fgSrcUp;
};

Texture2D<float> srcZ : register(t0);
Texture2D<float4> srcC : register(t1);
Texture2D<float> othZ : register(t2);
Texture2D<float> srcM : register(t3);
Texture2D<float> othM : register(t4);
SamplerState lin : register(s0);
SamplerState pnt : register(s1);

struct V {
    float4 pos : SV_Position;
    float2 suv : TEXCOORD0;
    float3 world : TEXCOORD1;
    float tz : TEXCOORD2;
    float stretched : TEXCOORD3;   // 1: this cell spans a depth discontinuity
    float isfg : TEXCOORD4;        // 1: a hand texel (its own lens)
};

// Linear depth as the reprojection uses it: sky (0) far away, and the hand band corrected
// by the measured gain.
bool masked(float value,float flag) {return flag>0.5f && (flag>1.5f ? value<0.5f : value>0.5f);}
float zfix(float z) { return z > 0.0f ? (z < fg.y ? z / fg.x : z) : srcLocSky.w; }
float zat(int2 p) {
    p = clamp(p, int2(0, 0), int2(dims.xy) - 1);
    return zfix(srcZ.Load(int3(p, 0)));
}
bool fgat(int2 p) {
    p = clamp(p, int2(0, 0), int2(dims.xy) - 1);
    return masked(srcM.Load(int3(p, 0)),maskFlags.x);
}

V vsmesh(uint vid0 : SV_VertexID) {
    uint vid = vid0 + (uint)fg.z;
    uint cell = vid / 6;
    uint k = vid - cell * 6;
    // The cell's corner for vertex k of its two triangles, (0,0) (1,0) (0,1) / (1,0) (1,1)
    // (0,1), from bit masks rather than an indexed constant array.
    const int2 corner = int2((0x1Au >> k) & 1u, (0x34u >> k) & 1u);
    uint cw = (uint)dims.z;
    int2 c = int2(cell % cw, cell / cw);
    int g = (int)srcUpGrid.w;
    int2 p0 = c * g;
    V o;
    o.pos = float4(0, 0, -1, 1);   // culled (z < 0) unless set below
    o.suv = float2(0, 0); o.world = float3(0, 0, 0); o.tz = 0; o.stretched = 0; o.isfg = 0;
    float z00 = zat(p0), z10 = zat(p0 + int2(g, 0)), z01 = zat(p0 + int2(0, g)), z11 = zat(p0 + int2(g, g));
    float zmin = min(min(z00, z10), min(z01, z11)), zmax = max(max(z00, z10), max(z01, z11));
    // A cell across a depth discontinuity is drawn, but only as a last resort: at the far
    // end of the z-buffer (it never hides a real surface of this source) and flagged, so
    // the other source's real surface wins over it in the compose. Dropping it instead
    // leaves a hole along every silhouette that only a neighbour search can fill.
    const bool edge = zmax > zmin * (1.0f + tgtUpEdgeRel.w) + othDims.w;
    int2 node = min(p0 + corner * g, int2(dims.xy) - 1);
    float z = zat(node);
    // A hand texel sits on the hands' own lens: the world's tangents times the gain, in the
    // source and in the target alike (the native frame draws the hands with that lens too).
    const bool fgv = fgat(node);
    const float lens = fgv ? fg.w : 1.0f;
    float2 uv = (float2(node) + 0.5f) / dims.xy;
    float x = uv.x * 2.0f - 1.0f, y = 1.0f - uv.y * 2.0f;
    const float3 sf = fgv ? fgSrcFwd.xyz : srcFwdTanH.xyz;
    const float3 sr = fgv ? fgSrcRight.xyz : srcRightTanV.xyz;
    const float3 su = fgv ? fgSrcUp.xyz : srcUpGrid.xyz;
    const float3 sl = fgv ? fgSrcLoc.xyz : srcLocSky.xyz;
    float3 d = sf + sr * (x * srcFwdTanH.w * lens) + su * (y * srcRightTanV.w * lens);
    float3 w = sl + d * z;
    float3 q = w - tgtLocFar.xyz;
    float tz = dot(q, tgtFwdTanH.xyz);
    if (tz < 0.5f) return o;
    // The z-buffer: the hands in its near half, the world in its far half - the foreground
    // wins as the game draws it (the weapon on top even when pushed into a wall; Dishonored's
    // guide 9.8) - and a discontinuity cell at the very end.
    const float zn = tz / tgtLocFar.w;
    const float zb = edge ? 0.9999f : (fgv ? 0.5f * zn : 0.5f + 0.5f * zn);
    o.pos = float4(dot(q, tgtRightTanV.xyz) / (tgtFwdTanH.w * lens), dot(q, tgtUpEdgeRel.xyz) / (tgtRightTanV.w * lens),
                   zb * tz, tz);
    o.suv = uv;
    o.world = d * z;   // relative to the source camera: precision, and the PS reprojects it

    o.tz = tz;
    o.stretched = edge ? 1.0f : 0.0f;
    o.isfg = fgv ? 1.0f : 0.0f;
    return o;
}

struct PO { float4 c : SV_Target0; float2 z : SV_Target1; };

PO psmesh(V i) {
    // The source texel this pixel shows: the interpolated surface point projected back
    // into the source camera. Interpolating the source UV itself is wrong - the
    // rasterizer's perspective-correct interpolation is linear in WORLD space, the UV is
    // not - and across a depth step that skews the colour onto the wrong side of the edge.
    float2 suv = i.suv;
    const float lens = i.isfg > 0.5f ? fg.w : 1.0f;   // the cell's lens (a mixed cell is an edge cell)
    const bool fgCell = i.isfg > 0.5f;
    const float3 sf = fgCell ? fgSrcFwd.xyz : srcFwdTanH.xyz;
    const float3 sr = fgCell ? fgSrcRight.xyz : srcRightTanV.xyz;
    const float3 su = fgCell ? fgSrcUp.xyz : srcUpGrid.xyz;
    float sz = dot(i.world, sf);
    if (sz > 1e-3f)
        suv = float2(dot(i.world, sr) / (sz * srcFwdTanH.w * lens) * 0.5f + 0.5f,
                     0.5f - dot(i.world, su) / (sz * srcRightTanV.w * lens) * 0.5f);
    // The mask is never filtered: the texel this pixel shows is hand or world.
    const bool isFg = masked(srcM.SampleLevel(pnt, suv, 0),maskFlags.x);
    const bool dropFg = isFg && maskFlags.y > 0.5f;   // source A's hands
    if(refineFlags.x>0.5f) {
        PO seed;
        seed.c=float4(suv,0,1);
        seed.z=float2(i.stretched>0.5f ? -i.tz : i.tz,isFg?1.0f:0.0f);
        return seed; // visibility tests belong to the final refined point
    }
    float3 world = (isFg ? fgSrcLoc.xyz : srcLocSky.xyz) + i.world;
    if (othDims.z > 0.5f || dropFg) {
        // This point in the other (fresh) eye's frame.
        float3 q = world - othLocStaleRel.xyz;
        float oz = dot(q, othFwdTanH.xyz);
        float2 ouv = float2(0, 0);
        bool inside = oz > 0.5f;
        if (inside) {
            // A hand point is looked up where the other eye DRAWS its hands: its lens.
            ouv = float2(dot(q, othRightTanV.xyz) / (oz * othFwdTanH.w * lens) * 0.5f + 0.5f,
                         0.5f - dot(q, othUpStaleAbs.xyz) / (oz * othRightTanV.w * lens) * 0.5f);
            inside = all(ouv > 0.002f) && all(ouv < 0.998f);
        }
        if (dropFg) {
            // A's hands are a frame old and move with the camera and the controllers: carried
            // as world geometry they land where a world object would. The fresh eye shows them
            // where they are now, so they are dropped wherever the fresh eye SEES the point (its
            // depth there matches) or sees past it (the hand moved away). They are kept where
            // the fresh frame does not contain the point (the arm at the frame edge) and where
            // the fresh eye sees that point HIDDEN behind something nearer - the far side of
            // the arm, the thumb behind the palm from the other eye (Dishonored's guide 9.7):
            // the fresh eye has no pixel for it, and the held eye's own beats a fill (the
            // seventh run's capture: tears along every sleeve). In the compose the fresh
            // eye's real pixels still win wherever it has them.
            if (inside) {
                float4 g4 = othZ.GatherRed(pnt, ouv);
                g4 = g4 > 0.0f ? (g4 < fg.y ? g4 / fg.x : g4) : srcLocSky.w;
                const float zo = min(min(g4.x, g4.y), min(g4.z, g4.w));
                // Seen = the same surface at the same depth (sub-uu agreement); hidden = the
                // fresh eye's surface there is nearer by more than depth noise. A sleeve is a
                // few uu thick, so this tolerance is far tighter than the stale test's.
                const bool hidden = zo < oz * 0.99f - 0.25f;
                if (!hidden) discard;
            }
        } else if (othDims.z > 0.5f && inside) {
            // The stale test: does the other (fresh) eye see THROUGH this point now? A
            // foreground fresh sample never condemns it: the game draws the weapon on top even
            // when it is geometrically behind a wall, so a farther hand there is occlusion,
            // not evidence that the wall moved.
            float4 g4 = othZ.GatherRed(pnt, ouv);
            g4 = g4 > 0.0f ? (g4 < fg.y ? g4 / fg.x : g4) : srcLocSky.w;
            // The NEAREST of the four: a silhouette texel never condemns a point.
            float zo = min(min(g4.x, g4.y), min(g4.z, g4.w));
            const bool othFg = masked(othM.SampleLevel(pnt, ouv, 0),maskFlags.z);
            if (!othFg && zo > oz * (1.0f + othLocStaleRel.w) + othUpStaleAbs.w) discard;
        }
    }
    PO o;
    o.c = srcC.SampleLevel(lin, suv, 0);
    o.c.a = 1.0f;
    // x: negative = a stretched (discontinuity) pixel: valid, but the last choice. y: the hands.
    o.z = float2(i.stretched > 0.5f ? -i.tz : i.tz, isFg ? 1.0f : 0.0f);
    return o;
}

Texture2D<float4> colA : register(t0);
Texture2D<float2> lzA : register(t1);
Texture2D<float4> colB : register(t2);
Texture2D<float2> lzB : register(t3);
Texture2D<float4> rawHeld : register(t4);   // the held eye's own last image, unwarped: the last resort

)"
R"(
// The mesh stores only a source-UV seed in refine mode. These are the original
// colour/depth/mask surfaces, never the mesh's filtered/stretched colour.
Texture2D<float> depthA : register(t5);
Texture2D<float> depthB : register(t6);
Texture2D<float4> imageA : register(t7);
Texture2D<float4> imageB : register(t8);
Texture2D<float> maskA : register(t9);
Texture2D<float> maskB : register(t10);
Texture2D<float4> nativeForeground : register(t11);
Texture2D<float> foregroundTransmittance : register(t12);

float2 source_dims(bool a) { return a ? dims.xy : othDims.xy; }
float source_z(bool a,float2 s) {
    return zfix(a ? depthA.SampleLevel(pnt,s,0) : depthB.SampleLevel(pnt,s,0));
}
bool source_hand(bool a,float2 s) {
    return masked(a ? maskA.SampleLevel(pnt,s,0) : maskB.SampleLevel(pnt,s,0),a?refineFlags.z:refineFlags.w);
}
float4 source_color(bool a,float2 s) {
    if(outDims.z<0.5f) return a ? imageA.SampleLevel(lin,s,0) : imageB.SampleLevel(lin,s,0);
    // A source-world colour must not filter in its old hand silhouette. The
    // native foreground has its own target-eye geometry below.
    const float2 size=source_dims(a), q=s*size-0.5f;
    const int2 base=int2(floor(q)), hi=int2(size)-1;
    const float2 f=frac(q); float4 sum=0; float weight=0;
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
        int2 p=clamp(base+int2(x,y),int2(0,0),hi);
        float mask=a?maskA.Load(int3(p,0)):maskB.Load(int3(p,0));
        float w=(x?f.x:1-f.x)*(y?f.y:1-f.y)*(!masked(mask,a?refineFlags.z:refineFlags.w)?1:0);
        sum+=(a?imageA.Load(int3(p,0)):imageB.Load(int3(p,0)))*w;weight+=w;
    }
    return weight>1e-6f?sum/weight:float4(0,0,0,1);
}
struct Candidate {
    float2 uv;
    float2 mapped;
    float3 world;
    float z;
    float err;
    bool hand;
};
Candidate invalid_candidate() {
    Candidate c;
    c.uv=c.mapped=float2(0,0);c.world=float3(0,0,0);c.z=0;c.err=1e9;c.hand=false;
    return c;
}
Candidate map_source(bool a,float2 s,float2 target) {
    Candidate c=invalid_candidate();c.uv=s;
    float4 sl=a ? srcLocSky : othLocStaleRel;
    float4 sf=a ? srcFwdTanH : othFwdTanH;
    float4 sr=a ? srcRightTanV : othRightTanV;
    float4 su=a ? srcUpGrid : othUpStaleAbs;
    c.hand=source_hand(a,s);
    if(a && c.hand) {
        sl.xyz=fgSrcLoc.xyz;sf.xyz=fgSrcFwd.xyz;
        sr.xyz=fgSrcRight.xyz;su.xyz=fgSrcUp.xyz;
    }
    const float lens=c.hand ? fg.w : 1.0f;
    const float2 ndc=s*float2(2,-2)+float2(-1,1);
    const float3 ray=sf.xyz+sr.xyz*(ndc.x*sf.w*lens)+su.xyz*(ndc.y*sr.w*lens);
    c.world=sl.xyz+ray*source_z(a,s);
    const float3 q=c.world-tgtLocFar.xyz;
    c.z=dot(q,tgtFwdTanH.xyz);
    if(c.z>0.5f && all(s>=0) && all(s<=1)) {
        c.mapped=float2(dot(q,tgtRightTanV.xyz)/(c.z*tgtFwdTanH.w*lens)*0.5f+0.5f,
                       0.5f-dot(q,tgtUpEdgeRel.xyz)/(c.z*tgtRightTanV.w*lens)*0.5f);
        // In depth texels: upscaling colour does not loosen the geometry test.
        c.err=length((c.mapped-target)*source_dims(a));
    }
    return c;
}
Candidate refine_source(bool a,float2 target,float2 seed) {
    float2 s=seed;
    [unroll] for(int i=0;i<3;++i) {
        Candidate c=map_source(a,s,target);
        s=saturate(s+(target-c.mapped));
    }
    Candidate result=map_source(a,s,target);
    // A converged point must still occupy the depth footprint reached by its
    // residual correction. Otherwise a small miss across a discontinuity
    // widens a one-texel rail into two pixels instead of refining its edge.
    const float z0=source_z(a,s);
    const float z1=source_z(a,saturate(s+(target-result.mapped)));
    if(abs(z1-z0)>min(z0,z1)*tgtUpEdgeRel.w+othDims.w) result.err=1e9;
    return result;
}
// Inspect the raw depth beside a seed as well as the coarse seed itself.
// A one-texel rail can fall between grid vertices; these adjacent depth texels
// provide a near-side retry even when the coarse mesh saw only sky.
Candidate refine_seed(bool a,float2 target,float2 seed,float signedSeedZ) {
    Candidate best=refine_source(a,target,seed);
    if(a && fgSrcLoc.w>0.5f && refineFlags.z>0.5f) {
        // Locomotion moves masked foreground independently of A's world. A
        // world triangle can therefore cover a foreground edge's mesh seed.
        // Try a rotation-only foreground hint in its own transported camera;
        // the depth solve and fresh-eye visibility test still decide validity.
        const float2 ndc=target*float2(2,-2)+float2(-1,1);
        const float3 ray=tgtFwdTanH.xyz+tgtRightTanV.xyz*(ndc.x*tgtFwdTanH.w*fg.w)+
                         tgtUpEdgeRel.xyz*(ndc.y*tgtRightTanV.w*fg.w);
        const float z=dot(ray,fgSrcFwd.xyz);
        const float2 hint=float2(dot(ray,fgSrcRight.xyz)/(z*srcFwdTanH.w*fg.w)*0.5f+0.5f,
                                0.5f-dot(ray,fgSrcUp.xyz)/(z*srcRightTanV.w*fg.w)*0.5f);
        if(z>0.5f && all(hint>=0) && all(hint<=1) && source_hand(true,hint)) {
            Candidate h=refine_source(true,target,hint);
            if(h.hand && h.err<=1.5f && (best.err>1.5f || !best.hand || h.z<best.z)) best=h;
        }
    }
    const float2 texel=1.0f/source_dims(a);
    const float2 D[4]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1)};
    float2 nearUv=seed;
    float nearZ=source_z(a,seed);
    bool nearHand=source_hand(a,seed);
    [unroll] for(int k=0;k<4;++k) {
        const float2 uv=saturate(seed+D[k]*texel);
        const float z=source_z(a,uv);
        const bool hand=source_hand(a,uv);
        if((hand && !nearHand) || (hand==nearHand && z<nearZ)) {
            nearUv=uv;nearZ=z;nearHand=hand;
        }
    }
    const bool rescue=signedSeedZ<0 ||
        best.z>abs(signedSeedZ)*(1.0f+tgtUpEdgeRel.w)+othDims.w ||
        nearZ<source_z(a,seed)*(1.0f-tgtUpEdgeRel.w)-othDims.w;
    if(rescue) {
        Candidate n=refine_source(a,target,nearUv);
        if(n.err<=1.5f && (best.err>1.5f || (n.hand && !best.hand) ||
           (n.hand==best.hand && n.z<best.z))) best=n;
    }
    return best;
}
// Run visibility at the refined sample, not at an interpolated mesh point.
bool reject_held(Candidate c,bool haveFresh) {
    if(!haveFresh || c.err>=6.0f) return false;
    const float lens=c.hand ? fg.w : 1.0f;
    const float3 q=c.world-othLocStaleRel.xyz;
    const float z=dot(q,othFwdTanH.xyz);
    if(z<=0.5f) return false;
    const float2 uv=float2(dot(q,othRightTanV.xyz)/(z*othFwdTanH.w*lens)*0.5f+0.5f,
                         0.5f-dot(q,othUpStaleAbs.xyz)/(z*othRightTanV.w*lens)*0.5f);
    if(any(uv<=0.002f) || any(uv>=0.998f)) return false;
    float4 d=depthB.GatherRed(pnt,uv);
    d=d>0.0f ? (d<fg.y ? d/fg.x : d) : srcLocSky.w;
    const float nearest=min(min(d.x,d.y),min(d.z,d.w));
    if(c.hand) {
        // The fresh eye may see the same foreground surface yet fail to solve
        // its silhouette from the other side. Keep A while depth and mask
        // agree; a valid fresh foreground candidate still wins the compose.
        // Seeing world past it (a moved hand) continues to discard the history.
        // Pair each gathered depth with its own mask texel. Checking only
        // the point-sampled mask discards a valid edge when that point falls
        // just outside the silhouette but a gathered neighbour confirms it.
        const float4 rawMask=maskB.GatherRed(pnt,uv);
        const float4 handMask=refineFlags.w>1.5f?1-rawMask:rawMask;
        const bool sameHand=refineFlags.w>0.5f &&
            any((handMask>0.5f) && (abs(d-z)<=z*0.01f+0.25f));
        return !sameHand && !(nearest<z*0.99f-0.25f);
    }
    return refineFlags.y>0.5f && !source_hand(false,uv) &&
        nearest>z*(1.0f+othLocStaleRel.w)+othUpStaleAbs.w;
}
float4 refined_compose(float4 pos) {
    const float2 target=pos.xy/outDims.xy;
    const int2 seedHi=int2(dims.zw)-1;
    const int2 sp=min(int2(target*dims.zw),seedHi);
    const bool useA=compose.z>0.5f,useB=compose.w>0.5f;
    const float2 za=useA ? lzA.Load(int3(sp,0)) : float2(0,0);
    const float2 zb=useB ? lzB.Load(int3(sp,0)) : float2(0,0);
    Candidate a=invalid_candidate(),b=invalid_candidate();
    if(useA) a=refine_seed(true,target,za.x!=0 ? colA.Load(int3(sp,0)).xy : target,za.x);
    if(useB) b=refine_seed(false,target,zb.x!=0 ? colB.Load(int3(sp,0)).xy : target,zb.x);
    const bool stale=useA && reject_held(a,useB);
    const bool worldOnly=outDims.z>0.5f;
    const bool goodA=a.err<=1.5f && !stale && (!worldOnly || !a.hand);
    const bool goodB=b.err<=1.5f && (!worldOnly || !b.hand);
    bool chooseA=false;float2 uv=target;int cls=-1;
    if(goodB && b.hand) {uv=b.uv;cls=4;}
    else if(goodA && goodB) {
        chooseA=!(b.z<a.z*(1.0f-compose.x));
        uv=chooseA?a.uv:b.uv;cls=chooseA?0:1;
    } else if(goodA) {chooseA=true;uv=a.uv;cls=0;}
    // A near-miss at a silhouette can still name the correct near world
    // surface while the other eye sees the far side through parallax.
    else if(!stale && !a.hand && a.err<6.0f && goodB && b.z>a.z*(1.0f+compose.x)) {
        chooseA=true;uv=a.uv;cls=8;
    } else if(goodB) {uv=b.uv;cls=1;}
    else {
        const float ea=!stale && !a.hand?a.err:1e9;
        const float eb=!b.hand?b.err:1e9;
        if(min(ea,eb)<6.0f) {chooseA=ea<=eb;uv=chooseA?a.uv:b.uv;cls=8;}
    }
    float4 color;
    if(cls>=0) color=source_color(chooseA,uv);
    else {
        // Disocclusion fill extends the farthest world seed; never a hand.
        float farthest=0;cls=2;
        const int2 D[8]={int2(1,0),int2(-1,0),int2(0,1),int2(0,-1),
                        int2(1,1),int2(-1,1),int2(1,-1),int2(-1,-1)};
        [loop] for(int r=1;r<=32 && farthest==0;r*=2) {
            [unroll] for(int k=0;k<8;++k) {
                const int2 q=clamp(sp+D[k]*r,int2(0,0),seedHi);
                const float2 az=useA?lzA.Load(int3(q,0)):float2(0,0);
                const float2 bz=useB?lzB.Load(int3(q,0)):float2(0,0);
                if(az.y<0.5f && abs(az.x)>farthest) {farthest=abs(az.x);uv=colA.Load(int3(q,0)).xy;chooseA=true;}
                if(bz.y<0.5f && abs(bz.x)>farthest) {farthest=abs(bz.x);uv=colB.Load(int3(q,0)).xy;chooseA=false;}
            }
        }
        if(farthest>0) color=source_color(chooseA,uv);
        else {
            if(!worldOnly) color=rawHeld.SampleLevel(lin,target,0);
            else if(useA && !source_hand(true,target)) color=source_color(true,target);
            else if(useB && !source_hand(false,target)) color=source_color(false,target);
            else color=float4(0,0,0,1);
            cls=3;
        }
    }
    if(compose.y>0.5f) {
        if(cls==0 && a.hand) cls=5;
        color.rgb*=cls==1?float3(0.6,0.7,1.4):cls==4?float3(0.6,1.4,0.6):cls==5?float3(1.4,1.4,0.5):
                   cls==2?float3(1.4,0.5,1.4):cls==3?float3(1.5,0.4,0.4):cls==8?float3(0.5,1.3,1.3):float3(1,1,1);
    }
    color.a=1;return color;
}

float4 vsfull(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 pscompose(float4 pos : SV_Position) : SV_Target {
    if(refineFlags.x>0.5f) {
        float4 c=refined_compose(pos);
        if(outDims.z>0.5f) {
            const int3 p=int3(int2(pos.xy),0);
            c.rgb=nativeForeground.Load(p).rgb+c.rgb*foregroundTransmittance.Load(p);
        }
        return c;
    }
    int2 p = int2(pos.xy);
    int2 hi = int2(outDims.xy) - 1;
    bool useA = compose.z > 0.5f, useB = compose.w > 0.5f;
    const float2 la = useA ? lzA.Load(int3(p, 0)) : float2(0, 0);
    const float2 lb = useB ? lzB.Load(int3(p, 0)) : float2(0, 0);
    const float za = la.x, zb = lb.x;
    const bool hands = maskFlags.w > 0.5f;
    const bool fgA = hands && la.y > 0.5f, fgB = hands && lb.y > 0.5f;
    float4 c = float4(0, 0, 0, 1);
    int cls = 0;
    const bool goodA = za > 0.0f, goodB = zb > 0.0f;   // real surfaces (negative = stretched)
    if (goodB && fgB) {
        // The hands and the weapon from the fresh eye: both eyes show them at the same instant.
        c = colB.Load(int3(p, 0)); cls = 4;
    } else if (goodA && goodB) {
        // A's hands survive only where the fresh eye has no pixel of them (its frame edge, or
        // hidden behind its own nearer surface): there B has at most the WORLD behind the arm,
        // which the normal nearer rule puts behind A's hand. (Letting any real B pixel outrank
        // them painted B's world along every sleeve edge - the seventh run's capture, tinted.)
        if (zb < za * (1.0f - compose.x)) { c = colB.Load(int3(p, 0)); cls = 1; }
        else { c = colA.Load(int3(p, 0)); cls = 0; }
    } else if (goodA) {
        c = colA.Load(int3(p, 0)); cls = 0;
    } else if (goodB) {
        c = colB.Load(int3(p, 0)); cls = 1;
    } else if (za < 0.0f) {
        c = colA.Load(int3(p, 0)); cls = 6;   // A's stretched sheet, the last resort
    } else if (zb < 0.0f) {
        c = colB.Load(int3(p, 0)); cls = 7;   // B's
    } else {
        // Neither source has this pixel (a disocclusion both missed, or the frame edge):
        // the FARTHEST valid neighbour, nearest radius first - holes open onto background.
        // World neighbours only: the hands draw on top of what is behind them, so a hand
        // texel is never background, however far its depth reads.
        const int2 D[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
                            int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };
        float best = 0.0f;
        cls = 2;
        [loop] for (int r = 1; r <= 64 && best <= 0.0f; r *= 2) {
            [unroll] for (int k = 0; k < 8; ++k) {
                int2 q = clamp(p + D[k] * r, int2(0, 0), hi);
                const float2 qla = useA ? lzA.Load(int3(q, 0)) : float2(0, 0);
                const float2 qlb = useB ? lzB.Load(int3(q, 0)) : float2(0, 0);
                float qa = (hands && qla.y > 0.5f) ? 0.0f : abs(qla.x);
                float qb = (hands && qlb.y > 0.5f) ? 0.0f : abs(qlb.x);
                if (qa > best) { best = qa; c = colA.Load(int3(q, 0)); }
                if (qb > best) { best = qb; c = colB.Load(int3(q, 0)); }
            }
        }
        if (best <= 0.0f) {
            // Nothing within reach in either source (the seventh-run capture: the whole outer
            // half of a sleeve went BLACK and flickered). The held eye's own image at this very
            // pixel - what the compositor would have shown - beats black every time.
            cls = 3;
            c = rawHeld.SampleLevel(lin, (float2(p) + 0.5f) / outDims.xy, 0);
        }
    }
    if (compose.y > 0.5f) {
        // blue = from the other eye, green = its hands, yellow = the held eye's own hands kept,
        // orange / cyan = a stretched sheet of A / B, magenta = fill, red = nothing, natural =
        // the held eye's own world.
        if (cls == 0 && fgA) cls = 5;
        c.rgb *= cls == 1 ? float3(0.6, 0.7, 1.4) : cls == 4 ? float3(0.6, 1.4, 0.6) : cls == 5 ? float3(1.4, 1.4, 0.5)
               : cls == 6 ? float3(1.5, 0.9, 0.4) : cls == 7 ? float3(0.5, 1.3, 1.3) : cls == 2 ? float3(1.4, 0.5, 1.4)
               : cls == 3 ? float3(1.5, 0.4, 0.4) : float3(1, 1, 1);
    }
    c.a = 1.0f;
    return c;
}
)";

struct alignas(16) CB {
    float srcLocSky[4], srcFwdTanH[4], srcRightTanV[4], srcUpGrid[4];
    float tgtLocFar[4], tgtFwdTanH[4], tgtRightTanV[4], tgtUpEdgeRel[4];
    float othLocStaleRel[4], othFwdTanH[4], othRightTanV[4], othUpStaleAbs[4];
    float dims[4], othDims[4], compose[4], outDims[4];
    float fg[4];   // hand depth gain, band (uu)
    float maskFlags[4], refineFlags[4];
    float fgSrcLoc[4], fgSrcFwd[4], fgSrcRight[4], fgSrcUp[4];
};
static_assert(sizeof(CB) == 23 * 16, "CB layout");

void set4(float* d, const float* s, float w) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = w; }

const float kFarUu = 400000.0f;   // z-buffer encoding range (the sky sits at skyUu < this)

ID3DBlob* compile_blob(const char* entry, const char* model, char* why, size_t cap) {
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kSrc, strlen(kSrc), "b1r_afw_gpu", nullptr, nullptr, entry, model,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        put(why, cap, "%s compile: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
        rel(err); rel(code); return nullptr;
    }
    rel(err);
    return code;
}
bool compile(ID3D11Device* dev, const char* entry, const char* model, ID3D11VertexShader** out, char* why, size_t cap) {
    ID3DBlob* code = compile_blob(entry, model, why, cap);
    if (!code) return false;
    const HRESULT hr = dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    rel(code);
    if (FAILED(hr)) { put(why, cap, "%s create 0x%08lX", entry, (unsigned long)hr); return false; }
    return true;
}
bool compile(ID3D11Device* dev, const char* entry, const char* model, ID3D11PixelShader** out, char* why, size_t cap) {
    ID3DBlob* code = compile_blob(entry, model, why, cap);
    if (!code) return false;
    const HRESULT hr = dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    rel(code);
    if (FAILED(hr)) { put(why, cap, "%s create 0x%08lX", entry, (unsigned long)hr); return false; }
    return true;
}

// Every piece of pipeline state these passes set, saved and put back.
struct SavedState {
    ID3D11DeviceContext* ctx;
    // All eight OM slots: fetching and re-binding only two would silently unbind
    // render targets 2..7 of whatever the game had bound at Present.
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr; FLOAT bf[4] = {}; UINT mask = 0;
    ID3D11DepthStencilState* ds = nullptr; UINT sref = 0;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_VIEWPORT vp[1] = {}; UINT nvp = 1;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr; ID3D11HullShader* hs = nullptr; ID3D11DomainShader* dsh = nullptr;
    ID3D11Buffer* vcb = nullptr; ID3D11Buffer* pcb = nullptr;
    ID3D11ShaderResourceView* vsrv[4] = {}; ID3D11ShaderResourceView* psrv[13] = {};
    ID3D11SamplerState* vsmp[2] = {}; ID3D11SamplerState* psmp[2] = {};
    ID3D11InputLayout* il = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo{};
    explicit SavedState(ID3D11DeviceContext* c) : ctx(c) {
        ctx->OMGetRenderTargets(8, rtv, &dsv);
        ctx->OMGetBlendState(&blend, bf, &mask);
        ctx->OMGetDepthStencilState(&ds, &sref);
        ctx->RSGetState(&rs);
        ctx->RSGetViewports(&nvp, vp);
        ctx->VSGetShader(&vs, nullptr, nullptr); ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->GSGetShader(&gs, nullptr, nullptr); ctx->HSGetShader(&hs, nullptr, nullptr); ctx->DSGetShader(&dsh, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, 1, &vcb); ctx->PSGetConstantBuffers(0, 1, &pcb);
        ctx->VSGetShaderResources(0, 4, vsrv); ctx->PSGetShaderResources(0, 13, psrv);
        ctx->VSGetSamplers(0, 2, vsmp); ctx->PSGetSamplers(0, 2, psmp);
        ctx->IAGetInputLayout(&il); ctx->IAGetPrimitiveTopology(&topo);
    }
    ~SavedState() {
        ID3D11ShaderResourceView* none[13] = {};
        ctx->VSSetShaderResources(0, 4, none); ctx->PSSetShaderResources(0, 13, none);
        ctx->OMSetRenderTargets(8, rtv, dsv);
        ctx->OMSetBlendState(blend, bf, mask);
        ctx->OMSetDepthStencilState(ds, sref);
        ctx->RSSetState(rs);
        if (nvp) ctx->RSSetViewports(nvp, vp);
        ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
        ctx->GSSetShader(gs, nullptr, 0); ctx->HSSetShader(hs, nullptr, 0); ctx->DSSetShader(dsh, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &vcb); ctx->PSSetConstantBuffers(0, 1, &pcb);
        ctx->VSSetShaderResources(0, 4, vsrv); ctx->PSSetShaderResources(0, 13, psrv);
        ctx->VSSetSamplers(0, 2, vsmp); ctx->PSSetSamplers(0, 2, psmp);
        ctx->IASetInputLayout(il); ctx->IASetPrimitiveTopology(topo);
        for (auto*& r : rtv) rel(r);
        rel(dsv); rel(blend); rel(ds); rel(rs); rel(vs); rel(ps); rel(gs); rel(hs); rel(dsh);
        rel(vcb); rel(pcb); rel(il);
        for (auto*& s : vsrv) rel(s);
        for (auto*& s : psrv) rel(s);
        for (auto*& s : vsmp) rel(s);
        for (auto*& s : psmp) rel(s);
    }
};

} // namespace

bool WarpGpu::init(ID3D11Device* dev, char* why, size_t cap) {
    if (ready_) return true;
    bool ok = compile(dev, "vsmesh", "vs_5_0", &vsMesh_, why, cap) &&
              compile(dev, "psmesh", "ps_5_0", &psMesh_, why, cap) &&
              compile(dev, "vsfull", "vs_5_0", &vsFull_, why, cap) &&
              compile(dev, "pscompose", "ps_5_0", &psCompose_, why, cap);
    if (!ok) { shutdown(); return false; }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sl = {};
    sl.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sl.AddressU = sl.AddressV = sl.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sl.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_SAMPLER_DESC sp = sl;
    sp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bl.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bl.IndependentBlendEnable = FALSE;
    D3D11_DEPTH_STENCIL_DESC dt = {};
    dt.DepthEnable = TRUE; dt.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dt.DepthFunc = D3D11_COMPARISON_LESS;
    D3D11_DEPTH_STENCIL_DESC dn = {};
    ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb_)) &&
         SUCCEEDED(dev->CreateSamplerState(&sl, &linear_)) &&
         SUCCEEDED(dev->CreateSamplerState(&sp, &point_)) &&
         SUCCEEDED(dev->CreateRasterizerState(&rd, &raster_)) &&
         SUCCEEDED(dev->CreateBlendState(&bl, &blend_)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&dt, &dsTest_)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&dn, &dsOff_));
    if (!ok) { put(why, cap, "pipeline state"); shutdown(); return false; }
    ready_ = true;
    return true;
}

void WarpGpu::release_targets() {
    for (int i = 0; i < 2; ++i) {
        rel(colRtv_[i]); rel(colSrv_[i]); rel(col_[i]);
        rel(lzRtv_[i]); rel(lzSrv_[i]); rel(lz_[i]);
    }
    rel(zbDsv_); rel(zb_);
    rel(outRtv_); rel(out_);
    w_ = h_ = seedW_ = seedH_ = 0;
    refine_ = hdr_ = false;
}

void WarpGpu::shutdown() {
    release_targets();
    rel(vsMesh_); rel(psMesh_); rel(vsFull_); rel(psCompose_);
    rel(cb_); rel(linear_); rel(point_); rel(raster_); rel(blend_); rel(dsTest_); rel(dsOff_);
    ready_ = false;
}

bool WarpGpu::ensure(ID3D11Device* dev, uint32_t w, uint32_t h, bool refine, bool hdr, char* why, size_t cap) {
    if (w == w_ && h == h_ && refine == refine_ && hdr == hdr_ && out_) return true;
    release_targets();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = refine ? (w+1)/2 : w; d.Height = refine ? (h+1)/2 : h; d.MipLevels = 1; d.ArraySize = 1;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    bool ok = true;
    for (int i = 0; ok && i < 2; ++i) {
        d.Format = refine ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &col_[i])) &&
             SUCCEEDED(dev->CreateRenderTargetView(col_[i], nullptr, &colRtv_[i])) &&
             SUCCEEDED(dev->CreateShaderResourceView(col_[i], nullptr, &colSrv_[i]));
        d.Format = DXGI_FORMAT_R32G32_FLOAT;   // x: signed depth, y: the hands
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &lz_[i])) &&
             SUCCEEDED(dev->CreateRenderTargetView(lz_[i], nullptr, &lzRtv_[i])) &&
             SUCCEEDED(dev->CreateShaderResourceView(lz_[i], nullptr, &lzSrv_[i]));
    }
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &zb_)) &&
         SUCCEEDED(dev->CreateDepthStencilView(zb_, nullptr, &zbDsv_));
    const uint32_t sw=d.Width,sh=d.Height;
    d.Width=w;d.Height=h;
    d.Format = hdr ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &out_)) &&
         SUCCEEDED(dev->CreateRenderTargetView(out_, nullptr, &outRtv_));
    if (!ok) { put(why, cap, "warp targets %ux%u", w, h); release_targets(); return false; }
    w_ = w; h_ = h; seedW_=sw;seedH_=sh;refine_=refine;hdr_=hdr;
    return true;
}

bool WarpGpu::mesh(ID3D11DeviceContext* ctx, const Source& src, const Source& other, const Params& p, int which,
                   bool staleTest) {
    const float zero4[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(colRtv_[which], zero4);
    ctx->ClearRenderTargetView(lzRtv_[which], zero4);
    ctx->ClearDepthStencilView(zbDsv_, D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (!src.valid || !src.color || !src.depth || !src.depthW || !src.depthH) return false;
    const int g = p.grid < 1 ? 1 : p.grid > 16 ? 16 : p.grid;
    const uint32_t cx = (src.depthW - 1 + g - 1) / g, cy = (src.depthH - 1 + g - 1) / g;
    CB cb = {};
    set4(cb.srcLocSky, src.cam.loc, p.skyUu);
    set4(cb.srcFwdTanH, src.cam.fwd, src.cam.tanH);
    set4(cb.srcRightTanV, src.cam.right, src.cam.tanV);
    set4(cb.srcUpGrid, src.cam.up, (float)g);
    const auto& fgCam = src.foregroundCameraValid ? src.foregroundCam : src.cam;
    set4(cb.fgSrcLoc, fgCam.loc, src.foregroundCameraValid ? 1.0f : 0.0f); set4(cb.fgSrcFwd, fgCam.fwd, 0);
    set4(cb.fgSrcRight, fgCam.right, 0); set4(cb.fgSrcUp, fgCam.up, 0);
    set4(cb.tgtLocFar, p.target.loc, kFarUu);
    set4(cb.tgtFwdTanH, p.target.fwd, p.target.tanH);
    set4(cb.tgtRightTanV, p.target.right, p.target.tanV);
    set4(cb.tgtUpEdgeRel, p.target.up, p.edgeRel);
    set4(cb.othLocStaleRel, other.cam.loc, p.staleRel);
    set4(cb.othFwdTanH, other.cam.fwd, other.cam.tanH);
    set4(cb.othRightTanV, other.cam.right, other.cam.tanV);
    set4(cb.othUpStaleAbs, other.cam.up, p.staleAbsUu);
    cb.dims[0] = (float)src.depthW; cb.dims[1] = (float)src.depthH; cb.dims[2] = (float)cx; cb.dims[3] = (float)cy;
    cb.othDims[0] = (float)other.depthW; cb.othDims[1] = (float)other.depthH;
    cb.othDims[2] = (staleTest && other.valid && other.depth) ? 1.0f : 0.0f;
    cb.othDims[3] = p.edgeAbsUu;
    cb.outDims[0] = (float)p.w; cb.outDims[1] = (float)p.h; cb.outDims[2] = p.hdrWorld ? 1.0f : 0.0f;
    cb.fg[0] = p.fgGain > 0.05f ? p.fgGain : 1.0f; cb.fg[1] = p.fgGain != 1.0f ? p.fgBandUu : 0.0f;
    cb.fg[2] = 0.0f;
    cb.fg[3] = p.fgTanGain > 0.1f ? p.fgTanGain : 1.0f;
    const bool useMask = p.handsFromFresh && src.mask;
    const bool othMask = p.handsFromFresh && other.valid && other.mask;
    cb.maskFlags[0] = useMask ? (src.invertedMask ? 2.0f : 1.0f) : 0.0f;
    // A's hands are dropped only when B is there to supply them.
    cb.maskFlags[1] = useMask && which == 0 && other.valid && other.depth ? 1.0f : 0.0f;
    cb.maskFlags[2] = othMask ? (other.invertedMask ? 2.0f : 1.0f) : 0.0f;
    cb.maskFlags[3] = p.handsFromFresh ? 1.0f : 0.0f;
    cb.refineFlags[0] = p.refine ? 1.0f : 0.0f;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(cb_, 0);
    ID3D11RenderTargetView* rtvs[2] = {colRtv_[which], lzRtv_[which]};
    ctx->OMSetRenderTargets(2, rtvs, zbDsv_);
    ctx->OMSetDepthStencilState(dsTest_, 0);
    ID3D11ShaderResourceView* vsrv[4] = {src.depth, nullptr, nullptr, useMask ? src.mask : nullptr};
    ID3D11ShaderResourceView* psrv[5] = {nullptr, src.color, other.depth, useMask ? src.mask : nullptr,
                                         othMask ? other.mask : nullptr};
    ID3D11SamplerState* smp[2] = {linear_, point_};
    ctx->VSSetShader(vsMesh_, nullptr, 0);
    ctx->PSSetShader(psMesh_, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb_);
    ctx->PSSetConstantBuffers(0, 1, &cb_);
    ctx->VSSetShaderResources(0, 4, vsrv);
    ctx->PSSetShaderResources(0, 5, psrv);
    ctx->PSSetSamplers(0, 2, smp);
    // In bands. SV_VertexID restarts at 0 in each draw (measured: afw-gpu-tests compares the
    // bands against one draw), so each band's first vertex rides in the constant buffer.
    const uint32_t total = cx * cy * 6;
    const uint32_t band = p.maxDrawVerts >= 6 ? p.maxDrawVerts / 6 * 6 : total;
    for (uint32_t start = 0; start < total; start += band) {
        if (start) {
            cb.fg[2] = (float)start;   // exact: total < 2^24
            if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
            memcpy(m.pData, &cb, sizeof(cb));
            ctx->Unmap(cb_, 0);
        }
        ctx->Draw(total - start < band ? total - start : band, 0);
    }
    ID3D11ShaderResourceView* none[11] = {};
    ctx->VSSetShaderResources(0, 4, none);
    ctx->PSSetShaderResources(0, 11, none);
    return true;
}

bool WarpGpu::run(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Source& held, const Source& fresh,
                  const Params& p, char* why, size_t cap) {
    if (!ready_ && !init(dev, why, cap)) return false;
    if (!p.w || !p.h) { put(why, cap, "no size"); return false; }
    if (!held.valid && !fresh.valid) { put(why, cap, "no source"); return false; }
    if (p.hdrWorld && (!p.refine || !p.nativeForeground || !p.foregroundTransmittance ||
        (held.valid && !held.mask) || (fresh.valid && !fresh.mask))) {
        put(why,cap,"HDR foreground inputs/refined world masks missing");return false;
    }
    if (!ensure(dev, p.w, p.h, p.refine, p.hdrWorld, why, cap)) return false;
    SavedState saved(ctx);
    D3D11_VIEWPORT vp = {0, 0, (float)seedW_, (float)seedH_, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster_);
    ctx->OMSetBlendState(blend_, nullptr, 0xFFFFFFFF);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    const bool haveA = p.useHeld && held.valid && mesh(ctx, held, fresh, p, 0, p.staleTest);
    if (!haveA && !p.useHeld) mesh(ctx, Source{}, fresh, p, 0, false);   // clears A
    const bool haveB = p.useFresh && fresh.valid && mesh(ctx, fresh, held, p, 1, false);
    if (!haveB && !p.useFresh) mesh(ctx, Source{}, held, p, 1, false);   // clears B
    // Compose into the native output; only the seed passes are half-sized.
    vp.Width=(float)p.w;vp.Height=(float)p.h;ctx->RSSetViewports(1,&vp);
    CB cb = {};
    set4(cb.srcLocSky,held.cam.loc,p.skyUu);
    set4(cb.srcFwdTanH,held.cam.fwd,held.cam.tanH);
    set4(cb.srcRightTanV,held.cam.right,held.cam.tanV);
    set4(cb.srcUpGrid,held.cam.up,(float)p.grid);
    const auto& fgCam = held.foregroundCameraValid ? held.foregroundCam : held.cam;
    set4(cb.fgSrcLoc,fgCam.loc,held.foregroundCameraValid?1.0f:0.0f);set4(cb.fgSrcFwd,fgCam.fwd,0);
    set4(cb.fgSrcRight,fgCam.right,0);set4(cb.fgSrcUp,fgCam.up,0);
    set4(cb.tgtLocFar,p.target.loc,kFarUu);
    set4(cb.tgtFwdTanH,p.target.fwd,p.target.tanH);
    set4(cb.tgtRightTanV,p.target.right,p.target.tanV);
    set4(cb.tgtUpEdgeRel,p.target.up,p.edgeRel);
    set4(cb.othLocStaleRel,fresh.cam.loc,p.staleRel);
    set4(cb.othFwdTanH,fresh.cam.fwd,fresh.cam.tanH);
    set4(cb.othRightTanV,fresh.cam.right,fresh.cam.tanV);
    set4(cb.othUpStaleAbs,fresh.cam.up,p.staleAbsUu);
    cb.dims[0]=(float)held.depthW;cb.dims[1]=(float)held.depthH;
    cb.dims[2]=(float)seedW_;cb.dims[3]=(float)seedH_;
    cb.othDims[0]=(float)fresh.depthW;cb.othDims[1]=(float)fresh.depthH;cb.othDims[3]=p.edgeAbsUu;
    cb.fg[0]=p.fgGain>0.05f?p.fgGain:1.0f;cb.fg[1]=p.fgGain!=1.0f?p.fgBandUu:0.0f;
    cb.fg[3]=p.fgTanGain>0.1f?p.fgTanGain:1.0f;
    cb.refineFlags[0]=p.refine?1.0f:0.0f;
    cb.refineFlags[1]=p.staleTest?1.0f:0.0f;
    cb.refineFlags[2]=p.handsFromFresh && held.mask?(held.invertedMask?2.0f:1.0f):0.0f;
    cb.refineFlags[3]=p.handsFromFresh && fresh.mask?(fresh.invertedMask?2.0f:1.0f):0.0f;
    cb.compose[0] = p.nearerRel; cb.compose[1] = p.tint ? 1.0f : 0.0f;
    cb.compose[2] = haveA ? 1.0f : 0.0f; cb.compose[3] = haveB ? 1.0f : 0.0f;
    cb.outDims[0] = (float)p.w; cb.outDims[1] = (float)p.h; cb.outDims[2] = p.hdrWorld ? 1.0f : 0.0f;
    cb.maskFlags[3] = p.handsFromFresh ? 1.0f : 0.0f;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { put(why, cap, "map"); return false; }
    memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(cb_, 0);
    ctx->OMSetRenderTargets(1, &outRtv_, nullptr);
    ctx->OMSetDepthStencilState(dsOff_, 0);
    ID3D11ShaderResourceView* srv[13] = {colSrv_[0], lzSrv_[0], colSrv_[1], lzSrv_[1],
        held.valid && held.color ? held.color : fresh.color,held.depth,fresh.depth,held.color,fresh.color,
        p.handsFromFresh?held.mask:nullptr,p.handsFromFresh?fresh.mask:nullptr,
        p.nativeForeground,p.foregroundTransmittance};
    ID3D11SamplerState* smp[2] = {linear_,point_};
    ctx->VSSetShader(vsFull_, nullptr, 0);
    ctx->PSSetShader(psCompose_, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb_);
    ctx->PSSetShaderResources(0, 13, srv);
    ctx->PSSetSamplers(0, 2, smp);
    ctx->Draw(3, 0);
    ++stats.runs;
    return true;
}

} // namespace bvr::b1r::afw
