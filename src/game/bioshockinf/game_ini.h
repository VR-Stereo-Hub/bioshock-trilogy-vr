#pragma once
// BioShock Infinite's resolution config lane (I6, session 41).
//
// THE AUTHORITATIVE STORE IS XUserOptions.ini (DR-I8, measured session 37):
//   %USERPROFILE%\Documents\My Games\BioShock Infinite\XGame\Config\
//   XUserOptions.ini, section [XCore.XUserOptionsManager], keys
//   ResolutionX / ResolutionY - honoured at the first Present after a boot.
// XEngine.ini's ResX/ResY is a boot-time DERIVED COPY: a write there is
// silently overwritten (measured - the fourth instance of "a verified write
// is not an honoured one"). This module NEVER touches XEngine.ini.
//
// Unlike BS2 (no live Exec seam - ini + relaunch only), Infinite ALSO has a
// live lane: `setres WxH` through reflect::exec_console resizes the
// backbuffer within 20 ms and the XR swapchain rebuild survives it (s38).
// The picker uses BOTH: live setres for this session, this ini write so the
// next boot agrees.
//
// Write discipline (BS1/BS2's proven chain, facts derived fresh):
//  - section-scoped in-place edit; the target keys exist in the shipped
//    template, so this NEVER ADDS a key, only replaces values in place
//  - one-time backup XUserOptions.ini.bvr-bak-res beside the original
//  - temp file + ReplaceFileW (atomic swap, original ACLs kept)
//  - read-back after write is LOGGED but is NOT acceptance: the only
//    acceptance that means anything is the backbuffer at first Present
//    after a relaunch (bvr::hud::backbuffer_dims).

#include <cstdint>

namespace bvr::bsi::game_ini {

struct Resolution {
    int x = 0, y = 0;
    bool valid = false;
};

// Read ResolutionX/Y from [XCore.XUserOptionsManager]. Fail-soft: a missing
// file or section returns valid=false.
Resolution read_resolution();

// Replace ResolutionX/Y in place (section-scoped, never adds keys). Range
// guard 640x480..16384x16384. Returns false on any failure; logs the
// read-back either way, with the not-acceptance caveat.
bool write_resolution(int w, int h);

// One line comparing the ini values against the live backbuffer.
void log_status(unsigned liveW, unsigned liveH);

} // namespace bvr::bsi::game_ini
