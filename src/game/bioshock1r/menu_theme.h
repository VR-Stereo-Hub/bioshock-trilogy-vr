#pragma once
#include <imgui.h>
struct ID3D11Device;

namespace bvr::b1r::menu::theme {
void initialize(ID3D11Device* device);
void begin(float scale, float textScale);
void end();
void background();
void title();
void subtitle();
void rule();
void note(const char* text);
bool button(const char* label, ImVec2 size = {}, bool selected = false);
bool slider(const char* id, float* value, float minimum, float maximum, const char* format);
bool section(const char* label);
float unit(float value);
bool assets_loaded();
} // namespace bvr::b1r::menu::theme
