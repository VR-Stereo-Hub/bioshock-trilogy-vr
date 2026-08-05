#pragma once

#include <cstring>
#include <cstdlib>

namespace bvr::diag {

// BVR_SKIP: comma-separated subsystem tokens to leave uninstalled, e.g.
//   BVR_SKIP=inspector,overlay
// Diagnostic-only (session 38: used to bisect the BS2 teardown fault to a
// subsystem with no rebuild per step). Unset = everything installs; nothing
// in shipping behavior reads this outside init paths.
inline bool skip(const char* token) {
    const char* list = std::getenv("BVR_SKIP");
    if (!list || !*list) return false;
    const size_t n = std::strlen(token);
    for (const char* p = list; *p;) {
        const char* end = std::strchr(p, ',');
        size_t len = end ? static_cast<size_t>(end - p) : std::strlen(p);
        if (len == n && std::strncmp(p, token, n) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

} // namespace bvr::diag
