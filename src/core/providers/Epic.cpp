#include "Epic.h"

#include <windows.h>

#include <map>

#include "../Json.h"
#include "../Util.h"

namespace lumen {

namespace {

struct CatalogInfo {
    std::wstring cover, hero, logo, description, developer;
};

// catcache.bin is a base64-encoded JSON array of catalog items with official key art.
std::map<std::string, CatalogInfo> ReadCatalog(const std::wstring& dataDir) {
    std::map<std::string, CatalogInfo> out;
    std::string raw;
    if (!ReadFileBytes(JoinPath(dataDir, L"Catalog\\catcache.bin"), raw)) return out;
    std::string text = Base64Decode(raw);
    json::Value root;
    if (text.empty() || !json::Parse(text, root) || !root.isArray()) return out;
    for (const auto& item : root.items()) {
        std::string id = item["id"].str();
        if (id.empty()) continue;
        CatalogInfo info;
        std::wstring tall, wide, thumb, offerTall, offerWide;
        for (const auto& img : item["keyImages"].items()) {
            const std::string& type = img["type"].asString();
            std::wstring url = Widen(img["url"].str());
            if (type == "DieselGameBoxTall") tall = url;
            else if (type == "DieselGameBox") wide = url;
            else if (type == "DieselGameBoxLogo") info.logo = url;
            else if (type == "Thumbnail") thumb = url;
            else if (type == "OfferImageTall") offerTall = url;
            else if (type == "OfferImageWide") offerWide = url;
        }
        info.cover = !tall.empty() ? tall : !offerTall.empty() ? offerTall : thumb;
        info.hero = !wide.empty() ? wide : offerWide;
        info.description = StripHtml(item["description"].str());
        info.developer = Widen(item["developer"].str());
        out[id] = std::move(info);
    }
    return out;
}

}  // namespace

bool EpicProvider::Scan(std::vector<Game>& out) {
    std::wstring programData = KnownEnv(L"ProgramData");
    if (programData.empty()) return false;
    std::wstring dataDir = JoinPath(programData, L"Epic\\EpicGamesLauncher\\Data");
    std::wstring manifests = JoinPath(dataDir, L"Manifests");
    auto files = ListFiles(manifests, L"*.item");
    if (files.empty()) return false;
    auto catalog = ReadCatalog(dataDir);

    for (const auto& f : files) {
        std::string text;
        if (!ReadFileBytes(JoinPath(manifests, f), text)) continue;
        json::Value m;
        if (!json::Parse(text, m) || !m.isObject()) continue;
        std::string app = m["AppName"].str();
        std::string mainApp = m["MainGameAppName"].str(app);
        if (app.empty() || app != mainApp) continue;               // DLC
        if (m["bIsIncompleteInstall"].asBool()) continue;
        if (m.find("bIsApplication") && !m["bIsApplication"].asBool()) continue;
        bool isGame = true;
        if (const json::Value* cats = m.find("AppCategories")) {
            isGame = false;
            for (const auto& c : cats->items()) isGame |= c.asString() == "games";
        }
        if (!isGame) continue;

        Game g;
        g.id = "epic:" + app;
        g.platform = Platform::Epic;
        g.platformId = app;
        g.name = Widen(m["DisplayName"].str(app));
        g.installDir = NormalizeSlashes(Widen(m["InstallLocation"].str()));
        std::wstring exe = Widen(m["LaunchExecutable"].str());
        if (!exe.empty()) g.exePath = JoinPath(g.installDir, NormalizeSlashes(exe));
        std::string ns = m["CatalogNamespace"].str(), item = m["CatalogItemId"].str();
        g.launchUri = Widen("com.epicgames.launcher://apps/" + ns + "%3A" + item + "%3A" + app +
                            "?action=launch&silent=true");
        g.sizeBytes = static_cast<uint64_t>(m["InstallSize"].asInt());
        g.installedAt = FileCreatedUnix(JoinPath(manifests, f));
        auto it = catalog.find(item);
        if (it != catalog.end()) {
            g.cover = it->second.cover;
            g.hero = it->second.hero;
            g.logo = it->second.logo;
            g.description = it->second.description;
            g.developer = it->second.developer;
        }
        if (!g.installDir.empty() && DirExists(g.installDir)) out.push_back(std::move(g));
    }
    return true;
}

bool GogProvider::Scan(std::vector<Game>& out) {
    const wchar_t* base = L"SOFTWARE\\WOW6432Node\\GOG.com\\Games";
    auto keys = RegSubKeys(HKEY_LOCAL_MACHINE, base);
    for (const auto& k : keys) {
        std::wstring sub = std::wstring(base) + L"\\" + k;
        std::wstring dependsOn = RegReadString(HKEY_LOCAL_MACHINE, sub.c_str(), L"dependsOn");
        if (!dependsOn.empty()) continue;  // DLC
        std::wstring name = RegReadString(HKEY_LOCAL_MACHINE, sub.c_str(), L"gameName");
        std::wstring path = RegReadString(HKEY_LOCAL_MACHINE, sub.c_str(), L"path");
        std::wstring exe = RegReadString(HKEY_LOCAL_MACHINE, sub.c_str(), L"exe");
        if (name.empty() || exe.empty() || !FileExists(exe)) continue;
        Game g;
        g.id = "gog:" + Narrow(k);
        g.platform = Platform::GOG;
        g.platformId = Narrow(k);
        g.name = name;
        g.installDir = NormalizeSlashes(path.empty() ? ParentDir(exe) : path);
        g.exePath = exe;
        g.args = RegReadString(HKEY_LOCAL_MACHINE, sub.c_str(), L"launchParam");
        g.installedAt = FileCreatedUnix(exe);
        out.push_back(std::move(g));
    }
    return !keys.empty();
}

}  // namespace lumen
