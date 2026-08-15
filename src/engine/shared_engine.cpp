#include "shared_engine.h"

#include "../common/logger.h"

#include <Windows.h>
#include <new>

namespace shuru {
namespace {

CRITICAL_SECTION g_shared_cs;
INIT_ONCE g_shared_once = INIT_ONCE_STATIC_INIT;
PinyinEngine* g_shared_engine = nullptr;
long g_shared_ref = 0;
std::wstring g_shared_lexicon;
HANDLE g_load_thread = nullptr;
volatile LONG g_loading = 0;
bool g_shutdown_waiting = false;

BOOL CALLBACK InitSharedCs(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_shared_cs);
    return TRUE;
}

void EnsureSharedCs() {
    InitOnceExecuteOnce(&g_shared_once, InitSharedCs, nullptr, nullptr);
}

DWORD WINAPI SharedEngineLoadProc(LPVOID) {
    PinyinEngine* eng = nullptr;
    std::wstring lexicon;

    EnsureSharedCs();
    EnterCriticalSection(&g_shared_cs);
    eng = g_shared_engine;
    lexicon = g_shared_lexicon;
    LeaveCriticalSection(&g_shared_cs);

    bool ok = false;
    if (eng != nullptr) {
        // 重活在 g_shared_cs 外：Activate 不再被大词库加载拖死
        ok = eng->Initialize(lexicon);
    }

    if (ok) {
        SHURU_LOG_INFO("SharedEngine async load ok");
    } else {
        SHURU_LOG_ERROR("SharedEngine async load failed");
    }

    // 线程句柄有信号后才能析构引擎；loading 仅表示 Initialize 的重活已结束。
    InterlockedExchange(&g_loading, 0);
    return ok ? 0 : 1;
}

// 调用方须已持有 g_shared_cs。仅启动线程，绝不在此同步 Initialize。
bool StartAsyncLoad_NoLock() {
    if (g_shared_engine == nullptr) {
        return false;
    }
    if (g_load_thread != nullptr) {
        const DWORD thread_state = WaitForSingleObject(g_load_thread, 0);
        if (thread_state == WAIT_TIMEOUT) {
            return true;
        }
        if (thread_state != WAIT_OBJECT_0) {
            SHURU_LOG_WARN("SharedEngine load thread state unavailable");
            return false;
        }
        CloseHandle(g_load_thread);
        g_load_thread = nullptr;
    }
    if (InterlockedCompareExchange(&g_loading, 1, 0) != 0) {
        return true;
    }
    g_load_thread = CreateThread(nullptr, 0, SharedEngineLoadProc, nullptr, 0, nullptr);
    if (g_load_thread == nullptr) {
        InterlockedExchange(&g_loading, 0);
        SHURU_LOG_ERROR("SharedEngine CreateThread failed (engine stays not-ready)");
        return false;
    }
    SHURU_LOG_INFO("SharedEngine async load started");
    return true;
}

}  // namespace

PinyinEngine* SharedEngine::Acquire(const std::wstring& lexicon_dir) {
    EnsureSharedCs();
    EnterCriticalSection(&g_shared_cs);

    if (g_shared_engine == nullptr) {
        auto* eng = new (std::nothrow) PinyinEngine();
        if (eng == nullptr) {
            LeaveCriticalSection(&g_shared_cs);
            return nullptr;
        }
        g_shared_engine = eng;
        g_shared_lexicon = lexicon_dir;
        StartAsyncLoad_NoLock();
        SHURU_LOG_INFO("SharedEngine created once (lazy load)");
    } else if (!lexicon_dir.empty() && !g_shared_lexicon.empty() && lexicon_dir != g_shared_lexicon) {
        SHURU_LOG_WARN("SharedEngine ignore alternate lexicon path");
    } else if (!g_shutdown_waiting && !g_shared_engine->IsReady() &&
               InterlockedCompareExchange(&g_loading, 0, 0) == 0) {
        if (g_shared_lexicon.empty() && !lexicon_dir.empty()) {
            g_shared_lexicon = lexicon_dir;
        }
        StartAsyncLoad_NoLock();
    }

    ++g_shared_ref;
    PinyinEngine* out = g_shared_engine;
    const long refs = g_shared_ref;
    const int ready = (out && out->IsReady()) ? 1 : 0;
    const long loading = static_cast<long>(InterlockedCompareExchange(&g_loading, 0, 0));
    LeaveCriticalSection(&g_shared_cs);
    SHURU_LOG_INFO("SharedEngine acquire ref=%ld ready=%d loading=%ld", refs, ready, loading);
    return out;
}

void SharedEngine::Release() {
    EnsureSharedCs();
    EnterCriticalSection(&g_shared_cs);
    if (g_shared_ref > 0) {
        --g_shared_ref;
    }
    const long refs = g_shared_ref;
    // 进程级热缓存：当引用计数降至 0 时保留已创建/已就绪的单例引擎，
    // 避免用户在不同窗口或输入法之间频繁切换时重复销毁并重新加载大词库。
    // 仅在显式调用 SharedEngine::Shutdown() 或进程退出时才物理回收。
    LeaveCriticalSection(&g_shared_cs);
    SHURU_LOG_INFO("SharedEngine release ref=%ld (cached hot)", refs);
}

PinyinEngine* SharedEngine::GetIfReady() {
    EnsureSharedCs();
    EnterCriticalSection(&g_shared_cs);
    PinyinEngine* out = (g_shared_engine && g_shared_engine->IsReady()) ? g_shared_engine : nullptr;
    LeaveCriticalSection(&g_shared_cs);
    return out;
}

bool SharedEngine::IsLoading() {
    EnsureSharedCs();
    EnterCriticalSection(&g_shared_cs);
    const bool thread_running =
        g_load_thread != nullptr && WaitForSingleObject(g_load_thread, 0) == WAIT_TIMEOUT;
    const bool loading =
        thread_running || InterlockedCompareExchange(&g_loading, 0, 0) != 0;
    LeaveCriticalSection(&g_shared_cs);
    return loading;
}

long SharedEngine::RefCount() {
    EnsureSharedCs();
    EnterCriticalSection(&g_shared_cs);
    const long refs = g_shared_ref;
    LeaveCriticalSection(&g_shared_cs);
    return refs;
}

void SharedEngine::Shutdown() {
    EnsureSharedCs();

    HANDLE thr = nullptr;
    PinyinEngine* expected_engine = nullptr;
    EnterCriticalSection(&g_shared_cs);
    if (g_shared_ref != 0 || g_shutdown_waiting) {
        const long refs = g_shared_ref;
        LeaveCriticalSection(&g_shared_cs);
        SHURU_LOG_WARN("SharedEngine shutdown skipped while refs=%ld waiting=%d",
                       refs, g_shutdown_waiting ? 1 : 0);
        return;
    }
    g_shutdown_waiting = true;
    expected_engine = g_shared_engine;
    thr = g_load_thread;
    LeaveCriticalSection(&g_shared_cs);

    DWORD wait = WAIT_OBJECT_0;
    if (thr != nullptr) {
        wait = WaitForSingleObject(thr, INFINITE);
        if (wait != WAIT_OBJECT_0) {
            EnterCriticalSection(&g_shared_cs);
            g_shutdown_waiting = false;
            LeaveCriticalSection(&g_shared_cs);
            SHURU_LOG_WARN("SharedEngine shutdown: load thread wait failed");
            return;
        }
    }

    PinyinEngine* cleanup = nullptr;
    EnterCriticalSection(&g_shared_cs);
    if (g_load_thread == thr) {
        g_load_thread = nullptr;
        if (thr != nullptr) {
            CloseHandle(thr);
        }
    }
    g_shutdown_waiting = false;
    // 等待加载线程期间 Acquire 可能重新取得或替换共享引擎；只回收仍未被引用的原实例。
    if (g_shared_ref == 0 && g_shared_engine == expected_engine &&
        InterlockedCompareExchange(&g_loading, 0, 0) == 0) {
        cleanup = g_shared_engine;
        g_shared_engine = nullptr;
        g_shared_lexicon.clear();
    } else if (g_shared_ref != 0 && g_shared_engine == expected_engine &&
               !g_shared_engine->IsReady() &&
               InterlockedCompareExchange(&g_loading, 0, 0) == 0) {
        StartAsyncLoad_NoLock();
    }
    LeaveCriticalSection(&g_shared_cs);

    delete cleanup;
    if (cleanup != nullptr) {
        SHURU_LOG_INFO("SharedEngine shutdown");
    } else if (expected_engine != nullptr) {
        SHURU_LOG_INFO("SharedEngine shutdown skipped after concurrent acquire");
    }
}

}  // namespace shuru
