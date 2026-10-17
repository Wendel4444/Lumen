#pragma once
// Game deals from third-party stores. There is no official deals API for these shops, so Lumen reads
// only what each store's public home page already shows (allowed by their robots.txt), identifies itself
// honestly, caches the result for a few hours and never retries pages that refuse automated access.
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lumen {

struct Deal {
    std::wstring title;
    std::wstring url;
    std::wstring image;
    std::wstring platform;   // "Steam · Europe", "PC (EA App)"...
    std::wstring currency;   // "R$", "€"...
    double price = 0;
    double oldPrice = 0;     // 0 when unknown
    int discount = 0;        // percent
    bool approx = false;     // converted from another currency
};

struct StoreDeals {
    enum class State { Idle, Loading, Ready, Unsupported, Error };
    std::string id;          // "gmg", "ig", "driffle", "nuuvem", "eneba"
    std::wstring name;
    std::wstring homeUrl;
    State state = State::Idle;
    std::wstring message;    // why there are no deals (unsupported store, offline...)
    std::vector<Deal> deals;
    int64_t fetchedAt = 0;
};

class DealsService {
public:
    DealsService();
    ~DealsService() { Stop(); }
    void Start();
    void Stop();
    void SetOnline(bool on);
    void Refresh(bool force);  // fetch stores whose cache is stale (or all, when forced)

    // UI thread: snapshot of the current state; true when something changed since last call.
    bool Poll(std::vector<StoreDeals>& out);

private:
    void Run();
    void FetchStore(StoreDeals& s, bool force);
    bool LoadCache(StoreDeals& s, int64_t maxAge);
    void SaveCache(const StoreDeals& s);
    double EurToBrl();

    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<StoreDeals> stores_;
    bool stop_ = false, pending_ = false, force_ = false, changed_ = true, online_ = true;
    double eurBrl_ = 0;
    int64_t eurFetchedAt_ = 0;
};

}  // namespace lumen
