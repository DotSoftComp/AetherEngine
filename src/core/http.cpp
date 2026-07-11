#include "http.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace ae {

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

HttpResponse httpRequest(const std::string& method, const std::string& url,
                         const std::vector<HttpHeader>& headers, const std::string& body,
                         int timeoutMs) {
    HttpResponse res;

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{}, path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2047;
    std::wstring wurl = widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        res.error = "malformed URL: " + url;
        return res;
    }
    bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(L"AetherEngine/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        res.error = "WinHttpOpen failed";
        return res;
    }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, widen(method).c_str(), path, nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              https ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    if (!req) {
        res.error = "connection failed: " + url;
        if (conn) WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return res;
    }

    std::wstring headerBlock;
    bool hasContentType = false;
    for (const HttpHeader& h : headers) {
        headerBlock += widen(h.name) + L": " + widen(h.value) + L"\r\n";
        if (_stricmp(h.name.c_str(), "Content-Type") == 0) hasContentType = true;
    }
    if (!body.empty() && !hasContentType)
        headerBlock += L"Content-Type: application/json\r\n";

    BOOL sent = WinHttpSendRequest(
        req, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
        headerBlock.empty() ? 0 : (DWORD)-1, body.empty() ? nullptr : (LPVOID)body.data(),
        (DWORD)body.size(), (DWORD)body.size(), 0);
    if (sent) sent = WinHttpReceiveResponse(req, nullptr);
    if (!sent) {
        res.error = "request failed (host down or timeout): " + url;
    } else {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        res.status = (int)status;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            size_t base = res.body.size();
            res.body.resize(base + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, &res.body[base], avail, &got)) break;
            res.body.resize(base + got);
            if (got == 0) break;
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return res;
}

} // namespace ae
