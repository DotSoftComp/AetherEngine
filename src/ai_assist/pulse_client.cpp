#include "pulse_client.h"
#include "../core/http.h"
#include "../core/log.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

namespace ae {

std::string pulseJsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

// ---- config persistence -----------------------------------------------------

static std::string pulseConfigPath() {
    char appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return "";
    std::string dir = std::string(appdata) + "\\AetherEngine";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\pulse.json";
}

PulseConfig PulseConfig::load() {
    PulseConfig cfg;
    std::ifstream f(pulseConfigPath(), std::ios::binary);
    if (!f) return cfg;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    JsonValue doc;
    if (!jsonParse(text.c_str(), text.size(), doc)) return cfg;
    if (const std::string* s = doc.string("baseUrl")) cfg.baseUrl = *s;
    if (const std::string* s = doc.string("apiKey")) cfg.apiKey = *s;
    return cfg;
}

bool PulseConfig::save() const {
    std::ofstream f(pulseConfigPath(), std::ios::binary);
    if (!f) return false;
    f << "{\n  \"baseUrl\": \"" << pulseJsonEscape(baseUrl) << "\",\n  \"apiKey\": \""
      << pulseJsonEscape(apiKey) << "\"\n}\n";
    return f.good();
}

// ---- request plumbing ---------------------------------------------------------

static std::vector<HttpHeader> authHeaders(const PulseConfig& cfg) {
    return {{"x-api-key", cfg.apiKey}};
}

static bool parseBody(const HttpResponse& r, JsonValue& out, std::string* error) {
    if (r.status == 0) {
        if (error) *error = r.error;
        return false;
    }
    if (!jsonParse(r.body.c_str(), r.body.size(), out)) {
        if (error) *error = "malformed response (HTTP " + std::to_string(r.status) + ")";
        return false;
    }
    if (!r.ok()) {
        const std::string* msg = out.string("error");
        if (error)
            *error = "HTTP " + std::to_string(r.status) + (msg ? ": " + *msg : "");
        return false;
    }
    return true;
}

// ---- endpoints ------------------------------------------------------------------

bool PulseClient::online(std::string* error) {
    if (!config.configured()) {
        if (error) *error = "no API key configured";
        return false;
    }
    HttpResponse r = httpGet(config.baseUrl + "/v1/agents", authHeaders(config));
    JsonValue doc;
    return parseBody(r, doc, error);
}

std::vector<PulseAgent> PulseClient::listAgents(std::string* error) {
    std::vector<PulseAgent> out;
    HttpResponse r = httpGet(config.baseUrl + "/v1/agents", authHeaders(config));
    JsonValue doc;
    if (!parseBody(r, doc, error)) return out;
    const JsonValue* arr = doc.find("agents");
    if (!arr) arr = &doc; // some deployments return a bare array
    for (size_t i = 0; i < arr->size(); ++i) {
        const JsonValue& a = (*arr)[i];
        PulseAgent ag;
        if (const std::string* s = a.string("id")) ag.id = *s;
        if (const std::string* s = a.string("name")) ag.name = *s;
        if (const std::string* s = a.string("role")) ag.role = *s;
        if (!ag.id.empty()) out.push_back(std::move(ag));
    }
    return out;
}

std::string PulseClient::createAgent(const std::string& name, const std::string& role,
                                     const std::string& personality, const std::string& mission,
                                     const std::string& decisionStyle,
                                     const std::string& systemPrompt, std::string* error) {
    std::ostringstream b;
    b << "{\"name\":\"" << pulseJsonEscape(name) << "\",\"role\":\"" << pulseJsonEscape(role)
      << "\",\"personality\":\"" << pulseJsonEscape(personality) << "\",\"mission\":\""
      << pulseJsonEscape(mission) << "\",\"decisionStyle\":\"" << pulseJsonEscape(decisionStyle)
      << "\",\"systemPrompt\":\"" << pulseJsonEscape(systemPrompt) << "\"}";
    HttpResponse r = httpPost(config.baseUrl + "/v1/agents", b.str(), authHeaders(config));
    JsonValue doc;
    if (!parseBody(r, doc, error)) return "";
    if (const JsonValue* agent = doc.find("agent"))
        if (const std::string* id = agent->string("id")) return *id;
    if (error) *error = "no agent id in response";
    return "";
}

PulseChatResult PulseClient::chat(const std::string& agentId, const std::string& message,
                                  const ChatOptions& opt) {
    PulseChatResult res;
    std::ostringstream b;
    b << "{\"message\":\"" << pulseJsonEscape(message) << "\"";
    if (!opt.sessionId.empty()) b << ",\"sessionId\":\"" << pulseJsonEscape(opt.sessionId) << "\"";
    if (!opt.context.empty()) {
        b << ",\"context\":[";
        for (size_t i = 0; i < opt.context.size(); ++i) {
            const PulseContextBlock& c = opt.context[i];
            b << (i ? "," : "") << "{\"category\":\"" << pulseJsonEscape(c.category)
              << "\",\"label\":\"" << pulseJsonEscape(c.label) << "\",\"data\":\""
              << pulseJsonEscape(c.data) << "\",\"priority\":\"high\"}";
        }
        b << "]";
    }
    if (!opt.jsonSchema.empty())
        b << ",\"responseFormat\":{\"type\":\"json\",\"schema\":\""
          << pulseJsonEscape(opt.jsonSchema) << "\"}";
    if (opt.knowledge)
        b << ",\"knowledge\":{\"enabled\":true,\"topK\":" << opt.knowledgeTopK << "}";
    b << ",\"options\":{\"reflection\":" << (opt.reflection ? "true" : "false")
      << ",\"temperature\":" << opt.temperature << ",\"maxTokens\":" << opt.maxTokens << "}}";

    HttpResponse r = httpPost(config.baseUrl + "/v1/agents/" + agentId + "/chat", b.str(),
                              authHeaders(config));
    JsonValue doc;
    if (!parseBody(r, doc, &res.error)) return res;

    res.ok = true;
    if (const std::string* s = doc.string("content")) res.content = *s;
    if (const std::string* s = doc.string("thoughts")) res.thoughts = *s;
    if (const std::string* s = doc.string("sessionId")) res.sessionId = *s;
    if (const JsonValue* p = doc.find("contentParsed")) {
        if (p->type == JsonValue::Object || p->type == JsonValue::Array) {
            res.parsed = *p;
            res.hasParsed = true;
        }
    }
    if (!res.hasParsed && !opt.jsonSchema.empty()) {
        // Some responses return the JSON only as text - parse it ourselves.
        JsonValue p;
        if (jsonParse(res.content.c_str(), res.content.size(), p) &&
            (p.type == JsonValue::Object || p.type == JsonValue::Array)) {
            res.parsed = std::move(p);
            res.hasParsed = true;
        }
    }
    if (const JsonValue* k = doc.find("knowledgeUsed")) {
        for (size_t i = 0; i < k->size(); ++i) {
            const JsonValue& c = (*k)[i];
            const std::string* name = c.string("sourceName");
            if (!name) name = c.string("name");
            if (!name) name = c.string("source");
            res.knowledgeUsed.push_back(name ? *name : "chunk " + std::to_string(i + 1));
        }
    }
    if (const JsonValue* u = doc.find("tokenUsage"))
        res.tokensUsed = (int)u->num("total_tokens", u->num("totalTokens", 0));
    return res;
}

std::string PulseClient::ingestKnowledge(const std::string& name, const std::string& content,
                                         const std::vector<std::string>& agentIds,
                                         std::string* error) {
    std::ostringstream b;
    b << "{\"name\":\"" << pulseJsonEscape(name) << "\",\"content\":\""
      << pulseJsonEscape(content) << "\"";
    if (!agentIds.empty()) {
        b << ",\"agentIds\":[";
        for (size_t i = 0; i < agentIds.size(); ++i)
            b << (i ? "," : "") << "\"" << pulseJsonEscape(agentIds[i]) << "\"";
        b << "]";
    }
    // The docs are reference material: chunk + embed, but skip the fact/QA
    // passes (slow, and reference tables aren't "facts" to contradict).
    b << ",\"extractFacts\":false,\"generateQa\":false}";
    HttpResponse r = httpPost(config.baseUrl + "/v1/knowledge-sources", b.str(),
                              authHeaders(config));
    JsonValue doc;
    if (!parseBody(r, doc, error)) return "";
    if (const std::string* id = doc.string("id")) return *id;
    if (error) *error = "no knowledge-source id in response";
    return "";
}

PulseChatResult PulseClient::complete(const std::string& systemPrompt,
                                      const std::string& userMessage, bool json,
                                      int maxTokens) {
    PulseChatResult res;
    std::ostringstream b;
    b << "{\"systemPrompt\":\"" << pulseJsonEscape(systemPrompt)
      << "\",\"messages\":[{\"role\":\"user\",\"content\":\"" << pulseJsonEscape(userMessage)
      << "\"}],\"json\":" << (json ? "true" : "false") << ",\"max_tokens\":" << maxTokens << "}";
    HttpResponse r = httpPost(config.baseUrl + "/v1/ai/complete", b.str(), authHeaders(config));
    JsonValue doc;
    if (!parseBody(r, doc, &res.error)) return res;
    res.ok = true;
    if (const std::string* s = doc.string("content")) res.content = *s;
    if (json) {
        JsonValue p;
        if (jsonParse(res.content.c_str(), res.content.size(), p) &&
            (p.type == JsonValue::Object || p.type == JsonValue::Array)) {
            res.parsed = std::move(p);
            res.hasParsed = true;
        }
    }
    return res;
}

} // namespace ae
