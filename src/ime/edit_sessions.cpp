#include "edit_sessions.h"
#include "display_attribute.h"
#include "first_key_recovery.h"

#include "../common/com_utils.h"
#include "../common/guid_def.h"
#include "../common/logger.h"

#include <array>
#include <new>
#include <InputScope.h>
#include <utility>

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

HRESULT ReadContextInputScopePrivacy(
    ITfContext* context,
    TfEditCookie edit_cookie,
    InputScopePrivacy* privacy) {
    if (privacy != nullptr) {
        *privacy = InputScopePrivacy::Unknown;
    }
    if (context == nullptr || privacy == nullptr) {
        return E_INVALIDARG;
    }

    TF_SELECTION selection {};
    ULONG fetched = 0;
    HRESULT hr = context->GetSelection(
        edit_cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(hr) || fetched != 1 || selection.range == nullptr) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    ITfProperty* property = nullptr;
    hr = context->GetProperty(GUID_ShuruInputScopeProperty, &property);
    if (FAILED(hr) || property == nullptr) {
        selection.range->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }

    VARIANT value;
    VariantInit(&value);
    hr = property->GetValue(edit_cookie, selection.range, &value);
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

    *privacy = ClassifyInputScopes(scopes, count);
    CoTaskMemFree(scopes);
    return S_OK;
}

// -------- InsertTextEditSession --------

InsertTextEditSession::InsertTextEditSession(
    ITfContext* context,
    TfClientId client_id,
    ITfComposition** composition,
    const std::wstring& text,
    ITfRange* recovered_composition_start,
    const std::wstring& expected_composition_text)
    : context_(context), client_id_(client_id), composition_(composition),
      text_(text), recovered_composition_start_(recovered_composition_start),
      expected_composition_text_(expected_composition_text) {
    if (context_) {
        context_->AddRef();
    }
    if (recovered_composition_start_) {
        recovered_composition_start_->AddRef();
    }
}

InsertTextEditSession::~InsertTextEditSession() {
    SafeRelease(&recovered_composition_start_);
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
            if (recovered_composition_start_ != nullptr) {
                if (expected_composition_text_.empty()) {
                    range->Release();
                    return E_UNEXPECTED;
                }

                // TSF 禁止活动组合修改其范围之外的文本。WinUI 若在输入期间
                // 把组合起点漂到第二个字母，必须在当前提交写会话内先恢复
                // composition 边界，再立即完成校验和替换，避免新的消息轮次
                // 让宿主再次移动起点。
                ITfRange* composition_start = nullptr;
                hr = recovered_composition_start_->Clone(&composition_start);
                if (SUCCEEDED(hr) && composition_start != nullptr) {
                    hr = composition_start->Collapse(ec, TF_ANCHOR_START);
                }
                if (SUCCEEDED(hr)) {
                    hr = (*composition_)->ShiftStart(ec, composition_start);
                }
                SafeRelease(&composition_start);
                if (FAILED(hr)) {
                    range->Release();
                    return hr;
                }
                range->Release();
                range = nullptr;
                hr = (*composition_)->GetRange(&range);
                if (FAILED(hr) || range == nullptr) {
                    SafeRelease(&range);
                    return FAILED(hr) ? hr : E_FAIL;
                }

                ITfRange* recovered_range = nullptr;
                hr = recovered_composition_start_->Clone(&recovered_range);
                if (SUCCEEDED(hr) && recovered_range != nullptr) {
                    hr = recovered_range->Collapse(ec, TF_ANCHOR_START);
                }
                LONG start_before_end = 1;
                if (SUCCEEDED(hr)) {
                    hr = recovered_range->CompareStart(
                        ec, range, TF_ANCHOR_END, &start_before_end);
                    if (SUCCEEDED(hr) && start_before_end > 0) {
                        hr = TF_E_INVALIDPOS;
                    }
                }
                if (SUCCEEDED(hr)) {
                    hr = recovered_range->ShiftEndToRange(
                        ec, range, TF_ANCHOR_END);
                }

                bool exact_text = false;
                if (SUCCEEDED(hr)) {
                    ITfRange* read_range = nullptr;
                    hr = recovered_range->Clone(&read_range);
                    if (SUCCEEDED(hr) && read_range != nullptr) {
                        std::wstring actual(
                            expected_composition_text_.size() + 1, L'\0');
                        ULONG actual_length = 0;
                        hr = read_range->GetText(
                            ec, TF_TF_MOVESTART, actual.data(),
                            static_cast<ULONG>(actual.size()), &actual_length);
                        BOOL exhausted = FALSE;
                        if (SUCCEEDED(hr)) {
                            hr = read_range->IsEmpty(ec, &exhausted);
                        }
                        if (SUCCEEDED(hr)) {
                            actual.resize(actual_length);
                            exact_text = exhausted &&
                                actual == expected_composition_text_;
                        }
                        read_range->Release();
                    }
                }
                if (FAILED(hr) || !exact_text) {
                    SHURU_LOG_WARN(
                        "recovered composition range rejected: hr=0x%08X exact=%d",
                        hr, exact_text ? 1 : 0);
                    SafeRelease(&recovered_range);
                    range->Release();
                    return FAILED(hr) ? hr : E_FAIL;
                }

                range->Release();
                range = recovered_range;
            }

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

    // 每次写入或更新组合文本后，均将光标选区折叠到组合末端，确保宿主（记事本、RichEdit等）
    // 光标紧随拼音末尾，避免光标停留在最左端。
    SetCaretToRangeEnd(context_, ec, range);

    range->Release();
    return S_OK;
}

// -------- AdoptExistingTextEditSession --------

AdoptExistingTextEditSession::AdoptExistingTextEditSession(
    ITfContext* context,
    ITfCompositionSink* sink,
    ITfRange* range,
    wchar_t expected_character,
    ITfComposition** composition,
    ITfRange** recovered_composition_start,
    TfGuidAtom display_atom,
    Preflight preflight,
    Completion completion)
    : context_(context), sink_(sink), range_(range),
      expected_character_(expected_character), composition_(composition),
      recovered_composition_start_(recovered_composition_start),
      display_atom_(display_atom), preflight_(std::move(preflight)),
      completion_(std::move(completion)) {
    if (context_ != nullptr) context_->AddRef();
    if (sink_ != nullptr) sink_->AddRef();
    if (range_ != nullptr) range_->AddRef();
}

AdoptExistingTextEditSession::~AdoptExistingTextEditSession() {
    SafeRelease(&range_);
    SafeRelease(&sink_);
    SafeRelease(&context_);
}

STDMETHODIMP AdoptExistingTextEditSession::QueryInterface(
    REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) return E_INVALIDARG;
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfEditSession)) {
        *ppvObj = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) AdoptExistingTextEditSession::AddRef() {
    return InterlockedIncrement(&ref_);
}

STDMETHODIMP_(ULONG) AdoptExistingTextEditSession::Release() {
    const LONG value = InterlockedDecrement(&ref_);
    if (value == 0) delete this;
    return static_cast<ULONG>(value);
}

HRESULT AdoptExistingTextEditSession::Finish(
    ExistingTextCompositionResult result,
    HRESULT hr,
    std::wstring adopted_text) {
    if (!completed_) {
        completed_ = true;
        if (completion_) completion_(result, std::move(adopted_text));
    }
    return hr;
}

STDMETHODIMP AdoptExistingTextEditSession::DoEditSession(TfEditCookie ec) {
    if (context_ == nullptr || sink_ == nullptr || range_ == nullptr ||
        composition_ == nullptr || recovered_composition_start_ == nullptr) {
        return Finish(ExistingTextCompositionResult::Failed, E_INVALIDARG);
    }
    if (*recovered_composition_start_ != nullptr) {
        return Finish(
            ExistingTextCompositionResult::CompositionActive, E_UNEXPECTED);
    }
    if (preflight_ && !preflight_()) {
        return Finish(ExistingTextCompositionResult::StaleRequest, S_FALSE);
    }
    if (*composition_ != nullptr) {
        return Finish(
            ExistingTextCompositionResult::CompositionActive, S_FALSE);
    }

    InputScopePrivacy privacy = InputScopePrivacy::Unknown;
    const HRESULT privacy_hr = ReadContextInputScopePrivacy(
        context_, ec, &privacy);
    if (SUCCEEDED(privacy_hr) && privacy == InputScopePrivacy::Sensitive) {
        return Finish(
            ExistingTextCompositionResult::SensitiveContext,
            E_ACCESSDENIED);
    }

    // OnEndEdit 只能申请异步写会话。会话真正执行前，宿主可能又把后续
    // 字母写到了首字母之后，因此以原始范围作为安全基线，再尝试扩展到
    // 当前折叠光标。即使宿主的选择端暂时落在首字母之前，也不能把原始
    // 范围缩成零长度，否则会出现“输入法有组合状态但首字母仍留在正文”。
    ITfRange* adoption_range = nullptr;
    HRESULT hr = range_->Clone(&adoption_range);
    if (FAILED(hr) || adoption_range == nullptr) {
        SafeRelease(&adoption_range);
        return Finish(ExistingTextCompositionResult::Failed,
                      FAILED(hr) ? hr : E_FAIL);
    }

    const auto read_recoverable_text =
        [this, ec](ITfRange* source, std::wstring* output) {
            if (source == nullptr || output == nullptr) return false;
            ITfRange* read_range = nullptr;
            if (FAILED(source->Clone(&read_range)) || read_range == nullptr) {
                SafeRelease(&read_range);
                return false;
            }
            std::array<wchar_t, kFirstKeyRecoveryMaximumTextLength + 1> text {};
            ULONG text_length = 0;
            const HRESULT text_hr = read_range->GetText(
                ec, TF_TF_MOVESTART, text.data(),
                static_cast<ULONG>(text.size()), &text_length);
            BOOL exhausted = FALSE;
            const HRESULT exhausted_hr = SUCCEEDED(text_hr)
                ? read_range->IsEmpty(ec, &exhausted) : text_hr;
            read_range->Release();
            if (FAILED(text_hr) || FAILED(exhausted_hr) || !exhausted) {
                return false;
            }
            std::wstring candidate(text.data(), text_length);
            if (!IsRecoverableAsciiRun(candidate, expected_character_)) {
                return false;
            }
            *output = std::move(candidate);
            return true;
        };

    std::wstring adopted_text;
    if (!read_recoverable_text(adoption_range, &adopted_text)) {
        adoption_range->Release();
        return Finish(ExistingTextCompositionResult::RangeChanged, S_FALSE);
    }

    TF_SELECTION selection {};
    ULONG fetched = 0;
    const HRESULT selection_hr = context_->GetSelection(
        ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (SUCCEEDED(selection_hr) && fetched == 1 && selection.range != nullptr) {
        BOOL selection_empty = FALSE;
        const HRESULT empty_hr = selection.range->IsEmpty(ec, &selection_empty);
        if (SUCCEEDED(empty_hr) && selection_empty) {
            LONG start_comparison = 1;
            if (SUCCEEDED(adoption_range->CompareStart(
                    ec, selection.range, TF_ANCHOR_START, &start_comparison)) &&
                start_comparison <= 0) {
                ITfRange* extended_range = nullptr;
                if (SUCCEEDED(adoption_range->Clone(&extended_range)) &&
                    extended_range != nullptr &&
                    SUCCEEDED(extended_range->ShiftEndToRange(
                        ec, selection.range, TF_ANCHOR_START))) {
                    std::wstring extended_text;
                    if (read_recoverable_text(
                            extended_range, &extended_text)) {
                        adoption_range->Release();
                        adoption_range = extended_range;
                        adopted_text = std::move(extended_text);
                    } else {
                        extended_range->Release();
                    }
                } else {
                    SafeRelease(&extended_range);
                }
            }
        } else if (SUCCEEDED(empty_hr) && !selection_empty) {
            selection.range->Release();
            adoption_range->Release();
            return Finish(
                ExistingTextCompositionResult::SelectionChanged, S_FALSE);
        }
        selection.range->Release();
    }

    // 该锚点必须在后续组合文本替换时留在首字母之前。TSF 的后向重力
    // 保证在锚点位置插入或替换文本后，新文本仍位于锚点之后。
    ITfRange* recovered_composition_start = nullptr;
    hr = adoption_range->Clone(&recovered_composition_start);
    if (SUCCEEDED(hr) && recovered_composition_start != nullptr) {
        hr = recovered_composition_start->Collapse(ec, TF_ANCHOR_START);
    }
    if (SUCCEEDED(hr)) {
        hr = recovered_composition_start->SetGravity(
            ec, TF_GRAVITY_BACKWARD, TF_GRAVITY_BACKWARD);
    }
    if (FAILED(hr) || recovered_composition_start == nullptr) {
        SafeRelease(&recovered_composition_start);
        adoption_range->Release();
        return Finish(ExistingTextCompositionResult::Failed,
                      FAILED(hr) ? hr : E_FAIL);
    }

    ITfContextComposition* context_composition = nullptr;
    hr = context_->QueryInterface(
        IID_ITfContextComposition,
        reinterpret_cast<void**>(&context_composition));
    if (FAILED(hr) || context_composition == nullptr) {
        recovered_composition_start->Release();
        adoption_range->Release();
        SafeRelease(&context_composition);
        return Finish(ExistingTextCompositionResult::Failed,
                      FAILED(hr) ? hr : E_NOINTERFACE);
    }

    ITfComposition* adopted_composition = nullptr;
    bool took_existing_composition = false;
    IEnumITfCompositionView* composition_views = nullptr;
    HRESULT find_hr = context_composition->FindComposition(
        ec, adoption_range, &composition_views);
    if (SUCCEEDED(find_hr) && composition_views != nullptr) {
        ITfCompositionView* existing_view = nullptr;
        ULONG view_count = 0;
        const HRESULT next_hr = composition_views->Next(
            1, &existing_view, &view_count);
        if (SUCCEEDED(next_hr) && view_count == 1 && existing_view != nullptr) {
            const HRESULT take_hr = context_composition->TakeOwnership(
                ec, existing_view, sink_, &adopted_composition);
            took_existing_composition = SUCCEEDED(take_hr) &&
                adopted_composition != nullptr;
            if (!took_existing_composition && FAILED(take_hr)) {
                hr = take_hr;
            }
        }
        SafeRelease(&existing_view);
        composition_views->Release();
    }
    if (!took_existing_composition) {
        hr = context_composition->StartComposition(
            ec, adoption_range, sink_, &adopted_composition);
    }
    context_composition->Release();
    if (FAILED(hr) || adopted_composition == nullptr) {
        recovered_composition_start->Release();
        adoption_range->Release();
        SafeRelease(&adopted_composition);
        return Finish(ExistingTextCompositionResult::Failed,
                      FAILED(hr) ? hr : E_FAIL);
    }

    *composition_ = adopted_composition;
    ITfRange* composition_range = nullptr;
    hr = adopted_composition->GetRange(&composition_range);
    if (FAILED(hr) || composition_range == nullptr) {
        recovered_composition_start->Release();
        adoption_range->Release();
        adopted_composition->EndComposition(ec);
        adopted_composition->Release();
        *composition_ = nullptr;
        SafeRelease(&composition_range);
        return Finish(ExistingTextCompositionResult::Failed,
                      FAILED(hr) ? hr : E_FAIL);
    }
    // 某些 WinUI 宿主会在 StartComposition 返回时把组合范围折叠到
    // 当前插入点。必须通过 ITfComposition 自身移动边界；只移动
    // GetRange 返回的快照不会更新组合对象，空格提交时首字母仍会残留。
    ITfRange* composition_start = nullptr;
    ITfRange* composition_end = nullptr;
    HRESULT boundary_hr = adoption_range->Clone(&composition_start);
    if (SUCCEEDED(boundary_hr) && composition_start != nullptr) {
        boundary_hr = composition_start->Collapse(ec, TF_ANCHOR_START);
    }
    if (SUCCEEDED(boundary_hr)) {
        boundary_hr = adoption_range->Clone(&composition_end);
        if (SUCCEEDED(boundary_hr) && composition_end != nullptr) {
            boundary_hr = composition_end->Collapse(ec, TF_ANCHOR_END);
        }
    }
    const HRESULT start_shift_hr = SUCCEEDED(boundary_hr)
        ? adopted_composition->ShiftStart(ec, composition_start)
        : boundary_hr;
    const HRESULT end_shift_hr = SUCCEEDED(start_shift_hr)
        ? adopted_composition->ShiftEnd(ec, composition_end)
        : start_shift_hr;
    SafeRelease(&composition_start);
    SafeRelease(&composition_end);
    if (FAILED(end_shift_hr)) {
        recovered_composition_start->Release();
        composition_range->Release();
        adoption_range->Release();
        adopted_composition->EndComposition(ec);
        adopted_composition->Release();
        *composition_ = nullptr;
        return Finish(ExistingTextCompositionResult::Failed, end_shift_hr);
    }
    ApplyCompositionDisplayAttribute(
        context_, ec, composition_range, display_atom_);
    SetCaretToRangeEnd(context_, ec, composition_range);
    *recovered_composition_start_ = recovered_composition_start;
    composition_range->Release();
    adoption_range->Release();
    return Finish(
        ExistingTextCompositionResult::Adopted,
        S_OK,
        std::move(adopted_text));
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
    return ReadContextInputScopePrivacy(context_, ec, out_privacy_);
}

} // namespace shuru
