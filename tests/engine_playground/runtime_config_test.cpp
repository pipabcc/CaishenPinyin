#include "common/runtime_config.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << "check failed line " << __LINE__ << '\n'; return 1; } } while (0)
int wmain() {
    namespace fs = std::filesystem;
    wchar_t root[MAX_PATH] = {};
    CHECK(GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH) > 0);
    fs::path dir = fs::path(root) / L"FacaiPinyin";
    fs::remove_all(dir);
    shuru::ReloadRuntimeConfig();
    auto c = shuru::GetRuntimeConfig();
    CHECK(c.learning_enabled && !c.content_logging_enabled && c.fuzzy_enabled);
    CHECK(c.candidate_count == 9 && c.candidate_font_size == 19 && !c.shuangpin_xiaohe);
    CHECK(c.display_name == L"发财拼音");
    fs::create_directories(dir);
    { std::ofstream f(dir / L"settings.ini"); f << u8"LearningEnabled=0\nContentLogging=1\nFuzzyEnabled=0\nFuzzyInitials=0\nFuzzyFinals=1\nFuzzyMissingVowel=0\nFullWidthPunctuation=0\nShuangpinXiaohe=1\nCandidateCount=5\nCandidateFontSize=24\nDisplayName=加油拼音\n"; }
    shuru::ReloadRuntimeConfig(); c = shuru::GetRuntimeConfig();
    CHECK(!c.learning_enabled && c.content_logging_enabled && !c.fuzzy_enabled);
    CHECK(!c.fuzzy_initials && c.fuzzy_finals && !c.fuzzy_missing_vowel);
    CHECK(!c.full_width_punctuation && c.shuangpin_xiaohe && c.candidate_count == 5 && c.candidate_font_size == 24);
    CHECK(c.display_name == L"加油拼音");
    { std::ofstream f(dir / L"settings.ini"); f << "CandidateCount=99\nCandidateFontSize=x\nContentLogging=maybe\n"; }
    shuru::ReloadRuntimeConfig(); c = shuru::GetRuntimeConfig();
    CHECK(c.candidate_count == 9 && c.candidate_font_size == 19 && !c.content_logging_enabled);
    CHECK(c.display_name == L"发财拼音");
    fs::remove_all(dir);
    std::cout << "runtime_config: OK\n";
    return 0;
}
