#pragma once
// Background service for data that is slow or remote: store details (description, genres, release date)
// and achievements. Results are cached on disk and handed to the UI thread through Poll().
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Game.h"
#include "Json.h"

namespace lumen {

class Library;

struct NewsItem {
    std::wstring title, url, snippet;
    int64_t date = 0;  // unix
};

struct MetadataConfig {
    bool online = true;
    std::string steamApiKey;
    std::string steamId64;
};

class MetadataService {
public:
    ~MetadataService() { Stop(); }
    void Start();
    void Stop();
    void SetConfig(const MetadataConfig& cfg);

    void RequestDetails(const Game& g);
    void RequestAchievements(const Game& g, const std::wstring& steamClientCacheFile, bool force = false);
    void RequestNews(const Game& g);
    void ForgetAll();  // next requests hit sources again (used by "Atualizar")

    // UI thread only.
    bool Poll(Library& lib);
    void ReapplyDetails(Library& lib);
    const AchievementSet* Achievements(const std::string& gameId) const;
    const std::vector<NewsItem>* News(const std::string& gameId) const;

private:
    struct Job {
        enum class Kind { SteamDetails, GogDetails, SteamAchievements, SteamNews } kind;
        std::string gameId, platformId;
        std::wstring cacheFile;
        bool force = false;
    };
    struct Details {
        std::string gameId;
        std::wstring description, developer, publisher, releaseDate, genres, header, hero, cover, logo, trailer;
    };

    void Run();
    void Enqueue(Job job);
    Details FetchSteamDetails(const Job& j, const MetadataConfig& cfg);
    Details FetchGogDetails(const Job& j, const MetadataConfig& cfg);
    AchievementSet FetchSteamAchievements(const Job& j, const MetadataConfig& cfg);
    std::vector<NewsItem> FetchSteamNews(const Job& j, const MetadataConfig& cfg);
    bool FetchCachedJson(const std::wstring& url, const std::string& cacheName, const MetadataConfig& cfg,
                         json::Value& out);
    bool Throttled();
    void NoteStatus(int status);

    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    bool stop_ = false;
    MetadataConfig cfg_;
    std::vector<Details> doneDetails_;
    std::vector<std::pair<std::string, AchievementSet>> doneAch_;
    std::vector<std::pair<std::string, std::vector<NewsItem>>> doneNews_;
    int64_t backoffUntil_ = 0;  // worker only

    // UI thread state
    std::set<std::string> askedDetails_, askedAch_, askedNews_;
    std::map<std::string, std::vector<NewsItem>> news_;
    std::map<std::string, Details> details_;
    std::map<std::string, AchievementSet> ach_;
};

}  // namespace lumen
