// Aether Engine — standalone game runtime (AetherRuntime.exe).
//
// Packaged: a game.json next to the exe (written by the editor's Build Game)
// makes it boot self-contained — game root is the exe folder.
// Development:
//   AetherRuntime --project <dir|.aeproj>          run a project
//   AetherRuntime ... --map assets/maps/foo.json   run a specific map
//   AetherRuntime ... --frames N --screenshot out.bmp   headless capture
//   AetherRuntime ... --verify [--frames N]        agent verification: scene
//        resave round-trip + N frames (default 300) + log scan; prints one
//        machine-readable "VERIFY PASS|FAIL {json}" line and exits nonzero
//        on any [error], unknown-type/dangling-link warning, or resave drift.
//   AetherRuntime ... --compare ref.bmp [--psnrmin N]   visual regression:
//        captures a screenshot (default <project>/verify_screenshot.bmp,
//        override with --screenshot) and compares it against the reference.
//        Prints "COMPARE PASS|FAIL {json}", writes an amplified *_diff.bmp
//        heatmap on failure, exits nonzero below the PSNR budget (default 35
//        dB; identical renders report 99). Combines with --verify.
//
// Runs the same World the editor authors, fullscreen with the in-game HUD
// (dialogue, missions) and no editor UI. Built /SUBSYSTEM:WINDOWS (no console
// flash); output attaches to the parent console when launched from one.
#include "core/window_sdl.h" // portable SDL3 host window
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep min/max macros out of <atomic>/<limits>
#include <windows.h> // host timing (QPC) + error dialogs; portable-ized in the Linux stage
#endif
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
#include "rhi/vulkan_probe.h"
#include "rhi/vulkan_context.h"
#include "rhi/spirv_compile.h"
#include "engine/plugin_manager.h"
#include "ui/font.h"
#include "ui/ui.h"
#include "ui/ui_document_component.h"
#include "audio/audio.h"
#include "narrative/dialogue_player.h"
#include "narrative/mission_hud.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
// Ask laptops with switchable graphics to use the discrete GPU (Windows-only).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

using namespace ae;

// Windows GUI-subsystem exes have no console; reattach stdout/stderr to the
// launching console so dev flows (--frames/--screenshot, log lines) still
// print. No-op on Linux/macOS/Android where stdout already works.
static void attachParentConsole() {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
}

// Portable fatal-error notice: always to stderr; a native dialog on desktop.
static void fatalDialog(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
#ifdef _WIN32
    MessageBoxA(nullptr, msg, "Aether Engine", MB_OK | MB_ICONERROR);
#endif
}

using Clock = std::chrono::steady_clock;
static double msBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Optional quality overrides ("settings" in game.json); -1 = keep the
// auto-tiered default.
struct QualityOverrides {
    float renderScale = -1.0f;
    int shadowCascades = -1;
    int spotShadows = -1;
    int pointShadows = -1;
    int vsync = -1;
    bool noInstancing = false;
};
static QualityOverrides g_quality;

// Boots a packaged game: game.json next to the exe defines the project.
// ---- --verify -----------------------------------------------------------------
// One command an agent runs to prove its change: everything below feeds the
// final VERIFY line. Failures are the log entries an agent must fix, each
// already precise (file:node:pin for script links, entity+type for scenes).

static std::string jsonEsc(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out;
}

// Scans the engine log, prints the report, returns the process exit code.
static int verifyReport(bool loaded, bool resaveOk, int frames, const std::string& map) {
    // Dedupe: the resave round-trip loads the scene (and its graphs) twice,
    // so the same schema warning appears twice in the log.
    auto pushUnique = [](std::vector<std::string>& v, const std::string& s) {
        for (const std::string& x : v)
            if (x == s) return;
        v.push_back(s);
    };
    std::vector<std::string> errors, warnings;
    int scriptLogs = 0;
    for (const LogEntry& e : logEntries()) {
        if (e.level == LogLevel::Error) pushUnique(errors, e.text);
        else if (e.level == LogLevel::Warn &&
                 (e.text.find("unknown") != std::string::npos ||
                  e.text.find("out of range") != std::string::npos ||
                  e.text.find("malformed") != std::string::npos))
            pushUnique(warnings, e.text);
        // Gameplay evidence: Log-node output only, not [Script] warnings.
        if (e.level == LogLevel::Info && e.text.rfind("[Script]", 0) == 0) ++scriptLogs;
    }
    bool ok = loaded && resaveOk && errors.empty() && warnings.empty();

    std::ostringstream j;
    j << "{\"ok\":" << (ok ? "true" : "false") << ",\"map\":\"" << jsonEsc(map)
      << "\",\"frames\":" << frames << ",\"loaded\":" << (loaded ? "true" : "false")
      << ",\"resave\":\"" << (loaded ? (resaveOk ? "ok" : "mismatch") : "skipped")
      << "\",\"scriptLogs\":" << scriptLogs << ",\"errors\":[";
    for (size_t i = 0; i < errors.size() && i < 25; ++i)
        j << (i ? "," : "") << "\"" << jsonEsc(errors[i]) << "\"";
    j << "],\"schemaWarnings\":[";
    for (size_t i = 0; i < warnings.size() && i < 25; ++i)
        j << (i ? "," : "") << "\"" << jsonEsc(warnings[i]) << "\"";
    j << "]}";
    std::printf("VERIFY %s %s\n", ok ? "PASS" : "FAIL", j.str().c_str());
    std::fflush(stdout);
    return ok ? 0 : 1;
}

// Visual regression: capture vs. reference. Prints one COMPARE line; on
// failure also logs AE_ERROR (so --verify fails with it) and writes an
// amplified heatmap next to the capture showing WHERE the frames diverge.
static bool runCompare(const std::string& shotPath, const std::string& refPath,
                       double psnrBudget) {
    int aw = 0, ah = 0, bw = 0, bh = 0;
    std::vector<uint8_t> a, b;
    std::string failReason, diffPath;
    ImageDiff d;
    if (!readBMP(shotPath.c_str(), aw, ah, a))
        failReason = "cannot read capture: " + shotPath;
    else if (!readBMP(refPath.c_str(), bw, bh, b))
        failReason = "cannot read reference: " + refPath +
                     " (create it with --screenshot first)";
    else if (aw != bw || ah != bh)
        failReason = "size mismatch: capture " + std::to_string(aw) + "x" + std::to_string(ah) +
                     " vs reference " + std::to_string(bw) + "x" + std::to_string(bh);

    bool ok = false;
    if (failReason.empty()) {
        d = compareImages(a, b, aw, ah);
        ok = d.psnr >= psnrBudget;
        if (!ok) {
            diffPath = shotPath;
            size_t dot = diffPath.rfind('.');
            diffPath.insert(dot == std::string::npos ? diffPath.size() : dot, "_diff");
            if (!writeDiffBMP(diffPath.c_str(), a, b, aw, ah)) diffPath.clear();
        }
    }

    if (ok)
        AE_LOG("[Compare] PASS psnr=%.1f dB (budget %.1f), meanAbs=%.2f, diffPixels=%.2f%% "
               "vs %s", d.psnr, psnrBudget, d.meanAbs, d.diffPct, refPath.c_str());
    else if (failReason.empty())
        AE_ERROR("[Compare] FAIL psnr=%.1f dB < budget %.1f (maxAbs=%d, diffPixels=%.2f%%) "
                 "vs %s — heatmap: %s", d.psnr, psnrBudget, d.maxAbs, d.diffPct,
                 refPath.c_str(), diffPath.c_str());
    else
        AE_ERROR("[Compare] FAIL: %s", failReason.c_str());

    std::ostringstream j;
    j << "{\"ok\":" << (ok ? "true" : "false");
    if (failReason.empty()) {
        char nums[160];
        std::snprintf(nums, sizeof(nums),
                      ",\"psnr\":%.2f,\"budget\":%.1f,\"meanAbs\":%.3f,\"maxAbs\":%d,"
                      "\"diffPct\":%.3f", d.psnr, psnrBudget, d.meanAbs, d.maxAbs, d.diffPct);
        j << nums;
    } else {
        j << ",\"error\":\"" << jsonEsc(failReason) << "\"";
    }
    j << ",\"screenshot\":\"" << jsonEsc(shotPath) << "\",\"reference\":\"" << jsonEsc(refPath)
      << "\"";
    if (!diffPath.empty()) j << ",\"diffHeatmap\":\"" << jsonEsc(diffPath) << "\"";
    j << "}";
    std::printf("COMPARE %s %s\n", ok ? "PASS" : "FAIL", j.str().c_str());
    std::fflush(stdout);
    return ok;
}

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
        g_quality.spotShadows = s->integer("spotShadows", -1);
        g_quality.pointShadows = s->integer("pointShadows", -1);
        if (s->find("vsync")) g_quality.vsync = s->flag("vsync", true) ? 1 : 0;
    }
    AE_LOG("[Game] packaged boot: %s (%s)", project.name.c_str(), project.root.c_str());
    return true;
}

int main(int argc, char** argv) {
    attachParentConsole();

    // --vulkan-probe: bring up Vulkan (instance/device/swapchain via SDL3),
    // report the GPU, exit. Proves the Vulkan path on this hardware without
    // the GL renderer — the groundwork the rhi_vulkan backend builds on.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vulkan-probe")) continue;
        std::string dev, err;
        bool ok = runVulkanProbe(&dev, &err);
        if (ok) std::printf("VULKAN OK %s\n", dev.c_str());
        else std::printf("VULKAN FAIL %s\n", err.c_str());
        std::fflush(stdout);
        return ok ? 0 : 1;
    }
    // --spirv-audit: compile every shader to SPIR-V (glslang) and report which
    // are already Vulkan-ready vs. still need the UBO uniform rework.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--spirv-audit")) continue;
        return runSpirvAudit() ? 0 : 1;
    }
    // --vulkan-clear [out.bmp]: bring up the Vulkan context and present N cleared
    // frames (foundation of rhi_vulkan); optional screenshot reads the swapchain
    // back to prove present + readback work.
    for (int i = 1; i < argc; ++i) {
        bool clear = !std::strcmp(argv[i], "--vulkan-clear");
        bool tri = !std::strcmp(argv[i], "--vulkan-triangle");
        if (!clear && !tri) continue;
        const char* out = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[i + 1] : nullptr;
        std::string dev, err;
        bool ok = runVulkanClear(out ? 120 : 240, out, tri, &dev, &err);
        std::printf("VULKAN-%s %s %s\n", tri ? "TRIANGLE" : "CLEAR", ok ? "OK" : "FAIL",
                    ok ? dev.c_str() : err.c_str());
        std::fflush(stdout);
        return ok ? 0 : 1;
    }

    int captureFrames = -1;
    const char* screenshotPath = nullptr;
    const char* comparePath = nullptr;
    double psnrBudget = 35.0;
    const char* mapPath = nullptr;
    const char* projectArg = nullptr;
    bool gpuProfile = false; // per-pass GPU timing in the benchmark report
    bool verifyMode = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--project") && i + 1 < argc) projectArg = argv[++i];
        if (!std::strcmp(argv[i], "--map") && i + 1 < argc) mapPath = argv[++i];
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) captureFrames = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshotPath = argv[++i];
        if (!std::strcmp(argv[i], "--verify")) verifyMode = true;
        if (!std::strcmp(argv[i], "--compare") && i + 1 < argc) comparePath = argv[++i];
        if (!std::strcmp(argv[i], "--psnrmin") && i + 1 < argc)
            psnrBudget = std::atof(argv[++i]);
        if (!std::strcmp(argv[i], "--gpuprofile")) gpuProfile = true;
        if (!std::strcmp(argv[i], "--noinstancing")) g_quality.noInstancing = true;
        if (!std::strcmp(argv[i], "--cascades") && i + 1 < argc)
            g_quality.shadowCascades = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--spotshadows") && i + 1 < argc)
            g_quality.spotShadows = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--pointshadows") && i + 1 < argc)
            g_quality.pointShadows = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--rscale") && i + 1 < argc)
            g_quality.renderScale = (float)std::atof(argv[++i]);
    }
    if (screenshotPath && captureFrames < 0) captureFrames = 60;
    if (verifyMode && captureFrames < 0) captureFrames = 300;

    // Packaged layout wins unless --project explicitly overrides it.
    Project project;
    bool booted = false;
    if (projectArg) booted = project.load(projectArg);
    else booted = loadPackagedGame(project) ||
                  false; // no game.json → fall through to the usage error
    if (!booted) {
        std::fprintf(stderr, "Usage: AetherRuntime --project <dir or .aeproj>\n");
        if (verifyMode) return verifyReport(false, false, 0, mapPath ? mapPath : "");
        fatalDialog("No game to run.\n\nEither place this exe in a packaged game folder "
                    "(game.json) or pass --project <dir or .aeproj>.");
        return 1;
    }

    // --compare needs a capture; default one into the project if none named.
    std::string autoShot;
    if (comparePath && !screenshotPath) {
        autoShot = joinPath(project.root, "verify_screenshot.bmp");
        screenshotPath = autoShot.c_str();
        if (captureFrames < 0) captureFrames = 60;
    }

    Window window;
    // The game runs fullscreen; headless captures/verification keep a plain
    // system window.
    WindowChrome chrome =
        (screenshotPath || verifyMode) ? WindowChrome::System : WindowChrome::Fullscreen;
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
    if (g_quality.spotShadows >= 0 && g_quality.spotShadows <= Renderer::kMaxSpotShadows)
        renderer.settings.spotShadows = g_quality.spotShadows;
    if (g_quality.pointShadows >= 0 && g_quality.pointShadows <= Renderer::kMaxPointShadows)
        renderer.settings.pointShadows = g_quality.pointShadows;
    if (g_quality.renderScale > 0.0f)
        renderer.settings.renderScale = g_quality.renderScale < 0.25f  ? 0.25f
                                        : g_quality.renderScale > 1.0f ? 1.0f
                                                                       : g_quality.renderScale;

    // `--frames N` without a screenshot is a real-timed benchmark: vsync off,
    // genuine dt, average frame time reported at exit. --verify rides the
    // same N-frames-then-exit path.
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
        if (verifyMode) return verifyReport(false, false, 0, startScene);
        fatalDialog("The project's startup scene could not be loaded.");
        return 1;
    }
    world.missions.load(joinPath(project.root, "assets\\missions\\missions.json"));
    world.actions.loadOrDefaults(assets.resolvePath("assets/input.json"));

    // Verify: serialization must round-trip byte-stable before anything runs —
    // drift here means a hand-edited (or agent-edited) file the engine would
    // silently rewrite differently.
    bool resaveOk = true;
    if (verifyMode) {
        std::string a = serializeWorld(world, assets);
        resaveOk = deserializeWorld(world, assets, a) && serializeWorld(world, assets) == a;
        if (!resaveOk) AE_ERROR("[Verify] scene resave round-trip drift: %s", startScene.c_str());
    }

    Font font;
    if (!font.bake("Segoe UI", 15)) std::fprintf(stderr, "Font bake failed (UI text unavailable)\n");
    UI ui;
    if (!ui.init(&font)) { std::fprintf(stderr, "UI init failed\n"); return 1; }

    Clock::time_point prev = Clock::now(), start = prev;
    int frame = 0;
    double passSums[FrameStats::PassCount] = {};
    double updateSum = 0.0, swapSum = 0.0;

    bool compareOk = true; // stays true when no --compare
    RenderScene renderScene;
    while (window.poll()) {
        Clock::time_point now = Clock::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        float time = std::chrono::duration<float>(now - start).count();
        prev = now;
        if (dt > 0.1f) dt = 0.1f;
        // Deterministic stepping for captures/verification.
        if (screenshotPath || verifyMode) { dt = 1.0f / 60.0f; time = frame / 60.0f; }

        const Input& in = window.input();
        if (in.keys[0x1B]) break; // Esc (Input.keys is VK-indexed; 0x1B = VK_ESCAPE)
        if (window.wasResized()) ensureRenderTarget(window.width(), window.height());

        Clock::time_point u0 = Clock::now();
        world.update(dt, time, in, true);
        processSaveRequests(world, assets);
        world.buildRenderScene(renderScene);
        // The listener is the resolved gameplay camera (valid after buildRenderScene).
        const Camera& cam = world.camera();
        audioEngine().setListener(cam.position, cam.forwardDir, cam.upDir);
        audioEngine().update(dt);
        updateSum += msBetween(u0, Clock::now());

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

        if (screenshotPath && ++frame >= captureFrames) {
            captureScreenshot(window.width(), window.height(), screenshotPath);
            if (comparePath)
                compareOk = runCompare(screenshotPath, assets.resolvePath(comparePath),
                                       psnrBudget);
            break;
        }
        Clock::time_point s0 = Clock::now();
        window.swapBuffers();
        swapSum += msBetween(s0, Clock::now());

        if (benchmark && ++frame >= captureFrames) {
            double secs = std::chrono::duration<double>(now - start).count();
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
    if (verifyMode) return verifyReport(true, resaveOk, frame, startScene);
    return compareOk ? 0 : 1;
}
