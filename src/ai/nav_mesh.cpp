#include "nav_mesh.h"
#include "../engine/world.h"
#include "../engine/engine_modules.h"
#include "../engine/entity.h"
#include "../physics/physics_components.h"
#include "../core/log.h"
#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <cmath>
#include <cstring>

namespace ae {

// ---------------------------------------------------------------------------
// input triangles from static colliders
// ---------------------------------------------------------------------------

static void pushTri(std::vector<float>& verts, std::vector<int>& tris, const Vec3& a,
                    const Vec3& b, const Vec3& c) {
    int base = (int)(verts.size() / 3);
    for (const Vec3* p : {&a, &b, &c}) {
        verts.push_back(p->x);
        verts.push_back(p->y);
        verts.push_back(p->z);
    }
    tris.push_back(base);
    tris.push_back(base + 1);
    tris.push_back(base + 2);
}

// Box: 8 corners in collider-local space (center + signed half extents),
// world-transformed, wound outward (top face normal +Y).
static void emitBox(std::vector<float>& verts, std::vector<int>& tris, const Mat4& world,
                    const Vec3& center, const Vec3& he) {
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        Vec3 local = center + Vec3(i & 1 ? he.x : -he.x, i & 2 ? he.y : -he.y,
                                   i & 4 ? he.z : -he.z);
        Vec4 w = world * Vec4(local, 1.0f);
        c[i] = Vec3(w.x, w.y, w.z);
    }
    static const int faces[6][4] = {
        {2, 6, 7, 3}, // +Y
        {0, 1, 5, 4}, // -Y
        {0, 4, 6, 2}, // -X
        {1, 3, 7, 5}, // +X
        {0, 2, 3, 1}, // -Z
        {4, 5, 7, 6}, // +Z
    };
    for (const int* f : faces) {
        pushTri(verts, tris, c[f[0]], c[f[1]], c[f[2]]);
        pushTri(verts, tris, c[f[0]], c[f[2]], c[f[3]]);
    }
}

// Sphere/capsule obstacles approximated by a closed 12-sided cylinder — for
// navmesh carving the silhouette is what matters, not the exact cap shape.
static void emitCylinder(std::vector<float>& verts, std::vector<int>& tris, const Mat4& world,
                         const Vec3& center, float radius, float totalHeight) {
    constexpr int N = 12;
    float hh = totalHeight * 0.5f;
    Vec3 top[N], bot[N];
    for (int i = 0; i < N; ++i) {
        float a = (float)i / N * 6.2831853f;
        Vec3 rim(std::cos(a) * radius, 0, std::sin(a) * radius);
        Vec4 t = world * Vec4(center + rim + Vec3(0, hh, 0), 1.0f);
        Vec4 b = world * Vec4(center + rim - Vec3(0, hh, 0), 1.0f);
        top[i] = Vec3(t.x, t.y, t.z);
        bot[i] = Vec3(b.x, b.y, b.z);
    }
    for (int i = 0; i < N; ++i) {
        int j = (i + 1) % N;
        // side (outward: rim order is CW from above, so i->j faces out)
        pushTri(verts, tris, bot[i], top[i], top[j]);
        pushTri(verts, tris, bot[i], top[j], bot[j]);
        // caps
        if (i >= 2) {
            pushTri(verts, tris, top[0], top[i - 1], top[i]);   // +Y fan
            pushTri(verts, tris, bot[0], bot[i], bot[i - 1]);   // -Y fan
        }
    }
}

void gatherNavTriangles(const World& world, std::vector<float>& verts, std::vector<int>& tris) {
    for (const auto& up : world.entities()) {
        Entity* e = up.get();
        if (!e || !e->active()) continue;
        auto* col = e->getComponent<ColliderComponent>();
        if (!col || col->isTrigger) continue;
        auto* rb = e->getComponent<RigidBodyComponent>();
        if (rb && rb->type() != BodyType::Static) continue; // dynamic props move

        Mat4 m = e->worldMatrix();
        switch (col->kind()) {
        case ColliderShape::Box:
            emitBox(verts, tris, m, col->center, col->halfExtents);
            break;
        case ColliderShape::Sphere:
            emitCylinder(verts, tris, m, col->center, col->radius, col->radius * 2.0f);
            break;
        case ColliderShape::Capsule:
            emitCylinder(verts, tris, m, col->center, col->radius, col->height);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Recast bake
// ---------------------------------------------------------------------------

struct NavMesh::Impl {
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    std::vector<Vec3> debugLines; // cached poly edges from the bake
    int polys = 0;

    void clear() {
        if (query) dtFreeNavMeshQuery(query);
        if (mesh) dtFreeNavMesh(mesh);
        query = nullptr;
        mesh = nullptr;
        debugLines.clear();
        polys = 0;
    }
    ~Impl() { clear(); }
};

NavMesh::NavMesh() : impl_(new Impl) {}
NavMesh::~NavMesh() = default;

void NavMesh::clear() { impl_->clear(); }
bool NavMesh::valid() const { return impl_->mesh != nullptr; }
int NavMesh::polyCount() const { return impl_->polys; }

bool NavMesh::bake(const World& world, const NavBuildSettings& s) {
    impl_->clear();
    if (!engineModules().enabled("ai")) {
        AE_WARN("[Nav] AI module is disabled for this project");
        return false;
    }

    std::vector<float> verts;
    std::vector<int> tris;
    gatherNavTriangles(world, verts, tris);
    int nverts = (int)verts.size() / 3;
    int ntris = (int)tris.size() / 3;
    if (!ntris) {
        AE_WARN("[Nav] bake: no static collider geometry in the scene");
        return false;
    }

    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.agentMaxSlope;
    cfg.walkableHeight = (int)std::ceil(s.agentHeight / cfg.ch);
    cfg.walkableClimb = (int)std::floor(s.agentMaxClimb / cfg.ch);
    cfg.walkableRadius = (int)std::ceil(s.agentRadius / cfg.cs);
    cfg.maxEdgeLen = (int)(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch * 1.0f;
    rcCalcBounds(verts.data(), nverts, cfg.bmin, cfg.bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    rcContext ctx(false);

    rcHeightfield* hf = rcAllocHeightfield();
    if (!rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs,
                             cfg.ch)) {
        rcFreeHeightField(hf);
        return false;
    }
    std::vector<unsigned char> areas(ntris, 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(),
                            ntris, areas.data());
    rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *hf,
                         cfg.walkableClimb);
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    bool ok = rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf);
    rcFreeHeightField(hf);
    ok = ok && rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);
    ok = ok && rcBuildDistanceField(&ctx, *chf);
    ok = ok && rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea);

    rcContourSet* cset = rcAllocContourSet();
    ok = ok && rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset);

    rcPolyMesh* pmesh = rcAllocPolyMesh();
    ok = ok && rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh);

    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    ok = ok && rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist,
                                     cfg.detailSampleMaxError, *dmesh);
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);

    if (!ok || !pmesh->npolys) {
        AE_WARN("[Nav] bake produced no walkable polygons");
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        return false;
    }

    // One flag bit = "walkable"; the default dtQueryFilter accepts any set bit.
    for (int i = 0; i < pmesh->npolys; ++i)
        if (pmesh->areas[i] == RC_WALKABLE_AREA) pmesh->flags[i] = 1;

    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = s.agentHeight;
    params.walkableRadius = s.agentRadius;
    params.walkableClimb = s.agentMaxClimb;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) {
        AE_ERROR("[Nav] dtCreateNavMeshData failed");
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        return false;
    }

    impl_->mesh = dtAllocNavMesh();
    if (dtStatusFailed(impl_->mesh->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
        AE_ERROR("[Nav] navmesh init failed");
        dtFree(navData);
        impl_->clear();
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        return false;
    }
    impl_->query = dtAllocNavMeshQuery();
    impl_->query->init(impl_->mesh, 2048);
    impl_->polys = pmesh->npolys;

    // Cache poly edges for the debug overlay (slightly lifted to stay visible).
    const float lift = 0.06f;
    for (int p = 0; p < pmesh->npolys; ++p) {
        const unsigned short* poly = &pmesh->polys[p * 2 * pmesh->nvp];
        int n = 0;
        while (n < pmesh->nvp && poly[n] != RC_MESH_NULL_IDX) ++n;
        for (int i = 0; i < n; ++i) {
            const unsigned short* va = &pmesh->verts[poly[i] * 3];
            const unsigned short* vb = &pmesh->verts[poly[(i + 1) % n] * 3];
            impl_->debugLines.push_back(Vec3(pmesh->bmin[0] + va[0] * cfg.cs,
                                             pmesh->bmin[1] + va[1] * cfg.ch + lift,
                                             pmesh->bmin[2] + va[2] * cfg.cs));
            impl_->debugLines.push_back(Vec3(pmesh->bmin[0] + vb[0] * cfg.cs,
                                             pmesh->bmin[1] + vb[1] * cfg.ch + lift,
                                             pmesh->bmin[2] + vb[2] * cfg.cs));
        }
    }

    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);
    AE_LOG("[Nav] baked navmesh: %d polys from %d input tris, grid %dx%d", impl_->polys,
           ntris, cfg.width, cfg.height);
    return true;
}

// ---------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------

bool NavMesh::findPath(const Vec3& start, const Vec3& end, std::vector<Vec3>& out,
                       float snapRadius) const {
    out.clear();
    if (!impl_->query) return false;

    dtQueryFilter filter;
    const float ext[3] = {snapRadius, snapRadius * 2.0f, snapRadius};
    dtPolyRef sref = 0, eref = 0;
    float spos[3], epos[3];
    impl_->query->findNearestPoly(&start.x, ext, &filter, &sref, spos);
    impl_->query->findNearestPoly(&end.x, ext, &filter, &eref, epos);
    if (!sref || !eref) return false;

    constexpr int kMaxPolys = 256;
    dtPolyRef polys[kMaxPolys];
    int npolys = 0;
    impl_->query->findPath(sref, eref, spos, epos, &filter, polys, &npolys, kMaxPolys);
    if (!npolys) return false;

    // Partial path (end unreachable): steer to the closest point we can reach.
    float target[3];
    rcVcopy(target, epos);
    if (polys[npolys - 1] != eref)
        impl_->query->closestPointOnPoly(polys[npolys - 1], epos, target, nullptr);

    float straight[kMaxPolys * 3];
    unsigned char flags[kMaxPolys];
    dtPolyRef sp[kMaxPolys];
    int nstraight = 0;
    impl_->query->findStraightPath(spos, target, polys, npolys, straight, flags, sp,
                                   &nstraight, kMaxPolys, 0);
    for (int i = 0; i < nstraight; ++i)
        out.push_back(Vec3(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]));
    return !out.empty();
}

Vec3 NavMesh::nearestPoint(const Vec3& p, float snapRadius) const {
    if (!impl_->query) return p;
    dtQueryFilter filter;
    const float ext[3] = {snapRadius, snapRadius * 2.0f, snapRadius};
    dtPolyRef ref = 0;
    float pos[3];
    impl_->query->findNearestPoly(&p.x, ext, &filter, &ref, pos);
    return ref ? Vec3(pos[0], pos[1], pos[2]) : p;
}

void NavMesh::debugLines(std::vector<Vec3>& out) const {
    out.insert(out.end(), impl_->debugLines.begin(), impl_->debugLines.end());
}

} // namespace ae
