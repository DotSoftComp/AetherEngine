#include "dialogue_scene.h"
#include "../core/json.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ae {

namespace {

struct KeyName { const char* name; int vk; };
const KeyName kKeyNames[] = {
    {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"SHIFT", VK_SHIFT},
    {"CTRL", VK_CONTROL}, {"TAB", VK_TAB},
    {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
};

int parseKeyName(const std::string& s) {
    for (auto& k : kKeyNames)
        if (s == k.name) return k.vk;
    if (s.size() == 1) return std::toupper((unsigned char)s[0]);
    return 0;
}

std::string keyName(int vk) {
    for (auto& k : kKeyNames)
        if (k.vk == vk) return k.name;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    return "?";
}

std::string jsonEscape(const std::string& s) {
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

const char* nodeTypeName(NodeType t) {
    switch (t) {
    case NodeType::Line: return "line";
    case NodeType::Choice: return "choice";
    case NodeType::Qte: return "qte";
    default: return "end";
    }
}
NodeType parseNodeType(const std::string& s) {
    if (s == "line") return NodeType::Line;
    if (s == "choice") return NodeType::Choice;
    if (s == "qte") return NodeType::Qte;
    return NodeType::End;
}

const char* qteTypeName(QteType t) {
    switch (t) {
    case QteType::Tap: return "tap";
    case QteType::Hold: return "hold";
    case QteType::Mash: return "mash";
    default: return "sequence";
    }
}
QteType parseQteType(const std::string& s) {
    if (s == "hold") return QteType::Hold;
    if (s == "mash") return QteType::Mash;
    if (s == "sequence") return QteType::Sequence;
    return QteType::Tap;
}

} // namespace

const DialogueNode* DialogueScene::find(const std::string& id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

bool saveDialogueScene(const DialogueScene& scene, const std::string& path) {
    std::ostringstream o;
    o << "{\n  \"name\": \"" << jsonEscape(scene.name) << "\",\n";
    o << "  \"start\": \"" << jsonEscape(scene.startNode) << "\",\n";
    o << "  \"nodes\": [\n";
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        const DialogueNode& n = scene.nodes[i];
        o << "    { \"id\": \"" << jsonEscape(n.id) << "\", \"type\": \"" << nodeTypeName(n.type) << "\"";
        if (!n.cameraName.empty())
            o << ", \"camera\": \"" << jsonEscape(n.cameraName) << "\", \"cameraBlend\": " << n.cameraBlend;
        if (!n.setFlag.empty())
            o << ", \"setFlag\": \"" << jsonEscape(n.setFlag) << "\", \"flagValue\": " << n.setFlagValue;
        if (n.edX != 0.0f || n.edY != 0.0f)
            o << ", \"pos\": [" << n.edX << ", " << n.edY << "]";
        switch (n.type) {
        case NodeType::Line:
            o << ", \"speaker\": \"" << jsonEscape(n.speaker) << "\""
              << ", \"text\": \"" << jsonEscape(n.text) << "\""
              << ", \"duration\": " << n.duration
              << ", \"next\": \"" << jsonEscape(n.next) << "\"";
            break;
        case NodeType::Choice:
            o << ", \"timeLimit\": " << n.timeLimit;
            if (!n.timeoutTarget.empty())
                o << ", \"timeoutTarget\": \"" << jsonEscape(n.timeoutTarget) << "\"";
            o << ", \"options\": [";
            for (size_t j = 0; j < n.options.size(); ++j) {
                if (j) o << ", ";
                o << "{ \"text\": \"" << jsonEscape(n.options[j].text) << "\", \"target\": \""
                  << jsonEscape(n.options[j].target) << "\" }";
            }
            o << "]";
            break;
        case NodeType::Qte:
            o << ", \"qteType\": \"" << qteTypeName(n.qteType) << "\", \"keys\": [";
            for (size_t j = 0; j < n.qteKeys.size(); ++j) {
                if (j) o << ", ";
                o << "\"" << keyName(n.qteKeys[j]) << "\"";
            }
            o << "]"
              << ", \"duration\": " << n.qteDuration
              << ", \"holdSeconds\": " << n.qteHoldSeconds
              << ", \"mashCount\": " << n.qteMashCount
              << ", \"success\": \"" << jsonEscape(n.successTarget) << "\""
              << ", \"fail\": \"" << jsonEscape(n.failTarget) << "\"";
            break;
        case NodeType::End:
            break;
        }
        o << " }";
        if (i + 1 < scene.nodes.size()) o << ",";
        o << "\n";
    }
    o << "  ]\n}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    return f.good();
}

bool loadDialogueScene(DialogueScene& scene, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(text.data(), (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root)) return false;

    DialogueScene out;
    if (const std::string* s = root.string("name")) out.name = *s;
    if (const std::string* s = root.string("start")) out.startNode = *s;

    const JsonValue* nodesJson = root.find("nodes");
    if (!nodesJson) return false;

    for (size_t i = 0; i < nodesJson->size(); ++i) {
        const JsonValue& nj = (*nodesJson)[i];
        DialogueNode n;
        if (const std::string* s = nj.string("id")) n.id = *s;
        if (const std::string* s = nj.string("type")) n.type = parseNodeType(*s);
        if (const std::string* s = nj.string("camera")) n.cameraName = *s;
        n.cameraBlend = (float)nj.num("cameraBlend", 0.4);
        if (const std::string* s = nj.string("setFlag")) n.setFlag = *s;
        n.setFlagValue = nj.integer("flagValue", 1);
        if (const JsonValue* pos = nj.find("pos")) {
            if (pos->size() >= 2) {
                n.edX = (float)(*pos)[0].number;
                n.edY = (float)(*pos)[1].number;
            }
        }

        switch (n.type) {
        case NodeType::Line:
            if (const std::string* s = nj.string("speaker")) n.speaker = *s;
            if (const std::string* s = nj.string("text")) n.text = *s;
            n.duration = (float)nj.num("duration", 2.0);
            if (const std::string* s = nj.string("next")) n.next = *s;
            break;
        case NodeType::Choice:
            n.timeLimit = (float)nj.num("timeLimit", 0.0);
            if (const std::string* s = nj.string("timeoutTarget")) n.timeoutTarget = *s;
            if (const JsonValue* opts = nj.find("options")) {
                for (size_t j = 0; j < opts->size(); ++j) {
                    const JsonValue& oj = (*opts)[j];
                    DialogueOption opt;
                    if (const std::string* s = oj.string("text")) opt.text = *s;
                    if (const std::string* s = oj.string("target")) opt.target = *s;
                    n.options.push_back(std::move(opt));
                }
            }
            break;
        case NodeType::Qte:
            if (const std::string* s = nj.string("qteType")) n.qteType = parseQteType(*s);
            if (const JsonValue* keys = nj.find("keys")) {
                for (size_t j = 0; j < keys->size(); ++j)
                    if ((*keys)[j].type == JsonValue::String)
                        n.qteKeys.push_back(parseKeyName((*keys)[j].str));
            }
            n.qteDuration = (float)nj.num("duration", 1.4);
            n.qteHoldSeconds = (float)nj.num("holdSeconds", 0.6);
            n.qteMashCount = nj.integer("mashCount", 8);
            if (const std::string* s = nj.string("success")) n.successTarget = *s;
            if (const std::string* s = nj.string("fail")) n.failTarget = *s;
            break;
        case NodeType::End:
            break;
        }
        out.nodes.push_back(std::move(n));
    }

    scene = std::move(out);
    return true;
}

} // namespace ae
