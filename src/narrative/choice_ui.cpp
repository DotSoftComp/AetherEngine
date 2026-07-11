#include "choice_ui.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace ae {

namespace {
const uint32_t kAccent   = rgba(0.62f, 0.53f, 0.98f);
const uint32_t kAccentLo = rgba(0.55f, 0.45f, 0.96f);
const uint32_t kGreen    = rgba(0.29f, 0.77f, 0.46f);
const uint32_t kRed      = rgba(0.87f, 0.33f, 0.35f);
const uint32_t kWhite    = rgba(0.95f, 0.96f, 0.99f);

const char* keyLabel(int vk) {
    switch (vk) {
    case VK_SPACE: return "SPACE";
    case VK_RETURN: return "ENTER";
    case VK_SHIFT: return "SHIFT";
    case VK_CONTROL: return "CTRL";
    case VK_TAB: return "TAB";
    default: break;
    }
    static char buf[2];
    buf[0] = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') ? (char)vk : '?';
    buf[1] = 0;
    return buf;
}

bool isArrowKey(int vk) { return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT; }
int arrowDir(int vk) {
    return vk == VK_UP ? 0 : vk == VK_DOWN ? 1 : vk == VK_LEFT ? 2 : 3;
}
} // namespace

// ---------------------------------------------------------------------------
// ChoicePrompt
// ---------------------------------------------------------------------------

void ChoicePrompt::begin(std::vector<ChoiceOption> options, float timeLimit) {
    options_ = std::move(options);
    for (auto& o : options_)
        for (auto& c : o.text) c = (char)std::toupper((unsigned char)c);
    hovered_ = -1;
    selected_ = -1;
    timeLimit_ = timeLimit;
    remaining_ = timeLimit;
    elapsed_ = 0.0f;
    active_ = true;
}

ChoiceResult ChoicePrompt::tick(UI& ui, float dt, const Rect& area) {
    if (!active_) return ChoiceResult::None;
    elapsed_ += dt;
    if (timeLimit_ > 0.0f) remaining_ = std::max(0.0f, remaining_ - dt);

    const int n = (int)options_.size();
    const float rowH = 58, gap = 14, rowW = 460;
    float totalH = n * rowH + (n - 1) * gap;
    float startY = area.y + area.h * 0.60f - totalH * 0.5f;
    float cx = area.x + area.w * 0.5f;

    std::vector<Rect> rows(n);
    hovered_ = -1;
    for (int i = 0; i < n; ++i) {
        rows[i] = {cx - rowW * 0.5f, startY + i * (rowH + gap), rowW, rowH};
        if (rows[i].contains(ui.mouseX(), ui.mouseY())) hovered_ = i;
    }

    int keySel = -1;
    for (int i = 0; i < n && i < 9; ++i)
        if (ui.input().keys['1' + i]) keySel = i;

    ChoiceResult result = ChoiceResult::None;
    if (keySel >= 0) { selected_ = keySel; result = ChoiceResult::Picked; }
    else if (hovered_ >= 0 && ui.mousePressed()) { selected_ = hovered_; result = ChoiceResult::Picked; }
    else if (timeLimit_ > 0.0f && remaining_ <= 0.0f) { selected_ = -1; result = ChoiceResult::TimedOut; }

    // ---- draw ----
    float fadeAll = clampf(elapsed_ / 0.15f, 0.0f, 1.0f);
    for (int i = 0; i < n; ++i) {
        float localT = clampf((elapsed_ - i * 0.05f) / 0.18f, 0.0f, 1.0f);
        float ease = localT * localT * (3.0f - 2.0f * localT); // smoothstep
        if (ease <= 0.001f) continue;
        float slide = (1.0f - ease) * 20.0f;
        Rect visR{rows[i].x, rows[i].y + slide, rows[i].w, rows[i].h};
        bool isHover = (hovered_ == i);

        ui.rectFill(visR, isHover ? rgba(0.55f, 0.45f, 0.96f, 0.16f * ease)
                                  : rgba(0.0f, 0.0f, 0.0f, 0.42f * ease));
        ui.rectFill({visR.x, visR.y, 3, visR.h},
                    isHover ? rgba(0.60f, 0.50f, 0.98f, ease) : rgba(1, 1, 1, 0.10f * ease));

        Vec2 dCenter(visR.x + 32, visR.y + visR.h * 0.5f);
        if (isHover) {
            ui.diamond(dCenter, 9, rgba(0.62f, 0.53f, 0.98f, ease));
        } else {
            float pulse = 0.75f + 0.20f * std::sin(elapsed_ * 2.4f + i * 0.9f);
            ui.diamondOutline(dCenter, 9, 1.5f, rgba(1, 1, 1, 0.55f * pulse * ease));
        }

        char numBuf[2] = {(char)('1' + i), 0};
        ui.text(visR.x + 10, visR.y + visR.h * 0.5f - 7, numBuf, rgba(1, 1, 1, 0.25f * ease));

        float tx = dCenter.x + 24;
        float ty = visR.y + (visR.h - ui.font()->lineHeight()) * 0.5f - 1;
        uint32_t textCol = rgba(0.93f, 0.95f, 0.99f, (isHover ? 1.0f : 0.72f) * ease);
        ui.text(tx, ty, options_[i].text.c_str(), textCol, 1.5f);
        ui.rectLine(visR, rgba(0, 0, 0, 0.5f * ease), 1.0f);
    }

    if (timeLimit_ > 0.0f) {
        Vec2 c(cx, startY - 40);
        float frac = remaining_ / timeLimit_;
        uint32_t ringCol = frac < 0.3f ? rgba(0.87f, 0.33f, 0.35f, fadeAll)
                                       : rgba(0.62f, 0.53f, 0.98f, fadeAll);
        ui.ringArc(c, 15, 11, 0, 360, rgba(1, 1, 1, 0.12f * fadeAll));
        ui.ringArc(c, 15, 11, 0, 360.0f * clampf(frac, 0.0f, 1.0f), ringCol);
    }

    if (result != ChoiceResult::None) active_ = false;
    return result;
}

// ---------------------------------------------------------------------------
// QteEvent
// ---------------------------------------------------------------------------

void QteEvent::begin(QteType type, std::vector<QteStep> steps, float duration,
                     float holdSeconds, int mashCount) {
    type_ = type;
    steps_ = std::move(steps);
    prevStepDown_.assign(steps_.size(), false);
    stepIndex_ = 0;
    duration_ = duration;
    remaining_ = duration;
    holdTarget_ = holdSeconds;
    holdProgress_ = 0.0f;
    mashTarget_ = mashCount;
    mashProgress_ = 0;
    active_ = true;
    resolved_ = false;
    shake_ = 0.0f;
    flashT_ = 0.0f;
    primed_ = false;
}

QteResult QteEvent::tick(UI& ui, float dt, const Rect& area) {
    if (!active_) return QteResult::None;
    QteResult result = QteResult::None;
    const Input& in = ui.input();

    if (!resolved_) {
        remaining_ = std::max(0.0f, remaining_ - dt);

        if (!primed_) {
            // Seed the real key state before evaluating anything. Without
            // this, a key still physically held from the *previous* prompt
            // reads as a fresh press against begin()'s all-false baseline,
            // resolving the new QTE instantly — its ring never gets to run,
            // which looks exactly like "the timer didn't reset".
            for (size_t j = 0; j < steps_.size(); ++j) prevStepDown_[j] = in.keys[steps_[j].key];
            primed_ = true;
        } else {
            int currentKey = steps_.empty() ? 0 : steps_[std::min(stepIndex_, steps_.size() - 1)].key;
            bool keyDown = currentKey ? in.keys[currentKey] : false;
            bool justPressed = keyDown && !prevStepDown_.empty() && !prevStepDown_[0] && type_ != QteType::Sequence;

            switch (type_) {
            case QteType::Tap:
                if (justPressed) result = QteResult::Success;
                break;
            case QteType::Hold:
                if (keyDown) holdProgress_ = std::min(holdTarget_, holdProgress_ + dt);
                else holdProgress_ = std::max(0.0f, holdProgress_ - dt * 2.0f);
                if (holdProgress_ >= holdTarget_) result = QteResult::Success;
                break;
            case QteType::Mash:
                if (justPressed) ++mashProgress_;
                if (mashProgress_ >= mashTarget_) result = QteResult::Success;
                break;
            case QteType::Sequence:
                for (size_t j = 0; j < steps_.size(); ++j) {
                    bool down = in.keys[steps_[j].key];
                    bool pressed = down && !prevStepDown_[j];
                    if (pressed) {
                        if (j == stepIndex_) {
                            ++stepIndex_;
                            if (stepIndex_ >= steps_.size()) result = QteResult::Success;
                            else remaining_ = duration_; // fresh window for the next key in the combo
                        } else if (j > stepIndex_) {
                            result = QteResult::Failed;
                        }
                    }
                    prevStepDown_[j] = down;
                }
                break;
            }
            if (type_ != QteType::Sequence && !prevStepDown_.empty()) prevStepDown_[0] = keyDown;
        }

        if (result == QteResult::None && remaining_ <= 0.0f) result = QteResult::Failed;

        if (result != QteResult::None) {
            resolved_ = true;
            if (result == QteResult::Success) flashT_ = 0.35f;
            else shake_ = 0.35f;
        }
    } else {
        if (flashT_ > 0.0f) flashT_ = std::max(0.0f, flashT_ - dt);
        if (shake_ > 0.0f) shake_ = std::max(0.0f, shake_ - dt);
        if (flashT_ <= 0.0f && shake_ <= 0.0f) active_ = false;
    }

    // ---- draw ----
    float shakeOff = shake_ > 0.0f ? std::sin(shake_ * 70.0f) * 7.0f * (shake_ / 0.35f) : 0.0f;
    float pop = flashT_ > 0.0f ? 1.0f + 0.22f * (flashT_ / 0.35f) : 1.0f;
    Vec2 c(area.x + area.w * 0.5f + shakeOff, area.y + area.h * 0.72f);
    float rOuter = 34.0f * pop, rInner = 26.0f * pop;

    // Solid dark backdrop so the prompt stays legible over bright/busy 3D
    // scenes — sized to bound the ring plus caption and any sequence/mash row.
    Rect backdrop{c.x - 130, c.y - 58, 260, 190};
    ui.rectFill(backdrop, rgba(0.03f, 0.03f, 0.04f, 0.80f));
    ui.rectLine(backdrop, rgba(0, 0, 0, 0.6f), 1.0f);
    ui.ringArc(c, rOuter + 10, 0.0f, 0, 360, rgba(0, 0, 0, 0.55f), 40); // dark disc behind the ring

    ui.ringArc(c, rOuter + 3, rOuter, 0, 360, rgba(0, 0, 0, 0.4f));
    ui.ringArc(c, rOuter, rInner, 0, 360, rgba(1, 1, 1, 0.10f));

    float frac = 1.0f;
    uint32_t fillCol = kAccentLo;
    if (flashT_ > 0.0f) fillCol = kGreen;
    else if (shake_ > 0.0f) fillCol = kRed;
    else if (type_ == QteType::Hold) frac = holdProgress_ / holdTarget_;
    else if (type_ == QteType::Mash) frac = (float)mashProgress_ / (float)mashTarget_;
    else frac = remaining_ / duration_;
    ui.ringArc(c, rOuter, rInner, 0, 360.0f * clampf(frac, 0.0f, 1.0f), fillCol);

    int key = steps_.empty() ? 0 : steps_[std::min(stepIndex_, steps_.size() ? steps_.size() - 1 : 0)].key;
    if (isArrowKey(key)) {
        ui.triangleArrow(c, 10 * pop, arrowDir(key), kWhite);
    } else {
        const char* label = keyLabel(key);
        float tw = ui.font()->textWidth(label);
        ui.text(c.x - tw * 0.5f, c.y - ui.font()->lineHeight() * 0.5f, label, kWhite);
    }

    const char* caption = type_ == QteType::Hold ? "HOLD" : type_ == QteType::Mash ? "MASH"
                        : type_ == QteType::Sequence ? "" : "PRESS";
    if (caption[0])
        ui.textCentered({c.x - 60, c.y + rOuter + 8, 120, 20}, caption, rgba(1, 1, 1, 0.7f));

    if (type_ == QteType::Sequence) {
        float spacing = 34.0f;
        float totalW = (steps_.empty() ? 0 : (float)(steps_.size() - 1)) * spacing;
        float sx = c.x - totalW * 0.5f, sy = c.y + rOuter + 30;
        for (size_t i = 0; i < steps_.size(); ++i) {
            Vec2 p(sx + i * spacing, sy);
            uint32_t col = i < stepIndex_ ? rgba(0.29f, 0.77f, 0.46f, 0.8f)
                          : i == stepIndex_ ? kWhite : rgba(1, 1, 1, 0.3f);
            if (isArrowKey(steps_[i].key)) ui.triangleArrow(p, 8, arrowDir(steps_[i].key), col);
            else ui.diamondOutline(p, 8, 1.4f, col);
        }
    }
    if (type_ == QteType::Mash) {
        int pips = std::min(mashTarget_, 12);
        float spacing = 12.0f;
        float sx = c.x - (pips - 1) * spacing * 0.5f, sy = c.y + rOuter + 14;
        for (int i = 0; i < pips; ++i) {
            bool on = i < mashProgress_ * pips / std::max(mashTarget_, 1);
            ui.rectFill({sx + i * spacing - 2, sy - 2, 4, 4}, on ? kAccentLo : rgba(1, 1, 1, 0.2f));
        }
    }

    return result;
}

} // namespace ae
