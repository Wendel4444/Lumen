#pragma once
#include <string>

namespace lumen {

enum class ThemeMode { Dark = 0, Light = 1, System = 2 };
enum class SortMode { Recent = 0, Name = 1, Playtime = 2, Added = 3 };

struct Settings {
    ThemeMode theme = ThemeMode::Dark;
    int accent = 0;          // index into the accent palette
    int cardSize = 1;        // 0 compact, 1 default, 2 large
    bool reduceMotion = false;
    bool blur = true;
    bool startFullscreen = false;

    bool sourceSteam = true;
    bool sourceEpic = true;
    bool sourceGog = true;
    bool onlineData = true;  // store details, CDN art, Steam Web API
    bool showApps = false;   // list software (OBS, Blender...) together with games

    // Wellbeing: Windows notifications while playing
    int remindInterval = 2;      // 0 off, 1 = 30 min, 2 = 1 h, 3 = 2 h
    bool sessionSummary = true;  // notify how long a session lasted when the game closes
    bool keepInTray = true;      // closing the window keeps Lumen in the tray so reminders keep working
    bool trayHintShown = false;
    int dailyGoal = 0;           // 0 off, 1 = 1 h, 2 = 2 h, 3 = 3 h, 4 = 4 h (daily play limit)
    bool uiSounds = true;        // soft console-like navigation sounds
    bool fullscreenOnController = false;
    bool trailers = true;        // play store trailers behind the selected game
    bool showcaseMode = true;    // when idle, the home screen cycles through games by itself

    int DailyGoalMinutes() const { return dailyGoal <= 0 || dailyGoal > 4 ? 0 : dailyGoal * 60; }

    int RemindMinutes() const {
        static const int m[] = {0, 30, 60, 120};
        return m[remindInterval < 0 || remindInterval > 3 ? 2 : remindInterval];
    }

    SortMode sort = SortMode::Recent;
    int viewMode = 0;        // library: 0 grid, 1 list
    std::string lastSelected;

    std::string steamApiKey;  // kept decrypted in memory, stored with DPAPI

    bool Load();
    bool Save() const;
};

}  // namespace lumen
