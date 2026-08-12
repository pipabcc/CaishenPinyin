#include "common/guid_def.h"
#include "engine/pinyin_engine.h"
#include <Windows.h>
#include <msctf.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using GetClassFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

class ScopedHealthLocalAppData {
public:
    bool Initialize() {
        DWORD original_length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (original_length > 0) {
            original_.resize(original_length);
            const DWORD written = GetEnvironmentVariableW(
                L"LOCALAPPDATA", original_.data(), original_length);
            if (written == 0 || written >= original_length) return false;
            original_.resize(written);
            had_original_ = true;
        }

        try {
            directory_ = std::filesystem::temp_directory_path() /
                (L"facai-release-health-" + std::to_wstring(GetCurrentProcessId()) +
                 L"-" + std::to_wstring(GetTickCount64()));
            std::filesystem::create_directories(directory_);
        } catch (...) {
            return false;
        }

        if (!SetEnvironmentVariableW(L"LOCALAPPDATA", directory_.c_str())) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(directory_, cleanup_error);
            return false;
        }
        active_ = true;
        return true;
    }

    ~ScopedHealthLocalAppData() {
        if (!active_) return;
        SetEnvironmentVariableW(
            L"LOCALAPPDATA", had_original_ ? original_.c_str() : nullptr);
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory_, cleanup_error);
    }

    const std::filesystem::path& directory() const noexcept { return directory_; }

private:
    std::filesystem::path directory_;
    std::wstring original_;
    bool had_original_ = false;
    bool active_ = false;
};

bool SamePath(const std::wstring& left, const std::wstring& right) {
    wchar_t normalized_left[MAX_PATH] {};
    wchar_t normalized_right[MAX_PATH] {};
    GetFullPathNameW(left.c_str(), MAX_PATH, normalized_left, nullptr);
    GetFullPathNameW(right.c_str(), MAX_PATH, normalized_right, nullptr);
    return _wcsicmp(normalized_left, normalized_right) == 0;
}

static bool ContainsTextInFirst(const shuru::EngineQueryResult& result,
                                const std::wstring& text,
                                size_t limit) {
    const size_t end = std::min(limit, result.candidates.size());
    return std::any_of(
        result.candidates.begin(), result.candidates.begin() + end,
        [&text](const shuru::Candidate& candidate) { return candidate.text == text; });
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fwprintf(
            stderr, L"usage: release_health_check <dll> <lexicon> [--registered]\n");
        return 2;
    }

    ScopedHealthLocalAppData isolated_user_data;
    if (!isolated_user_data.Initialize()) {
        std::fwprintf(stderr, L"failed to isolate health-check user data\n");
        return 1;
    }

    const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init_hr)) {
        std::fwprintf(
            stderr, L"CoInitializeEx failed: 0x%08lX\n",
            static_cast<unsigned long>(init_hr));
        return 1;
    }
    struct CoUninitializer {
        ~CoUninitializer() { CoUninitialize(); }
    } co_uninitializer;

    HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::fwprintf(stderr, L"LoadLibrary failed %lu for %ls\n", GetLastError(), argv[1]);
        return 1;
    }
    const char* exports[] = {
        "DllGetClassObject", "DllCanUnloadNow", "DllRegisterServer", "DllUnregisterServer"};
    for (const char* export_name : exports) {
        if (GetProcAddress(module, export_name) == nullptr) {
            std::fprintf(stderr, "missing export %s\n", export_name);
            return 1;
        }
    }

    auto get_class = reinterpret_cast<GetClassFn>(
        GetProcAddress(module, "DllGetClassObject"));
    IClassFactory* factory = nullptr;
    HRESULT hr = get_class(
        CLSID_ShuruTextService, IID_IClassFactory,
        reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) return 1;
    ITfTextInputProcessorEx* tip = nullptr;
    hr = factory->CreateInstance(
        nullptr, IID_ITfTextInputProcessorEx, reinterpret_cast<void**>(&tip));
    factory->Release();
    if (FAILED(hr) || tip == nullptr) return 1;
    tip->Release();

    shuru::PinyinEngine engine;
    if (!engine.Initialize(argv[2])) {
        std::fwprintf(stderr, L"lexicon initialize failed\n");
        return 1;
    }
    const std::filesystem::path expected_user_dict =
        isolated_user_data.directory() / L"FacaiPinyin" / L"data" /
        L"lexicon" / L"user_dict.txt";
    if (!SamePath(engine.user_dict_path(), expected_user_dict.wstring())) {
        std::fwprintf(stderr, L"health-check user dictionary is not isolated\n");
        return 1;
    }
    const std::filesystem::path custom_phrase_path =
        isolated_user_data.directory() / L"FacaiPinyin" / L"data" /
        L"lexicon" / L"custom_phrases.txt";
    std::filesystem::create_directories(custom_phrase_path.parent_path());
    {
        std::ofstream phrases(custom_phrase_path, std::ios::binary | std::ios::trunc);
        phrases << "sds\t\xE6\xB7\xB1\xE5\xBA\xA6\xE6\x80\x9D\xE8\x80\x83\t1\n";
    }
    if (!engine.ReloadCustomPhrases()) {
        std::fwprintf(stderr, L"custom phrase reload failed\n");
        return 1;
    }
    const auto custom_phrase_result = engine.Query("sds", 9);
    if (custom_phrase_result.candidates.empty() ||
        custom_phrase_result.candidates.front().text != L"深度思考" ||
        custom_phrase_result.candidates.front().learnable ||
        custom_phrase_result.candidates.front().from_user) {
        std::fwprintf(stderr, L"custom phrase query failed\n");
        return 1;
    }

    const auto shuru_result = engine.Query("shuru", 20);
    const bool found = std::any_of(
        shuru_result.candidates.begin(), shuru_result.candidates.end(),
        [](const shuru::Candidate& candidate) {
            return candidate.text.find(L"输入") != std::wstring::npos;
        });
    if (!found) {
        std::fwprintf(stderr, L"real dictionary query failed\n");
        return 1;
    }
    const auto duan = engine.Query("duan", 9);
    if (!ContainsTextInFirst(duan, L"短", 4)) {
        std::fwprintf(stderr, L"duan common-character ranking failed\n");
        return 1;
    }
    const auto duanju = engine.Query("duanju", 9);
    if (duanju.candidates.empty() || duanju.candidates.front().text != L"短剧" ||
        duanju.candidates.front().pinyin != "duanju") {
        std::fwprintf(stderr, L"duanju phrase ranking failed\n");
        return 1;
    }

    if (argc > 3 && wcscmp(argv[3], L"--registered") == 0) {
        wchar_t path[MAX_PATH] {};
        DWORD bytes = sizeof(path);
        const wchar_t* key =
            L"CLSID\\{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}\\InprocServer32";
        if (RegGetValueW(
                HKEY_CLASSES_ROOT, key, nullptr, RRF_RT_REG_SZ, nullptr,
                path, &bytes) != ERROR_SUCCESS ||
            !SamePath(path, argv[1])) {
            std::fwprintf(stderr, L"COM path mismatch: %ls\n", path);
            return 1;
        }

        ITfInputProcessorProfiles* profiles = nullptr;
        hr = CoCreateInstance(
            CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(&profiles));
        if (FAILED(hr) || profiles == nullptr) return 1;
        IEnumTfLanguageProfiles* enumerator = nullptr;
        hr = profiles->EnumLanguageProfiles(SHURU_LANGID, &enumerator);
        bool profile_found = false;
        if (SUCCEEDED(hr) && enumerator != nullptr) {
            TF_LANGUAGEPROFILE profile {};
            ULONG fetched = 0;
            while (enumerator->Next(1, &profile, &fetched) == S_OK) {
                if (IsEqualGUID(profile.clsid, CLSID_ShuruTextService) &&
                    IsEqualGUID(profile.guidProfile, GUID_ShuruProfile)) {
                    profile_found = true;
                    break;
                }
            }
            enumerator->Release();
        }
        profiles->Release();
        if (!profile_found) {
            std::fwprintf(stderr, L"TSF profile missing\n");
            return 1;
        }
    }

    FreeLibrary(module);
    std::wprintf(L"release DLL/COM/dictionary health passed\n");
    return 0;
}
