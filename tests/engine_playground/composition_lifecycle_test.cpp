#include "common/guid_def.h"
#include "common/typing_stats.h"
#include "ime/globals.h"
#include "ime/edit_sessions.h"
#include "ime/display_attribute.h"
#include "ime/text_service.h"
#include "../tsf_test_thread_manager.h"

#include <Windows.h>
#include <msctf.h>
#include <wrl/client.h>

#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
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
using shuru::test::TestThreadManager;

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
        if (++calls_ == 1) {
            ComPtr<ITfRange> range;
            BOOL empty = FALSE;
            if (SUCCEEDED(composition_->GetRange(&range)) &&
                SUCCEEDED(range->IsEmpty(cookie, &empty))) {
                committed_text_outside_composition_ = empty != FALSE;
            }
            return TF_E_SYNCHRONOUS;
        }
        return composition_->EndComposition(cookie);
    }
    int calls() const { return calls_; }
    bool committed_text_outside_composition() const {
        return committed_text_outside_composition_;
    }
private:
    LONG refs_ = 1;
    int calls_ = 0;
    bool committed_text_outside_composition_ = false;
    ComPtr<ITfComposition> composition_;
};

class CallbackCompositionSink final : public ITfCompositionSink {
public:
    explicit CallbackCompositionSink(std::function<HRESULT(TfEditCookie)> callback)
        : callback_(std::move(callback)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITfCompositionSink) return E_NOINTERFACE;
        *object = static_cast<ITfCompositionSink*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refs_);
        if (count == 0) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie cookie, ITfComposition*) override {
        ++calls_;
        return callback_(cookie);
    }
    int calls() const { return calls_; }
private:
    LONG refs_ = 1;
    int calls_ = 0;
    std::function<HRESULT(TfEditCookie)> callback_;
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

struct AdoptionOutcome {
    bool complete = false;
    shuru::ExistingTextCompositionResult result = shuru::ExistingTextCompositionResult::Failed;
    std::wstring text;
};

class DeferredActions {
public:
    shuru::AdoptExistingTextEditSession::DeferAction Scheduler() const {
        return [queue = queue_](std::function<void()> action) {
            queue->push_back(std::move(action));
            return true;
        };
    }
    void Drain() {
        while (!queue_->empty()) {
            auto action = std::move(queue_->front());
            queue_->pop_front();
            action();
        }
    }
private:
    std::shared_ptr<std::deque<std::function<void()>>> queue_ =
        std::make_shared<std::deque<std::function<void()>>>();
};

void Press(shuru::TextService* service, ITfContext* context, WPARAM key) {
    BOOL eaten = FALSE;
    Check(service->OnTestKeyDown(context, key, 1, &eaten), "test key");
    Require(eaten != FALSE, "test key was not accepted");
    Check(service->OnKeyDown(context, key, 1, &eaten), "key down");
    Require(eaten != FALSE, "key was not accepted");
}

enum class HandoffCase { TextChanged, SelectionChanged, RangeTooWide, StaleRequest };

// 将异步授锁提前到当前消息内，复现宿主收尾尚在消息队列中时新写会话已经执行的交错。
class EarlyGrantContext final : public ITfContext {
public:
    explicit EarlyGrantContext(ITfContext* context) : context_(context) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_INVALIDARG;
        if (iid == IID_IUnknown || iid == IID_ITfContext) {
            *object = static_cast<ITfContext*>(this);
            AddRef();
            return S_OK;
        }
        return context_->QueryInterface(iid, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refs_);
        if (!count) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId client, ITfEditSession* session,
                                                 DWORD flags, HRESULT* result) override {
        if (flags & TF_ES_ASYNC) flags = (flags & ~TF_ES_ASYNC) | TF_ES_SYNC;
        return context_->RequestEditSession(client, session, flags, result);
    }
    HRESULT STDMETHODCALLTYPE InWriteSession(TfClientId client, BOOL* value) override { return context_->InWriteSession(client, value); }
    HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie cookie, ULONG index, ULONG count, TF_SELECTION* values, ULONG* fetched) override { return context_->GetSelection(cookie, index, count, values, fetched); }
    HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie cookie, ULONG count, const TF_SELECTION* values) override { return context_->SetSelection(cookie, count, values); }
    HRESULT STDMETHODCALLTYPE GetStart(TfEditCookie cookie, ITfRange** value) override { return context_->GetStart(cookie, value); }
    HRESULT STDMETHODCALLTYPE GetEnd(TfEditCookie cookie, ITfRange** value) override { return context_->GetEnd(cookie, value); }
    HRESULT STDMETHODCALLTYPE GetActiveView(ITfContextView** value) override { return context_->GetActiveView(value); }
    HRESULT STDMETHODCALLTYPE EnumViews(IEnumTfContextViews** value) override { return context_->EnumViews(value); }
    HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS* value) override { return context_->GetStatus(value); }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID guid, ITfProperty** value) override { return context_->GetProperty(guid, value); }
    HRESULT STDMETHODCALLTYPE GetAppProperty(REFGUID guid, ITfReadOnlyProperty** value) override { return context_->GetAppProperty(guid, value); }
    HRESULT STDMETHODCALLTYPE TrackProperties(const GUID** properties, ULONG count, const GUID** app_properties, ULONG app_count, ITfReadOnlyProperty** value) override { return context_->TrackProperties(properties, count, app_properties, app_count, value); }
    HRESULT STDMETHODCALLTYPE EnumProperties(IEnumTfProperties** value) override { return context_->EnumProperties(value); }
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** value) override { return context_->GetDocumentMgr(value); }
    HRESULT STDMETHODCALLTYPE CreateRangeBackup(TfEditCookie cookie, ITfRange* range, ITfRangeBackup** value) override { return context_->CreateRangeBackup(cookie, range, value); }
private:
    LONG refs_ = 1;
    ComPtr<ITfContext> context_;
};

void CheckDelayedHostFinalization(ITfContext* context, ITfContextComposition* compositions,
                                  TfClientId client) {
    const std::wstring before = ReadText(context, client);
    DeferredActions deferred;
    auto schedule = deferred.Scheduler();
    bool finalization_ran = false;
    unsigned new_terminations = 0;
    ComPtr<ITfContextOwnerCompositionServices> owner_services;
    Check(context->QueryInterface(IID_PPV_ARGS(&owner_services)), "delayed finalization services");
    ComPtr<CallbackCompositionSink> old_sink;
    old_sink.Attach(new CallbackCompositionSink([&](TfEditCookie) {
        // 模拟 TSF One：原结束调用已返回，但其适配器在随后一轮消息里才完成收尾。
        schedule([&] {
            finalization_ran = true;
            if (CompositionCount(compositions) != 0)
                Check(owner_services->TerminateComposition(nullptr), "delayed host finalization");
        });
        return S_OK;
    }));
    ComPtr<CallbackCompositionSink> new_sink;
    new_sink.Attach(new CallbackCompositionSink([&](TfEditCookie) {
        ++new_terminations;
        return S_OK;
    }));
    ComPtr<ITfRange> letters;
    ComPtr<ITfComposition> old_composition;
    RunSession(context, client, [&](TfEditCookie cookie) {
        ComPtr<ITfRange> caret;
        Check(context->GetEnd(cookie, &caret), "delayed finalization caret");
        TF_SELECTION selection{caret.Get(), {TF_AE_END, FALSE}};
        Check(context->SetSelection(cookie, 1, &selection), "delayed finalization selection");
        ComPtr<ITfInsertAtSelection> insertion;
        Check(context->QueryInterface(IID_PPV_ARGS(&insertion)), "delayed finalization insertion");
        Check(insertion->InsertTextAtSelection(cookie, 0, L"ni", 2, &letters), "delayed finalization text");
        Check(compositions->StartComposition(cookie, letters.Get(), old_sink.Get(), &old_composition),
              "delayed finalization original composition");
        Check(letters->Clone(&caret), "delayed finalization end");
        Check(caret->Collapse(cookie, TF_ANCHOR_END), "delayed finalization collapse");
        selection.range = caret.Get();
        return context->SetSelection(cookie, 1, &selection);
    });
    ComPtr<ITfComposition> adopted;
    ComPtr<ITfRange> recovered;
    auto outcome = std::make_shared<AdoptionOutcome>();
    ComPtr<ITfContext> early_grant_context;
    early_grant_context.Attach(new EarlyGrantContext(context));
    ComPtr<ITfEditSession> adoption;
    adoption.Attach(new shuru::AdoptExistingTextEditSession(
        early_grant_context.Get(), new_sink.Get(), letters.Get(), L'n', adopted.GetAddressOf(), recovered.GetAddressOf(),
        shuru::RegisterDisplayAttributeAtom(), [] { return true; },
        [outcome](shuru::ExistingTextCompositionResult result, std::wstring text) {
            outcome->result = result;
            outcome->text = std::move(text);
            outcome->complete = true;
        }, client, schedule));
    RunSession(context, client, [&](TfEditCookie cookie) { return adoption->DoEditSession(cookie); });
    PumpUntil([&] { deferred.Drain(); return finalization_ran && outcome->complete; });
    Require(finalization_ran && outcome->complete && adopted &&
            outcome->result == shuru::ExistingTextCompositionResult::Adopted,
            "delayed host finalization prevented first-key handoff");
    std::printf("delayed host finalization: new_terminations=%u active=%d\n",
                new_terminations, CompositionCount(compositions));
    Require(new_terminations == 0 && CompositionCount(compositions) == 1,
            "host finalization terminated the recovered first-key composition");
    Require(ReadText(context, client) == before + L"ni", "handoff finalization changed existing text");
    RunSession(context, client, [&](TfEditCookie cookie) { return adopted->EndComposition(cookie); });
}

void CheckAdoptionReentry(ITfContext* context, ITfContextComposition* compositions,
                         TfClientId client, ITfCompositionSink* new_sink, HandoffCase scenario) {
    const std::wstring before = ReadText(context, client);
    ComPtr<ITfRange> letters;
    ComPtr<ITfComposition> existing;
    ComPtr<CallbackCompositionSink> old_sink;
    RunSession(context, client, [&](TfEditCookie cookie) {
        ComPtr<ITfRange> caret;
        Check(context->GetEnd(cookie, &caret), "handoff fixture caret");
        TF_SELECTION selection{caret.Get(), {TF_AE_END, FALSE}};
        Check(context->SetSelection(cookie, 1, &selection), "handoff fixture selection");
        ComPtr<ITfInsertAtSelection> insertion;
        Check(context->QueryInterface(IID_PPV_ARGS(&insertion)), "handoff fixture insertion");
        Check(insertion->InsertTextAtSelection(cookie, 0, L"ni", 2, &letters), "handoff fixture text");
        old_sink.Attach(new CallbackCompositionSink([context, range = letters, scenario](TfEditCookie callback_cookie) {
            if (scenario == HandoffCase::TextChanged)
                return range->SetText(callback_cookie, 0, L"xy", 2);
            if (scenario == HandoffCase::SelectionChanged) {
                ComPtr<ITfRange> start;
                const HRESULT hr = context->GetStart(callback_cookie, &start);
                if (FAILED(hr)) return hr;
                TF_SELECTION changed{start.Get(), {TF_AE_END, FALSE}};
                return context->SetSelection(callback_cookie, 1, &changed);
            }
            return S_OK;
        }));
        ComPtr<ITfRange> old_range;
        Check(letters->Clone(&old_range), "handoff fixture old range");
        if (scenario == HandoffCase::RangeTooWide) {
            LONG shifted = 0;
            Check(old_range->ShiftStart(cookie, -1, &shifted, nullptr), "handoff fixture wider range");
        }
        Check(compositions->StartComposition(cookie, old_range.Get(), old_sink.Get(), &existing),
              "handoff fixture old composition");
        Check(letters->Clone(&caret), "handoff fixture final caret");
        Check(caret->Collapse(cookie, TF_ANCHOR_END), "handoff fixture caret end");
        selection.range = caret.Get();
        return context->SetSelection(cookie, 1, &selection);
    });
    ComPtr<ITfComposition> adopted;
    ComPtr<ITfRange> recovered;
    auto outcome = std::make_shared<AdoptionOutcome>();
    auto allowed = std::make_shared<bool>(true);
    DeferredActions deferred;
    ComPtr<ITfEditSession> adoption;
    adoption.Attach(new shuru::AdoptExistingTextEditSession(
        context, new_sink, letters.Get(), L'n', adopted.GetAddressOf(), recovered.GetAddressOf(),
        shuru::RegisterDisplayAttributeAtom(), [allowed] { return *allowed; },
        [outcome](shuru::ExistingTextCompositionResult result, std::wstring) {
            outcome->result = result;
            outcome->complete = true;
        }, client, deferred.Scheduler()));
    RunSession(context, client, [&](TfEditCookie cookie) { return adoption->DoEditSession(cookie); });
    if (scenario == HandoffCase::StaleRequest) *allowed = false;
    PumpUntil([&] { deferred.Drain(); return outcome->complete; });
    Require(outcome->complete, "first-key handoff did not finish");
    const bool too_wide = scenario == HandoffCase::RangeTooWide;
    const bool untouched = too_wide || scenario == HandoffCase::StaleRequest;
    const auto expected_outcome = too_wide ? shuru::ExistingTextCompositionResult::CompositionActive
        : scenario == HandoffCase::StaleRequest ? shuru::ExistingTextCompositionResult::StaleRequest
        : scenario == HandoffCase::TextChanged ? shuru::ExistingTextCompositionResult::RangeChanged
                                              : shuru::ExistingTextCompositionResult::SelectionChanged;
    Require(outcome->result == expected_outcome && !adopted && !recovered,
            "first-key handoff ignored a changed or out-of-scope context");
    Require(old_sink->calls() == (untouched ? 0 : 1) && CompositionCount(compositions) == (untouched ? 1 : 0),
            "first-key handoff terminated an unrelated composition or created an extra one");
    Require(ReadText(context, client) == before + (scenario == HandoffCase::TextChanged ? L"xy" : L"ni"),
            "first-key handoff changed the host's text");
    if (scenario == HandoffCase::SelectionChanged) {
        RunSession(context, client, [&](TfEditCookie cookie) {
            TF_SELECTION selection{};
            ULONG fetched = 0;
            Check(context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched), "read changed caret");
            ComPtr<ITfRange> selected;
            selected.Attach(selection.range);
            Require(fetched == 1 && selected, "changed caret missing");
            ComPtr<ITfRange> start;
            Check(context->GetStart(cookie, &start), "read document start");
            LONG comparison = 1;
            Check(selected->CompareStart(cookie, start.Get(), TF_ANCHOR_START, &comparison), "compare changed caret");
            Require(comparison == 0, "handoff moved the host's new caret back");
            return S_OK;
        }, TF_ES_SYNC | TF_ES_READ);
    }
    if (untouched)
        RunSession(context, client, [&](TfEditCookie cookie) { return existing->EndComposition(cookie); });
}

void CheckCopyUnexpectedTerminationRecovery(
    ITfContext* context, ITfContextComposition* compositions,
    TfClientId client, shuru::TextService* service) {
    const std::wstring before = ReadText(context, client);

    // 1. 模拟用户按 Ctrl+C 快捷键
    service->ArmShortcutForFirstKeyRecoveryForTest(context, 'C');

    // 2. 模拟用户输入首个字母 'N'
    Press(service, context, 'N');
    Require(ReadText(context, client) == before + L"n", "initial composition letter 'n' mismatch");
    Require(CompositionCount(compositions) == 1, "active composition expected after 'N'");

    // 3. 取得刚刚建立的活动组合对象
    ComPtr<ITfComposition> current_composition;
    RunSession(context, client, [&](TfEditCookie) {
        ComPtr<IEnumITfCompositionView> views;
        Check(compositions->EnumCompositions(&views), "enumerate active composition");
        ComPtr<ITfCompositionView> view;
        ULONG fetched = 0;
        Check(views->Next(1, &view, &fetched), "read active composition view");
        Require(fetched == 1 && view, "missing active composition view");
        return view.As(&current_composition);
    }, TF_ES_SYNC | TF_ES_READ);
    Require(current_composition != nullptr, "failed to get current composition");

    // 4. 模拟宿主延后发起的 FinalizeComposition（结束当前组合并通知输入法）
    RunSession(context, client, [&](TfEditCookie cookie) {
        const HRESULT hr = current_composition->EndComposition(cookie);
        if (FAILED(hr)) return hr;
        return service->OnCompositionTerminated(cookie, current_composition.Get());
    });

    // 5. 消息队列循环，让 PostOwnerThreadAction 派发的自愈重建执行
    PumpUntil([&] { return CompositionCount(compositions) == 1; });
    Require(CompositionCount(compositions) == 1, "composition was not recovered after host termination");
    Require(ReadText(context, client) == before + L"n", "preedit text was lost after host termination");

    // 6. 继续输入字母 'I' 并按空格提交
    Press(service, context, 'I');
    Press(service, context, VK_SPACE);
    Require(ReadText(context, client) == before + L"你", "committed text mismatch after recovered composition");
    Require(CompositionCount(compositions) == 0, "composition remained active after space commit");

    // 7. 测试“复制后按空格再退格，随后输入”正常
    const std::wstring before2 = ReadText(context, client);
    service->ArmShortcutForFirstKeyRecoveryForTest(context, 'C');
    // 宿主输入空格
    RunSession(context, client, [&](TfEditCookie cookie) {
        ComPtr<ITfRange> caret;
        Check(context->GetEnd(cookie, &caret), "read caret for space");
        return caret->SetText(cookie, 0, L" ", 1);
    });
    // 宿主删除空格（退格）
    RunSession(context, client, [&](TfEditCookie cookie) {
        ComPtr<ITfRange> end_range;
        Check(context->GetEnd(cookie, &end_range), "read end for backspace");
        LONG shifted = 0;
        Check(end_range->ShiftStart(cookie, -1, &shifted, nullptr), "shift start for backspace");
        return end_range->SetText(cookie, 0, nullptr, 0);
    });
    // 随后正常输入 'N' + 'I' + 空格
    Press(service, context, 'N');
    Press(service, context, 'I');
    Press(service, context, VK_SPACE);
    Require(ReadText(context, client) == before2 + L"你", "text mismatch after copy-space-backspace sequence");
    Require(CompositionCount(compositions) == 0, "composition active after copy-space-backspace");

    // 8. 测试“Esc 主动取消时不发生误自愈”
    Press(service, context, 'N');
    Require(CompositionCount(compositions) == 1, "composition active after 'N'");
    Press(service, context, VK_ESCAPE);
    PumpUntil([&] { return CompositionCount(compositions) == 0; });
    Require(CompositionCount(compositions) == 0, "composition active after ESC");
    Require(ReadText(context, client) == before2 + L"你", "text changed after ESC cancellation");

    std::printf("copy-recovery lifecycle: unexpected termination recovery, space-backspace and ESC passed\n");
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
            lexicon << "jiaoyu\t教育\t100\nde\t的\t100\nyingyong\t应用\t100\n"
                       "qianjing\t前景\t100\nruhe\t如何\t100\n";
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
        // 富文本宿主在本轮写锁结束时读取组合范围，不能等到下一轮异步清理才移除正文。
        Require(delayed_end->committed_text_outside_composition(),
                "committed text remained in the composition while finalization was queued");
        commit.Reset();
        Press(service.Get(), context.Get(), 'N');
        PumpUntil([&] { return delayed_end->calls() == 2; });
        Require(delayed_end->calls() == 2 && CompositionCount(context_compositions.Get()) == 1 &&
                ReadText(context.Get(), client) == L"你保留完成n", "deferred commit was duplicated or lost");
        RunSession(context.Get(), client, [&](TfEditCookie cookie) {
            ComPtr<IEnumITfCompositionView> views;
            Check(context_compositions->EnumCompositions(&views), "enumerate following composition");
            ComPtr<ITfCompositionView> view;
            ULONG fetched = 0;
            Check(views->Next(1, &view, &fetched), "read following composition");
            Require(fetched == 1 && view, "following composition is missing");
            ComPtr<ITfRange> range;
            Check(view->GetRange(&range), "following composition range");
            ComPtr<ITfProperty> property;
            Check(context->GetProperty(GUID_PROP_ATTRIBUTE, &property), "following composition property");
            VARIANT value;
            VariantInit(&value);
            const HRESULT hr = property->GetValue(cookie, range.Get(), &value);
            const bool has_attribute = SUCCEEDED(hr) && value.vt == VT_I4;
            VariantClear(&value);
            Require(has_attribute, "old finalization cleared the following composition display attribute");
            return hr;
        }, TF_ES_SYNC | TF_ES_READ);
        Press(service.Get(), context.Get(), 'I');
        Press(service.Get(), context.Get(), VK_SPACE);
        Require(ReadText(context.Get(), client) == L"你保留完成你" &&
                CompositionCount(context_compositions.Get()) == 0,
                "delayed cleanup terminated the following input");

        std::wstring expected_text = L"你保留完成你";
        const struct { const char* pinyin; const wchar_t* text; } words[] = {
            {"JIAOYU", L"教育"}, {"DE", L"的"}, {"YINGYONG", L"应用"},
            {"QIANJING", L"前景"}, {"RUHE", L"如何"},
        };
        for (const auto& word : words) {
            for (const char* key = word.pinyin; *key; ++key)
                Press(service.Get(), context.Get(), static_cast<WPARAM>(*key));
            Press(service.Get(), context.Get(), VK_SPACE);
            expected_text += word.text;
            Require(ReadText(context.Get(), client) == expected_text &&
                    CompositionCount(context_compositions.Get()) == 0,
                    "successive space commits repeated a previously committed prefix");
        }

        // 复制后的首键恢复可能接管宿主只覆盖尾字母的组合。扩展范围后，
        // 所有接管的字母都必须带同一显示属性，不能继续使用扩展前的范围快照。
        ComPtr<ITfRange> fallen_letters;
        ComPtr<ITfComposition> host_composition;
        RunSession(context.Get(), client, [&](TfEditCookie cookie) {
            ComPtr<ITfInsertAtSelection> insertion;
            Check(context.As(&insertion), "first-key insertion interface");
            Check(insertion->InsertTextAtSelection(cookie, 0, L"ni", 2, &fallen_letters),
                  "insert fallen first-key letters");
            ComPtr<ITfRange> tail;
            Check(fallen_letters->Clone(&tail), "clone host composition tail");
            LONG shifted = 0;
            Check(tail->ShiftStart(cookie, 1, &shifted, nullptr), "host composition tail start");
            Check(context_compositions->StartComposition(cookie, tail.Get(), service.Get(), &host_composition),
                  "start host tail composition");
            ComPtr<ITfRange> caret;
            Check(fallen_letters->Clone(&caret), "clone first-key caret");
            Check(caret->Collapse(cookie, TF_ANCHOR_END), "collapse first-key caret");
            TF_SELECTION selection{caret.Get(), {TF_AE_END, FALSE}};
            return context->SetSelection(cookie, 1, &selection);
        });
        ComPtr<ITfClientId> client_ids;
        Check(manager.As(&client_ids), "first-key client identifiers");
        TfClientId handoff_client = TF_CLIENTID_NULL;
        Check(client_ids->GetClientId(CLSID_ShuruTextService, &handoff_client), "first-key service client identifier");
        Require(handoff_client != TF_CLIENTID_NULL && handoff_client != client,
                "handoff test must use distinct host and input-service clients");
        // TSF 范围绑定到取得它的编辑客户端，输入法通过自己的只读会话取得通知范围。
        ComPtr<ITfRange> service_letters;
        RunSession(context.Get(), handoff_client, [&](TfEditCookie cookie) {
            Check(context->GetEnd(cookie, &service_letters), "first-key service text end");
            LONG shifted = 0;
            Check(service_letters->ShiftStart(cookie, -2, &shifted, nullptr), "first-key service text start");
            Require(shifted == -2, "first-key service range incomplete");
            return S_OK;
        }, TF_ES_SYNC | TF_ES_READ);
        const TfGuidAtom display_atom = shuru::RegisterDisplayAttributeAtom();
        Require(display_atom != TF_INVALID_GUIDATOM, "first-key display atom unavailable");
        ComPtr<ITfComposition> adopted;
        ComPtr<ITfRange> recovered_start;
        auto adoption_outcome = std::make_shared<AdoptionOutcome>();
        shuru::CandidateWindow recovery_dispatcher;
        Require(recovery_dispatcher.Create(GetModuleHandleW(nullptr)), "create first-key dispatcher window");
        ComPtr<ITfEditSession> adoption;
        adoption.Attach(new shuru::AdoptExistingTextEditSession(
            context.Get(), service.Get(), service_letters.Get(), L'n', adopted.GetAddressOf(),
            recovered_start.GetAddressOf(), display_atom, [] { return true; },
            [adoption_outcome](shuru::ExistingTextCompositionResult status, std::wstring text) {
                adoption_outcome->result = status;
                adoption_outcome->text = std::move(text);
                adoption_outcome->complete = true;
            }, handoff_client, [&recovery_dispatcher](std::function<void()> action) {
                return recovery_dispatcher.PostOwnerThreadAction(std::move(action));
            }));
        RunSession(context.Get(), handoff_client, [&](TfEditCookie cookie) { return adoption->DoEditSession(cookie); });
        HWND recovery_owner = CreateWindowExW(0, L"STATIC", L"first-key-recovery-owner", WS_OVERLAPPED,
            0, 0, 120, 80, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(recovery_owner != nullptr, "create first-key dispatcher owner");
        // 首次候选展示绑定宿主并重建 HWND，不能丢掉尚未执行的首键交接。
        recovery_dispatcher.Show(POINT{40, 40}, recovery_owner);
        PumpUntil([&] { return adoption_outcome->complete; });
        Require(adoption_outcome->complete, "owner rebinding discarded the pending first-key handoff");
        std::printf("first-key recovery: status=%d chars=%zu active_compositions=%d\n",
                    static_cast<int>(adoption_outcome->result), adoption_outcome->text.size(), CompositionCount(context_compositions.Get()));
        Require(adopted && adoption_outcome->text == L"ni" && CompositionCount(context_compositions.Get()) == 1,
                "first-key recovery did not take over the host composition");
        RunSession(context.Get(), handoff_client, [&](TfEditCookie cookie) {
            ComPtr<ITfRange> range;
            Check(adopted->GetRange(&range), "read adopted composition range");
            for (const GUID* property_id : {&GUID_PROP_COMPOSING, &GUID_PROP_ATTRIBUTE}) {
                ComPtr<ITfProperty> property;
                Check(context->GetProperty(*property_id, &property), "read adopted composition property");
                VARIANT value;
                VariantInit(&value);
                const HRESULT hr = property->GetValue(cookie, range.Get(), &value);
                const LONG expected = *property_id == GUID_PROP_COMPOSING ? TRUE : static_cast<LONG>(display_atom);
                const bool covered = SUCCEEDED(hr) && value.vt == VT_I4 && value.lVal == expected;
                VariantClear(&value);
                Require(covered, "first-key recovery left part of the composition without its style or composing property");
            }
            return S_OK;
        }, TF_ES_SYNC | TF_ES_READ);
        ComPtr<ITfEditSession> recovered_commit;
        recovered_commit.Attach(new shuru::InsertTextEditSession(
            context.Get(), handoff_client, adopted.GetAddressOf(), L"你", recovered_start.Get(), L"ni"));
        RunSession(context.Get(), handoff_client, [&](TfEditCookie cookie) { return recovered_commit->DoEditSession(cookie); });
        Require(ReadText(context.Get(), client) == expected_text + L"你" &&
                CompositionCount(context_compositions.Get()) == 0,
                "recovered styled composition did not commit cleanly");
        RunSession(context.Get(), handoff_client, [&](TfEditCookie cookie) {
            for (const GUID* property_id : {&GUID_PROP_COMPOSING, &GUID_PROP_ATTRIBUTE}) {
                ComPtr<ITfProperty> property;
                Check(context->GetProperty(*property_id, &property), "read committed first-key property");
                VARIANT value;
                VariantInit(&value);
                const HRESULT hr = property->GetValue(cookie, service_letters.Get(), &value);
                const bool cleared = SUCCEEDED(hr) && (value.vt == VT_EMPTY || (value.vt == VT_I4 && value.lVal == 0));
                VariantClear(&value);
                Require(cleared, "first-key commit left a composing or display attribute");
            }
            return S_OK;
        }, TF_ES_SYNC | TF_ES_READ);
        DestroyWindow(recovery_owner);
        recovery_dispatcher.Destroy();
        CheckDelayedHostFinalization(context.Get(), context_compositions.Get(), client);
        CheckAdoptionReentry(context.Get(), context_compositions.Get(), client, service.Get(), HandoffCase::TextChanged);
        CheckAdoptionReentry(context.Get(), context_compositions.Get(), client, service.Get(), HandoffCase::SelectionChanged);
        CheckAdoptionReentry(context.Get(), context_compositions.Get(), client, service.Get(), HandoffCase::RangeTooWide);
        CheckAdoptionReentry(context.Get(), context_compositions.Get(), client, service.Get(), HandoffCase::StaleRequest);
        CheckCopyUnexpectedTerminationRecovery(context.Get(), context_compositions.Get(), client, service.Get());
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
