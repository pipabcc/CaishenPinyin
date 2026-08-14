#pragma once

#include <Windows.h>

namespace shuru {

class TextService;

// 每个 TSF UI 线程共享一套状态栏和软键盘；不同 UI 线程彼此隔离。
class SharedStatusUi {
public:
    static bool Acquire(HINSTANCE instance, DWORD* owner_thread_id);
    static void Release(DWORD owner_thread_id);

    // 绑定当前前台 TextService，回调只落到当前线程对应的实例。
    static void Bind(TextService* host);
    static void Unbind(TextService* host);

    static void Show();
    static void Hide();

    static void SyncFrom(bool english_mode, bool shuangpin_mode = false);
    static bool HasTrayIcon();
    static bool IsSoftKeyboardVisible();
    static void ShowSoftKeyboard(const POINT& anchor);
    static void HideSoftKeyboard();
    static void ToggleSoftKeyboard(const POINT& anchor);

    static long RefCount();

private:
    SharedStatusUi() = delete;
};

}  // namespace shuru
