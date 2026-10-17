#pragma once
// Asynchronous image pipeline: local files and http(s) URLs are downloaded (disk-cached), decoded
// with WIC on worker threads, uploaded to the GPU on the UI thread and evicted when unused.
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Gfx.h"

namespace lumen::ui {

class ImageCache {
public:
    ~ImageCache() { Stop(); }
    void Start();
    void Stop();
    void SetOnline(bool on) { online_ = on; }

    // Returns the GPU bitmap when ready, nullptr while loading or on failure. maxDim limits decode size;
    // trim crops fully transparent margins (logos often ship with large empty borders).
    ID2D1Bitmap1* Get(const std::wstring& src, int maxDim = 0, bool trim = false);
    bool Failed(const std::wstring& src) const;
    Color Average(const std::wstring& src) const;  // a == 0 when unknown

    void Tick(Gfx& g);  // upload finished decodes, evict unused entries

private:
    enum class State { Queued, Ready, Failed };
    struct Entry {
        State state = State::Queued;
        ComPtr<ID2D1Bitmap1> bmp;
        Color avg{0, 0, 0, 0};
        double lastUse = 0;
    };
    struct Request {
        std::wstring src;
        int maxDim = 0;
        bool network = false;
        bool trim = false;
    };
    struct Result {
        std::wstring src;
        bool ok = false;
        bool dropped = false;
        UINT w = 0, h = 0;
        std::vector<uint8_t> px;
        Color avg;
    };

    void DecodeWorker();
    void NetWorker();
    void Push(std::deque<Request>& q, Request r);
    void Finish(Result r);

    std::unordered_map<std::wstring, Entry> entries_;
    double now_ = 0, lastEvict_ = 0;
    uint32_t generation_ = 0;
    bool online_ = true;

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Request> decodeQ_, netQ_;
    std::vector<Result> done_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

}  // namespace lumen::ui
