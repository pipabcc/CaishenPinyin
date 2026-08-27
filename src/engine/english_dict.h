#pragma once

#include "candidate.h"
#include "engine_snapshot.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shuru {

// 英文单词词典：大小写不敏感查询，候选保留词库提供的规范大小写。
// 支持两种后端：堆模式（文本装载）与只读快照映射模式，查询行为一致。
class EnglishDictionary {
public:
    bool LoadFromFile(const std::wstring& path);
    // 注入只读映射快照；成功后进入映射模式，文本装载路径不再可用。
    bool AdoptMappedSnapshot(
        const void* base, size_t size, std::shared_ptr<void> region);
    bool is_mapped() const noexcept { return mapped_mode_; }
    bool empty() const { return is_mapped() ? snap_count_ == 0 : words_.empty(); }
    size_t Size() const { return is_mapped() ? snap_count_ : words_.size(); }

    // 精确匹配
    std::vector<Candidate> LookupExact(const std::string& word) const;
    // 前缀匹配，按词频排序
    std::vector<Candidate> LookupPrefix(
        const std::string& prefix, size_t limit = 32) const;

private:
    friend bool SerializeEngineSnapshot(
        class Dictionary*, EnglishDictionary*,
        const std::wstring&, const std::wstring&, const std::wstring&,
        std::vector<std::uint8_t>*);

    struct Item {
        std::string word;
        std::string display;
        int frequency = 0;
    };

    struct ItemView {
        std::string_view word;
        std::string_view display;
        int frequency = 0;
    };

    ItemView ItemAt(size_t index) const;
    // 键升序数组上做二分：返回第一个键 >= key 的下标。
    size_t LowerBound(std::string_view key) const;

    std::vector<Item> words_;  // 按规范化查询键升序，便于 lower_bound

    bool mapped_mode_ = false;
    std::shared_ptr<void> mapped_region_;
    const SnapshotEnglishRecord* snap_records_ = nullptr;
    const char* snap_blob_ = nullptr;
    std::uint32_t snap_count_ = 0;

    static std::string Normalize(const std::string& s);
};

}  // namespace shuru
