#pragma once

#include <string>

namespace shuru {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

void SetLogFilePath(const std::wstring& path);
void ShutdownLogger();
void LogWrite(LogLevel level, const char* file, int line, const char* fmt, ...);

} // namespace shuru

#define SHURU_LOG_DEBUG(...) ::shuru::LogWrite(::shuru::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define SHURU_LOG_INFO(...)  ::shuru::LogWrite(::shuru::LogLevel::Info,  __FILE__, __LINE__, __VA_ARGS__)
#define SHURU_LOG_WARN(...)  ::shuru::LogWrite(::shuru::LogLevel::Warn,  __FILE__, __LINE__, __VA_ARGS__)
#define SHURU_LOG_ERROR(...) ::shuru::LogWrite(::shuru::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
