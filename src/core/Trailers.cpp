#include "Trailers.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <vector>

#include "Http.h"
#include "Util.h"

namespace lumen {

namespace {
constexpr double kClipSeconds = 45.0;  // enough for a looping background, small on disk (~5-10 MB)
constexpr int kTargetHeight = 720;

std::wstring CacheDir() { return AppSubDir(L"cache\\video"); }

// Failures are logged (only failures) so missing trailers can be diagnosed.
bool Fail(const std::string& why, const std::wstring& url) {
    std::wstring log = JoinPath(AppDataDir(), L"trailers.log");
    std::string prev;
    ReadFileBytes(log, prev);
    if (prev.size() > 64 * 1024) prev.clear();
    WriteFileAtomic(log, prev + why + " " + Narrow(url) + "\n");
    return false;
}

std::wstring FileFor(const std::string& id) {
    std::string safe = id;
    for (auto& c : safe)
        if (c == ':' || c == '/' || c == '\\') c = '_';
    return JoinPath(CacheDir(), Widen(safe) + L".mp4");
}

// "https://host/a/b/master.m3u8?t=1" + "x/y.m4s" -> "https://host/a/b/x/y.m4s"
std::wstring Resolve(const std::wstring& base, const std::wstring& rel) {
    if (StartsWith(rel, L"http")) return rel;
    std::wstring b = base.substr(0, base.find(L'?'));
    return b.substr(0, b.find_last_of(L'/') + 1) + rel;
}

std::vector<std::wstring> Lines(const std::string& text) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        out.push_back(Widen(line));
        start = end + 1;
    }
    return out;
}

std::wstring Attr(const std::wstring& line, const std::wstring& key) {  // KEY=value or KEY="value"
    size_t p = line.find(key + L"=");
    if (p == std::wstring::npos) return {};
    p += key.size() + 1;
    if (p < line.size() && line[p] == L'"') {
        size_t e = line.find(L'"', p + 1);
        return line.substr(p + 1, e == std::wstring::npos ? std::wstring::npos : e - p - 1);
    }
    size_t e = line.find(L',', p);
    return line.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
}
}  // namespace

void TrailerService::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    TrimCache(CacheDir(), 40);
    thread_ = std::thread([this] { Run(); });
}

void TrailerService::Stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::wstring TrailerService::Get(const std::string& id, const std::wstring& url) {
    if (id.empty() || url.empty()) return {};
    std::wstring path = FileFor(id);
    std::lock_guard<std::mutex> lk(m_);
    auto it = state_.find(id);
    if (it != state_.end()) return it->second == State::Ready ? path : std::wstring();
    if (FileExists(path)) {
        state_[id] = State::Ready;
        return path;
    }
    if (!online_) return {};
    state_[id] = State::Pending;
    jobs_.push_front(Job{id, url, path});  // the game on screen first
    while (jobs_.size() > 3) {             // older requests are no longer on screen
        state_.erase(jobs_.back().id);
        jobs_.pop_back();
    }
    cv_.notify_one();
    return {};
}

void TrailerService::Run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        bool ok = Download(job.url, job.path);
        std::lock_guard<std::mutex> lk(m_);
        state_[job.id] = ok ? State::Ready : State::Failed;
    }
}

bool TrailerService::Download(const std::wstring& url, const std::wstring& outPath) {
    HttpResponse master = HttpGet(url, 15000, 1u << 20);
    if (!master.ok()) return Fail("master http " + std::to_string(master.status), url);
    if (master.body.find("#EXTM3U") == std::string::npos) return Fail("master not m3u8", url);

    // Pick the variant closest to 720p (or the smallest one if all are bigger).
    std::wstring variant;
    int bestH = -1, smallestH = INT_MAX;
    std::wstring smallest;
    auto lines = Lines(master.body);
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (!StartsWith(lines[i], L"#EXT-X-STREAM-INF")) continue;
        std::wstring res = Attr(lines[i], L"RESOLUTION");
        int h = 0;
        size_t x = res.find(L'x');
        if (x != std::wstring::npos) h = _wtoi(res.c_str() + x + 1);
        const std::wstring& uri = lines[i + 1];
        if (uri.empty() || uri[0] == L'#') continue;
        if (h <= kTargetHeight && h > bestH) {
            bestH = h;
            variant = uri;
        }
        if (h < smallestH) {
            smallestH = h;
            smallest = uri;
        }
    }
    if (variant.empty()) variant = smallest;
    if (variant.empty()) return Fail("no variant", url);
    std::wstring variantUrl = Resolve(url, variant);

    HttpResponse playlist = HttpGet(variantUrl, 15000, 1u << 20);
    if (!playlist.ok()) return Fail("variant http " + std::to_string(playlist.status), variantUrl);
    std::wstring init;
    std::vector<std::wstring> all;
    std::vector<double> durations;
    double pending = 0;
    for (const auto& line : Lines(playlist.body)) {
        if (StartsWith(line, L"#EXT-X-MAP")) init = Attr(line, L"URI");
        else if (StartsWith(line, L"#EXTINF:")) pending = _wtof(line.c_str() + 8);
        else if (!line.empty() && line[0] != L'#') {
            all.push_back(Resolve(variantUrl, line));
            durations.push_back(pending > 0 ? pending : 3.0);
        }
    }
    // Trailers usually open with age-rating and publisher cards: skip the first ~6 s when the trailer is long.
    size_t first = 0;
    for (double skipped = 0; all.size() > 8 && first < all.size() && skipped < 6.0; ++first) skipped += durations[first];
    std::vector<std::wstring> segments;
    double total = 0;
    for (size_t i = first; i < all.size() && total < kClipSeconds; ++i) {
        segments.push_back(all[i]);
        total += durations[i];
    }
    if (init.empty() || segments.empty()) return Fail("not fmp4", variantUrl);  // only fragmented-MP4 playlists

    std::string data;
    HttpResponse head = HttpGet(Resolve(variantUrl, init), 15000, 1u << 20);
    if (!head.ok()) return Fail("init http " + std::to_string(head.status), variantUrl);
    data += head.body;
    for (const auto& s : segments) {
        if (!online_) return false;
        HttpResponse seg = HttpGet(s, 20000, 16u << 20);
        if (!seg.ok()) break;  // keep what we have if a later chunk fails
        data += seg.body;
    }
    if (data.size() < head.body.size() + 64 * 1024) return false;
    return WriteFileAtomic(outPath, data);
}

void TrailerService::TrimCache(const std::wstring& dir, size_t keep) {
    std::vector<std::pair<int64_t, std::wstring>> files;
    for (const auto& f : ListFiles(dir, L"*.mp4")) {
        std::wstring p = JoinPath(dir, f);
        files.emplace_back(FileModifiedUnix(p), p);
    }
    if (files.size() <= keep) return;
    std::sort(files.begin(), files.end());  // oldest first
    for (size_t i = 0; i + keep < files.size(); ++i) DeleteFileW(files[i].second.c_str());
}

}  // namespace lumen
