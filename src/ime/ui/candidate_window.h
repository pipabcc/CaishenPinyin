#pragma once

#include "engine/candidate.h"
#include "ime_ui_logic.h"

#include <Windows.h>

#include <string>
#include <functional>
#include <vector>

namespace shuru {

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

    void SetContent(
        const std::wstring& composing,
        const std::vector<Candidate>& candidates,
        size_t selected_index,
        size_t page = 0,
        size_t page_size = 9);
    void SetSelectedIndex(size_t selected_index);
    void SetEnglishMode(bool english);
    void SetSelectionHandler(std::function<void(size_t)> handler) { on_select_ = std::move(handler); }

private:
    static constexpr int kHorizontalPadding = 9;
    static constexpr int kVerticalPadding = 5;
    static constexpr int kLineHeight = 30;
    static constexpr int kRowGap = 4;
    static constexpr int kHeaderTextGap = 12;
    static constexpr int kCornerRadius = 8;
    static constexpr int kMinWidth = 280;
    static constexpr int kMaxWidth = 1440;

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT font_comp_ = nullptr;
    bool visible_ = false;
    bool english_mode_ = false;
    int width_ = kMinWidth;
    int height_ = kLineHeight * 2 + kVerticalPadding * 2 + kRowGap;

    std::wstring composing_;
    std::vector<Candidate> candidates_;
    size_t selected_ = 0;
    size_t page_ = 0;
    size_t page_size_ = 9;
    std::vector<CandidateItemLayout> item_layout_;
    bool layout_dirty_ = true;
    bool paint_dirty_ = true;
    std::function<void(size_t)> on_select_;

    int Scale(int value) const;
    void EnsureFonts();
    void ApplyRoundedRegion();
    void RecalcSize();
    bool DisplayContentEquals(
        const std::wstring& composing,
        const std::vector<Candidate>& candidates,
        size_t page,
        size_t page_size) const;
    int MeasureText(HDC hdc, HFONT font, const std::wstring& text) const;
    int HitTestCandidate(int x, int y) const;
    LRESULT OnPaint();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

}  // namespace shuru
