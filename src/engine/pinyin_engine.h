#pragma once

#include "candidate.h"
#include "custom_phrase.h"
#include "dictionary.h"
#include "english_dict.h"
#include "fuzzy_pinyin.h"
#include "system_language_model.h"
#include "user_bigram.h"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace shuru {

enum class InputSchema {
    Quanpin = 0,
    ShuangpinXiaohe = 1,
};

struct QueryOptions {
    InputSchema schema = InputSchema::Quanpin;
    bool fuzzy_enabled = true;
    FuzzyConfig fuzzy_config {};
    // 最近一次上屏的候选文本（会话上下文）；用于 bigram 加权与整句转换。
    std::wstring context;
};

class PinyinEngine {
public:
    PinyinEngine();
    ~PinyinEngine();

    // 事务式重载：成功时发布新词库；失败时保留此前已就绪的快照和配置。
    bool Initialize(const std::wstring& lexicon_dir);
    bool IsReady() const;

    void SetFuzzyEnabled(bool enabled);
    bool FuzzyEnabled() const;
    void SetFuzzyConfig(const FuzzyConfig& config);
    FuzzyConfig GetFuzzyConfig() const;
    void SetInputSchema(InputSchema schema);
    InputSchema GetInputSchema() const;
    // 一次发布完整配置；新调用方应优先使用，旧的单项 setter 保持兼容。
    void SetQueryOptions(const QueryOptions& options);
    QueryOptions GetQueryOptions() const;

    // raw_input: 用户键入码（全拼字母或双拼码）
    EngineQueryResult Query(const std::string& raw_input, size_t limit = 9) const;
    EngineQueryResult Query(const std::string& raw_input, size_t limit,
                            const QueryOptions& options) const;
    // 返回用于界面显示的组合串（双拼时为解码预览）
    std::wstring FormatComposingDisplay(const std::string& raw_input) const;
    // 学习时尽量用全拼
    std::string ToFullPinyinForLearn(const std::string& raw_input) const;

    void Learn(const std::string& pinyin, const std::wstring& word);
    bool UndoLastLearning();
    // 记录「上一个上屏词 -> 本次上屏词」的搭配；由 TextService 在成功
    // 上屏后调用（密码域/关闭学习时不调用）。
    void ObserveBigram(const std::wstring& previous, const std::wstring& next);
    // 上屏后联想：返回 context 的高频后继（仅用户 bigram，冷启动为空）。
    std::vector<Candidate> PredictNext(const std::wstring& context, size_t limit) const;
    bool ExportUserDictionary(const std::wstring& path) const;
    bool ImportUserDictionary(const std::wstring& path);
    bool ClearUserDictionary();
    bool ReloadCustomPhrases();

    static bool IsPinyinLetter(wchar_t ch);
    static std::string NormalizeInput(const std::string& input);

    std::wstring user_dict_path() const;
    std::wstring custom_phrase_path() const;

private:
    struct LexiconSnapshot {
        Dictionary dictionary;
        EnglishDictionary english_dictionary;
        SystemLanguageModel language_model;
    };

    struct UserLexiconSnapshot {
        Dictionary dictionary;
    };

    struct UserDictSnapshot {
        std::wstring path;
        std::vector<UserDictionaryEntry> entries;
        std::uint64_t revision = 0;
    };

    std::shared_ptr<LexiconSnapshot> lexicon_;
    std::shared_ptr<UserLexiconSnapshot> user_lexicon_;
    std::shared_ptr<CustomPhraseDictionary> custom_phrases_;
    std::shared_ptr<const UserBigramModel> bigram_;
    bool ready_ = false;
    bool fuzzy_enabled_ = true;
    FuzzyConfig fuzzy_config_ {};
    InputSchema schema_ = InputSchema::Quanpin;
    std::string last_learned_pinyin_;
    std::wstring last_learned_word_;
    // 同一输入码下连续选择同一候选的次数；达到 2 次后该候选直接置顶。
    std::string repeat_selection_pinyin_;
    std::wstring repeat_selection_text_;
    int repeat_selection_count_ = 0;
    mutable CRITICAL_SECTION lock_ {};
    bool lock_ready_ = false;
    std::wstring user_dict_path_;
    std::wstring custom_phrase_path_;
    std::wstring bigram_path_;
    bool bigram_dirty_ = false;
    std::wstring lexicon_dir_;
    HANDLE save_event_ = nullptr;
    HANDLE save_thread_ = nullptr;
    volatile LONG save_stop_ = 0;
    std::uint64_t user_dict_revision_ = 0;
    bool user_dict_writable_ = false;

    bool CaptureUserDictSnapshot(UserDictSnapshot* snapshot);
    void CompleteUserDictSave(
        const UserDictSnapshot& snapshot,
        const std::vector<UserDictionaryEntry>& external_entries,
        bool succeeded);
    static bool PersistUserDictSnapshot(
        const UserDictSnapshot& snapshot,
        std::vector<UserDictionaryEntry>* external_entries);
    bool ScheduleUserDictSave();
    static DWORD WINAPI SaveThreadProc(LPVOID param);
    static bool LooksLikeJianpin(const std::string& pinyin);
    std::string ResolveQueryPinyin(const std::string& raw_input, std::string* preview) const;
};

}  // namespace shuru
