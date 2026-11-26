#pragma once
#include "common.h"

#include <vector>

namespace ep {

// Minimal blocking HTTP(S) GET via WinHTTP (follows redirects, honours the system proxy).
// Returns true on a 200 response; the body is limited to `maxBytes`.
// `publicOnly` is for URLs taken from third-party data: hosts that resolve to loopback/LAN/link-local
// addresses are refused, and every redirect hop is checked the same way (at most 5 hops).
bool HttpGet(const std::wstring& url, std::vector<uint8_t>& body, int timeoutMs = 6000, size_t maxBytes = 8u * 1024 * 1024,
             bool publicOnly = false);

}  // namespace ep
