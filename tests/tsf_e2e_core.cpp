#include "ime/edit_sessions.h"

#include <Windows.h>
#include <msctf.h>

#include <cstdio>
#include <string>

namespace {

class CompositionSink final : public ITfCompositionSink {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfCompositionSink) {
            *object = static_cast<ITfCompositionSink*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs_);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(
        TfEditCookie /*edit_cookie*/, ITfComposition* /*composition*/) override {
        return S_OK;
    }

private:
    LONG refs_ = 1;
};

class ReadTextEditSession final : public ITfEditSession {
public:
    ReadTextEditSession(ITfContext* context, std::wstring* output)
        : context_(context), output_(output) {
        if (context_ != nullptr) context_->AddRef();
    }

    ~ReadTextEditSession() {
        if (context_ != nullptr) context_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfEditSession) {
            *object = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs_);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }

    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie edit_cookie) override {
        if (context_ == nullptr || output_ == nullptr) return E_INVALIDARG;

        ITfRange* range = nullptr;
        HRESULT hr = context_->GetStart(edit_cookie, &range);
        if (FAILED(hr) || range == nullptr) return FAILED(hr) ? hr : E_FAIL;

        LONG moved = 0;
        range->ShiftEnd(edit_cookie, 512, &moved, nullptr);
        wchar_t buffer[512] {};
        ULONG length = 0;
        hr = range->GetText(edit_cookie, 0, buffer, 511, &length);
        range->Release();
        if (SUCCEEDED(hr)) output_->assign(buffer, length);
        return hr;
    }

private:
    LONG refs_ = 1;
    ITfContext* context_ = nullptr;
    std::wstring* output_ = nullptr;
};

class ReadSelectionEditSession final : public ITfEditSession {
public:
    ReadSelectionEditSession(ITfContext* context, bool* is_empty, bool* is_interim, TfActiveSelEnd* ase)
        : context_(context), is_empty_(is_empty), is_interim_(is_interim), ase_(ase) {
        if (context_ != nullptr) context_->AddRef();
    }
    ~ReadSelectionEditSession() {
        if (context_ != nullptr) context_->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfEditSession) {
            *object = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie ec) override {
        if (context_ == nullptr) return E_INVALIDARG;
        TF_SELECTION sel {};
        ULONG fetched = 0;
        HRESULT hr = context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
        if (FAILED(hr) || fetched == 0 || sel.range == nullptr) return FAILED(hr) ? hr : E_FAIL;
        BOOL empty = FALSE;
        sel.range->IsEmpty(ec, &empty);
        if (is_empty_) *is_empty_ = (empty != FALSE);
        if (is_interim_) *is_interim_ = (sel.style.fInterimChar != FALSE);
        if (ase_) *ase_ = sel.style.ase;
        sel.range->Release();
        return S_OK;
    }
private:
    LONG refs_ = 1;
    ITfContext* context_ = nullptr;
    bool* is_empty_ = nullptr;
    bool* is_interim_ = nullptr;
    TfActiveSelEnd* ase_ = nullptr;
};

class InsertPlainTextEditSession final : public ITfEditSession {
public:
    InsertPlainTextEditSession(
        ITfContext* context, const std::wstring& text, ITfRange** output_range)
        : context_(context), text_(text), output_range_(output_range) {
        if (context_ != nullptr) context_->AddRef();
        if (output_range_ != nullptr) *output_range_ = nullptr;
    }
    ~InsertPlainTextEditSession() {
        if (context_ != nullptr) context_->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfEditSession) {
            *object = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie ec) override {
        if (context_ == nullptr || output_range_ == nullptr) return E_INVALIDARG;
        ITfInsertAtSelection* insert = nullptr;
        HRESULT hr = context_->QueryInterface(
            IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert));
        if (FAILED(hr) || insert == nullptr) return FAILED(hr) ? hr : E_FAIL;
        ITfRange* range = nullptr;
        hr = insert->InsertTextAtSelection(
            ec, 0, text_.c_str(), static_cast<LONG>(text_.size()), &range);
        insert->Release();
        if (FAILED(hr) || range == nullptr) {
            if (range != nullptr) range->Release();
            return FAILED(hr) ? hr : E_FAIL;
        }
        ITfRange* caret = nullptr;
        if (SUCCEEDED(range->Clone(&caret)) && caret != nullptr) {
            caret->Collapse(ec, TF_ANCHOR_END);
            TF_SELECTION selection {};
            selection.range = caret;
            selection.style.ase = TF_AE_END;
            selection.style.fInterimChar = FALSE;
            context_->SetSelection(ec, 1, &selection);
            caret->Release();
        }
        *output_range_ = range;
        return S_OK;
    }
private:
    LONG refs_ = 1;
    ITfContext* context_ = nullptr;
    std::wstring text_;
    ITfRange** output_range_ = nullptr;
};

bool RunEditSession(
    ITfContext* context,
    TfClientId client_id,
    ITfEditSession* session,
    DWORD flags = TF_ES_SYNC | TF_ES_READWRITE) {
    if (context == nullptr || session == nullptr) return false;
    HRESULT session_result = E_FAIL;
    const HRESULT request_result =
        context->RequestEditSession(client_id, session, flags, &session_result);
    session->Release();
    return SUCCEEDED(request_result) && SUCCEEDED(session_result);
}

}  // namespace

int wmain() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        std::fprintf(stderr, "CoInitializeEx failed: 0x%08lx\n",
                     static_cast<unsigned long>(com_result));
        return 1;
    }

    ITfThreadMgrEx* thread_manager = nullptr;
    ITfDocumentMgr* document_manager = nullptr;
    ITfContext* context = nullptr;
    ITfComposition* composition = nullptr;
    auto* sink = new CompositionSink();
    TfClientId client_id = TF_CLIENTID_NULL;
    TfEditCookie edit_cookie = TF_INVALID_COOKIE;
    int result = 1;

    HRESULT hr = CoCreateInstance(
        CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgrEx, reinterpret_cast<void**>(&thread_manager));
    if (FAILED(hr) || thread_manager == nullptr) goto cleanup;

    // This test links the edit-session implementation directly. Loading any TIP registered
    // on the developer machine would make the test depend on an unrelated installed DLL.
    hr = thread_manager->ActivateEx(&client_id, TF_TMAE_NOACTIVATETIP);
    if (FAILED(hr)) goto cleanup;
    hr = thread_manager->CreateDocumentMgr(&document_manager);
    if (FAILED(hr) || document_manager == nullptr) goto cleanup;
    hr = document_manager->CreateContext(
        client_id, 0, nullptr, &context, &edit_cookie);
    if (FAILED(hr) || context == nullptr) goto cleanup;
    if (FAILED(document_manager->Push(context)) ||
        FAILED(thread_manager->SetFocus(document_manager))) {
        goto cleanup;
    }

    if (!RunEditSession(
            context, client_id,
            new shuru::SetCompositionEditSession(
                context, client_id, sink, &composition, L"suixinshuru"))) {
        goto cleanup;
    }

    {
        bool is_empty = false;
        bool is_interim = true;
        TfActiveSelEnd ase = TF_AE_NONE;
        if (!RunEditSession(
                context, client_id,
                new ReadSelectionEditSession(context, &is_empty, &is_interim, &ase),
                TF_ES_SYNC | TF_ES_READ) ||
            !is_empty || is_interim || ase != TF_AE_END) {
            std::fwprintf(stderr, L"composition caret not collapsed to end: empty=%d interim=%d ase=%d\n",
                          is_empty, is_interim, ase);
            goto cleanup;
        }
    }

    // 后续改写组合串不再重复 SetSelection；宿主应保留已经折叠到末端的光标。
    if (!RunEditSession(
            context, client_id,
            new shuru::SetCompositionEditSession(
                context, client_id, sink, &composition, L"suixinshurua",
                TF_INVALID_GUIDATOM, false))) {
        goto cleanup;
    }
    {
        bool is_empty = false;
        bool is_interim = true;
        TfActiveSelEnd ase = TF_AE_NONE;
        if (!RunEditSession(
                context, client_id,
                new ReadSelectionEditSession(context, &is_empty, &is_interim, &ase),
                TF_ES_SYNC | TF_ES_READ) ||
            !is_empty || is_interim || ase != TF_AE_END) {
            std::fwprintf(stderr,
                          L"composition update moved caret unexpectedly: empty=%d interim=%d ase=%d\n",
                          is_empty, is_interim, ase);
            goto cleanup;
        }
    }

    if (!RunEditSession(
            context, client_id,
            new shuru::InsertTextEditSession(
                context, client_id, &composition, L"\u968f\u5fc3"))) {
        goto cleanup;
    }

    // A committed prefix remains while the tail becomes a new real TSF composition.
    if (!RunEditSession(
            context, client_id,
            new shuru::SetCompositionEditSession(
                context, client_id, sink, &composition, L"shuru")) ||
        !RunEditSession(
            context, client_id,
            new shuru::InsertTextEditSession(
                context, client_id, &composition, L"\u8f93\u5165"))) {
        goto cleanup;
    }

    if (!RunEditSession(
            context, client_id,
            new shuru::InsertTextEditSession(
                context, client_id, &composition, L"1 "))) {
        goto cleanup;
    }

    {
        std::wstring text;
        if (!RunEditSession(
                context, client_id, new ReadTextEditSession(context, &text),
                TF_ES_SYNC | TF_ES_READ) ||
            text != L"\u968f\u5fc3\u8f93\u51651 ") {
            std::fwprintf(stderr, L"unexpected real TSF text: %ls\n", text.c_str());
            goto cleanup;
        }
    }

    // 某些宿主在 Ctrl+C 后绕过按键 sink，先把首字母作为普通文本写入。
    // 恢复路径必须直接接管该范围，再继续写入后续拼音，不能丢失或重复首字母。
    {
        ITfRange* plain_range = nullptr;
        if (!RunEditSession(
                context, client_id,
                new InsertPlainTextEditSession(
                    context, L"j", &plain_range)) ||
            plain_range == nullptr) {
            if (plain_range != nullptr) plain_range->Release();
            goto cleanup;
        }

        shuru::ExistingTextCompositionResult adoption =
            shuru::ExistingTextCompositionResult::Failed;
        if (!RunEditSession(
                context, client_id,
                new shuru::AdoptExistingTextEditSession(
                    context, sink, plain_range, L'j', &composition,
                    TF_INVALID_GUIDATOM, []() { return true; },
                    [&adoption](shuru::ExistingTextCompositionResult result) {
                        adoption = result;
                    })) ||
            adoption != shuru::ExistingTextCompositionResult::Adopted ||
            composition == nullptr) {
            plain_range->Release();
            goto cleanup;
        }
        plain_range->Release();

        if (!RunEditSession(
                context, client_id,
                new shuru::SetCompositionEditSession(
                    context, client_id, sink, &composition, L"jix",
                    TF_INVALID_GUIDATOM, false)) ||
            !RunEditSession(
                context, client_id,
                new shuru::InsertTextEditSession(
                    context, client_id, &composition, L"jix"))) {
            goto cleanup;
        }

        std::wstring text;
        if (!RunEditSession(
                context, client_id, new ReadTextEditSession(context, &text),
                TF_ES_SYNC | TF_ES_READ) ||
            text != L"\u968f\u5fc3\u8f93\u51651 jix") {
            std::fwprintf(
                stderr, L"unexpected adopted first-key text: %ls\n",
                text.c_str());
            goto cleanup;
        }
    }

    std::wprintf(
        L"real ITfThreadMgr/document/context composition/partial/submit flow passed\n");
    result = 0;

cleanup:
    if (composition != nullptr) composition->Release();
    if (document_manager != nullptr) document_manager->Pop(TF_POPF_ALL);
    sink->Release();
    if (context != nullptr) context->Release();
    if (document_manager != nullptr) document_manager->Release();
    if (thread_manager != nullptr) {
        if (client_id != TF_CLIENTID_NULL) thread_manager->Deactivate();
        thread_manager->Release();
    }
    CoUninitialize();
    return result;
}
