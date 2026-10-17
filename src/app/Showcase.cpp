// Home: full-screen artwork, horizontal catalog and the per-game page (same scene, animated between states).
#include <algorithm>
#include <cmath>

#include "App.h"
#include "core/Util.h"

namespace lumen {

using namespace ui;
using D2D1::Matrix3x2F;

namespace {
constexpr float M = 72.f;  // page margin

float EaseOut(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

std::wstring Capitalized(std::wstring s) {
    if (!s.empty()) s[0] = static_cast<wchar_t>(towupper(s[0]));
    return s;
}

struct ShowcaseLayout {
    float cardH, portW, landW, gap;
    float carouselY, panelsY, panelH;
    float btnTop, chipsTop, subtitleY, titleBottom;
};

ShowcaseLayout Compute(const Gfx& g, const Settings& s, float pt) {
    ShowcaseLayout L;
    float k = s.cardSize == 0 ? 0.84f : s.cardSize == 2 ? 1.14f : 1.f;
    L.cardH = 330.f * k;
    L.portW = L.cardH * 2.f / 3.f;
    L.landW = L.cardH * 2.06f;
    L.gap = 22.f;
    L.panelH = 300.f;
    float browseCarousel = g.H - 46 - L.cardH;
    L.carouselY = browseCarousel + EaseOut(pt) * (L.cardH + 140);
    L.panelsY = g.H - 46 - L.panelH + (1 - EaseOut(pt)) * 90;
    float btnBrowse = browseCarousel - 60 - 72;
    float btnPage = g.H - 46 - L.panelH - 46 - 72;
    L.btnTop = Lerp(btnBrowse, btnPage, pt);
    L.chipsTop = btnBrowse - 30 - 84;
    L.subtitleY = Lerp(L.chipsTop - 36, btnPage - 44, pt);
    L.titleBottom = L.subtitleY - 14;
    return L;
}
}  // namespace

void App::HandleShowcaseKeys(Ctx& c) {
    InputFrame& in = c.in;
    int n = static_cast<int>(show_.list.size());
    if (n == 0) return;
    Game* g = SelectedGame();
    auto move = [&](int d) {
        int ns = std::clamp(show_.sel + d, 0, n - 1);
        if (ns != show_.sel) {
            show_.sel = ns;
            show_.selId = library_.games()[show_.list[ns]].id;
        }
    };
    const int buttons = 5;  // play, favorite, info/categories, settings, platform
    auto activate = [&](int b) {
        if (!g) return;
        switch (b) {
            case 0: LaunchGame(*g); break;
            case 1: ToggleFavorite(*g); break;
            case 2:
                if (show_.page) OpenCategorySheet(g->id);
                else OpenGame(g->id, Tab::Home);
                break;
            case 3: OpenGameMenu(*g); break;
            case 4:
                if (launcher_.HasPlatformPage(*g)) launcher_.OpenInPlatform(*g);
                else launcher_.OpenInstallFolder(*g);
                break;
        }
    };

    if (in.Has(Key::Info) && g) {
        if (show_.page) CloseGamePage();
        else OpenGame(g->id, Tab::Home);
    }
    if (in.Has(Key::Favorite) && g) ToggleFavorite(*g);
    if (in.Has(Key::Menu) && g) OpenGameMenu(*g);

    if (show_.page) {
        if (in.Has(Key::Back)) {
            CloseGamePage();
            return;
        }
        if (show_.row == 0) {
            if (in.Has(Key::Left)) show_.button = std::max(0, show_.button - 1);
            if (in.Has(Key::Right)) show_.button = std::min(buttons - 1, show_.button + 1);
            if (in.Has(Key::Accept)) activate(show_.button);
            if (in.Has(Key::Down) || in.Has(Key::PageDown)) {
                show_.row = 1;
                show_.pageScrollTarget = std::min(show_.pageMax, show_.pageScrollTarget + 420);
            }
        } else {
            if (in.Has(Key::Down) || in.Has(Key::PageDown))
                show_.pageScrollTarget = std::min(show_.pageMax, show_.pageScrollTarget + 380);
            if (in.Has(Key::Up) || in.Has(Key::PageUp)) {
                show_.pageScrollTarget = std::max(0.f, show_.pageScrollTarget - 380);
                if (show_.pageScrollTarget <= 0) show_.row = 0;
            }
            if (in.Has(Key::Home)) {
                show_.pageScrollTarget = 0;
                show_.row = 0;
            }
            if (in.Has(Key::Accept) && g) LaunchGame(*g);
        }
        if (in.wheel != 0 && !c.blockMouse) {
            show_.pageScrollTarget = std::clamp(show_.pageScrollTarget - in.wheel * 160.f, 0.f, show_.pageMax);
            show_.row = show_.pageScrollTarget > 0 ? 1 : show_.row;
        }
        return;
    }

    // Catalog
    if (show_.row == 1) {
        if (in.Has(Key::Left)) move(-1);
        if (in.Has(Key::Right)) move(1);
        if (in.Has(Key::Up)) show_.row = 0;
        if (in.Has(Key::Accept) && g) OpenGame(g->id, Tab::Home);
    } else {
        if (in.Has(Key::Left)) show_.button = std::max(0, show_.button - 1);
        if (in.Has(Key::Right)) show_.button = std::min(buttons - 1, show_.button + 1);
        if (in.Has(Key::Down) || in.Has(Key::Back)) show_.row = 1;
        if (in.Has(Key::Accept)) activate(show_.button);
    }
    if (in.Has(Key::PageUp)) move(-5);
    if (in.Has(Key::PageDown)) move(5);
    if (in.Has(Key::Home)) move(-n);
    if (in.Has(Key::End)) move(n);
    if (in.wheel != 0 && !c.blockMouse) {
        show_.wheelAcc += in.wheel;
        while (show_.wheelAcc >= 1.f) {
            move(-1);
            show_.wheelAcc -= 1.f;
        }
        while (show_.wheelAcc <= -1.f) {
            move(1);
            show_.wheelAcc += 1.f;
        }
    }
}

void App::DrawBackground(Ctx& c, Game* game, float pt) {
    Gfx& g = gfx_;
    if (game && game->id != show_.bgId) {
        if (show_.pendingBg != game->id) {
            show_.pendingBg = game->id;
            show_.pendingSince = time_;
        }
        const std::wstring* src = nullptr;
        ID2D1Bitmap1* b = HeroBitmap(c, *game, &src);
        // Switch once the new artwork is decoded (or clearly unavailable) so the crossfade is clean.
        if (b || !src || time_ - show_.pendingSince > 0.8 || show_.bgId.empty()) {
            show_.bgPrevId = show_.bgId;
            show_.bgId = game->id;
            show_.bgChangedAt = time_;
        }
    }
    float fade = EaseOut(static_cast<float>((time_ - show_.bgChangedAt) / 0.55));
    Vec2 par{0, 0};
    if (c.in.mouseMode && c.in.mouse.x >= 0 && !settings_.reduceMotion) {
        float tx = (c.in.mouse.x / g.W - 0.5f) * -16.f, ty = (c.in.mouse.y / g.H - 0.5f) * -10.f;
        par.x = c.Anim(Id("parx"), tx, 3.f);
        par.y = c.Anim(Id("pary"), ty, 3.f);
    }
    float blurK = std::clamp(show_.pageScroll / 420.f, 0.f, 1.f) * pt;

    auto drawOne = [&](Game* gm, float alpha, double since, bool primary) {
        if (!gm || alpha <= 0.01f) return;
        float settle = 1.f - EaseOut(static_cast<float>((time_ - since) / 1.8));
        float zoom = 1.f + 0.045f * settle * c.Motion();
        // "Breathing": a very slow, continuous zoom so the artwork never looks frozen.
        zoom *= 1.f + 0.022f * (0.5f + 0.5f * std::sin(static_cast<float>(time_) * 0.26f)) * c.Motion();
        Rect screen{-24 + par.x, -24 + par.y, g.W + 48, g.H + 48};
        const std::wstring* src = nullptr;
        ID2D1Bitmap1* bmp = HeroBitmap(c, *gm, &src);
        if (bmp) {
            ID2D1Bitmap1* blurred = g.Blurred(*src, bmp);
            bool lowRes = bmp->GetSize().width < 1200;  // small header art looks better softened
            Vec2 anchor{0.5f, 0.28f};
            if (lowRes && blurred) g.Image(blurred, screen, 0, alpha, Fit::Cover, anchor, zoom);
            else g.Image(bmp, screen, 0, alpha, Fit::Cover, anchor, zoom);
            // Store trailer on top of the still artwork, faded in once frames arrive.
            if (primary && video_.IsOpen() && videoGameId_ == gm->id) {
                if (ID2D1Bitmap1* frame = video_.Frame(g)) {
                    float vf = c.Anim(Id(gm->id, 0x71DE0), 1.f, 1.6f);
                    g.Image(frame, screen, 0, alpha * vf, Fit::Cover, {0.5f, 0.5f}, zoom);
                }
            }
            if (blurred && blurK > 0) g.Image(blurred, screen, 0, alpha * blurK, Fit::Cover, anchor, zoom);
            if (primary && blurred) {
                c.backdrop.bmp = blurred;
                c.backdrop.rect = g.FitRect(bmp, screen, Fit::Cover, anchor, zoom);
                c.backdrop.opacity = alpha;
            }
        } else {
            Color col = GameColor(c, *gm);
            Color base = theme_.dark ? Color::Lerp(col, Color::Black(), 0.55f) : Color::Lerp(col, Color::White(), 0.55f);
            g.Gradient({0, 0, g.W, g.H}, 0, {g.W, 0}, {0, g.H}, {{0.f, base.A(alpha)}, {1.f, theme_.bg.A(alpha)}});
        }
    };
    g.FillRect({0, 0, g.W, g.H}, theme_.bg);
    if (fade < 1.f && !show_.bgPrevId.empty()) drawOne(library_.Find(show_.bgPrevId), 1.f, show_.bgChangedAt - 2, false);
    drawOne(library_.Find(show_.bgId), fade, show_.bgChangedAt, true);

    // Legibility scrims, tuned to stay subtle over bright art.
    Color s = theme_.scrim;
    float W = g.W, H = g.H;
    g.FillRect({0, 0, W, H}, s.A(0.16f + 0.22f * pt));
    g.Gradient({0, 0, W * 0.72f, H}, 0, {0, 0}, {W * 0.72f, 0},
               {{0.f, s.A(0.80f)}, {0.5f, s.A(0.42f)}, {1.f, s.A(0.f)}});
    g.Gradient({0, H * 0.32f, W, H * 0.68f}, 0, {0, H * 0.32f}, {0, H},
               {{0.f, s.A(0.f)}, {0.55f, s.A(0.62f)}, {1.f, s.A(0.94f)}});
    g.Gradient({0, 0, W, 210}, 0, {0, 0}, {0, 210}, {{0.f, s.A(0.55f)}, {1.f, s.A(0.f)}});
    if (Game* bgGame = library_.Find(show_.bgId)) DrawAmbient(c, *bgGame);
}

void App::DrawShowcase(Ctx& c) {
    Gfx& g = gfx_;
    if (show_.listRevision != library_.revision()) RebuildHomeList();
    if (show_.list.empty()) {
        g.FillRect({0, 0, g.W, g.H}, theme_.bg);
        g.Gradient({0, 0, g.W, g.H}, 0, {g.W, 0}, {0, g.H},
                   {{0.f, theme_.accent.A(0.22f)}, {0.6f, theme_.bg.A(0)}});
        TextStyle h = Body(46, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        h.font = FontRole::Display;
        h.align = Align::Center;
        g.Icon(glyph::Game, {g.W * 0.5f, g.H * 0.36f}, 64, theme_.text3);
        g.Text(L"Sua biblioteca está vazia", {0, g.H * 0.44f, g.W, 60}, h);
        TextStyle b = Body(19, theme_.text2);
        b.align = Align::Center;
        b.maxLines = 2;
        g.Text(L"Instale jogos pela Steam, Epic ou GOG, ou adicione um executável manualmente.",
               {g.W * 0.25f, g.H * 0.44f + 70, g.W * 0.5f, 60}, b);
        BtnSpec add{L"Adicionar jogo", glyph::Add, BtnKind::Primary, show_.button == 0};
        BtnSpec ref{L"Procurar de novo", glyph::Refresh, BtnKind::Glass, show_.button == 1};
        if (c.in.Has(Key::Left)) show_.button = 0;
        if (c.in.Has(Key::Right)) show_.button = 1;
        Rect r1{g.W * 0.5f - 290, g.H * 0.44f + 150, 280, 60}, r2{g.W * 0.5f + 10, g.H * 0.44f + 150, 280, 60};
        if (Button(c, Id("emptyadd"), r1, add) || (c.in.Has(Key::Accept) && show_.button == 0)) AddGameDialog();
        if (Button(c, Id("emptyref"), r2, ref) || (c.in.Has(Key::Accept) && show_.button == 1)) RefreshAll(true);
        return;
    }

    HandleShowcaseKeys(c);
    Game* game = SelectedGame();
    if (!game) return;
    RequestGameData(*game);
    float pt = pageT_;
    show_.pageScroll += (show_.pageScrollTarget - show_.pageScroll) * (1.f - std::exp(-c.dt * 12.f));
    if (!show_.page) show_.pageScrollTarget = 0;

    DrawBackground(c, game, pt);
    ShowcaseLayout L = Compute(g, settings_, pt);
    bool kb = !c.in.mouseMode;

    g.PushTransform(Matrix3x2F::Translation(0, -show_.pageScroll));
    c.scrollY = show_.pageScroll;  // hit testing follows the scrolled content

    // --- Title & subtitle ----------------------------------------------------
    float titleW = std::min(600.f, g.W * 0.4f);
    LogoOrTitle(c, *game, {M, L.titleBottom - 170, titleW, 170}, Align::Leading, 72);

    std::wstring sub;
    if (launcher_.IsRunning(game->id)) sub = L"EM EXECUÇÃO";
    if (game->favorite) sub += (sub.empty() ? L"" : L"  ·  ") + std::wstring(L"FAVORITO");
    if (game->isApp) sub += (sub.empty() ? L"" : L"  ·  ") + std::wstring(L"APLICATIVO");
    if (game->status) sub += (sub.empty() ? L"" : L"  ·  ") + ToUpper(PlayStatusName(static_cast<PlayStatus>(game->status)));
    sub += (sub.empty() ? L"" : L"  ·  ") + ToUpper(PlatformName(game->platform));
    if (!launcher_.IsRunning(game->id))
        sub += L"  ·  " + (game->LastActivity() > 0 ? ToUpper(L"Jogado " + FormatRelative(game->LastActivity()))
                                                    : std::wstring(L"NUNCA JOGADO"));
    TextStyle subStyle = Caps(14, theme_.accent);
    subStyle.tracking = 2.6f;
    subStyle.shadow = theme_.dark;
    g.Text(sub, {M, L.subtitleY, g.W * 0.6f, 22}, subStyle);

    // --- Stat chips (catalog only) -------------------------------------------
    if (pt < 0.99f) {
        g.PushAlpha(1.f - EaseOut(pt * 1.6f));
        struct ChipData {
            std::wstring label, value;
            float progress = -1;
        };
        std::vector<ChipData> chips;
        chips.push_back({L"TEMPO DE JOGO", FormatPlaytime(game->TotalMinutes())});
        if (game->achTotal > 0)
            chips.push_back({L"CONQUISTAS", std::to_wstring(game->achUnlocked) + L" / " + std::to_wstring(game->achTotal),
                             static_cast<float>(game->achUnlocked) / game->achTotal});
        if (game->sizeBytes > 0) chips.push_back({L"TAMANHO", FormatBytes(game->sizeBytes)});
        else chips.push_back({L"ADICIONADO", FormatRelative(game->addedAt)});
        float x = M;
        for (auto& ch : chips) {
            TextStyle ls = Caps(12, theme_.text2);
            TextStyle vs = Body(28, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            vs.font = FontRole::Display;
            float w = std::max(g.Measure(ch.label, ls).x, g.Measure(ch.value, vs).x) + 44;
            w = std::max(w, 150.f);
            Rect r{x, L.chipsTop, w, 84};
            Glass(c, r, 18);
            g.Text(ch.label, {r.x + 22, r.y + 16, w - 30, 16}, ls);
            g.Text(ch.value, {r.x + 22, r.y + 34, w - 30, 36}, vs);
            if (ch.progress >= 0) Progress(c, {r.x + 22, r.b() - 12, w - 44, 4}, ch.progress, theme_.accent, theme_.control);
            x += w + 14;
        }
        g.PopAlpha();
    }

    // --- Actions ---------------------------------------------------------------
    {
        bool rowFocus = kb && show_.row == 0 && !sheet_.open;
        bool running = launcher_.IsRunning(game->id);
        bool opening = show_.launchedId == game->id && time_ - show_.launchedAt < 6.0 && !running;
        BtnSpec play;
        play.kind = BtnKind::Primary;
        play.glyph = running ? glyph::Game : glyph::Play;
        play.label = running ? L"Em execução" : opening ? L"Abrindo…" : L"Jogar";
        play.fontSize = 22;
        play.focused = rowFocus && show_.button == 0;
        if (running) {
            int64_t mins = (NowUnix() - launcher_.RunningSince(game->id)) / 60;
            play.sublabel = mins < 1 ? L"agora mesmo" : L"há " + std::to_wstring(mins) + L" min";
        }
        Rect pr{M, L.btnTop, 300, 72};
        if (Button(c, Id("play"), pr, play)) LaunchGame(*game);

        float cx = pr.r() + 18 + 36;
        wchar_t third = show_.page ? glyph::Tag : glyph::Info;
        wchar_t glyphs[3] = {game->favorite ? glyph::HeartFill : glyph::Heart, third, glyph::Settings};
        for (int i = 0; i < 3; ++i) {
            bool on = i == 0 && game->favorite;
            if (IconButton(c, Id("act", i), {cx, pr.cy()}, 72, glyphs[i], rowFocus && show_.button == i + 1, on,
                           theme_.accent)) {
                if (i == 0) ToggleFavorite(*game);
                else if (i == 1) {
                    if (show_.page) OpenCategorySheet(game->id);
                    else OpenGame(game->id, Tab::Home);
                } else OpenGameMenu(*game);
            }
            cx += 72 + 16;
        }
        // Platform pill on the right.
        bool platform = launcher_.HasPlatformPage(*game);
        BtnSpec ps;
        ps.label = platform ? PlatformName(game->platform) : L"Pasta do jogo";
        ps.glyph = platform ? glyph::Link : glyph::Folder;
        ps.fontSize = 16;
        ps.focused = rowFocus && show_.button == 4;
        TextStyle pts = Body(16, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float pw = g.Measure(ps.label, pts).x + 70;
        Rect pp{g.W - M - pw, pr.cy() - 24, pw, 48};
        if (Button(c, Id("platform"), pp, ps)) {
            if (platform) launcher_.OpenInPlatform(*game);
            else launcher_.OpenInstallFolder(*game);
        }
    }

    // --- Game page panels and sections ------------------------------------------
    if (pt > 0.01f) {
        g.PushAlpha(EaseOut(pt));
        DrawGamePage(c, *game, pt);
        g.PopAlpha();
    }
    g.PopTransform();
    c.scrollY = 0;

    // Content scrolling under the top chrome fades out instead of colliding with it.
    if (pt > 0.01f && show_.pageScroll > 1.f) {
        float k = std::clamp(show_.pageScroll / 90.f, 0.f, 1.f) * pt;
        g.Gradient({0, 0, g.W, 150}, 0, {0, 0}, {0, 150},
                   {{0.f, theme_.scrim.A(0.92f * k)}, {0.6f, theme_.scrim.A(0.6f * k)}, {1.f, theme_.scrim.A(0)}});
    }

    // --- Carousel ----------------------------------------------------------------
    if (pt < 0.99f) {
        float alpha = 1.f - EaseOut(pt * 1.3f);
        g.PushAlpha(alpha);
        int n = static_cast<int>(show_.list.size());
        float target = show_.sel * (L.portW + L.gap);
        show_.scroll += (target - show_.scroll) * (1.f - std::exp(-c.dt * (settings_.reduceMotion ? 30.f : 11.f)));
        float x = M - show_.scroll;
        bool carouselFocus = kb && show_.row == 1 && !show_.page && !sheet_.open;
        // Cards before the first visible one only advance x.
        for (int i = 0; i < n; ++i) {
            Game& gm = library_.games()[show_.list[i]];
            bool selected = i == show_.sel;
            float w = c.Anim(Id(gm.id, 0xCA7D), selected ? L.landW : L.portW, 13.f);
            Rect r{x, L.carouselY, w, L.cardH};
            x += w + L.gap;
            if (r.r() < -40 || r.x > g.W + 40) continue;
            bool hover = c.Hover(r) && pt < 0.2f;
            if (hover) c.wantHand = true;
            float h = c.Anim(Id(gm.id, 0x40E7), hover ? 1.f : 0.f, 18.f);
            float lift = (h * 6.f) * c.Motion();
            Rect cr = r.Offset(0, -lift);
            float ring = c.Anim(Id(gm.id, 0x7196), selected ? (carouselFocus || !kb ? 1.f : 0.55f) : h * 0.35f, 16.f);
            // 3D-ish tilt towards the cursor plus a moving highlight (like a glossy poster).
            float tx = c.Anim(Id(gm.id, 0x7117), hover ? std::clamp((c.in.mouse.x - cr.cx()) / cr.w, -0.5f, 0.5f) : 0.f, 10.f);
            float ty = c.Anim(Id(gm.id, 0x7118), hover ? std::clamp((c.in.mouse.y - cr.cy()) / cr.h, -0.5f, 0.5f) : 0.f, 10.f);
            g.PushTransform(Matrix3x2F::Skew(-ty * 4.f * c.Motion(), tx * 4.f * c.Motion(), D2D1::Point2F(cr.cx(), cr.cy())));
            g.SoftShadow(cr, 14, 18, Color::Black(0.32f + 0.18f * ring), 10);
            float wl = (w - L.portW) / std::max(1.f, L.landW - L.portW);  // 0 portrait .. 1 landscape
            if (wl > 0.02f) GameArt(c, gm, cr, 14, ArtStyle::Landscape, std::min(1.f, wl * 1.4f));
            if (wl < 0.98f) GameArt(c, gm, cr, 14, ArtStyle::Portrait, 1.f - wl);
            if (!selected) g.FillRound(cr, 14, Color::Black(0.12f * (1.f - h)));
            if (h > 0.01f && !settings_.reduceMotion) {
                g.PushClip(cr, 14);
                g.Glow({c.in.mouse.x, c.in.mouse.y}, cr.w * 0.75f, Color::White(0.18f * h));
                g.PopClip();
            }
            if (ring > 0.01f) {
                Rect rr = cr.Inset(-5);
                g.StrokeRound(rr, 19, Color::White(ring), 3.f);
            }
            if (launcher_.IsRunning(gm.id)) {
                Rect badge{cr.x + 12, cr.y + 12, 118, 28};
                g.FillRound(badge, 14, Color::Black(0.6f));
                g.FillCircle({badge.x + 16, badge.cy()}, 5, Color::Hex(0x30D158));
                TextStyle bs = Caps(11, Color::White());
                bs.valign = VAlign::Center;
                g.Text(L"JOGANDO", {badge.x + 28, badge.y, 90, badge.h}, bs);
            } else if (HasFreshNews(gm)) {
                Rect badge{cr.r() - 108, cr.y + 12, 96, 26};
                g.FillRound(badge, 13, theme_.accent);
                TextStyle bs = Caps(10, Color::White(), Align::Center);
                bs.valign = VAlign::Center;
                g.Text(L"NOVIDADE", badge, bs);
            }
            g.PopTransform();
            if (c.Click(r) && pt < 0.2f) {
                show_.row = 1;
                if (selected) OpenGame(gm.id, Tab::Home);
                else {
                    show_.sel = i;
                    show_.selId = gm.id;
                }
            }
            if (c.in.rightReleased && r.Contains(c.in.mouse) && !c.blockMouse) {
                show_.sel = i;
                show_.selId = gm.id;
                OpenGameMenu(gm);
            }
        }
        // Position indicator
        TextStyle pos = Caps(12, theme_.text3, Align::Trailing);
        g.Text(std::to_wstring(show_.sel + 1) + L" / " + std::to_wstring(n), {g.W - M - 200, L.carouselY - 30, 200, 18}, pos);
        g.PopAlpha();
    }

    // Back button while on a game page.
    if (pt > 0.01f) {
        g.PushAlpha(pt);
        if (IconButton(c, Id("back"), {M + 23, 55}, 46, glyph::Back, false)) CloseGamePage();
        g.PopAlpha();
    }
}

void App::DrawGamePage(Ctx& c, Game& game, float pt) {
    Gfx& g = gfx_;
    ShowcaseLayout L = Compute(g, settings_, pt);
    float W = g.W;
    float innerW = W - 2 * M;
    float lw = std::floor((innerW - 24) * 0.58f), rw = innerW - 24 - lw;
    Rect left{M, L.panelsY, lw, L.panelH}, right{M + lw + 24, L.panelsY, rw, L.panelH};

    // Left: progress + description
    Glass(c, left, 24);
    {
        struct Stat {
            std::wstring label, value;
        };
        std::vector<Stat> stats = {{L"TEMPO DE JOGO", FormatPlaytime(game.TotalMinutes())},
                                   {L"ÚLTIMA SESSÃO", Capitalized(FormatRelative(game.LastActivity()))}};
        if (game.sizeBytes > 0) stats.push_back({L"TAMANHO", FormatBytes(game.sizeBytes)});
        else stats.push_back({L"ADICIONADO", Capitalized(FormatRelative(game.addedAt))});
        float x = left.x + 32;
        for (auto& s : stats) {
            TextStyle ls = Caps(12, theme_.text2);
            TextStyle vs = Body(34, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            vs.font = FontRole::Display;
            float w = std::max(g.Measure(s.label, ls).x, g.Measure(s.value, vs).x) + 48;
            g.Text(s.label, {x, left.y + 30, w, 16}, ls);
            g.Text(s.value, {x, left.y + 50, w, 44}, vs);
            x += w;
        }
        TextStyle ds = Body(17, theme_.text2);
        ds.maxLines = 4;
        ds.lineHeight = 1.45f;
        std::wstring desc = game.description;
        if (desc.empty()) {
            ds.color = theme_.text3;
            desc = game.platform == Platform::Custom ? L"Jogo adicionado manualmente. Use o menu de opções para escolher capa, fundo e logo."
                                                     : L"Nenhuma descrição disponível.";
        }
        g.Text(desc, {left.x + 32, left.y + 126, left.w - 64, left.h - 150}, ds);
    }

    // Right: achievements summary
    Glass(c, right, 24);
    const AchievementSet* a = meta_.Achievements(game.id);
    {
        TextStyle ls = Caps(12, theme_.text2);
        g.Text(L"CONQUISTAS", {right.x + 32, right.y + 30, 200, 16}, ls);
        if (a && !a->source.empty() && a->state == AchievementSet::State::Ready) {
            TextStyle src = Body(12, theme_.text3);
            src.align = Align::Trailing;
            g.Text(a->source, {right.r() - 260, right.y + 29, 228, 18}, src);
        }
        int unlocked = a && a->state == AchievementSet::State::Ready ? a->unlocked : game.achUnlocked;
        int total = a && a->state == AchievementSet::State::Ready ? a->total : game.achTotal;
        if (total > 0) {
            float frac = static_cast<float>(unlocked) / total;
            TextStyle big = Body(46, theme_.accent, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            big.font = FontRole::Display;
            std::wstring pct = FormatPercent(frac);
            float pw = g.Measure(pct, big).x;
            g.Text(pct, {right.x + 32, right.y + 48, pw + 4, 60}, big);
            TextStyle cnt = Body(17, theme_.text2);
            cnt.valign = VAlign::Bottom;
            g.Text(std::to_wstring(unlocked) + L" de " + std::to_wstring(total) + L" desbloqueadas",
                   {right.x + 32 + pw + 14, right.y + 48, right.w - pw - 80, 50}, cnt);
            Progress(c, {right.x + 32, right.y + 124, right.w - 64, 6}, frac, theme_.accent, theme_.control);
            // Most recent unlocks
            float ix = right.x + 32, iy = right.y + 152;
            int shown = 0, maxIcons = static_cast<int>((right.w - 64 + 10) / 62);
            if (a) {
                for (const auto& ach : a->list) {
                    if (!ach.achieved || ach.icon.empty()) continue;
                    if (shown >= maxIcons) break;
                    Rect ir{ix, iy, 52, 52};
                    if (ID2D1Bitmap1* ic = images_.Get(ach.icon, 128)) g.Image(ic, ir, 12);
                    else g.FillRound(ir, 12, theme_.control);
                    ix += 62;
                    ++shown;
                }
            }
            if (a && !a->list.empty()) {
                BtnSpec all{L"Ver todas as conquistas", 0, BtnKind::Plain};
                all.fontSize = 15;
                all.glyph = glyph::ChevronRight;
                Rect br{right.x + 20, right.b() - 58, 250, 40};
                if (Button(c, Id("seeall"), br, all)) {
                    show_.pageScrollTarget = std::min(show_.pageMax, g.H - 60.f);
                    show_.row = 1;
                }
            } else if (a && !a->message.empty()) {
                TextStyle ms = Body(14, theme_.text3);
                ms.maxLines = 2;
                g.Text(a->message, {right.x + 32, right.b() - 62, right.w - 64, 44}, ms);
            }
        } else {
            std::wstring msg;
            if (!a || a->state == AchievementSet::State::Loading || a->state == AchievementSet::State::Unknown)
                msg = L"Carregando…";
            else
                msg = a->message.empty() ? L"Nenhuma informação de conquistas disponível." : a->message;
            g.Icon(glyph::Trophy, {right.x + 52, right.y + 98}, 30, theme_.text3);
            TextStyle ms = Body(17, theme_.text2);
            ms.maxLines = 4;
            ms.lineHeight = 1.4f;
            g.Text(msg, {right.x + 32, right.y + 140, right.w - 64, 110}, ms);
        }
    }

    // Below the fold
    float y = g.H + 20;
    float bottom = y;
    DrawNewsSection(c, game, y, bottom);
    DrawAchievementsSection(c, game, bottom + 56, bottom);
    DrawScreenshotsSection(c, game, bottom + 56, bottom);
    DrawAboutSection(c, game, bottom + 56, bottom);
    show_.pageMax = std::max(0.f, bottom + 80 - g.H);
}

void App::DrawAchievementsSection(Ctx& c, Game& game, float y, float& outBottom) {
    Gfx& g = gfx_;
    const AchievementSet* a = meta_.Achievements(game.id);
    TextStyle hs = Body(34, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hs.font = FontRole::Display;
    g.Text(L"Conquistas", {M, y, 600, 46}, hs);
    y += 52;
    if (!a || a->state != AchievementSet::State::Ready || a->list.empty()) {
        std::wstring msg = !a || a->state == AchievementSet::State::Loading ? L"Carregando…"
                           : a->message.empty() ? L"Nenhuma conquista para mostrar." : a->message;
        TextStyle ms = Body(17, theme_.text2);
        ms.maxLines = 3;
        g.Text(msg, {M, y, g.W - 2 * M, 60}, ms);
        outBottom = y + 60;
        return;
    }
    std::wstring info = std::to_wstring(a->unlocked) + L" de " + std::to_wstring(a->total) +
                        L" desbloqueadas  ·  Fonte: " + a->source;
    g.Text(info, {M, y, g.W - 2 * M, 22}, Body(16, theme_.text2));
    y += 28;
    if (!a->complete || !a->message.empty()) {
        TextStyle ms = Body(14, theme_.text3);
        ms.maxLines = 2;
        g.Text(a->message, {M, y, g.W - 2 * M, 40}, ms);
        y += 30;
    }
    y += 18;
    int cols = g.W >= 1700 ? 3 : 2;
    float gap = 18;
    float tw = (g.W - 2 * M - gap * (cols - 1)) / cols, th = 124;
    float scrollTop = show_.pageScroll, viewBottom = show_.pageScroll + g.H;
    for (size_t i = 0; i < a->list.size(); ++i) {
        const Achievement& ach = a->list[i];
        Rect r{M + (i % cols) * (tw + gap), y + (i / cols) * (th + gap), tw, th};
        if (r.b() < scrollTop - 20 || r.y > viewBottom + 20) continue;
        Glass(c, r, 20, 0.9f);
        Rect ir{r.x + 20, r.y + 22, 64, 64};
        const std::wstring& iconSrc = (!ach.achieved && !ach.iconGray.empty()) ? ach.iconGray : ach.icon;
        if (ID2D1Bitmap1* ic = iconSrc.empty() ? nullptr : images_.Get(iconSrc, 128)) {
            g.Image(ic, ir, 14, ach.achieved ? 1.f : 0.38f);
        } else {
            g.FillRound(ir, 14, theme_.control);
            g.Icon(glyph::Trophy, {ir.cx(), ir.cy()}, 24, theme_.text3);
        }
        if (!ach.achieved) {
            g.FillCircle({ir.r() - 4, ir.b() - 4}, 13, theme_.dark ? Color::Hex(0x2C2C2E) : Color::White());
            g.Icon(glyph::Lock, {ir.r() - 4, ir.b() - 4}, 12, theme_.text2);
        }
        bool secret = ach.hidden && !ach.achieved;
        float tx = ir.r() + 18, tWidth = r.r() - tx - 20;
        g.Text(secret ? L"Conquista secreta" : ach.name, {tx, r.y + 18, tWidth, 24},
               Body(17, ach.achieved ? theme_.text : theme_.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        TextStyle ds = Body(14, theme_.text2);
        ds.maxLines = 2;
        g.Text(secret ? L"Os detalhes aparecem quando você desbloquear." : ach.description, {tx, r.y + 44, tWidth, 40}, ds);
        std::wstring meta;
        if (ach.achieved && ach.unlockedAt > 0) meta = FormatDate(ach.unlockedAt);
        if (ach.globalPercent > 0.f) {  // 0 in Steam's cache means "unknown", not "nobody"
            wchar_t buf[48];
            swprintf_s(buf, L"%.1f%% dos jogadores", ach.globalPercent);
            std::wstring p = buf;
            for (auto& ch : p)
                if (ch == L'.') ch = L',';
            meta += (meta.empty() ? L"" : L"  ·  ") + p;
        }
        TextStyle ms = Body(12, ach.achieved ? theme_.accent : theme_.text3, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        g.Text(meta, {tx, r.b() - 30, tWidth, 18}, ms);
    }
    int rows = static_cast<int>((a->list.size() + cols - 1) / cols);
    outBottom = y + rows * (th + gap);
}

void App::DrawScreenshotsSection(Ctx& c, Game& game, float y, float& outBottom) {
    const auto& shots = ScreenshotsOf(game);
    if (shots.empty()) {
        outBottom = y - 56;  // nothing to show: no gap either
        return;
    }
    Gfx& g = gfx_;
    TextStyle hs = Body(34, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hs.font = FontRole::Display;
    g.Text(L"Suas capturas", {M, y, 600, 46}, hs);
    std::wstring count = std::to_wstring(shots.size()) + (shots.size() == 1 ? L" captura" : L" capturas") +
                         L" feitas com a Steam (F12)";
    g.Text(count, {M, y + 52, 800, 22}, Body(16, theme_.text2));
    BtnSpec folder{L"Abrir pasta", glyph::Folder, BtnKind::Glass};
    folder.fontSize = 15;
    Rect fr{g.W - M - 170, y + 6, 170, 44};
    if (Button(c, Id("shotsfolder"), fr, folder))
        ShellExecuteW(nullptr, L"open", ParentDir(shots.front().first).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    y += 92;
    int cols = 4;
    float gap = 18, tw = (g.W - 2 * M - gap * (cols - 1)) / cols, th = tw * 9.f / 16.f;
    size_t shown = std::min<size_t>(shots.size(), 8);
    for (size_t i = 0; i < shown; ++i) {
        Rect r{M + (i % cols) * (tw + gap), y + (i / cols) * (th + gap), tw, th};
        bool hover = c.Hover(r);
        if (hover) c.wantHand = true;
        float h = c.Anim(Id("shot", i), hover ? 1.f : 0.f, 18.f);
        float sc = 1.f + 0.03f * h * c.Motion();
        g.PushTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(r.cx(), r.cy())));
        g.SoftShadow(r, 14, 14, Color::Black(0.35f), 6);
        if (ID2D1Bitmap1* b = images_.Get(shots[i].first, 960)) g.Image(b, r, 14);
        else g.FillRound(r, 14, theme_.control);
        if (h > 0.01f) g.StrokeRound(r.Inset(-4), 18, theme_.text.Fade(h), 2.5f);
        g.PopTransform();
        if (c.Click(r)) ShellExecuteW(nullptr, L"open", shots[i].first.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    int rows = static_cast<int>((shown + cols - 1) / cols);
    outBottom = y + rows * (th + gap) - gap;
}

void App::DrawAboutSection(Ctx& c, Game& game, float y, float& outBottom) {
    Gfx& g = gfx_;
    TextStyle hs = Body(34, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hs.font = FontRole::Display;
    g.Text(L"Sobre", {M, y, 600, 46}, hs);
    y += 64;
    std::vector<std::pair<std::wstring, std::wstring>> rows;
    auto add = [&](const wchar_t* k, const std::wstring& v) {
        if (!v.empty()) rows.emplace_back(k, v);
    };
    add(L"Suas notas", game.notes);
    if (game.status) add(L"Status", PlayStatusName(static_cast<PlayStatus>(game.status)));
    add(L"Desenvolvedor", game.developer);
    add(L"Distribuidora", game.publisher);
    add(L"Lançamento", game.releaseDate);
    add(L"Gêneros", game.genres);
    add(L"Plataforma", PlatformName(game.platform));
    if (game.sizeBytes) add(L"Tamanho em disco", FormatBytes(game.sizeBytes));
    add(L"Na biblioteca desde", FormatDate(game.addedAt));
    if (game.launches > 0)
        add(L"Aberto pelo Lumen", std::to_wstring(game.launches) + (game.launches == 1 ? L" vez" : L" vezes") +
                                      L" · última " + FormatRelative(game.lastLaunch));
    if (game.trackedSeconds > 0) add(L"Tempo medido pelo Lumen", FormatPlaytime(game.trackedSeconds / 60));
    std::wstring cats;
    for (const auto& cat : game.categories) cats += (cats.empty() ? L"" : L", ") + cat;
    for (const auto& cat : game.platformCollections) cats += (cats.empty() ? L"" : L", ") + cat;
    add(L"Categorias", cats);
    add(L"Pasta", game.installDir.empty() ? game.exePath : game.installDir);

    int cols = 2;
    float colW = (g.W - 2 * M - 24) / cols;
    float rowH = 70;
    int perCol = static_cast<int>((rows.size() + 1) / 2);
    Rect panel{M, y, g.W - 2 * M, perCol * rowH + 28};
    Glass(c, panel, 24, 0.95f);
    for (size_t i = 0; i < rows.size(); ++i) {
        int col = static_cast<int>(i) / std::max(1, perCol), row = static_cast<int>(i) % std::max(1, perCol);
        float x = panel.x + 32 + col * (colW + 24), ry = panel.y + 18 + row * rowH;
        g.Text(ToUpper(rows[i].first), {x, ry + 8, colW - 48, 16}, Caps(11, theme_.text3));
        g.Text(rows[i].second, {x, ry + 28, colW - 48, 26}, Body(17, theme_.text));
        if (row > 0) g.FillRect({x, ry, colW - 48, 1}, theme_.separator);
    }
    outBottom = panel.b();
}

}  // namespace lumen
