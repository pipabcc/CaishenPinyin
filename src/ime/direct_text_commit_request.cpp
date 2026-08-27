#include "direct_text_commit_request.h"

#include "../common/private_acl.h"
#include "../common/user_data_paths.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace shuru {
namespace {

constexpr char kRequestHeader[] = "CAISHEN_DIRECT_COMMIT_V1\n";
constexpr char kResultHeader[] = "CAISHEN_DIRECT_COMMIT_RESULT_V1\n";

std::wstring ReadEnvironmentVariable(const wchar_t* name) {
    if (name == nullptr || *name == L'\0') return {};
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name, value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}

std::wstring AppendPath(
    const std::wstring& directory,
    const std::wstring& file_name) {
    if (directory.empty() || file_name.empty()) return {};
    if (directory.back() == L'\\' || directory.back() == L'/') {
        return directory + file_name;
    }
    return directory + L"\\" + file_name;
}

bool ReadWholeFile(const std::wstring& path, std::vector<char>* bytes) {
    if (bytes == nullptr || path.empty()) return false;
    bytes->clear();
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size {};
    bool ok = GetFileSizeEx(file, &size) != FALSE &&
        size.QuadPart >= 0 && IsDirectTextCommitPayloadSizeValid(
            static_cast<std::uint64_t>(size.QuadPart));
    DWORD failure = ok ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    if (ok) {
        bytes->resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < bytes->size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(
                bytes->size() - offset,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD read = 0;
            if (!ReadFile(file, bytes->data() + offset, chunk, &read, nullptr) ||
                read == 0) {
                failure = GetLastError();
                if (failure == ERROR_SUCCESS) failure = ERROR_HANDLE_EOF;
                ok = false;
                break;
            }
            offset += read;
        }
    }
    CloseHandle(file);
    if (!ok) {
        bytes->clear();
        SetLastError(failure);
    }
    return ok;
}

bool DecodeStrictUtf8(
    const char* bytes,
    std::size_t length,
    std::wstring* text) {
    if (bytes == nullptr || length == 0 || text == nullptr ||
        length > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int byte_count = static_cast<int>(length);
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes, byte_count, nullptr, 0);
    if (required <= 0) return false;
    std::wstring decoded(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, bytes, byte_count,
            decoded.data(), required) != required) {
        return false;
    }
    if (decoded.find(L'\0') != std::wstring::npos) return false;
    *text = std::move(decoded);
    return true;
}

bool WriteFileAtomically(
    const std::wstring& path,
    const std::string& content) {
    if (path.empty() || content.empty()) return false;
    const std::wstring temporary = path + L"." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L".tmp";
    DeleteFileW(temporary.c_str());
    HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    std::size_t offset = 0;
    while (offset < content.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(
            content.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(
                file, content.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);

    if (ok) {
        ok = MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

}  // namespace

bool IsDirectTextCommitToken(const std::wstring& token) noexcept {
    return token.size() == 32 && std::all_of(
        token.begin(), token.end(), [](wchar_t character) {
            return (character >= L'0' && character <= L'9') ||
                   (character >= L'a' && character <= L'f') ||
                   (character >= L'A' && character <= L'F');
        });
}

bool IsDirectTextCommitPayloadSizeValid(
    std::uint64_t file_size) noexcept {
    constexpr std::uint64_t header_size = sizeof(kRequestHeader) - 1;
    return file_size > header_size &&
        file_size <= header_size + kDirectTextCommitMaximumPayloadBytes;
}

std::wstring CreateDirectTextCommitToken() {
    GUID id {};
    if (FAILED(CoCreateGuid(&id))) return {};
    wchar_t formatted[40] {};
    if (StringFromGUID2(id, formatted, ARRAYSIZE(formatted)) == 0) return {};
    std::wstring token;
    token.reserve(32);
    for (const wchar_t character : std::wstring(formatted)) {
        if ((character >= L'0' && character <= L'9') ||
            (character >= L'a' && character <= L'f') ||
            (character >= L'A' && character <= L'F')) {
            token.push_back(static_cast<wchar_t>(towlower(character)));
        }
    }
    return IsDirectTextCommitToken(token) ? token : std::wstring {};
}

std::wstring DirectTextCommitRequestDirectory() {
    const std::wstring override_directory = ReadEnvironmentVariable(
        L"CAISHEN_DIRECT_COMMIT_REQUEST_DIR");
    return override_directory.empty()
        ? CaishenUserDataPath(L"direct_commit_requests")
        : override_directory;
}

std::wstring DirectTextCommitRequestPath(const std::wstring& token) {
    return IsDirectTextCommitToken(token)
        ? AppendPath(DirectTextCommitRequestDirectory(), token + L".request")
        : std::wstring {};
}

std::wstring DirectTextCommitResultPath(const std::wstring& token) {
    return IsDirectTextCommitToken(token)
        ? AppendPath(DirectTextCommitRequestDirectory(), token + L".result")
        : std::wstring {};
}

std::wstring DirectTextCommitCancelPath(const std::wstring& token) {
    return IsDirectTextCommitToken(token)
        ? AppendPath(DirectTextCommitRequestDirectory(), token + L".cancel")
        : std::wstring {};
}

bool PrepareDirectTextCommitSession(const std::wstring& token) {
    if (!IsDirectTextCommitToken(token)) return false;
    const std::wstring directory = DirectTextCommitRequestDirectory();
    if (directory.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(directory), error);
    if (error) return false;
    // 正文可能来自剪贴板历史。目录使用受保护 DACL，并在允许当前用户后
    // 显式拒绝所有 AppContainer，避免父目录继承或沙箱组 ACE 泄露内容。
    if (!EnsureCurrentUserPrivatePath(directory, true) ||
        !EnsureAppContainerDenied(directory)) {
        return false;
    }
    DeleteDirectTextCommitSessionFiles(token);
    return true;
}

DirectTextCommitReadResult ReadAndDeleteDirectTextCommitRequest(
    const std::wstring& token,
    std::wstring* text) {
    if (text == nullptr || !IsDirectTextCommitToken(token)) {
        return DirectTextCommitReadResult::Invalid;
    }
    text->clear();
    const std::wstring path = DirectTextCommitRequestPath(token);
    if (path.empty()) return DirectTextCommitReadResult::Invalid;

    std::vector<char> bytes;
    if (!ReadWholeFile(path, &bytes)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
            error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
            return DirectTextCommitReadResult::NotReady;
        }
        // 文件已经出现但大小或读取不合法时，不能无限轮询同一坏请求。
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(path.c_str());
            return DirectTextCommitReadResult::Invalid;
        }
        return DirectTextCommitReadResult::NotReady;
    }

    DeleteFileW(path.c_str());
    constexpr std::size_t header_size = sizeof(kRequestHeader) - 1;
    if (bytes.size() <= header_size ||
        !std::equal(
            kRequestHeader, kRequestHeader + header_size, bytes.begin())) {
        return DirectTextCommitReadResult::Invalid;
    }
    if (!DecodeStrictUtf8(
            bytes.data() + header_size, bytes.size() - header_size, text)) {
        text->clear();
        return DirectTextCommitReadResult::Invalid;
    }
    return DirectTextCommitReadResult::Ready;
}

const char* DirectTextCommitResultName(
    DirectTextCommitResult result) noexcept {
    switch (result) {
    case DirectTextCommitResult::Success: return "success";
    case DirectTextCommitResult::TargetUnavailable: return "target-unavailable";
    case DirectTextCommitResult::ContextChanged: return "context-changed";
    case DirectTextCommitResult::SensitiveContext: return "sensitive-context";
    case DirectTextCommitResult::RequestExpired: return "request-expired";
    case DirectTextCommitResult::InvalidRequest: return "invalid-request";
    default: return "commit-failed";
    }
}

bool WriteDirectTextCommitResult(
    const std::wstring& token,
    DirectTextCommitResult result) {
    if (!IsDirectTextCommitToken(token)) return false;
    const std::wstring path = DirectTextCommitResultPath(token);
    if (path.empty()) return false;
    const std::string content = std::string(kResultHeader) +
        DirectTextCommitResultName(result) + "\n";
    return WriteFileAtomically(path, content);
}

bool ConsumeDirectTextCommitCancellation(
    const std::wstring& token) noexcept {
    const std::wstring path = DirectTextCommitCancelPath(token);
    if (path.empty()) return false;
    if (DeleteFileW(path.c_str())) return true;
    return false;
}

void DeleteDirectTextCommitSessionFiles(
    const std::wstring& token) noexcept {
    if (!IsDirectTextCommitToken(token)) return;
    const std::wstring request = DirectTextCommitRequestPath(token);
    const std::wstring result = DirectTextCommitResultPath(token);
    const std::wstring cancel = DirectTextCommitCancelPath(token);
    if (!request.empty()) DeleteFileW(request.c_str());
    if (!result.empty()) DeleteFileW(result.c_str());
    if (!cancel.empty()) DeleteFileW(cancel.c_str());
}

}  // namespace shuru
