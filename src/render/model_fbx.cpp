// Aether Engine — FBX importer via vendored ufbx (third_party/ufbx, MIT).
// Static geometry v1: meshes with per-material parts, node placements baked
// into draw transforms, PBR base color/roughness/metalness (+ base-color
// texture). Skins/animation from FBX are a later pass — rigged characters
// should ship as glTF for now.
#include "model_import.h"
#include "../../third_party/ufbx/ufbx.h"
#include <cstdio>
#include <vector>

namespace ae {

static Mat4 toMat4(const ufbx_matrix& m) {
    Mat4 r = Mat4::identity();
    // ufbx_matrix: 4 columns of 3 rows (affine); ours is column-major 4x4.
    for (int c = 0; c < 4; ++c) {
        r.m[c][0] = (float)m.cols[c].x;
        r.m[c][1] = (float)m.cols[c].y;
        r.m[c][2] = (float)m.cols[c].z;
    }
    return r;
}

bool Model::loadFbx(const char* path) {
    ufbx_load_opts opts = {};
    opts.generate_missing_normals = true;
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path, &opts, &error);
    if (!scene) {
        AE_ERROR("[FBX] %s: %s", path, error.description.data);
        return false;
    }

    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    std::string baseDir = slash == std::string::npos ? "" : p.substr(0, slash + 1);

    ModelBuild build(*this);
    std::vector<uint32_t> tri;

    for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
        ufbx_mesh* mesh = scene->meshes.data[mi];
        tri.resize(mesh->max_face_triangles * 3);

        for (size_t ii = 0; ii < mesh->instances.count; ++ii) {
            ufbx_node* node = mesh->instances.data[ii];
            Mat4 world = toMat4(node->geometry_to_world);

            for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
                const ufbx_mesh_part& part = mesh->material_parts.data[pi];
                if (part.num_triangles == 0) continue;

                // Material (ufbx maps classic FBX shading into a PBR view).
                Material mat;
                if (pi < mesh->materials.count && mesh->materials.data[pi]) {
                    ufbx_material* um = mesh->materials.data[pi];
                    ufbx_vec4 bc = um->pbr.base_color.value_vec4;
                    if (um->pbr.base_color.has_value)
                        mat.baseColor = Vec4((float)bc.x, (float)bc.y, (float)bc.z,
                                             (float)(bc.w > 0 ? bc.w : 1.0));
                    if (um->pbr.roughness.has_value)
                        mat.roughness = clampf((float)um->pbr.roughness.value_real, 0.04f, 1.0f);
                    if (um->pbr.metalness.has_value)
                        mat.metallic = clampf((float)um->pbr.metalness.value_real, 0.0f, 1.0f);
                    if (um->pbr.emission_color.has_value && um->pbr.emission_factor.has_value) {
                        ufbx_vec4 em = um->pbr.emission_color.value_vec4;
                        float ef = (float)um->pbr.emission_factor.value_real;
                        mat.emissive = Vec3((float)em.x, (float)em.y, (float)em.z) * ef;
                    }
                    if (um->pbr.base_color.texture) {
                        ufbx_texture* tex = um->pbr.base_color.texture;
                        std::string tp(tex->filename.data, tex->filename.length);
                        if (tp.empty() && tex->relative_filename.length)
                            tp = baseDir + std::string(tex->relative_filename.data,
                                                       tex->relative_filename.length);
                        mat.albedoTex = build.loadTexture(tp, /*srgb=*/true);
                    }
                    if (mat.baseColor.w < 0.999f) mat.blend = true;
                }
                int matIdx = build.addMaterial(mat);

                // Triangulate every face of this material part.
                MeshData data;
                for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                    ufbx_face face = mesh->faces.data[part.face_indices.data[fi]];
                    uint32_t numTris =
                        ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
                    for (uint32_t t = 0; t < numTris * 3; ++t) {
                        uint32_t ix = tri[t];
                        Vertex v;
                        ufbx_vec3 pos = ufbx_get_vertex_vec3(&mesh->vertex_position, ix);
                        v.position = Vec3((float)pos.x, (float)pos.y, (float)pos.z);
                        if (mesh->vertex_normal.exists) {
                            ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, ix);
                            v.normal = Vec3((float)n.x, (float)n.y, (float)n.z);
                        }
                        if (mesh->vertex_uv.exists) {
                            ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
                            v.uv = Vec2((float)uv.x, 1.0f - (float)uv.y);
                        }
                        data.indices.push_back((uint32_t)data.vertices.size());
                        data.vertices.push_back(v);
                    }
                }
                build.addDraw(data, matIdx, world);
            }
        }
    }

    bool ok = build.commit();
    if (ok)
        AE_LOG("[FBX] loaded %s (%d draws, %d materials)", path, (int)draws_.size(),
               (int)materials_.size());
    else
        AE_ERROR("[FBX] no geometry in %s", path);
    ufbx_free_scene(scene);
    return ok;
}

} // namespace ae
