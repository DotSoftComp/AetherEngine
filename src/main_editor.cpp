// Aether Engine — editor entry point (AetherEditor.exe).
//
//   AetherEditor --project <dir|.aeproj>           open a project
//   AetherEditor                                   open the Sample template (dev fallback)
//   AetherEditor ... --map assets/maps/foo.json    open a specific map instead of the startup scene
//   AetherEditor ... --play                        auto-enter Play mode
//   AetherEditor ... --frames N                    timed benchmark (vsync off)
//   AetherEditor ... --frames N --screenshot out.bmp   headless capture
//
// The editor wraps the same World the game runtime plays with the ImGui
// workspace (viewport, outliner, details, content browser, dialogue graph,
// missions) and a real play-in-editor snapshot/restore.
#include "core/window.h"
#include "core/log.h"
#include "core/capture.h"
#include "core/paths.h"
#include "render/renderer.h"
#include "engine/world.h"
#include "engine/assets.h"
#include "engine/scene_io.h"
#include "engine/project.h"
#include "engine/component_registry.h"
#include "engine/game_module.h"
#include "engine/engine_modules.h"
#include "rhi/rhi.h"
#include "engine/plugin_manager.h"
#include "engine/module_build.h"
#include "engine/packager.h"
#include "ui/font.h"
#include "editor/editor.h"
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

using namespace ae;

int main(int argc, char** argv) {
    int captureFrames = -1;
    const char* screenshotPath = nullptr;
    const char* mapPath = nullptr;
    const char* projectArg = nullptr;
    const char* resavePath = nullptr; // headless load→save serialization check
    bool autoPlay = false;
    bool compileOnly = false;         // headless script compile (CI / verification)
    const char* packageDir = nullptr; // headless Build Game into this folder
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--play")) autoPlay = true;
        if (!std::strcmp(argv[i], "--project") && i + 1 < argc) projectArg = argv[++i];
        if (!std::strcmp(argv[i], "--map") && i + 1 < argc) mapPath = argv[++i];
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) captureFrames = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshotPath = argv[++i];
        if (!std::strcmp(argv[i], "--resave") && i + 1 < argc) resavePath = argv[++i];
        if (!std::strcmp(argv[i], "--compile")) compileOnly = true;
        if (!std::strcmp(argv[i], "--package") && i + 1 < argc) packageDir = argv[++i];
    }
    if (screenshotPath && captureFrames < 0) captureFrames = 60;

    // --newproject <templateDir> <destDir> <name>: create from template, then
    // open the new project (also used headlessly by tooling).
    for (int i = 1; i + 3 < argc; ++i) {
        if (!std::strcmp(argv[i], "--newproject")) {
            std::string projFile, err;
            if (!createProjectFromTemplate(argv[i + 1], argv[i + 2], argv[i + 3], "0.1.0",
                                           &projFile, &err)) {
                std::fprintf(stderr, "New project failed: %s\n", err.c_str());
                return 1;
            }
            std::printf("Created project: %s\n", projFile.c_str());
            projectArg = argv[i + 2];
            break;
        }
    }

    Project project;
    if (projectArg) {
        if (!project.load(projectArg)) {
            std::fprintf(stderr, "Cannot open project: %s\n", projectArg);
            return 1;
        }
    } else {
        // Dev convenience: no --project falls back to the Sample template.
        std::string sample = joinPath(engineTemplatesDir(), "Sample");
        if (!project.load(sample)) {
            std::fprintf(stderr,
                         "No --project given and no Sample template found.\n"
                         "Usage: AetherEditor --project <dir or .aeproj>\n");
            return 1;
        }
        AE_WARN("[Editor] no --project given — opening the Sample template (%s)",
                project.root.c_str());
    }

    // Headless script compile: build the game module and exit (no window/GL).
    if (compileOnly) {
        bool ok = buildGameModule(project, "Release",
                                  [](const std::string& l) { std::printf("%s\n", l.c_str()); });
        return ok ? 0 : 1;
    }
    // Headless packaging: same pipeline as File > Build Game.
    if (packageDir) {
        PackageOptions opts;
        opts.outputDir = packageDir;
        bool ok = packageGame(project, opts,
                              [](const std::string& l) { std::printf("%s\n", l.c_str()); });
        return ok ? 0 : 1;
    }

    Window window;
    // The editor uses custom borderless chrome (its own title bar); headless
    // screenshot captures keep a plain system window.
    WindowChrome chrome =
        (screenshotPath || resavePath) ? WindowChrome::System : WindowChrome::Borderless;
    if (!window.create("Aether Editor", 1600, 900, chrome)) {
        std::fprintf(stderr, "Failed to create window / GL 4.5 context\n");
        return 1;
    }
    rhi::init();

    Renderer renderer;
    if (!renderer.init(window.width(), window.height())) {
        std::fprintf(stderr, "Renderer init failed\n");
        return 1;
    }
    window.setVSync(screenshotPath == nullptr);
    renderer.settings.vsync = screenshotPath == nullptr;

    engineModules().configure(project.moduleFlags);
    registerBuiltinComponents();

    // Native C++ scripts: load the project's compiled module (if any) before
    // any scene so script component types deserialize; then every enabled
    // project plugin (Plugins/<Name>/).
    GameModule gameModule;
    if (project.hasModule()) gameModule.load(project);
    PluginManager plugins;
    plugins.loadEnabled(project, /*hotCopy=*/true);

    AssetLibrary assets;
    assets.init(project.root);

    World world;
    std::string startScene = mapPath ? std::string(mapPath) : project.startupScene;
    std::string loadedScenePath;
    if (!startScene.empty()) {
        if (loadWorld(world, assets, assets.resolvePath(startScene)))
            loadedScenePath = assets.resolvePath(startScene);
        else
            AE_ERROR("[Editor] failed to load scene %s — starting empty", startScene.c_str());
    } else {
        AE_LOG("[Editor] project has no startup scene — starting empty");
    }
    world.missions.load(joinPath(project.root, "assets\\missions\\missions.json"));
    world.actions.loadOrDefaults(assets.resolvePath("assets/input.json"));

    // Headless serialization check: load → save → exit (diff against input).
    if (resavePath) {
        world.update(0.0f, 0.0f, window.input(), false);
        bool ok = saveWorld(world, assets, resavePath);
        std::printf("--resave %s: %s\n", ok ? "written" : "FAILED", resavePath);
        return ok ? 0 : 1;
    }

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    int frame = 0;
    double fpsTimer = 0.0;
    int fpsCount = 0;

    Font font;
    if (!font.bake("Segoe UI", 15)) std::fprintf(stderr, "Font bake failed (UI text unavailable)\n");
    Editor editor;
    if (!editor.init(&window, &renderer, &world, &assets, &font, project, &gameModule,
                     &plugins)) {
        std::fprintf(stderr, "Editor init failed\n");
        return 1;
    }
    if (!loadedScenePath.empty()) editor.setScenePath(loadedScenePath);
    if (autoPlay) editor.requestAutoPlay();

    // `--frames N` without a screenshot is a real-timed benchmark: run N
    // frames at genuine dt (vsync off) and report the average at exit.
    bool benchmark = captureFrames > 0 && !screenshotPath;
    if (benchmark) { window.setVSync(false); renderer.settings.vsync = false; }
    LARGE_INTEGER benchStart = prev;

    while (window.poll()) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - prev.QuadPart) / freq.QuadPart);
        prev = now;
        if (dt > 0.1f) dt = 0.1f;
        if (screenshotPath) dt = 1.0f / 60.0f;

        editor.frame(dt, 0.0f);

        if (screenshotPath && ++frame >= captureFrames) { captureScreenshot(window, screenshotPath); break; }
        window.swapBuffers();

        if (benchmark && ++frame >= captureFrames) {
            double secs = (double)(now.QuadPart - benchStart.QuadPart) / freq.QuadPart;
            AE_LOG("[Benchmark] %d frames in %.2fs = %.1f fps (%.2f ms/frame)", frame, secs,
                   frame / secs, secs / frame * 1000.0);
            break;
        }

        fpsTimer += dt; ++fpsCount;
        if (fpsTimer >= 0.5) {
            char title[160];
            std::snprintf(title, sizeof(title), "Aether Editor — %s — %.0f fps%s",
                          project.name.c_str(), fpsCount / fpsTimer,
                          editor.playing() ? "  [PLAYING]" : "");
            window.setTitle(title);
            fpsTimer = 0.0; fpsCount = 0;
        }
    }

    editor.shutdown();
    font.destroy();
    assets.destroy();
    renderer.shutdown();
    window.destroy();
    return 0;
}
