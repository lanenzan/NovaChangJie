#include "DisplayAttribute.h"

#include <oleauto.h>

namespace nova {

namespace {

// Dotted underline in the text colour, like the inbox Chinese IMEs.
const TF_DISPLAYATTRIBUTE kInputAttribute = {
    {TF_CT_NONE, 0},  // crText
    {TF_CT_NONE, 0},  // crBk
    TF_LS_DOT,        // lsStyle
    FALSE,            // fBoldLine
    {TF_CT_NONE, 0},  // crLine
    TF_ATTR_INPUT,    // bAttr
};

}  // namespace

DisplayAttributeInfo::DisplayAttributeInfo() { DllAddRef(); }
DisplayAttributeInfo::~DisplayAttributeInfo() { DllRelease(); }

STDMETHODIMP DisplayAttributeInfo::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_ITfDisplayAttributeInfo) {
        *ppv = static_cast<ITfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DisplayAttributeInfo::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) DisplayAttributeInfo::Release() {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP DisplayAttributeInfo::GetGUID(GUID* pguid) {
    if (!pguid) return E_INVALIDARG;
    *pguid = GUID_NovaDisplayAttributeInput;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::GetDescription(BSTR* pbstrDesc) {
    if (!pbstrDesc) return E_INVALIDARG;
    *pbstrDesc = SysAllocString(L"NovaChangJie Input");
    return *pbstrDesc ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP DisplayAttributeInfo::GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) {
    if (!pda) return E_INVALIDARG;
    *pda = kInputAttribute;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) { return E_NOTIMPL; }
STDMETHODIMP DisplayAttributeInfo::Reset() { return S_OK; }

EnumDisplayAttributeInfo::EnumDisplayAttributeInfo() { DllAddRef(); }
EnumDisplayAttributeInfo::~EnumDisplayAttributeInfo() { DllRelease(); }

STDMETHODIMP EnumDisplayAttributeInfo::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_IEnumTfDisplayAttributeInfo) {
        *ppv = static_cast<IEnumTfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::Release() {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP EnumDisplayAttributeInfo::Clone(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum) return E_INVALIDARG;
    auto* clone = new (std::nothrow) EnumDisplayAttributeInfo();
    if (!clone) return E_OUTOFMEMORY;
    clone->index_ = index_;
    *ppEnum = clone;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo, ULONG* pcFetched) {
    if (!rgInfo) return E_INVALIDARG;
    ULONG fetched = 0;
    // We provide exactly one attribute.
    while (fetched < ulCount && index_ < 1) {
        auto* info = new (std::nothrow) DisplayAttributeInfo();
        if (!info) break;
        rgInfo[fetched++] = info;
        ++index_;
    }
    if (pcFetched) *pcFetched = fetched;
    return fetched == ulCount ? S_OK : S_FALSE;
}

STDMETHODIMP EnumDisplayAttributeInfo::Reset() {
    index_ = 0;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Skip(ULONG ulCount) {
    if (ulCount > 0 && index_ == 0) {
        index_ = 1;
        return ulCount == 1 ? S_OK : S_FALSE;
    }
    return ulCount == 0 ? S_OK : S_FALSE;
}

}  // namespace nova
