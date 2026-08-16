#pragma once

#include "common/typing_stats.h"
#include "engine/candidate.h"
#include "ime_ui_logic.h"

#include <Windows.h>

#include <string>
#include <functional>
#include <vector>

namespace shuru {

// 返回一个像素被圆角矩形覆盖的 0..255 面积比例。候选框和选中背景
// 共用该 4x4 子像素算法，保证相同半径下边缘形状一致。
std::uint8_t RoundedRectanglePixelCoverage(
    int pixel_x,
    int pixel_y,
    int left,
    int top,
    int width,
    int height,
    int radius) noexcept;

class CandidateWindow {
public:
    CandidateWindow() = default;
    ~CandidateWindow();

    bool Create(HINSTANCE instance);
    void Destroy();

    void Show(const POINT& screen_pos);
    void Hide();
    bool IsVisible() const { return visible_; }
    SIZE WindowSize() const;
    POINT ScreenPosition() const;
    HWND GetHwnd() const noexcept { return hwnd_; }

    void SetContent(
        const std::wstring& composing,
        const std::vector<Candidate>& candidates,
        size_t selected_index,
        size_t page = 0,
        size_t page_size = 9);
    void SetSelectedIndex(size_t selected_index);
    bool ToggleExpanded();
    bool SetExpanded(bool expanded);
    bool IsExpanded() const noexcept { return expanded_; }
    void SetEnglishMode(bool english);
    void SetTypingStats(const TypingStatsSnapshot& snapshot);
    void SetSelectionHandler(std::function<void(size_t)> handler) { on_select_ = std::move(handler); }
    void SetDragHandler(std::function<void(POINT)> handler) { on_drag_ = std::move(handler); }
    void SetSearchHandler(std::function<void()> handler) { on_search_clicked_ = std::move(handler); }
    void SetClearSearchHandler(std::function<void()> handler) { on_search_cleared_ = std::move(handler); }
    void SetDeleteHandler(std::function<void(size_t)> handler) { on_delete_item_ = std::move(handler); }

    // 引擎就绪轮询：poll 返回 true 继续轮询，false 停止。
    void StartReadyPolling(std::function<bool()> poll);
    void StopReadyPolling();

    // V/VV 模式延时唤起独立窗口定时器
    void StartVModeTimer(std::function<void()> callback, UINT delay_ms = 220);
    void StopVModeTimer();

private:
    static constexpr int kHorizontalPadding = 9;
    static constexpr int kVerticalPadding = 5;
    static constexpr int kLineHeight = 30;
    static constexpr int kRowGap = 4;
    static constexpr int kHeaderTextGap = 12;
    static constexpr int kCornerRadius = 8;
    static constexpr int kShadowMargin = 12;
    static constexpr int kShadowBlurPasses = 3;
    static constexpr int kMinWidth = 280;
    static constexpr int kMaxWidth = 1440;
    static constexpr int kVerticalRowHeight = 32;
    static constexpr int kVerticalMaxVisible = 10;
    static constexpr int kExpandedMaxRows = 5;
    static constexpr int kExpandToggleWidth = 16;
    static constexpr int kExpandToggleGap = 5;
    static constexpr int kUtilityFontSize = 13;
    static constexpr UINT_PTR kReadyPollTimerId = 1;
    static constexpr UINT kReadyPollIntervalMs = 80;
    static constexpr UINT_PTR kVModeTimerId = 1002;
    static constexpr UINT_PTR kSkinAnimationTimerId = 1003;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr;
    HFONT font_comp_ = nullptr;
    HFONT font_meta_ = nullptr;
    HFONT font_header_title_ = nullptr;
    HFONT font_utility_ = nullptr;
    bool visible_ = false;
    bool english_mode_ = false;
    bool expanded_ = false;
    bool mouse_down_ = false;
    bool dragging_ = false;
    POINT drag_start_cursor_ {};
    POINT drag_start_window_ {};
    bool ready_poll_active_ = false;
    int width_ = kMinWidth;
    int height_ = kLineHeight * 2 + kVerticalPadding * 2 + kRowGap;

    // 竖向与滚动条交互状态
    int hovered_row_ = -1;
    bool hovered_delete_ = false;
    bool scrollbar_hovered_ = false;
    bool scrollbar_dragging_ = false;
    int scroll_offset_ = 0;
    int drag_start_y_ = 0;
    int drag_start_scroll_ = 0;
    RECT search_box_rect_ {};
    RECT search_clear_rect_ {};
    RECT expand_toggle_rect_ {};

    std::wstring composing_;
    TypingStatsSnapshot typing_stats_;
    std::vector<Candidate> candidates_;
    size_t selected_ = 0;
    size_t page_ = 0;
    size_t page_size_ = 9;
    std::vector<std::vector<CandidateItemLayout>> item_rows_;
    bool layout_dirty_ = true;
    bool paint_dirty_ = true;
    std::function<void(size_t)> on_select_;
    std::function<void(POINT)> on_drag_;
    std::function<void()> on_search_clicked_;
    std::function<void()> on_search_cleared_;
    std::function<void(size_t)> on_delete_item_;
    std::function<bool()> ready_poll_;
    bool vmode_timer_active_ = false;
    std::function<void()> vmode_timer_cb_;
    bool skin_animation_timer_active_ = false;
    std::wstring font_signature_;
    std::wstring layout_skin_id_;

    int Scale(int value) const;
    int ShadowMargin() const;
    void StopSkinAnimation();
    void SyncSkinAnimation();
    void ResetFonts();
    void EnsureFonts();
    void RecalcSize();
    bool DisplayContentEquals(
        const std::wstring& composing,
        const std::vector<Candidate>& candidates,
        size_t page,
        size_t page_size) const;
    int MeasureText(HDC hdc, HFONT font, const std::wstring& text) const;
    int HitTestCandidate(int x, int y) const;
    void RefreshTypingStats();
    void OpenSettings();
    void DrawContent(
        HDC hdc, uint8_t* pixels, int bitmap_width, int bitmap_height,
        int content_offset);
    bool UpdateLayeredWindowContent(const POINT& window_origin);
    LRESULT OnPaint();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

}  // namespace shuru
