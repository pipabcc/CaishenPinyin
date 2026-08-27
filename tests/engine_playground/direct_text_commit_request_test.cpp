#include "ime/direct_text_commit_request.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#define CHECK(x) do { if (!(x)) { \
    std::cerr << "check failed line " << __LINE__ << '\n'; return 1; } } while (0)

namespace {

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

}  // namespace

int wmain() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        (L"caishen-direct-commit-" +
         std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    CHECK(fs::create_directories(root, error) && !error);
    CHECK(SetEnvironmentVariableW(
        L"CAISHEN_DIRECT_COMMIT_REQUEST_DIR", root.c_str()));

    const std::wstring token = shuru::CreateDirectTextCommitToken();
    CHECK(shuru::IsDirectTextCommitToken(token));
    CHECK(shuru::PrepareDirectTextCommitSession(token));

    const fs::path request = shuru::DirectTextCommitRequestPath(token);
    const fs::path result = shuru::DirectTextCommitResultPath(token);
    const std::string payload = "CAISHEN_DIRECT_COMMIT_V1\n中文\r\n😀";
    {
        std::ofstream output(request, std::ios::binary);
        output << payload;
    }
    std::wstring text;
    CHECK(shuru::ReadAndDeleteDirectTextCommitRequest(token, &text) ==
          shuru::DirectTextCommitReadResult::Ready);
    CHECK(text == L"中文\r\n😀");
    CHECK(!fs::exists(request));

    CHECK(shuru::WriteDirectTextCommitResult(
        token, shuru::DirectTextCommitResult::Success));
    CHECK(ReadFile(result) ==
          "CAISHEN_DIRECT_COMMIT_RESULT_V1\nsuccess\n");
    shuru::DeleteDirectTextCommitSessionFiles(token);
    CHECK(!fs::exists(result));

    CHECK(!shuru::IsDirectTextCommitToken(L"../invalid"));
    CHECK(shuru::DirectTextCommitRequestPath(L"../invalid").empty());
    constexpr std::uint64_t header_size =
        sizeof("CAISHEN_DIRECT_COMMIT_V1\n") - 1;
    CHECK(!shuru::IsDirectTextCommitPayloadSizeValid(header_size));
    CHECK(shuru::IsDirectTextCommitPayloadSizeValid(header_size + 1));
    CHECK(shuru::IsDirectTextCommitPayloadSizeValid(
        header_size + shuru::kDirectTextCommitMaximumPayloadBytes));
    CHECK(!shuru::IsDirectTextCommitPayloadSizeValid(
        header_size + shuru::kDirectTextCommitMaximumPayloadBytes + 1));
    const fs::path cancel = shuru::DirectTextCommitCancelPath(token);
    { std::ofstream output(cancel); output << "cancel\n"; }
    CHECK(shuru::ConsumeDirectTextCommitCancellation(token));
    CHECK(!fs::exists(cancel));
    SetEnvironmentVariableW(L"CAISHEN_DIRECT_COMMIT_REQUEST_DIR", nullptr);
    fs::remove_all(root, error);
    std::cout << "direct_text_commit_request: OK\n";
    return 0;
}
