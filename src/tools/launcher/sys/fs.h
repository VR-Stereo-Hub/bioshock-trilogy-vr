// Adapted from the Dishonored VR launcher; see NOTICE.md for provenance and license.
// tools/installer/sys/fs.h - files, folders, hashes and strings for the launcher.
// Every path is a std::wstring and every call is the W API: a Steam library can
// sit under a user name that is not ASCII. Nothing here touches the mod's own
// paths.cpp (that derives the game folder from the DLL's HINSTANCE).
#pragma once
#include <windows.h>
#include <stdint.h>
#include <shlobj.h>
#include <string>
#include <vector>

namespace bvr::launcher::fs {

bool exists(const std::wstring& path);
bool is_dir(const std::wstring& path);
bool is_file(const std::wstring& path);
bool make_dir(const std::wstring& path, DWORD* err);          // one level; ok when it exists
bool make_dirs(const std::wstring& path, DWORD* err);         // creates missing parents
std::wstring join(const std::wstring& a, const std::wstring& b);
std::wstring parent(const std::wstring& path);
std::wstring filename(const std::wstring& path);
std::wstring strip_trailing_slashes(std::wstring path);

bool read_file(const std::wstring& path, std::vector<uint8_t>* out, DWORD* err);
// Writes <path>.tmp then moves it over <path>, so a half-written d3d9.dll can
// never be the one the game loads.
bool write_file_atomic(const std::wstring& path, const void* data, size_t size, DWORD* err);
bool copy_file(const std::wstring& from, const std::wstring& to, DWORD* err);   // overwrites
bool move_file(const std::wstring& from, const std::wstring& to, DWORD* err);   // overwrites
bool delete_file(const std::wstring& path, DWORD* err);        // a missing file is success
// Can this process create a file in the folder? Probes with CREATE_NEW +
// DELETE_ON_CLOSE so nothing is left behind. err is the Win32 reason on false.
bool probe_writable(const std::wstring& dir, DWORD* err);
uint64_t file_size(const std::wstring& path);                   // 0 when missing

bool sha256_bytes(const void* data, size_t size, std::string* hexLower);
bool sha256_file(const std::wstring& path, std::string* hexLower, DWORD* err);
// Does the byte range contain the ASCII needle? (the legacy-build marker check)
bool contains_ascii(const void* data, size_t size, const char* needle);

std::wstring known_folder(const KNOWNFOLDERID& id);            // "" when unknown
std::wstring env(const wchar_t* name);                          // "" when unset
std::wstring system_dir();                                      // SysWOW64 for this process
std::wstring temp_dir();
std::wstring module_path();                                     // this exe
std::wstring timestamp_local();                                 // yyyyMMdd-HHmmss
std::string  utc_now_iso();                                     // 2026-09-23T00:00:00Z

std::string  narrow(const std::wstring& s);                     // UTF-8
std::wstring widen(const std::string& s);                       // from UTF-8
std::string  narrow_acp(const std::wstring& s);                 // for the mod's char APIs
std::wstring win_error_text(DWORD err);                         // "Access is denied. (5)"
std::string  format(const char* fmt, ...);
std::wstring wformat(const wchar_t* fmt, ...);
bool iequals(const std::wstring& a, const std::wstring& b);

} // namespace bvr::launcher::fs
