// Adapted from the Dishonored VR launcher; see NOTICE.md for provenance and license.
#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <vector>
#include <cstdint>
namespace bvr::launcher::updates {
struct Release {
    std::string version, notes, published, assetUrl, sha256;
    uint64_t size=0;
    bool downloadable() const { return !assetUrl.empty() && sha256.size()==64 && size>0; }
};
struct Check { std::vector<Release> releases; std::string message; bool online=false; };
bool version(const std::string& text, uint32_t (&parts)[3]);
bool newer(const std::string& candidate,const std::string& current);
bool parse_releases(const std::string& json,std::vector<Release>* out,std::string* error);
std::wstring data_dir();
Check check(const std::atomic<bool>* cancel=nullptr);
bool verify(const std::wstring& file,const Release& release,std::string* error);
bool download(const Release& release,std::wstring* file,std::string* error,const std::atomic<bool>* cancel=nullptr);
bool start(const std::wstring& exe,const std::wstring& args,DWORD* error);
}
