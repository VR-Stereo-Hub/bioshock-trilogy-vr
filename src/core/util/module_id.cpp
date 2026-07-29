#include "core/util/module_id.h"

#include <windows.h>

namespace bvr::module_id {
namespace {

const IMAGE_NT_HEADERS* nt_headers(HMODULE mod) {
    if (!mod) return nullptr;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(mod) +
                                                         dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

} // namespace

Fingerprint host_exe() {
    Fingerprint f{};
    HMODULE host = GetModuleHandleW(nullptr);
    const IMAGE_NT_HEADERS* nt = nt_headers(host);
    if (!nt) return f;

    f.timeDateStamp = nt->FileHeader.TimeDateStamp;
    f.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    f.checkSum = nt->OptionalHeader.CheckSum;

    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
            f.fileBytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }
    f.valid = true;
    return f;
}

bool matches(const Fingerprint& f, uint32_t timeDateStamp, uint32_t sizeOfImage,
             uint64_t fileBytes) {
    if (!f.valid) return false;
    if (f.timeDateStamp != timeDateStamp) return false;
    if (f.sizeOfImage != sizeOfImage) return false;
    // A zero expected size means "not recorded"; the two PE fields already pin
    // the link, and the on-disk size is the extra signal that catches a repack.
    if (fileBytes && f.fileBytes && f.fileBytes != fileBytes) return false;
    return true;
}

} // namespace bvr::module_id
