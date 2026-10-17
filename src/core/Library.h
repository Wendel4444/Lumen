#pragma once
// Aggregates every provider into one list of games, merges the user's own data and answers
// search / filter / sort queries. All mutations go through here so persistence stays consistent.
#include <string>
#include <vector>

#include "Game.h"
#include "Settings.h"
#include "UserStore.h"
#include "providers/Steam.h"

namespace lumen {

enum class Filter { All, Favorites, Steam, Epic, Gog, Custom, Hidden, Category, Apps, Status };

struct LibraryQuery {
    std::wstring search;
    Filter filter = Filter::All;
    std::wstring category;
    PlayStatus status = PlayStatus::None;  // for Filter::Status
    SortMode sort = SortMode::Recent;
};

enum class ArtKind { Cover, Hero, Logo };

class Library {
public:
    Library(Settings& settings, UserStore& user) : settings_(settings), user_(user) {}

    void Refresh();          // full rescan of every enabled provider
    void RefreshPlaytime();  // cheap: re-read Steam playtime/achievement counts

    std::vector<Game>& games() { return games_; }
    const std::vector<Game>& games() const { return games_; }
    Game* Find(const std::string& id);
    int IndexOf(const std::string& id) const;

    std::vector<int> Query(const LibraryQuery& q) const;
    std::vector<std::wstring> Categories() const;  // user categories + platform collections
    int CountWhere(Filter f) const;
    int CountStatus(PlayStatus s) const;

    void SetStatus(Game& g, PlayStatus s);
    void SetNotes(Game& g, const std::wstring& notes);

    void SetFavorite(Game& g, bool on);
    void SetHidden(Game& g, bool on);
    void ToggleCategory(Game& g, const std::wstring& category);
    bool CreateCategory(const std::wstring& name);
    bool AddCustomGame(const std::wstring& exe, std::string* newId);
    bool RemoveCustomGame(const std::string& id);
    bool RenameCustomGame(const std::string& id, const std::wstring& name);
    bool SetArt(Game& g, ArtKind kind, const std::wstring& sourceFile);  // copies the file into Lumen's data folder
    void ClearArt(Game& g);
    void RecordLaunch(Game& g);
    void RecordSession(const std::string& id, int64_t seconds);

    const SteamProvider& steam() const { return steam_; }
    bool consumeDirty() {
        bool d = dirty_;
        dirty_ = false;
        return d;
    }
    int revision() const { return revision_; }

private:
    void ApplyUser(Game& g);
    Game MakeCustom(const CustomGame& c) const;

    Settings& settings_;
    UserStore& user_;
    SteamProvider steam_;
    std::vector<Game> games_;
    bool dirty_ = false;
    int revision_ = 0;
};

// Extracts the large icon of an executable into a PNG (used as art for manual games).
bool ExtractExeIcon(const std::wstring& exe, const std::wstring& pngOut);

}  // namespace lumen
