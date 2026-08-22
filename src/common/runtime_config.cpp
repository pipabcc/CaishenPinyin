#include "runtime_config.h"

#include "com_utils.h"
#include "user_data_paths.h"

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

std::wstring SettingsPath();

struct ConfigFileStamp {
    bool present = false;
    FILETIME write_time {};
    DWORD size_high = 0;
    DWORD size_low = 0;
};

ConfigFileStamp ReadConfigFileStamp() {
    ConfigFileStamp stamp;
    const std::wstring path = SettingsPath();
    if (path.empty()) return stamp;
    WIN32_FILE_ATTRIBUTE_DATA attributes {};
    if (!GetFileAttributesExW(
            path.c_str(), GetFileExInfoStandard, &attributes)) {
        return stamp;
    }
    stamp.present = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    stamp.write_time = attributes.ftLastWriteTime;
    stamp.size_high = attributes.nFileSizeHigh;
    stamp.size_low = attributes.nFileSizeLow;
    return stamp;
}

bool SameConfigFileStamp(
    const ConfigFileStamp& left, const ConfigFileStamp& right) noexcept {
    return left.present == right.present &&
        left.write_time.dwHighDateTime == right.write_time.dwHighDateTime &&
        left.write_time.dwLowDateTime == right.write_time.dwLowDateTime &&
        left.size_high == right.size_high && left.size_low == right.size_low;
}

ConfigFileStamp g_config_stamp;

std::wstring SettingsPath() {
    return CaishenUserDataPath(L"settings.ini");
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

CandidateFontSizeMode CandidateFontSizeModeFromLegacyValue(int value) noexcept {
    if (value <= 17) return CandidateFontSizeMode::Small;
    if (value <= 20) return CandidateFontSizeMode::Standard;
    if (value <= 24) return CandidateFontSizeMode::Large;
    return CandidateFontSizeMode::ExtraLarge;
}

CandidateFontSizeMode ReadCandidateFontSizeMode(
    const std::map<std::string, std::string>& values) {
    const auto found = values.find("CandidateFontSizeMode");
    if (found != values.end()) {
        if (found->second == "small") return CandidateFontSizeMode::Small;
        if (found->second == "standard") return CandidateFontSizeMode::Standard;
        if (found->second == "large") return CandidateFontSizeMode::Large;
        if (found->second == "extra_large") {
            return CandidateFontSizeMode::ExtraLarge;
        }
        return CandidateFontSizeMode::FollowSkin;
    }
    const int legacy = ReadInt(values, "CandidateFontSize", -1, 14, 32);
    return legacy < 0
        ? CandidateFontSizeMode::FollowSkin
        : CandidateFontSizeModeFromLegacyValue(legacy);
}

EnglishCandidatePosition ReadEnglishCandidatePosition(
    const std::map<std::string, std::string>& values) {
    const auto found = values.find("EnglishCandidatePosition");
    if (found == values.end()) return EnglishCandidatePosition::Middle;
    if (found->second == "first") return EnglishCandidatePosition::First;
    if (found->second == "last") return EnglishCandidatePosition::Last;
    return EnglishCandidatePosition::Middle;
}

std::wstring ReadCandidateFontFamily(
    const std::map<std::string, std::string>& values) {
    const auto found = values.find("CandidateFontFamily");
    if (found == values.end()) return {};
    return NormalizeCandidateFontFamily(Utf8ToWide(found->second));
}

RuntimeConfig ReadConfig() {
    const auto values = ReadValues();
    RuntimeConfig value;
    value.learning_enabled = ReadBool(values, "LearningEnabled", true);
    value.content_logging_enabled = ReadBool(values, "ContentLogging", false);
    value.full_width_punctuation = ReadBool(values, "FullWidthPunctuation", true);
    value.english_default = ReadBool(values, "EnglishDefault", false);
    value.english_mix_enabled = ReadBool(values, "EnglishMixEnabled", true);
    value.english_candidate_position = ReadEnglishCandidatePosition(values);
    value.fuzzy_enabled = ReadBool(values, "FuzzyEnabled", true);
    value.fuzzy_initials = ReadBool(values, "FuzzyInitials", true);
    value.fuzzy_finals = ReadBool(values, "FuzzyFinals", true);
    value.fuzzy_missing_vowel = ReadBool(values, "FuzzyMissingVowel", true);
    value.shuangpin_xiaohe = ReadBool(values, "ShuangpinXiaohe", false);
    value.association_enabled = ReadBool(values, "AssociationEnabled", true);
    value.candidate_count = ReadInt(values, "CandidateCount", 9, 3, 11);
    value.candidate_font_family = ReadCandidateFontFamily(values);
    value.candidate_font_size_mode = ReadCandidateFontSizeMode(values);
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
    const ConfigFileStamp observed_stamp = ReadConfigFileStamp();
    AcquireSRWLockShared(&g_lock);
    if (g_loaded && SameConfigFileStamp(g_config_stamp, observed_stamp)) {
        const RuntimeConfig value = g_config;
        ReleaseSRWLockShared(&g_lock);
        return value;
    }
    ReleaseSRWLockShared(&g_lock);

    AcquireSRWLockExclusive(&g_lock);
    if (!g_loaded || !SameConfigFileStamp(g_config_stamp, observed_stamp)) {
        g_config = ReadConfig();
        g_config_stamp = ReadConfigFileStamp();
        g_loaded = true;
    }
    const RuntimeConfig value = g_config;
    ReleaseSRWLockExclusive(&g_lock);
    return value;
}

void ReloadRuntimeConfig() {
    const RuntimeConfig value = ReadConfig();
    const ConfigFileStamp stamp = ReadConfigFileStamp();
    AcquireSRWLockExclusive(&g_lock);
    g_config = value;
    g_config_stamp = stamp;
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

std::wstring NormalizeCandidateFontFamily(std::wstring value) {
    const auto first = std::find_if_not(
        value.begin(), value.end(), [](wchar_t ch) {
            return std::iswspace(ch) != 0;
        });
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(), [](wchar_t ch) {
            return std::iswspace(ch) != 0;
        }).base();
    if (first >= last) return {};
    value = std::wstring(first, last);
    if (value.size() > 64 ||
        std::any_of(value.begin(), value.end(), [](wchar_t ch) {
            return std::iswcntrl(ch) != 0 ||
                (ch >= 0xD800 && ch <= 0xDFFF);
        })) {
        return {};
    }
    return value;
}

int ResolveCandidateFontSize(
    CandidateFontSizeMode mode, int skin_font_size) noexcept {
    switch (mode) {
    case CandidateFontSizeMode::Small:
        return 16;
    case CandidateFontSizeMode::Standard:
        return 19;
    case CandidateFontSizeMode::Large:
        return 22;
    case CandidateFontSizeMode::ExtraLarge:
        return 26;
    case CandidateFontSizeMode::FollowSkin:
    default:
        return (std::max)(14, (std::min)(32, skin_font_size));
    }
}

}  // namespace shuru
