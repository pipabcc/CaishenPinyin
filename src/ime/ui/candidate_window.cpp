#include "candidate_window.h"
#include "skin_manager.h"
#include "common/runtime_config.h"

#include <Windows.h>
#include <objbase.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace shuru {
namespace {

Gdiplus::Color ToGdiplusColor(COLORREF c, BYTE alpha = 255) noexcept {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

std::wstring CandidateFontFamilyForTheme(
    const RuntimeConfig& config,
    const SkinTheme& skin) {
    if (!config.candidate_font_family.empty()) {
        return config.candidate_font_family;
    }
    return skin.font_family.empty()
        ? std::wstring(L"Microsoft YaHei UI") : skin.font_family;
}

int CandidateLineHeightForTheme(
    bool use_native_layout, int candidate_font_size) noexcept {
    constexpr int kDefaultCandidateLineHeight = 30;
    return use_native_layout
        ? (std::max)(24, candidate_font_size + 6)
        : (std::max)(kDefaultCandidateLineHeight, candidate_font_size + 10);
}

void AddRoundedRectangleToPath(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::RectF& rect,
    float radius) {
    if (radius <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }
    const float max_r = (std::min)(rect.Width, rect.Height) / 2.0f;
    if (radius > max_r) radius = max_r;
    const float d = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void DrawCandidatePinIcon(
    Gdiplus::Graphics& graphics,
    const RECT& rect,
    COLORREF color,
    bool pinned,
    bool hovered) {
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    if (width <= 0.0f || height <= 0.0f) return;

    const float center_x = (rect.left + rect.right) / 2.0f;
    const float center_y = (rect.top + rect.bottom) / 2.0f;
    if (hovered) {
        const float diameter = (std::min)(width, height) - 2.0f;
        Gdiplus::SolidBrush hover_brush(Gdiplus::Color(28, 15, 23, 42));
        graphics.FillEllipse(
            &hover_brush, center_x - diameter / 2.0f,
            center_y - diameter / 2.0f, diameter, diameter);
    }

    const float scale = (std::max)(0.75f, (std::min)(width / 18.0f, height / 30.0f));
    const float top = center_y - 6.0f * scale;
    Gdiplus::PointF head[] = {
        {center_x - 4.0f * scale, top},
        {center_x + 4.0f * scale, top},
        {center_x + 2.5f * scale, top + 2.5f * scale},
        {center_x + 2.5f * scale, top + 6.0f * scale},
        {center_x - 2.5f * scale, top + 6.0f * scale},
        {center_x - 2.5f * scale, top + 2.5f * scale},
    };
    Gdiplus::GraphicsPath head_path;
    head_path.AddPolygon(head, ARRAYSIZE(head));
    Gdiplus::Pen pen(ToGdiplusColor(color), (std::max)(1.0f, scale));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    if (pinned) {
        Gdiplus::SolidBrush brush(ToGdiplusColor(color));
        graphics.FillPath(&brush, &head_path);
    } else {
        graphics.DrawPath(&pen, &head_path);
    }
    graphics.DrawLine(
        &pen, center_x, top + 6.0f * scale,
        center_x, top + 11.5f * scale);
    graphics.DrawLine(
        &pen, center_x, top + 11.5f * scale,
        center_x - 1.5f * scale, top + 9.5f * scale);
}

}  // namespace

constexpr int kMaskSamplesPerAxis = 4;

std::uint8_t RoundedRectanglePixelCoverage(
    int pixel_x,
    int pixel_y,
    int left,
    int top,
    int width,
    int height,
    int radius) noexcept {
    if (width <= 0 || height <= 0 || pixel_x < left || pixel_y < top ||
        pixel_x >= left + width || pixel_y >= top + height) {
        return 0;
    }
    const int right = left + width;
    const int bottom = top + height;
    const int rounded_radius = (std::max)(
        0, (std::min)(radius, (std::min)(width, height) / 2));
    if (rounded_radius == 0 ||
        (pixel_x >= left + rounded_radius && pixel_x < right - rounded_radius) ||
        (pixel_y >= top + rounded_radius && pixel_y < bottom - rounded_radius)) {
        return 255;
    }

    int covered = 0;
    for (int sample_y = 0; sample_y < kMaskSamplesPerAxis; ++sample_y) {
        const double sample_y_position = pixel_y +
            (sample_y + 0.5) / static_cast<double>(kMaskSamplesPerAxis);
        const double nearest_y = (std::max)(
            static_cast<double>(top + rounded_radius),
            (std::min)(sample_y_position,
                       static_cast<double>(bottom - rounded_radius)));
        for (int sample_x = 0; sample_x < kMaskSamplesPerAxis; ++sample_x) {
            const double sample_x_position = pixel_x +
                (sample_x + 0.5) / static_cast<double>(kMaskSamplesPerAxis);
            const double nearest_x = (std::max)(
                static_cast<double>(left + rounded_radius),
                (std::min)(sample_x_position,
                           static_cast<double>(right - rounded_radius)));
            const double delta_x = sample_x_position - nearest_x;
            const double delta_y = sample_y_position - nearest_y;
            if (delta_x * delta_x + delta_y * delta_y <=
                static_cast<double>(rounded_radius * rounded_radius)) {
                ++covered;
            }
        }
    }
    constexpr int kSampleCount =
        kMaskSamplesPerAxis * kMaskSamplesPerAxis;
    return static_cast<std::uint8_t>(
        (covered * 255 + kSampleCount / 2) / kSampleCount);
}

namespace {

const wchar_t kWindowClass[] = L"ShuruCandidateWindowClass";
constexpr uint8_t kShadowOpacity = 44;
constexpr wchar_t kRuntimeSettingsChangedMessageName[] =
    L"CaishenPinyin.RuntimeSettingsChanged.v1";

std::vector<uint8_t> BuildRoundedCardMask(
    int bitmap_width,
    int bitmap_height,
    int left,
    int top,
    int card_width,
    int card_height,
    int radius) {
    std::vector<uint8_t> mask(
        static_cast<size_t>(bitmap_width) * static_cast<size_t>(bitmap_height), 0);
    if (bitmap_width <= 0 || bitmap_height <= 0 ||
        card_width <= 0 || card_height <= 0) {
        return mask;
    }

    const int right = (std::min)(bitmap_width, left + card_width);
    const int bottom = (std::min)(bitmap_height, top + card_height);
    for (int y = (std::max)(0, top); y < bottom; ++y) {
        for (int x = (std::max)(0, left); x < right; ++x) {
            const size_t index = static_cast<size_t>(y) * bitmap_width + x;
            mask[index] = RoundedRectanglePixelCoverage(
                x, y, left, top, card_width, card_height, radius);
        }
    }
    return mask;
}

void BoxBlurHorizontal(
    const std::vector<uint8_t>& source,
    std::vector<uint8_t>* destination,
    int width,
    int height,
    int radius) {
    const int divisor = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        int sum = 0;
        const size_t row = static_cast<size_t>(y) * width;
        for (int x = -radius; x <= radius; ++x) {
            if (x >= 0 && x < width) sum += source[row + x];
        }
        for (int x = 0; x < width; ++x) {
            (*destination)[row + x] = static_cast<uint8_t>(sum / divisor);
            const int remove_x = x - radius;
            const int add_x = x + radius + 1;
            if (remove_x >= 0) sum -= source[row + remove_x];
            if (add_x < width) sum += source[row + add_x];
        }
    }
}

void BoxBlurVertical(
    const std::vector<uint8_t>& source,
    std::vector<uint8_t>* destination,
    int width,
    int height,
    int radius) {
    const int divisor = radius * 2 + 1;
    for (int x = 0; x < width; ++x) {
        int sum = 0;
        for (int y = -radius; y <= radius; ++y) {
            if (y >= 0 && y < height) {
                sum += source[static_cast<size_t>(y) * width + x];
            }
        }
        for (int y = 0; y < height; ++y) {
            (*destination)[static_cast<size_t>(y) * width + x] =
                static_cast<uint8_t>(sum / divisor);
            const int remove_y = y - radius;
            const int add_y = y + radius + 1;
            if (remove_y >= 0) {
                sum -= source[static_cast<size_t>(remove_y) * width + x];
            }
            if (add_y < height) {
                sum += source[static_cast<size_t>(add_y) * width + x];
            }
        }
    }
}

void BlurMask(
    std::vector<uint8_t>* mask,
    int width,
    int height,
    int radius,
    int passes) {
    if (mask == nullptr || radius <= 0 || passes <= 0) return;
    std::vector<uint8_t> scratch(mask->size(), 0);
    for (int pass = 0; pass < passes; ++pass) {
        BoxBlurHorizontal(*mask, &scratch, width, height, radius);
        BoxBlurVertical(scratch, mask, width, height, radius);
    }
}

void BlendSolidColor(
    uint8_t* pixels,
    const std::vector<uint8_t>& mask,
    int bitmap_width,
    int bitmap_height,
    COLORREF color) {
    if (pixels == nullptr || bitmap_width <= 0 || bitmap_height <= 0 ||
        mask.size() != static_cast<size_t>(bitmap_width) * bitmap_height) {
        return;
    }
    const uint32_t blue = GetBValue(color);
    const uint32_t green = GetGValue(color);
    const uint32_t red = GetRValue(color);
    for (size_t index = 0; index < mask.size(); ++index) {
        const uint32_t coverage = mask[index];
        if (coverage == 0) continue;
        const uint32_t inverse = 255 - coverage;
        uint8_t* pixel = pixels + index * 4;
        pixel[0] = static_cast<uint8_t>(
            (blue * coverage + pixel[0] * inverse + 127) / 255);
        pixel[1] = static_cast<uint8_t>(
            (green * coverage + pixel[1] * inverse + 127) / 255);
        pixel[2] = static_cast<uint8_t>(
            (red * coverage + pixel[2] * inverse + 127) / 255);
    }
}

}  // namespace

UINT RuntimeSettingsChangedMessage() noexcept {
    static const UINT message = RegisterWindowMessageW(
        kRuntimeSettingsChangedMessageName);
    return message;
}

BYTE CandidateLayeredFontQuality() noexcept {
    return ANTIALIASED_QUALITY;
}

void CompositeCandidateSurfaceAndShadow(
    std::uint8_t* pixels,
    const std::vector<std::uint8_t>& surface_alpha,
    int width,
    int height,
    int shadow_offset,
    int blur_radius,
    int blur_passes,
    std::uint8_t shadow_opacity) {
    if (pixels == nullptr || width <= 0 || height <= 0 ||
        surface_alpha.size() != static_cast<size_t>(width) * height) {
        return;
    }

    std::vector<uint8_t> shadow_mask(surface_alpha.size(), 0);
    if (shadow_opacity > 0 && blur_radius > 0 && blur_passes > 0) {
        for (int y = 0; y < height; ++y) {
            const int destination_y = y + shadow_offset;
            if (destination_y < 0 || destination_y >= height) continue;
            const size_t source_row = static_cast<size_t>(y) * width;
            const size_t destination_row =
                static_cast<size_t>(destination_y) * width;
            std::copy_n(surface_alpha.begin() + source_row, width,
                        shadow_mask.begin() + destination_row);
        }
        BlurMask(&shadow_mask, width, height, blur_radius, blur_passes);
    }

    for (size_t index = 0; index < surface_alpha.size(); ++index) {
        const uint32_t foreground_alpha = surface_alpha[index];
        const uint32_t shadow_alpha =
            (static_cast<uint32_t>(shadow_mask[index]) * shadow_opacity + 127) /
            255;
        const uint32_t outside_shadow =
            ((255 - foreground_alpha) * shadow_alpha + 127) / 255;
        pixels[index * 4 + 3] = static_cast<uint8_t>(
            foreground_alpha + outside_shadow);
    }
}

CandidateWindow::~CandidateWindow() {
    Destroy();
}

bool CandidateWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    if (hwnd_ != nullptr) {
        return true;
    }

    // 候选窗可能在设置保存后才首次创建，此处补读配置以覆盖通知丢失场景。
    ReloadRuntimeConfig();

    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kWindowClass,
        L"",
        WS_POPUP,
        0,
        0,
        width_ + kShadowMargin * 2,
        height_ + kShadowMargin * 2,
        nullptr,
        nullptr,
        instance,
        this);

    if (hwnd_ != nullptr) RefreshTypingStats();

    return hwnd_ != nullptr;
}

void CandidateWindow::Destroy() {
    if (hwnd_ != nullptr) {
        StopReadyPolling();
        StopShiftReleasePolling();
        StopDeferredAction();
        StopSkinAnimation();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    ready_poll_ = nullptr;
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (font_comp_ != nullptr) {
        DeleteObject(font_comp_);
        font_comp_ = nullptr;
    }
    if (font_meta_ != nullptr) {
        DeleteObject(font_meta_);
        font_meta_ = nullptr;
    }
    if (font_header_title_ != nullptr) {
        DeleteObject(font_header_title_);
        font_header_title_ = nullptr;
    }
    if (font_utility_ != nullptr) {
        DeleteObject(font_utility_);
        font_utility_ = nullptr;
    }
    visible_ = false;
    instance_ = nullptr;
}

void CandidateWindow::ResetFonts() {
    for (HFONT* font : {&font_, &font_comp_, &font_meta_,
                        &font_header_title_, &font_utility_}) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    for (void** font : {&gdip_font_, &gdip_font_comp_, &gdip_font_meta_,
                        &gdip_font_header_title_, &gdip_font_utility_}) {
        if (*font != nullptr) {
            delete static_cast<Gdiplus::Font*>(*font);
            *font = nullptr;
        }
    }
    font_signature_.clear();
}

void CandidateWindow::ReloadRuntimeSettings() {
    const POINT anchor = visible_ ? ScreenPosition() : POINT {};
    ReloadRuntimeConfig();
    const std::wstring skin_id = GetRuntimeConfig().skin_id;
    SkinManager::Instance().ReloadSkin(skin_id);
    layout_skin_id_.clear();
    ResetFonts();
    StopSkinAnimation();
    layout_dirty_ = true;
    paint_dirty_ = true;
    if (visible_) {
        Show(anchor);
    }
}

void CandidateWindow::EnsureFonts() {
    const RuntimeConfig config = GetRuntimeConfig();
    SkinManager::Instance().EnsureSkin(config.skin_id);
    const auto& skin = SkinManager::Instance().CurrentTheme();
    const std::wstring family = CandidateFontFamilyForTheme(config, skin);
    const bool use_native_layout = skin.native_appearance &&
        !UsesPlainUtilityBackground();
    const int candidate_size = ResolveCandidateFontSize(
        config.candidate_font_size_mode, skin.font_size);
    const int utility_size = CandidateVModeListFontSize(candidate_size);
    const std::wstring signature = family + L"\n" +
        std::to_wstring(candidate_size) + L"\n" +
        std::to_wstring(utility_size) + L"\n" +
        std::to_wstring(use_native_layout ? 1 : 0) + L"\n" +
        std::to_wstring(Scale(100));
    if (!font_signature_.empty() && font_signature_ != signature) ResetFonts();
    font_signature_ = signature;
    const BYTE font_quality = CandidateLayeredFontQuality();
    if (font_ == nullptr) {
        font_ = CreateFontW(
            -Scale(candidate_size), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            font_quality, DEFAULT_PITCH | FF_DONTCARE,
            family.c_str());
    }
    if (font_comp_ == nullptr) {
        font_comp_ = CreateFontW(
            -Scale(candidate_size), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            font_quality, DEFAULT_PITCH | FF_DONTCARE,
            family.c_str());
    }
    if (font_meta_ == nullptr) {
        font_meta_ = CreateFontW(
            -Scale(CandidateMetadataFontSize(candidate_size)),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            font_quality, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
    }
    if (font_header_title_ == nullptr) {
        font_header_title_ = CreateFontW(
            -Scale(CandidateMetadataFontSize(candidate_size)),
            0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            font_quality, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
    }
    if (font_utility_ == nullptr) {
        font_utility_ = CreateFontW(
            -Scale(utility_size), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, font_quality,
            DEFAULT_PITCH | FF_DONTCARE, family.c_str());
    }

    if (gdip_font_ == nullptr) {
        Gdiplus::FontFamily font_family(family.c_str());
        if (!font_family.IsAvailable()) {
            gdip_font_ = new Gdiplus::Font(
                L"Microsoft YaHei UI",
                static_cast<Gdiplus::REAL>(Scale(candidate_size)),
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        } else {
            gdip_font_ = new Gdiplus::Font(
                &font_family,
                static_cast<Gdiplus::REAL>(Scale(candidate_size)),
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        }
    }
    if (gdip_font_comp_ == nullptr) {
        Gdiplus::FontFamily font_family(family.c_str());
        if (!font_family.IsAvailable()) {
            gdip_font_comp_ = new Gdiplus::Font(
                L"Microsoft YaHei UI",
                static_cast<Gdiplus::REAL>(Scale(candidate_size)),
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        } else {
            gdip_font_comp_ = new Gdiplus::Font(
                &font_family,
                static_cast<Gdiplus::REAL>(Scale(candidate_size)),
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        }
    }
    if (gdip_font_meta_ == nullptr) {
        const int meta_size = CandidateMetadataFontSize(candidate_size);
        gdip_font_meta_ = new Gdiplus::Font(
            L"Microsoft YaHei UI",
            static_cast<Gdiplus::REAL>(Scale(meta_size)),
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    }
    if (gdip_font_header_title_ == nullptr) {
        const int meta_size = CandidateMetadataFontSize(candidate_size);
        gdip_font_header_title_ = new Gdiplus::Font(
            L"Microsoft YaHei UI",
            static_cast<Gdiplus::REAL>(Scale(meta_size)),
            Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    }
    if (gdip_font_utility_ == nullptr) {
        Gdiplus::FontFamily font_family(family.c_str());
        gdip_font_utility_ = font_family.IsAvailable()
            ? static_cast<void*>(new Gdiplus::Font(
                &font_family,
                static_cast<Gdiplus::REAL>(Scale(utility_size)),
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel))
            : static_cast<void*>(new Gdiplus::Font(
                L"Microsoft YaHei UI",
                static_cast<Gdiplus::REAL>(Scale(utility_size)),
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel));
    }
}

int CandidateWindow::Scale(int value) const {
    UINT dpi = 96;
    if (hwnd_ != nullptr) {
        const UINT window_dpi = GetDpiForWindow(hwnd_);
        if (window_dpi != 0) {
            dpi = window_dpi;
        }
    }
    return MulDiv(value, static_cast<int>(dpi), 96);
}

int CandidateWindow::ShadowMargin() const {
    SkinManager::Instance().EnsureSkin(GetRuntimeConfig().skin_id);
    const auto& skin = SkinManager::Instance().CurrentTheme();
    const bool has_shadow = vertical_utility_mode_ || skin.has_shadow;
    return has_shadow ? Scale(kShadowMargin) : 0;
}

bool CandidateWindow::UsesPlainUtilityBackground() const {
    SkinManager::Instance().EnsureSkin(GetRuntimeConfig().skin_id);
    return ShouldUsePlainUtilityBackground(
        utility_mode_, SkinManager::Instance().CurrentTheme().is_user_skin);
}

SIZE CandidateWindow::WindowSize() const {
    return SIZE {width_, height_};
}

POINT CandidateWindow::ScreenPosition() const {
    RECT rect {};
    if (hwnd_ != nullptr && GetWindowRect(hwnd_, &rect)) {
        const int shadow_margin = ShadowMargin();
        return POINT {rect.left + shadow_margin, rect.top + shadow_margin};
    }
    return POINT {};
}

int CandidateWindow::MeasureText(HDC hdc, HFONT font, const std::wstring& text) const {
    if (hdc == nullptr || font == nullptr || text.empty()) {
        return 0;
    }
    HGDIOBJ old = SelectObject(hdc, font);
    SIZE sz {};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(hdc, old);
    return sz.cx;
}

void CandidateWindow::Show(const POINT& screen_pos) {
    if (hwnd_ == nullptr) {
        return;
    }
    const std::wstring configured_skin_id = GetRuntimeConfig().skin_id;
    if (layout_skin_id_ != configured_skin_id) {
        layout_skin_id_ = configured_skin_id;
        ResetFonts();
        SkinManager::Instance().EnsureSkin(configured_skin_id);
        layout_dirty_ = true;
        paint_dirty_ = true;
    }
    POINT pos = screen_pos;
    HMONITOR monitor = MonitorFromPoint(pos, MONITOR_DEFAULTTONEAREST);
    if (!visible_) RefreshTypingStats();
    if (layout_dirty_) RecalcSize();
    const int shadow_margin = ShadowMargin();
    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        const int work_width = (std::max)(1,
            static_cast<int>(mi.rcWork.right - mi.rcWork.left) -
            Scale(8) - shadow_margin * 2);
        const int work_height = (std::max)(1,
            static_cast<int>(mi.rcWork.bottom - mi.rcWork.top) -
            Scale(8) - shadow_margin * 2);
        width_ = (std::min)(width_, work_width);
        height_ = (std::min)(height_, work_height);
        if (pos.x + width_ + shadow_margin > mi.rcWork.right - Scale(4)) {
            pos.x = mi.rcWork.right - width_ - shadow_margin - Scale(4);
        }
        if (pos.x - shadow_margin < mi.rcWork.left + Scale(4)) {
            pos.x = mi.rcWork.left + shadow_margin + Scale(4);
        }
        if (pos.y + height_ + shadow_margin > mi.rcWork.bottom - Scale(4)) {
            pos.y = screen_pos.y - height_ - shadow_margin - Scale(4);
        }
        if (pos.y - shadow_margin < mi.rcWork.top + Scale(4)) {
            pos.y = mi.rcWork.top + shadow_margin + Scale(4);
        }
    }
    const POINT window_origin {pos.x - shadow_margin, pos.y - shadow_margin};
    const int window_width = width_ + shadow_margin * 2;
    const int window_height = height_ + shadow_margin * 2;
    RECT previous_rect {};
    const bool has_previous_rect = GetWindowRect(hwnd_, &previous_rect) != FALSE;
    const bool position_changed = !visible_ || !has_previous_rect ||
        previous_rect.left != window_origin.x || previous_rect.top != window_origin.y;
    const bool size_changed = !has_previous_rect ||
        previous_rect.right - previous_rect.left != window_width ||
        previous_rect.bottom - previous_rect.top != window_height;
    const bool needs_show = !visible_;
    if (needs_show || position_changed || size_changed || paint_dirty_) {
        if (!UpdateLayeredWindowContent(window_origin)) return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, window_origin.x, window_origin.y, window_width, window_height,
                 SWP_NOACTIVATE | (needs_show ? SWP_SHOWWINDOW : 0));
    visible_ = true;
    SyncSkinAnimation();
    if (needs_show) {
        // 像素和最终尺寸已经由 UpdateLayeredWindow 原子提交，此处只切换可见性。
        UpdateWindow(hwnd_);
    }
}

void CandidateWindow::Hide() {
    if (hwnd_ == nullptr) {
        return;
    }
    StopDeferredAction();
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    StopSkinAnimation();
    SkinManager::Instance().ResetAnimation();
    SetWindowPos(hwnd_, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void CandidateWindow::StopSkinAnimation() {
    if (skin_animation_timer_active_ && hwnd_ != nullptr) {
        KillTimer(hwnd_, kSkinAnimationTimerId);
    }
    skin_animation_timer_active_ = false;
}

void CandidateWindow::SyncSkinAnimation() {
    StopSkinAnimation();
    auto& manager = SkinManager::Instance();
    if (visible_ && hwnd_ != nullptr && manager.HasAnimation() &&
        !UsesPlainUtilityBackground()) {
        skin_animation_timer_active_ = SetTimer(
            hwnd_, kSkinAnimationTimerId,
            manager.CurrentFrameDelayMs(), nullptr) != 0;
    }
}

bool CandidateWindow::DisplayContentEquals(
    const std::wstring& composing,
    const std::vector<Candidate>& candidates,
    size_t page,
    size_t page_size) const {
    if (composing_ != composing || page_ != page || page_size_ != page_size ||
        candidates_.size() != candidates.size()) {
        return false;
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates_[i].text != candidates[i].text ||
            candidates_[i].full_content != candidates[i].full_content ||
            candidates_[i].action != candidates[i].action ||
            candidates_[i].action_data != candidates[i].action_data ||
            candidates_[i].pinned != candidates[i].pinned) {
            return false;
        }
    }
    return true;
}

void CandidateWindow::SetContent(
    const std::wstring& composing,
    const std::vector<Candidate>& candidates,
    size_t selected_index,
    size_t page,
    size_t page_size,
    bool utility_mode,
    bool vertical_utility_mode) {
    const size_t normalized_page_size = (std::max)(size_t{1}, page_size);
    const size_t page_count = candidates.empty() ? 1 :
        (candidates.size() + normalized_page_size - 1) / normalized_page_size;
    size_t normalized_selected = selected_index < candidates.size() ? selected_index : 0;
    size_t normalized_page = (std::min)(page, page_count - 1);
    if (!candidates.empty()) normalized_page = normalized_selected / normalized_page_size;

    const bool must_collapse = expanded_ &&
        candidates.size() <= normalized_page_size;
    if (must_collapse) expanded_ = false;
    const bool mode_changed = utility_mode_ != utility_mode ||
        vertical_utility_mode_ != vertical_utility_mode;
    const bool layout_changed = must_collapse || mode_changed ||
        !DisplayContentEquals(
            composing, candidates, normalized_page, normalized_page_size);
    const bool selection_changed = selected_ != normalized_selected;
    if (!layout_changed && !selection_changed) return;

    composing_ = composing;
    candidates_ = candidates;
    selected_ = normalized_selected;
    page_size_ = normalized_page_size;
    page_ = normalized_page;
    utility_mode_ = utility_mode;
    vertical_utility_mode_ = vertical_utility_mode;

    if (vertical_utility_mode_) {
        const int max_scroll = (candidates.size() > static_cast<size_t>(kVerticalMaxVisible)) ? (static_cast<int>(candidates.size()) - kVerticalMaxVisible) : 0;
        if (normalized_selected < static_cast<size_t>(scroll_offset_)) {
            scroll_offset_ = static_cast<int>(normalized_selected);
        } else if (normalized_selected >=
                   static_cast<size_t>(scroll_offset_ + kVerticalMaxVisible)) {
            scroll_offset_ = static_cast<int>(normalized_selected) -
                kVerticalMaxVisible + 1;
        }
        scroll_offset_ = (std::max)(0, (std::min)(max_scroll, scroll_offset_));
        if (hovered_row_ >= static_cast<int>(candidates.size())) {
            hovered_row_ = -1;
            hovered_delete_ = false;
        }
    } else {
        scroll_offset_ = 0;
        hovered_row_ = -1;
        hovered_delete_ = false;
    }

    layout_dirty_ = layout_dirty_ || layout_changed;
    paint_dirty_ = true;
    if (layout_dirty_) RecalcSize();
}

void CandidateWindow::SetSelectedIndex(size_t selected_index) {
    const size_t previous_page = page_;
    selected_ = selected_index;
    if (selected_ >= candidates_.size()) {
        selected_ = 0;
    }
    if (!candidates_.empty()) {
        page_ = selected_ / page_size_;
    } else {
        page_ = 0;
    }
    if (vertical_utility_mode_) {
        const int max_scroll = candidates_.size() >
                static_cast<size_t>(kVerticalMaxVisible)
            ? static_cast<int>(candidates_.size()) - kVerticalMaxVisible
            : 0;
        if (selected_ < static_cast<size_t>(scroll_offset_)) {
            scroll_offset_ = static_cast<int>(selected_);
        } else if (selected_ >=
                   static_cast<size_t>(scroll_offset_ + kVerticalMaxVisible)) {
            scroll_offset_ = static_cast<int>(selected_) -
                kVerticalMaxVisible + 1;
        }
        scroll_offset_ = (std::max)(
            0, (std::min)(max_scroll, scroll_offset_));
    }
    if (page_ != previous_page) {
        layout_dirty_ = true;
        RecalcSize();
    }
    paint_dirty_ = true;
    if (visible_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool CandidateWindow::SetExpanded(bool expanded) {
    const bool next = expanded && !vertical_utility_mode_ &&
        candidates_.size() > page_size_;
    if (expanded_ == next) return false;
    expanded_ = next;
    layout_dirty_ = true;
    paint_dirty_ = true;
    RecalcSize();
    if (visible_ && hwnd_ != nullptr) {
        Show(ScreenPosition());
    }
    return true;
}

bool CandidateWindow::ToggleExpanded() {
    return SetExpanded(!expanded_);
}

void CandidateWindow::SetEnglishMode(bool english) {
    english_mode_ = english;
    if (visible_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void CandidateWindow::SetTypingStats(const TypingStatsSnapshot& snapshot) {
    if (!snapshot.available) return;
    if (typing_stats_.available == snapshot.available &&
        typing_stats_.daily_count == snapshot.daily_count) {
        return;
    }
    typing_stats_ = snapshot;
    layout_dirty_ = true;
    paint_dirty_ = true;
    RecalcSize();
    if (visible_ && hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void CandidateWindow::SetPinningEnabled(bool enabled) {
    if (pinning_enabled_ == enabled) return;
    pinning_enabled_ = enabled;
    hovered_candidate_ = -1;
    hovered_pin_ = false;
    pressed_pin_candidate_ = -1;
    layout_dirty_ = true;
    paint_dirty_ = true;
    if (visible_ && hwnd_ != nullptr) {
        const POINT anchor = ScreenPosition();
        RecalcSize();
        Show(anchor);
    }
}

void CandidateWindow::RefreshTypingStats() {
    SetTypingStats(TypingStatsStore().Load());
}

void CandidateWindow::StartReadyPolling(std::function<bool()> poll) {
    if (hwnd_ == nullptr) return;
    ready_poll_ = std::move(poll);
    if (!ready_poll_active_) {
        ready_poll_active_ =
            SetTimer(hwnd_, kReadyPollTimerId, kReadyPollIntervalMs, nullptr) != 0;
    }
}

void CandidateWindow::StopReadyPolling() {
    if (ready_poll_active_ && hwnd_ != nullptr) {
        KillTimer(hwnd_, kReadyPollTimerId);
    }
    ready_poll_active_ = false;
    ready_poll_ = nullptr;
}

void CandidateWindow::StartShiftReleasePolling(
    std::function<bool()> poll) {
    if (hwnd_ == nullptr) return;
    StopShiftReleasePolling();
    shift_release_poll_ = std::move(poll);
    if (shift_release_poll_) {
        shift_release_poll_active_ = SetTimer(
            hwnd_, kShiftReleasePollTimerId,
            kShiftReleasePollIntervalMs, nullptr) != 0;
        if (!shift_release_poll_active_) shift_release_poll_ = nullptr;
    }
}

void CandidateWindow::StopShiftReleasePolling() {
    if (shift_release_poll_active_ && hwnd_ != nullptr) {
        KillTimer(hwnd_, kShiftReleasePollTimerId);
    }
    shift_release_poll_active_ = false;
    shift_release_poll_ = nullptr;
}

void CandidateWindow::StartDeferredAction(
    std::function<void()> action, UINT delay_ms) {
    if (hwnd_ == nullptr) return;
    StopDeferredAction();
    deferred_action_ = std::move(action);
    if (deferred_action_) {
        const UINT interval = (std::max)(UINT{1}, delay_ms);
        // WM_TIMER 可能在 KillTimer 后仍以旧 ID 留在消息队列中。每次
        // 请求换用新 ID，旧消息不会误执行当前这一次的回调。
        ++deferred_timer_serial_;
        if (deferred_timer_serial_ == 0) {
            deferred_timer_serial_ = 1;
        }
        deferred_timer_id_ = kDeferredActionTimerId + deferred_timer_serial_;
        deferred_action_active_ = SetTimer(
            hwnd_, deferred_timer_id_, interval, nullptr) != 0;
        if (!deferred_action_active_) {
            deferred_action_ = nullptr;
        }
    }
}

void CandidateWindow::StopDeferredAction() {
    if (deferred_action_active_ && hwnd_ != nullptr) {
        KillTimer(hwnd_, deferred_timer_id_);
    }
    deferred_action_active_ = false;
    deferred_action_ = nullptr;
}

void CandidateWindow::StartVModeTimer(std::function<void()> callback, UINT delay_ms) {
    if (hwnd_ == nullptr) return;
    StopVModeTimer();
    vmode_timer_cb_ = std::move(callback);
    if (vmode_timer_cb_) {
        vmode_timer_active_ = SetTimer(hwnd_, kVModeTimerId, delay_ms, nullptr) != 0;
    }
}

void CandidateWindow::StopVModeTimer() {
    if (vmode_timer_active_ && hwnd_ != nullptr) {
        KillTimer(hwnd_, kVModeTimerId);
    }
    vmode_timer_active_ = false;
    vmode_timer_cb_ = nullptr;
}

void CandidateWindow::OpenSettings() {
    if (!LaunchSettingsExecutable(hwnd_, instance_)) {
        MessageBoxW(hwnd_, L"无法找到或启动 ShuruSettings.exe。请重新安装设置程序。",
                    L"财神输入法", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
}

void CandidateWindow::RecalcSize() {
    EnsureFonts();
    const auto& skin = SkinManager::Instance().CurrentTheme();
    const RuntimeConfig config = GetRuntimeConfig();
    const bool use_native_layout = skin.native_appearance &&
        !UsesPlainUtilityBackground();
    const int candidate_size = ResolveCandidateFontSize(
        config.candidate_font_size_mode, skin.font_size);
    const int line_height = Scale(CandidateLineHeightForTheme(
        use_native_layout, candidate_size));

    if (gdip_font_ == nullptr || gdip_font_comp_ == nullptr ||
        gdip_font_meta_ == nullptr || gdip_font_utility_ == nullptr) {
        width_ = Scale(kMinWidth);
        height_ = Scale(kVerticalPadding * 2 + kLineHeight * 2 + kRowGap);
        layout_dirty_ = false;
        return;
    }

    auto* font = static_cast<Gdiplus::Font*>(gdip_font_);
    auto* font_comp = static_cast<Gdiplus::Font*>(gdip_font_comp_);
    auto* font_meta = static_cast<Gdiplus::Font*>(gdip_font_meta_);

    Gdiplus::Bitmap dummy(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&dummy);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces | Gdiplus::StringFormatFlagsNoWrap);

    const auto measure_str = [&](
        Gdiplus::Font* f, const std::wstring& text,
        const CandidateTextStyle& style) -> int {
        if (f == nullptr || text.empty()) return 0;
        const float directwrite_width = directwrite_text_.MeasureText(
            text, style, hwnd_ == nullptr ? 96 : GetDpiForWindow(hwnd_));
        if (directwrite_width > 0.0f) {
            return static_cast<int>(std::ceil(directwrite_width));
        }
        Gdiplus::RectF bbox;
        g.MeasureString(text.c_str(), static_cast<INT>(text.size()), f, Gdiplus::PointF(0, 0), &format, &bbox);
        return static_cast<int>(std::ceil(bbox.Width));
    };

    if (vertical_utility_mode_) {
        width_ = Scale(350);
        const int top_bar_h = Scale(38);
        const int row_h = Scale(kVerticalRowHeight);
        const size_t visible_count = candidates_.empty() ? 1 : (std::min)(candidates_.size(), static_cast<size_t>(kVerticalMaxVisible));
        height_ = top_bar_h + static_cast<int>(visible_count) * row_h + Scale(10);
        layout_dirty_ = false;
        return;
    }

    const std::wstring family = CandidateFontFamilyForTheme(config, skin);
    const CandidateTextStyle candidate_style {
        family, static_cast<float>(candidate_size),
        CandidateTextWeight::Regular, CandidateTextAlignment::Near, false};
    const CandidateTextStyle composing_style {
        family, static_cast<float>(candidate_size),
        CandidateTextWeight::Regular, CandidateTextAlignment::Near, true};
    const CandidateTextStyle metadata_style {
        L"Microsoft YaHei UI",
        static_cast<float>(CandidateMetadataFontSize(candidate_size)),
        CandidateTextWeight::Regular, CandidateTextAlignment::Near, false};
    const int comp_w = measure_str(
        font_comp, composing_.empty() ? L" " : composing_, composing_style);
    const std::wstring status_text = BuildTypingStatisticsText(
        typing_stats_.daily_count);
    const bool can_expand = candidates_.size() > page_size_;
    const int header_right_w = measure_str(font_meta, status_text, metadata_style) +
        (can_expand ? Scale(kExpandToggleGap + kExpandToggleWidth) : 0);
    int cand_w = 0;
    const int padding = use_native_layout
        ? Scale(skin.candidate_margin.left)
        : Scale(kHorizontalPadding);
    const int candidate_right_padding = use_native_layout
        ? Scale(skin.candidate_margin.right)
        : padding;
    const size_t first_page = expanded_
        ? CandidateExpandedFirstPage(page_, kExpandedMaxRows)
        : page_;
    const size_t row_count = expanded_
        ? CandidateExpandedRowCount(
            candidates_.size(), page_size_, page_, kExpandedMaxRows)
        : 1;
    item_rows_.clear();
    item_rows_.reserve(row_count);
    for (size_t row = 0; row < row_count; ++row) {
        const size_t row_page = first_page + row;
        const size_t begin = row_page * page_size_;
        const size_t end = (std::min)(
            candidates_.size(), begin + page_size_);
        const bool numbered = row_page == page_;
        std::vector<int> item_widths;
        item_widths.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
            std::wstring item;
            if (numbered) {
                item = std::to_wstring(i - begin + 1) + L".";
            }
            item += candidates_[i].text;
            int item_width = measure_str(font, item, candidate_style);
            if (pinning_enabled_) item_width += Scale(kPinReservedWidth);
            item_widths.push_back(item_width);
        }
        item_rows_.push_back(BuildCandidateRowLayout(
            item_widths, begin,
            use_native_layout ? padding : padding + Scale(4), Scale(4),
            Scale(8), Scale(16)));
        cand_w = (std::max)(cand_w,
            CandidateRowRequiredWidth(item_rows_.back(), candidate_right_padding));
    }

    const int header_left = use_native_layout
        ? Scale(skin.pinyin_margin.left) : padding + Scale(4);
    const int header_right = use_native_layout
        ? Scale((std::max)(skin.pinyin_margin.right,
                           skin.candidate_margin.right)) : padding;
    const int header_w = CandidateHeaderRequiredWidth(
        comp_w, header_right_w, header_left, header_right,
        Scale(kHeaderTextGap));
    const int minimum_width = use_native_layout
        ? Scale((std::max)(skin.native_width, kMinWidth))
        : Scale(kMinWidth);
    width_ = (std::max)(minimum_width,
        (std::min)(Scale(kMaxWidth), (std::max)(header_w, cand_w)));
    if (use_native_layout) {
        const int candidate_top = Scale(skin.pinyin_margin.top) +
            line_height + Scale(
                skin.pinyin_margin.bottom + skin.candidate_margin.top);
        const int content_height = candidate_top +
            static_cast<int>(row_count) * line_height +
            Scale(skin.candidate_margin.bottom);
        height_ = (std::max)(Scale(skin.native_height), content_height);
    } else {
        const auto vertical = BuildCandidateWindowVerticalLayout(
            Scale(kVerticalPadding), line_height, Scale(kRowGap));
        height_ = vertical.window_height +
            static_cast<int>(row_count - 1) * line_height;
    }
    layout_dirty_ = false;
}

int CandidateWindow::HitTestCandidate(int x, int y) const {
    const int shadow_margin = ShadowMargin();
    x -= shadow_margin;
    y -= shadow_margin;

    if (vertical_utility_mode_) {
        const int top_bar_h = Scale(38);
        const int row_h = Scale(kVerticalRowHeight);
        if (y < top_bar_h || y >= height_) return -1;
        const int row = (y - top_bar_h) / row_h;
        const int idx = scroll_offset_ + row;
        if (idx >= 0 && static_cast<size_t>(idx) < candidates_.size()) {
            return idx;
        }
        return -1;
    }

    SkinManager::Instance().EnsureSkin(GetRuntimeConfig().skin_id);
    const auto& skin = SkinManager::Instance().CurrentTheme();
    const RuntimeConfig config = GetRuntimeConfig();
    const bool use_native_layout = skin.native_appearance &&
        !UsesPlainUtilityBackground();
    const int candidate_size = ResolveCandidateFontSize(
        config.candidate_font_size_mode, skin.font_size);
    const int row_height = Scale(CandidateLineHeightForTheme(
        use_native_layout, candidate_size));
    const int candidate_top = use_native_layout
        ? Scale(skin.pinyin_margin.top) + row_height +
            Scale(skin.pinyin_margin.bottom + skin.candidate_margin.top)
        : BuildCandidateWindowVerticalLayout(
            Scale(kVerticalPadding), row_height, Scale(kRowGap)).candidate_top;
    const int row = (y - candidate_top) / row_height;
    if (y < candidate_top || row < 0 ||
        static_cast<size_t>(row) >= item_rows_.size()) {
        return -1;
    }

    int hit = -1;
    for (const auto& item : item_rows_[static_cast<size_t>(row)]) {
        if (x >= item.hit_left && x < item.hit_right) {
            hit = static_cast<int>(item.index);
            break;
        }
    }
    return hit;
}

int CandidateWindow::HitTestPin(int x, int y) const {
    if (!pinning_enabled_ || vertical_utility_mode_) return -1;
    const int shadow_margin = ShadowMargin();
    x -= shadow_margin;
    y -= shadow_margin;

    SkinManager::Instance().EnsureSkin(GetRuntimeConfig().skin_id);
    const auto& skin = SkinManager::Instance().CurrentTheme();
    const RuntimeConfig config = GetRuntimeConfig();
    const bool use_native_layout = skin.native_appearance &&
        !UsesPlainUtilityBackground();
    const int candidate_size = ResolveCandidateFontSize(
        config.candidate_font_size_mode, skin.font_size);
    const int row_height = Scale(CandidateLineHeightForTheme(
        use_native_layout, candidate_size));
    const int candidate_top = use_native_layout
        ? Scale(skin.pinyin_margin.top) + row_height +
            Scale(skin.pinyin_margin.bottom + skin.candidate_margin.top)
        : BuildCandidateWindowVerticalLayout(
            Scale(kVerticalPadding), row_height, Scale(kRowGap)).candidate_top;
    const int row = (y - candidate_top) / row_height;
    if (y < candidate_top || row < 0 ||
        static_cast<size_t>(row) >= item_rows_.size()) {
        return -1;
    }

    const int content_right_padding = use_native_layout
        ? Scale(skin.candidate_margin.right)
        : Scale(kHorizontalPadding);
    for (const auto& item : item_rows_[static_cast<size_t>(row)]) {
        const RECT rect = BuildCandidatePinRect(
            item, width_, content_right_padding,
            Scale(kPinReservedWidth), candidate_top + row * row_height,
            candidate_top + (row + 1) * row_height);
        if (x >= rect.left && x < rect.right &&
            y >= rect.top && y < rect.bottom) {
            return static_cast<int>(item.index);
        }
    }
    return -1;
}

void CandidateWindow::DrawContent(
    void* graphics_ptr, uint8_t* pixels, int bitmap_width, int bitmap_height,
    int content_offset) {
    if (graphics_ptr == nullptr || pixels == nullptr ||
        bitmap_width <= 0 || bitmap_height <= 0) return;
    auto& g = *reinterpret_cast<Gdiplus::Graphics*>(graphics_ptr);

    SkinManager::Instance().EnsureSkin(GetRuntimeConfig().skin_id);
    const auto& skin = SkinManager::Instance().CurrentTheme();
    const RuntimeConfig config = GetRuntimeConfig();
    const bool plain_utility_background = UsesPlainUtilityBackground();
    const bool use_native_layout = skin.native_appearance &&
        !plain_utility_background;

    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    // 绘制背景：优先使用皮肤 9-Slice 位图切片，否则使用默认白底圆角矩形
    bool drew_bg = false;
    if (skin.has_bg_image && !plain_utility_background) {
        drew_bg = SkinManager::Instance().DrawBackground(
            &g, width_, height_, hwnd_ == nullptr ? 96 : GetDpiForWindow(hwnd_));
    }
    if (!drew_bg) {
        Gdiplus::GraphicsPath bg_path;
        Gdiplus::RectF bg_rect(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
        const int corner_radius = vertical_utility_mode_
            ? 8 : skin.corner_radius;
        AddRoundedRectangleToPath(bg_path, bg_rect, static_cast<float>(Scale(corner_radius)));
        const COLORREF background = plain_utility_background
            ? skin.utility_background_color : RGB(255, 255, 255);
        Gdiplus::SolidBrush bg_brush(ToGdiplusColor(background));
        g.FillPath(&bg_brush, &bg_path);
        Gdiplus::Pen border_pen(Gdiplus::Color(255, 226, 232, 240), 1.0f);
        g.DrawPath(&border_pen, &bg_path);
    }

    EnsureFonts();
    if (gdip_font_ == nullptr || gdip_font_comp_ == nullptr ||
        gdip_font_meta_ == nullptr || gdip_font_utility_ == nullptr) return;

    auto* font = static_cast<Gdiplus::Font*>(gdip_font_);
    auto* font_comp = static_cast<Gdiplus::Font*>(gdip_font_comp_);
    auto* font_meta = static_cast<Gdiplus::Font*>(gdip_font_meta_);
    auto* font_header_title = static_cast<Gdiplus::Font*>(gdip_font_header_title_);
    auto* font_utility = static_cast<Gdiplus::Font*>(gdip_font_utility_);
    const UINT dpi = hwnd_ == nullptr ? 96 : GetDpiForWindow(hwnd_);
    const int candidate_size = ResolveCandidateFontSize(
        config.candidate_font_size_mode, skin.font_size);
    const std::wstring candidate_family = directwrite_text_.ResolveFontFamily(
        CandidateFontFamilyForTheme(config, skin));
    const bool directwrite_frame_started = directwrite_text_.BeginFrame(
        bitmap_width, bitmap_height, dpi);

    const auto draw_text = [&](
        const std::wstring& text, const RECT& rect, COLORREF color,
        const CandidateTextStyle& style, Gdiplus::Font* fallback_font,
        Gdiplus::StringFormat* fallback_format) {
        if (rect.right <= rect.left || rect.bottom <= rect.top || text.empty()) return;
        RECT destination_rect = rect;
        OffsetRect(&destination_rect, content_offset, content_offset);
        if (directwrite_frame_started && directwrite_text_.DrawTextInFrame(
                destination_rect, text, color, style)) {
            return;
        }
        Gdiplus::SolidBrush brush(ToGdiplusColor(color));
        Gdiplus::RectF fallback_rect(
            static_cast<float>(rect.left), static_cast<float>(rect.top),
            static_cast<float>(rect.right - rect.left),
            static_cast<float>(rect.bottom - rect.top));
        g.DrawString(text.c_str(), -1, fallback_font, fallback_rect,
                     fallback_format, &brush);
    };
    const auto finish_text_frame = [&]() {
        if (!directwrite_frame_started) return;
        // GDI+ 可能仍缓存对同一 DIB 的背景和矢量绘制，先同步再叠加文字层。
        g.Flush(Gdiplus::FlushIntentionSync);
        directwrite_text_.CompositeFrame(
            pixels, bitmap_width, bitmap_height);
        directwrite_text_.EndFrame();
    };

    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetAlignment(Gdiplus::StringAlignmentNear);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    Gdiplus::StringFormat format_center(Gdiplus::StringFormat::GenericTypographic());
    format_center.SetAlignment(Gdiplus::StringAlignmentCenter);
    format_center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format_center.SetFormatFlags(format_center.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

    if (vertical_utility_mode_) {
        // ================= 竖向瀑布流绘制 =================
        const int top_bar_h = Scale(38);
        const int row_h = Scale(kVerticalRowHeight);
        const int padding = Scale(12);

        // 1. 顶部标题 (字号适中精致)
        std::wstring title = composing_.rfind(L"vv", 0) == 0 ? L"自定义短语" : L"复制记录";
        const RECT title_rc {padding, Scale(4), padding + Scale(120),
                             Scale(4) + top_bar_h};
        const CandidateTextStyle title_style {
            L"Microsoft YaHei UI",
            static_cast<float>(CandidateMetadataFontSize(candidate_size)),
            CandidateTextWeight::SemiBold, CandidateTextAlignment::Near, true};
        const COLORREF title_color = plain_utility_background
            ? EnsureCandidateTextContrast(
                skin.pinyin_color, skin.utility_background_color)
            : skin.pinyin_color;
        draw_text(title, title_rc, title_color, title_style,
                  font_header_title, &format);

        // 2. 右侧可点击搜索框胶囊
        std::wstring search_query;
        if (composing_.rfind(L"vv", 0) == 0 && composing_.size() > 2) search_query = composing_.substr(2);
        else if (composing_.rfind(L"v", 0) == 0 && composing_.size() > 1 && composing_[1] != L'v') search_query = composing_.substr(1);

        const int box_w = Scale(145);
        search_box_rect_ = RECT {width_ - padding - box_w, Scale(6), width_ - padding, top_bar_h - Scale(6)};

        Gdiplus::RectF search_rf(
            static_cast<float>(search_box_rect_.left),
            static_cast<float>(search_box_rect_.top),
            static_cast<float>(search_box_rect_.right - search_box_rect_.left),
            static_cast<float>(search_box_rect_.bottom - search_box_rect_.top));
        Gdiplus::GraphicsPath search_path;
        AddRoundedRectangleToPath(search_path, search_rf, static_cast<float>(Scale(6)));
        Gdiplus::SolidBrush search_bg(Gdiplus::Color(255, 248, 250, 252));
        Gdiplus::Pen search_pen(Gdiplus::Color(255, 226, 232, 240), 1.0f);
        g.FillPath(&search_bg, &search_path);
        g.DrawPath(&search_pen, &search_path);

        std::wstring search_text = search_query.empty() ? L"🔍 搜索..." : (L"🔍 " + search_query);
        const RECT search_text_rc {
            search_box_rect_.left + Scale(8), search_box_rect_.top,
            search_box_rect_.right - (search_query.empty() ? Scale(6) : Scale(20)),
            search_box_rect_.bottom};
        const CandidateTextStyle search_style {
            L"Microsoft YaHei UI", 14.0f, CandidateTextWeight::Regular,
            CandidateTextAlignment::Near, true};
        draw_text(search_text, search_text_rc,
                  search_query.empty() ? RGB(148, 163, 184) : RGB(30, 41, 59),
                  search_style, font_meta, &format);

        if (!search_query.empty()) {
            search_clear_rect_ = RECT {search_box_rect_.right - Scale(20), search_box_rect_.top, search_box_rect_.right - Scale(2), search_box_rect_.bottom};
            CandidateTextStyle clear_style = search_style;
            clear_style.alignment = CandidateTextAlignment::Center;
            draw_text(L"✕", search_clear_rect_, RGB(148, 163, 184),
                      clear_style, font_meta, &format_center);
        } else {
            search_clear_rect_ = {};
        }

        // 3. 分隔线
        Gdiplus::Pen sep_pen(ToGdiplusColor(skin.separator_color), 1.0f);
        g.DrawLine(&sep_pen,
            static_cast<float>(Scale(6)), static_cast<float>(top_bar_h),
            static_cast<float>(width_ - Scale(6)), static_cast<float>(top_bar_h));

        // 4. 竖向候选列表
        if (candidates_.empty()) {
            const RECT empty_rc {padding, top_bar_h + Scale(16),
                                 width_ - padding, top_bar_h + Scale(50)};
            const CandidateTextStyle empty_style {
                candidate_family,
                static_cast<float>(CandidateVModeListFontSize(candidate_size)),
                CandidateTextWeight::Regular, CandidateTextAlignment::Center, true};
            const COLORREF empty_color = plain_utility_background
                ? EnsureCandidateTextContrast(
                    skin.status_text_color, skin.utility_background_color)
                : skin.status_text_color;
            draw_text(L"暂无记录", empty_rc, empty_color,
                      empty_style, font_utility, &format_center);
        } else {
            const size_t start_idx = static_cast<size_t>(scroll_offset_);
            const size_t end_idx = (std::min)(candidates_.size(), start_idx + static_cast<size_t>(kVerticalMaxVisible));

            for (size_t i = start_idx; i < end_idx; ++i) {
                const int row_y = top_bar_h + static_cast<int>(i - start_idx) * row_h + Scale(3);
                const int right_bound = width_ - Scale(candidates_.size() > static_cast<size_t>(kVerticalMaxVisible) ? 12 : 6);
                RECT row_rc {Scale(6), row_y, right_bound, row_y + row_h};

                const bool is_hovered = (static_cast<int>(i) == hovered_row_);
                const bool is_selected = (i == selected_);

                if (is_selected || is_hovered) {
                    Gdiplus::RectF row_rf(
                        static_cast<float>(row_rc.left),
                        static_cast<float>(row_rc.top + Scale(1)),
                        static_cast<float>(row_rc.right - row_rc.left),
                        static_cast<float>(row_rc.bottom - row_rc.top - Scale(2)));
                    Gdiplus::GraphicsPath row_path;
                    AddRoundedRectangleToPath(row_path, row_rf, static_cast<float>(Scale(6)));
                    Gdiplus::SolidBrush row_bg(is_selected
                        ? Gdiplus::Color(255, 238, 242, 255)
                        : Gdiplus::Color(255, 248, 250, 252));
                    g.FillPath(&row_bg, &row_path);
                }

                // 绘制圆点 •
                COLORREF dot_color = is_selected ? skin.highlight_bg_color : skin.index_color;
                COLORREF item_text_color = is_selected
                    ? skin.highlight_bg_color : skin.candidate_color;
                if (plain_utility_background) {
                    const COLORREF row_background = is_selected
                        ? RGB(238, 242, 255)
                        : (is_hovered ? RGB(248, 250, 252)
                                      : skin.utility_background_color);
                    dot_color = EnsureCandidateTextContrast(
                        dot_color, row_background);
                    item_text_color = EnsureCandidateTextContrast(
                        item_text_color, row_background);
                }
                const CandidateTextStyle utility_style {
                    candidate_family,
                    static_cast<float>(CandidateVModeListFontSize(candidate_size)),
                    CandidateTextWeight::Regular, CandidateTextAlignment::Near, true};
                const RECT dot_rc {Scale(14), row_y, Scale(26), row_y + row_h};
                draw_text(L"•", dot_rc, dot_color, utility_style,
                          font_utility, &format);

                // 绘制文本
                const RECT item_text_rc {Scale(26), row_y,
                                         row_rc.right - Scale(30), row_y + row_h};
                draw_text(candidates_[i].text, item_text_rc, item_text_color,
                          utility_style, font_utility, &format);

                // 悬停显示精致矢量简约垃圾桶
                if (is_hovered) {
                    const float btn_x = static_cast<float>(row_rc.right - Scale(22));
                    const float btn_y = static_cast<float>(row_y + (row_h - Scale(14)) / 2);
                    const float icon_w = static_cast<float>(Scale(11));
                    const float icon_h = static_cast<float>(Scale(13));
                    Gdiplus::Color trash_color = hovered_delete_ ? Gdiplus::Color(255, 239, 68, 68) : Gdiplus::Color(255, 156, 163, 175);
                    Gdiplus::Pen trash_pen(trash_color, 1.0f);

                    g.DrawLine(&trash_pen, btn_x + Scale(4), btn_y, btn_x + Scale(7), btn_y);
                    g.DrawLine(&trash_pen, btn_x + Scale(1), btn_y + Scale(2), btn_x + icon_w - Scale(1), btn_y + Scale(2));
                    const Gdiplus::PointF body_pts[4] = {
                        Gdiplus::PointF(btn_x + Scale(2), btn_y + Scale(3)),
                        Gdiplus::PointF(btn_x + icon_w - Scale(2), btn_y + Scale(3)),
                        Gdiplus::PointF(btn_x + icon_w - Scale(3), btn_y + icon_h),
                        Gdiplus::PointF(btn_x + Scale(3), btn_y + icon_h)
                    };
                    g.DrawPolygon(&trash_pen, body_pts, 4);
                    g.DrawLine(&trash_pen, btn_x + Scale(4), btn_y + Scale(5), btn_x + Scale(4), btn_y + icon_h - Scale(2));
                    g.DrawLine(&trash_pen, btn_x + Scale(6), btn_y + Scale(5), btn_x + Scale(6), btn_y + icon_h - Scale(2));
                }
            }
        }

        // 5. 动态呼吸滚动条
        if (candidates_.size() > static_cast<size_t>(kVerticalMaxVisible)) {
            const int track_top = top_bar_h + Scale(6);
            const int track_bottom = height_ - Scale(6);
            const int track_h = track_bottom - track_top;
            const int total_items = static_cast<int>(candidates_.size());
            const int thumb_h = (std::max)(Scale(24), (track_h * kVerticalMaxVisible) / total_items);
            const int max_scroll = total_items - kVerticalMaxVisible;
            const int thumb_y = track_top + ((track_h - thumb_h) * scroll_offset_) / (max_scroll > 0 ? max_scroll : 1);
            const int thumb_w = Scale(scrollbar_hovered_ || scrollbar_dragging_ ? 7 : 3);
            const int thumb_x = width_ - Scale(3) - thumb_w;

            Gdiplus::RectF thumb_rf(
                static_cast<float>(thumb_x), static_cast<float>(thumb_y),
                static_cast<float>(thumb_w), static_cast<float>(thumb_h));
            Gdiplus::GraphicsPath thumb_path;
            AddRoundedRectangleToPath(thumb_path, thumb_rf, static_cast<float>(Scale(3)));
            Gdiplus::Color thumb_color = (scrollbar_hovered_ || scrollbar_dragging_)
                ? Gdiplus::Color(255, 148, 163, 184)
                : Gdiplus::Color(255, 226, 232, 240);
            Gdiplus::SolidBrush thumb_brush(thumb_color);
            g.FillPath(&thumb_brush, &thumb_path);
        }

        finish_text_frame();
        return;
    }

    // ================= 普通横向拼音候选模式 =================
    const int horizontal_padding = use_native_layout
        ? Scale(skin.candidate_margin.left)
        : Scale(kHorizontalPadding);
    const int line_height = Scale(CandidateLineHeightForTheme(
        use_native_layout, candidate_size));
    CandidateWindowVerticalLayout vertical;
    if (use_native_layout) {
        vertical.composing_top = Scale(skin.pinyin_margin.top);
        vertical.composing_bottom = vertical.composing_top + line_height;
        vertical.separator_y = vertical.composing_bottom +
            Scale(skin.pinyin_margin.bottom + skin.candidate_margin.top) / 2;
        vertical.candidate_top = vertical.composing_bottom +
            Scale(skin.pinyin_margin.bottom + skin.candidate_margin.top);
        vertical.candidate_bottom = vertical.candidate_top + line_height;
        vertical.window_height = height_;
    } else {
        vertical = BuildCandidateWindowVerticalLayout(
            Scale(kVerticalPadding), line_height, Scale(kRowGap));
    }
    const std::wstring status_text = BuildTypingStatisticsText(
        typing_stats_.daily_count);
    const bool can_expand = candidates_.size() > page_size_;

    const CandidateTextStyle metadata_style {
        L"Microsoft YaHei UI",
        static_cast<float>(CandidateMetadataFontSize(candidate_size)),
        CandidateTextWeight::Regular, CandidateTextAlignment::Near, false};
    float status_width_value = directwrite_text_.MeasureText(
        status_text, metadata_style, dpi);
    if (status_width_value <= 0.0f) {
        Gdiplus::RectF status_bbox;
        g.MeasureString(status_text.c_str(), static_cast<INT>(status_text.size()),
                        font_meta, Gdiplus::PointF(0, 0), &format, &status_bbox);
        status_width_value = status_bbox.Width;
    }
    const int status_width = static_cast<int>(std::ceil(status_width_value));

    const int header_right_width = status_width +
        (can_expand ? Scale(kExpandToggleGap + kExpandToggleWidth) : 0);
    const auto header = BuildCandidateHeaderLayout(
        width_, header_right_width,
        use_native_layout
            ? Scale(skin.pinyin_margin.left)
            : horizontal_padding + Scale(4),
        use_native_layout
            ? Scale((std::max)(skin.pinyin_margin.right,
                               skin.candidate_margin.right))
            : horizontal_padding,
        Scale(kHeaderTextGap));

    // 绘制拼音
    std::wstring comp_draw = composing_;
    if (comp_draw.empty()) {
        comp_draw = english_mode_ ? L"[EN]" : L"";
    }
    const RECT composing_rect {header.composing_left, vertical.composing_top,
                               header.composing_right, vertical.composing_bottom};
    const CandidateTextStyle composing_style {
        candidate_family, static_cast<float>(candidate_size),
        CandidateTextWeight::Regular, CandidateTextAlignment::Near, true};
    const COLORREF composing_color = plain_utility_background
        ? EnsureCandidateTextContrast(
            skin.pinyin_color, skin.utility_background_color)
        : skin.pinyin_color;
    draw_text(comp_draw, composing_rect, composing_color, composing_style,
              font_comp, &format);

    // 绘制字数统计
    const RECT status_rect {header.page_left, vertical.composing_top,
                            header.page_left + status_width,
                            vertical.composing_bottom};
    const COLORREF metadata_color = plain_utility_background
        ? EnsureCandidateTextContrast(
            skin.status_text_color, skin.utility_background_color)
        : skin.status_text_color;
    draw_text(status_text, status_rect, metadata_color, metadata_style,
              font_meta, &format);

    // 绘制展开/收起折线箭头
    expand_toggle_rect_ = {};
    if (can_expand) {
        const int toggle_left = status_rect.right + Scale(kExpandToggleGap);
        expand_toggle_rect_ = RECT {
            toggle_left, vertical.composing_top,
            toggle_left + Scale(kExpandToggleWidth),
            vertical.composing_bottom};
        const float center_x = static_cast<float>(expand_toggle_rect_.left + expand_toggle_rect_.right) / 2.0f;
        const float center_y = static_cast<float>(expand_toggle_rect_.top + expand_toggle_rect_.bottom) / 2.0f;
        const float half_width = static_cast<float>(Scale(4));
        const float half_height = static_cast<float>(Scale(2));
        Gdiplus::PointF chevron[3];
        if (expanded_) {
            chevron[0] = Gdiplus::PointF(center_x - half_width, center_y + half_height);
            chevron[1] = Gdiplus::PointF(center_x, center_y - half_height);
            chevron[2] = Gdiplus::PointF(center_x + half_width, center_y + half_height);
        } else {
            chevron[0] = Gdiplus::PointF(center_x - half_width, center_y - half_height);
            chevron[1] = Gdiplus::PointF(center_x, center_y + half_height);
            chevron[2] = Gdiplus::PointF(center_x + half_width, center_y - half_height);
        }
        Gdiplus::Pen arrow_pen(ToGdiplusColor(metadata_color), static_cast<float>((std::max)(1, Scale(1))));
        arrow_pen.SetLineJoin(Gdiplus::LineJoinMiter);
        g.DrawLines(&arrow_pen, chevron, 3);
    }

    // 分隔线
    if (skin.show_separator) {
        Gdiplus::Pen sep_pen(ToGdiplusColor(skin.separator_color), 1.0f);
        g.DrawLine(&sep_pen,
            static_cast<float>(Scale(8)), static_cast<float>(vertical.separator_y),
            static_cast<float>(width_ - Scale(8)), static_cast<float>(vertical.separator_y));
    }

    // 候选：逐项绘制，选中高亮
    const size_t first_page = expanded_
        ? CandidateExpandedFirstPage(page_, kExpandedMaxRows)
        : page_;
    for (size_t row = 0; row < item_rows_.size(); ++row) {
        const size_t row_page = first_page + row;
        const size_t begin = row_page * page_size_;
        const bool numbered = row_page == page_;
        const int row_top = vertical.candidate_top +
            static_cast<int>(row) * line_height;
        const int row_bottom = row_top + line_height;
        for (size_t slot = 0; slot < item_rows_[row].size(); ++slot) {
            const size_t i = item_rows_[row][slot].index;
            if (i >= candidates_.size()) continue;

            const bool is_selected = (i == selected_);
            const int hit_left = item_rows_[row][slot].hit_left;
            const int hit_right = item_rows_[row][slot].hit_right;
            const int text_left = item_rows_[row][slot].text_left;

            // 绘制选中高亮胶囊背景
            if (is_selected && !use_native_layout) {
                Gdiplus::RectF hl_rf(
                    static_cast<float>(hit_left + Scale(2)),
                    static_cast<float>(row_top + Scale(3)),
                    static_cast<float>(hit_right - hit_left - Scale(4)),
                    static_cast<float>(row_bottom - row_top - Scale(6)));
                Gdiplus::GraphicsPath hl_path;
                AddRoundedRectangleToPath(hl_path, hl_rf, static_cast<float>(Scale(skin.corner_radius)));
                Gdiplus::SolidBrush hl_brush(ToGdiplusColor(skin.highlight_bg_color));
                g.FillPath(&hl_brush, &hl_path);
            }

            COLORREF num_color = is_selected ? skin.highlight_color : skin.index_color;
            COLORREF txt_color = is_selected ? skin.highlight_color : skin.candidate_color;
            if (plain_utility_background) {
                const COLORREF text_background = is_selected
                    ? skin.highlight_bg_color
                    : skin.utility_background_color;
                num_color = EnsureCandidateTextContrast(
                    num_color, text_background);
                txt_color = EnsureCandidateTextContrast(
                    txt_color, text_background);
            }

            const int visible_item_right = CandidateItemTextRight(
                width_, hit_right, use_native_layout
                    ? Scale(skin.candidate_margin.right)
                    : horizontal_padding);
            const RECT candidate_pin_rect = BuildCandidatePinRect(
                item_rows_[row][slot], width_,
                use_native_layout
                    ? Scale(skin.candidate_margin.right)
                    : horizontal_padding,
                Scale(kPinReservedWidth), row_top, row_bottom);
            const int item_text_right = pinning_enabled_
                ? (std::max)(
                    text_left,
                    static_cast<int>(candidate_pin_rect.left) - Scale(2))
                : visible_item_right;
            const CandidateTextStyle candidate_style {
                candidate_family, static_cast<float>(candidate_size),
                CandidateTextWeight::Regular, CandidateTextAlignment::Near, true};
            if (numbered) {
                std::wstring num_str = std::to_wstring(i - begin + 1) + L".";
                float num_width_value = directwrite_text_.MeasureText(
                    num_str, candidate_style, dpi);
                if (num_width_value <= 0.0f) {
                    Gdiplus::RectF num_bbox;
                    g.MeasureString(num_str.c_str(), static_cast<INT>(num_str.size()),
                                    font, Gdiplus::PointF(0, 0), &format, &num_bbox);
                    num_width_value = num_bbox.Width;
                }
                const int num_width = static_cast<int>(std::ceil(num_width_value));
                const RECT num_rc {text_left, row_top,
                                   text_left + num_width, row_bottom};
                draw_text(num_str, num_rc, num_color, candidate_style,
                          font, &format);
                const RECT text_rc {text_left + num_width, row_top,
                                    item_text_right, row_bottom};
                draw_text(candidates_[i].text, text_rc, txt_color,
                          candidate_style, font, &format);
            } else {
                const RECT text_rc {text_left, row_top,
                                    item_text_right, row_bottom};
                draw_text(candidates_[i].text, text_rc, txt_color,
                          candidate_style, font, &format);
            }

            if (pinning_enabled_ &&
                (candidates_[i].pinned ||
                 static_cast<int>(i) == hovered_candidate_)) {
                const bool pin_hovered = static_cast<int>(i) == hovered_candidate_ &&
                    hovered_pin_;
                COLORREF pin_color = candidates_[i].pinned
                    ? skin.highlight_bg_color : skin.candidate_color;
                const COLORREF icon_background = is_selected && !use_native_layout
                    ? skin.highlight_bg_color : skin.utility_background_color;
                pin_color = EnsureCandidateTextContrast(
                    pin_color, icon_background, 3.0);
                DrawCandidatePinIcon(
                    g, candidate_pin_rect, pin_color,
                    candidates_[i].pinned, pin_hovered);
            }
        }
    }
    finish_text_frame();
}

bool CandidateWindow::UpdateLayeredWindowContent(const POINT& window_origin) {
    if (hwnd_ == nullptr || width_ <= 0 || height_ <= 0) return false;

    const int shadow_margin = ShadowMargin();
    const int bitmap_width = width_ + shadow_margin * 2;
    const int bitmap_height = height_ + shadow_margin * 2;
    const size_t pixel_count =
        static_cast<size_t>(bitmap_width) * static_cast<size_t>(bitmap_height);

    BITMAPINFO bitmap_info {};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = bitmap_width;
    bitmap_info.bmiHeader.biHeight = -bitmap_height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) return false;
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    void* bitmap_bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screen_dc, &bitmap_info, DIB_RGB_COLORS, &bitmap_bits, nullptr, 0);
    if (memory_dc == nullptr || bitmap == nullptr || bitmap_bits == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memory_dc != nullptr) DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    std::memset(bitmap_bits, 0, pixel_count * 4);
    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);

    // 在 32 位 PARGB DIB 上创建 Gdiplus Graphics 完成高保真绘制
    {
        Gdiplus::Bitmap surface_bitmap(
            bitmap_width, bitmap_height, bitmap_width * 4,
            PixelFormat32bppPARGB, static_cast<BYTE*>(bitmap_bits));
        Gdiplus::Graphics graphics(&surface_bitmap);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        graphics.TranslateTransform(
            static_cast<Gdiplus::REAL>(shadow_margin),
            static_cast<Gdiplus::REAL>(shadow_margin));

        DrawContent(
            &graphics, static_cast<uint8_t*>(bitmap_bits),
            bitmap_width, bitmap_height, shadow_margin);
    }

    auto* pixels = static_cast<uint8_t*>(bitmap_bits);
    SkinManager::Instance().EnsureSkin(GetRuntimeConfig().skin_id);
    const auto& skin = SkinManager::Instance().CurrentTheme();

    const bool has_shadow = vertical_utility_mode_ || skin.has_shadow;
    const bool plain_utility_background = UsesPlainUtilityBackground();
    if (has_shadow && shadow_margin > 0) {
        std::vector<uint8_t> surface_alpha(pixel_count, 0);
        if (skin.has_bg_image && !plain_utility_background) {
            for (size_t index = 0; index < pixel_count; ++index) {
                surface_alpha[index] = pixels[index * 4 + 3];
            }
        } else {
            const int corner_radius = vertical_utility_mode_
                ? 8 : skin.corner_radius;
            surface_alpha = BuildRoundedCardMask(
                bitmap_width, bitmap_height, shadow_margin, shadow_margin,
                width_, height_, Scale(corner_radius));
        }
        CompositeCandidateSurfaceAndShadow(
            pixels, surface_alpha, bitmap_width, bitmap_height,
            Scale(2), (std::max)(1, Scale(4)),
            kShadowBlurPasses, kShadowOpacity);
    }

    POINT destination_origin = window_origin;
    SIZE window_size {bitmap_width, bitmap_height};
    POINT source_origin {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL updated = UpdateLayeredWindow(
        hwnd_, screen_dc, &destination_origin, &window_size, memory_dc,
        &source_origin, 0, &blend, ULW_ALPHA);

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    if (updated) paint_dirty_ = false;
    return updated != FALSE;
}

LRESULT CandidateWindow::OnPaint() {
    PAINTSTRUCT ps {};
    BeginPaint(hwnd_, &ps);
    EndPaint(hwnd_, &ps);

    RECT window_rect {};
    if (GetWindowRect(hwnd_, &window_rect)) {
        UpdateLayeredWindowContent(POINT {window_rect.left, window_rect.top});
    }

    return 0;
}

LRESULT CALLBACK CandidateWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    CandidateWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<CandidateWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    const UINT runtime_settings_changed = RuntimeSettingsChangedMessage();
    if (runtime_settings_changed != 0 && msg == runtime_settings_changed) {
        self->ReloadRuntimeSettings();
        return 0;
    }

    switch (msg) {
    case WM_PAINT:
        return self->OnPaint();
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED: {
        // 字体和布局尺寸依赖窗口所在显示器的 DPI，跨屏移动后立即重建。
        self->ResetFonts();
        self->layout_dirty_ = true;
        self->paint_dirty_ = true;
        self->RecalcSize();
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        self->UpdateLayeredWindowContent(POINT {suggested->left, suggested->top});
        return 0;
    }
    case WM_NCHITTEST: {
        RECT window_rect {};
        if (!GetWindowRect(hwnd, &window_rect)) break;
        const int x = static_cast<short>(LOWORD(lparam)) - window_rect.left;
        const int y = static_cast<short>(HIWORD(lparam)) - window_rect.top;
        const int shadow_margin = self->ShadowMargin();
        if (x < shadow_margin || x >= shadow_margin + self->width_ ||
            y < shadow_margin || y >= shadow_margin + self->height_) {
            return HTTRANSPARENT;
        }
        return HTCLIENT;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (wparam == kSkinAnimationTimerId) {
            self->skin_animation_timer_active_ = false;
            auto& manager = SkinManager::Instance();
            if (self->visible_ && manager.AdvanceFrame()) {
                RECT window_rect {};
                self->paint_dirty_ = true;
                if (GetWindowRect(hwnd, &window_rect)) {
                    self->UpdateLayeredWindowContent(
                        POINT {window_rect.left, window_rect.top});
                }
                self->SyncSkinAnimation();
            }
            return 0;
        }
        if (wparam == kVModeTimerId) {
            KillTimer(hwnd, kVModeTimerId);
            self->vmode_timer_active_ = false;
            auto cb = std::move(self->vmode_timer_cb_);
            self->vmode_timer_cb_ = nullptr;
            if (cb) cb();
            return 0;
        }
        if (wparam == self->deferred_timer_id_) {
            if (!self->deferred_action_active_) return 0;
            KillTimer(hwnd, self->deferred_timer_id_);
            self->deferred_action_active_ = false;
            auto action = std::move(self->deferred_action_);
            self->deferred_action_ = nullptr;
            if (action) action();
            return 0;
        }
        if (wparam == kShiftReleasePollTimerId) {
            const std::function<bool()> poll = self->shift_release_poll_;
            if (!poll || !poll()) {
                self->StopShiftReleasePolling();
            }
            return 0;
        }
        if (wparam == kReadyPollTimerId) {
            // 先拷贝再调用：回调内部可能触发 Stop/StartReadyPolling 替换
            // ready_poll_，直接调用成员会销毁正在执行的 lambda。
            const std::function<bool()> poll = self->ready_poll_;
            if (!poll || !poll()) {
                self->StopReadyPolling();
            }
            return 0;
        }
        break;
    case WM_MOUSEWHEEL: {
        if (self->vertical_utility_mode_ &&
            self->candidates_.size() > static_cast<size_t>(kVerticalMaxVisible)) {
            const short delta = static_cast<short>(HIWORD(wparam));
            const int step = (delta > 0 ? -3 : 3);
            const int max_scroll = static_cast<int>(self->candidates_.size()) - kVerticalMaxVisible;
            self->scroll_offset_ = (std::max)(0, (std::min)(max_scroll, self->scroll_offset_ + step));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        const int shadow_margin = self->ShadowMargin();
        const int x = static_cast<short>(LOWORD(lparam)) - shadow_margin;
        const int y = static_cast<short>(HIWORD(lparam)) - shadow_margin;

        if (!self->vertical_utility_mode_ && self->expand_toggle_rect_.right > 0 &&
            x >= self->expand_toggle_rect_.left &&
            x < self->expand_toggle_rect_.right &&
            y >= self->expand_toggle_rect_.top &&
            y < self->expand_toggle_rect_.bottom) {
            self->ToggleExpanded();
            return 0;
        }

        const int pin_candidate = self->HitTestPin(
            static_cast<short>(LOWORD(lparam)),
            static_cast<short>(HIWORD(lparam)));
        if (pin_candidate >= 0) {
            self->pressed_pin_candidate_ = pin_candidate;
            SetCapture(hwnd);
            return 0;
        }

        if (self->vertical_utility_mode_) {
            // 1. 检查是否点击搜索框清空按钮
            if (self->search_clear_rect_.right > 0 &&
                x >= self->search_clear_rect_.left && x <= self->search_clear_rect_.right &&
                y >= self->search_clear_rect_.top && y <= self->search_clear_rect_.bottom) {
                if (self->on_search_cleared_) self->on_search_cleared_();
                return 0;
            }

            // 2. 检查是否点击搜索框
            if (x >= self->search_box_rect_.left && x <= self->search_box_rect_.right &&
                y >= self->search_box_rect_.top && y <= self->search_box_rect_.bottom) {
                if (self->on_search_clicked_) self->on_search_clicked_();
                return 0;
            }

            // 3. 检查是否点击滚动条区域
            if (x >= self->width_ - self->Scale(12) && self->candidates_.size() > static_cast<size_t>(kVerticalMaxVisible)) {
                self->scrollbar_dragging_ = true;
                self->drag_start_y_ = y;
                self->drag_start_scroll_ = self->scroll_offset_;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            // 4. 检查是否点击垃圾桶
            if (self->hovered_delete_ && self->hovered_row_ >= 0) {
                if (self->on_delete_item_) {
                    self->on_delete_item_(static_cast<size_t>(self->hovered_row_));
                }
                return 0;
            }
        }

        // 按下即捕获；位移超过系统拖动阈值判定为拖动，否则抬起时按点击选词。
        self->mouse_down_ = true;
        self->dragging_ = false;
        GetCursorPos(&self->drag_start_cursor_);
        RECT wr {};
        if (GetWindowRect(hwnd, &wr)) {
            self->drag_start_window_ = POINT {wr.left, wr.top};
        }
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int shadow_margin = self->ShadowMargin();
        const int x = static_cast<short>(LOWORD(lparam)) - shadow_margin;
        const int y = static_cast<short>(HIWORD(lparam)) - shadow_margin;

        TRACKMOUSEEVENT tracking {
            sizeof(tracking), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tracking);

        if (self->vertical_utility_mode_) {
            bool need_redraw = false;

            // 滚动条拖拽处理
            if (self->scrollbar_dragging_) {
                const int top_bar_h = self->Scale(38);
                const int track_h = self->height_ - top_bar_h - self->Scale(12);
                const int total_items = static_cast<int>(self->candidates_.size());
                const int max_scroll = total_items - kVerticalMaxVisible;
                if (track_h > 0 && max_scroll > 0) {
                    const int dy = y - self->drag_start_y_;
                    const int scroll_delta = (dy * max_scroll) / track_h;
                    const int new_scroll = (std::max)(0, (std::min)(max_scroll, self->drag_start_scroll_ + scroll_delta));
                    if (new_scroll != self->scroll_offset_) {
                        self->scroll_offset_ = new_scroll;
                        need_redraw = true;
                    }
                }
            } else {
                const bool scroll_hover = (x >= self->width_ - self->Scale(12) && x <= self->width_);
                if (scroll_hover != self->scrollbar_hovered_) {
                    self->scrollbar_hovered_ = scroll_hover;
                    need_redraw = true;
                }

                const int top_bar_h = self->Scale(38);
                const int row_h = self->Scale(kVerticalRowHeight);
                int row = -1;
                bool del_hover = false;
                if (y >= top_bar_h && y < self->height_ && x >= 0 && x < self->width_ - self->Scale(12)) {
                    const int local_row = (y - top_bar_h) / row_h;
                    const int idx = self->scroll_offset_ + local_row;
                    if (idx >= 0 && static_cast<size_t>(idx) < self->candidates_.size()) {
                        row = idx;
                        if (x >= self->width_ - self->Scale(38)) {
                            del_hover = true;
                        }
                    }
                }

                if (row != self->hovered_row_ || del_hover != self->hovered_delete_) {
                    self->hovered_row_ = row;
                    self->hovered_delete_ = del_hover;
                    need_redraw = true;
                }
            }

            if (need_redraw) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else {
            const int hovered_candidate = self->HitTestCandidate(
                static_cast<short>(LOWORD(lparam)),
                static_cast<short>(HIWORD(lparam)));
            const bool hovered_pin = self->HitTestPin(
                static_cast<short>(LOWORD(lparam)),
                static_cast<short>(HIWORD(lparam))) >= 0;
            if (hovered_candidate != self->hovered_candidate_ ||
                hovered_pin != self->hovered_pin_) {
                self->hovered_candidate_ = hovered_candidate;
                self->hovered_pin_ = hovered_pin;
                self->paint_dirty_ = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }

        if (!self->mouse_down_) {
            break;
        }
        POINT cursor {};
        GetCursorPos(&cursor);
        const int dx = cursor.x - self->drag_start_cursor_.x;
        const int dy = cursor.y - self->drag_start_cursor_.y;
        if (!self->dragging_) {
            if (std::abs(dx) < GetSystemMetrics(SM_CXDRAG) &&
                std::abs(dy) < GetSystemMetrics(SM_CYDRAG)) {
                break;
            }
            self->dragging_ = true;
        }
        SetWindowPos(hwnd, nullptr,
                     self->drag_start_window_.x + dx,
                     self->drag_start_window_.y + dy,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self->hovered_row_ != -1 || self->hovered_delete_ ||
            self->hovered_candidate_ != -1 || self->hovered_pin_ ||
            self->scrollbar_hovered_) {
            self->hovered_row_ = -1;
            self->hovered_delete_ = false;
            self->hovered_candidate_ = -1;
            self->hovered_pin_ = false;
            self->scrollbar_hovered_ = false;
            self->paint_dirty_ = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        self->mouse_down_ = false;
        self->dragging_ = false;
        self->scrollbar_dragging_ = false;
        self->pressed_pin_candidate_ = -1;
        break;
    case WM_LBUTTONUP: {
        if (self->pressed_pin_candidate_ >= 0) {
            const int pressed = self->pressed_pin_candidate_;
            self->pressed_pin_candidate_ = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            const int released = self->HitTestPin(
                static_cast<short>(LOWORD(lparam)),
                static_cast<short>(HIWORD(lparam)));
            if (released == pressed && self->on_pin_) {
                self->on_pin_(static_cast<size_t>(pressed));
            }
            return 0;
        }
        if (self->scrollbar_dragging_) {
            self->scrollbar_dragging_ = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        const bool was_dragging = self->dragging_;
        const bool was_down = self->mouse_down_;
        self->mouse_down_ = false;
        self->dragging_ = false;
        if (was_down && GetCapture() == hwnd) {
            ReleaseCapture();
        }
        if (was_dragging) {
            RECT wr {};
            if (GetWindowRect(hwnd, &wr) && self->on_drag_) {
                const int shadow_margin = self->ShadowMargin();
                self->on_drag_(POINT {
                    wr.left + shadow_margin, wr.top + shadow_margin});
            }
            return 0;
        }
        const int index = self->HitTestCandidate(
            static_cast<short>(LOWORD(lparam)),
            static_cast<short>(HIWORD(lparam)));
        if (index >= 0) {
            self->selected_ = static_cast<size_t>(index);
            self->SetSelectedIndex(self->selected_);
            if (self->on_select_) {
                self->on_select_(self->selected_);
            }
        }
        return 0;
    }
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        self->OpenSettings();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace shuru
