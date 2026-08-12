#pragma once

#include <Windows.h>
#include <Unknwn.h>
#include <objbase.h>

namespace shuru {

class ClassFactory : public IClassFactory {
public:
    ClassFactory();
    virtual ~ClassFactory();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IClassFactory
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObj) override;
    STDMETHODIMP LockServer(BOOL fLock) override;

private:
    LONG ref_ = 1;
};

} // namespace shuru
