// gpu-test-sources: src/game/bioshock1r/resize.cpp
// Exercise the production SetRes door with a synthetic x86 viewport/device. No game.
#include "gpu_test_util.h"
#include "game/bioshock1r/resize.h"
#include "game/bioshock1r/patterns.h"
#include <string>
namespace p = bvr::b1r::patterns;
namespace rs = bvr::b1r::resize;
namespace {
alignas(4) uint8_t viewport[p::kViewportFlagsOffset + 4] = {};
void* table[p::kDeviceSetResSlot + 1] = {};
void* device[1] = {table};
uint32_t width = 640, height = 480;
int calls = 0, resolved = 0, returned = 1;
bool live = true, argsOk = false;
std::wstring dataDir;
int& reentry() { return *reinterpret_cast<int*>(viewport + p::kViewportReentryOffset); }
int __fastcall setres_body(void* dev, void*, void* vp, int w, int h, int full, int bits, int a, int b) {
    ++calls;
    argsOk = dev == device && vp == viewport && w == 2000 && h == 2100 && full == 0 &&
             bits == 0 && a == 0 && b == 1 && reentry() == 12;
    return returned;
}
// Same entry shape, no copied game implementation; tail-call our own fixture function.
__declspec(naked) int __fastcall good(void*, void*, void*, int, int, int, int, int, int) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { push 0 }
    __asm { add esp, 4 }
    __asm { pop ebp }
    __asm { jmp setres_body }
}
__declspec(naked) int __fastcall wrong(void*, void*, void*, int, int, int, int, int, int) {
    __asm { xor eax, eax }
    __asm { ret 28 }
}
} // namespace
namespace bvr::log {
const wchar_t* data_dir() { return dataDir.c_str(); }
void write(const char* fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); printf("\n"); va_end(a);
}
}
namespace bvr::d3d11_hook { void* game_window() { return nullptr; } }
namespace bvr::hud { bool backbuffer_dims(unsigned* w, unsigned* h) { *w = width; *h = height; return true; } }
namespace bvr::b1r::input_drive {
bool resolve_engine_objects(void** client, void** vp) {
    ++resolved; *client = viewport; *vp = live ? viewport : nullptr; return live;
}
}
namespace bvr::pattern_scan {
bool is_memory_valid(const void* ptr, size_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    return ptr && VirtualQuery(ptr, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
        !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
        reinterpret_cast<uintptr_t>(ptr) + n <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}
}
int main() {
    wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
    dataDir = cwd;
    const std::wstring ini = dataDir + L"\\resize-test.ini";
    DeleteFileW(ini.c_str()); // This fixture's own file, never the player's ini.
    const auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    bvr::pattern_scan::ProcessImage image{base, nt->OptionalHeader.SizeOfImage};
    *reinterpret_cast<void**>(viewport + p::kViewportDeviceOffset) = device;
    reentry() = 11;
    table[p::kDeviceSetResSlot] = reinterpret_cast<void*>(&good);
    auto rva = [&](const void* ptr) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(base)); };
    auto overrides = [&](uint32_t vt, uint32_t fn) {
        wchar_t v[24]; swprintf_s(v, L"0x%X", vt);
        WritePrivateProfileStringW(L"Bioshock1R", L"RenderDeviceVtableRva", v, ini.c_str());
        swprintf_s(v, L"0x%X", fn);
        WritePrivateProfileStringW(L"Bioshock1R", L"RenderDeviceSetResRva", v, ini.c_str());
        rs::init(image, ini.c_str());
    };
    auto ask = [&] { rs::request(2000, 2100, "offline fixture"); rs::tick(); };
    rs::init(image, ini.c_str()); ask();
    gt::check(calls == 0 && reentry() == 11 && !rs::busy(), "default pins reject another in-image vtable and entry",
              "calls %d, counter %d, %s", calls, reentry(), rs::status());
    overrides(rva(table) + sizeof(void*), rva(reinterpret_cast<void*>(&good))); ask();
    gt::check(calls == 0 && reentry() == 11, "wrong vtable refuses before mutation", "calls %d, counter %d", calls, reentry());
    overrides(rva(table), rva(reinterpret_cast<void*>(&wrong))); ask();
    gt::check(calls == 0 && reentry() == 11, "wrong slot entry refuses before mutation", "calls %d, counter %d", calls, reentry());
    table[p::kDeviceSetResSlot] = reinterpret_cast<void*>(&wrong); ask();
    gt::check(calls == 0 && !rs::busy(), "matching override never bypasses the prologue", "calls %d, %s", calls, rs::status());
    table[p::kDeviceSetResSlot] = reinterpret_cast<void*>(&good);
    overrides(rva(table), rva(reinterpret_cast<void*>(&good)));
    live = false; ask();
    gt::check(calls == 0 && reentry() == 11, "a lost viewport is resolved again and refused", "calls %d, resolves %d", calls, resolved);
    live = true; ask();
    gt::check(calls == 1 && argsOk && reentry() == 11 && rs::busy(), "matching identity calls the ABI and restores reentry",
              "calls %d, ABI %d, counter %d", calls, (int)argsOk, reentry());
    rs::tick();
    gt::check(rs::busy(), "success is not confirmation until the backbuffer follows", "%s", rs::status());
    width = 2000; height = 2100; rs::tick();
    uint32_t w = 0, h = 0; rs::last_confirmed(&w, &h);
    gt::check(!rs::busy() && w == 2000 && h == 2100, "the observed backbuffer confirms the resize", "%ux%u %s", w, h, rs::status());
    width = 640; height = 480;
    WritePrivateProfileStringW(L"Bioshock1R", L"RenderDeviceSetResRva", L"invalid", ini.c_str());
    rs::init(image, ini.c_str()); ask();
    gt::check(calls == 1 && !rs::busy(), "malformed override fails closed", "calls %d, %s", calls, rs::status());
    overrides(rva(table), rva(reinterpret_cast<void*>(&good)));
    returned = 0; ask();
    gt::check(calls == 2 && !rs::busy() && reentry() == 11, "engine refusal restores reentry and releases the queue",
              "calls %d counter %d %s", calls, reentry(), rs::status());
    DeleteFileW(ini.c_str());
    return gt::finish("resize");
}
