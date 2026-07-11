#include "project.h"
#include "../core/json.h"
#include "../core/log.h"
#include "../core/paths.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ae {

static std::string readTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string jsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

bool Project::load(const std::string& pathOrDir) {
    std::string projFile = pathOrDir;
    if (isDirectory(pathOrDir)) projFile = joinPath(pathOrDir, "project.aeproj");
    if (!pathExists(projFile)) {
        AE_ERROR("[Project] no project manifest at %s", projFile.c_str());
        return false;
    }

    std::string text = readTextFile(projFile);
    JsonValue doc;
    if (!jsonParse(text.c_str(), text.size(), doc) || doc.type != JsonValue::Object) {
        AE_ERROR("[Project] malformed manifest: %s", projFile.c_str());
        return false;
    }

    std::error_code ec;
    fs::path abs = fs::absolute(projFile, ec);
    file = ec ? projFile : abs.string();
    root = parentPath(file);

    name = doc.string("name") ? *doc.string("name") : "Untitled";
    engineVersion = doc.string("engineVersion") ? *doc.string("engineVersion") : "";
    startupScene = doc.string("startupScene") ? *doc.string("startupScene") : "";
    moduleName.clear();
    sourceDir = "Source";
    if (const JsonValue* mod = doc.find("module")) {
        if (const std::string* n = mod->string("name")) moduleName = *n;
        if (const std::string* d = mod->string("sourceDir")) sourceDir = *d;
    }
    moduleFlags.clear();
    if (const JsonValue* mods = doc.find("modules"))
        for (const auto& kv : mods->obj)
            if (kv.second.type == JsonValue::Bool)
                moduleFlags.emplace_back(kv.first, kv.second.boolean);
    AE_LOG("[Project] %s (%s)", name.c_str(), root.c_str());
    return true;
}

bool Project::save(const std::string& path) const {
    std::ostringstream o;
    o << "{\n";
    o << "  \"name\": \"" << jsonEsc(name) << "\",\n";
    o << "  \"engineVersion\": \"" << jsonEsc(engineVersion) << "\",\n";
    o << "  \"startupScene\": \"" << jsonEsc(startupScene) << "\"";
    if (hasModule()) {
        o << ",\n  \"module\": { \"name\": \"" << jsonEsc(moduleName)
          << "\", \"sourceDir\": \"" << jsonEsc(sourceDir) << "\" }";
    }
    o << "\n}\n";
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << o.str();
    return true;
}

bool createProjectFromTemplate(const std::string& templateDir, const std::string& destDir,
                               const std::string& name, const std::string& engineVersion,
                               std::string* outProjectFile, std::string* outError) {
    auto fail = [&](const std::string& msg) {
        if (outError) *outError = msg;
        AE_ERROR("[Project] %s", msg.c_str());
        return false;
    };

    Project tmpl;
    if (!tmpl.load(templateDir)) return fail("template has no valid project.aeproj: " + templateDir);
    if (pathExists(joinPath(destDir, "project.aeproj")))
        return fail("destination already contains a project: " + destDir);

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) return fail("cannot create " + destDir + ": " + ec.message());

    // Recursive copy, skipping build intermediates and VCS state.
    const fs::path srcRoot(templateDir);
    for (fs::recursive_directory_iterator it(srcRoot, ec), end; it != end && !ec; it.increment(ec)) {
        const fs::path& p = it->path();
        std::string base = p.filename().string();
        if (it->is_directory() && (base == "Intermediate" || base == "Binaries" || base == ".git")) {
            it.disable_recursion_pending();
            continue;
        }
        fs::path rel = fs::relative(p, srcRoot, ec);
        fs::path dst = fs::path(destDir) / rel;
        if (it->is_directory()) fs::create_directories(dst, ec);
        else fs::copy_file(p, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("copy failed for " + p.string() + ": " + ec.message());
    }
    if (ec) return fail("template copy failed: " + ec.message());

    Project proj = tmpl;
    proj.name = name;
    proj.engineVersion = engineVersion;
    proj.file = joinPath(destDir, "project.aeproj");
    proj.root = destDir;
    if (!proj.save(proj.file)) return fail("cannot write " + proj.file);

    if (outProjectFile) *outProjectFile = proj.file;
    AE_LOG("[Project] created '%s' at %s (template: %s)", name.c_str(), destDir.c_str(),
           tmpl.name.c_str());
    return true;
}

} // namespace ae
