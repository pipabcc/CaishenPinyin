#include "globals.h"

#include "../common/com_utils.h"
#include "../common/logger.h"

namespace shuru {

HINSTANCE g_module = nullptr;
std::atomic<long> g_dll_ref{0};
CRITICAL_SECTION g_cs;

void DllAddRef() {
    g_dll_ref.fetch_add(1);
}

void DllRelease() {
    g_dll_ref.fetch_sub(1);
}

std::wstring GetLexiconDirectory() {
    // Prefer the independently deployed, versioned system package.  The small
    // current file is replaced atomically by the installer, so readers see
    // either the old complete package or the new complete package.
    wchar_t program_data[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"ProgramData", program_data, static_cast<DWORD>(_countof(program_data)));
    if (length > 0 && length < _countof(program_data)) {
        const std::wstring root =
            std::wstring(program_data) + L"\\FacaiPinyin\\data\\lexicon";
        const std::wstring pointer_path = root + L"\\current";
        HANDLE file = CreateFileW(pointer_path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char version[128] = {};
            DWORD read = 0;
            if (ReadFile(file, version, sizeof(version) - 1, &read, nullptr) && read > 0) {
                CloseHandle(file);
                std::wstring value;
                for (DWORD i = 0; i < read; ++i) {
                    const unsigned char ch = static_cast<unsigned char>(version[i]);
                    if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
                    if (!(ch >= '0' && ch <= '9') && !(ch >= 'A' && ch <= 'Z') &&
                        !(ch >= 'a' && ch <= 'z') && ch != '.' && ch != '_' && ch != '-') {
                        value.clear();
                        break;
                    }
                    value.push_back(static_cast<wchar_t>(ch));
                }
                if (!value.empty()) {
                    const std::wstring deployed = root + L"\\versions\\" + value;
                    const DWORD attr = GetFileAttributesW((deployed + L"\\base_dict.txt").c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                        SHURU_LOG_INFO("using deployed lexicon package");
                        return deployed;
                    }
                }
            } else {
                CloseHandle(file);
            }
        }
    }

    // Compatibility and smooth migration path for pre-package installations.
    const std::wstring dir = GetModuleDirectory(g_module);
    if (dir.empty()) {
        return L".\\data\\lexicon";
    }
    SHURU_LOG_WARN("deployed lexicon unavailable; using DLL-adjacent legacy package");
    return dir + L"\\data\\lexicon";
}

} // namespace shuru
