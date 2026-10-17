// Deals: promotions shown on the home pages of third-party stores, in one consistent grid.
#include <algorithm>
#include <cmath>

#include "App.h"
#include "core/Util.h"

namespace lumen {

using namespace ui;
using D2D1::Matrix3x2F;

namespace {
constexpr float M = 72.f;

std::wstring Money(const std::wstring& currency, double v) {
    long long cents = std::llround(v * 100.0);
    std::wstring s = FormatInt(cents / 100) + L"," + (cents % 100 < 10 ? L"0" : L"") + std::to_wstring(cents % 100);
    return (currency.empty() ? L"" : currency + L" ") + s;
}

Color StoreColor(const std::string& id) {
    if (id == "gmg") return Color::Hex(0x0FA968);
    if (id == "ig") return Color::Hex(0xFF5400);
    if (id == "driffle") return Color::Hex(0x6E4BFF);
    return Color::Hex(0x8E8E93);
}
}  // namespace

void App::DrawDeals(Ctx& c) {
    Gfx& g = gfx_;
    InputFrame& in = c.in;

    // Fetch on first visit and whenever the cache gets old (the service itself caches for 3 h).
    if (time_ - dealsUi_.lastRequest > 1800) {
        dealsUi_.lastRequest = time_;
        deals_.Refresh(false);
    }
    if (deals_.Poll(dealsUi_.stores)) {
        // Flatten, and mark deals for games already in the library ("Você já tem").
        dealsUi_.items.clear();
        for (size_t s = 0; s < dealsUi_.stores.size(); ++s)
            for (const auto& d : dealsUi_.stores[s].deals) {
                DealItem it{s, d, false};
                for (const auto& gm : library_.games())
                    if (gm.name.size() > 3 && !gm.isApp && ContainsInsensitive(d.title, gm.name)) {
                        it.owned = true;
                        break;
                    }
                dealsUi_.items.push_back(std::move(it));
            }
    }
    auto& st = dealsUi_;

    // Visible list
    std::vector<int> visible;
    for (size_t i = 0; i < st.items.size(); ++i)
        if (st.filter == 0 || static_cast<int>(st.items[i].store) == st.filter - 1) visible.push_back(static_cast<int>(i));
    std::stable_sort(visible.begin(), visible.end(), [&](int a, int b) {
        const Deal& x = st.items[a].deal;
        const Deal& y = st.items[b].deal;
        if (st.sort == 1) return x.price < y.price;
        return x.discount > y.discount;
    });
    int count = static_cast<int>(visible.size());
    st.sel = std::clamp(st.sel, 0, std::max(0, count - 1));

    // Layout
    float gap = 24;
    int cols = std::max(1, static_cast<int>((g.W - 2 * M + gap) / (380 + gap)));
    float cw = (g.W - 2 * M - gap * (cols - 1)) / cols, imgH = cw * 0.5625f, ch = imgH + 128;
    float top = 316;
    int rows = (count + cols - 1) / cols;
    std::vector<size_t> unsupported;
    for (size_t s = 0; s < st.stores.size(); ++s)
        if (st.stores[s].state == StoreDeals::State::Unsupported) unsupported.push_back(s);
    float contentH = top + rows * (ch + gap) + (unsupported.empty() ? 0 : 150) + 40;

    // Keyboard / controller
    bool kb = !in.mouseMode;
    std::vector<int> chipStores;  // store index for each chip after "Todas"
    for (size_t s = 0; s < st.stores.size(); ++s)
        if (st.stores[s].state != StoreDeals::State::Unsupported) chipStores.push_back(static_cast<int>(s));
    if (!sheet_.open) {
        if (st.zone == 0) {
            int chip = 0;
            for (size_t k = 0; k < chipStores.size(); ++k)
                if (st.filter == chipStores[k] + 1) chip = static_cast<int>(k) + 1;
            if (in.Has(Key::Left)) chip = std::max(0, chip - 1);
            if (in.Has(Key::Right)) chip = std::min(static_cast<int>(chipStores.size()), chip + 1);
            st.filter = chip == 0 ? 0 : chipStores[chip - 1] + 1;
            if (in.Has(Key::Down) || in.Has(Key::Accept)) st.zone = 1;
        } else if (count > 0) {
            int s = st.sel;
            if (in.Has(Key::Left)) s = std::max(0, s - 1);
            if (in.Has(Key::Right)) s = std::min(count - 1, s + 1);
            if (in.Has(Key::Down)) s = std::min(count - 1, s + cols);
            if (in.Has(Key::Up)) {
                if (s < cols) st.zone = 0;
                else s -= cols;
            }
            if (s != st.sel) {
                st.sel = s;
                float rowTop = top + (s / cols) * (ch + gap);
                if (rowTop - st.scrollTarget < top) st.scrollTarget = std::max(0.f, rowTop - top);
                if (rowTop + ch - st.scrollTarget > g.H - 30) st.scrollTarget = rowTop + ch - g.H + 30;
            }
            if (in.Has(Key::Accept)) {
                const Deal& d = st.items[visible[st.sel]].deal;
                ShellExecuteW(nullptr, L"open", d.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                PlaySfx(Sfx::Select);
            }
        }
        if (in.Has(Key::Back)) SwitchTab(Tab::Home);
        st.scrollTarget -= in.wheel * 150.f;
    }
    st.scrollTarget = std::clamp(st.scrollTarget, 0.f, std::max(0.f, contentH - g.H));
    st.scroll += (st.scrollTarget - st.scroll) * (1.f - std::exp(-c.dt * 14.f));

    // Ambient tint from the best deal
    if (count > 0) {
        if (ID2D1Bitmap1* bmp = images_.Get(st.items[visible[0]].deal.image, 960))
            if (ID2D1Bitmap1* blurred = g.Blurred(st.items[visible[0]].deal.image, bmp))
                g.Image(blurred, {0, 0, g.W, g.H}, 0, theme_.dark ? 0.35f : 0.25f, Fit::Cover);
        g.Gradient({0, 0, g.W, g.H}, 0, {0, 0}, {0, g.H}, {{0.f, theme_.bg.A(0.55f)}, {1.f, theme_.bg.A(0.97f)}});
    }

    g.PushTransform(Matrix3x2F::Translation(0, -st.scroll));
    c.scrollY = st.scroll;

    // Header
    TextStyle title = Body(46, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    title.font = FontRole::Display;
    g.Text(L"Ofertas", {M, 112, 600, 60}, title);
    int64_t newest = 0;
    bool loading = false;
    for (const auto& s : st.stores) {
        newest = std::max(newest, s.fetchedAt);
        loading |= s.state == StoreDeals::State::Loading;
    }
    std::wstring sub = L"Promoções em destaque nas páginas das lojas";
    if (loading) sub += L"  ·  atualizando…";
    else if (newest) sub += L"  ·  atualizado " + FormatRelative(newest) + L" às " + FormatDateTime(newest).substr(8);
    g.Text(sub, {M + 2, 172, 1100, 24}, Body(17, theme_.text2));
    BtnSpec refresh{L"Atualizar", glyph::Refresh, BtnKind::Glass};
    refresh.fontSize = 15;
    Rect rr{g.W - M - 170, 120, 170, 46};
    if (Button(c, Id("dealsrefresh"), rr, refresh)) {
        if (time_ - st.lastForced > 120) {  // be gentle with the stores
            st.lastForced = time_;
            deals_.Refresh(true);
            ShowToast(L"Buscando ofertas…", glyph::Refresh);
        } else {
            ShowToast(L"Acabou de atualizar. Tente de novo em alguns minutos.", glyph::Info);
        }
    }

    // Filter chips + sort
    {
        float x = M, cy = 228;
        auto chip = [&](const std::wstring& label, int filterValue, uint64_t id, Color dot) {
            int n = 0;
            for (const auto& it : st.items)
                if (filterValue == 0 || static_cast<int>(it.store) == filterValue - 1) ++n;
            std::wstring text = label + L"  " + std::to_wstring(n);
            float w = ChipWidth(c, text) + (dot.a > 0 ? 14 : 0);
            Rect r{x, cy, w, 40};
            bool focused = kb && st.zone == 0 && st.filter == filterValue;
            if (Chip(c, id, r, text, st.filter == filterValue, focused)) {
                st.filter = filterValue;
                st.sel = 0;
                st.scrollTarget = 0;
            }
            if (dot.a > 0) g.FillCircle({r.r() - 16, r.cy()}, 5, dot);
            x += w + 10;
        };
        chip(L"Todas", 0, Id("dealchip", 0), {0, 0, 0, 0});
        for (int s : chipStores) chip(st.stores[s].name, s + 1, Id("dealchip", s + 1), StoreColor(st.stores[s].id));
        std::vector<std::wstring> sorts = {L"Maior desconto", L"Menor preço"};
        Rect sr{g.W - M - 300, cy - 2, 300, 44};
        int clicked = Segmented(c, Id("dealsort"), sr, sorts, st.sort, false);
        if (clicked >= 0) st.sort = clicked;
    }

    // Store problems (offline, page changed...)
    float y = top;
    for (const auto& s : st.stores) {
        if (s.state != StoreDeals::State::Error || (st.filter != 0 && &s != &st.stores[st.filter - 1])) continue;
        g.Text(s.name + L": " + s.message, {M, y - 30, g.W - 2 * M, 20}, Body(14, theme_.text3));
    }

    // Grid
    if (count == 0) {
        TextStyle h = Body(22, theme_.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        h.align = Align::Center;
        bool anyLoading = loading || st.stores.empty();
        if (anyLoading) {
            for (int i = 0; i < cols * 2; ++i) {
                Rect r{M + (i % cols) * (cw + gap), y + (i / cols) * (ch + gap), cw, ch};
                float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(time_) * 3.f + i * 0.4f);
                g.FillRound(r, 18, theme_.control.Fade(pulse));
            }
        } else {
            g.Text(settings_.onlineData ? L"Nenhuma oferta encontrada agora." : L"Ative “Buscar informações online” em Ajustes para ver ofertas.",
                   {0, y + 60, g.W, 30}, h);
        }
    }
    for (int i = 0; i < count; ++i) {
        const DealItem& it = st.items[visible[i]];
        const Deal& d = it.deal;
        Rect r{M + (i % cols) * (cw + gap), y + (i / cols) * (ch + gap), cw, ch};
        if (r.b() < st.scroll - 20 || r.y > st.scroll + g.H + 20) continue;
        bool hover = c.Hover(r);
        if (hover) {
            c.wantHand = true;
            if (in.mouseMode) st.sel = i;
        }
        bool focused = kb && st.zone == 1 && st.sel == i;
        float f = c.Anim(Id(Narrow(d.url), 0xF0C5), focused ? 1.f : 0.f, 16.f);
        float h = c.Anim(Id(Narrow(d.url), 0x40E7), hover ? 1.f : 0.f, 18.f);
        Vec2 m = c.Mouse();
        float tx = c.Anim(Id(Narrow(d.url), 0x7117), hover ? std::clamp((m.x - r.cx()) / r.w, -0.5f, 0.5f) : 0.f, 10.f);
        float ty = c.Anim(Id(Narrow(d.url), 0x7118), hover ? std::clamp((m.y - r.cy()) / r.h, -0.5f, 0.5f) : 0.f, 10.f);
        float sc = 1.f + (0.035f * std::max(f, h)) * c.Motion();
        g.PushTransform(Matrix3x2F::Scale(sc, sc, D2D1::Point2F(r.cx(), r.cy())) *
                        Matrix3x2F::Skew(-ty * 3.f * c.Motion(), tx * 3.f * c.Motion(), D2D1::Point2F(r.cx(), r.cy())));
        g.SoftShadow(r, 18, 18 + 8 * std::max(f, h), Color::Black(theme_.dark ? 0.35f : 0.12f), 8);
        g.FillRound(r, 18, theme_.dark ? Color::Hex(0x1C1C1E, 0.92f) : Color::White(0.95f));
        Rect img{r.x, r.y, r.w, imgH};
        g.PushClip(r, 18);
        if (ID2D1Bitmap1* b = images_.Get(d.image, 960)) {
            float fade = c.AnimFrom(Id(Narrow(d.image), 0xFADE), 1.f, 0.f, 10.f);
            g.Image(b, img, 0, fade, Fit::Cover, {0.5f, 0.4f});
        } else {
            g.FillRect(img, theme_.control);
        }
        if (h > 0.01f && !settings_.reduceMotion) g.Glow(m, r.w * 0.7f, Color::White(0.14f * h));
        g.PopClip();
        // Discount badge + store pill on the artwork
        TextStyle badge = Body(16, Color::White(), DWRITE_FONT_WEIGHT_BOLD);
        badge.align = Align::Center;
        badge.valign = VAlign::Center;
        std::wstring pct = L"-" + std::to_wstring(d.discount) + L"%";
        float bw = g.Measure(pct, badge).x + 24;
        Rect br{r.x + 14, r.y + 14, bw, 32};
        g.FillRound(br, 16, theme_.accent);
        g.Text(pct, br, badge);
        const StoreDeals& store = st.stores[it.store];
        TextStyle sp = Caps(10, Color::White(), Align::Center);
        sp.valign = VAlign::Center;
        float sw = g.Measure(ToUpper(store.name), sp).x + 26;
        Rect pr{r.r() - 14 - sw, r.y + 16, sw, 26};
        g.FillRound(pr, 13, Color::Black(0.62f));
        g.FillCircle({pr.x + 11, pr.cy()}, 4, StoreColor(store.id));
        g.Text(ToUpper(store.name), {pr.x + 8, pr.y, pr.w, pr.h}, sp);
        if (it.owned) {
            Rect ob{r.x + 14, img.b() - 40, 150, 26};
            g.FillRound(ob, 13, Color::Black(0.62f));
            TextStyle os = Caps(10, Color::White(), Align::Center);
            os.valign = VAlign::Center;
            g.Text(L"NA SUA BIBLIOTECA", ob, os);
        }
        // Text block
        TextStyle ts = Body(18, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        ts.maxLines = 2;
        ts.lineHeight = 1.2f;
        g.Text(d.title, {r.x + 18, img.b() + 14, r.w - 36, 46}, ts);
        g.Text(d.platform, {r.x + 18, img.b() + 62, r.w - 36, 18}, Body(13, theme_.text3));
        TextStyle ps = Body(22, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        ps.font = FontRole::Display;
        std::wstring now = (d.approx ? L"≈ " : L"") + Money(d.currency, d.price);
        float pw = g.Measure(now, ps).x;
        g.Text(now, {r.x + 18, r.b() - 42, pw + 4, 30}, ps);
        if (d.oldPrice > d.price) {
            TextStyle os = Body(15, theme_.text3);
            std::wstring old = Money(d.currency, d.oldPrice);
            float ow = g.Measure(old, os).x;
            Rect orr{r.x + 18 + pw + 12, r.b() - 36, ow + 2, 20};
            g.Text(old, orr, os);
            g.Line({orr.x, orr.cy() + 1}, {orr.x + ow, orr.cy() + 1}, theme_.text3, 1.2f);  // strikethrough
        }
        if (f > 0.01f) g.StrokeRound(r.Inset(-5), 23, theme_.text.Fade(f), 3.f);
        g.PopTransform();
        if (c.Click(r)) {
            ShellExecuteW(nullptr, L"open", d.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            PlaySfx(Sfx::Select);
        }
    }

    // Stores that cannot be read automatically: honest shortcut instead of fake data.
    if (!unsupported.empty() && st.filter == 0) {
        float uy = y + rows * (ch + gap) + 16;
        Rect panel{M, uy, g.W - 2 * M, 112};
        Glass(c, panel, 22, 0.95f);
        g.Text(L"OUTRAS LOJAS", {panel.x + 28, panel.y + 24, 300, 16}, Caps(12, theme_.text2));
        std::wstring why;
        for (size_t s : unsupported) why += (why.empty() ? L"" : L"  ") + st.stores[s].message;
        TextStyle ws = Body(15, theme_.text2);
        ws.maxLines = 2;
        g.Text(why + L" Abra o site para ver as ofertas.", {panel.x + 28, panel.y + 48, panel.w * 0.6f, 44}, ws);
        float bx = panel.r() - 24;
        for (auto it = unsupported.rbegin(); it != unsupported.rend(); ++it) {
            const StoreDeals& s = st.stores[*it];
            BtnSpec b{L"Abrir " + s.name, glyph::Link, BtnKind::Glass};
            b.fontSize = 15;
            float w = g.Measure(b.label, Body(15, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD)).x + 72;
            Rect r{bx - w, panel.cy() - 23, w, 46};
            if (Button(c, Id(s.id, 0x0AE), r, b)) ShellExecuteW(nullptr, L"open", s.homeUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            bx -= w + 12;
        }
    }

    g.PopTransform();
    c.scrollY = 0;
    // Content scrolling under the navigation fades out instead of colliding with it.
    if (st.scroll > 1.f) {
        float k = std::clamp(st.scroll / 80.f, 0.f, 1.f);
        g.Gradient({0, 0, g.W, 140}, 0, {0, 0}, {0, 140},
                   {{0.f, theme_.bg.A(0.95f * k)}, {0.6f, theme_.bg.A(0.7f * k)}, {1.f, theme_.bg.A(0)}});
    }
}

}  // namespace lumen
