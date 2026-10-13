#pragma once
// Opt-in creation-time metadata. The D3D object's private data owns the copy;
// a recycled COM address can never inherit an earlier shader's bytecode.
#include <cstddef>
#include <cstdint>
#include <vector>
struct ID3D11Device;
struct ID3D11DeviceChild;
namespace bvr::shader_bytecode {
struct InputElement {
    char semantic[64];
    uint32_t index, format, slot, offset, classification, step;
};
// Call before device creation. Only calls originating in this module qualify.
void configure(const void* moduleBase, size_t moduleBytes);
bool install(ID3D11Device* sampleDevice);
// At first Present, additionally restrict collection to this exact device.
void bind_device(ID3D11Device* device);
void stop();
bool read(ID3D11DeviceChild* object, std::vector<uint8_t>& bytes);
struct Stats { uint32_t stored, refused, failed; uint64_t bytes; bool hooks; };
Stats stats();
} // namespace bvr::shader_bytecode
