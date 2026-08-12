#include "engine/shared_engine.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

struct ShutdownContext {
    HANDLE started = nullptr;
};

DWORD WINAPI ShutdownProc(LPVOID parameter) {
    auto* context = static_cast<ShutdownContext*>(parameter);
    SetEvent(context->started);
    shuru::SharedEngine::Shutdown();
    return 0;
}

bool CreateSlowLexicon(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    std::ofstream output(directory / L"base_dict.txt", std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    constexpr char kEntry[] = "ni\t\xE4\xBD\xA0\t100\n";
    for (int i = 0; i < 400000; ++i) {
        output.write(kEntry, sizeof(kEntry) - 1);
    }
    return static_cast<bool>(output);
}

}  // namespace

int wmain() {
    using namespace shuru;

    wchar_t temp_path[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp_path) == 0) {
        std::wcerr << L"无法取得临时目录\n";
        return 1;
    }
    const std::filesystem::path test_root =
        std::filesystem::path(temp_path) /
        (L"FacaiSharedEngineTest-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path lexicon = test_root / L"lexicon";
    std::error_code cleanup_error;
    std::filesystem::remove_all(test_root, cleanup_error);
    if (!CreateSlowLexicon(lexicon)) {
        std::wcerr << L"无法创建生命周期测试词库\n";
        return 2;
    }

    PinyinEngine* first = SharedEngine::Acquire(lexicon.wstring());
    if (first == nullptr || !SharedEngine::IsLoading()) {
        std::wcerr << L"共享引擎没有进入异步加载状态\n";
        return 3;
    }

    using Clock = std::chrono::steady_clock;
    std::chrono::milliseconds maximum_readiness_latency {0};
    const auto readiness_deadline = Clock::now() + std::chrono::milliseconds(75);
    while (SharedEngine::IsLoading() && Clock::now() < readiness_deadline) {
        const auto started = Clock::now();
        first->IsReady();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started);
        maximum_readiness_latency = (std::max)(maximum_readiness_latency, elapsed);
        Sleep(1);
    }
    if (maximum_readiness_latency > std::chrono::milliseconds(100)) {
        std::wcerr << L"加载期间 IsReady 被词库构建锁阻塞: "
                   << maximum_readiness_latency.count() << L" ms\n";
        return 10;
    }
    SharedEngine::Release();

    ShutdownContext context;
    context.started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (context.started == nullptr) {
        return 4;
    }
    HANDLE shutdown_thread = CreateThread(nullptr, 0, ShutdownProc, &context, 0, nullptr);
    if (shutdown_thread == nullptr) {
        CloseHandle(context.started);
        return 5;
    }
    WaitForSingleObject(context.started, INFINITE);
    Sleep(25);

    PinyinEngine* acquired_during_shutdown = SharedEngine::Acquire(lexicon.wstring());
    const DWORD shutdown_wait = WaitForSingleObject(shutdown_thread, 20000);
    CloseHandle(shutdown_thread);
    CloseHandle(context.started);

    if (shutdown_wait != WAIT_OBJECT_0 || acquired_during_shutdown == nullptr) {
        std::wcerr << L"共享引擎关闭线程没有正常完成\n";
        return 6;
    }
    if (SharedEngine::RefCount() != 1) {
        std::wcerr << L"并发 Acquire 的引用被 Shutdown 清除\n";
        return 7;
    }

    for (int i = 0; i < 2000 && !acquired_during_shutdown->IsReady(); ++i) {
        Sleep(5);
    }
    if (!acquired_during_shutdown->IsReady()) {
        std::wcerr << L"并发取得的共享引擎没有完成加载\n";
        return 8;
    }
    if (acquired_during_shutdown->Query("ni", 1).candidates.empty()) {
        std::wcerr << L"并发取得的共享引擎不可查询\n";
        return 9;
    }

    SharedEngine::Release();
    SharedEngine::Shutdown();
    std::filesystem::remove_all(test_root, cleanup_error);
    std::wcout << L"shared_engine_lifecycle: OK, max IsReady latency="
               << maximum_readiness_latency.count() << L" ms\n";
    return 0;
}
