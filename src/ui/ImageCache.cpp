#include "ImageCache.h"

#include <objbase.h>

#include <algorithm>
#include <atomic>

#include "core/Http.h"
#include "core/Util.h"

namespace lumen::ui {

namespace {

constexpr int kDefaultMaxDim = 3840;
constexpr size_t kMaxQueue = 48;
constexpr int64_t kMissRetrySeconds = 3 * 86400;

std::atomic<bool> g_online{true};

struct DecodeJob {
    std::wstring src, path;
    int maxDim;
};

std::wstring DiskCachePath(const std::wstring& url) {
    std::wstring clean = url.substr(0, url.find(L'?'));
    std::wstring ext = FileExt(clean);
    if (ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".webp") ext = L".img";
    static const std::wstring dir = AppSubDir(L"cache\\img");
    return JoinPath(dir, Widen(HexId(Hash64(Narrow(url)))) + ext);
}

bool DecodeFile(IWICImagingFactory* wic, const std::wstring& path, int maxDim, UINT& w, UINT& h,
                std::vector<uint8_t>& px) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &dec)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;

    ComPtr<IWICBitmapSource> src = frame;
    // Convert first so scaling happens in premultiplied space (no dark fringes around logos).
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)))
        return false;
    src = conv;
    int limit = maxDim > 0 ? maxDim : kDefaultMaxDim;
    if (static_cast<int>(std::max(w, h)) > limit) {
        float k = static_cast<float>(limit) / std::max(w, h);
        UINT nw = std::max<UINT>(1, static_cast<UINT>(w * k)), nh = std::max<UINT>(1, static_cast<UINT>(h * k));
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(src.Get(), nw, nh, WICBitmapInterpolationModeFant))) {
            src = scaler;
            w = nw;
            h = nh;
        }
    }
    px.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(src->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()));
}

// Crops rows/columns whose alpha is (almost) zero, keeping a 2 px margin.
void TrimTransparent(std::vector<uint8_t>& px, UINT& w, UINT& h) {
    UINT x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = &px[static_cast<size_t>(y) * w * 4];
        for (UINT x = 0; x < w; ++x) {
            if (row[x * 4 + 3] > 10) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x0 > x1 || y0 > y1) return;  // fully transparent: leave as is
    x0 = x0 > 2 ? x0 - 2 : 0;
    y0 = y0 > 2 ? y0 - 2 : 0;
    x1 = std::min(w - 1, x1 + 2);
    y1 = std::min(h - 1, y1 + 2);
    UINT nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    if (nw == w && nh == h) return;
    std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4);
    for (UINT y = 0; y < nh; ++y)
        memcpy(&out[static_cast<size_t>(y) * nw * 4], &px[(static_cast<size_t>(y + y0) * w + x0) * 4], nw * 4);
    px.swap(out);
    w = nw;
    h = nh;
}

Color AverageColor(const std::vector<uint8_t>& px, UINT w, UINT h) {
    double r = 0, g = 0, b = 0, a = 0;
    const UINT steps = 24;
    for (UINT yi = 0; yi < steps; ++yi) {
        UINT y = h * yi / steps;
        for (UINT xi = 0; xi < steps; ++xi) {
            UINT x = w * xi / steps;
            const uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            b += p[0];
            g += p[1];
            r += p[2];
            a += p[3];
        }
    }
    if (a <= 0) return {0, 0, 0, 0};
    return {static_cast<float>(r / a), static_cast<float>(g / a), static_cast<float>(b / a), 1.f};
}

}  // namespace

void ImageCache::Start() {
    if (!threads_.empty()) return;
    stop_ = false;
    for (int i = 0; i < 2; ++i) threads_.emplace_back([this] { DecodeWorker(); });
    for (int i = 0; i < 2; ++i) threads_.emplace_back([this] { NetWorker(); });
}

void ImageCache::Stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        decodeQ_.clear();
        netQ_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

void ImageCache::Push(std::deque<Request>& q, Request r) {
    // Newest first: whatever is on screen now matters most. Stale requests fall off the back.
    q.push_front(std::move(r));
    while (q.size() > kMaxQueue) {
        Result dropped;
        const std::wstring& s = q.back().src;
        dropped.src = s.substr(0, s.find(L'\n'));
        dropped.dropped = true;
        q.pop_back();
        done_.push_back(std::move(dropped));
    }
}

ID2D1Bitmap1* ImageCache::Get(const std::wstring& src, int maxDim, bool trim) {
    if (src.empty()) return nullptr;
    auto it = entries_.find(src);
    if (it != entries_.end()) {
        it->second.lastUse = now_;
        return it->second.state == State::Ready ? it->second.bmp.Get() : nullptr;
    }
    Entry& e = entries_[src];
    e.lastUse = now_;
    g_online = online_;
    Request r{src, maxDim, StartsWith(src, L"http"), trim};
    {
        std::lock_guard<std::mutex> lk(m_);
        Push(r.network ? netQ_ : decodeQ_, std::move(r));
    }
    cv_.notify_all();
    return nullptr;
}

bool ImageCache::Failed(const std::wstring& src) const {
    if (src.empty()) return true;
    auto it = entries_.find(src);
    return it != entries_.end() && it->second.state == State::Failed;
}

Color ImageCache::Average(const std::wstring& src) const {
    auto it = entries_.find(src);
    return it != entries_.end() ? it->second.avg : Color{0, 0, 0, 0};
}

void ImageCache::Finish(Result r) {
    std::lock_guard<std::mutex> lk(m_);
    done_.push_back(std::move(r));
}

void ImageCache::Tick(Gfx& g) {
    now_ = g.now;
    if (g.generation() != generation_) {
        // New GPU device: every bitmap is invalid. Drop them; they reload on demand.
        generation_ = g.generation();
        for (auto it = entries_.begin(); it != entries_.end();)
            it = it->second.state == State::Ready ? entries_.erase(it) : std::next(it);
    }
    std::vector<Result> batch;
    {
        std::lock_guard<std::mutex> lk(m_);
        batch.swap(done_);
    }
    int uploads = 0;
    std::vector<Result> later;
    for (auto& r : batch) {
        auto it = entries_.find(r.src);
        if (it == entries_.end()) continue;
        Entry& e = it->second;
        if (r.dropped) {
            if (e.state == State::Queued) entries_.erase(it);
            continue;
        }
        if (!r.ok) {
            e.state = State::Failed;
            continue;
        }
        if (now_ - e.lastUse > 10.0) {  // nobody needs it anymore
            entries_.erase(it);
            continue;
        }
        if (uploads >= 4) {  // spread GPU uploads over frames to avoid hitches
            later.push_back(std::move(r));
            continue;
        }
        ID2D1Bitmap1* bmp = g.CreateBitmap(r.w, r.h, r.px.data(), r.w * 4);
        if (!bmp) {
            e.state = State::Failed;
            continue;
        }
        e.bmp.Attach(bmp);
        e.state = State::Ready;
        e.avg = r.avg;
        ++uploads;
    }
    if (!later.empty()) {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& r : later) done_.push_back(std::move(r));
    }
    if (now_ - lastEvict_ > 1.0) {
        lastEvict_ = now_;
        for (auto it = entries_.begin(); it != entries_.end();)
            it = (it->second.state == State::Ready && now_ - it->second.lastUse > 8.0) ? entries_.erase(it)
                                                                                         : std::next(it);
    }
}

void ImageCache::DecodeWorker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        for (;;) {
            Request req;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || !decodeQ_.empty(); });
                if (stop_) break;
                req = std::move(decodeQ_.front());
                decodeQ_.pop_front();
            }
            Result r;
            r.src = req.src;
            // Network requests arrive here with the downloaded file path appended after '\n'.
            std::wstring path = req.src;
            size_t nl = req.src.find(L'\n');
            if (nl != std::wstring::npos) {
                r.src = req.src.substr(0, nl);
                path = req.src.substr(nl + 1);
            }
            r.ok = wic && DecodeFile(wic.Get(), path, req.maxDim, r.w, r.h, r.px);
            if (r.ok && req.trim) {
                // A "logo" without any transparency is a banner (some stores ship those): reject it so the
                // UI falls back to typography instead of drawing a box over the artwork.
                bool anyTransparent = false;
                for (size_t i = 3; i < r.px.size() && !anyTransparent; i += 4) anyTransparent = r.px[i] < 16;
                if (anyTransparent) TrimTransparent(r.px, r.w, r.h);
                else r.ok = false;
            }
            if (r.ok) r.avg = AverageColor(r.px, r.w, r.h);
            else if (nl != std::wstring::npos) {
                DeleteFileW(path.c_str());  // corrupt download: remember the miss instead
                WriteFileAtomic(path + L".miss", "");
            }
            Finish(std::move(r));
        }
    }
    CoUninitialize();
}

void ImageCache::NetWorker() {
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || !netQ_.empty(); });
            if (stop_) return;
            req = std::move(netQ_.front());
            netQ_.pop_front();
        }
        std::wstring path = DiskCachePath(req.src);
        bool have = FileExists(path);
        if (!have) {
            int64_t miss = FileModifiedUnix(path + L".miss");
            bool recentMiss = miss > 0 && NowUnix() - miss < kMissRetrySeconds;
            if (!recentMiss && g_online) {
                HttpResponse resp = HttpGet(req.src, 15000);
                if (resp.ok() && resp.body.size() > 64 && WriteFileAtomic(path, resp.body)) {
                    have = true;
                    DeleteFileW((path + L".miss").c_str());
                } else if (resp.status == 404 || resp.status == 403) {
                    WriteFileAtomic(path + L".miss", "");
                }
            }
        }
        if (!have) {
            Result r;
            r.src = req.src;
            Finish(std::move(r));
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            Push(decodeQ_, Request{req.src + L"\n" + path, req.maxDim, false, req.trim});
        }
        cv_.notify_all();
    }
}

}  // namespace lumen::ui
