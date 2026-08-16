#pragma once

#include <string>

namespace shuru {

struct RuntimeConfig {
    bool offline = true;
    bool learning_enabled = true;
    bool content_logging_enabled = false;
    bool full_width_punctuation = true;
    bool english_default = false;
    bool fuzzy_enabled = true;
    bool fuzzy_initials = true;
    bool fuzzy_finals = true;
    bool fuzzy_missing_vowel = true;
    bool shuangpin_xiaohe = false;
    bool association_enabled = true;  // 上屏后联想候选
    int candidate_count = 9;
    int candidate_font_size = 19;
    std::wstring display_name = L"财神输入法";
    std::wstring tray_text = L"财";  // 任务栏语言指示按钮上的自定义字符
    bool v_mode_open_window = false;  // 按 v 是否直接打开剪贴板搜索窗口
    bool vv_mode_open_window = false; // 按 vv 是否直接打开自定义短语搜索窗口
};

// Thread-safe, process-cached settings. Values are read from
// %LOCALAPPDATA%\CaishenPinyin\settings.ini; missing/invalid values use safe defaults.
RuntimeConfig GetRuntimeConfig();
void ReloadRuntimeConfig();
bool IsValidDisplayName(const std::wstring& value) noexcept;
std::wstring NormalizeDisplayName(std::wstring value);
// 托盘指示字符：1-2 个可见字符（如「财」「中」）。
std::wstring NormalizeTrayText(std::wstring value);

}  // namespace shuru
