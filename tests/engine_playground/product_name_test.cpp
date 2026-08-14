#include "common/version.h"
#include "common/runtime_config.h"

#include <Windows.h>

#include <cwchar>
#include <iostream>
#include <string>

namespace {

bool ReadRegistryString(
    HKEY root,
    const wchar_t* key_path,
    const wchar_t* value_name,
    std::wstring* value) {
    if (value == nullptr) {
        return false;
    }
    DWORD bytes = 0;
    const LSTATUS size_status = RegGetValueW(
        root, key_path, value_name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (size_status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return false;
    }

    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    const LSTATUS read_status = RegGetValueW(
        root, key_path, value_name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes);
    if (read_status != ERROR_SUCCESS) {
        return false;
    }
    buffer.resize(wcsnlen_s(buffer.c_str(), buffer.size()));
    *value = std::move(buffer);
    return true;
}

}  // namespace

int wmain() {
    const std::wstring product_name = shuru::GetRuntimeConfig().display_name;
    if (product_name != L"财神输入法") {
        std::wcerr << L"产品名不是财神输入法，实际为: " << product_name << L'\n';
        return 1;
    }

    // 注册表不存在时不阻止纯构建环境测试；存在时必须与产品名一致。
    constexpr wchar_t kProfilePath[] =
        L"SOFTWARE\\Microsoft\\CTF\\TIP\\{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}"
        L"\\LanguageProfile\\0x00000804\\{3A8B5C2E-9D1F-4E6A-B7C8-5D2E9F1A3B6C}";
    std::wstring registered_description;
    if (ReadRegistryString(
            HKEY_LOCAL_MACHINE,
            kProfilePath,
            L"Description",
            &registered_description) &&
        registered_description != product_name) {
        std::wcerr << L"已注册输入法名称仍为: " << registered_description << L'\n';
        return 2;
    }

    std::wcout << L"product_name: 财神输入法\n";
    return 0;
}
