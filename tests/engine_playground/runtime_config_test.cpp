#include "common/runtime_config.h"
#include "common/user_data_paths.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << "check failed line " << __LINE__ << '\n'; return 1; } } while (0)
int wmain() {
    namespace fs = std::filesystem;
    CHECK(shuru::IsPackageVirtualizedLocalAppData(
        L"C:\\Users\\tester\\AppData\\Local\\Packages\\Host_abc\\AC"));
    CHECK(shuru::ResolveCanonicalLocalAppData(
              L"C:\\Users\\tester\\AppData\\Local\\Packages\\Host_abc\\AC",
              L"C:\\Users\\tester") ==
          L"C:\\Users\\tester\\AppData\\Local");
    CHECK(shuru::ResolveCanonicalLocalAppData(
              L"C:\\temp\\local-app-data", L"C:\\Users\\tester") ==
          L"C:\\temp\\local-app-data");
    wchar_t root[MAX_PATH] = {};
    CHECK(GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH) > 0);
    fs::path dir = fs::path(root) / L"CaishenPinyin";
    fs::remove_all(dir);
    shuru::ReloadRuntimeConfig();
    auto c = shuru::GetRuntimeConfig();
    CHECK(c.learning_enabled && !c.content_logging_enabled && c.fuzzy_enabled);
    CHECK(c.english_mix_enabled &&
          c.english_candidate_position ==
              shuru::EnglishCandidatePosition::Middle);
    CHECK(c.candidate_count == 9 && c.candidate_font_family.empty() &&
          c.candidate_font_size_mode == shuru::CandidateFontSizeMode::FollowSkin &&
          !c.shuangpin_xiaohe);
    CHECK(shuru::ResolveCandidateFontSize(c.candidate_font_size_mode, 18) == 18);
    CHECK(c.display_name == L"财神输入法");
    fs::create_directories(dir);
    { std::ofstream f(dir / L"settings.ini"); f << u8"LearningEnabled=0\nContentLogging=1\nEnglishMixEnabled=0\nEnglishCandidatePosition=first\nFuzzyEnabled=0\nFuzzyInitials=0\nFuzzyFinals=1\nFuzzyMissingVowel=0\nFullWidthPunctuation=0\nShuangpinXiaohe=1\nCandidateCount=5\nCandidateFontSize=24\nDisplayName=加油拼音\n"; }
    shuru::ReloadRuntimeConfig(); c = shuru::GetRuntimeConfig();
    CHECK(!c.learning_enabled && c.content_logging_enabled && !c.fuzzy_enabled);
    CHECK(!c.english_mix_enabled &&
          c.english_candidate_position ==
              shuru::EnglishCandidatePosition::First);
    CHECK(!c.fuzzy_initials && c.fuzzy_finals && !c.fuzzy_missing_vowel);
    CHECK(!c.full_width_punctuation && c.shuangpin_xiaohe &&
          c.candidate_count == 5 &&
          c.candidate_font_size_mode == shuru::CandidateFontSizeMode::Large &&
          shuru::ResolveCandidateFontSize(c.candidate_font_size_mode, 18) == 22);
    CHECK(c.display_name == L"加油拼音");
    { std::ofstream f(dir / L"settings.ini"); f << "CandidateCount=11\nCandidateFontFamily=DengXian\nCandidateFontSizeMode=extra_large\nEnglishCandidatePosition=last\nContentLogging=maybe\n"; }
    shuru::ReloadRuntimeConfig(); c = shuru::GetRuntimeConfig();
    CHECK(c.candidate_count == 11 && c.candidate_font_family == L"DengXian" &&
          c.candidate_font_size_mode == shuru::CandidateFontSizeMode::ExtraLarge &&
          shuru::ResolveCandidateFontSize(c.candidate_font_size_mode, 18) == 26 &&
          !c.content_logging_enabled);
    CHECK(c.english_mix_enabled &&
          c.english_candidate_position ==
              shuru::EnglishCandidatePosition::Last);
    CHECK(c.display_name == L"财神输入法");
    { std::ofstream f(dir / L"settings.ini"); f << "CandidateCount=99\nCandidateFontFamily=%0A\nCandidateFontSizeMode=unknown\nEnglishMixEnabled=maybe\nEnglishCandidatePosition=unknown\n"; }
    shuru::ReloadRuntimeConfig(); c = shuru::GetRuntimeConfig();
    CHECK(c.candidate_count == 9 &&
          c.candidate_font_size_mode == shuru::CandidateFontSizeMode::FollowSkin);
    CHECK(c.english_mix_enabled &&
          c.english_candidate_position ==
              shuru::EnglishCandidatePosition::Middle);
    CHECK(shuru::ResolveCandidateFontSize(shuru::CandidateFontSizeMode::Small, 32) == 16);
    CHECK(shuru::ResolveCandidateFontSize(shuru::CandidateFontSizeMode::Standard, 14) == 19);
    fs::remove_all(dir);
    std::cout << "runtime_config: OK\n";
    return 0;
}
