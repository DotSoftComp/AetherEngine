// Aether Engine — local TRS transform for an entity in the scene graph.
#pragma once
#include "../core/math3d.h"

namespace ae {

struct Transform {
    Vec3 position{0, 0, 0};
    Vec4 rotation = quatIdentity(); // quaternion (x, y, z, w)
    Vec3 scaling{1, 1, 1};

    Mat4 matrix() const {
        return translate(position) *
               quatToMat4(rotation.x, rotation.y, rotation.z, rotation.w) *
               scale(scaling);
    }

    void setEulerY(float yawRad) { rotation = quatAxisAngle(Vec3(0, 1, 0), yawRad); }
    void rotateAxis(const Vec3& axis, float angleRad) {
        rotation = quatNormalize(quatMul(quatAxisAngle(axis, angleRad), rotation));
    }
};

} // namespace ae
