#pragma once
// Immediate-mode UI toolkit on top of Gfx: animation state, hit testing, theme tokens and
// the small set of refined widgets every screen is built from.
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Game.h"
#include "core/Settings.h"
#include "Gfx.h"
#include "ImageCache.h"
#include "Input.h"

namespace lumen::ui {

struct Theme {
    bool dark = true;
    Color bg, bg2, text, text2, text3, separator;
    Color glass, glassStroke, control, controlHover;
    Color focusFill, focusText;  // focused controls invert (tvOS style)
    Color accent, accentText;
    Color scrim;                 // colour of the gradients laid over artwork
};

constexpr int kAccentCount = 6;
constexpr int kDynamicAccent = 6;  // accent follows the selected game's artwork
Color VividFrom(Color c);          // a saturated, readable accent derived from an artwork colour
extern const Color kAccents[kAccentCount];
extern const wchar_t* kAccentNames[kAccentCount];
Theme MakeTheme(bool dark, int accent);

uint64_t Id(const char* s, uint64_t salt = 0);
uint64_t Id(const std::string& s, uint64_t salt = 0);

struct Backdrop {
    ID2D1Bitmap1* bmp = nullptr;  // blurred artwork used behind glass panels
    Rect rect;                    // where the artwork is drawn on screen
    float opacity = 1.f;
};

class Ctx {
public:
    Ctx(Gfx& g, ImageCache& img, InputFrame& in, const Theme& th, const Settings& s)
        : g(g), img(img), in(in), th(th), settings(s) {}

    Gfx& g;
    ImageCache& img;
    InputFrame& in;
    const Theme& th;
    const Settings& settings;
    float dt = 0;
    double t = 0;
    bool blockMouse = false;  // a modal sheet is open
    // Vertical scroll applied (via a translation) to the content being drawn. Hit testing works in
    // content coordinates, so the mouse must be shifted by the same amount.
    float scrollY = 0;
    Vec2 Mouse() const { return {in.mouse.x, in.mouse.y + scrollY}; }
    Vec2 PressPos() const { return {in.pressPos.x, in.pressPos.y + scrollY}; }
    bool wantHand = false;    // set by hovered clickable widgets
    Backdrop backdrop;

    float Anim(uint64_t id, float target, float speed = 14.f);
    float AnimFrom(uint64_t id, float target, float initial, float speed = 14.f);
    void SetAnim(uint64_t id, float value);
    bool Hover(const Rect& r) const;
    bool Click(const Rect& r) const;
    bool Pressing(const Rect& r) const;
    float Motion() const { return settings.reduceMotion ? 0.f : 1.f; }
    void Collect();  // forget animations that were not touched recently

private:
    struct AnimState {
        float v;
        double touched;
    };
    std::unordered_map<uint64_t, AnimState> anims_;
};

// --- Widgets ---------------------------------------------------------------
enum class BtnKind { Primary, Glass, Plain };

struct BtnSpec {
    std::wstring label;
    wchar_t glyph = 0;
    BtnKind kind = BtnKind::Glass;
    bool focused = false;
    bool on = false;  // toggled state (e.g. favorite)
    float fontSize = 17;
    std::wstring sublabel;
};

void Glass(Ctx& c, const Rect& r, float radius, float strength = 1.f);
bool Button(Ctx& c, uint64_t id, const Rect& r, const BtnSpec& spec);
bool IconButton(Ctx& c, uint64_t id, Vec2 center, float d, wchar_t glyph, bool focused, bool on = false,
                Color onColor = {});
void FocusRing(Ctx& c, const Rect& r, float radius, float t, float gap = 5.f);
void Progress(Ctx& c, const Rect& r, float frac, Color fill, Color track);
void Ring(Ctx& c, Vec2 center, float radius, float width, float frac, Color fill, Color track);
bool Toggle(Ctx& c, uint64_t id, const Rect& r, bool on, bool focused);
int Segmented(Ctx& c, uint64_t id, const Rect& r, const std::vector<std::wstring>& items, int sel, bool focused);
bool TextField(Ctx& c, uint64_t id, const Rect& r, const std::wstring& text, const wchar_t* placeholder,
               bool editing, wchar_t glyph = 0, bool masked = false);
float ChipWidth(Ctx& c, const std::wstring& label, wchar_t glyph = 0);
bool Chip(Ctx& c, uint64_t id, const Rect& r, const std::wstring& label, bool selected, bool focused,
          wchar_t glyph = 0);
TextStyle Caps(float size, Color color, Align align = Align::Leading);
TextStyle Body(float size, Color color, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL);

// --- Game artwork ----------------------------------------------------------
enum class ArtStyle { Portrait, Landscape };
void GameArt(Ctx& c, const Game& g, const Rect& r, float radius, ArtStyle style, float opacity = 1.f);
void Placeholder(Ctx& c, const Game& g, const Rect& r, float radius, bool portrait);
ID2D1Bitmap1* HeroBitmap(Ctx& c, const Game& g, const std::wstring** srcOut = nullptr);
Color GameColor(Ctx& c, const Game& g);
// Logo art if available, otherwise the name set in large type. Anchored to the bottom of `box`.
void LogoOrTitle(Ctx& c, const Game& g, const Rect& box, Align align, float titleSize);

}  // namespace lumen::ui
