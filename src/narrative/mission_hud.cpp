#include "mission_hud.h"
#include <string>

namespace ae {

void drawMissionHUD(UI& ui, const MissionSystem& missions, const Rect& area) {
    const uint32_t cAccent = rgba(0.55f, 0.45f, 0.96f);
    const uint32_t cText = rgba(0.92f, 0.94f, 0.97f);
    const uint32_t cDim = rgba(0.55f, 0.58f, 0.64f);
    const uint32_t cDone = rgba(0.32f, 0.78f, 0.48f);

    float x = area.x + 18.0f;
    float y = area.y + 16.0f;

    for (const auto& m : missions.missions) {
        if (m.state != MissionState::Active) continue;

        std::string title = m.name.empty() ? m.id : m.name;
        float w = ui.measureText(title.c_str(), 1.5f) + 26.0f;
        ui.rectFill({x - 6, y - 3, w, 24}, rgba(0, 0, 0, 0.55f));
        ui.rectFill({x - 6, y - 3, 3, 24}, cAccent);
        ui.text(x + 4, y, title.c_str(), cText, 1.5f);
        y += 28.0f;

        for (const auto& o : m.objectives) {
            if (o.state == ObjectiveState::Locked) continue;
            bool done = o.state == ObjectiveState::Complete;
            uint32_t col = done ? cDone : (o.state == ObjectiveState::Failed ? rgba(0.85f, 0.3f, 0.3f) : cText);
            if (done)
                ui.diamond(Vec2(x + 6, y + 8), 5.0f, cDone);
            else
                ui.diamondOutline(Vec2(x + 6, y + 8), 5.0f, 1.5f, cAccent);
            ui.text(x + 18, y, o.text.empty() ? o.id.c_str() : o.text.c_str(),
                    done ? cDim : col);
            y += 20.0f;
        }
        y += 10.0f;
    }

    // Completion toasts, centered near the top, fading out.
    float ty = area.y + area.h * 0.12f;
    for (const auto& t : missions.toasts) {
        float a = t.age < 3.2f ? 1.0f : clampf(1.0f - (t.age - 3.2f) / 0.8f, 0.0f, 1.0f);
        float in = clampf(t.age / 0.25f, 0.0f, 1.0f); // slide in
        float w = ui.measureText(t.text.c_str(), 1.2f) + 44.0f;
        Rect r{area.x + area.w * 0.5f - w * 0.5f, ty - 14.0f * (1.0f - in), w, 26};
        ui.rectFill(r, rgba(0, 0, 0, 0.6f * a * in));
        ui.diamond(Vec2(r.x + 14, r.y + r.h * 0.5f), 5.0f, rgba(0.32f, 0.78f, 0.48f, a * in));
        ui.text(r.x + 26, r.y + 5, t.text.c_str(), rgba(1, 1, 1, a * in), 1.2f);
        ty += 32.0f;
    }
}

} // namespace ae
