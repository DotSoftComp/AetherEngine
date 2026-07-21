// Aether Engine — scripted input playback (see demo_input.h).
#include "demo_input.h"
#include "../core/json.h"
#include "../core/log.h"
#include <fstream>

namespace ae {

namespace {
// Reads a two-element number array ("move", "look"), leaving `out` untouched
// when the key is absent or the wrong shape.
void readPair(const JsonValue* v, float out[2]) {
    if (!v || v->size() < 2) return;
    out[0] = (float)(*v)[0].number;
    out[1] = (float)(*v)[1].number;
}
} // namespace

bool DemoInput::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_ERROR("[Demo] cannot open %s", path.c_str());
        return false;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root)) {
        AE_ERROR("[Demo] malformed JSON: %s", path.c_str());
        return false;
    }
    const JsonValue* steps = root.find("steps");
    if (!steps || steps->type != JsonValue::Array) {
        AE_ERROR("[Demo] no \"steps\" array in %s", path.c_str());
        return false;
    }
    steps_.clear();
    for (size_t i = 0; i < steps->size(); ++i) {
        const JsonValue& s = (*steps)[i];
        Step st;
        st.seconds = (float)s.num("seconds", 1.0);
        readPair(s.find("move"), st.move);
        readPair(s.find("look"), st.look);
        st.fire = s.flag("fire", false);
        st.use = s.flag("use", false);
        st.jump = s.flag("jump", false);
        if (const std::string* k = s.string("keys")) st.keys = *k;
        if (const std::string* l = s.string("label")) st.label = *l;
        steps_.push_back(std::move(st));
    }
    AE_LOG("[Demo] loaded %d steps (%.1fs) from %s", (int)steps_.size(), duration(),
           path.c_str());
    return true;
}

float DemoInput::duration() const {
    float total = 0.0f;
    for (const Step& s : steps_) total += s.seconds;
    return total;
}

void DemoInput::sample(float time, float dt, Input& out) {
    out = Input{}; // released by default: past the end, the player stands still

    float t = 0.0f;
    int index = -1;
    for (size_t i = 0; i < steps_.size(); ++i) {
        if (time < t + steps_[i].seconds) { index = (int)i; break; }
        t += steps_[i].seconds;
    }
    if (index < 0) return;
    const Step& s = steps_[index];

    if (index != lastAnnounced_) {
        lastAnnounced_ = index;
        if (!s.label.empty()) AE_LOG("[Demo] %6.2fs  %s", time, s.label.c_str());
    }

    // Movement maps onto the same virtual keys the input map binds, so a demo
    // exercises the real action/axis resolution rather than bypassing it.
    if (s.move[1] > 0.1f) out.keys['W'] = true;
    if (s.move[1] < -0.1f) out.keys['S'] = true;
    if (s.move[0] > 0.1f) out.keys['D'] = true;
    if (s.move[0] < -0.1f) out.keys['A'] = true;
    if (s.jump) out.keys[0x20] = true;  // VK_SPACE
    if (s.use) out.keys['E'] = true;
    for (char c : s.keys) {
        unsigned char code = (unsigned char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        out.keys[code] = true;
    }
    out.mouseButtons[0] = s.fire;
    // Look is authored per second so a demo reads the same at any frame rate.
    out.mouseDX = s.look[0] * dt;
    out.mouseDY = s.look[1] * dt;
}

} // namespace ae
