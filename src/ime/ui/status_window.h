#pragma once

#include <Windows.h>

#include <functional>
#include <string>

namespace shuru {

// 右下角状态条：中/英、全/双、软键盘和设置入口。
class StatusWindow {
public:
    using ToggleKeyboardFn = std::function<void()>;
    using ToggleModeFn = std::function<void()>;
    using ToggleSchemaFn = std::function<void()>;
    using OwnerThreadTaskFn = void (*)(DWORD owner_thread_id, UINT task);

    StatusWindow() = default;
    ~StatusWindow();

    bool Create(HINSTANCE instance);
    void Destroy();
    void AbandonAfterOwnerThreadExit() noexcept;
    bool IsOwnedByCurrentThread() const noexcept;

    void Show();
    void Hide();
    void SetEnglishMode(bool english);
    void SetShuangpinMode(bool shuangpin);
    void SetKeyboardOpen(bool open);
    bool IsKeyboardOpen() const { return keyboard_open_; }
    bool IsVisible() const { return visible_; }

    void SetToggleKeyboardHandler(ToggleKeyboardFn fn) { on_toggle_keyboard_ = std::move(fn); }
    void SetToggleModeHandler(ToggleModeFn fn) { on_toggle_mode_ = std::move(fn); }
    void SetToggleSchemaHandler(ToggleSchemaFn fn) { on_toggle_schema_ = std::move(fn); }
    void SetOwnerThreadTaskHandler(DWORD owner_thread_id, OwnerThreadTaskFn fn) noexcept;
    bool PostOwnerThreadTask(UINT task) const noexcept;

    // 财神主题托盘图标：安装到系统通知区；图标字符从 RuntimeConfig::tray_text 取得。
    bool InstallTrayIcon(const std::wstring& tray_text, bool english_mode);
    void UninstallTrayIcon();
    // 输入模式变化时更新托盘图标字符与颜色。
    void UpdateTrayIcon(const std::wstring& tray_text, bool english_mode);
    bool HasTrayIcon() const { return tray_installed_; }

private:
    static constexpr int kBaseWidth = 216;
    static constexpr int kBaseHeight = 36;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr;
    bool english_mode_ = false;
    bool shuangpin_mode_ = false;
    bool keyboard_open_ = false;
    bool visible_ = false;
    ToggleKeyboardFn on_toggle_keyboard_;
    ToggleModeFn on_toggle_mode_;
    ToggleSchemaFn on_toggle_schema_;
    DWORD owner_thread_id_ = 0;
    OwnerThreadTaskFn on_owner_thread_task_ = nullptr;

    int Scale(int value) const;
    void RecreateFont();
    void PlaceBottomRight();
    LRESULT OnPaint();
    LRESULT OnLButtonUp(int x, int y);
    LRESULT OnRButtonUp(int x, int y);
    LRESULT OnTrayIconMsg(LPARAM lparam);
    bool LaunchSettings() const;
    static HICON CreateTextHIcon(const std::wstring& text, bool english_mode);
    static constexpr UINT kOwnerThreadTaskMessage = WM_APP + 0x51;
    static constexpr UINT kTrayIconCallbackMsg   = WM_APP + 0x52;
    static constexpr UINT kTrayIconId = 1;
    bool tray_installed_ = false;
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

// 简易软键盘（字母 + 空格/退格/Esc），通过回调注入按键语义
class SoftKeyboardWindow {
public:
    using KeyHandler = std::function<void(wchar_t ch, bool is_special)>;
    // is_special: ch 为 VK 语义时用 'S'=space 'B'=back 'E'=esc

    SoftKeyboardWindow() = default;
    ~SoftKeyboardWindow();

    bool Create(HINSTANCE instance);
    void Destroy();
    void AbandonAfterOwnerThreadExit() noexcept;
    void Show(const POINT& anchor);
    void Hide();
    bool IsVisible() const { return visible_; }
    void SetKeyHandler(KeyHandler fn) { on_key_ = std::move(fn); }

private:
    static constexpr int kCols = 10;
    static constexpr int kRows = 4;
    static constexpr int kKeyW = 36;
    static constexpr int kKeyH = 34;
    static constexpr int kGap = 4;

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    bool visible_ = false;
    KeyHandler on_key_;
    int hover_ = -1;

    int Scale(int value) const;
    void RecreateFont();
    SIZE WindowSize() const;
    RECT KeyRect(int index) const;
    int KeyCount() const;
    bool HitTest(int x, int y, int* index) const;
    void EmitIndex(int index);
    LRESULT OnPaint();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

}  // namespace shuru
