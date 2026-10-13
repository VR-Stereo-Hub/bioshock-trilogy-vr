#pragma once
// Private scratch-only stencil experiment. Never borrows a bit from live depth.
#include "game/bioshock1r/native_proof.h"
#include <d3d11.h>
namespace bvr::frame_inspector { struct DrawArgs; }
namespace bvr::b1r::native_proof::detail {
struct ProofSurface {
    ID3D11Texture2D *scratch, *before, *expected, *result;
};
bool masked_eligible(ID3D11DeviceContext*, UINT dsvFlags);
bool masked_init(ID3D11Device*);
void masked_stop();
bool masked_run(ID3D11DeviceContext*, const frame_inspector::DrawArgs&,
                const ProofSurface& colour, const ProofSurface& depth,
                ID3D11RenderTargetView*, ID3D11DepthStencilView*,
                MaskStats (&totals)[5], uint32_t draw);
// Independent CPU oracle for the synthetic tile workload, not a repair mask.
bool mask_selected(UINT x, UINT y, UINT mode);
bool compare_selected(ID3D11DeviceContext*, const ProofSurface&, bool depth,
                      UINT mode, MaskStats& result);
}
