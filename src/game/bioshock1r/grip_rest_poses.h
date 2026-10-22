#pragma once
// GENERATED 2026-10-21 - the free hand's REST POSE on each two-handed gun, from the gun's idle clip at 0 s on
// the hands rig (NEWPlayerHands-all.glb, tools\bsmesh; the Blender measurement is described in
// gun_geometry.h). Every left-hand bone against the wrist (Bip01_L_Hand, engine bone 6) and the wrist
// against the grip socket bone (R_grip, 43): Unreal axes, rig units, quaternions x y z w. This is the
// grip pose the two-handed hold draws whenever it holds (docs/bioshock1/HELD_GUN_AUDIT.md 2.3): the
// fingers curled as the game curls them on that gun's fore-end, the wrist at the game's turn against the
// gun. Mirrored for the left-hand hold by the rig's own pair mirror (mirror_math.h), never re-measured.
// The palm centre (the five palm bones' mean, in the wrist) is 9.215 0.119 0.313 on every gun - the
// rig's rigid palm (palette.cpp), which is the check that the rows are in the engine's own frame.
#include "game/bioshock1r/gun_geometry.h"

namespace bvr::b1r::gun_geometry {

// Shotgun: Shotgun__FidgetShotgun at 0.00 s (NEWPlayerHands-all.glb)
//   wrist vs 43: p 45.266 7.427 14.980  q -0.800795 -0.487867 0.110595 0.329367 ; palm centre in the wrist 9.215 0.119 0.313
inline constexpr RestBone kShotgunRestBones[] = {
    {7, {1.3674f, -1.9279f, -3.8891f}, {-0.727388f, 0.231096f, 0.053888f, 0.643892f}}, // kBone_L_Thumb1
    {8, {5.7043f, -3.2318f, -5.7267f}, {-0.735433f, 0.204053f, 0.030052f, 0.645444f}}, // kBone_L_Thumb2
    {9, {8.8326f, -4.1254f, -6.7785f}, {-0.701762f, 0.300048f, 0.115624f, 0.635714f}}, // kBone_L_Thumb3
    {10, {11.3943f, 0.6631f, -3.8253f}, {-0.013817f, 0.036616f, -0.335418f, 0.941256f}}, // kBone_L_Index1
    {11, {14.7518f, -2.0864f, -4.0847f}, {-0.025156f, 0.029981f, -0.628007f, 0.777223f}}, // kBone_L_Index2
    {12, {15.4338f, -5.2706f, -4.1335f}, {-0.032781f, 0.021380f, -0.826714f, 0.561260f}}, // kBone_L_Index3
    {13, {12.0340f, 0.8465f, -0.9173f}, {-0.004837f, 0.010363f, -0.452521f, 0.891681f}}, // kBone_L_Middle1
    {14, {14.9072f, -3.0825f, -0.9860f}, {-0.009257f, 0.006717f, -0.828196f, 0.560323f}}, // kBone_L_Middle2
    {15, {13.6213f, -6.2920f, -0.9590f}, {-0.011131f, 0.002630f, -0.980188f, 0.197738f}}, // kBone_L_Middle3
    {16, {11.6518f, -0.3666f, 1.8425f}, {0.021071f, -0.035925f, -0.543025f, 0.838683f}}, // kBone_L_Ring1
    {17, {13.4914f, -4.4838f, 2.0112f}, {0.036125f, -0.020727f, -0.887714f, 0.458507f}}, // kBone_L_Ring2
    {18, {11.6088f, -7.1450f, 1.8639f}, {0.041465f, -0.003904f, -0.997897f, 0.049660f}}, // kBone_L_Ring3
    {19, {10.9949f, -0.5494f, 4.4626f}, {0.068397f, -0.078413f, -0.677399f, 0.728219f}}, // kBone_L_Pinky1
    {20, {11.2639f, -4.3849f, 4.5455f}, {0.097188f, -0.037164f, -0.939850f, 0.325354f}}, // kBone_L_Pinky2
    {21, {9.8770f, -5.5004f, 4.2597f}, {-0.104000f, -0.003253f, 0.992584f, 0.062856f}}, // kBone_L_Pinky3
};
// MachineGun: TommyGun__FidgetTommygun at 0.00 s (NEWPlayerHands-all.glb)
//   wrist vs 43: p 34.828 0.243 1.912  q -0.897785 -0.429240 -0.010951 0.098058 ; palm centre in the wrist 9.215 0.119 0.313
inline constexpr RestBone kMachineGunRestBones[] = {
    {7, {1.3674f, -1.9279f, -3.8891f}, {-0.690961f, 0.354762f, -0.118139f, 0.618676f}}, // kBone_L_Thumb1
    {8, {4.8881f, -5.0383f, -5.2366f}, {-0.740137f, 0.235543f, -0.218879f, 0.590600f}}, // kBone_L_Thumb2
    {9, {7.6003f, -7.1145f, -5.0800f}, {-0.774533f, -0.058163f, -0.423759f, 0.465988f}}, // kBone_L_Thumb3
    {10, {11.3943f, 0.6631f, -3.8253f}, {0.000000f, 0.060797f, -0.120902f, 0.990801f}}, // kBone_L_Index1
    {11, {15.5825f, -0.3785f, -4.3490f}, {-0.027995f, 0.053968f, -0.563552f, 0.823840f}}, // kBone_L_Index2
    {12, {16.7516f, -3.4124f, -4.5359f}, {-0.054000f, 0.027933f, -0.935582f, 0.347837f}}, // kBone_L_Index3
    {13, {12.0340f, 0.8465f, -0.9173f}, {-0.004492f, 0.010363f, -0.424541f, 0.905338f}}, // kBone_L_Middle1
    {14, {15.1462f, -2.8960f, -0.9901f}, {-0.009673f, 0.005833f, -0.871134f, 0.490915f}}, // kBone_L_Middle2
    {15, {13.3558f, -5.8537f, -0.9516f}, {-0.011204f, 0.001436f, -0.995151f, 0.097709f}}, // kBone_L_Middle3
    {16, {11.6517f, -0.3666f, 1.8425f}, {0.019344f, -0.036961f, -0.502954f, 0.863306f}}, // kBone_L_Ring1
    {17, {13.8690f, -4.2918f, 2.0427f}, {0.036128f, -0.020860f, -0.887022f, 0.459838f}}, // kBone_L_Ring2
    {18, {11.9943f, -6.9588f, 1.8962f}, {0.041565f, -0.003565f, -0.998323f, 0.040146f}}, // kBone_L_Ring3
    {19, {10.9949f, -0.5494f, 4.4626f}, {0.040416f, 0.013818f, -0.521953f, 0.851904f}}, // kBone_L_Pinky1
    {20, {12.7438f, -3.9652f, 4.2098f}, {0.029211f, 0.031163f, -0.860590f, 0.507505f}}, // kBone_L_Pinky2
    {21, {11.8728f, -5.5366f, 4.0622f}, {0.014909f, 0.040027f, -0.989819f, 0.135774f}}, // kBone_L_Pinky3
};
// GrenadeLauncher: GrenadeLauncher__FidgetLauncher at 0.00 s (NEWPlayerHands-all.glb)
//   wrist vs 43: p 47.070 10.616 11.438  q -0.524410 -0.669323 0.090250 0.518513 ; palm centre in the wrist 9.215 0.119 0.313
inline constexpr RestBone kGrenadeLauncherRestBones[] = {
    {7, {1.3674f, -1.9279f, -3.8891f}, {-0.708743f, 0.423504f, -0.011054f, 0.564097f}}, // kBone_L_Thumb1
    {8, {4.5004f, -4.9227f, -6.1476f}, {-0.694464f, 0.446535f, 0.007464f, 0.564156f}}, // kBone_L_Thumb2
    {9, {6.5557f, -7.0146f, -7.9058f}, {-0.617952f, 0.547548f, 0.093708f, 0.556368f}}, // kBone_L_Thumb3
    {10, {11.3943f, 0.6631f, -3.8253f}, {0.000001f, -0.000000f, 0.000000f, 1.000000f}}, // kBone_L_Index1
    {11, {15.7417f, 0.6631f, -3.8253f}, {0.000000f, 0.000000f, -0.086704f, 0.996234f}}, // kBone_L_Index2
    {12, {18.9495f, 0.1005f, -3.8253f}, {0.000000f, 0.000000f, -0.361655f, 0.932312f}}, // kBone_L_Index3
    {13, {12.0340f, 0.8465f, -0.9173f}, {-0.001727f, 0.011400f, -0.188953f, 0.981918f}}, // kBone_L_Middle1
    {14, {16.5531f, -0.9600f, -1.0231f}, {-0.004444f, 0.010639f, -0.421747f, 0.906640f}}, // kBone_L_Middle2
    {15, {18.7799f, -3.6046f, -1.0768f}, {-0.006546f, 0.009492f, -0.599936f, 0.799965f}}, // kBone_L_Middle3
    {16, {11.6518f, -0.3666f, 1.8425f}, {0.012090f, -0.039725f, -0.333345f, 0.941890f}}, // kBone_L_Ring1
    {17, {15.1472f, -3.2046f, 2.1438f}, {0.020407f, -0.036163f, -0.529435f, 0.847334f}}, // kBone_L_Ring2
    {18, {16.5725f, -6.1372f, 2.2733f}, {0.028255f, -0.030428f, -0.711916f, 0.701036f}}, // kBone_L_Ring3
    {19, {10.9949f, -0.5494f, 4.4626f}, {0.040071f, -0.096031f, -0.413141f, 0.904703f}}, // kBone_L_Pinky1
    {20, {13.4569f, -3.4539f, 5.0035f}, {0.073114f, -0.074039f, -0.721851f, 0.684181f}}, // kBone_L_Pinky2
    {21, {13.3612f, -5.2540f, 4.9959f}, {0.086547f, -0.057767f, -0.845033f, 0.524492f}}, // kBone_L_Pinky3
};
// ChemicalThrower: ChemicalThrower__FidgetChem at 0.00 s (NEWPlayerHands-all.glb)
//   wrist vs 43: p 8.233 -19.212 29.020  q 0.223938 0.511365 0.111685 0.822122 ; palm centre in the wrist 9.215 0.119 0.313
inline constexpr RestBone kChemicalThrowerRestBones[] = {
    {7, {1.3674f, -1.9279f, -3.8891f}, {-0.717227f, 0.413141f, -0.031089f, 0.560297f}}, // kBone_L_Thumb1
    {8, {4.5769f, -4.9945f, -5.9338f}, {-0.762590f, 0.321804f, -0.099756f, 0.552221f}}, // kBone_L_Thumb2
    {9, {7.2199f, -7.0494f, -6.6288f}, {-0.824970f, 0.067274f, -0.267391f, 0.493358f}}, // kBone_L_Thumb3
    {10, {11.3943f, 0.6631f, -3.8253f}, {0.008636f, -0.016235f, -0.464611f, 0.885324f}}, // kBone_L_Index1
    {11, {13.8625f, -2.9146f, -3.7352f}, {0.016816f, -0.007442f, -0.912030f, 0.409712f}}, // kBone_L_Index2
    {12, {11.7009f, -5.3493f, -3.8152f}, {0.018383f, -0.000453f, -0.999375f, 0.030189f}}, // kBone_L_Index3
    {13, {12.0340f, 0.8465f, -0.9173f}, {-0.005183f, 0.010018f, -0.484646f, 0.874638f}}, // kBone_L_Middle1
    {14, {14.6142f, -3.2809f, -0.9782f}, {-0.009269f, 0.006427f, -0.837666f, 0.546066f}}, // kBone_L_Middle2
    {15, {13.2192f, -6.4446f, -0.9487f}, {-0.011171f, 0.001568f, -0.993802f, 0.110588f}}, // kBone_L_Middle3
    {16, {11.6518f, -0.3666f, 1.8425f}, {0.017617f, -0.037652f, -0.463575f, 0.885082f}}, // kBone_L_Ring1
    {17, {14.2120f, -4.0757f, 2.0696f}, {0.036567f, -0.019772f, -0.899311f, 0.435329f}}, // kBone_L_Ring2
    {18, {12.1944f, -6.6354f, 1.9111f}, {-0.041405f, -0.003699f, 0.990186f, 0.133431f}}, // kBone_L_Ring3
    {19, {10.9949f, -0.5494f, 4.4626f}, {0.055270f, -0.088086f, -0.554079f, 0.825944f}}, // kBone_L_Pinky1
    {20, {12.4197f, -4.1068f, 4.7867f}, {0.096197f, -0.039496f, -0.931147f, 0.349502f}}, // kBone_L_Pinky2
    {21, {11.0908f, -5.2938f, 4.5135f}, {-0.103949f, 0.002876f, 0.994574f, 0.002832f}}, // kBone_L_Pinky3
};
// Crossbow: Crossbow__FidgetCrossbow at 0.00 s (NEWPlayerHands-all.glb)
//   wrist vs 43: p 23.107 -1.601 -0.033  q -0.549909 -0.456740 0.395045 0.576999 ; palm centre in the wrist 9.215 0.119 0.313
inline constexpr RestBone kCrossbowRestBones[] = {
    {7, {1.3674f, -1.9279f, -3.8891f}, {-0.712594f, 0.380325f, 0.048016f, 0.587586f}}, // kBone_L_Thumb1
    {8, {4.8183f, -4.3012f, -6.4079f}, {-0.700008f, 0.403021f, 0.066867f, 0.585740f}}, // kBone_L_Thumb2
    {9, {7.0962f, -5.9626f, -8.3423f}, {-0.634828f, 0.499430f, 0.150345f, 0.570052f}}, // kBone_L_Thumb3
    {10, {11.3943f, 0.6631f, -3.8253f}, {0.004491f, -0.017963f, -0.251132f, 0.967776f}}, // kBone_L_Index1
    {11, {15.1905f, -1.4508f, -3.6839f}, {0.009452f, -0.015921f, -0.518061f, 0.855143f}}, // kBone_L_Index2
    {12, {16.6975f, -4.3374f, -3.6272f}, {0.013466f, -0.012708f, -0.733228f, 0.679731f}}, // kBone_L_Index3
    {13, {12.0340f, 0.8465f, -0.9173f}, {-0.004146f, 0.010709f, -0.389651f, 0.920891f}}, // kBone_L_Middle1
    {14, {15.4227f, -2.6474f, -0.9976f}, {-0.008078f, 0.008162f, -0.724970f, 0.688685f}}, // kBone_L_Middle2
    {15, {15.2453f, -6.1005f, -0.9960f}, {-0.010177f, 0.005321f, -0.900011f, 0.435716f}}, // kBone_L_Middle3
    {16, {11.6518f, -0.3666f, 1.8425f}, {0.016926f, -0.037998f, -0.446994f, 0.893569f}}, // kBone_L_Ring1
    {17, {14.3481f, -3.9773f, 2.0807f}, {0.030063f, -0.028750f, -0.752269f, 0.657542f}}, // kBone_L_Ring2
    {18, {13.9125f, -7.2112f, 2.0565f}, {0.036739f, -0.019507f, -0.902541f, 0.428591f}}, // kBone_L_Ring3
    {19, {10.9949f, -0.5494f, 4.4626f}, {0.047325f, -0.092576f, -0.480501f, 0.870809f}}, // kBone_L_Pinky1
    {20, {12.9990f, -3.8015f, 4.9078f}, {0.067016f, -0.079491f, -0.664807f, 0.739744f}}, // kBone_L_Pinky2
    {21, {13.1854f, -5.5937f, 4.9592f}, {0.086206f, -0.058124f, -0.841827f, 0.529640f}}, // kBone_L_Pinky3
};

inline constexpr RestPose kRestPoses[] = {
    {L"Shotgun", {45.2663f, 7.4266f, 14.9802f}, {-0.800795f, -0.487867f, 0.110595f, 0.329367f}, {9.2150f, 0.1187f, 0.3125f}, kShotgunRestBones, 15},
    {L"MachineGun", {34.8275f, 0.2425f, 1.9122f}, {-0.897785f, -0.429240f, -0.010951f, 0.098058f}, {9.2150f, 0.1187f, 0.3125f}, kMachineGunRestBones, 15},
    {L"GrenadeLauncher", {47.0703f, 10.6158f, 11.4375f}, {-0.524410f, -0.669323f, 0.090250f, 0.518513f}, {9.2150f, 0.1187f, 0.3125f}, kGrenadeLauncherRestBones, 15},
    {L"ChemicalThrower", {8.2325f, -19.2118f, 29.0203f}, {0.223938f, 0.511365f, 0.111685f, 0.822122f}, {9.2150f, 0.1187f, 0.3125f}, kChemicalThrowerRestBones, 15},
    {L"Crossbow", {23.1066f, -1.6007f, -0.0333f}, {-0.549909f, -0.456740f, 0.395045f, 0.576999f}, {9.2150f, 0.1187f, 0.3125f}, kCrossbowRestBones, 15},
};

// The rest pose for a gun's class, or null (a gun with no two-handed idle clip).
inline const RestPose* rest_pose(const wchar_t* cls) {
    for (const RestPose& r : kRestPoses)
        if (same(r.cls, cls)) return &r;
    return nullptr;
}

} // namespace bvr::b1r::gun_geometry
