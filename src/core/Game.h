#pragma once
// Domain model shared by providers, library and UI.
#include <cstdint>
#include <string>
#include <vector>

namespace lumen {

enum class Platform { Steam, Epic, GOG, Custom };

inline const wchar_t* PlatformName(Platform p) {
    switch (p) {
        case Platform::Steam: return L"Steam";
        case Platform::Epic: return L"Epic Games";
        case Platform::GOG: return L"GOG";
        default: return L"Manual";
    }
}

struct Achievement {
    std::string id;
    std::wstring name;
    std::wstring description;
    std::wstring icon;       // URL or local path
    std::wstring iconGray;   // locked icon, when the source provides one
    bool achieved = false;
    bool hidden = false;
    int64_t unlockedAt = 0;  // unix
    float globalPercent = -1.f;
};

struct AchievementSet {
    enum class State { Unknown, Loading, Ready, Unsupported, NoAchievements, NotSynced, Error };
    State state = State::Unknown;
    int unlocked = 0;
    int total = 0;
    bool complete = false;        // list contains every achievement of the game
    std::wstring source;          // human readable data source
    std::wstring message;         // explanation when unavailable
    std::vector<Achievement> list;
};

struct Game {
    // Identity (stable across runs, used as persistence key): "steam:460790", "epic:Fortnite", "custom:ab12..."
    std::string id;
    Platform platform = Platform::Custom;
    std::string platformId;  // appid / AppName / GOG product id

    std::wstring name;
    std::wstring installDir;
    std::wstring exePath;
    std::wstring args;
    std::wstring launchUri;
    uint64_t sizeBytes = 0;
    int64_t installedAt = 0;

    // Platform statistics (-1 = unknown)
    int64_t playMinutes = -1;
    int64_t lastPlayed = 0;
    int achUnlocked = -1;
    int achTotal = -1;

    // Art: local file path or http(s) URL. Empty = generate.
    std::wstring cover;   // portrait 2:3
    std::wstring hero;    // wide background
    std::wstring logo;    // transparent title logo
    std::wstring header;  // landscape capsule
    std::wstring icon;    // executable icon (custom games)
    std::wstring trailer; // HLS playlist of the store trailer (Steam)

    // Descriptive data (local cache first, then online)
    std::wstring description;
    std::wstring developer;
    std::wstring publisher;
    std::wstring releaseDate;
    std::wstring genres;

    std::vector<std::wstring> platformCollections;  // e.g. Steam collections
    bool platformFavorite = false;
    bool isApp = false;  // software/tool according to the platform (e.g. Steam "Application")

    // User data merged from UserStore
    bool favorite = false;
    bool hidden = false;
    std::vector<std::wstring> categories;
    int64_t addedAt = 0;
    int launches = 0;
    int64_t lastLaunch = 0;
    int64_t trackedSeconds = 0;  // sessions measured by Lumen
    int status = 0;              // PlayStatus (see UserStore.h)
    std::wstring notes;

    // Derived helpers
    int64_t TotalMinutes() const {
        int64_t m = playMinutes > 0 ? playMinutes : 0;
        if (platform != Platform::Steam) m += trackedSeconds / 60;
        return (playMinutes < 0 && trackedSeconds == 0) ? -1 : m;
    }
    int64_t LastActivity() const { return lastPlayed > lastLaunch ? lastPlayed : lastLaunch; }
};

}  // namespace lumen
