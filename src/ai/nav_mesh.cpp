#include "nav_mesh.h"
#include "../engine/world.h"
#include "../engine/engine_modules.h"
#include "../engine/entity.h"
#include "../engine/components.h"
#include "../physics/physics_components.h"
#include "../render/gltf.h"
#include "../core/log.h"
#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourCrowd.h>
#include <DetourTileCache.h>
#include <DetourTileCacheBuilder.h>
#include <algorithm>
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

        // Static collider geometry (the default source).
        if (auto* col = e->getComponent<ColliderComponent>()) {
            auto* rb = e->getComponent<RigidBodyComponent>();
            bool dynamicBody = rb && rb->type() != BodyType::Static;
            if (!col->isTrigger && !dynamicBody) {
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

        // Opt-in render geometry: MeshRenderers / Models flagged navStatic feed
        // their actual triangles in (floors and props without colliders).
        if (auto* mr = e->getComponent<MeshRenderer>()) {
            if (mr->navStatic && mr->mesh && !mr->mesh->navPositions().empty()) {
                Mat4 m = e->worldMatrix();
                const std::vector<Vec3>& pos = mr->mesh->navPositions();
                const std::vector<uint32_t>& idx = mr->mesh->navIndices();
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
        if (auto* mc = e->getComponent<ModelComponent>()) {
            if (mc->navStatic && mc->model)
                mc->model->collectNavTriangles(e->worldMatrix(), verts, tris);
        }
    }
}

// ---------------------------------------------------------------------------
// tile-cache infrastructure (allocator, passthrough compressor, mesh process)
// ---------------------------------------------------------------------------
namespace {

constexpr int kMaxLayers = 32;          // navmesh layers per tile
constexpr int kExpectedLayers = 4;      // sizing hint for maxTiles

// Bump allocator reused for every tile build (no per-tile churn).
struct LinearAlloc : dtTileCacheAlloc {
    std::vector<unsigned char> buf;
    size_t top = 0;
    explicit LinearAlloc(size_t cap) { buf.resize(cap); }
    void reset() override { top = 0; }
    void* alloc(size_t size) override {
        if (top + size > buf.size()) return nullptr; // grow guard (caps tile size)
        void* p = &buf[top];
        top += size;
        return p;
    }
    void free(void*) override {}
};

// The tile cache expects a compressor; we store layers raw (memcpy). Simpler
// and plenty fast for the tile sizes we use — no fastlz dependency.
struct RawCompressor : dtTileCacheCompressor {
    int maxCompressedSize(int bufferSize) override { return bufferSize; }
    dtStatus compress(const unsigned char* buffer, int bufferSize, unsigned char* out,
                      int /*maxOut*/, int* outSize) override {
        std::memcpy(out, buffer, bufferSize);
        *outSize = bufferSize;
        return DT_SUCCESS;
    }
    dtStatus decompress(const unsigned char* comp, int compSize, unsigned char* out,
                        int maxOut, int* outSize) override {
        int n = compSize < maxOut ? compSize : maxOut;
        std::memcpy(out, comp, n);
        *outSize = n;
        return DT_SUCCESS;
    }
};

// Flags each rebuilt tile's polys walkable (bit 0), matching the default filter.
struct WalkableProcess : dtTileCacheMeshProcess {
    void process(dtNavMeshCreateParams* params, unsigned char* areas,
                 unsigned short* flags) override {
        for (int i = 0; i < params->polyCount; ++i) {
            if (areas[i] == DT_TILECACHE_WALKABLE_AREA) areas[i] = 1;
            flags[i] = areas[i] ? 1 : 0;
        }
    }
};

int ilog2(unsigned v) {
    int r = 0;
    while (v >>= 1) ++r;
    return r;
}
unsigned nextPow2(unsigned v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

} // namespace

// ---------------------------------------------------------------------------
// NavMesh::Impl
// ---------------------------------------------------------------------------
struct NavMesh::Impl {
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtTileCache* tileCache = nullptr;
    dtCrowd* crowd = nullptr;
    LinearAlloc alloc{64 * 1024};
    RawCompressor comp;
    WalkableProcess proc;
    NavBuildSettings settings;
    int polys = 0;

    void clear() {
        if (crowd) dtFreeCrowd(crowd);
        if (query) dtFreeNavMeshQuery(query);
        if (tileCache) dtFreeTileCache(tileCache);
        if (mesh) dtFreeNavMesh(mesh);
        crowd = nullptr;
        query = nullptr;
        tileCache = nullptr;
        mesh = nullptr;
        polys = 0;
    }
    ~Impl() { clear(); }
};

NavMesh::NavMesh() : impl_(new Impl) {}
NavMesh::~NavMesh() = default;

void NavMesh::clear() { impl_->clear(); }
bool NavMesh::valid() const { return impl_->mesh != nullptr; }
int NavMesh::polyCount() const { return impl_->polys; }

// Rasterizes one tile's triangles into cache layers (adapted from the Recast
// TempObstacles sample, without the chunky-mesh acceleration — for our scene
// sizes a full-scan per tile is fine).
namespace {
struct TileLayer {
    unsigned char* data = nullptr;
    int size = 0;
};

int rasterizeTile(rcContext& ctx, int tx, int ty, const rcConfig& cfg, const float* verts,
                  int nverts, const int* tris, int ntris, dtTileCacheCompressor& comp,
                  TileLayer* out, int maxOut) {
    const float tcs = cfg.tileSize * cfg.cs;
    rcConfig tcfg = cfg;
    tcfg.bmin[0] = cfg.bmin[0] + tx * tcs - tcfg.borderSize * tcfg.cs;
    tcfg.bmin[2] = cfg.bmin[2] + ty * tcs - tcfg.borderSize * tcfg.cs;
    tcfg.bmax[0] = cfg.bmin[0] + (tx + 1) * tcs + tcfg.borderSize * tcfg.cs;
    tcfg.bmax[2] = cfg.bmin[2] + (ty + 1) * tcs + tcfg.borderSize * tcfg.cs;

    rcHeightfield* solid = rcAllocHeightfield();
    if (!rcCreateHeightfield(&ctx, *solid, tcfg.width, tcfg.height, tcfg.bmin, tcfg.bmax, tcfg.cs,
                             tcfg.ch)) {
        rcFreeHeightField(solid);
        return 0;
    }
    std::vector<unsigned char> areas(ntris, 0);
    rcMarkWalkableTriangles(&ctx, tcfg.walkableSlopeAngle, verts, nverts, tris, ntris,
                            areas.data());
    rcRasterizeTriangles(&ctx, verts, nverts, tris, areas.data(), ntris, *solid,
                         tcfg.walkableClimb);
    rcFilterLowHangingWalkableObstacles(&ctx, tcfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, tcfg.walkableHeight, tcfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, tcfg.walkableHeight, *solid);

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    bool ok = rcBuildCompactHeightfield(&ctx, tcfg.walkableHeight, tcfg.walkableClimb, *solid,
                                        *chf);
    rcFreeHeightField(solid);
    ok = ok && rcErodeWalkableArea(&ctx, tcfg.walkableRadius, *chf);

    rcHeightfieldLayerSet* lset = rcAllocHeightfieldLayerSet();
    ok = ok && rcBuildHeightfieldLayers(&ctx, *chf, tcfg.borderSize, tcfg.walkableHeight, *lset);
    rcFreeCompactHeightfield(chf);
    if (!ok) {
        rcFreeHeightfieldLayerSet(lset);
        return 0;
    }

    int n = 0;
    for (int i = 0; i < lset->nlayers && n < maxOut; ++i) {
        const rcHeightfieldLayer* layer = &lset->layers[i];
        dtTileCacheLayerHeader header{};
        header.magic = DT_TILECACHE_MAGIC;
        header.version = DT_TILECACHE_VERSION;
        header.tx = tx;
        header.ty = ty;
        header.tlayer = i;
        rcVcopy(header.bmin, layer->bmin);
        rcVcopy(header.bmax, layer->bmax);
        header.width = (unsigned char)layer->width;
        header.height = (unsigned char)layer->height;
        header.minx = (unsigned char)layer->minx;
        header.maxx = (unsigned char)layer->maxx;
        header.miny = (unsigned char)layer->miny;
        header.maxy = (unsigned char)layer->maxy;
        header.hmin = (unsigned short)layer->hmin;
        header.hmax = (unsigned short)layer->hmax;
        if (dtStatusSucceed(dtBuildTileCacheLayer(&comp, &header, layer->heights, layer->areas,
                                                  layer->cons, &out[n].data, &out[n].size)))
            ++n;
    }
    rcFreeHeightfieldLayerSet(lset);
    return n;
}
} // namespace

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
        AE_WARN("[Nav] bake: no static collider or navStatic mesh geometry in the scene");
        return false;
    }
    impl_->settings = s;

    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.agentMaxSlope;
    cfg.walkableHeight = (int)std::ceil(s.agentHeight / cfg.ch);
    cfg.walkableClimb = (int)std::floor(s.agentMaxClimb / cfg.ch);
    cfg.walkableRadius = (int)std::ceil(s.agentRadius / cfg.cs);
    cfg.maxEdgeLen = (int)(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.maxVertsPerPoly = 6;
    cfg.tileSize = s.tileSize;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    rcCalcBounds(verts.data(), nverts, cfg.bmin, cfg.bmax);
    int gw = 0, gh = 0;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &gw, &gh);
    const int tw = (gw + cfg.tileSize - 1) / cfg.tileSize;
    const int th = (gh + cfg.tileSize - 1) / cfg.tileSize;

    // Tile cache.
    dtTileCacheParams tcparams{};
    rcVcopy(tcparams.orig, cfg.bmin);
    tcparams.cs = cfg.cs;
    tcparams.ch = cfg.ch;
    tcparams.width = cfg.tileSize;
    tcparams.height = cfg.tileSize;
    tcparams.walkableHeight = s.agentHeight;
    tcparams.walkableRadius = s.agentRadius;
    tcparams.walkableClimb = s.agentMaxClimb;
    tcparams.maxSimplificationError = cfg.maxSimplificationError;
    tcparams.maxTiles = tw * th * kExpectedLayers;
    tcparams.maxObstacles = 256;

    impl_->tileCache = dtAllocTileCache();
    if (dtStatusFailed(impl_->tileCache->init(&tcparams, &impl_->alloc, &impl_->comp,
                                              &impl_->proc))) {
        AE_ERROR("[Nav] tile cache init failed");
        impl_->clear();
        return false;
    }

    // Navmesh (multi-tile).
    dtNavMeshParams nmparams{};
    rcVcopy(nmparams.orig, cfg.bmin);
    nmparams.tileWidth = cfg.tileSize * cfg.cs;
    nmparams.tileHeight = cfg.tileSize * cfg.cs;
    int tileBits = std::min(ilog2(nextPow2((unsigned)(tw * th * kExpectedLayers))), 14);
    int polyBits = 22 - tileBits;
    nmparams.maxTiles = 1 << tileBits;
    nmparams.maxPolys = 1 << polyBits;

    impl_->mesh = dtAllocNavMesh();
    if (dtStatusFailed(impl_->mesh->init(&nmparams))) {
        AE_ERROR("[Nav] navmesh init failed");
        impl_->clear();
        return false;
    }

    // Rasterize + add every tile's layers to the cache.
    rcContext ctx(false);
    int addedLayers = 0;
    for (int ty = 0; ty < th; ++ty) {
        for (int tx = 0; tx < tw; ++tx) {
            TileLayer layers[kMaxLayers];
            int n = rasterizeTile(ctx, tx, ty, cfg, verts.data(), nverts, tris.data(), ntris,
                                  impl_->comp, layers, kMaxLayers);
            for (int i = 0; i < n; ++i) {
                dtCompressedTileRef ref = 0;
                if (dtStatusFailed(impl_->tileCache->addTile(layers[i].data, layers[i].size,
                                                             DT_COMPRESSEDTILE_FREE_DATA, &ref)))
                    dtFree(layers[i].data);
                else
                    ++addedLayers;
            }
        }
    }
    for (int ty = 0; ty < th; ++ty)
        for (int tx = 0; tx < tw; ++tx)
            impl_->tileCache->buildNavMeshTilesAt(tx, ty, impl_->mesh);

    // Count polys across all navmesh tiles.
    impl_->polys = 0;
    const dtNavMesh* cmesh = impl_->mesh;
    for (int i = 0; i < cmesh->getMaxTiles(); ++i) {
        const dtMeshTile* t = cmesh->getTile(i);
        if (t && t->header) impl_->polys += t->header->polyCount;
    }
    if (!impl_->polys) {
        AE_WARN("[Nav] bake produced no walkable polygons");
        impl_->clear();
        return false;
    }

    impl_->query = dtAllocNavMeshQuery();
    impl_->query->init(impl_->mesh, 2048);

    // Crowd for local avoidance (max radius generous; agents set their own).
    impl_->crowd = dtAllocCrowd();
    impl_->crowd->init(256, 2.0f, impl_->mesh);

    ++generation_;
    AE_LOG("[Nav] baked navmesh: %d polys, %d tile layers, grid %dx%d (%d x %d tiles) from %d tris",
           impl_->polys, addedLayers, gw, gh, tw, th, ntris);
    return true;
}

// ---------------------------------------------------------------------------
// dynamic obstacles + per-frame update
// ---------------------------------------------------------------------------

NavObstacle NavMesh::addObstacle(const Vec3& pos, float radius, float height) {
    if (!impl_->tileCache) return 0;
    dtObstacleRef ref = 0;
    if (dtStatusFailed(impl_->tileCache->addObstacle(&pos.x, radius, height, &ref))) return 0;
    return (NavObstacle)ref;
}

void NavMesh::removeObstacle(NavObstacle id) {
    if (impl_->tileCache && id) impl_->tileCache->removeObstacle((dtObstacleRef)id);
}

void NavMesh::update(float dt, const World&) {
    if (impl_->tileCache && impl_->mesh) {
        bool upToDate = false;
        // A few iterations drain the request queue and rebuild dirty tiles.
        for (int i = 0; i < 4 && !upToDate; ++i)
            impl_->tileCache->update(dt, impl_->mesh, &upToDate);
    }
    if (impl_->crowd) impl_->crowd->update(dt, nullptr);
}

// ---------------------------------------------------------------------------
// crowd
// ---------------------------------------------------------------------------

int NavMesh::addAgent(const Vec3& pos, float radius, float height, float maxSpeed) {
    if (!impl_->crowd) return -1;
    dtCrowdAgentParams ap{};
    ap.radius = radius;
    ap.height = height;
    ap.maxAcceleration = maxSpeed * 4.0f;
    ap.maxSpeed = maxSpeed;
    ap.collisionQueryRange = radius * 12.0f;
    ap.pathOptimizationRange = radius * 30.0f;
    ap.separationWeight = 2.0f;
    ap.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OBSTACLE_AVOIDANCE |
                     DT_CROWD_SEPARATION;
    ap.obstacleAvoidanceType = 3;
    return impl_->crowd->addAgent(&pos.x, &ap);
}

void NavMesh::removeAgent(int idx) {
    if (impl_->crowd && idx >= 0) impl_->crowd->removeAgent(idx);
}

void NavMesh::setAgentTarget(int idx, const Vec3& target) {
    if (!impl_->crowd || idx < 0 || !impl_->query) return;
    dtQueryFilter filter;
    const float ext[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef ref = 0;
    float pos[3];
    impl_->query->findNearestPoly(&target.x, ext, &filter, &ref, pos);
    if (ref) impl_->crowd->requestMoveTarget(idx, ref, pos);
}

void NavMesh::resetAgentTarget(int idx) {
    if (impl_->crowd && idx >= 0) impl_->crowd->resetMoveTarget(idx);
}

void NavMesh::setAgentParams(int idx, float maxSpeed, float radius) {
    if (!impl_->crowd || idx < 0) return;
    const dtCrowdAgent* a = impl_->crowd->getAgent(idx);
    if (!a || !a->active) return;
    dtCrowdAgentParams ap = a->params;
    ap.maxSpeed = maxSpeed;
    ap.maxAcceleration = maxSpeed * 4.0f;
    ap.radius = radius;
    impl_->crowd->updateAgentParameters(idx, &ap);
}

bool NavMesh::agentValid(int idx) const {
    if (!impl_->crowd || idx < 0) return false;
    const dtCrowdAgent* a = impl_->crowd->getAgent(idx);
    return a && a->active;
}

Vec3 NavMesh::agentPosition(int idx) const {
    if (!impl_->crowd || idx < 0) return Vec3(0, 0, 0);
    const dtCrowdAgent* a = impl_->crowd->getAgent(idx);
    return a && a->active ? Vec3(a->npos[0], a->npos[1], a->npos[2]) : Vec3(0, 0, 0);
}

Vec3 NavMesh::agentVelocity(int idx) const {
    if (!impl_->crowd || idx < 0) return Vec3(0, 0, 0);
    const dtCrowdAgent* a = impl_->crowd->getAgent(idx);
    return a && a->active ? Vec3(a->vel[0], a->vel[1], a->vel[2]) : Vec3(0, 0, 0);
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
    // Walk every navmesh tile's polygons and emit their edges. Recomputed on
    // demand so runtime obstacle carving shows up live in the overlay.
    const dtNavMesh* mesh = impl_->mesh;
    if (!mesh) return;
    const float lift = 0.06f;
    for (int t = 0; t < mesh->getMaxTiles(); ++t) {
        const dtMeshTile* tile = mesh->getTile(t);
        if (!tile || !tile->header) continue;
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly* poly = &tile->polys[i];
            if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            for (int j = 0; j < poly->vertCount; ++j) {
                const float* va = &tile->verts[poly->verts[j] * 3];
                const float* vb = &tile->verts[poly->verts[(j + 1) % poly->vertCount] * 3];
                out.push_back(Vec3(va[0], va[1] + lift, va[2]));
                out.push_back(Vec3(vb[0], vb[1] + lift, vb[2]));
            }
        }
    }
}

} // namespace ae
