#include "Steam.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <set>

#include "../Json.h"
#include "../Util.h"
#include "../Vdf.h"

namespace lumen {

namespace {

constexpr uint64_t kSteamId64Base = 76561197960265728ull;

bool IsTool(const std::string& appId, const std::wstring& name) {
    static const std::set<std::string> kTools = {
        "228980",  "1070560", "1391110", "1628350", "1493710", "2180100", "1887720", "1580130", "961940",
        "1113280", "1245040", "1420170", "2348590", "2805730", "3658110", "250820"};
    if (kTools.count(appId)) return true;
    static const wchar_t* kPatterns[] = {L"Proton ", L"Steam Linux Runtime", L"Steamworks Common", L"Redistributable"};
    for (auto* p : kPatterns)
        if (name.find(p) != std::wstring::npos) return true;
    return false;
}

struct ArtSet {
    std::wstring cover, hero, logo, header;
};

// Steam keeps art in appcache/librarycache/<appid>/ (sometimes in hashed sub-folders),
// older clients used flat "<appid>_library_600x900.jpg" names.
ArtSet FindArt(const std::wstring& cacheRoot, const std::string& appId) {
    ArtSet a;
    std::wstring dir = JoinPath(cacheRoot, Widen(appId));
    std::vector<std::wstring> dirs = {dir};
    for (auto& sub : ListDirs(dir)) dirs.push_back(JoinPath(dir, sub));

    auto pick = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        for (const wchar_t* n : names)
            for (auto& d : dirs) {
                std::wstring p = JoinPath(d, n);
                if (FileExists(p)) return p;
            }
        return {};
    };
    a.cover = pick({L"library_600x900_brazilian.jpg", L"library_600x900.jpg", L"library_capsule_brazilian.jpg",
                    L"library_capsule.jpg"});
    a.hero = pick({L"library_hero.jpg"});
    a.logo = pick({L"logo_brazilian.png", L"logo.png"});
    a.header = pick({L"header_brazilian.jpg", L"header.jpg", L"library_header_brazilian.jpg", L"library_header.jpg"});

    auto flat = [&](const wchar_t* suffix) {
        std::wstring p = JoinPath(cacheRoot, Widen(appId) + suffix);
        return FileExists(p) ? p : std::wstring();
    };
    if (a.cover.empty()) a.cover = flat(L"_library_600x900.jpg");
    if (a.hero.empty()) a.hero = flat(L"_library_hero.jpg");
    if (a.logo.empty()) a.logo = flat(L"_logo.png");
    if (a.header.empty()) a.header = flat(L"_header.jpg");
    return a;
}

std::wstring CdnUrl(const std::string& appId, const char* file) {
    return Widen("https://cdn.cloudflare.steamstatic.com/steam/apps/" + appId + "/" + file);
}

struct LocalStats {
    int64_t playMinutes = -1;
    int64_t lastPlayed = 0;
};

std::map<std::string, LocalStats> ReadLocalConfig(const std::wstring& userDir, std::wstring* persona) {
    std::map<std::string, LocalStats> out;
    std::string text;
    if (userDir.empty() || !ReadFileBytes(JoinPath(userDir, L"config\\localconfig.vdf"), text)) return out;
    vdf::Node root;
    if (!vdf::Parse(text, root)) return out;
    const vdf::Node* store = root.get("UserLocalConfigStore");
    if (!store) return out;
    if (persona) {
        if (const vdf::Node* friends = store->get("friends")) *persona = Widen(friends->str("PersonaName"));
    }
    const vdf::Node* apps = store->path({"Software", "Valve", "Steam", "apps"});
    if (!apps) return out;
    for (size_t i = 0; i < apps->keys.size(); ++i) {
        const vdf::Node& a = apps->children[i];
        LocalStats s;
        if (a.get("Playtime")) s.playMinutes = a.num("Playtime");
        s.lastPlayed = a.num("LastPlayed");
        out[apps->keys[i]] = s;
    }
    return out;
}

// Steam library collections (incl. favorites) synced to cloud storage.
void ReadCollections(const std::wstring& userDir, std::map<std::string, std::vector<std::wstring>>& byApp,
                     std::set<std::string>& favorites) {
    std::string text;
    if (!ReadFileBytes(JoinPath(userDir, L"config\\cloudstorage\\cloud-storage-namespace-1.json"), text)) return;
    json::Value root;
    if (!json::Parse(text, root) || !root.isArray()) return;
    for (const auto& entry : root.items()) {
        const std::string& key = entry[0].asString();
        if (key.rfind("user-collections.", 0) != 0) continue;
        const json::Value& meta = entry[1];
        if (meta["is_deleted"].asBool()) continue;
        json::Value col;
        if (!json::Parse(meta["value"].asString(), col)) continue;
        std::string id = col["id"].str();
        const json::Value& added = col["added"];
        if (!added.isArray()) continue;  // dynamic collections have filters, not explicit members
        std::wstring name = Widen(col["name"].str());
        for (const auto& a : added.items()) {
            std::string app = std::to_string(a.asInt());
            if (id == "favorite") favorites.insert(app);
            else if (id != "hidden" && !name.empty()) byApp[app].push_back(name);
        }
    }
}

void ReadClientCache(const std::wstring& file, Game& g) {
    std::string text;
    if (!ReadFileBytes(file, text)) return;
    json::Value root;
    if (!json::Parse(text, root) || !root.isArray()) return;
    for (const auto& entry : root.items()) {
        const std::string& key = entry[0].asString();
        const json::Value& data = entry[1]["data"];
        if (key == "achievements") {
            if (data["nTotal"].isNumber()) {
                g.achTotal = static_cast<int>(data["nTotal"].asInt());
                g.achUnlocked = static_cast<int>(data["nAchieved"].asInt());
            }
        } else if (key == "descriptions") {
            std::string snip = data["strSnippet"].str();
            if (snip.empty()) snip = data["strFullDescription"].str();
            if (!snip.empty()) g.description = StripHtml(snip);
        } else if (key == "associations") {
            auto join = [](const json::Value& arr) {
                std::wstring s;
                for (const auto& d : arr.items()) {
                    std::wstring n = Widen(d["strName"].str());
                    if (n.empty()) continue;
                    if (!s.empty()) s += L", ";
                    s += n;
                }
                return s;
            };
            g.developer = join(data["rgDevelopers"]);
            g.publisher = join(data["rgPublishers"]);
        }
    }
}

// --- appinfo.vdf (binary) -----------------------------------------------------------------
// Only used to read common/type ("Game", "Application", "Tool"...) of installed apps, so software
// such as OBS or Blender can be separated from games without any network access.

bool ParseBinaryKv(const uint8_t*& p, const uint8_t* end, vdf::Node& node, const std::vector<std::string>& strings,
                   bool indexedKeys, int depth) {
    if (depth > 32) return false;
    auto cstr = [&](std::string& out) {
        const uint8_t* s = p;
        while (p < end && *p) ++p;
        if (p >= end) return false;
        out.assign(reinterpret_cast<const char*>(s), p - s);
        ++p;
        return true;
    };
    while (p < end) {
        uint8_t type = *p++;
        if (type == 0x08 || type == 0x0B) return true;
        std::string key;
        if (indexedKeys) {
            if (end - p < 4) return false;
            uint32_t idx;
            memcpy(&idx, p, 4);
            p += 4;
            if (idx < strings.size()) key = strings[idx];
        } else if (!cstr(key)) {
            return false;
        }
        vdf::Node child;
        switch (type) {
            case 0x00:
                child.isObject = true;
                if (!ParseBinaryKv(p, end, child, strings, indexedKeys, depth + 1)) return false;
                break;
            case 0x01:
                if (!cstr(child.value)) return false;
                break;
            case 0x02: {
                if (end - p < 4) return false;
                int32_t v;
                memcpy(&v, p, 4);
                p += 4;
                child.value = std::to_string(v);
                break;
            }
            case 0x03: case 0x04: case 0x06:
                if (end - p < 4) return false;
                p += 4;
                break;
            case 0x07: case 0x0A:
                if (end - p < 8) return false;
                p += 8;
                break;
            default: return false;  // unknown / wide strings: give up on this entry
        }
        node.keys.push_back(std::move(key));
        node.children.push_back(std::move(child));
    }
    return true;
}

std::map<std::string, std::string> ReadAppTypes(const std::wstring& steamPath, const std::set<std::string>& wanted) {
    std::map<std::string, std::string> out;
    std::string data;
    if (wanted.empty() || !ReadFileBytes(JoinPath(steamPath, L"appcache\\appinfo.vdf"), data) || data.size() < 16)
        return out;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(data.data());
    size_t size = data.size();
    auto u32 = [&](size_t off) {
        uint32_t v = 0;
        if (off + 4 <= size) memcpy(&v, base + off, 4);
        return v;
    };
    uint32_t magic = u32(0);
    int version = magic == 0x07564429 ? 29 : magic == 0x07564428 ? 28 : magic == 0x07564427 ? 27 : 0;
    if (!version) return out;
    size_t pos = 8;
    std::vector<std::string> strings;
    if (version >= 29) {
        int64_t tableOff = 0;
        memcpy(&tableOff, base + 8, 8);
        pos = 16;
        if (tableOff <= 0 || static_cast<size_t>(tableOff) + 4 > size) return out;
        uint32_t count = u32(static_cast<size_t>(tableOff));
        const char* s = reinterpret_cast<const char*>(base) + tableOff + 4;
        const char* e = reinterpret_cast<const char*>(base) + size;
        strings.reserve(count);
        for (uint32_t i = 0; i < count && s < e; ++i) {
            size_t len = strnlen(s, e - s);
            strings.emplace_back(s, len);
            s += len + 1;
        }
    }
    size_t header = 4 + 4 + 8 + 20 + 4 + (version >= 28 ? 20 : 0);
    while (pos + 8 <= size) {
        uint32_t appid = u32(pos);
        if (appid == 0) break;
        size_t entryEnd = pos + 8 + u32(pos + 4);
        if (entryEnd > size) break;
        std::string id = std::to_string(appid);
        if (wanted.count(id)) {
            const uint8_t* p = base + pos + 8 + header;
            vdf::Node root;
            root.isObject = true;
            if (ParseBinaryKv(p, base + entryEnd, root, strings, version >= 29, 0)) {
                const vdf::Node* type = root.path({"appinfo", "common", "type"});
                if (type) out[id] = type->value;
            }
            if (out.size() == wanted.size()) break;
        }
        pos = entryEnd;
    }
    return out;
}

}  // namespace

uint32_t SteamProvider::RunningAppId() {
    return RegReadDword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"RunningAppID", 0);
}

std::wstring SteamProvider::LibraryCacheFile(const std::string& appId) const {
    if (userDir_.empty()) return {};
    return JoinPath(userDir_, L"config\\librarycache\\" + Widen(appId) + L".json");
}

std::vector<std::pair<std::wstring, std::wstring>> SteamProvider::Screenshots(const std::string& appId) const {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    if (userDir_.empty()) return out;
    std::wstring dir = JoinPath(userDir_, L"760\\remote\\" + Widen(appId) + L"\\screenshots");
    std::wstring thumbs = JoinPath(dir, L"thumbnails");
    std::vector<std::pair<int64_t, std::wstring>> files;
    for (const auto& f : ListFiles(dir, L"*.jpg")) {
        std::wstring p = JoinPath(dir, f);
        files.emplace_back(FileModifiedUnix(p), f);
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [t, f] : files) {
        std::wstring full = JoinPath(dir, f), thumb = JoinPath(thumbs, f);
        out.emplace_back(full, FileExists(thumb) ? thumb : full);
    }
    return out;
}

bool SteamProvider::Scan(std::vector<Game>& out, bool allowCdnArt) {
    steamPath_ = RegReadString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steamPath_.empty())
        steamPath_ = RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    steamPath_ = NormalizeSlashes(steamPath_);
    if (steamPath_.empty() || !DirExists(steamPath_)) {
        steamPath_.clear();
        return false;
    }

    // Most recently active local account = newest localconfig.vdf.
    userDir_.clear();
    steamId64_.clear();
    int64_t newest = 0;
    std::wstring userdata = JoinPath(steamPath_, L"userdata");
    for (auto& d : ListDirs(userdata)) {
        if (d == L"0" || d.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
        int64_t t = FileModifiedUnix(JoinPath(userdata, d + L"\\config\\localconfig.vdf"));
        if (t > newest) {
            newest = t;
            userDir_ = JoinPath(userdata, d);
            steamId64_ = std::to_string(kSteamId64Base + std::stoull(d));
        }
    }
    avatar_.clear();
    if (!steamId64_.empty()) {
        std::wstring av = JoinPath(steamPath_, L"config\\avatarcache\\" + Widen(steamId64_) + L".png");
        if (FileExists(av)) avatar_ = av;
    }

    auto stats = ReadLocalConfig(userDir_, &persona_);
    std::map<std::string, std::vector<std::wstring>> collections;
    std::set<std::string> favorites;
    if (!userDir_.empty()) ReadCollections(userDir_, collections, favorites);

    // Libraries
    std::vector<std::wstring> libs = {steamPath_};
    std::string text;
    if (ReadFileBytes(JoinPath(steamPath_, L"steamapps\\libraryfolders.vdf"), text)) {
        vdf::Node root;
        if (vdf::Parse(text, root)) {
            if (const vdf::Node* lf = root.get("libraryfolders")) {
                for (const auto& lib : lf->children) {
                    std::wstring p = NormalizeSlashes(Widen(lib.isObject ? lib.str("path") : lib.value));
                    if (p.empty()) continue;
                    bool dup = false;
                    for (auto& l : libs) dup |= (_wcsicmp(l.c_str(), p.c_str()) == 0);
                    if (!dup) libs.push_back(p);
                }
            }
        }
    }

    std::wstring cacheRoot = JoinPath(steamPath_, L"appcache\\librarycache");
    std::set<std::string> seen;
    size_t firstNew = out.size();
    for (const auto& lib : libs) {
        std::wstring apps = JoinPath(lib, L"steamapps");
        for (const auto& file : ListFiles(apps, L"appmanifest_*.acf")) {
            std::string acf;
            if (!ReadFileBytes(JoinPath(apps, file), acf)) continue;
            vdf::Node root;
            if (!vdf::Parse(acf, root)) continue;
            const vdf::Node* st = root.get("AppState");
            if (!st) continue;
            std::string appId = st->str("appid");
            std::wstring name = Widen(st->str("name"));
            std::wstring installdir = Widen(st->str("installdir"));
            if (appId.empty() || name.empty() || installdir.empty() || seen.count(appId) || IsTool(appId, name))
                continue;
            seen.insert(appId);

            Game g;
            g.id = "steam:" + appId;
            g.platform = Platform::Steam;
            g.platformId = appId;
            g.name = name;
            g.installDir = JoinPath(apps, L"common\\" + installdir);
            g.launchUri = Widen("steam://rungameid/" + appId);
            g.sizeBytes = static_cast<uint64_t>(st->num("SizeOnDisk"));
            g.installedAt = FileCreatedUnix(JoinPath(apps, file));
            g.lastPlayed = st->num("LastPlayed");
            auto it = stats.find(appId);
            if (it != stats.end()) {
                g.playMinutes = it->second.playMinutes;
                if (it->second.lastPlayed > g.lastPlayed) g.lastPlayed = it->second.lastPlayed;
            } else if (!userDir_.empty()) {
                g.playMinutes = 0;
            }

            ArtSet art = FindArt(cacheRoot, appId);
            g.cover = art.cover;
            g.hero = art.hero;
            g.logo = art.logo;
            g.header = art.header;
            if (allowCdnArt) {
                if (g.cover.empty()) g.cover = CdnUrl(appId, "library_600x900_2x.jpg");
                if (g.hero.empty()) g.hero = CdnUrl(appId, "library_hero.jpg");
                if (g.logo.empty()) g.logo = CdnUrl(appId, "logo.png");
                if (g.header.empty()) g.header = CdnUrl(appId, "header.jpg");
            }

            ReadClientCache(LibraryCacheFile(appId), g);
            auto col = collections.find(appId);
            if (col != collections.end()) g.platformCollections = col->second;
            g.platformFavorite = favorites.count(appId) > 0;
            out.push_back(std::move(g));
        }
    }

    auto types = ReadAppTypes(steamPath_, seen);
    for (size_t i = firstNew; i < out.size(); ++i) {
        auto it = types.find(out[i].platformId);
        if (it == types.end()) continue;
        std::string t = it->second;
        for (auto& ch : t) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        out[i].isApp = t == "application" || t == "tool";
    }
    return true;
}

void SteamProvider::RefreshPlaytime(std::vector<Game>& games) const {
    auto stats = ReadLocalConfig(userDir_, nullptr);
    for (auto& g : games) {
        if (g.platform != Platform::Steam) continue;
        auto it = stats.find(g.platformId);
        if (it == stats.end()) continue;
        if (it->second.playMinutes >= 0) g.playMinutes = it->second.playMinutes;
        if (it->second.lastPlayed > g.lastPlayed) g.lastPlayed = it->second.lastPlayed;
        ReadClientCache(LibraryCacheFile(g.platformId), g);
    }
}

}  // namespace lumen
