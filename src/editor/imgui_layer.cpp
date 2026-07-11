#include "imgui_layer.h"
#include "../core/log.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace ae {

namespace {

ImFont* g_fontBold = nullptr;

LRESULT msgHook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
}

// A UE-flavoured dark theme with Aether Engine's violet accent.
void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 16.0f;
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 8.0f;
    s.TabBarBorderSize = 2.0f;
    s.DockingSeparatorSize = 2.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 2.0f;

    ImVec4* c = s.Colors;
    const ImVec4 bg0(0.055f, 0.055f, 0.065f, 1.00f); // deepest (viewport gutter)
    const ImVec4 bg1(0.090f, 0.090f, 0.105f, 1.00f); // panel background
    const ImVec4 bg2(0.130f, 0.130f, 0.150f, 1.00f); // fields / child panels
    const ImVec4 bg3(0.180f, 0.180f, 0.210f, 1.00f); // hovered fields
    const ImVec4 acc(0.55f, 0.45f, 0.96f, 1.00f);    // Aether Engine violet
    const ImVec4 accDim(0.36f, 0.30f, 0.62f, 1.00f);
    const ImVec4 accDark(0.24f, 0.20f, 0.42f, 1.00f);

    c[ImGuiCol_Text] = ImVec4(0.88f, 0.89f, 0.92f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.50f, 0.55f, 1.00f);
    c[ImGuiCol_WindowBg] = bg1;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.075f, 0.09f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = bg2;
    c[ImGuiCol_FrameBgHovered] = bg3;
    c[ImGuiCol_FrameBgActive] = accDark;
    c[ImGuiCol_TitleBg] = bg0;
    c[ImGuiCol_TitleBgActive] = bg0;
    c[ImGuiCol_TitleBgCollapsed] = bg0;
    c[ImGuiCol_MenuBarBg] = bg0;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.05f, 0.06f, 0.60f);
    c[ImGuiCol_ScrollbarGrab] = bg3;
    c[ImGuiCol_ScrollbarGrabHovered] = accDim;
    c[ImGuiCol_ScrollbarGrabActive] = acc;
    c[ImGuiCol_CheckMark] = acc;
    c[ImGuiCol_SliderGrab] = accDim;
    c[ImGuiCol_SliderGrabActive] = acc;
    c[ImGuiCol_Button] = bg2;
    c[ImGuiCol_ButtonHovered] = bg3;
    c[ImGuiCol_ButtonActive] = accDark;
    c[ImGuiCol_Header] = accDark;
    c[ImGuiCol_HeaderHovered] = accDim;
    c[ImGuiCol_HeaderActive] = acc;
    c[ImGuiCol_Separator] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    c[ImGuiCol_SeparatorHovered] = accDim;
    c[ImGuiCol_SeparatorActive] = acc;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.2f, 0.2f, 0.24f, 0.6f);
    c[ImGuiCol_ResizeGripHovered] = accDim;
    c[ImGuiCol_ResizeGripActive] = acc;
    c[ImGuiCol_Tab] = bg0;
    c[ImGuiCol_TabHovered] = accDim;
    c[ImGuiCol_TabSelected] = bg1;
    c[ImGuiCol_TabSelectedOverline] = acc;
    c[ImGuiCol_TabDimmed] = bg0;
    c[ImGuiCol_TabDimmedSelected] = bg1;
    c[ImGuiCol_TabDimmedSelectedOverline] = accDark;
    c[ImGuiCol_DockingPreview] = ImVec4(acc.x, acc.y, acc.z, 0.55f);
    c[ImGuiCol_DockingEmptyBg] = bg0;
    c[ImGuiCol_PlotLines] = acc;
    c[ImGuiCol_PlotLinesHovered] = ImVec4(0.8f, 0.7f, 1.0f, 1.0f);
    c[ImGuiCol_PlotHistogram] = accDim;
    c[ImGuiCol_PlotHistogramHovered] = acc;
    c[ImGuiCol_TableHeaderBg] = bg0;
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(acc.x, acc.y, acc.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = acc;
    c[ImGuiCol_NavCursor] = acc;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.1f, 0.1f, 0.1f, 0.4f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

} // namespace

bool imguiInit(Window& window, const std::string& iniPath) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDockingWithShift = false;
    static std::string g_iniPath;
    g_iniPath = iniPath;
    io.IniFilename = g_iniPath.c_str();

    applyTheme();

    // Segoe UI matches the rest of the engine's Windows-native approach; fall
    // back to the bundled ProggyClean if the system font is missing.
    ImFont* base =
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    if (!base) io.Fonts->AddFontDefault();
    g_fontBold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 17.0f);

    if (!ImGui_ImplWin32_Init(window.hwnd())) {
        AE_ERROR("[ImGui] Win32 backend init failed");
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 450")) {
        AE_ERROR("[ImGui] OpenGL3 backend init failed");
        return false;
    }
    window.setMessageHook(&msgHook);
    AE_LOG("[ImGui] %s initialized (docking)", IMGUI_VERSION);
    return true;
}

void imguiShutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_fontBold = nullptr;
}

void imguiBeginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void imguiEndFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

ImFont* imguiBoldFont() { return g_fontBold; }

} // namespace ae
