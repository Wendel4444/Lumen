#include "Ui.h"

#include <algorithm>
#include <cmath>

#include "core/Util.h"

namespace lumen::ui {

using D2D1::Matrix3x2F;

const Color kAccents[kAccentCount] = {Color::Hex(0xE8412C), Color::Hex(0x0A84FF), Color::Hex(0x5E5CE6),
                                      Color::Hex(0x30D158), Color::Hex(0xFF9F0A), Color::Hex(0xFF375F)};
const wchar_t* kAccentNames[kAccentCount] = {L"Vermelho", L"Azul", L"Índigo", L"Verde", L"Laranja", L"Rosa"};

Color VividFrom(Color c) {
    // RGB -> HSV, then push saturation/brightness into a range that works as a button colour.
    float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    float h = 0;
    if (d > 1e-4f) {
        if (mx == c.r) h = std::fmod((c.g - c.b) / d, 6.f);
        else if (mx == c.g) h = (c.b - c.r) / d + 2.f;
        else h = (c.r - c.g) / d + 4.f;
        h /= 6.f;
        if (h < 0) h += 1.f;
    }
    float s = mx > 0 ? d / mx : 0;
    if (s < 0.12f) return Color::Hex(0x8E8E93);  // greyscale art: neutral grey accent
    return Color::Hsv(h, std::clamp(s * 1.4f, 0.55f, 0.85f), 0.88f);
}

Theme MakeTheme(bool dark, int accent) {
    Theme t;
    t.dark = dark;
    t.accent = kAccents[std::clamp(accent, 0, kAccentCount - 1)];
    t.accentText = Color::White();
    if (dark) {
        t.bg = Color::Hex(0x0B0B0D);
        t.bg2 = Color::Hex(0x161618);
        t.text = Color::White();
        t.text2 = Color::White(0.64f);
        t.text3 = Color::White(0.40f);
        t.separator = Color::White(0.09f);
        t.glass = Color{0.07f, 0.07f, 0.08f, 0.55f};
        t.glassStroke = Color::White(0.10f);
        t.control = Color::White(0.10f);
        t.controlHover = Color::White(0.18f);
        t.focusFill = Color::White();
        t.focusText = Color::Hex(0x0B0B0D);
        t.scrim = Color::Hex(0x050506);
    } else {
        t.bg = Color::Hex(0xF2F2F7);
        t.bg2 = Color::Hex(0xFFFFFF);
        t.text = Color::Hex(0x1C1C1E);
        t.text2 = Color::Hex(0x1C1C1E, 0.62f);
        t.text3 = Color::Hex(0x1C1C1E, 0.40f);
        t.separator = Color::Hex(0x000000, 0.08f);
        t.glass = Color{1.f, 1.f, 1.f, 0.62f};
        t.glassStroke = Color::Hex(0x000000, 0.06f);
        t.control = Color::Hex(0x000000, 0.06f);
        t.controlHover = Color::Hex(0x000000, 0.10f);
        t.focusFill = Color::Hex(0x1C1C1E);
        t.focusText = Color::White();
        t.scrim = Color::Hex(0xF2F2F7);
    }
    return t;
}

uint64_t Id(const char* s, uint64_t salt) { return Hash64(s) * 31 + salt * 0x9E3779B97F4A7C15ull; }
uint64_t Id(const std::string& s, uint64_t salt) { return Hash64(s) * 31 + salt * 0x9E3779B97F4A7C15ull; }

// --- Ctx -------------------------------------------------------------------

float Ctx::Anim(uint64_t id, float target, float speed) {
    auto it = anims_.find(id);
    if (it == anims_.end()) {
        anims_[id] = {target, t};
        return target;
    }
    AnimState& a = it->second;
    a.touched = t;
    if (settings.reduceMotion) speed *= 2.5f;
    float k = 1.f - std::exp(-dt * speed);
    a.v += (target - a.v) * k;
    if (std::fabs(target - a.v) < 0.0005f) a.v = target;
    return a.v;
}

float Ctx::AnimFrom(uint64_t id, float target, float initial, float speed) {
    if (anims_.find(id) == anims_.end()) anims_[id] = {initial, t};
    return Anim(id, target, speed);
}

void Ctx::SetAnim(uint64_t id, float value) { anims_[id] = {value, t}; }

bool Ctx::Hover(const Rect& r) const { return !blockMouse && in.mouseMode && r.Contains(Mouse()); }

bool Ctx::Click(const Rect& r) const {
    return !blockMouse && in.released && r.Contains(Mouse()) && r.Contains(PressPos());
}

bool Ctx::Pressing(const Rect& r) const {
    return !blockMouse && in.mouseDown && r.Contains(Mouse()) && r.Contains(PressPos());
}

void Ctx::Collect() {
    for (auto it = anims_.begin(); it != anims_.end();)
        it = (t - it->second.touched > 30.0) ? anims_.erase(it) : std::next(it);
}

// --- Widgets ---------------------------------------------------------------

TextStyle Caps(float size, Color color, Align align) {
    TextStyle s;
    s.size = size;
    s.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    s.color = color;
    s.tracking = size * 0.14f;
    s.align = align;
    return s;
}

TextStyle Body(float size, Color color, DWRITE_FONT_WEIGHT w) {
    TextStyle s;
    s.size = size;
    s.color = color;
    s.weight = w;
    return s;
}

void Glass(Ctx& c, const Rect& r, float radius, float strength) {
    Gfx& g = c.g;
    g.SoftShadow(r, radius, 22.f, Color::Black(c.th.dark ? 0.22f * strength : 0.06f * strength), 8.f);
    if (c.settings.blur && c.backdrop.bmp) {
        g.PushClip(r, radius);
        g.Image(c.backdrop.bmp, c.backdrop.rect, 0, c.backdrop.opacity * strength, Fit::Fill);
        g.PopClip();
    }
    Color fill = c.th.glass;
    if (!c.settings.blur || !c.backdrop.bmp) fill.a = std::min(1.f, fill.a + (c.th.dark ? 0.2f : 0.25f));
    g.FillRound(r, radius, fill.Fade(strength));
    g.StrokeRound(r, radius, c.th.glassStroke.Fade(strength), 1.f);
}

void FocusRing(Ctx& c, const Rect& r, float radius, float t, float gap) {
    if (t <= 0.01f) return;
    Rect ring{r.x - gap, r.y - gap, r.w + gap * 2, r.h + gap * 2};
    c.g.StrokeRound(ring, radius + gap, c.th.text.Fade(t), 3.f);
}

bool Button(Ctx& c, uint64_t id, const Rect& r, const BtnSpec& spec) {
    Gfx& g = c.g;
    bool hover = c.Hover(r);
    if (hover) c.wantHand = true;
    float f = c.Anim(id ^ 0xF0C5, spec.focused ? 1.f : 0.f, 16.f);
    float h = c.Anim(id ^ 0x40E7, hover ? 1.f : 0.f, 20.f);
    float p = c.Anim(id ^ 0x9E55, c.Pressing(r) ? 1.f : 0.f, 30.f);
    float sc = 1.f + (0.045f * f + 0.012f * h) * c.Motion() - 0.03f * p;
    float radius = r.h * 0.5f;

    g.PushTransform(Matrix3x2F::Scale(sc, sc, D2D1::Point2F(r.cx(), r.cy())));
    Color fg = c.th.text;
    if (spec.kind == BtnKind::Primary) {
        Color fill = Color::Lerp(c.th.accent, Color::White(), 0.10f * h);
        g.SoftShadow(r, radius, 20.f + 10.f * f, c.th.accent.A(0.18f + 0.32f * f), 4.f);
        g.Gradient(r, radius, {r.x, r.y}, {r.x, r.b()},
                   {{0.f, Color::Lerp(fill, Color::White(), 0.10f)}, {1.f, fill}});
        g.StrokeRound(r, radius, Color::White(0.14f + 0.86f * f), 1.f + 2.f * f);
        fg = c.th.accentText;
    } else if (spec.kind == BtnKind::Glass) {
        Color base = Color::Lerp(c.th.control, c.th.controlHover, h);
        if (spec.on) base = Color::Lerp(base, c.th.text, 0.18f);
        Color fill = Color::Lerp(base, c.th.focusFill, f);
        if (f > 0.01f) g.SoftShadow(r, radius, 18.f, Color::Black(0.3f * f), 6.f);
        g.FillRound(r, radius, fill);
        if (f < 0.99f) g.StrokeRound(r, radius, c.th.glassStroke.Fade(1.f - f), 1.f);
        fg = Color::Lerp(c.th.text, c.th.focusText, f);
    } else {
        Color fill = Color::Lerp(c.th.control.A(0), c.th.control, std::max(h, f));
        g.FillRound(r, radius, fill);
        FocusRing(c, r, radius, f, 3.f);
    }

    TextStyle ts = Body(spec.fontSize, fg, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float textW = spec.label.empty() ? 0 : g.Measure(spec.label, ts).x;
    float iconW = spec.glyph ? spec.fontSize * 1.15f : 0;
    float gap = (spec.glyph && !spec.label.empty()) ? spec.fontSize * 0.55f : 0;
    float total = iconW + gap + textW;
    float x = r.cx() - total * 0.5f;
    if (spec.glyph && spec.sublabel.empty()) g.Icon(spec.glyph, {x + iconW * 0.5f, r.cy()}, spec.fontSize * 1.05f, fg);
    if (!spec.label.empty()) {
        ts.valign = VAlign::Center;
        if (spec.sublabel.empty()) {
            g.Text(spec.label, {x + iconW + gap, r.y, textW + 4, r.h}, ts);
        } else {
            TextStyle sub = Body(spec.fontSize * 0.68f, fg.Fade(0.7f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            float sw = g.Measure(spec.sublabel, sub).x;
            float x2 = r.cx() - (iconW + gap + std::max(textW, sw)) * 0.5f + iconW + gap;
            if (spec.glyph) g.Icon(spec.glyph, {x2 - gap - iconW * 0.5f, r.cy()}, spec.fontSize * 1.05f, fg);
            TextStyle top = ts;
            top.valign = VAlign::Bottom;
            g.Text(spec.label, {x2, r.y, textW + 4, r.h * 0.6f}, top);
            sub.valign = VAlign::Top;
            g.Text(spec.sublabel, {x2, r.y + r.h * 0.58f, sw + 4, r.h * 0.4f}, sub);
        }
    }
    g.PopTransform();
    return c.Click(r);
}

bool IconButton(Ctx& c, uint64_t id, Vec2 center, float d, wchar_t glyph, bool focused, bool on, Color onColor) {
    Gfx& g = c.g;
    Rect r{center.x - d * 0.5f, center.y - d * 0.5f, d, d};
    bool hover = c.Hover(r);
    if (hover) c.wantHand = true;
    float f = c.Anim(id ^ 0xF0C5, focused ? 1.f : 0.f, 16.f);
    float h = c.Anim(id ^ 0x40E7, hover ? 1.f : 0.f, 20.f);
    float p = c.Anim(id ^ 0x9E55, c.Pressing(r) ? 1.f : 0.f, 30.f);
    float sc = 1.f + (0.07f * f + 0.03f * h) * c.Motion() - 0.06f * p;
    g.PushTransform(Matrix3x2F::Scale(sc, sc, D2D1::Point2F(center.x, center.y)));
    if (f > 0.01f) g.SoftShadow(r, d * 0.5f, 18.f, Color::Black(0.3f * f), 6.f);
    Color fill = Color::Lerp(Color::Lerp(c.th.control, c.th.controlHover, h), c.th.focusFill, f);
    g.FillCircle(center, d * 0.5f, fill);
    if (f < 0.99f) g.StrokeCircle(center, d * 0.5f - 0.5f, c.th.glassStroke.Fade(1.f - f), 1.f);
    Color fg = Color::Lerp(c.th.text, c.th.focusText, f);
    if (on && onColor.a > 0) fg = Color::Lerp(onColor, c.th.focusText, f * 0.3f);
    g.Icon(glyph, center, d * 0.34f, fg);
    g.PopTransform();
    return c.Click(r);
}

void Progress(Ctx& c, const Rect& r, float frac, Color fill, Color track) {
    frac = std::clamp(frac, 0.f, 1.f);
    c.g.FillRound(r, r.h * 0.5f, track);
    if (frac > 0) c.g.FillRound({r.x, r.y, std::max(r.h, r.w * frac), r.h}, r.h * 0.5f, fill);
}

void Ring(Ctx& c, Vec2 center, float radius, float width, float frac, Color fill, Color track) {
    c.g.StrokeCircle(center, radius, track, width);
    c.g.Arc(center, radius, 0.f, 360.f * std::clamp(frac, 0.f, 1.f), fill, width);
}

bool Toggle(Ctx& c, uint64_t id, const Rect& r, bool on, bool focused) {
    Gfx& g = c.g;
    bool hover = c.Hover(r);
    if (hover) c.wantHand = true;
    float k = c.Anim(id ^ 0x70661E, on ? 1.f : 0.f, 18.f);
    float f = c.Anim(id ^ 0xF0C5, focused ? 1.f : 0.f, 16.f);
    Rect track{r.r() - 52, r.cy() - 16, 52, 32};
    g.FillRound(track, 16, Color::Lerp(c.th.dark ? Color::White(0.16f) : Color::Black(0.12f), c.th.accent, k));
    float kx = track.x + 16 + k * 20;
    g.SoftShadow({kx - 13, track.cy() - 13, 26, 26}, 13, 4, Color::Black(0.18f), 1.5f);
    g.FillCircle({kx, track.cy()}, 13, Color::White());
    FocusRing(c, track, 16, f, 4);
    return c.Click(r);
}

int Segmented(Ctx& c, uint64_t id, const Rect& r, const std::vector<std::wstring>& items, int sel, bool focused) {
    Gfx& g = c.g;
    int n = static_cast<int>(items.size());
    if (n == 0) return -1;
    float f = c.Anim(id ^ 0xF0C5, focused ? 1.f : 0.f, 16.f);
    g.FillRound(r, r.h * 0.5f, c.th.control);
    float w = (r.w - 6) / n;
    float pos = c.Anim(id ^ 0x5E1, static_cast<float>(sel), 16.f);
    Rect knob{r.x + 3 + pos * w, r.y + 3, w, r.h - 6};
    g.SoftShadow(knob, knob.h * 0.5f, 6, Color::Black(c.th.dark ? 0.25f : 0.08f), 2);
    g.FillRound(knob, knob.h * 0.5f, c.th.dark ? Color::White(0.24f) : Color::White());
    int clicked = -1;
    for (int i = 0; i < n; ++i) {
        Rect cell{r.x + 3 + i * w, r.y, w, r.h};
        bool hov = c.Hover(cell);
        if (hov) c.wantHand = true;
        TextStyle ts = Body(15, i == sel ? c.th.text : c.th.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        ts.align = Align::Center;
        ts.valign = VAlign::Center;
        g.Text(items[i], cell, ts);
        if (c.Click(cell)) clicked = i;
    }
    FocusRing(c, r, r.h * 0.5f, f, 4);
    return clicked;
}

bool TextField(Ctx& c, uint64_t id, const Rect& r, const std::wstring& text, const wchar_t* placeholder,
               bool editing, wchar_t glyph, bool masked) {
    Gfx& g = c.g;
    bool hover = c.Hover(r);
    float e = c.Anim(id ^ 0xED17, editing ? 1.f : 0.f, 16.f);
    float h = c.Anim(id ^ 0x40E7, hover ? 1.f : 0.f, 20.f);
    Color fill = Color::Lerp(Color::Lerp(c.th.control, c.th.controlHover, h), c.th.dark ? Color::White(0.16f) : Color::White(), e);
    g.FillRound(r, r.h * 0.5f, fill);
    g.StrokeRound(r, r.h * 0.5f, Color::Lerp(c.th.glassStroke, c.th.accent, e), 1.f + e);
    float x = r.x + 18;
    if (glyph) {
        g.Icon(glyph, {x + 9, r.cy()}, 16, c.th.text2);
        x += 30;
    }
    std::wstring shown = masked ? std::wstring(text.size(), L'•') : text;
    TextStyle ts = Body(17, c.th.text);
    ts.valign = VAlign::Center;
    Rect tr{x, r.y, r.r() - x - 16, r.h};
    if (shown.empty()) {
        ts.color = c.th.text3;
        g.Text(placeholder, tr, ts);
    } else {
        g.PushClip(tr);
        float tw = g.Measure(shown, ts).x;
        float shift = std::max(0.f, tw - tr.w + 4);
        g.Text(shown, {tr.x - shift, tr.y, tw + 8, tr.h}, ts);
        g.PopClip();
    }
    if (editing && std::fmod(c.t, 1.0) < 0.6) {
        float tw = shown.empty() ? 0 : std::min(g.Measure(shown, ts).x, tr.w - 4);
        g.FillRect({tr.x + tw + 1, r.cy() - 11, 2, 22}, c.th.accent);
    }
    if (hover) c.wantHand = true;
    return c.Click(r);
}

float ChipWidth(Ctx& c, const std::wstring& label, wchar_t glyph) {
    return c.g.Measure(label, Body(15, c.th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD)).x + 46 + (glyph ? 24 : 0);
}

bool Chip(Ctx& c, uint64_t id, const Rect& r, const std::wstring& label, bool selected, bool focused, wchar_t glyph) {
    Gfx& g = c.g;
    bool hover = c.Hover(r);
    if (hover) c.wantHand = true;
    float s = c.Anim(id ^ 0x5E1, selected ? 1.f : 0.f, 18.f);
    float f = c.Anim(id ^ 0xF0C5, focused ? 1.f : 0.f, 16.f);
    float h = c.Anim(id ^ 0x40E7, hover ? 1.f : 0.f, 20.f);
    Color fill = Color::Lerp(Color::Lerp(c.th.control, c.th.controlHover, h), c.th.focusFill, s);
    g.FillRound(r, r.h * 0.5f, fill);
    Color fg = Color::Lerp(c.th.text, c.th.focusText, s);
    float x = r.x + 18;
    if (glyph) {
        g.Icon(glyph, {x + 8, r.cy()}, 14, fg);
        x += 24;
    }
    TextStyle ts = Body(15, fg, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ts.valign = VAlign::Center;
    g.Text(label, {x, r.y, r.r() - x - 12, r.h}, ts);
    FocusRing(c, r, r.h * 0.5f, f, 4);
    return c.Click(r);
}

// --- Artwork ---------------------------------------------------------------

Color GameColor(Ctx& c, const Game& g) {
    Color a = c.img.Average(g.hero);
    if (a.a <= 0) a = c.img.Average(g.cover);
    if (a.a <= 0) a = c.img.Average(g.header);
    if (a.a > 0) return a;
    float hue = (Hash64(Narrow(g.name)) % 1000) / 1000.f;
    return Color::Hsv(hue, 0.55f, 0.45f);
}

void Placeholder(Ctx& c, const Game& g, const Rect& r, float radius, bool portrait) {
    Gfx& gx = c.g;
    float hue = (Hash64(Narrow(g.name)) % 1000) / 1000.f;
    Color top = Color::Hsv(hue, 0.50f, 0.46f), bottom = Color::Hsv(hue + 0.07f, 0.62f, 0.14f);
    gx.Gradient(r, radius, {r.x, r.y}, {r.r(), r.b()}, {{0.f, top}, {1.f, bottom}});
    gx.Gradient(r, radius, {r.x, r.y}, {r.x, r.b()}, {{0.f, Color::White(0.10f)}, {0.5f, Color::White(0)}});

    float iconSize = std::min(r.w, r.h) * (portrait ? 0.34f : 0.28f);
    bool hasIcon = false;
    if (!g.icon.empty()) {
        if (ID2D1Bitmap1* ic = c.img.Get(g.icon)) {
            Rect ir{r.cx() - iconSize * 0.5f, r.y + r.h * (portrait ? 0.24f : 0.18f), iconSize, iconSize};
            gx.SoftShadow(ir.Inset(iconSize * 0.15f), iconSize * 0.2f, iconSize * 0.2f, Color::Black(0.25f), 4);
            gx.Image(ic, ir, 0, 1, Fit::Contain);
            hasIcon = true;
        }
    }
    TextStyle ts;
    ts.font = FontRole::Display;
    ts.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    ts.color = Color::White(0.95f);
    ts.shadow = true;
    if (portrait) {
        ts.size = std::max(14.f, r.w * 0.12f);
        ts.maxLines = 4;
        ts.valign = VAlign::Bottom;
        ts.lineHeight = 1.12f;
        gx.Text(g.name, r.Inset(r.w * 0.09f), ts);
        TextStyle p = Caps(std::max(9.f, r.w * 0.045f), Color::White(0.6f));
        gx.Text(ToUpper(PlatformName(g.platform)), {r.x + r.w * 0.09f, r.y + r.w * 0.09f, r.w, 20}, p);
    } else {
        ts.size = std::max(16.f, r.h * 0.13f);
        ts.maxLines = 2;
        ts.align = Align::Center;
        ts.valign = hasIcon ? VAlign::Bottom : VAlign::Center;
        gx.Text(g.name, r.Inset(r.w * 0.08f, r.h * 0.12f), ts);
    }
}

ID2D1Bitmap1* HeroBitmap(Ctx& c, const Game& g, const std::wstring** srcOut) {
    for (const std::wstring* src : {&g.hero, &g.header}) {
        if (src->empty() || c.img.Failed(*src)) continue;
        ID2D1Bitmap1* b = c.img.Get(*src);
        if (srcOut) *srcOut = src;
        return b;  // may be null while loading: don't skip to a lower-quality fallback yet
    }
    if (srcOut) *srcOut = nullptr;
    return nullptr;
}

static bool HeroUnavailable(Ctx& c, const Game& g) {
    return (g.hero.empty() || c.img.Failed(g.hero)) && (g.header.empty() || c.img.Failed(g.header));
}

void GameArt(Ctx& c, const Game& g, const Rect& r, float radius, ArtStyle style, float opacity) {
    Gfx& gx = c.g;
    gx.PushAlpha(opacity);
    if (style == ArtStyle::Portrait) {
        bool missing = g.cover.empty() || c.img.Failed(g.cover);
        ID2D1Bitmap1* b = missing ? nullptr : c.img.Get(g.cover, 1200);
        if (b) {
            float fade = c.AnimFrom(Id(g.id, 0xC0), 1.f, 0.f, 10.f);
            if (fade < 1.f) gx.FillRound(r, radius, c.th.control);
            gx.Image(b, r, radius, fade, Fit::Cover);
        } else if (missing) {
            Placeholder(c, g, r, radius, true);
        } else {
            gx.FillRound(r, radius, c.th.control);
        }
    } else {
        const std::wstring* src = nullptr;
        ID2D1Bitmap1* b = HeroBitmap(c, g, &src);
        if (b) {
            float fade = c.AnimFrom(Id(g.id, 0xC1), 1.f, 0.f, 10.f);
            if (fade < 1.f) gx.FillRound(r, radius, c.th.control);
            gx.Image(b, r, radius, fade, Fit::Cover, {0.5f, 0.35f});
            if (src == &g.hero) {
                // Hero art has no title: overlay the logo (or the name).
                gx.Gradient(r, radius, {r.x, r.y + r.h * 0.4f}, {r.x, r.b()},
                            {{0.f, Color::Black(0)}, {1.f, Color::Black(0.45f)}});
                Rect box{r.x + r.w * 0.16f, r.y + r.h * 0.2f, r.w * 0.68f, r.h * 0.6f};
                ID2D1Bitmap1* logo = (g.logo.empty() || c.img.Failed(g.logo)) ? nullptr : c.img.Get(g.logo, 1600, true);
                if (logo) {
                    gx.Image(logo, box, 0, fade, Fit::Contain, {0.5f, 0.5f});
                } else if (g.logo.empty() || c.img.Failed(g.logo)) {
                    TextStyle ts;
                    ts.font = FontRole::Display;
                    ts.size = std::max(18.f, r.h * 0.13f);
                    ts.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
                    ts.align = Align::Center;
                    ts.valign = VAlign::Center;
                    ts.maxLines = 2;
                    ts.shadow = true;
                    gx.Text(g.name, box, ts);
                }
            }
        } else if (HeroUnavailable(c, g)) {
            Placeholder(c, g, r, radius, false);
        } else {
            gx.FillRound(r, radius, c.th.control);
        }
    }
    gx.PopAlpha();
}

void LogoOrTitle(Ctx& c, const Game& g, const Rect& box, Align align, float titleSize) {
    Gfx& gx = c.g;
    bool noLogo = g.logo.empty() || c.img.Failed(g.logo);
    if (!noLogo) {
        ID2D1Bitmap1* logo = c.img.Get(g.logo, 1600, true);
        if (!logo) return;  // loading: avoid a flash of text
        float fade = c.AnimFrom(Id(g.id, 0x10C0), 1.f, 0.f, 10.f);
        float ax = align == Align::Center ? 0.5f : align == Align::Trailing ? 1.f : 0.f;
        gx.Image(logo, box, 0, fade, Fit::Contain, {ax, 1.f});
        return;
    }
    TextStyle ts;
    ts.font = FontRole::Display;
    ts.size = titleSize;
    ts.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    ts.color = c.th.text;
    ts.align = align;
    ts.valign = VAlign::Bottom;
    ts.maxLines = 2;
    ts.lineHeight = 1.05f;
    ts.tracking = titleSize * 0.01f;
    ts.shadow = c.th.dark;
    gx.Text(g.name, box, ts);
}

}  // namespace lumen::ui
