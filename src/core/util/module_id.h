#pragma once
// Identity of a loaded module, for deciding whether hardcoded addresses derived
// from one build may be trusted against the binary actually running.
// Game-agnostic: the expected values live in each game's patterns header.
//
// WHY (session 27). Adapters are selected by exe BASENAME, and every storefront
// ships BioShock Remastered as the same `BioshockHD.exe`. So an Epic, GOG or
// repatched build is not rejected - it is DETECTED as supported, and then
// roughly a hundred absolute RVAs from the 2022-04-13 Steam build are applied to
// a different binary. Hooking paths mostly survive that because they are
// prologue- or vtable-gated and refuse, but the object-scan KEYS degrade to
// "search memory for an arbitrary constant and then write to whatever passes a
// plausibility test", which is the worst available failure mode.

#include <cstdint>

namespace bvr::module_id {

struct Fingerprint {
    uint32_t timeDateStamp = 0; // PE FileHeader; the authoritative build id
    uint32_t sizeOfImage = 0;   // PE OptionalHeader
    uint32_t checkSum = 0;      // PE OptionalHeader; 0 in many linker configs
    uint64_t fileBytes = 0;     // on-disk size, catches a repack at the same link
    bool valid = false;         // false if the PE headers were unreadable
};

// Fingerprint of the process's main module.
Fingerprint host_exe();

// True when `f` matches the expected build. checkSum is compared only when both
// sides have a non-zero value, because the shipped BioShock 1 exe carries 0.
bool matches(const Fingerprint& f, uint32_t timeDateStamp, uint32_t sizeOfImage,
             uint64_t fileBytes);

} // namespace bvr::module_id
