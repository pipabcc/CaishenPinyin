#include "pinyin_engine.h"

#include "fuzzy_pinyin.h"
#include "pinyin_syllables.h"
#include "pinyin_lattice.h"
#include "shuangpin.h"

#include <Windows.h>
#include <cstring>

#include "../common/logger.h"
#include "../common/private_acl.h"
#include "../common/runtime_config.h"

#include <algorithm>
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
    return local_app_data + L"\\FacaiPinyin\\data\\lexicon\\user_dict.txt";
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

int SourcePriority(const Candidate& candidate) {
    switch (candidate.source) {
    case CandidateSource::Exact: return 1;
    case CandidateSource::Prefix: return 2;
    case CandidateSource::WordGraph: return 3;
    case CandidateSource::Jianpin: return 4;
    case CandidateSource::Mixed: return 5;
    case CandidateSource::Fuzzy: return 6;
    case CandidateSource::English: return 7;
    case CandidateSource::Raw: return 8;
    }
    return 8;
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
    try {
        loaded_lexicon = std::make_shared<LexiconSnapshot>();
        loaded_user_lexicon = std::make_shared<UserLexiconSnapshot>();
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
    const bool user_path_private = EnsureCurrentUserOnlyPath(loaded_user_dict_path, false);
    if (!user_path_private) {
        SHURU_LOG_WARN("user dictionary ACL hardening unavailable; learning writes disabled");
    }

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
    // 英文单词词库
    const std::wstring en_path = lexicon_dir + L"\\en_dict.txt";
    if (FileExists(en_path)) {
        const bool en_ok = loaded_english_dictionary.LoadFromFile(en_path);
        SHURU_LOG_INFO("en_dict load %s size=%zu", en_ok ? "ok" : "fail", loaded_english_dictionary.Size());
    } else {
        SHURU_LOG_WARN("en_dict.txt missing");
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
        user_dict_revision_ = 0;
        lexicon_dir_ = lexicon_dir;
        user_dict_path_ = loaded_user_dict_path;
        user_dict_writable_ = user_path_private;
        ready_ = true;
        fuzzy_enabled = fuzzy_enabled_;
    }
    SHURU_LOG_INFO("PinyinEngine ready, dict_size=%zu jianpin=%zu fuzzy=%d",
                   dictionary_size, jianpin_size, fuzzy_enabled ? 1 : 0);
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
    const bool fuzzy_enabled = options.fuzzy_enabled;
    FuzzyConfig fuzzy_config = options.fuzzy_config;
    const InputSchema schema = options.schema;
    {
        CsGuard guard(&lock_);
        if (!ready_ || !lexicon_ || !user_lexicon_) return result;
        lexicon = lexicon_; user_lexicon = user_lexicon_;
    }
    const std::string normalized = NormalizeInput(raw_input);
    std::string preview;
    const std::string query = schema == InputSchema::ShuangpinXiaohe
        ? DecodeXiaoheShuangpin(pinyin_data::RemoveSyllableSeparators(normalized), &preview)
        : normalized;
    if (schema == InputSchema::Quanpin) preview = normalized;
    if (query.empty()) return result;
    const std::string compact = pinyin_data::RemoveSyllableSeparators(query);
    const auto lattice = pinyin_data::BuildSyllableLattice(query);
    const bool single_complete_syllable = schema == InputSchema::Quanpin &&
        pinyin_data::Syllables().count(compact) != 0;

    std::vector<Candidate> pool;
    auto score = [&](Candidate& c) {
        const double coverage = query.empty() ? 0.0 : double(c.covered_input_len) / double(query.size());
        c.ranking_score = coverage * 10000.0 - double(c.segment_count > 0 ? c.segment_count - 1 : 0) * 55.0
            + std::log1p(double((std::max)(0, c.frequency))) * 28.0
            - double(c.match_cost) * 0.20 + double((std::min)(90, c.learning_score)) * 2.0
            + (c.from_user && c.learning_score > 0 ? 400.0 : 0.0);
    };
    auto better = [&](const Candidate& a, const Candidate& b) {
        const int source_a = SourcePriority(a);
        const int source_b = SourcePriority(b);
        if (source_a != source_b) return source_a < source_b;
        if (a.ranking_score != b.ranking_score) return a.ranking_score > b.ranking_score;
        if (a.covered_input_len != b.covered_input_len) return a.covered_input_len > b.covered_input_len;
        if (a.match_cost != b.match_cost) return a.match_cost < b.match_cost;
        if (a.frequency != b.frequency) return a.frequency > b.frequency;
        if (a.pinyin != b.pinyin) return a.pinyin < b.pinyin;
        return a.text < b.text;
    };
    std::unordered_map<std::wstring, size_t> by_text;
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
            // Older builds could learn a prefix prediction under the typed prefix
            // (duan -> 短剑). Such rows are kept on disk for recoverability, but are
            // structurally invalid and must never enter ranking or word-graph edges.
            if (c.from_user && (!IsSyllableAligned(c) ||
                (lexicon->dictionary.ContainsWord(c.text) &&
                 !lexicon->dictionary.ContainsWordPinyin(c.text, c.pinyin)))) continue;
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

    // Word graph only follows explicit lattice boundaries, so apostrophes are hard.
    struct Path { std::wstring text; int frequency=0; int learning=0; size_t segments=0; bool from_user=false; };
    std::vector<std::vector<Path>> paths(query.size()+1); paths[0].push_back({});
    std::vector<size_t> legal_ends;
    std::vector<bool> is_legal_end(query.size() + 1, false);
    for (const auto& lp : lattice) for (const auto& edge : lp.edges) {
        if (edge.end <= query.size() && !is_legal_end[edge.end]) {
            is_legal_end[edge.end] = true;
            legal_ends.push_back(edge.end);
        }
    }
    std::sort(legal_ends.begin(), legal_ends.end());
    for (size_t begin=0; begin<query.size(); ++begin) {
        if (query[begin]=='\'' && !paths[begin].empty()) { paths[begin+1]=paths[begin]; continue; }
        if (paths[begin].empty()) continue;
        // Dictionary edges may span several syllables; every reachable lattice boundary is legal.
        for (const size_t endpos : legal_ends) {
            if (endpos <= begin) continue;
            if (query.find('\'', begin) < endpos) continue;
            std::string edge_py=pinyin_data::RemoveSyllableSeparators(query.substr(begin,endpos-begin));
            auto edges=user_lexicon->dictionary.LookupExact(edge_py); auto base=lexicon->dictionary.LookupExact(edge_py); edges.insert(edges.end(),base.begin(),base.end());
            std::sort(edges.begin(),edges.end(),[](const Candidate&a,const Candidate&b){return a.frequency>b.frequency;}); if(edges.size()>4)edges.resize(4);
            for(const auto& prefix:paths[begin]) for(const auto& edge:edges) paths[endpos].push_back({prefix.text+edge.text,prefix.frequency+edge.frequency,prefix.learning+edge.learning_score,prefix.segments+1,prefix.from_user||edge.from_user});
            auto& bucket=paths[endpos]; std::sort(bucket.begin(),bucket.end(),[](const Path&a,const Path&b){if(a.segments!=b.segments)return a.segments<b.segments;if(a.learning!=b.learning)return a.learning>b.learning;if(a.frequency!=b.frequency)return a.frequency>b.frequency;return a.text<b.text;}); if(bucket.size()>128)bucket.resize(128);
        }
    }
    size_t covered=query.size(); while(covered>0 && paths[covered].empty()) --covered;
    for(const auto& path:paths[covered]) if(!path.text.empty()) { Candidate c; c.text=path.text; c.pinyin=pinyin_data::RemoveSyllableSeparators(query.substr(0,covered)); c.frequency=path.segments == 0 ? 0 : path.frequency / static_cast<int>(path.segments); c.learning_score=(std::min)(90,path.learning); c.from_user=path.from_user; add({c},covered,covered==query.size()?10:35,path.segments,true,CandidateSource::WordGraph); }

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
    if (single_complete_syllable) {
        auto is_exact_single = [](const Candidate& candidate) {
            return candidate.source == CandidateSource::Exact && IsBmpChineseWord(candidate.text) &&
                candidate.text.size() == 1;
        };
        auto is_preferred_phrase = [](const Candidate& candidate) {
            return IsBmpChineseWord(candidate.text) && candidate.text.size() > 1 &&
                candidate.source <= CandidateSource::WordGraph;
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
            if (!moved[i] && is_preferred_phrase(pool[i])) {
                ordered.push_back(std::move(pool[i]));
                moved[i] = true;
                ++preferred_phrases;
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
    if(pool.size()>limit)pool.resize(limit);
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

    NamedMutexLock dictionary_mutex(L"Local\\FacaiPinyin.UserDictionary", 5000);
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
