#pragma once
// GPU renderer: Direct3D 11 swap chain + Direct2D 1.1 device context + DirectWrite.
// Everything is drawn in "logical" units: the window is mapped to a 1080-unit-high canvas
// (or 1280 units wide on narrow windows), so layouts scale with any resolution.
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite_1.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lumen::ui {

using Microsoft::WRL::ComPtr;

struct Vec2 {
    float x = 0, y = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float r() const { return x + w; }
    float b() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    bool Contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    Rect Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect Inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect Offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    Rect ScaledAboutCenter(float s) const { return {cx() - w * s * 0.5f, cy() - h * s * 0.5f, w * s, h * s}; }
    bool Intersects(const Rect& o) const { return x < o.r() && o.x < r() && y < o.b() && o.y < b(); }
};

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    static Color Hex(uint32_t rgb, float a = 1.f) {
        return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
    }
    static Color White(float a = 1.f) { return {1, 1, 1, a}; }
    static Color Black(float a = 1.f) { return {0, 0, 0, a}; }
    Color A(float na) const { return {r, g, b, na}; }
    Color Fade(float k) const { return {r, g, b, a * k}; }
    static Color Lerp(const Color& x, const Color& y, float t) {
        return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
    }
    static Color Hsv(float h, float s, float v, float a = 1.f);
};

enum class FontRole { Display, Text, Icon };
enum class Align { Leading, Center, Trailing };
enum class VAlign { Top, Center, Bottom };
enum class Fit { Cover, Contain, Fill };

struct TextStyle {
    float size = 16;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    Color color = Color::White();
    Align align = Align::Leading;
    VAlign valign = VAlign::Top;
    float tracking = 0;   // extra spacing per character, logical units
    int maxLines = 1;     // 0 = unlimited (wrap)
    FontRole font = FontRole::Text;
    float lineHeight = 0; // multiplier of size; 0 = font default
    bool shadow = false;
};

// Common Segoe Fluent / MDL2 glyphs. Trophy is not in the icon font: it is a private code point
// that Gfx::Icon draws as vector art.
namespace glyph {
constexpr wchar_t Play = 0xE768, Settings = 0xE713, Info = 0xE946, Heart = 0xEB51, HeartFill = 0xEB52,
                  Folder = 0xE8B7, Search = 0xE721, Add = 0xE710, Back = 0xE72B, Forward = 0xE72A, More = 0xE712,
                  Grid = 0xF0E2, List = 0xE8FD, Sort = 0xE8CB, Lock = 0xE72E, Trophy = 0xF8F0, Clock = 0xE823,
                  Calendar = 0xE787, Wifi = 0xE701, WifiOff = 0xEB5E, Ethernet = 0xE839, Battery = 0xE83F,
                  Charging = 0xE83E, Tag = 0xE8EC, Hide = 0xED1A, View = 0xE890, Delete = 0xE74D, Image = 0xEB9F,
                  Refresh = 0xE72C, Game = 0xE7FC, Library = 0xE8F1, Home = 0xE80F, Chart = 0xE9D2,
                  Checkmark = 0xE73E, Cancel = 0xE711, Shuffle = 0xE8B1, Link = 0xE71B, Key = 0xE8D7,
                  ChevronRight = 0xE76C, ChevronDown = 0xE70D, Edit = 0xE70F, Store = 0xE719, Storage = 0xEDA2,
                  Star = 0xE734, StarFill = 0xE735, Running = 0xE805, Keyboard = 0xE765, Light = 0xE706,
                  Contrast = 0xE7A1, Download = 0xE896, FullScreen = 0xE740, BackToWindow = 0xE73F,
                  Bell = 0xEA8F;
}

class Gfx {
public:
    bool Init(HWND hwnd);
    void Shutdown();
    void Resize(UINT w, UINT h);
    bool BeginFrame();  // false: nothing can be drawn this frame
    void EndFrame();
    void RequestSnapshot(const std::wstring& pngPath) { snapshotPath_ = pngPath; }
    bool SnapshotDone() const { return snapshotDone_; }
    uint32_t generation() const { return generation_; }

    float scale = 1.f;  // device pixels per logical unit
    float W = 1920, H = 1080;
    UINT pxW = 0, pxH = 0;

    // State stacks
    void PushAlpha(float a);
    void PopAlpha();
    float alpha() const { return alpha_.back(); }
    void PushTransform(const D2D1::Matrix3x2F& local);
    void PopTransform();
    void PushClip(const Rect& r, float radius = 0);
    void PopClip();

    // Primitives (logical units)
    void FillRect(const Rect& r, Color c);
    void FillRound(const Rect& r, float radius, Color c);
    void StrokeRound(const Rect& r, float radius, Color c, float width);
    void FillCircle(Vec2 c, float radius, Color col);
    void StrokeCircle(Vec2 c, float radius, Color col, float width);
    void Line(Vec2 a, Vec2 b, Color c, float width);
    void Arc(Vec2 c, float radius, float startDeg, float sweepDeg, Color col, float width);
    void Gradient(const Rect& r, float radius, Vec2 p0, Vec2 p1,
                  std::initializer_list<std::pair<float, Color>> stops);
    void SoftShadow(const Rect& r, float radius, float spread, Color c, float dy = 0);

    // Images
    void Image(ID2D1Bitmap1* bmp, const Rect& dst, float radius = 0, float opacity = 1, Fit fit = Fit::Cover,
               Vec2 anchor = {0.5f, 0.5f}, float zoom = 1.f);
    Rect FitRect(ID2D1Bitmap1* bmp, const Rect& dst, Fit fit, Vec2 anchor = {0.5f, 0.5f}, float zoom = 1.f) const;
    ID2D1Bitmap1* Blurred(const std::wstring& key, ID2D1Bitmap1* src);  // ready next frame
    ID2D1Bitmap1* CreateBitmap(UINT w, UINT h, const void* premulBgra, UINT pitch);
    ID2D1Bitmap1* CreateOpaqueBitmap(UINT w, UINT h);  // alpha ignored (video frames)
    // Soft radial light: `c` at the centre fading to transparent at `radius`.
    void Glow(Vec2 center, float radius, Color c);

    // Text
    void Text(std::wstring_view s, const Rect& r, const TextStyle& st);
    Vec2 Measure(std::wstring_view s, const TextStyle& st, float maxWidth = 100000.f);
    void Icon(wchar_t g, Vec2 center, float size, Color c);

    ID2D1DeviceContext* dc() const { return dc_.Get(); }
    IWICImagingFactory* wic() const { return wic_.Get(); }
    double now = 0;  // seconds, set by the app each frame

private:
    bool CreateDevice();
    void ReleaseDevice();
    bool CreateTarget();
    void ProcessBlurQueue();
    IDWriteTextFormat* Format(FontRole role, float size, DWRITE_FONT_WEIGHT weight);
    ComPtr<IDWriteTextLayout> Layout(std::wstring_view s, const TextStyle& st, float w, float h);
    ComPtr<IDWriteTextLayout> CreateLayout(std::wstring_view s, const TextStyle& st, float w, float h);
    ID2D1SolidColorBrush* Brush(Color c);
    void ApplyTransform();
    void UpdateScale();

    HWND hwnd_ = nullptr;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<IDWriteFactory1> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID2D1Device> device_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1Bitmap1> snapshotTarget_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1Effect> blur_;
    ComPtr<ID2D1StrokeStyle> roundCap_;
    uint32_t generation_ = 0;
    bool drawing_ = false;
    UINT pendingW_ = 0, pendingH_ = 0;  // resize requested mid-frame

    std::vector<float> alpha_ = {1.f};
    std::vector<D2D1::Matrix3x2F> transforms_;
    std::vector<int> clipKinds_;  // 0 axis aligned, 1 layer

    std::wstring fontDisplay_ = L"Segoe UI", fontText_ = L"Segoe UI", fontIcon_ = L"Segoe MDL2 Assets";
    std::unordered_map<uint64_t, ComPtr<IDWriteTextFormat>> formats_;
    std::unordered_map<IDWriteTextFormat*, ComPtr<IDWriteInlineObject>> ellipsis_;
    struct LayoutEntry {
        ComPtr<IDWriteTextLayout> layout;
        double lastUse = 0;
    };
    std::unordered_map<uint64_t, LayoutEntry> layouts_;
    struct GradEntry {
        ComPtr<ID2D1LinearGradientBrush> brush;
        double lastUse = 0;
    };
    std::unordered_map<uint64_t, GradEntry> grads_;  // device-dependent
    ComPtr<ID2D1BitmapBrush1> bitmapBrush_;            // device-dependent
    std::unordered_map<uint64_t, ComPtr<ID2D1RadialGradientBrush>> glows_;  // device-dependent
    double lastSweep_ = 0;

    struct BlurEntry {
        ComPtr<ID2D1Bitmap1> src, out;
        double lastUse = 0;
    };
    std::unordered_map<std::wstring, BlurEntry> blurs_;
    std::vector<std::wstring> blurQueue_;

    std::wstring snapshotPath_;
    bool snapshotDone_ = false;
};

}  // namespace lumen::ui
