#include "ime/ui/shared_status_ui.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

constexpr UINT kStopThread = WM_APP + 0x71;

struct UiThreadContext {
    HINSTANCE instance = nullptr;
    HANDLE ready = nullptr;
    std::atomic<bool> ok {false};
    std::atomic<DWORD> owner_thread_id {0};
};

void PumpMessagesUntilStopped() {
    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kStopThread) {
            break;
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

    context->owner_thread_id.store(owner_thread_id, std::memory_order_release);
    context->ok.store(owner_matches, std::memory_order_release);
    SetEvent(context->ready);

    if (owner_matches) {
        PumpMessagesUntilStopped();
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
    if (instance == nullptr || ready_a == nullptr || ready_b == nullptr) {
        std::fwprintf(stderr, L"测试初始化失败\n");
        if (ready_a) CloseHandle(ready_a);
        if (ready_b) CloseHandle(ready_b);
        return 1;
    }

    UiThreadContext context_a {instance, ready_a};
    UiThreadContext context_b {instance, ready_b};
    std::thread thread_a(RunUiThread, &context_a);
    std::thread thread_b(RunUiThread, &context_b);

    const bool ready = WaitReady(ready_a) && WaitReady(ready_b);
    const DWORD thread_a_id = context_a.owner_thread_id.load(std::memory_order_acquire);
    const DWORD thread_b_id = context_b.owner_thread_id.load(std::memory_order_acquire);
    const bool isolated = context_a.ok.load(std::memory_order_acquire) &&
                          context_b.ok.load(std::memory_order_acquire) &&
                          thread_a_id != 0 && thread_b_id != 0 && thread_a_id != thread_b_id &&
                          shuru::SharedStatusUi::RefCount() == 2;

    // 从非所有者线程释放 A，验证释放任务会回到窗口所有者线程执行。
    if (ready && thread_a_id != 0) {
        shuru::SharedStatusUi::Release(thread_a_id);
    }
    const ULONGLONG retire_deadline = GetTickCount64() + 5000;
    while (shuru::SharedStatusUi::RefCount() != 1 && GetTickCount64() < retire_deadline) {
        Sleep(1);
    }
    const bool cross_thread_release_ok = shuru::SharedStatusUi::RefCount() == 1;

    if (thread_a_id != 0) PostThreadMessageW(thread_a_id, kStopThread, 0, 0);
    if (thread_b_id != 0) PostThreadMessageW(thread_b_id, kStopThread, 0, 0);
    thread_a.join();
    thread_b.join();

    CloseHandle(ready_a);
    CloseHandle(ready_b);

    const bool released = shuru::SharedStatusUi::RefCount() == 0;
    if (!ready || !isolated || !cross_thread_release_ok || !released) {
        std::fwprintf(stderr,
                      L"SharedStatusUi 线程生命周期失败 ready=%d isolated=%d cross=%d released=%d ref=%ld\n",
                      ready ? 1 : 0,
                      isolated ? 1 : 0,
                      cross_thread_release_ok ? 1 : 0,
                      released ? 1 : 0,
                      shuru::SharedStatusUi::RefCount());
        return 1;
    }

    std::wprintf(L"SharedStatusUi 双线程隔离与跨线程释放通过\n");
    return 0;
}
