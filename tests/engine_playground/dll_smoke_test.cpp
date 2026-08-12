#include "common/guid_def.h"

#include <Windows.h>
#include <msctf.h>

#include <cstdio>

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using DllCanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: dll_smoke_test <dll>\n");
        return 2;
    }
    HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    const auto get_class = reinterpret_cast<DllGetClassObjectFn>(
        GetProcAddress(module, "DllGetClassObject"));
    const auto can_unload = reinterpret_cast<DllCanUnloadNowFn>(
        GetProcAddress(module, "DllCanUnloadNow"));
    if (get_class == nullptr || can_unload == nullptr) {
        FreeLibrary(module);
        return 1;
    }

    IClassFactory* factory = nullptr;
    HRESULT hr = get_class(
        CLSID_ShuruTextService,
        IID_IClassFactory,
        reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        FreeLibrary(module);
        return 1;
    }
    ITfTextInputProcessorEx* service = nullptr;
    hr = factory->CreateInstance(
        nullptr,
        IID_ITfTextInputProcessorEx,
        reinterpret_cast<void**>(&service));
    factory->Release();
    if (FAILED(hr) || service == nullptr) {
        FreeLibrary(module);
        return 1;
    }
    service->Release();
    const HRESULT unload_result = can_unload();
    const BOOL freed = FreeLibrary(module);
    if (unload_result != S_OK || !freed) {
        std::fprintf(stderr, "DLL unload failed: hr=0x%08lX win32=%lu\n",
                     static_cast<unsigned long>(unload_result), GetLastError());
        return 1;
    }
    std::printf("DLL factory/lifecycle smoke test passed\n");
    return 0;
}
