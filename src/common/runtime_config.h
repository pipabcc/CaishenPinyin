#pragma once

#include "english_candidate_position.h"

#include <string>

namespace shuru {

enum class CandidateFontSizeMode {
    FollowSkin = 0,
    Small = 1,
    Standard = 2,
    Large = 3,
    ExtraLarge = 4,
};

struct RuntimeConfig {
    bool offline = true;
    bool learning_enabled = true;
    bool content_logging_enabled = false;
    bool full_width_punctuation = true;
    bool english_default = false;
    bool english_mix_enabled = true;
    EnglishCandidatePosition english_candidate_position =
        EnglishCandidatePosition::Middle;
    bool fuzzy_enabled = true;
    bool fuzzy_initials = true;
    bool fuzzy_finals = true;
    bool fuzzy_missing_vowel = true;
    bool shuangpin_xiaohe = false;
    bool association_enabled = true;  // 上屏后联想候选
    int candidate_count = 9;
    std::wstring candidate_font_family;  // 空值表示跟随皮肤
    CandidateFontSizeMode candidate_font_size_mode =
        CandidateFontSizeMode::FollowSkin;
    std::wstring display_name = L"财神输入法";
    std::wstring tray_text = L"财";  // 任务栏语言指示按钮上的自定义字符
    bool v_mode_open_window = false;  // 按 v 是否直接打开剪贴板搜索窗口
    bool vv_mode_open_window = false; // 按 vv 是否直接打开自定义短语搜索窗口
    std::wstring skin_id = L"classic_blue"; // 当前选中的皮肤ID（默认原版经典蓝调）
};

// Thread-safe, process-cached settings. Values are read from
// %LOCALAPPDATA%\CaishenPinyin\settings.ini; missing/invalid values use safe defaults.
RuntimeConfig GetRuntimeConfig();
void ReloadRuntimeConfig();
bool IsValidDisplayName(const std::wstring& value) noexcept;
std::wstring NormalizeDisplayName(std::wstring value);
// 托盘指示字符：1-2 个可见字符（如「财」「中」）。
std::wstring NormalizeTrayText(std::wstring value);
std::wstring NormalizeCandidateFontFamily(std::wstring value);
int ResolveCandidateFontSize(
    CandidateFontSizeMode mode, int skin_font_size) noexcept;

}  // namespace shuru
