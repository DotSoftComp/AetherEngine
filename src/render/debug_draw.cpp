// Aether Engine — debug-draw shape helpers (see debug_draw.h).
#include "debug_draw.h"
#include <cmath>

namespace ae {

DebugDraw& debugDraw() {
    static DebugDraw g;
    return g;
}

void DebugDraw::circle(const Vec3& center, const Vec3& u, const Vec3& v, float r,
                       const Vec3& color, int segments) {
    Vec3 prev = center + u * r;
    for (int i = 1; i <= segments; ++i) {
        float a = (float)i / segments * 2.0f * PI;
        Vec3 p = center + u * (std::cos(a) * r) + v * (std::sin(a) * r);
        line(prev, p, color);
        prev = p;
    }
}

void DebugDraw::wireBox(const Mat4& xform, const Vec3& half, const Vec3& color) {
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        Vec3 l(i & 1 ? half.x : -half.x, i & 2 ? half.y : -half.y, i & 4 ? half.z : -half.z);
        Vec4 w = xform * Vec4(l, 1.0f);
        c[i] = Vec3(w.x, w.y, w.z);
    }
    static const int e[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                 {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& ed : e) line(c[ed[0]], c[ed[1]], color);
}

void DebugDraw::wireSphere(const Vec3& center, float r, const Vec3& color) {
    circle(center, Vec3(1, 0, 0), Vec3(0, 1, 0), r, color);
    circle(center, Vec3(1, 0, 0), Vec3(0, 0, 1), r, color);
    circle(center, Vec3(0, 1, 0), Vec3(0, 0, 1), r, color);
}

void DebugDraw::wireCapsule(const Vec3& center, float radius, float halfSegment,
                            const Vec3& color) {
    Vec3 top = center + Vec3(0, halfSegment, 0);
    Vec3 bot = center - Vec3(0, halfSegment, 0);
    circle(top, Vec3(1, 0, 0), Vec3(0, 0, 1), radius, color);
    circle(bot, Vec3(1, 0, 0), Vec3(0, 0, 1), radius, color);
    circle(top, Vec3(1, 0, 0), Vec3(0, 1, 0), radius, color, 12);
    circle(bot, Vec3(1, 0, 0), Vec3(0, -1, 0), radius, color, 12);
    for (int i = 0; i < 4; ++i) {
        float a = i * 0.5f * PI;
        Vec3 off(std::cos(a) * radius, 0, std::sin(a) * radius);
        line(top + off, bot + off, color);
    }
}

} // namespace ae
