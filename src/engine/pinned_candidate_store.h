#pragma once

#include "candidate.h"

#include <Windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace shuru {

enum class PinnedCandidateSchema : std::uint8_t {
    Quanpin = 0,
    ShuangpinXiaohe = 1,
};

enum class PinnedCandidateToggleResult : std::uint8_t {
    Failed = 0,
    Pinned = 1,
    Unpinned = 2,
};

// 每个输入方案、每个原始输入码至多保存一个固定首选。文件采用原子替换，
// 查询端通过文件标识和修改时间检测其他宿主进程写入的新版本。
class PinnedCandidateStore {
public:
    explicit PinnedCandidateStore(std::wstring path = {});

    std::optional<std::wstring> Lookup(
        PinnedCandidateSchema schema,
        const std::string& raw_code) const;
    PinnedCandidateToggleResult Toggle(
        PinnedCandidateSchema schema,
        const std::string& raw_code,
        const std::wstring& candidate_text);
    void Promote(
        PinnedCandidateSchema schema,
        const std::string& raw_code,
        std::vector<Candidate>* candidates) const;

    static std::wstring DefaultPath();

private:
    struct FileStamp {
        bool observed = false;
        bool exists = false;
        DWORD volume_serial = 0;
        DWORD file_index_high = 0;
        DWORD file_index_low = 0;
        DWORD write_time_high = 0;
        DWORD write_time_low = 0;
        DWORD size_high = 0;
        DWORD size_low = 0;

        bool operator==(const FileStamp& other) const noexcept;
    };

    using EntryMap = std::map<std::string, std::wstring>;

    std::wstring path_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    mutable EntryMap entries_;
    mutable FileStamp stamp_;
    mutable bool loaded_ = false;

    static std::optional<std::string> NormalizeCode(
        PinnedCandidateSchema schema,
        const std::string& raw_code);
    static bool IsValidCandidateText(const std::wstring& text) noexcept;
    static FileStamp ReadFileStamp(const std::wstring& path);
    static EntryMap ReadEntries(const std::wstring& path);
    static bool WriteEntries(const std::wstring& path, const EntryMap& entries);
    void ReloadIfChanged() const;
};

}  // namespace shuru
