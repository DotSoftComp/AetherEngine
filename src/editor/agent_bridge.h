// Aether Engine — agent bridge: PulseLABS drives a LIVE editor over localhost.
//
// A tiny HTTP server bound to 127.0.0.1 only. Every request needs the shared
// token from %APPDATA%/AetherEngine/pulse.json (x-aether-token header), so the
// bridge is reachable exclusively by PulseLABS tooling running as this user —
// it is not an open "plug any agent" port.
//
//   GET  /health   liveness probe (no auth) — lets PulseLABS find a running editor
//   POST /rpc      {"method": "entity.spawn", "params": {...}}
//                  → {"ok": true, "result": ...} | {"ok": false, "error": "..."}
//
// The socket thread never touches the engine: it parses the request, queues
// it, and blocks until the editor's main thread services the queue via pump()
// once per frame (same worker→main handoff as the script-compile log).
#pragma once
#include "../core/json.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ae {

class AgentBridge {
public:
    // Returns the JSON response body for one queued request.
    using Handler = std::function<std::string(const std::string& method, const JsonValue& params)>;

    ~AgentBridge() { stop(); }

    // Binds 127.0.0.1:port and starts the listener thread. False when the port
    // is taken (typically a second editor instance).
    bool start(int port, const std::string& token);
    void stop();
    bool running() const { return running_; }
    int port() const { return port_; }

    // Main thread, once per frame: executes every queued request.
    void pump(const Handler& handle);

private:
    struct Pending {
        std::string method;
        JsonValue params;
        std::string response;
        bool done = false;
    };

    void serverLoop();
    void handleConnection(uintptr_t socket);
    // Queues the request and blocks until pump() answers (or the bridge stops).
    std::string dispatch(const std::string& method, JsonValue params);

    std::thread thread_;
    std::atomic<bool> running_{false};
    uintptr_t listenSock_ = (uintptr_t)~0; // INVALID_SOCKET without winsock in the header
    int port_ = 0;
    std::string token_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Pending>> queue_;
};

} // namespace ae
