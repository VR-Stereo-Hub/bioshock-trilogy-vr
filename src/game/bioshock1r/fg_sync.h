#pragma once
// The player-scene ctor's camera substitution (legacy Fg name), shared with the offline game.
// Addresses and hooking stay in scenedraw; a failed lookup leaves every argument intact.
#include <cstdint>
namespace bvr::b1r::fg_sync {
template <class Lookup>
int substitute(bool enabled, bool stereo, int alternateSign, bool secondPass,
               float loc[3], int32_t rot[3], Lookup lookup) {
    if (!enabled || (!stereo && alternateSign == 0)) return 0;
    const int eye = !stereo && alternateSign ? (alternateSign > 0 ? 1 : 0) : (secondPass ? 1 : 0);
    float l[3]; int32_t r[3];
    if (!lookup(eye, l, r)) return -1;
    for (int i = 0; i < 3; ++i) { loc[i] = l[i]; rot[i] = r[i]; }
    return 1;
}
} // namespace bvr::b1r::fg_sync
