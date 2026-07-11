// Aether Engine — minimal HTTP client (WinHTTP) for backend integrations
// (the PulseLABS AI assistant). Synchronous — call from worker threads, never
// from the render/UI thread.
#pragma once
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
HttpResponse httpRequest(const std::string& method, const std::string& url,
                         const std::vector<HttpHeader>& headers = {},
                         const std::string& body = std::string(),
                         int timeoutMs = 120000);

inline HttpResponse httpGet(const std::string& url, const std::vector<HttpHeader>& h = {}) {
    return httpRequest("GET", url, h);
}
inline HttpResponse httpPost(const std::string& url, const std::string& body,
                             const std::vector<HttpHeader>& h = {}) {
    return httpRequest("POST", url, h, body);
}

} // namespace ae
