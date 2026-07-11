// Aether Engine — minimal linear algebra for real-time rendering (column-major, OpenGL conventions)
#pragma once
#include <cmath>

namespace ae {

constexpr float PI = 3.14159265358979323846f;
inline float radians(float deg) { return deg * PI / 180.0f; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float v) : x(v), y(v), z(v) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) { float l = length(v); return l > 1e-8f ? v / l : Vec3(0, 1, 0); }

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    Vec3 xyz() const { return {x, y, z}; }
};

// Column-major 4x4: m[col][row], matches GLSL/OpenGL memory layout.
struct Mat4 {
    float m[4][4] = {};

    static Mat4 identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rw = 0; rw < 4; ++rw)
                r.m[c][rw] = m[0][rw] * o.m[c][0] + m[1][rw] * o.m[c][1] +
                             m[2][rw] * o.m[c][2] + m[3][rw] * o.m[c][3];
        return r;
    }

    Vec4 operator*(const Vec4& v) const {
        return {
            m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w,
            m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w,
        };
    }

    const float* data() const { return &m[0][0]; }
};

inline Mat4 translate(const Vec3& t) {
    Mat4 r = Mat4::identity();
    r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z;
    return r;
}

inline Mat4 scale(const Vec3& s) {
    Mat4 r = Mat4::identity();
    r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
    return r;
}

inline Mat4 rotate(float angleRad, const Vec3& axis) {
    Vec3 a = normalize(axis);
    float c = std::cos(angleRad), s = std::sin(angleRad), ic = 1.0f - c;
    Mat4 r = Mat4::identity();
    r.m[0][0] = c + a.x * a.x * ic;      r.m[0][1] = a.y * a.x * ic + a.z * s; r.m[0][2] = a.z * a.x * ic - a.y * s;
    r.m[1][0] = a.x * a.y * ic - a.z * s; r.m[1][1] = c + a.y * a.y * ic;      r.m[1][2] = a.z * a.y * ic + a.x * s;
    r.m[2][0] = a.x * a.z * ic + a.y * s; r.m[2][1] = a.y * a.z * ic - a.x * s; r.m[2][2] = c + a.z * a.z * ic;
    return r;
}

inline Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    Vec3 f = normalize(center - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r = Mat4::identity();
    r.m[0][0] = s.x;  r.m[1][0] = s.y;  r.m[2][0] = s.z;
    r.m[0][1] = u.x;  r.m[1][1] = u.y;  r.m[2][1] = u.z;
    r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
    r.m[3][0] = -dot(s, eye);
    r.m[3][1] = -dot(u, eye);
    r.m[3][2] = dot(f, eye);
    return r;
}

inline Mat4 perspective(float fovyRad, float aspect, float zNear, float zFar) {
    float t = std::tan(fovyRad * 0.5f);
    Mat4 r;
    r.m[0][0] = 1.0f / (aspect * t);
    r.m[1][1] = 1.0f / t;
    r.m[2][2] = -(zFar + zNear) / (zFar - zNear);
    r.m[2][3] = -1.0f;
    r.m[3][2] = -(2.0f * zFar * zNear) / (zFar - zNear);
    return r;
}

inline Mat4 ortho(float l, float r_, float b, float t, float n, float f) {
    Mat4 r = Mat4::identity();
    r.m[0][0] = 2.0f / (r_ - l);
    r.m[1][1] = 2.0f / (t - b);
    r.m[2][2] = -2.0f / (f - n);
    r.m[3][0] = -(r_ + l) / (r_ - l);
    r.m[3][1] = -(t + b) / (t - b);
    r.m[3][2] = -(f + n) / (f - n);
    return r;
}

// Unit quaternion (x, y, z, w) -> rotation matrix.
inline Mat4 quatToMat4(float x, float y, float z, float w) {
    Mat4 r = Mat4::identity();
    r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y + z * w);     r.m[0][2] = 2 * (x * z - y * w);
    r.m[1][0] = 2 * (x * y - z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z + x * w);
    r.m[2][0] = 2 * (x * z + y * w);     r.m[2][1] = 2 * (y * z - x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}

// Quaternion (x, y, z, w) helpers for the engine transform hierarchy.
inline Vec4 quatIdentity() { return Vec4(0, 0, 0, 1); }

inline Vec4 quatAxisAngle(const Vec3& axis, float angleRad) {
    Vec3 a = normalize(axis);
    float h = angleRad * 0.5f;
    float s = std::sin(h);
    return Vec4(a.x * s, a.y * s, a.z * s, std::cos(h));
}

inline Vec4 quatMul(const Vec4& a, const Vec4& b) {
    return Vec4(
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

inline Vec4 quatConj(const Vec4& q) { return Vec4(-q.x, -q.y, -q.z, q.w); }

inline Vec4 quatNormalize(const Vec4& q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-8f ? Vec4(q.x / l, q.y / l, q.z / l, q.w / l) : quatIdentity();
}

// Rotates v by q, built from quatToMat4 so it's guaranteed consistent with
// how Transform::matrix() rotates meshes/cameras (same column-major multiply
// as Mat4::operator*(Vec4)).
inline Vec3 quatRotate(const Vec4& q, const Vec3& v) {
    Mat4 m = quatToMat4(q.x, q.y, q.z, q.w);
    return Vec3(m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z,
                m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z,
                m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z);
}

// Matrix(columns)->quaternion, the inverse of quatToMat4 (Shepperd's method).
// colX/colY/colZ are where local +X/+Y/+Z map to — i.e. exactly the columns
// quatToMat4 would produce, so round-tripping reproduces the same basis.
inline Vec4 quatFromBasis(const Vec3& colX, const Vec3& colY, const Vec3& colZ) {
    float R[3][3] = {
        {colX.x, colY.x, colZ.x},
        {colX.y, colY.y, colZ.y},
        {colX.z, colY.z, colZ.z},
    };
    float trace = R[0][0] + R[1][1] + R[2][2];
    Vec4 q;
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (R[2][1] - R[1][2]) / s;
        q.y = (R[0][2] - R[2][0]) / s;
        q.z = (R[1][0] - R[0][1]) / s;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        float s = std::sqrt(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2.0f;
        q.x = 0.25f * s;
        q.w = (R[2][1] - R[1][2]) / s;
        q.y = (R[0][1] + R[1][0]) / s;
        q.z = (R[0][2] + R[2][0]) / s;
    } else if (R[1][1] > R[2][2]) {
        float s = std::sqrt(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2.0f;
        q.y = 0.25f * s;
        q.w = (R[0][2] - R[2][0]) / s;
        q.x = (R[0][1] + R[1][0]) / s;
        q.z = (R[1][2] + R[2][1]) / s;
    } else {
        float s = std::sqrt(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2.0f;
        q.z = 0.25f * s;
        q.w = (R[1][0] - R[0][1]) / s;
        q.x = (R[0][2] + R[2][0]) / s;
        q.y = (R[1][2] + R[2][1]) / s;
    }
    return quatNormalize(q);
}

// Quaternion that orients local -Z (our "forward") to point along `forward`.
// Used by camera rigs (third-person, follow, dialogue cuts) to aim at a
// target without touching yaw/pitch state.
inline Vec4 quatLookAt(const Vec3& forward, const Vec3& upHint = Vec3(0, 1, 0)) {
    Vec3 f = normalize(forward);
    Vec3 back = -f; // local +Z maps to "back" under the -Z-forward convention
    Vec3 right = cross(upHint, back);
    if (dot(right, right) < 1e-10f) right = cross(Vec3(1, 0, 0), back); // forward ~= upHint
    right = normalize(right);
    Vec3 up = cross(back, right);
    return quatFromBasis(right, up, back);
}

// Inverse for affine + projective matrices (general 4x4 via cofactors).
Mat4 inverse(const Mat4& in);

} // namespace ae
