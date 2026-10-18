// NovaChangJie text service - the input mode button (GUID_LBI_INPUTMODE).
// Windows 11 shows it in the taskbar as the 中 / 英 indicator.
#pragma once

#include "Globals.h"

#include <ctfutb.h>

namespace nova {

class TextService;

class LangBarButton final : public ITfLangBarItemButton, public ITfSource {
public:
    explicit LangBarButton(TextService* owner);

    void Update();  // Mode changed: refresh icon, text and tooltip.
    void Detach();  // Owner is going away.

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
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

private:
    ~LangBarButton();
    void ShowMenu(POINT pt);
    const wchar_t* ModeGlyph() const;
    bool FullMoon() const;
    void RunCommand(UINT id);

    std::atomic<ULONG> refs_{1};
    TextService* owner_ = nullptr;
    ComPtr<ITfLangBarItemSink> sink_;
    HWND menuOwner_ = nullptr;
};

}  // namespace nova
