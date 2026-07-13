// Aether Engine — a compact immediate-mode GUI for the editor.
//
// One batched vertex stream of colored/textured quads drawn in call order, so
// panels layer correctly. Solid fills sample the font atlas's white texel;
// text samples glyphs; images bind their own texture. Widgets return their
// interaction result the same frame (button click, drag delta, selection).
#pragma once
#include "../rhi/rhi.h"
#include "../gl/gl_api.h"
#include "../core/math3d.h"
#include "../core/input.h"
#include "../render/shader.h"
#include "font.h"
#include <vector>
#include <cstdint>

namespace ae {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    Rect inset(float m) const { return {x + m, y + m, w - 2 * m, h - 2 * m}; }
};

inline uint32_t rgba(float r, float g, float b, float a = 1.0f) {
    auto b8 = [](float v) { return (uint32_t)(clampf(v, 0, 1) * 255.0f + 0.5f); };
    return b8(r) | (b8(g) << 8) | (b8(b) << 16) | (b8(a) << 24);
}
inline uint32_t rgbaV(const Vec3& c, float a = 1.0f) { return rgba(c.x, c.y, c.z, a); }

// Editor theme (dark, cool-grey with a violet accent).
namespace theme {
constexpr uint32_t bg         = 0xFF1A1614; // window background (ABGR-ish via rgba())
}

class UI {
public:
    bool init(Font* font);
    void destroy();

    void begin(int screenW, int screenH, const Input& input);
    // Uploads + draws. `targetFBO` 0 = the window backbuffer; the editor
    // passes its viewport FBO so the in-game HUD composites into the 3D view.
    void end(rhi::FramebufferHandle targetFBO = {});

    // ---- primitives ----
    void rectFill(const Rect& r, uint32_t color);
    void rectLine(const Rect& r, uint32_t color, float t = 1.0f);
    void gradientV(const Rect& r, uint32_t top, uint32_t bottom);
    void text(float x, float y, const char* s, uint32_t color, float spacing = 0.0f,
              float scale = 1.0f);
    void textCentered(const Rect& r, const char* s, uint32_t color, float scale = 1.0f);
    float measureText(const char* s, float spacing = 0.0f) const;
    void image(const Rect& r, unsigned tex, bool flipY); // rhi texture id
    void pushClip(const Rect& r);
    void popClip();

    // ---- vector shapes (solid fill, drawn against the font atlas's white
    // texel) — the building blocks for Detroit-style diamond/ring HUD prompts.
    void triFill(Vec2 a, Vec2 b, Vec2 c, uint32_t color);
    void convexFill(const Vec2* pts, int count, uint32_t color);
    void line(Vec2 a, Vec2 b, float thickness, uint32_t color);
    void diamond(Vec2 center, float size, uint32_t color);
    void diamondOutline(Vec2 center, float size, float thickness, uint32_t color);
    void triangleArrow(Vec2 center, float size, int direction, uint32_t color); // 0=up,1=down,2=left,3=right
    // Filled ring/arc between two radii, degrees measured clockwise from top
    // (0 = 12 o'clock) — used for countdown/progress dials.
    void ringArc(Vec2 center, float rOuter, float rInner, float startDeg, float endDeg,
                 uint32_t color, int segments = 48);

    // ---- widgets (id = any stable pointer) ----
    bool button(const void* id, const Rect& r, const char* label,
                uint32_t base, uint32_t hover);
    bool selectable(const void* id, const Rect& r, const char* label, bool selected);
    // Horizontal jog: returns pixels dragged this frame (0 when idle).
    float jog(const void* id, const Rect& r);
    // Absolute drag-float editor; returns true if value changed.
    bool dragFloat(const void* id, const Rect& r, const char* label, float* value,
                   float speed);

    // ---- input state helpers ----
    const Input& input() const { return *input_; }
    bool mousePressed() const { return mouseDown_ && !prevMouseDown_; }
    bool mouseReleased() const { return !mouseDown_ && prevMouseDown_; }
    float mouseX() const { return mouseX_; }
    float mouseY() const { return mouseY_; }
    Font* font() const { return font_; }

private:
    struct Vertex { float x, y, u, v; uint32_t col; };
    struct Command { unsigned tex; int first, count; Rect clip; };

    void quad(unsigned tex, const Rect& r, float u0, float v0, float u1, float v1, uint32_t col);
    void solid(const Rect& r, uint32_t color) {
        quad(font_->atlas(), r, font_->whiteU(), font_->whiteV(), font_->whiteU(),
             font_->whiteV(), color);
    }
    // Opens a new draw command when the texture or clip rect changed since the
    // last primitive, so batches stay contiguous in the vertex stream.
    void ensureCmd(unsigned tex);

    Font* font_ = nullptr;
    Shader shader_;
    rhi::StreamHandle stream_;
    std::vector<Vertex> verts_;
    std::vector<Command> cmds_;
    std::vector<Rect> clipStack_;
    int screenW_ = 0, screenH_ = 0;

    const Input* input_ = nullptr;
    float mouseX_ = 0, mouseY_ = 0;
    bool mouseDown_ = false, prevMouseDown_ = false;
    const void* active_ = nullptr; // widget currently capturing the drag
};

} // namespace ae
