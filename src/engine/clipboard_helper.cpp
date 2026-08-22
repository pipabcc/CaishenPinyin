#include "clipboard_helper.h"
#include "custom_phrase.h"
#include "../common/com_utils.h"
#include "../common/logger.h"
#include "../common/user_data_paths.h"

#include <Windows.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>

namespace shuru {
namespace {

constexpr char kTextPasteRequestHeader[] = "CAISHEN_TEXT_PASTE_V1\n";

std::wstring ReadEnvironmentVariable(const wchar_t* name) {
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
    const std::wstring& directory, const wchar_t* relative_path) {
    if (directory.empty()) return {};
    if (directory.back() == L'\\' || directory.back() == L'/') {
        return directory + relative_path;
    }
    return directory + L"\\" + relative_path;
}

std::wstring GetClipboardJsonPath() {
    const std::wstring override_directory = ReadEnvironmentVariable(
        L"CAISHEN_CLIPBOARD_DATA_DIR");
    if (!override_directory.empty()) {
        return AppendPath(override_directory, L"history.json");
    }
    return AppendPath(
        CaishenLocalAppData(),
        L"CaishenPinyin\\clipboard\\history.json");
}

std::wstring GetClipboardDatabasePath() {
    const std::wstring override_directory = ReadEnvironmentVariable(
        L"CAISHEN_CLIPBOARD_DATA_DIR");
    if (!override_directory.empty()) {
        return AppendPath(override_directory, L"history.db");
    }
    return AppendPath(
        CaishenLocalAppData(),
        L"CaishenPinyin\\clipboard\\history.db");
}

std::wstring GetTextPasteRequestDirectory() {
    const std::wstring override_directory = ReadEnvironmentVariable(
        L"CAISHEN_PASTE_REQUEST_DIR");
    if (!override_directory.empty()) return override_directory;
    return AppendPath(
        CaishenLocalAppData(),
        L"CaishenPinyin\\paste_requests");
}

bool IsValidRequestToken(const std::wstring& token) noexcept {
    return token.size() == 32 && std::all_of(
        token.begin(), token.end(), [](wchar_t character) {
            return (character >= L'0' && character <= L'9') ||
                   (character >= L'a' && character <= L'f') ||
                   (character >= L'A' && character <= L'F');
        });
}

std::wstring CreateRequestToken() {
    GUID id {};
    if (FAILED(CoCreateGuid(&id))) return {};
    wchar_t formatted[40] {};
    if (StringFromGUID2(id, formatted, ARRAYSIZE(formatted)) == 0) return {};
    std::wstring token;
    token.reserve(32);
    for (const wchar_t character : std::wstring(formatted)) {
        if (iswxdigit(character)) {
            token.push_back(static_cast<wchar_t>(towlower(character)));
        }
    }
    return IsValidRequestToken(token) ? token : std::wstring{};
}

std::wstring TextPasteRequestPath(const std::wstring& token) {
    if (!IsValidRequestToken(token)) return {};
    return AppendPath(
        GetTextPasteRequestDirectory(), (token + L".txt").c_str());
}

struct SimpleClipboardItem {
    std::wstring id;
    int type = 0;
    std::wstring content;
    std::wstring display_title;
    std::wstring image_path;
    std::string serialized_object;
};

constexpr wchar_t kClipboardHistoryMutexName[] =
    L"Local\\CaishenPinyinClipboardHistoryV1";
constexpr DWORD kClipboardHistoryLockTimeoutMs = 2000;

SRWLOCK g_clipboard_lock = SRWLOCK_INIT;
std::vector<SimpleClipboardItem> g_cached_items;
FILETIME g_last_loaded_ft {};
ULONGLONG g_last_loaded_size = 0;
std::wstring g_last_loaded_path;
bool g_cache_loaded = false;

class ScopedSrwExclusiveLock {
public:
    explicit ScopedSrwExclusiveLock(SRWLOCK* lock) noexcept : lock_(lock) {
        AcquireSRWLockExclusive(lock_);
    }

    ~ScopedSrwExclusiveLock() {
        ReleaseSRWLockExclusive(lock_);
    }

    ScopedSrwExclusiveLock(const ScopedSrwExclusiveLock&) = delete;
    ScopedSrwExclusiveLock& operator=(const ScopedSrwExclusiveLock&) = delete;

private:
    SRWLOCK* lock_;
};

class ClipboardHistoryFileLock {
public:
    ClipboardHistoryFileLock() noexcept {
        handle_ = CreateMutexW(nullptr, FALSE, kClipboardHistoryMutexName);
        if (handle_ == nullptr) return;
        const DWORD result = WaitForSingleObject(
            handle_, kClipboardHistoryLockTimeoutMs);
        acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }

    ~ClipboardHistoryFileLock() {
        if (acquired_) ReleaseMutex(handle_);
        if (handle_ != nullptr) CloseHandle(handle_);
    }

    ClipboardHistoryFileLock(const ClipboardHistoryFileLock&) = delete;
    ClipboardHistoryFileLock& operator=(const ClipboardHistoryFileLock&) = delete;

    bool acquired() const noexcept { return acquired_; }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

class SqliteConnection {
public:
    explicit SqliteConnection(const std::wstring& path) noexcept {
        const std::string utf8_path = WideToUtf8(path);
        if (utf8_path.empty()) return;
        if (sqlite3_open_v2(
                utf8_path.c_str(), &database_,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                nullptr) != SQLITE_OK) {
            if (database_ != nullptr) sqlite3_close_v2(database_);
            database_ = nullptr;
            return;
        }
        sqlite3_busy_timeout(database_, 2000);
    }

    ~SqliteConnection() {
        if (database_ != nullptr) sqlite3_close_v2(database_);
    }

    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    sqlite3* get() const noexcept { return database_; }

private:
    sqlite3* database_ = nullptr;
};

class SqliteStatement {
public:
    SqliteStatement(sqlite3* database, const char* sql) noexcept {
        if (database == nullptr || sql == nullptr) return;
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) !=
            SQLITE_OK) {
            statement_ = nullptr;
        }
    }

    ~SqliteStatement() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }

    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
};

std::string SqliteText(sqlite3_stmt* statement, int column) {
    if (statement == nullptr) return {};
    const auto* value = sqlite3_column_text(statement, column);
    const int length = sqlite3_column_bytes(statement, column);
    if (value == nullptr || length <= 0) return {};
    return std::string(
        reinterpret_cast<const char*>(value), static_cast<size_t>(length));
}

enum class DatabaseQueryStatus {
    Unavailable,
    Success,
    Failed,
};

DatabaseQueryStatus QueryClipboardDatabase(
    const std::string& query, size_t limit,
    std::vector<Candidate>* candidates) {
    if (candidates == nullptr) return DatabaseQueryStatus::Failed;
    const std::wstring path = GetClipboardDatabasePath();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return DatabaseQueryStatus::Unavailable;
    }

    SqliteConnection connection(path);
    if (connection.get() == nullptr) return DatabaseQueryStatus::Failed;
    bool use_fts = query.size() >= 3;
    if (use_fts) {
        SqliteStatement fts_check(connection.get(), R"sql(
            SELECT 1 FROM sqlite_master
            WHERE type='table' AND name='clipboard_records_fts' LIMIT 1;
        )sql");
        use_fts = fts_check.get() != nullptr &&
            sqlite3_step(fts_check.get()) == SQLITE_ROW;
    }
    const char* sql = use_fts ? R"sql(
        SELECT r.id, r.type,
               CASE WHEN length(r.content)>?3 THEN '' ELSE r.content END,
               CASE WHEN r.display_title='' THEN substr(r.content, 1, 80)
                    ELSE r.display_title END,
               r.image_path, length(r.content)
        FROM clipboard_records AS r
        JOIN clipboard_records_fts ON clipboard_records_fts.rowid=r.row_id
        WHERE clipboard_records_fts MATCH ?1
        ORDER BY r.created_utc_ms DESC, r.row_id DESC
        LIMIT ?2;
    )sql" : R"sql(
        SELECT id, type,
               CASE WHEN length(content)>?3 THEN '' ELSE content END,
               CASE WHEN display_title='' THEN substr(content, 1, 80)
                    ELSE display_title END,
               image_path, length(content)
        FROM clipboard_records
        WHERE ?1 = ''
           OR instr(lower(display_title), lower(?1)) > 0
           OR instr(lower(content), lower(?1)) > 0
        ORDER BY created_utc_ms DESC, row_id DESC
        LIMIT ?2;
    )sql";
    SqliteStatement statement(connection.get(), sql);
    if (statement.get() == nullptr) return DatabaseQueryStatus::Failed;
    const std::string bound_query = use_fts
        ? '"' + query + '"'
        : query;
    if (sqlite3_bind_text(
            statement.get(), 1, bound_query.data(),
            static_cast<int>(bound_query.size()),
            SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(
            statement.get(), 2,
            static_cast<sqlite3_int64>((std::min)(
                limit, static_cast<size_t>((std::numeric_limits<int>::max)())))) !=
            SQLITE_OK) {
        return DatabaseQueryStatus::Failed;
    }
    if (sqlite3_bind_int64(
            statement.get(), 3,
            static_cast<sqlite3_int64>(kDirectTextCommitLimit)) != SQLITE_OK) {
        return DatabaseQueryStatus::Failed;
    }

    candidates->clear();
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const std::string id = SqliteText(statement.get(), 0);
        const int type = sqlite3_column_int(statement.get(), 1);
        const std::string content = SqliteText(statement.get(), 2);
        const std::string title = SqliteText(statement.get(), 3);
        const sqlite3_int64 content_length = sqlite3_column_int64(
            statement.get(), 5);

        Candidate candidate;
        candidate.text = Utf8ToWide(title.empty() ? content : title);
        candidate.full_content = Utf8ToWide(content);
        candidate.pinyin = "v" + query;
        candidate.covered_input_len = 1 + query.size();
        candidate.learnable = false;
        candidate.source = CandidateSource::Dynamic;
        if ((type == 1 ||
             content_length > static_cast<sqlite3_int64>(
                 kDirectTextCommitLimit)) &&
            !id.empty()) {
            candidate.action = CandidateAction::PasteClipboardRecord;
            candidate.action_data = Utf8ToWide(id);
        }
        candidates->push_back(std::move(candidate));
    }
    if (result != SQLITE_DONE) {
        candidates->clear();
        return DatabaseQueryStatus::Failed;
    }
    return DatabaseQueryStatus::Success;
}

std::optional<bool> DeleteClipboardDatabaseRecord(
    const std::wstring& full_content,
    const std::wstring& record_id) {
    const std::wstring path = GetClipboardDatabasePath();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt;
    }
    SqliteConnection connection(path);
    if (connection.get() == nullptr) return false;
    if (sqlite3_exec(
            connection.get(), "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
        return false;
    }

    bool deleted = false;
    std::string id;
    {
        const std::string stable_id = WideToUtf8(record_id);
        const std::string content = WideToUtf8(full_content);
        SqliteStatement select(connection.get(), R"sql(
            SELECT id FROM clipboard_records
            WHERE (?1 <> '' AND id=?1)
               OR (?1 = '' AND (content=?2 OR display_title=?2))
            LIMIT 1;
        )sql");
        if (select.get() != nullptr &&
            sqlite3_bind_text(
                select.get(), 1, stable_id.data(),
                static_cast<int>(stable_id.size()), SQLITE_TRANSIENT) ==
                SQLITE_OK &&
            sqlite3_bind_text(
                select.get(), 2, content.data(),
                static_cast<int>(content.size()), SQLITE_TRANSIENT) ==
                SQLITE_OK &&
            sqlite3_step(select.get()) == SQLITE_ROW) {
            id = SqliteText(select.get(), 0);
        }
    }
    if (!id.empty()) {
        SqliteStatement remove(
            connection.get(), "DELETE FROM clipboard_records WHERE id=?1;");
        if (remove.get() != nullptr &&
            sqlite3_bind_text(
                remove.get(), 1, id.data(), static_cast<int>(id.size()),
                SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_step(remove.get()) == SQLITE_DONE) {
            deleted = sqlite3_changes(connection.get()) > 0;
        }
    }
    const char* completion = deleted ? "COMMIT;" : "ROLLBACK;";
    if (sqlite3_exec(
            connection.get(), completion, nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
        return false;
    }
    return deleted;
}

bool WriteFileAtomically(
    const std::wstring& path, const std::string& content) {
    const std::wstring temporary_path = path + L".ime-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE file = CreateFileW(
        temporary_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool written = true;
    size_t offset = 0;
    while (offset < content.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(
            content.size() - offset, static_cast<size_t>(MAXDWORD)));
        DWORD bytes_written = 0;
        if (!WriteFile(file, content.data() + offset, chunk,
                       &bytes_written, nullptr) ||
            bytes_written != chunk) {
            written = false;
            break;
        }
        offset += bytes_written;
    }
    if (written) written = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);

    if (written) {
        written = MoveFileExW(
            temporary_path.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!written) DeleteFileW(temporary_path.c_str());
    return written;
}

// 安全的 JSON 字符串反转义函数
std::wstring SafeJsonUnescape(const std::string& input) {
    if (input.empty()) return {};

    // 1. 先将 UTF-8 转换为 std::wstring
    std::wstring winput = Utf8ToWide(input);
    std::wstring output;
    output.reserve(winput.size());

    for (size_t i = 0; i < winput.size(); ++i) {
        if (winput[i] == L'\\' && i + 1 < winput.size()) {
            wchar_t next = winput[i + 1];
            switch (next) {
            case L'\"': output.push_back(L'\"'); i++; break;
            case L'\\': output.push_back(L'\\'); i++; break;
            case L'/': output.push_back(L'/'); i++; break;
            case L'b': output.push_back(L'\b'); i++; break;
            case L'f': output.push_back(L'\f'); i++; break;
            case L'n': output.push_back(L'\n'); i++; break;
            case L'r': output.push_back(L'\r'); i++; break;
            case L't': output.push_back(L'\t'); i++; break;
            case L'u': {
                if (i + 5 < winput.size()) {
                    std::wstring hex_str = winput.substr(i + 2, 4);
                    wchar_t* end_ptr = nullptr;
                    unsigned long code = wcstoul(hex_str.c_str(), &end_ptr, 16);
                    if (end_ptr != hex_str.c_str()) {
                        output.push_back(static_cast<wchar_t>(code));
                        i += 5;
                        break;
                    }
                }
                output.push_back(winput[i]);
                break;
            }
            default:
                output.push_back(next);
                i++;
                break;
            }
        } else {
            output.push_back(winput[i]);
        }
    }
    return output;
}

ULONGLONG FileSize(const WIN32_FILE_ATTRIBUTE_DATA& attributes) noexcept {
    ULARGE_INTEGER size {};
    size.LowPart = attributes.nFileSizeLow;
    size.HighPart = attributes.nFileSizeHigh;
    return size.QuadPart;
}

void SkipJsonWhitespace(const std::string& json, size_t* position) {
    if (position == nullptr) return;
    while (*position < json.size() &&
           (json[*position] == ' ' || json[*position] == '\t' ||
            json[*position] == '\r' || json[*position] == '\n')) {
        ++*position;
    }
}

bool ReloadClipboardJsonLocked() {
    const std::wstring path = GetClipboardJsonPath();
    if (path.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA attr {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            g_cached_items.clear();
            g_last_loaded_ft = {};
            g_last_loaded_size = 0;
            g_last_loaded_path = path;
            g_cache_loaded = true;
            return true;
        }
        return false;
    }
    if (g_cache_loaded &&
        path == g_last_loaded_path &&
        attr.ftLastWriteTime.dwLowDateTime == g_last_loaded_ft.dwLowDateTime &&
        attr.ftLastWriteTime.dwHighDateTime == g_last_loaded_ft.dwHighDateTime &&
        FileSize(attr) == g_last_loaded_size) {
        return true;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();
    const size_t json_begin = json.find_first_not_of(" \t\r\n");
    const size_t json_end = json.find_last_not_of(" \t\r\n");
    if (json_begin == std::string::npos || json[json_begin] != '[' ||
        json_end == std::string::npos || json[json_end] != ']') {
        SHURU_LOG_WARN("clipboard history JSON is incomplete; keeping cache");
        return false;
    }

    std::vector<SimpleClipboardItem> items;
    size_t pos = json_begin + 1;
    SkipJsonWhitespace(json, &pos);
    while (pos < json_end) {
        if (json[pos] != '{') {
            SHURU_LOG_WARN("clipboard history JSON array item is invalid");
            return false;
        }
        const size_t obj_start = pos;

        // 跳过字符串及嵌套对象，保留未来新增的结构化字段。
        size_t obj_end = std::string::npos;
        bool in_string = false;
        bool is_escaped = false;
        int object_depth = 1;
        for (size_t k = obj_start + 1; k < json_end; ++k) {
            if (is_escaped) {
                is_escaped = false;
                continue;
            }
            if (json[k] == '\\') {
                is_escaped = true;
                continue;
            }
            if (json[k] == '\"') {
                in_string = !in_string;
                continue;
            }
            if (!in_string && json[k] == '{') {
                ++object_depth;
            } else if (!in_string && json[k] == '}' && --object_depth == 0) {
                obj_end = k;
                break;
            }
        }
        if (obj_end == std::string::npos) {
            SHURU_LOG_WARN("clipboard history JSON object is incomplete");
            return false;
        }

        std::string obj_str = json.substr(obj_start, obj_end - obj_start + 1);

        auto extract_field = [&](const std::string& field_name) -> std::string {
            std::string key = "\"" + field_name + "\"";
            size_t kpos = obj_str.find(key);
            if (kpos == std::string::npos) return {};
            kpos += key.size();
            SkipJsonWhitespace(obj_str, &kpos);
            if (kpos >= obj_str.size() || obj_str[kpos] != ':') return {};
            ++kpos;
            SkipJsonWhitespace(obj_str, &kpos);
            if (kpos < obj_str.size() && obj_str[kpos] == '\"') {
                size_t vstart = kpos + 1;
                size_t vend = std::string::npos;
                bool esc = false;
                for (size_t m = vstart; m < obj_str.size(); ++m) {
                    if (esc) {
                        esc = false;
                        continue;
                    }
                    if (obj_str[m] == '\\') {
                        esc = true;
                        continue;
                    }
                    if (obj_str[m] == '\"') {
                        vend = m;
                        break;
                    }
                }
                if (vend != std::string::npos && vend >= vstart) {
                    return obj_str.substr(vstart, vend - vstart);
                }
            }
            return {};
        };

        auto extract_integer_field = [&](const std::string& field_name) -> int {
            const std::string key = "\"" + field_name + "\"";
            size_t key_position = obj_str.find(key);
            if (key_position == std::string::npos) return 0;
            key_position += key.size();
            SkipJsonWhitespace(obj_str, &key_position);
            if (key_position >= obj_str.size() || obj_str[key_position] != ':')
                return 0;
            ++key_position;
            SkipJsonWhitespace(obj_str, &key_position);
            int value = 0;
            bool found_digit = false;
            while (key_position < obj_str.size() &&
                   obj_str[key_position] >= '0' &&
                   obj_str[key_position] <= '9') {
                found_digit = true;
                value = value * 10 + (obj_str[key_position] - '0');
                ++key_position;
            }
            return found_digit ? value : 0;
        };

        std::string id_str = extract_field("id");
        std::string content_str = extract_field("content");
        std::string title_str = extract_field("display_title");
        std::string image_path_str = extract_field("image_path");

        if (!content_str.empty()) {
            SimpleClipboardItem item;
            item.id = SafeJsonUnescape(id_str);
            item.type = extract_integer_field("type");
            item.content = SafeJsonUnescape(content_str);
            item.display_title = title_str.empty() ? item.content : SafeJsonUnescape(title_str);
            item.image_path = SafeJsonUnescape(image_path_str);
            item.serialized_object = std::move(obj_str);
            if (item.display_title.size() > 32) {
                item.display_title = item.display_title.substr(0, 32) + L"...";
            }
            items.push_back(std::move(item));
        }

        pos = obj_end + 1;
        SkipJsonWhitespace(json, &pos);
        if (pos == json_end) break;
        if (json[pos] != ',') {
            SHURU_LOG_WARN("clipboard history JSON array delimiter is invalid");
            return false;
        }
        ++pos;
        SkipJsonWhitespace(json, &pos);
        if (pos >= json_end) {
            SHURU_LOG_WARN("clipboard history JSON has a trailing delimiter");
            return false;
        }
    }

    g_cached_items = std::move(items);
    g_last_loaded_ft = attr.ftLastWriteTime;
    g_last_loaded_size = FileSize(attr);
    g_last_loaded_path = path;
    g_cache_loaded = true;
    return true;
}

bool SaveClipboardJsonLocked() {
    const std::wstring path = GetClipboardJsonPath();
    if (path.empty()) return false;

    std::ostringstream ss;
    ss << "[\n";
    for (size_t i = 0; i < g_cached_items.size(); ++i) {
        const auto& item = g_cached_items[i];
        ss << "  " << item.serialized_object
           << (i + 1 < g_cached_items.size() ? ",\n" : "\n");
    }
    ss << "]\n";

    if (!WriteFileAtomically(path, ss.str())) return false;

    WIN32_FILE_ATTRIBUTE_DATA attr {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
        g_last_loaded_ft = attr.ftLastWriteTime;
        g_last_loaded_size = FileSize(attr);
    }
    g_last_loaded_path = path;
    g_cache_loaded = true;
    return true;
}

std::wstring ToLowerWide(std::wstring text) {
    for (auto& ch : text) {
        ch = towlower(ch);
    }
    return text;
}

}  // namespace

bool ShouldPasteTextExternally(std::size_t text_length) noexcept {
    return text_length > kDirectTextCommitLimit;
}

bool CreateTextPasteRequest(
    const std::wstring& text,
    std::wstring* request_token) {
    if (request_token == nullptr || text.empty()) return false;
    request_token->clear();
    const std::wstring directory = GetTextPasteRequestDirectory();
    if (directory.empty()) return false;
    std::error_code directory_error;
    std::filesystem::create_directories(directory, directory_error);
    if (directory_error) return false;

    const std::wstring token = CreateRequestToken();
    const std::wstring path = TextPasteRequestPath(token);
    if (path.empty()) return false;
    const std::string encoded = WideToUtf8(text);
    if (encoded.empty() && !text.empty()) return false;
    const std::string payload = std::string(kTextPasteRequestHeader) + encoded;
    if (!WriteFileAtomically(path, payload)) return false;
    *request_token = token;
    return true;
}

bool DeleteTextPasteRequest(const std::wstring& request_token) noexcept {
    const std::wstring path = TextPasteRequestPath(request_token);
    if (path.empty()) return false;
    if (DeleteFileW(path.c_str())) return true;
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

std::vector<Candidate> GetClipboardCandidates(
    const std::string& query,
    size_t limit) {
    if (limit == 0) return {};
    ScopedSrwExclusiveLock process_lock(&g_clipboard_lock);

    std::vector<Candidate> database_candidates;
    const DatabaseQueryStatus database_status = QueryClipboardDatabase(
        query, limit, &database_candidates);
    if (database_status == DatabaseQueryStatus::Success) {
        return database_candidates;
    }

    ClipboardHistoryFileLock file_lock;
    if (file_lock.acquired()) {
        (void)ReloadClipboardJsonLocked();
    } else {
        SHURU_LOG_WARN("clipboard history lock timed out during query");
    }

    std::vector<Candidate> candidates;
    std::wstring query_w = ToLowerWide(Utf8ToWide(query));

    for (const auto& item : g_cached_items) {
        if (!query_w.empty()) {
            std::wstring title_lower = ToLowerWide(item.display_title);
            std::wstring content_lower = ToLowerWide(item.content);
            if (title_lower.find(query_w) == std::wstring::npos &&
                content_lower.find(query_w) == std::wstring::npos) {
                continue;
            }
        }

        Candidate cand;
        cand.text = item.display_title;
        cand.pinyin = "v" + query;
        cand.covered_input_len = 1 + query.size();
        cand.learnable = false;
        cand.source = CandidateSource::Dynamic;
        if ((item.type == 1 || ShouldPasteTextExternally(item.content.size())) &&
            !item.id.empty()) {
            cand.action = CandidateAction::PasteClipboardRecord;
            cand.action_data = item.id;
        } else {
            cand.full_content = item.content;
        }
        candidates.push_back(std::move(cand));

        if (candidates.size() >= limit) break;
    }

    return candidates;
}

bool DeleteClipboardCandidate(
    const std::wstring& full_content,
    const std::wstring& record_id) {
    ScopedSrwExclusiveLock process_lock(&g_clipboard_lock);
    if (const auto database_result =
            DeleteClipboardDatabaseRecord(full_content, record_id);
        database_result.has_value()) {
        return *database_result;
    }
    ClipboardHistoryFileLock file_lock;
    if (!file_lock.acquired()) {
        SHURU_LOG_WARN("clipboard history lock timed out during delete");
        return false;
    }
    if (!ReloadClipboardJsonLocked()) {
        SHURU_LOG_WARN("clipboard history reload failed during delete");
        return false;
    }

    auto it = std::find_if(g_cached_items.begin(), g_cached_items.end(),
        [&](const SimpleClipboardItem& item) {
            return item.content == full_content || item.display_title == full_content;
        });

    if (it != g_cached_items.end()) {
        const size_t index = static_cast<size_t>(
            std::distance(g_cached_items.begin(), it));
        SimpleClipboardItem removed = std::move(*it);
        g_cached_items.erase(it);
        if (SaveClipboardJsonLocked()) return true;
        g_cached_items.insert(
            g_cached_items.begin() + static_cast<std::ptrdiff_t>(index),
            std::move(removed));
        SHURU_LOG_WARN("clipboard history atomic replace failed");
    }
    return false;
}

std::vector<Candidate> GetCustomPhraseCandidates(
    const std::string& query,
    size_t limit) {
    std::vector<Candidate> candidates;
    if (limit == 0) return candidates;

    std::wstring query_w = ToLowerWide(Utf8ToWide(query));
    std::wstring dict_path = GetCustomPhrasePath();
    std::ifstream file(dict_path, std::ios::binary);
    if (!file.is_open()) return candidates;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::string code, phrase_utf8, pos_str;
        if (std::getline(ss, code, '\t') && std::getline(ss, phrase_utf8, '\t')) {
            std::wstring phrase_w = Utf8ToWide(phrase_utf8);
            std::wstring code_w = Utf8ToWide(code);

            if (!query_w.empty()) {
                std::wstring phrase_lower = ToLowerWide(phrase_w);
                std::wstring code_lower = ToLowerWide(code_w);
                if (code_lower.find(query_w) == std::wstring::npos &&
                    phrase_lower.find(query_w) == std::wstring::npos) {
                    continue;
                }
            }

            Candidate cand;
            cand.text = code_w + L": " + (phrase_w.size() > 28 ? phrase_w.substr(0, 28) + L"..." : phrase_w);
            cand.full_content = phrase_w;
            cand.pinyin = "vv" + query;
            cand.covered_input_len = 2 + query.size();
            cand.learnable = false;
            cand.source = CandidateSource::CustomPhrase;
            candidates.push_back(std::move(cand));

            if (candidates.size() >= limit) break;
        }
    }

    return candidates;
}

bool DeleteCustomPhraseCandidate(const std::wstring& phrase) {
    std::wstring dict_path = GetCustomPhrasePath();
    std::ifstream file(dict_path, std::ios::binary);
    if (!file.is_open()) return false;

    std::vector<std::string> lines;
    std::string line;
    bool found = false;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            lines.push_back(line);
            continue;
        }
        std::stringstream ss(line);
        std::string code, phrase_utf8;
        if (std::getline(ss, code, '\t') && std::getline(ss, phrase_utf8, '\t')) {
            std::wstring pw = Utf8ToWide(phrase_utf8);
            if (pw == phrase) {
                found = true;
                continue; // 过滤掉删除项
            }
        }
        lines.push_back(line);
    }
    file.close();

    if (found) {
        std::ofstream out(dict_path, std::ios::trunc | std::ios::binary);
        if (out.is_open()) {
            for (const auto& l : lines) {
                out << l << "\n";
            }
            out.close();
            return true;
        }
    }
    return false;
}

}  // namespace shuru
