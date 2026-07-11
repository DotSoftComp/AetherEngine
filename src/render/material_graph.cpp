// Aether Engine — material-graph codegen + compilation (see material_graph.h).
//
// ADDING A NODE: append one block to buildDefs() — pins, params, and the GLSL
// expression it emits. The codegen and the canvas editor pick it up.
#include "material_graph.h"
#include "image.h"
#include "../core/json.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace ae {

const char* mgTypeName(MGType t) {
    switch (t) {
    case MGType::F1: return "Float";
    case MGType::F2: return "Vec2";
    case MGType::F3: return "Vec3";
    }
    return "?";
}

unsigned mgTypeColor(MGType t) { // ABGR
    switch (t) {
    case MGType::F1: return 0xff6fd66fu; // green
    case MGType::F2: return 0xffd2c850u; // cyan
    case MGType::F3: return 0xff46b4f0u; // orange
    }
    return 0xffffffffu;
}

// ---- codegen context ------------------------------------------------------------
struct MGEmitCtx {
    // Distinct (path, srgb) texture slots, capped at 4 (units 10..13).
    std::vector<std::pair<std::string, bool>> textures;
    int textureSlot(const std::string& path, bool srgb) {
        for (size_t i = 0; i < textures.size(); ++i)
            if (textures[i].first == path && textures[i].second == srgb) return (int)i;
        if (textures.size() >= 4) return -1;
        textures.push_back({path, srgb});
        return (int)textures.size() - 1;
    }
};

static std::string glslType(MGType t) {
    switch (t) {
    case MGType::F1: return "float";
    case MGType::F2: return "vec2";
    case MGType::F3: return "vec3";
    }
    return "vec3";
}

// Implicit conversion between the three value types.
static std::string convert(const std::string& expr, MGType from, MGType to) {
    if (from == to) return expr;
    if (to == MGType::F1) return "(" + expr + ")" + (from == MGType::F2 ? ".x" : ".x");
    if (to == MGType::F2) {
        if (from == MGType::F1) return "vec2(" + expr + ")";
        return "(" + expr + ").xy";
    }
    // to F3
    if (from == MGType::F1) return "vec3(" + expr + ")";
    return "vec3(" + expr + ", 0.0)";
}

static std::string num(float v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%g", v);
    std::string s = buf;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
    return s;
}
static std::string vec3s(const Vec3& v) {
    return "vec3(" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + ")";
}

// ---- node registry ----------------------------------------------------------------
namespace {

struct Def {
    MGNodeDef d;
    Def(const char* type, const char* category, MGType out) {
        d.type = type;
        d.category = category;
        d.out = out;
    }
    Def& in(const char* name, MGType t, const char* defExpr) {
        d.in.push_back({name, t, defExpr});
        return *this;
    }
    Def& params(const char* p, const char* n = nullptr, const char* v = nullptr,
                bool color = false) {
        d.pLabel = p;
        d.nLabel = n;
        d.vLabel = v;
        d.vIsColor = color;
        return *this;
    }
    Def& emit(std::function<std::string(const MGNode&, const std::vector<std::string>&,
                                        MGEmitCtx&)> fn) {
        d.emit = std::move(fn);
        return *this;
    }
};

std::vector<MGNodeDef> buildDefs() {
    std::vector<Def> b;

    // ---- Inputs ----
    b.emplace_back("Color", "Input", MGType::F3);
    b.back().params(nullptr, nullptr, "Color", true).emit(
        [](const MGNode& n, const std::vector<std::string>&, MGEmitCtx&) { return vec3s(n.v); });

    b.emplace_back("Scalar", "Input", MGType::F1);
    b.back().params(nullptr, "Value").emit(
        [](const MGNode& n, const std::vector<std::string>&, MGEmitCtx&) { return num(n.n); });

    b.emplace_back("Time", "Input", MGType::F1);
    b.back().emit([](const MGNode&, const std::vector<std::string>&, MGEmitCtx&) {
        return std::string("uTime");
    });

    b.emplace_back("UV", "Input", MGType::F2);
    b.back().params(nullptr, nullptr, "Tiling XY").emit(
        [](const MGNode& n, const std::vector<std::string>&, MGEmitCtx&) {
            float tx = n.v.x != 0.0f ? n.v.x : 1.0f;
            float ty = n.v.y != 0.0f ? n.v.y : 1.0f;
            return "(fs.uv * vec2(" + num(tx) + ", " + num(ty) + "))";
        });

    b.emplace_back("WorldPos", "Input", MGType::F3);
    b.back().emit([](const MGNode&, const std::vector<std::string>&, MGEmitCtx&) {
        return std::string("fs.worldPos");
    });

    b.emplace_back("VertexNormal", "Input", MGType::F3);
    b.back().emit([](const MGNode&, const std::vector<std::string>&, MGEmitCtx&) {
        return std::string("normalize(fs.normal)");
    });

    b.emplace_back("Fresnel", "Input", MGType::F1);
    b.back().params(nullptr, "Power (def 3)").emit(
        [](const MGNode& n, const std::vector<std::string>&, MGEmitCtx&) {
            float p = n.n > 0.0f ? n.n : 3.0f;
            return "pow(1.0 - clamp(dot(normalize(fs.normal), normalize(uCamPos - fs.worldPos)), "
                   "0.0, 1.0), " + num(p) + ")";
        });

    // ---- Textures ----
    b.emplace_back("Texture", "Texture", MGType::F3);
    b.back().in("UV", MGType::F2, "fs.uv").params("Image path", "Linear (1 = not sRGB)").emit(
        [](const MGNode& n, const std::vector<std::string>& in, MGEmitCtx& ctx) {
            if (n.p.empty()) return std::string("vec3(1.0, 0.0, 1.0)"); // magenta = unset
            int slot = ctx.textureSlot(n.p, /*srgb=*/n.n == 0.0f);
            if (slot < 0) return std::string("vec3(1.0, 0.0, 1.0)"); // slot cap hit
            return "texture(uMGTex" + std::to_string(slot) + ", " + in[0] + ").rgb";
        });

    b.emplace_back("TextureAlpha", "Texture", MGType::F1);
    b.back().in("UV", MGType::F2, "fs.uv").params("Image path").emit(
        [](const MGNode& n, const std::vector<std::string>& in, MGEmitCtx& ctx) {
            int slot = ctx.textureSlot(n.p, true);
            if (slot < 0 || n.p.empty()) return std::string("1.0");
            return "texture(uMGTex" + std::to_string(slot) + ", " + in[0] + ").a";
        });

    b.emplace_back("Panner", "Texture", MGType::F2);
    b.back().in("UV", MGType::F2, "fs.uv").params(nullptr, nullptr, "Speed XY").emit(
        [](const MGNode& n, const std::vector<std::string>& in, MGEmitCtx&) {
            return "(" + in[0] + " + uTime * vec2(" + num(n.v.x) + ", " + num(n.v.y) + "))";
        });

    // ---- Math (element-wise on vec3; floats splat automatically) ----
    auto bin = [&](const char* name, const char* op) {
        b.emplace_back(name, "Math", MGType::F3);
        std::string o = op;
        b.back().in("A", MGType::F3, "vec3(0.0)").in("B", MGType::F3, "vec3(0.0)").emit(
            [o](const MGNode&, const std::vector<std::string>& in, MGEmitCtx&) {
                return "(" + in[0] + " " + o + " " + in[1] + ")";
            });
    };
    bin("Add", "+");
    bin("Subtract", "-");
    bin("Multiply", "*");

    b.emplace_back("Lerp", "Math", MGType::F3);
    b.back().in("A", MGType::F3, "vec3(0.0)").in("B", MGType::F3, "vec3(1.0)").in(
        "T", MGType::F1, "0.5").emit(
        [](const MGNode&, const std::vector<std::string>& in, MGEmitCtx&) {
            return "mix(" + in[0] + ", " + in[1] + ", " + in[2] + ")";
        });

    b.emplace_back("OneMinus", "Math", MGType::F3);
    b.back().in("A", MGType::F3, "vec3(0.0)").emit(
        [](const MGNode&, const std::vector<std::string>& in, MGEmitCtx&) {
            return "(vec3(1.0) - " + in[0] + ")";
        });

    b.emplace_back("Sin", "Math", MGType::F3);
    b.back().in("A", MGType::F3, "vec3(0.0)").emit(
        [](const MGNode&, const std::vector<std::string>& in, MGEmitCtx&) {
            return "sin(" + in[0] + ")";
        });

    b.emplace_back("Clamp01", "Math", MGType::F3);
    b.back().in("A", MGType::F3, "vec3(0.0)").emit(
        [](const MGNode&, const std::vector<std::string>& in, MGEmitCtx&) {
            return "clamp(" + in[0] + ", vec3(0.0), vec3(1.0))";
        });

    b.emplace_back("Power", "Math", MGType::F3);
    b.back().in("A", MGType::F3, "vec3(0.0)").in("Exp", MGType::F1, "2.0").emit(
        [](const MGNode&, const std::vector<std::string>& in, MGEmitCtx&) {
            return "pow(max(" + in[0] + ", vec3(0.0)), vec3(" + in[1] + "))";
        });

    b.emplace_back("Dot", "Math", MGType::F1);
    b.back().in("A", MGType::F3, "vec3(0.0)").in("B", MGType::F3, "vec3(0.0)").emit(
        [](const MGNode&, const std::vector<std::string>& in, MGEmitCtx&) {
            return "dot(" + in[0] + ", " + in[1] + ")";
        });

    // ---- Output (special: consumed by the generator, never emits an expr) ----
    b.emplace_back("Output", "Output", MGType::F3);
    b.back().in("BaseColor", MGType::F3, "vec3(0.8)")
        .in("Metallic", MGType::F1, "0.0")
        .in("Roughness", MGType::F1, "0.5")
        .in("Emissive", MGType::F3, "vec3(0.0)")
        .in("Opacity", MGType::F1, "1.0")
        .in("AO", MGType::F1, "1.0")
        .params(nullptr, "Transparent (1 = blend)");

    std::vector<MGNodeDef> out;
    out.reserve(b.size());
    for (auto& x : b) out.push_back(std::move(x.d));
    return out;
}

} // namespace

const std::vector<MGNodeDef>& materialNodeDefs() {
    static const std::vector<MGNodeDef> g = buildDefs();
    return g;
}
const MGNodeDef* materialNodeDef(const std::string& type) {
    for (const auto& d : materialNodeDefs())
        if (d.type == type) return &d;
    return nullptr;
}

// ---- codegen ------------------------------------------------------------------
namespace {

struct Generator {
    const MaterialGraph& g;
    MGEmitCtx& ctx;
    std::ostringstream lines;
    std::map<int, std::string> emitted; // nodeIdx -> var name
    std::set<int> inProgress;           // cycle guard
    int varCounter = 0;

    // Emits node `idx` (once) and returns an expression of its out type.
    std::string emitNode(int idx) {
        auto it = emitted.find(idx);
        if (it != emitted.end()) return it->second;
        if (inProgress.count(idx)) return "vec3(0.0)"; // cycle: dead value
        inProgress.insert(idx);

        const MGNode& n = g.nodes[idx];
        const MGNodeDef* def = materialNodeDef(n.type);
        if (!def || !def->emit) { inProgress.erase(idx); return "vec3(0.0)"; }

        std::vector<std::string> inExpr;
        for (size_t p = 0; p < def->in.size(); ++p)
            inExpr.push_back(inputExpr(n, *def, (int)p));

        std::string expr = def->emit(n, inExpr, ctx);
        std::string var = "mg" + std::to_string(varCounter++);
        lines << "    " << glslType(def->out) << " " << var << " = " << expr << ";\n";
        emitted[idx] = var;
        inProgress.erase(idx);
        return var;
    }

    // Expression for one input pin (linked node converted to pin type, or default).
    std::string inputExpr(const MGNode& n, const MGNodeDef& def, int pin) {
        if (pin < (int)n.in.size() && !n.in[pin].empty()) {
            int src = g.indexOf(n.in[pin]);
            if (src >= 0) {
                const MGNodeDef* sd = materialNodeDef(g.nodes[src].type);
                if (sd && sd->emit)
                    return convert(emitNode(src), sd->out, def.in[pin].type);
            }
        }
        return def.in[pin].defaultExpr;
    }
};

} // namespace

bool generateMaterialGLSL(const MaterialGraph& g, std::string& declsOut, std::string& codeOut,
                          std::vector<std::pair<std::string, bool>>& texturesOut) {
    int out = g.outputNode();
    if (out < 0) return false;
    const MGNode& o = g.nodes[out];
    const MGNodeDef* odef = materialNodeDef("Output");

    MGEmitCtx ctx;
    Generator gen{g, ctx};

    std::string base = gen.inputExpr(o, *odef, 0);
    std::string metal = gen.inputExpr(o, *odef, 1);
    std::string rough = gen.inputExpr(o, *odef, 2);
    std::string emis = gen.inputExpr(o, *odef, 3);
    std::string opac = gen.inputExpr(o, *odef, 4);
    std::string aoE = gen.inputExpr(o, *odef, 5);

    std::ostringstream code;
    code << gen.lines.str();
    code << "    albedo = " << base << ";\n";
    code << "    metallic = " << metal << ";\n";
    code << "    roughness = " << rough << ";\n";
    code << "    emissiveV = " << emis << ";\n";
    code << "    alpha = uBaseColor.a * (" << opac << ");\n";
    code << "    ao = " << aoE << ";\n";
    codeOut = code.str();

    std::ostringstream decls;
    for (size_t i = 0; i < ctx.textures.size(); ++i)
        decls << "layout(binding = " << (10 + i) << ") uniform sampler2D uMGTex" << i << ";\n";
    declsOut = decls.str();
    texturesOut = ctx.textures;
    return true;
}

// ---- (de)serialization ------------------------------------------------------------
static std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

bool saveMaterialGraph(const MaterialGraph& g, const std::string& path) {
    std::ostringstream o;
    o << "{\n  \"materialGraph\": 1,\n  \"nodes\": [\n";
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const MGNode& n = g.nodes[i];
        o << "    { \"id\": \"" << esc(n.id) << "\", \"type\": \"" << esc(n.type) << "\"";
        if (!n.p.empty()) o << ", \"p\": \"" << esc(n.p) << "\"";
        if (n.n != 0.0f) o << ", \"n\": " << n.n;
        if (n.v.x != 0 || n.v.y != 0 || n.v.z != 0)
            o << ", \"v\": [" << n.v.x << ", " << n.v.y << ", " << n.v.z << "]";
        o << ", \"in\": [";
        for (size_t p = 0; p < n.in.size(); ++p)
            o << (p ? ", " : "") << "\"" << esc(n.in[p]) << "\"";
        o << "], \"x\": " << n.x << ", \"y\": " << n.y << " }";
        o << (i + 1 < g.nodes.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        AE_ERROR("[MatGraph] cannot write %s", path.c_str());
        return false;
    }
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    AE_LOG("[MatGraph] saved %d nodes to %s", (int)g.nodes.size(), path.c_str());
    return f.good();
}

bool loadMaterialGraph(MaterialGraph& g, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_ERROR("[MatGraph] cannot open %s", path.c_str());
        return false;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root) || !root.find("nodes")) {
        AE_ERROR("[MatGraph] malformed graph: %s", path.c_str());
        return false;
    }
    g = MaterialGraph{};
    const JsonValue& nodes = *root.find("nodes");
    for (size_t i = 0; i < nodes.size(); ++i) {
        const JsonValue& jn = nodes[i];
        MGNode n;
        if (const std::string* s = jn.string("id")) n.id = *s;
        if (const std::string* s = jn.string("type")) n.type = *s;
        if (const std::string* s = jn.string("p")) n.p = *s;
        n.n = (float)jn.num("n", 0.0);
        if (const JsonValue* a = jn.find("v"))
            if (a->size() == 3)
                n.v = Vec3((float)(*a)[0].number, (float)(*a)[1].number, (float)(*a)[2].number);
        if (const JsonValue* a = jn.find("in"))
            for (size_t p = 0; p < a->size(); ++p) n.in.push_back((*a)[p].str);
        n.x = (float)jn.num("x", 0.0);
        n.y = (float)jn.num("y", 0.0);
        if (const MGNodeDef* def = materialNodeDef(n.type))
            while (n.in.size() < def->in.size()) n.in.push_back("");
        g.nodes.push_back(std::move(n));
    }
    return true;
}

// ---- asset compilation --------------------------------------------------------------
bool MaterialGraphAsset::compile(const std::string& absPath) {
    valid = false;
    path = absPath;
    if (!loadMaterialGraph(graph, absPath)) return false;

    std::string decls, code;
    std::vector<std::pair<std::string, bool>> texRefs;
    if (!generateMaterialGLSL(graph, decls, code, texRefs)) {
        AE_ERROR("[MatGraph] %s has no Output node", absPath.c_str());
        return false;
    }

    // Splice the generated block into the standard PBR fragment source.
    std::string fs = loadShaderSource("pbr.frag");
    std::string vs = loadShaderSource("pbr.vert");
    if (fs.empty() || vs.empty()) return false;
    size_t ver = fs.find('\n'); // after "#version 450 core"
    if (ver == std::string::npos) return false;
    fs.insert(ver + 1, "#define MATERIAL_GRAPH\n");
    size_t dm = fs.find("//__MG_DECLS__");
    if (dm != std::string::npos) fs.replace(dm, strlen("//__MG_DECLS__"), decls);
    size_t cm = fs.find("//__MG_CODE__");
    if (cm != std::string::npos) fs.replace(cm, strlen("//__MG_CODE__"), code);

    if (!shader.loadFromSource(vs, fs, absPath.c_str())) {
        AE_ERROR("[MatGraph] shader compile failed: %s", absPath.c_str());
        return false;
    }

    // Load referenced textures (relative paths resolve against the graph file's
    // project — the caller passes absolute paths in `p` after resolution, or
    // project-relative which we resolve against the graph's directory root).
    for (auto& t : textures) t.destroy();
    textures.clear();
    textures.resize(texRefs.size());
    for (size_t i = 0; i < texRefs.size(); ++i) {
        std::string tp = texRefs[i].first;
        if (tp.empty()) continue;
        // Project-relative: resolve against the assets/ root two levels up
        // from assets/materials/<file>.json.
        if (!(tp.size() > 1 && (tp[1] == ':' || tp[0] == '/'))) {
            std::string dir = absPath;
            size_t s1 = dir.find_last_of("\\/");
            if (s1 != std::string::npos) dir = dir.substr(0, s1);          // .../materials
            size_t s2 = dir.find_last_of("\\/");
            if (s2 != std::string::npos) dir = dir.substr(0, s2);          // .../assets
            size_t s3 = dir.find_last_of("\\/");
            if (s3 != std::string::npos) dir = dir.substr(0, s3);          // project root
            tp = dir + "/" + tp;
        }
        std::ifstream tf(tp, std::ios::binary | std::ios::ate);
        if (!tf) {
            AE_WARN("[MatGraph] missing texture: %s", tp.c_str());
            continue;
        }
        size_t sz = (size_t)tf.tellg();
        tf.seekg(0);
        std::vector<uint8_t> bytes(sz);
        tf.read((char*)bytes.data(), (std::streamsize)sz);
        ImageData img;
        if (decodeImage(bytes.data(), bytes.size(), img))
            textures[i].createCompressed(img.width, img.height, img.rgba.data(),
                                         texRefs[i].second,
                                         contentHash64(bytes.data(), bytes.size()));
    }

    int outIdx = graph.outputNode();
    blend = outIdx >= 0 && graph.nodes[outIdx].n != 0.0f;
    valid = true;
    AE_LOG("[MatGraph] compiled %s (%d nodes, %d textures%s)", absPath.c_str(),
           (int)graph.nodes.size(), (int)textures.size(), blend ? ", blended" : "");
    return true;
}

} // namespace ae
