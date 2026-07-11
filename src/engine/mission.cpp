#include "mission.h"
#include "world.h"
#include "entity.h"
#include "../core/json.h"
#include "../core/log.h"
#include <fstream>
#include <sstream>

namespace ae {

void MissionSystem::setFlag(const std::string& name, int value) {
    if (name.empty()) return;
    auto it = flags_.find(name);
    if (it != flags_.end() && it->second == value) return;
    flags_[name] = value;
    AE_LOG("[Mission] flag %s = %d", name.c_str(), value);
}

int MissionSystem::flag(const std::string& name) const {
    auto it = flags_.find(name);
    return it == flags_.end() ? 0 : it->second;
}

Mission* MissionSystem::find(const std::string& id) {
    for (auto& m : missions)
        if (m.id == id) return &m;
    return nullptr;
}

void MissionSystem::activateObjectives(Mission& m) {
    bool first = true;
    for (auto& o : m.objectives) {
        if (o.state == ObjectiveState::Complete || o.state == ObjectiveState::Failed) {
            continue;
        }
        if (!m.sequential || first) {
            o.state = ObjectiveState::Active;
            first = false;
        } else {
            o.state = ObjectiveState::Locked;
        }
    }
}

void MissionSystem::startMission(const std::string& id) {
    Mission* m = find(id);
    if (!m || m->state == MissionState::Active) return;
    m->state = MissionState::Active;
    activateObjectives(*m);
    AE_LOG("[Mission] started: %s", m->name.empty() ? m->id.c_str() : m->name.c_str());
}

void MissionSystem::resetRuntime() {
    flags_.clear();
    toasts.clear();
    for (auto& m : missions) {
        m.state = MissionState::NotStarted;
        for (auto& o : m.objectives) o.state = ObjectiveState::Locked;
    }
}

void MissionSystem::update(World& world, float dt) {
    for (auto& t : toasts) t.age += dt;
    while (!toasts.empty() && toasts.front().age > 4.0f) toasts.erase(toasts.begin());

    Entity* player = world.find(playerEntityName);

    for (auto& m : missions) {
        if (m.state == MissionState::NotStarted && m.autoStart) {
            m.state = MissionState::Active;
            activateObjectives(m);
            AE_LOG("[Mission] started: %s", m.name.empty() ? m.id.c_str() : m.name.c_str());
        }
        if (m.state != MissionState::Active) continue;

        for (auto& o : m.objectives) {
            if (o.state != ObjectiveState::Active) continue;
            bool complete = false;
            switch (o.type) {
            case ObjectiveType::Reach: {
                if (!player) break;
                Entity* target = world.find(o.targetEntity);
                if (!target) break;
                Vec3 d = player->worldPosition() - target->worldPosition();
                complete = dot(d, d) <= o.radius * o.radius;
                break;
            }
            case ObjectiveType::Flag:
                complete = flag(o.flag) == o.flagValue;
                break;
            }
            if (complete) {
                o.state = ObjectiveState::Complete;
                toasts.push_back({o.text.empty() ? o.id : o.text, 0.0f});
                AE_LOG("[Mission] objective complete: %s", o.text.c_str());
                if (m.sequential) activateObjectives(m); // unlock the next one
            }
        }

        bool allDone = true;
        for (const auto& o : m.objectives)
            if (!o.optional && o.state != ObjectiveState::Complete) { allDone = false; break; }
        if (allDone && !m.objectives.empty()) {
            m.state = MissionState::Complete;
            toasts.push_back({"Mission complete: " + (m.name.empty() ? m.id : m.name), 0.0f});
            AE_LOG("[Mission] complete: %s", m.name.empty() ? m.id.c_str() : m.name.c_str());
        }
    }
}

// ---- persistence -----------------------------------------------------------

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        default: out += c;
        }
    }
    return out;
}

bool MissionSystem::save(const std::string& path) const {
    std::ostringstream o;
    o << "{\n  \"player\": \"" << jsonEscape(playerEntityName) << "\",\n  \"missions\": [\n";
    for (size_t i = 0; i < missions.size(); ++i) {
        const Mission& m = missions[i];
        o << "    { \"id\": \"" << jsonEscape(m.id) << "\", \"name\": \"" << jsonEscape(m.name)
          << "\", \"description\": \"" << jsonEscape(m.description) << "\", \"autoStart\": "
          << (m.autoStart ? "true" : "false") << ", \"sequential\": "
          << (m.sequential ? "true" : "false") << ",\n      \"objectives\": [\n";
        for (size_t j = 0; j < m.objectives.size(); ++j) {
            const Objective& ob = m.objectives[j];
            o << "        { \"id\": \"" << jsonEscape(ob.id) << "\", \"text\": \""
              << jsonEscape(ob.text) << "\", \"type\": \""
              << (ob.type == ObjectiveType::Reach ? "reach" : "flag") << "\"";
            if (ob.type == ObjectiveType::Reach)
                o << ", \"target\": \"" << jsonEscape(ob.targetEntity) << "\", \"radius\": "
                  << ob.radius;
            else
                o << ", \"flag\": \"" << jsonEscape(ob.flag) << "\", \"value\": " << ob.flagValue;
            if (ob.optional) o << ", \"optional\": true";
            o << " }" << (j + 1 < m.objectives.size() ? "," : "") << "\n";
        }
        o << "      ] }" << (i + 1 < missions.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    return f.good();
}

bool MissionSystem::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root)) return false;

    missions.clear();
    if (const std::string* s = root.string("player")) playerEntityName = *s;
    const JsonValue* ms = root.find("missions");
    if (!ms) return true;

    for (size_t i = 0; i < ms->size(); ++i) {
        const JsonValue& mj = (*ms)[i];
        Mission m;
        if (const std::string* s = mj.string("id")) m.id = *s;
        if (const std::string* s = mj.string("name")) m.name = *s;
        if (const std::string* s = mj.string("description")) m.description = *s;
        m.autoStart = mj.flag("autoStart", false);
        m.sequential = mj.flag("sequential", true);
        if (const JsonValue* objs = mj.find("objectives")) {
            for (size_t j = 0; j < objs->size(); ++j) {
                const JsonValue& oj = (*objs)[j];
                Objective ob;
                if (const std::string* s = oj.string("id")) ob.id = *s;
                if (const std::string* s = oj.string("text")) ob.text = *s;
                if (const std::string* s = oj.string("type"))
                    ob.type = (*s == "flag") ? ObjectiveType::Flag : ObjectiveType::Reach;
                if (const std::string* s = oj.string("target")) ob.targetEntity = *s;
                ob.radius = (float)oj.num("radius", 2.5);
                if (const std::string* s = oj.string("flag")) ob.flag = *s;
                ob.flagValue = oj.integer("value", 1);
                ob.optional = oj.flag("optional", false);
                m.objectives.push_back(std::move(ob));
            }
        }
        missions.push_back(std::move(m));
    }
    AE_LOG("[Mission] loaded %d mission(s) from %s", (int)missions.size(), path.c_str());
    return true;
}

} // namespace ae
