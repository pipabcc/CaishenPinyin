#include "candidate_window.h"
#include "common/runtime_config.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace shuru {
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
constexpr uint8_t kShadowOpacity = 48;

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

std::wstring BuildPageText(size_t candidate_count, size_t page, size_t page_size) {
    const size_t normalized_page_size = (std::max)(size_t {1}, page_size);
    if (candidate_count <= normalized_page_size) return {};
    const size_t pages = (candidate_count + normalized_page_size - 1) /
        normalized_page_size;
    return L"‹  " + std::to_wstring((std::min)(page, pages - 1) + 1) +
        L"/" + std::to_wstring(pages) + L"  ›";
}

}  // namespace

CandidateWindow::~CandidateWindow() {
    Destroy();
}

bool CandidateWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    if (hwnd_ != nullptr) {
        return true;
    }

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
    if (font_utility_ != nullptr) {
        DeleteObject(font_utility_);
        font_utility_ = nullptr;
    }
    visible_ = false;
    instance_ = nullptr;
}

void CandidateWindow::EnsureFonts() {
    if (font_ == nullptr) {
        font_ = CreateFontW(
            -Scale(GetRuntimeConfig().candidate_font_size), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
    }
    if (font_comp_ == nullptr) {
        font_comp_ = CreateFontW(
            -Scale(GetRuntimeConfig().candidate_font_size + 1), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
    }
    if (font_meta_ == nullptr) {
        font_meta_ = CreateFontW(
            -Scale(CandidateMetadataFontSize(GetRuntimeConfig().candidate_font_size)),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
    }
    if (font_utility_ == nullptr) {
        font_utility_ = CreateFontW(
            -Scale(kUtilityFontSize), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
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

SIZE CandidateWindow::WindowSize() const {
    return SIZE {width_, height_};
}

POINT CandidateWindow::ScreenPosition() const {
    RECT rect {};
    if (hwnd_ != nullptr && GetWindowRect(hwnd_, &rect)) {
        const int shadow_margin = Scale(kShadowMargin);
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
    POINT pos = screen_pos;
    HMONITOR monitor = MonitorFromPoint(pos, MONITOR_DEFAULTTONEAREST);
    if (!visible_) RefreshTypingStats();
    if (layout_dirty_) RecalcSize();
    const int shadow_margin = Scale(kShadowMargin);
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
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                     (needs_show ? SWP_SHOWWINDOW : 0));
    visible_ = true;
    if (needs_show) {
        // 像素和最终尺寸已经由 UpdateLayeredWindow 原子提交，此处只切换可见性。
        UpdateWindow(hwnd_);
    }
}

void CandidateWindow::Hide() {
    if (hwnd_ == nullptr) {
        return;
    }
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
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
            candidates_[i].action_data != candidates[i].action_data) {
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
    size_t page_size) {
    const size_t normalized_page_size = (std::max)(size_t{1}, page_size);
    const size_t page_count = candidates.empty() ? 1 :
        (candidates.size() + normalized_page_size - 1) / normalized_page_size;
    size_t normalized_selected = selected_index < candidates.size() ? selected_index : 0;
    size_t normalized_page = (std::min)(page, page_count - 1);
    if (!candidates.empty()) normalized_page = normalized_selected / normalized_page_size;

    const bool layout_changed = !DisplayContentEquals(
        composing, candidates, normalized_page, normalized_page_size);
    const bool selection_changed = selected_ != normalized_selected;
    if (!layout_changed && !selection_changed) return;

    composing_ = composing;
    candidates_ = candidates;
    selected_ = normalized_selected;
    page_size_ = normalized_page_size;
    page_ = normalized_page;

    const bool is_v_mode = (!composing.empty() && (composing[0] == L'v' || composing[0] == L'V') && composing.rfind(L"vvv", 0) != 0);
    if (is_v_mode) {
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
    const bool is_v_mode = !composing_.empty() &&
        (composing_[0] == L'v' || composing_[0] == L'V') &&
        composing_.rfind(L"vvv", 0) != 0;
    if (is_v_mode) {
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

void CandidateWindow::OpenSettings() {
    if (!LaunchSettingsExecutable(hwnd_, instance_)) {
        MessageBoxW(hwnd_, L"无法找到或启动 ShuruSettings.exe。请重新安装设置程序。",
                    L"财神输入法", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
}

void CandidateWindow::RecalcSize() {
    HDC hdc = GetDC(nullptr);
    EnsureFonts();

    if (hdc == nullptr || font_ == nullptr || font_comp_ == nullptr ||
        font_meta_ == nullptr || font_utility_ == nullptr) {
        if (hdc != nullptr) {
            ReleaseDC(nullptr, hdc);
        }
        width_ = Scale(kMinWidth);
        height_ = Scale(kVerticalPadding * 2 + kLineHeight * 2 + kRowGap);
        layout_dirty_ = false;
        return;
    }

    const bool is_v_mode = (!composing_.empty() && (composing_[0] == L'v' || composing_[0] == L'V') && composing_.rfind(L"vvv", 0) != 0);

    if (is_v_mode) {
        width_ = Scale(350);
        const int top_bar_h = Scale(38);
        const int row_h = Scale(kVerticalRowHeight);
        const size_t visible_count = candidates_.empty() ? 1 : (std::min)(candidates_.size(), static_cast<size_t>(kVerticalMaxVisible));
        height_ = top_bar_h + static_cast<int>(visible_count) * row_h + Scale(10);
        layout_dirty_ = false;
        ReleaseDC(nullptr, hdc);
        return;
    }

    const int comp_w = MeasureText(
        hdc, font_comp_, composing_.empty() ? L" " : composing_);
    const std::wstring page_text = BuildPageText(
        candidates_.size(), page_, page_size_);
    const std::wstring status_text = BuildTypingStatisticsText(
        typing_stats_.daily_count);
    const std::wstring header_right_text = page_text.empty()
        ? status_text
        : status_text + L"   " + page_text;
    const int page_w = MeasureText(hdc, font_meta_, header_right_text);
    int cand_w = 0;
    const size_t begin = page_ * page_size_;
    const size_t end = (std::min)(candidates_.size(), begin + page_size_);
    std::vector<int> item_widths;
    item_widths.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        std::wstring item = std::to_wstring(i - begin + 1) + L"." + candidates_[i].text;
        item_widths.push_back(MeasureText(hdc, font_, item));
    }
    const int padding = Scale(kHorizontalPadding);
    item_layout_ = BuildCandidateRowLayout(
        item_widths, begin, padding + Scale(4), Scale(4), Scale(8), Scale(16));
    cand_w = CandidateRowRequiredWidth(item_layout_, padding);
    ReleaseDC(nullptr, hdc);

    const int header_w = CandidateHeaderRequiredWidth(
        comp_w, page_w, padding + Scale(4), padding, Scale(kHeaderTextGap));
    width_ = (std::max)(Scale(kMinWidth),
        (std::min)(Scale(kMaxWidth), (std::max)(header_w, cand_w)));
    const auto vertical = BuildCandidateWindowVerticalLayout(
        Scale(kVerticalPadding), Scale(kLineHeight), Scale(kRowGap));
    height_ = vertical.window_height;
    layout_dirty_ = false;
}

int CandidateWindow::HitTestCandidate(int x, int y) const {
    const int shadow_margin = Scale(kShadowMargin);
    x -= shadow_margin;
    y -= shadow_margin;

    const bool is_v_mode = (!composing_.empty() && (composing_[0] == L'v' || composing_[0] == L'V') && composing_.rfind(L"vvv", 0) != 0);
    if (is_v_mode) {
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

    const auto vertical = BuildCandidateWindowVerticalLayout(
        Scale(kVerticalPadding), Scale(kLineHeight), Scale(kRowGap));
    if (y < vertical.candidate_top || y >= vertical.candidate_bottom) {
        return -1;
    }

    int hit = -1;
    for (const auto& item : item_layout_) {
        if (x >= item.hit_left && x < item.hit_right) {
            hit = static_cast<int>(item.index);
            break;
        }
    }
    return hit;
}

void CandidateWindow::DrawContent(
    HDC hdc, uint8_t* pixels, int bitmap_width, int bitmap_height,
    int content_offset) {
    RECT rc {0, 0, width_, height_};
    HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    EnsureFonts();
    if (font_ == nullptr || font_comp_ == nullptr || font_meta_ == nullptr ||
        font_utility_ == nullptr) return;
    SetBkMode(hdc, TRANSPARENT);

    const bool is_v_mode = (!composing_.empty() && (composing_[0] == L'v' || composing_[0] == L'V') && composing_.rfind(L"vvv", 0) != 0);

    if (is_v_mode) {
        // ================= 竖向瀑布流绘制 =================
        const int top_bar_h = Scale(38);
        const int row_h = Scale(kVerticalRowHeight);
        const int padding = Scale(12);

        // 1. 顶部标题
        SelectObject(hdc, font_comp_);
        SetTextColor(hdc, RGB(15, 23, 42));
        std::wstring title = composing_.rfind(L"vv", 0) == 0 ? L"自定义短语" : L"复制记录";
        RECT title_rc {padding, Scale(4), padding + Scale(120), top_bar_h};
        DrawTextW(hdc, title.c_str(), -1, &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 2. 右侧可点击搜索框胶囊
        std::wstring search_query;
        if (composing_.rfind(L"vv", 0) == 0 && composing_.size() > 2) search_query = composing_.substr(2);
        else if (composing_.rfind(L"v", 0) == 0 && composing_.size() > 1 && composing_[1] != L'v') search_query = composing_.substr(1);

        const int box_w = Scale(145);
        search_box_rect_ = RECT {width_ - padding - box_w, Scale(6), width_ - padding, top_bar_h - Scale(6)};

        HPEN cap_pen = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
        HBRUSH cap_brush = CreateSolidBrush(RGB(248, 250, 252));
        HGDIOBJ old_pen = SelectObject(hdc, cap_pen);
        HGDIOBJ old_brush = SelectObject(hdc, cap_brush);
        RoundRect(hdc, search_box_rect_.left, search_box_rect_.top, search_box_rect_.right, search_box_rect_.bottom, Scale(6), Scale(6));
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(cap_brush);
        DeleteObject(cap_pen);

        SelectObject(hdc, font_meta_);
        std::wstring search_text = search_query.empty() ? L"🔍 搜索..." : (L"🔍 " + search_query);
        SetTextColor(hdc, search_query.empty() ? RGB(148, 163, 184) : RGB(30, 41, 59));
        RECT text_rc = search_box_rect_;
        text_rc.left += Scale(8);
        text_rc.right -= search_query.empty() ? Scale(6) : Scale(20);
        DrawTextW(hdc, search_text.c_str(), -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (!search_query.empty()) {
            search_clear_rect_ = RECT {search_box_rect_.right - Scale(20), search_box_rect_.top, search_box_rect_.right - Scale(2), search_box_rect_.bottom};
            SetTextColor(hdc, RGB(148, 163, 184));
            DrawTextW(hdc, L"✕", -1, &search_clear_rect_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            search_clear_rect_ = {};
        }

        // 3. 分隔线
        HPEN sep = CreatePen(PS_SOLID, 1, RGB(241, 245, 249));
        old_pen = SelectObject(hdc, sep);
        MoveToEx(hdc, Scale(6), top_bar_h, nullptr);
        LineTo(hdc, width_ - Scale(6), top_bar_h);
        SelectObject(hdc, old_pen);
        DeleteObject(sep);

        // 4. 竖向候选列表
        SelectObject(hdc, font_utility_);
        if (candidates_.empty()) {
            SetTextColor(hdc, RGB(148, 163, 184));
            RECT empty_rc {padding, top_bar_h + Scale(16), width_ - padding, top_bar_h + Scale(50)};
            DrawTextW(hdc, L"暂无记录", -1, &empty_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
                    GdiFlush();
                    const std::vector<uint8_t> row_mask =
                        BuildRoundedCardMask(
                            bitmap_width, bitmap_height,
                            content_offset + row_rc.left,
                            content_offset + row_rc.top + Scale(1),
                            row_rc.right - row_rc.left,
                            row_rc.bottom - row_rc.top - Scale(2),
                            Scale(6));
                    BlendSolidColor(
                        pixels, row_mask, bitmap_width, bitmap_height,
                        is_selected
                            ? RGB(238, 242, 255)
                            : RGB(248, 250, 252));
                }

                // 绘制圆点 •
                SetTextColor(hdc, is_selected ? RGB(79, 70, 229) : RGB(100, 116, 139));
                RECT dot_rc {Scale(14), row_y, Scale(26), row_y + row_h};
                DrawTextW(hdc, L"•", -1, &dot_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // 绘制文本
                SetTextColor(hdc, is_selected ? RGB(30, 27, 75) : RGB(30, 41, 59));
                RECT item_text_rc {Scale(26), row_y, row_rc.right - (is_hovered ? Scale(30) : Scale(8)), row_y + row_h};
                DrawTextW(hdc, candidates_[i].text.c_str(), -1, &item_text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                // 悬停显示垃圾桶 🗑
                if (is_hovered) {
                    SelectObject(hdc, font_meta_);
                    SetTextColor(hdc, hovered_delete_ ? RGB(239, 68, 68) : RGB(148, 163, 184));
                    RECT del_rc {row_rc.right - Scale(26), row_y, row_rc.right - Scale(4), row_y + row_h};
                    DrawTextW(hdc, L"🗑", -1, &del_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, font_utility_);
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

            RECT thumb_rc {thumb_x, thumb_y, thumb_x + thumb_w, thumb_y + thumb_h};
            COLORREF thumb_color = (scrollbar_hovered_ || scrollbar_dragging_) ? RGB(148, 163, 184) : RGB(226, 232, 240);
            HBRUSH thumb_brush = CreateSolidBrush(thumb_color);
            HPEN thumb_pen = CreatePen(PS_SOLID, 1, thumb_color);
            old_pen = SelectObject(hdc, thumb_pen);
            old_brush = SelectObject(hdc, thumb_brush);
            RoundRect(hdc, thumb_rc.left, thumb_rc.top, thumb_rc.right, thumb_rc.bottom, Scale(3), Scale(3));
            SelectObject(hdc, old_brush);
            SelectObject(hdc, old_pen);
            DeleteObject(thumb_brush);
            DeleteObject(thumb_pen);
        }

        return;
    }

    // ================= 普通横向拼音候选模式 =================
    const int horizontal_padding = Scale(kHorizontalPadding);
    const int line_height = Scale(kLineHeight);
    const auto vertical = BuildCandidateWindowVerticalLayout(
        Scale(kVerticalPadding), line_height, Scale(kRowGap));
    const std::wstring page_text = BuildPageText(
        candidates_.size(), page_, page_size_);
    const std::wstring status_text = BuildTypingStatisticsText(
        typing_stats_.daily_count);
    const std::wstring header_right_text = page_text.empty()
        ? status_text
        : status_text + L"   " + page_text;
    const int page_width = MeasureText(hdc, font_meta_, header_right_text);
    const auto header = BuildCandidateHeaderLayout(
        rc.right, page_width, horizontal_padding + Scale(4), horizontal_padding,
        Scale(kHeaderTextGap));
    RECT composing_rect {header.composing_left, vertical.composing_top,
                         header.composing_right, vertical.composing_bottom};
    HGDIOBJ old_font = SelectObject(hdc, font_comp_);
    SetTextColor(hdc, RGB(15, 23, 42));
    std::wstring comp_draw = composing_;
    if (comp_draw.empty()) {
        comp_draw = english_mode_ ? L"[EN]" : L"";
    }
    DrawTextW(hdc, comp_draw.c_str(), -1, &composing_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 右侧内容（普通打字显示字数与页码）
    SelectObject(hdc, font_meta_);
    SetTextColor(hdc, RGB(90, 100, 115));
    RECT page_rect {header.page_left, vertical.composing_top,
                    header.page_right, vertical.composing_bottom};
    DrawTextW(hdc, header_right_text.c_str(), -1, &page_rect,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // 分隔线
    HPEN sep = CreatePen(PS_SOLID, 1, RGB(230, 233, 238));
    HGDIOBJ old_sep = SelectObject(hdc, sep);
    MoveToEx(hdc, Scale(8), vertical.separator_y, nullptr);
    LineTo(hdc, rc.right - Scale(8), vertical.separator_y);
    SelectObject(hdc, old_sep);
    DeleteObject(sep);

    // 候选：逐项绘制，选中高亮
    SelectObject(hdc, font_);
    const int y2 = vertical.candidate_top;
    const size_t begin = page_ * page_size_;
    const size_t end = (std::min)(candidates_.size(), begin + page_size_);
    for (size_t slot = 0; slot < item_layout_.size() && begin + slot < end; ++slot) {
        const size_t i = begin + slot;
        wchar_t prefix[16];
        swprintf_s(prefix, L"%u.", static_cast<unsigned>(i - begin + 1));
        std::wstring item = prefix;
        item += candidates_[i].text;

        RECT item_rc {item_layout_[slot].hit_left, y2,
                      item_layout_[slot].hit_right, vertical.candidate_bottom};

        if (i == selected_) {
            RECT hl {item_rc.left + Scale(2), item_rc.top + Scale(3),
                     item_rc.right - Scale(2), item_rc.bottom - Scale(3)};
            GdiFlush();
            const std::vector<uint8_t> highlight_mask = BuildRoundedCardMask(
                bitmap_width, bitmap_height,
                content_offset + hl.left, content_offset + hl.top,
                hl.right - hl.left, hl.bottom - hl.top,
                Scale(kCornerRadius));
            BlendSolidColor(
                pixels, highlight_mask, bitmap_width, bitmap_height,
                RGB(22, 119, 255));
            SetTextColor(hdc, RGB(255, 255, 255));
        } else {
            SetTextColor(hdc, RGB(30, 30, 30));
        }

        RECT text_rc_item {item_layout_[slot].text_left, y2,
                           item_layout_[slot].hit_right, vertical.candidate_bottom};
        DrawTextW(hdc, item.c_str(), -1, &text_rc_item,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, old_font);
}

bool CandidateWindow::UpdateLayeredWindowContent(const POINT& window_origin) {
    if (hwnd_ == nullptr || width_ <= 0 || height_ <= 0) return false;

    const int shadow_margin = Scale(kShadowMargin);
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
    const int saved_dc = SaveDC(memory_dc);
    SetViewportOrgEx(memory_dc, shadow_margin, shadow_margin, nullptr);
    DrawContent(
        memory_dc, static_cast<uint8_t*>(bitmap_bits),
        bitmap_width, bitmap_height, shadow_margin);
    if (saved_dc != 0) RestoreDC(memory_dc, saved_dc);
    GdiFlush();

    const std::vector<uint8_t> card_mask = BuildRoundedCardMask(
        bitmap_width, bitmap_height, shadow_margin, shadow_margin,
        width_, height_, Scale(kCornerRadius));
    std::vector<uint8_t> shadow_mask(pixel_count, 0);
    const int shadow_offset = Scale(2);
    for (int y = 0; y < bitmap_height - shadow_offset; ++y) {
        const size_t source_row = static_cast<size_t>(y) * bitmap_width;
        const size_t destination_row =
            static_cast<size_t>(y + shadow_offset) * bitmap_width;
        for (int x = 0; x < bitmap_width; ++x) {
            shadow_mask[destination_row + x] = card_mask[source_row + x];
        }
    }
    BlurMask(&shadow_mask, bitmap_width, bitmap_height,
             (std::max)(1, Scale(4)), kShadowBlurPasses);

    auto* pixels = static_cast<uint8_t*>(bitmap_bits);
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint32_t card_alpha = card_mask[i];
        const uint32_t shadow_alpha =
            (static_cast<uint32_t>(shadow_mask[i]) * kShadowOpacity + 127) / 255;
        const uint32_t final_alpha = card_alpha +
            ((255 - card_alpha) * shadow_alpha + 127) / 255;
        uint8_t* pixel = pixels + i * 4;
        pixel[0] = static_cast<uint8_t>((pixel[0] * card_alpha + 127) / 255);
        pixel[1] = static_cast<uint8_t>((pixel[1] * card_alpha + 127) / 255);
        pixel[2] = static_cast<uint8_t>((pixel[2] * card_alpha + 127) / 255);
        pixel[3] = static_cast<uint8_t>(final_alpha);
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

    switch (msg) {
    case WM_PAINT:
        return self->OnPaint();
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED: {
        // 字体和布局尺寸依赖窗口所在显示器的 DPI，跨屏移动后立即重建。
        if (self->font_ != nullptr) {
            DeleteObject(self->font_);
            self->font_ = nullptr;
        }
        if (self->font_comp_ != nullptr) {
            DeleteObject(self->font_comp_);
            self->font_comp_ = nullptr;
        }
        if (self->font_meta_ != nullptr) {
            DeleteObject(self->font_meta_);
            self->font_meta_ = nullptr;
        }
        if (self->font_utility_ != nullptr) {
            DeleteObject(self->font_utility_);
            self->font_utility_ = nullptr;
        }
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
        const int shadow_margin = self->Scale(kShadowMargin);
        if (x < shadow_margin || x >= shadow_margin + self->width_ ||
            y < shadow_margin || y >= shadow_margin + self->height_) {
            return HTTRANSPARENT;
        }
        return HTCLIENT;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
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
        const bool is_v_mode = (!self->composing_.empty() && (self->composing_[0] == L'v' || self->composing_[0] == L'V') && self->composing_.rfind(L"vvv", 0) != 0);
        if (is_v_mode && self->candidates_.size() > static_cast<size_t>(kVerticalMaxVisible)) {
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
        const int shadow_margin = self->Scale(kShadowMargin);
        const int x = static_cast<short>(LOWORD(lparam)) - shadow_margin;
        const int y = static_cast<short>(HIWORD(lparam)) - shadow_margin;
        const bool is_v_mode = (!self->composing_.empty() && (self->composing_[0] == L'v' || self->composing_[0] == L'V') && self->composing_.rfind(L"vvv", 0) != 0);

        if (is_v_mode) {
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
        const int shadow_margin = self->Scale(kShadowMargin);
        const int x = static_cast<short>(LOWORD(lparam)) - shadow_margin;
        const int y = static_cast<short>(HIWORD(lparam)) - shadow_margin;
        const bool is_v_mode = (!self->composing_.empty() && (self->composing_[0] == L'v' || self->composing_[0] == L'V') && self->composing_.rfind(L"vvv", 0) != 0);

        if (is_v_mode) {
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
    case WM_CAPTURECHANGED:
        self->mouse_down_ = false;
        self->dragging_ = false;
        self->scrollbar_dragging_ = false;
        break;
    case WM_LBUTTONUP: {
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
                const int shadow_margin = self->Scale(kShadowMargin);
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
