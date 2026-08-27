#pragma once

#include "pinyin_engine.h"

#include <string>

namespace shuru {

// 进程内单例引擎：多 TextService 实例共享同一份词库。
// Acquire 立即返回；词库在后台线程加载，IsReady/GetIfReady 在完成前为未就绪。
class SharedEngine {
public:
    static PinyinEngine* Acquire(const std::wstring& lexicon_dir);
    static void Release();
    static PinyinEngine* GetIfReady();
    static bool IsLoading();
    static long RefCount();
    static void Shutdown();

    // 有界等待词库就绪：快照冷加载毫秒级完成，正常情况下立即返回 true。
    // 超时返回当前是否就绪；加载失败或无加载在途时立即返回 false。
    // 供按键路径替代"原始拼音保底"，绝不能无限阻塞 UI 线程。
    static bool WaitForReady(unsigned long timeout_ms);

    // 最近一次加载是否失败；失败态由下一次 Acquire 重试自动清除。
    static bool HasFailed();

private:
    SharedEngine() = delete;
};

}  // namespace shuru
