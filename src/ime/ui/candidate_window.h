#pragma once

#include "common/typing_stats.h"
#include "engine/candidate.h"
#include "directwrite_text_renderer.h"
#include "ime_ui_logic.h"

#include <Windows.h>

#include <deque>
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

// 将已经预乘的候选框表面与由其 Alpha 扩散得到的黑色阴影合成。
// surface_alpha 与 pixels 尺寸均为 width * height。
void CompositeCandidateSurfaceAndShadow(
    std::uint8_t* pixels,
    const std::vector<std::uint8_t>& surface_alpha,
    int width,
    int height,
    int shadow_offset,
    int blur_radius,
    int blur_passes,
    std::uint8_t shadow_opacity);

UINT RuntimeSettingsChangedMessage() noexcept;

// 分层窗口必须使用灰度抗锯齿；ClearType 子像素会在 Alpha 合成后产生彩边。
BYTE CandidateLayeredFontQuality() noexcept;

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
    bool UsesPlainUtilityBackgroundForTesting() const {
        return UsesPlainUtilityBackground();
    }
    bool IsSkinAnimationTimerActiveForTesting() const noexcept {
        return skin_animation_timer_active_;
    }

    void SetContent(
        const std::wstring& composing,
        const std::vector<Candidate>& candidates,
        size_t selected_index,
        size_t page = 0,
        size_t page_size = 9,
        bool utility_mode = false,
        bool vertical_utility_mode = false);
    void SetSelectedIndex(size_t selected_index);
    bool ToggleExpanded();
    bool SetExpanded(bool expanded);
    bool IsExpanded() const noexcept { return expanded_; }
    void SetEnglishMode(bool english);
    void SetTypingStats(const TypingStatsSnapshot& snapshot);
    void SetPinningEnabled(bool enabled);
    void SetSelectionHandler(std::function<void(size_t)> handler) { on_select_ = std::move(handler); }
    void SetPinHandler(std::function<void(size_t)> handler) { on_pin_ = std::move(handler); }
    void SetDragHandler(std::function<void(POINT)> handler) { on_drag_ = std::move(handler); }
    void SetSearchHandler(std::function<void()> handler) { on_search_clicked_ = std::move(handler); }
    void SetClearSearchHandler(std::function<void()> handler) { on_search_cleared_ = std::move(handler); }
    void SetDeleteHandler(std::function<void(size_t)> handler) { on_delete_item_ = std::move(handler); }

    // 引擎就绪轮询：poll 返回 true 继续轮询，false 停止。
    void StartReadyPolling(std::function<bool()> poll);
    void StopReadyPolling();

    // 某些宿主不派发 Shift KeyUp；在窗口线程轮询物理释放状态。
    void StartShiftReleasePolling(std::function<bool()> poll);
    void StopShiftReleasePolling();

    // 宿主接管 Ctrl/Alt/Win 快捷键后可能不再回调 KeyUp；独立轮询用于
    // 清理输入法内部的旧修饰键世代，不与 Shift 单击状态机共用定时器。
    void StartShortcutReleasePolling(std::function<bool()> poll);
    void StopShortcutReleasePolling();

    // 将需要读取宿主布局的操作推迟到当前 TSF 编辑消息返回后执行。
    // 连续请求会合并为一次回调，避免在旧布局和新布局之间来回移动。
    void StartDeferredAction(std::function<void()> action, UINT delay_ms = 16);
    void StopDeferredAction();
    bool IsDeferredActionActive() const noexcept {
        return deferred_action_active_;
    }

    // 在候选窗所属 UI 线程的下一轮消息中执行。用于等待当前 TSF
    // 编辑会话完全退出，不与候选定位、V 模式或轮询定时器共享状态。
    bool PostOwnerThreadAction(std::function<void()> action);

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
    static constexpr int kShadowMargin = 16;
    static constexpr int kShadowBlurPasses = 3;
    static constexpr int kMinWidth = 220;
    static constexpr int kMaxWidth = 1440;
    static constexpr int kVerticalRowHeight = 32;
    static constexpr int kVerticalMaxVisible = 10;
    static constexpr int kExpandedMaxRows = 5;
    static constexpr int kExpandToggleWidth = 16;
    static constexpr int kExpandToggleGap = 5;
    static constexpr int kPinReservedWidth = 14;
    // 选中胶囊在序号左侧与候选词右侧各留出的空白；同时为排版误差
    // 提供余量，避免末字被绘制矩形裁掉。项间距必须大于两倍内边距，
    // 否则相邻胶囊会咬合在一起。
    static constexpr int kHighlightPaddingX = 8;
    static constexpr int kCandidateItemGap = 18;
    static constexpr UINT_PTR kReadyPollTimerId = 1;
    static constexpr UINT kReadyPollIntervalMs = 80;
    static constexpr UINT_PTR kVModeTimerId = 1002;
    static constexpr UINT_PTR kSkinAnimationTimerId = 1003;
    // 延迟动作会为每一代递增编号，使用独立区间避免与固定轮询定时器碰撞。
    static constexpr UINT_PTR kDeferredActionTimerId = 0x4000;
    static constexpr UINT_PTR kShiftReleasePollTimerId = 1005;
    static constexpr UINT kShiftReleasePollIntervalMs = 10;
    static constexpr UINT_PTR kShortcutReleasePollTimerId = 1006;
    static constexpr UINT kShortcutReleasePollIntervalMs = 10;
    static constexpr UINT kOwnerThreadActionMessage = WM_APP + 0x31A;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr;
    HFONT font_comp_ = nullptr;
    HFONT font_meta_ = nullptr;
    HFONT font_header_title_ = nullptr;
    HFONT font_utility_ = nullptr;
    void* gdip_font_ = nullptr;
    void* gdip_font_comp_ = nullptr;
    void* gdip_font_meta_ = nullptr;
    void* gdip_font_header_title_ = nullptr;
    void* gdip_font_utility_ = nullptr;
    DirectWriteTextRenderer directwrite_text_;
    bool visible_ = false;
    bool english_mode_ = false;
    bool expanded_ = false;
    bool mouse_down_ = false;
    bool dragging_ = false;
    POINT drag_start_cursor_ {};
    POINT drag_start_window_ {};
    bool ready_poll_active_ = false;
    bool shift_release_poll_active_ = false;
    bool shortcut_release_poll_active_ = false;
    int width_ = kMinWidth;
    int height_ = kLineHeight * 2 + kVerticalPadding * 2 + kRowGap;

    // 竖向与滚动条交互状态
    int hovered_row_ = -1;
    bool hovered_delete_ = false;
    int hovered_candidate_ = -1;
    bool hovered_pin_ = false;
    int pressed_pin_candidate_ = -1;
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
    bool utility_mode_ = false;
    bool vertical_utility_mode_ = false;
    bool pinning_enabled_ = false;
    std::vector<std::vector<CandidateItemLayout>> item_rows_;
    bool layout_dirty_ = true;
    bool paint_dirty_ = true;
    std::function<void(size_t)> on_select_;
    std::function<void(size_t)> on_pin_;
    std::function<void(POINT)> on_drag_;
    std::function<void()> on_search_clicked_;
    std::function<void()> on_search_cleared_;
    std::function<void(size_t)> on_delete_item_;
    std::function<bool()> ready_poll_;
    std::function<bool()> shift_release_poll_;
    std::function<bool()> shortcut_release_poll_;
    bool vmode_timer_active_ = false;
    std::function<void()> vmode_timer_cb_;
    bool deferred_action_active_ = false;
    // 每次延迟请求使用新的定时器 ID。KillTimer 无法撤回已经排队的
    // WM_TIMER；递增 ID 可让这些旧消息在窗口过程中被直接忽略。
    UINT_PTR deferred_timer_id_ = kDeferredActionTimerId;
    UINT_PTR deferred_timer_serial_ = 0;
    std::function<void()> deferred_action_;
    std::deque<std::function<void()>> owner_thread_actions_;
    bool skin_animation_timer_active_ = false;
    std::wstring font_signature_;
    std::wstring layout_skin_id_;

    int Scale(int value) const;
    int ShadowMargin() const;
    bool UsesPlainUtilityBackground() const;
    void StopSkinAnimation();
    void SyncSkinAnimation();
    void ResetFonts();
    void ReloadRuntimeSettings();
    void EnsureFonts();
    void RecalcSize();
    bool DisplayContentEquals(
        const std::wstring& composing,
        const std::vector<Candidate>& candidates,
        size_t page,
        size_t page_size) const;
    int MeasureText(HDC hdc, HFONT font, const std::wstring& text) const;
    int HitTestCandidate(int x, int y) const;
    int HitTestPin(int x, int y) const;
    void RefreshTypingStats();
    void OpenSettings();
    void DrawContent(
        void* graphics, uint8_t* pixels, int bitmap_width, int bitmap_height,
        int content_offset);
    bool UpdateLayeredWindowContent(const POINT& window_origin);
    LRESULT OnPaint();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

}  // namespace shuru
