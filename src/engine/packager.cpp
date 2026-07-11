#include "packager.h"
#include "module_build.h"
#include "project.h"
#include "../core/paths.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ae {

namespace {

std::string jsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

bool copyTree(const std::string& from, const std::string& to,
              const std::function<void(const std::string&)>& onLine) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        onLine("[Package] copy failed " + from + " -> " + to + ": " + ec.message());
        return false;
    }
    return true;
}

bool copyOne(const std::string& from, const std::string& to,
             const std::function<void(const std::string&)>& onLine) {
    std::error_code ec;
    fs::create_directories(fs::path(to).parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        onLine("[Package] copy failed " + from + " -> " + to + ": " + ec.message());
        return false;
    }
    return true;
}

} // namespace

bool packageGame(const Project& project, const PackageOptions& opts,
                 const std::function<void(const std::string&)>& onLine) {
    std::string name = opts.gameName.empty() ? project.name : opts.gameName;
    std::string startupScene = opts.startupScene.empty() ? project.startupScene : opts.startupScene;
    if (name.empty() || opts.outputDir.empty()) {
        onLine("[Package] game name and output directory are required");
        return false;
    }
    if (startupScene.empty()) {
        onLine("[Package] the project has no startup scene to ship");
        return false;
    }
    std::string out = joinPath(opts.outputDir, name);
    std::string bin = engineBinDir();
    onLine("[Package] packaging '" + name + "' to " + out);

    // 1. Native scripts, fresh Release build.
    if (project.hasModule()) {
        onLine("[Package] compiling game module...");
        if (!buildGameModule(project, "Release", onLine)) return false;
    }

    // 2. Engine binaries (runtime player, renamed to the game).
    std::error_code ec;
    fs::create_directories(out, ec);
    if (ec) {
        onLine("[Package] cannot create " + out + ": " + ec.message());
        return false;
    }
    if (!copyOne(joinPath(bin, "AetherRuntime.exe"), joinPath(out, name + ".exe"), onLine))
        return false;
    if (!copyOne(joinPath(bin, "AetherCore.dll"), joinPath(out, "AetherCore.dll"), onLine))
        return false;

    // 3. Engine shaders.
    onLine("[Package] copying shaders...");
    if (!copyTree(joinPath(engineRoot(), "shaders"), joinPath(out, "shaders"), onLine))
        return false;

    // 4. Project content (copy-all; referenced-only pruning is a later pass).
    onLine("[Package] copying assets...");
    if (!copyTree(joinPath(project.root, "assets"), joinPath(out, "assets"), onLine)) return false;

    // 5. Game module binary, same Binaries/ layout the dev project uses.
    if (project.hasModule()) {
        std::string dll = joinPath(project.root, "Binaries\\" + project.moduleName + ".dll");
        if (!copyOne(dll, joinPath(out, "Binaries\\" + project.moduleName + ".dll"), onLine))
            return false;
    }

    // 5b. Plugins: manifest + compiled binary per plugin (sources stay home).
    {
        std::string pluginsRoot = joinPath(project.root, "Plugins");
        if (isDirectory(pluginsRoot)) {
            std::error_code ec;
            for (fs::directory_iterator it(pluginsRoot, ec), end; it != end && !ec;
                 it.increment(ec)) {
                if (!it->is_directory()) continue;
                std::string name = it->path().filename().string();
                std::string src = it->path().string();
                if (!pathExists(joinPath(src, "plugin.json"))) continue;
                std::string dst = joinPath(out, "Plugins\\" + name);
                onLine("[Package] plugin " + name);
                if (!copyOne(joinPath(src, "plugin.json"), joinPath(dst, "plugin.json"),
                             onLine))
                    return false;
                std::string dll = joinPath(src, "Binaries\\" + name + ".dll");
                if (pathExists(dll) &&
                    !copyOne(dll, joinPath(dst, "Binaries\\" + name + ".dll"), onLine))
                    return false;
            }
        }
    }

    // 6. game.json — what AetherRuntime boots from when found next to the exe.
    {
        std::ostringstream o;
        o << "{\n";
        o << "  \"name\": \"" << jsonEsc(name) << "\",\n";
        o << "  \"startupScene\": \"" << jsonEsc(startupScene) << "\",\n";
        if (project.hasModule())
            o << "  \"module\": \"" << jsonEsc(project.moduleName) << "\",\n";
        o << "  \"development\": " << (opts.development ? "true" : "false") << ",\n";
        o << "  \"engineVersion\": \"" << jsonEsc(project.engineVersion) << "\"\n";
        o << "}\n";
        std::ofstream f(joinPath(out, "game.json"), std::ios::binary);
        if (!f) {
            onLine("[Package] cannot write game.json");
            return false;
        }
        f << o.str();
    }

    onLine("[Package] done: " + joinPath(out, name + ".exe"));
    onLine("[Package] note: target machines need the VC++ x64 redistributable");
    return true;
}

} // namespace ae
