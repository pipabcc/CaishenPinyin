#include "candidate_window.h"
#include "common/runtime_config.h"

#include <algorithm>
#include <cstdio>

namespace shuru {
namespace {

const wchar_t kWindowClass[] = L"ShuruCandidateWindowClass";

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
    if (hwnd_ != nullptr) {
        return true;
    }

    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kWindowClass,
        L"",
        WS_POPUP,
        0,
        0,
        width_,
        height_,
        nullptr,
        nullptr,
        instance,
        this);

    if (hwnd_ != nullptr) ApplyRoundedRegion();

    return hwnd_ != nullptr;
}

void CandidateWindow::Destroy() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (font_comp_ != nullptr) {
        DeleteObject(font_comp_);
        font_comp_ = nullptr;
    }
    visible_ = false;
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

void CandidateWindow::ApplyRoundedRegion() {
    if (hwnd_ == nullptr || width_ <= 0 || height_ <= 0) return;
    const int diameter = Scale(kCornerRadius * 2);
    HRGN region = CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1, diameter, diameter);
    if (region == nullptr) return;
    // 成功后区域句柄的所有权转移给系统；不立即触发背景擦除，统一由双缓冲重绘。
    if (SetWindowRgn(hwnd_, region, FALSE) == 0) DeleteObject(region);
}

SIZE CandidateWindow::WindowSize() const {
    return SIZE {width_, height_};
}

POINT CandidateWindow::ScreenPosition() const {
    RECT rect {};
    if (hwnd_ != nullptr && GetWindowRect(hwnd_, &rect)) {
        return POINT {rect.left, rect.top};
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
    if (layout_dirty_) RecalcSize();
    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        const int work_width = (std::max)(1, static_cast<int>(mi.rcWork.right - mi.rcWork.left) - Scale(8));
        const int work_height = (std::max)(1, static_cast<int>(mi.rcWork.bottom - mi.rcWork.top) - Scale(8));
        width_ = (std::min)(width_, work_width);
        height_ = (std::min)(height_, work_height);
        if (pos.x + width_ > mi.rcWork.right - Scale(4)) {
            pos.x = mi.rcWork.right - width_ - Scale(4);
        }
        if (pos.x < mi.rcWork.left + Scale(4)) {
            pos.x = mi.rcWork.left + Scale(4);
        }
        if (pos.y + height_ > mi.rcWork.bottom - Scale(4)) {
            pos.y = screen_pos.y - height_ - Scale(4);
        }
        if (pos.y < mi.rcWork.top + Scale(4)) {
            pos.y = mi.rcWork.top + Scale(4);
        }
    }
    RECT previous_rect {};
    const bool has_previous_rect = GetWindowRect(hwnd_, &previous_rect) != FALSE;
    const bool position_changed = !visible_ || !has_previous_rect ||
        previous_rect.left != pos.x || previous_rect.top != pos.y;
    const bool size_changed = !has_previous_rect ||
        previous_rect.right - previous_rect.left != width_ ||
        previous_rect.bottom - previous_rect.top != height_;
    const bool needs_show = !visible_;
    if (needs_show || position_changed || size_changed) {
        UINT flags = SWP_NOACTIVATE;
        if (needs_show) flags |= SWP_SHOWWINDOW;
        if (!position_changed) flags |= SWP_NOMOVE;
        if (!size_changed) flags |= SWP_NOSIZE;
        SetWindowPos(
            hwnd_, HWND_TOPMOST, pos.x, pos.y, width_, height_, flags);
        if (size_changed) ApplyRoundedRegion();
    }
    visible_ = true;
    if (paint_dirty_ || needs_show) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        paint_dirty_ = false;
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
        if (candidates_[i].text != candidates[i].text) return false;
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
    layout_dirty_ = layout_changed;
    paint_dirty_ = true;
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
    if (visible_) {
        if (page_ != previous_page) RecalcSize();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void CandidateWindow::SetEnglishMode(bool english) {
    english_mode_ = english;
    if (visible_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void CandidateWindow::RecalcSize() {
    HDC hdc = GetDC(nullptr);
    EnsureFonts();

    if (hdc == nullptr || font_ == nullptr || font_comp_ == nullptr) {
        if (hdc != nullptr) {
            ReleaseDC(nullptr, hdc);
        }
        width_ = Scale(kMinWidth);
        height_ = Scale(kVerticalPadding * 2 + kLineHeight * 2 + kRowGap);
        return;
    }

    const int comp_w = MeasureText(
        hdc, font_comp_, composing_.empty() ? L" " : composing_);
    const std::wstring page_text = BuildPageText(
        candidates_.size(), page_, page_size_);
    const int page_w = MeasureText(hdc, font_, page_text);
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

LRESULT CandidateWindow::OnPaint() {
    PAINTSTRUCT ps {};
    HDC hdc = BeginPaint(hwnd_, &ps);
    if (hdc == nullptr) {
        return 0;
    }

    RECT rc {};
    GetClientRect(hwnd_, &rc);

    // 双缓冲
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
    if (mem == nullptr || bmp == nullptr) {
        if (bmp != nullptr) DeleteObject(bmp);
        if (mem != nullptr) DeleteDC(mem);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    // 圆角背景与外边框；窗口区域也使用相同半径，四个角不会留下透明矩形命中区。
    HBRUSH bg = CreateSolidBrush(RGB(250, 251, 252));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(198, 205, 214));
    HGDIOBJ old_pen = SelectObject(mem, pen);
    HGDIOBJ old_brush = SelectObject(mem, bg);
    const int corner_diameter = Scale(kCornerRadius * 2);
    RoundRect(mem, rc.left, rc.top, rc.right, rc.bottom,
              corner_diameter, corner_diameter);
    SelectObject(mem, old_brush);
    SelectObject(mem, old_pen);
    DeleteObject(bg);
    DeleteObject(pen);

    // 左侧强调条
    HBRUSH accent = CreateSolidBrush(RGB(22, 119, 255));
    RECT accent_rc {1, Scale(kCornerRadius / 2), Scale(4),
                    rc.bottom - Scale(kCornerRadius / 2)};
    FillRect(mem, &accent_rc, accent);
    DeleteObject(accent);

    EnsureFonts();
    SetBkMode(mem, TRANSPARENT);

    // 组合拼音行（加大、无下划线）
    const int horizontal_padding = Scale(kHorizontalPadding);
    const int line_height = Scale(kLineHeight);
    const auto vertical = BuildCandidateWindowVerticalLayout(
        Scale(kVerticalPadding), line_height, Scale(kRowGap));
    const std::wstring page_text = BuildPageText(
        candidates_.size(), page_, page_size_);
    const int page_width = MeasureText(mem, font_, page_text);
    const auto header = BuildCandidateHeaderLayout(
        rc.right, page_width, horizontal_padding + Scale(4), horizontal_padding,
        Scale(kHeaderTextGap));
    RECT composing_rect {header.composing_left, vertical.composing_top,
                         header.composing_right, vertical.composing_bottom};
    HGDIOBJ old_font = SelectObject(mem, font_comp_);
    SetTextColor(mem, RGB(15, 23, 42));
    std::wstring comp_draw = composing_;
    if (comp_draw.empty()) {
        comp_draw = english_mode_ ? L"[EN]" : L"";
    }
    DrawTextW(mem, comp_draw.c_str(), -1, &composing_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!page_text.empty()) {
        // 页码与候选序号使用同一正常字重字体，避免抢过组合串的视觉层级。
        SelectObject(mem, font_);
        RECT page_rect {header.page_left, vertical.composing_top,
                        header.page_right, vertical.composing_bottom};
        DrawTextW(mem, page_text.c_str(), -1, &page_rect,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    // 预览字母不加下划线

    // 分隔线
    HPEN sep = CreatePen(PS_SOLID, 1, RGB(230, 233, 238));
    HGDIOBJ old_sep = SelectObject(mem, sep);
    MoveToEx(mem, Scale(8), vertical.separator_y, nullptr);
    LineTo(mem, rc.right - Scale(8), vertical.separator_y);
    SelectObject(mem, old_sep);
    DeleteObject(sep);

    // 候选：逐项绘制，选中高亮
    SelectObject(mem, font_);
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
            HBRUSH hot = CreateSolidBrush(RGB(22, 119, 255));
            FillRect(mem, &item_rc, hot);
            DeleteObject(hot);
            SetTextColor(mem, RGB(255, 255, 255));
        } else {
            SetTextColor(mem, RGB(30, 30, 30));
        }

        RECT text_rc {item_layout_[slot].text_left, y2,
                      item_layout_[slot].hit_right, vertical.candidate_bottom};
        DrawTextW(mem, item.c_str(), -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(mem, old_font);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(hwnd_, &ps);
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
        self->layout_dirty_ = true;
        self->paint_dirty_ = true;
        self->RecalcSize();
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     self->width_, self->height_, SWP_NOZORDER | SWP_NOACTIVATE);
        self->ApplyRoundedRegion();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONUP: {
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
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace shuru
