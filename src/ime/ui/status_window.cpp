#include "status_window.h"
#include "ime_ui_logic.h"
#include "../../common/runtime_config.h"
#include <algorithm>
#include <cstdio>
#include <shellapi.h>

namespace shuru {
namespace {

const wchar_t kStatusClass[] = L"ShuruStatusWindowClass";
const wchar_t kSoftClass[] = L"ShuruSoftKeyboardClass";
#if defined(SHURU_TRAY_OWNER_MUTEX_TEST)
const GUID kTrayIconGuid = {
    0x52d5c920, 0x47c2, 0x4a1f,
    {0x9d, 0x64, 0x7b, 0x85, 0x6a, 0x30, 0x2e, 0xfe}};
#else
const GUID kTrayIconGuid = {
    0x52d5c920, 0x47c2, 0x4a1f,
    {0x9d, 0x64, 0x7b, 0x85, 0x6a, 0x30, 0x2e, 0xf1}};
#endif

HANDLE CreateTrayOwnerMutex() {
#if defined(SHURU_TRAY_OWNER_MUTEX_TEST)
    wchar_t name[96] {};
    swprintf_s(
        name, L"Local\\CaishenPinyin.TrayIcon.Owner.Test.%lu",
        GetCurrentProcessId());
    return CreateMutexW(nullptr, FALSE, name);
#else
    return CreateMutexW(
        nullptr, FALSE, L"Local\\CaishenPinyin.TrayIcon.Owner.v1");
#endif
}

}  // namespace

StatusWindow::~StatusWindow() {
    Destroy();
}

bool StatusWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    if (hwnd_) {
        return true;
    }
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kStatusClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kStatusClass, L"",
        WS_POPUP,
        0, 0, kBaseWidth, kBaseHeight,
        nullptr, nullptr, instance, this);
    if (!hwnd_) {
        return false;
    }
    RecreateFont();
    return true;
}

int StatusWindow::Scale(int value) const {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    return MulDiv(value, dpi ? static_cast<int>(dpi) : 96, 96);
}

void StatusWindow::RecreateFont() {
    if (font_) {
        DeleteObject(font_);
    }
    font_ = CreateFontW(-Scale(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void StatusWindow::Destroy() {
    UninstallTrayIcon();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    visible_ = false;
}

void StatusWindow::AbandonAfterOwnerThreadExit() noexcept {
    // 线程退出时系统已经销毁该线程创建的窗口；这里只释放进程级 GDI 对象。
    hwnd_ = nullptr;
    tray_installed_ = false;
    if (tray_owner_mutex_) {
        // 所有者线程退出后 mutex 已被系统标记为 abandoned；此处不能从
        // 当前清理线程调用 ReleaseMutex，只关闭本进程遗留的句柄。
        CloseHandle(tray_owner_mutex_);
        tray_owner_mutex_ = nullptr;
        tray_owner_thread_id_ = 0;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    visible_ = false;
}

bool StatusWindow::IsOwnedByCurrentThread() const noexcept {
    return hwnd_ != nullptr &&
           GetWindowThreadProcessId(hwnd_, nullptr) == GetCurrentThreadId();
}

void StatusWindow::PlaceBottomRight() {
    if (!hwnd_) {
        return;
    }
    POINT pt {};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    RECT work {0, 0, 800, 600};
    if (mon && GetMonitorInfoW(mon, &mi)) {
        work = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    const int width = Scale(kBaseWidth);
    const int height = Scale(kBaseHeight);
    const int margin = Scale(12);
    const int x = work.right - width - margin;
    const int y = work.bottom - height - margin;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void StatusWindow::Show() {
    // 悬浮状态栏已停用；窗口对象仍保留，供跨线程生命周期与软键盘使用。
    Hide();
}

void StatusWindow::Hide() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = false;
}

void StatusWindow::SetEnglishMode(bool english) {
    english_mode_ = english;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void StatusWindow::SetShuangpinMode(bool shuangpin) {
    shuangpin_mode_ = shuangpin;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void StatusWindow::SetKeyboardOpen(bool open) {
    keyboard_open_ = open;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void StatusWindow::SetOwnerThreadTaskHandler(
    DWORD owner_thread_id,
    OwnerThreadTaskFn fn) noexcept {
    owner_thread_id_ = owner_thread_id;
    on_owner_thread_task_ = fn;
}

bool StatusWindow::PostOwnerThreadTask(UINT task) const noexcept {
    return hwnd_ != nullptr && PostMessageW(hwnd_, kOwnerThreadTaskMessage, task, 0) != FALSE;
}

LRESULT StatusWindow::OnPaint() {
    PAINTSTRUCT ps {};
    HDC hdc = BeginPaint(hwnd_, &ps);
    if (hdc == nullptr) {
        return 0;
    }
    RECT rc {};
    GetClientRect(hwnd_, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    if (mem == nullptr || bmp == nullptr) {
        if (bmp != nullptr) DeleteObject(bmp);
        if (mem != nullptr) DeleteDC(mem);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(RGB(32, 36, 44));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 80, 96));
    HGDIOBJ old_pen = SelectObject(mem, pen);
    HGDIOBJ old_br = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, 0, 0, rc.right, rc.bottom);
    SelectObject(mem, old_br);
    SelectObject(mem, old_pen);
    DeleteObject(pen);

    SetBkMode(mem, TRANSPARENT);
    HFONT old_font = nullptr;
    if (font_) {
        old_font = static_cast<HFONT>(SelectObject(mem, font_));
    }

    const int edge = Scale(4);
    const int gap = Scale(3);
    const int inner = rc.right - edge * 2 - gap * 3;
    const int w = inner / 4;
    SetTextColor(mem, RGB(255, 255, 255));

    // 中/英
    RECT left {edge, edge, edge + w, rc.bottom - edge};
    HBRUSH mode_br = CreateSolidBrush(english_mode_ ? RGB(80, 90, 110) : RGB(40, 110, 210));
    FillRect(mem, &left, mode_br);
    DeleteObject(mode_br);
    DrawTextW(mem, english_mode_ ? L"英" : L"中", -1, &left, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 全/双
    RECT mid {left.right + gap, edge, left.right + gap + w, rc.bottom - edge};
    HBRUSH schema_br = CreateSolidBrush(shuangpin_mode_ ? RGB(120, 80, 180) : RGB(60, 66, 78));
    FillRect(mem, &mid, schema_br);
    DeleteObject(schema_br);
    DrawTextW(mem, shuangpin_mode_ ? L"双" : L"全", -1, &mid, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 软键盘
    RECT right {mid.right + gap, edge, mid.right + gap + w, rc.bottom - edge};
    HBRUSH kb_br = CreateSolidBrush(keyboard_open_ ? RGB(46, 160, 120) : RGB(60, 66, 78));
    FillRect(mem, &right, kb_br);
    DeleteObject(kb_br);
    DrawTextW(mem, L"键", -1, &right, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 设置
    RECT settings {right.right + gap, edge, rc.right - edge, rc.bottom - edge};
    HBRUSH settings_br = CreateSolidBrush(RGB(60, 66, 78));
    FillRect(mem, &settings, settings_br);
    DeleteObject(settings_br);
    DrawTextW(mem, L"设", -1, &settings, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (old_font) {
        SelectObject(mem, old_font);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
    return 0;
}

LRESULT StatusWindow::OnLButtonUp(int x, int /*y*/) {
    RECT rc {};
    GetClientRect(hwnd_, &rc);
    const int edge = Scale(4);
    const int gap = Scale(3);
    const int inner = rc.right - edge * 2 - gap * 3;
    const int w = inner / 4;
    const int x1 = edge + w;
    const int x2 = x1 + gap + w;
    const int x3 = x2 + gap + w;
    if (x < x1) {
        if (on_toggle_mode_) on_toggle_mode_();
    } else if (x < x2) {
        if (on_toggle_schema_) on_toggle_schema_();
    } else if (x < x3) {
        if (on_toggle_keyboard_) on_toggle_keyboard_();
    } else if (!LaunchSettings()) {
        MessageBoxW(hwnd_, L"无法找到或启动 ShuruSettings.exe。请重新安装设置程序。",
                    L"财神输入法", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    return 0;
}

bool StatusWindow::LaunchSettings() const {
    return LaunchSettingsExecutable(hwnd_, instance_);
}

LRESULT StatusWindow::OnRButtonUp(int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return 0;
    AppendMenuW(menu, MF_STRING, 1, L"输入法设置");
    POINT point {x, y};
    ClientToScreen(hwnd_, &point);
    // 无激活窗口上的菜单需要以前台窗口作为所有者，否则可能立即消失。
    SetForegroundWindow(hwnd_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        point.x, point.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command == 1 && !LaunchSettings()) {
        MessageBoxW(hwnd_, L"无法找到或启动 ShuruSettings.exe。请检查安装目录或开发构建目录。",
                    L"财神输入法", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    return 0;
}

LRESULT CALLBACK StatusWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    StatusWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<StatusWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<StatusWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    switch (msg) {
    case kOwnerThreadTaskMessage: {
        // 任务可能销毁当前窗口，因此先复制到栈上，调用后不再访问 self。
        const OwnerThreadTaskFn handler = self->on_owner_thread_task_;
        const DWORD owner_thread_id = self->owner_thread_id_;
        if (handler != nullptr) {
            handler(owner_thread_id, static_cast<UINT>(wparam));
        }
        return 0;
    }
    case WM_PAINT:
        return self->OnPaint();
    case WM_LBUTTONUP:
        return self->OnLButtonUp(static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)));
    case WM_RBUTTONUP:
        return self->OnRButtonUp(static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)));
    case kTrayIconCallbackMsg:
        return self->OnTrayIconMsg(lparam);
    case WM_CONTEXTMENU: {
        POINT point {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        if (point.x == -1 && point.y == -1) {
            RECT rect {};
            GetWindowRect(hwnd, &rect);
            point = POINT {rect.left + self->Scale(8), rect.bottom};
        }
        ScreenToClient(hwnd, &point);
        return self->OnRButtonUp(point.x, point.y);
    }
    case WM_DPICHANGED: {
        self->RecreateFont();
        const auto* rect = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top,
                     rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ---- 托盘图标（财神主题） ----

// 用 GDI 绘制一个 size×size 的文字图标并返回 HICON。
// chinese 模式：深红底（#C0392B）+ 白字；英文模式：石板灰底（#4A5568）+ 白字。
// 使用 DIBSection 32bpp，绘制后统一把 alpha 置为 0xFF（GDI 不写 alpha）。
/*static*/
HICON StatusWindow::CreateTextHIcon(const std::wstring& text, bool english_mode) {
    const int sz = 32;
    BITMAPV5HEADER bih {};
    bih.bV5Size        = sizeof(bih);
    bih.bV5Width       = sz;
    bih.bV5Height      = -sz;
    bih.bV5Planes      = 1;
    bih.bV5BitCount    = 32;
    bih.bV5Compression = BI_BITFIELDS;
    bih.bV5RedMask     = 0x00FF0000;
    bih.bV5GreenMask   = 0x0000FF00;
    bih.bV5BlueMask    = 0x000000FF;
    bih.bV5AlphaMask   = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color_bmp = CreateDIBSection(
        screen, reinterpret_cast<BITMAPINFO*>(&bih),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color_bmp || !bits) return nullptr;

    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, color_bmp);

    const COLORREF bg = english_mode ? RGB(74, 85, 104) : RGB(192, 57, 43);
    HBRUSH br = CreateSolidBrush(bg);
    RECT rc {0, 0, sz, sz};
    FillRect(mem, &rc, br);
    DeleteObject(br);

    const int font_h = text.size() == 1 ? -20 : -16;
    HFONT font = CreateFontW(font_h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ old_font = SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    DrawTextW(mem, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, old_font);
    DeleteObject(font);

    // GDI 不写 alpha 通道；全部置 0xFF 使图标完全不透明
    DWORD* px = static_cast<DWORD*>(bits);
    for (int i = 0; i < sz * sz; ++i) px[i] |= 0xFF000000;

    SelectObject(mem, old);
    DeleteDC(mem);

    HBITMAP mask_bmp = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii {};
    ii.fIcon    = TRUE;
    ii.hbmColor = color_bmp;
    ii.hbmMask  = mask_bmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color_bmp);
    DeleteObject(mask_bmp);
    return icon;
}

bool StatusWindow::InstallTrayIcon(const std::wstring& /*tray_text*/, bool /*english_mode*/) {
    // 依用户需求：托盘区不再显示打开设置页面的通知图标。
    // 确保清理此前可能已存在的系统托盘通知图标，并不再添加新图标。
    if (hwnd_) {
        NOTIFYICONDATAW nid {};
        nid.cbSize   = sizeof(nid);
        nid.hWnd     = hwnd_;
        nid.uID      = kTrayIconId;
        nid.uFlags   = NIF_GUID;
        nid.guidItem = kTrayIconGuid;
        if (!Shell_NotifyIconW(NIM_DELETE, &nid)) {
            nid.uFlags = 0;
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
    }
    tray_installed_ = false;
    ReleaseTrayOwnership();
    return true;
}

void StatusWindow::UninstallTrayIcon() {
    if (hwnd_ && tray_installed_) {
        NOTIFYICONDATAW nid {};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = hwnd_;
        nid.uID    = kTrayIconId;
        nid.uFlags = NIF_GUID;
        nid.guidItem = kTrayIconGuid;
        if (!Shell_NotifyIconW(NIM_DELETE, &nid)) {
            nid.uFlags = 0;
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
    }
    tray_installed_ = false;
    ReleaseTrayOwnership();
}

bool StatusWindow::TryAcquireTrayOwnership() {
    if (tray_owner_mutex_) return true;
    HANDLE mutex = CreateTrayOwnerMutex();
    if (!mutex) return false;
    const DWORD wait = WaitForSingleObject(mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return false;
    }
    tray_owner_mutex_ = mutex;
    tray_owner_thread_id_ = GetCurrentThreadId();
    return true;
}

void StatusWindow::ReleaseTrayOwnership() noexcept {
    if (!tray_owner_mutex_) return;
    if (tray_owner_thread_id_ == GetCurrentThreadId()) {
        ReleaseMutex(tray_owner_mutex_);
    }
    CloseHandle(tray_owner_mutex_);
    tray_owner_mutex_ = nullptr;
    tray_owner_thread_id_ = 0;
}

void StatusWindow::UpdateTrayIcon(const std::wstring& tray_text, bool english_mode) {
    if (!hwnd_ || !tray_installed_) return;
    HICON icon = CreateTextHIcon(tray_text.empty() ? L"财" : tray_text, english_mode);

    NOTIFYICONDATAW nid {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_GUID;
    nid.hIcon  = icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
    nid.guidItem = kTrayIconGuid;
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        nid.uFlags = NIF_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (icon) DestroyIcon(icon);
}

LRESULT StatusWindow::OnTrayIconMsg(LPARAM lparam) {
    const UINT event = LOWORD(lparam);
    if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
        if (!LaunchSettings()) {
            MessageBoxW(hwnd_, L"无法找到或启动 ShuruSettings.exe。请重新安装设置程序。",
                        L"财神输入法", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
    } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return 0;
        AppendMenuW(menu, MF_STRING, 1, L"输入法设置");
        POINT pt {};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        const UINT cmd = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (cmd == 1 && !LaunchSettings()) {
            MessageBoxW(hwnd_, L"无法找到或启动 ShuruSettings.exe。",
                        L"财神输入法", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
    }
    return 0;
}

// ---- SoftKeyboard ----

namespace {
const wchar_t kKeys[] = L"qwertyuiopasdfghjklzxcvbnm";  // 26
// extras: space back esc
}

SoftKeyboardWindow::~SoftKeyboardWindow() {
    Destroy();
}

bool SoftKeyboardWindow::Create(HINSTANCE instance) {
    if (hwnd_) {
        return true;
    }
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kSoftClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const int w = kCols * (kKeyW + kGap) + kGap + 8;
    const int h = kRows * (kKeyH + kGap) + kGap + 8;
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kSoftClass, L"",
        WS_POPUP,
        0, 0, w, h,
        nullptr, nullptr, instance, this);
    RecreateFont();
    return hwnd_ != nullptr;
}

void SoftKeyboardWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    visible_ = false;
}

void SoftKeyboardWindow::AbandonAfterOwnerThreadExit() noexcept {
    hwnd_ = nullptr;
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    visible_ = false;
    hover_ = -1;
}

int SoftKeyboardWindow::Scale(int value) const {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    return MulDiv(value, dpi ? static_cast<int>(dpi) : 96, 96);
}

void SoftKeyboardWindow::RecreateFont() {
    if (font_) {
        DeleteObject(font_);
    }
    font_ = CreateFontW(-Scale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

SIZE SoftKeyboardWindow::WindowSize() const {
    return SIZE {
        Scale(kCols * (kKeyW + kGap) + kGap + 8),
        Scale(kRows * (kKeyH + kGap) + kGap + 8)
    };
}

RECT SoftKeyboardWindow::KeyRect(int index) const {
    if (index < 0 || index >= KeyCount()) {
        return RECT {};
    }
    const int row = index < 26 ? index / kCols : 3;
    const int col = index < 26 ? index % kCols : (index == 26 ? 0 : (index == 27 ? 5 : 8));
    const int width = index == 26 ? kKeyW * 4 : (index >= 27 ? kKeyW * 2 : kKeyW);
    const int left = Scale(kGap + col * (kKeyW + kGap) + 4);
    const int top = Scale(kGap + row * (kKeyH + kGap) + 4);
    return RECT {left, top, left + Scale(width), top + Scale(kKeyH)};
}

int SoftKeyboardWindow::KeyCount() const {
    return 26 + 3;  // letters + space/back/esc
}

bool SoftKeyboardWindow::HitTest(int x, int y, int* index) const {
    if (!index) {
        return false;
    }
    for (int i = 0; i < KeyCount(); ++i) {
        const RECT key_rect = KeyRect(i);
        if (PtInRect(&key_rect, POINT {x, y})) {
            *index = i;
            return true;
        }
    }
    return false;
}

void SoftKeyboardWindow::EmitIndex(int index) {
    if (!on_key_) {
        return;
    }
    if (index >= 0 && index < 26) {
        on_key_(kKeys[index], false);
        return;
    }
    if (index == 26) {
        on_key_(L'S', true);
    } else if (index == 27) {
        on_key_(L'B', true);
    } else if (index == 28) {
        on_key_(L'E', true);
    }
}

void SoftKeyboardWindow::Show(const POINT& anchor) {
    if (!hwnd_) {
        return;
    }
    const SIZE size = WindowSize();
    const int w = size.cx;
    const int h = size.cy;
    int x = anchor.x;
    int y = anchor.y + Scale(8);
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        if (x + w > mi.rcWork.right) {
            x = mi.rcWork.right - w - Scale(8);
        }
        if (y + h > mi.rcWork.bottom) {
            y = anchor.y - h - Scale(8);
        }
        if (x < mi.rcWork.left) {
            x = mi.rcWork.left + Scale(8);
        }
        if (y < mi.rcWork.top) {
            y = mi.rcWork.top + Scale(8);
        }
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SoftKeyboardWindow::Hide() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = false;
}

LRESULT SoftKeyboardWindow::OnPaint() {
    PAINTSTRUCT ps {};
    HDC hdc = BeginPaint(hwnd_, &ps);
    if (hdc == nullptr) {
        return 0;
    }
    RECT rc {};
    GetClientRect(hwnd_, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    if (mem == nullptr || bmp == nullptr) {
        if (bmp != nullptr) DeleteObject(bmp);
        if (mem != nullptr) DeleteDC(mem);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(28, 30, 36));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    HFONT oldf = font_ ? static_cast<HFONT>(SelectObject(mem, font_)) : nullptr;
    SetTextColor(mem, RGB(240, 240, 240));

    auto draw_key = [&](int i, const wchar_t* label) {
        const RECT kr = KeyRect(i);
        const bool hot = (i == hover_);
        HBRUSH br = CreateSolidBrush(hot ? RGB(55, 120, 210) : RGB(52, 56, 66));
        FillRect(mem, &kr, br);
        DeleteObject(br);
        RECT draw_rect = kr;
        DrawTextW(mem, label, -1, &draw_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    for (int i = 0; i < 26; ++i) {
        wchar_t label[2] = {kKeys[i], 0};
        draw_key(i, label);
    }
    draw_key(26, L"空格");
    draw_key(27, L"⌫");
    draw_key(28, L"Esc");

    if (oldf) {
        SelectObject(mem, oldf);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
    return 0;
}

LRESULT CALLBACK SoftKeyboardWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    SoftKeyboardWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SoftKeyboardWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SoftKeyboardWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    switch (msg) {
    case WM_PAINT:
        return self->OnPaint();
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tracking {
            sizeof(tracking), TME_LEAVE, hwnd, 0
        };
        TrackMouseEvent(&tracking);
        int idx = -1;
        self->HitTest(static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)), &idx);
        if (idx != self->hover_) {
            self->hover_ = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int idx = -1;
        if (self->HitTest(static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)), &idx)) {
            self->EmitIndex(idx);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        self->hover_ = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        self->RecreateFont();
        const auto* rect = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top,
                     rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace shuru
