#include "runtime_config.h"

#include "com_utils.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <cwctype>

namespace shuru {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
RuntimeConfig g_config;
bool g_loaded = false;

std::wstring SettingsPath() {
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (length == 0) return {};
    std::wstring root(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), length);
    if (written == 0 || written >= length) return {};
    root.resize(written);
    return root + L"\\CaishenPinyin\\settings.ini";
}

std::map<std::string, std::string> ReadValues() {
    std::map<std::string, std::string> values;
    std::ifstream input(SettingsPath());
    std::string line;
    while (std::getline(input, line)) {
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        values[line.substr(0, split)] = line.substr(split + 1);
    }
    return values;
}

bool ReadBool(const std::map<std::string, std::string>& values,
              const char* name, bool fallback) {
    const auto it = values.find(name);
    if (it == values.end() || (it->second != "0" && it->second != "1")) return fallback;
    return it->second == "1";
}

int ReadInt(const std::map<std::string, std::string>& values, const char* name,
            int fallback, int minimum, int maximum) {
    const auto it = values.find(name);
    if (it == values.end()) return fallback;
    try {
        size_t used = 0;
        const int parsed = std::stoi(it->second, &used);
        return used == it->second.size() && parsed >= minimum && parsed <= maximum
                   ? parsed : fallback;
    } catch (...) { return fallback; }
}

std::wstring ReadDisplayName(
    const std::map<std::string, std::string>& values,
    const std::wstring& fallback) {
    const auto found = values.find("DisplayName");
    if (found == values.end()) return fallback;
    const std::wstring decoded = Utf8ToWide(found->second);
    const std::wstring normalized = NormalizeDisplayName(decoded);
    return normalized.empty() ? fallback : normalized;
}

RuntimeConfig ReadConfig() {
    const auto values = ReadValues();
    RuntimeConfig value;
    value.learning_enabled = ReadBool(values, "LearningEnabled", true);
    value.content_logging_enabled = ReadBool(values, "ContentLogging", false);
    value.full_width_punctuation = ReadBool(values, "FullWidthPunctuation", true);
    value.english_default = ReadBool(values, "EnglishDefault", false);
    value.fuzzy_enabled = ReadBool(values, "FuzzyEnabled", true);
    value.fuzzy_initials = ReadBool(values, "FuzzyInitials", true);
    value.fuzzy_finals = ReadBool(values, "FuzzyFinals", true);
    value.fuzzy_missing_vowel = ReadBool(values, "FuzzyMissingVowel", true);
    value.shuangpin_xiaohe = ReadBool(values, "ShuangpinXiaohe", false);
    value.association_enabled = ReadBool(values, "AssociationEnabled", true);
    value.candidate_count = ReadInt(values, "CandidateCount", 9, 3, 9);
    value.candidate_font_size = ReadInt(values, "CandidateFontSize", 19, 14, 32);
    value.display_name = ReadDisplayName(values, L"财神输入法");
    value.v_mode_open_window = ReadBool(values, "VModeOpenWindow", false);
    value.vv_mode_open_window = ReadBool(values, "VvModeOpenWindow", false);
    {
        const auto found = values.find("SkinId");
        if (found != values.end() && !found->second.empty()) {
            value.skin_id = Utf8ToWide(found->second);
        }
    }
    {
        const auto found = values.find("TrayText");
        if (found != values.end()) {
            const std::wstring normalized = NormalizeTrayText(Utf8ToWide(found->second));
            if (!normalized.empty()) value.tray_text = normalized;
        }
    }
    return value;
}

}  // namespace

RuntimeConfig GetRuntimeConfig() {
    AcquireSRWLockShared(&g_lock);
    if (g_loaded) {
        const RuntimeConfig value = g_config;
        ReleaseSRWLockShared(&g_lock);
        return value;
    }
    ReleaseSRWLockShared(&g_lock);

    AcquireSRWLockExclusive(&g_lock);
    if (!g_loaded) {
        g_config = ReadConfig();
        g_loaded = true;
    }
    const RuntimeConfig value = g_config;
    ReleaseSRWLockExclusive(&g_lock);
    return value;
}

void ReloadRuntimeConfig() {
    const RuntimeConfig value = ReadConfig();
    AcquireSRWLockExclusive(&g_lock);
    g_config = value;
    g_loaded = true;
    ReleaseSRWLockExclusive(&g_lock);
}

bool IsValidDisplayName(const std::wstring& value) noexcept {
    if (value.empty() || value.size() > 24) return false;
    return std::none_of(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswcntrl(ch) != 0 || (ch >= 0xD800 && ch <= 0xDFFF);
    });
}

std::wstring NormalizeDisplayName(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }).base();
    if (first >= last) return {};
    value = std::wstring(first, last);
    return IsValidDisplayName(value) ? value : std::wstring{};
}

std::wstring NormalizeTrayText(std::wstring value) {
    value = NormalizeDisplayName(std::move(value));
    if (value.empty() || value.size() > 2) return {};
    return value;
}

}  // namespace shuru
