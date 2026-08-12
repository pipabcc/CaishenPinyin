#pragma once

#include <msctf.h>

#include "../common/guid_def.h"

namespace shuru {

class DisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
    DisplayAttributeInfo();
    virtual ~DisplayAttributeInfo() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetGUID(GUID* pguid) override;
    STDMETHODIMP GetDescription(BSTR* pbstrDesc) override;
    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) override;
    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE* pda) override;
    STDMETHODIMP Reset() override;

private:
    LONG ref_ = 1;
    TF_DISPLAYATTRIBUTE attr_ {};
    void InitDefault();
};

class EnumDisplayAttributeInfo : public IEnumTfDisplayAttributeInfo {
public:
    EnumDisplayAttributeInfo();
    explicit EnumDisplayAttributeInfo(ULONG index);
    virtual ~EnumDisplayAttributeInfo() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo, ULONG* pcFetched) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Skip(ULONG ulCount) override;

private:
    LONG ref_ = 1;
    ULONG index_ = 0;
};

bool ApplyCompositionDisplayAttribute(ITfContext* context, TfEditCookie ec, ITfRange* range, TfGuidAtom atom);
bool ClearCompositionDisplayAttribute(ITfContext* context, TfEditCookie ec, ITfRange* range);
TfGuidAtom RegisterDisplayAttributeAtom();

}  // namespace shuru
