#include "ime/langbar_item.h"
#include "common/guid_def.h"

#include <ctffunc.h>
#include <ctfutb.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "[FAIL] " #condition " at line " << __LINE__        \
                      << std::endl;                                            \
            std::exit(1);                                                      \
        }                                                                      \
    } while (false)

namespace {

class MockLangBarItemSink : public ITfLangBarItemSink {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ITfLangBarItemSink) {
            *ppvObj = static_cast<ITfLangBarItemSink*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++ref_count_;
    }

    STDMETHODIMP_(ULONG) Release() override {
        return --ref_count_;
    }

    STDMETHODIMP OnUpdate(DWORD dwFlags) override {
        last_update_flags_ = dwFlags;
        update_count_++;
        return S_OK;
    }

    ULONG ref_count_ = 1;
    DWORD last_update_flags_ = 0;
    int update_count_ = 0;
};

bool HasTransparentAndVisiblePixels(HICON icon) {
    ICONINFO info {};
    if (icon == nullptr || !GetIconInfo(icon, &info)) return false;

    BITMAP bitmap {};
    bool valid = info.hbmColor != nullptr &&
        GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap) &&
        bitmap.bmWidth > 0 && bitmap.bmHeight > 0;
    bool transparent = false;
    bool visible = false;
    if (valid) {
        BITMAPINFO dib {};
        dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        dib.bmiHeader.biWidth = bitmap.bmWidth;
        dib.bmiHeader.biHeight = -bitmap.bmHeight;
        dib.bmiHeader.biPlanes = 1;
        dib.bmiHeader.biBitCount = 32;
        dib.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint32_t> pixels(
            static_cast<size_t>(bitmap.bmWidth) * bitmap.bmHeight);
        HDC dc = GetDC(nullptr);
        valid = dc != nullptr && GetDIBits(
            dc, info.hbmColor, 0, static_cast<UINT>(bitmap.bmHeight),
            pixels.data(), &dib, DIB_RGB_COLORS) != 0;
        if (dc != nullptr) ReleaseDC(nullptr, dc);
        if (valid) {
            for (const std::uint32_t pixel : pixels) {
                const BYTE alpha = static_cast<BYTE>(pixel >> 24);
                transparent = transparent || alpha == 0;
                visible = visible || alpha != 0;
            }
        }
    }
    if (info.hbmColor != nullptr) DeleteObject(info.hbmColor);
    if (info.hbmMask != nullptr) DeleteObject(info.hbmMask);
    return valid && transparent && visible;
}

void TestQueryInterfaceAndRefCounting() {
    auto* item = new shuru::LangBarItemButton();
    CHECK(item != nullptr);

    IUnknown* punk = nullptr;
    HRESULT hr = item->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&punk));
    CHECK(SUCCEEDED(hr) && punk != nullptr);
    punk->Release();

    ITfLangBarItem* plbi = nullptr;
    hr = item->QueryInterface(IID_ITfLangBarItem, reinterpret_cast<void**>(&plbi));
    CHECK(SUCCEEDED(hr) && plbi != nullptr);
    plbi->Release();

    ITfLangBarItemButton* pbtn = nullptr;
    hr = item->QueryInterface(IID_ITfLangBarItemButton, reinterpret_cast<void**>(&pbtn));
    CHECK(SUCCEEDED(hr) && pbtn != nullptr);
    pbtn->Release();

    ITfSource* psource = nullptr;
    hr = item->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&psource));
    CHECK(SUCCEEDED(hr) && psource != nullptr);
    psource->Release();

    IUnknown* pbad = nullptr;
    hr = item->QueryInterface(IID_ITfTextInputProcessor, reinterpret_cast<void**>(&pbad));
    CHECK(FAILED(hr) && pbad == nullptr);

    item->Release();
    std::cout << "[PASS] TestQueryInterfaceAndRefCounting" << std::endl;
}

void TestGetInfoAndStatus() {
    auto* item = new shuru::LangBarItemButton();

    TF_LANGBARITEMINFO info {};
    HRESULT hr = item->GetInfo(&info);
    CHECK(SUCCEEDED(hr));
    CHECK(IsEqualGUID(info.clsidService, CLSID_ShuruTextService));
    CHECK(IsEqualGUID(info.guidItem, GUID_LBI_INPUTMODE));
    CHECK((info.dwStyle & TF_LBI_STYLE_BTN_BUTTON) != 0);
    CHECK((info.dwStyle & TF_LBI_STYLE_SHOWNINTRAY) != 0);
    CHECK((info.dwStyle & TF_LBI_STYLE_TEXTCOLORICON) != 0);
    CHECK(wcslen(info.szDescription) > 0);

    DWORD status = 999;
    hr = item->GetStatus(&status);
    CHECK(SUCCEEDED(hr));
    CHECK(status == 0);

    hr = item->Show(TRUE);
    CHECK(SUCCEEDED(hr));

    item->Release();
    std::cout << "[PASS] TestGetInfoAndStatus" << std::endl;
}

void TestTextAndTooltip() {
    auto* item = new shuru::LangBarItemButton();

    BSTR text = nullptr;
    HRESULT hr = item->GetText(&text);
    CHECK(SUCCEEDED(hr) && text != nullptr);
    CHECK(wcscmp(text, L"中") == 0);
    SysFreeString(text);

    BSTR tip = nullptr;
    hr = item->GetTooltipString(&tip);
    CHECK(SUCCEEDED(hr) && tip != nullptr);
    CHECK(wcsstr(tip, L"中文模式") != nullptr);
    SysFreeString(tip);

    item->SetEnglishMode(true);

    text = nullptr;
    hr = item->GetText(&text);
    CHECK(SUCCEEDED(hr) && text != nullptr);
    CHECK(wcscmp(text, L"英") == 0);
    SysFreeString(text);

    tip = nullptr;
    hr = item->GetTooltipString(&tip);
    CHECK(SUCCEEDED(hr) && tip != nullptr);
    CHECK(wcsstr(tip, L"英文模式") != nullptr);
    SysFreeString(tip);

    item->Release();
    std::cout << "[PASS] TestTextAndTooltip" << std::endl;
}

void TestIcons() {
    auto* item = new shuru::LangBarItemButton();

    HICON icon_zh = nullptr;
    HRESULT hr = item->GetIcon(&icon_zh);
    CHECK(SUCCEEDED(hr) && icon_zh != nullptr);
    CHECK(HasTransparentAndVisiblePixels(icon_zh));
    DestroyIcon(icon_zh);

    item->SetEnglishMode(true);
    HICON icon_en = nullptr;
    hr = item->GetIcon(&icon_en);
    CHECK(SUCCEEDED(hr) && icon_en != nullptr);
    DestroyIcon(icon_en);

    HICON custom_sz = shuru::LangBarItemButton::CreateModeIcon(false, 32);
    CHECK(custom_sz != nullptr);
    DestroyIcon(custom_sz);

    item->Release();
    std::cout << "[PASS] TestIcons" << std::endl;
}

void TestSinkNotifications() {
    auto* item = new shuru::LangBarItemButton();
    MockLangBarItemSink sink;

    DWORD cookie = 0;
    HRESULT hr = item->AdviseSink(IID_ITfLangBarItemSink, &sink, &cookie);
    CHECK(SUCCEEDED(hr) && cookie != 0);

    CHECK(sink.update_count_ == 0);

    item->SetEnglishMode(true);
    CHECK(sink.update_count_ == 1);
    CHECK((sink.last_update_flags_ & TF_LBI_ICON) != 0);
    CHECK((sink.last_update_flags_ & TF_LBI_TOOLTIP) != 0);
    CHECK((sink.last_update_flags_ & TF_LBI_TEXT) != 0);

    // 相同模式不重复触发
    item->SetEnglishMode(true);
    CHECK(sink.update_count_ == 1);

    item->SetEnglishMode(false);
    CHECK(sink.update_count_ == 2);

    hr = item->UnadviseSink(cookie);
    CHECK(SUCCEEDED(hr));

    item->SetEnglishMode(true);
    CHECK(sink.update_count_ == 2); // 已经 Unadvise，不应再递增

    item->Release();
    std::cout << "[PASS] TestSinkNotifications" << std::endl;
}

void TestClickToggle() {
    auto* item = new shuru::LangBarItemButton();
    bool toggled = false;
    item->SetToggleCallback([&toggled]() {
        toggled = true;
    });

    POINT pt {0, 0};
    RECT rc {0, 0, 16, 16};
    HRESULT hr = item->OnClick(TF_LBI_CLK_LEFT, pt, &rc);
    CHECK(SUCCEEDED(hr));
    CHECK(toggled);

    item->Detach();
    toggled = false;
    hr = item->OnClick(TF_LBI_CLK_LEFT, pt, &rc);
    CHECK(SUCCEEDED(hr));
    CHECK(!toggled);

    item->Release();
    std::cout << "[PASS] TestClickToggle" << std::endl;
}

} // namespace

int main() {
    TestQueryInterfaceAndRefCounting();
    TestGetInfoAndStatus();
    TestTextAndTooltip();
    TestIcons();
    TestSinkNotifications();
    TestClickToggle();

    std::cout << "All langbar_item tests PASSED!" << std::endl;
    return 0;
}
