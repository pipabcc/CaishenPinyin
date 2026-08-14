#include "engine/pinyin_engine.h"
#include "engine/dictionary.h"
#include "common/com_utils.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool ContainsText(const shuru::EngineQueryResult& result, const std::wstring& expected) {
    for (const auto& candidate : result.candidates) {
        if (candidate.text == expected) {
            return true;
        }
    }
    return false;
}

bool ContainsTextInFirst(
    const shuru::EngineQueryResult& result,
    const std::wstring& expected,
    size_t count) {
    const size_t end = (std::min)(count, result.candidates.size());
    for (size_t i = 0; i < end; ++i) {
        if (result.candidates[i].text == expected) {
            return true;
        }
    }
    return false;
}

bool IsSingleBmpCharacter(const std::wstring& text) {
    return text.size() == 1 && text.front() >= L'\x4e00' && text.front() <= L'\x9fff';
}

bool VerifyCandidate(
    shuru::PinyinEngine& engine,
    const std::string& input,
    const std::wstring& expected) {
    const auto result = engine.Query(input, 9);
    if (ContainsText(result, expected)) {
        return true;
    }
    std::cerr << "candidate assertion failed: input=" << input
              << " expected=" << shuru::WideToUtf8(expected) << " actual=";
    for (const auto& candidate : result.candidates) {
        std::cerr << shuru::WideToUtf8(candidate.text) << ',';
    }
    std::cerr << '\n';
    return false;
}

bool VerifyMixedSentence(
    shuru::PinyinEngine& engine,
    const std::string& input,
    const std::wstring& expected,
    const std::string& expected_segmentation) {
    const auto result = engine.Query(input, 9);
    if (!result.candidates.empty()) {
        const auto& candidate = result.candidates.front();
        if (candidate.text == expected &&
            candidate.source == shuru::CandidateSource::MixedSentence &&
            candidate.covered_input_len == input.size() &&
            candidate.input_segmentation == expected_segmentation &&
            !candidate.learn_segments.empty()) {
            return true;
        }
    }
    std::cerr << "mixed sentence assertion failed: input=" << input
              << " expected=" << shuru::WideToUtf8(expected) << " actual=";
    for (const auto& candidate : result.candidates) {
        std::cerr << shuru::WideToUtf8(candidate.text)
                  << "[" << candidate.input_segmentation << "]" << ',';
    }
    std::cerr << '\n';
    return false;
}

bool VerifyMixedPrefixLookupIsBoundedAndStable() {
    shuru::Dictionary dictionary;
    dictionary.BeginBulkLoad();
    dictionary.AddWord("xuexiao", L"学校", 1000, false);
    dictionary.AddWord("xixi", L"西西", 900, false);
    dictionary.AddWord("li", L"里", 800, false);
    dictionary.AddWord("you", L"有", 700, false);
    dictionary.AddWord("yige", L"一个", 600, false);
    dictionary.AddWord("meinv", L"美女", 500, false);
    dictionary.EndBulkLoad();

    constexpr size_t kLimit = 3;
    const std::string input = "xxliyouyigmein";
    for (size_t length = 1; length <= input.size(); ++length) {
        const std::string prefix = input.substr(0, length);
        const auto first = dictionary.LookupMixedPrefixes(prefix, kLimit);
        const auto second = dictionary.LookupMixedPrefixes(prefix, kLimit);
        if (first.size() > kLimit || first.size() != second.size()) return false;
        for (size_t index = 0; index < first.size(); ++index) {
            if (first[index].candidate.text != second[index].candidate.text ||
                first[index].consumed_input != second[index].consumed_input ||
                first[index].segmented_input != second[index].segmented_input ||
                first[index].consumed_input > length) {
                return false;
            }
        }
    }
    return true;
}

bool VerifyPrefixLookupIsBestFirstAndStable() {
    shuru::Dictionary dictionary;
    dictionary.BeginBulkLoad();
    for (int index = 0; index < 300; ++index) {
        std::string key = "xianga";
        key.push_back(static_cast<char>('a' + (index / 26) % 26));
        key.push_back(static_cast<char>('a' + index % 26));
        dictionary.AddWord(key, L"低频词", 1, false);
    }
    dictionary.AddWord("xiangzzx", L"想法", 1000, false);
    dictionary.AddWord("xiangzzy", L"想象", 900, false);
    dictionary.AddWord("xiangzzz", L"想要", 800, false);
    dictionary.EndBulkLoad();

    const auto first = dictionary.LookupPrefix("xiang", 3);
    const auto second = dictionary.LookupPrefix("xiang", 3);
    if (first.size() != 3 || second.size() != first.size()) return false;
    const std::wstring expected[] = {L"想法", L"想象", L"想要"};
    for (size_t index = 0; index < first.size(); ++index) {
        if (first[index].text != expected[index] ||
            first[index].text != second[index].text ||
            first[index].pinyin != second[index].pinyin) {
            return false;
        }
    }
    return true;
}

bool VerifyQueryStaysResponsiveDuringSave(
    shuru::PinyinEngine& engine,
    const std::string& learned_pinyin,
    const std::wstring& learned_word) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\CaishenPinyin.UserDictionary");
    if (mutex == nullptr) {
        std::cerr << "failed to create user dictionary test mutex\n";
        return false;
    }
    const DWORD mutex_wait = WaitForSingleObject(mutex, 5000);
    if (mutex_wait != WAIT_OBJECT_0 && mutex_wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        std::cerr << "failed to acquire user dictionary test mutex\n";
        return false;
    }

    engine.Learn(learned_pinyin, learned_word);
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(1500);
    std::chrono::milliseconds maximum_latency {0};
    bool candidates_ok = true;
    while (Clock::now() < deadline) {
        const auto started = Clock::now();
        const auto result = engine.Query("nihao", 9);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        maximum_latency = (std::max)(maximum_latency, elapsed);
        candidates_ok = candidates_ok && ContainsText(result, L"你好");
        Sleep(5);
    }

    ReleaseMutex(mutex);
    CloseHandle(mutex);
    std::cout << "maximum query latency during user dictionary save: "
              << maximum_latency.count() << " ms\n";
    if (!candidates_ok || maximum_latency > std::chrono::milliseconds(1000)) {
        std::cerr << "query was blocked by user dictionary persistence\n";
        return false;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace shuru;
    namespace fs = std::filesystem;

    const std::wstring lexicon = argc >= 2 ? argv[1] : L"data\\lexicon";
    const std::wstring learned_word = L"自动测试词";
    const std::string learned_pinyin = "zidongceshici";
    std::wstring user_dict_path;
    const fs::path test_local_app_data = fs::temp_directory_path() /
        (L"FacaiEnginePhase1-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code cleanup_error;
    fs::remove_all(test_local_app_data, cleanup_error);
    fs::create_directories(test_local_app_data, cleanup_error);
    if (cleanup_error ||
        !SetEnvironmentVariableW(L"LOCALAPPDATA", test_local_app_data.c_str())) return 12;

    const fs::path custom_phrase_path = test_local_app_data / L"CaishenPinyin" /
        L"data" / L"lexicon" / L"custom_phrases.txt";
    fs::create_directories(custom_phrase_path.parent_path(), cleanup_error);
    if (cleanup_error) return 20;
    {
        std::ofstream custom_phrase_file(
            custom_phrase_path, std::ios::binary | std::ios::trunc);
        custom_phrase_file << "\xEF\xBB\xBF# custom phrase test\n"
                           << "sds\t" << WideToUtf8(L"深度思考") << "\t1\n"
                           << "sds\t" << WideToUtf8(L"认真思考") << "\t1\n"
                           << "sds\t" << WideToUtf8(L"延长思考") << "\t3\n";
        if (!custom_phrase_file) return 20;
    }

    {
        PinyinEngine engine;
        if (!engine.Initialize(lexicon)) {
            std::wcerr << L"引擎初始化失败: " << lexicon << L"\n";
            return 1;
        }
        if (!engine.IsReady() ||
            !VerifyCandidate(engine, "nihao", L"你好") ||
            !VerifyCandidate(engine, "nh", L"你好") ||
            !VerifyCandidate(engine, "mhu", L"模糊") ||
            !VerifyCandidate(engine, "sunguo", L"孙国") ||
            !VerifyCandidate(engine, "shurufa", L"输入法")) {
            return 2;
        }

        const auto xiang = engine.Query("xiang", 90);
        const auto xian = engine.Query("xian", 90);
        if (xiang.candidates.empty() || xiang.candidates.front().text != L"想" ||
            xian.candidates.empty() || xian.candidates.front().text != L"现" ||
            xiang.candidates.front().lexeme_prior == 0 ||
            xian.candidates.front().lexeme_prior == 0 ||
            !ContainsText(xiang, L"想法") ||
            !ContainsText(xiang, L"想象") ||
            !ContainsText(xiang, L"想要")) {
            std::cerr << "short lexeme prior or prefix recall failed: xiang=";
            for (const auto& candidate : xiang.candidates) {
                std::cerr << WideToUtf8(candidate.text) << ',';
            }
            std::cerr << " xian=";
            for (const auto& candidate : xian.candidates) {
                std::cerr << WideToUtf8(candidate.text) << ',';
            }
            std::cerr << '\n';
            return 23;
        }
        engine.Learn("xian", L"先");
        const auto learned_xian = engine.Query("xian", 9);
        if (learned_xian.candidates.empty() ||
            learned_xian.candidates.front().text != L"先" ||
            !engine.UndoLastLearning()) {
            std::cerr << "user learning did not override system lexeme prior\n";
            return 24;
        }

        const auto segmented_sentence = engine.Query("womenzhidao", 20);
        const auto segmented = std::find_if(
            segmented_sentence.candidates.begin(), segmented_sentence.candidates.end(),
            [](const Candidate& candidate) { return candidate.text == L"我们知道"; });
        const auto predicted_suffix = engine.Query("haoduoc", 20);
        const auto predicted = std::find_if(
            predicted_suffix.candidates.begin(), predicted_suffix.candidates.end(),
            [](const Candidate& candidate) { return candidate.text == L"好多次"; });
        const auto segmented_double_partial = engine.Query("yingw", 20);
        const auto double_partial = std::find_if(
            segmented_double_partial.candidates.begin(),
            segmented_double_partial.candidates.end(),
            [](const Candidate& candidate) { return candidate.text == L"应我"; });
        const auto segmented_double_exact = engine.Query("renzhen", 20);
        const auto double_exact = std::find_if(
            segmented_double_exact.candidates.begin(),
            segmented_double_exact.candidates.end(),
            [](const Candidate& candidate) { return candidate.text == L"认真"; });
        if (segmented == segmented_sentence.candidates.end() ||
            segmented->input_segmentation != "wo'men'zhi'dao" ||
            predicted == predicted_suffix.candidates.end() ||
            predicted->input_segmentation != "hao'duo'c" ||
            double_partial == segmented_double_partial.candidates.end() ||
            double_partial->input_segmentation != "ying'w" ||
            double_exact == segmented_double_exact.candidates.end() ||
            double_exact->input_segmentation != "ren'zhen" ||
            !xiang.candidates.front().input_segmentation.empty()) {
            std::cerr << "candidate input segmentation failed\n";
            return 25;
        }

        if (!VerifyMixedSentence(
                engine, "xxliyouyigmein", L"学校里有一个美女",
                "x'x'li'you'yi'g'mei'n") ||
            !VerifyMixedSentence(
                engine, "womenyqchifan", L"我们一起吃饭",
                "wo'men'y'q'chi'fan") ||
            !VerifyMixedSentence(
                engine, "jintianqshangb", L"今天去上班",
                "jin'tian'q'shang'b") ||
            !VerifyMixedSentence(
                engine, "mingtianxwqubj", L"明天下午去北京",
                "ming'tian'x'w'qu'b'j") ||
            !VerifyMixedPrefixLookupIsBoundedAndStable() ||
            !VerifyPrefixLookupIsBestFirstAndStable()) {
            return 22;
        }

        const auto custom_phrases = engine.Query("sds", 9);
        if (custom_phrases.candidates.size() < 3 ||
            custom_phrases.candidates[0].text != L"深度思考" ||
            custom_phrases.candidates[1].text != L"认真思考" ||
            custom_phrases.candidates[2].text != L"延长思考" ||
            custom_phrases.candidates[0].learnable ||
            custom_phrases.candidates[0].from_user ||
            custom_phrases.candidates[0].source != CandidateSource::CustomPhrase) {
            std::cerr << "custom phrase position or metadata failed\n";
            return 20;
        }

        {
            std::ofstream custom_phrase_file(
                custom_phrase_path, std::ios::binary | std::ios::trunc);
            custom_phrase_file << "sds\t" << WideToUtf8(L"重新加载短语") << "\t2\n";
        }
        if (!engine.ReloadCustomPhrases()) return 21;
        const auto reloaded_phrases = engine.Query("sds", 9);
        if (reloaded_phrases.candidates.size() < 2 ||
            reloaded_phrases.candidates[1].text != L"重新加载短语") {
            std::cerr << "custom phrase reload failed\n";
            return 21;
        }


        const auto duan = engine.Query("duan", 9);
        if (duan.candidates.size() < 4 ||
            !std::all_of(
                duan.candidates.begin(), duan.candidates.begin() + 4,
                [](const Candidate& candidate) {
                    return IsSingleBmpCharacter(candidate.text) && candidate.pinyin == "duan";
                }) ||
            !ContainsTextInFirst(duan, L"短", 4)) {
            std::cerr << "single-syllable candidates were crowded out: ";
            for (const auto& candidate : duan.candidates) {
                std::cerr << WideToUtf8(candidate.text) << ',';
            }
            std::cerr << '\n';
            return 13;
        }

        engine.Learn("duan", L"短剑");
        const auto filtered_legacy_prediction = engine.Query("duan", 20);
        if (std::any_of(
                filtered_legacy_prediction.candidates.begin(),
                filtered_legacy_prediction.candidates.end(),
                [](const Candidate& candidate) {
                    return candidate.text == L"短剑" && candidate.pinyin == "duan";
                })) {
            std::cerr << "legacy prefix learning was not filtered\n";
            return 15;
        }

        const auto duanju = engine.Query("duanju", 9);
        if (duanju.candidates.empty() || duanju.candidates.front().text != L"短剧" ||
            duanju.candidates.front().pinyin != "duanju") {
            std::cerr << "exact multi-syllable word did not rank first: ";
            for (const auto& candidate : duanju.candidates) {
                std::cerr << WideToUtf8(candidate.text) << ',';
            }
            std::cerr << '\n';
            return 14;
        }

        const auto exact_rank = engine.Query("nihao", 9);
        if (exact_rank.candidates.empty() || exact_rank.candidates.front().match_cost != 0) {
            std::cerr << "exact candidate did not outrank fuzzy candidates\n";
            return 10;
        }
        const auto fuzzy_rank = engine.Query("mhu", 9);
        bool weighted_recovery = false;
        for (const auto& candidate : fuzzy_rank.candidates) {
            if (candidate.text == L"模糊" && candidate.match_cost > 0) weighted_recovery = true;
        }
        if (!weighted_recovery) return 11;

        const auto mixed_language = engine.Query("duolaAmeng", 9);
        {
            const bool ok = !mixed_language.candidates.empty() &&
                mixed_language.candidates.front().text == L"哆啦A梦" &&
                // 混输候选改为按拼音子段学习（learn_segments 非空），整体仍
                // 标记为 learnable=true；学习时由 LearnCandidate 逐段处理。
                mixed_language.candidates.front().learnable &&
                !mixed_language.candidates.front().learn_segments.empty();
            if (!ok) {
                std::cerr << "mixed Chinese/English candidate failed: ";
                for (const auto& candidate : mixed_language.candidates)
                    std::cerr << WideToUtf8(candidate.text) << ',';
                std::cerr << '\n';
                return 16;
            }
        }

        const auto correction_started = std::chrono::steady_clock::now();
        const auto corrected = engine.Query("zehhsigejuicuo", 9);
        const auto correction_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - correction_started);
        if (corrected.candidates.empty() ||
            corrected.candidates.front().text != L"这是个纠错" ||
            corrected.candidates.front().source != CandidateSource::Correction ||
            correction_latency > std::chrono::milliseconds(100)) {
            std::cerr << "bounded correction failed: ";
            for (const auto& candidate : corrected.candidates)
                std::cerr << WideToUtf8(candidate.text) << ',';
            std::cerr << " latency=" << correction_latency.count() << "ms\n";
            return 17;
        }

        const auto time_shortcut = engine.Query("sj", 9);
        if (time_shortcut.candidates.size() < 4 ||
            time_shortcut.candidates.front().source != CandidateSource::Dynamic ||
            time_shortcut.candidates.front().learnable) {
            std::cerr << "time shortcut did not rank first\n";
            return 18;
        }

        const auto calculator = engine.Query("v1+2*3", 9);
        if (calculator.candidates.size() != 1 ||
            calculator.candidates.front().text != L"7" ||
            calculator.candidates.front().learnable) {
            std::cerr << "calculator candidate failed\n";
            return 19;
        }

        user_dict_path = engine.user_dict_path();
        if (user_dict_path.empty() || user_dict_path == lexicon + L"\\user_dict.txt") {
            std::wcerr << L"用户词典没有使用独立可写目录\n";
            return 3;
        }

        if (!VerifyQueryStaysResponsiveDuringSave(engine, learned_pinyin, learned_word)) {
            return 4;
        }
        if (!VerifyCandidate(engine, learned_pinyin, learned_word)) {
            return 5;
        }

        const std::wstring promoted = L"拟好";
        engine.Learn("nihao", promoted);
        const auto after_learning = engine.Query("nihao", 9);
        if (after_learning.candidates.empty() || after_learning.candidates.front().text != promoted) {
            std::cerr << "learned candidate was not promoted to first place\n";
            return 9;
        }
    }

    if (!std::filesystem::is_regular_file(user_dict_path)) {
        std::wcerr << L"异步用户词典未在析构前完成保存: " << user_dict_path << L"\n";
        return 6;
    }

    {
        PinyinEngine reloaded;
        if (!reloaded.Initialize(lexicon) ||
            !VerifyCandidate(reloaded, learned_pinyin, learned_word)) {
            std::wcerr << L"用户词典重新加载失败\n";
            return 7;
        }
    }

    fs::remove_all(test_local_app_data, cleanup_error);
    std::wcout << L"engine_phase1: OK\n";
    return 0;
}
