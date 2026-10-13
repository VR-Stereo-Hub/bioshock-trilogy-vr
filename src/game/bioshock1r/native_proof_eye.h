#pragma once
#include "game/bioshock1r/native_proof.h"
#include <vector>
struct ID3D11Buffer;
struct ID3D11DeviceChild;
#include "game/bioshock1r/native_proof_mask.h"

namespace bvr::b1r::native_proof::detail {
// One draw, current native geometry. A byte-identical constant-buffer clone must
// reproduce the game draw before a translated-eye clone is exercised. The latter
// is NOT an independent native target-eye oracle; its changed pixels are telemetry.
// Uses the existing eight surfaces; all CB/OM bindings are restored before return.
bool eye_run(ID3D11DeviceContext*, const frame_inspector::DrawArgs&, const ProofSurface& colour,
             const ProofSurface& depth, ID3D11RenderTargetView*, ID3D11DepthStencilView*,
             float shiftRight, const float worldEye[3], uint32_t drawNumber, EyeStats& total);
// Continuous immediate foreground replay. Constants come from CPU uploads,
// never staging readback; geometry and material resources are consumed now.
using ConstantReader = bool (*)(ID3D11Buffer*, std::vector<uint8_t>&);
struct ShaderUsage { std::vector<unsigned> resources; bool readsDepth = false; };
bool stream_shader_usage(ID3D11DeviceChild*, unsigned stage, ShaderUsage&);
bool stream_eye_replay(ID3D11DeviceContext*, const frame_inspector::DrawArgs&,
                       ID3D11RenderTargetView* colour, ID3D11DepthStencilView* depth,
                       ID3D11RenderTargetView* transmittance, ID3D11RenderTargetView* sourceCoverage,
                       ID3D11DepthStencilView* sourceReadOnlyDepth, float shiftRight, const float worldEye[3],
                       ConstantReader, float* tanH, float* tanV, const char** reason);
struct StreamStats {
    uint64_t constantSlotsRead=0, constantSlotsRestored=0, stateCreates=0, replayDraws=0;
};
StreamStats stream_eye_stats();
void stream_eye_reset();
} // namespace bvr::b1r::native_proof::detail
