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

private:
    SharedEngine() = delete;
};

}  // namespace shuru
