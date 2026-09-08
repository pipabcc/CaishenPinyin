#pragma once

#include <Windows.h>
#include <msctf.h>
#include <wrl/client.h>

namespace shuru::test {

// 按键由测试直接派发；真实文档、编辑会话与通知仍由 Windows TSF 管理。
class TestThreadManager final : public ITfThreadMgr, public ITfKeystrokeMgr {
public:
    explicit TestThreadManager(ITfThreadMgr* manager) : manager_(manager) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfThreadMgr)
            *object = static_cast<ITfThreadMgr*>(this);
        else if (iid == IID_ITfKeystrokeMgr)
            *object = static_cast<ITfKeystrokeMgr*>(this);
        else return manager_->QueryInterface(iid, object);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refs_);
        if (count == 0) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE Activate(TfClientId* id) override { return manager_->Activate(id); }
    HRESULT STDMETHODCALLTYPE Deactivate() override { return manager_->Deactivate(); }
    HRESULT STDMETHODCALLTYPE CreateDocumentMgr(ITfDocumentMgr** value) override {
        return manager_->CreateDocumentMgr(value);
    }
    HRESULT STDMETHODCALLTYPE EnumDocumentMgrs(IEnumTfDocumentMgrs** value) override {
        return manager_->EnumDocumentMgrs(value);
    }
    HRESULT STDMETHODCALLTYPE GetFocus(ITfDocumentMgr** value) override { return manager_->GetFocus(value); }
    HRESULT STDMETHODCALLTYPE SetFocus(ITfDocumentMgr* value) override { return manager_->SetFocus(value); }
    HRESULT STDMETHODCALLTYPE AssociateFocus(HWND window, ITfDocumentMgr* value, ITfDocumentMgr** previous) override {
        return manager_->AssociateFocus(window, value, previous);
    }
    HRESULT STDMETHODCALLTYPE IsThreadFocus(BOOL* value) override { return manager_->IsThreadFocus(value); }
    HRESULT STDMETHODCALLTYPE GetFunctionProvider(REFCLSID id, ITfFunctionProvider** value) override {
        return manager_->GetFunctionProvider(id, value);
    }
    HRESULT STDMETHODCALLTYPE EnumFunctionProviders(IEnumTfFunctionProviders** value) override {
        return manager_->EnumFunctionProviders(value);
    }
    HRESULT STDMETHODCALLTYPE GetGlobalCompartment(ITfCompartmentMgr** value) override {
        return manager_->GetGlobalCompartment(value);
    }
    HRESULT STDMETHODCALLTYPE AdviseKeyEventSink(TfClientId id, ITfKeyEventSink* sink, BOOL) override {
        return id != TF_CLIENTID_NULL && sink != nullptr ? S_OK : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE UnadviseKeyEventSink(TfClientId) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetForeground(CLSID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TestKeyDown(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TestKeyUp(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE KeyDown(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE KeyUp(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPreservedKey(ITfContext*, const TF_PRESERVEDKEY*, GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsPreservedKey(REFGUID, const TF_PRESERVEDKEY*, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE PreserveKey(TfClientId, REFGUID, const TF_PRESERVEDKEY*, const WCHAR*, ULONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnpreserveKey(REFGUID, const TF_PRESERVEDKEY*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPreservedKeyDescription(REFGUID, const WCHAR*, ULONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPreservedKeyDescription(REFGUID, BSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SimulatePreservedKey(ITfContext*, REFGUID, BOOL*) override { return E_NOTIMPL; }
private:
    LONG refs_ = 1;
    Microsoft::WRL::ComPtr<ITfThreadMgr> manager_;
};

}  // namespace shuru::test
