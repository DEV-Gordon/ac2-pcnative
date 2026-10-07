#pragma once
#include "imgui.h"

// Shared look for the launcher and the in-game overlay.
namespace ui {
extern ImFont* body;
extern ImFont* bold;
extern ImFont* title;

// Call after ImGui::CreateContext(). scale = UI scale factor (1.0 for 720p).
void setup(float scale);

constexpr ImU32 kAccent = IM_COL32(64, 156, 255, 255);
inline ImVec4 accent() { return ImVec4(0.25f, 0.61f, 1.0f, 1.0f); }
inline ImVec4 ok() { return ImVec4(0.38f, 0.85f, 0.52f, 1.0f); }
inline ImVec4 warn() { return ImVec4(1.0f, 0.75f, 0.30f, 1.0f); }
inline ImVec4 bad() { return ImVec4(1.0f, 0.45f, 0.40f, 1.0f); }
inline ImVec4 dim() { return ImVec4(0.58f, 0.64f, 0.72f, 1.0f); }

// Small "(?)" marker with a tooltip.
void help(const char* text);
// Section header with a thin accent line.
void section(const char* text);
}
