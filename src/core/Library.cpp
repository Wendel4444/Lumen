#include "Library.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

#include "Util.h"
#include "providers/Epic.h"

using Microsoft::WRL::ComPtr;

namespace lumen {

namespace {
std::wstring ArtDir(const std::string& id) {
    std::string safe = id;
    for (auto& c : safe)
        if (c == ':' || c == '/' || c == '\\') c = '_';
    std::wstring d = JoinPath(AppSubDir(L"art"), Widen(safe));
    EnsureDir(d);
    return d;
}

bool Contains(const std::vector<std::wstring>& v, const std::wstring& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
}  // namespace

Game Library::MakeCustom(const CustomGame& c) const {
    Game g;
    g.id = c.id;
    g.platform = Platform::Custom;
    g.platformId = c.id.substr(c.id.find(':') + 1);
    g.name = c.name;
    g.exePath = c.exe;
    g.args = c.args;
    g.installDir = ParentDir(c.exe);
    g.installedAt = c.addedAt;
    std::wstring icon = JoinPath(ArtDir(c.id), L"icon.png");
    if (FileExists(icon)) g.icon = icon;
    return g;
}

void Library::ApplyUser(Game& g) {
    const UserGameData* existing = user_.Find(g.id);
    if (!existing) {
        UserGameData& d = user_.Get(g.id);
        d.addedAt = g.installedAt > 0 ? g.installedAt : NowUnix();
        d.favorite = g.platformFavorite;  // first sight: inherit the platform's favorite flag
        dirty_ = true;
        existing = &d;
    }
    const UserGameData& d = *existing;
    g.favorite = d.favorite;
    g.hidden = d.hidden;
    g.categories = d.categories;
    g.addedAt = d.addedAt;
    g.launches = d.launches;
    g.lastLaunch = d.lastLaunch;
    g.trackedSeconds = d.trackedSeconds;
    g.status = static_cast<int>(d.status);
    g.notes = d.notes;
    if (!d.coverOverride.empty() && FileExists(d.coverOverride)) g.cover = d.coverOverride;
    if (!d.heroOverride.empty() && FileExists(d.heroOverride)) g.hero = d.heroOverride;
    if (!d.logoOverride.empty() && FileExists(d.logoOverride)) g.logo = d.logoOverride;
}

void Library::Refresh() {
    std::vector<Game> fresh;
    if (settings_.sourceSteam) steam_.Scan(fresh, settings_.onlineData);
    if (settings_.sourceEpic) EpicProvider().Scan(fresh);
    if (settings_.sourceGog) GogProvider().Scan(fresh);
    for (const auto& c : user_.custom) fresh.push_back(MakeCustom(c));

    // Keep descriptive data fetched online during this session.
    for (auto& g : fresh) {
        if (const Game* old = Find(g.id)) {
            if (g.description.empty()) g.description = old->description;
            if (g.developer.empty()) g.developer = old->developer;
            if (g.publisher.empty()) g.publisher = old->publisher;
            if (g.releaseDate.empty()) g.releaseDate = old->releaseDate;
            if (g.genres.empty()) g.genres = old->genres;
            if (g.header.empty()) g.header = old->header;
            if (g.hero.empty()) g.hero = old->hero;
            if (g.cover.empty()) g.cover = old->cover;
            if (g.logo.empty()) g.logo = old->logo;
            if (g.trailer.empty()) g.trailer = old->trailer;
        }
        ApplyUser(g);
    }
    games_ = std::move(fresh);
    ++revision_;
}

void Library::RefreshPlaytime() {
    steam_.RefreshPlaytime(games_);
    ++revision_;
}

Game* Library::Find(const std::string& id) {
    for (auto& g : games_)
        if (g.id == id) return &g;
    return nullptr;
}

int Library::IndexOf(const std::string& id) const {
    for (size_t i = 0; i < games_.size(); ++i)
        if (games_[i].id == id) return static_cast<int>(i);
    return -1;
}

static bool Matches(const Game& g, const LibraryQuery& q, bool showApps) {
    if (q.filter == Filter::Hidden) {
        if (!g.hidden) return false;
    } else if (g.hidden) {
        return false;
    }
    if (q.filter == Filter::Apps) {
        if (!g.isApp) return false;
    } else if (g.isApp && !showApps && q.filter != Filter::Hidden) {
        return false;
    }
    switch (q.filter) {
        case Filter::Favorites: if (!g.favorite) return false; break;
        case Filter::Steam: if (g.platform != Platform::Steam) return false; break;
        case Filter::Epic: if (g.platform != Platform::Epic) return false; break;
        case Filter::Gog: if (g.platform != Platform::GOG) return false; break;
        case Filter::Custom: if (g.platform != Platform::Custom) return false; break;
        case Filter::Category:
            if (!Contains(g.categories, q.category) && !Contains(g.platformCollections, q.category)) return false;
            break;
        case Filter::Status:
            if (g.status != static_cast<int>(q.status)) return false;
            break;
        default: break;
    }
    if (!q.search.empty()) {
        if (!ContainsInsensitive(g.name, q.search) && !ContainsInsensitive(g.developer, q.search) &&
            !ContainsInsensitive(g.genres, q.search))
            return false;
    }
    return true;
}

std::vector<int> Library::Query(const LibraryQuery& q) const {
    std::vector<int> idx;
    for (size_t i = 0; i < games_.size(); ++i)
        if (Matches(games_[i], q, settings_.showApps)) idx.push_back(static_cast<int>(i));
    auto byName = [&](int a, int b) { return CompareNatural(games_[a].name, games_[b].name) < 0; };
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        const Game& x = games_[a];
        const Game& y = games_[b];
        switch (q.sort) {
            case SortMode::Name: return byName(a, b);
            case SortMode::Playtime: {
                int64_t mx = x.TotalMinutes(), my = y.TotalMinutes();
                if (mx != my) return mx > my;
                return byName(a, b);
            }
            case SortMode::Added:
                if (x.addedAt != y.addedAt) return x.addedAt > y.addedAt;
                return byName(a, b);
            default:
                if (x.LastActivity() != y.LastActivity()) return x.LastActivity() > y.LastActivity();
                return byName(a, b);
        }
    });
    return idx;
}

std::vector<std::wstring> Library::Categories() const {
    std::vector<std::wstring> out = user_.categories;
    std::vector<std::wstring> platform;
    for (const auto& g : games_)
        for (const auto& c : g.platformCollections)
            if (!Contains(out, c) && !Contains(platform, c)) platform.push_back(c);
    std::sort(platform.begin(), platform.end(),
              [](const std::wstring& a, const std::wstring& b) { return CompareNatural(a, b) < 0; });
    out.insert(out.end(), platform.begin(), platform.end());
    return out;
}

int Library::CountWhere(Filter f) const {
    LibraryQuery q;
    q.filter = f;
    int n = 0;
    for (const auto& g : games_) n += Matches(g, q, settings_.showApps) ? 1 : 0;
    return n;
}

int Library::CountStatus(PlayStatus s) const {
    LibraryQuery q;
    q.filter = Filter::Status;
    q.status = s;
    int n = 0;
    for (const auto& g : games_) n += Matches(g, q, settings_.showApps) ? 1 : 0;
    return n;
}

void Library::SetStatus(Game& g, PlayStatus s) {
    g.status = static_cast<int>(s);
    user_.Get(g.id).status = s;
    dirty_ = true;
}

void Library::SetNotes(Game& g, const std::wstring& notes) {
    g.notes = Trim(notes);
    user_.Get(g.id).notes = g.notes;
    dirty_ = true;
}

void Library::SetFavorite(Game& g, bool on) {
    g.favorite = on;
    user_.Get(g.id).favorite = on;
    dirty_ = true;
}

void Library::SetHidden(Game& g, bool on) {
    g.hidden = on;
    user_.Get(g.id).hidden = on;
    dirty_ = true;
}

void Library::ToggleCategory(Game& g, const std::wstring& category) {
    auto& cats = user_.Get(g.id).categories;
    auto it = std::find(cats.begin(), cats.end(), category);
    if (it == cats.end()) cats.push_back(category);
    else cats.erase(it);
    g.categories = cats;
    CreateCategory(category);
    dirty_ = true;
}

bool Library::CreateCategory(const std::wstring& raw) {
    std::wstring name = Trim(raw);
    if (name.empty() || Contains(user_.categories, name)) return false;
    user_.categories.push_back(name);
    dirty_ = true;
    return true;
}

bool Library::AddCustomGame(const std::wstring& exe, std::string* newId) {
    if (!FileExists(exe)) return false;
    for (const auto& c : user_.custom)
        if (_wcsicmp(c.exe.c_str(), exe.c_str()) == 0) {
            if (newId) *newId = c.id;
            return false;
        }
    CustomGame c;
    std::wstring lower = exe;
    for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    c.id = "custom:" + HexId(Hash64(Narrow(lower))).substr(0, 12);
    c.name = FileStem(exe);
    // ProductName is usually the game's title, but engines and system tools put generic names
    // there; fall back to FileDescription, then to the file name.
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (size) {
        std::vector<BYTE> buf(size);
        if (GetFileVersionInfoW(exe.c_str(), 0, size, buf.data())) {
            struct Lang { WORD lang, cp; }* langs = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) &&
                len >= sizeof(Lang)) {
                auto query = [&](const wchar_t* field) {
                    wchar_t q[128];
                    swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].lang, langs[0].cp, field);
                    wchar_t* val = nullptr;
                    UINT vlen = 0;
                    return (VerQueryValueW(buf.data(), q, reinterpret_cast<void**>(&val), &vlen) && vlen > 1)
                               ? Trim(val)
                               : std::wstring();
                };
                auto generic = [](const std::wstring& s) {
                    for (const wchar_t* bad : {L"Microsoft", L"Windows", L"Unity", L"Unreal", L"Operating System",
                                               L"Sistema operacional", L"Launcher"})
                        if (ContainsInsensitive(s, bad)) return true;
                    return s.size() < 2;
                };
                std::wstring product = query(L"ProductName"), desc = query(L"FileDescription");
                if (!product.empty() && !generic(product)) c.name = product;
                else if (!desc.empty() && !generic(desc)) c.name = desc;
            }
        }
    }
    c.exe = exe;
    c.addedAt = NowUnix();
    ExtractExeIcon(exe, JoinPath(ArtDir(c.id), L"icon.png"));
    user_.custom.push_back(c);
    UserGameData& d = user_.Get(c.id);
    d.addedAt = c.addedAt;
    Game g = MakeCustom(c);
    ApplyUser(g);
    games_.push_back(std::move(g));
    if (newId) *newId = c.id;
    dirty_ = true;
    ++revision_;
    return true;
}

bool Library::RenameCustomGame(const std::string& id, const std::wstring& raw) {
    std::wstring name = Trim(raw);
    if (name.empty()) return false;
    for (auto& c : user_.custom) {
        if (c.id != id) continue;
        c.name = name;
        if (Game* g = Find(id)) g->name = name;
        dirty_ = true;
        return true;
    }
    return false;
}

bool Library::RemoveCustomGame(const std::string& id) {
    auto it = std::find_if(user_.custom.begin(), user_.custom.end(), [&](const CustomGame& c) { return c.id == id; });
    if (it == user_.custom.end()) return false;
    user_.custom.erase(it);
    user_.games.erase(id);
    games_.erase(std::remove_if(games_.begin(), games_.end(), [&](const Game& g) { return g.id == id; }),
                 games_.end());
    dirty_ = true;
    ++revision_;
    return true;
}

bool Library::SetArt(Game& g, ArtKind kind, const std::wstring& src) {
    const wchar_t* base = kind == ArtKind::Cover ? L"cover" : kind == ArtKind::Hero ? L"hero" : L"logo";
    // Unique name per change so cached decodes of the previous image are never reused.
    std::wstring dst =
        JoinPath(ArtDir(g.id), std::wstring(base) + L"_" + std::to_wstring(NowUnix()) + FileExt(src));
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) return false;
    UserGameData& d = user_.Get(g.id);
    std::wstring& previous = kind == ArtKind::Cover ? d.coverOverride : kind == ArtKind::Hero ? d.heroOverride
                                                                                              : d.logoOverride;
    if (!previous.empty() && previous != dst) DeleteFileW(previous.c_str());
    if (kind == ArtKind::Cover) d.coverOverride = g.cover = dst;
    else if (kind == ArtKind::Hero) d.heroOverride = g.hero = dst;
    else d.logoOverride = g.logo = dst;
    dirty_ = true;
    ++revision_;
    return true;
}

void Library::ClearArt(Game& g) {
    UserGameData& d = user_.Get(g.id);
    d.coverOverride.clear();
    d.heroOverride.clear();
    d.logoOverride.clear();
    dirty_ = true;
    Refresh();
}

void Library::RecordLaunch(Game& g) {
    int64_t now = NowUnix();
    user_.RecordLaunch(g.id, now);
    g.launches++;
    g.lastLaunch = now;
    dirty_ = true;
}

void Library::RecordSession(const std::string& id, int64_t seconds) {
    user_.RecordSession(id, seconds);
    if (Game* g = Find(id)) g->trackedSeconds = user_.Get(id).trackedSeconds;
    dirty_ = true;
}

bool ExtractExeIcon(const std::wstring& exe, const std::wstring& pngOut) {
    HICON icon = nullptr;
    UINT got = PrivateExtractIconsW(exe.c_str(), 0, 256, 256, &icon, nullptr, 1, LR_DEFAULTCOLOR);
    if (got == 0 || got == 0xFFFFFFFF || !icon) return false;
    bool ok = false;
    ComPtr<IWICImagingFactory> wic;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        ComPtr<IWICBitmap> bmp;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, &bmp)) && SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(pngOut.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
            UINT w = 0, h = 0;
            bmp->GetSize(&w, &h);
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            frame->SetSize(w, h);
            frame->SetPixelFormat(&fmt);
            ok = SUCCEEDED(frame->WriteSource(bmp.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
                 SUCCEEDED(enc->Commit());
        }
    }
    DestroyIcon(icon);
    return ok;
}

}  // namespace lumen
