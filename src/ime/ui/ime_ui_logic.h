#pragma once

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/logger.h"
#include "common/com_utils.h"
#include "common/user_data_paths.h"
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

inline bool IsUtilityMode(const std::string& composing) noexcept {
    return !composing.empty() &&
        (composing.front() == 'v' || composing.front() == 'V');
}

inline bool IsVerticalUtilityMode(const std::string& composing) noexcept {
    if (!IsUtilityMode(composing)) return false;
    // vvv 是计算器入口，必须继续进入拼音引擎的特殊输入分支。
    return composing.size() < 3 ||
        ((composing[1] != 'v' && composing[1] != 'V') ||
         (composing[2] != 'v' && composing[2] != 'V'));
}

inline bool ShouldUsePlainUtilityBackground(
    bool utility_mode,
    bool is_user_skin) noexcept {
    return utility_mode && is_user_skin;
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
    int text_right = 0;
    int highlight_left = 0;
    int highlight_right = 0;
    int pin_left = 0;
    int pin_right = 0;
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

inline std::vector<int> BuildCandidateColumnWidths(
    const std::vector<std::vector<int>>& row_text_widths) {
    std::size_t column_count = 0;
    for (const auto& row : row_text_widths) {
        column_count = (std::max)(column_count, row.size());
    }
    std::vector<int> widths(column_count, 0);
    for (const auto& row : row_text_widths) {
        for (std::size_t column = 0; column < row.size(); ++column) {
            widths[column] = (std::max)(
                widths[column], (std::max)(0, row[column]));
        }
    }
    return widths;
}

inline std::vector<CandidateItemLayout> BuildCandidateRowLayout(
    const std::vector<int>& text_widths,
    const std::vector<int>& column_widths,
    std::size_t first_index,
    int text_left,
    int highlight_padding,
    int pin_width,
    int item_gap) {
    std::vector<CandidateItemLayout> layout;
    layout.reserve(text_widths.size());
    int current = text_left;
    for (std::size_t slot = 0; slot < text_widths.size(); ++slot) {
        const int text_width = (std::max)(0, text_widths[slot]);
        const int column_width = slot < column_widths.size()
            ? (std::max)(text_width, (std::max)(0, column_widths[slot]))
            : text_width;
        const int padding = (std::max)(0, highlight_padding);
        const int reserved_pin_width = (std::max)(0, pin_width);
        CandidateItemLayout item;
        item.index = first_index + slot;
        item.text_left = current;
        item.text_right = current + text_width;
        item.highlight_left = current - padding;
        item.highlight_right = item.text_right + padding;
        item.pin_left = current + column_width + padding;
        item.pin_right = item.pin_left + reserved_pin_width;
        item.hit_left = item.highlight_left;
        item.hit_right = item.pin_right;
        layout.push_back(item);
        current += column_width + reserved_pin_width +
            (std::max)(0, item_gap);
    }
    return layout;
}

inline int CandidateRowRequiredWidth(
    const std::vector<CandidateItemLayout>& layout,
    int right_padding) noexcept {
    return layout.empty() ? right_padding : layout.back().hit_right + right_padding;
}

inline int CandidateItemTextRight(
    int window_width,
    const CandidateItemLayout& item,
    int content_right_padding) noexcept {
    return (std::min)(
        (std::max)(0, window_width - (std::max)(0, content_right_padding)),
        item.highlight_right);
}

inline RECT BuildCandidatePinRect(
    const CandidateItemLayout& item,
    int window_width,
    int content_right_padding,
    int row_top,
    int row_bottom) noexcept {
    const int content_right = (std::max)(
        0, window_width - (std::max)(0, content_right_padding));
    const int right = (std::min)(content_right, item.pin_right);
    const int left = (std::min)(right, item.pin_left);
    return RECT {left, row_top, right, (std::max)(row_top, row_bottom)};
}

inline double CandidateColorLuminance(COLORREF color) noexcept {
    const auto linear = [](BYTE channel) noexcept {
        const double value = static_cast<double>(channel) / 255.0;
        return value <= 0.04045
            ? value / 12.92
            : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(GetRValue(color)) +
           0.7152 * linear(GetGValue(color)) +
           0.0722 * linear(GetBValue(color));
}

inline double CandidateColorContrast(COLORREF left, COLORREF right) noexcept {
    const double left_luminance = CandidateColorLuminance(left);
    const double right_luminance = CandidateColorLuminance(right);
    return ((std::max)(left_luminance, right_luminance) + 0.05) /
           ((std::min)(left_luminance, right_luminance) + 0.05);
}

inline COLORREF EnsureCandidateTextContrast(
    COLORREF configured,
    COLORREF background,
    double minimum_contrast = 4.5) noexcept {
    if (CandidateColorContrast(configured, background) >= minimum_contrast) {
        return configured;
    }
    constexpr COLORREF kDarkText = RGB(30, 41, 59);
    constexpr COLORREF kLightText = RGB(255, 255, 255);
    return CandidateColorContrast(kDarkText, background) >=
            CandidateColorContrast(kLightText, background)
        ? kDarkText : kLightText;
}

inline bool IsReliableCandidateRect(const RECT& rect, bool clipped = false) noexcept {
    return !clipped && rect.bottom > rect.top && rect.right >= rect.left;
}

inline bool IsUsableCandidateHostRect(const RECT& rect) noexcept {
    return rect.right > rect.left && rect.bottom > rect.top;
}

inline bool IsCandidateRectPlausibleForHost(
    const RECT& rect,
    const RECT& host_rect,
    int tolerance = 64) noexcept {
    if (!IsReliableCandidateRect(rect) ||
        !IsUsableCandidateHostRect(host_rect)) {
        return false;
    }
    const int margin = (std::max)(0, tolerance);
    return rect.right >= host_rect.left - margin &&
           rect.left <= host_rect.right + margin &&
           rect.bottom >= host_rect.top - margin &&
           rect.top <= host_rect.bottom + margin;
}

inline std::wstring SettingsDirectory(const std::wstring& path) {
    const auto split = path.find_last_of(L"\\/");
    return split == std::wstring::npos ? std::wstring{} : path.substr(0, split);
}

inline std::wstring SettingsLowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

inline bool IsSafeSettingsVersion(const std::wstring& value) noexcept {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](wchar_t character) {
            return (character >= L'0' && character <= L'9') ||
                   (character >= L'a' && character <= L'z') ||
                   (character >= L'A' && character <= L'Z') ||
                   character == L'.' || character == L'_' || character == L'-';
        });
}

inline std::wstring SettingsInstallRootFromPath(
    const std::wstring& path) {
    const std::wstring normalized = SettingsLowercase(path);
    const std::wstring marker = L"\\versions\\";
    const std::size_t marker_position = normalized.find(marker);
    if (marker_position == std::wstring::npos) return {};
    const std::size_t version_start = marker_position + marker.size();
    const std::size_t version_end = path.find_first_of(L"\\/", version_start);
    if (version_end == std::wstring::npos || version_end == version_start) {
        return {};
    }
    const std::wstring version = path.substr(version_start,
        version_end - version_start);
    return IsSafeSettingsVersion(version)
        ? path.substr(0, marker_position) : std::wstring{};
}

inline std::wstring ReadSettingsPointer(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input.is_open()) return {};
    std::string value;
    std::getline(input, value);
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }
    if (!value.empty() && value.back() == '\r') value.pop_back();
    const std::wstring wide = Utf8ToWide(value);
    return IsSafeSettingsVersion(wide) ? wide : std::wstring{};
}

inline void AddSettingsExecutableCandidate(
    std::vector<std::wstring>* paths, const std::wstring& directory) {
    if (paths == nullptr || directory.empty()) return;
    const std::wstring candidate = directory + L"\\ShuruSettings.exe";
    const std::wstring normalized = SettingsLowercase(candidate);
    const bool duplicate = std::any_of(paths->begin(), paths->end(),
        [&](const std::wstring& existing) {
            return SettingsLowercase(existing) == normalized;
        });
    if (!duplicate) paths->push_back(candidate);
}

inline std::vector<std::wstring> SettingsExecutableCandidatesForRoot(
    const std::wstring& module_path,
    const std::wstring& registered_module_path,
    const std::wstring& install_root,
    const std::wstring& current_version) {
    std::vector<std::wstring> paths;
    if (!install_root.empty() && IsSafeSettingsVersion(current_version)) {
        AddSettingsExecutableCandidate(
            &paths, install_root + L"\\versions\\" + current_version);
    }
    AddSettingsExecutableCandidate(
        &paths, SettingsDirectory(registered_module_path));
    const std::wstring module_dir = SettingsDirectory(module_path);
    AddSettingsExecutableCandidate(&paths, module_dir);
    if (!install_root.empty()) {
        AddSettingsExecutableCandidate(&paths, install_root);
    }
    AddSettingsExecutableCandidate(
        &paths, module_dir + L"\\settings\\bin\\Release\\net8.0-windows");
    AddSettingsExecutableCandidate(
        &paths, module_dir + L"\\..\\settings\\bin\\Release\\net8.0-windows");
    return paths;
}

inline std::vector<std::wstring> SettingsExecutableCandidates(
    const std::wstring& module_path,
    const std::wstring& registered_module_path = {}) {
    std::wstring install_root = SettingsInstallRootFromPath(module_path);
    if (install_root.empty()) {
        install_root = SettingsInstallRootFromPath(registered_module_path);
    }
    const std::wstring current_version = install_root.empty()
        ? std::wstring{}
        : ReadSettingsPointer(install_root + L"\\current");
    return SettingsExecutableCandidatesForRoot(
        module_path, registered_module_path, install_root, current_version);
}

inline bool StartSettingsProcess(
    const std::wstring& path, const wchar_t* params) {
    std::wstring command_line = L"\"" + path + L"\"";
    if (params != nullptr && *params != L'\0') {
        command_line += L" ";
        command_line += params;
    }
    std::vector<wchar_t> mutable_command(command_line.begin(),
        command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process_info {};
    const std::wstring working_directory = SettingsDirectory(path);
    const BOOL created = CreateProcessW(
        path.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT, nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup, &process_info);
    if (!created) return false;
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
}

// 请求文件里写的是固定关键字而不是原始命令行：请求目录对所有沙箱应用可写，
// 关键字化可确保它无法演变成任意进程启动。不在白名单内的参数不代理。
inline const wchar_t* SettingsUiRequestCommand(const wchar_t* params) noexcept {
    if (params == nullptr || *params == L'\0') return L"settings";
    if (wcscmp(params, L"-quick") == 0) return L"clipboard";
    if (wcscmp(params, L"-quick phrases") == 0) return L"phrases";
    return nullptr;
}

inline std::wstring CreateUiRequestToken() {
    GUID id {};
    if (FAILED(CoCreateGuid(&id))) return {};
    wchar_t formatted[40] {};
    if (StringFromGUID2(id, formatted, ARRAYSIZE(formatted)) == 0) return {};
    std::wstring token;
    token.reserve(32);
    for (const wchar_t character : std::wstring(formatted)) {
        if (iswxdigit(character) != 0) {
            token.push_back(static_cast<wchar_t>(towlower(character)));
        }
    }
    return token.size() == 32 ? token : std::wstring {};
}

// AppContainer 宿主被系统禁止创建包外进程，把打开 UI 的意图落成请求文件，
// 交给常驻的剪贴板监听进程代为执行。
inline bool WriteSettingsUiRequest(const wchar_t* params) {
    const wchar_t* command = SettingsUiRequestCommand(params);
    if (command == nullptr) return false;
    const std::wstring directory = CaishenUserDataPath(L"ui_requests");
    if (directory.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(directory), error);
    if (error) return false;
    const std::wstring token = CreateUiRequestToken();
    if (token.empty()) return false;

    const std::wstring path = directory + L"\\" + token + L".txt";
    // 临时文件不用 .txt 后缀，避免监听方在改名前就被唤醒读到半截内容。
    const std::wstring temporary = directory + L"\\" + token + L".tmp";
    {
        std::ofstream output(
            std::filesystem::path(temporary),
            std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << "CAISHEN_UI_REQUEST_V1\n";
        for (const wchar_t character : std::wstring(command)) {
            output << static_cast<char>(character);
        }
        output << '\n';
        output.flush();
        if (!output) {
            output.close();
            DeleteFileW(temporary.c_str());
            return false;
        }
    }
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

inline bool LaunchSettingsExecutable(HWND owner, HINSTANCE module, const wchar_t* params = nullptr) {
    // 沙箱宿主里 ShellExecute 与 CreateProcess 都会以 ERROR_ACCESS_DENIED
    // 失败，没有可回退的启动路径，直接改走请求文件代理。
    if (IsCurrentProcessAppContainer()) {
        if (WriteSettingsUiRequest(params)) return true;
        SHURU_LOG_WARN(
            "settings ui request rejected in app container params=%ls",
            params == nullptr ? L"(none)" : params);
        return false;
    }
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
        const DWORD shell_error = GetLastError();
        if (StartSettingsProcess(path, params)) return true;
        SHURU_LOG_WARN(
            "settings-launch failed path=%ls shell=%p error=%lu create-error=%lu",
            path.c_str(), reinterpret_cast<void*>(result), shell_error,
            GetLastError());
    }
    return false;
}

// 沙箱中失败只可能是代理进程没常驻，与「设置程序缺失」是两回事，
// 沿用同一句错误提示会把用户引向重装。
inline void ReportSettingsLaunchFailure(HWND owner) {
    const wchar_t* message = IsCurrentProcessAppContainer()
        ? L"设置程序未在后台运行，无法从系统沙箱中打开。\n"
          L"请先从开始菜单的「财神输入法设置」快捷方式打开一次。"
        : L"无法找到或启动 ShuruSettings.exe。请重新安装设置程序。";
    MessageBoxW(owner, message, L"财神输入法",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
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
    // 沙箱宿主既启动不了监听进程，Local\ 命名空间又与普通宿主隔离，
    // IsClipboardMonitorRunning 永远为假——不加这道闸每次激活都会白试一次
    // 注定失败的进程启动。监听进程由普通宿主负责拉起。
    if (IsCurrentProcessAppContainer()) return true;
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
