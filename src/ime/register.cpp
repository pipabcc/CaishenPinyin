#include "register.h"

#include "globals.h"
#include "../common/com_utils.h"
#include "../common/guid_def.h"
#include "../common/logger.h"
#include "../common/runtime_config.h"
#include "../common/version.h"

#include <msctf.h>
#include <cwchar>
#include <string>

namespace shuru {
namespace {

HRESULT SetRegistryKeyValue(HKEY root, const std::wstring& key_path, const std::wstring& value_name, const std::wstring& value) {
    HKEY key = nullptr;
    LONG err = RegCreateKeyExW(root, key_path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
    if (err != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(err);
    }
    err = RegSetValueExW(
        key,
        value_name.empty() ? nullptr : value_name.c_str(),
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(err);
}

HRESULT SetRegistryKeyValueDWORD(HKEY root, const std::wstring& key_path, const std::wstring& value_name, DWORD value) {
    HKEY key = nullptr;
    LONG err = RegCreateKeyExW(root, key_path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
    if (err != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(err);
    }
    err = RegSetValueExW(
        key,
        value_name.empty() ? nullptr : value_name.c_str(),
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(DWORD));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(err);
}

std::wstring GetModulePath() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(g_module, path, MAX_PATH) == 0) {
        return {};
    }
    return path;
}

// {49D2F9CE-1F5E-11D7-A6D3-00065B84435C} - 双模式（桌面+沉浸）
static const GUID kGuidCatDualMode = {
    0x49d2f9ce, 0x1f5e, 0x11d7,
    {0xa6, 0xd3, 0x00, 0x06, 0x5b, 0x84, 0x43, 0x5c}};

// {CCF05DD7-4A87-11D7-A6E2-00065B84435C} - 输入法覆盖支持
static const GUID kGuidCatInputMethodOverride = {
    0xccf05dd7, 0x4a87, 0x11d7,
    {0xa6, 0xe2, 0x00, 0x06, 0x5b, 0x84, 0x43, 0x5c}};

static const GUID kTipCategories[] = {
    GUID_TFCAT_TIP_KEYBOARD,
    GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    GUID_TFCAT_TIPCAP_COMLESS,
    kGuidCatDualMode,
    kGuidCatInputMethodOverride,
};

HRESULT RegisterTipCategory() {
    ITfCategoryMgr* category_mgr = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_TF_CategoryMgr,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ITfCategoryMgr,
        reinterpret_cast<void**>(&category_mgr));
    if (FAILED(hr)) {
        SHURU_LOG_ERROR("CoCreate ITfCategoryMgr failed: 0x%08X", hr);
        return hr;
    }

    for (const auto& cat : kTipCategories) {
        hr = category_mgr->RegisterCategory(CLSID_ShuruTextService, cat, CLSID_ShuruTextService);
        if (FAILED(hr)) {
            SHURU_LOG_WARN("RegisterCategory failed: 0x%08X", hr);
        }
    }
    category_mgr->Release();
    SHURU_LOG_INFO("RegisterTipCategory ok");
    return S_OK;
}

HRESULT UnregisterTipCategory() {
    ITfCategoryMgr* category_mgr = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_TF_CategoryMgr,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ITfCategoryMgr,
        reinterpret_cast<void**>(&category_mgr));
    if (FAILED(hr)) {
        return hr;
    }
    for (const auto& cat : kTipCategories) {
        category_mgr->UnregisterCategory(CLSID_ShuruTextService, cat, CLSID_ShuruTextService);
    }
    category_mgr->Release();
    return S_OK;
}

} // namespace

HRESULT RegisterServer() {
    const std::wstring clsid = GuidToString(CLSID_ShuruTextService);
    if (clsid.empty()) {
        return E_FAIL;
    }

    const std::wstring product_name = GetRuntimeConfig().display_name;
    const std::wstring clsid_key = L"CLSID\\" + clsid;
    HRESULT hr = SetRegistryKeyValue(HKEY_CLASSES_ROOT, clsid_key, L"", product_name);
    if (FAILED(hr)) {
        return hr;
    }

    const std::wstring inproc = clsid_key + L"\\InprocServer32";
    hr = SetRegistryKeyValue(HKEY_CLASSES_ROOT, inproc, L"", GetModulePath());
    if (FAILED(hr)) {
        return hr;
    }
    hr = SetRegistryKeyValue(HKEY_CLASSES_ROOT, inproc, L"ThreadingModel", L"Apartment");
    if (FAILED(hr)) {
        return hr;
    }

    SHURU_LOG_INFO("RegisterServer ok");
    return S_OK;
}

HRESULT UnregisterServer() {
    const std::wstring clsid = GuidToString(CLSID_ShuruTextService);
    if (clsid.empty()) {
        return E_FAIL;
    }
    const std::wstring clsid_key = L"CLSID\\" + clsid;
    const LONG err = RegDeleteTreeW(HKEY_CLASSES_ROOT, clsid_key.c_str());
    if (err != ERROR_SUCCESS && err != ERROR_FILE_NOT_FOUND) {
        return HRESULT_FROM_WIN32(err);
    }
    SHURU_LOG_INFO("UnregisterServer ok");
    return S_OK;
}

HRESULT RegisterProfile() {
    const std::wstring product_name = GetRuntimeConfig().display_name;
    HRESULT hr = RegisterTipCategory();
    if (FAILED(hr)) {
        return hr;
    }

    ITfInputProcessorProfiles* profiles = nullptr;
    hr = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfiles,
        reinterpret_cast<void**>(&profiles));
    if (FAILED(hr)) {
        SHURU_LOG_ERROR("CoCreate ITfInputProcessorProfiles failed: 0x%08X", hr);
        return hr;
    }

    hr = profiles->Register(CLSID_ShuruTextService);
    if (FAILED(hr)) {
        profiles->Release();
        return hr;
    }

    // 先移除再添加，确保 Windows 输入法列表刷新自定义名称。
    profiles->RemoveLanguageProfile(CLSID_ShuruTextService, SHURU_LANGID, GUID_ShuruProfile);

    const std::wstring module_path = GetModulePath();

    hr = profiles->AddLanguageProfile(
        CLSID_ShuruTextService,
        SHURU_LANGID,
        GUID_ShuruProfile,
        product_name.c_str(),
        static_cast<ULONG>(product_name.size()),
        module_path.c_str(),
        static_cast<ULONG>(module_path.size()),
        0);
    if (FAILED(hr)) {
        profiles->Release();
        SHURU_LOG_ERROR("AddLanguageProfile failed: 0x%08X", hr);
        return hr;
    }

    // 双写注册表 Description/Display Description/IconFile/IconIndex（HKLM/HKCU），确保输入法列表与托盘刷新自定义图标与名称。
    {
        const std::wstring tip =
            L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + GuidToString(CLSID_ShuruTextService) +
            L"\\LanguageProfile\\0x00000804\\" + GuidToString(GUID_ShuruProfile);
        SetRegistryKeyValue(HKEY_LOCAL_MACHINE, tip, L"Description", product_name);
        SetRegistryKeyValue(HKEY_LOCAL_MACHINE, tip, L"Display Description", product_name);
        if (!module_path.empty()) {
            SetRegistryKeyValue(HKEY_LOCAL_MACHINE, tip, L"IconFile", module_path);
            SetRegistryKeyValueDWORD(HKEY_LOCAL_MACHINE, tip, L"IconIndex", 0);
        }

        SetRegistryKeyValue(HKEY_CURRENT_USER, tip, L"Description", product_name);
        SetRegistryKeyValue(HKEY_CURRENT_USER, tip, L"Display Description", product_name);
        if (!module_path.empty()) {
            SetRegistryKeyValue(HKEY_CURRENT_USER, tip, L"IconFile", module_path);
            SetRegistryKeyValueDWORD(HKEY_CURRENT_USER, tip, L"IconIndex", 0);
        }

        SetRegistryKeyValue(HKEY_CLASSES_ROOT, L"CLSID\\" + GuidToString(CLSID_ShuruTextService), L"", product_name);
    }

    // Enable for current user
    hr = profiles->EnableLanguageProfile(CLSID_ShuruTextService, SHURU_LANGID, GUID_ShuruProfile, TRUE);
    if (FAILED(hr)) {
        SHURU_LOG_WARN("EnableLanguageProfile hr=0x%08X", hr);
    }

    hr = profiles->EnableLanguageProfileByDefault(CLSID_ShuruTextService, SHURU_LANGID, GUID_ShuruProfile, TRUE);
    if (FAILED(hr)) {
        SHURU_LOG_WARN("EnableLanguageProfileByDefault hr=0x%08X", hr);
    }

    profiles->Release();
    SHURU_LOG_INFO("RegisterProfile ok");
    return S_OK;
}

HRESULT UnregisterProfile() {
    UnregisterTipCategory();

    ITfInputProcessorProfiles* profiles = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfiles,
        reinterpret_cast<void**>(&profiles));
    if (FAILED(hr)) {
        return hr;
    }

    profiles->RemoveLanguageProfile(CLSID_ShuruTextService, SHURU_LANGID, GUID_ShuruProfile);
    hr = profiles->Unregister(CLSID_ShuruTextService);
    profiles->Release();
    SHURU_LOG_INFO("UnregisterProfile hr=0x%08X", hr);
    return hr;
}
} // namespace shuru
