#include "ui.h"
#include "../rhi/rhi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ae {

bool UI::init(Font* font) {
    font_ = font;
    if (!shader_.load("ui.vert", "ui.frag")) return false;

    const rhi::StreamAttr attrs[] = {
        {0, 2, offsetof(Vertex, x)},
        {1, 2, offsetof(Vertex, u)},
        {2, 4, offsetof(Vertex, col), /*unorm8=*/true},
    };
    stream_ = rhi::createStream(sizeof(Vertex), attrs, 3);
    return true;
}

void UI::begin(int screenW, int screenH, const Input& input) {
    screenW_ = screenW;
    screenH_ = screenH;
    input_ = &input;
    prevMouseDown_ = mouseDown_;
    mouseDown_ = input.mouseButtons[0];
    mouseX_ = input.mouseX;
    mouseY_ = input.mouseY;
    if (!mouseDown_) active_ = nullptr;

    verts_.clear();
    cmds_.clear();
    clipStack_.clear();
    clipStack_.push_back({0, 0, (float)screenW, (float)screenH});
}

void UI::ensureCmd(unsigned tex) {
    const Rect& clip = clipStack_.back();
    // Merge into the current command when texture + clip match, else open a new
    // one (keeps draw order and lets each command carry a scissor rect).
    if (cmds_.empty() || cmds_.back().tex != tex || cmds_.back().clip.x != clip.x ||
        cmds_.back().clip.y != clip.y || cmds_.back().clip.w != clip.w ||
        cmds_.back().clip.h != clip.h) {
        cmds_.push_back({tex, (int)verts_.size(), 0, clip});
    }
}

void UI::quad(unsigned tex, const Rect& r, float u0, float v0, float u1, float v1, uint32_t col) {
    ensureCmd(tex);
    float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    Vertex a{x0, y0, u0, v0, col}, b{x1, y0, u1, v0, col};
    Vertex c{x1, y1, u1, v1, col}, d{x0, y1, u0, v1, col};
    verts_.push_back(a); verts_.push_back(b); verts_.push_back(c);
    verts_.push_back(a); verts_.push_back(c); verts_.push_back(d);
    cmds_.back().count += 6;
}

void UI::triFill(Vec2 a, Vec2 b, Vec2 c, uint32_t color) {
    unsigned tex = font_->atlas();
    float u = font_->whiteU(), v = font_->whiteV();
    ensureCmd(tex);
    verts_.push_back({a.x, a.y, u, v, color});
    verts_.push_back({b.x, b.y, u, v, color});
    verts_.push_back({c.x, c.y, u, v, color});
    cmds_.back().count += 3;
}

void UI::convexFill(const Vec2* pts, int count, uint32_t color) {
    for (int i = 1; i + 1 < count; ++i) triFill(pts[0], pts[i], pts[i + 1], color);
}

void UI::line(Vec2 a, Vec2 b, float thickness, uint32_t color) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * thickness * 0.5f, ny = dx / len * thickness * 0.5f;
    Vec2 pts[4] = {Vec2(a.x + nx, a.y + ny), Vec2(b.x + nx, b.y + ny),
                   Vec2(b.x - nx, b.y - ny), Vec2(a.x - nx, a.y - ny)};
    convexFill(pts, 4, color);
}

void UI::diamond(Vec2 center, float size, uint32_t color) {
    Vec2 pts[4] = {Vec2(center.x, center.y - size), Vec2(center.x + size, center.y),
                   Vec2(center.x, center.y + size), Vec2(center.x - size, center.y)};
    convexFill(pts, 4, color);
}

void UI::diamondOutline(Vec2 center, float size, float thickness, uint32_t color) {
    Vec2 p[4] = {Vec2(center.x, center.y - size), Vec2(center.x + size, center.y),
                 Vec2(center.x, center.y + size), Vec2(center.x - size, center.y)};
    for (int i = 0; i < 4; ++i) line(p[i], p[(i + 1) % 4], thickness, color);
}

void UI::triangleArrow(Vec2 center, float size, int direction, uint32_t color) {
    Vec2 pts[3];
    switch (direction) {
    case 0: // up
        pts[0] = Vec2(center.x, center.y - size);
        pts[1] = Vec2(center.x - size * 0.8f, center.y + size * 0.6f);
        pts[2] = Vec2(center.x + size * 0.8f, center.y + size * 0.6f);
        break;
    case 1: // down
        pts[0] = Vec2(center.x, center.y + size);
        pts[1] = Vec2(center.x - size * 0.8f, center.y - size * 0.6f);
        pts[2] = Vec2(center.x + size * 0.8f, center.y - size * 0.6f);
        break;
    case 2: // left
        pts[0] = Vec2(center.x - size, center.y);
        pts[1] = Vec2(center.x + size * 0.6f, center.y - size * 0.8f);
        pts[2] = Vec2(center.x + size * 0.6f, center.y + size * 0.8f);
        break;
    default: // right
        pts[0] = Vec2(center.x + size, center.y);
        pts[1] = Vec2(center.x - size * 0.6f, center.y - size * 0.8f);
        pts[2] = Vec2(center.x - size * 0.6f, center.y + size * 0.8f);
        break;
    }
    convexFill(pts, 3, color);
}

void UI::ringArc(Vec2 center, float rOuter, float rInner, float startDeg, float endDeg,
                 uint32_t color, int segments) {
    unsigned tex = font_->atlas();
    float u = font_->whiteU(), v = font_->whiteV();
    ensureCmd(tex);
    float span = endDeg - startDeg;
    int segs = std::max(1, (int)(segments * clampf(std::fabs(span) / 360.0f, 0.03f, 1.0f)));
    for (int i = 0; i < segs; ++i) {
        // -90 so 0 degrees sits at 12 o'clock; increasing degrees sweeps
        // clockwise, matching a countdown timer.
        float a0 = radians(startDeg + span * (float)i / segs - 90.0f);
        float a1 = radians(startDeg + span * (float)(i + 1) / segs - 90.0f);
        Vec2 o0(center.x + std::cos(a0) * rOuter, center.y + std::sin(a0) * rOuter);
        Vec2 o1(center.x + std::cos(a1) * rOuter, center.y + std::sin(a1) * rOuter);
        Vec2 i0(center.x + std::cos(a0) * rInner, center.y + std::sin(a0) * rInner);
        Vec2 i1(center.x + std::cos(a1) * rInner, center.y + std::sin(a1) * rInner);
        verts_.push_back({o0.x, o0.y, u, v, color});
        verts_.push_back({o1.x, o1.y, u, v, color});
        verts_.push_back({i1.x, i1.y, u, v, color});
        verts_.push_back({o0.x, o0.y, u, v, color});
        verts_.push_back({i1.x, i1.y, u, v, color});
        verts_.push_back({i0.x, i0.y, u, v, color});
        cmds_.back().count += 6;
    }
}

void UI::rectFill(const Rect& r, uint32_t color) { solid(r, color); }

void UI::rectLine(const Rect& r, uint32_t color, float t) {
    solid({r.x, r.y, r.w, t}, color);
    solid({r.x, r.y + r.h - t, r.w, t}, color);
    solid({r.x, r.y, t, r.h}, color);
    solid({r.x + r.w - t, r.y, t, r.h}, color);
}

void UI::gradientV(const Rect& r, uint32_t top, uint32_t bottom) {
    unsigned tex = font_->atlas();
    float u = font_->whiteU(), v = font_->whiteV();
    ensureCmd(tex);
    float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    Vertex a{x0, y0, u, v, top}, b{x1, y0, u, v, top};
    Vertex c{x1, y1, u, v, bottom}, d{x0, y1, u, v, bottom};
    verts_.push_back(a); verts_.push_back(b); verts_.push_back(c);
    verts_.push_back(a); verts_.push_back(c); verts_.push_back(d);
    cmds_.back().count += 6;
}

void UI::text(float x, float y, const char* s, uint32_t color, float spacing, float scale) {
    unsigned atlas = font_->atlas();
    float penX = x;
    for (; *s; ++s) {
        const Glyph& g = font_->glyph(*s);
        if (g.w > 0 && *s != ' ')
            quad(atlas, {penX, y, g.w * scale, g.h * scale}, g.u0, g.v0, g.u1, g.v1, color);
        penX += (g.advance + spacing) * scale;
    }
}

void UI::textCentered(const Rect& r, const char* s, uint32_t color, float scale) {
    float tw = font_->textWidth(s) * scale;
    float tx = r.x + (r.w - tw) * 0.5f;
    float ty = r.y + (r.h - font_->lineHeight() * scale) * 0.5f;
    text(tx, ty, s, color, 0.0f, scale);
}

float UI::measureText(const char* s, float spacing) const {
    float w = 0;
    bool first = true;
    for (; *s; ++s) {
        if (!first) w += spacing;
        w += font_->glyph(*s).advance;
        first = false;
    }
    return w;
}

void UI::image(const Rect& r, unsigned tex, bool flipY) {
    float v0 = flipY ? 1.0f : 0.0f, v1 = flipY ? 0.0f : 1.0f;
    quad(tex, r, 0, v0, 1, v1, rgba(1, 1, 1, 1));
}

void UI::pushClip(const Rect& r) {
    // Intersect with the current clip so nested panels stay contained.
    const Rect& c = clipStack_.back();
    float x0 = std::fmax(r.x, c.x), y0 = std::fmax(r.y, c.y);
    float x1 = std::fmin(r.x + r.w, c.x + c.w), y1 = std::fmin(r.y + r.h, c.y + c.h);
    clipStack_.push_back({x0, y0, std::fmax(0.0f, x1 - x0), std::fmax(0.0f, y1 - y0)});
}

void UI::popClip() {
    if (clipStack_.size() > 1) clipStack_.pop_back();
}

bool UI::button(const void* id, const Rect& r, const char* label, uint32_t base, uint32_t hover) {
    bool over = r.contains(mouseX_, mouseY_);
    rectFill(r, over ? hover : base);
    textCentered(r, label, rgba(0.94f, 0.95f, 0.97f));
    return over && mousePressed();
}

bool UI::selectable(const void* id, const Rect& r, const char* label, bool selected) {
    bool over = r.contains(mouseX_, mouseY_);
    if (selected) rectFill(r, rgba(0.36f, 0.30f, 0.72f, 0.85f));
    else if (over) rectFill(r, rgba(1, 1, 1, 0.06f));
    float ty = r.y + (r.h - font_->lineHeight()) * 0.5f;
    text(r.x + 8, ty, label, selected ? rgba(1, 1, 1) : rgba(0.80f, 0.82f, 0.86f));
    return over && mousePressed();
}

float UI::jog(const void* id, const Rect& r) {
    bool over = r.contains(mouseX_, mouseY_);
    if (over && mousePressed()) active_ = id;
    float delta = 0.0f;
    if (active_ == id && mouseDown_) delta = input_->mouseDX;
    rectFill(r, active_ == id ? rgba(0.30f, 0.26f, 0.50f) : (over ? rgba(1, 1, 1, 0.10f) : rgba(1, 1, 1, 0.05f)));
    return delta;
}

bool UI::dragFloat(const void* id, const Rect& r, const char* label, float* value, float speed) {
    bool over = r.contains(mouseX_, mouseY_);
    if (over && mousePressed()) active_ = id;
    bool changed = false;
    if (active_ == id && mouseDown_ && input_->mouseDX != 0.0f) {
        *value += input_->mouseDX * speed;
        changed = true;
    }
    rectFill(r, active_ == id ? rgba(0.30f, 0.26f, 0.50f) : (over ? rgba(1, 1, 1, 0.10f) : rgba(0.10f, 0.10f, 0.12f)));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %.2f", label, *value);
    float ty = r.y + (r.h - font_->lineHeight()) * 0.5f;
    text(r.x + 6, ty, buf, rgba(0.85f, 0.87f, 0.90f));
    return changed;
}

void UI::end(rhi::FramebufferHandle targetFBO) {
    if (!verts_.empty())
        rhi::setStreamData(stream_, verts_.data(), verts_.size() * sizeof(Vertex));

    rhi::bindFramebuffer(targetFBO);
    rhi::setViewport(0, 0, screenW_, screenH_);
    rhi::setState({false, true, rhi::Blend::Alpha, false});
    rhi::setScissor(true, 0, 0, screenW_, screenH_);

    shader_.use();
    shader_.setVec2("uScreen", (float)screenW_, (float)screenH_);

    for (const Command& c : cmds_) {
        if (c.count == 0) continue;
        int sx = (int)c.clip.x;
        int sy = screenH_ - (int)(c.clip.y + c.clip.h); // GL scissor origin bottom-left
        int sw = (int)c.clip.w, sh = (int)c.clip.h;
        if (sw <= 0 || sh <= 0) continue;
        rhi::setScissor(true, sx, sy, sw, sh);
        rhi::bindTexture(0, c.tex);
        rhi::drawStream(stream_, rhi::Topology::Triangles, c.first, c.count);
    }

    rhi::setScissor(false);
    rhi::setState({});
}

void UI::destroy() {
    shader_.destroy();
    rhi::destroyStream(stream_);
}

} // namespace ae
