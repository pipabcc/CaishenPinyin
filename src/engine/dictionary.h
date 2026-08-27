#pragma once

#include "candidate.h"
#include "engine_snapshot.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shuru {

struct UserDictionaryEntry {
    std::string pinyin;
    std::wstring word;
    int frequency = 0;
    int selection_count = 0;
    std::int64_t last_used_unix = 0;
};

struct MixedPrefixMatch {
    Candidate candidate;
    size_t consumed_input = 0;
    size_t abbreviated_syllables = 0;
    size_t omitted_letters = 0;
    size_t syllable_count = 0;
    std::string segmented_input;
};

class Dictionary {
public:
    bool LoadFromFile(const std::wstring& path, bool from_user = false);
    bool LoadFromUtf8Lines(const std::vector<std::string>& lines, bool from_user = false);

    // 只读映射快照模式：注入后本词典仅支持查询；所有可变操作被拒绝并记录
    // 警告。引擎只对独立的用户词典实例做学习写入，系统词典纯查询，因此
    // 映射模式无需覆盖层。region 持有映射生命周期（UnmapViewOfFile 等）。
    bool AdoptMappedSnapshot(
        const void* base, size_t size, std::shared_ptr<void> region);
    bool is_mapped() const noexcept { return mapped_mode_; }

    // 批量装载模式：期间 AddWord 跳过桶内排序与简拼/trie 增量索引维护，
    // EndBulkLoad 统一排序并重建一次索引。仅用于初始加载，加载耗时从
    // 每插入一次 sort 整桶的 O(N·B·logB) 降为一次性 O(N·logB)。
    void BeginBulkLoad();
    void EndBulkLoad();

    void AddWord(const std::string& pinyin, const std::wstring& word, int frequency, bool from_user);
    void IncreaseUserWord(
        const std::string& pinyin,
        const std::wstring& word,
        int delta = 10,
        int minimum_frequency = 0,
        std::int64_t now_unix = 0);
    int LookupFrequency(const std::string& pinyin, const std::wstring& word) const;
    bool ContainsWord(const std::wstring& word) const;
    bool ContainsWordPinyin(const std::wstring& word, const std::string& pinyin) const;
    bool DecreaseUserWord(const std::string& pinyin, const std::wstring& word, int delta = 20);
    static int ComputeLearningScore(int selection_count, std::int64_t last_used_unix, std::int64_t now_unix = 0);
    void ClearUserEntries();

    bool SaveUserToFile(const std::wstring& path) const;
    std::vector<UserDictionaryEntry> SnapshotUserEntries() const;
    void ImportUserEntries(const std::vector<UserDictionaryEntry>& entries);
    bool dirty() const { return dirty_; }
    void clear_dirty() const { dirty_ = false; }

    std::vector<Candidate> LookupExact(const std::string& pinyin) const;
    std::vector<Candidate> LookupPrefix(const std::string& pinyin_prefix, size_t limit = 64) const;
    std::vector<Candidate> LookupJianpin(const std::string& jianpin, size_t limit = 64) const;
    std::vector<Candidate> LookupMixed(const std::string& input, size_t limit = 64) const;
    std::vector<MixedPrefixMatch> LookupMixedPrefixes(
        const std::string& input, size_t limit = 64) const;

    size_t Size() const;
    size_t JianpinSize() const;

    // 从多字词按音节对齐反推单字，解决 sun 只有「损失」没有「孙/损」
    size_t DeriveSingleCharacters();

private:
    friend bool SerializeEngineSnapshot(
        Dictionary*, EnglishDictionary*,
        const std::wstring&, const std::wstring&, const std::wstring&,
        std::vector<std::uint8_t>*);

    struct Entry {
        std::wstring word;
        int frequency = 0;
        bool from_user = false;
        int selection_count = 0;
        std::int64_t last_used_unix = 0;
    };

    // 词桶的统一只读视图：堆模式指向 map_ 桶，映射模式指向快照词条切片。
    struct BucketRef {
        const std::vector<Entry>* heap = nullptr;
        const SnapshotEntryRecord* recs = nullptr;
        std::uint32_t rec_count = 0;
    };

    struct TrieNode {
        int first_child = -1;
        int next_sibling = -1;
        int max_frequency = 0;
        int terminal_frequency = 0;
        char label = 0;
        bool terminal = false;
    };

    struct SyllableTrieNode {
        int first_child = -1;
        int next_sibling = -1;
        int max_frequency = 0;
        std::uint16_t syllable_id = 0;
        bool terminal = false;
    };

    struct SyllableSpellingNode {
        std::array<int, 26> children {};
        int syllable_id = -1;

        SyllableSpellingNode() { children.fill(-1); }
    };

    std::unordered_map<std::string, std::vector<Entry>> map_;
    std::map<std::pair<std::string, std::wstring>, UserDictionaryEntry> user_entries_;
    std::vector<TrieNode> trie_;  // trie_[0] = root
    std::vector<SyllableTrieNode> syllable_trie_;  // syllable_trie_[0] = root
    std::vector<std::string> syllable_values_;
    std::unordered_map<std::string, std::uint16_t> syllable_ids_;
    std::vector<SyllableSpellingNode> syllable_spelling_trie_;
    std::vector<int> syllable_root_children_;
    std::vector<std::array<std::uint64_t, 2>> word_fingerprints_;
    mutable bool dirty_ = false;
    bool bulk_loading_ = false;

    BucketRef FindBucket(std::string_view key) const;
    void AppendBucketCandidates(
        const std::string& key, const BucketRef& bucket,
        std::vector<Candidate>* out) const;
    std::wstring MappedEntryWord(const SnapshotEntryRecord& record) const;

    // 双模式访问器：查询路径一律经由它们触达 Trie/音节表/指纹，屏蔽
    // 堆容器与只读映射的差异。
    int TrieNodeTotal() const;
    const TrieNode& TrieNodeAt(int index) const;
    int StrTrieNodeTotal() const;
    const SyllableTrieNode& StrTrieNodeAt(int index) const;
    std::int32_t RootChildAt(size_t syllable_id) const;
    size_t SylValueTotal() const;
    std::string_view SylValueAt(size_t syllable_id) const;
    size_t FingerprintTotal() const;
    void FingerprintAt(size_t index, std::uint64_t (*out)[2]) const;
    bool TrieEmpty() const;
    bool StrTrieEmpty() const;

    // 快照映射状态；shared_ptr 保证最后一个持有词库快照的对象析构后才解映射。
    bool mapped_mode_ = false;
    std::shared_ptr<void> mapped_region_;
    const unsigned char* snap_base_ = nullptr;
    const EngineSnapshotHeader* snap_header_ = nullptr;
    const SnapshotKeyIndexEntry* snap_key_index_ = nullptr;
    const SnapshotEntryRecord* snap_entries_ = nullptr;
    const char* snap_keys_blob_ = nullptr;
    const wchar_t* snap_words_blob_ = nullptr;
    const TrieNode* snap_trie_ = nullptr;
    const SyllableTrieNode* snap_str_trie_ = nullptr;
    const std::int32_t* snap_root_children_ = nullptr;
    const std::uint32_t* snap_syl_offsets_ = nullptr;
    const char* snap_syl_blob_ = nullptr;
    const std::uint64_t (*snap_fingerprints_)[2] = nullptr;
    std::uint32_t snap_key_count_ = 0;
    std::uint32_t snap_entry_count_ = 0;
    std::uint32_t snap_fp_count_ = 0;

    void EnsureTrieRoot();
    void EnsureSyllableTrieRoot();
    void EnsureSyllableTable();
    int FindTrieChild(int node, char label) const;
    int FindSyllableChild(int node, std::uint16_t syllable_id) const;
    void TrieInsert(const std::string& pinyin);
    void SyllableTrieInsert(const std::string& pinyin);
    void CollectTriePrefix(const std::string& prefix, size_t limit, std::vector<std::string>* out_keys) const;
    void CollectTrieSubtree(
        int node, const std::string& prefix, size_t limit,
        std::vector<std::string>* out_keys) const;

    void RebuildTrieIndex();
    void RebuildSyllableTrieIndex();
    void RebuildJianpinIndex();
    void RebuildWordFingerprints();
    void IndexWordFingerprint(const std::wstring& word);
    void IndexPinyinKey(const std::string& pinyin);

    static void SortEntries(std::vector<Entry>& entries);
    static std::vector<Candidate> ToCandidates(const std::string& pinyin, const std::vector<Entry>& entries);
};

}  // namespace shuru
