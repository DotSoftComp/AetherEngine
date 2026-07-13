// Aether Engine — particle emitter simulation (see particles.h).
#include "particles.h"
#include "assets.h"
#include "world.h"
#include <cmath>

namespace ae {

float ParticlesComponent::frand() {
    rng_ = rng_ * 1664525u + 1013904223u; // LCG: fast, deterministic per emitter
    return (rng_ >> 8) * (1.0f / 16777216.0f);
}

void ParticlesComponent::onDeserialized(AssetLibrary& assets) {
    texId_ = texturePath.empty() ? 0 : assets.uiImage(texturePath);
}

void ParticlesComponent::onUpdate(float dt) {
    if (dt <= 0.0f) return;

    // ---- advance ----
    for (auto& p : pool_) {
        if (p.life <= 0.0f) continue;
        p.vel.y += gravity * dt;
        float damp = clampf(drag * dt, 0.0f, 1.0f);
        p.vel = p.vel * (1.0f - damp);
        p.pos += p.vel * dt;
        p.rot += p.spin * dt;
        p.life -= dt;
    }

    // ---- spawn ----
    spawnAcc_ += rate * dt;
    int toSpawn = (int)spawnAcc_;
    spawnAcc_ -= (float)toSpawn;
    if (toSpawn <= 0) return;

    Mat4 wm = entity().worldMatrix();
    Vec3 origin = entity().worldPosition();
    Vec3 axis = normalize(Vec3(wm.m[1][0], wm.m[1][1], wm.m[1][2])); // local +Y

    for (int s = 0; s < toSpawn; ++s) {
        // Reuse a dead slot; grow up to the cap.
        Particle* slot = nullptr;
        for (auto& p : pool_)
            if (p.life <= 0.0f) { slot = &p; break; }
        if (!slot) {
            if ((int)pool_.size() >= maxParticles) break;
            pool_.emplace_back();
            slot = &pool_.back();
        }

        // Random direction in a cone around the emission axis: perturb the axis
        // by two orthogonal tangents scaled by a random radius within spread.
        Vec3 t1 = normalize(cross(axis, std::fabs(axis.y) < 0.95f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
        Vec3 t2 = cross(axis, t1);
        float maxTan = std::tan(radians(clampf(spreadDeg, 0.0f, 89.0f)));
        float ang = frand() * 2.0f * PI;
        float rad = std::sqrt(frand()) * maxTan;
        Vec3 dir = normalize(axis + (t1 * std::cos(ang) + t2 * std::sin(ang)) * rad);

        float spd = speed * (1.0f + (frand() * 2.0f - 1.0f) * speedJitter);
        slot->maxLife = lifetime * (1.0f + (frand() * 2.0f - 1.0f) * lifetimeJitter);
        if (slot->maxLife < 0.05f) slot->maxLife = 0.05f;
        slot->life = slot->maxLife;
        slot->speedScale = 1.0f;
        slot->vel = dir * spd;
        slot->pos = worldSpace ? origin : Vec3(0, 0, 0);
        slot->rot = randomRotation ? frand() * 2.0f * PI : 0.0f;
        slot->spin = radians(spinDeg) * (1.0f + (frand() * 2.0f - 1.0f) * spinJitter);
    }
}

void ParticlesComponent::contribute(RenderScene& out) {
    // Gather live particles with their current (lerped) size/color.
    ParticleBatch batch;
    batch.additive = additive;
    batch.texture = texId_;
    batch.flipCols = flipbookCols > 1 ? flipbookCols : 1;
    batch.flipRows = flipbookRows > 1 ? flipbookRows : 1;
    batch.softFade = softFade;
    int frames = batch.flipCols * batch.flipRows;
    Mat4 wm = entity().worldMatrix();
    for (const auto& p : pool_) {
        if (p.life <= 0.0f) continue;
        float t = 1.0f - p.life / p.maxLife; // 0 birth .. 1 death
        ParticlePoint pt;
        Vec3 pos = p.pos;
        if (!worldSpace) {
            Vec4 wp = wm * Vec4(pos, 1.0f);
            pos = Vec3(wp.x, wp.y, wp.z);
        }
        pt.pos = pos;
        pt.size = lerpf(sizeStart, sizeEnd, t);
        pt.color = Vec4(lerpf(colorStart.x, colorEnd.x, t), lerpf(colorStart.y, colorEnd.y, t),
                        lerpf(colorStart.z, colorEnd.z, t), lerpf(colorStart.w, colorEnd.w, t));
        pt.rot = p.rot;
        if (frames > 1) {
            // fps > 0 plays the grid at that rate (looping); 0 spreads it
            // once across the particle's lifetime.
            float age = p.maxLife - p.life;
            pt.frame = flipbookFps > 0.0f ? std::fmod(age * flipbookFps, (float)frames)
                                          : t * (float)frames;
        }
        batch.points.push_back(pt);
    }
    if (!batch.points.empty()) out.particles.push_back(std::move(batch));
}

} // namespace ae
