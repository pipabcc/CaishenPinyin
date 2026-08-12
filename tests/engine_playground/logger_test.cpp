#include "common/logger.h"

#include <Windows.h>

#include <cstdio>
#include <string>

namespace {

bool GetSize(const std::wstring& path, ULONGLONG* size) {
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    *size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
}

}  // namespace

int wmain() {
    wchar_t temp_dir[MAX_PATH] = {};
    if (GetTempPathW(ARRAYSIZE(temp_dir), temp_dir) == 0) {
        return 1;
    }
    const std::wstring path = std::wstring(temp_dir) +
                              L"FacaiIme-logger-test-" +
                              std::to_wstring(GetCurrentProcessId()) + L".log";
    const std::wstring backup_path = path + L".1";
    DeleteFileW(path.c_str());
    DeleteFileW(backup_path.c_str());

    shuru::SetLogFilePath(path);
    std::string payload(1900, 'x');
    for (int i = 0; i < 2600; ++i) {
        shuru::LogWrite(shuru::LogLevel::Warn, __FILE__, __LINE__, "%04d %s", i, payload.c_str());
    }
    shuru::ShutdownLogger();

    ULONGLONG active_size = 0;
    ULONGLONG backup_size = 0;
    const bool ok = GetSize(path, &active_size) &&
                    GetSize(backup_path, &backup_size) &&
                    active_size > 0 && active_size <= 4ULL * 1024 * 1024 &&
                    backup_size > 0 && backup_size <= 4ULL * 1024 * 1024;

    DeleteFileW(path.c_str());
    DeleteFileW(backup_path.c_str());
    if (!ok) {
        std::fprintf(stderr, "logger rotation failed: active=%llu backup=%llu\n",
                     active_size, backup_size);
        return 1;
    }
    std::printf("logger rotation passed: active=%llu backup=%llu\n",
                active_size, backup_size);
    return 0;
}
