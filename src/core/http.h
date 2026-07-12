// Aether Engine — minimal HTTP client (WinHTTP) for backend integrations
// (the PulseLABS AI assistant, the hub's engine auto-updater). Synchronous —
// call from worker threads, never from the render/UI thread.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace ae {

struct HttpResponse {
    int status = 0;        // 0 = transport failure (host down, timeout, ...)
    std::string body;
    std::string error;     // transport-level error text when status == 0
    bool ok() const { return status >= 200 && status < 300; }
};

struct HttpHeader {
    std::string name, value;
};

// method: "GET", "POST", "PUT", "DELETE". `url` is absolute (http/https).
// Content-Type defaults to application/json for bodies unless overridden.
// `timeoutMs` bounds send/receive; resolve/connect are capped much shorter so
// a *down* host still fails fast even with a generous generation timeout.
HttpResponse httpRequest(const std::string& method, const std::string& url,
                         const std::vector<HttpHeader>& headers = {},
                         const std::string& body = std::string(),
                         int timeoutMs = 120000);

inline HttpResponse httpGet(const std::string& url, const std::vector<HttpHeader>& h = {},
                            int timeoutMs = 120000) {
    return httpRequest("GET", url, h, std::string(), timeoutMs);
}
inline HttpResponse httpPost(const std::string& url, const std::string& body,
                             const std::vector<HttpHeader>& h = {}, int timeoutMs = 120000) {
    return httpRequest("POST", url, h, body, timeoutMs);
}

// GET `url` streaming the body to `destFile` (large downloads never sit in
// RAM). Follows redirects (GitHub release assets redirect to a CDN host).
// `progress(done, total)` is called as bytes arrive — `total` is -1 when the
// server sent no Content-Length; return false to abort (partial file is
// deleted). Returns false on any failure with `errorOut` filled in.
bool httpDownloadToFile(const std::string& url, const std::string& destFile,
                        const std::vector<HttpHeader>& headers,
                        const std::function<bool(long long done, long long total)>& progress,
                        std::string* errorOut, int timeoutMs = 15 * 60 * 1000);

} // namespace ae
