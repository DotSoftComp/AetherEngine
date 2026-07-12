// Minimal single-connection HTTP/1.1 loop (Winsock). One request per
// connection, Connection: close — PulseLABS calls are sequential and local,
// so there is nothing to win from keep-alive or parallel accepts.
#include "agent_bridge.h"
#include "../ai_assist/pulse_client.h" // pulseJsonEscape
#include "../core/log.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

namespace ae {

namespace {

constexpr size_t kMaxBody = 8 * 1024 * 1024; // 8 MB — a whole scene fits many times over

std::string httpResponse(int code, const char* codeText, const std::string& body) {
    std::string r = "HTTP/1.1 " + std::to_string(code) + " " + codeText + "\r\n";
    r += "Content-Type: application/json\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    // CORS so the PulseLABS web panel can call the bridge from the browser.
    r += "Access-Control-Allow-Origin: *\r\n";
    r += "Access-Control-Allow-Headers: content-type, x-aether-token\r\n";
    r += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    r += "Connection: close\r\n\r\n";
    r += body;
    return r;
}

std::string errorBody(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + pulseJsonEscape(msg) + "\"}";
}

void sendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, (int)std::min<size_t>(data.size() - sent, 1 << 16), 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

// Case-insensitive header lookup in the raw header block ("name: value\r\n").
std::string headerValue(const std::string& head, const std::string& name) {
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    std::string key = "\r\n" + name + ":";
    size_t at = lower.find(key);
    if (at == std::string::npos) return "";
    size_t v = at + key.size();
    size_t end = lower.find("\r\n", v);
    std::string value = head.substr(v, end == std::string::npos ? std::string::npos : end - v);
    size_t a = value.find_first_not_of(" \t");
    size_t b = value.find_last_not_of(" \t");
    return a == std::string::npos ? "" : value.substr(a, b - a + 1);
}

} // namespace

bool AgentBridge::start(int port, const std::string& token) {
    if (running_) return true;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    // Loopback only — the bridge must never be reachable from the network.
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(s, 4) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }

    listenSock_ = (uintptr_t)s;
    port_ = port;
    token_ = token;
    running_ = true;
    thread_ = std::thread([this]() { serverLoop(); });
    return true;
}

void AgentBridge::stop() {
    if (!running_) return;
    running_ = false;
    closesocket((SOCKET)listenSock_); // unblocks accept()
    cv_.notify_all();                 // unblocks any dispatch() still waiting
    if (thread_.joinable()) thread_.join();
    listenSock_ = (uintptr_t)INVALID_SOCKET;
}

void AgentBridge::serverLoop() {
    while (running_) {
        SOCKET c = accept((SOCKET)listenSock_, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        DWORD timeoutMs = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
        handleConnection((uintptr_t)c);
        closesocket(c);
    }
}

void AgentBridge::handleConnection(uintptr_t socket) {
    SOCKET c = (SOCKET)socket;

    // Read until the header terminator, then Content-Length more bytes.
    std::string raw;
    size_t headerEnd = std::string::npos;
    char buf[8192];
    while (headerEnd == std::string::npos) {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) return;
        raw.append(buf, (size_t)n);
        headerEnd = raw.find("\r\n\r\n");
        if (raw.size() > kMaxBody) return;
    }
    std::string head = raw.substr(0, headerEnd + 2); // keep a trailing \r\n for headerValue
    size_t contentLength = (size_t)std::strtoul(headerValue(head, "content-length").c_str(),
                                                nullptr, 10);
    if (contentLength > kMaxBody) {
        sendAll(c, httpResponse(413, "Payload Too Large", errorBody("body too large")));
        return;
    }
    std::string body = raw.substr(headerEnd + 4);
    while (body.size() < contentLength) {
        int n = recv(c, buf, (int)std::min(sizeof(buf), contentLength - body.size()), 0);
        if (n <= 0) return;
        body.append(buf, (size_t)n);
    }

    size_t sp1 = head.find(' ');
    size_t sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    std::string verb = head.substr(0, sp1);
    std::string path = head.substr(sp1 + 1, sp2 - sp1 - 1);

    if (verb == "OPTIONS") { // CORS preflight for the x-aether-token header
        sendAll(c, httpResponse(204, "No Content", ""));
        return;
    }
    if (verb == "GET" && path == "/health") {
        sendAll(c, httpResponse(200, "OK",
                                "{\"ok\":true,\"service\":\"aether-agent-bridge\"}"));
        return;
    }
    if (verb != "POST" || path != "/rpc") {
        sendAll(c, httpResponse(404, "Not Found", errorBody("POST /rpc or GET /health")));
        return;
    }
    if (headerValue(head, "x-aether-token") != token_) {
        sendAll(c, httpResponse(401, "Unauthorized",
                                errorBody("bad or missing x-aether-token (see "
                                          "%APPDATA%/AetherEngine/pulse.json)")));
        return;
    }

    JsonValue req;
    if (!jsonParse(body.c_str(), body.size(), req) || req.type != JsonValue::Object) {
        sendAll(c, httpResponse(400, "Bad Request", errorBody("malformed JSON body")));
        return;
    }
    const std::string* method = req.string("method");
    if (!method || method->empty()) {
        sendAll(c, httpResponse(400, "Bad Request", errorBody("missing \"method\"")));
        return;
    }
    JsonValue params;
    if (const JsonValue* p = req.find("params")) params = *p;

    sendAll(c, httpResponse(200, "OK", dispatch(*method, std::move(params))));
}

std::string AgentBridge::dispatch(const std::string& method, JsonValue params) {
    auto pending = std::make_shared<Pending>();
    pending->method = method;
    pending->params = std::move(params);

    std::unique_lock<std::mutex> lock(mutex_);
    queue_.push_back(pending);
    // The editor services the queue once per frame; a long stall means a modal
    // dialog or a hung main thread — report it instead of hanging the client.
    bool answered = cv_.wait_for(lock, std::chrono::seconds(20), [&]() {
        return pending->done || !running_;
    });
    if (!answered || !pending->done)
        return errorBody(running_ ? "editor did not service the request in 20s (modal dialog?)"
                                  : "bridge shutting down");
    return pending->response;
}

void AgentBridge::pump(const Handler& handle) {
    std::vector<std::shared_ptr<Pending>> work;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        work.swap(queue_);
    }
    for (auto& p : work) {
        std::string response = handle(p->method, p->params);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            p->response = std::move(response);
            p->done = true;
        }
        cv_.notify_all();
    }
}

} // namespace ae
