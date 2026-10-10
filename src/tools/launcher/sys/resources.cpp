#include "sys/resources.h"
#include "model/launcher.h"
#include "sys/fs.h"
#include "payload_ids.h"
#include "bvr_version.h"
#include <windows.h>

namespace bvr::launcher {
std::vector<unsigned char> resource_bytes(int id) {
    auto module = GetModuleHandleW(nullptr);
    auto entry = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!entry) return {};
    const auto size = SizeofResource(module, entry);
    const auto data = static_cast<const unsigned char*>(LockResource(LoadResource(module, entry)));
    if (!data || !size) return {};
    return {data, data + size};
}
Payload embedded_payload() {
    Payload p;
    p.version = BVR_VERSION; p.build = BVR_BUILD_ID; p.configuration = BVR_LAUNCHER_CONFIG;
    const struct { int id; const wchar_t* name; } files[] = {
        {IDR_PROXY, L"xinput1_3.dll"}, {IDR_MOD, L"bioshockvr.dll"},
        {IDR_SHIM, L"bvr_steamvr32.dll"}, {IDR_OPENVR, L"openvr_api.dll"}
    };
    for (const auto& f : files) {
        PayloadFile entry; entry.name = f.name; entry.bytes = resource_bytes(f.id);
        if (entry.bytes.empty() || !fs::sha256_bytes(entry.bytes.data(), entry.bytes.size(), &entry.hash)) {
            p.error = "The embedded installation files are incomplete.";
            return p;
        }
        p.files.push_back(std::move(entry));
    }
    p.valid = p.configuration == "RelWithDebInfo" || p.configuration == "Release";
    if (!p.valid) p.error = "A Debug launcher cannot install a play build.";
    return p;
}
}
