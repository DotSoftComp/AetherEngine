#include "http.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <cstdio>

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
    // Long timeouts are for slow generation (local models), not slow connects:
    // an unreachable host should fail in seconds regardless.
    int connectMs = timeoutMs < 15000 ? timeoutMs : 15000;
    WinHttpSetTimeouts(session, connectMs, connectMs, timeoutMs, timeoutMs);

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

bool httpDownloadToFile(const std::string& url, const std::string& destFile,
                        const std::vector<HttpHeader>& headers,
                        const std::function<bool(long long, long long)>& progress,
                        std::string* errorOut, int timeoutMs) {
    auto fail = [&](const std::string& why) {
        if (errorOut) *errorOut = why;
        return false;
    };

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{}, path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2047;
    std::wstring wurl = widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return fail("malformed URL: " + url);
    bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(L"AetherEngine/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return fail("WinHttpOpen failed");
    int connectMs = timeoutMs < 15000 ? timeoutMs : 15000;
    WinHttpSetTimeouts(session, connectMs, connectMs, timeoutMs, timeoutMs);

    HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              https ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    auto closeAll = [&] {
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
    };
    if (!req) {
        closeAll();
        return fail("connection failed: " + url);
    }

    std::wstring headerBlock;
    for (const HttpHeader& h : headers)
        headerBlock += widen(h.name) + L": " + widen(h.value) + L"\r\n";

    BOOL sent = WinHttpSendRequest(
        req, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
        headerBlock.empty() ? 0 : (DWORD)-1, nullptr, 0, 0, 0);
    if (sent) sent = WinHttpReceiveResponse(req, nullptr);
    if (!sent) {
        closeAll();
        return fail("request failed (host down or timeout): " + url);
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        closeAll();
        return fail("HTTP " + std::to_string(status) + ": " + url);
    }

    long long total = -1;
    {
        wchar_t lenBuf[32]{};
        DWORD lenSize = sizeof(lenBuf);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX))
            total = _wtoi64(lenBuf);
    }

    FILE* f = nullptr;
    if (fopen_s(&f, destFile.c_str(), "wb") != 0 || !f) {
        closeAll();
        return fail("cannot write " + destFile);
    }

    long long done = 0;
    std::string why;
    std::vector<char> buf(256 * 1024);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            why = "read failed mid-download: " + url;
            break;
        }
        if (avail == 0) break; // finished
        while (avail > 0) {
            DWORD want = avail < (DWORD)buf.size() ? avail : (DWORD)buf.size();
            DWORD got = 0;
            if (!WinHttpReadData(req, buf.data(), want, &got) || got == 0) {
                why = "read failed mid-download: " + url;
                avail = 0;
                break;
            }
            if (fwrite(buf.data(), 1, got, f) != got) {
                why = "disk write failed: " + destFile;
                avail = 0;
                break;
            }
            done += got;
            avail -= got;
            if (progress && !progress(done, total)) {
                why = "cancelled";
                avail = 0;
            }
        }
        if (!why.empty()) break;
    }
    fclose(f);
    closeAll();

    if (why.empty() && total >= 0 && done != total)
        why = "truncated download (" + std::to_string(done) + " of " + std::to_string(total) +
              " bytes)";
    if (!why.empty()) {
        DeleteFileA(destFile.c_str());
        return fail(why);
    }
    return true;
}

} // namespace ae
