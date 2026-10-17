#pragma once
// Plays a local video (silently, looping) for animated backgrounds. Decoding runs on a worker thread with
// Media Foundation's Source Reader (H.264 -> RGB32); the newest frame is copied into a GPU bitmap on demand.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Gfx.h"

namespace lumen::ui {

class VideoPlayer {
public:
    ~VideoPlayer() { Close(); }
    static void Startup();   // once per process (MFStartup)
    static void Shutdown();

    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const { return thread_.joinable(); }
    bool Failed() const { return failed_; }
    const std::wstring& Path() const { return path_; }

    // Newest decoded frame as a bitmap (nullptr until the first frame is ready).
    ID2D1Bitmap1* Frame(Gfx& g);

private:
    void Decode();

    std::wstring path_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};

    std::mutex m_;
    std::vector<uint8_t> pixels_;  // BGRX, tightly packed
    UINT w_ = 0, h_ = 0;
    bool fresh_ = false;

    ComPtr<ID2D1Bitmap1> bmp_;  // UI thread only
    UINT bmpW_ = 0, bmpH_ = 0;
    uint32_t gen_ = 0;
    bool hasFrame_ = false;
};

}  // namespace lumen::ui
