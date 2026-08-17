#include "directwrite_text_renderer.h"

#include <dwrite.h>
#include <dwrite_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "dwrite.lib")

namespace shuru {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kFallbackFontFamily[] = L"Microsoft YaHei UI";

IDWriteFactory* Factory(void* value) noexcept {
    return static_cast<IDWriteFactory*>(value);
}

IDWriteGdiInterop* GdiInterop(void* value) noexcept {
    return static_cast<IDWriteGdiInterop*>(value);
}

DWRITE_FONT_WEIGHT ToDirectWriteWeight(CandidateTextWeight weight) noexcept {
    switch (weight) {
    case CandidateTextWeight::SemiBold:
        return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    case CandidateTextWeight::Bold:
        return DWRITE_FONT_WEIGHT_BOLD;
    case CandidateTextWeight::Regular:
    default:
        return DWRITE_FONT_WEIGHT_NORMAL;
    }
}

float PixelsPerDip(UINT dpi) noexcept {
    return static_cast<float>(dpi == 0 ? 96 : dpi) / 96.0f;
}

float PixelsToDip(int pixels, UINT dpi) noexcept {
    return static_cast<float>(pixels) / PixelsPerDip(dpi);
}

bool CreateTextFormat(
    IDWriteFactory* factory,
    const CandidateTextStyle& style,
    IDWriteTextFormat** result) {
    if (factory == nullptr || result == nullptr || style.font_size <= 0.0f) {
        return false;
    }
    *result = nullptr;
    const std::wstring family = style.font_family.empty()
        ? kFallbackFontFamily : style.font_family;
    ComPtr<IDWriteTextFormat> format;
    HRESULT hr = factory->CreateTextFormat(
        family.c_str(), nullptr, ToDirectWriteWeight(style.weight),
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        style.font_size, L"zh-CN", &format);
    if (FAILED(hr)) return false;

    format->SetTextAlignment(style.alignment == CandidateTextAlignment::Center
        ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    *result = format.Detach();
    return true;
}

bool CreateTextLayout(
    IDWriteFactory* factory,
    const std::wstring& text,
    const CandidateTextStyle& style,
    float width_dip,
    float height_dip,
    IDWriteTextLayout** result) {
    if (result == nullptr || text.empty() || width_dip <= 0.0f ||
        height_dip <= 0.0f) {
        return false;
    }
    *result = nullptr;
    ComPtr<IDWriteTextFormat> format;
    if (!CreateTextFormat(factory, style, &format)) return false;

    if (style.trim_with_ellipsis) {
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
            DWRITE_TRIMMING trimming {
                DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, ellipsis.Get());
        }
    }

    return SUCCEEDED(factory->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
        width_dip, height_dip, result));
}

void CompositePremultiplied(
    std::uint8_t* destination,
    int destination_width,
    int destination_height,
    const std::uint8_t* source,
    int source_width,
    int source_height,
    int destination_left,
    int destination_top) {
    for (int source_y = 0; source_y < source_height; ++source_y) {
        const int destination_y = destination_top + source_y;
        if (destination_y < 0 || destination_y >= destination_height) continue;
        for (int source_x = 0; source_x < source_width; ++source_x) {
            const int destination_x = destination_left + source_x;
            if (destination_x < 0 || destination_x >= destination_width) continue;

            const std::uint8_t* source_pixel = source +
                (static_cast<size_t>(source_y) * source_width + source_x) * 4;
            const std::uint32_t source_alpha = source_pixel[3];
            if (source_alpha == 0) continue;
            std::uint8_t* destination_pixel = destination +
                (static_cast<size_t>(destination_y) * destination_width +
                 destination_x) * 4;
            const std::uint32_t inverse = 255 - source_alpha;
            destination_pixel[0] = static_cast<std::uint8_t>(
                source_pixel[0] + (destination_pixel[0] * inverse + 127) / 255);
            destination_pixel[1] = static_cast<std::uint8_t>(
                source_pixel[1] + (destination_pixel[1] * inverse + 127) / 255);
            destination_pixel[2] = static_cast<std::uint8_t>(
                source_pixel[2] + (destination_pixel[2] * inverse + 127) / 255);
            destination_pixel[3] = static_cast<std::uint8_t>(
                source_alpha + (destination_pixel[3] * inverse + 127) / 255);
        }
    }
}

class DirectWriteGlyphRenderer final : public IDWriteTextRenderer {
public:
    DirectWriteGlyphRenderer(
        IDWriteBitmapRenderTarget* target,
        IDWriteRenderingParams* params,
        COLORREF color)
        : target_(target), params_(params), color_(color) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) ||
            iid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --references_;
        if (value == 0) delete this;
        return value;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(
        void*, BOOL* disabled) override {
        if (disabled == nullptr) return E_POINTER;
        *disabled = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(
        void*, DWRITE_MATRIX* transform) override {
        if (transform == nullptr) return E_POINTER;
        *transform = DWRITE_MATRIX {1, 0, 0, 1, 0, 0};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(
        void*, FLOAT* pixels_per_dip) override {
        if (pixels_per_dip == nullptr) return E_POINTER;
        *pixels_per_dip = target_->GetPixelsPerDip();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
        const DWRITE_GLYPH_RUN* run,
        const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        const FLOAT pixels_per_dip = target_->GetPixelsPerDip();
        y = std::round(y * pixels_per_dip) / pixels_per_dip;
        return target_->DrawGlyphRun(
            x, y, mode, run, params_, color_, nullptr);
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(
        void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(
        void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
        return S_OK;
    }

private:
    ULONG references_ = 1;
    IDWriteBitmapRenderTarget* target_;
    IDWriteRenderingParams* params_;
    COLORREF color_;
};

}  // namespace

DirectWriteTextRenderer::DirectWriteTextRenderer() {
    IDWriteFactory* factory = nullptr;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&factory)))) {
        return;
    }
    factory_ = factory;
    IDWriteGdiInterop* interop = nullptr;
    if (FAILED(factory->GetGdiInterop(&interop))) {
        factory->Release();
        factory_ = nullptr;
        return;
    }
    gdi_interop_ = interop;
}

DirectWriteTextRenderer::~DirectWriteTextRenderer() {
    EndFrame();
    if (gdi_interop_ != nullptr) GdiInterop(gdi_interop_)->Release();
    if (factory_ != nullptr) Factory(factory_)->Release();
}

bool DirectWriteTextRenderer::IsAvailable() const noexcept {
    return factory_ != nullptr && gdi_interop_ != nullptr;
}

bool DirectWriteTextRenderer::IsFontFamilyAvailable(
    const std::wstring& family) const {
    if (!IsAvailable() || family.empty()) return false;
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(Factory(factory_)->GetSystemFontCollection(&fonts, FALSE))) {
        return false;
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(fonts->FindFamilyName(family.c_str(), &index, &exists)) &&
        exists != FALSE;
}

std::wstring DirectWriteTextRenderer::ResolveFontFamily(
    const std::wstring& requested) const {
    return IsFontFamilyAvailable(requested) ? requested : kFallbackFontFamily;
}

float DirectWriteTextRenderer::MeasureText(
    const std::wstring& text,
    const CandidateTextStyle& style,
    UINT dpi) const {
    if (!IsAvailable() || text.empty()) return 0.0f;
    CandidateTextStyle resolved = style;
    resolved.font_family = ResolveFontFamily(style.font_family);
    ComPtr<IDWriteTextLayout> layout;
    if (!CreateTextLayout(
            Factory(factory_), text, resolved, 4096.0f,
            (std::max)(64.0f, resolved.font_size * 3.0f), &layout)) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics {};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
    return metrics.widthIncludingTrailingWhitespace * PixelsPerDip(dpi);
}

bool DirectWriteTextRenderer::DrawText(
    std::uint8_t* destination_pixels,
    int destination_width,
    int destination_height,
    const RECT& pixel_rect,
    const std::wstring& text,
    COLORREF color,
    const CandidateTextStyle& style,
    UINT dpi) {
    const int pixel_width = pixel_rect.right - pixel_rect.left;
    const int pixel_height = pixel_rect.bottom - pixel_rect.top;
    if (!IsAvailable() || destination_pixels == nullptr ||
        destination_width <= 0 || destination_height <= 0 || text.empty() ||
        pixel_width <= 0 || pixel_height <= 0) {
        return false;
    }

    if (!BeginFrame(destination_width, destination_height, dpi)) return false;
    const bool drawn = DrawTextInFrame(pixel_rect, text, color, style) &&
        CompositeFrame(destination_pixels, destination_width,
                       destination_height);
    EndFrame();
    return drawn;
}

bool DirectWriteTextRenderer::BeginFrame(int width, int height, UINT dpi) {
    EndFrame();
    if (!IsAvailable() || width <= 0 || height <= 0) return false;
    IDWriteBitmapRenderTarget* target = nullptr;
    if (FAILED(GdiInterop(gdi_interop_)->CreateBitmapRenderTarget(
            nullptr, static_cast<UINT32>(width), static_cast<UINT32>(height),
            &target))) {
        return false;
    }
    target->SetPixelsPerDip(PixelsPerDip(dpi));
    ComPtr<IDWriteBitmapRenderTarget1> grayscale_target;
    if (FAILED(target->QueryInterface(IID_PPV_ARGS(&grayscale_target))) ||
        FAILED(grayscale_target->SetTextAntialiasMode(
            DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE))) {
        target->Release();
        return false;
    }
    IDWriteRenderingParams* params = nullptr;
    if (FAILED(Factory(factory_)->CreateCustomRenderingParams(
            2.0f, 1.0f, 1.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
            DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &params))) {
        target->Release();
        return false;
    }
    HDC memory_dc = target->GetMemoryDC();
    HBITMAP bitmap = static_cast<HBITMAP>(GetCurrentObject(memory_dc, OBJ_BITMAP));
    DIBSECTION section {};
    if (bitmap == nullptr ||
        GetObjectW(bitmap, sizeof(section), &section) != sizeof(section) ||
        section.dsBm.bmBits == nullptr) {
        params->Release();
        target->Release();
        return false;
    }
    std::memset(section.dsBm.bmBits, 0,
                static_cast<size_t>(width) * height * 4);
    frame_target_ = target;
    frame_rendering_params_ = params;
    frame_dpi_ = dpi == 0 ? 96 : dpi;
    return true;
}

bool DirectWriteTextRenderer::DrawTextInFrame(
    const RECT& pixel_rect,
    const std::wstring& text,
    COLORREF color,
    const CandidateTextStyle& style) {
    auto* target = static_cast<IDWriteBitmapRenderTarget*>(frame_target_);
    auto* params = static_cast<IDWriteRenderingParams*>(frame_rendering_params_);
    const int pixel_width = pixel_rect.right - pixel_rect.left;
    const int pixel_height = pixel_rect.bottom - pixel_rect.top;
    if (target == nullptr || params == nullptr || text.empty() ||
        pixel_width <= 0 || pixel_height <= 0) {
        return false;
    }
    CandidateTextStyle resolved = style;
    resolved.font_family = ResolveFontFamily(style.font_family);
    ComPtr<IDWriteTextLayout> layout;
    if (!CreateTextLayout(
            Factory(factory_), text, resolved,
            PixelsToDip(pixel_width, frame_dpi_),
            PixelsToDip(pixel_height, frame_dpi_), &layout)) {
        return false;
    }
    const int saved_dc = SaveDC(target->GetMemoryDC());
    IntersectClipRect(
        target->GetMemoryDC(), pixel_rect.left, pixel_rect.top,
        pixel_rect.right, pixel_rect.bottom);
    ComPtr<IDWriteTextRenderer> renderer;
    renderer.Attach(new DirectWriteGlyphRenderer(target, params, color));
    const HRESULT draw_result = layout->Draw(
        nullptr, renderer.Get(), PixelsToDip(pixel_rect.left, frame_dpi_),
        PixelsToDip(pixel_rect.top, frame_dpi_));
    if (saved_dc != 0) RestoreDC(target->GetMemoryDC(), saved_dc);
    return SUCCEEDED(draw_result);
}

bool DirectWriteTextRenderer::CompositeFrame(
    std::uint8_t* destination_pixels,
    int destination_width,
    int destination_height) {
    auto* target = static_cast<IDWriteBitmapRenderTarget*>(frame_target_);
    if (target == nullptr || destination_pixels == nullptr) return false;
    SIZE size {};
    if (FAILED(target->GetSize(&size)) || size.cx <= 0 || size.cy <= 0 ||
        size.cx != destination_width || size.cy != destination_height) {
        return false;
    }
    HBITMAP bitmap = static_cast<HBITMAP>(
        GetCurrentObject(target->GetMemoryDC(), OBJ_BITMAP));
    DIBSECTION section {};
    if (bitmap == nullptr ||
        GetObjectW(bitmap, sizeof(section), &section) != sizeof(section) ||
        section.dsBm.bmBits == nullptr) {
        return false;
    }
    CompositePremultiplied(
        destination_pixels, destination_width, destination_height,
        static_cast<std::uint8_t*>(section.dsBm.bmBits),
        size.cx, size.cy, 0, 0);
    return true;
}

void DirectWriteTextRenderer::EndFrame() noexcept {
    if (frame_rendering_params_ != nullptr) {
        static_cast<IDWriteRenderingParams*>(frame_rendering_params_)->Release();
        frame_rendering_params_ = nullptr;
    }
    if (frame_target_ != nullptr) {
        static_cast<IDWriteBitmapRenderTarget*>(frame_target_)->Release();
        frame_target_ = nullptr;
    }
    frame_dpi_ = 96;
}

int CandidateVModeListFontSize(int candidate_font_size) noexcept {
    return (std::max)(14, candidate_font_size - 4);
}

}  // namespace shuru
