#pragma once

#include <Windows.h>
#include <msctf.h>
#include <ctfutb.h>
#include <functional>

namespace shuru {

// TSF 语言栏状态按钮：在系统任务栏通知区/输入法托盘呈现“中”/“英”状态图标，
// 支持实时模式更新与鼠标左键点击切换，完全与具体宿主服务解耦。
class LangBarItemButton :
    public ITfLangBarItemButton,
    public ITfSource {
public:
    using ModeToggleCallback = std::function<void()>;
    using MenuCallback = std::function<void()>;

    LangBarItemButton();
    virtual ~LangBarItemButton();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfLangBarItem
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* pInfo) override;
    STDMETHODIMP GetStatus(DWORD* pdwStatus) override;
    STDMETHODIMP Show(BOOL fShow) override;
    STDMETHODIMP GetTooltipString(BSTR* pbstrToolTip) override;

    // ITfLangBarItemButton
    STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT* prcArea) override;
    STDMETHODIMP InitMenu(ITfMenu* pMenu) override;
    STDMETHODIMP OnMenuSelect(UINT wID) override;
    STDMETHODIMP GetIcon(HICON* phIcon) override;
    STDMETHODIMP GetText(BSTR* pbstrText) override;

    // ITfSource
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) override;
    STDMETHODIMP UnadviseSink(DWORD dwCookie) override;

    // 状态与回调接口
    void SetEnglishMode(bool english);
    bool IsEnglishMode() const noexcept { return english_mode_; }
    void SetToggleCallback(ModeToggleCallback callback) { toggle_callback_ = std::move(callback); }
    void SetMenuCallback(MenuCallback callback) { menu_callback_ = std::move(callback); }
    void Detach() noexcept;

    // 生成“中”或“英”模式图标
    static HICON CreateModeIcon(bool english_mode, int icon_size = 0);

private:
    LONG ref_ = 1;
    bool english_mode_ = false;
    ModeToggleCallback toggle_callback_;
    MenuCallback menu_callback_;
    ITfLangBarItemSink* sink_ = nullptr;
    DWORD sink_cookie_ = 0;
};

} // namespace shuru
