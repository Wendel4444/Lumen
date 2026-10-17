#include "Util.h"

#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace lumen {

// --- Strings ---------------------------------------------------------------

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ToUpper(std::wstring_view s) {
    if (s.empty()) return {};
    int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, s.data(),
                          static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, s.data(),
                  static_cast<int>(s.size()), out.data(), n, nullptr, nullptr, 0);
    return out;
}

bool StartsWith(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ContainsInsensitive(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (hay.empty()) return false;
    int r = FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,
                            FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC, hay.data(),
                            static_cast<int>(hay.size()), needle.data(), static_cast<int>(needle.size()), nullptr,
                            nullptr, nullptr, 0);
    return r >= 0;
}

int CompareNatural(std::wstring_view a, std::wstring_view b) {
    int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.data(),
                            static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), nullptr, nullptr, 0);
    return r == 0 ? 0 : r - 2;
}

std::wstring Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

std::wstring StripHtml(const std::string& html) {
    std::string text;
    text.reserve(html.size());
    for (size_t i = 0; i < html.size();) {
        char c = html[i];
        if (c == '<') {
            size_t end = html.find('>', i);
            if (end == std::string::npos) break;
            std::string tag = html.substr(i + 1, std::min<size_t>(end - i - 1, 4));
            for (auto& ch : tag) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (tag.rfind("br", 0) == 0 || tag.rfind("/p", 0) == 0 || tag.rfind("/h", 0) == 0 || tag.rfind("li", 0) == 0)
                text += ' ';
            i = end + 1;
            continue;
        }
        if (c == '&') {
            size_t end = html.find(';', i);
            if (end != std::string::npos && end - i <= 10) {
                std::string ent = html.substr(i + 1, end - i - 1);
                std::wstring rep;
                if (ent == "amp") rep = L"&";
                else if (ent == "lt") rep = L"<";
                else if (ent == "gt") rep = L">";
                else if (ent == "quot") rep = L"\"";
                else if (ent == "apos" || ent == "#39") rep = L"'";
                else if (ent == "nbsp") rep = L" ";
                else if (!ent.empty() && ent[0] == '#') {
                    unsigned long cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                                           ? std::strtoul(ent.c_str() + 2, nullptr, 16)
                                           : std::strtoul(ent.c_str() + 1, nullptr, 10);
                    if (cp > 0 && cp < 0xFFFF) rep = std::wstring(1, static_cast<wchar_t>(cp));
                }
                if (!rep.empty()) {
                    text += Narrow(rep);
                    i = end + 1;
                    continue;
                }
            }
        }
        text += c;
        ++i;
    }
    // Collapse whitespace.
    std::wstring w = Widen(text), out;
    out.reserve(w.size());
    bool space = false;
    for (wchar_t ch : w) {
        if (iswspace(ch)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += L' ';
        space = false;
        out += ch;
    }
    return out;
}

uint64_t Hash64(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string HexId(uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

// --- Files -----------------------------------------------------------------

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (512ll << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = data.empty() || WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != data.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& path) {
    if (DirExists(path)) return true;
    return SHCreateDirectoryExW(nullptr, path.c_str(), nullptr) == ERROR_SUCCESS || DirExists(path);
}

static int64_t FileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000000ull) - 11644473600ll;
}

int64_t FileModifiedUnix(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return FileTimeToUnix(d.ftLastWriteTime);
}

int64_t FileCreatedUnix(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return FileTimeToUnix(d.ftCreationTime);
}

std::wstring NormalizeSlashes(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring ParentDir(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

std::wstring FileStem(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    std::wstring name = s == std::wstring::npos ? path : path.substr(s + 1);
    size_t d = name.find_last_of(L'.');
    return d == std::wstring::npos ? name : name.substr(0, d);
}

std::wstring FileExt(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/.");
    if (s == std::wstring::npos || path[s] != L'.') return {};
    std::wstring e = path.substr(s);
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    return e;
}

std::vector<std::wstring> ListFiles(const std::wstring& dir, const std::wstring& pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(JoinPath(dir, pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<std::wstring> ListDirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(JoinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories,
                                nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") != 0 &&
            wcscmp(fd.cFileName, L"..") != 0)
            out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::wstring KnownEnv(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return (n && n < ARRAYSIZE(buf)) ? std::wstring(buf, n) : std::wstring();
}

std::wstring AppDataDir() {
    static std::wstring dir = [] {
        // Portable mode: a "portable" file next to the executable keeps all data beside it.
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring exeDir = ParentDir(exe);
        if (FileExists(JoinPath(exeDir, L"portable"))) {
            std::wstring d = JoinPath(exeDir, L"LumenData");
            EnsureDir(d);
            return d;
        }
        PWSTR p = nullptr;
        std::wstring base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) base = p;
        CoTaskMemFree(p);
        std::wstring d = JoinPath(base, L"Lumen");
        EnsureDir(d);
        return d;
    }();
    return dir;
}

int64_t OwnBuildTime() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return FileModifiedUnix(exe);
}

std::wstring AppSubDir(const wchar_t* name) {
    std::wstring d = JoinPath(AppDataDir(), name);
    EnsureDir(d);
    return d;
}

std::wstring RegReadString(void* root, const wchar_t* subKey, const wchar_t* value) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(static_cast<HKEY>(root), subKey, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf,
                     &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

uint32_t RegReadDword(void* root, const wchar_t* subKey, const wchar_t* value, uint32_t def) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(static_cast<HKEY>(root), subKey, value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return def;
    return v;
}

std::vector<std::wstring> RegSubKeys(void* root, const wchar_t* subKey) {
    std::vector<std::wstring> out;
    HKEY k;
    if (RegOpenKeyExW(static_cast<HKEY>(root), subKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = ARRAYSIZE(name);
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.emplace_back(name, len);
    }
    RegCloseKey(k);
    return out;
}

// --- Time & formatting -----------------------------------------------------

int64_t NowUnix() { return static_cast<int64_t>(std::time(nullptr)); }

static std::wstring OneDecimal(double v) {
    wchar_t buf[32];
    if (v >= 100) swprintf_s(buf, L"%.0f", v);
    else swprintf_s(buf, L"%.1f", v);
    std::wstring s = buf;
    for (auto& c : s)
        if (c == L'.') c = L',';
    if (s.size() > 2 && s.compare(s.size() - 2, 2, L",0") == 0) s.resize(s.size() - 2);
    return s;
}

std::wstring FormatPlaytime(int64_t minutes) {
    if (minutes < 0) return L"—";
    if (minutes == 0) return L"0 min";
    if (minutes < 60) return std::to_wstring(minutes) + L" min";
    return OneDecimal(minutes / 60.0) + L" h";
}

std::wstring FormatHoursShort(int64_t minutes) {
    if (minutes <= 0) return L"0 h";
    if (minutes < 60) return std::to_wstring(minutes) + L" min";
    return FormatInt(minutes / 60) + L" h";
}

static int64_t LocalDayIndex(int64_t unix) {
    __time64_t t = unix;
    tm lt{};
    if (_localtime64_s(&lt, &t) != 0) return unix / 86400;
    lt.tm_hour = 12;
    lt.tm_min = lt.tm_sec = 0;
    return static_cast<int64_t>(_mktime64(&lt)) / 86400;
}

std::wstring FormatRelative(int64_t unix) {
    if (unix <= 0) return L"nunca";
    int64_t days = LocalDayIndex(NowUnix()) - LocalDayIndex(unix);
    if (days <= 0) return L"hoje";
    if (days == 1) return L"ontem";
    if (days < 7) return L"há " + std::to_wstring(days) + L" dias";
    if (days < 14) return L"há 1 semana";
    if (days < 31) return L"há " + std::to_wstring(days / 7) + L" semanas";
    if (days < 60) return L"há 1 mês";
    if (days < 365) return L"há " + std::to_wstring(days / 30) + L" meses";
    if (days < 730) return L"há 1 ano";
    return L"há " + std::to_wstring(days / 365) + L" anos";
}

std::wstring FormatDate(int64_t unix) {
    if (unix <= 0) return L"—";
    static const wchar_t* months[] = {L"jan.", L"fev.", L"mar.", L"abr.", L"mai.", L"jun.",
                                      L"jul.", L"ago.", L"set.", L"out.", L"nov.", L"dez."};
    __time64_t t = unix;
    tm lt{};
    if (_localtime64_s(&lt, &t) != 0) return L"—";
    wchar_t buf[64];
    swprintf_s(buf, L"%d de %s de %d", lt.tm_mday, months[lt.tm_mon], lt.tm_year + 1900);
    return buf;
}

std::wstring FormatDateTime(int64_t unix) {
    __time64_t t = unix;
    tm lt{};
    if (unix <= 0 || _localtime64_s(&lt, &t) != 0) return L"—";
    wchar_t buf[64];
    swprintf_s(buf, L"%02d/%02d · %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
    return buf;
}

std::wstring FormatBytes(uint64_t bytes) {
    if (bytes == 0) return L"—";
    double v = static_cast<double>(bytes);
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    int u = 0;
    while (v >= 1000 && u < 4) {
        v /= 1024;
        ++u;
    }
    return OneDecimal(v) + L" " + units[u];
}

std::wstring FormatPercent(double frac) {
    int p = static_cast<int>(std::floor(frac * 100.0 + 0.5));
    return std::to_wstring(std::clamp(p, 0, 100)) + L"%";
}

std::wstring FormatInt(int64_t v) {
    std::wstring s = std::to_wstring(v < 0 ? -v : v), out;
    int n = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (n && n % 3 == 0) out += L'.';
        out += *it;
        ++n;
    }
    if (v < 0) out += L'-';
    std::reverse(out.begin(), out.end());
    return out;
}

// --- Secrets ---------------------------------------------------------------

static std::string Base64Encode(const BYTE* data, DWORD len) {
    DWORD n = 0;
    CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string out(n, '\0');
    CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &n);
    out.resize(n);
    return out;
}

std::string Base64Decode(std::string_view in) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(in.data(), static_cast<DWORD>(in.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr,
                              nullptr))
        return {};
    std::string out(n, '\0');
    if (!CryptStringToBinaryA(in.data(), static_cast<DWORD>(in.size()), CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(out.data()), &n, nullptr, nullptr))
        return {};
    out.resize(n);
    return out;
}

std::string ProtectSecret(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Lumen", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string b64 = Base64Encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return b64;
}

std::string UnprotectSecret(const std::string& blob) {
    if (blob.empty()) return {};
    std::string raw = Base64Decode(blob);
    if (raw.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(raw.size()), reinterpret_cast<BYTE*>(raw.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

}  // namespace lumen
