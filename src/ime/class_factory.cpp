#include "class_factory.h"
#include <new>

#include "globals.h"
#include "text_service.h"
#include "../common/logger.h"

namespace shuru {

ClassFactory::ClassFactory() {
    DllAddRef();
}

ClassFactory::~ClassFactory() {
    DllRelease();
}

STDMETHODIMP ClassFactory::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
        *ppvObj = static_cast<IClassFactory*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) ClassFactory::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) ClassFactory::Release() {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) {
        delete this;
    }
    return static_cast<ULONG>(v);
}

STDMETHODIMP ClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;

    if (pUnkOuter != nullptr) {
        return CLASS_E_NOAGGREGATION;
    }

    TextService* service = new (std::nothrow) TextService();
    if (service == nullptr) {
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = service->QueryInterface(riid, ppvObj);
    service->Release();
    if (FAILED(hr)) {
        SHURU_LOG_ERROR("TextService QI failed: 0x%08X", hr);
    }
    return hr;
}

STDMETHODIMP ClassFactory::LockServer(BOOL fLock) {
    if (fLock) {
        DllAddRef();
    } else {
        DllRelease();
    }
    return S_OK;
}

} // namespace shuru
