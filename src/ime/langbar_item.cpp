#include "langbar_item.h"

#include "../common/guid_def.h"

#include <ctffunc.h>
#include <olectl.h>
#include <algorithm>
#include <cwchar>

namespace shuru {

namespace {

constexpr DWORD kDefaultSinkCookie = 0x53485552; // "SHUR"

} // namespace

LangBarItemButton::LangBarItemButton() = default;

LangBarItemButton::~LangBarItemButton() {
    if (sink_ != nullptr) {
        sink_->Release();
        sink_ = nullptr;
    }
}

STDMETHODIMP LangBarItemButton::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) {
        return E_POINTER;
    }
    *ppvObj = nullptr;

    if (riid == IID_IUnknown || riid == IID_ITfLangBarItem || riid == IID_ITfLangBarItemButton) {
        *ppvObj = static_cast<ITfLangBarItemButton*>(this);
    } else if (riid == IID_ITfSource) {
        *ppvObj = static_cast<ITfSource*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) LangBarItemButton::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) LangBarItemButton::Release() {
    const LONG count = InterlockedDecrement(&ref_);
    if (count <= 0) {
        delete this;
        return 0;
    }
    return static_cast<ULONG>(count);
}

STDMETHODIMP LangBarItemButton::GetInfo(TF_LANGBARITEMINFO* pInfo) {
    if (pInfo == nullptr) {
        return E_INVALIDARG;
    }
    *pInfo = TF_LANGBARITEMINFO {};
    pInfo->clsidService = CLSID_ShuruTextService;
    // Windows 8 起仅会在任务栏接纳这个系统输入模式 GUID。
    pInfo->guidItem = GUID_LBI_INPUTMODE;
    pInfo->dwStyle = TF_LBI_STYLE_BTN_BUTTON |
        TF_LBI_STYLE_SHOWNINTRAY |
        TF_LBI_STYLE_TEXTCOLORICON;
    pInfo->ulSort = 0;
    wcscpy_s(pInfo->szDescription, ARRAYSIZE(pInfo->szDescription), L"输入模式");
    return S_OK;
}

STDMETHODIMP LangBarItemButton::GetStatus(DWORD* pdwStatus) {
    if (pdwStatus == nullptr) {
        return E_INVALIDARG;
    }
    *pdwStatus = 0;
    return S_OK;
}

STDMETHODIMP LangBarItemButton::Show(BOOL /*fShow*/) {
    // 该项始终可见，不声明 HIDDENSTATUSCONTROL。保持 S_OK 与
    // Windows SampleIME/小狼毫实现一致，避免语言栏把 E_NOTIMPL
    // 当作状态刷新失败。
    if (sink_ != nullptr) {
        sink_->OnUpdate(TF_LBI_STATUS);
    }
    return S_OK;
}

STDMETHODIMP LangBarItemButton::GetTooltipString(BSTR* pbstrToolTip) {
    if (pbstrToolTip == nullptr) {
        return E_INVALIDARG;
    }
    *pbstrToolTip = SysAllocString(
        english_mode_
            ? L"财神输入法 - 英文模式 (Shift 键切换)"
            : L"财神输入法 - 中文模式 (Shift 键切换)");
    return *pbstrToolTip != nullptr ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarItemButton::OnClick(TfLBIClick click, POINT /*pt*/, const RECT* /*prcArea*/) {
    if (click == TF_LBI_CLK_LEFT) {
        if (toggle_callback_) {
            toggle_callback_();
        }
        return S_OK;
    } else if (click == TF_LBI_CLK_RIGHT) {
        if (menu_callback_) {
            menu_callback_();
        }
        return S_OK;
    }
    return S_OK;
}

STDMETHODIMP LangBarItemButton::InitMenu(ITfMenu* /*pMenu*/) {
    return S_OK;
}

STDMETHODIMP LangBarItemButton::OnMenuSelect(UINT /*wID*/) {
    return S_OK;
}

STDMETHODIMP LangBarItemButton::GetIcon(HICON* phIcon) {
    if (phIcon == nullptr) {
        return E_INVALIDARG;
    }
    *phIcon = CreateModeIcon(english_mode_);
    return *phIcon != nullptr ? S_OK : E_FAIL;
}

STDMETHODIMP LangBarItemButton::GetText(BSTR* pbstrText) {
    if (pbstrText == nullptr) {
        return E_INVALIDARG;
    }
    *pbstrText = SysAllocString(english_mode_ ? L"英" : L"中");
    return *pbstrText != nullptr ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarItemButton::AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) {
    if (punk == nullptr || pdwCookie == nullptr) {
        return E_INVALIDARG;
    }
    if (riid != IID_ITfLangBarItemSink) {
        return CONNECT_E_CANNOTCONNECT;
    }
    if (sink_ != nullptr) {
        return CONNECT_E_ADVISELIMIT;
    }

    HRESULT hr = punk->QueryInterface(IID_ITfLangBarItemSink, reinterpret_cast<void**>(&sink_));
    if (FAILED(hr)) {
        return hr;
    }

    sink_cookie_ = kDefaultSinkCookie;
    *pdwCookie = sink_cookie_;
    return S_OK;
}

STDMETHODIMP LangBarItemButton::UnadviseSink(DWORD dwCookie) {
    if (dwCookie != sink_cookie_ || sink_ == nullptr) {
        return CONNECT_E_NOCONNECTION;
    }
    sink_->Release();
    sink_ = nullptr;
    sink_cookie_ = 0;
    return S_OK;
}

void LangBarItemButton::SetEnglishMode(bool english) {
    if (english_mode_ == english) {
        return;
    }
    english_mode_ = english;
    if (sink_ != nullptr) {
        sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TOOLTIP | TF_LBI_TEXT);
    }
}

void LangBarItemButton::Detach() noexcept {
    toggle_callback_ = nullptr;
    menu_callback_ = nullptr;
}

/*static*/
HICON LangBarItemButton::CreateModeIcon(bool english_mode, int icon_size) {
    int sz = icon_size;
    if (sz <= 0) {
        sz = GetSystemMetrics(SM_CXSMICON);
        if (sz <= 0) {
            sz = 16;
        }
    }
    sz = (std::max)(16, (std::min)(sz, 64));

    BITMAPV5HEADER bih {};
    bih.bV5Size        = sizeof(bih);
    bih.bV5Width       = sz;
    bih.bV5Height      = -sz; // top-down
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
    if (screen != nullptr) ReleaseDC(nullptr, screen);
    if (!color_bmp || !bits) {
        return nullptr;
    }

    HDC mem = CreateCompatibleDC(nullptr);
    if (mem == nullptr) {
        DeleteObject(color_bmp);
        return nullptr;
    }
    HGDIOBJ old_bmp = SelectObject(mem, color_bmp);
    if (old_bmp == nullptr) {
        DeleteDC(mem);
        DeleteObject(color_bmp);
        return nullptr;
    }

    // 先在黑底上用白色灰度字形渲染覆盖率，再转为
    // 透明背景的黑色 ARGB。TEXTCOLORICON 会把字形映射为系统文字色。
    HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
    if (br == nullptr) {
        SelectObject(mem, old_bmp);
        DeleteDC(mem);
        DeleteObject(color_bmp);
        return nullptr;
    }
    RECT rc {0, 0, sz, sz};
    FillRect(mem, &rc, br);
    DeleteObject(br);

    // 字体尺寸自适应
    const int font_h = -((sz * 13) / 16);
    HFONT font = CreateFontW(
        font_h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (font == nullptr) {
        SelectObject(mem, old_bmp);
        DeleteDC(mem);
        DeleteObject(color_bmp);
        return nullptr;
    }
    HGDIOBJ old_font = SelectObject(mem, font);
    if (old_font == nullptr) {
        DeleteObject(font);
        SelectObject(mem, old_bmp);
        DeleteDC(mem);
        DeleteObject(color_bmp);
        return nullptr;
    }

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    const wchar_t* text = english_mode ? L"英" : L"中";
    const int drawn = DrawTextW(
        mem, text, 1, &rc,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(mem, old_font);
    if (drawn == 0) {
        DeleteObject(font);
        SelectObject(mem, old_bmp);
        DeleteDC(mem);
        DeleteObject(color_bmp);
        return nullptr;
    }

    // GDI 不会写入 DIB Alpha，以灰度值还原字形覆盖率。
    DWORD* px = static_cast<DWORD*>(bits);
    const int total_pixels = sz * sz;
    for (int i = 0; i < total_pixels; ++i) {
        const BYTE blue = static_cast<BYTE>(px[i] & 0xFF);
        const BYTE green = static_cast<BYTE>((px[i] >> 8) & 0xFF);
        const BYTE red = static_cast<BYTE>((px[i] >> 16) & 0xFF);
        const BYTE alpha = (std::max)(red, (std::max)(green, blue));
        px[i] = static_cast<DWORD>(alpha) << 24;
    }

    SelectObject(mem, old_bmp);
    DeleteDC(mem);

    HBITMAP mask_bmp = CreateBitmap(sz, sz, 1, 1, nullptr);
    if (mask_bmp == nullptr) {
        DeleteObject(font);
        DeleteObject(color_bmp);
        return nullptr;
    }
    HDC mask_dc = CreateCompatibleDC(nullptr);
    if (mask_dc == nullptr) {
        DeleteObject(font);
        DeleteObject(mask_bmp);
        DeleteObject(color_bmp);
        return nullptr;
    }
    HGDIOBJ old_mask = SelectObject(mask_dc, mask_bmp);
    if (old_mask == nullptr) {
        DeleteDC(mask_dc);
        DeleteObject(font);
        DeleteObject(mask_bmp);
        DeleteObject(color_bmp);
        return nullptr;
    }
    PatBlt(mask_dc, 0, 0, sz, sz, WHITENESS);
    HGDIOBJ mask_font = SelectObject(mask_dc, font);
    if (mask_font == nullptr) {
        SelectObject(mask_dc, old_mask);
        DeleteDC(mask_dc);
        DeleteObject(font);
        DeleteObject(mask_bmp);
        DeleteObject(color_bmp);
        return nullptr;
    }
    SetBkMode(mask_dc, TRANSPARENT);
    SetTextColor(mask_dc, RGB(0, 0, 0));
    const int mask_drawn = DrawTextW(
        mask_dc, text, 1, &rc,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mask_dc, mask_font);
    SelectObject(mask_dc, old_mask);
    DeleteDC(mask_dc);
    DeleteObject(font);
    if (mask_drawn == 0) {
        DeleteObject(mask_bmp);
        DeleteObject(color_bmp);
        return nullptr;
    }
    ICONINFO ii {};
    ii.fIcon    = TRUE;
    ii.hbmColor = color_bmp;
    ii.hbmMask  = mask_bmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color_bmp);
    DeleteObject(mask_bmp);
    return icon;
}

} // namespace shuru
