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

// {34745C63-B2F0-4784-8B67-5E12C8701A31}
static const GUID kTfCatTipKeyboard = {
    0x34745C63, 0xB2F0, 0x4784, {0x8B, 0x67, 0x5E, 0x12, 0xC8, 0x70, 0x1A, 0x31}};

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

std::wstring GetModulePath() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(g_module, path, MAX_PATH) == 0) {
        return {};
    }
    return path;
}

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

    hr = category_mgr->RegisterCategory(CLSID_ShuruTextService, kTfCatTipKeyboard, CLSID_ShuruTextService);
    if (SUCCEEDED(hr)) {
        hr = category_mgr->RegisterCategory(CLSID_ShuruTextService, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, CLSID_ShuruTextService);
        if (FAILED(hr)) {
            SHURU_LOG_WARN("RegisterCategory display attribute provider failed: 0x%08X", hr);
            hr = S_OK; // 非致命
        }
    }
    category_mgr->Release();
    if (FAILED(hr)) {
        SHURU_LOG_ERROR("RegisterCategory keyboard failed: 0x%08X", hr);
        return hr;
    }
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
    category_mgr->UnregisterCategory(CLSID_ShuruTextService, kTfCatTipKeyboard, CLSID_ShuruTextService);
    category_mgr->UnregisterCategory(CLSID_ShuruTextService, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, CLSID_ShuruTextService);
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

    hr = profiles->AddLanguageProfile(
        CLSID_ShuruTextService,
        SHURU_LANGID,
        GUID_ShuruProfile,
        product_name.c_str(),
        static_cast<ULONG>(product_name.size()),
        nullptr,
        0,
        0);
    if (FAILED(hr)) {
        profiles->Release();
        SHURU_LOG_ERROR("AddLanguageProfile failed: 0x%08X", hr);
        return hr;
    }

    // 双写注册表 Description（HKLM/HKCU），避免输入法列表缓存旧名。
    {
        const std::wstring tip =
            L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + GuidToString(CLSID_ShuruTextService) +
            L"\\LanguageProfile\\0x00000804\\" + GuidToString(GUID_ShuruProfile);
        SetRegistryKeyValue(HKEY_LOCAL_MACHINE, tip, L"Description", product_name);
        SetRegistryKeyValue(HKEY_CURRENT_USER, tip, L"Description", product_name);
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
