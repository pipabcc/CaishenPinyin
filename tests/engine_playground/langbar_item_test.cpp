#include "ime/langbar_item.h"
#include "common/guid_def.h"

#include <ctfutb.h>
#include <cassert>
#include <iostream>

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

void TestQueryInterfaceAndRefCounting() {
    auto* item = new shuru::LangBarItemButton();
    assert(item != nullptr);

    IUnknown* punk = nullptr;
    HRESULT hr = item->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&punk));
    assert(SUCCEEDED(hr) && punk != nullptr);
    punk->Release();

    ITfLangBarItem* plbi = nullptr;
    hr = item->QueryInterface(IID_ITfLangBarItem, reinterpret_cast<void**>(&plbi));
    assert(SUCCEEDED(hr) && plbi != nullptr);
    plbi->Release();

    ITfLangBarItemButton* pbtn = nullptr;
    hr = item->QueryInterface(IID_ITfLangBarItemButton, reinterpret_cast<void**>(&pbtn));
    assert(SUCCEEDED(hr) && pbtn != nullptr);
    pbtn->Release();

    ITfSource* psource = nullptr;
    hr = item->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&psource));
    assert(SUCCEEDED(hr) && psource != nullptr);
    psource->Release();

    IUnknown* pbad = nullptr;
    hr = item->QueryInterface(IID_ITfTextInputProcessor, reinterpret_cast<void**>(&pbad));
    assert(FAILED(hr) && pbad == nullptr);

    item->Release();
    std::cout << "[PASS] TestQueryInterfaceAndRefCounting" << std::endl;
}

void TestGetInfoAndStatus() {
    auto* item = new shuru::LangBarItemButton();

    TF_LANGBARITEMINFO info {};
    HRESULT hr = item->GetInfo(&info);
    assert(SUCCEEDED(hr));
    assert(IsEqualGUID(info.clsidService, CLSID_ShuruTextService));
    assert(IsEqualGUID(info.guidItem, GUID_ShuruLangBarItem_Mode));
    assert((info.dwStyle & TF_LBI_STYLE_BTN_BUTTON) != 0);
    assert((info.dwStyle & TF_LBI_STYLE_SHOWNINTRAY) != 0);
    assert(wcslen(info.szDescription) > 0);

    DWORD status = 999;
    hr = item->GetStatus(&status);
    assert(SUCCEEDED(hr));
    assert(status == 0);

    hr = item->Show(TRUE);
    assert(SUCCEEDED(hr));

    item->Release();
    std::cout << "[PASS] TestGetInfoAndStatus" << std::endl;
}

void TestTextAndTooltip() {
    auto* item = new shuru::LangBarItemButton();

    BSTR text = nullptr;
    HRESULT hr = item->GetText(&text);
    assert(SUCCEEDED(hr) && text != nullptr);
    assert(wcscmp(text, L"中") == 0);
    SysFreeString(text);

    BSTR tip = nullptr;
    hr = item->GetTooltipString(&tip);
    assert(SUCCEEDED(hr) && tip != nullptr);
    assert(wcsstr(tip, L"中文模式") != nullptr);
    SysFreeString(tip);

    item->SetEnglishMode(true);

    text = nullptr;
    hr = item->GetText(&text);
    assert(SUCCEEDED(hr) && text != nullptr);
    assert(wcscmp(text, L"英") == 0);
    SysFreeString(text);

    tip = nullptr;
    hr = item->GetTooltipString(&tip);
    assert(SUCCEEDED(hr) && tip != nullptr);
    assert(wcsstr(tip, L"英文模式") != nullptr);
    SysFreeString(tip);

    item->Release();
    std::cout << "[PASS] TestTextAndTooltip" << std::endl;
}

void TestIcons() {
    auto* item = new shuru::LangBarItemButton();

    HICON icon_zh = nullptr;
    HRESULT hr = item->GetIcon(&icon_zh);
    assert(SUCCEEDED(hr) && icon_zh != nullptr);
    DestroyIcon(icon_zh);

    item->SetEnglishMode(true);
    HICON icon_en = nullptr;
    hr = item->GetIcon(&icon_en);
    assert(SUCCEEDED(hr) && icon_en != nullptr);
    DestroyIcon(icon_en);

    HICON custom_sz = shuru::LangBarItemButton::CreateModeIcon(false, 32);
    assert(custom_sz != nullptr);
    DestroyIcon(custom_sz);

    item->Release();
    std::cout << "[PASS] TestIcons" << std::endl;
}

void TestSinkNotifications() {
    auto* item = new shuru::LangBarItemButton();
    MockLangBarItemSink sink;

    DWORD cookie = 0;
    HRESULT hr = item->AdviseSink(IID_ITfLangBarItemSink, &sink, &cookie);
    assert(SUCCEEDED(hr) && cookie != 0);

    assert(sink.update_count_ == 0);

    item->SetEnglishMode(true);
    assert(sink.update_count_ == 1);
    assert((sink.last_update_flags_ & TF_LBI_ICON) != 0);
    assert((sink.last_update_flags_ & TF_LBI_TOOLTIP) != 0);

    // 相同模式不重复触发
    item->SetEnglishMode(true);
    assert(sink.update_count_ == 1);

    item->SetEnglishMode(false);
    assert(sink.update_count_ == 2);

    hr = item->UnadviseSink(cookie);
    assert(SUCCEEDED(hr));

    item->SetEnglishMode(true);
    assert(sink.update_count_ == 2); // 已经 Unadvise，不应再递增

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
    assert(SUCCEEDED(hr));
    assert(toggled);

    item->Detach();
    toggled = false;
    hr = item->OnClick(TF_LBI_CLK_LEFT, pt, &rc);
    assert(SUCCEEDED(hr));
    assert(!toggled);

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
