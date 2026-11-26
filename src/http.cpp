#include "http.h"

#include <winhttp.h>

#include <cstring>

#include "log.h"

namespace ep {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// Is this resolved address somewhere a third-party URL may send us? Used for favicon links from
// radio-browser.info, which anyone can edit, so a station could point its logo at the player's router.
bool IsPublicIPv4(uint32_t a) {  // host byte order
    const uint8_t b0 = a >> 24, b1 = (a >> 16) & 0xff;
    if (b0 == 0 || b0 == 10 || b0 == 127) return false;     // "this" network, private, loopback
    if (b0 == 100 && (b1 & 0xc0) == 64) return false;       // 100.64/10 carrier-grade NAT / Tailscale
    if (b0 == 169 && b1 == 254) return false;               // link-local
    if (b0 == 172 && (b1 & 0xf0) == 16) return false;       // 172.16/12
    if (b0 == 192 && b1 == 168) return false;               // 192.168/16
    if (b0 == 192 && b1 == 0 && ((a >> 8) & 0xff) == 0) return false;  // 192.0.0/24 protocol assignments
    if (b0 == 198 && (b1 & 0xfe) == 18) return false;       // 198.18/15 benchmarking
    if (b0 >= 224) return false;                            // multicast, reserved, broadcast
    return true;
}

bool IsPublicAddress(const sockaddr* sa) {
    if (sa->sa_family == AF_INET) return IsPublicIPv4(ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr));
    if (sa->sa_family != AF_INET6) return false;
    const uint8_t* b = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr.s6_addr;
    static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(b, kMapped, 12) == 0)  // ::ffff:a.b.c.d -> judge the IPv4 inside
        return IsPublicIPv4(uint32_t(b[12]) << 24 | uint32_t(b[13]) << 16 | uint32_t(b[14]) << 8 | b[15]);
    bool zeroPrefix = true;  // :: and ::1 (and the deprecated ::a.b.c.d form)
    for (int i = 0; i < 12; ++i) zeroPrefix = zeroPrefix && b[i] == 0;
    if (zeroPrefix) return false;
    if ((b[0] & 0xfe) == 0xfc) return false;                 // fc00::/7 unique local
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return false; // fe80::/10 link-local
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return false; // fec0::/10 old site-local
    if (b[0] == 0xff) return false;                          // multicast
    if (b[0] == 0x20 && b[1] == 0x02) return false;          // 2002::/16 6to4 can wrap a private IPv4
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) return false;  // 64:ff9b::/96 NAT64
    return true;
}

// True only when every address `host` resolves to is public. Fails closed: unresolvable means no.
bool HostIsPublic(const wchar_t* host) {
    static const bool wsaReady = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!wsaReady) return false;
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    if (GetAddrInfoW(host, nullptr, &hints, &res) != 0 || !res) return false;
    bool ok = true;
    for (ADDRINFOW* a = res; a && ok; a = a->ai_next) ok = IsPublicAddress(a->ai_addr);
    FreeAddrInfoW(res);
    return ok;
}

// Single request. With `publicOnly`, WinHTTP's own redirect handling is off and a 3xx hands its
// Location back through `redirect` so the caller can vet the next hop before following it.
bool GetOnce(const std::wstring& url, std::vector<uint8_t>& body, int timeoutMs, size_t maxBytes, bool publicOnly,
             std::wstring& redirect) {
    body.clear();
    redirect.clear();

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) return false;
    if (publicOnly) {
        if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
        if (!HostIsPublic(host)) {
            Logf("HTTP refused non-public host: %ls", host);
            return false;
        }
    }

    Handle session(WinHttpOpen(L"CoachNowPlaying/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        session.h = WinHttpOpen(L"CoachNowPlaying/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.h) return false;
    }
    WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    Handle connection(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connection.h) return false;
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle request(WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.h) return false;
    if (publicOnly) {
        DWORD off = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off))) return false;
    }

    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        return false;
    }
    if (publicOnly) {
        // The host was vetted before connecting, but WinHTTP resolves it again itself, so a DNS answer that
        // changes in between (rebinding) could still land on a LAN address. Check where the request actually
        // went and drop the response if that isn't public. Behind a proxy this is the proxy's address.
        WINHTTP_CONNECTION_INFO ci{};
        ci.cbSize = sizeof(ci);
        DWORD ciSize = sizeof(ci);
        if (!WinHttpQueryOption(request.h, WINHTTP_OPTION_CONNECTION_INFO, &ci, &ciSize) ||
            !IsPublicAddress(reinterpret_cast<const sockaddr*>(&ci.RemoteAddress))) {
            Logf("HTTP dropped response from non-public peer: %ls", host);
            return false;
        }
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    if (publicOnly && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
        wchar_t loc[2048] = {};
        DWORD locSize = sizeof(loc);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &locSize,
                                WINHTTP_NO_HEADER_INDEX)) {
            std::wstring l(loc);
            std::wstring origin = std::wstring(uc.nScheme == INTERNET_SCHEME_HTTPS ? L"https://" : L"http://") + host +
                                  L":" + std::to_wstring(uc.nPort);
            if (l.rfind(L"http://", 0) == 0 || l.rfind(L"https://", 0) == 0) redirect = l;
            else if (l.rfind(L"//", 0) == 0) redirect = (uc.nScheme == INTERNET_SCHEME_HTTPS ? L"https:" : L"http:") + l;
            else if (l.rfind(L"/", 0) == 0) redirect = origin + l;
        }
        return false;
    }
    if (status != 200) return false;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return false;
        if (available == 0) break;
        if (body.size() + available > maxBytes) return false;
        size_t old = body.size();
        body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, body.data() + old, available, &read)) return false;
        body.resize(old + read);
    }
    return true;
}

}  // namespace

bool HttpGet(const std::wstring& url, std::vector<uint8_t>& body, int timeoutMs, size_t maxBytes, bool publicOnly) {
    std::wstring next = url, redirect;
    for (int hop = 0; hop < 5; ++hop) {
        if (GetOnce(next, body, timeoutMs, maxBytes, publicOnly, redirect)) return true;
        if (redirect.empty()) return false;
        next = redirect;
    }
    return false;
}

}  // namespace ep
