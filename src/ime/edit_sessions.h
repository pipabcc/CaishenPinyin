#pragma once

#include <Windows.h>
#include <msctf.h>
#include <functional>
#include <string>

#include "input_policy.h"

namespace shuru {

HRESULT ReadContextInputScopePrivacy(
    ITfContext* context,
    TfEditCookie edit_cookie,
    InputScopePrivacy* privacy);

class InsertTextEditSession : public ITfEditSession {
public:
    InsertTextEditSession(ITfContext* context, TfClientId client_id, ITfComposition** composition, const std::wstring& text);
    virtual ~InsertTextEditSession();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    LONG ref_ = 1;
    ITfContext* context_ = nullptr;
    TfClientId client_id_ = TF_CLIENTID_NULL;
    ITfComposition** composition_ = nullptr;
    std::wstring text_;
};

class SetCompositionEditSession : public ITfEditSession {
public:
    SetCompositionEditSession(
        ITfContext* context, TfClientId client_id, ITfCompositionSink* sink,
        ITfComposition** composition, const std::wstring& text,
        TfGuidAtom display_atom = TF_INVALID_GUIDATOM,
        bool move_caret_to_end = true);
    virtual ~SetCompositionEditSession();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    LONG ref_ = 1;
    ITfContext* context_ = nullptr;
    TfClientId client_id_ = TF_CLIENTID_NULL;
    ITfCompositionSink* sink_ = nullptr;
    ITfComposition** composition_ = nullptr;
    std::wstring text_;
    TfGuidAtom display_atom_ = TF_INVALID_GUIDATOM;
    bool move_caret_to_end_ = true;
};

enum class ExistingTextCompositionResult {
    Adopted,
    StaleRequest,
    SensitiveContext,
    RangeChanged,
    SelectionChanged,
    CompositionActive,
    Failed,
};

// 将宿主已经写入的单字符范围直接纳入 TSF 组合，不删除或重插该字符。
class AdoptExistingTextEditSession : public ITfEditSession {
public:
    using Preflight = std::function<bool()>;
    using Completion = std::function<void(ExistingTextCompositionResult)>;

    AdoptExistingTextEditSession(
        ITfContext* context,
        ITfCompositionSink* sink,
        ITfRange* range,
        wchar_t expected_character,
        ITfComposition** composition,
        TfGuidAtom display_atom,
        Preflight preflight,
        Completion completion);
    virtual ~AdoptExistingTextEditSession();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    HRESULT Finish(ExistingTextCompositionResult result, HRESULT hr);

    LONG ref_ = 1;
    ITfContext* context_ = nullptr;
    ITfCompositionSink* sink_ = nullptr;
    ITfRange* range_ = nullptr;
    wchar_t expected_character_ = 0;
    ITfComposition** composition_ = nullptr;
    TfGuidAtom display_atom_ = TF_INVALID_GUIDATOM;
    Preflight preflight_;
    Completion completion_;
    bool completed_ = false;
};

class EndCompositionEditSession : public ITfEditSession {
public:
    explicit EndCompositionEditSession(ITfComposition** composition);
    virtual ~EndCompositionEditSession();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    LONG ref_ = 1;
    ITfComposition** composition_ = nullptr;
};

// 只读会话：用合法 edit cookie 取组合/选区屏幕矩形
class GetTextExtEditSession : public ITfEditSession {
public:
    GetTextExtEditSession(ITfContext* context, ITfComposition* composition, RECT* out_rect, bool* out_ok);
    virtual ~GetTextExtEditSession();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    LONG ref_ = 1;
    ITfContext* context_ = nullptr;
    ITfComposition* composition_ = nullptr;
    RECT* out_rect_ = nullptr;
    bool* out_ok_ = nullptr;
};

// 只读会话：获取宿主 InputScope（密码框识别）
class GetInputScopeEditSession : public ITfEditSession {
public:
    GetInputScopeEditSession(ITfContext* context, InputScopePrivacy* out_privacy);
    virtual ~GetInputScopeEditSession();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    LONG ref_ = 1;
    ITfContext* context_ = nullptr;
    InputScopePrivacy* out_privacy_ = nullptr;
};

}  // namespace shuru
