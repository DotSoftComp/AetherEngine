// Aether Engine — standalone game runtime (AetherRuntime.exe).
//
// Packaged: a game.json next to the exe (written by the editor's Build Game)
// makes it boot self-contained — game root is the exe folder.
// Development:
//   AetherRuntime --project <dir|.aeproj>          run a project
//   AetherRuntime ... --map assets/maps/foo.json   run a specific map
//   AetherRuntime ... --frames N --screenshot out.bmp   headless capture
//
// Runs the same World the editor authors, fullscreen with the in-game HUD
// (dialogue, missions) and no editor UI. Built /SUBSYSTEM:WINDOWS (no console
// flash); output attaches to the parent console when launched from one.
#include "core/window.h"
#include "core/log.h"
#include "core/capture.h"
#include "core/json.h"
#include "core/paths.h"
#include "render/renderer.h"
#include "engine/world.h"
#include "engine/assets.h"
#include "engine/scene_io.h"
#include "engine/project.h"
#include "engine/component_registry.h"
#include "engine/save_game.h"
#include "engine/game_module.h"
#include "engine/engine_modules.h"
#include "rhi/rhi.h"
#include "engine/plugin_manager.h"
#include "ui/font.h"
#include "ui/ui.h"
#include "ui/ui_document_component.h"
#include "audio/audio.h"
#include "narrative/dialogue_player.h"
#include "narrative/mission_hud.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

using namespace ae;

// GUI-subsystem exe: reattach stdout/stderr to the launching console so dev
// flows (--frames/--screenshot, log lines) still print. When the launcher
// already redirected the handles (pipes/files), leave them alone.
static void attachParentConsole() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

// Optional quality overrides ("settings" in game.json); -1 = keep the
// auto-tiered default.
struct QualityOverrides {
    float renderScale = -1.0f;
    int shadowCascades = -1;
    int vsync = -1;
    bool noInstancing = false;
};
static QualityOverrides g_quality;

// Boots a packaged game: game.json next to the exe defines the project.
static bool loadPackagedGame(Project& project) {
    std::string path = joinPath(engineBinDir(), "game.json");
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    JsonValue doc;
    if (!jsonParse(text.c_str(), text.size(), doc) || doc.type != JsonValue::Object) {
        AE_ERROR("[Game] malformed game.json: %s", path.c_str());
        return false;
    }
    project.root = engineBinDir();
    project.file = path;
    project.name = doc.string("name") ? *doc.string("name") : "Game";
    project.startupScene = doc.string("startupScene") ? *doc.string("startupScene") : "";
    project.moduleName = doc.string("module") ? *doc.string("module") : "";
    project.engineVersion = doc.string("engineVersion") ? *doc.string("engineVersion") : "";
    if (const JsonValue* mods = doc.find("modules"))
        for (const auto& kv : mods->obj)
            if (kv.second.type == JsonValue::Bool)
                project.moduleFlags.emplace_back(kv.first, kv.second.boolean);
    if (const JsonValue* s = doc.find("settings")) {
        g_quality.renderScale = (float)s->num("renderScale", -1.0);
        g_quality.shadowCascades = s->integer("shadowCascades", -1);
        if (s->find("vsync")) g_quality.vsync = s->flag("vsync", true) ? 1 : 0;
    }
    AE_LOG("[Game] packaged boot: %s (%s)", project.name.c_str(), project.root.c_str());
    return true;
}

int main(int argc, char** argv) {
    attachParentConsole();
    int captureFrames = -1;
    const char* screenshotPath = nullptr;
    const char* mapPath = nullptr;
    const char* projectArg = nullptr;
    bool gpuProfile = false; // per-pass GPU timing in the benchmark report
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--project") && i + 1 < argc) projectArg = argv[++i];
        if (!std::strcmp(argv[i], "--map") && i + 1 < argc) mapPath = argv[++i];
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) captureFrames = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshotPath = argv[++i];
        if (!std::strcmp(argv[i], "--gpuprofile")) gpuProfile = true;
        if (!std::strcmp(argv[i], "--noinstancing")) g_quality.noInstancing = true;
        if (!std::strcmp(argv[i], "--cascades") && i + 1 < argc)
            g_quality.shadowCascades = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--rscale") && i + 1 < argc)
            g_quality.renderScale = (float)std::atof(argv[++i]);
    }
    if (screenshotPath && captureFrames < 0) captureFrames = 60;

    // Packaged layout wins unless --project explicitly overrides it.
    Project project;
    bool booted = false;
    if (projectArg) booted = project.load(projectArg);
    else booted = loadPackagedGame(project) ||
                  false; // no game.json → fall through to the usage error
    if (!booted) {
        std::fprintf(stderr, "Usage: AetherRuntime --project <dir or .aeproj>\n");
        MessageBoxA(nullptr,
                    "No game to run.\n\nEither place this exe in a packaged game folder "
                    "(game.json) or pass --project <dir or .aeproj>.",
                    "Aether Engine", MB_OK | MB_ICONERROR);
        return 1;
    }

    Window window;
    // The game runs fullscreen; headless screenshot captures keep a plain
    // system window.
    WindowChrome chrome = screenshotPath ? WindowChrome::System : WindowChrome::Fullscreen;
    if (!window.create(project.name.c_str(), 1600, 900, chrome)) {
        std::fprintf(stderr, "Failed to create window / GL 4.5 context\n");
        return 1;
    }
    rhi::init();

    Renderer renderer;
    if (!renderer.init(window.width(), window.height())) {
        std::fprintf(stderr, "Renderer init failed\n");
        return 1;
    }
    // Quality: weak-GPU auto-tier first, then explicit game/CLI overrides.
    applyGpuAutoTier(renderer.settings);
    if (g_quality.shadowCascades >= 1 && g_quality.shadowCascades <= Renderer::kNumCascades)
        renderer.settings.shadowCascades = g_quality.shadowCascades;
    if (g_quality.renderScale > 0.0f)
        renderer.settings.renderScale = g_quality.renderScale < 0.25f  ? 0.25f
                                        : g_quality.renderScale > 1.0f ? 1.0f
                                                                       : g_quality.renderScale;

    // `--frames N` without a screenshot is a real-timed benchmark: vsync off,
    // genuine dt, average frame time reported at exit.
    bool benchmark = captureFrames > 0 && !screenshotPath;
    bool uncapped = screenshotPath != nullptr || benchmark;
    bool wantVsync = !uncapped && g_quality.vsync != 0;
    window.setVSync(wantVsync);
    renderer.settings.vsync = wantVsync;
    renderer.settings.profileGpu = gpuProfile;
    if (g_quality.noInstancing) renderer.settings.instancing = false;

    // 3D renders at renderScale into an offscreen target and is upscaled to
    // the backbuffer; the HUD stays at native resolution.
    rhi::FramebufferHandle scaledFBO;
    rhi::TextureHandle scaledTex;
    int renderW = 0, renderH = 0;
    auto ensureRenderTarget = [&](int w, int h) {
        float scale = renderer.settings.renderScale;
        scale = scale < 0.25f ? 0.25f : scale > 1.0f ? 1.0f : scale;
        int nw = scale < 0.999f ? (w * (int)(scale * 100.0f)) / 100 : w;
        int nh = scale < 0.999f ? (h * (int)(scale * 100.0f)) / 100 : h;
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        if (nw == renderW && nh == renderH) return;
        renderW = nw;
        renderH = nh;
        renderer.resize(renderW, renderH);
        if (scaledTex.valid()) {
            rhi::destroyTexture(scaledTex);
            rhi::destroyFramebuffer(scaledFBO);

        }
        if (renderW != w || renderH != h) {
            scaledTex = rhi::createTexture2D(renderW, renderH, 1, rhi::TexFormat::RGBA8);
            rhi::SamplerDesc smp;
            smp.mipmaps = false;
            smp.repeat = false;
            rhi::setSampler(scaledTex, smp);
            scaledFBO = rhi::createFramebuffer();
            rhi::attachColor(scaledFBO, 0, scaledTex);
        }
        renderer.setOutputFramebuffer(scaledFBO);
    };
    ensureRenderTarget(window.width(), window.height());

    engineModules().configure(project.moduleFlags);
    registerBuiltinComponents();
    if (engineModules().enabled("audio"))
        audioEngine().init(); // no-op-safe if the platform has no audio device
    else
        AE_LOG("[Modules] audio disabled by project - engine runs silent");

    // Native C++ scripts ship as the project's compiled game module. Loaded in
    // place (no hot-copy): game folders may be read-only, and the runtime
    // never rebuilds. Then every enabled project plugin (Plugins/<Name>/).
    GameModule gameModule;
    if (project.hasModule()) gameModule.load(project, /*hotCopy=*/false);
    PluginManager plugins;
    plugins.loadEnabled(project, /*hotCopy=*/false);

    AssetLibrary assets;
    assets.init(project.root);

    World world;
    std::string startScene = mapPath ? std::string(mapPath) : project.startupScene;
    if (startScene.empty() || !loadWorld(world, assets, assets.resolvePath(startScene))) {
        AE_ERROR("[Game] cannot load startup scene '%s'", startScene.c_str());
        MessageBoxA(nullptr, "The project's startup scene could not be loaded.",
                    "Aether Engine", MB_OK | MB_ICONERROR);
        return 1;
    }
    world.missions.load(joinPath(project.root, "assets\\missions\\missions.json"));
    world.actions.loadOrDefaults(assets.resolvePath("assets/input.json"));

    Font font;
    if (!font.bake("Segoe UI", 15)) std::fprintf(stderr, "Font bake failed (UI text unavailable)\n");
    UI ui;
    if (!ui.init(&font)) { std::fprintf(stderr, "UI init failed\n"); return 1; }

    LARGE_INTEGER freq, prev, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    start = prev;
    int frame = 0;
    double passSums[FrameStats::PassCount] = {};
    double updateSum = 0.0, swapSum = 0.0;

    RenderScene renderScene;
    while (window.poll()) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - prev.QuadPart) / freq.QuadPart);
        float time = (float)((double)(now.QuadPart - start.QuadPart) / freq.QuadPart);
        prev = now;
        if (dt > 0.1f) dt = 0.1f;
        if (screenshotPath) { dt = 1.0f / 60.0f; time = frame / 60.0f; }

        const Input& in = window.input();
        if (in.keys[VK_ESCAPE]) break;
        if (window.wasResized()) ensureRenderTarget(window.width(), window.height());

        LARGE_INTEGER u0, u1;
        QueryPerformanceCounter(&u0);
        world.update(dt, time, in, true);
        processSaveRequests(world, assets);
        world.buildRenderScene(renderScene);
        // The listener is the resolved gameplay camera (valid after buildRenderScene).
        const Camera& cam = world.camera();
        audioEngine().setListener(cam.position, cam.forwardDir, cam.upDir);
        audioEngine().update(dt);
        QueryPerformanceCounter(&u1);
        updateSum += (double)(u1.QuadPart - u0.QuadPart) * 1000.0 / freq.QuadPart;

        renderer.render(renderScene, world.camera(), time);
        if (gpuProfile)
            for (int p = 0; p < FrameStats::PassCount; ++p)
                passSums[p] += renderer.stats.msPass[p];
        if (scaledFBO.valid()) {
            rhi::blitToBackbuffer(scaledFBO, renderW, renderH, window.width(),
                                  window.height());
            rhi::bindFramebuffer({});
            rhi::setViewport(0, 0, window.width(), window.height());
        }

        ui.begin(window.width(), window.height(), in);
        Rect screen{0, 0, (float)window.width(), (float)window.height()};
        if (DialoguePlayer* active = world.activeDialogue()) {
            active->update(ui, dt, screen);
            if (active->finished()) world.setActiveDialogue(nullptr);
        }
        drawMissionHUD(ui, world.missions, screen);
        for (const auto& e : world.entities()) {
            if (!e->active()) continue;
            for (const auto& c : e->components())
                if (auto* doc = dynamic_cast<UIDocumentComponent*>(c.get()))
                    doc->draw(ui, screen);
        }
        ui.end();

        if (screenshotPath && ++frame >= captureFrames) { captureScreenshot(window, screenshotPath); break; }
        LARGE_INTEGER s0, s1;
        QueryPerformanceCounter(&s0);
        window.swapBuffers();
        QueryPerformanceCounter(&s1);
        swapSum += (double)(s1.QuadPart - s0.QuadPart) * 1000.0 / freq.QuadPart;

        if (benchmark && ++frame >= captureFrames) {
            double secs = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
            AE_LOG("[Benchmark] %dx%d: %d frames in %.2fs = %.1f fps (%.2f ms/frame)",
                   window.width(), window.height(), frame, secs, frame / secs,
                   secs / frame * 1000.0);
            AE_LOG("[Benchmark] cpu update %.2f ms, swap %.2f ms", updateSum / frame,
                   swapSum / frame);
            AE_LOG("[Benchmark] draws %d (instanced %d covering %d objects), culled %d",
                   renderer.stats.drawCalls, renderer.stats.instancedDraws,
                   renderer.stats.instancedObjects, renderer.stats.culled);
            if (gpuProfile)
                for (int p = 0; p < FrameStats::PassCount; ++p)
                    AE_LOG("[Benchmark]   %-9s %7.2f ms", FrameStats::passName(p),
                           passSums[p] / frame);
            break;
        }
    }
    audioEngine().shutdown();
    ui.destroy();
    font.destroy();
    assets.destroy();
    renderer.shutdown();
    window.destroy();
    return 0;
}
