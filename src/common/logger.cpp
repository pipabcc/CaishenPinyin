#include "logger.h"
#include "runtime_config.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace shuru {
namespace {

CRITICAL_SECTION g_log_cs;
INIT_ONCE g_log_once = INIT_ONCE_STATIC_INIT;
wchar_t g_log_path[MAX_PATH] = {};
HANDLE g_log_handle = INVALID_HANDLE_VALUE;
std::wstring g_open_path;
volatile LONG g_verbose_logging = -1;
constexpr LONGLONG kMaxLogFileBytes = 4LL * 1024 * 1024;

BOOL CALLBACK InitLogCs(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_log_cs);
    return TRUE;
}

void EnsureLogCs() {
    InitOnceExecuteOnce(&g_log_once, InitLogCs, nullptr, nullptr);
}

void CloseLogFileLocked() {
    if (g_log_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log_handle);
        g_log_handle = INVALID_HANDLE_VALUE;
    }
    g_open_path.clear();
}

bool OpenLogFileLocked(const wchar_t* path) {
    g_log_handle = CreateFileW(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (g_log_handle == INVALID_HANDLE_VALUE) {
        g_open_path.clear();
        return false;
    }
    g_open_path = path;
    return true;
}

bool RotateLogIfNeededLocked(const wchar_t* path, DWORD incoming_bytes) {
    LARGE_INTEGER size {};
    if (g_log_handle == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(g_log_handle, &size) ||
        size.QuadPart + incoming_bytes <= kMaxLogFileBytes) {
        return true;
    }
    CloseLogFileLocked();
    const std::wstring backup_path = std::wstring(path) + L".1";
    MoveFileExW(path, backup_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    return OpenLogFileLocked(path);
}

const char* LevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "LOG";
}

bool ShouldWrite(LogLevel level) {
    if (level == LogLevel::Warn || level == LogLevel::Error) {
        return true;
    }
    LONG cached = InterlockedCompareExchange(&g_verbose_logging, -1, -1);
    if (cached < 0) {
        wchar_t value[8] = {};
        const DWORD length = GetEnvironmentVariableW(
            L"FACAI_IME_VERBOSE_LOG", value, ARRAYSIZE(value));
        const LONG enabled = length > 0 && value[0] != L'0' ? 1 : 0;
        InterlockedCompareExchange(&g_verbose_logging, enabled, -1);
        cached = InterlockedCompareExchange(&g_verbose_logging, -1, -1);
    }
    return cached != 0 && GetRuntimeConfig().content_logging_enabled;
}

} // namespace

void SetLogFilePath(const std::wstring& path) {
    EnsureLogCs();
    EnterCriticalSection(&g_log_cs);
    CloseLogFileLocked();
    if (path.empty()) {
        g_log_path[0] = 0;
    } else {
        wcsncpy_s(g_log_path, path.c_str(), _TRUNCATE);
    }
    LeaveCriticalSection(&g_log_cs);
}

void LogWrite(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (!ShouldWrite(level)) {
        return;
    }
    char body[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    SYSTEMTIME st {};
    GetLocalTime(&st);

    char line_buf[2560];
    std::snprintf(
        line_buf,
        sizeof(line_buf),
        "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %s:%d %s\r\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        LevelName(level),
        file ? file : "?",
        line,
        body);

    wchar_t path[MAX_PATH] = {};
    EnsureLogCs();
    EnterCriticalSection(&g_log_cs);
    if (g_log_path[0] != 0) {
        wcsncpy_s(path, g_log_path, _TRUNCATE);
    } else {
        DWORD len = GetTempPathW(MAX_PATH, path);
        if (len == 0 || len >= MAX_PATH) {
            LeaveCriticalSection(&g_log_cs);
            return;
        }
        wcscat_s(path, L"CaishenIme.log");
    }

    if (g_log_handle == INVALID_HANDLE_VALUE || g_open_path != path) {
        CloseLogFileLocked();
        if (!OpenLogFileLocked(path)) {
            LeaveCriticalSection(&g_log_cs);
            return;
        }
    }

    const DWORD line_size = static_cast<DWORD>(strnlen_s(line_buf, sizeof(line_buf)));
    if (!RotateLogIfNeededLocked(path, line_size)) {
        LeaveCriticalSection(&g_log_cs);
        return;
    }
    DWORD written = 0;
    WriteFile(g_log_handle, line_buf, line_size, &written, nullptr);
    LeaveCriticalSection(&g_log_cs);
}

void ShutdownLogger() {
    EnsureLogCs();
    EnterCriticalSection(&g_log_cs);
    CloseLogFileLocked();
    LeaveCriticalSection(&g_log_cs);
}

} // namespace shuru
