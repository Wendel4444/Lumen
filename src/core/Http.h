#pragma once
#include <string>

namespace lumen {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::wstring error;
    bool ok() const { return status >= 200 && status < 300; }
};

// Blocking HTTPS GET (WinHTTP). Call only from worker threads.
HttpResponse HttpGet(const std::wstring& url, int timeoutMs = 10000, size_t maxBytes = 32u << 20);

std::wstring UrlEncode(const std::string& s);

}  // namespace lumen
