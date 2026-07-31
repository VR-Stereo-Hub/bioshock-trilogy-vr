#pragma once
// Process-wide value scanning / poking for live discovery of engine globals
// (CheatEngine-style narrowing, driven through the command-file seam). Debug
// tooling only: results are logged, never baked in - anything found here
// graduates into game/<title>/patterns.cpp with a documented derivation
// before production use. All entry points run on the game thread.

#include <cstddef>
#include <cstdint>

namespace bvr::value_scan {

// Fresh sweep of all committed writable private/image memory (our own DLL
// excluded) for the exact 32-bit pattern of `value` at 4-byte alignment.
// Replaces the candidate set. Returns the candidate count. The _u32 variants
// scan for integer-typed values (UE2.5 int config properties - an ini line
// like "HorizontalFOV=130" with no decimals is an int property, invisible to
// a float scan).
size_t scan_f32(float value);
size_t scan_u32(uint32_t value);

// Keep only candidates whose current value equals `value` exactly.
size_t rescan_f32(float value);
size_t rescan_u32(uint32_t value);

size_t count();

// Log up to n candidates: index, address, module+RVA or heap, current value.
void list(size_t n);

// Log and return candidate idx's current value (NaN on failure).
float read_at(size_t idx);

// Write `value` to candidate idx / candidates [lo, hi] inclusive. Original
// bits are remembered (obfuscated so they never match a later scan) for
// restore_all. Returns success / number written.
bool poke(size_t idx, float value);
bool poke_u32(size_t idx, uint32_t value);
size_t poke_range(size_t lo, size_t hi, float value);
size_t poke_range_u32(size_t lo, size_t hi, uint32_t value);

// Restore every poked address to its remembered bits. Returns count restored.
size_t restore_all();

// Write to an arbitrary address (validated + SEH-guarded).
bool poke_addr(uintptr_t addr, float value);
bool poke_addr_u32(uintptr_t addr, uint32_t value);

// SEH-guarded read of an arbitrary address. For per-frame liveness probes on a
// candidate address a scan nominated: a heap object can be freed between
// rounds, and writing through a stale pointer every frame is a wild write.
// False means "do not touch this address again".
bool safe_read_f32(uintptr_t addr, float* out);

// Log a hex+ascii dump of [addr, addr+len), len capped at 1024.
void hexdump(uintptr_t addr, size_t len);

// Log every 4-aligned float field in [addr, addr+len) whose value lies in
// [lo, hi] (offset + value). Property hunting on live objects; len capped 4K.
void float_sweep(uintptr_t addr, size_t len, float lo, float hi);

// For candidate idx, sweep the main module and writable heap for 32-bit
// values P with 0 <= candidate - P <= maxDelta (plausible owning-object base
// pointers). Logs each hit's location and the implied field offset.
size_t ptr_scan(size_t idx, uint32_t maxDelta);

// Log the main module and mod DLL base/size (for RVA arithmetic in logs).
void log_module_bases();

// Sweep the main module for `text` as both ANSI and UTF-16LE, log each hit's
// RVA and how many code immediates reference it (anchor hunting for a future
// signature). Read-only.
void log_string_scan(const char* text);

} // namespace bvr::value_scan
