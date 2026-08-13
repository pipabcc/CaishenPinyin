#include "text_service.h"

#include "engine/special_input.h"
#include "ui/shared_status_ui.h"
#include "ui/ime_ui_logic.h"
#include "../engine/shared_engine.h"
#include "display_attribute.h"
#include "../common/guid_def.h"
#include "edit_sessions.h"
#include <new>


#include "globals.h"
#include "../common/com_utils.h"
#include "../common/logger.h"
#include "../common/runtime_config.h"

#include <cctype>
#include <cwchar>

namespace shuru {

namespace {

bool IsShiftKey(WPARAM wparam) {
    return wparam == VK_SHIFT || wparam == VK_LSHIFT || wparam == VK_RSHIFT;
}

// Ctrl/Alt/Win 按下时把快捷键交给应用（Ctrl+A/C/V 等），IME 不吞键。
bool HasShortcutModifier() {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
}

bool IsCtrlOnlyDown() {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    return ctrl && !alt && !win;
}

std::wstring PinyinToWide(const std::string& pinyin) {
    return std::wstring(pinyin.begin(), pinyin.end());
}

bool IsNumpadDigit(WPARAM wparam) {
    return wparam >= VK_NUMPAD0 && wparam <= VK_NUMPAD9;
}

bool TryMapChinesePunctuation(WPARAM wparam, bool shift, wchar_t* punctuation) {
    if (punctuation == nullptr) {
        return false;
    }
    switch (wparam) {
    case VK_OEM_COMMA:  *punctuation = shift ? L'《' : L'，'; return true;
    case VK_OEM_PERIOD: *punctuation = shift ? L'》' : L'。'; return true;
    case VK_OEM_1:      *punctuation = shift ? L'：' : L'；'; return true;
    case VK_OEM_2:      *punctuation = shift ? L'？' : L'、'; return true;
    case VK_OEM_4:      *punctuation = L'【'; return true;
    case VK_OEM_6:      *punctuation = L'】'; return true;
    case '1':
        if (shift) { *punctuation = L'！'; return true; }
        return false;
    case '9':
        if (shift) { *punctuation = L'（'; return true; }
        return false;
    case '0':
        if (shift) { *punctuation = L'）'; return true; }
        return false;
    default:
        return false;
    }
}

bool IsPasswordWindow(HWND window) {
    for (HWND current = window; current != nullptr; current = GetParent(current)) {
        const LONG_PTR style = GetWindowLongPtrW(current, GWL_STYLE);
        if ((style & ES_PASSWORD) != 0) {
            return true;
        }
        wchar_t class_name[64] = {};
        if (GetClassNameW(current, class_name, ARRAYSIZE(class_name)) > 0 &&
            (_wcsicmp(class_name, L"PasswordBox") == 0 ||
             _wcsicmp(class_name, L"CredentialEdit") == 0)) {
            return true;
        }
    }
    return false;
}

bool ReadDefaultEnglishMode() {
    return GetRuntimeConfig().english_default;
}

} // namespace

TextService::TextService() {
    DllAddRef();
}

TextService::~TextService() {
    RollbackActivation();
    ClearCompositionState();
    candidate_window_.SetSelectionHandler({});
    candidate_window_.Destroy();
    SharedStatusUi::Unbind(this);
    if (status_ui_acquired_) {
        SharedStatusUi::Release(status_ui_thread_id_);
        status_ui_acquired_ = false;
        status_ui_thread_id_ = 0;
    }
    if (engine_ != nullptr) {
        SharedEngine::Release();
        engine_ = nullptr;
    }
    SafeRelease(&composition_);
    SafeRelease(&edit_context_);
    SafeRelease(&thread_mgr_);
    DllRelease();
}

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_INVALIDARG;
    }
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextInputProcessor) ||
        IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
        *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
        *ppvObj = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
        *ppvObj = static_cast<ITfKeyEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfTextEditSink)) {
        *ppvObj = static_cast<ITfTextEditSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfTextLayoutSink)) {
        *ppvObj = static_cast<ITfTextLayoutSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
        *ppvObj = static_cast<ITfCompositionSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider)) {
        *ppvObj = static_cast<ITfDisplayAttributeProvider*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() {
    return InterlockedIncrement(&ref_);
}

bool TextService::TryAddRefForUiCallback() noexcept {
    LONG observed = InterlockedCompareExchange(&ref_, 0, 0);
    while (observed > 0) {
        const LONG desired = observed + 1;
        const LONG previous = InterlockedCompareExchange(&ref_, desired, observed);
        if (previous == observed) {
            return true;
        }
        observed = previous;
    }
    return false;
}

STDMETHODIMP_(ULONG) TextService::Release() {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) {
        // 必须在析构开始前撤下回调目标。否则 UI 线程可能从 ref=0 的对象上
        // AddRef，将已经进入析构的 TextService 错误“复活”。
        SharedStatusUi::Unbind(this);
        delete this;
    }
    return static_cast<ULONG>(v);
}

HRESULT TextService::InitEngine() {
    // 已拿到共享引擎指针即可；词库可能仍在后台加载，不阻塞 Activate
    if (engine_ != nullptr) {
        return S_OK;
    }
    // 候选窗/状态栏延后到首次焦点或组字，Activate 只拿引擎指针
    engine_ = SharedEngine::Acquire(GetLexiconDirectory());
    if (engine_ == nullptr) {
        SHURU_LOG_ERROR("shared engine acquire failed");
        return E_FAIL;
    }
    if (!engine_->IsReady()) {
        SHURU_LOG_INFO("shared engine loading in background");
    } else {
        if (!engine_->ReloadCustomPhrases()) {
            SHURU_LOG_WARN("custom phrase reload failed; retaining previous snapshot");
        }
        shuangpin_mode_ = (engine_->GetInputSchema() == InputSchema::ShuangpinXiaohe);
        const RuntimeConfig config = GetRuntimeConfig();
        QueryOptions options = engine_->GetQueryOptions();
        options.schema = config.shuangpin_xiaohe ? InputSchema::ShuangpinXiaohe : InputSchema::Quanpin;
        options.fuzzy_enabled = config.fuzzy_enabled;
        options.fuzzy_config.initials = config.fuzzy_initials;
        options.fuzzy_config.finals = config.fuzzy_finals;
        options.fuzzy_config.missing_vowel = config.fuzzy_missing_vowel;
        engine_->SetQueryOptions(options);
        shuangpin_mode_ = config.shuangpin_xiaohe;
    }
    return S_OK;
}

void TextService::EnsureUiWindows() {
    if (!candidate_window_.Create(g_module)) {
        SHURU_LOG_WARN("candidate window create failed");
    } else {
        candidate_window_.SetSelectionHandler([this](size_t index) {
            OnCandidateSelected(index);
        });
    }
    if (!status_ui_acquired_) {
        DWORD owner_thread_id = 0;
        if (!SharedStatusUi::Acquire(g_module, &owner_thread_id)) {
            SHURU_LOG_WARN("shared status ui acquire failed");
        } else {
            status_ui_acquired_ = true;
            status_ui_thread_id_ = owner_thread_id;
        }
    }
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* ptim, TfClientId tid) {
    return ActivateEx(ptim, tid, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD /*dwFlags*/) {
    if (ptim == nullptr || tid == TF_CLIENTID_NULL) return E_INVALIDARG;
    if (!activation_state_.Empty()) return TF_E_ALREADY_EXISTS;

    owner_thread_id_ = GetCurrentThreadId();
    // Settings are process-cached; re-read on every TSF activation so switching away/back
    // applies an atomically replaced settings file without restarting the host process.
    ReloadRuntimeConfig();
    thread_mgr_ = ptim;
    thread_mgr_->AddRef();
    activation_state_.Add(ActivationResource::ThreadManager);
    client_id_ = tid;
    english_mode_ = ReadDefaultEnglishMode();
    punctuation_state_.Reset();
    recent_committed_text_.clear();

    HRESULT hr = InitEngine();
    if (FAILED(hr)) goto failed;
    activation_state_.Add(ActivationResource::Engine);
    display_atom_ = RegisterDisplayAttributeAtom();

    hr = AdviseThreadMgrEventSink();
    if (FAILED(hr)) goto failed;
    activation_state_.Add(ActivationResource::ThreadSink);

    hr = AdviseKeyEventSink();
    if (FAILED(hr)) goto failed;
    key_sink_advised_ = true;
    activation_state_.Add(ActivationResource::KeySink);

    ITfDocumentMgr* doc = nullptr;
    hr = thread_mgr_->GetFocus(&doc);
    if (SUCCEEDED(hr) && doc != nullptr) {
        hr = AdviseTextEditSink(doc);
        doc->Release();
        if (FAILED(hr)) goto failed;
        activation_state_.Add(ActivationResource::TextSink);
    } else if (FAILED(hr)) {
        goto failed;
    }

    SHURU_LOG_INFO("TextService activated, client_id=%u", static_cast<unsigned>(client_id_));
    return S_OK;

failed:
    SHURU_LOG_ERROR("TextService activation rolled back: 0x%08X", hr);
    RollbackActivation();
    return hr;
}

bool TextService::IsOwnerThread() const noexcept {
    return owner_thread_id_ == 0 || owner_thread_id_ == GetCurrentThreadId();
}

void TextService::RollbackActivation() noexcept {
    shift_tap_.Reset();
    if (IsOwnerThread()) {
        EndComposition();
        candidate_window_.Hide();
        SharedStatusUi::HideSoftKeyboard();
        SharedStatusUi::Unbind(this);
    }
    ClearCompositionState();
    UnadviseTextEditSink();
    if (key_sink_advised_) UnadviseKeyEventSink();
    key_sink_advised_ = false;
    UnadviseThreadMgrEventSink();
    SafeRelease(&composition_);
    SafeRelease(&edit_context_);
    SafeRelease(&thread_mgr_);
    if (engine_ != nullptr) {
        SharedEngine::Release();
        engine_ = nullptr;
    }
    if (status_ui_acquired_ && IsOwnerThread()) {
        SharedStatusUi::Release(status_ui_thread_id_);
        status_ui_acquired_ = false;
        status_ui_thread_id_ = 0;
    }
    client_id_ = TF_CLIENTID_NULL;
    display_atom_ = TF_INVALID_GUIDATOM;
    owner_thread_id_ = 0;
    activation_state_.Clear();
}

STDMETHODIMP TextService::Deactivate() {
    if (!IsOwnerThread()) return RPC_E_WRONG_THREAD;
    RollbackActivation();
    SHURU_LOG_INFO("TextService deactivated");
    return S_OK;
}

HRESULT TextService::AdviseThreadMgrEventSink() {
    ITfSource* source = nullptr;
    HRESULT hr = thread_mgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
    if (FAILED(hr)) {
        return hr;
    }
    hr = source->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this), &thread_mgr_cookie_);
    source->Release();
    return hr;
}

HRESULT TextService::UnadviseThreadMgrEventSink() {
    if (thread_mgr_ == nullptr || thread_mgr_cookie_ == TF_INVALID_COOKIE) {
        return S_OK;
    }
    ITfSource* source = nullptr;
    HRESULT hr = thread_mgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
    if (SUCCEEDED(hr)) {
        source->UnadviseSink(thread_mgr_cookie_);
        source->Release();
    }
    thread_mgr_cookie_ = TF_INVALID_COOKIE;
    return S_OK;
}

HRESULT TextService::AdviseKeyEventSink() {
    ITfKeystrokeMgr* key_mgr = nullptr;
    HRESULT hr = thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&key_mgr));
    if (FAILED(hr)) {
        return hr;
    }
    hr = key_mgr->AdviseKeyEventSink(client_id_, static_cast<ITfKeyEventSink*>(this), TRUE);
    key_mgr->Release();
    return hr;
}

HRESULT TextService::UnadviseKeyEventSink() {
    if (thread_mgr_ == nullptr || !key_sink_advised_) {
        return S_OK;
    }
    ITfKeystrokeMgr* key_mgr = nullptr;
    HRESULT hr = thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&key_mgr));
    if (SUCCEEDED(hr)) {
        key_mgr->UnadviseKeyEventSink(client_id_);
        key_mgr->Release();
    }
    key_sink_advised_ = false;
    return S_OK;
}

HRESULT TextService::AdviseTextEditSink(ITfDocumentMgr* doc_mgr) {
    UnadviseTextEditSink();
    if (doc_mgr == nullptr) {
        return S_OK;
    }

    ITfContext* context = nullptr;
    HRESULT hr = doc_mgr->GetTop(&context);
    if (FAILED(hr) || context == nullptr) {
        return hr;
    }

    ITfSource* source = nullptr;
    hr = context->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
    if (FAILED(hr)) {
        context->Release();
        return hr;
    }

    hr = source->AdviseSink(
        IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this), &text_edit_cookie_);
    if (SUCCEEDED(hr)) {
        const HRESULT layout_hr = source->AdviseSink(
            IID_ITfTextLayoutSink,
            static_cast<ITfTextLayoutSink*>(this),
            &text_layout_cookie_);
        if (FAILED(layout_hr)) {
            text_layout_cookie_ = TF_INVALID_COOKIE;
            SHURU_LOG_WARN("text layout sink unavailable: 0x%08X", layout_hr);
        }
    }
    source->Release();
    if (SUCCEEDED(hr)) {
        edit_context_ = context;
    } else {
        context->Release();
    }
    return hr;
}

HRESULT TextService::UnadviseTextEditSink() {
    if (edit_context_ == nullptr) {
        SafeRelease(&edit_context_);
        text_edit_cookie_ = TF_INVALID_COOKIE;
        text_layout_cookie_ = TF_INVALID_COOKIE;
        return S_OK;
    }
    ITfSource* source = nullptr;
    if (SUCCEEDED(edit_context_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source)))) {
        if (text_layout_cookie_ != TF_INVALID_COOKIE) source->UnadviseSink(text_layout_cookie_);
        if (text_edit_cookie_ != TF_INVALID_COOKIE) source->UnadviseSink(text_edit_cookie_);
        source->Release();
    }
    text_edit_cookie_ = TF_INVALID_COOKIE;
    text_layout_cookie_ = TF_INVALID_COOKIE;
    SafeRelease(&edit_context_);
    return S_OK;
}

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr* /*pdim*/) { return S_OK; }
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr* /*pdim*/) { return S_OK; }

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* /*pdimPrevFocus*/) {
    shift_tap_.Reset();
    // 文档切换时先结束旧上下文中的组合，避免旧拼音残留到新文档。
    EndComposition();
    ClearCompositionState();
    candidate_window_.Hide();
    AdviseTextEditSink(pdimFocus);
    if (pdimFocus == nullptr) {
        candidate_window_.Hide();
    }
    return S_OK;
}

STDMETHODIMP TextService::OnPushContext(ITfContext* /*pic*/) { return S_OK; }
STDMETHODIMP TextService::OnPopContext(ITfContext* /*pic*/) { return S_OK; }

STDMETHODIMP TextService::OnSetFocus(BOOL fForeground) {
    if (fForeground) {
        EnsureUiWindows();
        SharedStatusUi::Bind(this);
        SyncStatusUi();
        SharedStatusUi::Hide();
    } else {
        shift_tap_.Reset();
        SharedStatusUi::Unbind(this);
        candidate_window_.Hide();
        SharedStatusUi::HideSoftKeyboard();
    }
    return S_OK;
}

bool TextService::IsAsciiPrintable(WPARAM wparam) {
    return wparam >= 0x20 && wparam <= 0x7E;
}

bool TextService::IsPasswordContext(ITfContext* context) const {
    if (context == nullptr) {
        return false;
    }

    InputScopePrivacy privacy = InputScopePrivacy::Unknown;
    if (client_id_ != TF_CLIENTID_NULL) {
        auto* session = new (std::nothrow) GetInputScopeEditSession(context, &privacy);
        if (session != nullptr) {
            HRESULT session_result = E_FAIL;
            const HRESULT request_result = context->RequestEditSession(
                client_id_, session, TF_ES_SYNC | TF_ES_READ, &session_result);
            session->Release();
            if (SUCCEEDED(request_result) && SUCCEEDED(session_result) &&
                privacy != InputScopePrivacy::Unknown) {
                return privacy == InputScopePrivacy::Sensitive;
            }
        }
    }

    // 老式 Win32 控件可能不公开输入范围，此时再用窗口样式和类名兜底。
    ITfContextView* view = nullptr;
    if (FAILED(context->GetActiveView(&view)) || view == nullptr) {
        return false;
    }
    HWND target = nullptr;
    view->GetWnd(&target);
    view->Release();
    return IsPasswordWindow(target);
}

bool TextService::CommitCandidate(ITfContext* context, const Candidate& candidate) {
    const CandidateCommitPlan plan = PlanCandidateCommit(composing_pinyin_, candidate);
    if (!plan.has_coverage) return false;
    const HRESULT hr = CommitText(context, plan.committed);
    if (FAILED(hr)) return false;
    if (candidate.learnable && engine_ != nullptr && engine_->IsReady() &&
        !IsPasswordContext(context))
        engine_->Learn(
            plan.learned_pinyin.empty()
                ? engine_->ToFullPinyinForLearn(plan.learned_input)
                : plan.learned_pinyin,
            candidate.text);
    composing_pinyin_ = plan.remaining;
    candidate_state_ = {}; current_result_ = {};
    if (composing_pinyin_.empty()) { candidate_window_.Hide(); return true; }
    if (FAILED(SetCompositionString(context, PinyinToWide(composing_pinyin_)))) {
        ClearCompositionState(); candidate_window_.Hide(); return true;
    }
    RefreshCandidates(); UpdateCandidateWindow(context); return true;
}

void TextService::OnCandidateSelected(size_t index) {
    if (edit_context_ == nullptr || index >= current_result_.candidates.size()) return;
    if (!CommitCandidate(edit_context_, current_result_.candidates[index]))
        SHURU_LOG_WARN("mouse candidate commit failed or zero coverage");
}

bool TextService::IsKeyEaten(ITfContext* context, WPARAM wparam) const {
    if (IsPasswordContext(context)) {
        return false;
    }
    // Ctrl+Space：中英切换，仍由 IME 吃掉
    if (wparam == VK_SPACE && IsCtrlOnlyDown()) {
        return true;
    }
    if (english_mode_) {
        return false;
    }
    // Ctrl+A / Ctrl+C / Alt+* / Win+* 等快捷键一律不拦截
    if (HasShortcutModifier()) {
        return false;
    }
    if (!composing_pinyin_.empty()) {
        const bool calculator = IsCalculatorInput(composing_pinyin_);
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (calculator && DecideCalculatorKey(
                wparam, shift, (GetKeyState(VK_NUMLOCK) & 1) != 0).handled) {
            return true;
        }
        if (PinyinEngine::IsPinyinLetter(static_cast<wchar_t>(wparam)) ||
            wparam == VK_BACK || wparam == VK_SPACE || wparam == VK_ESCAPE ||
            wparam == VK_RETURN || wparam == VK_LEFT || wparam == VK_RIGHT ||
            wparam == VK_UP || wparam == VK_DOWN || wparam == VK_PRIOR || wparam == VK_NEXT ||
            wparam == VK_OEM_MINUS || wparam == VK_OEM_PLUS ||
            wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD) {
            return true;
        }
        // 主键盘 1-9 选词；数字小键盘始终输入 ASCII 数字，绝不选词。
        if (wparam >= '1' && wparam <= '9') {
            const int number = static_cast<int>(wparam - '1');
            return engine_ == nullptr || !engine_->IsReady() ||
                   candidate_state_.IsSelectableSlot(static_cast<size_t>(number));
        }
        if (IsNumpadDigit(wparam) || wparam == VK_DECIMAL || wparam == VK_DIVIDE ||
            wparam == VK_MULTIPLY || wparam == VK_ADD || wparam == VK_SUBTRACT) {
            return !IsNumpadDigit(wparam) || (GetKeyState(VK_NUMLOCK) & 1) != 0;
        }
    }
    if (wparam == VK_F9 || wparam == VK_F10) {
        return wparam == VK_F9 || !composing_pinyin_.empty();
    }
    wchar_t punctuation = 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (TryMapChinesePunctuation(wparam, shift, &punctuation) || wparam == VK_OEM_7) {
        return true;
    }
    return PinyinEngine::IsPinyinLetter(static_cast<wchar_t>(wparam));
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (pfEaten == nullptr) {
        return E_INVALIDARG;
    }
    if (IsPasswordContext(pic)) {
        shift_tap_.Reset();
        *pfEaten = FALSE;
        return S_OK;
    }
    if (IsShiftKey(wParam)) {
        // 单独按 Shift：预备在 KeyUp 切换中/英；与其它键组合则取消
        // bit30=1 表示按键重复，不重新武装
        const bool repeated = (lParam & (1 << 30)) != 0;
        *pfEaten = shift_tap_.Begin(
            !composing_pinyin_.empty(), HasShortcutModifier(), repeated) ? TRUE : FALSE;
        return S_OK;
    }
    // Shift+字母/方向键等：当作修饰，不切换中英
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0) {
        shift_tap_.CancelAction();
    }
    *pfEaten = IsKeyEaten(pic, wParam) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) {
    if (pfEaten == nullptr) {
        return E_INVALIDARG;
    }
    if (IsShiftKey(wParam)) {
        const ShiftTapRelease release = shift_tap_.TestKeyUp(IsPasswordContext(pic));
        if (release.action == ShiftTapAction::ToggleEnglishMode) ToggleEnglishMode();
        *pfEaten = release.eaten ? TRUE : FALSE;
        return S_OK;
    }
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) {
    if (pfEaten == nullptr) {
        return E_INVALIDARG;
    }
    if (IsShiftKey(wParam)) {
        const ShiftTapRelease release = shift_tap_.KeyUp(
            !composing_pinyin_.empty(), IsPasswordContext(pic));
        if (release.action == ShiftTapAction::CommitRawComposition) {
            const bool committed = CommitRawComposition(pic);
            if (!committed) SHURU_LOG_WARN("Shift raw composition commit failed");
            *pfEaten = release.eaten ? TRUE : FALSE;
            return S_OK;
        }
        if (release.action == ShiftTapAction::ToggleEnglishMode) ToggleEnglishMode();
        *pfEaten = release.eaten ? TRUE : FALSE;
        return S_OK;
    }
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext* /*pic*/, REFGUID /*rguid*/, BOOL* pfEaten) {
    if (pfEaten == nullptr) {
        return E_INVALIDARG;
    }
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) {
    SHURU_LOG_INFO("OnKeyDown wParam=%u", static_cast<unsigned>(wParam));
    if (pfEaten == nullptr) {
        return E_INVALIDARG;
    }
    bool eaten = false;
    HandleKeyDown(pic, wParam, &eaten);
    *pfEaten = eaten ? TRUE : FALSE;
    return S_OK;
}

bool TextService::HandleKeyDown(ITfContext* context, WPARAM wparam, bool* eaten) {
    if (eaten == nullptr) {
        return false;
    }
    *eaten = false;

    if (IsPasswordContext(context)) {
        shift_tap_.Reset();
        if (!composing_pinyin_.empty()) {
            const HRESULT hr = EndComposition();
            if (SUCCEEDED(hr)) {
                ClearCompositionState();
                candidate_window_.Hide();
            } else {
                SHURU_LOG_WARN("password transition cancel failed: 0x%08X", hr);
            }
        }
        return true;
    }

    // Shift 本身不在 KeyDown 切换；若收到其它键则取消「单独 Shift」
    if (IsShiftKey(wparam)) {
        *eaten = shift_tap_.ShouldEatKeyUp();
        return true;
    }
    shift_tap_.CancelAction();

    // Ctrl+A/C/V 等：绝不当拼音字母处理
    if (HasShortcutModifier()) {
        if (wparam == VK_SPACE && IsCtrlOnlyDown()) {
            // 落到下方中英切换
        } else {
            *eaten = false;
            return true;
        }
    }

    if (wparam == VK_SPACE && IsCtrlOnlyDown()) {
        ToggleEnglishMode();
        *eaten = true;
        return true;
    }

    if (english_mode_) {
        *eaten = false;
        return true;
    }

    if (engine_ == nullptr || !engine_->IsReady()) {
        InitEngine();
    }

    // Apostrophe is a hard pinyin boundary while composing (xi'an != xian).
    const bool shift_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (IsCalculatorInput(composing_pinyin_)) {
        const bool num_lock = (GetKeyState(VK_NUMLOCK) & 1) != 0;
        const CalculatorKeyDecision calculator_key = DecideCalculatorKey(
            wparam, shift_down, num_lock);
        if (calculator_key.handled) {
            if (composing_pinyin_.size() >= 65) {
                *eaten = true;
                return true;
            }
            composing_pinyin_.push_back(calculator_key.character);
            candidate_state_ = {};
            const HRESULT hr = SetCompositionString(
                context, PinyinToWide(composing_pinyin_));
            if (FAILED(hr)) {
                composing_pinyin_.pop_back();
                RefreshCandidates();
                return true;
            }
            RefreshCandidates();
            UpdateCandidateWindow(context);
            *eaten = true;
            return true;
        }
    }
    if (wparam == VK_OEM_7 && !shift_down && !composing_pinyin_.empty()) {
        composing_pinyin_.push_back('\'');
        candidate_state_.selected = 0;
        candidate_state_.page = 0;
        if (SUCCEEDED(SetCompositionString(context, PinyinToWide(composing_pinyin_)))) {
            RefreshCandidates();
            UpdateCandidateWindow(context);
            *eaten = true;
        } else {
            composing_pinyin_.pop_back();
            RefreshCandidates();
        }
        return true;
    }

    // 组合期间这些键属于候选分页。必须先于中文标点处理，否则标点状态机
    // 会抢先提交文本，使配置声明的翻页键永远不可达。
    const CandidatePagingDirection paging = GetCandidatePagingDirection(wparam, shift_down);
    if (!composing_pinyin_.empty() && paging != CandidatePagingDirection::None) {
        if (paging == CandidatePagingDirection::Previous)
            candidate_state_.PreviousPage();
        else
            candidate_state_.NextPage();
        SyncCandidateWindowCandidates();
        UpdateCandidateWindow(context);
        *eaten = true;
        return true;
    }

    // 中文标点状态机。URL/邮箱和小数点上下文保留半角字符。

    const RuntimeConfig config = GetRuntimeConfig();
    const std::wstring punctuation = punctuation_state_.Translate(
        wparam, shift_down, config.full_width_punctuation, recent_committed_text_);
    if (!punctuation.empty()) {
        std::wstring text;
        if (!composing_pinyin_.empty()) {
            if (engine_ != nullptr && engine_->IsReady() && !current_result_.candidates.empty()) {
                const size_t index = candidate_state_.selected < current_result_.candidates.size()
                    ? candidate_state_.selected : 0;
                const Candidate& candidate = current_result_.candidates[index];
                text = candidate.text;
                if (candidate.learnable) {
                    engine_->Learn(
                        candidate.pinyin.empty() ? engine_->ToFullPinyinForLearn(composing_pinyin_) : candidate.pinyin,
                        candidate.text);
                }
            } else {
                text = PinyinToWide(composing_pinyin_);
            }
        }
        text += punctuation;
        const HRESULT hr = CommitText(context, text);
        if (SUCCEEDED(hr)) {
            ClearCompositionState();
            candidate_window_.Hide();
            *eaten = true;
        }
        return true;
    }

    // Escape 清空
    if (wparam == VK_ESCAPE && !composing_pinyin_.empty()) {
        const HRESULT hr = EndComposition();
        if (SUCCEEDED(hr)) {
            ClearCompositionState();
            candidate_window_.Hide();
            *eaten = true;
        }
        return true;
    }

    // Backspace
    if (wparam == VK_BACK && !composing_pinyin_.empty()) {
        const std::string previous = composing_pinyin_;
        composing_pinyin_.pop_back();
        candidate_state_.selected = 0;
        candidate_state_.page = 0;
        if (composing_pinyin_.empty()) {
            const HRESULT hr = EndComposition();
            if (SUCCEEDED(hr)) {
                ClearCompositionState();
                candidate_window_.Hide();
            } else {
                composing_pinyin_ = previous;
                *eaten = false;
                return true;
            }
        } else {
            const HRESULT hr = SetCompositionString(context, PinyinToWide(composing_pinyin_));
            if (FAILED(hr)) {
                composing_pinyin_ = previous;
                RefreshCandidates();
                *eaten = false;
                return true;
            }
            RefreshCandidates();
            UpdateCandidateWindow(context);
        }
        *eaten = true;
        return true;
    }

    // NumPad 从不选词。NumLock 关闭时数字键保留导航语义；运算键始终
    // 以 ASCII 提交。组合中统一提交原始拼音 + 键值，策略清晰且不学习。
    const bool num_lock = (GetKeyState(VK_NUMLOCK) & 1) != 0;
    const NumpadDecision numpad = DecideNumpadKey(wparam, num_lock, composing_pinyin_);
    if (numpad.handled) {
        const HRESULT hr = CommitText(context, numpad.text);
        if (SUCCEEDED(hr)) {
            ClearCompositionState();
            candidate_window_.Hide();
            *eaten = true;
        }
        return true;
    }

    // 仅主键盘数字 1-9 选词。
    int number = -1;
    if (wparam >= '1' && wparam <= '9') {
        number = static_cast<int>(wparam - '1');
    }
    if (number >= 0 && !composing_pinyin_.empty()) {
        if (engine_ == nullptr || !engine_->IsReady()) {
            std::wstring raw = PinyinToWide(composing_pinyin_);
            raw.push_back(static_cast<wchar_t>('1' + number));
            const HRESULT hr = CommitText(context, raw);
            if (SUCCEEDED(hr)) {
                ClearCompositionState();
                candidate_window_.Hide();
                *eaten = true;
            }
            return true;
        }
        const size_t global_index = candidate_state_.GlobalIndex(static_cast<size_t>(number));
        if (candidate_state_.IsSelectableSlot(static_cast<size_t>(number)) &&
            global_index < current_result_.candidates.size()) {
            const auto& cand = current_result_.candidates[global_index];
            if (CommitCandidate(context, cand)) *eaten = true;
            return true;
        }
    }

    // 空格上屏首选
    if (wparam == VK_SPACE && !composing_pinyin_.empty()) {
        if (engine_ == nullptr || !engine_->IsReady()) {
            const HRESULT hr = CommitText(context, PinyinToWide(composing_pinyin_) + L" ");
            if (SUCCEEDED(hr)) {
                ClearCompositionState();
                candidate_window_.Hide();
                *eaten = true;
            }
            return true;
        }
        HRESULT hr = E_FAIL;
        if (!current_result_.candidates.empty()) {
            const auto& cand = current_result_.candidates[
                candidate_state_.selected < current_result_.candidates.size()
                    ? candidate_state_.selected : 0];
            if (CommitCandidate(context, cand)) hr = S_OK;
        } else {
            hr = CommitText(context, PinyinToWide(composing_pinyin_));
        }
        if (SUCCEEDED(hr)) *eaten = true;
        return true;
    }

    // 计算器 Enter 上屏结果；其它组合仍上屏原文。单独 Shift 始终走原文路径。
    if (wparam == VK_RETURN && !composing_pinyin_.empty()) {
        if (IsCalculatorInput(composing_pinyin_) && !current_result_.candidates.empty()) {
            const size_t index = candidate_state_.selected < current_result_.candidates.size()
                ? candidate_state_.selected : 0;
            *eaten = CommitCandidate(context, current_result_.candidates[index]);
        } else {
            *eaten = CommitRawComposition(context);
        }
        return true;
    }

    // 左右切换候选
    if (!composing_pinyin_.empty() && (wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_UP || wparam == VK_DOWN)) {
        if (!current_result_.candidates.empty()) {
            if (wparam == VK_LEFT || wparam == VK_UP) {
                candidate_state_.MovePrevious();
            } else {
                candidate_state_.MoveNext();
            }
            // 同步完整候选内容和当前页，跨页选择时窗口立即滚动。
            SyncCandidateWindowCandidates();
            UpdateCandidateWindow(context);
            *eaten = true;
        }
        return true;
    }

    // F9：软键盘
    if (wparam == VK_F9) {
        ToggleSoftKeyboard();
        *eaten = true;
        return true;
    }

    // F10：全拼 / 小鹤双拼
    if (wparam == VK_F10 && !composing_pinyin_.empty()) {
        OnStatusToggleSchema();
        *eaten = true;
        return true;
    }

    // 字母输入
    if (PinyinEngine::IsPinyinLetter(static_cast<wchar_t>(wparam))) {
        char ch = static_cast<char>(wparam);
        // TSF 传入的是虚拟键码（始终为大写 A-Z）；Shift 按下时保留
        // 大写，作为中英混输的原样片段。
        if (ch >= 'A' && ch <= 'Z' && !shift_down) {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        *eaten = true;
        composing_pinyin_.push_back(ch);
        const HRESULT hr = SetCompositionString(context, PinyinToWide(composing_pinyin_));
        if (FAILED(hr)) {
            // 缓冲已经由输入法接管。保留完整内容供后续按键重试，避免宿主
            // 暂时拒绝首次编辑会话时首字母被直接上屏或丢失。
            RefreshCandidates();
            SHURU_LOG_WARN("SetCompositionString rejected key: 0x%08X", hr);
            return true;
        }
        RefreshCandidates();
        UpdateCandidateWindow(context);
        *eaten = true;
        return true;
    }

    *eaten = false;
    return true;
}

void TextService::RefreshCandidates() {
    EnsureUiWindows();

    current_result_ = {};
    candidate_state_.selected = 0;
    candidate_state_.page = 0;

    std::wstring comp = PinyinToWide(composing_pinyin_);
    if (engine_ == nullptr) {
        InitEngine();
    }

    if (engine_ == nullptr) {
        candidate_display_ = comp;
        SyncCandidateWindowCandidates();
        return;
    }

    if (!engine_->IsReady()) {
        // 加载中：组合串旁提示，不提供可上屏假候选，避免空格打出提示文案
        if (!comp.empty()) {
            comp += L"  · 词库加载中…";
        }
        candidate_display_ = comp;
        SyncCandidateWindowCandidates();
        return;
    }

    const RuntimeConfig runtime = GetRuntimeConfig();
    QueryOptions options = engine_->GetQueryOptions();
    options.schema = runtime.shuangpin_xiaohe ? InputSchema::ShuangpinXiaohe : InputSchema::Quanpin;
    options.fuzzy_enabled = runtime.fuzzy_enabled;
    options.fuzzy_config.initials = runtime.fuzzy_initials;
    options.fuzzy_config.finals = runtime.fuzzy_finals;
    options.fuzzy_config.missing_vowel = runtime.fuzzy_missing_vowel;
    engine_->SetQueryOptions(options);
    shuangpin_mode_ = runtime.shuangpin_xiaohe;
    const size_t candidate_count = static_cast<size_t>(runtime.candidate_count);
    current_result_ = engine_->Query(composing_pinyin_, candidate_count * 10, options);
    candidate_state_.total = current_result_.candidates.size();
    candidate_state_.page_size = candidate_count;
    candidate_state_.Clamp();
    std::wstring disp = engine_->FormatComposingDisplay(composing_pinyin_);
    if (disp.empty()) {
        disp = PinyinToWide(composing_pinyin_);
    }
    candidate_display_ = disp;
    SyncCandidateWindowCandidates();
}

void TextService::SyncCandidateWindowCandidates() {
    candidate_state_.total = current_result_.candidates.size();
    candidate_state_.Clamp();
    candidate_window_.SetContent(
        candidate_display_,
        current_result_.candidates,
        candidate_state_.selected,
        candidate_state_.page,
        candidate_state_.PageSize());
}


bool TextService::GetCaretScreenRect(ITfContext* context, RECT* rect) {
    if (rect == nullptr) {
        return false;
    }
    *rect = {};

    // 1) 只读 edit session + 合法 cookie 的 GetTextExt（多应用更稳）
    if (context != nullptr && client_id_ != TF_CLIENTID_NULL) {
        bool ok = false;
        RECT rc {};
        auto* session = new (std::nothrow) GetTextExtEditSession(context, composition_, &rc, &ok);
        if (session != nullptr) {
            HRESULT hr_session = S_OK;
            const HRESULT hr = context->RequestEditSession(
                client_id_, session, TF_ES_SYNC | TF_ES_READ, &hr_session);
            session->Release();
            if (SUCCEEDED(hr) && ok && IsReliableCandidateRect(rc)) {
                *rect = rc;
                return true;
            }
        }
    }

    HWND target = nullptr;
    if (context != nullptr) {
        ITfContextView* view = nullptr;
        if (SUCCEEDED(context->GetActiveView(&view)) && view != nullptr) {
            view->GetWnd(&target);
            view->Release();
        }
    }

    if (target == nullptr) {
        target = GetForegroundWindow();
    }

    // 3) GUITHREADINFO 光标
    if (target != nullptr) {
        DWORD tid = GetWindowThreadProcessId(target, nullptr);
        GUITHREADINFO gi {};
        gi.cbSize = sizeof(gi);
        if (GetGUIThreadInfo(tid, &gi)) {
            if (gi.hwndCaret != nullptr) {
                RECT rc = gi.rcCaret;
                if (IsReliableCandidateRect(rc)) {
                    MapWindowPoints(gi.hwndCaret, nullptr, reinterpret_cast<POINT*>(&rc), 2);
                    *rect = rc;
                    return true;
                }
            }
        }

    }

    // No reliable insertion rectangle yet. Keep the candidate hidden and retry
    // after the asynchronous edit/layout notification instead of flashing at (0,0).
    return false;
}

void TextService::UpdateCandidateWindow(ITfContext* context) {
    RECT rc {};
    if (!GetCaretScreenRect(context, &rc)) {
        if (!candidate_window_.IsVisible() || !has_last_candidate_rect_) {
            candidate_window_.Hide();
            return;
        }
        rc = last_candidate_rect_;
    } else {
        last_candidate_rect_ = rc;
        has_last_candidate_rect_ = true;
    }
    int gap = 6;
    HWND fg = GetForegroundWindow();
    if (fg) {
        const UINT dpi = GetDpiForWindow(fg);
        if (dpi > 0) {
            gap = MulDiv(6, static_cast<int>(dpi), 96);
        }
    }
    POINT pt {rc.left, (rc.bottom > rc.top ? rc.bottom : rc.top) + gap};
    // CandidateWindow::Show 使用实际布局尺寸和所在显示器工作区定位，无需在此
    // 用固定预留像素重复估算。
    candidate_window_.Show(pt);
    if (SharedStatusUi::IsSoftKeyboardVisible()) {
        const SIZE candidate_size = candidate_window_.WindowSize();
        const POINT candidate_pos = candidate_window_.ScreenPosition();
        POINT sk {candidate_pos.x, candidate_pos.y + candidate_size.cy + gap};
        SharedStatusUi::ShowSoftKeyboard(sk);
    }
}

void TextService::ClearCompositionState() {
    composing_pinyin_.clear();
    candidate_state_ = {};
    current_result_ = {};
    candidate_display_.clear();
    has_last_candidate_rect_ = false;
}

HRESULT TextService::EndComposition() {
    if (composition_ == nullptr || edit_context_ == nullptr) {
        if (composition_ != nullptr) {
            SHURU_LOG_WARN("composition released without edit context");
        }
        SafeRelease(&composition_);
        return S_OK;
    }
    auto* session = new (std::nothrow) EndCompositionEditSession(&composition_);
    if (session == nullptr) {
        return E_OUTOFMEMORY;
    }
    HRESULT hr_session = S_OK;
    const HRESULT hr = edit_context_->RequestEditSession(client_id_, session, TF_ES_SYNC | TF_ES_READWRITE, &hr_session);
    session->Release();
    return SUCCEEDED(hr) ? hr_session : hr;
}

HRESULT TextService::CommitText(ITfContext* context, const std::wstring& text) {
    if (context == nullptr) {
        SHURU_LOG_ERROR("CommitText context is null");
        return E_FAIL;
    }
    auto* session = new (std::nothrow) InsertTextEditSession(context, client_id_, &composition_, text);
    if (session == nullptr) {
        return E_OUTOFMEMORY;
    }
    HRESULT hr_session = S_OK;
    const HRESULT hr = context->RequestEditSession(client_id_, session, TF_ES_SYNC | TF_ES_READWRITE, &hr_session);
    session->Release();
    const HRESULT final_hr = SUCCEEDED(hr) ? hr_session : hr;
    if (SUCCEEDED(final_hr) && !IsPasswordContext(context)) {
        recent_committed_text_ += text;
        if (recent_committed_text_.size() > 128) {
            recent_committed_text_.erase(0, recent_committed_text_.size() - 128);
        }
        candidate_window_.SetTypingStats(typing_stats_.Record(text));
    }
    SHURU_LOG_INFO("CommitText req=0x%08X session=0x%08X len=%u", hr, hr_session, static_cast<unsigned>(text.size()));
    return final_hr;
}

bool TextService::CommitRawComposition(ITfContext* context) {
    if (context == nullptr || composing_pinyin_.empty()) return false;
    const std::wstring raw = PinyinToWide(composing_pinyin_);
    const HRESULT hr = CommitText(context, raw);
    if (FAILED(hr)) return false;
    ClearCompositionState();
    candidate_window_.Hide();
    return true;
}

HRESULT TextService::SetCompositionString(ITfContext* context, const std::wstring& text) {
    if (context == nullptr) {
        return E_FAIL;
    }
    auto* session = new (std::nothrow) SetCompositionEditSession(
        context, client_id_, static_cast<ITfCompositionSink*>(this),
        &composition_, text, display_atom_);
    if (session == nullptr) {
        return E_OUTOFMEMORY;
    }
    HRESULT hr_session = S_OK;
    composition_edit_in_progress_ = true;
    const HRESULT hr = context->RequestEditSession(client_id_, session, TF_ES_SYNC | TF_ES_READWRITE, &hr_session);
    composition_edit_in_progress_ = false;
    session->Release();
    return SUCCEEDED(hr) ? hr_session : hr;
}

STDMETHODIMP TextService::OnEndEdit(ITfContext* pic, TfEditCookie /*ecReadOnly*/, ITfEditRecord* /*pEditRecord*/) {
    if (!composition_edit_in_progress_ && !composing_pinyin_.empty() && pic == edit_context_)
        UpdateCandidateWindow(pic);
    return S_OK;
}

STDMETHODIMP TextService::OnLayoutChange(
    ITfContext* pic, TfLayoutCode lcode, ITfContextView* /*view*/) {
    if (pic != edit_context_ || composing_pinyin_.empty()) return S_OK;
    if (composition_edit_in_progress_) return S_OK;
    if (lcode == TF_LC_DESTROY) {
        candidate_window_.Hide();
        has_last_candidate_rect_ = false;
        return S_OK;
    }
    // 首字布局以及后续滚动、重排都会触发此通知。宿主排版完成后，
    // 重新读取权威的 TSF 光标矩形。
    UpdateCandidateWindow(pic);
    return S_OK;
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie /*ecWrite*/, ITfComposition* /*pComposition*/) {
    SafeRelease(&composition_);
    ClearCompositionState();
    candidate_window_.Hide();
    return S_OK;
}


STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum) {
        return E_INVALIDARG;
    }
    *ppEnum = new (std::nothrow) shuru::EnumDisplayAttributeInfo();
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) {
    if (!ppInfo) {
        return E_INVALIDARG;
    }
    *ppInfo = nullptr;
    if (!IsEqualGUID(guid, GUID_ShuruDisplayAttribute)) {
        return E_INVALIDARG;
    }
    *ppInfo = new (std::nothrow) shuru::DisplayAttributeInfo();
    return *ppInfo ? S_OK : E_OUTOFMEMORY;
}

void TextService::SyncStatusUi() {
    SharedStatusUi::SyncFrom(english_mode_, shuangpin_mode_);
}

void TextService::ToggleSoftKeyboard() {
    RECT rc {};
    POINT pt {};
    if (GetCaretScreenRect(edit_context_, &rc)) {
        pt = POINT {rc.left, rc.bottom > rc.top ? rc.bottom : rc.top};
    } else {
        GetCursorPos(&pt);
    }
    SharedStatusUi::ToggleSoftKeyboard(pt);
    SyncStatusUi();
}

void TextService::ToggleEnglishMode() {
    english_mode_ = !english_mode_;
    candidate_window_.SetEnglishMode(english_mode_);
    if (english_mode_) {
        ClearCompositionState();
        EndComposition();
        candidate_window_.Hide();
    }
    SyncStatusUi();
    SHURU_LOG_INFO("english_mode=%d (shift/toggle)", english_mode_ ? 1 : 0);
}

void TextService::OnStatusToggleEnglish() {
    ToggleEnglishMode();
}

void TextService::OnStatusToggleSchema() {
    if (engine_ == nullptr) {
        InitEngine();
    }
    if (engine_ == nullptr) {
        return;
    }
    const InputSchema cur = engine_->GetInputSchema();
    const InputSchema next = (cur == InputSchema::Quanpin)
                                 ? InputSchema::ShuangpinXiaohe
                                 : InputSchema::Quanpin;
    engine_->SetInputSchema(next);
    engine_->SetFuzzyEnabled(true);
    shuangpin_mode_ = (next == InputSchema::ShuangpinXiaohe);
    ClearCompositionState();
    EndComposition();
    candidate_window_.Hide();
    SyncStatusUi();
    SHURU_LOG_INFO("schema -> %s", shuangpin_mode_ ? "shuangpin-xiaohe" : "quanpin");
}

void TextService::OnStatusToggleKeyboard() {
    ToggleSoftKeyboard();
}

void TextService::OnStatusSoftKey(wchar_t ch, bool is_special) {
    OnSoftKey(ch, is_special);
}

void TextService::SendVirtualKey(WORD vk) {
    INPUT inputs[2] {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void TextService::OnSoftKey(wchar_t ch, bool is_special) {
    if (is_special) {
        if (ch == L'S') {
            SendVirtualKey(VK_SPACE);
        } else if (ch == L'B') {
            SendVirtualKey(VK_BACK);
        } else if (ch == L'E') {
            SendVirtualKey(VK_ESCAPE);
        }
        return;
    }
    if (ch >= L'a' && ch <= L'z') {
        SendVirtualKey(static_cast<WORD>('A' + (ch - L'a')));
    }
}


} // namespace shuru
