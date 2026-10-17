// Everything that makes the app feel alive: background trailers, ambient light, showcase mode,
// the cinematic launch, celebrations of real progress and game news.
#include <algorithm>
#include <cmath>
#include <map>

#include "App.h"
#include "core/Util.h"

namespace lumen {

using namespace ui;
using D2D1::Matrix3x2F;

namespace {
constexpr float M = 72.f;
constexpr float kPi = 3.14159265f;

float EaseOut(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}
float Frac(float x) { return x - std::floor(x); }
float Rand(uint32_t i, uint32_t salt) {  // deterministic pseudo-random in [0,1)
    uint32_t h = i * 2654435761u ^ (salt * 2246822519u);
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (h & 0xFFFFFF) / 16777216.f;
}
}  // namespace

// --- State ------------------------------------------------------------------------------------------

bool App::Animating() const {
    return video_.IsOpen() || time_ - launchFx_.start < 3.4 || !celebrations_.empty() || Wall() < awakeUntil_;
}

void App::UpdateTrailer() {
    Game* g = tab_ == Tab::Home ? SelectedGame() : nullptr;
    bool visible = snapshotMode_ || (IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && active_);
    // Offline is fine once a trailer is cached: TrailerService only downloads when online.
    bool allow = g && settings_.trailers && !g->trailer.empty() && visible &&
                 !videoFailed_.count(g->id) && Wall() - lastInput_ < 180.0 && time_ - launchFx_.start > 4.0;
    if (video_.IsOpen() && (!allow || videoGameId_ != g->id || video_.Failed())) {
        if (video_.Failed()) videoFailed_.insert(videoGameId_);
        video_.Close();
        videoGameId_.clear();
    }
    if (!allow) {
        videoWantId_.clear();
        return;
    }
    // Start after the user settles on a game for a moment (no downloads while scrolling past).
    if (videoWantId_ != g->id) {
        videoWantId_ = g->id;
        videoWantSince_ = time_;
    }
    if (!video_.IsOpen() && time_ - videoWantSince_ > 1.2) {
        std::wstring path = trailers_.Get(g->id, g->trailer);
        if (!path.empty() && video_.Open(path)) {
            videoGameId_ = g->id;
            ctx_->SetAnim(Id(g->id, 0x71DE0), 0.f);  // fade the video in from the still artwork
        }
    }
}

void App::AttractTick() {
    // Showcase mode: when nobody touches anything for a while, the home screen browses by itself.
    bool eligible = settings_.showcaseMode && tab_ == Tab::Home && !show_.page && !sheet_.open && !profileOpen_ &&
                    active_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && show_.list.size() > 1 &&
                    celebrations_.empty() && Wall() - lastInput_ > 45.0;
    if (!eligible) {
        nextAttract_ = 0;
        return;
    }
    if (nextAttract_ == 0) nextAttract_ = Wall() + 0.5;
    if (Wall() < nextAttract_) return;
    show_.sel = (show_.sel + 1) % static_cast<int>(show_.list.size());
    show_.selId = library_.games()[show_.list[show_.sel]].id;
    nextAttract_ = Wall() + 16.0;
    awakeUntil_ = Wall() + 3.0;  // full frame rate for the crossfade
}

void App::RefreshProgress() {
    struct Before {
        int ach;
        int64_t minutes;
    };
    std::map<std::string, Before> before;
    for (const auto& g : library_.games()) before[g.id] = {g.achUnlocked, g.TotalMinutes()};
    library_.RefreshPlaytime();

    static const int kMarks[] = {10, 25, 50, 100, 250, 500, 1000};
    for (auto& g : library_.games()) {
        auto it = before.find(g.id);
        if (it == before.end()) continue;
        if (it->second.ach >= 0 && g.achUnlocked > it->second.ach) {
            int k = g.achUnlocked - it->second.ach;
            celebrations_.push_back(Celebration{Celebration::Kind::Achievements, g.id, k});
            Notify(k == 1 ? L"Nova conquista desbloqueada!" : std::to_wstring(k) + L" novas conquistas!",
                   g.name + L" · " + std::to_wstring(g.achUnlocked) + L" de " + std::to_wstring(g.achTotal));
            meta_.RequestAchievements(g, library_.steam().LibraryCacheFile(g.platformId), true);
        }
        int64_t a = it->second.minutes, b = g.TotalMinutes();
        for (int m : kMarks) {
            if (a >= 0 && a < m * 60 && b >= m * 60) {
                celebrations_.push_back(Celebration{Celebration::Kind::Milestone, g.id, m});
                Notify(L"Marco: " + std::to_wstring(m) + L" horas",
                       L"Você passou de " + std::to_wstring(m) + L" h em " + g.name + L".");
            }
        }
    }
}

bool App::HasFreshNews(const Game& g) const {
    const auto* news = meta_.News(g.id);
    return news && !news->empty() && NowUnix() - news->front().date < 7 * 86400;
}

// --- Drawing -------------------------------------------------------------------------------------------

void App::DrawAmbient(Ctx& c, Game& game) {
    // Slow, soft light in the colour of the artwork, drifting behind the interface.
    if (settings_.reduceMotion) return;
    Gfx& g = gfx_;
    Color base = VividFrom(GameColor(c, game));
    float t = static_cast<float>(time_);
    Vec2 par{0, 0};
    if (c.in.mouseMode && c.in.mouse.x >= 0) par = {(c.in.mouse.x / g.W - 0.5f) * 30.f, (c.in.mouse.y / g.H - 0.5f) * 20.f};
    for (uint32_t i = 0; i < 8; ++i) {
        float sp = 0.05f + 0.06f * Rand(i, 1);
        float x = g.W * (0.30f + 0.65f * Rand(i, 2)) + std::sin(t * sp + i) * g.W * 0.07f + par.x * (0.5f + Rand(i, 3));
        float y = g.H * (0.10f + 0.75f * Rand(i, 4)) + std::cos(t * sp * 1.3f + i * 2.f) * g.H * 0.06f + par.y;
        float r = 170.f + 170.f * Rand(i, 5);
        float a = 0.05f + 0.05f * (0.5f + 0.5f * std::sin(t * 0.35f + i * 1.7f));
        g.Glow({x, y}, r, base.A(a));
    }
}

void App::DrawLaunchFx(Ctx& c) {
    float t = static_cast<float>(time_ - launchFx_.start);
    if (t < 0 || t > 3.2f) return;
    Game* game = library_.Find(launchFx_.id);
    if (!game) return;
    Gfx& g = gfx_;
    float a = std::min(EaseOut(t / 0.35f), 1.f - EaseOut((t - 2.6f) / 0.6f));
    g.PushAlpha(a);
    g.FillRect({0, 0, g.W, g.H}, Color::Black(0.9f));
    // The artwork slowly settles (a "dolly in"), like a console starting a game.
    const std::wstring* src = nullptr;
    if (ID2D1Bitmap1* hero = HeroBitmap(c, *game, &src)) {
        float zoom = 1.18f - 0.18f * EaseOut(t / 3.2f) * c.Motion();
        g.Image(hero, {0, 0, g.W, g.H}, 0, 0.75f, Fit::Cover, {0.5f, 0.35f}, zoom);
    }
    g.Gradient({0, 0, g.W, g.H}, 0, {0, 0}, {0, g.H},
               {{0.f, Color::Black(0.55f)}, {0.45f, Color::Black(0.25f)}, {1.f, Color::Black(0.9f)}});
    Rect box{g.W * 0.3f, g.H * 0.30f, g.W * 0.4f, g.H * 0.22f};
    LogoOrTitle(c, *game, box, Align::Center, 80);
    std::wstring how = game->platform == Platform::Steam ? L"Abrindo pela Steam…"
                       : game->platform == Platform::Epic ? L"Abrindo pela Epic Games…"
                                                          : L"Abrindo…";
    TextStyle ts = Body(20, Color::White(0.85f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ts.align = Align::Center;
    g.Text(how, {0, g.H * 0.58f, g.W, 30}, ts);
    // Spinner + progress line
    Vec2 sc{g.W * 0.5f, g.H * 0.66f};
    g.StrokeCircle(sc, 16, Color::White(0.15f), 3);
    g.Arc(sc, 16, t * 400.f, 100.f, theme_.accent, 3);
    float pw = 260;
    Progress(c, {sc.x - pw / 2, g.H * 0.72f, pw, 3}, std::min(1.f, t / 2.6f), Color::White(0.8f), Color::White(0.12f));
    g.PopAlpha();
}

void App::DrawCelebration(Ctx& c) {
    if (celebrations_.empty()) return;
    Celebration& cel = celebrations_.front();
    Game* game = library_.Find(cel.gameId);
    if (!game) {
        celebrations_.pop_front();
        return;
    }
    if (cel.start < 0) {
        cel.start = time_;
        PlaySfx(Sfx::Launch);
        if (cel.kind == Celebration::Kind::Achievements)
            meta_.RequestAchievements(*game, library_.steam().LibraryCacheFile(game->platformId), true);
    }
    float t = static_cast<float>(time_ - cel.start);
    if (t > 6.f) {
        celebrations_.pop_front();
        return;
    }
    Gfx& g = gfx_;
    float a = std::min(EaseOut(t / 0.3f), 1.f - EaseOut((t - 5.4f) / 0.6f));
    g.PushAlpha(a);
    g.FillRect({0, 0, g.W, g.H}, Color::Black(0.55f));

    // Card
    Rect card{(g.W - 620) / 2, g.H * 0.30f, 620, 330};
    float pop = 0.9f + 0.1f * EaseOut(t / 0.45f);
    g.PushTransform(Matrix3x2F::Scale(pop, pop, D2D1::Point2F(card.cx(), card.cy())));
    g.SoftShadow(card, 30, 40, Color::Black(0.5f), 16);
    g.FillRound(card, 30, theme_.dark ? Color::Hex(0x1C1C1E, 0.97f) : Color::White(0.98f));
    // Artwork strip at the top of the card
    if (ID2D1Bitmap1* hero = HeroBitmap(c, *game)) {
        g.PushClip({card.x, card.y, card.w, 120}, 0);
        g.Image(hero, {card.x, card.y, card.w, 160}, 30, 0.85f, Fit::Cover, {0.5f, 0.3f});
        g.PopClip();
        g.Gradient({card.x, card.y + 40, card.w, 80}, 0, {0, card.y + 40}, {0, card.y + 120},
                   {{0.f, (theme_.dark ? Color::Hex(0x1C1C1E) : Color::White()).A(0)},
                    {1.f, theme_.dark ? Color::Hex(0x1C1C1E) : Color::White()}});
    }
    // Trophy (or clock) medallion with a little bounce
    Vec2 medal{card.cx(), card.y + 118};
    float bounce = 1.f + 0.18f * std::sin(std::min(t * 7.f, kPi)) * c.Motion();
    g.Glow(medal, 90, theme_.accent.A(0.35f));
    g.FillCircle(medal, 40 * bounce, theme_.accent);
    g.Icon(cel.kind == Celebration::Kind::Achievements ? glyph::Trophy : glyph::Clock, medal, 34 * bounce, Color::White());

    TextStyle title = Body(34, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    title.font = FontRole::Display;
    title.align = Align::Center;
    std::wstring head = cel.kind == Celebration::Kind::Achievements
                            ? (cel.count == 1 ? L"Nova conquista!" : std::to_wstring(cel.count) + L" novas conquistas!")
                            : std::to_wstring(cel.count) + L" horas de jogo";
    g.Text(head, {card.x, card.y + 172, card.w, 44}, title);
    TextStyle sub = Body(17, theme_.text2);
    sub.align = Align::Center;
    std::wstring line = game->name;
    if (cel.kind == Celebration::Kind::Achievements && game->achTotal > 0)
        line += L"  ·  " + std::to_wstring(game->achUnlocked) + L" de " + std::to_wstring(game->achTotal);
    g.Text(line, {card.x + 20, card.y + 220, card.w - 40, 24}, sub);
    // Icons of the most recent unlocks, when the achievement list is known
    if (const AchievementSet* set = meta_.Achievements(game->id)) {
        std::vector<const Achievement*> recent;
        for (const auto& ach : set->list)
            if (ach.achieved && !ach.icon.empty()) recent.push_back(&ach);
        std::sort(recent.begin(), recent.end(), [](auto* x, auto* y) { return x->unlockedAt > y->unlockedAt; });
        size_t n = std::min<size_t>(recent.size(), std::max(1, std::min(cel.count, 6)));
        float x = card.cx() - (n * 52.f + (n - 1) * 10.f) / 2;
        for (size_t i = 0; i < n; ++i) {
            if (ID2D1Bitmap1* ic = images_.Get(recent[i]->icon, 128)) g.Image(ic, {x, card.y + 258, 52, 52}, 12);
            x += 62;
        }
    }
    g.PopTransform();

    // Confetti burst
    if (!settings_.reduceMotion) {
        Color palette[] = {theme_.accent, Color::Hex(0xFFD60A), Color::Hex(0x30D158), Color::Hex(0x0A84FF),
                           Color::Hex(0xFF375F), Color::White()};
        for (uint32_t i = 0; i < 90; ++i) {
            float vx = (Rand(i, 11) - 0.5f) * 1300.f, vy = -350.f - Rand(i, 12) * 750.f;
            float x = card.cx() + (Rand(i, 13) - 0.5f) * 200.f + vx * t;
            float y = card.y + 100 + vy * t + 0.5f * 1200.f * t * t;
            if (y > g.H + 20) continue;
            float rot = Rand(i, 14) * 360.f + t * (Rand(i, 15) - 0.5f) * 900.f;
            g.PushTransform(Matrix3x2F::Rotation(rot, D2D1::Point2F(x, y)));
            g.FillRect({x - 6, y - 3.5f, 12, 7}, palette[i % 6].Fade(std::clamp(3.5f - t, 0.f, 1.f)));
            g.PopTransform();
        }
    }
    g.PopAlpha();
}

void App::DrawNewsSection(Ctx& c, Game& game, float y, float& outBottom) {
    const auto* news = meta_.News(game.id);
    if (!news || news->empty()) {
        outBottom = y - 56;
        return;
    }
    Gfx& g = gfx_;
    TextStyle hs = Body(34, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hs.font = FontRole::Display;
    g.Text(L"Novidades", {M, y, 600, 46}, hs);
    g.Text(L"Anúncios oficiais publicados na Steam", {M, y + 52, 800, 22}, Body(16, theme_.text2));
    y += 92;
    size_t n = std::min<size_t>(news->size(), 3);
    float gap = 18, w = (g.W - 2 * M - gap * 2) / 3, h = 214;
    for (size_t i = 0; i < n; ++i) {
        const NewsItem& it = (*news)[i];
        Rect r{M + i * (w + gap), y, w, h};
        bool hover = c.Hover(r);
        if (hover) c.wantHand = true;
        float hv = c.Anim(Id("news", i), hover ? 1.f : 0.f, 18.f);
        g.PushTransform(Matrix3x2F::Translation(0, -4 * hv * c.Motion()));
        Glass(c, r, 22, 0.95f);
        bool fresh = NowUnix() - it.date < 7 * 86400;
        std::wstring when = ToUpper(FormatRelative(it.date));
        g.Text(when, {r.x + 24, r.y + 22, r.w - 48, 16}, Caps(11, fresh ? theme_.accent : theme_.text3));
        TextStyle ts = Body(19, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        ts.maxLines = 2;
        ts.lineHeight = 1.25f;
        g.Text(it.title, {r.x + 24, r.y + 46, r.w - 48, 52}, ts);
        TextStyle ss = Body(14, theme_.text2);
        ss.maxLines = 3;
        ss.lineHeight = 1.4f;
        g.Text(it.snippet, {r.x + 24, r.y + 110, r.w - 48, 64}, ss);
        g.Text(L"Ler na Steam", {r.x + 24, r.b() - 36, 200, 20}, Body(14, theme_.accent, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        g.PopTransform();
        if (c.Click(r) && !it.url.empty()) ShellExecuteW(nullptr, L"open", it.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    outBottom = y + h;
}

}  // namespace lumen
