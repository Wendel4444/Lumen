#include "Http.h"

#include <windows.h>
#include <winhttp.h>

namespace lumen {

namespace {
HINTERNET Session() {
    static HINTERNET s = [] {
        HINTERNET h = WinHttpOpen(L"Lumen/1.0 (Windows; game library)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h)  // Windows < 8.1
            h = WinHttpOpen(L"Lumen/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) {
            DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
        }
        return h;
    }();
    return s;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};
}  // namespace

HttpResponse HttpGet(const std::wstring& url, int timeoutMs, size_t maxBytes) {
    HttpResponse r;
    HINTERNET session = Session();
    if (!session) {
        r.error = L"WinHTTP indisponível";
        return r;
    }
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        r.error = L"URL inválida";
        return r;
    }
    Handle conn{WinHttpConnect(session, host, uc.nPort, 0)};
    if (!conn.h) {
        r.error = L"Falha de conexão";
        return r;
    }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle req{WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  flags)};
    if (!req.h) {
        r.error = L"Falha ao criar requisição";
        return r;
    }
    WinHttpSetTimeouts(req.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        r.error = L"Sem conexão com a internet";
        return r;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        size_t old = r.body.size();
        if (old + avail > maxBytes) {
            r.error = L"Resposta grande demais";
            r.status = 0;
            r.body.clear();
            return r;
        }
        r.body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, r.body.data() + old, avail, &read)) {
            r.body.resize(old);
            break;
        }
        r.body.resize(old + read);
    }
    return r;
}

std::wstring UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            out += L'%';
            out += static_cast<wchar_t>(hex[c >> 4]);
            out += static_cast<wchar_t>(hex[c & 15]);
        }
    }
    return out;
}

}  // namespace lumen
