#include "pinned_candidate_store.h"

#include "../common/user_data_paths.h"

#include "../common/com_utils.h"
#include "../common/private_acl.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>

namespace shuru {
namespace {

constexpr wchar_t kPinnedCandidateMutexName[] =
    L"Local\\CaishenPinyin.PinnedCandidates";
constexpr std::size_t kMaximumCodeLength = 64;
constexpr std::size_t kMaximumCandidateLength = 128;

class MutexGuard {
public:
    explicit MutexGuard(DWORD timeout_ms) {
        mutex_ = CreateMutexW(nullptr, FALSE, kPinnedCandidateMutexName);
        if (mutex_ == nullptr) return;
        const DWORD wait = WaitForSingleObject(mutex_, timeout_ms);
        owns_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }

    ~MutexGuard() {
        if (owns_) ReleaseMutex(mutex_);
        if (mutex_ != nullptr) CloseHandle(mutex_);
    }

    bool owns() const noexcept { return owns_; }

private:
    HANDLE mutex_ = nullptr;
    bool owns_ = false;
};

const char* SchemaName(PinnedCandidateSchema schema) noexcept {
    return schema == PinnedCandidateSchema::ShuangpinXiaohe
        ? "xiaohe" : "quanpin";
}

std::optional<PinnedCandidateSchema> ParseSchema(const std::string& value) {
    if (value == "quanpin") return PinnedCandidateSchema::Quanpin;
    if (value == "xiaohe") return PinnedCandidateSchema::ShuangpinXiaohe;
    return std::nullopt;
}

std::string EntryKey(PinnedCandidateSchema schema, const std::string& code) {
    return std::string(SchemaName(schema)) + '\t' + code;
}

}  // namespace

PinnedCandidateStore::PinnedCandidateStore(std::wstring path)
    : path_(path.empty() ? DefaultPath() : std::move(path)) {}

bool PinnedCandidateStore::FileStamp::operator==(
    const FileStamp& other) const noexcept {
    return observed == other.observed && exists == other.exists &&
        volume_serial == other.volume_serial &&
        file_index_high == other.file_index_high &&
        file_index_low == other.file_index_low &&
        write_time_high == other.write_time_high &&
        write_time_low == other.write_time_low &&
        size_high == other.size_high && size_low == other.size_low;
}

std::optional<std::string> PinnedCandidateStore::NormalizeCode(
    PinnedCandidateSchema schema,
    const std::string& raw_code) {
    (void)schema;
    if (raw_code.empty() || raw_code.size() > kMaximumCodeLength) {
        return std::nullopt;
    }
    std::string normalized;
    normalized.reserve(raw_code.size());
    for (const unsigned char ch : raw_code) {
        if (ch >= 'A' && ch <= 'Z') {
            normalized.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if ((ch >= 'a' && ch <= 'z') || ch == '\'') {
            normalized.push_back(static_cast<char>(ch));
        } else {
            return std::nullopt;
        }
    }
    return normalized;
}

bool PinnedCandidateStore::IsValidCandidateText(
    const std::wstring& text) noexcept {
    return !text.empty() && text.size() <= kMaximumCandidateLength &&
        std::none_of(text.begin(), text.end(), [](wchar_t ch) {
            return ch < L' ' || ch == L'\x7F' ||
                (ch >= 0xD800 && ch <= 0xDFFF);
        });
}

PinnedCandidateStore::FileStamp PinnedCandidateStore::ReadFileStamp(
    const std::wstring& path) {
    FileStamp stamp;
    if (path.empty()) return stamp;
    HANDLE file = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        stamp.observed = error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND;
        stamp.exists = false;
        return stamp;
    }

    BY_HANDLE_FILE_INFORMATION info {};
    if (GetFileInformationByHandle(file, &info)) {
        stamp.observed = true;
        stamp.exists = true;
        stamp.volume_serial = info.dwVolumeSerialNumber;
        stamp.file_index_high = info.nFileIndexHigh;
        stamp.file_index_low = info.nFileIndexLow;
        stamp.write_time_high = info.ftLastWriteTime.dwHighDateTime;
        stamp.write_time_low = info.ftLastWriteTime.dwLowDateTime;
        stamp.size_high = info.nFileSizeHigh;
        stamp.size_low = info.nFileSizeLow;
    }
    CloseHandle(file);
    return stamp;
}

PinnedCandidateStore::EntryMap PinnedCandidateStore::ReadEntries(
    const std::wstring& path) {
    EntryMap entries;
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const std::size_t first = line.find('\t');
        const std::size_t second = first == std::string::npos
            ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos ||
            line.find('\t', second + 1) != std::string::npos) {
            continue;
        }
        const auto schema = ParseSchema(line.substr(0, first));
        if (!schema) continue;
        const auto code = NormalizeCode(
            *schema, line.substr(first + 1, second - first - 1));
        const std::wstring text = Utf8ToWide(line.substr(second + 1));
        if (!code || !IsValidCandidateText(text)) continue;
        entries[EntryKey(*schema, *code)] = text;
    }
    return entries;
}

bool PinnedCandidateStore::WriteEntries(
    const std::wstring& path,
    const EntryMap& entries) {
    if (path.empty()) return false;
    std::error_code error;
    const std::filesystem::path target(path);
    const std::filesystem::path directory = target.parent_path();
    if (directory.empty()) return false;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    EnsureCurrentUserOnlyPath(directory.wstring(), true);

    const std::wstring temporary = path + L".tmp-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    {
        std::ofstream output(
            std::filesystem::path(temporary),
            std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << "# Caishen Pinyin pinned candidates v1\n";
        for (const auto& [key, text] : entries) {
            output << key << '\t' << WideToUtf8(text) << '\n';
        }
        output.flush();
        if (!output) {
            output.close();
            DeleteFileW(temporary.c_str());
            return false;
        }
    }
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    EnsureCurrentUserOnlyPath(path, false);
    return true;
}

void PinnedCandidateStore::ReloadIfChanged() const {
    const FileStamp observed = ReadFileStamp(path_);
    AcquireSRWLockShared(&lock_);
    const bool current = loaded_ && observed.observed && observed == stamp_;
    ReleaseSRWLockShared(&lock_);
    if (current || !observed.observed) return;

    EntryMap entries = observed.exists ? ReadEntries(path_) : EntryMap {};
    const FileStamp confirmed = ReadFileStamp(path_);
    if (!confirmed.observed || !(confirmed == observed)) return;

    AcquireSRWLockExclusive(&lock_);
    entries_ = std::move(entries);
    stamp_ = confirmed;
    loaded_ = true;
    ReleaseSRWLockExclusive(&lock_);
}

std::optional<std::wstring> PinnedCandidateStore::Lookup(
    PinnedCandidateSchema schema,
    const std::string& raw_code) const {
    const auto code = NormalizeCode(schema, raw_code);
    if (!code || path_.empty()) return std::nullopt;
    ReloadIfChanged();
    AcquireSRWLockShared(&lock_);
    const auto found = entries_.find(EntryKey(schema, *code));
    std::optional<std::wstring> result;
    if (found != entries_.end()) result = found->second;
    ReleaseSRWLockShared(&lock_);
    return result;
}

PinnedCandidateToggleResult PinnedCandidateStore::Toggle(
    PinnedCandidateSchema schema,
    const std::string& raw_code,
    const std::wstring& candidate_text) {
    const auto code = NormalizeCode(schema, raw_code);
    if (!code || !IsValidCandidateText(candidate_text) || path_.empty()) {
        return PinnedCandidateToggleResult::Failed;
    }

    MutexGuard guard(2000);
    if (!guard.owns()) return PinnedCandidateToggleResult::Failed;
    EntryMap entries = ReadEntries(path_);
    const std::string key = EntryKey(schema, *code);
    const auto found = entries.find(key);
    const bool unpin = found != entries.end() &&
        found->second == candidate_text;
    if (unpin) {
        entries.erase(found);
    } else {
        entries[key] = candidate_text;
    }
    if (!WriteEntries(path_, entries)) {
        return PinnedCandidateToggleResult::Failed;
    }

    const FileStamp updated_stamp = ReadFileStamp(path_);
    AcquireSRWLockExclusive(&lock_);
    entries_ = std::move(entries);
    stamp_ = updated_stamp;
    loaded_ = updated_stamp.observed;
    ReleaseSRWLockExclusive(&lock_);
    return unpin ? PinnedCandidateToggleResult::Unpinned
                 : PinnedCandidateToggleResult::Pinned;
}

void PinnedCandidateStore::Promote(
    PinnedCandidateSchema schema,
    const std::string& raw_code,
    std::vector<Candidate>* candidates) const {
    if (candidates == nullptr) return;
    for (auto& candidate : *candidates) candidate.pinned = false;
    const auto pinned = Lookup(schema, raw_code);
    if (!pinned) return;
    const auto found = std::find_if(
        candidates->begin(), candidates->end(), [&](const Candidate& candidate) {
            return candidate.text == *pinned;
        });
    if (found == candidates->end()) return;
    found->pinned = true;
    if (found != candidates->begin()) {
        std::rotate(candidates->begin(), found, found + 1);
    }
}

std::wstring PinnedCandidateStore::DefaultPath() {
    return CaishenUserDataPath(L"data\\pinned_candidates.tsv");
}

}  // namespace shuru
