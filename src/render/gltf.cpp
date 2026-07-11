#include "gltf.h"
#include <cctype>
#include "image.h"
#include "../core/json.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <unordered_set>

namespace ae {

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    out.resize((size_t)f.tellg());
    f.seekg(0);
    f.read((char*)out.data(), out.size());
    return f.good() || f.eof();
}

std::vector<uint8_t> base64Decode(const char* s, size_t len) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(len * 3 / 4);
    int acc = 0, bits = 0;
    for (size_t i = 0; i < len; ++i) {
        int v = val(s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return out;
}

constexpr int CT_BYTE = 5120, CT_UBYTE = 5121, CT_SHORT = 5122, CT_USHORT = 5123,
              CT_UINT = 5125, CT_FLOAT = 5126;

int componentSize(int ct) {
    switch (ct) {
    case CT_BYTE: case CT_UBYTE: return 1;
    case CT_SHORT: case CT_USHORT: return 2;
    default: return 4;
    }
}

int typeComponents(const std::string& t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2") return 2;
    if (t == "VEC3") return 3;
    if (t == "VEC4") return 4;
    if (t == "MAT4") return 16;
    return 0;
}

struct AccessorView {
    const uint8_t* data = nullptr;
    size_t stride = 0;
    size_t count = 0;
    int componentType = 0;
    int components = 0;

    bool valid() const { return data != nullptr && count > 0; }

    void readFloats(size_t i, float* out, int n) const {
        const uint8_t* e = data + i * stride;
        for (int c = 0; c < n; ++c) {
            if (c >= components) { out[c] = 0; continue; }
            switch (componentType) {
            case CT_FLOAT: std::memcpy(&out[c], e + c * 4, 4); break;
            case CT_UBYTE: out[c] = e[c] / 255.0f; break;
            case CT_USHORT: { uint16_t v; std::memcpy(&v, e + c * 2, 2); out[c] = v / 65535.0f; break; }
            case CT_BYTE: { int8_t v; std::memcpy(&v, e + c, 1); out[c] = v < -127 ? -1.0f : v / 127.0f; break; }
            case CT_SHORT: { int16_t v; std::memcpy(&v, e + c * 2, 2); out[c] = v < -32767 ? -1.0f : v / 32767.0f; break; }
            default: out[c] = 0; break;
            }
        }
    }

    void readUints(size_t i, uint32_t* out, int n) const {
        const uint8_t* e = data + i * stride;
        for (int c = 0; c < n; ++c) {
            if (c >= components) { out[c] = 0; continue; }
            switch (componentType) {
            case CT_UBYTE: out[c] = e[c]; break;
            case CT_USHORT: { uint16_t v; std::memcpy(&v, e + c * 2, 2); out[c] = v; break; }
            default: { uint32_t v; std::memcpy(&v, e + c * 4, 4); out[c] = v; break; }
            }
        }
    }

    uint32_t readIndex(size_t i) const {
        uint32_t v;
        readUints(i, &v, 1);
        return v;
    }
};

struct Loader {
    JsonValue doc;
    std::string baseDir;
    std::vector<std::vector<uint8_t>> buffers;

    bool loadBuffers(const std::vector<uint8_t>& glbBin) {
        const JsonValue* bufs = doc.find("buffers");
        if (!bufs) return true;
        buffers.resize(bufs->size());
        for (size_t i = 0; i < bufs->size(); ++i) {
            const JsonValue& b = (*bufs)[i];
            const std::string* uri = b.string("uri");
            if (!uri) {
                if (i == 0 && !glbBin.empty()) { buffers[i] = glbBin; continue; }
                std::fprintf(stderr, "[glTF] buffer %zu has no data\n", i);
                return false;
            }
            if (uri->rfind("data:", 0) == 0) {
                size_t comma = uri->find(',');
                if (comma == std::string::npos) return false;
                buffers[i] = base64Decode(uri->c_str() + comma + 1, uri->size() - comma - 1);
            } else if (!readFile(baseDir + *uri, buffers[i])) {
                std::fprintf(stderr, "[glTF] cannot read buffer file %s\n", uri->c_str());
                return false;
            }
        }
        return true;
    }

    AccessorView accessor(int index) const {
        AccessorView v;
        const JsonValue* accs = doc.find("accessors");
        if (!accs || index < 0 || (size_t)index >= accs->size()) return v;
        const JsonValue& a = (*accs)[index];

        v.componentType = a.integer("componentType", CT_FLOAT);
        v.count = (size_t)a.num("count", 0);
        const std::string* type = a.string("type");
        v.components = type ? typeComponents(*type) : 0;

        int bvIndex = a.integer("bufferView", -1);
        const JsonValue* bvs = doc.find("bufferViews");
        if (bvIndex < 0 || !bvs || (size_t)bvIndex >= bvs->size()) return v;
        const JsonValue& bv = (*bvs)[bvIndex];

        int bufIndex = bv.integer("buffer", 0);
        if (bufIndex < 0 || (size_t)bufIndex >= buffers.size()) return v;
        const std::vector<uint8_t>& buf = buffers[bufIndex];

        size_t elemSize = (size_t)componentSize(v.componentType) * v.components;
        v.stride = (size_t)bv.num("byteStride", 0);
        if (v.stride == 0) v.stride = elemSize;

        size_t offset = (size_t)bv.num("byteOffset", 0) + (size_t)a.num("byteOffset", 0);
        if (offset + (v.count ? (v.count - 1) * v.stride + elemSize : 0) > buf.size()) {
            std::fprintf(stderr, "[glTF] accessor %d out of buffer bounds\n", index);
            return AccessorView{};
        }
        v.data = buf.data() + offset;
        return v;
    }

    bool bufferViewBytes(int index, const uint8_t** data, size_t* size) const {
        const JsonValue* bvs = doc.find("bufferViews");
        if (!bvs || index < 0 || (size_t)index >= bvs->size()) return false;
        const JsonValue& bv = (*bvs)[index];
        int bufIndex = bv.integer("buffer", 0);
        if ((size_t)bufIndex >= buffers.size()) return false;
        const std::vector<uint8_t>& buf = buffers[bufIndex];
        size_t offset = (size_t)bv.num("byteOffset", 0);
        size_t len = (size_t)bv.num("byteLength", 0);
        if (offset + len > buf.size()) return false;
        *data = buf.data() + offset;
        *size = len;
        return true;
    }
};

// Normalized quaternion lerp with hemisphere correction (nlerp ~ slerp for
// the small per-keyframe angles found in animation data).
Vec4 nlerpQuat(const Vec4& a, const Vec4& b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    float sign = d < 0.0f ? -1.0f : 1.0f;
    Vec4 r(a.x + (b.x * sign - a.x) * t,
           a.y + (b.y * sign - a.y) * t,
           a.z + (b.z * sign - a.z) * t,
           a.w + (b.w * sign - a.w) * t);
    float len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (len < 1e-8f) return Vec4(0, 0, 0, 1);
    return Vec4(r.x / len, r.y / len, r.z / len, r.w / len);
}

} // namespace

bool Model::load(const char* path) {
    std::string p(path);
    size_t dot = p.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : p.substr(dot);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".obj") return loadObj(path);
    if (ext == ".fbx") return loadFbx(path);
    return loadGltf(path);
}

bool Model::loadGltf(const char* path) {
    std::vector<uint8_t> file;
    if (!readFile(path, file)) {
        std::fprintf(stderr, "[glTF] cannot open %s\n", path);
        return false;
    }

    Loader loader;
    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    loader.baseDir = slash == std::string::npos ? "" : p.substr(0, slash + 1);

    std::vector<uint8_t> glbBin;
    if (file.size() >= 12 && !std::memcmp(file.data(), "glTF", 4)) {
        size_t pos = 12;
        std::string jsonText;
        while (pos + 8 <= file.size()) {
            uint32_t chunkLen, chunkType;
            std::memcpy(&chunkLen, file.data() + pos, 4);
            std::memcpy(&chunkType, file.data() + pos + 4, 4);
            pos += 8;
            if (pos + chunkLen > file.size()) break;
            if (chunkType == 0x4E4F534A)
                jsonText.assign((char*)file.data() + pos, chunkLen);
            else if (chunkType == 0x004E4942)
                glbBin.assign(file.data() + pos, file.data() + pos + chunkLen);
            pos += chunkLen;
        }
        if (!jsonParse(jsonText.c_str(), jsonText.size(), loader.doc)) {
            std::fprintf(stderr, "[glTF] GLB JSON parse failed\n");
            return false;
        }
    } else {
        if (!jsonParse((char*)file.data(), file.size(), loader.doc)) {
            std::fprintf(stderr, "[glTF] JSON parse failed\n");
            return false;
        }
    }

    if (!loader.loadBuffers(glbBin)) return false;
    const JsonValue& doc = loader.doc;

    // ---- images (baseColor/emissive are sRGB) ----
    std::unordered_set<int> srgbImages;
    const JsonValue* texturesJson = doc.find("textures");
    auto imageOfTexture = [&](int texIndex) -> int {
        if (!texturesJson || texIndex < 0 || (size_t)texIndex >= texturesJson->size()) return -1;
        return (*texturesJson)[texIndex].integer("source", -1);
    };
    const JsonValue* materialsJson = doc.find("materials");
    if (materialsJson) {
        for (size_t i = 0; i < materialsJson->size(); ++i) {
            const JsonValue& m = (*materialsJson)[i];
            if (const JsonValue* pbr = m.find("pbrMetallicRoughness"))
                if (const JsonValue* t = pbr->find("baseColorTexture"))
                    srgbImages.insert(imageOfTexture(t->integer("index", -1)));
            if (const JsonValue* t = m.find("emissiveTexture"))
                srgbImages.insert(imageOfTexture(t->integer("index", -1)));
        }
    }

    const JsonValue* imagesJson = doc.find("images");
    std::vector<unsigned> imageTex(imagesJson ? imagesJson->size() : 0, 0);
    if (imagesJson) {
        textures_.reserve(imagesJson->size());
        for (size_t i = 0; i < imagesJson->size(); ++i) {
            const JsonValue& img = (*imagesJson)[i];
            std::vector<uint8_t> fileBytes;
            const uint8_t* bytes = nullptr;
            size_t size = 0;
            int bvIndex = img.integer("bufferView", -1);
            if (bvIndex >= 0) {
                if (!loader.bufferViewBytes(bvIndex, &bytes, &size)) continue;
            } else if (const std::string* uri = img.string("uri")) {
                if (uri->rfind("data:", 0) == 0) {
                    size_t comma = uri->find(',');
                    if (comma == std::string::npos) continue;
                    fileBytes = base64Decode(uri->c_str() + comma + 1, uri->size() - comma - 1);
                } else if (!readFile(loader.baseDir + *uri, fileBytes)) {
                    std::fprintf(stderr, "[glTF] cannot read image %s\n", uri->c_str());
                    continue;
                }
                bytes = fileBytes.data();
                size = fileBytes.size();
            } else {
                continue;
            }

            ImageData decoded;
            if (!decodeImage(bytes, size, decoded)) {
                std::fprintf(stderr, "[glTF] image %zu decode failed\n", i);
                continue;
            }
            textures_.emplace_back();
            textures_.back().createCompressed(decoded.width, decoded.height,
                                              decoded.rgba.data(),
                                              srgbImages.count((int)i) != 0,
                                              contentHash64(bytes, size));
            imageTex[i] = textures_.back().id();
        }
    }

    auto glTexture = [&](const JsonValue& parent, const char* key, float* scaleOut = nullptr,
                         const char* scaleKey = nullptr) -> unsigned {
        const JsonValue* t = parent.find(key);
        if (!t) return 0;
        if (scaleOut && scaleKey) *scaleOut = (float)t->num(scaleKey, 1.0);
        int img = imageOfTexture(t->integer("index", -1));
        return img >= 0 && (size_t)img < imageTex.size() ? imageTex[img] : 0;
    };

    // ---- materials ----
    if (materialsJson) {
        materials_.resize(materialsJson->size());
        for (size_t i = 0; i < materialsJson->size(); ++i) {
            const JsonValue& m = (*materialsJson)[i];
            Material& mat = materials_[i];
            mat.metallic = 1.0f;
            mat.roughness = 1.0f;
            if (const JsonValue* pbr = m.find("pbrMetallicRoughness")) {
                if (const JsonValue* f = pbr->find("baseColorFactor"))
                    if (f->size() == 4)
                        mat.baseColor = Vec4((float)(*f)[0].number, (float)(*f)[1].number,
                                             (float)(*f)[2].number, (float)(*f)[3].number);
                mat.metallic = (float)pbr->num("metallicFactor", 1.0);
                mat.roughness = (float)pbr->num("roughnessFactor", 1.0);
                mat.albedoTex = glTexture(*pbr, "baseColorTexture");
                mat.mrTex = glTexture(*pbr, "metallicRoughnessTexture");
            }
            mat.normalTex = glTexture(m, "normalTexture", &mat.normalScale, "scale");
            mat.occlusionTex = glTexture(m, "occlusionTexture", &mat.occlusionStrength, "strength");
            mat.emissiveTex = glTexture(m, "emissiveTexture");
            if (const JsonValue* e = m.find("emissiveFactor"))
                if (e->size() == 3)
                    mat.emissive = Vec3((float)(*e)[0].number, (float)(*e)[1].number, (float)(*e)[2].number);
            const std::string* alphaMode = m.string("alphaMode");
            if (alphaMode && *alphaMode == "MASK")
                mat.alphaCutoff = (float)m.num("alphaCutoff", 0.5);
            if (alphaMode && *alphaMode == "BLEND") mat.blend = true;
            mat.doubleSided = m.flag("doubleSided", false);
        }
    }

    // ---- meshes: one engine Mesh per primitive ----
    const JsonValue* meshesJson = doc.find("meshes");
    std::vector<std::vector<std::pair<int, int>>> primitiveIndex(meshesJson ? meshesJson->size() : 0);
    if (meshesJson) {
        for (size_t mi = 0; mi < meshesJson->size(); ++mi) {
            const JsonValue* prims = (*meshesJson)[mi].find("primitives");
            if (!prims) continue;
            for (size_t pi = 0; pi < prims->size(); ++pi) {
                const JsonValue& prim = (*prims)[pi];
                if (prim.integer("mode", 4) != 4) continue;
                const JsonValue* attrs = prim.find("attributes");
                if (!attrs) continue;

                AccessorView pos = loader.accessor(attrs->integer("POSITION", -1));
                if (!pos.valid()) continue;
                AccessorView nrm = loader.accessor(attrs->integer("NORMAL", -1));
                AccessorView tan = loader.accessor(attrs->integer("TANGENT", -1));
                AccessorView uv = loader.accessor(attrs->integer("TEXCOORD_0", -1));
                AccessorView joints = loader.accessor(attrs->integer("JOINTS_0", -1));
                AccessorView weights = loader.accessor(attrs->integer("WEIGHTS_0", -1));

                MeshData data;
                data.vertices.resize(pos.count);
                for (size_t v = 0; v < pos.count; ++v) {
                    Vertex& vert = data.vertices[v];
                    float tmp[4];
                    pos.readFloats(v, tmp, 3);
                    vert.position = Vec3(tmp[0], tmp[1], tmp[2]);
                    boundsMin_ = Vec3(std::fmin(boundsMin_.x, tmp[0]), std::fmin(boundsMin_.y, tmp[1]),
                                      std::fmin(boundsMin_.z, tmp[2]));
                    boundsMax_ = Vec3(std::fmax(boundsMax_.x, tmp[0]), std::fmax(boundsMax_.y, tmp[1]),
                                      std::fmax(boundsMax_.z, tmp[2]));
                    if (nrm.valid() && v < nrm.count) {
                        nrm.readFloats(v, tmp, 3);
                        vert.normal = Vec3(tmp[0], tmp[1], tmp[2]);
                    }
                    if (tan.valid() && v < tan.count) {
                        tan.readFloats(v, tmp, 4);
                        vert.tangent = Vec4(Vec3(tmp[0], tmp[1], tmp[2]), tmp[3]);
                    }
                    if (uv.valid() && v < uv.count) {
                        uv.readFloats(v, tmp, 2);
                        vert.uv = Vec2(tmp[0], tmp[1]);
                    }
                    if (joints.valid() && weights.valid() && v < joints.count && v < weights.count) {
                        uint32_t j[4];
                        joints.readUints(v, j, 4);
                        weights.readFloats(v, tmp, 4);
                        float sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
                        float norm = sum > 1e-6f ? 1.0f / sum : 0.0f;
                        for (int c = 0; c < 4; ++c) {
                            vert.joints[c] = (float)j[c];
                            vert.weights[c] = tmp[c] * norm;
                        }
                    }
                }

                AccessorView idx = loader.accessor(prim.integer("indices", -1));
                if (idx.valid()) {
                    data.indices.resize(idx.count);
                    for (size_t k = 0; k < idx.count; ++k)
                        data.indices[k] = idx.readIndex(k);
                } else {
                    data.indices.resize(pos.count);
                    for (size_t k = 0; k < pos.count; ++k)
                        data.indices[k] = (uint32_t)k;
                }

                if (!tan.valid()) computeTangents(data);

                meshes_.emplace_back();
                meshes_.back().upload(data);
                primitiveIndex[mi].push_back({(int)meshes_.size() - 1, prim.integer("material", -1)});
            }
        }
    }

    // ---- nodes (persisted for animation) ----
    const JsonValue* nodesJson = doc.find("nodes");
    if (nodesJson) {
        nodes_.resize(nodesJson->size());
        for (size_t i = 0; i < nodesJson->size(); ++i) {
            const JsonValue& nj = (*nodesJson)[i];
            Node& n = nodes_[i];
            if (const JsonValue* m = nj.find("matrix")) {
                if (m->size() == 16) {
                    n.hasMatrix = true;
                    for (int c = 0; c < 4; ++c)
                        for (int rw = 0; rw < 4; ++rw)
                            n.matrix.m[c][rw] = (float)(*m)[c * 4 + rw].number;
                }
            }
            if (const JsonValue* tv = nj.find("translation"))
                if (tv->size() == 3)
                    n.t = Vec3((float)(*tv)[0].number, (float)(*tv)[1].number, (float)(*tv)[2].number);
            if (const JsonValue* rv = nj.find("rotation"))
                if (rv->size() == 4)
                    n.r = Vec4((float)(*rv)[0].number, (float)(*rv)[1].number,
                               (float)(*rv)[2].number, (float)(*rv)[3].number);
            if (const JsonValue* sv = nj.find("scale"))
                if (sv->size() == 3)
                    n.s = Vec3((float)(*sv)[0].number, (float)(*sv)[1].number, (float)(*sv)[2].number);
            if (const JsonValue* children = nj.find("children")) {
                for (size_t c = 0; c < children->size(); ++c) {
                    int child = (int)(*children)[c].number;
                    n.children.push_back(child);
                    if ((size_t)child < nodesJson->size()) {
                        // parent set below once all nodes exist
                    }
                }
            }

            int meshIndex = nj.integer("mesh", -1);
            if (meshIndex >= 0 && (size_t)meshIndex < primitiveIndex.size()) {
                for (auto [engineMesh, material] : primitiveIndex[meshIndex]) {
                    Draw d;
                    d.node = (int)i;
                    d.mesh = engineMesh;
                    d.material = material;
                    d.skin = nj.integer("skin", -1);
                    draws_.push_back(d);
                }
            }
        }
        for (size_t i = 0; i < nodes_.size(); ++i)
            for (int c : nodes_[i].children)
                if ((size_t)c < nodes_.size()) nodes_[c].parent = (int)i;
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].parent < 0) roots_.push_back((int)i);
    }

    // ---- skins ----
    if (const JsonValue* skinsJson = doc.find("skins")) {
        skins_.resize(skinsJson->size());
        for (size_t i = 0; i < skinsJson->size(); ++i) {
            const JsonValue& sj = (*skinsJson)[i];
            Skin& skin = skins_[i];
            if (const JsonValue* joints = sj.find("joints"))
                for (size_t j = 0; j < joints->size(); ++j)
                    skin.joints.push_back((int)(*joints)[j].number);
            skin.inverseBind.assign(skin.joints.size(), Mat4::identity());
            AccessorView ibm = loader.accessor(sj.integer("inverseBindMatrices", -1));
            if (ibm.valid()) {
                for (size_t j = 0; j < skin.joints.size() && j < ibm.count; ++j)
                    ibm.readFloats(j, &skin.inverseBind[j].m[0][0], 16);
            }
            skin.palette.assign(skin.joints.size(), Mat4::identity());
        }
    }

    // ---- animations ----
    if (const JsonValue* animsJson = doc.find("animations")) {
        clips_.resize(animsJson->size());
        for (size_t a = 0; a < animsJson->size(); ++a) {
            const JsonValue& aj = (*animsJson)[a];
            Clip& clip = clips_[a];
            const std::string* name = aj.string("name");
            clip.name = name ? *name : "clip" + std::to_string(a);

            const JsonValue* samplers = aj.find("samplers");
            const JsonValue* channels = aj.find("channels");
            if (!samplers || !channels) continue;
            for (size_t c = 0; c < channels->size(); ++c) {
                const JsonValue& cj = (*channels)[c];
                const JsonValue* target = cj.find("target");
                if (!target) continue;
                const std::string* pathStr = target->string("path");
                if (!pathStr) continue;
                int path;
                if (*pathStr == "translation") path = 0;
                else if (*pathStr == "rotation") path = 1;
                else if (*pathStr == "scale") path = 2;
                else continue; // morph weights unsupported

                int samplerIdx = cj.integer("sampler", -1);
                if (samplerIdx < 0 || (size_t)samplerIdx >= samplers->size()) continue;
                const JsonValue& sj = (*samplers)[samplerIdx];

                Channel ch;
                ch.node = target->integer("node", -1);
                ch.path = path;
                const std::string* interp = sj.string("interpolation");
                if (interp && *interp == "STEP") ch.interp = 1;
                else if (interp && *interp == "CUBICSPLINE") ch.interp = 2;

                AccessorView input = loader.accessor(sj.integer("input", -1));
                AccessorView output = loader.accessor(sj.integer("output", -1));
                if (!input.valid() || !output.valid()) continue;
                ch.times.resize(input.count);
                for (size_t k = 0; k < input.count; ++k)
                    input.readFloats(k, &ch.times[k], 1);
                int comp = path == 1 ? 4 : 3;
                ch.values.resize(output.count * comp);
                for (size_t k = 0; k < output.count; ++k)
                    output.readFloats(k, &ch.values[k * comp], comp);
                if (!ch.times.empty())
                    clip.duration = std::fmax(clip.duration, ch.times.back());
                clip.channels.push_back(std::move(ch));
            }
        }
    }

    computeWorlds();
    for (auto& skin : skins_)
        for (size_t j = 0; j < skin.joints.size(); ++j)
            skin.palette[j] = nodes_[skin.joints[j]].world * skin.inverseBind[j];

    std::printf("[glTF] %s: %zu meshes, %zu materials, %zu textures, %zu draws, "
                "%zu skins, %zu clips\n",
                path, meshes_.size(), materials_.size(), textures_.size(), draws_.size(),
                skins_.size(), clips_.size());
    return !meshes_.empty();
}

void Model::computeWorlds() {
    // Depth-first from roots so parents are always resolved first.
    std::vector<int> stack(roots_.rbegin(), roots_.rend());
    for (int r : roots_) nodes_[r].world = Mat4::identity();
    std::vector<Mat4> parentWorld(nodes_.size(), Mat4::identity());
    while (!stack.empty()) {
        int ni = stack.back();
        stack.pop_back();
        Node& n = nodes_[ni];
        Mat4 local = n.hasMatrix
                         ? n.matrix
                         : translate(n.t) * quatToMat4(n.r.x, n.r.y, n.r.z, n.r.w) * scale(n.s);
        n.world = parentWorld[ni] * local;
        for (int c : n.children) {
            parentWorld[c] = n.world;
            stack.push_back(c);
        }
    }
}

void Model::sample(float time) {
    if (clips_.empty() || activeClip_ < 0 || (size_t)activeClip_ >= clips_.size()) return;
    const Clip& clip = clips_[activeClip_];
    float t = clip.duration > 0 ? std::fmod(time, clip.duration) : 0.0f;

    for (const Channel& ch : clip.channels) {
        if (ch.node < 0 || (size_t)ch.node >= nodes_.size() || ch.times.empty()) continue;
        Node& n = nodes_[ch.node];
        n.hasMatrix = false; // animated nodes always use TRS

        // Locate the keyframe segment (times are sorted ascending).
        size_t k = 0;
        while (k + 1 < ch.times.size() && ch.times[k + 1] < t) ++k;
        size_t k1 = k + 1 < ch.times.size() ? k + 1 : k;
        float t0 = ch.times[k], t1 = ch.times[k1];
        float f = (ch.interp == 1 || t1 <= t0) ? 0.0f : clampf((t - t0) / (t1 - t0), 0.0f, 1.0f);

        int comp = ch.path == 1 ? 4 : 3;
        // CUBICSPLINE stores in-tangent/value/out-tangent triplets; sample values.
        int keyStride = ch.interp == 2 ? comp * 3 : comp;
        int valueOffset = ch.interp == 2 ? comp : 0;
        const float* v0 = &ch.values[k * keyStride + valueOffset];
        const float* v1 = &ch.values[k1 * keyStride + valueOffset];

        if (ch.path == 1) {
            Vec4 q = nlerpQuat(Vec4(v0[0], v0[1], v0[2], v0[3]),
                               Vec4(v1[0], v1[1], v1[2], v1[3]), f);
            n.r = q;
        } else {
            Vec3 val(lerpf(v0[0], v1[0], f), lerpf(v0[1], v1[1], f), lerpf(v0[2], v1[2], f));
            if (ch.path == 0) n.t = val;
            else n.s = val;
        }
    }

    computeWorlds();
    for (auto& skin : skins_)
        for (size_t j = 0; j < skin.joints.size(); ++j)
            skin.palette[j] = nodes_[skin.joints[j]].world * skin.inverseBind[j];
}

int Model::clipIndex(const std::string& name) const {
    for (size_t i = 0; i < clips_.size(); ++i)
        if (clips_[i].name == name) return (int)i;
    return -1;
}

float Model::clipDuration(int i) const {
    return (i >= 0 && (size_t)i < clips_.size()) ? clips_[i].duration : 0.0f;
}

namespace {
// One node's local TRS — the unit of pose blending.
struct PoseTRS {
    Vec3 t, s;
    Vec4 r;
};
} // namespace

void Model::sampleClipTime(int clip, float t) {
    sampleBlended(clip, t, clip, t, 0.0f);
}

void Model::sampleBlended(int clipA, float tA, int clipB, float tB, float w) {
    if (clips_.empty()) return;
    if (clipA < 0 || (size_t)clipA >= clips_.size()) clipA = 0;
    if (clipB < 0 || (size_t)clipB >= clips_.size()) clipB = clipA;

    // Start both poses from the nodes' current TRS so channels a clip doesn't
    // animate stay put (and blend as no-ops).
    std::vector<PoseTRS> poseA(nodes_.size()), poseB(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i) {
        poseA[i] = {nodes_[i].t, nodes_[i].s, nodes_[i].r};
        poseB[i] = poseA[i];
    }

    auto evalInto = [&](int clipIdx, float t, std::vector<PoseTRS>& pose) {
        const Clip& clip = clips_[clipIdx];
        for (const Channel& ch : clip.channels) {
            if (ch.node < 0 || (size_t)ch.node >= nodes_.size() || ch.times.empty()) continue;
            nodes_[ch.node].hasMatrix = false;
            size_t k = 0;
            while (k + 1 < ch.times.size() && ch.times[k + 1] < t) ++k;
            size_t k1 = k + 1 < ch.times.size() ? k + 1 : k;
            float t0 = ch.times[k], t1 = ch.times[k1];
            float f = (ch.interp == 1 || t1 <= t0) ? 0.0f
                                                   : clampf((t - t0) / (t1 - t0), 0.0f, 1.0f);
            int comp = ch.path == 1 ? 4 : 3;
            int keyStride = ch.interp == 2 ? comp * 3 : comp;
            int valueOffset = ch.interp == 2 ? comp : 0;
            const float* v0 = &ch.values[k * keyStride + valueOffset];
            const float* v1 = &ch.values[k1 * keyStride + valueOffset];
            PoseTRS& p = pose[ch.node];
            if (ch.path == 1) {
                p.r = nlerpQuat(Vec4(v0[0], v0[1], v0[2], v0[3]),
                                Vec4(v1[0], v1[1], v1[2], v1[3]), f);
            } else {
                Vec3 val(lerpf(v0[0], v1[0], f), lerpf(v0[1], v1[1], f), lerpf(v0[2], v1[2], f));
                if (ch.path == 0) p.t = val;
                else p.s = val;
            }
        }
    };
    evalInto(clipA, tA, poseA);
    if (w > 0.0001f) evalInto(clipB, tB, poseB);

    float bw = clampf(w, 0.0f, 1.0f);
    for (size_t i = 0; i < nodes_.size(); ++i) {
        Node& n = nodes_[i];
        if (bw <= 0.0001f) {
            n.t = poseA[i].t;
            n.r = poseA[i].r;
            n.s = poseA[i].s;
        } else {
            n.t = poseA[i].t + (poseB[i].t - poseA[i].t) * bw;
            n.s = poseA[i].s + (poseB[i].s - poseA[i].s) * bw;
            n.r = nlerpQuat(poseA[i].r, poseB[i].r, bw);
        }
    }

    computeWorlds();
    for (auto& skin : skins_)
        for (size_t j = 0; j < skin.joints.size(); ++j)
            skin.palette[j] = nodes_[skin.joints[j]].world * skin.inverseBind[j];
}

void Model::emit(RenderScene& out, const Mat4& base) const {
    for (const Draw& d : draws_) {
        Renderable e;
        e.mesh = &meshes_[d.mesh];
        e.boundsMin = e.mesh->boundsMin();
        e.boundsMax = e.mesh->boundsMax();
        if (d.material >= 0 && (size_t)d.material < materials_.size())
            e.material = materials_[d.material];
        if (d.skin >= 0 && (size_t)d.skin < skins_.size()) {
            // Skinned: the palette carries the pose in model space, so the
            // per-vertex skinning matrix already includes the node transform.
            e.transform = base;
            e.jointMatrices = &skins_[d.skin].palette;
        } else {
            e.transform = base * nodes_[d.node].world;
        }
        out.entities.push_back(e);
    }
}

void Model::destroy() {
    for (auto& m : meshes_) m.destroy();
    for (auto& t : textures_) t.destroy();
    meshes_.clear();
    textures_.clear();
    materials_.clear();
    nodes_.clear();
    roots_.clear();
    skins_.clear();
    clips_.clear();
    draws_.clear();
}

} // namespace ae
