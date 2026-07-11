// Aether Engine — Dear ImGui platform layer: context creation, the Win32
// message bridge into our own Window, per-frame begin/end (including
// ImGuizmo), and the editor's dark theme + fonts.
#pragma once
#include "../core/window.h"
#include <string>

struct ImFont;

namespace ae {

// iniPath: where ImGui persists the dock layout (kept per-project).
bool imguiInit(Window& window, const std::string& iniPath = "editor_layout.ini");
void imguiShutdown();
void imguiBeginFrame(); // NewFrame for backends + ImGui + ImGuizmo
void imguiEndFrame();   // Render + draw to the currently bound framebuffer

ImFont* imguiBoldFont(); // heavier face for headers/toolbar (may be null)

} // namespace ae
