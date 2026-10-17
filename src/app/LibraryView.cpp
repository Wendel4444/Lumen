// Library: search, filters, categories, sorting and grid/list views.
#include <algorithm>
#include <cmath>

#include "App.h"
#include "core/Util.h"

namespace lumen {

using namespace ui;
using D2D1::Matrix3x2F;

namespace {
constexpr float M = 72.f;
const wchar_t* kSortNames[] = {L"Recentes", L"Nome", L"Tempo de jogo", L"Adicionados"};

struct ChipDef {
    std::wstring label;
    Filter filter;
    std::wstring category;
    wchar_t glyph = 0;
    PlayStatus status = PlayStatus::None;
};
}  // namespace

void App::DrawLibrary(Ctx& c) {
    Gfx& g = gfx_;
    InputFrame& in = c.in;

    // --- Filters available right now ---------------------------------------------
    std::vector<ChipDef> chips = {{L"Todos", Filter::All, L"", 0}, {L"Favoritos", Filter::Favorites, L"", glyph::HeartFill}};
    // Backlog statuses that are actually in use
    static const std::pair<PlayStatus, wchar_t> kStatusChips[] = {{PlayStatus::Playing, glyph::Game},
                                                                  {PlayStatus::Queued, glyph::Clock},
                                                                  {PlayStatus::Finished, glyph::Checkmark},
                                                                  {PlayStatus::Dropped, glyph::Hide}};
    for (auto [st, gl] : kStatusChips)
        if (library_.CountStatus(st)) chips.push_back({PlayStatusName(st), Filter::Status, L"", gl, st});
    if (library_.CountWhere(Filter::Steam)) chips.push_back({L"Steam", Filter::Steam, L""});
    if (library_.CountWhere(Filter::Epic)) chips.push_back({L"Epic Games", Filter::Epic, L""});
    if (library_.CountWhere(Filter::Gog)) chips.push_back({L"GOG", Filter::Gog, L""});
    if (library_.CountWhere(Filter::Custom)) chips.push_back({L"Manuais", Filter::Custom, L""});
    for (const auto& cat : library_.Categories()) chips.push_back({cat, Filter::Category, cat, glyph::Tag});
    if (!settings_.showApps && library_.CountWhere(Filter::Apps))
        chips.push_back({L"Aplicativos", Filter::Apps, L"", glyph::Grid});
    if (library_.CountWhere(Filter::Hidden)) chips.push_back({L"Ocultos", Filter::Hidden, L"", glyph::Hide});
    int activeChip = 0;
    for (size_t i = 0; i < chips.size(); ++i)
        if (chips[i].filter == lib_.filter && chips[i].category == lib_.category && chips[i].status == lib_.status)
            activeChip = static_cast<int>(i);
    if (lib_.filter != Filter::All && activeChip == 0) {  // category vanished
        lib_.filter = Filter::All;
        lib_.category.clear();
        lib_.status = PlayStatus::None;
    }
    auto applyChip = [&](int i) {
        i = std::clamp(i, 0, static_cast<int>(chips.size()) - 1);
        lib_.filter = chips[i].filter;
        lib_.category = chips[i].category;
        lib_.status = chips[i].status;
        lib_.chipFocus = i;
        lib_.sel = 0;
        lib_.scrollTarget = 0;
        activeChip = i;
    };

    // --- Text input (type anywhere to search) -----------------------------------------
    if (!sheet_.open) {
        if (!in.text.empty()) {
            lib_.search += in.text;
            lib_.editing = true;
            lib_.sel = 0;
            lib_.scrollTarget = 0;
        }
        if (in.Has(Key::Backspace) && !lib_.search.empty()) {
            lib_.search.pop_back();
            lib_.sel = 0;
        }
        if (in.Has(Key::Paste) && OpenClipboard(hwnd_)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    lib_.search += Trim(p).substr(0, 60);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        if (in.Has(Key::Back)) {
            if (!lib_.search.empty()) lib_.search.clear();
            else if (lib_.editing) lib_.editing = false;
            else if (lib_.zone != 2) lib_.zone = 2;
            else SwitchTab(Tab::Home);
        }
    }

    LibraryQuery q;
    q.search = lib_.search;
    q.filter = lib_.filter;
    q.category = lib_.category;
    q.status = lib_.status;
    q.sort = settings_.sort;
    lib_.results = library_.Query(q);
    int count = static_cast<int>(lib_.results.size());
    lib_.sel = std::clamp(lib_.sel, 0, std::max(0, count - 1));
    Game* focused = count ? &library_.games()[lib_.results[lib_.sel]] : nullptr;

    // --- Ambient backdrop: the focused game's art, heavily blurred ---------------------------
    if (focused) {
        const std::wstring* src = nullptr;
        if (ID2D1Bitmap1* hero = HeroBitmap(c, *focused, &src)) {
            if (ID2D1Bitmap1* blurred = g.Blurred(*src, hero)) {
                float a = c.Anim(Id(focused->id, 0xAB), 1.f, 4.f);
                g.Image(blurred, {0, 0, g.W, g.H}, 0, (theme_.dark ? 0.42f : 0.30f) * a, Fit::Cover);
            }
        }
    }
    g.Gradient({0, 0, g.W, g.H}, 0, {0, 0}, {0, g.H},
               {{0.f, theme_.bg.A(0.55f)}, {0.45f, theme_.bg.A(0.82f)}, {1.f, theme_.bg.A(0.97f)}});

    // --- Layout ----------------------------------------------------------------------------
    float sizeK = settings_.cardSize == 0 ? 0.82f : settings_.cardSize == 2 ? 1.16f : 1.f;
    bool list = settings_.viewMode == 1;
    float cardW = 186 * sizeK, cardH = cardW * 1.5f, gap = 30;
    int cols = list ? 1 : std::max(1, static_cast<int>((g.W - 2 * M + gap) / (cardW + gap)));
    float gapX = (!list && cols > 1) ? (g.W - 2 * M - cols * cardW) / (cols - 1) : 0;
    float rowH = list ? 88.f : cardH + 74.f;
    float contentTop = 356;
    float viewH = g.H - contentTop;
    int rows = (count + cols - 1) / std::max(1, cols);
    float maxScroll = std::max(0.f, rows * rowH + 40 - viewH);

    // --- Keyboard ------------------------------------------------------------------------------
    bool kb = !in.mouseMode;
    if (!sheet_.open) {
        if (lib_.zone == 0) {
            if (in.Has(Key::Left)) lib_.toolbarFocus = std::max(0, lib_.toolbarFocus - 1);
            if (in.Has(Key::Right)) lib_.toolbarFocus = std::min(3, lib_.toolbarFocus + 1);
            if (in.Has(Key::Down)) {
                lib_.zone = 1;
                lib_.editing = false;
            }
            if (in.Has(Key::Accept)) {
                switch (lib_.toolbarFocus) {
                    case 0: lib_.editing = !lib_.editing; break;
                    case 1:
                        settings_.sort = static_cast<SortMode>((static_cast<int>(settings_.sort) + 1) % 4);
                        settingsDirty_ = true;
                        break;
                    case 2:
                        settings_.viewMode = 1 - settings_.viewMode;
                        settingsDirty_ = true;
                        break;
                    case 3: AddGameDialog(); break;
                }
            }
        } else if (lib_.zone == 1) {
            if (in.Has(Key::Left)) applyChip(activeChip - 1);
            if (in.Has(Key::Right)) applyChip(activeChip + 1);
            if (in.Has(Key::Up)) lib_.zone = 0;
            if (in.Has(Key::Down) || in.Has(Key::Accept)) lib_.zone = 2;
        } else if (count > 0) {
            int s = lib_.sel;
            if (in.Has(Key::Left)) s = std::max(0, s - 1);
            if (in.Has(Key::Right)) s = std::min(count - 1, s + 1);
            if (in.Has(Key::Down)) s = std::min(count - 1, s + cols);
            if (in.Has(Key::PageDown)) s = std::min(count - 1, s + cols * 3);
            if (in.Has(Key::PageUp)) s = std::max(0, s - cols * 3);
            if (in.Has(Key::Home)) s = 0;
            if (in.Has(Key::End)) s = count - 1;
            if (in.Has(Key::Up)) {
                if (s < cols) lib_.zone = 1;
                else s -= cols;
            }
            if (s != lib_.sel) {
                lib_.sel = s;
                lib_.editing = false;
                float rowTop = (s / cols) * rowH;
                if (rowTop < lib_.scrollTarget) lib_.scrollTarget = rowTop;
                if (rowTop + rowH > lib_.scrollTarget + viewH - 20) lib_.scrollTarget = rowTop + rowH - viewH + 20;
            }
            focused = &library_.games()[lib_.results[lib_.sel]];
            if (in.Has(Key::Accept)) OpenGame(focused->id, Tab::Library);
            if (in.Has(Key::Favorite)) ToggleFavorite(*focused);
            if (in.Has(Key::Menu)) OpenGameMenu(*focused);
        } else if (in.Has(Key::Up)) {
            lib_.zone = 1;
        }
        if (in.wheel != 0) lib_.scrollTarget -= in.wheel * 150.f;
    }
    lib_.scrollTarget = std::clamp(lib_.scrollTarget, 0.f, maxScroll);
    lib_.scroll += (lib_.scrollTarget - lib_.scroll) * (1.f - std::exp(-c.dt * 14.f));

    // --- Header ---------------------------------------------------------------------------------
    TextStyle title = Body(46, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    title.font = FontRole::Display;
    g.Text(L"Biblioteca", {M, 112, 600, 60}, title);
    int64_t minutes = 0;
    int visible = 0;
    for (const auto& gm : library_.games())
        if (!gm.hidden && (!gm.isApp || settings_.showApps)) {
            ++visible;
            minutes += std::max<int64_t>(0, gm.TotalMinutes());
        }
    std::wstring summary = std::to_wstring(visible) + (visible == 1 ? L" jogo" : L" jogos") + L"  ·  " +
                           FormatHoursShort(minutes) + L" jogadas";
    if (count != visible && lib_.filter != Filter::Apps && lib_.filter != Filter::Hidden)
        summary = std::to_wstring(count) + L" de " + summary;
    g.Text(summary, {M + 2, 172, 800, 24}, Body(17, theme_.text2));

    // --- Toolbar ----------------------------------------------------------------------------------
    float ty = 214;
    Rect search{M, ty, 420, 48};
    if (TextField(c, Id("libsearch"), search, lib_.search, L"Buscar por nome, gênero ou estúdio", lib_.editing,
                  glyph::Search)) {
        lib_.editing = true;
        lib_.zone = 0;
        lib_.toolbarFocus = 0;
    }
    FocusRing(c, search, 24, c.Anim(Id("lib_tf0"), kb && lib_.zone == 0 && lib_.toolbarFocus == 0 && !lib_.editing ? 1.f : 0.f), 4);
    if (!lib_.search.empty()) {
        Rect clear{search.r() - 44, search.y + 6, 36, 36};
        if (IconButton(c, Id("libclear"), {clear.cx(), clear.cy()}, 30, glyph::Cancel, false)) lib_.search.clear();
    }
    BtnSpec sort;
    sort.label = std::wstring(L"Ordenar: ") + kSortNames[static_cast<int>(settings_.sort)];
    sort.glyph = glyph::Sort;
    sort.fontSize = 15;
    sort.focused = kb && lib_.zone == 0 && lib_.toolbarFocus == 1;
    float sw = g.Measure(sort.label, Body(15, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD)).x + 64;
    Rect sortR{search.r() + 14, ty, sw, 48};
    if (Button(c, Id("libsort"), sortR, sort)) {
        settings_.sort = static_cast<SortMode>((static_cast<int>(settings_.sort) + 1) % 4);
        settingsDirty_ = true;
    }
    Rect viewR{sortR.r() + 14, ty, 112, 48};
    {
        float f = c.Anim(Id("libview_f"), kb && lib_.zone == 0 && lib_.toolbarFocus == 2 ? 1.f : 0.f);
        g.FillRound(viewR, 24, theme_.control);
        float pos = c.Anim(Id("libview"), static_cast<float>(settings_.viewMode), 16.f);
        Rect knob{viewR.x + 4 + pos * 52, viewR.y + 4, 52, 40};
        g.FillRound(knob, 20, theme_.dark ? Color::White(0.24f) : Color::White());
        for (int i = 0; i < 2; ++i) {
            Rect cell{viewR.x + 4 + i * 52, viewR.y, 52, 48};
            g.Icon(i == 0 ? glyph::Grid : glyph::List, {cell.cx(), cell.cy()}, 16,
                   settings_.viewMode == i ? theme_.text : theme_.text2);
            if (c.Hover(cell)) c.wantHand = true;
            if (c.Click(cell) && settings_.viewMode != i) {
                settings_.viewMode = i;
                settingsDirty_ = true;
            }
        }
        FocusRing(c, viewR, 24, f, 4);
    }
    BtnSpec add{L"Adicionar jogo", glyph::Add, BtnKind::Primary, kb && lib_.zone == 0 && lib_.toolbarFocus == 3};
    add.fontSize = 15;
    Rect addR{g.W - M - 200, ty, 200, 48};
    if (Button(c, Id("libadd"), addR, add)) AddGameDialog();

    // --- Filter chips ---------------------------------------------------------------------------------
    {
        float cy = 284, x = M;
        float totalW = 0;
        std::vector<float> widths;
        for (auto& ch : chips) {
            widths.push_back(ChipWidth(c, ch.label, ch.glyph));
            totalW += widths.back() + 10;
        }
        // Keep the focused chip in view when the row overflows.
        float avail = g.W - 2 * M;
        float focusX = 0;
        for (int i = 0; i < activeChip; ++i) focusX += widths[i] + 10;
        float target = totalW > avail ? std::clamp(focusX - avail * 0.4f, 0.f, totalW - avail) : 0.f;
        float shift = c.Anim(Id("chipshift"), target, 12.f);
        g.PushClip({M - 8, cy - 8, avail + 16, 56});
        x -= shift;
        for (size_t i = 0; i < chips.size(); ++i) {
            Rect r{x, cy, widths[i], 40};
            bool chipFocus = kb && lib_.zone == 1 && static_cast<int>(i) == activeChip;
            if (Chip(c, Id("chip", i), r, chips[i].label, static_cast<int>(i) == activeChip, chipFocus, chips[i].glyph))
                applyChip(static_cast<int>(i));
            x += widths[i] + 10;
        }
        g.PopClip();
    }

    // --- Content -------------------------------------------------------------------------------------------
    Rect clip{0, contentTop - 14, g.W, g.H - contentTop + 14};
    g.PushClip(clip);
    if (count == 0) {
        TextStyle h = Body(26, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        h.align = Align::Center;
        g.Icon(glyph::Search, {g.W * 0.5f, contentTop + 120}, 40, theme_.text3);
        g.Text(lib_.search.empty() ? L"Nenhum jogo aqui ainda" : L"Nada encontrado", {0, contentTop + 160, g.W, 36}, h);
        TextStyle b = Body(17, theme_.text2);
        b.align = Align::Center;
        g.Text(lib_.search.empty() ? L"Use categorias e favoritos para organizar sua biblioteca."
                                   : L"Tente outro nome, ou limpe a busca com Esc.",
               {0, contentTop + 202, g.W, 26}, b);
    }
    for (int i = 0; i < count; ++i) {
        Game& gm = library_.games()[lib_.results[i]];
        int row = i / cols, col = i % cols;
        float y = contentTop + row * rowH - lib_.scroll;
        if (y > g.H || y + rowH < contentTop - 40) continue;
        bool isFocus = kb && lib_.zone == 2 && i == lib_.sel && !sheet_.open;
        if (!list) {
            Rect r{M + col * (cardW + gapX), y, cardW, cardH};
            bool hover = c.Hover(r);
            if (hover) {
                c.wantHand = true;
                if (in.mouseMode) lib_.sel = i;
            }
            float f = c.Anim(Id(gm.id, 0x11BF), isFocus ? 1.f : 0.f, 16.f);
            float h = c.Anim(Id(gm.id, 0x11B4), hover ? 1.f : 0.f, 18.f);
            float sc = 1.f + (0.06f * f + 0.035f * h) * c.Motion();
            float tx = c.Anim(Id(gm.id, 0x7117), hover ? std::clamp((in.mouse.x - r.cx()) / r.w, -0.5f, 0.5f) : 0.f, 10.f);
            float ty = c.Anim(Id(gm.id, 0x7118), hover ? std::clamp((in.mouse.y - r.cy()) / r.h, -0.5f, 0.5f) : 0.f, 10.f);
            g.PushTransform(Matrix3x2F::Scale(sc, sc, D2D1::Point2F(r.cx(), r.cy())) *
                            Matrix3x2F::Skew(-ty * 5.f * c.Motion(), tx * 5.f * c.Motion(), D2D1::Point2F(r.cx(), r.cy())));
            g.SoftShadow(r, 14, 16 + 10 * std::max(f, h), Color::Black(theme_.dark ? 0.35f + 0.2f * f : 0.12f + 0.1f * f), 8);
            GameArt(c, gm, r, 14, ArtStyle::Portrait);
            if (gm.favorite) {
                Vec2 hc{r.r() - 20, r.y + 20};
                g.FillCircle(hc, 14, Color::Black(0.55f));
                g.Icon(glyph::HeartFill, hc, 12, theme_.accent);
            }
            if (launcher_.IsRunning(gm.id)) g.FillCircle({r.x + 18, r.y + 18}, 6, Color::Hex(0x30D158));
            if (gm.status == static_cast<int>(PlayStatus::Finished)) {
                // "Zerado" seal: a small checkmark badge at the bottom of the cover
                Rect badge{r.x + 10, r.b() - 38, 92, 28};
                g.FillRound(badge, 14, Color::Black(0.62f));
                g.Icon(glyph::Checkmark, {badge.x + 16, badge.cy()}, 12, Color::Hex(0x30D158));
                TextStyle bs = Caps(10, Color::White());
                bs.valign = VAlign::Center;
                g.Text(L"ZERADO", {badge.x + 28, badge.y, 64, badge.h}, bs);
            }
            if (h > 0.01f && !settings_.reduceMotion) {  // glossy highlight following the cursor
                g.PushClip(r, 14);
                g.Glow({in.mouse.x, in.mouse.y}, r.w * 0.8f, Color::White(0.2f * h));
                g.PopClip();
            }
            if (f > 0.01f) g.StrokeRound(r.Inset(-5), 19, theme_.text.Fade(f), 3.f);
            g.PopTransform();
            TextStyle ns = Body(15, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            g.Text(gm.name, {r.x, r.b() + 12 + 6 * f, cardW, 20}, ns);
            std::wstring meta = gm.TotalMinutes() > 0 ? FormatPlaytime(gm.TotalMinutes()) : PlatformName(gm.platform);
            if (gm.achTotal > 0) meta += L"  ·  " + FormatPercent(static_cast<double>(gm.achUnlocked) / gm.achTotal);
            g.Text(meta, {r.x, r.b() + 34 + 6 * f, cardW, 18}, Body(13, theme_.text3));
            if (c.Click(r)) {
                lib_.sel = i;
                OpenGame(gm.id, Tab::Library);
            }
            if (in.rightReleased && r.Contains(in.mouse) && !c.blockMouse) OpenGameMenu(gm);
        } else {
            Rect r{M, y, g.W - 2 * M, 76};
            bool hover = c.Hover(r);
            if (hover) {
                c.wantHand = true;
                if (in.mouseMode) lib_.sel = i;
            }
            float f = c.Anim(Id(gm.id, 0x11BF), isFocus ? 1.f : 0.f, 16.f);
            float h = c.Anim(Id(gm.id, 0x11B4), hover ? 1.f : 0.f, 18.f);
            g.FillRound(r, 18, theme_.control.Fade(0.5f + 0.5f * std::max(h, f)));
            Rect thumb{r.x + 8, r.y + 8, 128, 60};
            GameArt(c, gm, thumb, 11, ArtStyle::Landscape);
            TextStyle ns = Body(18, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            g.Text(gm.name, {thumb.r() + 22, r.y + 14, r.w * 0.45f, 26}, ns);
            std::wstring meta = std::wstring(PlatformName(gm.platform)) + L"  ·  " + FormatPlaytime(gm.TotalMinutes()) +
                                L"  ·  " + (gm.LastActivity() > 0 ? L"Jogado " + FormatRelative(gm.LastActivity()) : L"Nunca jogado");
            g.Text(meta, {thumb.r() + 22, r.y + 42, r.w * 0.5f, 20}, Body(14, theme_.text2));
            float rx = r.r() - 24;
            if (gm.favorite) {
                g.Icon(glyph::HeartFill, {rx - 8, r.cy()}, 16, theme_.accent);
                rx -= 40;
            }
            if (gm.achTotal > 0) {
                float frac = static_cast<float>(gm.achUnlocked) / gm.achTotal;
                Ring(c, {rx - 14, r.cy()}, 14, 3.5f, frac, theme_.accent, theme_.control);
                TextStyle ps = Body(14, theme_.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                ps.align = Align::Trailing;
                ps.valign = VAlign::Center;
                g.Text(std::to_wstring(gm.achUnlocked) + L"/" + std::to_wstring(gm.achTotal), {rx - 140, r.y, 108, r.h}, ps);
            }
            if (f > 0.01f) g.StrokeRound(r.Inset(-4), 22, theme_.text.Fade(f), 3.f);
            if (c.Click(r)) OpenGame(gm.id, Tab::Library);
            if (in.rightReleased && r.Contains(in.mouse) && !c.blockMouse) OpenGameMenu(gm);
        }
    }
    g.PopClip();
    // Soft fade where content scrolls under the toolbar.
    if (lib_.scroll > 1)
        g.Gradient({0, contentTop - 14, g.W, 30}, 0, {0, contentTop - 14}, {0, contentTop + 16},
                   {{0.f, theme_.bg.A(0.85f)}, {1.f, theme_.bg.A(0)}});
}

}  // namespace lumen
