#pragma once
// Everything the user decides about their library: favorites, categories, hidden games, play status,
// notes, manually added executables, art overrides, launch history and observed play sessions.
// Persisted as library.json.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lumen {

// Personal progress status (backlog). Order matters: it is persisted as an integer.
enum class PlayStatus { None = 0, Queued = 1, Playing = 2, Finished = 3, Dropped = 4 };

inline const wchar_t* PlayStatusName(PlayStatus s) {
    switch (s) {
        case PlayStatus::Queued: return L"Na fila";
        case PlayStatus::Playing: return L"Jogando";
        case PlayStatus::Finished: return L"Zerado";
        case PlayStatus::Dropped: return L"Abandonado";
        default: return L"Sem status";
    }
}

struct UserGameData {
    bool favorite = false;
    bool hidden = false;
    std::vector<std::wstring> categories;
    int64_t addedAt = 0;
    int launches = 0;
    int64_t lastLaunch = 0;
    int64_t trackedSeconds = 0;
    PlayStatus status = PlayStatus::None;
    std::wstring notes;
    std::wstring coverOverride, heroOverride, logoOverride;
};

struct CustomGame {
    std::string id;
    std::wstring name;
    std::wstring exe;
    std::wstring args;
    int64_t addedAt = 0;
};

struct HistoryEntry {
    std::string id;
    int64_t time = 0;
    int64_t seconds = 0;  // measured session length, 0 if unknown
};

// A play session observed by Lumen (Steam game detected running, or an executable it started).
struct PlaySession {
    std::string id;
    int64_t start = 0;    // unix
    int64_t seconds = 0;
};

class UserStore {
public:
    bool Load();
    bool Save() const;

    UserGameData& Get(const std::string& id) { return games[id]; }
    const UserGameData* Find(const std::string& id) const;

    void RecordLaunch(const std::string& id, int64_t when);
    void RecordSession(const std::string& id, int64_t seconds);

    // Seconds played in [from, to) according to recorded sessions (sessions are clipped to the range).
    int64_t SecondsBetween(int64_t from, int64_t to) const;

    std::map<std::string, UserGameData> games;
    std::vector<std::wstring> categories;
    std::vector<CustomGame> custom;
    std::vector<HistoryEntry> history;  // newest first, capped
    std::vector<PlaySession> sessions;  // oldest first, capped
};

}  // namespace lumen
