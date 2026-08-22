#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <cwctype>
#include <string>

namespace shuru {

// 当前进程是否运行在 AppContainer 沙箱中（例如开始菜单搜索框宿主
// SearchHost.exe）。沙箱进程既无权修改文件 DACL，也无法启动包外进程，
// 两类操作都必须由普通宿主代劳。
inline bool IsCurrentProcessAppContainer() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD is_app_container = 0;
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(
        token, TokenIsAppContainer, &is_app_container,
        sizeof(is_app_container), &size);
    CloseHandle(token);
    return ok != FALSE && is_app_container != 0;
}

inline std::wstring ReadUserDataEnvironmentValue(const wchar_t* name) {
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

inline bool IsPackageVirtualizedLocalAppData(
    const std::wstring& local_app_data) {
    if (local_app_data.empty()) return false;
    std::wstring normalized(local_app_data);
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    const bool has_packages_segment =
        normalized.find(L"\\packages\\") != std::wstring::npos;
    if (!has_packages_segment) return false;
    // Windows Store/packaged desktop hosts commonly use either the AC
    // redirected root or LocalCache\Local.  Both are process-local and must
    // not become the source of IME settings, skins, learning, or statistics.
    const auto has_segment = [&](const wchar_t* segment) {
        const std::size_t position = normalized.find(segment);
        if (position == std::wstring::npos) return false;
        const std::size_t end = position + wcslen(segment);
        return end == normalized.size() || normalized[end] == L'\\';
    };
    return has_segment(L"\\ac") || has_segment(L"\\localcache");
}

inline std::wstring ResolveCanonicalLocalAppData(
    const std::wstring& local_app_data,
    const std::wstring& user_profile) {
    if (!IsPackageVirtualizedLocalAppData(local_app_data) ||
        user_profile.empty()) {
        return local_app_data;
    }
    return user_profile + L"\\AppData\\Local";
}

inline std::wstring CaishenLocalAppData() {
    const std::wstring local_app_data =
        ReadUserDataEnvironmentValue(L"LOCALAPPDATA");
    std::wstring user_profile =
        ReadUserDataEnvironmentValue(L"USERPROFILE");
    if (user_profile.empty()) {
        user_profile = ReadUserDataEnvironmentValue(L"HOMEDRIVE") +
            ReadUserDataEnvironmentValue(L"HOMEPATH");
    }
    const std::wstring resolved = ResolveCanonicalLocalAppData(
        local_app_data, user_profile);
    return resolved.empty() ? local_app_data : resolved;
}

inline std::wstring CaishenUserDataPath(const wchar_t* relative_path) {
    if (relative_path == nullptr || *relative_path == L'\0') return {};
    const std::wstring root = CaishenLocalAppData();
    if (root.empty()) return {};
    return root + L"\\CaishenPinyin\\" + relative_path;
}

}  // namespace shuru
