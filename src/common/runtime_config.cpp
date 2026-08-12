#include "runtime_config.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>

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
    return root + L"\\FacaiPinyin\\settings.ini";
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
    value.candidate_count = ReadInt(values, "CandidateCount", 9, 3, 9);
    value.candidate_font_size = ReadInt(values, "CandidateFontSize", 19, 14, 32);
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

}  // namespace shuru
