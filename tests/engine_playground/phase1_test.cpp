#include "engine/pinyin_engine.h"
#include "engine/pinyin_lattice.h"
#include "engine/dictionary.h"
#include "common/com_utils.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

bool VerifyAllCharactersReachable(
    shuru::PinyinEngine& engine,
    const std::filesystem::path& character_dictionary_path) {
    std::ifstream input(character_dictionary_path, std::ios::binary);
    if (!input) return false;

    std::unordered_map<std::string, std::unordered_set<std::wstring>> expected;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const size_t first_tab = line.find('\t');
        const size_t second_tab = first_tab == std::string::npos
            ? std::string::npos
            : line.find('\t', first_tab + 1);
        if (first_tab == std::string::npos || second_tab == std::string::npos)
            return false;
        const std::string pinyin = line.substr(0, first_tab);
        const std::wstring character = shuru::Utf8ToWide(
            line.substr(first_tab + 1, second_tab - first_tab - 1));
        if (!IsSingleBmpCharacter(character)) return false;
        expected[pinyin].insert(character);
    }

    size_t missing_count = 0;
    for (const auto& item : expected) {
        const auto result = engine.Query(item.first, 256);
        std::unordered_set<std::wstring> actual;
        for (const auto& candidate : result.candidates) {
            if (candidate.source == shuru::CandidateSource::Exact &&
                IsSingleBmpCharacter(candidate.text)) {
                actual.insert(candidate.text);
            }
        }
        for (const auto& character : item.second) {
            if (actual.count(character) != 0) continue;
            if (missing_count < 12) {
                std::cerr << "unreachable character: pinyin=" << item.first
                          << " text=" << shuru::WideToUtf8(character) << '\n';
            }
            ++missing_count;
        }
    }
    if (missing_count != 0) {
        std::cerr << "unreachable character count=" << missing_count << '\n';
        return false;
    }
    return true;
}

bool VerifyCorrection(
    shuru::PinyinEngine& engine,
    const std::string& input,
    const std::wstring& expected) {
    const auto started = std::chrono::steady_clock::now();
    const auto result = engine.Query(input, 9);
    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (latency <= std::chrono::milliseconds(1500)) {
        for (const auto& candidate : result.candidates) {
            if (candidate.text == expected &&
                candidate.source == shuru::CandidateSource::Correction &&
                candidate.covered_input_len == input.size() &&
                candidate.correction_edit_cost > 0) {
                return true;
            }
        }
    }
    std::cerr << "correction assertion failed: input=" << input
              << " expected=" << shuru::WideToUtf8(expected)
              << " latency=" << latency.count() << "ms actual=";
    for (const auto& candidate : result.candidates) {
        std::cerr << shuru::WideToUtf8(candidate.text)
                  << "[py=" << candidate.pinyin
                  << " source=" << static_cast<int>(candidate.source)
                  << " edits=" << candidate.correction_edit_cost
                  << " language=" << candidate.language_score
                  << " score=" << candidate.ranking_score << "],";
    }
    std::cerr << '\n';
    return false;
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
    for (const auto& candidate : result.candidates) {
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
                  << "[" << candidate.input_segmentation << "]"
                  << " f=" << candidate.frequency
                  << " ls=" << candidate.language_score
                  << " seg=" << candidate.segment_count
                  << " cost=" << candidate.match_cost
                  << " rank=" << candidate.ranking_score << ',';
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
        const auto xiao = engine.Query("xiao", 256);
        const auto tong = engine.Query("tong", 256);
        if (!ContainsText(xiao, L"晓") || !ContainsText(tong, L"彤") ||
            !VerifyAllCharactersReachable(
                engine, fs::path(lexicon) / L"char_dict.txt")) {
            std::cerr << "complete single-character reachability failed\n";
            return 30;
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
            [](const Candidate& candidate) {
                return candidate.text.size() >= 2 &&
                    candidate.input_segmentation == "ying'w";
            });
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
            auto dump_segmentation = [](const char* input,
                                        const EngineQueryResult& value) {
                std::cerr << input << '=';
                for (const auto& candidate : value.candidates) {
                    std::cerr << WideToUtf8(candidate.text) << '['
                              << candidate.input_segmentation << ",source="
                              << static_cast<int>(candidate.source) << "],";
                }
                std::cerr << '\n';
            };
            std::cerr << "candidate input segmentation failed\n";
            dump_segmentation("womenzhidao", segmented_sentence);
            dump_segmentation("haoduoc", predicted_suffix);
            dump_segmentation("yingw", segmented_double_partial);
            dump_segmentation("renzhen", segmented_double_exact);
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
        if (duan.candidates.size() < 6 ||
            !std::all_of(
                duan.candidates.begin(), duan.candidates.begin() + 6,
                [](const Candidate& candidate) {
                    return IsSingleBmpCharacter(candidate.text) && candidate.pinyin == "duan";
                }) ||
            !ContainsTextInFirst(duan, L"短", 6)) {
            std::cerr << "single-syllable candidates were crowded out: ";
            for (const auto& candidate : duan.candidates) {
                std::cerr << WideToUtf8(candidate.text) << ',';
            }
            std::cerr << '\n';
            return 13;
        }

        const auto wan = engine.Query("wan", 9);
        if (wan.candidates.size() < 6 ||
            !std::all_of(
                wan.candidates.begin(), wan.candidates.begin() + 6,
                [](const Candidate& candidate) {
                    return IsSingleBmpCharacter(candidate.text) &&
                        candidate.pinyin == "wan" &&
                        candidate.source != CandidateSource::Correction;
                })) {
            std::cerr << "complete wan syllable did not keep six singles\n";
            return 33;
        }

        // 完整单音节先展示 6 个单字；一旦进入第二音节，覆盖全部已输入
        // 字母的多字词应提前，首音节单字和纠错均排在其后。
        const std::vector<std::string> partial_tail_inputs = {
            "wanq", "renz", "haod"};
        for (const auto& input : partial_tail_inputs) {
            const auto ranked = engine.Query(input, 9);
            if (ranked.candidates.size() < 7) {
                std::cerr << "partial-tail candidate count too small: " << input << '\n';
                return 33;
            }
            const auto lattice = pinyin_data::BuildSyllableLattice(input);
            const auto path = std::find_if(
                lattice.begin(), lattice.end(), [](const auto& value) {
                    return value.covered > 0 && value.edges.size() >= 2 &&
                        value.edges.back().partial;
                });
            if (path == lattice.end()) {
                std::cerr << "partial-tail lattice missing: " << input << '\n';
                return 33;
            }
            const auto is_related_phrase = [&](const Candidate& candidate) {
                return candidate.text.size() >= 2 &&
                    candidate.source != CandidateSource::Correction &&
                    candidate.covered_input_len >= input.size() &&
                    candidate.pinyin.size() >= input.size() &&
                    candidate.pinyin.compare(0, input.size(), input) == 0;
            };
            constexpr size_t related_prefix_count = 5;
            if (!std::all_of(
                    ranked.candidates.begin(),
                    ranked.candidates.begin() + related_prefix_count,
                    is_related_phrase)) {
                std::cerr << "partial-tail ranking failed: " << input << ' ';
                for (const auto& candidate : ranked.candidates) {
                    std::cerr << WideToUtf8(candidate.text) << '['
                              << static_cast<int>(candidate.source) << "],";
                }
                std::cerr << '\n';
                return 33;
            }
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
        std::cout << "bounded correction latency: "
                  << correction_latency.count() << " ms\n";
        if (corrected.candidates.empty() ||
            corrected.candidates.front().text != L"这是个纠错" ||
            corrected.candidates.front().source != CandidateSource::Correction ||
            correction_latency > std::chrono::milliseconds(300)) {
            std::cerr << "bounded correction failed: ";
            for (const auto& candidate : corrected.candidates) {
                std::cerr << WideToUtf8(candidate.text)
                          << "[py=" << candidate.pinyin
                          << " edits=" << candidate.correction_edit_cost
                          << " keyboard=" << candidate.correction_ranking_cost
                          << " seg=" << candidate.segment_count
                          << " cost=" << candidate.match_cost
                          << " f=" << candidate.frequency
                          << " ls=" << candidate.language_score
                          << " rank=" << candidate.ranking_score << "],";
            }
            std::cerr << " latency=" << correction_latency.count() << "ms\n";
            return 17;
        }

        const auto transposed = engine.Query("chognqi", 9);
        if (transposed.candidates.empty() ||
            transposed.candidates.front().text != L"重启" ||
            transposed.candidates.front().source != CandidateSource::Correction ||
            transposed.candidates.front().input_segmentation != "chogn'qi") {
            std::cerr << "transposition correction failed: ";
            for (const auto& candidate : transposed.candidates) {
                std::cerr << WideToUtf8(candidate.text) << "["
                          << candidate.input_segmentation << "],";
            }
            std::cerr << '\n';
            return 26;
        }
        const auto correct_transposed = engine.Query("chongqi", 9);
        if (correct_transposed.candidates.empty() ||
            correct_transposed.candidates.front().text != L"重启" ||
            correct_transposed.candidates.front().source != CandidateSource::Exact) {
            std::cerr << "correct spelling changed after correction support\n";
            return 27;
        }
        const auto duplicated = engine.Query("chonngqi", 9);
        if (duplicated.candidates.empty() ||
            duplicated.candidates.front().text != L"重启" ||
            duplicated.candidates.front().source != CandidateSource::Correction ||
            duplicated.candidates.front().input_segmentation != "chonng'qi") {
            std::cerr << "duplicate-letter correction failed\n";
            return 28;
        }
        const auto typing_prefix = engine.Query("zhengc", 9);
        if (typing_prefix.candidates.empty() ||
            typing_prefix.candidates.front().source == CandidateSource::Correction) {
            std::cerr << "correction displaced an in-progress prefix\n";
            return 29;
        }
        const std::vector<std::pair<std::string, std::wstring>> correction_cases = {
            {"nihoa", L"你好"},
            {"woemnzhidao", L"我们知道"},
            {"gognzuo", L"工作"},
            {"gonzuo", L"工作"},
            {"gongzzuo", L"工作"},
            {"gongzup", L"工作"},
            {"mingtain", L"明天"},
            {"xihuna", L"喜欢"},
            {"xihun", L"喜欢"},
            {"shenem", L"什么"},
            {"shme", L"什么"},
        };
        for (const auto& correction_case : correction_cases) {
            if (!VerifyCorrection(
                    engine, correction_case.first, correction_case.second)) {
                return 31;
            }
        }

        const std::vector<std::pair<std::string, std::wstring>> correct_cases = {
            {"nihao", L"你好"},
            {"womenzhidao", L"我们知道"},
            {"gongzuo", L"工作"},
            {"mingtian", L"明天"},
            {"xihuan", L"喜欢"},
            {"shenme", L"什么"},
        };
        for (const auto& correct_case : correct_cases) {
            const auto result = engine.Query(correct_case.first, 9);
            if (result.candidates.empty() ||
                result.candidates.front().text != correct_case.second ||
                result.candidates.front().source == CandidateSource::Correction) {
                std::cerr << "correct input was changed: "
                          << correct_case.first << '\n';
                return 32;
            }
        }

        const auto time_shortcut = engine.Query("sj", 9);
        if (time_shortcut.candidates.size() < 4 ||
            time_shortcut.candidates.front().source != CandidateSource::Dynamic ||
            time_shortcut.candidates.front().learnable) {
            std::cerr << "time shortcut did not rank first\n";
            return 18;
        }

        const auto calculator = engine.Query("vvv1+2*3", 9);
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
