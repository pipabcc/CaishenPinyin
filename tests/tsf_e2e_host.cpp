#include "common/guid_def.h"

#include <Windows.h>
#include <ctffunc.h>
#include <msctf.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

constexpr GUID kSearchBoxIntegrationStyle = {
    0xe6d1bd11, 0x82f7, 0x4903,
    {0xae, 0x21, 0x1a, 0x63, 0x97, 0xcd, 0xe2, 0xeb}};

class UiElementSink final : public ITfUIElementSink {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfUIElementSink) {
            *object = static_cast<ITfUIElementSink*>(this);
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

    HRESULT STDMETHODCALLTYPE BeginUIElement(
        DWORD element_id, BOOL* show) override {
        if (show == nullptr) return E_INVALIDARG;
        last_element_id_ = element_id;
        ++begin_count_;
        *show = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD element_id) override {
        if (element_id == last_element_id_) ++update_count_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD element_id) override {
        if (element_id == last_element_id_) ++end_count_;
        return S_OK;
    }

    void Reset() noexcept {
        last_element_id_ = TF_INVALID_UIELEMENTID;
        begin_count_ = 0;
        update_count_ = 0;
        end_count_ = 0;
    }

    DWORD last_element_id() const noexcept { return last_element_id_; }
    unsigned begin_count() const noexcept { return begin_count_; }
    unsigned update_count() const noexcept { return update_count_; }
    unsigned end_count() const noexcept { return end_count_; }

private:
    LONG refs_ = 1;
    DWORD last_element_id_ = TF_INVALID_UIELEMENTID;
    unsigned begin_count_ = 0;
    unsigned update_count_ = 0;
    unsigned end_count_ = 0;
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

bool RunReadSession(
    ITfContext* context, TfClientId client_id, std::wstring* output) {
    auto* session = new ReadTextEditSession(context, output);
    HRESULT session_result = E_FAIL;
    const HRESULT request_result = context->RequestEditSession(
        client_id, session, TF_ES_SYNC | TF_ES_READ, &session_result);
    session->Release();
    return SUCCEEDED(request_result) && SUCCEEDED(session_result);
}

bool SendKey(
    ITfKeyEventSink* key_sink,
    ITfContext* context,
    WPARAM key,
    bool expected_eaten) {
    BOOL test_eaten = FALSE;
    HRESULT hr = key_sink->OnTestKeyDown(context, key, 0, &test_eaten);
    if (FAILED(hr) || (test_eaten != FALSE) != expected_eaten) {
        std::fprintf(
            stderr, "test-key mismatch: key=%lu hr=0x%08lx eaten=%d expected=%d\n",
            static_cast<unsigned long>(key), static_cast<unsigned long>(hr),
            test_eaten, expected_eaten ? 1 : 0);
        return false;
    }
    BOOL eaten = FALSE;
    hr = key_sink->OnKeyDown(context, key, 0, &eaten);
    if (FAILED(hr) || (eaten != FALSE) != expected_eaten) {
        std::fprintf(
            stderr, "key mismatch: key=%lu hr=0x%08lx eaten=%d expected=%d\n",
            static_cast<unsigned long>(key), static_cast<unsigned long>(hr),
            eaten, expected_eaten ? 1 : 0);
        return false;
    }
    return true;
}

bool SendText(
    ITfKeyEventSink* key_sink,
    ITfContext* context,
    const char* text,
    WPARAM commit_key) {
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        const WPARAM key = static_cast<WPARAM>(
            std::toupper(static_cast<unsigned char>(*cursor)));
        if (!SendKey(key_sink, context, key, true)) return false;
    }
    return SendKey(key_sink, context, commit_key, true);
}

bool SendComposingText(
    ITfKeyEventSink* key_sink,
    ITfContext* context,
    const char* text) {
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        const WPARAM key = static_cast<WPARAM>(
            std::toupper(static_cast<unsigned char>(*cursor)));
        if (!SendKey(key_sink, context, key, true)) return false;
    }
    return true;
}

bool ToggleEnglishWithShift(
    ITfKeyEventSink* key_sink, ITfContext* context) {
    if (key_sink == nullptr || context == nullptr) return false;
    BOOL eaten = FALSE;
    HRESULT hr = key_sink->OnTestKeyDown(context, VK_SHIFT, 0, &eaten);
    if (FAILED(hr) || eaten != FALSE) return false;
    eaten = FALSE;
    hr = key_sink->OnKeyDown(context, VK_SHIFT, 0, &eaten);
    if (FAILED(hr) || eaten != FALSE) return false;
    eaten = FALSE;
    hr = key_sink->OnTestKeyUp(context, VK_SHIFT, 0, &eaten);
    if (FAILED(hr) || eaten == FALSE) return false;
    eaten = TRUE;
    hr = key_sink->OnKeyUp(context, VK_SHIFT, 0, &eaten);
    return SUCCEEDED(hr) && eaten == FALSE;
}

bool GetCompartmentDword(
    ITfThreadMgr* thread_manager, REFGUID guid, DWORD* value) {
    if (thread_manager == nullptr || value == nullptr) return false;
    ITfCompartmentMgr* manager = nullptr;
    HRESULT hr = thread_manager->QueryInterface(
        IID_ITfCompartmentMgr, reinterpret_cast<void**>(&manager));
    if (FAILED(hr) || manager == nullptr) return false;
    ITfCompartment* compartment = nullptr;
    hr = manager->GetCompartment(guid, &compartment);
    manager->Release();
    if (FAILED(hr) || compartment == nullptr) return false;
    VARIANT data;
    VariantInit(&data);
    hr = compartment->GetValue(&data);
    if (SUCCEEDED(hr) && data.vt == VT_I4) {
        *value = static_cast<DWORD>(data.lVal);
    } else if (SUCCEEDED(hr)) {
        hr = E_UNEXPECTED;
    }
    VariantClear(&data);
    compartment->Release();
    return SUCCEEDED(hr);
}

bool VerifyInputMode(ITfThreadMgr* thread_manager, bool chinese) {
    DWORD open = 0;
    DWORD conversion = 0;
    const bool read = GetCompartmentDword(
                          thread_manager,
                          GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &open) &&
        GetCompartmentDword(
            thread_manager, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
            &conversion);
    std::wprintf(
        L"input-mode open=%lu conversion=0x%08lx expected=%ls\n",
        static_cast<unsigned long>(open),
        static_cast<unsigned long>(conversion),
        chinese ? L"Chinese" : L"English");
    return read && (open != 0) == chinese &&
        ((conversion & TF_CONVERSIONMODE_NATIVE) != 0) == chinese;
}

bool HasVisibleCandidateWindow() {
    struct SearchState {
        bool found = false;
    } state;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND window, LPARAM value) -> BOOL {
            auto* search = reinterpret_cast<SearchState*>(value);
            wchar_t class_name[128] {};
            if (IsWindowVisible(window) &&
                GetClassNameW(window, class_name, ARRAYSIZE(class_name)) > 0 &&
                wcscmp(class_name, L"ShuruCandidateWindowClass") == 0) {
                search->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));
    return state.found;
}

bool InspectAndFinalizeCandidateUi(
    ITfUIElementMgr* manager,
    DWORD element_id,
    ITfDocumentMgr* expected_document,
    ITfCandidateListUIElement** retained_candidates) {
    if (manager == nullptr || element_id == TF_INVALID_UIELEMENTID ||
        expected_document == nullptr || retained_candidates == nullptr) {
        return false;
    }
    *retained_candidates = nullptr;

    ITfUIElement* element = nullptr;
    ITfCandidateListUIElement* candidates = nullptr;
    ITfCandidateListUIElementBehavior* behavior = nullptr;
    ITfIntegratableCandidateListUIElement* integratable = nullptr;
    ITfDocumentMgr* document = nullptr;
    bool passed = false;

    HRESULT hr = manager->GetUIElement(element_id, &element);
    if (FAILED(hr) || element == nullptr) goto cleanup;
    {
        GUID guid {};
        BOOL shown = FALSE;
        if (FAILED(element->GetGUID(&guid)) ||
            !IsEqualGUID(guid, GUID_ShuruCandidateUIElement) ||
            FAILED(element->IsShown(&shown)) || shown == FALSE) {
            goto cleanup;
        }
    }
    hr = element->QueryInterface(
        IID_ITfCandidateListUIElement,
        reinterpret_cast<void**>(&candidates));
    if (FAILED(hr) || candidates == nullptr) goto cleanup;
    hr = element->QueryInterface(
        IID_ITfCandidateListUIElementBehavior,
        reinterpret_cast<void**>(&behavior));
    if (FAILED(hr) || behavior == nullptr) goto cleanup;
    hr = element->QueryInterface(
        IID_ITfIntegratableCandidateListUIElement,
        reinterpret_cast<void**>(&integratable));
    if (FAILED(hr) || integratable == nullptr) goto cleanup;

    {
        DWORD flags = 0;
        UINT count = 0;
        UINT selection = 0;
        UINT page_count = 0;
        UINT current_page = 0;
        constexpr DWORD expected_flags =
            TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
            TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
        if (FAILED(candidates->GetUpdatedFlags(&flags)) ||
            (flags & expected_flags) != expected_flags ||
            FAILED(candidates->GetDocumentMgr(&document)) ||
            document != expected_document ||
            FAILED(candidates->GetCount(&count)) || count == 0 ||
            FAILED(candidates->GetSelection(&selection)) ||
            selection >= count) {
            goto cleanup;
        }

        std::vector<UINT> page_indexes(count);
        hr = candidates->GetPageIndex(
            page_indexes.data(), count, &page_count);
        if (FAILED(hr) || page_count == 0 || page_count > count ||
            page_indexes[0] != 0 ||
            FAILED(candidates->GetCurrentPage(&current_page)) ||
            current_page >= page_count) {
            goto cleanup;
        }
        for (UINT page = 1; page < page_count; ++page) {
            if (page_indexes[page] <= page_indexes[page - 1] ||
                page_indexes[page] >= count) {
                goto cleanup;
            }
        }

        UINT test_candidate = selection;
        bool found_expected_candidate = false;
        for (UINT index = 0; index < count; ++index) {
            BSTR value = nullptr;
            hr = candidates->GetString(index, &value);
            if (FAILED(hr) || value == nullptr || SysStringLen(value) == 0) {
                SysFreeString(value);
                goto cleanup;
            }
            if (std::wstring(value, SysStringLen(value)).find(L"\u6d4b\u8bd5") !=
                std::wstring::npos) {
                test_candidate = index;
                found_expected_candidate = true;
            }
            SysFreeString(value);
        }
        if (!found_expected_candidate ||
            FAILED(behavior->SetSelection(test_candidate))) {
            goto cleanup;
        }
        UINT updated_selection = 0;
        if (FAILED(candidates->GetSelection(&updated_selection)) ||
            updated_selection != test_candidate) {
            goto cleanup;
        }
    }

    {
        TfIntegratableCandidateListSelectionStyle selection_style =
            STYLE_IMPLIED_SELECTION;
        BOOL show_numbers = FALSE;
        BOOL eaten = TRUE;
        if (FAILED(integratable->SetIntegrationStyle(
                kSearchBoxIntegrationStyle)) ||
            FAILED(integratable->GetSelectionStyle(&selection_style)) ||
            selection_style != STYLE_ACTIVE_SELECTION ||
            FAILED(integratable->ShowCandidateNumbers(&show_numbers)) ||
            show_numbers == FALSE ||
            FAILED(integratable->OnKeyDown(VK_F24, 0, &eaten)) ||
            eaten != FALSE) {
            goto cleanup;
        }
    }

    candidates->AddRef();
    *retained_candidates = candidates;
    if (FAILED(behavior->Finalize())) {
        (*retained_candidates)->Release();
        *retained_candidates = nullptr;
        goto cleanup;
    }
    passed = true;

cleanup:
    if (document != nullptr) document->Release();
    if (integratable != nullptr) integratable->Release();
    if (behavior != nullptr) behavior->Release();
    if (candidates != nullptr) candidates->Release();
    if (element != nullptr) element->Release();
    return passed;
}

bool InspectAndFinalizeExactCandidateUi(
    ITfUIElementMgr* manager, DWORD element_id) {
    if (manager == nullptr || element_id == TF_INVALID_UIELEMENTID) return false;
    ITfUIElement* element = nullptr;
    ITfIntegratableCandidateListUIElement* integratable = nullptr;
    HRESULT hr = manager->GetUIElement(element_id, &element);
    if (SUCCEEDED(hr) && element != nullptr) {
        hr = element->QueryInterface(
            IID_ITfIntegratableCandidateListUIElement,
            reinterpret_cast<void**>(&integratable));
    }
    if (SUCCEEDED(hr) && integratable != nullptr) {
        hr = integratable->FinalizeExactCompositionString();
    }
    if (integratable != nullptr) integratable->Release();
    if (element != nullptr) element->Release();
    return SUCCEEDED(hr);
}

HRESULT PrepareDirectTipActivation(
    ITfThreadMgr* thread_manager, TfClientId* service_client_id) {
    if (thread_manager == nullptr || service_client_id == nullptr) {
        return E_INVALIDARG;
    }
    *service_client_id = TF_CLIENTID_NULL;

    ITfClientId* client_ids = nullptr;
    HRESULT hr = thread_manager->QueryInterface(
        IID_ITfClientId, reinterpret_cast<void**>(&client_ids));
    if (FAILED(hr) || client_ids == nullptr) return FAILED(hr) ? hr : E_FAIL;

    ITfInputProcessorProfileMgr* profiles = nullptr;
    hr = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfileMgr,
        reinterpret_cast<void**>(&profiles));
    if (SUCCEEDED(hr) && profiles != nullptr) {
        TF_INPUTPROCESSORPROFILE active_profile {};
        hr = profiles->GetActiveProfile(
            GUID_TFCAT_TIP_KEYBOARD, &active_profile);
        if (SUCCEEDED(hr) &&
            active_profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR) {
            TfClientId active_client_id = TF_CLIENTID_NULL;
            hr = client_ids->GetClientId(
                active_profile.clsid, &active_client_id);
            if (SUCCEEDED(hr) && active_client_id != TF_CLIENTID_NULL) {
                ITfKeystrokeMgr* keystrokes = nullptr;
                hr = thread_manager->QueryInterface(
                    IID_ITfKeystrokeMgr,
                    reinterpret_cast<void**>(&keystrokes));
                if (SUCCEEDED(hr) && keystrokes != nullptr) {
                    // ThreadMgr::Activate 可能把用户当前 TIP 加载到测试进程。
                    // 只解除该进程内的 sink，让直接加载的待测 TIP 成为前台。
                    (void)keystrokes->UnadviseKeyEventSink(active_client_id);
                    keystrokes->Release();
                }
            }
        }
        profiles->Release();
    }

    hr = client_ids->GetClientId(
        CLSID_ShuruTextService, service_client_id);
    client_ids->Release();
    return hr;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) return 1;

    ITfThreadMgr* thread_manager = nullptr;
    TfClientId client_id = TF_CLIENTID_NULL;
    TfClientId service_client_id = TF_CLIENTID_NULL;
    ITfDocumentMgr* document_manager = nullptr;
    ITfContext* body_context = nullptr;
    ITfContext* search_context = nullptr;
    TfEditCookie body_cookie = TF_INVALID_COOKIE;
    TfEditCookie search_cookie = TF_INVALID_COOKIE;
    HMODULE ime_module = nullptr;
    ITfTextInputProcessorEx* text_service = nullptr;
    ITfKeyEventSink* key_sink = nullptr;
    ITfUIElementMgr* ui_element_manager = nullptr;
    ITfSource* ui_element_source = nullptr;
    UiElementSink* ui_element_sink = nullptr;
    ITfCandidateListUIElement* retained_candidates = nullptr;
    DWORD ui_element_sink_cookie = TF_INVALID_COOKIE;
    bool text_service_deactivated = false;
    int result = 1;
    const char* failure_stage = "CoCreateInstance(CLSID_TF_ThreadMgr)";

    ITfThreadMgrEx* thread_manager_ex = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgrEx, reinterpret_cast<void**>(&thread_manager_ex));
    if (FAILED(hr) || thread_manager_ex == nullptr) goto cleanup;
    thread_manager = thread_manager_ex;
    failure_stage = "ITfThreadMgrEx::ActivateEx(UIELEMENTENABLEDONLY)";
    hr = thread_manager_ex->ActivateEx(
        &client_id, TF_TMAE_UIELEMENTENABLEDONLY);
    if (FAILED(hr)) goto cleanup;
    failure_stage = "PrepareDirectTipActivation";
    hr = PrepareDirectTipActivation(thread_manager, &service_client_id);
    if (FAILED(hr) || service_client_id == TF_CLIENTID_NULL) goto cleanup;
    failure_stage = "ITfThreadMgr::CreateDocumentMgr";
    hr = thread_manager->CreateDocumentMgr(&document_manager);
    if (FAILED(hr) || document_manager == nullptr) goto cleanup;
    failure_stage = "ITfDocumentMgr::CreateContext(body)";
    hr = document_manager->CreateContext(
        client_id, 0, nullptr, &body_context, &body_cookie);
    if (FAILED(hr) || body_context == nullptr) goto cleanup;
    failure_stage = "ITfDocumentMgr::Push(body)/ITfThreadMgr::SetFocus";
    if (FAILED(document_manager->Push(body_context)) ||
        FAILED(thread_manager->SetFocus(document_manager))) {
        goto cleanup;
    }

    failure_stage = "Advise ITfUIElementSink";
    hr = thread_manager->QueryInterface(
        IID_ITfUIElementMgr,
        reinterpret_cast<void**>(&ui_element_manager));
    if (FAILED(hr) || ui_element_manager == nullptr) goto cleanup;
    hr = thread_manager->QueryInterface(
        IID_ITfSource, reinterpret_cast<void**>(&ui_element_source));
    if (FAILED(hr) || ui_element_source == nullptr) goto cleanup;
    ui_element_sink = new UiElementSink();
    hr = ui_element_source->AdviseSink(
        IID_ITfUIElementSink,
        static_cast<ITfUIElementSink*>(ui_element_sink),
        &ui_element_sink_cookie);
    if (FAILED(hr)) goto cleanup;

    failure_stage = "LoadLibraryW(ShuruIme.dll)";
    if (GetFileAttributesW(argv[1]) == INVALID_FILE_ATTRIBUTES) goto cleanup;
    ime_module = LoadLibraryW(argv[1]);
    if (ime_module == nullptr) goto cleanup;
    {
        failure_stage = "DllGetClassObject";
        const auto get_class_object = reinterpret_cast<GetClassObject>(
            GetProcAddress(ime_module, "DllGetClassObject"));
        IClassFactory* factory = nullptr;
        hr = get_class_object != nullptr
            ? get_class_object(
                  CLSID_ShuruTextService, IID_IClassFactory,
                  reinterpret_cast<void**>(&factory))
            : E_POINTER;
        if (FAILED(hr) || factory == nullptr) goto cleanup;
        failure_stage = "IClassFactory::CreateInstance";
        hr = factory->CreateInstance(
            nullptr, IID_ITfTextInputProcessorEx,
            reinterpret_cast<void**>(&text_service));
        factory->Release();
        if (FAILED(hr) || text_service == nullptr) goto cleanup;
    }
    failure_stage = "ITfTextInputProcessorEx::ActivateEx";
    // 宿主将按键直接发给待测 sink，同时使用该输入服务 CLSID 对应的
    // ClientId，保持键盘 sink、编辑会话与真实 TSF 激活约束一致。
    hr = text_service->ActivateEx(thread_manager, service_client_id, 0);
    if (FAILED(hr)) goto cleanup;
    failure_stage = "QueryInterface(IID_ITfKeyEventSink)";
    hr = text_service->QueryInterface(
        IID_ITfKeyEventSink, reinterpret_cast<void**>(&key_sink));
    if (FAILED(hr) || key_sink == nullptr) goto cleanup;

    Sleep(2500);

    // 无传统语言栏的宿主也必须在激活阶段公开完整的中文输入模式。
    failure_stage = "VerifyInputMode(initial Chinese)";
    if (!VerifyInputMode(thread_manager, true)) {
        std::fprintf(stderr, "Chinese input-mode compartments are inconsistent\n");
        goto cleanup;
    }

    failure_stage = "ToggleInputMode(English)";
    if (!ToggleEnglishWithShift(key_sink, body_context) ||
        !VerifyInputMode(thread_manager, false)) {
        std::fprintf(stderr, "English input-mode compartments are inconsistent\n");
        goto cleanup;
    }
    failure_stage = "ToggleInputMode(Chinese)";
    if (!ToggleEnglishWithShift(key_sink, body_context) ||
        !VerifyInputMode(thread_manager, true)) {
        std::fprintf(stderr, "restored Chinese input-mode compartments are inconsistent\n");
        goto cleanup;
    }

    // 等后台词库完成初始化，保证候选断言检验的是上下文切换，而不是
    // 首次启动时的原始拼音回退路径。
    Sleep(7500);

    // 在正文中保留一个未提交组合，再压入搜索上下文。这能暴露把全局
    // composition_ 错误用于新上下文的问题。
    failure_stage = "SendKey(body composition)";
    if (!SendKey(key_sink, body_context, 'C', true) ||
        !SendKey(key_sink, body_context, 'E', true)) {
        goto cleanup;
    }
    failure_stage = "ITfDocumentMgr::CreateContext/Push(search)";
    hr = document_manager->CreateContext(
        client_id, 0, nullptr, &search_context, &search_cookie);
    if (FAILED(hr) || search_context == nullptr ||
        FAILED(document_manager->Push(search_context))) {
        goto cleanup;
    }
    ui_element_sink->Reset();
    failure_stage = "SendComposingText(search context)";
    if (!SendComposingText(key_sink, search_context, "ceshi")) goto cleanup;
    failure_stage = "InspectAndFinalizeCandidateUi";
    if (ui_element_sink->begin_count() == 0 ||
        ui_element_sink->update_count() == 0 ||
        HasVisibleCandidateWindow() ||
        !InspectAndFinalizeCandidateUi(
            ui_element_manager, ui_element_sink->last_element_id(),
            document_manager, &retained_candidates) ||
        ui_element_sink->end_count() == 0) {
        std::fprintf(stderr, "UI-less candidate list contract failed\n");
        goto cleanup;
    }

    {
        failure_stage = "ReadText(search context)";
        std::wstring search_text;
        if (!RunReadSession(search_context, client_id, &search_text) ||
            search_text.find(L"\u6d4b\u8bd5") == std::wstring::npos) {
            std::fwprintf(
                stderr, L"unexpected search-context text: %ls\n",
                search_text.c_str());
            goto cleanup;
        }
    }

    ui_element_sink->Reset();
    failure_stage = "FinalizeExactCompositionString";
    if (!SendComposingText(key_sink, search_context, "raw") ||
        ui_element_sink->begin_count() == 0 ||
        HasVisibleCandidateWindow() ||
        !InspectAndFinalizeExactCandidateUi(
            ui_element_manager, ui_element_sink->last_element_id()) ||
        ui_element_sink->end_count() == 0) {
        std::fprintf(stderr, "integratable exact-composition finalize failed\n");
        goto cleanup;
    }

    failure_stage = "ITfDocumentMgr::Pop(search)";
    if (FAILED(document_manager->Pop(0))) goto cleanup;
    search_context->Release();
    search_context = nullptr;

    // 弹出搜索上下文后必须恢复正文栈顶，并能立刻开始下一轮组合。
    failure_stage = "SendText(restored body context)";
    if (!SendText(key_sink, body_context, "shuru", VK_SPACE)) goto cleanup;
    {
        failure_stage = "ReadText(restored body context)";
        std::wstring body_text;
        if (!RunReadSession(body_context, client_id, &body_text) ||
            body_text.find(L"\u8f93\u5165") == std::wstring::npos) {
            std::fwprintf(
                stderr, L"unexpected body-context text: %ls\n",
                body_text.c_str());
            goto cleanup;
        }
    }

    failure_stage = "Deactivate/disconnect retained UIElement";
    hr = text_service->Deactivate();
    if (FAILED(hr)) goto cleanup;
    text_service_deactivated = true;
    {
        UINT stale_count = 0;
        if (retained_candidates == nullptr ||
            retained_candidates->GetCount(&stale_count) != TF_E_DISCONNECTED) {
            std::fprintf(stderr, "retained UIElement did not disconnect\n");
            goto cleanup;
        }
    }

    std::wprintf(
        L"input-mode compartments, UI-less candidates, and stacked TSF contexts passed\n");
    result = 0;

cleanup:
    if (result != 0) {
        std::fprintf(
            stderr, "TSF host failed at %s: hr=0x%08lx win32=%lu\n",
            failure_stage, static_cast<unsigned long>(hr),
            static_cast<unsigned long>(GetLastError()));
    }
    if (key_sink != nullptr) key_sink->Release();
    if (text_service != nullptr) {
        if (!text_service_deactivated) text_service->Deactivate();
        text_service->Release();
    }
    if (retained_candidates != nullptr) retained_candidates->Release();
    if (ui_element_source != nullptr &&
        ui_element_sink_cookie != TF_INVALID_COOKIE) {
        ui_element_source->UnadviseSink(ui_element_sink_cookie);
        ui_element_sink_cookie = TF_INVALID_COOKIE;
    }
    if (ui_element_sink != nullptr) ui_element_sink->Release();
    if (ui_element_source != nullptr) ui_element_source->Release();
    if (ui_element_manager != nullptr) ui_element_manager->Release();
    if (search_context != nullptr) search_context->Release();
    if (document_manager != nullptr) document_manager->Pop(TF_POPF_ALL);
    if (body_context != nullptr) body_context->Release();
    if (document_manager != nullptr) document_manager->Release();
    if (thread_manager != nullptr) {
        if (client_id != TF_CLIENTID_NULL) thread_manager->Deactivate();
        thread_manager->Release();
    }
    // DLL 内部词库可能仍在完成后台缓存构建。测试进程退出时由系统统一
    // 回收模块，避免在后台线程仍持有代码地址时主动 FreeLibrary。
    CoUninitialize();
    return result;
}
