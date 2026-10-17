#include "Settings.h"

#include <algorithm>

#include "Json.h"
#include "Util.h"

namespace lumen {

static std::wstring SettingsPath() { return JoinPath(AppDataDir(), L"settings.json"); }

bool Settings::Load() {
    std::string text;
    if (!ReadFileBytes(SettingsPath(), text)) return false;
    json::Value v;
    if (!json::Parse(text, v) || !v.isObject()) return false;

    theme = static_cast<ThemeMode>(std::clamp<int64_t>(v["theme"].asInt(0), 0, 2));
    accent = static_cast<int>(std::clamp<int64_t>(v["accent"].asInt(0), 0, 6));  // 6 = colour of the selected game
    cardSize = static_cast<int>(std::clamp<int64_t>(v["cardSize"].asInt(1), 0, 2));
    reduceMotion = v["reduceMotion"].asBool(false);
    blur = v["blur"].asBool(true);
    startFullscreen = v["startFullscreen"].asBool(false);
    sourceSteam = v["sourceSteam"].asBool(true);
    sourceEpic = v["sourceEpic"].asBool(true);
    sourceGog = v["sourceGog"].asBool(true);
    onlineData = v["onlineData"].asBool(true);
    showApps = v["showApps"].asBool(false);
    remindInterval = static_cast<int>(std::clamp<int64_t>(v["remindInterval"].asInt(2), 0, 3));
    sessionSummary = v["sessionSummary"].asBool(true);
    keepInTray = v["keepInTray"].asBool(true);
    trayHintShown = v["trayHintShown"].asBool(false);
    dailyGoal = static_cast<int>(std::clamp<int64_t>(v["dailyGoal"].asInt(0), 0, 4));
    uiSounds = v["uiSounds"].asBool(true);
    fullscreenOnController = v["fullscreenOnController"].asBool(false);
    trailers = v["trailers"].asBool(true);
    showcaseMode = v["showcaseMode"].asBool(true);
    sort = static_cast<SortMode>(std::clamp<int64_t>(v["sort"].asInt(0), 0, 3));
    viewMode = static_cast<int>(std::clamp<int64_t>(v["viewMode"].asInt(0), 0, 1));
    lastSelected = v["lastSelected"].str();
    steamApiKey = UnprotectSecret(v["steamApiKey"].str());
    return true;
}

bool Settings::Save() const {
    json::Value v = json::Value::Object();
    v.set("version", 1);
    v.set("theme", static_cast<int>(theme));
    v.set("accent", accent);
    v.set("cardSize", cardSize);
    v.set("reduceMotion", reduceMotion);
    v.set("blur", blur);
    v.set("startFullscreen", startFullscreen);
    v.set("sourceSteam", sourceSteam);
    v.set("sourceEpic", sourceEpic);
    v.set("sourceGog", sourceGog);
    v.set("onlineData", onlineData);
    v.set("showApps", showApps);
    v.set("remindInterval", remindInterval);
    v.set("sessionSummary", sessionSummary);
    v.set("keepInTray", keepInTray);
    v.set("trayHintShown", trayHintShown);
    v.set("dailyGoal", dailyGoal);
    v.set("uiSounds", uiSounds);
    v.set("fullscreenOnController", fullscreenOnController);
    v.set("trailers", trailers);
    v.set("showcaseMode", showcaseMode);
    v.set("sort", static_cast<int>(sort));
    v.set("viewMode", viewMode);
    v.set("lastSelected", lastSelected);
    v.set("steamApiKey", ProtectSecret(steamApiKey));
    return WriteFileAtomic(SettingsPath(), json::Serialize(v, true));
}

}  // namespace lumen
