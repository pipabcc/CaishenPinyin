#include <Windows.h>
#include <msctf.h>
#include "common/guid_def.h"

#include <cstdio>

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using DllCanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc != 2) {
        std::fprintf(stderr, "usage: dll_smoke_test <dll>\n");
        return 2;
    }
    __try {
        std::printf("Starting LoadLibraryW: %ls\n", argv[1]);
        HMODULE module = LoadLibraryW(argv[1]);
        if (module == nullptr) {
            std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
            return 1;
        }
        std::printf("Loaded library handle: %p\n", module);
        const auto get_class = reinterpret_cast<DllGetClassObjectFn>(
            GetProcAddress(module, "DllGetClassObject"));
        const auto can_unload = reinterpret_cast<DllCanUnloadNowFn>(
            GetProcAddress(module, "DllCanUnloadNow"));
        if (get_class == nullptr || can_unload == nullptr) {
            FreeLibrary(module);
            return 1;
        }

        std::printf("Getting class factory\n");
        IClassFactory* factory = nullptr;
        HRESULT hr = get_class(
            CLSID_ShuruTextService,
            IID_IClassFactory,
            reinterpret_cast<void**>(&factory));
        std::printf("get_class hr=0x%08X, factory=%p\n", hr, factory);
        if (FAILED(hr) || factory == nullptr) {
            FreeLibrary(module);
            return 1;
        }
        ITfTextInputProcessorEx* service = nullptr;
        hr = factory->CreateInstance(
            nullptr,
            IID_ITfTextInputProcessorEx,
            reinterpret_cast<void**>(&service));
        std::printf("CreateInstance hr=0x%08X, service=%p\n", hr, service);
        factory->Release();
        if (FAILED(hr) || service == nullptr) {
            FreeLibrary(module);
            return 1;
        }
        void** vtbl = *reinterpret_cast<void***>(service);
        std::printf("service vtable: QI=%p, AddRef=%p, Release=%p\n", vtbl[0], vtbl[1], vtbl[2]);
        std::printf("About to release service...\n");
        service->Release();
        std::printf("Released service successfully.\n");
        const HRESULT unload_result = can_unload();
        const BOOL freed = FreeLibrary(module);
        std::printf("can_unload=0x%08X, freed=%d\n", unload_result, freed);
        if (unload_result != S_OK || !freed) {
            std::fprintf(stderr, "DLL unload failed: hr=0x%08lX win32=%lu\n",
                         static_cast<unsigned long>(unload_result), GetLastError());
            return 1;
        }
        std::printf("DLL factory/lifecycle smoke test passed\n");
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("SEH Exception caught: 0x%08X\n", GetExceptionCode());
        return 1;
    }
}
