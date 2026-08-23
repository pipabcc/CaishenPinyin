// firstkey_sim: 确定性复现"首键绕过输入法直接落字"的接管链路。
//
// 真实故障：窗口切换后立刻快速打字、或 Ctrl+V 粘贴后继续输入时，第一
// 个字母在宿主输入管线内绕过 TSF 按键 sink，直接以原始文本落进文档；
// 后续字母正常组词。这里作为宿主，用应用侧编辑会话模拟"绕过落字"，
// 再驱动后续按键，检验 TextService 的首键接管网是否把字母回收进组词串。
//
// 用法：firstkey_sim <ShuruIme.dll 路径> [mode]
//   mode = onendedit（默认）：落字时触发器新鲜，检验 OnEndEdit 接管网。
//   mode = tight     ：落字后立刻继续按键（无泵间隔），检验缓冲时序。
//   mode = sweep     ：落字时触发器已过期（OnEndEdit 网必然拒绝），随后
//                      用 Ctrl+C 重新武装，检验按键清扫网兜底。
//   mode = paste     ：用 Ctrl+V 粘贴武装触发器（粘贴后首键掉落场景），
//                      先整段写入"粘贴内容"再让首键掉落，随后连打，
//                      检验粘贴武装 + OnEndEdit/清扫接管网全链路。
//   mode = paste_blind：在从未被输入法绑定的上下文上重复 paste 时序，
//                      粘贴与掉落都不产生 OnEndEdit（绑定滞后/失明），
//                      检验清扫网独立兜底。
// 退出码：0=接管成功；3=首键未被接管（复现用户 bug）；其他=环境失败。

#include "common/guid_def.h"

#include <Windows.h>
#include <msctf.h>

#include <cstdio>
#include <cwctype>
#include <string>

namespace {

// ---------- 应用侧原始插入会话：模拟真实应用的 WM_CHAR 落字 ----------
class AppTypeTextSession final : public ITfEditSession {
public:
    AppTypeTextSession(ITfContext* context, const wchar_t* text)
        : context_(context), text_(text) {
        if (context_ != nullptr) context_->AddRef();
    }
    ~AppTypeTextSession() {
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

    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie edit_cookie) override {
        if (context_ == nullptr) return E_FAIL;
        TF_SELECTION selection {};
        ULONG fetched = 0;
        HRESULT hr = context_->GetSelection(
            edit_cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (FAILED(hr) || fetched != 1 || selection.range == nullptr) {
            return FAILED(hr) ? hr : E_FAIL;
        }
        hr = selection.range->SetText(
            edit_cookie, 0, text_, static_cast<ULONG>(wcslen(text_)));
        if (SUCCEEDED(hr)) {
            selection.range->Collapse(edit_cookie, TF_ANCHOR_END);
            hr = context_->SetSelection(edit_cookie, 1, &selection);
        }
        selection.range->Release();
        return hr;
    }

private:
    LONG refs_ = 1;
    ITfContext* context_ = nullptr;
    const wchar_t* text_ = nullptr;
};

bool AppTypeRawInsert(
    ITfContext* context, TfClientId client_id, const wchar_t* text) {
    auto* session = new AppTypeTextSession(context, text);
    HRESULT session_result = E_FAIL;
    const HRESULT request_result = context->RequestEditSession(
        client_id, session, TF_ES_SYNC | TF_ES_READWRITE, &session_result);
    session->Release();
    return SUCCEEDED(request_result) && SUCCEEDED(session_result);
}

// ---------- 读全文会话 ----------
class ReadTextSession final : public ITfEditSession {
public:
    ReadTextSession(ITfContext* context, std::wstring* output)
        : context_(context), output_(output) {
        if (context_ != nullptr) context_->AddRef();
    }
    ~ReadTextSession() {
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

bool ReadAllText(
    ITfContext* context, TfClientId client_id, std::wstring* output) {
    auto* session = new ReadTextSession(context, output);
    HRESULT session_result = E_FAIL;
    const HRESULT request_result = context->RequestEditSession(
        client_id, session, TF_ES_SYNC | TF_ES_READ, &session_result);
    session->Release();
    return SUCCEEDED(request_result) && SUCCEEDED(session_result);
}

// ---------- 消息泵：让 TSF 异步编辑会话（接管会话）有机会运行 ----------
void PumpFor(DWORD milliseconds) {
    const DWORD until = GetTickCount() + milliseconds;
    while (GetTickCount() < until) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjectsEx(
            0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

// 模拟真实 Ctrl+C / Ctrl+V 武装首键接管触发器：物理按住左 Ctrl（让
// GetAsyncKeyState/GetKeyState 如实反映），并按真实键序把 VK_CONTROL 与
// 目标键送进按键 sink（先按 Ctrl 才能驱动 DLL 内的修饰键状态机），
// 再物理释放 Ctrl。宿主进程内窗口激活类事件不会到达，这是与真实路径
// 一致的武装方式。
bool ArmTriggerViaShortcut(
    ITfKeyEventSink* key_sink, ITfContext* context, byte shortcut_key) {
    if (key_sink == nullptr || context == nullptr) return false;
    BOOL eaten = FALSE;
    keybd_event(VK_LCONTROL, 0, 0, 0);
    PumpFor(150);
    HRESULT hr = key_sink->OnTestKeyDown(context, VK_CONTROL, 0, &eaten);
    PumpFor(60);
    if (SUCCEEDED(hr)) {
        hr = key_sink->OnTestKeyDown(context, shortcut_key, 0, &eaten);
        PumpFor(60);
    }
    keybd_event(VK_LCONTROL, 0, KEYEVENTF_KEYUP, 0);
    PumpFor(100);
    if (SUCCEEDED(hr)) {
        (void)key_sink->OnTestKeyUp(context, VK_CONTROL, 0, &eaten);
        (void)key_sink->OnKeyUp(context, VK_CONTROL, 0, &eaten);
    }
    return SUCCEEDED(hr);
}

bool SendKeyRelaxed(
    ITfKeyEventSink* key_sink, ITfContext* context, WPARAM key) {
    BOOL test_eaten = FALSE;
    HRESULT hr = key_sink->OnTestKeyDown(context, key, 0, &test_eaten);
    if (FAILED(hr)) return false;
    BOOL eaten = FALSE;
    hr = key_sink->OnKeyDown(context, key, 0, &eaten);
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
    if (argc < 2) return 2;
    enum class Mode { OnEndEdit, Tight, Sweep, Paste, PasteMouse };
    Mode mode = Mode::OnEndEdit;
    if (argc >= 3) {
        if (wcscmp(argv[2], L"tight") == 0) mode = Mode::Tight;
        else if (wcscmp(argv[2], L"sweep") == 0) mode = Mode::Sweep;
        else if (wcscmp(argv[2], L"paste") == 0) mode = Mode::Paste;
        else if (wcscmp(argv[2], L"paste_mouse") == 0) mode = Mode::PasteMouse;
    }
    const wchar_t fallen =
        (mode == Mode::Paste || mode == Mode::PasteMouse) ? L'c' : L'n';
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) return 1;

    ITfThreadMgr* thread_manager = nullptr;
    TfClientId client_id = TF_CLIENTID_NULL;
    TfClientId service_client_id = TF_CLIENTID_NULL;
    ITfDocumentMgr* document_manager = nullptr;
    ITfContext* body_context = nullptr;
    HMODULE ime_module = nullptr;
    ITfTextInputProcessorEx* text_service = nullptr;
    ITfKeyEventSink* key_sink = nullptr;
    bool text_service_deactivated = false;
    int result = 1;
    const char* failure_stage = "CoCreateInstance(CLSID_TF_ThreadMgr)";

    ITfThreadMgrEx* thread_manager_ex = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgrEx, reinterpret_cast<void**>(&thread_manager_ex));
    if (FAILED(hr) || thread_manager_ex == nullptr) goto cleanup;
    thread_manager = thread_manager_ex;
    failure_stage = "ITfThreadMgrEx::ActivateEx";
    hr = thread_manager_ex->ActivateEx(&client_id, TF_TMAE_UIELEMENTENABLEDONLY);
    if (FAILED(hr)) goto cleanup;
    failure_stage = "PrepareDirectTipActivation";
    hr = PrepareDirectTipActivation(thread_manager, &service_client_id);
    if (FAILED(hr) || service_client_id == TF_CLIENTID_NULL) goto cleanup;
    failure_stage = "ITfThreadMgr::CreateDocumentMgr";
    hr = thread_manager->CreateDocumentMgr(&document_manager);
    if (FAILED(hr) || document_manager == nullptr) goto cleanup;
    failure_stage = "ITfDocumentMgr::CreateContext(body)";
    TfEditCookie body_cookie = TF_INVALID_COOKIE;
    hr = document_manager->CreateContext(
        client_id, 0, nullptr, &body_context, &body_cookie);
    if (FAILED(hr) || body_context == nullptr) goto cleanup;
    failure_stage = "ITfDocumentMgr::Push(body)/ITfThreadMgr::SetFocus";
    if (FAILED(document_manager->Push(body_context)) ||
        FAILED(thread_manager->SetFocus(document_manager))) {
        goto cleanup;
    }

    failure_stage = "LoadLibraryW(ShuruIme.dll)";
    if (GetFileAttributesW(argv[1]) == INVALID_FILE_ATTRIBUTES) goto cleanup;
    ime_module = LoadLibraryW(argv[1]);
    if (ime_module == nullptr) goto cleanup;
    {
        failure_stage = "DllGetClassObject";
        const auto get_class_object = reinterpret_cast<
            HRESULT(STDAPICALLTYPE*)(
                REFCLSID, REFIID, void**)>(GetProcAddress(
                    ime_module, "DllGetClassObject"));
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
    hr = text_service->ActivateEx(thread_manager, service_client_id, 0);
    if (FAILED(hr)) goto cleanup;
    failure_stage = "QueryInterface(IID_ITfKeyEventSink)";
    hr = text_service->QueryInterface(
        IID_ITfKeyEventSink, reinterpret_cast<void**>(&key_sink));
    if (FAILED(hr) || key_sink == nullptr) goto cleanup;

    // 等词库就绪，保证空格提交的是真实候选而不是引擎未就绪的拼音回退，
    // 这样最终文本才能区分"接管成功（你好）"与"接管失败（n+候选）"。
    PumpFor(9000);

    // 武装首键接管触发器（对应真实场景"用户 Ctrl+C / Ctrl+V 后立刻输入"）。
    // 宿主进程内窗口激活类事件不会到达，用真实快捷键路径武装。
    // paste_mouse 模式不武装：还原右键菜单粘贴等无快捷键路径，检验
    // 粘贴编辑自身的 OnEndEdit 观察窗刷新能否独立完成首键接管。
    if (mode == Mode::OnEndEdit || mode == Mode::Tight ||
        mode == Mode::Paste) {
        failure_stage = "Arm trigger (shortcut)";
        const byte arm_key = (mode == Mode::Paste) ? 'V' : 'C';
        if (!ArmTriggerViaShortcut(key_sink, body_context, arm_key)) {
            goto cleanup;
        }
        PumpFor(100);
    }

    // paste/paste_mouse 模式先模拟宿主真实完成粘贴：应用侧整段写入剪贴
    // 板内容。这次写入本身会触发 OnEndEdit；若接管网把"非单字母写入"
    // 当作解除触发器的理由，随后的首键掉落就无人回收——这正是用户实际
    // 遇到的"粘贴后输入 ceshi，c 直接上屏"的时序。
    if (mode == Mode::Paste || mode == Mode::PasteMouse) {
        failure_stage = "AppTypeRawInsert(pasted content)";
        if (!AppTypeRawInsert(
                body_context, client_id, L"粘贴的正文内容。")) {
            goto cleanup;
        }
        PumpFor(120);
    }

    // ---- 阶段 1：模拟首键绕过 —— 应用侧直接插入原始字母 ----
    failure_stage = "AppTypeRawInsert(fallen first letter)";
    if (!AppTypeRawInsert(
            body_context, client_id, std::wstring(1, fallen).c_str())) {
        goto cleanup;
    }

    // ---- 阶段 2：消息泵 / 立即连打 / 触发器过期后重新武装，按模式区分 ----
    if (mode == Mode::OnEndEdit) {
        PumpFor(1200);
    } else if (mode == Mode::Sweep) {
        // OnEndEdit 网此刻必然拒绝（触发器过期）；用 Ctrl+C 重新武装，
        // 检验第二个字母到达按键 sink 时清扫网接管残留字母。
        PumpFor(200);
        failure_stage = "Re-arm trigger after fall (Ctrl+C)";
        if (!ArmTriggerViaShortcut(key_sink, body_context, 'C')) goto cleanup;
        PumpFor(100);
    } else if (mode == Mode::PasteMouse) {
        // 无快捷键武装：稍作停顿还原"右键粘贴后开始打字"的节奏。
        PumpFor(200);
    }

    // ---- 阶段 3：后续按键照常走按键 sink ----
    failure_stage = "SendKey(following letters)";
    {
        // paste 模式还原用户实际输入 ceshi；其余模式输入 nihao。
        const wchar_t rest_paste[] = {L'e', L's', L'h', L'i', L'\0'};
        const wchar_t rest_default[] = {L'i', L'h', L'a', L'o', L'\0'};
        const wchar_t* rest =
            (mode == Mode::Paste || mode == Mode::PasteMouse)
                ? rest_paste : rest_default;
        for (const wchar_t* p = rest; *p != L'\0'; ++p) {
            if (!SendKeyRelaxed(
                    key_sink, body_context,
                    static_cast<WPARAM>(::towupper(*p)))) {
                goto cleanup;
            }
            PumpFor(60);
        }
    }
    PumpFor(400);
    failure_stage = "SendKey(space commit)";
    if (!SendKeyRelaxed(key_sink, body_context, VK_SPACE)) goto cleanup;
    PumpFor(600);

    // ---- 阶段 4：判决 ----
    failure_stage = "ReadAllText(verdict)";
    {
        std::wstring text;
        if (!ReadAllText(body_context, client_id, &text)) goto cleanup;
        std::fwprintf(stdout, L"final text: %ls\n", text.c_str());
        // 隐私注意：这里只打印测试工具自己的固定文本。
        const bool recovered =
            text.find(std::wstring(1, fallen)) == std::wstring::npos;
        if (!recovered) {
            std::fwprintf(
                stdout,
                L"VERDICT: FIRST KEY NOT RECOVERED (raw letter remains)\n");
            result = 3;
        } else {
            std::fwprintf(stdout, L"VERDICT: first key recovered\n");
            result = 0;
        }
    }

    (void)text_service->Deactivate();
    text_service_deactivated = true;

cleanup:
    if (result == 1) {
        std::fprintf(
            stderr, "firstkey_sim failed at %s: hr=0x%08lx win32=%lu\n",
            failure_stage, static_cast<unsigned long>(hr),
            static_cast<unsigned long>(GetLastError()));
    }
    if (key_sink != nullptr) key_sink->Release();
    if (text_service != nullptr) {
        if (!text_service_deactivated) text_service->Deactivate();
        text_service->Release();
    }
    if (document_manager != nullptr) document_manager->Pop(TF_POPF_ALL);
    if (body_context != nullptr) body_context->Release();
    if (document_manager != nullptr) document_manager->Release();
    if (thread_manager != nullptr) {
        if (client_id != TF_CLIENTID_NULL) thread_manager->Deactivate();
        thread_manager->Release();
    }
    CoUninitialize();
    return result;
}
