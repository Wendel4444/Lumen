#include "Gfx.h"

#include <algorithm>
#include <cmath>

namespace lumen::ui {

using D2D1::Matrix3x2F;

Color Color::Hsv(float h, float s, float v, float a) {
    h = std::fmod(h, 1.f) * 6.f;
    int i = static_cast<int>(h);
    float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
        case 0: return {v, t, p, a};
        case 1: return {q, v, p, a};
        case 2: return {p, v, t, a};
        case 3: return {p, q, v, a};
        case 4: return {t, p, v, a};
        default: return {v, p, q, a};
    }
}

static D2D1_COLOR_F ToD2D(Color c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
static D2D1_RECT_F ToD2D(const Rect& r) { return D2D1::RectF(r.x, r.y, r.r(), r.b()); }

bool Gfx::Init(HWND hwnd) {
    hwnd_ = hwnd;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory1),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
        return false;

    // Prefer Windows 11 variable fonts, fall back gracefully on Windows 10.
    ComPtr<IDWriteFontCollection> sys;
    dwrite_->GetSystemFontCollection(&sys);
    auto has = [&](const wchar_t* name) {
        UINT32 idx;
        BOOL exists = FALSE;
        return sys && SUCCEEDED(sys->FindFamilyName(name, &idx, &exists)) && exists;
    };
    if (has(L"Segoe UI Variable Display")) fontDisplay_ = L"Segoe UI Variable Display";
    if (has(L"Segoe UI Variable Text")) fontText_ = L"Segoe UI Variable Text";
    if (has(L"Segoe Fluent Icons")) fontIcon_ = L"Segoe Fluent Icons";

    RECT rc;
    GetClientRect(hwnd, &rc);
    pxW = std::max<UINT>(1, rc.right - rc.left);
    pxH = std::max<UINT>(1, rc.bottom - rc.top);
    UpdateScale();
    return CreateDevice();
}

void Gfx::UpdateScale() {
    // 1080 logical units tall; narrow windows are 1280 wide instead so layouts never get cramped.
    scale = pxH / 1080.f;
    if (pxW / scale < 1280.f) scale = pxW / 1280.f;
    W = pxW / scale;
    H = pxH / scale;
}

bool Gfx::CreateDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    d3d_.As(&dxgiDevice);
    if (FAILED(factory_->CreateDevice(dxgiDevice.Get(), &device_))) return false;
    if (FAILED(device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;

    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> dxgiFactory;
    adapter->GetParent(IID_PPV_ARGS(&dxgiFactory));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = pxW;
    sd.Height = pxH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = dxgiFactory->CreateSwapChainForHwnd(d3d_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;  // Windows 8.x
        hr = dxgiFactory->CreateSwapChainForHwnd(d3d_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_);
    }
    if (FAILED(hr)) return false;
    dxgiFactory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    dc_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
    dc_->CreateEffect(CLSID_D2D1GaussianBlur, &blur_);
    D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                   D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(ssp, nullptr, 0, &roundCap_);
    ++generation_;
    blurs_.clear();
    blurQueue_.clear();
    return CreateTarget();
}

void Gfx::ReleaseDevice() {
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    snapshotTarget_.Reset();
    blurs_.clear();
    blurQueue_.clear();
    grads_.clear();
    glows_.clear();
    bitmapBrush_.Reset();
    blur_.Reset();
    brush_.Reset();
    roundCap_.Reset();
    swap_.Reset();
    dc_.Reset();
    device_.Reset();
    d3d_.Reset();
}

void Gfx::Shutdown() { ReleaseDevice(); }

bool Gfx::CreateTarget() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
    dc_->SetTarget(target_.Get());
    return true;
}

void Gfx::Resize(UINT w, UINT h) {
    w = std::max<UINT>(1, w);
    h = std::max<UINT>(1, h);
    if (drawing_) {
        // WM_SIZE can arrive synchronously in the middle of a frame (e.g. SetWindowPos from a click handler).
        // Resizing the swap chain while Direct2D still holds the back buffer fails, so apply it next frame.
        pendingW_ = w;
        pendingH_ = h;
        return;
    }
    if (w == pxW && h == pxH) return;
    pxW = w;
    pxH = h;
    UpdateScale();
    if (!swap_) return;
    dc_->SetTarget(nullptr);
    target_.Reset();
    if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        ReleaseDevice();
        CreateDevice();
        return;
    }
    CreateTarget();
}

void Gfx::ProcessBlurQueue() {
    for (const auto& key : blurQueue_) {
        auto it = blurs_.find(key);
        if (it == blurs_.end() || !it->second.src) continue;
        ID2D1Bitmap1* src = it->second.src.Get();
        D2D1_SIZE_F s = src->GetSize();
        if (s.width < 1 || s.height < 1) continue;
        UINT w = 480, h = std::max<UINT>(1, static_cast<UINT>(480.f * s.height / s.width));
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> reduced, out;
        if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &reduced)) ||
            FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &out)))
            continue;
        dc_->SetTarget(reduced.Get());
        dc_->BeginDraw();
        dc_->SetTransform(Matrix3x2F::Identity());
        dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc_->DrawBitmap(src, D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), 1.f,
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        dc_->EndDraw();
        blur_->SetInput(0, reduced.Get());
        blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 9.f);
        blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);
        dc_->SetTarget(out.Get());
        dc_->BeginDraw();
        dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc_->DrawImage(blur_.Get());
        dc_->EndDraw();
        blur_->SetInput(0, nullptr);
        it->second.out = out;
        it->second.src.Reset();
    }
    blurQueue_.clear();
    // Evict blurred backdrops nobody asked for recently.
    for (auto it = blurs_.begin(); it != blurs_.end();)
        it = (now - it->second.lastUse > 20.0) ? blurs_.erase(it) : std::next(it);
}

bool Gfx::BeginFrame() {
    if (pendingW_) {
        UINT w = pendingW_, h = pendingH_;
        pendingW_ = pendingH_ = 0;
        Resize(w, h);
    }
    if (!dc_ && !CreateDevice()) return false;
    if (!target_) return false;
    ProcessBlurQueue();

    snapshotDone_ = false;
    if (!snapshotPath_.empty()) {
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        snapshotTarget_.Reset();
        dc_->CreateBitmap(D2D1::SizeU(pxW, pxH), nullptr, 0, props, &snapshotTarget_);
    }
    dc_->SetTarget(snapshotTarget_ ? snapshotTarget_.Get() : target_.Get());
    dc_->BeginDraw();
    drawing_ = true;
    alpha_.assign(1, 1.f);
    transforms_.assign(1, Matrix3x2F::Scale(scale, scale));
    clipKinds_.clear();
    ApplyTransform();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 1));
    return true;
}

static bool SavePng(IWICImagingFactory* wic, ID2D1DeviceContext* dc, ID2D1Bitmap1* src, UINT w, UINT h,
                    const std::wstring& path) {
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &cpu))) return false;
    if (FAILED(cpu->CopyFromBitmap(nullptr, src, nullptr))) return false;
    D2D1_MAPPED_RECT mapped;
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    bool ok = false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetSize(w, h);
        frame->SetPixelFormat(&fmt);
        ok = IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
             SUCCEEDED(frame->WritePixels(h, mapped.pitch, mapped.pitch * h, mapped.bits)) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    cpu->Unmap();
    return ok;
}

void Gfx::EndFrame() {
    if (!drawing_) return;
    while (!clipKinds_.empty()) PopClip();
    drawing_ = false;
    HRESULT hr = dc_->EndDraw();
    if (snapshotTarget_) {
        snapshotDone_ = SUCCEEDED(hr) && SavePng(wic_.Get(), dc_.Get(), snapshotTarget_.Get(), pxW, pxH, snapshotPath_);
        snapshotTarget_.Reset();
        snapshotPath_.clear();
        dc_->SetTarget(target_.Get());
        return;
    }
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseDevice();
        CreateDevice();
        return;
    }
    hr = swap_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        ReleaseDevice();
        CreateDevice();
    }
}

// --- State -----------------------------------------------------------------

void Gfx::PushAlpha(float a) { alpha_.push_back(alpha_.back() * std::clamp(a, 0.f, 1.f)); }
void Gfx::PopAlpha() {
    if (alpha_.size() > 1) alpha_.pop_back();
}

void Gfx::ApplyTransform() { dc_->SetTransform(transforms_.back()); }

void Gfx::PushTransform(const Matrix3x2F& local) {
    transforms_.push_back(local * transforms_.back());
    ApplyTransform();
}

void Gfx::PopTransform() {
    if (transforms_.size() > 1) transforms_.pop_back();
    ApplyTransform();
}

void Gfx::PushClip(const Rect& r, float radius) {
    if (radius <= 0) {
        dc_->PushAxisAlignedClip(ToD2D(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        clipKinds_.push_back(0);
        return;
    }
    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ToD2D(r), radius, radius), &geo);
    dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                   nullptr);
    clipKinds_.push_back(1);
}

void Gfx::PopClip() {
    if (clipKinds_.empty()) return;
    if (clipKinds_.back() == 0) dc_->PopAxisAlignedClip();
    else dc_->PopLayer();
    clipKinds_.pop_back();
}

ID2D1SolidColorBrush* Gfx::Brush(Color c) {
    c.a *= alpha_.back();
    brush_->SetColor(ToD2D(c));
    return brush_.Get();
}

// --- Primitives ------------------------------------------------------------

void Gfx::FillRect(const Rect& r, Color c) { dc_->FillRectangle(ToD2D(r), Brush(c)); }

void Gfx::FillRound(const Rect& r, float radius, Color c) {
    if (radius <= 0) return FillRect(r, c);
    dc_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(r), radius, radius), Brush(c));
}

void Gfx::StrokeRound(const Rect& r, float radius, Color c, float width) {
    Rect in = r.Inset(width * 0.5f);
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(in), std::max(0.f, radius - width * 0.5f),
                                                std::max(0.f, radius - width * 0.5f)),
                              Brush(c), width);
}

void Gfx::FillCircle(Vec2 c, float radius, Color col) {
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y), radius, radius), Brush(col));
}

void Gfx::StrokeCircle(Vec2 c, float radius, Color col, float width) {
    dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y), radius, radius), Brush(col), width);
}

void Gfx::Line(Vec2 a, Vec2 b, Color c, float width) {
    dc_->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), Brush(c), width, roundCap_.Get());
}

void Gfx::Arc(Vec2 c, float radius, float startDeg, float sweepDeg, Color col, float width) {
    if (sweepDeg <= 0.01f) return;
    if (sweepDeg >= 359.9f) return StrokeCircle(c, radius, col, width);
    auto pt = [&](float deg) {
        float rad = (deg - 90.f) * 3.14159265f / 180.f;
        return D2D1::Point2F(c.x + radius * std::cos(rad), c.y + radius * std::sin(rad));
    };
    ComPtr<ID2D1PathGeometry> path;
    factory_->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    sink->BeginFigure(pt(startDeg), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(pt(startDeg + sweepDeg), D2D1::SizeF(radius, radius), 0.f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  sweepDeg > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    dc_->DrawGeometry(path.Get(), Brush(col), width, roundCap_.Get());
}

namespace {
// FNV-1a over arbitrary bytes, for cache keys.
struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    template <typename T>
    void add(const T& v) { bytes(&v, sizeof(v)); }
};
}  // namespace

void Gfx::Gradient(const Rect& r, float radius, Vec2 p0, Vec2 p1,
                   std::initializer_list<std::pair<float, Color>> stops) {
    // Stop collections are immutable and costly to create: cache one brush per stop set.
    std::vector<D2D1_GRADIENT_STOP> s;
    Hasher hs;
    for (auto& [pos, c] : stops) {
        Color k = c.Fade(alpha_.back());
        // Quantize so animated alphas don't create a new brush every frame.
        k.a = std::round(k.a * 255.f) / 255.f;
        s.push_back({pos, ToD2D(k)});
        hs.add(pos);
        hs.add(k);
    }
    auto it = grads_.find(hs.h);
    if (it == grads_.end()) {
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> lg;
        if (FAILED(dc_->CreateGradientStopCollection(s.data(), static_cast<UINT32>(s.size()), &coll)) ||
            FAILED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {1, 1}), coll.Get(), &lg)))
            return;
        it = grads_.emplace(hs.h, GradEntry{lg, now}).first;
    }
    it->second.lastUse = now;
    ID2D1LinearGradientBrush* lg = it->second.brush.Get();
    lg->SetStartPoint(D2D1::Point2F(p0.x, p0.y));
    lg->SetEndPoint(D2D1::Point2F(p1.x, p1.y));
    if (radius > 0) dc_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(r), radius, radius), lg);
    else dc_->FillRectangle(ToD2D(r), lg);
}

void Gfx::SoftShadow(const Rect& r, float radius, float spread, Color c, float dy) {
    constexpr int kSteps = 10;
    Color step = c.A(c.a / kSteps);
    for (int i = kSteps; i >= 1; --i) {
        float e = spread * i / kSteps;
        Rect s{r.x - e, r.y - e + dy, r.w + 2 * e, r.h + 2 * e};
        FillRound(s, radius + e, step);
    }
}

// --- Images ----------------------------------------------------------------

ID2D1Bitmap1* Gfx::CreateBitmap(UINT w, UINT h, const void* data, UINT pitch) {
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap1* bmp = nullptr;
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), data, pitch, props, &bmp))) return nullptr;
    return bmp;
}

ID2D1Bitmap1* Gfx::CreateOpaqueBitmap(UINT w, UINT h) {
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ID2D1Bitmap1* bmp = nullptr;
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &bmp))) return nullptr;
    return bmp;
}

void Gfx::Glow(Vec2 center, float radius, Color c) {
    c.a *= alpha_.back();
    if (c.a <= 0.003f || radius <= 0) return;
    // One brush per (quantized) colour; position and size are cheap to change.
    uint64_t key = (static_cast<uint64_t>(c.r * 63) << 18) | (static_cast<uint64_t>(c.g * 63) << 12) |
                   (static_cast<uint64_t>(c.b * 63) << 6) | static_cast<uint64_t>(c.a * 63);
    auto it = glows_.find(key);
    if (it == glows_.end()) {
        if (glows_.size() > 64) glows_.clear();
        D2D1_GRADIENT_STOP stops[3] = {{0.f, ToD2D(c)}, {0.45f, ToD2D(c.A(c.a * 0.45f))}, {1.f, ToD2D(c.A(0))}};
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1RadialGradientBrush> rb;
        if (FAILED(dc_->CreateGradientStopCollection(stops, 3, &coll)) ||
            FAILED(dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({0, 0}, {0, 0}, 1, 1), coll.Get(), &rb)))
            return;
        it = glows_.emplace(key, rb).first;
    }
    ID2D1RadialGradientBrush* rb = it->second.Get();
    rb->SetCenter(D2D1::Point2F(center.x, center.y));
    rb->SetRadiusX(radius);
    rb->SetRadiusY(radius);
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y), radius, radius), rb);
}

Rect Gfx::FitRect(ID2D1Bitmap1* bmp, const Rect& dst, Fit fit, Vec2 anchor, float zoom) const {
    D2D1_SIZE_F s = bmp->GetSize();
    if (fit == Fit::Fill || s.width < 1 || s.height < 1) return dst;
    float k = fit == Fit::Cover ? std::max(dst.w / s.width, dst.h / s.height)
                                : std::min(dst.w / s.width, dst.h / s.height);
    k *= zoom;
    float w = s.width * k, h = s.height * k;
    return {dst.x + (dst.w - w) * anchor.x, dst.y + (dst.h - h) * anchor.y, w, h};
}

void Gfx::Image(ID2D1Bitmap1* bmp, const Rect& dst, float radius, float opacity, Fit fit, Vec2 anchor, float zoom) {
    if (!bmp) return;
    float op = opacity * alpha_.back();
    if (op <= 0.003f) return;
    Rect fr = FitRect(bmp, dst, fit, anchor, zoom);
    D2D1_SIZE_F s = bmp->GetSize();
    bool clipNeeded = radius > 0 || (fit == Fit::Cover && (fr.w > dst.w + 0.5f || fr.h > dst.h + 0.5f));
    if (!clipNeeded) {
        dc_->DrawBitmap(bmp, ToD2D(fr), op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return;
    }
    if (radius <= 0) {
        // Crop through the source rectangle: cheaper than a clip.
        float kx = s.width / fr.w, ky = s.height / fr.h;
        Rect vis{std::max(dst.x, fr.x), std::max(dst.y, fr.y), 0, 0};
        vis.w = std::min(dst.r(), fr.r()) - vis.x;
        vis.h = std::min(dst.b(), fr.b()) - vis.y;
        D2D1_RECT_F src = D2D1::RectF((vis.x - fr.x) * kx, (vis.y - fr.y) * ky, (vis.r() - fr.x) * kx,
                                      (vis.b() - fr.y) * ky);
        dc_->DrawBitmap(bmp, ToD2D(vis), op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src);
        return;
    }
    if (!bitmapBrush_) {
        D2D1_BITMAP_BRUSH_PROPERTIES1 bp = D2D1::BitmapBrushProperties1(
            D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        if (FAILED(dc_->CreateBitmapBrush(nullptr, &bp, nullptr, &bitmapBrush_))) return;
    }
    bitmapBrush_->SetBitmap(bmp);
    bitmapBrush_->SetOpacity(op);
    bitmapBrush_->SetTransform(Matrix3x2F::Scale(fr.w / s.width, fr.h / s.height) * Matrix3x2F::Translation(fr.x, fr.y));
    Rect area{std::max(dst.x, fr.x), std::max(dst.y, fr.y), 0, 0};
    area.w = std::min(dst.r(), fr.r()) - area.x;
    area.h = std::min(dst.b(), fr.b()) - area.y;
    dc_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(area), radius, radius), bitmapBrush_.Get());
    bitmapBrush_->SetBitmap(nullptr);  // don't keep evicted bitmaps alive
}

ID2D1Bitmap1* Gfx::Blurred(const std::wstring& key, ID2D1Bitmap1* src) {
    auto it = blurs_.find(key);
    if (it != blurs_.end()) {
        it->second.lastUse = now;
        return it->second.out.Get();
    }
    if (!src) return nullptr;
    BlurEntry e;
    e.src = src;
    e.lastUse = now;
    blurs_.emplace(key, std::move(e));
    blurQueue_.push_back(key);
    return nullptr;
}

// --- Text ------------------------------------------------------------------

IDWriteTextFormat* Gfx::Format(FontRole role, float size, DWRITE_FONT_WEIGHT weight) {
    uint64_t key = (static_cast<uint64_t>(role) << 48) | (static_cast<uint64_t>(weight) << 32) |
                   static_cast<uint32_t>(size * 100.f);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    const std::wstring& family = role == FontRole::Display ? fontDisplay_ : role == FontRole::Icon ? fontIcon_ : fontText_;
    ComPtr<IDWriteTextFormat> f;
    dwrite_->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                              size, L"pt-br", &f);
    formats_[key] = f;
    return f.Get();
}

ComPtr<IDWriteTextLayout> Gfx::Layout(std::wstring_view s, const TextStyle& st, float w, float h) {
    // Text layout (shaping, line breaking) is the most expensive part of a frame: reuse layouts
    // for identical text + style + box. Sizes are rounded so sub-pixel animation doesn't defeat the cache.
    Hasher hs;
    hs.bytes(s.data(), s.size() * sizeof(wchar_t));
    float qw = std::round(w * 2.f), qh = std::round(std::min(h, 100000.f) * 2.f);
    hs.add(qw);
    hs.add(qh);
    hs.add(st.size);
    hs.add(st.weight);
    hs.add(st.align);
    hs.add(st.valign);
    hs.add(st.tracking);
    hs.add(st.maxLines);
    hs.add(st.font);
    hs.add(st.lineHeight);
    auto cached = layouts_.find(hs.h);
    if (cached != layouts_.end()) {
        cached->second.lastUse = now;
        return cached->second.layout;
    }
    ComPtr<IDWriteTextLayout> layout = CreateLayout(s, st, w, h);
    if (layout) layouts_.emplace(hs.h, LayoutEntry{layout, now});
    if (now - lastSweep_ > 2.0) {
        lastSweep_ = now;
        for (auto it = layouts_.begin(); it != layouts_.end();)
            it = (now - it->second.lastUse > 3.0) ? layouts_.erase(it) : std::next(it);
        for (auto it = grads_.begin(); it != grads_.end();)
            it = (now - it->second.lastUse > 3.0) ? grads_.erase(it) : std::next(it);
    }
    return layout;
}

ComPtr<IDWriteTextLayout> Gfx::CreateLayout(std::wstring_view s, const TextStyle& st, float w, float h) {
    ComPtr<IDWriteTextLayout> layout;
    IDWriteTextFormat* fmt = Format(st.font, st.size, st.weight);
    if (!fmt || FAILED(dwrite_->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), fmt, std::max(1.f, w),
                                                 std::max(1.f, h), &layout)))
        return nullptr;
    layout->SetTextAlignment(st.align == Align::Center     ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : st.align == Align::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                           : DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(st.valign == VAlign::Center   ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                  : st.valign == VAlign::Bottom ? DWRITE_PARAGRAPH_ALIGNMENT_FAR
                                                                : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetWordWrapping(st.maxLines == 1 ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
    if (st.lineHeight > 0)
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, st.size * st.lineHeight, st.size * st.lineHeight * 0.8f);
    if (st.maxLines != 0) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject>& ellipsis = ellipsis_[fmt];
        if (!ellipsis) dwrite_->CreateEllipsisTrimmingSign(fmt, &ellipsis);
        layout->SetTrimming(&trim, ellipsis.Get());
    }
    if (st.tracking != 0) {
        ComPtr<IDWriteTextLayout1> l1;
        if (SUCCEEDED(layout.As(&l1)))
            l1->SetCharacterSpacing(0, st.tracking, 0, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(s.size())});
    }
    return layout;
}

void Gfx::Text(std::wstring_view s, const Rect& r, const TextStyle& st) {
    if (s.empty()) return;
    float h = r.h;
    if (st.maxLines > 1) {
        float lh = st.size * (st.lineHeight > 0 ? st.lineHeight : 1.33f);
        h = std::min(h, lh * st.maxLines + 1.f);
    }
    auto layout = Layout(s, st, r.w, h);
    if (!layout) return;
    Rect box{r.x, r.y, r.w, h};
    if (st.maxLines > 1 && st.valign != VAlign::Top && h < r.h) {
        // Re-center the clamped box inside the requested rect.
        DWRITE_TEXT_METRICS m;
        layout->GetMetrics(&m);
        float th = std::min(m.height, h);
        box.y = st.valign == VAlign::Center ? r.y + (r.h - th) * 0.5f : r.b() - th;
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    if (st.shadow) {
        dc_->DrawTextLayout(D2D1::Point2F(box.x, box.y + 1.5f), layout.Get(), Brush(Color::Black(0.35f * st.color.a)),
                            D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    dc_->DrawTextLayout(D2D1::Point2F(box.x, box.y), layout.Get(), Brush(st.color),
                        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

Vec2 Gfx::Measure(std::wstring_view s, const TextStyle& st, float maxWidth) {
    if (s.empty()) return {0, st.size * 1.33f};
    TextStyle t = st;
    t.align = Align::Leading;
    t.valign = VAlign::Top;
    auto layout = Layout(s, t, maxWidth, 100000.f);
    if (!layout) return {};
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

static void DrawTrophy(ID2D1Factory1* f, ID2D1DeviceContext* dc, ID2D1Brush* brush, ID2D1StrokeStyle* stroke,
                       Vec2 c, float s) {
    auto P = [&](float x, float y) { return D2D1::Point2F(c.x + x * s, c.y + y * s); };
    ComPtr<ID2D1PathGeometry> path;
    f->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> k;
    path->Open(&k);
    // Cup
    k->BeginFigure(P(-0.30f, -0.40f), D2D1_FIGURE_BEGIN_HOLLOW);
    k->AddLine(P(0.30f, -0.40f));
    k->AddLine(P(0.30f, -0.12f));
    k->AddBezier(D2D1::BezierSegment(P(0.30f, 0.10f), P(0.13f, 0.20f), P(0.f, 0.20f)));
    k->AddBezier(D2D1::BezierSegment(P(-0.13f, 0.20f), P(-0.30f, 0.10f), P(-0.30f, -0.12f)));
    k->EndFigure(D2D1_FIGURE_END_CLOSED);
    // Handles
    k->BeginFigure(P(-0.30f, -0.31f), D2D1_FIGURE_BEGIN_HOLLOW);
    k->AddBezier(D2D1::BezierSegment(P(-0.52f, -0.33f), P(-0.52f, -0.04f), P(-0.27f, -0.03f)));
    k->EndFigure(D2D1_FIGURE_END_OPEN);
    k->BeginFigure(P(0.30f, -0.31f), D2D1_FIGURE_BEGIN_HOLLOW);
    k->AddBezier(D2D1::BezierSegment(P(0.52f, -0.33f), P(0.52f, -0.04f), P(0.27f, -0.03f)));
    k->EndFigure(D2D1_FIGURE_END_OPEN);
    // Stem and base
    k->BeginFigure(P(0.f, 0.20f), D2D1_FIGURE_BEGIN_HOLLOW);
    k->AddLine(P(0.f, 0.36f));
    k->EndFigure(D2D1_FIGURE_END_OPEN);
    k->BeginFigure(P(-0.20f, 0.40f), D2D1_FIGURE_BEGIN_HOLLOW);
    k->AddLine(P(0.20f, 0.40f));
    k->EndFigure(D2D1_FIGURE_END_OPEN);
    k->Close();
    dc->DrawGeometry(path.Get(), brush, std::max(1.f, s * 0.085f), stroke);
}

void Gfx::Icon(wchar_t g, Vec2 center, float size, Color c) {
    if (g == glyph::Trophy) {
        DrawTrophy(factory_.Get(), dc_.Get(), Brush(c), roundCap_.Get(), center, size);
        return;
    }
    TextStyle st;
    st.font = FontRole::Icon;
    st.size = size;
    st.color = c;
    st.align = Align::Center;
    st.valign = VAlign::Center;
    wchar_t s[2] = {g, 0};
    Text(std::wstring_view(s, 1), Rect{center.x - size, center.y - size, size * 2, size * 2}, st);
}

}  // namespace lumen::ui
