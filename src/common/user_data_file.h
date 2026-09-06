#pragma once

#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace shuru {

inline constexpr wchar_t kUserLearningMutex[] = L"Local\\CaishenPinyin.UserDictionary";
inline constexpr char kUserGenerationPrefix[] = "# generation=";
inline constexpr char kBigramGenerationPrefix[] = "# bigram_generation=";

class UserLearningFileLock {
public:
    explicit UserLearningFileLock(DWORD timeout = 5000) {
        handle_ = CreateMutexW(nullptr, FALSE, kUserLearningMutex);
        if (handle_ == nullptr) return;
        const auto result = WaitForSingleObject(handle_, timeout);
        owns_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~UserLearningFileLock() {
        if (owns_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
    bool owns() const noexcept { return owns_; }
    UserLearningFileLock(const UserLearningFileLock&) = delete;
    UserLearningFileLock& operator=(const UserLearningFileLock&) = delete;
private:
    HANDLE handle_ = nullptr;
    bool owns_ = false;
};

struct UserDataFileStamp {
    std::uint64_t id = 0;
    std::uint64_t time = 0;
    std::uint64_t size = 0;
    DWORD volume = 0;
    bool present = false;
    bool valid = false;
    bool operator==(const UserDataFileStamp& other) const noexcept {
        return id == other.id && time == other.time && size == other.size &&
            volume == other.volume && present == other.present && valid == other.valid;
    }
};

inline UserDataFileStamp UserDataStampFromHandle(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return {};
    return {
        (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow,
        (std::uint64_t{info.ftLastWriteTime.dwHighDateTime} << 32) | info.ftLastWriteTime.dwLowDateTime,
        (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow,
        info.dwVolumeSerialNumber, true,
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0
    };
}

inline UserDataFileStamp ReadUserDataFileStamp(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        UserDataFileStamp stamp;
        stamp.valid = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        return stamp;
    }
    const auto stamp = UserDataStampFromHandle(file);
    CloseHandle(file);
    return stamp;
}

inline bool ReadUserDataFile(const std::wstring& path, std::string* text,
                             UserDataFileStamp* stamp) {
    if (text == nullptr || stamp == nullptr) return false;
    text->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *stamp = ReadUserDataFileStamp(path);
        return stamp->valid && !stamp->present;
    }
    *stamp = UserDataStampFromHandle(file);
    bool ok = stamp->valid && stamp->size <= 64 * 1024 * 1024;
    try {
        if (ok) {
            text->resize(static_cast<size_t>(stamp->size));
            DWORD read = 0;
            ok = text->empty() || (ReadFile(file, text->data(), static_cast<DWORD>(text->size()),
                                           &read, nullptr) && read == text->size());
        }
    } catch (...) {
        ok = false;
    }
    CloseHandle(file);
    if (!ok) text->clear();
    return ok;
}

inline std::string HexGeneration(const unsigned char* bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::string value(32, '0');
    for (size_t index = 0; index < 16; ++index) {
        value[index * 2] = hex[bytes[index] >> 4];
        value[index * 2 + 1] = hex[bytes[index] & 15];
    }
    return value;
}

inline std::string NewUserDataGeneration() {
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};
    return HexGeneration(bytes);
}

inline bool ParseUserDataGeneration(const std::string& text, std::string* generation,
                                    const std::string& prefix = kUserGenerationPrefix) {
    generation->clear();
    size_t position = 0;
    while (position < text.size()) {
        const auto end = text.find('\n', position);
        auto line = text.substr(position, end == std::string::npos ? end : end - position);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (position == 0 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        if (line.compare(0, prefix.size(), prefix) == 0) {
            *generation = line.substr(prefix.size());
            return generation->size() == 32 && std::all_of(generation->begin(), generation->end(),
                [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
        }
        if (!line.empty() && line.front() != '#') break;
        if (end == std::string::npos) break;
        position = end + 1;
    }
    return true;
}

inline std::string LegacyUserDataGeneration(const UserDataFileStamp& stamp) {
    // 无标记的旧文件以文件身份和修改时间划分代次，兼容旧版清空/替换。
    std::uint64_t values[2] = {stamp.id ^ stamp.volume, stamp.time ^ stamp.size};
    return HexGeneration(reinterpret_cast<const unsigned char*>(values));
}

}  // namespace shuru
