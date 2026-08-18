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
    selection.style.ase = TF_AE_END;
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

SetCompositionEditSession::SetCompositionEditSession(
    ITfContext* context, TfClientId client_id, ITfCompositionSink* sink,
    ITfComposition** composition, const std::wstring& text, TfGuidAtom display_atom,
    bool move_caret_to_end)
    : context_(context), client_id_(client_id), sink_(sink),
      composition_(composition), text_(text), display_atom_(display_atom),
      move_caret_to_end_(move_caret_to_end) {
    if (context_) {
        context_->AddRef();
    }
    if (sink_) sink_->AddRef();
}

SetCompositionEditSession::~SetCompositionEditSession() {
    SafeRelease(&context_);
    SafeRelease(&sink_);
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
    if (context_ == nullptr || composition_ == nullptr) {
        return E_FAIL;
    }

    if (*composition_ == nullptr) {
        ITfInsertAtSelection* insert = nullptr;
        HRESULT hr = context_->QueryInterface(
            IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert));
        if (FAILED(hr) || insert == nullptr) return FAILED(hr) ? hr : E_NOINTERFACE;
        ITfRange* insertion_range = nullptr;
        hr = insert->InsertTextAtSelection(
            ec, TF_IAS_QUERYONLY, nullptr, 0, &insertion_range);
        insert->Release();
        if (FAILED(hr) || insertion_range == nullptr) {
            SafeRelease(&insertion_range);
            return FAILED(hr) ? hr : E_FAIL;
        }

        ITfContextComposition* context_composition = nullptr;
        hr = context_->QueryInterface(
            IID_ITfContextComposition,
            reinterpret_cast<void**>(&context_composition));
        if (SUCCEEDED(hr) && context_composition != nullptr) {
            hr = context_composition->StartComposition(
                ec, insertion_range, sink_, composition_);
            context_composition->Release();
        }
        insertion_range->Release();
        if (FAILED(hr) || *composition_ == nullptr) {
            return FAILED(hr) ? hr : E_FAIL;
        }
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

    // 只在组合创建时设置一次选择端点。每个字母都调用 SetSelection 会让
    // 部分宿主先按旧布局通知一次，再按新布局通知一次，直接造成闪跳。
    if (move_caret_to_end_) {
        SetCaretToRangeEnd(context_, ec, range);
    }

    range->Release();
    return S_OK;
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
    const bool is_composition = composition_ != nullptr;
    if (composition_) {
        composition_->GetRange(&range);
        // 组合范围的末端就是当前输入光标。下面优先测量末字符的字形
        // 范围，再取其右边缘；部分宿主在重排窗口内对“折叠后的末端”
        // 会暂时返回组合起点，直接使用该折叠矩形就会造成候选窗闪回。
        caret_anchor = TF_ANCHOR_END;
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

    if (is_composition) {
        ITfRange* tail_range = nullptr;
        if (SUCCEEDED(range->Clone(&tail_range)) && tail_range != nullptr) {
            if (SUCCEEDED(tail_range->Collapse(ec, TF_ANCHOR_END))) {
                LONG shifted = 0;
                if (SUCCEEDED(tail_range->ShiftStart(
                        ec, -1, &shifted, nullptr)) && shifted == -1) {
                    RECT tail_rect {};
                    BOOL tail_clipped = FALSE;
                    const HRESULT tail_hr = view->GetTextExt(
                        ec, tail_range, &tail_rect, &tail_clipped);
                    if (SUCCEEDED(tail_hr) && !tail_clipped &&
                        tail_rect.right > tail_rect.left &&
                        tail_rect.bottom > tail_rect.top) {
                        tail_range->Release();
                        range->Release();
                        view->Release();
                        tail_rect.left = tail_rect.right;
                        *out_rect_ = tail_rect;
                        if (out_ok_) *out_ok_ = true;
                        return S_OK;
                    }
                }
            }
            tail_range->Release();
        }
    }

    // 其次测量完整组合范围。它比折叠端点更能反映已经完成的排版，
    // 同时作为跨行或末字符暂不可测量时的兼容回退。
    RECT full_rect {};
    BOOL full_clipped = FALSE;
    HRESULT full_hr = view->GetTextExt(ec, range, &full_rect, &full_clipped);
    if (is_composition && SUCCEEDED(full_hr) && !full_clipped &&
        full_rect.right > full_rect.left &&
        full_rect.bottom > full_rect.top) {
        range->Release();
        view->Release();
        full_rect.left = full_rect.right;
        *out_rect_ = full_rect;
        if (out_ok_) *out_ok_ = true;
        return S_OK;
    }

    // 组合末端暂时没有可用字形范围时宁可等待下一次布局通知，也不能
    // 回退到折叠点。某些宿主此时会把折叠点错误地报告为组合起点。
    if (is_composition) {
        range->Release();
        view->Release();
        return E_FAIL;
    }

    // 非组合选区仍优先测量折叠后的实际插入点。
    ITfRange* caret_range = nullptr;
    if (SUCCEEDED(range->Clone(&caret_range)) && caret_range != nullptr) {
        if (SUCCEEDED(caret_range->Collapse(ec, caret_anchor))) {
            RECT rc {};
            BOOL clipped = FALSE;
            HRESULT hr = view->GetTextExt(ec, caret_range, &rc, &clipped);
            if (SUCCEEDED(hr) && !clipped && rc.bottom > rc.top && rc.right >= rc.left) {
                caret_range->Release();
                range->Release();
                view->Release();
                *out_rect_ = rc;
                if (out_ok_) *out_ok_ = true;
                return S_OK;
            }
        }
        caret_range->Release();
    }

    // 若折叠点测量未就绪（如单字符刚插入时），尝试测量整段组合串
    RECT rc {};
    BOOL clipped = FALSE;
    HRESULT hr = view->GetTextExt(ec, range, &rc, &clipped);
    range->Release();
    view->Release();
    if (SUCCEEDED(hr) && !clipped && rc.bottom > rc.top && rc.right >= rc.left) {
        RECT caret_rc = rc;
        if (caret_anchor == TF_ANCHOR_END) {
            caret_rc.left = rc.right;
        } else {
            caret_rc.right = rc.left;
        }
        *out_rect_ = caret_rc;
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
