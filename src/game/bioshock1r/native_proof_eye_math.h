#pragma once
// Local stereo proof. Packed columns match reflected column_major float4x4
// and float4x3 constants. No engine offsets, camera pointers or memory writes.
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>

namespace bvr::b1r::native_proof::detail {
struct EyePatch {
    float wvp[16]{};
    float eye[3]{};
    double delta[3]{};
    double residual = 0;
    float projectionX = 0;
    double originErrorUu = 0, shiftedOriginErrorUu = 0, translationErrorUu = 0;
};
struct EyeDiagnostic {
    const char* reason = "none";
    unsigned component = 0;
    double actual = 0, expected = 0, limit = 0;
};
inline bool eye_reject(EyeDiagnostic* diagnostic, const char* reason,
                       unsigned component = 0, double actual = 0, double expected = 0, double limit = 0) {
    if (diagnostic) *diagnostic = {reason,component,actual,expected,limit};
    return false;
}
// Solve clip-x, clip-y, clip-w against local xyz. This handles rotated, scaled
// and sheared local geometry; normalizing a matrix row would not.
inline bool eye_solve(double a[3][4], double out[3]) {
    double scale = 0;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) scale = (std::max)(scale, std::fabs(a[i][j]));
    if (!std::isfinite(scale) || scale < 1e-12) return false;
    for (unsigned col = 0; col < 3; ++col) {
        unsigned pivot = col;
        for (unsigned row = col + 1; row < 3; ++row)
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) pivot = row;
        if (std::fabs(a[pivot][col]) < scale * 1e-10) return false;
        for (unsigned j = col; j < 4; ++j) std::swap(a[col][j], a[pivot][j]);
        const double d = a[col][col];
        for (unsigned j = col; j < 4; ++j) a[col][j] /= d;
        for (unsigned row = 0; row < 3; ++row) {
            if (row == col) continue;
            const double m = a[row][col];
            for (unsigned j = col; j < 4; ++j) a[row][j] -= m * a[col][j];
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        out[i] = a[i][3];
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}
inline bool close_float(float a, float b, double tolerance = 2e-5) {
    return std::isfinite(a) && std::isfinite(b) &&
           std::fabs(double(a) - b) <= tolerance * (std::max)(1.0, (std::max)(std::fabs(double(a)), std::fabs(double(b))));
}
// BS1's matrix-kind 7 is built from PROJECTION and the fixed screen transform,
// without the view/model matrices (native-constants-matrices.py, MD5 in notes).
// Its first three packed columns are [2/Px,0,-1/Px,0],
// [0,-2/Py,1/Py,0], [0,0,1,0] for the supported symmetric perspective lens.
// Refuse other projections instead of treating their diagonal as a scale.
inline bool make_eye_patch(const float wvp[16], const float eye[3], const float screen[12],
                           const float clip[4], float shiftRight, EyePatch& out, EyeDiagnostic* diagnostic = nullptr,
                           const float worldEye[3] = nullptr) {
    if (diagnostic) *diagnostic = {};
    if (!wvp) return eye_reject(diagnostic,"missing-or-ambiguous-worldViewProj");
    if (!eye) return eye_reject(diagnostic,"missing-or-ambiguous-localEyePos");
    if (!screen) return eye_reject(diagnostic,"missing-or-ambiguous-screenDataToCamera");
    if (!clip) return eye_reject(diagnostic,"missing-or-ambiguous-vertexClipPlane");
    if (!std::isfinite(shiftRight) || std::fabs(shiftRight) > 128.0f)
        return eye_reject(diagnostic,"shift-invalid",0,shiftRight,0,128);
    for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(wvp[i]))
        return eye_reject(diagnostic,"worldViewProj-nonfinite",i,wvp[i]);
    for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(eye[i]))
        return eye_reject(diagnostic,"localEyePos-nonfinite",i,eye[i]);
    for (unsigned i = 0; i < 12; ++i) if (!std::isfinite(screen[i]))
        return eye_reject(diagnostic,"screenDataToCamera-nonfinite",i,screen[i]);
    // The native disabled-clip branch uploads zero, measured in run nineteen.
    // Keep nonzero planes refused until their source is proven.
    for (unsigned i = 0; i < 4; ++i) if (clip[i] != 0.0f)
        return eye_reject(diagnostic,"vertexClipPlane-nonzero",i,clip[i]);
    if (!(screen[0] > 0.0001f && screen[0] < 10000.0f))
        return eye_reject(diagnostic,"screenDataToCamera-horizontal-scale",0,screen[0]);
    if (!(screen[5] < -0.0001f && screen[5] > -10000.0f))
        return eye_reject(diagnostic,"screenDataToCamera-vertical-scale",5,screen[5]);
    const float expected[12] = {screen[0],0,-0.5f*screen[0],0, 0,screen[5],-0.5f*screen[5],0, 0,0,1,0};
    for (unsigned i = 0; i < 12; ++i) if (!close_float(screen[i], expected[i]))
        return eye_reject(diagnostic,"screenDataToCamera-shape",i,screen[i],expected[i],
            2e-5*(std::max)(1.0,(std::max)(std::fabs(double(screen[i])),std::fabs(double(expected[i])))));
    // Native kind 2 is model * (view * projection), with rounded float products
    // and sums at both stages. localEyePos comes separately from the float inverse
    // model and world camera. Local cancellation alone misses the upstream loss.
    // A fresh same-thread camera value supplies the absolute-world scale. This
    // conservative numerical budget is also capped by a 0.01 uu (0.1 mm at 100 uu/m) 3D
    // accuracy limit. Neither budget is evidence of native target-eye equality.
    double worldMagnitude = 0;
    if (worldEye) for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(worldEye[i]))
            return eye_reject(diagnostic,"worldEye-nonfinite",i,worldEye[i]);
        worldMagnitude += std::fabs(double(worldEye[i]));
    }
    const double px = 2.0 / screen[0], py = -2.0 / screen[5];
    const double projection[3] = {px,py,1};
    constexpr double maxOriginErrorUu = 0.01;
    double originErrorSquared = 0;
    const unsigned rows[3] = {0,4,12};
    double a[3][4]{};
    double worst = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const float* row = wvp + rows[i];
        double residual = row[3], magnitude = std::fabs(double(row[3]));
        for (unsigned j = 0; j < 3; ++j) {
            a[i][j] = row[j];
            residual += double(row[j]) * eye[j];
            magnitude += std::fabs(double(row[j]) * eye[j]);
        }
        // Preserve the old local-scale check when no world sample is supplied.
        // Projection scales convert the world-space precision budget to clip units.
        const double allowance = 32.0 * FLT_EPSILON *
            ((std::max)(1.0, magnitude) + projection[i]*worldMagnitude);
        if (std::fabs(residual) > allowance)
            return eye_reject(diagnostic,"localEyePos-origin",rows[i]/4,residual,0,allowance);
        worst = (std::max)(worst, std::fabs(residual) / allowance);
        const double cameraResidual = residual / projection[i];
        originErrorSquared += cameraResidual*cameraResidual;
        a[i][3] = i == 0 ? double(shiftRight) * px : 0.0;
    }
    const double originError = std::sqrt(originErrorSquared);
    if (originError > maxOriginErrorUu)
        return eye_reject(diagnostic,"localEyePos-accuracy-budget",3,originError,0,maxOriginErrorUu);
    EyePatch candidate;
    if (!eye_solve(a, candidate.delta)) return eye_reject(diagnostic,"local-model-singular");
    std::memcpy(candidate.wvp, wvp, sizeof(candidate.wvp));
    std::memcpy(candidate.eye, eye, sizeof(candidate.eye));
    // Affine local geometry has homogeneous w=1. A camera-right translation
    // changes only clip-x's translation, but its LOCAL eye position changes too.
    if (shiftRight != 0.0f) {
        candidate.wvp[3] = float(double(wvp[3]) - double(shiftRight) * px);
        for (unsigned i = 0; i < 3; ++i) candidate.eye[i] = float(double(eye[i]) + candidate.delta[i]);
    }
    for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(candidate.wvp[i]))
        return eye_reject(diagnostic,"shifted-worldViewProj-nonfinite",i,candidate.wvp[i]);
    for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(candidate.eye[i]))
        return eye_reject(diagnostic,"shifted-localEyePos-nonfinite",i,candidate.eye[i]);
    // Both values can round back to their source while still projecting to the
    // origin. Check the requested displacement itself survives float storage.
    const double clipShift = (double(wvp[3])-candidate.wvp[3])/px;
    double eyeShiftErrorSquared = 0;
    for (unsigned i=0;i<3;++i) {
        double moved = 0;
        for (unsigned j=0;j<3;++j) moved += double(wvp[rows[i]+j])*(double(candidate.eye[j])-eye[j]);
        const double error = moved/projection[i] - (i==0 ? double(shiftRight) : 0.0);
        eyeShiftErrorSquared += error*error;
    }
    candidate.translationErrorUu = (std::max)(std::fabs(clipShift-shiftRight),std::sqrt(eyeShiftErrorSquared));
    if (candidate.translationErrorUu > maxOriginErrorUu)
        return eye_reject(diagnostic,"shift-float-resolution",3,candidate.translationErrorUu,0,maxOriginErrorUu);
    double shiftedErrorSquared = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const float* row = candidate.wvp + rows[i];
        double residual = row[3];
        for (unsigned j = 0; j < 3; ++j) residual += double(row[j])*candidate.eye[j];
        residual /= projection[i]; shiftedErrorSquared += residual*residual;
    }
    const double shiftedError = std::sqrt(shiftedErrorSquared);
    if (shiftedError > maxOriginErrorUu)
        return eye_reject(diagnostic,"shifted-localEyePos-accuracy-budget",3,shiftedError,0,maxOriginErrorUu);
    candidate.originErrorUu=originError; candidate.shiftedOriginErrorUu=shiftedError;
    candidate.residual = worst; candidate.projectionX = float(px); out = candidate;
    return true;
}
} // namespace bvr::b1r::native_proof::detail
