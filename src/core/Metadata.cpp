#include "Metadata.h"

#include <algorithm>
#include <map>

#include "Http.h"
#include "Json.h"
#include "Library.h"
#include "Util.h"

namespace lumen {

namespace {

constexpr int64_t kDetailsTtl = 14 * 86400;
constexpr int64_t kWebAchTtl = 60 * 60;

std::wstring CacheFile(const std::string& name) { return JoinPath(AppSubDir(L"cache"), Widen(name)); }

// Reads a cached response; returns false when missing (or stale and staleOk == false).
bool ReadCache(const std::wstring& path, int64_t ttl, std::string& out, bool staleOk = false) {
    int64_t mtime = FileModifiedUnix(path);
    if (mtime == 0) return false;
    if (!staleOk && NowUnix() - mtime > ttl) return false;
    return ReadFileBytes(path, out);
}

json::Value AchToJson(const AchievementSet& s) {
    json::Value v = json::Value::Object();
    v.set("unlocked", s.unlocked);
    v.set("total", s.total);
    v.set("complete", s.complete);
    json::Value list = json::Value::Array();
    for (const auto& a : s.list) {
        json::Value o = json::Value::Object();
        o.set("id", a.id);
        o.set("name", Narrow(a.name));
        o.set("desc", Narrow(a.description));
        o.set("icon", Narrow(a.icon));
        o.set("iconGray", Narrow(a.iconGray));
        o.set("achieved", a.achieved);
        o.set("hidden", a.hidden);
        o.set("unlockedAt", a.unlockedAt);
        o.set("pct", static_cast<double>(a.globalPercent));
        list.push(std::move(o));
    }
    v.set("list", std::move(list));
    return v;
}

AchievementSet AchFromJson(const json::Value& v) {
    AchievementSet s;
    s.unlocked = static_cast<int>(v["unlocked"].asInt());
    s.total = static_cast<int>(v["total"].asInt());
    s.complete = v["complete"].asBool();
    for (const auto& o : v["list"].items()) {
        Achievement a;
        a.id = o["id"].str();
        a.name = Widen(o["name"].str());
        a.description = Widen(o["desc"].str());
        a.icon = Widen(o["icon"].str());
        a.iconGray = Widen(o["iconGray"].str());
        a.achieved = o["achieved"].asBool();
        a.hidden = o["hidden"].asBool();
        a.unlockedAt = o["unlockedAt"].asInt();
        a.globalPercent = static_cast<float>(o["pct"].asNumber(-1));
        s.list.push_back(std::move(a));
    }
    s.state = s.total > 0 ? AchievementSet::State::Ready : AchievementSet::State::NoAchievements;
    return s;
}

void SortAchievements(std::vector<Achievement>& list) {
    std::stable_sort(list.begin(), list.end(), [](const Achievement& a, const Achievement& b) {
        if (a.achieved != b.achieved) return a.achieved;
        if (a.achieved) return a.unlockedAt > b.unlockedAt;
        return a.globalPercent > b.globalPercent;  // easiest locked ones first
    });
}

// Achievements the Steam client caches for its library page. Counts are exact; the list is partial.
bool ParseClientCache(const std::wstring& file, AchievementSet& set) {
    std::string text;
    if (file.empty() || !ReadFileBytes(file, text)) return false;
    json::Value root;
    if (!json::Parse(text, root) || !root.isArray()) return false;

    const json::Value* data = nullptr;
    const json::Value* mapData = nullptr;
    for (const auto& e : root.items()) {
        if (e[0].asString() == "achievements") data = &e[1]["data"];
        else if (e[0].asString() == "achievementmap") mapData = &e[1]["data"];
    }
    if (!data || !data->isObject()) return false;
    set.total = static_cast<int>((*data)["nTotal"].asInt());
    set.unlocked = static_cast<int>((*data)["nAchieved"].asInt());

    std::map<std::string, Achievement> known;
    auto read = [](const json::Value& a) {
        Achievement x;
        x.id = a["strID"].str();
        x.name = Widen(a["strName"].str());
        x.description = Widen(a["strDescription"].str());
        x.icon = Widen(a["strImage"].str());
        x.hidden = a["bHidden"].asBool();
        x.unlockedAt = a["rtUnlocked"].asInt();
        x.globalPercent = static_cast<float>(a["flAchieved"].asNumber(-1));
        return x;
    };
    for (const char* vec : {"vecHighlight", "vecAchievedHidden", "vecUnachieved"}) {
        for (const auto& a : (*data)[vec].items()) {
            if (a["strID"].str().empty() || !a.find("bAchieved")) continue;
            Achievement x = read(a);
            x.achieved = a["bAchieved"].asBool();
            known[x.id] = std::move(x);
        }
    }
    int achievedKnown = 0;
    for (auto& [id, a] : known) achievedKnown += a.achieved ? 1 : 0;

    // The achievement map lists definitions without unlock state. Its entries can only be shown as
    // locked when every unlocked achievement is already known — otherwise their state is unknown.
    if (mapData && achievedKnown >= set.unlocked) {
        json::Value inner;
        if (json::Parse(mapData->asString(), inner)) {
            for (const auto& appEntry : inner.items()) {
                for (const auto& pair : appEntry[1].items()) {
                    const json::Value& a = pair[1];
                    std::string id = a["strID"].str();
                    if (id.empty() || known.count(id)) continue;
                    Achievement x = read(a);
                    x.achieved = false;
                    known[id] = std::move(x);
                }
            }
        }
    }
    for (auto& [id, a] : known) set.list.push_back(std::move(a));
    SortAchievements(set.list);
    set.complete = set.total > 0 && static_cast<int>(set.list.size()) >= set.total;
    return true;
}

std::wstring JoinNames(const json::Value& arr, const char* field = nullptr) {
    std::wstring s;
    for (const auto& v : arr.items()) {
        std::wstring n = Widen(field ? v[field].str() : v.str());
        if (n.empty()) continue;
        if (!s.empty()) s += L", ";
        s += n;
    }
    return s;
}

}  // namespace

void MetadataService::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void MetadataService::Stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void MetadataService::SetConfig(const MetadataConfig& cfg) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_ = cfg;
}

void MetadataService::Enqueue(Job job) {
    {
        std::lock_guard<std::mutex> lk(m_);
        jobs_.push_front(std::move(job));  // newest request = what the user is looking at
    }
    cv_.notify_one();
}

void MetadataService::RequestDetails(const Game& g) {
    if (g.platform != Platform::Steam && g.platform != Platform::GOG) return;
    if (!askedDetails_.insert(g.id).second) return;
    Job j;
    j.kind = g.platform == Platform::Steam ? Job::Kind::SteamDetails : Job::Kind::GogDetails;
    j.gameId = g.id;
    j.platformId = g.platformId;
    Enqueue(std::move(j));
}

void MetadataService::RequestAchievements(const Game& g, const std::wstring& clientCache, bool force) {
    if (g.platform != Platform::Steam) {
        AchievementSet& s = ach_[g.id];
        s.state = AchievementSet::State::Unsupported;
        s.message = g.platform == Platform::Custom
                        ? L"Jogos adicionados manualmente não têm uma fonte de conquistas."
                        : std::wstring(L"A ") + PlatformName(g.platform) +
                              L" não oferece uma API pública de conquistas sem login.";
        return;
    }
    if (!force && !askedAch_.insert(g.id).second) return;
    askedAch_.insert(g.id);
    AchievementSet& s = ach_[g.id];
    if (s.state == AchievementSet::State::Unknown) s.state = AchievementSet::State::Loading;
    Job j;
    j.kind = Job::Kind::SteamAchievements;
    j.gameId = g.id;
    j.platformId = g.platformId;
    j.cacheFile = clientCache;
    j.force = force;
    Enqueue(std::move(j));
}

void MetadataService::ForgetAll() {
    askedDetails_.clear();
    askedAch_.clear();
    askedNews_.clear();
}

const AchievementSet* MetadataService::Achievements(const std::string& id) const {
    auto it = ach_.find(id);
    return it == ach_.end() ? nullptr : &it->second;
}

// Fills only what the platform's local data (or the user's own art) left empty.
template <typename D>
static void ApplyDetails(Game& g, const D& d) {
    if (g.description.empty()) g.description = d.description;
    if (g.developer.empty()) g.developer = d.developer;
    if (g.publisher.empty()) g.publisher = d.publisher;
    if (!d.releaseDate.empty()) g.releaseDate = d.releaseDate;
    if (!d.genres.empty()) g.genres = d.genres;
    if (g.header.empty() || StartsWith(g.header, L"http")) {
        if (!d.header.empty()) g.header = d.header;
    }
    if (g.hero.empty() && !d.hero.empty()) g.hero = d.hero;
    if (g.cover.empty() && !d.cover.empty()) g.cover = d.cover;
    if (g.logo.empty() && !d.logo.empty()) g.logo = d.logo;
    if (g.trailer.empty()) g.trailer = d.trailer;
}

bool MetadataService::Poll(Library& lib) {
    std::vector<Details> d;
    std::vector<std::pair<std::string, AchievementSet>> a;
    std::vector<std::pair<std::string, std::vector<NewsItem>>> n;
    {
        std::lock_guard<std::mutex> lk(m_);
        d.swap(doneDetails_);
        a.swap(doneAch_);
        n.swap(doneNews_);
    }
    for (auto& [id, items] : n) news_[id] = std::move(items);
    for (auto& det : d) {
        if (Game* g = lib.Find(det.gameId)) ApplyDetails(*g, det);
        details_[det.gameId] = std::move(det);
    }
    for (auto& [id, set] : a) {
        if (Game* g = lib.Find(id)) {
            if (set.state == AchievementSet::State::Ready || set.state == AchievementSet::State::NoAchievements) {
                g->achUnlocked = set.unlocked;
                g->achTotal = set.total;
            }
        }
        ach_[id] = std::move(set);
    }
    return !d.empty() || !a.empty() || !n.empty();
}

void MetadataService::ReapplyDetails(Library& lib) {
    for (auto& [id, det] : details_)
        if (Game* g = lib.Find(id)) ApplyDetails(*g, det);
    for (auto& [id, set] : ach_)
        if (Game* g = lib.Find(id); g && set.state == AchievementSet::State::Ready) {
            g->achUnlocked = set.unlocked;
            g->achTotal = set.total;
        }
}

bool MetadataService::Throttled() { return NowUnix() < backoffUntil_; }

void MetadataService::NoteStatus(int status) {
    if (status == 429 || status >= 500) backoffUntil_ = NowUnix() + 300;
}

void MetadataService::Run() {
    for (;;) {
        Job job;
        MetadataConfig cfg;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            cfg = cfg_;
        }
        if (job.kind == Job::Kind::SteamAchievements) {
            AchievementSet s = FetchSteamAchievements(job, cfg);
            std::lock_guard<std::mutex> lk(m_);
            doneAch_.emplace_back(job.gameId, std::move(s));
        } else if (job.kind == Job::Kind::SteamNews) {
            auto n = FetchSteamNews(job, cfg);
            std::lock_guard<std::mutex> lk(m_);
            doneNews_.emplace_back(job.gameId, std::move(n));
        } else {
            Details d = job.kind == Job::Kind::SteamDetails ? FetchSteamDetails(job, cfg) : FetchGogDetails(job, cfg);
            d.gameId = job.gameId;
            std::lock_guard<std::mutex> lk(m_);
            doneDetails_.push_back(std::move(d));
        }
    }
}

MetadataService::Details MetadataService::FetchSteamDetails(const Job& j, const MetadataConfig& cfg) {
    Details d;
    std::wstring path = CacheFile("steam_" + j.platformId + ".json");
    std::string text;
    bool have = ReadCache(path, kDetailsTtl, text);
    if (!have && cfg.online && !Throttled()) {
        HttpResponse r = HttpGet(L"https://store.steampowered.com/api/appdetails?l=brazilian&appids=" +
                                 Widen(j.platformId));
        NoteStatus(r.status);
        if (r.ok() && !r.body.empty()) {
            WriteFileAtomic(path, r.body);
            text = std::move(r.body);
            have = true;
        }
    }
    if (!have) have = ReadCache(path, kDetailsTtl, text, true);  // stale but better than nothing
    json::Value root;
    if (!have || !json::Parse(text, root)) return d;
    const json::Value& entry = root[j.platformId];
    if (!entry["success"].asBool()) return d;
    const json::Value& data = entry["data"];
    d.description = StripHtml(data["short_description"].str());
    d.developer = JoinNames(data["developers"]);
    d.publisher = JoinNames(data["publishers"]);
    d.releaseDate = Widen(data["release_date"]["date"].str());
    d.genres = JoinNames(data["genres"], "description");
    d.header = Widen(data["header_image"].str());
    // Trailer: the highlighted movie first. Steam now ships HLS playlists (fragmented MP4 segments).
    const json::Value* best = nullptr;
    for (const auto& m : data["movies"].items()) {
        if (m["hls_h264"].str().empty()) continue;
        if (!best || (m["highlight"].asBool() && !(*best)["highlight"].asBool())) best = &m;
    }
    if (best) d.trailer = Widen((*best)["hls_h264"].str());
    return d;
}

// Steam announcements use BBCode and inline links: keep readable text only.
static std::wstring CleanNews(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == '[') {  // [b], [/url], [img]...[/img]
            size_t end = raw.find(']', i);
            if (end == std::string::npos) break;
            std::string tag = raw.substr(i + 1, end - i - 1);
            if (tag.rfind("img", 0) == 0 || tag.rfind("previewyoutube", 0) == 0 || tag.rfind("video", 0) == 0) {
                size_t close = raw.find("[/", end);  // drop the whole element
                size_t close2 = close == std::string::npos ? std::string::npos : raw.find(']', close);
                i = close2 == std::string::npos ? end + 1 : close2 + 1;
            } else {
                i = end + 1;
            }
            out += ' ';
            continue;
        }
        if (raw.compare(i, 4, "http") == 0 || raw.compare(i, 1, "{") == 0) {  // bare links, {STEAM_CLAN_IMAGE}
            while (i < raw.size() && !isspace(static_cast<unsigned char>(raw[i]))) ++i;
            continue;
        }
        out += raw[i++];
    }
    return StripHtml(out);
}

std::vector<NewsItem> MetadataService::FetchSteamNews(const Job& j, const MetadataConfig& cfg) {
    std::vector<NewsItem> items;
    std::wstring path = CacheFile("news_" + j.platformId + ".json");
    std::string text;
    bool have = ReadCache(path, 6 * 3600, text);
    if (!have && cfg.online && !Throttled()) {
        HttpResponse r = HttpGet(L"https://api.steampowered.com/ISteamNews/GetNewsForApp/v2/?count=4&maxlength=600"
                                 L"&feeds=steam_community_announcements&appid=" +
                                 Widen(j.platformId));
        NoteStatus(r.status);
        if (r.ok() && !r.body.empty()) {
            WriteFileAtomic(path, r.body);
            text = std::move(r.body);
            have = true;
        }
    }
    if (!have) have = ReadCache(path, 0, text, true);
    json::Value root;
    if (!have || !json::Parse(text, root)) return items;
    for (const auto& n : root["appnews"]["newsitems"].items()) {
        NewsItem it;
        it.title = Trim(Widen(n["title"].str()));
        it.url = Widen(n["url"].str());
        it.date = n["date"].asInt();
        it.snippet = CleanNews(n["contents"].str());
        if (!it.title.empty()) items.push_back(std::move(it));
    }
    return items;
}

void MetadataService::RequestNews(const Game& g) {
    if (g.platform != Platform::Steam || !askedNews_.insert(g.id).second) return;
    Job j;
    j.kind = Job::Kind::SteamNews;
    j.gameId = g.id;
    j.platformId = g.platformId;
    {
        std::lock_guard<std::mutex> lk(m_);
        jobs_.push_back(std::move(j));  // background priority: after what the user is looking at
    }
    cv_.notify_one();
}

const std::vector<NewsItem>* MetadataService::News(const std::string& id) const {
    auto it = news_.find(id);
    return it == news_.end() ? nullptr : &it->second;
}

bool MetadataService::FetchCachedJson(const std::wstring& url, const std::string& cacheName,
                                      const MetadataConfig& cfg, json::Value& out) {
    std::wstring path = CacheFile(cacheName);
    std::string text;
    bool have = ReadCache(path, kDetailsTtl, text);
    if (!have && cfg.online && !Throttled()) {
        HttpResponse r = HttpGet(url);
        NoteStatus(r.status);
        if (r.ok() && !r.body.empty()) {
            WriteFileAtomic(path, r.body);
            text = std::move(r.body);
            have = true;
        }
    }
    if (!have) have = ReadCache(path, kDetailsTtl, text, true);  // stale but better than nothing
    return have && json::Parse(text, out);
}

MetadataService::Details MetadataService::FetchGogDetails(const Job& j, const MetadataConfig& cfg) {
    Details d;
    // 1) GOG's games database (what GOG Galaxy itself uses): official cover, background, logo and facts.
    json::Value db;
    if (FetchCachedJson(L"https://gamesdb.gog.com/platforms/gog/external_releases/" + Widen(j.platformId),
                        "gogdb_" + j.platformId + ".json", cfg, db)) {
        const json::Value& game = db["game"];
        // Image URLs are templates: ".../<hash>{formatter}.{ext}?namespace=gamesdb". No formatter = original size.
        auto image = [&](const char* key, const char* ext) {
            std::string u = game[key]["url_format"].str();
            size_t p = u.find("{formatter}");
            if (p != std::string::npos) u.erase(p, 11);
            p = u.find("{ext}");
            if (p != std::string::npos) u.replace(p, 5, ext);
            return Widen(u);
        };
        d.cover = image("vertical_cover", "jpg");
        d.hero = image("background", "jpg");
        if (d.hero.empty()) d.hero = image("horizontal_artwork", "jpg");
        d.logo = image("logo", "png");
        d.description = StripHtml(db["summary"]["*"].str());
        std::wstring genres;
        for (const auto& gn : game["genres"].items()) {
            std::wstring n = Widen(gn["name"]["*"].str());
            if (!n.empty()) genres += (genres.empty() ? L"" : L", ") + n;
        }
        d.genres = genres;
        d.developer = JoinNames(game["developers"], "name");
        d.publisher = JoinNames(game["publishers"], "name");
        std::string rel = game["first_release_date"].str();
        if (rel.size() >= 10) d.releaseDate = Widen(rel.substr(8, 2) + "/" + rel.substr(5, 2) + "/" + rel.substr(0, 4));
    }
    // 2) The store API has the description in Portuguese, and a background if the database lacks one.
    json::Value root;
    if (FetchCachedJson(L"https://api.gog.com/products/" + Widen(j.platformId) + L"?expand=description&locale=pt-BR",
                        "gog_" + j.platformId + ".json", cfg, root)) {
        std::wstring lead = StripHtml(root["description"]["lead"].str());
        if (!lead.empty()) d.description = lead;
        if (d.hero.empty()) {
            std::string bg = root["images"]["background"].str();
            if (bg.rfind("//", 0) == 0) bg = "https:" + bg;
            d.hero = Widen(bg);
        }
        std::string rel = root["release_date"].str();
        if (d.releaseDate.empty() && rel.size() >= 10)
            d.releaseDate = Widen(rel.substr(8, 2) + "/" + rel.substr(5, 2) + "/" + rel.substr(0, 4));
    }
    return d;
}

AchievementSet MetadataService::FetchSteamAchievements(const Job& j, const MetadataConfig& cfg) {
    AchievementSet local;
    bool haveLocal = ParseClientCache(j.cacheFile, local);
    local.source = L"Cache local da Steam";

    // Steam Web API: complete list (requires the user's own key and a public "game details" profile).
    if (cfg.online && !cfg.steamApiKey.empty() && !cfg.steamId64.empty()) {
        std::wstring path = CacheFile("ach_" + j.platformId + ".json");
        std::string text;
        if (!j.force && ReadCache(path, kWebAchTtl, text)) {
            json::Value v;
            if (json::Parse(text, v)) {
                AchievementSet s = AchFromJson(v);
                s.source = L"Steam Web API";
                return s;
            }
        }
        std::wstring key = Widen(cfg.steamApiKey), app = Widen(j.platformId);
        std::wstring error;
        HttpResponse schema =
            HttpGet(L"https://api.steampowered.com/ISteamUserStats/GetSchemaForGame/v2/?l=brazilian&key=" + key +
                    L"&appid=" + app);
        json::Value sch;
        if (schema.ok() && json::Parse(schema.body, sch)) {
            const json::Value& defs = sch["game"]["availableGameStats"]["achievements"];
            if (!defs.isArray() || defs.size() == 0) {
                AchievementSet none;
                none.state = AchievementSet::State::NoAchievements;
                none.source = L"Steam Web API";
                none.message = L"Este jogo não tem conquistas.";
                return none;
            }
            HttpResponse player =
                HttpGet(L"https://api.steampowered.com/ISteamUserStats/GetPlayerAchievements/v1/?key=" + key +
                        L"&steamid=" + Widen(cfg.steamId64) + L"&appid=" + app);
            json::Value pl;
            if (player.ok() && json::Parse(player.body, pl) && pl["playerstats"]["success"].asBool()) {
                std::map<std::string, std::pair<bool, int64_t>> state;
                for (const auto& a : pl["playerstats"]["achievements"].items())
                    state[a["apiname"].str()] = {a["achieved"].asInt() != 0, a["unlocktime"].asInt()};
                std::map<std::string, float> pct;
                HttpResponse global = HttpGet(
                    L"https://api.steampowered.com/ISteamUserStats/GetGlobalAchievementPercentagesForApp/v2/?gameid=" +
                    app);
                json::Value gl;
                if (global.ok() && json::Parse(global.body, gl)) {
                    for (const auto& a : gl["achievementpercentages"]["achievements"].items()) {
                        const json::Value& p = a["percent"];
                        pct[a["name"].str()] = static_cast<float>(
                            p.isNumber() ? p.asNumber() : std::strtod(p.asString().c_str(), nullptr));
                    }
                }
                AchievementSet s;
                s.source = L"Steam Web API";
                for (const auto& def : defs.items()) {
                    Achievement a;
                    a.id = def["name"].str();
                    a.name = Widen(def["displayName"].str());
                    a.description = Widen(def["description"].str());
                    a.icon = Widen(def["icon"].str());
                    a.iconGray = Widen(def["icongray"].str());
                    a.hidden = def["hidden"].asInt() != 0;
                    auto st = state.find(a.id);
                    if (st != state.end()) {
                        a.achieved = st->second.first;
                        a.unlockedAt = st->second.second;
                    }
                    auto p = pct.find(a.id);
                    if (p != pct.end()) a.globalPercent = p->second;
                    s.unlocked += a.achieved ? 1 : 0;
                    s.list.push_back(std::move(a));
                }
                s.total = static_cast<int>(s.list.size());
                s.complete = true;
                SortAchievements(s.list);
                s.state = AchievementSet::State::Ready;
                WriteFileAtomic(path, json::Serialize(AchToJson(s)));
                return s;
            }
            error = (player.status == 403 || player.ok())
                        ? L"Perfil privado: deixe \"Detalhes do jogo\" público na Steam para ver a lista completa."
                        : L"A Steam Web API não respondeu.";
        } else {
            error = schema.status == 403 ? L"Chave da Steam Web API inválida." : L"A Steam Web API não respondeu.";
        }
        // Network failure: fall back to the last complete list we fetched.
        if (ReadCache(path, kWebAchTtl, text, true)) {
            json::Value v;
            if (json::Parse(text, v)) {
                AchievementSet s = AchFromJson(v);
                s.source = L"Steam Web API (cópia salva)";
                s.message = error;
                return s;
            }
        }
        local.message = error;
    }

    if (!haveLocal) {
        local.state = AchievementSet::State::NotSynced;
        local.message = L"A Steam ainda não sincronizou as conquistas deste jogo. Abra-o uma vez pela Steam.";
        return local;
    }
    if (local.total == 0) {
        local.state = AchievementSet::State::NoAchievements;
        local.message = L"Este jogo não tem conquistas.";
        return local;
    }
    local.state = AchievementSet::State::Ready;
    if (!local.complete && local.message.empty())
        local.message = L"Lista parcial do cache da Steam. Adicione sua chave da Steam Web API em Ajustes para ver todas.";
    return local;
}

}  // namespace lumen
