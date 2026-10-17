// Statistics computed from real library data only (platform playtime, achievements, Lumen launch history).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <map>

#include "App.h"
#include "core/Util.h"

namespace lumen {

using namespace ui;

namespace {
constexpr float M = 72.f;
}

void App::DrawStats(Ctx& c) {
    Gfx& g = gfx_;
    InputFrame& in = c.in;

    // --- Aggregate ------------------------------------------------------------------------------------
    int games = 0, favorites = 0, withAch = 0, achUnlocked = 0, achTotal = 0, perfect = 0;
    int64_t minutes = 0;
    uint64_t disk = 0;
    std::map<Platform, int> perPlatform;
    std::vector<const Game*> played, recent;
    for (const auto& gm : library_.games()) {
        if (gm.hidden || (gm.isApp && !settings_.showApps)) continue;
        ++games;
        favorites += gm.favorite;
        int64_t m = gm.TotalMinutes();
        if (m > 0) {
            minutes += m;
            played.push_back(&gm);
        }
        if (gm.achTotal > 0) {
            ++withAch;
            achUnlocked += gm.achUnlocked;
            achTotal += gm.achTotal;
            perfect += gm.achUnlocked == gm.achTotal;
        }
        disk += gm.sizeBytes;
        perPlatform[gm.platform]++;
        if (gm.LastActivity() > 0) recent.push_back(&gm);
    }
    std::sort(played.begin(), played.end(), [](auto* a, auto* b) { return a->TotalMinutes() > b->TotalMinutes(); });
    std::sort(recent.begin(), recent.end(), [](auto* a, auto* b) { return a->LastActivity() > b->LastActivity(); });

    if (!sheet_.open) {
        if (in.Has(Key::Down) || in.Has(Key::PageDown)) stats_.scrollTarget += 240;
        if (in.Has(Key::Up) || in.Has(Key::PageUp)) stats_.scrollTarget -= 240;
        if (in.Has(Key::Back)) SwitchTab(Tab::Home);
        stats_.scrollTarget -= in.wheel * 150.f;
    }

    // Ambient tint from the most played game.
    if (!played.empty()) {
        const std::wstring* src = nullptr;
        if (ID2D1Bitmap1* hero = HeroBitmap(c, *played[0], &src))
            if (ID2D1Bitmap1* blurred = g.Blurred(*src, hero))
                g.Image(blurred, {0, 0, g.W, g.H}, 0, theme_.dark ? 0.32f : 0.22f, Fit::Cover);
        g.Gradient({0, 0, g.W, g.H}, 0, {0, 0}, {0, g.H}, {{0.f, theme_.bg.A(0.6f)}, {1.f, theme_.bg.A(0.97f)}});
    }

    float contentH = 1180 + 260;
    stats_.scrollTarget = std::clamp(stats_.scrollTarget, 0.f, std::max(0.f, contentH - g.H));
    stats_.scroll += (stats_.scrollTarget - stats_.scroll) * (1.f - std::exp(-c.dt * 14.f));
    g.PushTransform(D2D1::Matrix3x2F::Translation(0, -stats_.scroll));
    c.scrollY = stats_.scroll;  // hit testing follows the scrolled content

    TextStyle title = Body(46, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    title.font = FontRole::Display;
    g.Text(L"Estatísticas", {M, 112, 800, 60}, title);
    g.Text(L"Sua biblioteca em números — calculados a partir dos dados das plataformas e do Lumen.",
           {M + 2, 172, 1000, 24}, Body(17, theme_.text2));

    // --- Tiles ------------------------------------------------------------------------------------------
    struct Tile {
        wchar_t glyph;
        std::wstring label, value, detail;
        float progress = -1;
    };
    std::vector<Tile> tiles = {
        {glyph::Library, L"JOGOS", std::to_wstring(games),
         std::to_wstring(favorites) + (favorites == 1 ? L" favorito" : L" favoritos")},
        {glyph::Clock, L"TEMPO TOTAL", FormatHoursShort(minutes),
         played.empty() ? L"Sem registros ainda" : L"em " + std::to_wstring(played.size()) + L" jogos"},
        {glyph::Trophy, L"CONQUISTAS", achTotal ? FormatInt(achUnlocked) : L"—",
         achTotal ? FormatPercent(static_cast<double>(achUnlocked) / achTotal) + L" de " + FormatInt(achTotal) +
                        (perfect ? L" · " + std::to_wstring(perfect) + L" 100%" : L"")
                  : L"Nenhum jogo com dados",
         achTotal ? static_cast<float>(achUnlocked) / achTotal : -1.f},
        {glyph::Storage, L"ESPAÇO EM DISCO", disk ? FormatBytes(disk) : L"—", L"jogos com tamanho conhecido"},
    };
    float tileGap = 20, tileW = (g.W - 2 * M - tileGap * 3) / 4, tileY = 222, tileH = 160;
    for (size_t i = 0; i < tiles.size(); ++i) {
        Rect r{M + i * (tileW + tileGap), tileY, tileW, tileH};
        Glass(c, r, 24);
        g.Icon(tiles[i].glyph, {r.x + 40, r.y + 40}, 20, theme_.accent);
        g.Text(tiles[i].label, {r.x + 68, r.y + 32, r.w - 90, 16}, Caps(12, theme_.text2));
        TextStyle v = Body(42, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        v.font = FontRole::Display;
        g.Text(tiles[i].value, {r.x + 28, r.y + 62, r.w - 56, 54}, v);
        g.Text(tiles[i].detail, {r.x + 28, r.y + 118, r.w - 56, 20}, Body(14, theme_.text2));
        if (tiles[i].progress >= 0) Progress(c, {r.x + 28, r.b() - 14, r.w - 56, 4}, tiles[i].progress, theme_.accent, theme_.control);
    }

    // --- Activity: calendar of sessions observed by Lumen (GitHub-style) -----------------------------------
    float ay = tileY + tileH + 26;
    {
        // As many weeks as fit in the panel (about a year on a 1080p screen).
        float calLeft = M + 300, calRight = g.W - M - 32, cellSize = 20, cellGap = 4;
        const int kWeeks = std::max(8, static_cast<int>((calRight - calLeft + cellGap) / (cellSize + cellGap)));
        int64_t now = NowUnix(), todayMid = LocalMidnight(now);
        // Per-day totals are cached: localtime conversions for thousands of sessions every frame add up.
        static struct {
            size_t sessions = SIZE_MAX;
            int64_t today = 0;
            int weeks = 0;
            std::map<int64_t, int64_t> perDay;
            std::vector<int64_t> days;  // midnight of each cell, column-major (week by week)
            int weekday = 0;
        } cache;
        if (cache.sessions != user_.sessions.size() || cache.today != todayMid || cache.weeks != kWeeks) {
            cache.sessions = user_.sessions.size();
            cache.weeks = kWeeks;
            cache.today = todayMid;
            cache.perDay.clear();
            for (const auto& s : user_.sessions) cache.perDay[LocalMidnight(s.start)] += s.seconds;
            __time64_t t = now;
            tm lt{};
            _localtime64_s(&lt, &t);
            cache.weekday = lt.tm_wday;  // 0 = Sunday
            cache.days.clear();
            int64_t first = todayMid - static_cast<int64_t>(kWeeks - 1) * 7 * 86400 - cache.weekday * 86400ll;
            for (int i = 0; i < kWeeks * 7; ++i) cache.days.push_back(LocalMidnight(first + i * 86400ll + 43200));
        }
        auto secondsOn = [&](int64_t mid) {
            auto it = cache.perDay.find(mid);
            int64_t s = it == cache.perDay.end() ? 0 : it->second;
            if (mid == todayMid) s = TodaySeconds();
            return s;
        };
        int64_t week = 0;
        for (int i = 0; i < 7; ++i) week += secondsOn(LocalMidnight(now - i * 86400ll));
        int streak = 0;  // consecutive days with play, ending today (or yesterday)
        for (int i = 0; i < 400; ++i) {
            int64_t mid = LocalMidnight(now - i * 86400ll);
            if (secondsOn(mid) >= 60) ++streak;
            else if (i > 0) break;
        }

        Rect panel{M, ay, g.W - 2 * M, 236};
        Glass(c, panel, 24);
        g.Text(L"ATIVIDADE", {panel.x + 32, panel.y + 28, 300, 16}, Caps(12, theme_.text2));
        struct Fact {
            std::wstring label, value;
        } facts[3] = {{L"HOJE", FormatPlaytime(TodaySeconds() / 60)},
                      {L"ÚLTIMOS 7 DIAS", FormatPlaytime(week / 60)},
                      {L"SEQUÊNCIA", std::to_wstring(streak) + (streak == 1 ? L" dia" : L" dias")}};
        for (int i = 0; i < 3; ++i) {
            float fy = panel.y + 58 + i * 56;
            g.Text(facts[i].label, {panel.x + 32, fy, 200, 14}, Caps(10, theme_.text3));
            TextStyle vs = Body(24, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            vs.font = FontRole::Display;
            g.Text(facts[i].value, {panel.x + 32, fy + 16, 260, 32}, vs);
        }
        // Calendar
        float left = calLeft, right = calRight, gap = cellGap, cell = cellSize;
        float top = panel.y + 56;
        static const wchar_t* months[] = {L"jan", L"fev", L"mar", L"abr", L"mai", L"jun",
                                          L"jul", L"ago", L"set", L"out", L"nov", L"dez"};
        int lastMonth = -1;
        std::wstring tip;
        Rect tipAnchor;
        bool any =!user_.sessions.empty() || TodaySeconds() > 0;
        for (int col = 0; col < kWeeks; ++col) {
            float x = left + col * (cell + gap);
            // Month label above the first column of each month
            __time64_t t0 = cache.days[col * 7];
            tm m{};
            _localtime64_s(&m, &t0);
            if (m.tm_mon != lastMonth) {
                if (lastMonth != -1 || col == 0) g.Text(months[m.tm_mon], {x, panel.y + 30, 60, 16}, Body(12, theme_.text3));
                lastMonth = m.tm_mon;
            }
            for (int row = 0; row < 7; ++row) {
                int64_t mid = cache.days[col * 7 + row];
                if (mid > todayMid) continue;
                int64_t s = secondsOn(mid);
                Color col2 = theme_.control;
                if (s >= 60) {
                    float k = s < 1800 ? 0.32f : s < 3600 ? 0.55f : s < 7200 ? 0.8f : 1.f;
                    col2 = theme_.accent.Fade(k);
                }
                Rect r{x, top + row * (cell + gap), cell, cell};
                g.FillRound(r, 4, col2);
                if (mid == todayMid) g.StrokeRound(r.Inset(-2), 6, theme_.text.Fade(0.8f), 1.5f);
                if (c.Hover(r)) {
                    tip = FormatDate(mid) + L"  ·  " + (s >= 60 ? FormatPlaytime(s / 60) : L"sem sessões");
                    tipAnchor = r;
                }
            }
        }
        if (!tip.empty()) {  // drawn last so no cell covers it
            TextStyle ts = Body(13, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            float tw = g.Measure(tip, ts).x + 24;
            Rect tr{std::min(tipAnchor.cx() - tw / 2, right - tw), tipAnchor.y - 40, tw, 30};
            g.SoftShadow(tr, 10, 10, Color::Black(0.3f), 4);
            g.FillRound(tr, 10, theme_.dark ? Color::Hex(0x2C2C2E) : Color::White());
            ts.valign = VAlign::Center;
            g.Text(tip, tr.Inset(12, 0), ts);
        }
        if (!any) {
            // Empty state laid over the (empty) calendar
            std::wstring msg = L"Suas sessões aparecem aqui a partir de agora. O Lumen precisa estar aberto ou na bandeja.";
            TextStyle e = Body(14, theme_.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            float mw = g.Measure(msg, e).x + 40;
            Rect mr{(left + right - mw) / 2, top + 7 * (cell + gap) / 2 - 20, mw, 40};
            g.FillRound(mr, 20, theme_.dark ? Color::Hex(0x1C1C1E, 0.92f) : Color::White(0.95f));
            e.align = Align::Center;
            e.valign = VAlign::Center;
            g.Text(msg, mr, e);
        }
    }

    // --- Most played ---------------------------------------------------------------------------------------
    float py = ay + 236 + 24;
    float lw = std::floor((g.W - 2 * M - 24) * 0.56f), rw = g.W - 2 * M - 24 - lw;
    Rect left{M, py, lw, 520};
    Glass(c, left, 24);
    g.Text(L"MAIS JOGADOS", {left.x + 32, left.y + 28, 300, 16}, Caps(12, theme_.text2));
    if (played.empty()) {
        g.Text(L"O tempo de jogo aparece aqui quando a Steam registrar suas sessões, ou quando você abrir jogos pelo Lumen.",
               {left.x + 32, left.y + 64, left.w - 64, 60}, [&] {
                   TextStyle t = Body(16, theme_.text2);
                   t.maxLines = 3;
                   return t;
               }());
    }
    float maxM = played.empty() ? 1.f : static_cast<float>(played[0]->TotalMinutes());
    for (size_t i = 0; i < std::min<size_t>(6, played.size()); ++i) {
        const Game& gm = *played[i];
        float ry = left.y + 64 + i * 74;
        Rect thumb{left.x + 32, ry, 42, 63};
        Rect row{left.x + 20, ry - 6, left.w - 40, 74};
        if (c.Hover(row)) {
            c.wantHand = true;
            g.FillRound(row, 14, theme_.control.Fade(0.6f));
        }
        GameArt(c, gm, thumb, 8, ArtStyle::Portrait);
        float tx = thumb.r() + 18, barW = left.r() - 32 - tx - 90;
        g.Text(gm.name, {tx, ry + 4, barW, 22}, Body(16, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        float frac = gm.TotalMinutes() / maxM;
        float anim = c.AnimFrom(Id(gm.id, 0x57A7), frac, 0.f, 6.f);
        Progress(c, {tx, ry + 38, barW * anim, 8}, 1.f, theme_.accent.Fade(1.f - i * 0.11f), theme_.control.A(0));
        TextStyle hs = Body(15, theme_.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        hs.align = Align::Trailing;
        g.Text(FormatPlaytime(gm.TotalMinutes()), {left.r() - 132, ry + 30, 100, 22}, hs);
        if (c.Click(row)) OpenGame(gm.id, Tab::Stats);
    }

    // --- Platforms + recent ----------------------------------------------------------------------------------
    Rect right{M + lw + 24, py, rw, 520};
    Glass(c, right, 24);
    g.Text(L"POR PLATAFORMA", {right.x + 32, right.y + 28, 300, 16}, Caps(12, theme_.text2));
    Color platColors[] = {Color::Hex(0x5AC8FA), Color::Hex(0xA2A2A7), Color::Hex(0xBF5AF2), theme_.accent};
    Platform plats[] = {Platform::Steam, Platform::Epic, Platform::GOG, Platform::Custom};
    Rect bar{right.x + 32, right.y + 62, right.w - 64, 14};
    float bx = bar.x;
    g.FillRound(bar, 7, theme_.control);
    g.PushClip(bar, 7);
    for (int i = 0; i < 4; ++i) {
        int n = perPlatform[plats[i]];
        if (!n || !games) continue;
        float w = bar.w * n / games;
        g.FillRect({bx, bar.y, w, bar.h}, platColors[i]);
        bx += w;
    }
    g.PopClip();
    float lx = bar.x;
    for (int i = 0; i < 4; ++i) {
        int n = perPlatform[plats[i]];
        if (!n) continue;
        std::wstring label = std::wstring(PlatformName(plats[i])) + L"  " + std::to_wstring(n);
        TextStyle ls = Body(14, theme_.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float w = g.Measure(label, ls).x;
        g.FillCircle({lx + 5, bar.b() + 25}, 5, platColors[i]);
        g.Text(label, {lx + 16, bar.b() + 15, w + 4, 20}, ls);
        lx += w + 40;
    }

    g.Text(L"JOGADOS RECENTEMENTE", {right.x + 32, right.y + 140, 300, 16}, Caps(12, theme_.text2));
    for (size_t i = 0; i < std::min<size_t>(5, recent.size()); ++i) {
        const Game& gm = *recent[i];
        float ry = right.y + 172 + i * 66;
        Rect row{right.x + 20, ry - 6, right.w - 40, 62};
        if (c.Hover(row)) {
            c.wantHand = true;
            g.FillRound(row, 14, theme_.control.Fade(0.6f));
        }
        Rect thumb{right.x + 32, ry, 104, 50};
        GameArt(c, gm, thumb, 9, ArtStyle::Landscape);
        g.Text(gm.name, {thumb.r() + 16, ry + 4, right.w - 220, 22}, Body(16, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        std::wstring when = FormatRelative(gm.LastActivity());
        when[0] = static_cast<wchar_t>(towupper(when[0]));
        g.Text(when + L"  ·  " + FormatPlaytime(gm.TotalMinutes()), {thumb.r() + 16, ry + 28, right.w - 220, 20},
               Body(14, theme_.text2));
        if (c.Click(row)) OpenGame(gm.id, Tab::Stats);
    }

    // --- Lumen launch history ------------------------------------------------------------------------------------
    Rect hist{M, py + 520 + 24, g.W - 2 * M, 300};
    Glass(c, hist, 24);
    g.Text(L"INICIADOS PELO LUMEN", {hist.x + 32, hist.y + 28, 400, 16}, Caps(12, theme_.text2));
    if (user_.history.empty()) {
        g.Text(L"Quando você abrir um jogo por aqui, ele aparece neste histórico.", {hist.x + 32, hist.y + 64, hist.w - 64, 24},
               Body(16, theme_.text2));
    }
    float colW = (hist.w - 64) / 2;
    for (size_t i = 0; i < std::min<size_t>(8, user_.history.size()); ++i) {
        const HistoryEntry& h = user_.history[i];
        const Game* gm = library_.Find(h.id);
        float x = hist.x + 32 + (i / 4) * colW, y = hist.y + 64 + (i % 4) * 54;
        g.Text(gm ? gm->name : Widen(h.id), {x, y, colW - 200, 22}, Body(16, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        std::wstring detail = FormatDateTime(h.time);
        if (h.seconds > 0) detail += L"  ·  sessão de " + FormatPlaytime(h.seconds / 60);
        g.Text(detail, {x, y + 24, colW - 40, 20}, Body(14, theme_.text2));
    }
    g.PopTransform();
    c.scrollY = 0;
}

}  // namespace lumen
