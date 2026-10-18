// NovaChangJie text service - candidate list: a TSF UI element (so games and
// immersive apps can draw it themselves) plus our own Windows 11 style popup.
#pragma once

#include "Globals.h"

#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <vector>

#include "Engine.h"

namespace nova {

class TextService;

class CandidateUI final : public ITfCandidateListUIElementBehavior {
public:
    explicit CandidateUI(TextService* owner);

    void SetOptions(const Options& options);
    // Shows, refreshes or hides the list to match the engine. `anchor` is the
    // composition rectangle in screen coordinates (nullptr keeps the old one).
    void Update(const Engine& engine, const RECT* anchor, ITfContext* context);
    void Move(const RECT& anchor);
    void SetThreadFocus(bool focused);
    bool IsVisible() const { return elementBegun_; }
    void Destroy();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfUIElement
    STDMETHODIMP GetDescription(BSTR* pbstrDescription) override;
    STDMETHODIMP GetGUID(GUID* pguid) override;
    STDMETHODIMP Show(BOOL bShow) override;
    STDMETHODIMP IsShown(BOOL* pbShow) override;

    // ITfCandidateListUIElement
    STDMETHODIMP GetUpdatedFlags(DWORD* pdwFlags) override;
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** ppdim) override;
    STDMETHODIMP GetCount(UINT* puCount) override;
    STDMETHODIMP GetSelection(UINT* puIndex) override;
    STDMETHODIMP GetString(UINT uIndex, BSTR* pstr) override;
    STDMETHODIMP GetPageIndex(UINT* pIndex, UINT uSize, UINT* puPageCnt) override;
    STDMETHODIMP SetPageIndex(UINT* pIndex, UINT uPageCnt) override;
    STDMETHODIMP GetCurrentPage(UINT* puPage) override;

    // ITfCandidateListUIElementBehavior
    STDMETHODIMP SetSelection(UINT nIndex) override;
    STDMETHODIMP Finalize() override;
    STDMETHODIMP Abort() override;

private:
    ~CandidateUI();

    struct Item {
        std::wstring label;
        std::wstring text;
        std::wstring hint;
        float labelWidth = 0, textWidth = 0, hintWidth = 0;
        D2D1_RECT_F rect = {};  // Item rectangle in DIPs (for hit testing).
    };
    struct Palette {
        D2D1_COLOR_F background, border, text, selectedText, secondary, hint, selection, accent;
    };

    void EndElement();
    void ShowWindowIfAllowed();
    void HideWindow();
    bool EnsureWindow();
    bool EnsureDeviceResources();
    void DiscardDeviceResources();
    bool EnsureTextFormats();
    void LayoutItems();
    void PositionWindow();
    void Render();
    void OnClick(POINT pt);
    Palette CurrentPalette() const;
    float Scale() const;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    std::atomic<ULONG> refs_{1};
    TextService* owner_ = nullptr;
    Options options_;

    // Snapshot of the engine's list.
    std::vector<std::wstring> all_;  // Every candidate, for UI element clients.
    std::vector<Item> page_;
    size_t selection_ = 0;
    size_t pageStart_ = 0;
    size_t pageSize_ = 9;
    size_t pageCount_ = 0;
    ComPtr<ITfDocumentMgr> documentMgr_;

    // UI element state.
    DWORD elementId_ = static_cast<DWORD>(-1);
    bool elementBegun_ = false;
    bool showOwnWindow_ = true;
    bool threadFocused_ = true;

    // Window and rendering.
    HWND hwnd_ = nullptr;
    RECT anchor_ = {};
    bool haveAnchor_ = false;
    D2D1_SIZE_F size_ = {};  // Content size in DIPs.
    D2D1_RECT_F prevRect_ = {}, nextRect_ = {};
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<IDWriteTextFormat> textFormat_, labelFormat_, hintFormat_;
    float formatFontSize_ = 0;
    bool dwmRounded_ = false;
};

// Called from DllMain on unload.
void UnregisterCandidateWindowClass();

}  // namespace nova
