#include "ime/ui/shared_status_ui.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

constexpr UINT kStopThread = WM_APP + 0x71;
constexpr UINT kSyncTrayThread = WM_APP + 0x72;

struct UiThreadContext {
    HINSTANCE instance = nullptr;
    HANDLE ready = nullptr;
    HANDLE sync_done = nullptr;
    std::atomic<bool> ok {false};
    std::atomic<bool> has_tray_icon {false};
    std::atomic<DWORD> owner_thread_id {0};
};

void PumpMessagesUntilStopped(UiThreadContext* context) {
    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kStopThread) {
            break;
        }
        if (message.message == kSyncTrayThread) {
            shuru::SharedStatusUi::SyncFrom(false, false);
            context->has_tray_icon.store(
                shuru::SharedStatusUi::HasTrayIcon(),
                std::memory_order_release);
            SetEvent(context->sync_done);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void RunUiThread(UiThreadContext* context) {
    // 强制创建线程消息队列，保证主线程可用 PostThreadMessage 结束循环。
    MSG queued_message {};
    PeekMessageW(&queued_message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    DWORD owner_thread_id = 0;
    const bool acquired = shuru::SharedStatusUi::Acquire(context->instance, &owner_thread_id);
    const bool owner_matches = acquired && owner_thread_id == GetCurrentThreadId();
    if (owner_matches) {
        shuru::SharedStatusUi::Show();
    }

    HWND status_window = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM output) {
        wchar_t class_name[64] {};
        if (GetClassNameW(window, class_name, ARRAYSIZE(class_name)) > 0 &&
            wcscmp(class_name, L"ShuruStatusWindowClass") == 0) {
            *reinterpret_cast<HWND*>(output) = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&status_window));
    const bool status_hidden = status_window != nullptr && IsWindowVisible(status_window) == FALSE;

    context->owner_thread_id.store(owner_thread_id, std::memory_order_release);
    context->has_tray_icon.store(
        shuru::SharedStatusUi::HasTrayIcon(), std::memory_order_release);
    context->ok.store(owner_matches && status_hidden, std::memory_order_release);
    SetEvent(context->ready);

    if (owner_matches) {
        PumpMessagesUntilStopped(context);
        // A 可能已经由测试主线程跨线程释放；Release 对重复释放为幂等操作。
        shuru::SharedStatusUi::Release(owner_thread_id);
    }
}

bool WaitReady(HANDLE event) {
    return WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
}

}  // namespace

int wmain() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HANDLE ready_a = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ready_b = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE sync_a = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE sync_b = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (instance == nullptr || ready_a == nullptr || ready_b == nullptr ||
        sync_a == nullptr || sync_b == nullptr) {
        std::fwprintf(stderr, L"测试初始化失败\n");
        if (ready_a) CloseHandle(ready_a);
        if (ready_b) CloseHandle(ready_b);
        if (sync_a) CloseHandle(sync_a);
        if (sync_b) CloseHandle(sync_b);
        return 1;
    }

    UiThreadContext context_a {instance, ready_a, sync_a};
    UiThreadContext context_b {instance, ready_b, sync_b};
    std::thread thread_a(RunUiThread, &context_a);
    std::thread thread_b(RunUiThread, &context_b);

    const bool ready = WaitReady(ready_a) && WaitReady(ready_b);
    const DWORD thread_a_id = context_a.owner_thread_id.load(std::memory_order_acquire);
    const DWORD thread_b_id = context_b.owner_thread_id.load(std::memory_order_acquire);
    const bool tray_a = context_a.has_tray_icon.load(std::memory_order_acquire);
    const bool tray_b = context_b.has_tray_icon.load(std::memory_order_acquire);
    const bool isolated = context_a.ok.load(std::memory_order_acquire) &&
                          context_b.ok.load(std::memory_order_acquire) &&
                          thread_a_id != 0 && thread_b_id != 0 && thread_a_id != thread_b_id &&
                          tray_a != tray_b &&
                          shuru::SharedStatusUi::RefCount() == 2;

    const DWORD tray_owner_thread_id = tray_a ? thread_a_id : thread_b_id;
    const DWORD takeover_thread_id = tray_a ? thread_b_id : thread_a_id;
    HANDLE takeover_event = tray_a ? sync_b : sync_a;

    // 从非所有者线程释放实际托盘所有者，验证窗口销毁回到所有者线程，
    // 随后让另一个已有 UI 线程在下一次同步时接管唯一图标。
    if (ready && tray_owner_thread_id != 0) {
        shuru::SharedStatusUi::Release(tray_owner_thread_id);
    }
    const ULONGLONG retire_deadline = GetTickCount64() + 5000;
    while (shuru::SharedStatusUi::RefCount() != 1 && GetTickCount64() < retire_deadline) {
        Sleep(1);
    }
    const bool cross_thread_release_ok = shuru::SharedStatusUi::RefCount() == 1;
    bool tray_handoff_ok = false;
    if (cross_thread_release_ok && takeover_thread_id != 0 &&
        PostThreadMessageW(takeover_thread_id, kSyncTrayThread, 0, 0)) {
        tray_handoff_ok = WaitReady(takeover_event) &&
            (tray_a
                ? context_b.has_tray_icon.load(std::memory_order_acquire)
                : context_a.has_tray_icon.load(std::memory_order_acquire));
    }

    if (thread_a_id != 0) PostThreadMessageW(thread_a_id, kStopThread, 0, 0);
    if (thread_b_id != 0) PostThreadMessageW(thread_b_id, kStopThread, 0, 0);
    thread_a.join();
    thread_b.join();

    CloseHandle(ready_a);
    CloseHandle(ready_b);
    CloseHandle(sync_a);
    CloseHandle(sync_b);

    const bool released = shuru::SharedStatusUi::RefCount() == 0;
    if (!ready || !isolated || !cross_thread_release_ok ||
        !tray_handoff_ok || !released) {
        std::fprintf(stderr,
                      "SharedStatusUi Failed: ready=%d isolated=%d tray=%d/%d cross=%d handoff=%d released=%d ref=%ld\n",
                      ready ? 1 : 0,
                      isolated ? 1 : 0,
                      tray_a ? 1 : 0,
                      tray_b ? 1 : 0,
                      cross_thread_release_ok ? 1 : 0,
                      tray_handoff_ok ? 1 : 0,
                      released ? 1 : 0,
                      shuru::SharedStatusUi::RefCount());
        return 1;
    }

    std::wprintf(L"SharedStatusUi 双线程隔离与跨线程释放通过\n");
    return 0;
}
