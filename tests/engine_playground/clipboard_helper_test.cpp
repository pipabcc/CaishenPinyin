#include "common/logger.h"
#include "engine/clipboard_helper.h"

#include <Windows.h>
#include <winsqlite/winsqlite3.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "failed at line " << __LINE__ << ": " #condition "\n"; \
            return 1; \
        } \
    } while (false)

bool WriteUtf8(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(output);
}

std::string ReadUtf8(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

bool CreateClipboardDatabase(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    const std::string utf8_path = path.u8string();
    if (sqlite3_open_v2(
            utf8_path.c_str(), &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (database != nullptr) sqlite3_close_v2(database);
        return false;
    }
    const char* sql = u8R"sql(
        CREATE TABLE clipboard_records (
            row_id INTEGER PRIMARY KEY AUTOINCREMENT,
            id TEXT NOT NULL UNIQUE,
            type INTEGER NOT NULL,
            content TEXT NOT NULL,
            display_title TEXT NOT NULL,
            image_path TEXT NOT NULL DEFAULT '',
            created_utc_ms INTEGER NOT NULL,
            metadata_json TEXT NOT NULL DEFAULT '{}'
        );
        INSERT INTO clipboard_records(
            id, type, content, display_title, image_path, created_utc_ms)
        VALUES
            ('db-text', 0, 'SQLite 文本内容', 'SQLite 文本', '', 100),
            ('db-image', 1, 'C:\temp\db.png', '[图片] db.png',
             'C:\temp\db.png', 200);
        CREATE VIRTUAL TABLE clipboard_records_fts USING fts5(
            display_title,
            content,
            content='clipboard_records',
            content_rowid='row_id',
            tokenize='trigram'
        );
        INSERT INTO clipboard_records_fts(clipboard_records_fts)
        VALUES('rebuild');
    )sql";
    int result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    if (result == SQLITE_OK) {
        sqlite3_stmt* insert = nullptr;
        result = sqlite3_prepare_v2(database, R"sql(
            INSERT INTO clipboard_records(
                id, type, content, display_title, image_path, created_utc_ms)
            VALUES('db-long', 0, ?1, '十八万字符记录', '', 50);
        )sql", -1, &insert, nullptr);
        const std::string long_text(180000, 'x');
        if (result == SQLITE_OK) {
            result = sqlite3_bind_text(
                insert, 1, long_text.data(),
                static_cast<int>(long_text.size()), SQLITE_TRANSIENT);
        }
        if (result == SQLITE_OK && sqlite3_step(insert) != SQLITE_DONE) {
            result = SQLITE_ERROR;
        }
        if (insert != nullptr) sqlite3_finalize(insert);
    }
    sqlite3_close_v2(database);
    return result == SQLITE_OK;
}

}  // namespace

int main() {
    using namespace shuru;

    wchar_t temporary_root[MAX_PATH] {};
    CHECK(GetTempPathW(ARRAYSIZE(temporary_root), temporary_root) != 0);
    const std::filesystem::path test_directory =
        std::filesystem::path(temporary_root) /
        (L"caishen-clipboard-test-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    CHECK(std::filesystem::create_directories(test_directory));
    CHECK(SetEnvironmentVariableW(
        L"CAISHEN_CLIPBOARD_DATA_DIR",
        test_directory.c_str()) != FALSE);
    SetLogFilePath((test_directory / L"clipboard-test.log").wstring());

    const std::filesystem::path history = test_directory / L"history.json";
    const std::string initial_json =
        u8"[\n"
        u8"  {\"id\":\"one\",\"type\":0,\"content\":\"第一行\\r\\n第二行\","
        u8"\"display_title\":\"多行文本\",\"future\":{\"nested\":true}},\n"
        u8"  {\"id\":\"two\",\"type\":0,\"content\":\"保留字段测试\","
        u8"\"display_title\":\"待删除\",\"future_text\":\"必须保留\"},\n"
        u8"  {\"id\":\"image\",\"type\":1,\"content\":\"C:\\\\temp\\\\a.png\","
        u8"\"display_title\":\"[图片] a.png\",\"image_path\":\"C:\\\\temp\\\\a.png\"}\n"
        u8"]\n";
    CHECK(WriteUtf8(history, initial_json));

    const auto all = GetClipboardCandidates("", 10);
    CHECK(all.size() == 3);
    CHECK(all[0].text == L"多行文本");
    CHECK(all[0].full_content == L"第一行\r\n第二行");
    CHECK(all[0].covered_input_len == 1 && !all[0].learnable);
    const auto filtered = GetClipboardCandidates("字段", 10);
    CHECK(filtered.size() == 1 && filtered[0].text == L"待删除");
    CHECK(GetClipboardCandidates("", 0).empty());
    CHECK(all[2].action == CandidateAction::PasteClipboardRecord);
    CHECK(all[2].action_data == L"image");

    CHECK(DeleteClipboardCandidate(L"保留字段测试"));
    const std::string after_delete = ReadUtf8(history);
    CHECK(after_delete.find("\"id\":\"two\"") == std::string::npos);
    CHECK(after_delete.find("\"future\":{\"nested\":true}") != std::string::npos);
    CHECK(after_delete.find("\"image_path\"") != std::string::npos);
    CHECK(GetClipboardCandidates("", 10).size() == 2);

    const std::string malformed_json =
        u8"[{\"content\":\"不得采用\",\"display_title\":\"损坏\"} garbage]";
    CHECK(WriteUtf8(history, malformed_json));
    const auto cached_after_corruption = GetClipboardCandidates("", 10);
    CHECK(cached_after_corruption.size() == 2);
    CHECK(!DeleteClipboardCandidate(L"第一行\r\n第二行"));
    CHECK(ReadUtf8(history) == malformed_json);

    const std::filesystem::path database = test_directory / L"history.db";
    CHECK(CreateClipboardDatabase(database));
    const auto database_items = GetClipboardCandidates("", 10);
    CHECK(database_items.size() == 3);
    CHECK(database_items[0].action == CandidateAction::PasteClipboardRecord);
    CHECK(database_items[0].action_data == L"db-image");
    CHECK(database_items[0].full_content == L"C:\\temp\\db.png");
    CHECK(database_items[1].text == L"SQLite 文本");
    CHECK(database_items[2].text == L"十八万字符记录");
    CHECK(database_items[2].full_content.empty());
    CHECK(database_items[2].action == CandidateAction::PasteClipboardRecord);
    CHECK(database_items[2].action_data == L"db-long");
    const auto database_filtered = GetClipboardCandidates("SQLite", 10);
    CHECK(database_filtered.size() == 1);
    CHECK(database_filtered[0].action == CandidateAction::CommitText);
    CHECK(DeleteClipboardCandidate(
        database_items[0].full_content, database_items[0].action_data));
    const auto database_after_delete = GetClipboardCandidates("", 10);
    CHECK(database_after_delete.size() == 2);
    CHECK(database_after_delete[0].action == CandidateAction::CommitText);

    CHECK(!ShouldPasteTextExternally(kDirectTextCommitLimit));
    CHECK(ShouldPasteTextExternally(kDirectTextCommitLimit + 1));
    const std::filesystem::path request_directory =
        test_directory / L"paste-requests";
    CHECK(SetEnvironmentVariableW(
        L"CAISHEN_PASTE_REQUEST_DIR", request_directory.c_str()) != FALSE);
    const std::wstring large_request(kDirectTextCommitLimit + 1, L'x');
    std::wstring request_token;
    CHECK(CreateTextPasteRequest(large_request, &request_token));
    CHECK(request_token.size() == 32);
    const std::filesystem::path request_path =
        request_directory / (request_token + L".txt");
    const std::string request_payload = ReadUtf8(request_path);
    CHECK(request_payload ==
        std::string("CAISHEN_TEXT_PASTE_V1\n") +
        std::string(kDirectTextCommitLimit + 1, 'x'));
    CHECK(DeleteTextPasteRequest(request_token));
    CHECK(!std::filesystem::exists(request_path));
    SetEnvironmentVariableW(L"CAISHEN_PASTE_REQUEST_DIR", nullptr);

    for (const auto& entry : std::filesystem::directory_iterator(test_directory)) {
        CHECK(entry.path().extension() != L".tmp");
    }

    ShutdownLogger();
    SetEnvironmentVariableW(L"CAISHEN_CLIPBOARD_DATA_DIR", nullptr);
    std::error_code cleanup_error;
    std::filesystem::remove_all(test_directory, cleanup_error);
    CHECK(!cleanup_error);
    std::cout << "clipboard_helper: OK\n";
    return 0;
}
