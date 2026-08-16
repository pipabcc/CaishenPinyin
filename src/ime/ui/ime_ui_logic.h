#pragma once

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/candidate.h"

namespace shuru {

inline std::size_t CandidateQueryLimit(
    std::size_t page_size, bool single_complete_syllable) noexcept {
    constexpr std::size_t kBufferedPageCount = 10;
    constexpr std::size_t kSingleSyllableBudget = 256;
    const std::size_t normalized_page_size = (std::max)(std::size_t{1}, page_size);
    const std::size_t regular_budget = normalized_page_size * kBufferedPageCount;
    return single_complete_syllable
        ? (std::max)(regular_budget, kSingleSyllableBudget)
        : regular_budget;
}

struct ShortcutModifierDecisionCache {
    WPARAM key = 0;
    LPARAM key_data = 0;
    DWORD tick = 0;
    bool decision = false;
    bool valid = false;

    void Store(
        WPARAM value_key, LPARAM value_key_data, bool value_decision,
        DWORD value_tick) noexcept {
        key = value_key;
        key_data = value_key_data;
        decision = value_decision;
        tick = value_tick;
        valid = true;
    }

    bool Consume(
        WPARAM value_key, LPARAM value_key_data, DWORD value_tick,
        bool* value_decision, DWORD maximum_age_ms = 1000) noexcept {
        const bool matches = valid && key == value_key &&
            key_data == value_key_data &&
            value_tick - tick <= maximum_age_ms;
        valid = false;
        if (!matches || value_decision == nullptr) return false;
        *value_decision = decision;
        return true;
    }

    void Clear() noexcept { valid = false; }
};

inline bool IsVerticalUtilityMode(const std::string& composing) noexcept {
    if (composing.empty() ||
        (composing.front() != 'v' && composing.front() != 'V')) {
        return false;
    }
    // vvv 是计算器入口，必须继续进入拼音引擎的特殊输入分支。
    return composing.size() < 3 || composing.compare(0, 3, "vvv") != 0;
}

inline bool TryGetVerticalUtilityFilterDigit(
    WPARAM key, bool num_lock, char* digit) noexcept {
    if (digit == nullptr) return false;
    if (key >= '0' && key <= '9') {
        *digit = static_cast<char>(key);
        return true;
    }
    if (num_lock && key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        *digit = static_cast<char>('0' + key - VK_NUMPAD0);
        return true;
    }
    return false;
}


struct CandidateCommitPlan {
    std::wstring committed;
    std::string remaining;
    std::string learned_input;
    // 候选自身携带规范全拼时，学习必须使用它。前缀预测例如
    // duan -> 短剑不能错误写成 duan -> 短剑。
    std::string learned_pinyin;
    bool has_coverage = false;
};

inline const std::wstring& CandidateCommitText(
    const Candidate& candidate) noexcept {
    return candidate.full_content.empty()
        ? candidate.text
        : candidate.full_content;
}

inline CandidateCommitPlan PlanCandidateCommit(
    const std::string& composing, const Candidate& candidate) {
    CandidateCommitPlan plan;
    const std::size_t covered = (std::min)(candidate.covered_input_len, composing.size());
    plan.has_coverage = covered != 0;
    if (!plan.has_coverage) return plan;
    plan.committed = CandidateCommitText(candidate);
    plan.remaining = composing.substr(covered);
    plan.learned_input = composing.substr(0, covered);
    plan.learned_pinyin = candidate.pinyin;
    return plan;
}

enum class CandidatePagingDirection {
    None,
    Previous,
    Next,
};

enum class CandidateRowDirection {
    None,
    Up,
    Down,
};

inline CandidatePagingDirection GetCandidatePagingDirection(
    WPARAM key, bool shift_down) noexcept {
    if (key == VK_PRIOR) return CandidatePagingDirection::Previous;
    if (key == VK_NEXT) return CandidatePagingDirection::Next;
    if (shift_down) return CandidatePagingDirection::None;
    if (key == VK_OEM_MINUS)
        return CandidatePagingDirection::Previous;
    if (key == VK_OEM_PLUS)
        return CandidatePagingDirection::Next;
    return CandidatePagingDirection::None;
}

inline CandidateRowDirection GetCandidateRowDirection(
    WPARAM key, bool shift_down) noexcept {
    if (key == VK_UP) return CandidateRowDirection::Up;
    if (key == VK_DOWN) return CandidateRowDirection::Down;
    if (shift_down) return CandidateRowDirection::None;
    if (key == VK_OEM_COMMA) return CandidateRowDirection::Up;
    if (key == VK_OEM_PERIOD) return CandidateRowDirection::Down;
    return CandidateRowDirection::None;
}

struct CandidatePageState {
    static constexpr std::size_t kDefaultPageSize = 9;

    std::size_t total = 0;
    std::size_t page = 0;
    std::size_t selected = 0;
    std::size_t page_size = kDefaultPageSize;

    std::size_t PageSize() const noexcept {
        return (std::max)(std::size_t{1}, page_size);
    }

    std::size_t PageCount() const noexcept {
        return total == 0 ? 1 : (total + PageSize() - 1) / PageSize();
    }

    void Clamp() noexcept {
        if (total == 0) {
            page = 0;
            selected = 0;
            return;
        }
        selected = (std::min)(selected, total - 1);
        page = selected / PageSize();
    }

    void Select(std::size_t index) noexcept {
        if (total == 0) {
            Clamp();
            return;
        }
        selected = (std::min)(index, total - 1);
        page = selected / PageSize();
    }

    void MovePrevious() noexcept {
        if (total == 0) return;
        Select((selected + total - 1) % total);
    }

    void MoveNext() noexcept {
        if (total == 0) return;
        Select((selected + 1) % total);
    }

    void MoveRowUp() noexcept {
        Clamp();
        const std::size_t row_width = PageSize();
        if (total == 0 || selected < row_width) return;
        Select(selected - row_width);
    }

    void MoveRowDown() noexcept {
        Clamp();
        if (total == 0) return;
        const std::size_t row_width = PageSize();
        const std::size_t current_row = selected / row_width;
        if (current_row + 1 >= PageCount()) return;
        const std::size_t next_row_start = (current_row + 1) * row_width;
        Select((std::min)(next_row_start + selected % row_width, total - 1));
    }

    void PreviousPage() noexcept {
        Clamp();
        if (page > 0) --page;
        selected = (std::min)(page * PageSize(), total == 0 ? std::size_t{0} : total - 1);
    }

    void NextPage() noexcept {
        Clamp();
        if (page + 1 < PageCount()) ++page;
        selected = (std::min)(page * PageSize(), total == 0 ? std::size_t{0} : total - 1);
    }

    std::size_t GlobalIndex(std::size_t slot) const noexcept {
        return page * PageSize() + slot;
    }

    bool IsSelectableSlot(std::size_t slot) const noexcept {
        return slot < PageSize() && GlobalIndex(slot) < total;
    }
};

inline std::wstring CandidateComposingDisplay(
    const std::vector<Candidate>& candidates,
    std::size_t selected,
    const std::wstring& fallback) {
    if (selected >= candidates.size() ||
        candidates[selected].input_segmentation.empty()) {
        return fallback;
    }
    const std::string& segmented = candidates[selected].input_segmentation;
    return std::wstring(segmented.begin(), segmented.end());
}

struct CandidateItemLayout {
    std::size_t index = 0;
    int text_left = 0;
    int hit_left = 0;
    int hit_right = 0;
};

struct CandidateWindowVerticalLayout {
    int composing_top = 0;
    int composing_bottom = 0;
    int separator_y = 0;
    int candidate_top = 0;
    int candidate_bottom = 0;
    int window_height = 0;
};

struct CandidateHeaderLayout {
    int composing_left = 0;
    int composing_right = 0;
    int page_left = 0;
    int page_right = 0;
};

inline int CandidateHeaderRequiredWidth(
    int composing_width,
    int page_width,
    int left_padding,
    int right_padding,
    int text_gap) noexcept {
    const int composing = (std::max)(0, composing_width);
    const int page = (std::max)(0, page_width);
    const int gap = page == 0 ? 0 : (std::max)(0, text_gap);
    return (std::max)(0, left_padding) + composing + gap + page +
        (std::max)(0, right_padding);
}

inline CandidateHeaderLayout BuildCandidateHeaderLayout(
    int window_width,
    int page_width,
    int left_padding,
    int right_padding,
    int text_gap) noexcept {
    CandidateHeaderLayout layout;
    layout.composing_left = (std::max)(0, left_padding);
    const int content_right = (std::max)(
        layout.composing_left, window_width - (std::max)(0, right_padding));
    layout.page_right = content_right;

    const int page = (std::max)(0, page_width);
    if (page == 0) {
        layout.page_left = content_right;
        layout.composing_right = content_right;
        return layout;
    }

    layout.page_left = (std::max)(layout.composing_left, content_right - page);
    layout.composing_right = (std::max)(
        layout.composing_left, layout.page_left - (std::max)(0, text_gap));
    return layout;
}

inline CandidateWindowVerticalLayout BuildCandidateWindowVerticalLayout(
    int vertical_padding,
    int line_height,
    int row_gap) noexcept {
    CandidateWindowVerticalLayout layout;
    const int padding = (std::max)(0, vertical_padding);
    const int height = (std::max)(1, line_height);
    const int gap = (std::max)(0, row_gap);
    layout.composing_top = padding;
    layout.composing_bottom = layout.composing_top + height;
    layout.separator_y = layout.composing_bottom + gap / 2;
    layout.candidate_top = layout.composing_bottom + gap;
    layout.candidate_bottom = layout.candidate_top + height;
    layout.window_height = layout.candidate_bottom + padding;
    return layout;
}

inline std::vector<CandidateItemLayout> BuildCandidateRowLayout(
    const std::vector<int>& text_widths,
    std::size_t first_index,
    int text_left,
    int left_padding,
    int right_padding,
    int item_gap) {
    std::vector<CandidateItemLayout> layout;
    layout.reserve(text_widths.size());
    int current = text_left;
    for (std::size_t slot = 0; slot < text_widths.size(); ++slot) {
        const int width = (std::max)(0, text_widths[slot]);
        layout.push_back({first_index + slot, current, current - left_padding,
                          current + width + right_padding});
        current += width + item_gap;
    }
    return layout;
}

inline int CandidateRowRequiredWidth(
    const std::vector<CandidateItemLayout>& layout,
    int right_padding) noexcept {
    return layout.empty() ? right_padding : layout.back().hit_right + right_padding;
}

inline bool IsReliableCandidateRect(const RECT& rect, bool clipped = false) noexcept {
    return !clipped && rect.bottom > rect.top && rect.right >= rect.left;
}

inline std::vector<std::wstring> SettingsExecutableCandidates(
    const std::wstring& module_path,
    const std::wstring& registered_module_path = {}) {
    auto directory = [](const std::wstring& path) {
        const auto split = path.find_last_of(L"\\/");
        return split == std::wstring::npos ? std::wstring{} : path.substr(0, split);
    };
    std::vector<std::wstring> paths;
    auto add = [&](const std::wstring& dir) {
        if (dir.empty()) return;
        const std::wstring candidate = dir + L"\\ShuruSettings.exe";
        if (std::find(paths.begin(), paths.end(), candidate) == paths.end()) paths.push_back(candidate);
    };
    add(directory(registered_module_path));
    const std::wstring module_dir = directory(module_path);
    add(module_dir);
    add(module_dir + L"\\settings\\bin\\Release\\net8.0-windows");
    add(module_dir + L"\\..\\settings\\bin\\Release\\net8.0-windows");
    return paths;
}

inline bool LaunchSettingsExecutable(HWND owner, HINSTANCE module, const wchar_t* params = nullptr) {
    wchar_t module_path[MAX_PATH] = {};
    if (module == nullptr ||
        GetModuleFileNameW(module, module_path, ARRAYSIZE(module_path)) == 0) {
        return false;
    }
    for (const auto& path : SettingsExecutableCandidates(module_path)) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            owner, L"open", path.c_str(), params, nullptr, SW_SHOWNORMAL));
        if (result > 32) return true;
    }
    return false;
}

inline bool IsClipboardMonitorRunning() noexcept {
    HANDLE mutex = OpenMutexW(
        SYNCHRONIZE, FALSE, L"Local\\CaishenPinyinClipboardMonitorV2");
    if (mutex != nullptr) {
        CloseHandle(mutex);
        return true;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

inline bool EnsureClipboardMonitorExecutable(HINSTANCE module) {
    return IsClipboardMonitorRunning() ||
        LaunchSettingsExecutable(nullptr, module, L"-clipboard-monitor");
}

inline std::wstring FormatUnsignedWithSeparators(std::uint64_t value) {
    std::wstring digits = std::to_wstring(value);
    for (std::ptrdiff_t position = static_cast<std::ptrdiff_t>(digits.size()) - 3;
         position > 0;
         position -= 3) {
        digits.insert(static_cast<std::size_t>(position), 1, L',');
    }
    return digits;
}

inline int CandidateMetadataFontSize(int candidate_font_size) noexcept {
    return (std::max)(11, candidate_font_size - 4);
}

inline std::wstring BuildTypingStatisticsText(std::uint64_t daily_count) {
    return L"今日 " + FormatUnsignedWithSeparators(daily_count) + L" 字";
}

inline std::size_t CandidateExpandedFirstPage(
    std::size_t current_page,
    std::size_t maximum_rows) noexcept {
    const std::size_t rows = (std::max)(std::size_t{1}, maximum_rows);
    return current_page / rows * rows;
}

inline std::size_t CandidateExpandedRowCount(
    std::size_t candidate_count,
    std::size_t page_size,
    std::size_t current_page,
    std::size_t maximum_rows) noexcept {
    const std::size_t normalized_page_size =
        (std::max)(std::size_t{1}, page_size);
    const std::size_t page_count = candidate_count == 0
        ? 1
        : (candidate_count + normalized_page_size - 1) /
            normalized_page_size;
    const std::size_t first_page = CandidateExpandedFirstPage(
        (std::min)(current_page, page_count - 1), maximum_rows);
    return (std::min)(
        (std::max)(std::size_t{1}, maximum_rows),
        page_count - first_page);
}

}  // namespace shuru
