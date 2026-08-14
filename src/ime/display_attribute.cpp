#include "display_attribute.h"

#include "../common/logger.h"

#include <oleauto.h>
#include <new>

namespace shuru {
namespace {

// {B5C31062-789A-4C39-8F6C-0D2F1A3B4C5D} 不需要，使用已有 GUID_ShuruDisplayAttribute

}  // namespace

void DisplayAttributeInfo::InitDefault() {
    // 文本色：系统默认
    attr_.crText.type = TF_CT_NONE;
    // 背景：无
    attr_.crBk.type = TF_CT_NONE;
    // 组合串无下划线
    attr_.lsStyle = TF_LS_NONE;
    attr_.fBoldLine = FALSE;
    attr_.crLine.type = TF_CT_NONE;
    attr_.bAttr = TF_ATTR_INPUT;
}

DisplayAttributeInfo::DisplayAttributeInfo() {
    InitDefault();
}

STDMETHODIMP DisplayAttributeInfo::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) {
        *ppvObj = static_cast<ITfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DisplayAttributeInfo::AddRef() {
    return InterlockedIncrement(&ref_);
}

STDMETHODIMP_(ULONG) DisplayAttributeInfo::Release() {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) {
        delete this;
    }
    return static_cast<ULONG>(v);
}

STDMETHODIMP DisplayAttributeInfo::GetGUID(GUID* pguid) {
    if (!pguid) {
        return E_INVALIDARG;
    }
    *pguid = GUID_ShuruDisplayAttribute;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::GetDescription(BSTR* pbstrDesc) {
    if (!pbstrDesc) {
        return E_INVALIDARG;
    }
    *pbstrDesc = SysAllocString(L"财神输入法组合串");
    return *pbstrDesc ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP DisplayAttributeInfo::GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) {
    if (!pda) {
        return E_INVALIDARG;
    }
    *pda = attr_;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::SetAttributeInfo(const TF_DISPLAYATTRIBUTE* pda) {
    if (!pda) {
        return E_INVALIDARG;
    }
    attr_ = *pda;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::Reset() {
    InitDefault();
    return S_OK;
}

EnumDisplayAttributeInfo::EnumDisplayAttributeInfo() = default;
EnumDisplayAttributeInfo::EnumDisplayAttributeInfo(ULONG index) : index_(index) {}

STDMETHODIMP EnumDisplayAttributeInfo::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo)) {
        *ppvObj = static_cast<IEnumTfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::AddRef() {
    return InterlockedIncrement(&ref_);
}

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::Release() {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) {
        delete this;
    }
    return static_cast<ULONG>(v);
}

STDMETHODIMP EnumDisplayAttributeInfo::Clone(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum) {
        return E_INVALIDARG;
    }
    auto* p = new (std::nothrow) EnumDisplayAttributeInfo(index_);
    if (!p) {
        return E_OUTOFMEMORY;
    }
    *ppEnum = p;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo, ULONG* pcFetched) {
    if (!rgInfo) {
        return E_INVALIDARG;
    }
    ULONG fetched = 0;
    if (ulCount > 0 && index_ == 0) {
        rgInfo[0] = new (std::nothrow) DisplayAttributeInfo();
        if (!rgInfo[0]) {
            return E_OUTOFMEMORY;
        }
        ++index_;
        fetched = 1;
    }
    if (pcFetched) {
        *pcFetched = fetched;
    }
    return (fetched == ulCount) ? S_OK : S_FALSE;
}

STDMETHODIMP EnumDisplayAttributeInfo::Reset() {
    index_ = 0;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Skip(ULONG ulCount) {
    if (ulCount > 0 && index_ == 0) {
        index_ = 1;
        return (ulCount == 1) ? S_OK : S_FALSE;
    }
    return S_FALSE;
}

TfGuidAtom RegisterDisplayAttributeAtom() {
    ITfCategoryMgr* mgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfCategoryMgr, reinterpret_cast<void**>(&mgr));
    if (FAILED(hr) || !mgr) {
        SHURU_LOG_WARN("CategoryMgr for display attr failed: 0x%08X", hr);
        return TF_INVALID_GUIDATOM;
    }
    TfGuidAtom atom = TF_INVALID_GUIDATOM;
    hr = mgr->RegisterGUID(GUID_ShuruDisplayAttribute, &atom);
    mgr->Release();
    if (FAILED(hr)) {
        SHURU_LOG_WARN("RegisterGUID display attr failed: 0x%08X", hr);
        return TF_INVALID_GUIDATOM;
    }
    return atom;
}

bool ApplyCompositionDisplayAttribute(ITfContext* context, TfEditCookie ec, ITfRange* range, TfGuidAtom atom) {
    if (!context || !range || atom == TF_INVALID_GUIDATOM) {
        return false;
    }
    ITfProperty* prop = nullptr;
    HRESULT hr = context->GetProperty(GUID_PROP_ATTRIBUTE, &prop);
    if (FAILED(hr) || !prop) {
        return false;
    }
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_I4;
    var.lVal = static_cast<LONG>(atom);
    hr = prop->SetValue(ec, range, &var);
    prop->Release();
    return SUCCEEDED(hr);
}

bool ClearCompositionDisplayAttribute(ITfContext* context, TfEditCookie ec, ITfRange* range) {
    if (!context || !range) {
        return false;
    }
    ITfProperty* prop = nullptr;
    HRESULT hr = context->GetProperty(GUID_PROP_ATTRIBUTE, &prop);
    if (FAILED(hr) || !prop) {
        return false;
    }
    hr = prop->Clear(ec, range);
    prop->Release();
    return SUCCEEDED(hr);
}

}  // namespace shuru
