#include "edit_sessions.h"
#include "display_attribute.h"

#include "../common/com_utils.h"
#include "../common/guid_def.h"
#include "../common/logger.h"

#include <new>
#include <InputScope.h>

namespace shuru {
namespace {

void SetCaretToRangeEnd(ITfContext* context, TfEditCookie ec, ITfRange* range) {
    if (context == nullptr || range == nullptr) {
        return;
    }
    ITfRange* clone = nullptr;
    if (FAILED(range->Clone(&clone)) || clone == nullptr) {
        return;
    }
    clone->Collapse(ec, TF_ANCHOR_END);
    TF_SELECTION selection {};
    selection.range = clone;
    selection.style.ase = TF_AE_NONE;
    selection.style.fInterimChar = FALSE;
    context->SetSelection(ec, 1, &selection);
    clone->Release();
}

}  // namespace

// -------- InsertTextEditSession --------

InsertTextEditSession::InsertTextEditSession(ITfContext* context, TfClientId client_id, ITfComposition** composition, const std::wstring& text)
    : context_(context), client_id_(client_id), composition_(composition), text_(text) {
    if (context_) {
        context_->AddRef();
    }
}

InsertTextEditSession::~InsertTextEditSession() {
    SafeRelease(&context_);
}

STDMETHODIMP InsertTextEditSession::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
        *ppvObj = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) InsertTextEditSession::AddRef() {
    return InterlockedIncrement(&ref_);
}

STDMETHODIMP_(ULONG) InsertTextEditSession::Release() {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) {
        delete this;
    }
    return static_cast<ULONG>(v);
}

STDMETHODIMP InsertTextEditSession::DoEditSession(TfEditCookie ec) {
    if (context_ == nullptr) {
        return E_FAIL;
    }

    // 核心：若有组合串，必须“原位替换”为最终文本，再结束组合。
    // 若先 EndComposition 再插入，拼音会残留在文档中，形成拉丁残片。
    if (composition_ && *composition_) {
        ITfRange* range = nullptr;
        HRESULT hr = (*composition_)->GetRange(&range);
        if (SUCCEEDED(hr) && range != nullptr) {
            hr = range->SetText(ec, 0, text_.c_str(), static_cast<LONG>(text_.size()));
            if (SUCCEEDED(hr)) {
                SetCaretToRangeEnd(context_, ec, range);
                range->Release();

                const HRESULT end_hr = (*composition_)->EndComposition(ec);
                (*composition_)->Release();
                *composition_ = nullptr;
                if (FAILED(end_hr)) {
                    SHURU_LOG_ERROR("EndComposition after insert failed: 0x%08X", end_hr);
                    return end_hr;
                }
                SHURU_LOG_INFO("InsertText via composition replace, chars=%u", static_cast<unsigned>(text_.size()));
                return S_OK;
            }
            range->Release();
            SHURU_LOG_WARN("composition SetText failed: 0x%08X", hr);
            // 组合范围写入失败时不能结束组合再从选区插入，否则会留下拼音残片或重复文本。
            return hr;
        } else {
            SHURU_LOG_ERROR("composition GetRange failed: 0x%08X", hr);
            return FAILED(hr) ? hr : E_FAIL;
        }
    }

    ITfInsertAtSelection* insert = nullptr;
    HRESULT hr = context_->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert));
    if (FAILED(hr) || insert == nullptr) {
        SHURU_LOG_ERROR("InsertText QI ITfInsertAtSelection failed: 0x%08X", hr);
        return hr;
    }

    ITfRange* range = nullptr;
    hr = insert->InsertTextAtSelection(ec, 0, text_.c_str(), static_cast<LONG>(text_.size()), &range);
    insert->Release();
    if (FAILED(hr)) {
        SHURU_LOG_ERROR("InsertTextAtSelection failed: 0x%08X len=%u", hr, static_cast<unsigned>(text_.size()));
        return hr;
    }

    if (range != nullptr) {
        SetCaretToRangeEnd(context_, ec, range);
        range->Release();
    }

    SHURU_LOG_INFO("InsertText via selection, chars=%u", static_cast<unsigned>(text_.size()));
    return S_OK;
}

// -------- SetCompositionEditSession --------

SetCompositionEditSession::SetCompositionEditSession(ITfContext* context, TfClientId client_id, ITfComposition** composition, const std::wstring& text, TfGuidAtom display_atom)
    : context_(context), client_id_(client_id), composition_(composition), text_(text), display_atom_(display_atom) {
    if (context_) {
        context_->AddRef();
    }
}

SetCompositionEditSession::~SetCompositionEditSession() {
    SafeRelease(&context_);
}

STDMETHODIMP SetCompositionEditSession::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
        *ppvObj = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) SetCompositionEditSession::AddRef() {
    return InterlockedIncrement(&ref_);
}

STDMETHODIMP_(ULONG) SetCompositionEditSession::Release() {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) {
        delete this;
    }
    return static_cast<ULONG>(v);
}

STDMETHODIMP SetCompositionEditSession::DoEditSession(TfEditCookie ec) {
    if (context_ == nullptr || composition_ == nullptr || *composition_ == nullptr) {
        return E_FAIL;
    }

    ITfRange* range = nullptr;
    HRESULT hr = (*composition_)->GetRange(&range);
    if (FAILED(hr) || range == nullptr) {
        SHURU_LOG_ERROR("SetComposition GetRange failed: 0x%08X", hr);
        return hr;
    }

    hr = range->SetText(ec, 0, text_.c_str(), static_cast<LONG>(text_.size()));
    if (FAILED(hr)) {
        range->Release();
        SHURU_LOG_ERROR("SetComposition SetText failed: 0x%08X", hr);
        return hr;
    }

    ApplyCompositionDisplayAttribute(context_, ec, range, display_atom_);

    // 选中整段组合串，并标记 interim，多数应用会显示为“正在组合”样式
    ITfRange* sel_range = nullptr;
    if (SUCCEEDED(range->Clone(&sel_range)) && sel_range != nullptr) {
        TF_SELECTION selection {};
        selection.range = sel_range;
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = TRUE;
        context_->SetSelection(ec, 1, &selection);
        sel_range->Release();
    }

    range->Release();
    return S_OK;
}

// -------- StartCompositionEditSession --------

StartCompositionEditSession::StartCompositionEditSession(ITfContext* context, TfClientId client_id, ITfCompositionSink* sink, ITfComposition** composition)
    : context_(context), client_id_(client_id), sink_(sink), composition_(composition) {
    if (context_) {
        context_->AddRef();
    }
    if (sink_) {
        sink_->AddRef();
    }
}

StartCompositionEditSession::~StartCompositionEditSession() {
    SafeRelease(&context_);
    SafeRelease(&sink_);
}

STDMETHODIMP StartCompositionEditSession::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
        *ppvObj = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) StartCompositionEditSession::AddRef() {
    return InterlockedIncrement(&ref_);
}

STDMETHODIMP_(ULONG) StartCompositionEditSession::Release() {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) {
        delete this;
    }
    return static_cast<ULONG>(v);
}

STDMETHODIMP StartCompositionEditSession::DoEditSession(TfEditCookie ec) {
    if (context_ == nullptr || composition_ == nullptr) {
        return E_FAIL;
    }
    if (*composition_ != nullptr) {
        return S_OK;
    }

    ITfInsertAtSelection* insert = nullptr;
    HRESULT hr = context_->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert));
    if (FAILED(hr)) {
        SHURU_LOG_ERROR("StartComp QI insert failed: 0x%08X", hr);
        return hr;
    }

    ITfRange* range = nullptr;
    hr = insert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range);
    insert->Release();
    if (FAILED(hr) || range == nullptr) {
        SHURU_LOG_ERROR("StartComp QUERYONLY failed: 0x%08X", hr);
        return hr;
    }

    ITfContextComposition* context_composition = nullptr;
    hr = context_->QueryInterface(IID_ITfContextComposition, reinterpret_cast<void**>(&context_composition));
    if (FAILED(hr)) {
        range->Release();
        SHURU_LOG_ERROR("StartComp QI contextcomp failed: 0x%08X", hr);
        return hr;
    }

    hr = context_composition->StartComposition(ec, range, sink_, composition_);
    context_composition->Release();
    range->Release();
    if (FAILED(hr)) {
        SHURU_LOG_ERROR("StartComposition failed: 0x%08X", hr);
    }
    return hr;
}

// -------- EndCompositionEditSession --------

EndCompositionEditSession::EndCompositionEditSession(ITfComposition** composition)
    : composition_(composition) {}

EndCompositionEditSession::~EndCompositionEditSession() = default;

STDMETHODIMP EndCompositionEditSession::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
        *ppvObj = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EndCompositionEditSession::AddRef() {
    return InterlockedIncrement(&ref_);
}

STDMETHODIMP_(ULONG) EndCompositionEditSession::Release() {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) {
        delete this;
    }
    return static_cast<ULONG>(v);
}

STDMETHODIMP EndCompositionEditSession::DoEditSession(TfEditCookie ec) {
    if (composition_ == nullptr || *composition_ == nullptr) {
        return S_OK;
    }

    // 取消组合时清空组合串文本，避免拼音残留
    ITfRange* range = nullptr;
    HRESULT clear_hr = (*composition_)->GetRange(&range);
    if (FAILED(clear_hr) || range == nullptr) {
        SHURU_LOG_ERROR("Cancel composition GetRange failed: 0x%08X", clear_hr);
        return FAILED(clear_hr) ? clear_hr : E_FAIL;
    }
    clear_hr = range->SetText(ec, 0, L"", 0);
    range->Release();
    if (FAILED(clear_hr)) {
        SHURU_LOG_ERROR("Cancel composition SetText failed: 0x%08X", clear_hr);
        return clear_hr;
    }

    const HRESULT end_hr = (*composition_)->EndComposition(ec);
    if (FAILED(end_hr)) {
        SHURU_LOG_ERROR("Cancel composition failed: 0x%08X", end_hr);
        return end_hr;
    }
    (*composition_)->Release();
    *composition_ = nullptr;
    return S_OK;
}

// -------- GetTextExtEditSession --------

GetTextExtEditSession::GetTextExtEditSession(ITfContext* context, ITfComposition* composition, RECT* out_rect, bool* out_ok)
    : context_(context), composition_(composition), out_rect_(out_rect), out_ok_(out_ok) {
    if (context_) context_->AddRef();
    if (composition_) composition_->AddRef();
}

GetTextExtEditSession::~GetTextExtEditSession() {
    SafeRelease(&context_);
    SafeRelease(&composition_);
}

STDMETHODIMP GetTextExtEditSession::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_INVALIDARG;
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
        *ppvObj = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) GetTextExtEditSession::AddRef() { return InterlockedIncrement(&ref_); }
STDMETHODIMP_(ULONG) GetTextExtEditSession::Release() {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) delete this;
    return static_cast<ULONG>(v);
}

STDMETHODIMP GetTextExtEditSession::DoEditSession(TfEditCookie ec) {
    if (out_ok_) *out_ok_ = false;
    if (!context_ || !out_rect_) return E_FAIL;

    ITfContextView* view = nullptr;
    if (FAILED(context_->GetActiveView(&view)) || !view) {
        return E_FAIL;
    }

    ITfRange* range = nullptr;
    TfAnchor caret_anchor = TF_ANCHOR_END;
    if (composition_) {
        composition_->GetRange(&range);
    }
    if (!range) {
        TF_SELECTION sel {};
        ULONG fetched = 0;
        if (SUCCEEDED(context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) && fetched == 1 && sel.range) {
            range = sel.range;
            caret_anchor = sel.style.ase == TF_AE_START ? TF_ANCHOR_START : TF_ANCHOR_END;
        }
    }
    if (!range) {
        view->Release();
        return E_FAIL;
    }

    // 整段组合串的 GetTextExt 左边界是拼音起点；候选窗应跟随当前插入端，
    // 因此先把范围折叠到实际光标锚点。
    const HRESULT collapse_hr = range->Collapse(ec, caret_anchor);
    if (FAILED(collapse_hr)) {
        range->Release();
        view->Release();
        return collapse_hr;
    }

    RECT rc {};
    BOOL clipped = FALSE;
    HRESULT hr = view->GetTextExt(ec, range, &rc, &clipped);
    range->Release();
    view->Release();
    if (SUCCEEDED(hr) && !clipped && rc.bottom > rc.top && rc.right >= rc.left) {
        *out_rect_ = rc;
        if (out_ok_) *out_ok_ = true;
        return S_OK;
    }
    return E_FAIL;
}

// -------- GetInputScopeEditSession --------

GetInputScopeEditSession::GetInputScopeEditSession(
    ITfContext* context,
    InputScopePrivacy* out_privacy)
    : context_(context), out_privacy_(out_privacy) {
    if (context_) {
        context_->AddRef();
    }
}

GetInputScopeEditSession::~GetInputScopeEditSession() {
    SafeRelease(&context_);
}

STDMETHODIMP GetInputScopeEditSession::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
        *ppvObj = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) GetInputScopeEditSession::AddRef() {
    return InterlockedIncrement(&ref_);
}

STDMETHODIMP_(ULONG) GetInputScopeEditSession::Release() {
    const LONG value = InterlockedDecrement(&ref_);
    if (value == 0) {
        delete this;
    }
    return static_cast<ULONG>(value);
}

STDMETHODIMP GetInputScopeEditSession::DoEditSession(TfEditCookie ec) {
    if (out_privacy_) {
        *out_privacy_ = InputScopePrivacy::Unknown;
    }
    if (context_ == nullptr || out_privacy_ == nullptr) {
        return E_INVALIDARG;
    }

    TF_SELECTION selection {};
    ULONG fetched = 0;
    HRESULT hr = context_->GetSelection(
        ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(hr) || fetched != 1 || selection.range == nullptr) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    ITfProperty* property = nullptr;
    hr = context_->GetProperty(GUID_ShuruInputScopeProperty, &property);
    if (FAILED(hr) || property == nullptr) {
        selection.range->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }

    VARIANT value;
    VariantInit(&value);
    hr = property->GetValue(ec, selection.range, &value);
    property->Release();
    selection.range->Release();
    if (FAILED(hr)) {
        VariantClear(&value);
        return hr;
    }
    if (value.vt != VT_UNKNOWN || value.punkVal == nullptr) {
        VariantClear(&value);
        return S_FALSE;
    }

    ITfInputScope* input_scope = nullptr;
    hr = value.punkVal->QueryInterface(
        IID_ITfInputScope, reinterpret_cast<void**>(&input_scope));
    VariantClear(&value);
    if (FAILED(hr) || input_scope == nullptr) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }

    InputScope* scopes = nullptr;
    UINT count = 0;
    hr = input_scope->GetInputScopes(&scopes, &count);
    input_scope->Release();
    if (FAILED(hr)) {
        CoTaskMemFree(scopes);
        return hr;
    }

    *out_privacy_ = ClassifyInputScopes(scopes, count);
    CoTaskMemFree(scopes);
    return S_OK;
}

} // namespace shuru
