// core/vr/apilayer_guard.h
//
// See apilayer_guard.cpp. Call once from init(), before anything can reach
// xrCreateInstance.
#pragma once

namespace bvr::vr {

void apilayer_guard();

} // namespace bvr::vr
