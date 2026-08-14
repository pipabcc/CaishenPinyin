#include "shared_status_ui.h"

#include "status_window.h"
#include "../text_service.h"
#include "../../common/logger.h"
#include "../../common/runtime_config.h"

#include <memory>
#include <new>
#include <unordered_map>
#include <utility>

namespace shuru {
namespace {

constexpr UINT kRetireThreadUiTask = 1;

struct ThreadUiState {
    long refs = 0;
    bool retire_pending = false;
    TextService* host = nullptr;
    std::shared_ptr<StatusWindow> status;
    std::shared_ptr<SoftKeyboardWindow> soft;
};

struct UiSnapshot {
    DWORD owner_thread_id = 0;
    std::shared_ptr<StatusWindow> status;
    std::shared_ptr<SoftKeyboardWindow> soft;
};

CRITICAL_SECTION g_ui_cs;
INIT_ONCE g_ui_once = INIT_ONCE_STATIC_INIT;
std::unordered_map<DWORD, std::shared_ptr<ThreadUiState>> g_thread_ui;
long g_ui_ref = 0;

BOOL CALLBACK InitUiCs(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_ui_cs);
    return TRUE;
}

void EnsureUiCs() {
    InitOnceExecuteOnce(&g_ui_once, InitUiCs, nullptr, nullptr);
}

std::shared_ptr<ThreadUiState> NewThreadUiState() noexcept {
    try {
        return std::make_shared<ThreadUiState>();
    } catch (...) {
        return {};
    }
}

bool InsertThreadUiState(
    DWORD thread_id,
    const std::shared_ptr<ThreadUiState>& state) noexcept {
    try {
        return g_thread_ui.emplace(thread_id, state).second;
    } catch (...) {
        return false;
    }
}

UiSnapshot TakeCurrentThreadSnapshot_NoLock() {
    const auto found = g_thread_ui.find(GetCurrentThreadId());
    if (found == g_thread_ui.end()) {
        return {};
    }
    return UiSnapshot {found->first, found->second->status, found->second->soft};
}

UiSnapshot TakeCurrentThreadSnapshot() {
    EnsureUiCs();
    EnterCriticalSection(&g_ui_cs);
    UiSnapshot snapshot = TakeCurrentThreadSnapshot_NoLock();
    LeaveCriticalSection(&g_ui_cs);
    return snapshot;
}

UiSnapshot RetireThreadUi_NoLock(DWORD thread_id) {
    const auto found = g_thread_ui.find(thread_id);
    if (found == g_thread_ui.end() || found->second->refs != 0) {
        return {};
    }

    found->second->host = nullptr;
    if (found->second->retire_pending) {
        --g_ui_ref;
        found->second->retire_pending = false;
    }
    UiSnapshot retired {found->first, found->second->status, found->second->soft};
    found->second->status.reset();
    found->second->soft.reset();
    g_thread_ui.erase(found);
    return retired;
}

void AbandonAfterOwnerThreadExit(UiSnapshot* snapshot) noexcept {
    if (snapshot == nullptr) {
        return;
    }
    if (snapshot->soft) {
        snapshot->soft->AbandonAfterOwnerThreadExit();
    }
    if (snapshot->status) {
        snapshot->status->AbandonAfterOwnerThreadExit();
    }
}

void HandleOwnerThreadTask(DWORD owner_thread_id, UINT task) {
    if (task != kRetireThreadUiTask || GetCurrentThreadId() != owner_thread_id) {
        return;
    }

    EnsureUiCs();
    EnterCriticalSection(&g_ui_cs);
    UiSnapshot retired = RetireThreadUi_NoLock(owner_thread_id);
    LeaveCriticalSection(&g_ui_cs);

    // 最后一个窗口快照必须在创建窗口的线程上析构。
    retired = {};
}

template <typename Callback>
void DispatchToHost(DWORD thread_id, Callback&& callback) {
    TextService* host = nullptr;
    EnterCriticalSection(&g_ui_cs);
    const auto found = g_thread_ui.find(thread_id);
    if (found != g_thread_ui.end() && found->second->host != nullptr &&
        found->second->host->TryAddRefForUiCallback()) {
        host = found->second->host;
    }
    LeaveCriticalSection(&g_ui_cs);

    if (host != nullptr) {
        callback(host);
        host->Release();
    }
}

void InstallHandlers_NoLock(DWORD thread_id, const std::shared_ptr<ThreadUiState>& state) {
    if (!state || !state->status || !state->soft) {
        return;
    }
    state->status->SetOwnerThreadTaskHandler(thread_id, HandleOwnerThreadTask);
    state->status->SetToggleModeHandler([thread_id]() {
        DispatchToHost(thread_id, [](TextService* host) { host->OnStatusToggleEnglish(); });
    });
    state->status->SetToggleSchemaHandler([thread_id]() {
        DispatchToHost(thread_id, [](TextService* host) { host->OnStatusToggleSchema(); });
    });
    state->status->SetToggleKeyboardHandler([thread_id]() {
        DispatchToHost(thread_id, [](TextService* host) { host->OnStatusToggleKeyboard(); });
    });
    state->soft->SetKeyHandler([thread_id](wchar_t ch, bool special) {
        DispatchToHost(thread_id, [ch, special](TextService* host) {
            host->OnStatusSoftKey(ch, special);
        });
    });
}

}  // namespace

bool SharedStatusUi::Acquire(HINSTANCE instance, DWORD* owner_thread_id) {
    if (owner_thread_id == nullptr) {
        return false;
    }
    *owner_thread_id = 0;

    EnsureUiCs();
    const DWORD thread_id = GetCurrentThreadId();
    std::shared_ptr<ThreadUiState> state;
    UiSnapshot stale;

    EnterCriticalSection(&g_ui_cs);
    const auto found = g_thread_ui.find(thread_id);
    if (found != g_thread_ui.end()) {
        state = found->second;
        if (state->refs > 0 && state->status && state->soft) {
            ++state->refs;
            ++g_ui_ref;
            const long refs = g_ui_ref;
            LeaveCriticalSection(&g_ui_cs);
            *owner_thread_id = thread_id;
            SHURU_LOG_INFO("SharedStatusUi acquire total_ref=%ld thread=%lu", refs, thread_id);
            return true;
        }

        // refs==0 表示跨线程 Release 已排队等待所有者线程销毁。当前调用正好
        // 回到所有者线程，可取消待销毁状态并安全复用现有窗口。
        if (state->refs == 0 && state->status && state->soft &&
            state->status->IsOwnedByCurrentThread()) {
            state->refs = 1;
            if (state->retire_pending) {
                state->retire_pending = false;
            } else {
                ++g_ui_ref;
            }
            const long refs = g_ui_ref;
            LeaveCriticalSection(&g_ui_cs);
            *owner_thread_id = thread_id;
            SHURU_LOG_INFO("SharedStatusUi revived total_ref=%ld thread=%lu", refs, thread_id);
            return true;
        }

        if (state->refs == 0 && state->status && state->soft) {
            // 系统可能复用已退出线程的 ID，不能复活旧线程拥有的 HWND。
            stale = RetireThreadUi_NoLock(thread_id);
        } else {
            // 窗口创建期间发生同线程消息重入，不能等待自己完成。
            LeaveCriticalSection(&g_ui_cs);
            SHURU_LOG_WARN("SharedStatusUi reentrant acquire deferred thread=%lu", thread_id);
            return false;
        }
    }

    state = NewThreadUiState();
    if (!state || !InsertThreadUiState(thread_id, state)) {
        LeaveCriticalSection(&g_ui_cs);
        SHURU_LOG_ERROR("SharedStatusUi state allocation failed thread=%lu", thread_id);
        return false;
    }
    LeaveCriticalSection(&g_ui_cs);

    AbandonAfterOwnerThreadExit(&stale);
    stale = {};

    std::shared_ptr<StatusWindow> status(new (std::nothrow) StatusWindow());
    std::shared_ptr<SoftKeyboardWindow> soft(new (std::nothrow) SoftKeyboardWindow());
    const bool created = status && soft && status->Create(instance) && soft->Create(instance);
    if (!created) {
        if (soft) soft->Destroy();
        if (status) status->Destroy();
        EnterCriticalSection(&g_ui_cs);
        const auto current = g_thread_ui.find(thread_id);
        if (current != g_thread_ui.end() && current->second == state) {
            g_thread_ui.erase(current);
        }
        LeaveCriticalSection(&g_ui_cs);
        SHURU_LOG_ERROR("SharedStatusUi create failed thread=%lu", thread_id);
        return false;
    }

    EnterCriticalSection(&g_ui_cs);
    const auto current = g_thread_ui.find(thread_id);
    if (current == g_thread_ui.end() || current->second != state) {
        LeaveCriticalSection(&g_ui_cs);
        soft->Destroy();
        status->Destroy();
        return false;
    }
    state->status = std::move(status);
    state->soft = std::move(soft);
    state->refs = 1;
    ++g_ui_ref;
    InstallHandlers_NoLock(thread_id, state);
    // 第一个 StatusWindow 创建时安装托盘图标（每进程一个）。
    const bool first_window = (g_ui_ref == 1);
    const long refs = g_ui_ref;
    LeaveCriticalSection(&g_ui_cs);

    if (first_window) {
        const RuntimeConfig cfg = GetRuntimeConfig();
        state->status->InstallTrayIcon(cfg.tray_text, false);
    }

    *owner_thread_id = thread_id;
    SHURU_LOG_INFO("SharedStatusUi created total_ref=%ld thread=%lu", refs, thread_id);
    return true;
}

void SharedStatusUi::Release(DWORD owner_thread_id) {
    if (owner_thread_id == 0) {
        return;
    }

    EnsureUiCs();
    UiSnapshot retired;
    std::shared_ptr<StatusWindow> owner_window;
    long refs = 0;

    EnterCriticalSection(&g_ui_cs);
    const auto found = g_thread_ui.find(owner_thread_id);
    if (found != g_thread_ui.end() && found->second->refs > 0) {
        --found->second->refs;
        if (found->second->refs == 0) {
            found->second->host = nullptr;
            if (GetCurrentThreadId() == owner_thread_id) {
                --g_ui_ref;
                retired = RetireThreadUi_NoLock(owner_thread_id);
            } else {
                // 保留一个卸载保护引用，直到所有者线程真正销毁窗口。
                found->second->retire_pending = true;
                owner_window = found->second->status;
            }
        } else {
            --g_ui_ref;
        }
    }
    refs = g_ui_ref;
    LeaveCriticalSection(&g_ui_cs);

    if (owner_window && !owner_window->PostOwnerThreadTask(kRetireThreadUiTask)) {
        // 窗口线程已退出时 Windows 会清理其 HWND。保留空引用状态会阻止 DLL
        // 安全卸载，因此仅清理注册表项；不在错误线程调用 DestroyWindow。
        UiSnapshot abandoned;
        EnterCriticalSection(&g_ui_cs);
        const auto owner_state = g_thread_ui.find(owner_thread_id);
        if (owner_state != g_thread_ui.end() && owner_state->second->refs == 0) {
            abandoned = RetireThreadUi_NoLock(owner_thread_id);
        }
        LeaveCriticalSection(&g_ui_cs);
        AbandonAfterOwnerThreadExit(&abandoned);
        abandoned = {};
        SHURU_LOG_WARN("SharedStatusUi owner thread unavailable thread=%lu", owner_thread_id);
    }

    retired = {};
    SHURU_LOG_INFO("SharedStatusUi release total_ref=%ld owner_thread=%lu caller_thread=%lu",
                   refs, owner_thread_id, GetCurrentThreadId());
}

void SharedStatusUi::Bind(TextService* host) {
    EnsureUiCs();
    EnterCriticalSection(&g_ui_cs);
    const auto found = g_thread_ui.find(GetCurrentThreadId());
    if (found != g_thread_ui.end() && found->second->refs > 0) {
        found->second->host = host;
    }
    LeaveCriticalSection(&g_ui_cs);
}

void SharedStatusUi::Unbind(TextService* host) {
    if (host == nullptr) {
        return;
    }

    EnsureUiCs();
    UiSnapshot snapshot;
    const DWORD current_thread_id = GetCurrentThreadId();
    EnterCriticalSection(&g_ui_cs);
    for (auto& entry : g_thread_ui) {
        if (entry.second->host == host) {
            entry.second->host = nullptr;
            if (entry.first == current_thread_id) {
                snapshot = UiSnapshot {entry.first, entry.second->status, entry.second->soft};
            }
        }
    }
    LeaveCriticalSection(&g_ui_cs);

    // 异常地跨线程析构时只撤下回调目标，不在错误线程操作 HWND。
    if (snapshot.soft) snapshot.soft->Hide();
    if (snapshot.status) {
        snapshot.status->SetKeyboardOpen(false);
        snapshot.status->Hide();
    }
}

void SharedStatusUi::Show() {
    const UiSnapshot snapshot = TakeCurrentThreadSnapshot();
    if (snapshot.status) snapshot.status->Show();
}

void SharedStatusUi::Hide() {
    const UiSnapshot snapshot = TakeCurrentThreadSnapshot();
    if (snapshot.soft) snapshot.soft->Hide();
    if (snapshot.status) {
        snapshot.status->SetKeyboardOpen(false);
        snapshot.status->Hide();
    }
}

void SharedStatusUi::SyncFrom(bool english_mode, bool shuangpin_mode) {
    const UiSnapshot snapshot = TakeCurrentThreadSnapshot();
    if (snapshot.status) {
        snapshot.status->SetEnglishMode(english_mode);
        snapshot.status->SetShuangpinMode(shuangpin_mode);
        snapshot.status->SetKeyboardOpen(snapshot.soft && snapshot.soft->IsVisible());
        // 托盘图标随输入模式同步更新
        if (snapshot.status->HasTrayIcon()) {
            const RuntimeConfig cfg = GetRuntimeConfig();
            snapshot.status->UpdateTrayIcon(cfg.tray_text, english_mode);
        }
    }
}

bool SharedStatusUi::IsSoftKeyboardVisible() {
    const UiSnapshot snapshot = TakeCurrentThreadSnapshot();
    return snapshot.soft && snapshot.soft->IsVisible();
}

void SharedStatusUi::ShowSoftKeyboard(const POINT& anchor) {
    const UiSnapshot snapshot = TakeCurrentThreadSnapshot();
    if (snapshot.soft) snapshot.soft->Show(anchor);
    if (snapshot.status) snapshot.status->SetKeyboardOpen(snapshot.soft != nullptr);
}

void SharedStatusUi::HideSoftKeyboard() {
    const UiSnapshot snapshot = TakeCurrentThreadSnapshot();
    if (snapshot.soft) snapshot.soft->Hide();
    if (snapshot.status) snapshot.status->SetKeyboardOpen(false);
}

void SharedStatusUi::ToggleSoftKeyboard(const POINT& anchor) {
    if (IsSoftKeyboardVisible()) {
        HideSoftKeyboard();
    } else {
        ShowSoftKeyboard(anchor);
    }
}

long SharedStatusUi::RefCount() {
    EnsureUiCs();
    EnterCriticalSection(&g_ui_cs);
    const long refs = g_ui_ref;
    LeaveCriticalSection(&g_ui_cs);
    return refs;
}

}  // namespace shuru
