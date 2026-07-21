# Aether Engine — engine core DLL + editor / game-runtime executables.
#
#   ae_core        (AetherCore.dll)   engine runtime: core/gl/render/engine/ui/narrative
#   imgui_lib      (static)           vendored Dear ImGui docking + ImGuizmo
#   AetherEditor   (exe)              ImGui editor shell around ae_core
#   AetherRuntime  (exe)              standalone game player (no editor deps)
#
# Everything lands in build/bin/<config>/ so AetherCore.dll sits next to every
# exe — the same layout a packaged game or engine install uses.
cmake_minimum_required(VERSION 3.16)
project(AetherEngine C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Overridable so the release workflow can stamp the tag's version into the
# build: cmake -DAE_VERSION=x.y.z (the hub's updater compares this to GitHub).
set(AE_VERSION "0.1.0" CACHE STRING "Engine version (x.y.z)")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUIZMO_DIR ${CMAKE_SOURCE_DIR}/third_party/ImGuizmo)

# ---- Jolt Physics (fetched, built as a static lib linked into AetherCore) ---
# Must match our toolchain: keep our compile flags (don't let Jolt force /MT or
# its own flags) so the CRT (/MD) is consistent across the DLL boundary.
include(FetchContent)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.5.0
    GIT_SHALLOW ON
    SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(JoltPhysics)

# ---- Recast/Detour navigation (fetched, static libs linked into AetherCore) -
# The same navmesh library Unreal uses: Recast voxelizes world triangles into
# walkable polygons, Detour answers path queries over them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5) # v1.6.0 predates CMake 4's floor
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG v1.6.0
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(recastnavigation)

# ---- SDL3 (portable window/input/audio + the Android bootstrap) -------------
# The runtime host window is SDL3 so games run on Windows/Linux/macOS/Android
# from one path; the editor keeps its bespoke Win32 window (desktop-only).
# Static so the runtime ships as a single exe; minimal build (no tests).
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.0
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(SDL3)

# ---- Vulkan (the unifying backend: Windows/Linux/Android/Switch; macOS via
# MoltenVK). Headers only + the volk meta-loader (dynamic entry-point loading
# through SDL's Vulkan loader), so no LunarG SDK / vulkan-1 import lib needed. -
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.3.283
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(VulkanHeaders)

add_library(volk STATIC third_party/volk/volk.c)
target_link_libraries(volk PUBLIC Vulkan::Headers)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
target_include_directories(volk PUBLIC ${CMAKE_SOURCE_DIR}/third_party/volk)

# ---- glslang: GLSL -> SPIR-V (no system Vulkan SDK on the dev box). Minimal
# build: no optimizer (drops the SPIRV-Tools dep), no binaries, no tests. -----
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG vulkan-sdk-1.3.283.0
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(glslang)

# ---- engine core (AetherCore.dll) -----------------------------------------
add_library(ae_core SHARED
    src/core/math3d.cpp
    src/core/json.cpp
    src/core/log.cpp
    src/core/guid.cpp
    src/core/capture.cpp
    src/core/paths.cpp
    src/core/process.cpp
    src/core/gamepad.cpp
    src/core/http.cpp
    src/ai_assist/pulse_client.cpp
    src/ai_assist/dev_team.cpp
    src/gl/gl_api.cpp
    src/rhi/rhi_gl.cpp
    src/render/shader.cpp
    src/render/mesh.cpp
    src/render/texture.cpp
    src/render/image.cpp
    src/render/gltf.cpp
    src/render/renderer.cpp
    src/engine/world.cpp
    src/engine/assets.cpp
    src/engine/scene_io.cpp
    src/engine/mission.cpp
    src/engine/project.cpp
    src/engine/component_registry.cpp
    src/engine/input_actions.cpp
    src/engine/particles.cpp
    src/engine/anim_graph.cpp
    src/engine/anim_ik.cpp
    src/engine/data_table.cpp
    src/engine/demo_input.cpp
    src/engine/doc_gen.cpp
    src/engine/agent_bridge_help.cpp
    src/engine/save_game.cpp
    src/engine/engine_modules.cpp
    src/engine/game_module.cpp
    src/engine/plugin_manager.cpp
    src/engine/module_build.cpp
    src/engine/packager.cpp
    src/physics/physics.cpp
    src/ai/nav_mesh.cpp
    src/ai/behavior_tree.cpp
    src/audio/audio.cpp
    src/script/script_graph.cpp
    src/render/material_graph.cpp
    src/render/debug_draw.cpp
    src/render/model_obj.cpp
    src/render/model_fbx.cpp
    third_party/ufbx/ufbx.c
    src/ui/font.cpp
    src/ui/ui_document.cpp
    src/ui/ui.cpp
    src/narrative/choice_ui.cpp
    src/narrative/dialogue_scene.cpp
    src/narrative/dialogue_player.cpp
    src/narrative/mission_hud.cpp
)
target_compile_definitions(ae_core PRIVATE AE_BUILD_CORE)
target_include_directories(ae_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ae_core PUBLIC opengl32 user32 gdi32
                              PRIVATE ole32 shlwapi windowscodecs uuid dwmapi xaudio2 xinput9_1_0 Jolt
                              PRIVATE Recast Detour DetourCrowd DetourTileCache)
# Classes/functions are exported wholesale; global data additionally carries
# AE_API (see src/core/ae_api.h).
set_target_properties(ae_core PROPERTIES
    OUTPUT_NAME AetherCore
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    # Import lib next to the DLL so a packaged install can ship it as the SDK.
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ---- vendored Dear ImGui (docking) + ImGuizmo ------------------------------
add_library(imgui_lib STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUIZMO_DIR}/src/ImGuizmo.cpp
)
target_include_directories(imgui_lib PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUIZMO_DIR}/src)
target_compile_definitions(imgui_lib PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui_lib PUBLIC opengl32 user32 gdi32)

# ---- editor ----------------------------------------------------------------
add_executable(AetherEditor
    src/main_editor.cpp
    src/core/window.cpp          # Win32 window (editor's custom chrome)
    src/editor/imgui_layer.cpp
    src/editor/editor.cpp
    src/editor/inspector_visitor.cpp
    src/editor/dialogue_graph.cpp
    src/editor/script_graph_panel.cpp
    src/editor/material_graph_panel.cpp
    src/editor/ui_designer_panel.cpp
    src/editor/input_map_panel.cpp
    src/editor/anim_graph_panel.cpp
    src/editor/data_table_panel.cpp
    src/editor/ai_panel.cpp
    src/editor/mission_panel.cpp
    src/editor/agent_bridge.cpp
    src/editor/bridge_commands.cpp
)
target_link_libraries(AetherEditor PRIVATE ae_core imgui_lib comdlg32 ws2_32)

# ---- game runtime ----------------------------------------------------------
# /SUBSYSTEM:WINDOWS so packaged games never flash a console window; the entry
# point stays a plain main() via /ENTRY, and the runtime re-attaches to a
# parent console for dev/CI output.
add_executable(AetherRuntime WIN32
    src/main_runtime.cpp
    src/core/window_sdl.cpp      # SDL3 window (portable: Win/Linux/macOS/Android)
    src/rhi/vulkan_probe.cpp     # Vulkan bring-up (foundation for rhi_vulkan)
    src/rhi/vulkan_context.cpp   # Vulkan device/swapchain/frame loop (rhi_vulkan core)
    src/rhi/spirv_compile.cpp    # GLSL -> SPIR-V (glslang) for the Vulkan backend
)
target_link_libraries(AetherRuntime PRIVATE ae_core SDL3::SDL3-static volk
                      glslang SPIRV glslang-default-resource-limits)
target_compile_definitions(AetherRuntime PRIVATE VK_NO_PROTOTYPES)
if(MSVC)
    target_link_options(AetherRuntime PRIVATE /ENTRY:mainCRTStartup)
endif()

# ---- launcher hub ------------------------------------------------------------
# Lists engine installs + projects (%APPDATA%/AetherEngine/launcher.json),
# creates projects from Templates/, opens them in the right editor.
add_executable(AetherHub WIN32
    src/hub/hub_main.cpp
    src/hub/launcher_state.cpp
    src/hub/engine_update.cpp
    src/core/window.cpp          # Win32 window (hub shares the editor's chrome)
    src/editor/imgui_layer.cpp
)
target_link_libraries(AetherHub PRIVATE ae_core imgui_lib comdlg32 shell32)
if(MSVC)
    target_link_options(AetherHub PRIVATE /ENTRY:mainCRTStartup)
endif()

if(MSVC)
    foreach(tgt ae_core imgui_lib AetherEditor AetherRuntime AetherHub)
        target_compile_options(${tgt} PRIVATE /W3 /permissive- /MP)
    endforeach()
endif()

# ---- headless smoke tests (console) ----------------------------------------
add_executable(AetherPhysSmoke tools/phys_smoke.cpp)
target_link_libraries(AetherPhysSmoke PRIVATE ae_core)

add_executable(AetherUndoSmoke tools/undo_smoke.cpp)
target_link_libraries(AetherUndoSmoke PRIVATE ae_core)

add_executable(AetherInputSmoke tools/input_smoke.cpp)
target_link_libraries(AetherInputSmoke PRIVATE ae_core)

add_executable(AetherSaveSmoke tools/save_smoke.cpp)
target_link_libraries(AetherSaveSmoke PRIVATE ae_core)

add_executable(AetherNavSmoke tools/nav_smoke.cpp)
target_link_libraries(AetherNavSmoke PRIVATE ae_core)

add_executable(AetherDocGen tools/doc_gen.cpp)
target_link_libraries(AetherDocGen PRIVATE ae_core)

add_executable(AetherAiSmoke tools/ai_smoke.cpp)
target_link_libraries(AetherAiSmoke PRIVATE ae_core)

# ---- engine install manifest ------------------------------------------------
# Identifies an engine install to the launcher/hub and records the SDK paths a
# project's GameModule build needs (dev layout: headers straight from src/).
file(GENERATE OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/engine.json CONTENT
"{
  \"name\": \"AetherEngine\",
  \"version\": \"${AE_VERSION}\",
  \"toolset\": \"${MSVC_TOOLSET_VERSION}\",
  \"sdkInclude\": \"${CMAKE_SOURCE_DIR}/src\",
  \"sdkLib\": \"$<TARGET_LINKER_FILE_DIR:ae_core>\"
}
")

# Shaders belong to the engine install: keep a copy next to the binaries.
add_custom_command(TARGET ae_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:ae_core>/shaders
)
