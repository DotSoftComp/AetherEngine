// Aether Engine — immediate-mode debug drawing: colored 3D lines and wire
// shapes collected from anywhere (editor gizmos, gameplay code, the DrawLine
// script node) and rendered depth-tested over the frame, then cleared. The
// editor uses it to visualize colliders and light ranges; games can leave
// calls in — the collector is cheap and the pass skips when empty.
#pragma once
#include "../core/math3d.h"
#include <vector>

namespace ae {

class DebugDraw {
public:
    struct Line {
        Vec3 a, b;
        Vec3 color;
    };

    void line(const Vec3& a, const Vec3& b, const Vec3& color) { lines_.push_back({a, b, color}); }
    void circle(const Vec3& center, const Vec3& u, const Vec3& v, float r, const Vec3& color,
                int segments = 24);
    // Oriented wire box: unit cube corners transformed by `xform` (center +
    // rotation + half-extents baked in by the caller).
    void wireBox(const Mat4& xform, const Vec3& half, const Vec3& color);
    void wireSphere(const Vec3& center, float r, const Vec3& color);
    // Vertical capsule (Y axis), matching the physics collider convention.
    void wireCapsule(const Vec3& center, float radius, float halfSegment, const Vec3& color);

    const std::vector<Line>& lines() const { return lines_; }
    void clear() { lines_.clear(); }

private:
    std::vector<Line> lines_;
};

// Process-wide collector (lives in AetherCore.dll).
DebugDraw& debugDraw();

} // namespace ae
