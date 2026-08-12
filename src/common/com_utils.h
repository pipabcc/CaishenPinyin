#pragma once

#include <Windows.h>
#include <objbase.h>
#include <string>

namespace shuru {

inline std::wstring GuidToString(const GUID& guid) {
    wchar_t buf[64] = {};
    if (StringFromGUID2(guid, buf, 64) == 0) {
        return {};
    }
    return buf;
}

inline std::wstring GetModuleDirectory(HINSTANCE module) {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    std::wstring full(path);
    const size_t pos = full.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return {};
    }
    return full.substr(0, pos);
}

inline std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], needed);
    return out;
}

inline std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], needed, nullptr, nullptr);
    return out;
}

template <typename T>
inline void SafeRelease(T** obj) {
    if (obj && *obj) {
        (*obj)->Release();
        *obj = nullptr;
    }
}

} // namespace shuru
