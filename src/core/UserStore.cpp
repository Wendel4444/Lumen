#include "UserStore.h"

#include <algorithm>

#include "Json.h"
#include "Util.h"

namespace lumen {

static std::wstring StorePath() { return JoinPath(AppDataDir(), L"library.json"); }
static constexpr size_t kMaxHistory = 300;
static constexpr size_t kMaxSessions = 5000;

bool UserStore::Load() {
    std::string text;
    if (!ReadFileBytes(StorePath(), text)) return false;
    json::Value root;
    if (!json::Parse(text, root) || !root.isObject()) return false;

    const json::Value& g = root["games"];
    for (size_t i = 0; i < g.size(); ++i) {
        const json::Value& o = g.items()[i];
        UserGameData d;
        d.favorite = o["favorite"].asBool();
        d.hidden = o["hidden"].asBool();
        for (const auto& c : o["categories"].items()) d.categories.push_back(Widen(c.asString()));
        d.addedAt = o["added"].asInt();
        d.launches = static_cast<int>(o["launches"].asInt());
        d.lastLaunch = o["lastLaunch"].asInt();
        d.trackedSeconds = o["trackedSeconds"].asInt();
        int64_t st = o["status"].asInt();
        d.status = static_cast<PlayStatus>(st >= 0 && st <= 4 ? st : 0);
        d.notes = Widen(o["notes"].str());
        d.coverOverride = Widen(o["cover"].str());
        d.heroOverride = Widen(o["hero"].str());
        d.logoOverride = Widen(o["logo"].str());
        games[g.keys()[i]] = std::move(d);
    }
    for (const auto& c : root["categories"].items()) categories.push_back(Widen(c.asString()));
    for (const auto& c : root["custom"].items()) {
        CustomGame cg;
        cg.id = c["id"].str();
        cg.name = Widen(c["name"].str());
        cg.exe = Widen(c["exe"].str());
        cg.args = Widen(c["args"].str());
        cg.addedAt = c["added"].asInt();
        if (!cg.id.empty() && !cg.exe.empty()) custom.push_back(std::move(cg));
    }
    for (const auto& h : root["history"].items()) {
        HistoryEntry e{h["id"].str(), h["t"].asInt(), h["s"].asInt()};
        if (!e.id.empty()) history.push_back(std::move(e));
    }
    for (const auto& s : root["sessions"].items()) {
        PlaySession p{s["id"].str(), s["s"].asInt(), s["d"].asInt()};
        if (!p.id.empty() && p.seconds > 0) sessions.push_back(std::move(p));
    }
    return true;
}

bool UserStore::Save() const {
    json::Value root = json::Value::Object();
    root.set("version", 1);
    json::Value g = json::Value::Object();
    for (const auto& [id, d] : games) {
        json::Value o = json::Value::Object();
        if (d.favorite) o.set("favorite", true);
        if (d.hidden) o.set("hidden", true);
        if (!d.categories.empty()) {
            json::Value cats = json::Value::Array();
            for (const auto& c : d.categories) cats.push(Narrow(c));
            o.set("categories", std::move(cats));
        }
        o.set("added", d.addedAt);
        if (d.launches) o.set("launches", d.launches);
        if (d.lastLaunch) o.set("lastLaunch", d.lastLaunch);
        if (d.trackedSeconds) o.set("trackedSeconds", d.trackedSeconds);
        if (d.status != PlayStatus::None) o.set("status", static_cast<int>(d.status));
        if (!d.notes.empty()) o.set("notes", Narrow(d.notes));
        if (!d.coverOverride.empty()) o.set("cover", Narrow(d.coverOverride));
        if (!d.heroOverride.empty()) o.set("hero", Narrow(d.heroOverride));
        if (!d.logoOverride.empty()) o.set("logo", Narrow(d.logoOverride));
        g.set(id, std::move(o));
    }
    root.set("games", std::move(g));

    json::Value cats = json::Value::Array();
    for (const auto& c : categories) cats.push(Narrow(c));
    root.set("categories", std::move(cats));

    json::Value cu = json::Value::Array();
    for (const auto& c : custom) {
        json::Value o = json::Value::Object();
        o.set("id", c.id);
        o.set("name", Narrow(c.name));
        o.set("exe", Narrow(c.exe));
        o.set("args", Narrow(c.args));
        o.set("added", c.addedAt);
        cu.push(std::move(o));
    }
    root.set("custom", std::move(cu));

    json::Value hist = json::Value::Array();
    for (const auto& h : history) {
        json::Value o = json::Value::Object();
        o.set("id", h.id);
        o.set("t", h.time);
        if (h.seconds) o.set("s", h.seconds);
        hist.push(std::move(o));
    }
    root.set("history", std::move(hist));

    json::Value ses = json::Value::Array();
    for (const auto& s : sessions) {
        json::Value o = json::Value::Object();
        o.set("id", s.id);
        o.set("s", s.start);
        o.set("d", s.seconds);
        ses.push(std::move(o));
    }
    root.set("sessions", std::move(ses));
    return WriteFileAtomic(StorePath(), json::Serialize(root, true));
}

const UserGameData* UserStore::Find(const std::string& id) const {
    auto it = games.find(id);
    return it == games.end() ? nullptr : &it->second;
}

void UserStore::RecordLaunch(const std::string& id, int64_t when) {
    auto& d = games[id];
    d.launches++;
    d.lastLaunch = when;
    history.insert(history.begin(), HistoryEntry{id, when, 0});
    if (history.size() > kMaxHistory) history.resize(kMaxHistory);
}

void UserStore::RecordSession(const std::string& id, int64_t seconds) {
    if (seconds <= 0) return;
    int64_t end = NowUnix(), start = end - seconds;
    games[id].trackedSeconds += seconds;
    sessions.push_back(PlaySession{id, start, seconds});
    if (sessions.size() > kMaxSessions) sessions.erase(sessions.begin(), sessions.begin() + (sessions.size() - kMaxSessions));
    // Attach the length to the launch that started this session (if Lumen launched it).
    for (auto& h : history) {
        if (h.id != id) continue;
        if (h.seconds == 0 && h.time >= start - 180) h.seconds = seconds;
        break;
    }
}

int64_t UserStore::SecondsBetween(int64_t from, int64_t to) const {
    int64_t total = 0;
    for (const auto& s : sessions) {
        int64_t a = std::max(from, s.start), b = std::min(to, s.start + s.seconds);
        if (b > a) total += b - a;
    }
    return total;
}

}  // namespace lumen
