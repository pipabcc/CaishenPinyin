#include "langbar_item.h"

#include "../common/guid_def.h"

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
    pInfo->clsidService = CLSID_ShuruTextService;
    pInfo->guidItem = GUID_ShuruLangBarItem_Mode;
    pInfo->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
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
    ReleaseDC(nullptr, screen);
    if (!color_bmp || !bits) {
        return nullptr;
    }

    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old_bmp = SelectObject(mem, color_bmp);

    // 背景：深灰/黑底色
    const COLORREF bg = RGB(32, 36, 44);
    HBRUSH br = CreateSolidBrush(bg);
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
    HGDIOBJ old_font = SelectObject(mem, font);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    const wchar_t* text = english_mode ? L"英" : L"中";
    DrawTextW(mem, text, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(mem, old_font);
    DeleteObject(font);

    // 将所有像素 Alpha 置为 0xFF
    DWORD* px = static_cast<DWORD*>(bits);
    const int total_pixels = sz * sz;
    for (int i = 0; i < total_pixels; ++i) {
        px[i] |= 0xFF000000;
    }

    SelectObject(mem, old_bmp);
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

} // namespace shuru
