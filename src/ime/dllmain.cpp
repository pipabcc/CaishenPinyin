#include <Windows.h>
#include "class_factory.h"

#include "globals.h"
#include "register.h"
#include "../engine/shared_engine.h"
#include "ui/shared_status_ui.h"
#include "../common/guid_def.h"
#include "../common/logger.h"

#include <new>

using namespace shuru;

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID /*lpReserved*/) {
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        g_module = hInstance;
        DisableThreadLibraryCalls(hInstance);
        InitializeCriticalSection(&g_cs);
        break;
    case DLL_PROCESS_DETACH:
        // 不在 Loader Lock 中等待后台线程或销毁窗口；正常卸载前由引用归零路径完成回收。
        DeleteCriticalSection(&g_cs);
        break;
    default:
        break;
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    if (g_dll_ref.load() != 0 || SharedEngine::IsLoading() ||
        SharedEngine::RefCount() != 0 || SharedStatusUi::RefCount() != 0) {
        return S_FALSE;
    }
    SharedEngine::Shutdown();
    if (g_dll_ref.load() != 0 || SharedEngine::IsLoading() ||
        SharedEngine::RefCount() != 0 || SharedStatusUi::RefCount() != 0) {
        return S_FALSE;
    }
    ShutdownLogger();
    return g_dll_ref.load() == 0 && !SharedEngine::IsLoading() &&
                   SharedEngine::RefCount() == 0 && SharedStatusUi::RefCount() == 0
               ? S_OK
               : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;

    if (!IsEqualCLSID(rclsid, CLSID_ShuruTextService)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    ClassFactory* factory = new (std::nothrow) ClassFactory();
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = factory->QueryInterface(riid, ppvObj);
    factory->Release();
    return hr;
}

STDAPI DllRegisterServer() {
    HRESULT hr = RegisterServer();
    if (FAILED(hr)) {
        return hr;
    }
    hr = RegisterProfile();
    if (FAILED(hr)) {
        UnregisterServer();
        return hr;
    }
    return S_OK;
}

STDAPI DllUnregisterServer() {
    UnregisterProfile();
    return UnregisterServer();
}
