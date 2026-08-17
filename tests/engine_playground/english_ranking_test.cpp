#include "engine/pinyin_engine.h"
#include "common/com_utils.h"

#include <iostream>
#include <string>

namespace {

bool IsEnglishFirst(
    const shuru::EngineQueryResult& result, const std::wstring& text) {
    return !result.candidates.empty() && result.candidates.front().is_english &&
        result.candidates.front().text == text &&
        result.candidates.front().covered_input_len > 0;
}

bool IsChineseFirst(const shuru::EngineQueryResult& result) {
    return !result.candidates.empty() && !result.candidates.front().is_english;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    shuru::PinyinEngine engine;
    if (!engine.Initialize(argv[1])) return 3;
    const auto easy = engine.Query("easy", 9);
    if (!IsEnglishFirst(easy, L"easy")) {
        return 4;
    }
    const auto engli = engine.Query("engli", 9);
    if (!IsEnglishFirst(engli, L"English")) return 5;
    const auto uppercase = engine.Query("ENGLI", 9);
    if (!IsEnglishFirst(uppercase, L"English")) { std::cerr << "uppercase ranking failed\n"; return 6; }
    if (!IsChineseFirst(engine.Query("shi", 9)) ||
        !IsChineseFirst(engine.Query("nihao", 9)) ||
        !IsChineseFirst(engine.Query("women", 9)) ||
        !IsChineseFirst(engine.Query("nihoa", 9))) {
        std::cerr << "Chinese protection failed\n";
        return 7;
    }
    std::cout << "english_ranking: OK\n";
    return 0;
}
