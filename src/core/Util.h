#pragma once
// Small platform helpers shared by core and UI: strings, files, paths, time, formatting.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lumen {

// --- Strings ---------------------------------------------------------------
std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);
std::wstring ToUpper(std::wstring_view s);
bool StartsWith(std::wstring_view s, std::wstring_view prefix);
bool ContainsInsensitive(std::wstring_view haystack, std::wstring_view needle);  // ignores case and accents
int CompareNatural(std::wstring_view a, std::wstring_view b);                    // locale-aware, digits as numbers
std::wstring Trim(std::wstring_view s);
std::wstring StripHtml(const std::string& html);  // tags removed, entities decoded, whitespace collapsed
uint64_t Hash64(std::string_view s);
std::string HexId(uint64_t v);

// --- Files & paths ---------------------------------------------------------
bool ReadFileBytes(const std::wstring& path, std::string& out);
bool WriteFileAtomic(const std::wstring& path, const std::string& data);
bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool EnsureDir(const std::wstring& path);
int64_t FileModifiedUnix(const std::wstring& path);  // 0 if missing
int64_t FileCreatedUnix(const std::wstring& path);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring ParentDir(const std::wstring& path);
std::wstring FileStem(const std::wstring& path);
std::wstring FileExt(const std::wstring& path);  // with dot, lower case
std::wstring NormalizeSlashes(std::wstring p);
std::vector<std::wstring> ListFiles(const std::wstring& dir, const std::wstring& pattern);
std::vector<std::wstring> ListDirs(const std::wstring& dir);

std::wstring AppDataDir();                        // %LOCALAPPDATA%\Lumen
int64_t OwnBuildTime();                           // modification time of this executable (identifies the build)
std::wstring AppSubDir(const wchar_t* name);      // created on demand
std::wstring KnownEnv(const wchar_t* name);

std::wstring RegReadString(void* rootKey, const wchar_t* subKey, const wchar_t* value);
uint32_t RegReadDword(void* rootKey, const wchar_t* subKey, const wchar_t* value, uint32_t def = 0);
std::vector<std::wstring> RegSubKeys(void* rootKey, const wchar_t* subKey);

// --- Time & formatting (pt-BR) --------------------------------------------
int64_t NowUnix();
std::wstring FormatPlaytime(int64_t minutes);        // "1,9 h", "45 min", "—"
std::wstring FormatHoursShort(int64_t minutes);      // "412 h"
std::wstring FormatRelative(int64_t unix);           // "hoje", "ontem", "há 10 dias", "há 3 meses"
std::wstring FormatDate(int64_t unix);               // "15 de out. de 2026"
std::wstring FormatDateTime(int64_t unix);           // "15/10 · 21:04"
std::wstring FormatBytes(uint64_t bytes);            // "12,4 GB"
std::wstring FormatPercent(double frac);             // "62%"
std::wstring FormatInt(int64_t v);

// --- Secrets ---------------------------------------------------------------
std::string ProtectSecret(const std::string& plain);   // DPAPI (current user) + base64
std::string UnprotectSecret(const std::string& blob);
std::string Base64Decode(std::string_view in);

}  // namespace lumen
