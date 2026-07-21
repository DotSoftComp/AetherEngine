#include "gltf.h"
#include <algorithm>
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
    std::unordered_set<int> srgbImages, normalImages;
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
            // Normal maps get BC5 (two-channel) instead of BC1/BC3.
            if (const JsonValue* t = m.find("normalTexture"))
                normalImages.insert(imageOfTexture(t->integer("index", -1)));
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
            std::string diskPath; // external images can carry an import sidecar
            int bvIndex = img.integer("bufferView", -1);
            if (bvIndex >= 0) {
                if (!loader.bufferViewBytes(bvIndex, &bytes, &size)) continue;
            } else if (const std::string* uri = img.string("uri")) {
                if (uri->rfind("data:", 0) == 0) {
                    size_t comma = uri->find(',');
                    if (comma == std::string::npos) continue;
                    fileBytes = base64Decode(uri->c_str() + comma + 1, uri->size() - comma - 1);
                } else {
                    diskPath = loader.baseDir + *uri;
                    if (!readFile(diskPath, fileBytes)) {
                        std::fprintf(stderr, "[glTF] cannot read image %s\n", uri->c_str());
                        continue;
                    }
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
            // glTF's own material slots imply srgb/normal; a sidecar next to an
            // external image can still override (maxSize, mipBias, ...).
            TextureImportSettings ts;
            ts.srgb = srgbImages.count((int)i) != 0;
            ts.normalMap = normalImages.count((int)i) != 0;
            if (ts.normalMap) ts.srgb = false;
            if (!diskPath.empty()) ts = loadImportSettings(diskPath, ts);
            textures_.emplace_back();
            textures_.back().createImported(decoded.width, decoded.height, decoded.rgba.data(),
                                            ts, contentHash64(bytes, size));
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
    std::vector<int> meshTargetCount(meshesJson ? meshesJson->size() : 0, 0);
    std::vector<std::vector<float>> meshDefaultWeights(meshesJson ? meshesJson->size() : 0);
    if (meshesJson) {
        for (size_t mi = 0; mi < meshesJson->size(); ++mi) {
            const JsonValue* prims = (*meshesJson)[mi].find("primitives");
            if (!prims) continue;
            if (const JsonValue* dw = (*meshesJson)[mi].find("weights"))
                for (size_t w = 0; w < dw->size(); ++w)
                    meshDefaultWeights[mi].push_back((float)(*dw)[w].number);
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

                // Morph targets: POSITION/NORMAL deltas per target, CPU-blended
                // into per-instance dynamic meshes at pose finalize.
                const JsonValue* targetsJson = prim.find("targets");
                bool hasTargets = targetsJson && targetsJson->size() > 0;
                meshes_.emplace_back();
                meshes_.back().upload(data, hasTargets, /*keepNavGeo=*/true);
                int engineMesh = (int)meshes_.size() - 1;
                if (hasTargets) {
                    meshTargetCount[mi] =
                        std::max(meshTargetCount[mi], (int)targetsJson->size());
                    MorphData md;
                    for (size_t ti = 0; ti < targetsJson->size(); ++ti) {
                        const JsonValue& tj = (*targetsJson)[ti];
                        MorphData::Target tg;
                        AccessorView dp = loader.accessor(tj.integer("POSITION", -1));
                        if (dp.valid()) {
                            tg.dpos.resize(dp.count);
                            for (size_t v = 0; v < dp.count; ++v) {
                                float tmp[3];
                                dp.readFloats(v, tmp, 3);
                                tg.dpos[v] = Vec3(tmp[0], tmp[1], tmp[2]);
                            }
                        }
                        AccessorView dn = loader.accessor(tj.integer("NORMAL", -1));
                        if (dn.valid()) {
                            tg.dnrm.resize(dn.count);
                            for (size_t v = 0; v < dn.count; ++v) {
                                float tmp[3];
                                dn.readFloats(v, tmp, 3);
                                tg.dnrm[v] = Vec3(tmp[0], tmp[1], tmp[2]);
                            }
                        }
                        md.targets.push_back(std::move(tg));
                    }
                    md.base = std::move(data);
                    morphs_[engineMesh] = std::move(md);
                }
                primitiveIndex[mi].push_back({engineMesh, prim.integer("material", -1)});
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
            if (const std::string* nm = nj.string("name")) n.name = *nm;
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
                // Default morph weights: node override, else mesh, else zeros.
                if (meshTargetCount[meshIndex] > 0) {
                    std::vector<float> dw(meshTargetCount[meshIndex], 0.0f);
                    const JsonValue* nw = nj.find("weights");
                    const std::vector<float>& mw = meshDefaultWeights[meshIndex];
                    for (size_t w = 0; w < dw.size(); ++w) {
                        if (nw && w < nw->size()) dw[w] = (float)(*nw)[w].number;
                        else if (w < mw.size()) dw[w] = mw[w];
                    }
                    morphDefaults_[(int)i] = std::move(dw);
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
                else if (*pathStr == "weights") path = 3;
                else continue;

                int samplerIdx = cj.integer("sampler", -1);
                if (samplerIdx < 0 || (size_t)samplerIdx >= samplers->size()) continue;
                const JsonValue& sj = (*samplers)[samplerIdx];

                Channel ch;
                ch.node = target->integer("node", -1);
                ch.path = path;
                const std::string* interp = sj.string("interpolation");
                if (interp && *interp == "STEP") ch.interp = 1;
                else if (interp && *interp == "CUBICSPLINE") ch.interp = 2;

                int comp = path == 1 ? 4 : 3;
                if (path == 3) {
                    // One float per morph target per key (SCALAR accessor).
                    int meshIdx = nodesJson && ch.node >= 0 && (size_t)ch.node < nodesJson->size()
                                      ? (*nodesJson)[ch.node].integer("mesh", -1)
                                      : -1;
                    comp = meshIdx >= 0 && (size_t)meshIdx < meshTargetCount.size()
                               ? meshTargetCount[meshIdx]
                               : 0;
                    if (comp <= 0) continue;
                }
                ch.comp = comp;

                AccessorView input = loader.accessor(sj.integer("input", -1));
                AccessorView output = loader.accessor(sj.integer("output", -1));
                if (!input.valid() || !output.valid()) continue;
                ch.times.resize(input.count);
                for (size_t k = 0; k < input.count; ++k)
                    input.readFloats(k, &ch.times[k], 1);
                if (path == 3) {
                    ch.values.resize(output.count);
                    for (size_t k = 0; k < output.count; ++k)
                        output.readFloats(k, &ch.values[k], 1);
                } else {
                    ch.values.resize(output.count * comp);
                    for (size_t k = 0; k < output.count; ++k)
                        output.readFloats(k, &ch.values[k * comp], comp);
                }
                // Guard against inconsistent key/value counts (drop, don't crash).
                size_t keyStride = ch.interp == 2 ? (size_t)comp * 3 : (size_t)comp;
                if (ch.values.size() < ch.times.size() * keyStride) continue;
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

int Model::clipIndex(const std::string& name) const {
    for (size_t i = 0; i < clips_.size(); ++i)
        if (clips_[i].name == name) return (int)i;
    return -1;
}

float Model::clipDuration(int i) const {
    return (i >= 0 && (size_t)i < clips_.size()) ? clips_[i].duration : 0.0f;
}

int Model::nodeIndex(const std::string& name) const {
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].name == name) return (int)i;
    return -1;
}

std::vector<float> Model::subtreeMask(const std::string& bone) const {
    int root = nodeIndex(bone);
    if (root < 0) return {};
    std::vector<float> mask(nodes_.size(), 0.0f);
    std::vector<int> stack{root};
    while (!stack.empty()) {
        int n = stack.back();
        stack.pop_back();
        mask[n] = 1.0f;
        for (int c : nodes_[n].children) stack.push_back(c);
    }
    return mask;
}

// Sample one channel at time t into out[ch.comp] floats. CUBICSPLINE stores
// in-tangent/value/out-tangent triplets; we sample the values.
void Model::sampleChannel(const Channel& ch, float t, float* out) {
    size_t k = 0;
    while (k + 1 < ch.times.size() && ch.times[k + 1] < t) ++k;
    size_t k1 = k + 1 < ch.times.size() ? k + 1 : k;
    float t0 = ch.times[k], t1 = ch.times[k1];
    float f = (ch.interp == 1 || t1 <= t0) ? 0.0f : clampf((t - t0) / (t1 - t0), 0.0f, 1.0f);
    size_t keyStride = ch.interp == 2 ? (size_t)ch.comp * 3 : (size_t)ch.comp;
    size_t valueOffset = ch.interp == 2 ? (size_t)ch.comp : 0;
    const float* v0 = &ch.values[k * keyStride + valueOffset];
    const float* v1 = &ch.values[k1 * keyStride + valueOffset];
    if (ch.path == 1) {
        Vec4 q = nlerpQuat(Vec4(v0[0], v0[1], v0[2], v0[3]),
                           Vec4(v1[0], v1[1], v1[2], v1[3]), f);
        out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
    } else {
        for (int c = 0; c < ch.comp; ++c) out[c] = lerpf(v0[c], v1[c], f);
    }
}

bool Model::clipTranslation(int clip, int node, float t, Vec3& out) const {
    if (clip < 0 || (size_t)clip >= clips_.size() || node < 0) return false;
    for (const Channel& ch : clips_[clip].channels) {
        if (ch.node != node || ch.path != 0 || ch.times.empty()) continue;
        float v[4];
        sampleChannel(ch, t, v);
        out = Vec3(v[0], v[1], v[2]);
        return true;
    }
    return false;
}

int Model::guessRootBone() const {
    int best = -1, bestDepth = 1 << 30;
    for (const Clip& c : clips_) {
        for (const Channel& ch : c.channels) {
            if (ch.path != 0 || ch.node < 0 || (size_t)ch.node >= nodes_.size()) continue;
            int depth = 0;
            for (int n = ch.node; nodes_[n].parent >= 0; n = nodes_[n].parent) ++depth;
            if (depth < bestDepth) {
                bestDepth = depth;
                best = ch.node;
            }
        }
    }
    return best;
}

void Model::initPose(ModelPose& p) const {
    p.destroyGpu();
    p.locals.resize(nodes_.size());
    p.useMatrix.resize(nodes_.size());
    p.worlds.assign(nodes_.size(), Mat4::identity());
    p.palettes.resize(skins_.size());
    for (size_t s = 0; s < skins_.size(); ++s)
        p.palettes[s].assign(skins_[s].joints.size(), Mat4::identity());
    resetPoseLocals(p);
    finalizePose(p);
}

void Model::resetPoseLocals(ModelPose& p) const {
    if (p.locals.size() != nodes_.size()) return;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        p.locals[i] = {nodes_[i].t, nodes_[i].r, nodes_[i].s};
        p.useMatrix[i] = nodes_[i].hasMatrix ? 1 : 0;
    }
    // Explicit default weights on every morph node keep pose blending symmetric.
    p.morphWeights = morphDefaults_;
}

void Model::evalClip(int clip, float t, ModelPose& p) const {
    if (clip < 0 || (size_t)clip >= clips_.size() || p.locals.size() != nodes_.size()) return;
    float out[4];
    for (const Channel& ch : clips_[clip].channels) {
        if (ch.node < 0 || (size_t)ch.node >= nodes_.size() || ch.times.empty()) continue;
        if (ch.path == 3) {
            std::vector<float>& w = p.morphWeights[ch.node];
            w.resize((size_t)ch.comp);
            sampleChannel(ch, t, w.data());
            continue;
        }
        p.useMatrix[ch.node] = 0; // animated nodes always pose via TRS
        sampleChannel(ch, t, out);
        ModelPose::NodeTRS& l = p.locals[ch.node];
        if (ch.path == 1) l.r = Vec4(out[0], out[1], out[2], out[3]);
        else if (ch.path == 0) l.t = Vec3(out[0], out[1], out[2]);
        else l.s = Vec3(out[0], out[1], out[2]);
    }
}

void Model::blendPose(ModelPose& dst, const ModelPose& src, float w,
                      const std::vector<float>* mask) {
    if (dst.locals.size() != src.locals.size()) return;
    for (size_t i = 0; i < dst.locals.size(); ++i) {
        float wi = mask && i < mask->size() ? w * (*mask)[i] : w;
        if (wi <= 0.0001f) continue;
        ModelPose::NodeTRS& a = dst.locals[i];
        const ModelPose::NodeTRS& b = src.locals[i];
        a.t = a.t + (b.t - a.t) * wi;
        a.s = a.s + (b.s - a.s) * wi;
        a.r = nlerpQuat(a.r, b.r, wi);
        if (!src.useMatrix[i]) dst.useMatrix[i] = 0;
    }
    for (const auto& [node, sw] : src.morphWeights) {
        float wi = mask && node >= 0 && (size_t)node < mask->size() ? w * (*mask)[node] : w;
        if (wi <= 0.0001f) continue;
        std::vector<float>& dw = dst.morphWeights[node];
        if (dw.size() < sw.size()) dw.resize(sw.size(), 0.0f);
        for (size_t i = 0; i < sw.size(); ++i) dw[i] += (sw[i] - dw[i]) * wi;
    }
}

void Model::finalizePose(ModelPose& p) const {
    if (p.locals.size() != nodes_.size()) return;

    // Worlds: depth-first from roots so parents are always resolved first.
    std::vector<Mat4> parentWorld(nodes_.size(), Mat4::identity());
    std::vector<int> stack(roots_.rbegin(), roots_.rend());
    while (!stack.empty()) {
        int ni = stack.back();
        stack.pop_back();
        const Node& n = nodes_[ni];
        const ModelPose::NodeTRS& l = p.locals[ni];
        Mat4 local = p.useMatrix[ni]
                         ? n.matrix
                         : translate(l.t) * quatToMat4(l.r.x, l.r.y, l.r.z, l.r.w) * scale(l.s);
        p.worlds[ni] = parentWorld[ni] * local;
        for (int c : n.children) {
            parentWorld[c] = p.worlds[ni];
            stack.push_back(c);
        }
    }
    for (size_t s = 0; s < skins_.size(); ++s)
        for (size_t j = 0; j < skins_[s].joints.size(); ++j)
            p.palettes[s][j] = p.worlds[skins_[s].joints[j]] * skins_[s].inverseBind[j];

    // Morph targets: CPU-blend base + weighted deltas into this instance's
    // dynamic mesh whenever the weights changed.
    for (const auto& [meshIdx, md] : morphs_) {
        // Find the node driving this mesh (first draw that references it).
        int node = -1;
        for (const Draw& d : draws_)
            if (d.mesh == meshIdx) { node = d.node; break; }
        std::vector<float> weights(md.targets.size(), 0.0f);
        auto wit = p.morphWeights.find(node);
        if (wit != p.morphWeights.end())
            for (size_t i = 0; i < weights.size() && i < wit->second.size(); ++i)
                weights[i] = wit->second[i];

        ModelPose::MorphMesh& mm = p.morphMeshes[meshIdx];
        if (mm.created && mm.lastWeights == weights) continue;

        MeshData data;
        data.vertices = md.base.vertices;
        bool touchNormals = false;
        for (size_t ti = 0; ti < md.targets.size(); ++ti) {
            float wt = weights[ti];
            if (std::fabs(wt) < 1e-4f) continue;
            const MorphData::Target& tg = md.targets[ti];
            for (size_t v = 0; v < data.vertices.size() && v < tg.dpos.size(); ++v)
                data.vertices[v].position = data.vertices[v].position + tg.dpos[v] * wt;
            if (!tg.dnrm.empty()) {
                touchNormals = true;
                for (size_t v = 0; v < data.vertices.size() && v < tg.dnrm.size(); ++v)
                    data.vertices[v].normal = data.vertices[v].normal + tg.dnrm[v] * wt;
            }
        }
        if (touchNormals)
            for (Vertex& v : data.vertices) v.normal = normalize(v.normal);

        if (!mm.created) {
            data.indices = md.base.indices;
            mm.mesh.upload(data, true);
            mm.created = true;
        } else {
            mm.mesh.updateVertices(data.vertices);
        }
        mm.lastWeights = std::move(weights);
    }
}

void Model::collectNavTriangles(const Mat4& base, std::vector<float>& verts,
                                std::vector<int>& tris) const {
    for (const Draw& d : draws_) {
        const Mesh& mesh = meshes_[d.mesh];
        const std::vector<Vec3>& pos = mesh.navPositions();
        const std::vector<uint32_t>& idx = mesh.navIndices();
        if (pos.empty() || idx.empty()) continue;
        // Skinned draws already carry the pose in their palette; for nav we use
        // the bind pose, so apply the node's model-space world (rigid draws) —
        // skinned meshes are baked at base (their verts are bind-pose local).
        Mat4 m = d.skin >= 0 ? base : base * nodes_[d.node].world;
        int vbase = (int)(verts.size() / 3);
        for (const Vec3& p : pos) {
            Vec4 w = m * Vec4(p, 1.0f);
            verts.push_back(w.x);
            verts.push_back(w.y);
            verts.push_back(w.z);
        }
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            tris.push_back(vbase + (int)idx[i]);
            tris.push_back(vbase + (int)idx[i + 1]);
            tris.push_back(vbase + (int)idx[i + 2]);
        }
    }
}

void Model::emit(RenderScene& out, const Mat4& base, const ModelPose* pose) const {
    bool usePose = pose && pose->locals.size() == nodes_.size();
    for (const Draw& d : draws_) {
        Renderable e;
        e.mesh = &meshes_[d.mesh];
        if (usePose) {
            auto mit = pose->morphMeshes.find(d.mesh);
            if (mit != pose->morphMeshes.end() && mit->second.created)
                e.mesh = &mit->second.mesh;
        }
        e.boundsMin = meshes_[d.mesh].boundsMin();
        e.boundsMax = meshes_[d.mesh].boundsMax();
        if (d.material >= 0 && (size_t)d.material < materials_.size())
            e.material = materials_[d.material];
        if (d.skin >= 0 && (size_t)d.skin < skins_.size()) {
            // Skinned: the palette carries the pose in model space, so the
            // per-vertex skinning matrix already includes the node transform.
            e.transform = base;
            e.jointMatrices = usePose ? &pose->palettes[d.skin] : &skins_[d.skin].palette;
        } else {
            e.transform = base * (usePose ? pose->worlds[d.node] : nodes_[d.node].world);
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
    morphs_.clear();
    morphDefaults_.clear();
}

} // namespace ae
