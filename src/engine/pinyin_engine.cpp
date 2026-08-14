#include "pinyin_engine.h"

#include "fuzzy_pinyin.h"
#include "pinyin_correction.h"
#include "pinyin_syllables.h"
#include "pinyin_lattice.h"
#include "shuangpin.h"
#include "special_input.h"

#include <Windows.h>
#include <cstring>

#include "../common/logger.h"
#include "../common/private_acl.h"
#include "../common/runtime_config.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace shuru {
namespace {

bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring GetWritableUserDictPath(const std::wstring& lexicon_dir) {
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (length == 0) {
        return lexicon_dir + L"\\user_dict.txt";
    }
    std::wstring local_app_data(static_cast<size_t>(length), L'\0');
    const DWORD written = GetEnvironmentVariableW(
        L"LOCALAPPDATA", &local_app_data[0], length);
    if (written == 0 || written >= length) {
        return lexicon_dir + L"\\user_dict.txt";
    }
    local_app_data.resize(written);
    return local_app_data + L"\\CaishenPinyin\\data\\lexicon\\user_dict.txt";
}

struct CsGuard {
    CRITICAL_SECTION* cs;
    explicit CsGuard(CRITICAL_SECTION* c) : cs(c) { EnterCriticalSection(cs); }
    ~CsGuard() { LeaveCriticalSection(cs); }
    CsGuard(const CsGuard&) = delete;
    CsGuard& operator=(const CsGuard&) = delete;
};

class NamedMutexLock {
public:
    NamedMutexLock(const wchar_t* name, DWORD timeout) {
        mutex_ = CreateMutexW(nullptr, FALSE, name);
        if (mutex_ == nullptr) {
            return;
        }
        const DWORD wait = WaitForSingleObject(mutex_, timeout);
        owns_mutex_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }

    ~NamedMutexLock() {
        if (owns_mutex_) {
            ReleaseMutex(mutex_);
        }
        if (mutex_ != nullptr) {
            CloseHandle(mutex_);
        }
    }

    bool owns_mutex() const { return owns_mutex_; }

    NamedMutexLock(const NamedMutexLock&) = delete;
    NamedMutexLock& operator=(const NamedMutexLock&) = delete;

private:
    HANDLE mutex_ = nullptr;
    bool owns_mutex_ = false;
};

bool IsBmpChineseWord(const std::wstring& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch >= L'\x4e00' && ch <= L'\x9fff';
    });
}

bool IsSyllableAligned(const Candidate& candidate) {
    if (!IsBmpChineseWord(candidate.text)) return true;
    const auto paths = pinyin_data::BuildSyllableLattice(candidate.pinyin, 16);
    return std::any_of(paths.begin(), paths.end(), [&](const auto& path) {
        return path.complete && path.covered == candidate.pinyin.size() &&
            path.edges.size() == candidate.text.size();
    });
}

int SourcePriority(
    const Candidate& candidate, bool prefer_single_edit_correction = false) {
    switch (candidate.source) {
    case CandidateSource::Dynamic: return 0;
    case CandidateSource::Exact: return 1;
    case CandidateSource::WordGraph: return 2;
    case CandidateSource::MixedSentence:
        return prefer_single_edit_correction ? 6 : 3;
    // 键前缀/补全预测优先于纠错：zhengc 下「正常」（键前缀）不应被
    // 纠错变体「政策」无条件压制，纠错是编辑距离兜底手段。
    case CandidateSource::Prefix: return 4;
    // 单次编辑的纠错可信度高于碎片化混拼；多次编辑仍作为兜底。
    case CandidateSource::Correction:
        return prefer_single_edit_correction && candidate.correction_edit_cost > 1
            ? 7 : 5;
    case CandidateSource::Jianpin: return prefer_single_edit_correction ? 8 : 6;
    case CandidateSource::Mixed: return prefer_single_edit_correction ? 9 : 7;
    case CandidateSource::Fuzzy: return prefer_single_edit_correction ? 10 : 8;
    case CandidateSource::English: return prefer_single_edit_correction ? 11 : 9;
    case CandidateSource::LiteralMixed: return prefer_single_edit_correction ? 12 : 10;
    case CandidateSource::CustomPhrase: return 0;
    case CandidateSource::Raw: return prefer_single_edit_correction ? 13 : 11;
    }
    return 12;
}

}  // namespace

PinyinEngine::PinyinEngine() {
    InitializeCriticalSection(&lock_);
    lock_ready_ = true;
    save_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (save_event_ != nullptr) {
        save_thread_ = CreateThread(nullptr, 0, &PinyinEngine::SaveThreadProc, this, 0, nullptr);
        if (save_thread_ == nullptr) {
            CloseHandle(save_event_);
            save_event_ = nullptr;
            SHURU_LOG_WARN("user dictionary async save thread unavailable");
        }
    }
}

PinyinEngine::~PinyinEngine() {
    if (save_thread_ != nullptr) {
        InterlockedExchange(&save_stop_, 1);
        if (save_event_ != nullptr) {
            SetEvent(save_event_);
        }
        WaitForSingleObject(save_thread_, INFINITE);
        CloseHandle(save_thread_);
        save_thread_ = nullptr;
    }
    if (save_event_ != nullptr) {
        CloseHandle(save_event_);
        save_event_ = nullptr;
    }
    if (lock_ready_) {
        DeleteCriticalSection(&lock_);
        lock_ready_ = false;
    }
}

bool PinyinEngine::IsReady() const {
    CsGuard guard(&lock_);
    return ready_;
}

bool PinyinEngine::FuzzyEnabled() const {
    CsGuard guard(&lock_);
    return fuzzy_enabled_;
}

void PinyinEngine::SetFuzzyConfig(const FuzzyConfig& config) {
    CsGuard guard(&lock_);
    fuzzy_config_ = config;
}

FuzzyConfig PinyinEngine::GetFuzzyConfig() const {
    CsGuard guard(&lock_);
    return fuzzy_config_;
}

InputSchema PinyinEngine::GetInputSchema() const {
    CsGuard guard(&lock_);
    return schema_;
}

void PinyinEngine::SetQueryOptions(const QueryOptions& options) {
    CsGuard guard(&lock_);
    schema_ = options.schema;
    fuzzy_enabled_ = options.fuzzy_enabled;
    fuzzy_config_ = options.fuzzy_config;
}

QueryOptions PinyinEngine::GetQueryOptions() const {
    CsGuard guard(&lock_);
    QueryOptions options;
    options.schema = schema_;
    options.fuzzy_enabled = fuzzy_enabled_;
    options.fuzzy_config = fuzzy_config_;
    return options;
}

std::wstring PinyinEngine::user_dict_path() const {
    CsGuard guard(&lock_);
    return user_dict_path_;
}

std::wstring PinyinEngine::custom_phrase_path() const {
    CsGuard guard(&lock_);
    return custom_phrase_path_;
}

DWORD WINAPI PinyinEngine::SaveThreadProc(LPVOID param) {
    auto* self = static_cast<PinyinEngine*>(param);
    if (self == nullptr || self->save_event_ == nullptr) {
        return 1;
    }

    for (;;) {
        if (WaitForSingleObject(self->save_event_, INFINITE) != WAIT_OBJECT_0) {
            return 1;
        }
        // 合并短时间内连续上屏，避免每个字都触发一次完整词典写盘。
        if (InterlockedCompareExchange(&self->save_stop_, 0, 0) == 0) {
            WaitForSingleObject(self->save_event_, 250);
        }

        const bool stopping = InterlockedCompareExchange(&self->save_stop_, 0, 0) != 0;
        UserDictSnapshot snapshot;
        if (self->CaptureUserDictSnapshot(&snapshot)) {
            std::vector<UserDictionaryEntry> external_entries;
            const bool succeeded = PersistUserDictSnapshot(snapshot, &external_entries);
            self->CompleteUserDictSave(snapshot, external_entries, succeeded);
            if (!succeeded) {
                SHURU_LOG_WARN("async user dictionary save failed");
            }
        }

        // 用户 bigram 借同一保存线程与去抖节奏落盘；快照不可变，写盘在锁外。
        std::shared_ptr<const UserBigramModel> bigram_to_save;
        std::wstring bigram_path;
        {
            CsGuard guard(&self->lock_);
            if (self->bigram_dirty_ && self->bigram_ && !self->bigram_path_.empty()) {
                bigram_to_save = self->bigram_;
                bigram_path = self->bigram_path_;
                self->bigram_dirty_ = false;
            }
        }
        if (bigram_to_save && !bigram_to_save->SaveToFile(bigram_path)) {
            SHURU_LOG_WARN("user bigram save failed");
        }

        if (stopping) {
            return 0;
        }
    }
}

bool PinyinEngine::Initialize(const std::wstring& lexicon_dir) {
    // 词库解析和索引构建可能持续数百毫秒，必须在查询锁外完成。
    // 加载成功后仅用一次短临界区发布完整快照，查询线程要么看到旧状态，要么看到新状态。
    std::shared_ptr<LexiconSnapshot> loaded_lexicon;
    std::shared_ptr<UserLexiconSnapshot> loaded_user_lexicon;
    std::shared_ptr<CustomPhraseDictionary> loaded_custom_phrases;
    try {
        loaded_lexicon = std::make_shared<LexiconSnapshot>();
        loaded_user_lexicon = std::make_shared<UserLexiconSnapshot>();
        loaded_custom_phrases = std::make_shared<CustomPhraseDictionary>();
    } catch (...) {
        SHURU_LOG_ERROR("PinyinEngine init allocation failed; previous snapshot retained");
        return false;
    }
    Dictionary& loaded_dictionary = loaded_lexicon->dictionary;
    Dictionary& loaded_user_dictionary = loaded_user_lexicon->dictionary;
    EnglishDictionary& loaded_english_dictionary = loaded_lexicon->english_dictionary;
    const std::wstring base = lexicon_dir + L"\\base_dict.txt";
    const std::wstring legacy_user_dict_path = lexicon_dir + L"\\user_dict.txt";
    const std::wstring loaded_user_dict_path = GetWritableUserDictPath(lexicon_dir);
    const std::wstring loaded_custom_phrase_path = GetCustomPhrasePath(lexicon_dir);
    const bool user_path_private = EnsureCurrentUserOnlyPath(loaded_user_dict_path, false);
    if (!user_path_private) {
        SHURU_LOG_WARN("user dictionary ACL hardening unavailable; learning writes disabled");
    }

    // 基础词库 + 单字库 + 单字反推共用一次批量装载：期间跳过逐条排序与
    // 索引维护，结束时统一排序并只重建一次简拼/trie 索引。
    loaded_dictionary.BeginBulkLoad();
    const bool base_ok = loaded_dictionary.LoadFromFile(base, false);
    // 完整单字库（含多音字）
    const std::wstring chars = lexicon_dir + L"\\char_dict.txt";
    if (FileExists(chars)) {
        const bool char_ok = loaded_dictionary.LoadFromFile(chars, false);
        SHURU_LOG_INFO("char_dict load %s", char_ok ? "ok" : "fail");
    } else {
        SHURU_LOG_WARN("char_dict.txt missing, fallback derive-only");
    }
    if (base_ok) {
        loaded_dictionary.DeriveSingleCharacters();
    }
    loaded_dictionary.EndBulkLoad();
    // 英文单词词库
    const std::wstring en_path = lexicon_dir + L"\\en_dict.txt";
    if (FileExists(en_path)) {
        const bool en_ok = loaded_english_dictionary.LoadFromFile(en_path);
        SHURU_LOG_INFO("en_dict load %s size=%zu", en_ok ? "ok" : "fail", loaded_english_dictionary.Size());
    } else {
        SHURU_LOG_WARN("en_dict.txt missing");
    }
    const std::wstring language_model_path = lexicon_dir + L"\\system_ngram.bin";
    if (FileExists(language_model_path)) {
        const bool model_ok = loaded_lexicon->language_model.LoadFromFile(
            language_model_path);
        SHURU_LOG_INFO(
            "system language model load %s bigrams=%zu trigrams=%zu",
            model_ok ? "ok" : "fail",
            loaded_lexicon->language_model.bigram_size(),
            loaded_lexicon->language_model.trigram_size());
    } else {
        SHURU_LOG_WARN("system_ngram.bin missing, mixed sentence ranking degraded");
    }
    const std::wstring lexeme_prior_path =
        lexicon_dir + L"\\system_lexeme_prior.bin";
    if (FileExists(lexeme_prior_path)) {
        const bool prior_ok = loaded_lexicon->lexeme_prior_model.LoadFromFile(
            lexeme_prior_path);
        SHURU_LOG_INFO(
            "system lexeme prior load %s records=%zu",
            prior_ok ? "ok" : "fail",
            loaded_lexicon->lexeme_prior_model.size());
    } else {
        SHURU_LOG_WARN(
            "system_lexeme_prior.bin missing, short candidate ranking degraded");
    }

    if (FileExists(loaded_user_dict_path)) {
        loaded_user_dictionary.LoadFromFile(loaded_user_dict_path, true);
    } else if (legacy_user_dict_path != loaded_user_dict_path && FileExists(legacy_user_dict_path)) {
        // 兼容旧版本安装目录词典，首次学习时迁移到用户可写目录。
        loaded_user_dictionary.LoadFromFile(legacy_user_dict_path, true);
        SHURU_LOG_INFO("legacy user dictionary loaded for migration");
    } else {
        SHURU_LOG_INFO("user dict not found, will create on learn");
    }

    if (!loaded_custom_phrases->LoadFromFile(loaded_custom_phrase_path)) {
        SHURU_LOG_WARN("custom phrase file could not be read; using empty snapshot");
        loaded_custom_phrases = std::make_shared<CustomPhraseDictionary>();
    }

    // 用户 bigram 与用户词典同目录；缺失/损坏时从空模型开始。
    std::shared_ptr<UserBigramModel> loaded_bigram;
    std::wstring loaded_bigram_path;
    try {
        loaded_bigram = std::make_shared<UserBigramModel>();
        const std::filesystem::path user_dict_file(loaded_user_dict_path);
        loaded_bigram_path = (user_dict_file.parent_path() / L"user_bigram.txt").wstring();
        if (FileExists(loaded_bigram_path)) {
            loaded_bigram->LoadFromFile(loaded_bigram_path);
        }
    } catch (...) {
        loaded_bigram = std::make_shared<UserBigramModel>();
    }

    if (!base_ok) {
        // 失败不清空已发布快照：宿主可继续使用旧词库，后台稍后重试加载。
        SHURU_LOG_ERROR("PinyinEngine init failed; previous snapshot retained");
        return false;
    }

    loaded_user_dictionary.clear_dirty();
    const size_t dictionary_size = loaded_dictionary.Size() + loaded_user_dictionary.Size();
    const size_t jianpin_size = loaded_dictionary.JianpinSize();
    bool fuzzy_enabled = true;
    {
        CsGuard guard(&lock_);
        // 发布基础词库快照；Query 只在锁内复制 shared_ptr，检索、模糊音和排序
        // 均在锁外执行，因此多个 TextService 查询不会彼此串行等待。
        lexicon_ = std::move(loaded_lexicon);
        user_lexicon_ = std::move(loaded_user_lexicon);
        custom_phrases_ = std::move(loaded_custom_phrases);
        bigram_ = std::move(loaded_bigram);
        bigram_path_ = loaded_bigram_path;
        bigram_dirty_ = false;
        user_dict_revision_ = 0;
        lexicon_dir_ = lexicon_dir;
        user_dict_path_ = loaded_user_dict_path;
        custom_phrase_path_ = loaded_custom_phrase_path;
        user_dict_writable_ = user_path_private;
        ready_ = true;
        fuzzy_enabled = fuzzy_enabled_;
    }
    SHURU_LOG_INFO("PinyinEngine ready, dict_size=%zu jianpin=%zu fuzzy=%d",
                   dictionary_size, jianpin_size, fuzzy_enabled ? 1 : 0);
    return true;
}

bool PinyinEngine::ReloadCustomPhrases() {
    std::wstring path;
    {
        CsGuard guard(&lock_);
        path = custom_phrase_path_.empty() ? GetCustomPhrasePath(lexicon_dir_) : custom_phrase_path_;
    }
    std::shared_ptr<CustomPhraseDictionary> loaded;
    try {
        loaded = std::make_shared<CustomPhraseDictionary>();
    } catch (...) {
        return false;
    }
    if (!loaded->LoadFromFile(path)) return false;
    {
        CsGuard guard(&lock_);
        custom_phrases_ = std::move(loaded);
        custom_phrase_path_ = path;
    }
    return true;
}

void PinyinEngine::SetFuzzyEnabled(bool enabled) {
    CsGuard guard(&lock_);
    fuzzy_enabled_ = enabled;
}

void PinyinEngine::SetInputSchema(InputSchema schema) {
    CsGuard guard(&lock_);
    schema_ = schema;
    SHURU_LOG_INFO("InputSchema=%d", static_cast<int>(schema_));
}

bool PinyinEngine::IsPinyinLetter(wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
}

std::string PinyinEngine::NormalizeInput(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char ch : input) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if (ch >= 'a' && ch <= 'z') {
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\'') {
            out.push_back('\'');
        }
    }
    return out;
}

bool PinyinEngine::LooksLikeJianpin(const std::string& pinyin) {
    if (pinyin.size() < 2) {
        return false;
    }
    for (char c : pinyin) {
        if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'v') {
            return false;
        }
    }
    return true;
}

std::string PinyinEngine::ResolveQueryPinyin(const std::string& raw_input, std::string* preview) const {
    const std::string norm = NormalizeInput(raw_input);
    if (schema_ == InputSchema::ShuangpinXiaohe) {
        return DecodeXiaoheShuangpin(norm, preview);
    }
    if (preview) {
        *preview = norm;
    }
    return norm;
}

std::wstring PinyinEngine::FormatComposingDisplay(const std::string& raw_input) const {
    std::vector<MixedInputSegment> mixed_segments;
    if (IsCalculatorInput(raw_input) || ParseMixedInput(raw_input, &mixed_segments)) {
        return std::wstring(raw_input.begin(), raw_input.end());
    }
    CsGuard guard(&lock_);
    std::string preview;
    ResolveQueryPinyin(raw_input, &preview);
    if (schema_ == InputSchema::ShuangpinXiaohe) {
        // 双拼：码 + 全拼预览
        std::wstring show(raw_input.begin(), raw_input.end());
        if (!preview.empty()) {
            show += L" → ";
            show += std::wstring(preview.begin(), preview.end());
        }
        return show;
    }
    return std::wstring(preview.begin(), preview.end());
}

std::string PinyinEngine::ToFullPinyinForLearn(const std::string& raw_input) const {
    CsGuard guard(&lock_);
    return ResolveQueryPinyin(raw_input, nullptr);
}

EngineQueryResult PinyinEngine::Query(const std::string& raw_input, size_t limit) const {
    QueryOptions options;
    {
        CsGuard guard(&lock_);
        options.schema = schema_;
        options.fuzzy_enabled = fuzzy_enabled_;
        options.fuzzy_config = fuzzy_config_;
    }
    return Query(raw_input, limit, options);
}

EngineQueryResult PinyinEngine::Query(const std::string& raw_input, size_t limit,
                                      const QueryOptions& options) const {
    EngineQueryResult result;
    if (limit == 0) return result;
    std::shared_ptr<LexiconSnapshot> lexicon;
    std::shared_ptr<UserLexiconSnapshot> user_lexicon;
    std::shared_ptr<CustomPhraseDictionary> custom_phrases;
    const bool fuzzy_enabled = options.fuzzy_enabled;
    FuzzyConfig fuzzy_config = options.fuzzy_config;
    const InputSchema schema = options.schema;
    std::string repeat_pinyin;
    std::wstring repeat_text;
    int repeat_count = 0;
    std::shared_ptr<const UserBigramModel> bigram;
    {
        CsGuard guard(&lock_);
        if (!ready_ || !lexicon_ || !user_lexicon_) return result;
        lexicon = lexicon_; user_lexicon = user_lexicon_; custom_phrases = custom_phrases_;
        bigram = bigram_;
        repeat_pinyin = repeat_selection_pinyin_;
        repeat_text = repeat_selection_text_;
        repeat_count = repeat_selection_count_;
    }
    const std::wstring& context = options.context;
    auto bigram_count = [&](const std::wstring& previous, const std::wstring& next) {
        return bigram ? bigram->Count(previous, next) : 0;
    };
    auto apply_lexeme_prior = [&](Candidate& candidate) {
        if (candidate.lexeme_prior == 0 && !candidate.pinyin.empty() &&
            !candidate.text.empty()) {
            candidate.lexeme_prior = lexicon->lexeme_prior_model.Lookup(
                candidate.pinyin, candidate.text);
        }
    };
    auto ranking_frequency = [&](const Candidate& candidate) {
        return candidate.lexeme_prior != 0
            ? static_cast<double>(candidate.lexeme_prior)
            : static_cast<double>((std::max)(0, candidate.frequency));
    };

    if (schema == InputSchema::Quanpin && IsCalculatorInput(raw_input) &&
        raw_input.size() > 1) {
        result.candidates = BuildCalculatorCandidates(raw_input);
        if (!result.candidates.empty()) result.matched_pinyin_len = raw_input.size();
        return result;
    }

    std::vector<MixedInputSegment> mixed_segments;
    if (schema == InputSchema::Quanpin && ParseMixedInput(raw_input, &mixed_segments)) {
        struct MixedPath {
            std::wstring text;
            double score = 0.0;
            std::vector<std::pair<std::string, std::wstring>> chosen;
        };
        std::vector<MixedPath> paths(1);
        for (const auto& segment : mixed_segments) {
            if (segment.literal) {
                for (auto& path : paths)
                    path.text.append(segment.text.begin(), segment.text.end());
                continue;
            }
            const auto segment_result = Query(segment.text, (std::min)(size_t{4}, limit), options);
            std::vector<const Candidate*> choices;
            for (const auto& candidate : segment_result.candidates) {
                if (candidate.covered_input_len == segment.text.size() &&
                    candidate.source != CandidateSource::Raw &&
                    candidate.source != CandidateSource::Dynamic) {
                    choices.push_back(&candidate);
                    if (choices.size() >= 4) break;
                }
            }
            if (choices.empty()) return result;
            std::vector<MixedPath> expanded;
            expanded.reserve((std::min)(size_t{64}, paths.size() * choices.size()));
            for (const auto& path : paths) {
                for (std::size_t choice = 0; choice < choices.size(); ++choice) {
                    MixedPath next = path;
                    next.text += choices[choice]->text;
                    next.score += choices[choice]->ranking_score - static_cast<double>(choice) * 10.0;
                    next.chosen.push_back({choices[choice]->pinyin, choices[choice]->text});
                    expanded.push_back(std::move(next));
                    if (expanded.size() >= 64) break;
                }
                if (expanded.size() >= 64) break;
            }
            std::sort(expanded.begin(), expanded.end(), [](const MixedPath& left, const MixedPath& right) {
                if (left.score != right.score) return left.score > right.score;
                return left.text < right.text;
            });
            paths = std::move(expanded);
        }
        std::unordered_set<std::wstring> seen;
        for (const auto& path : paths) {
            if (!seen.insert(path.text).second) continue;
            Candidate candidate;
            candidate.text = path.text;
            candidate.pinyin = raw_input;
            candidate.covered_input_len = raw_input.size();
            candidate.source = CandidateSource::LiteralMixed;
            // 整体没有合法拼音串，改为按拼音子段学习：选过一次「L站」后，
            // zhan 段的「站」获得用户词加成，下次混输直接排前。
            candidate.learnable = true;
            candidate.learn_segments = path.chosen;
            candidate.ranking_score = path.score;
            result.candidates.push_back(std::move(candidate));
            if (result.candidates.size() >= limit) break;
        }
        if (!result.candidates.empty()) result.matched_pinyin_len = raw_input.size();
        return result;
    }

    std::vector<Candidate> dynamic_candidates = BuildCurrentTimeCandidates(raw_input);
    const std::string normalized = NormalizeInput(raw_input);
    std::string preview;
    std::string query = schema == InputSchema::ShuangpinXiaohe
        ? DecodeXiaoheShuangpin(pinyin_data::RemoveSyllableSeparators(normalized), &preview)
        : normalized;
    if (schema == InputSchema::Quanpin) {
        preview = normalized;
        // lue/nue 是 lve/nve 的常用替代拼写（词库与音节表统一用 v 形式），
        // 等长替换，不影响覆盖长度的对应关系。
        for (size_t pos = query.find("lue"); pos != std::string::npos; pos = query.find("lue", pos + 1))
            query[pos + 1] = 'v';
        for (size_t pos = query.find("nue"); pos != std::string::npos; pos = query.find("nue", pos + 1))
            query[pos + 1] = 'v';
    }
    if (query.empty()) return result;
    const std::string compact = pinyin_data::RemoveSyllableSeparators(query);
    const auto lattice = pinyin_data::BuildSyllableLattice(query);
    const bool single_complete_syllable = schema == InputSchema::Quanpin &&
        pinyin_data::Syllables().count(compact) != 0;

    std::vector<Candidate> pool;
    bool prefer_single_edit_correction = false;
    auto score = [&](Candidate& c) {
        apply_lexeme_prior(c);
        const double coverage = query.empty() ? 0.0 : double(c.covered_input_len) / double(query.size());
        // 上文搭配加成：刚上屏「发财」后，baofu 的「暴富」应压过更高频的「报复」。
        const double context_bonus = context.empty()
            ? 0.0
            : double((std::min)(3, bigram_count(context, c.text))) * 90.0;
        c.ranking_score = coverage * 10000.0 - double(c.segment_count > 0 ? c.segment_count - 1 : 0) * 55.0
            + std::log1p(ranking_frequency(c)) * 28.0
            - double(c.match_cost) * 0.20 + double((std::min)(90, c.learning_score)) * 2.0
            + (c.from_user && c.learning_score > 0 ? 400.0 : 0.0)
            + context_bonus + c.language_score * 1.5;
    };
    auto better = [&](const Candidate& a, const Candidate& b) {
        // 覆盖全部输入的候选优先于部分覆盖：zhengt 的补全「整体/整天」应
        // 排在词图只覆盖 zheng 的「正」之前，haoduoc 的「好多次」应排在
        // 只覆盖 haoduo 的「好多」之前。
        const bool full_a = a.covered_input_len >= query.size();
        const bool full_b = b.covered_input_len >= query.size();
        if (full_a != full_b) return full_a;
        const int source_a = SourcePriority(a, prefer_single_edit_correction);
        const int source_b = SourcePriority(b, prefer_single_edit_correction);
        if (source_a != source_b) return source_a < source_b;
        if (a.source == CandidateSource::Correction &&
            b.source == CandidateSource::Correction &&
            a.segment_count != b.segment_count) {
            return a.segment_count < b.segment_count;
        }
        if (a.ranking_score != b.ranking_score) return a.ranking_score > b.ranking_score;
        if (a.covered_input_len != b.covered_input_len) return a.covered_input_len > b.covered_input_len;
        if (a.match_cost != b.match_cost) return a.match_cost < b.match_cost;
        if (a.lexeme_prior != b.lexeme_prior)
            return a.lexeme_prior > b.lexeme_prior;
        if (a.frequency != b.frequency) return a.frequency > b.frequency;
        if (a.pinyin != b.pinyin) return a.pinyin < b.pinyin;
        return a.text < b.text;
    };
    std::unordered_map<std::wstring, size_t> by_text;
    // 旧版可能在打过的前缀下学到整词（duan -> 短剑，c -> 词）。这类行保留
    // 在盘上以便恢复，但结构非法，绝不能进入排序或词图边。
    auto acceptable_user_candidate = [&](const Candidate& c) {
        if (!c.from_user) return true;
        if (!IsSyllableAligned(c)) return false;
        return !lexicon->dictionary.ContainsWord(c.text) ||
               lexicon->dictionary.ContainsWordPinyin(c.text, c.pinyin);
    };
    std::string correction_display_segmentation;
    int active_correction_edit_cost = 0;
    auto add = [&](std::vector<Candidate> items, size_t covered, int cost, size_t segments = 1,
                   bool enforce_spelling_boundaries = true,
                   CandidateSource source = CandidateSource::Exact) {
        for (auto& c : items) {
            c.covered_input_len = (std::min)(covered, query.size());
            c.match_cost = cost;
            // Exact abbreviation spelling outranks longer abbreviation prefixes.
            if (cost == 70 && pinyin_data::ToJianpin(c.pinyin) == compact)
                c.match_cost = 0;
            c.segment_count = (std::max)(size_t{1}, segments);
            c.source = source;
            if (source == CandidateSource::Correction &&
                c.correction_edit_cost == 0) {
                c.correction_edit_cost = active_correction_edit_cost;
            }
            if (source == CandidateSource::Correction &&
                c.input_segmentation.empty() && !correction_display_segmentation.empty()) {
                c.input_segmentation = correction_display_segmentation;
            }
            if (!acceptable_user_candidate(c)) continue;
            if (enforce_spelling_boundaries &&
                !pinyin_data::CandidateRespectsHardBoundaries(query.substr(0, c.covered_input_len), c)) continue;
            score(c);
            const auto found = by_text.find(c.text);
            if (found == by_text.end()) { by_text[c.text] = pool.size(); pool.push_back(std::move(c)); }
            else if (better(c, pool[found->second])) pool[found->second] = std::move(c);
        }
    };

    // Exact/prefix candidates consume what the user typed, never the dictionary suffix.
    add(user_lexicon->dictionary.LookupExact(compact), query.size(), 0);
    add(lexicon->dictionary.LookupExact(compact), query.size(), 0);
    add(user_lexicon->dictionary.LookupPrefix(compact, limit), query.size(), 25, 1, true, CandidateSource::Prefix);
    add(lexicon->dictionary.LookupPrefix(compact, limit), query.size(), 25, 1, true, CandidateSource::Prefix);
    // 全拼/声母混合恢复需要扫描更多词典状态。已有字面精确或前缀结果时，
    // 普通输入应留在索引路径，例如输入 renzhen 过程中的 renz。
    const bool has_literal_candidate = !pool.empty();
    if (schema == InputSchema::Quanpin) {
        add(user_lexicon->dictionary.LookupJianpin(compact, limit), query.size(), 70, 1, true, CandidateSource::Jianpin);
        add(lexicon->dictionary.LookupJianpin(compact, limit), query.size(), 70, 1, true, CandidateSource::Jianpin);
        // Mixed full/initial matching is an abbreviation feature, not fuzzy recovery.
        // Disable it for incomplete syllable spellings (for example mhu), otherwise
        // strict snapshots still admit mohu and the broad mixed pool can crowd out
        // the deliberately costed missing-vowel candidate.
        bool complete_spelling = false;
        for (const auto& path : lattice) {
            if (path.complete && path.covered == query.size()) {
                complete_spelling = true;
                break;
            }
        }
        const bool mixed_abbreviation = !has_literal_candidate &&
            !complete_spelling && compact.size() >= 4;
        if (mixed_abbreviation) {
            add(user_lexicon->dictionary.LookupMixed(compact, limit), query.size(), 45, 1, true, CandidateSource::Mixed);
            add(lexicon->dictionary.LookupMixed(compact, limit), query.size(), 45, 1, true, CandidateSource::Mixed);
        }
    }

    // 词图只沿合法音节边界移动。纠错变体复用同一个有界实现，避免为每个
    // 变体扫描整张词典。
    // 路径打分在对数域累积联合概率：每增加一段要付出 kSegmentLogTotal 的
    // 归一化代价，多段组合的字频虚高（好+度+哦 平均上百万）不再压过真正
    // 的整词（好多）。
    constexpr double kSegmentLogTotal = 17.0;
    auto joint_frequency = [&](double log_freq_sum, size_t segments) {
        const double joint = segments <= 1
            ? log_freq_sum
            : log_freq_sum - kSegmentLogTotal * static_cast<double>(segments - 1);
        const double clamped = (std::max)(0.0, (std::min)(20.0, joint));
        return static_cast<int>(std::llround(std::expm1(clamped)));
    };
    struct Path { std::wstring text; std::wstring last_word; double log_freq=0.0; int learning=0; size_t segments=0; bool from_user=false; };
    auto add_word_graph = [&](const std::string& graph_query,
                              CandidateSource source,
                              int full_cost,
                              bool enforce_boundaries) {
        const auto graph_lattice = pinyin_data::BuildSyllableLattice(graph_query);
        std::vector<std::vector<Path>> paths(graph_query.size() + 1);
        // 空路径以会话上文为「前词」，使 bigram 对句首词也生效。
        paths[0].push_back({std::wstring(), context, 0.0, 0, 0, false});
        std::vector<size_t> legal_ends;
        std::vector<bool> is_legal_end(graph_query.size() + 1, false);
        for (const auto& lattice_path : graph_lattice) {
            for (const auto& edge : lattice_path.edges) {
                if (edge.end <= graph_query.size() && !is_legal_end[edge.end]) {
                    is_legal_end[edge.end] = true;
                    legal_ends.push_back(edge.end);
                }
            }
        }
        std::sort(legal_ends.begin(), legal_ends.end());
        for (size_t begin = 0; begin < graph_query.size(); ++begin) {
            if (graph_query[begin] == '\'' && !paths[begin].empty()) {
                paths[begin + 1] = paths[begin];
                continue;
            }
            if (paths[begin].empty()) continue;
            for (const size_t endpos : legal_ends) {
                if (endpos <= begin) continue;
                const size_t quote = graph_query.find('\'', begin);
                if (quote != std::string::npos && quote < endpos) continue;
                const std::string edge_pinyin = pinyin_data::RemoveSyllableSeparators(
                    graph_query.substr(begin, endpos - begin));
                auto edges = user_lexicon->dictionary.LookupExact(edge_pinyin);
                auto base = lexicon->dictionary.LookupExact(edge_pinyin);
                edges.insert(edges.end(), base.begin(), base.end());
                // 脏用户行（如旧版学到的 c -> 词）会伪造出直达输入末尾的词边，
                // 既挤掉合法切分又阻断尾部补全，必须在成边前剔除。
                edges.erase(std::remove_if(edges.begin(), edges.end(), [&](const Candidate& c) {
                    return !acceptable_user_candidate(c);
                }), edges.end());
                for (auto& edge : edges) apply_lexeme_prior(edge);
                std::sort(edges.begin(), edges.end(), [&](const Candidate& left, const Candidate& right) {
                    if (left.from_user != right.from_user)
                        return left.from_user > right.from_user;
                    if (left.learning_score != right.learning_score)
                        return left.learning_score > right.learning_score;
                    const double left_frequency = ranking_frequency(left);
                    const double right_frequency = ranking_frequency(right);
                    if (left_frequency != right_frequency)
                        return left_frequency > right_frequency;
                    return left.text < right.text;
                });
                if (edges.size() > 4) edges.resize(4);
                for (const auto& prefix : paths[begin]) {
                    for (const auto& edge : edges) {
                        // 用户搭配折算进对数频率：一次计数约等于频率 ×12，
                        // 「发财→暴富」学习一次即可在同段数路径内胜出。
                        const int pair_count = bigram_count(prefix.last_word, edge.text);
                        const double bigram_boost = pair_count > 0
                            ? 2.5 * double((std::min)(3, pair_count))
                            : 0.0;
                        paths[endpos].push_back({
                            prefix.text + edge.text,
                            edge.text,
                            prefix.log_freq + std::log1p(ranking_frequency(edge)) + bigram_boost,
                            prefix.learning + edge.learning_score,
                            prefix.segments + 1,
                            prefix.from_user || edge.from_user,
                        });
                    }
                }
                auto& bucket = paths[endpos];
                std::sort(bucket.begin(), bucket.end(), [](const Path& left, const Path& right) {
                    if (left.segments != right.segments) return left.segments < right.segments;
                    if (left.learning != right.learning) return left.learning > right.learning;
                    if (left.log_freq != right.log_freq) return left.log_freq > right.log_freq;
                    return left.text < right.text;
                });
                if (bucket.size() > 128) bucket.resize(128);
            }
        }
        size_t covered = graph_query.size();
        while (covered > 0 && paths[covered].empty()) --covered;
        if (source == CandidateSource::Correction && covered != graph_query.size()) {
            return false;
        }
        // 尾部声母/不完整音节补全：haoduoc 的 c 补出「好多次/好多词」，
        // zhengt 的 t 补出「整体/整天」。把尾巴当作某个音节的前缀，用该
        // 前缀音节的高频单字接到已达路径上，生成覆盖全部输入的预测候选。
        if (source == CandidateSource::WordGraph && covered < graph_query.size() &&
            !paths[covered].empty()) {
            const std::string tail = pinyin_data::RemoveSyllableSeparators(
                graph_query.substr(covered));
            if (!tail.empty() && tail.size() <= 2 &&
                tail.find('\'') == std::string::npos) {
                struct TailWord {
                    std::wstring text;
                    std::string syllable;
                    int frequency = 0;
                    int learning = 0;
                    bool from_user = false;
                };
                std::vector<TailWord> tail_words;
                for (const auto& syllable : pinyin_data::Syllables()) {
                    if (syllable.size() <= tail.size() ||
                        syllable.compare(0, tail.size(), tail) != 0) continue;
                    auto matches = user_lexicon->dictionary.LookupExact(syllable);
                    auto base_matches = lexicon->dictionary.LookupExact(syllable);
                    matches.insert(matches.end(), base_matches.begin(), base_matches.end());
                    for (auto& match : matches) apply_lexeme_prior(match);
                    std::sort(matches.begin(), matches.end(), [&](
                        const Candidate& left, const Candidate& right) {
                        if (left.from_user != right.from_user)
                            return left.from_user > right.from_user;
                        if (left.learning_score != right.learning_score)
                            return left.learning_score > right.learning_score;
                        const double left_frequency = ranking_frequency(left);
                        const double right_frequency = ranking_frequency(right);
                        if (left_frequency != right_frequency)
                            return left_frequency > right_frequency;
                        return left.text < right.text;
                    });
                    size_t taken = 0;
                    for (const auto& match : matches) {
                        if (match.text.size() != 1) continue;
                        const int rank_frequency = static_cast<int>((std::min)(
                            ranking_frequency(match),
                            static_cast<double>((std::numeric_limits<int>::max)())));
                        tail_words.push_back({match.text, syllable, rank_frequency,
                                              match.learning_score, match.from_user});
                        if (++taken >= 2) break;
                    }
                }
                std::sort(tail_words.begin(), tail_words.end(),
                          [](const TailWord& left, const TailWord& right) {
                    if (left.learning != right.learning) return left.learning > right.learning;
                    if (left.frequency != right.frequency) return left.frequency > right.frequency;
                    return left.syllable < right.syllable;
                });
                if (tail_words.size() > 8) tail_words.resize(8);
                const std::string covered_pinyin = pinyin_data::RemoveSyllableSeparators(
                    graph_query.substr(0, covered));
                size_t used_paths = 0;
                for (const auto& path : paths[covered]) {
                    if (used_paths++ >= 2) break;
                    for (const auto& tail_word : tail_words) {
                        Candidate candidate;
                        candidate.text = path.text + tail_word.text;
                        candidate.pinyin = covered_pinyin + tail_word.syllable;
                        const size_t segments = path.segments + 1;
                        candidate.frequency = joint_frequency(
                            path.log_freq + std::log1p(double((std::max)(0, tail_word.frequency))),
                            segments);
                        candidate.learning_score = (std::min)(90, path.learning + tail_word.learning);
                        // 组合是引擎生造的预测，不继承「用户选过这个词」的
                        // from_user 加成，否则含高频学习字的任意组合都会置顶。
                        candidate.from_user = false;
                        // 补全的读音超出用户输入，无法按原字面校验音节边界。
                        add({candidate}, graph_query.size(), 30, segments,
                            false, CandidateSource::Prefix);
                    }
                }
            }
        }
        for (const auto& path : paths[covered]) {
            if (path.text.empty()) continue;
            Candidate candidate;
            candidate.text = path.text;
            candidate.pinyin = pinyin_data::RemoveSyllableSeparators(
                graph_query.substr(0, covered));
            candidate.frequency = joint_frequency(path.log_freq, path.segments);
            candidate.learning_score = (std::min)(90, path.learning);
            // 多段组合不继承 from_user：+400 加成只属于用户真正选过的整词。
            candidate.from_user = path.from_user && path.segments <= 1;
            const bool complete = covered == graph_query.size();
            add({candidate}, source == CandidateSource::Correction ? query.size() : covered,
                complete ? full_cost : full_cost + 25, path.segments,
                enforce_boundaries, source);
        }
        return covered == graph_query.size() && !paths[covered].empty();
    };

    add_word_graph(query, CandidateSource::WordGraph, 10, true);

    // 全拼与声母简写可在一句话中任意交错。词典的音节 Trie 只沿当前输入
    // 能匹配的分支前进；这里再以输入位置为节点组合多个词，避免旧
    // LookupMixed 对整张词典扫描且只能命中一个词的限制。
    bool has_complete_mixed_path = false;
    int best_complete_mixed_cost = (std::numeric_limits<int>::max)();
    if (schema == InputSchema::Quanpin && !has_literal_candidate &&
        query.find('\'') == std::string::npos && compact.size() >= 4 &&
        compact.size() <= 48) {
        struct MixedPath {
            std::wstring text;
            std::wstring last_word;
            std::string full_pinyin;
            std::string segmented_input;
            std::vector<std::pair<std::string, std::wstring>> learn_segments;
            double log_frequency = 0.0;
            int learning = 0;
            size_t words = 0;
            size_t syllables = 0;
            size_t abbreviated = 0;
            size_t omitted_letters = 0;
            double language_score = 0.0;
            bool from_user = false;
        };
        constexpr size_t kMixedBeamWidth = 64;
        std::vector<std::vector<MixedPath>> mixed_paths(compact.size() + 1);
        mixed_paths[0].push_back({std::wstring(), context});

        constexpr double kOmittedLetterCost = 0.50;
        constexpr double kAbbreviatedWordCost = 1.50;
        auto mixed_path_quality = [&](const MixedPath& path) {
            const double segment_cost = path.words <= 1
                ? 0.0
                : kSegmentLogTotal * static_cast<double>(path.words - 1);
            return path.log_frequency - segment_cost +
                static_cast<double>((std::min)(90, path.learning)) / 14.0 -
                kOmittedLetterCost * static_cast<double>(path.omitted_letters) -
                kAbbreviatedWordCost * static_cast<double>(path.abbreviated) +
                path.language_score * 0.32;
        };
        auto mixed_path_better = [&](const MixedPath& left, const MixedPath& right) {
            const double left_quality = mixed_path_quality(left);
            const double right_quality = mixed_path_quality(right);
            if (left_quality != right_quality) return left_quality > right_quality;
            if (left.words != right.words) return left.words < right.words;
            if (left.abbreviated != right.abbreviated)
                return left.abbreviated < right.abbreviated;
            return left.text < right.text;
        };

        for (size_t begin = 0; begin < compact.size(); ++begin) {
            auto& prefixes = mixed_paths[begin];
            if (prefixes.empty()) continue;
            std::sort(prefixes.begin(), prefixes.end(), mixed_path_better);
            if (prefixes.size() > kMixedBeamWidth) prefixes.resize(kMixedBeamWidth);

            const std::string remaining = compact.substr(begin);
            constexpr size_t kMixedEdgeLimit = 128;
            auto matches = user_lexicon->dictionary.LookupMixedPrefixes(
                remaining, kMixedEdgeLimit);
            auto base_matches = lexicon->dictionary.LookupMixedPrefixes(
                remaining, kMixedEdgeLimit);
            matches.insert(matches.end(),
                           std::make_move_iterator(base_matches.begin()),
                           std::make_move_iterator(base_matches.end()));
            for (auto& match : matches) apply_lexeme_prior(match.candidate);

            std::sort(matches.begin(), matches.end(), [&](const auto& left, const auto& right) {
                if (left.consumed_input != right.consumed_input)
                    return left.consumed_input < right.consumed_input;
                if (left.candidate.text != right.candidate.text)
                    return left.candidate.text < right.candidate.text;
                if (left.candidate.from_user != right.candidate.from_user)
                    return left.candidate.from_user > right.candidate.from_user;
                if (left.candidate.learning_score != right.candidate.learning_score)
                    return left.candidate.learning_score > right.candidate.learning_score;
                const double left_frequency = ranking_frequency(left.candidate);
                const double right_frequency = ranking_frequency(right.candidate);
                if (left_frequency != right_frequency)
                    return left_frequency > right_frequency;
                return left.candidate.pinyin < right.candidate.pinyin;
            });
            matches.erase(std::unique(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
                return left.consumed_input == right.consumed_input &&
                    left.candidate.text == right.candidate.text;
            }), matches.end());

            for (const auto& match : matches) {
                if (match.consumed_input == 0 ||
                    begin + match.consumed_input > compact.size() ||
                    !acceptable_user_candidate(match.candidate)) {
                    continue;
                }
                const size_t end = begin + match.consumed_input;
                for (const auto& prefix : prefixes) {
                    MixedPath next = prefix;
                    const int pair_count = bigram_count(
                        prefix.last_word, match.candidate.text);
                    const double learned_pair_boost = pair_count > 0
                        ? 2.5 * double((std::min)(3, pair_count))
                        : 0.0;
                    const double system_language_boost =
                        lexicon->language_model.AppendScore(
                            prefix.text, match.candidate.text);
                    next.text += match.candidate.text;
                    next.last_word = match.candidate.text;
                    next.full_pinyin += match.candidate.pinyin;
                    if (!next.segmented_input.empty() &&
                        !match.segmented_input.empty()) {
                        next.segmented_input.push_back('\'');
                    }
                    next.segmented_input += match.segmented_input;
                    next.learn_segments.push_back({
                        match.candidate.pinyin, match.candidate.text});
                    // 反推单字会继承其所在长词的峰值词频，可能达到千万级；
                    // 该值适合保证单字可见，不适合直接当作整句语言概率。
                    // 混拼长句中的单字仍要封顶；否则“下/力”等超高单字先验
                    // 会压过“学校/美女”这类完整词边，破坏既有长句模型。
                    const double mixed_frequency = match.candidate.text.size() == 1
                        ? (std::min)(600000.0, ranking_frequency(match.candidate))
                        : ranking_frequency(match.candidate);
                    next.log_frequency += std::log1p(
                        (std::max)(0.0, mixed_frequency)) +
                        learned_pair_boost;
                    next.language_score += system_language_boost;
                    next.learning += match.candidate.learning_score;
                    ++next.words;
                    next.syllables += match.syllable_count;
                    next.abbreviated += match.abbreviated_syllables;
                    next.omitted_letters += match.omitted_letters;
                    next.from_user = next.from_user || match.candidate.from_user;
                    mixed_paths[end].push_back(std::move(next));
                }
                auto& destination = mixed_paths[end];
                if (destination.size() > kMixedBeamWidth * 4) {
                    std::sort(destination.begin(), destination.end(), mixed_path_better);
                    destination.resize(kMixedBeamWidth);
                }
            }
        }

        auto& complete_paths = mixed_paths.back();
        std::sort(complete_paths.begin(), complete_paths.end(), mixed_path_better);
        if (complete_paths.size() > kMixedBeamWidth)
            complete_paths.resize(kMixedBeamWidth);
        const size_t mixed_candidate_quota = (std::min)(
            size_t {32}, (std::max)(size_t {4}, limit / 2));
        size_t added_mixed_candidates = 0;
        for (const auto& path : complete_paths) {
            if (path.text.empty() || path.abbreviated == 0) continue;
            Candidate candidate;
            candidate.text = path.text;
            candidate.pinyin = path.full_pinyin;
            candidate.input_segmentation = path.segmented_input;
            candidate.learn_segments = path.learn_segments;
            candidate.frequency = joint_frequency(path.log_frequency, path.words);
            candidate.language_score = path.language_score;
            candidate.learning_score = (std::min)(90, path.learning);
            candidate.from_user = path.from_user && path.words <= 1;
            const int mixed_cost = 25 +
                static_cast<int>(path.abbreviated) * 4 +
                static_cast<int>(path.omitted_letters) * 9;
            best_complete_mixed_cost = (std::min)(best_complete_mixed_cost, mixed_cost);
            add({candidate}, query.size(), mixed_cost, path.words,
                false, CandidateSource::MixedSentence);
            has_complete_mixed_path = true;
            if (++added_mixed_candidates >= mixed_candidate_quota) break;
        }
    }

    const bool has_complete_exact_path = std::any_of(pool.begin(), pool.end(), [&](const Candidate& candidate) {
        return candidate.covered_input_len == query.size() &&
            (candidate.source == CandidateSource::Exact ||
             candidate.source == CandidateSource::WordGraph);
    });
    constexpr int kWeakMixedSentenceCost = 80;
    const bool should_try_correction = !has_complete_mixed_path ||
        best_complete_mixed_cost >= kWeakMixedSentenceCost;
    if (schema == InputSchema::Quanpin && !has_complete_exact_path &&
        should_try_correction &&
        query.find('\'') == std::string::npos && compact.size() >= 5) {
        PinyinCorrectionLimits correction_limits;
        if (has_complete_mixed_path) correction_limits.max_total_cost = 1;
        // 最终只展示至多四个纠错候选。保留两倍候选供词频排序即可，
        // 避免为默认 24 个拼写变体逐一重复构建完整词图。
        correction_limits.max_results = 8;
        for (const auto& correction :
             GeneratePinyinCorrections(compact, correction_limits)) {
            // 纯尾部删除的变体等价于「输入还没打完」：zhengt 打字进行中会被
            // 解释成「多打了 t 的 zheng」，把 zheng 的单字顶成全覆盖候选。
            // 这类场景交给前缀补全处理，纠错只保留中间位置的编辑。
            if (correction.pinyin.size() < compact.size() &&
                compact.compare(0, correction.pinyin.size(), correction.pinyin) == 0) {
                continue;
            }
            const int correction_cost = 80 + correction.cost * 20;
            const bool previous_preference = prefer_single_edit_correction;
            if (has_complete_mixed_path && correction.cost == 1) {
                prefer_single_edit_correction = true;
            }
            correction_display_segmentation = correction.input_segmentation;
            active_correction_edit_cost = correction.cost;
            auto add_correction_exact = [&](std::vector<Candidate> items) {
                for (auto& candidate : items) {
                    candidate.correction_edit_cost = correction.cost;
                }
                add(std::move(items), query.size(), correction_cost,
                    correction.syllable_count, false, CandidateSource::Correction);
            };
            add_correction_exact(user_lexicon->dictionary.LookupExact(correction.pinyin));
            add_correction_exact(lexicon->dictionary.LookupExact(correction.pinyin));
            add_word_graph(correction.pinyin, CandidateSource::Correction,
                           correction_cost, false);
            if (!previous_preference &&
                std::none_of(pool.begin(), pool.end(), [](const Candidate& candidate) {
                    return candidate.source == CandidateSource::Correction &&
                        candidate.correction_edit_cost == 1;
                })) {
                prefer_single_edit_correction = false;
            }
            active_correction_edit_cost = 0;
            correction_display_segmentation.clear();
            if (pool.size() > limit * 8) break;
        }
    }

    // Fuzzy variants are generated from each retained segmentation, with bounded cost/work.
    if (fuzzy_enabled && query.find('\'')==std::string::npos) {
        // Missing-vowel recovery must not be starved by the general variant ranking cap.
        // Probe its small, deterministic space directly (<= 6 * (n + 1)), then run the
        // weighted initial/final expansion. This restores e.g. mhu -> mohu while bounded.
        if (fuzzy_config.missing_vowel) {
            static constexpr char kVowels[] = "aeiouv";
            size_t recovery_work = 0;
            for (size_t pos = 0; pos <= compact.size() && recovery_work < 64; ++pos) {
                for (const char* vowel = kVowels; *vowel && recovery_work < 64; ++vowel, ++recovery_work) {
                    std::string recovered = compact;
                    recovered.insert(recovered.begin() + static_cast<std::string::difference_type>(pos), *vowel);
                    // Exact lookup is authoritative; greedy syllable segmentation can reject
                    // valid overlapping recoveries such as mohu.
                    add(user_lexicon->dictionary.LookupExact(recovered),query.size(),fuzzy_config.missing_vowel_cost,1,false,CandidateSource::Fuzzy);
                    add(lexicon->dictionary.LookupExact(recovered),query.size(),fuzzy_config.missing_vowel_cost,1,false,CandidateSource::Fuzzy);
                }
            }
        }
        fuzzy_config.max_variants=(std::min)(size_t{24},fuzzy_config.max_variants);
        for(const auto& variant:ExpandFuzzyPinyinWeighted(compact,fuzzy_config)) if(variant.pinyin!=compact) {
            // Fuzzy recovery intentionally changes spelling, so validate only explicit apostrophe
            // boundaries (already excluded above), not the original literal syllable spelling.
            add(user_lexicon->dictionary.LookupExact(variant.pinyin),query.size(),variant.cost,1,false,CandidateSource::Fuzzy);
            add(lexicon->dictionary.LookupExact(variant.pinyin),query.size(),variant.cost,1,false,CandidateSource::Fuzzy);
            // Exact indexed lookup above is sufficient for recovered full spellings;
            // do not scan the dictionary for every fuzzy variant.
            if(pool.size()>limit*16)break;
        }
    }
    // Longest valid prefix is candidate-local partial coverage.
    for(size_t n=query.size(); n>0; --n) {
        if(query[n-1]=='\'')continue; std::string prefix=pinyin_data::RemoveSyllableSeparators(query.substr(0,n));
        auto u=user_lexicon->dictionary.LookupExact(prefix); auto b=lexicon->dictionary.LookupExact(prefix);
        if (u.empty() && b.empty() && fuzzy_enabled && schema == InputSchema::Quanpin && compact.size() == 4 && n == 3) {
            u = user_lexicon->dictionary.LookupMixed(prefix, limit);
            b = lexicon->dictionary.LookupMixed(prefix, limit);
        }
        if(!u.empty()||!b.empty()){add(std::move(u),n,40,1,true,CandidateSource::Prefix);add(std::move(b),n,40,1,true,CandidateSource::Prefix);break;}
    }
    if(schema==InputSchema::Quanpin && !lexicon->english_dictionary.empty() && compact.size()>=2) {
        add(lexicon->english_dictionary.LookupExact(compact),query.size(),0,1,true,CandidateSource::English);
        add(lexicon->english_dictionary.LookupPrefix(compact,limit),query.size(),30,1,true,CandidateSource::English);
    }
    if(pool.empty()){Candidate raw;raw.text=std::wstring(preview.begin(),preview.end());raw.pinyin=compact;add({raw},0,1000,1,true,CandidateSource::Raw);}
    std::sort(pool.begin(),pool.end(),better);
    // 纠错结果优先于部分匹配，但不能占满候选集合。输入尾部暂时无效时
    // 仍需保留最长合法前缀，供用户先提交已有中文再继续输入。
    {
        const size_t correction_quota = (std::min)(size_t{4}, limit);
        size_t retained_corrections = 0;
        pool.erase(std::remove_if(pool.begin(), pool.end(), [&](const Candidate& candidate) {
            if (candidate.source != CandidateSource::Correction) return false;
            return retained_corrections++ >= correction_quota;
        }), pool.end());
    }
    if (single_complete_syllable) {
        auto is_exact_single = [](const Candidate& candidate) {
            return candidate.source == CandidateSource::Exact && IsBmpChineseWord(candidate.text) &&
                candidate.text.size() == 1;
        };
        auto is_dictionary_phrase = [](const Candidate& candidate) {
            return IsBmpChineseWord(candidate.text) && candidate.text.size() > 1 &&
                (candidate.source == CandidateSource::Exact ||
                 candidate.source == CandidateSource::Prefix);
        };
        auto is_graph_phrase = [](const Candidate& candidate) {
            return IsBmpChineseWord(candidate.text) && candidate.text.size() > 1 &&
                (candidate.source == CandidateSource::WordGraph ||
                 candidate.source == CandidateSource::MixedSentence);
        };
        std::vector<Candidate> ordered;
        ordered.reserve(pool.size());
        std::vector<bool> moved(pool.size(), false);
        size_t reserved_singles = 0;
        for (size_t i = 0; i < pool.size() && reserved_singles < 4; ++i) {
            if (is_exact_single(pool[i])) {
                ordered.push_back(std::move(pool[i]));
                moved[i] = true;
                ++reserved_singles;
            }
        }
        // 一页九项时形成“4 个常用单字 + 5 个词组”；更大的内部查询也只让
        // 有限数量的词组插队，避免扩库后数百个前缀词永久淹没其余单字。
        const size_t phrase_quota = limit > reserved_singles
            ? (std::min)(size_t{5}, limit - reserved_singles)
            : 0;
        size_t preferred_phrases = 0;
        for (size_t i = 0; i < pool.size() && preferred_phrases < phrase_quota; ++i) {
            if (!moved[i] && is_dictionary_phrase(pool[i])) {
                ordered.push_back(std::move(pool[i]));
                moved[i] = true;
                ++preferred_phrases;
            }
        }
        for (size_t i = 0; i < pool.size() && preferred_phrases < phrase_quota; ++i) {
            if (!moved[i] && is_graph_phrase(pool[i])) {
                ordered.push_back(std::move(pool[i]));
                moved[i] = true;
                ++preferred_phrases;
            }
        }
        // 第一页之后继续列出词典词组，再进入生僻单字。否则内部 90 项预算
        // 会被同音单字耗尽，用户永远翻不到“想法/想象/想要”等前缀词。
        for (size_t i = 0; i < pool.size(); ++i) {
            if (!moved[i] && is_dictionary_phrase(pool[i])) {
                ordered.push_back(std::move(pool[i]));
                moved[i] = true;
            }
        }
        for (size_t i = 0; i < pool.size(); ++i) {
            if (!moved[i] && is_exact_single(pool[i])) {
                ordered.push_back(std::move(pool[i]));
                moved[i] = true;
            }
        }
        for (size_t i = 0; i < pool.size(); ++i) {
            if (!moved[i]) {
                ordered.push_back(std::move(pool[i]));
            }
        }
        pool = std::move(ordered);
    }
    // 同一输入码下连续选择两次同一候选后直接置顶（短期记忆，主流输入法行为）。
    if (repeat_count >= 2 && repeat_pinyin == compact) {
        for (size_t i = 1; i < pool.size(); ++i) {
            if (pool[i].text == repeat_text) {
                std::rotate(pool.begin(), pool.begin() + i, pool.begin() + i + 1);
                break;
            }
        }
    }
    if(pool.size()>limit)pool.resize(limit);
    if (!dynamic_candidates.empty()) {
        std::unordered_set<std::wstring> dynamic_text;
        std::vector<Candidate> combined;
        combined.reserve(dynamic_candidates.size() + pool.size());
        for (auto& candidate : dynamic_candidates) {
            dynamic_text.insert(candidate.text);
            combined.push_back(std::move(candidate));
        }
        for (auto& candidate : pool) {
            if (dynamic_text.insert(candidate.text).second)
                combined.push_back(std::move(candidate));
        }
        if (combined.size() > limit) combined.resize(limit);
        pool = std::move(combined);
    }
    if (custom_phrases) {
        InsertCustomPhraseCandidates(
            custom_phrases->LookupExact(normalized), normalized,
            raw_input.size(), limit, &pool);
    }
    if (schema == InputSchema::Quanpin) {
        std::unordered_map<size_t, std::vector<pinyin_data::SyllablePath>>
            segmentation_lattices;
        for (auto& candidate : pool) {
            if (!candidate.input_segmentation.empty() ||
                candidate.text.size() < 2 || candidate.covered_input_len == 0) continue;
            const size_t covered = (std::min)(candidate.covered_input_len, query.size());
            auto found = segmentation_lattices.find(covered);
            if (found == segmentation_lattices.end()) {
                found = segmentation_lattices.emplace(
                    covered,
                    pinyin_data::BuildSyllableLattice(query.substr(0, covered))).first;
            }
            candidate.input_segmentation =
                pinyin_data::BuildCandidateInputSegmentationFromLattice(
                    query.substr(0, covered), found->second, candidate);
        }
    }
    for(const auto& c:pool) result.matched_pinyin_len=(std::max)(result.matched_pinyin_len,c.covered_input_len);
    result.candidates=std::move(pool); return result;
}

bool PinyinEngine::CaptureUserDictSnapshot(UserDictSnapshot* snapshot) {
    if (snapshot == nullptr) {
        return false;
    }
    CsGuard guard(&lock_);
    if (!user_lexicon_ || !user_lexicon_->dictionary.dirty() || user_dict_path_.empty()) {
        return false;
    }
    snapshot->path = user_dict_path_;
    snapshot->entries = user_lexicon_->dictionary.SnapshotUserEntries();
    snapshot->revision = user_dict_revision_;
    return true;
}

void PinyinEngine::CompleteUserDictSave(
    const UserDictSnapshot& snapshot,
    const std::vector<UserDictionaryEntry>& external_entries,
    bool succeeded) {
    if (!succeeded) {
        return;
    }
    CsGuard guard(&lock_);
    if (!user_lexicon_) {
        return;
    }
    if (user_lexicon_.use_count() != 1) {
        try {
            user_lexicon_ = std::make_shared<UserLexiconSnapshot>(*user_lexicon_);
        } catch (...) {
            SHURU_LOG_WARN("user dictionary merge allocation failed");
            return;
        }
    }
    user_lexicon_->dictionary.ImportUserEntries(external_entries);
    if (snapshot.revision == user_dict_revision_) {
        user_lexicon_->dictionary.clear_dirty();
    }
}

bool PinyinEngine::PersistUserDictSnapshot(
    const UserDictSnapshot& snapshot,
    std::vector<UserDictionaryEntry>* external_entries) {
    if (snapshot.path.empty() || external_entries == nullptr) {
        return false;
    }

    NamedMutexLock dictionary_mutex(L"Local\\CaishenPinyin.UserDictionary", 5000);
    if (!dictionary_mutex.owns_mutex()) {
        return false;
    }

    Dictionary merged_dictionary;
    merged_dictionary.ImportUserEntries(snapshot.entries);

    std::vector<UserDictionaryEntry> disk_entries;
    if (FileExists(snapshot.path)) {
        Dictionary disk_dictionary;
        if (disk_dictionary.LoadFromFile(snapshot.path, true)) {
            disk_entries = disk_dictionary.SnapshotUserEntries();
            merged_dictionary.ImportUserEntries(disk_entries);
        }
    }

    std::map<std::pair<std::string, std::wstring>, UserDictionaryEntry> local_frequencies;
    for (const auto& entry : snapshot.entries) {
        local_frequencies[{entry.pinyin, entry.word}] = entry;
    }
    external_entries->clear();
    for (const auto& entry : disk_entries) {
        const auto local = local_frequencies.find({entry.pinyin, entry.word});
        if (local == local_frequencies.end() || entry.frequency > local->second.frequency || entry.selection_count > local->second.selection_count || entry.last_used_unix > local->second.last_used_unix) {
            external_entries->push_back(entry);
        }
    }

    return merged_dictionary.SaveUserToFile(snapshot.path);
}

bool PinyinEngine::ScheduleUserDictSave() {
    if (save_thread_ != nullptr && save_event_ != nullptr) {
        if (SetEvent(save_event_)) {
            return true;
        }
    }
    return false;
}

void PinyinEngine::ObserveBigram(const std::wstring& previous, const std::wstring& next) {
    if (previous.empty() || next.empty()) return;
    bool scheduled = false;
    {
        CsGuard guard(&lock_);
        const RuntimeConfig config = GetRuntimeConfig();
        if (!config.learning_enabled || !ready_ || !bigram_) return;
        const std::int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // 快照写时复制：查询线程继续读旧模型，锁内只做小模型拷贝与发布。
        std::shared_ptr<UserBigramModel> updated;
        try {
            updated = std::make_shared<UserBigramModel>(*bigram_);
        } catch (...) {
            return;
        }
        updated->Observe(previous, next, now_unix);
        bigram_ = std::move(updated);
        bigram_dirty_ = true;
        scheduled = ScheduleUserDictSave();
    }
    (void)scheduled;
}

std::vector<Candidate> PinyinEngine::PredictNext(
    const std::wstring& context, size_t limit) const {
    std::vector<Candidate> out;
    if (context.empty() || limit == 0) return out;
    std::shared_ptr<const UserBigramModel> bigram;
    {
        CsGuard guard(&lock_);
        if (!ready_ || !bigram_) return out;
        bigram = bigram_;
    }
    for (const auto& successor : bigram->Successors(context, limit)) {
        Candidate candidate;
        candidate.text = successor.text;
        candidate.frequency = successor.count;
        candidate.source = CandidateSource::Dynamic;
        candidate.learnable = false;  // 联想选择经 ObserveBigram 强化，不入用户词库
        out.push_back(std::move(candidate));
    }
    return out;
}

void PinyinEngine::Learn(const std::string& pinyin, const std::wstring& word) {
    bool scheduled = false;
    {
        CsGuard guard(&lock_);
        const RuntimeConfig config = GetRuntimeConfig();
        if (!config.learning_enabled || !user_dict_writable_ ||
            !ready_ || !user_lexicon_ || pinyin.empty() || word.empty()) {
            return;
        }
        // 基础词库永久只读；学习只复制通常很小的用户覆盖层，避免复制完整索引。
        if (user_lexicon_.use_count() != 1) {
            try {
                user_lexicon_ = std::make_shared<UserLexiconSnapshot>(*user_lexicon_);
            } catch (...) {
                SHURU_LOG_WARN("user dictionary learn allocation failed");
                return;
            }
        }
        const std::string normalized_pinyin = NormalizeInput(pinyin);
        const int base_frequency = lexicon_
            ? lexicon_->dictionary.LookupFrequency(normalized_pinyin, word)
            : 0;
        user_lexicon_->dictionary.IncreaseUserWord(
            normalized_pinyin, word, 20, base_frequency);
        last_learned_pinyin_ = normalized_pinyin;
        last_learned_word_ = word;
        if (repeat_selection_pinyin_ == normalized_pinyin &&
            repeat_selection_text_ == word) {
            ++repeat_selection_count_;
        } else {
            repeat_selection_pinyin_ = normalized_pinyin;
            repeat_selection_text_ = word;
            repeat_selection_count_ = 1;
        }
        if (user_lexicon_->dictionary.dirty()) {
            ++user_dict_revision_;
            scheduled = ScheduleUserDictSave();
        }
    }
    if (scheduled) {
        return;
    }

    // 后台线程不可用时仍保证学习数据落盘，但文件操作不占用查询锁。
    UserDictSnapshot snapshot;
    if (CaptureUserDictSnapshot(&snapshot)) {
        std::vector<UserDictionaryEntry> external_entries;
        const bool succeeded = PersistUserDictSnapshot(snapshot, &external_entries);
        CompleteUserDictSave(snapshot, external_entries, succeeded);
        if (!succeeded) {
            SHURU_LOG_WARN("PersistUserDict fallback failed");
        }
    }
}

bool PinyinEngine::UndoLastLearning() {
    bool changed = false;
    {
        CsGuard guard(&lock_);
        if (!user_lexicon_ || last_learned_pinyin_.empty() || last_learned_word_.empty()) return false;
        if (user_lexicon_.use_count() != 1) {
            try { user_lexicon_ = std::make_shared<UserLexiconSnapshot>(*user_lexicon_); }
            catch (...) { return false; }
        }
        changed = user_lexicon_->dictionary.DecreaseUserWord(
            last_learned_pinyin_, last_learned_word_, 20);
        if (changed) {
            ++user_dict_revision_;
            ScheduleUserDictSave();
        }
        last_learned_pinyin_.clear();
        last_learned_word_.clear();
    }
    return changed;
}

bool PinyinEngine::ExportUserDictionary(const std::wstring& path) const {
    Dictionary snapshot;
    {
        CsGuard guard(&lock_);
        if (!user_lexicon_ || path.empty()) return false;
        snapshot.ImportUserEntries(user_lexicon_->dictionary.SnapshotUserEntries());
    }
    return snapshot.SaveUserToFile(path);
}

bool PinyinEngine::ImportUserDictionary(const std::wstring& path) {
    Dictionary imported;
    if (!imported.LoadFromFile(path, true)) return false;
    {
        CsGuard guard(&lock_);
        if (!user_lexicon_ || !user_dict_writable_) return false;
        if (user_lexicon_.use_count() != 1) {
            try { user_lexicon_ = std::make_shared<UserLexiconSnapshot>(*user_lexicon_); }
            catch (...) { return false; }
        }
        user_lexicon_->dictionary.ImportUserEntries(imported.SnapshotUserEntries());
        // ImportUserEntries represents persisted state; force this merged state to disk.
        for (const auto& entry : imported.SnapshotUserEntries())
            user_lexicon_->dictionary.IncreaseUserWord(entry.pinyin, entry.word, 1, entry.frequency - 1);
        ++user_dict_revision_;
        ScheduleUserDictSave();
    }
    return true;
}

bool PinyinEngine::ClearUserDictionary() {
    {
        CsGuard guard(&lock_);
        if (!user_lexicon_ || !user_dict_writable_) return false;
        user_lexicon_ = std::make_shared<UserLexiconSnapshot>();
        user_lexicon_->dictionary.ClearUserEntries();
        ++user_dict_revision_;
        last_learned_pinyin_.clear();
        last_learned_word_.clear();
        ScheduleUserDictSave();
    }
    return true;
}

}  // namespace shuru
