#pragma once
#include "model/view_state.h"
namespace bvr::launcher::app {
inline constexpr int kWidth = 1100, kHeight = 850;
bool render(ViewState& state, const std::wstring& path, int width, int height, float scale, std::string* why);
int run(ViewState state, bool preview = false);
std::wstring operation_arguments(const Environment& env, const Choices& choices, const std::string& operation);
}
