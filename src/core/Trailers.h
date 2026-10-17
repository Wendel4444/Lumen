#pragma once
// Store trailers for animated backgrounds. Steam publishes trailers as HLS playlists of fragmented-MP4
// segments; this service picks the ~720p variant, downloads the init segment plus the first ~45 seconds
// and concatenates them into a regular local .mp4 that Media Foundation can play (and loop) offline.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace lumen {

class TrailerService {
public:
    ~TrailerService() { Stop(); }
    void Start();
    void Stop();
    void SetOnline(bool on) { online_ = on; }

    // Local file of the trailer when it is ready; otherwise schedules the download (once) and returns "".
    std::wstring Get(const std::string& gameId, const std::wstring& playlistUrl);

private:
    enum class State { Pending, Ready, Failed };
    struct Job {
        std::string id;
        std::wstring url, path;
    };
    void Run();
    bool Download(const std::wstring& url, const std::wstring& outPath);
    static void TrimCache(const std::wstring& dir, size_t keep);

    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::map<std::string, State> state_;
    bool stop_ = false;
    std::atomic<bool> online_{true};
};

}  // namespace lumen
