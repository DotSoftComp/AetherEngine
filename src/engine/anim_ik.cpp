// Aether Engine — two-bone IK solve (see anim_ik.h).
#include "anim_ik.h"
#include "components.h"
#include "world.h"
#include "../core/log.h"
#include <cmath>

namespace ae {

ModelComponent* TwoBoneIKComponent::findModel(Entity** modelEntity) {
    if (auto* mc = entity().getComponent<ModelComponent>()) {
        *modelEntity = &entity();
        return mc;
    }
    for (Entity* c : entity().children()) {
        if (auto* mc = c->getComponent<ModelComponent>()) {
            *modelEntity = c;
            return mc;
        }
    }
    return nullptr;
}

void TwoBoneIKComponent::onLateUpdate(float) {
    if (weight <= 0.001f || endBone.empty()) return;
    Entity* modelEntity = nullptr;
    ModelComponent* mc = findModel(&modelEntity);
    Model* model = mc ? mc->model : nullptr;
    if (!model) return;
    ModelPose& pose = mc->pose();
    if (pose.empty()) return;

    int e = model->nodeIndex(endBone);
    int m = e >= 0 ? model->nodeParent(e) : -1;
    int r = m >= 0 ? model->nodeParent(m) : -1;
    if (r < 0) {
        if (!warned_) {
            AE_WARN("[IK] end bone '%s' %s", endBone.c_str(),
                    e < 0 ? "not found in model" : "has no two-parent chain");
            warned_ = true;
        }
        return;
    }

    // Resolve the target (and optional pole) entity, upgrading name refs.
    auto resolve = [&](EntityRef& ref, const std::string& name) -> Entity* {
        Entity* t = ref.get(world());
        if (!t && !name.empty()) {
            t = world().find(name);
            if (t) ref.set(t);
        }
        return t;
    };
    Entity* target = resolve(targetRef, targetName);
    if (!target) return;
    Entity* pole = resolve(poleRef, poleName);

    // Everything solves in model space.
    Mat4 toModel = inverse(modelEntity->worldMatrix());
    auto toModelPos = [&](const Vec3& w) {
        Vec4 p = toModel * Vec4(w.x, w.y, w.z, 1.0f);
        return Vec3(p.x, p.y, p.z);
    };
    Vec3 t = toModelPos(target->worldPosition());

    auto jointPos = [&](int n) {
        return Vec3(pose.worlds[n].m[3][0], pose.worlds[n].m[3][1], pose.worlds[n].m[3][2]);
    };
    Vec3 a = jointPos(r), b = jointPos(m), c = jointPos(e);
    float l1 = length(b - a), l2 = length(c - b);
    if (l1 < 1e-5f || l2 < 1e-5f) return;

    Vec3 toTarget = t - a;
    float dist = length(toTarget);
    if (dist < 1e-5f) return;
    Vec3 d = toTarget * (1.0f / dist);
    dist = clampf(dist, std::fabs(l1 - l2) + 1e-3f, l1 + l2 - 1e-3f);

    // Bend plane: toward the pole if given, else keep the current bend.
    Vec3 planeRef = pole ? toModelPos(pole->worldPosition()) - a : b - a;
    Vec3 axis = cross(d, planeRef);
    if (dot(axis, axis) < 1e-8f) {
        axis = cross(d, Vec3(0, 1, 0));
        if (dot(axis, axis) < 1e-8f) axis = cross(d, Vec3(1, 0, 0));
    }
    axis = normalize(axis);

    // Law of cosines: the root-side interior angle of the (l1, l2, dist)
    // triangle; the mid joint lands by rotating the aim direction into the
    // bend plane.
    float cosA = clampf((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist), -1.0f, 1.0f);
    Vec3 bNew = a + quatRotate(quatAxisAngle(axis, std::acos(cosA)), d) * l1;
    Vec3 cNew = a + d * dist;

    // Write back as local rotations: rotate the root joint to land the mid,
    // then the mid joint to land the effector.
    auto worldRot = [&](int n) { return quatFromMat4(pose.worlds[n]); };
    Vec4 q1 = quatFromTo(b - a, bNew - a);
    Vec4 rootWorldNew = quatNormalize(quatMul(q1, worldRot(r)));
    Vec3 cAfterQ1 = a + quatRotate(q1, c - a);
    Vec4 q2 = quatFromTo(cAfterQ1 - bNew, cNew - bNew);
    Vec4 midWorldNew = quatNormalize(quatMul(q2, quatMul(q1, worldRot(m))));

    int rParent = model->nodeParent(r);
    Vec4 parentRot = rParent >= 0 ? worldRot(rParent) : quatIdentity();
    Vec4 rootLocal = quatNormalize(quatMul(quatConj(parentRot), rootWorldNew));
    Vec4 midLocal = quatNormalize(quatMul(quatConj(rootWorldNew), midWorldNew));

    float w = clampf(weight, 0.0f, 1.0f);
    pose.locals[r].r = nlerpQuat(pose.locals[r].r, rootLocal, w);
    pose.locals[m].r = nlerpQuat(pose.locals[m].r, midLocal, w);
    pose.useMatrix[r] = pose.useMatrix[m] = 0;
    model->finalizePose(pose);
}

} // namespace ae
