#pragma once
// BioShock 1 temporal guides on the GPU: the scene depth and camera motion vectors that
// DLSS/DLAA (and the alternate-frame warp) need, from what the mod already knows - the
// game's own scene depth buffer (copied by temporal.cpp) and the exact camera each eye
// image was rendered with (camera.cpp publishes it per build). No mod dependencies, so
// tools/tests/temporal-gpu-tests.cpp runs these shaders against a synthetic scene.
//
// The method is the Dishonored VR mod's (core/gfx/dlss_gpu.h, motion_gpu.h there): a pixel
// of the current eye image is turned back into a world point from its depth and the
// current camera, and projected into the previous camera of the SAME eye. Camera motion
// only - anything moving on its own (NPCs, particles) carries the camera's vector, which
// DLSS's own history rejection handles.
//
// BioShock specifics (ENGINE_NOTES "D3D11 frame map", and the BioShock VR DLSS/DLAA
// community fork's temporal_guides.cpp, MIT, which established them for this exe):
//   - the scene depth is D24_UNORM_S8_UINT at the backbuffer size, conventional (0 near,
//     1 far), read here through an R24_UNORM_X8_TYPELESS view of a copy;
//   - the projection is a standard D3D perspective with a 10 uu near plane and a finite far
//     plane (65536 uu in gameplay); farUu 0 selects the infinite-far form instead;
//   - the camera is the UE2.5 FRotator basis (X forward, Y right, Z up) at the CalcView
//     location, with the symmetric tangents the projection layer claims.
//
// Outputs (render size, one set reused per call - the caller copies what it keeps):
//   dlssDepth  R32_FLOAT, reversed (1 - raw): what dlss_client.h wants (DepthInverted).
//   linearZ    R32_FLOAT, view depth along the camera's forward axis in uu (0 = no depth).
//   motion     R16G16_FLOAT, previous UV minus current UV. Zero when !historyValid.
//              Pixels nearer than bodyUu keep the rotation part only (the Dishonored
//              mod's body band, where its depth includes the arms; off by default here,
//              because BS1's world depth does not - see the foreground below).
//   hands      R8_UNORM, 1 where the FOREGROUND pass (the arms and the weapon, drawn
//              after the world through the world lens) left depth. Their depth replaces
//              the world's there, so linearZ and the vectors place them where they are;
//              DLSS gets this as its "bias current colour" mask, because the arms move
//              with the controllers and no camera vector describes them.

#include <stddef.h>
#include <stdint.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Buffer;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11UnorderedAccessView;
struct ID3D11ComputeShader;
struct ID3D11SamplerState;

namespace bvr::b1r::tgpu {

struct Camera {
    float loc[3] = {};            // uu, world
    float fwd[3] = {1, 0, 0};     // the FRotator basis (ue_rot_basis)
    float right[3] = {0, 1, 0};
    float up[3] = {0, 0, 1};
    float tanH = 1, tanV = 1;     // symmetric half-FOV tangents of the render
};

struct Params {
    uint32_t w = 0, h = 0;        // the depth (and eye image) size
    float nearUu = 10.0f;
    float farUu = 65536.0f;       // 0 = infinite far plane
    bool depthReversed = false;   // the raw depth is 1 near, 0 far
    bool historyValid = false;    // false: motion is zero
    float bodyUu = 0.0f;          // nearer than this: rotation-only vectors (0 = off)
    bool haveForeground = false;  // run()'s fgDepth holds the foreground pass's depth
    Camera cur, prev;
};

// The CPU form of the shader's reprojection (host tests and audits). u, v in [0,1] of the
// current image; returns false when the point is behind the previous camera.
bool reproject(const Params& p, float u, float v, float viewZ, float* pu, float* pv);
// Device depth -> view z (uu) with the same formula as the shader.
float view_z(const Params& p, float raw);

// The vector audit: at a sparse grid, the luminance error between the current image and the
// previous one sampled where the vectors say each pixel came from, against the same with no
// movement and with rotation-only vectors, and against the vectors recomputed with the depth
// scaled by each kAuditScales entry (a wrong near plane shows up as a better score at a scale
// other than 1). Read back without stalling: each call folds in the oldest finished result.
const int kAuditScales = 5;
const float kAuditScaleValues[kAuditScales] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
struct Audit {
    uint64_t frames = 0, samples = 0;
    double zeroErr = 0, rotErr = 0, vecErr = 0;
    double scaleErr[kAuditScales] = {};
    void clear() { *this = Audit{}; }
};

class TemporalGpu {
public:
    bool init(ID3D11Device* dev, char* why, size_t cap);
    void shutdown();
    // rawDepth: the world pass's depth (R24_UNORM_X8 or R32_FLOAT view), w x h. fgDepth:
    // the foreground pass's (same formats; cleared = far), or null (p.haveForeground off).
    bool run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* rawDepth,
             ID3D11ShaderResourceView* fgDepth, const Params& p, char* why, size_t cap);
    // After run(): audit `cur` against `prev` (both w x h colour SRVs of one eye). Cheap; the
    // result lands in `audit` a few calls later.
    void audit_frame(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* cur,
                     ID3D11ShaderResourceView* prev, const Params& p);
    ID3D11Texture2D* dlss_depth() const { return dlssDepth_; }
    ID3D11Texture2D* linear_z() const { return linearZ_; }
    ID3D11ShaderResourceView* linear_z_srv() const { return linearZSrv_; }
    ID3D11Texture2D* motion() const { return motion_; }
    ID3D11ShaderResourceView* motion_srv() const { return motionSrv_; }
    ID3D11Texture2D* hands() const { return hands_; }
    ID3D11ShaderResourceView* hands_srv() const { return handsSrv_; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    Audit audit;

private:
    bool ensure(ID3D11Device* dev, uint32_t w, uint32_t h, char* why, size_t cap);
    void release_targets();
    bool ready_ = false;
    ID3D11ComputeShader* cs_ = nullptr;
    ID3D11ComputeShader* csAudit_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11SamplerState* linear_ = nullptr;
    ID3D11Texture2D* dlssDepth_ = nullptr;
    ID3D11UnorderedAccessView* dlssDepthUav_ = nullptr;
    ID3D11Texture2D* linearZ_ = nullptr;
    ID3D11UnorderedAccessView* linearZUav_ = nullptr;
    ID3D11ShaderResourceView* linearZSrv_ = nullptr;
    ID3D11Texture2D* motion_ = nullptr;
    ID3D11UnorderedAccessView* motionUav_ = nullptr;
    ID3D11ShaderResourceView* motionSrv_ = nullptr;
    ID3D11Texture2D* hands_ = nullptr;
    ID3D11UnorderedAccessView* handsUav_ = nullptr;
    ID3D11ShaderResourceView* handsSrv_ = nullptr;
    ID3D11Buffer* auditBuf_ = nullptr;
    ID3D11UnorderedAccessView* auditUav_ = nullptr;
    ID3D11Buffer* auditStage_[3] = {};
    bool auditPending_[3] = {};
    int auditNext_ = 0;
    uint32_t w_ = 0, h_ = 0;
};

// Temporal anti-aliasing for any GPU (the Dishonored mod's custom TAA, rebuilt on these
// guides): each eye keeps its own history; a pixel's history is fetched where the motion
// vector says it was, clamped to the current 3x3 neighbourhood's colour range (in YCoCg),
// and blended with the current colour. No history, off screen, or the hands mask = the
// current colour. Same size in and out (no upscaling - that is DLSS's job).
class TaaGpu {
public:
    bool init(ID3D11Device* dev, char* why, size_t cap);
    void shutdown();
    // cur: this eye's image (w x h SRV). motion/hands: TemporalGpu's for this image (hands
    // may be null). Writes this eye's new history, returned by out(eye).
    bool run(ID3D11Device* dev, ID3D11DeviceContext* ctx, int eye, ID3D11ShaderResourceView* cur,
             ID3D11ShaderResourceView* motion, ID3D11ShaderResourceView* hands, uint32_t w, uint32_t h,
             bool historyValid, float currentWeight, char* why, size_t cap);
    ID3D11Texture2D* out(int eye) const { return hist_[eye & 1][cur_[eye & 1]]; }
    void forget(int eye) { valid_[eye & 1] = false; }

private:
    bool ready_ = false;
    ID3D11ComputeShader* cs_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11SamplerState* linear_ = nullptr;
    ID3D11Texture2D* hist_[2][2] = {};
    ID3D11ShaderResourceView* histSrv_[2][2] = {};
    ID3D11UnorderedAccessView* histUav_[2][2] = {};
    int cur_[2] = {};
    bool valid_[2] = {};
    uint32_t w_[2] = {}, h_[2] = {};
};

} // namespace bvr::b1r::tgpu
