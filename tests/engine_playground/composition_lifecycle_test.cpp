#include "common/guid_def.h"
#include "common/typing_stats.h"
#include "ime/globals.h"
#include "ime/edit_sessions.h"
#include "ime/text_service.h"

#include <Windows.h>
#include <msctf.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Check(HRESULT result, const char* stage) {
    if (FAILED(result)) {
        std::fprintf(stderr, "%s: 0x%08lx\n", stage, static_cast<unsigned long>(result));
        throw std::runtime_error(stage);
    }
}

// 不绑定物理键盘或激活已安装的输入法；文档、范围和编辑会话仍由 Windows TSF 提供。
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
    ComPtr<ITfThreadMgr> manager_;
};

class CallbackEditSession final : public ITfEditSession {
public:
    explicit CallbackEditSession(std::function<HRESULT(TfEditCookie)> callback)
        : callback_(std::move(callback)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
        *object = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refs_);
        if (count == 0) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        return callback_(cookie);
    }
private:
    LONG refs_ = 1;
    std::function<HRESULT(TfEditCookie)> callback_;
};

class FailFirstEndComposition final : public ITfComposition {
public:
    explicit FailFirstEndComposition(ITfComposition* composition) : composition_(composition) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfComposition) return E_NOINTERFACE;
        *object = static_cast<ITfComposition*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refs_);
        if (count == 0) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE GetRange(ITfRange** range) override { return composition_->GetRange(range); }
    HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie cookie, ITfRange* range) override {
        return composition_->ShiftStart(cookie, range);
    }
    HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie cookie, ITfRange* range) override {
        return composition_->ShiftEnd(cookie, range);
    }
    HRESULT STDMETHODCALLTYPE EndComposition(TfEditCookie cookie) override {
        if (++calls_ == 1) return TF_E_SYNCHRONOUS;
        return composition_->EndComposition(cookie);
    }
    int calls() const { return calls_; }
private:
    LONG refs_ = 1;
    int calls_ = 0;
    ComPtr<ITfComposition> composition_;
};

void RunSession(ITfContext* context, TfClientId client,
                std::function<HRESULT(TfEditCookie)> callback,
                DWORD flags = TF_ES_SYNC | TF_ES_READWRITE) {
    ComPtr<ITfEditSession> session;
    session.Attach(new CallbackEditSession(std::move(callback)));
    HRESULT result = E_FAIL;
    Check(context->RequestEditSession(client, session.Get(), flags, &result), "request edit");
    Check(result, "edit session");
}

std::wstring ReadText(ITfContext* context, TfClientId client) {
    std::wstring text;
    RunSession(context, client, [&](TfEditCookie cookie) {
        ComPtr<ITfRange> range;
        HRESULT hr = context->GetStart(cookie, &range);
        if (FAILED(hr)) return hr;
        LONG shifted = 0;
        hr = range->ShiftEnd(cookie, 256, &shifted, nullptr);
        if (FAILED(hr)) return hr;
        wchar_t buffer[256]{};
        ULONG count = 0;
        hr = range->GetText(cookie, 0, buffer, 256, &count);
        if (SUCCEEDED(hr)) text.assign(buffer, count);
        return hr;
    }, TF_ES_SYNC | TF_ES_READ);
    return text;
}

int CompositionCount(ITfContextComposition* context) {
    ComPtr<IEnumITfCompositionView> views;
    Check(context->EnumCompositions(&views), "enumerate compositions");
    int count = 0;
    for (;;) {
        ComPtr<ITfCompositionView> view;
        ULONG fetched = 0;
        const HRESULT hr = views->Next(1, &view, &fetched);
        Check(hr, "next composition");
        if (fetched == 0) return count;
        ++count;
    }
}

template<class Predicate>
void PumpUntil(Predicate completed) {
    const ULONGLONG deadline = GetTickCount64() + 1000;
    while (!completed() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
}

void Press(shuru::TextService* service, ITfContext* context, WPARAM key) {
    BOOL eaten = FALSE;
    Check(service->OnTestKeyDown(context, key, 1, &eaten), "test key");
    Require(eaten != FALSE, "test key was not accepted");
    Check(service->OnKeyDown(context, key, 1, &eaten), "key down");
    Require(eaten != FALSE, "key was not accepted");
}
}  // namespace

int RunCompositionLifecycleTest() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    const fs::path root = fs::temp_directory_path() /
        (L"caishen-composition-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    wchar_t previous_local[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", previous_local, ARRAYSIZE(previous_local));
    ComPtr<ITfThreadMgrEx> manager;
    ComPtr<ITfThreadMgr> test_manager;
    ComPtr<ITfDocumentMgr> document;
    ComPtr<ITfDocumentMgr> second_document;
    ComPtr<ITfContext> context;
    ComPtr<ITfContext> second_context;
    ComPtr<ITfContextComposition> context_compositions;
    ComPtr<ITfComposition> previous_composition;
    ComPtr<shuru::TextService> service;
    TfClientId client = TF_CLIENTID_NULL;
    bool acquired_engine = false;
    int result = 1;
    try {
        Require(!fs::exists(root), "test directory already exists");
        fs::create_directories(root / L"local" / L"CaishenPinyin");
        fs::create_directories(root / L"lexicon");
        {
            std::ofstream config(root / L"local" / L"CaishenPinyin" / L"settings.ini");
            config << "LearningEnabled=0\nContentLogging=0\nOffline=1\n";
            std::ofstream lexicon(root / L"lexicon" / L"base_dict.txt", std::ios::binary);
            lexicon << "ni\t\xE4\xBD\xA0\t100\nhao\t\xE5\xA5\xBD\t100\n";
        }
        Require(SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"local").c_str()) != FALSE,
                "isolate user directory");
        shuru::g_module = GetModuleHandleW(nullptr);
        Require(shuru::SharedEngine::Acquire((root / L"lexicon").wstring()) != nullptr,
                "create test engine");
        acquired_engine = true;
        Require(shuru::SharedEngine::WaitForReady(5000), "test engine not ready");

        Check(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&manager)), "create thread manager");
        Check(manager->ActivateEx(&client, TF_TMAE_NOACTIVATETIP), "activate thread manager");
        Check(manager->CreateDocumentMgr(&document), "create document");
        TfEditCookie context_cookie = TF_INVALID_COOKIE;
        Check(document->CreateContext(client, 0, nullptr, &context, &context_cookie), "create context");
        Check(document->Push(context.Get()), "push context");
        Check(manager->CreateDocumentMgr(&second_document), "create second document");
        Check(second_document->CreateContext(client, 0, nullptr, &second_context, &context_cookie),
              "create second context");
        Check(second_document->Push(second_context.Get()), "push second context");
        Check(manager->SetFocus(document.Get()), "focus test document");
        Check(context.As(&context_compositions), "context compositions");
        test_manager.Attach(new TestThreadManager(manager.Get()));
        service.Attach(new shuru::TextService());
        Check(service->Activate(test_manager.Get(), client), "activate test service");

        // 保留已结束组合的真实 COM 对象，模拟复制/切换后迟到的旧组合通知。
        RunSession(context.Get(), client, [&](TfEditCookie cookie) {
            ComPtr<ITfRange> insertion;
            HRESULT hr = context->GetEnd(cookie, &insertion);
            if (FAILED(hr)) return hr;
            hr = context_compositions->StartComposition(cookie, insertion.Get(), service.Get(),
                                                         &previous_composition);
            return FAILED(hr) ? hr : previous_composition->EndComposition(cookie);
        });
        Press(service.Get(), context.Get(), 'N');
        Require(ReadText(context.Get(), client) == L"n", "initial composition text mismatch");
        RunSession(context.Get(), client, [&](TfEditCookie cookie) {
            return service->OnCompositionTerminated(cookie, previous_composition.Get());
        });
        Press(service.Get(), context.Get(), 'I');
        const int active = CompositionCount(context_compositions.Get());
        std::printf("active compositions after stale notification: %d\n", active);
        Require(ReadText(context.Get(), client) == L"ni" && active == 1,
                "old notification lost the current composition");
        Press(service.Get(), context.Get(), VK_SPACE);
        Require(ReadText(context.Get(), client) == L"你" &&
                CompositionCount(context_compositions.Get()) == 0,
                "committed text left an orphan composition");

        Press(service.Get(), context.Get(), 'H');
        Press(service.Get(), context.Get(), 'A');
        // 复制/选择变化的通知可能仍处于宿主只读编辑会话，不能同步申请写锁。
        RunSession(context.Get(), client, [&](TfEditCookie) {
            const HRESULT hr = manager->SetFocus(second_document.Get());
            // 无物理焦点的测试线程直接派发通知，保持真实文档的焦点状态一致。
            return FAILED(hr) ? hr : service->OnSetFocus(second_document.Get(), document.Get());
        }, TF_ES_SYNC | TF_ES_READ);
        Press(service.Get(), second_context.Get(), 'N');
        PumpUntil([&] { return CompositionCount(context_compositions.Get()) == 0; });
        const int after_focus = CompositionCount(context_compositions.Get());
        std::printf("active compositions after read-only focus transition: %d\n", after_focus);
        Require(after_focus == 0, "focus transition abandoned an active composition");
        Require(ReadText(context.Get(), client) == L"你", "focus cleanup changed the committed prefix");
        Require(ReadText(second_context.Get(), client) == L"n", "old cleanup changed the new preedit");
        Press(service.Get(), second_context.Get(), 'I');
        Press(service.Get(), second_context.Get(), VK_SPACE);
        Require(ReadText(second_context.Get(), client) == L"你" &&
                ReadText(context.Get(), client) == L"你", "text crossed input contexts");
        Check(manager->SetFocus(document.Get()), "restore first document focus");
        Check(service->OnSetFocus(document.Get(), second_document.Get()), "notify first document focus");

        // 排队结束期间若宿主已改写正文，仅结束旧组合，不删除宿主的新文字。
        ComPtr<ITfComposition> changed_composition;
        RunSession(context.Get(), client, [&](TfEditCookie cookie) {
            ComPtr<ITfRange> range;
            HRESULT hr = context->GetEnd(cookie, &range);
            if (FAILED(hr)) return hr;
            hr = context_compositions->StartComposition(cookie, range.Get(), service.Get(), &changed_composition);
            if (FAILED(hr)) return hr;
            hr = changed_composition->GetRange(&range);
            if (FAILED(hr)) return hr;
            return range->SetText(cookie, 0, L"保留", 2);
        });
        ComPtr<ITfEditSession> cleanup;
        cleanup.Attach(new shuru::EndCompositionEditSession(changed_composition.Get(), L"ha"));
        RunSession(context.Get(), client, [&](TfEditCookie cookie) { return cleanup->DoEditSession(cookie); });
        Require(ReadText(context.Get(), client) == L"你保留" &&
                CompositionCount(context_compositions.Get()) == 0, "cleanup deleted externally changed text");
        cleanup.Reset();
        changed_composition.Reset();

        // 最终文字写入成功但结束暂时失败时，旧对象必须由排队清理保活。
        ComPtr<ITfComposition> pending_commit;
        RunSession(context.Get(), client, [&](TfEditCookie cookie) {
            ComPtr<ITfRange> range;
            HRESULT hr = context->GetEnd(cookie, &range);
            if (FAILED(hr)) return hr;
            return context_compositions->StartComposition(cookie, range.Get(), service.Get(), &pending_commit);
        });
        ComPtr<FailFirstEndComposition> delayed_end;
        delayed_end.Attach(new FailFirstEndComposition(pending_commit.Get()));
        ComPtr<ITfComposition> commit_slot(delayed_end.Get());
        ComPtr<ITfEditSession> commit;
        commit.Attach(new shuru::InsertTextEditSession(
            context.Get(), client, commit_slot.GetAddressOf(), L"完成"));
        RunSession(context.Get(), client, [&](TfEditCookie cookie) { return commit->DoEditSession(cookie); });
        Require(!commit_slot, "committed composition was not retired");
        commit.Reset();
        Press(service.Get(), context.Get(), 'N');
        PumpUntil([&] { return delayed_end->calls() == 2; });
        Require(delayed_end->calls() == 2 && CompositionCount(context_compositions.Get()) == 1 &&
                ReadText(context.Get(), client) == L"你保留完成n", "deferred commit was duplicated or lost");
        Press(service.Get(), context.Get(), 'I');
        Press(service.Get(), context.Get(), VK_SPACE);
        Require(ReadText(context.Get(), client) == L"你保留完成你" &&
                CompositionCount(context_compositions.Get()) == 0,
                "delayed cleanup terminated the following input");
        std::printf("composition lifecycle: stale callback, focus, external text and deferred commit passed\n");
        result = 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "composition lifecycle: %s\n", error.what());
    }
    if (service) service->Deactivate();
    previous_composition.Reset();
    service.Reset();
    if (document) document->Pop(TF_POPF_ALL);
    if (second_document) second_document->Pop(TF_POPF_ALL);
    context_compositions.Reset();
    context.Reset();
    document.Reset();
    second_context.Reset();
    second_document.Reset();
    test_manager.Reset();
    if (manager && client != TF_CLIENTID_NULL) manager->Deactivate();
    manager.Reset();
    if (acquired_engine) shuru::SharedEngine::Release();
    shuru::SharedEngine::Shutdown();
    shuru::TryShutdownAsyncTypingStats();
    SetEnvironmentVariableW(L"LOCALAPPDATA", *previous_local ? previous_local : nullptr);
    CoUninitialize();
    std::error_code error;
    fs::remove_all(root, error);
    return result;
}
