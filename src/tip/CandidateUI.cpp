#include "CandidateUI.h"

#include <dwmapi.h>
#include <oleauto.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "TextService.h"

namespace nova {

namespace {

constexpr wchar_t kWindowClass[] = L"NovaChangJieCandidateWindow";
constexpr float kPadding = 5.0f;
constexpr float kGap = 8.0f;
constexpr float kArrowWidth = 22.0f;
std::atomic<bool> g_classRegistered{false};

D2D1_COLOR_F Rgb(UINT32 rgb, float alpha = 1.0f) {
    return D2D1::ColorF(rgb, alpha);
}

D2D1_COLOR_F FromColorRef(COLORREF c, float alpha = 1.0f) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, alpha);
}

DWORD ReadUserDword(const wchar_t* key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value;
    return fallback;
}

bool AppsUseDarkTheme() {
    return ReadUserDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", 1) == 0;
}

bool HighContrast() {
    HIGHCONTRASTW hc = {sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

D2D1_COLOR_F AccentColor(bool dark) {
    // DWM stores the accent as 0xAABBGGRR.
    DWORD abgr = ReadUserDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFC06700);
    float r = (abgr & 0xFF) / 255.0f, g = ((abgr >> 8) & 0xFF) / 255.0f, b = ((abgr >> 16) & 0xFF) / 255.0f;
    if (dark) {  // Lighten for contrast on a dark surface.
        r = r + (1 - r) * 0.45f;
        g = g + (1 - g) * 0.45f;
        b = b + (1 - b) * 0.45f;
    }
    return D2D1::ColorF(r, g, b, 1.0f);
}

float MeasureWidth(IDWriteFactory* factory, IDWriteTextFormat* format, const std::wstring& s) {
    if (s.empty()) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), format, 4096.0f, 4096.0f,
                                         &layout)))
        return 0;
    DWRITE_TEXT_METRICS m = {};
    layout->GetMetrics(&m);
    return std::ceil(m.widthIncludingTrailingWhitespace);
}

}  // namespace

void UnregisterCandidateWindowClass() {
    if (g_classRegistered.exchange(false)) UnregisterClassW(kWindowClass, g_hInstance);
}

CandidateUI::CandidateUI(TextService* owner) : owner_(owner) { DllAddRef(); }

CandidateUI::~CandidateUI() {
    if (hwnd_) DestroyWindow(hwnd_);
    DllRelease();
}

// ---------------------------------------------------------------------------
// IUnknown / ITfUIElement / ITfCandidateListUIElement(Behavior)

STDMETHODIMP CandidateUI::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_ITfUIElement || riid == IID_ITfCandidateListUIElement ||
        riid == IID_ITfCandidateListUIElementBehavior) {
        *ppv = static_cast<ITfCandidateListUIElementBehavior*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CandidateUI::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) CandidateUI::Release() {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP CandidateUI::GetDescription(BSTR* pbstrDescription) {
    if (!pbstrDescription) return E_INVALIDARG;
    *pbstrDescription = SysAllocString(L"NovaChangJie candidate list");
    return *pbstrDescription ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CandidateUI::GetGUID(GUID* pguid) {
    if (!pguid) return E_INVALIDARG;
    *pguid = GUID_NovaCandidateUIElement;
    return S_OK;
}

STDMETHODIMP CandidateUI::Show(BOOL bShow) {
    showOwnWindow_ = bShow != FALSE;
    if (showOwnWindow_)
        ShowWindowIfAllowed();
    else
        HideWindow();
    return S_OK;
}

STDMETHODIMP CandidateUI::IsShown(BOOL* pbShow) {
    if (!pbShow) return E_INVALIDARG;
    *pbShow = hwnd_ && IsWindowVisible(hwnd_);
    return S_OK;
}

STDMETHODIMP CandidateUI::GetUpdatedFlags(DWORD* pdwFlags) {
    if (!pdwFlags) return E_INVALIDARG;
    *pdwFlags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING |
                TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
    return S_OK;
}

STDMETHODIMP CandidateUI::GetDocumentMgr(ITfDocumentMgr** ppdim) {
    if (!ppdim) return E_INVALIDARG;
    *ppdim = documentMgr_.Get();
    if (*ppdim) (*ppdim)->AddRef();
    return *ppdim ? S_OK : E_FAIL;
}

STDMETHODIMP CandidateUI::GetCount(UINT* puCount) {
    if (!puCount) return E_INVALIDARG;
    *puCount = static_cast<UINT>(all_.size());
    return S_OK;
}

STDMETHODIMP CandidateUI::GetSelection(UINT* puIndex) {
    if (!puIndex) return E_INVALIDARG;
    *puIndex = static_cast<UINT>(selection_);
    return S_OK;
}

STDMETHODIMP CandidateUI::GetString(UINT uIndex, BSTR* pstr) {
    if (!pstr) return E_INVALIDARG;
    *pstr = nullptr;
    if (uIndex >= all_.size()) return E_INVALIDARG;
    *pstr = SysAllocStringLen(all_[uIndex].c_str(), static_cast<UINT>(all_[uIndex].size()));
    return *pstr ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CandidateUI::GetPageIndex(UINT* pIndex, UINT uSize, UINT* puPageCnt) {
    if (!puPageCnt) return E_INVALIDARG;
    *puPageCnt = static_cast<UINT>(pageCount_);
    if (pIndex) {
        if (uSize < pageCount_) return E_NOT_SUFFICIENT_BUFFER;
        for (size_t i = 0; i < pageCount_; ++i) pIndex[i] = static_cast<UINT>(i * pageSize_);
    }
    return S_OK;
}

STDMETHODIMP CandidateUI::SetPageIndex(UINT*, UINT) { return E_NOTIMPL; }  // Fixed page size.

STDMETHODIMP CandidateUI::GetCurrentPage(UINT* puPage) {
    if (!puPage) return E_INVALIDARG;
    *puPage = pageSize_ ? static_cast<UINT>(selection_ / pageSize_) : 0;
    return S_OK;
}

STDMETHODIMP CandidateUI::SetSelection(UINT nIndex) {
    if (!owner_ || nIndex >= all_.size()) return E_INVALIDARG;
    owner_->SetCandidateSelection(nIndex);
    return S_OK;
}

STDMETHODIMP CandidateUI::Finalize() {
    if (!owner_) return E_FAIL;
    ComPtr<CandidateUI> keep(this);
    owner_->SelectCandidate(selection_);
    return S_OK;
}

STDMETHODIMP CandidateUI::Abort() {
    if (!owner_) return E_FAIL;
    ComPtr<CandidateUI> keep(this);
    owner_->CancelComposition();
    return S_OK;
}

// ---------------------------------------------------------------------------
// Updates from the text service

void CandidateUI::SetOptions(const Options& options) {
    options_ = options;
    if (formatFontSize_ != static_cast<float>(options_.fontSize)) {
        textFormat_.Reset();
        labelFormat_.Reset();
        hintFormat_.Reset();
    }
}

void CandidateUI::Update(const Engine& engine, const RECT* anchor, ITfContext* context) {
    if (!owner_) return;
    if (anchor) {
        anchor_ = *anchor;
        haveAnchor_ = true;
    }
    if (!engine.CandidatesVisible()) {
        EndElement();
        HideWindow();
        all_.clear();
        page_.clear();
        haveAnchor_ = anchor != nullptr;
        return;
    }

    const auto& candidates = engine.Candidates();
    all_.clear();
    all_.reserve(candidates.size());
    for (const Candidate& c : candidates)
        all_.emplace_back(reinterpret_cast<const wchar_t*>(c.text.c_str()), c.text.size());
    selection_ = engine.Selection();
    pageSize_ = engine.PageSize();
    pageStart_ = engine.PageStart();
    pageCount_ = engine.PageCount();

    page_.clear();
    for (size_t i = pageStart_; i < candidates.size() && i < pageStart_ + pageSize_; ++i) {
        Item item;
        const std::u16string& label = candidates[i].label;
        item.label = label.empty() ? std::to_wstring(i - pageStart_ + 1)
                                   : std::wstring(reinterpret_cast<const wchar_t*>(label.c_str()), label.size());
        item.text = all_[i];
        for (char c : candidates[i].hint)
            item.hint += options_.showLetters ? static_cast<wchar_t>(c) : static_cast<wchar_t>(Engine::RadicalFor(c));
        page_.push_back(std::move(item));
    }

    documentMgr_.Reset();
    if (context) context->GetDocumentMgr(&documentMgr_);

    ComPtr<ITfUIElementMgr> mgr;
    if (owner_->ThreadMgr()) owner_->ThreadMgr()->QueryInterface(IID_PPV_ARGS(&mgr));
    if (!elementBegun_) {
        BOOL show = TRUE;
        elementId_ = static_cast<DWORD>(-1);
        if (mgr && FAILED(mgr->BeginUIElement(this, &show, &elementId_))) {
            elementId_ = static_cast<DWORD>(-1);
            show = TRUE;
        }
        elementBegun_ = true;
        showOwnWindow_ = show != FALSE;
    } else if (mgr && elementId_ != static_cast<DWORD>(-1)) {
        mgr->UpdateUIElement(elementId_);
    }
    ShowWindowIfAllowed();
}

void CandidateUI::Move(const RECT& anchor) {
    anchor_ = anchor;
    haveAnchor_ = true;
    if (hwnd_ && IsWindowVisible(hwnd_)) PositionWindow();
}

void CandidateUI::SetThreadFocus(bool focused) {
    threadFocused_ = focused;
    if (focused)
        ShowWindowIfAllowed();
    else
        HideWindow();
}

void CandidateUI::EndElement() {
    if (!elementBegun_) return;
    elementBegun_ = false;
    if (elementId_ != static_cast<DWORD>(-1) && owner_ && owner_->ThreadMgr()) {
        ComPtr<ITfUIElementMgr> mgr;
        if (SUCCEEDED(owner_->ThreadMgr()->QueryInterface(IID_PPV_ARGS(&mgr)))) mgr->EndUIElement(elementId_);
    }
    elementId_ = static_cast<DWORD>(-1);
}

void CandidateUI::Destroy() {
    EndElement();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    DiscardDeviceResources();
    documentMgr_.Reset();
    owner_ = nullptr;
}

// ---------------------------------------------------------------------------
// Window

bool CandidateUI::EnsureWindow() {
    if (hwnd_) return true;
    if (!g_classRegistered.load()) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) g_classRegistered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, L"", WS_POPUP, 0, 0,
                            1, 1, nullptr, nullptr, g_hInstance, this);
    if (!hwnd_) return false;
    // Windows 11: rounded corners and a theme-matched border (ignored on 10).
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
    dwmRounded_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner)));
    return true;
}

float CandidateUI::Scale() const {
    UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    return (dpi ? dpi : 96) / 96.0f;
}

bool CandidateUI::EnsureTextFormats() {
    if (!dwrite_ &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    if (textFormat_) return true;
    float size = static_cast<float>(options_.fontSize);
    auto make = [&](const wchar_t* family, float px, ComPtr<IDWriteTextFormat>& out) {
        if (FAILED(dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, px, L"zh-TW", &out)))
            return false;
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };
    if (!make(L"Microsoft JhengHei UI", size, textFormat_) ||
        !make(L"Segoe UI Variable Text", std::round(size * 0.72f), labelFormat_) ||
        !make(L"Microsoft JhengHei UI", std::round(size * 0.68f), hintFormat_)) {
        textFormat_.Reset();
        return false;
    }
    formatFontSize_ = size;
    return true;
}

void CandidateUI::LayoutItems() {
    if (!EnsureTextFormats()) return;
    const float rowH = std::ceil(options_.fontSize * 1.75f);
    const bool vertical = options_.layout == CandidateLayout::Vertical;
    float maxLabel = 0, maxText = 0, maxHint = 0;
    for (Item& it : page_) {
        it.labelWidth = MeasureWidth(dwrite_.Get(), labelFormat_.Get(), it.label);
        it.textWidth = MeasureWidth(dwrite_.Get(), textFormat_.Get(), it.text);
        it.hintWidth = MeasureWidth(dwrite_.Get(), hintFormat_.Get(), it.hint);
        maxLabel = std::max(maxLabel, it.labelWidth);
        maxText = std::max(maxText, it.textWidth);
        maxHint = std::max(maxHint, it.hintWidth);
    }
    const bool paged = pageCount_ > 1;
    if (vertical) {
        float rowW = 12 + maxLabel + kGap + std::max(maxText, options_.fontSize * 2.0f) +
                     (maxHint > 0 ? kGap + maxHint : 0) + 12;
        float y = kPadding;
        for (Item& it : page_) {
            it.rect = D2D1::RectF(kPadding, y, kPadding + rowW, y + rowH);
            y += rowH;
        }
        if (paged) {
            float footer = std::ceil(rowH * 0.8f);
            nextRect_ = D2D1::RectF(kPadding + rowW - kArrowWidth, y, kPadding + rowW, y + footer);
            prevRect_ = D2D1::RectF(nextRect_.left - kArrowWidth, y, nextRect_.left, y + footer);
            y += footer;
        }
        size_ = D2D1::SizeF(rowW + 2 * kPadding, y + kPadding);
    } else {
        float x = kPadding;
        for (Item& it : page_) {
            float w = 8 + it.labelWidth + 5 + it.textWidth + (it.hintWidth > 0 ? 5 + it.hintWidth : 0) + 8;
            it.rect = D2D1::RectF(x, kPadding, x + w, kPadding + rowH);
            x += w;
        }
        if (paged) {
            prevRect_ = D2D1::RectF(x + 4, kPadding, x + 4 + kArrowWidth, kPadding + rowH);
            nextRect_ = D2D1::RectF(prevRect_.right, kPadding, prevRect_.right + kArrowWidth, kPadding + rowH);
            x = nextRect_.right;
        }
        size_ = D2D1::SizeF(x + kPadding, rowH + 2 * kPadding);
    }
    if (!paged) prevRect_ = nextRect_ = D2D1::RectF();
}

void CandidateUI::PositionWindow() {
    if (!hwnd_) return;
    for (int pass = 0; pass < 2; ++pass) {
        const float s = Scale();
        const int w = static_cast<int>(std::ceil(size_.width * s));
        const int h = static_cast<int>(std::ceil(size_.height * s));

        POINT pt;
        LONG anchorTop;
        if (haveAnchor_) {
            pt = {anchor_.left, anchor_.bottom + 2};
            anchorTop = anchor_.top;
        } else {
            GUITHREADINFO gti = {sizeof(gti)};
            if (GetGUIThreadInfo(GetCurrentThreadId(), &gti) && gti.hwndCaret) {
                POINT tl = {gti.rcCaret.left, gti.rcCaret.top}, br = {gti.rcCaret.left, gti.rcCaret.bottom};
                ClientToScreen(gti.hwndCaret, &tl);
                ClientToScreen(gti.hwndCaret, &br);
                pt = {br.x, br.y + 2};
                anchorTop = tl.y;
            } else {
                GetCursorPos(&pt);
                pt.y += 20;
                anchorTop = pt.y - 20;
            }
        }

        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& work = mi.rcWork;
        if (pt.y + h > work.bottom) pt.y = anchorTop - h - 2;  // Flip above the text.
        pt.x = std::max(work.left, std::min(pt.x, work.right - w));
        pt.y = std::max(work.top, std::min(pt.y, work.bottom - h));

        SetWindowPos(hwnd_, HWND_TOPMOST, pt.x, pt.y, w, h, SWP_NOACTIVATE);
        if (Scale() == s) break;  // Moved to a monitor with another DPI: redo.
    }
    if (target_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        float dpi = Scale() * 96.0f;
        target_->SetDpi(dpi, dpi);
        target_->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
    }
}

void CandidateUI::ShowWindowIfAllowed() {
    if (!showOwnWindow_ || !threadFocused_ || page_.empty() || !elementBegun_) {
        HideWindow();
        return;
    }
    if (!EnsureWindow()) return;
    LayoutItems();
    PositionWindow();
    if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void CandidateUI::HideWindow() {
    if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

// ---------------------------------------------------------------------------
// Rendering

CandidateUI::Palette CandidateUI::CurrentPalette() const {
    Palette p;
    if (HighContrast()) {
        p.background = FromColorRef(GetSysColor(COLOR_WINDOW));
        p.border = FromColorRef(GetSysColor(COLOR_WINDOWTEXT));
        p.text = p.secondary = p.hint = FromColorRef(GetSysColor(COLOR_WINDOWTEXT));
        p.selection = FromColorRef(GetSysColor(COLOR_HIGHLIGHT));
        p.selectedText = FromColorRef(GetSysColor(COLOR_HIGHLIGHTTEXT));
        p.accent = p.selectedText;
        return p;
    }
    const bool dark = AppsUseDarkTheme();
    if (dark) {
        p.background = Rgb(0x2C2C2C);
        p.border = Rgb(0x454545);
        p.text = p.selectedText = Rgb(0xFFFFFF);
        p.secondary = Rgb(0xC8C8C8);
        p.hint = Rgb(0x9D9D9D);
        p.selection = Rgb(0xFFFFFF, 0.085f);
    } else {
        p.background = Rgb(0xF9F9F9);
        p.border = Rgb(0xDADADA);
        p.text = p.selectedText = Rgb(0x1A1A1A);
        p.secondary = Rgb(0x5C5C5C);
        p.hint = Rgb(0x8B8B8B);
        p.selection = Rgb(0x000000, 0.06f);
    }
    p.accent = AccentColor(dark);
    return p;
}

bool CandidateUI::EnsureDeviceResources() {
    if (target_) return true;
    if (!d2d_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float dpi = Scale() * 96.0f;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN), dpi, dpi);
    return SUCCEEDED(d2d_->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
        &target_));
}

void CandidateUI::DiscardDeviceResources() { target_.Reset(); }

void CandidateUI::Render() {
    if (!hwnd_ || page_.empty() || !EnsureTextFormats() || !EnsureDeviceResources()) return;
    const Palette p = CurrentPalette();
    const bool vertical = options_.layout == CandidateLayout::Vertical;

    // Keep the DWM border in step with the theme (Windows 11).
    COLORREF border = RGB(static_cast<int>(p.border.r * 255), static_cast<int>(p.border.g * 255),
                          static_cast<int>(p.border.b * 255));
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));

    target_->BeginDraw();
    target_->Clear(p.background);
    ComPtr<ID2D1SolidColorBrush> brush;
    target_->CreateSolidColorBrush(p.text, &brush);
    if (!brush) {
        target_->EndDraw();
        return;
    }
    if (!dwmRounded_) {
        brush->SetColor(p.border);
        target_->DrawRectangle(D2D1::RectF(0.5f, 0.5f, size_.width - 0.5f, size_.height - 0.5f), brush.Get());
    }

    for (size_t i = 0; i < page_.size(); ++i) {
        const Item& it = page_[i];
        const bool selected = pageStart_ + i == selection_;
        D2D1_RECT_F r = it.rect;
        if (selected) {
            brush->SetColor(p.selection);
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), 4, 4), brush.Get());
            brush->SetColor(p.accent);
            float cy = (r.top + r.bottom) / 2, ph = (r.bottom - r.top) * 0.42f;
            if (vertical)
                target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 3, cy - ph / 2, r.left + 6, cy + ph / 2), 1.5f, 1.5f), brush.Get());
            else
                target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF((r.left + r.right) / 2 - 8, r.bottom - 4, (r.left + r.right) / 2 + 8, r.bottom - 1.5f), 1.5f, 1.5f), brush.Get());
        }
        float x = r.left + (vertical ? 12.0f : 8.0f);
        brush->SetColor(selected ? p.selectedText : p.secondary);
        target_->DrawText(it.label.c_str(), static_cast<UINT32>(it.label.size()), labelFormat_.Get(),
                          D2D1::RectF(x, r.top, x + it.labelWidth + 1, r.bottom), brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        x += vertical ? 0 : it.labelWidth + 5;
        if (vertical) {
            float labelColumn = 0;
            for (const Item& o : page_) labelColumn = std::max(labelColumn, o.labelWidth);
            x += labelColumn + kGap;
        }
        brush->SetColor(selected ? p.selectedText : p.text);
        target_->DrawText(it.text.c_str(), static_cast<UINT32>(it.text.size()), textFormat_.Get(),
                          D2D1::RectF(x, r.top, x + it.textWidth + 1, r.bottom), brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        if (!it.hint.empty()) {
            x += it.textWidth + (vertical ? kGap : 5.0f);
            brush->SetColor(p.hint);
            target_->DrawText(it.hint.c_str(), static_cast<UINT32>(it.hint.size()), hintFormat_.Get(),
                              D2D1::RectF(x, r.top, x + it.hintWidth + 1, r.bottom), brush.Get());
        }
    }

    if (pageCount_ > 1) {
        const size_t page = pageSize_ ? selection_ / pageSize_ : 0;
        std::wstring counter = std::to_wstring(page + 1) + L"/" + std::to_wstring(pageCount_);
        float cw = MeasureWidth(dwrite_.Get(), labelFormat_.Get(), counter);
        brush->SetColor(p.hint);
        if (vertical)
            target_->DrawText(counter.c_str(), static_cast<UINT32>(counter.size()), labelFormat_.Get(),
                              D2D1::RectF(prevRect_.left - cw - 4, prevRect_.top, prevRect_.left, prevRect_.bottom),
                              brush.Get());
        brush->SetColor(page > 0 ? p.secondary : p.hint);
        target_->DrawText(L"‹", 1, textFormat_.Get(),
                          D2D1::RectF(prevRect_.left + 6, prevRect_.top, prevRect_.right, prevRect_.bottom), brush.Get());
        brush->SetColor(page + 1 < pageCount_ ? p.secondary : p.hint);
        target_->DrawText(L"›", 1, textFormat_.Get(),
                          D2D1::RectF(nextRect_.left + 6, nextRect_.top, nextRect_.right, nextRect_.bottom), brush.Get());
    }

    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

void CandidateUI::OnClick(POINT pt) {
    if (!owner_) return;
    ComPtr<CandidateUI> keep(this);
    const float s = Scale();
    const float x = pt.x / s, y = pt.y / s;
    auto inside = [&](const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (pageCount_ > 1 && inside(prevRect_)) {
        owner_->ChangePage(-1);
        return;
    }
    if (pageCount_ > 1 && inside(nextRect_)) {
        owner_->ChangePage(1);
        return;
    }
    for (size_t i = 0; i < page_.size(); ++i) {
        if (inside(page_[i].rect)) {
            owner_->SelectCandidate(pageStart_ + i);
            return;
        }
    }
}

LRESULT CALLBACK CandidateUI::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<CandidateUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (self) self->Render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONUP:
        if (self) self->OnClick({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_SIZE:
        if (self && self->target_) self->target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        if (self) {
            self->target_.Reset();
            self->hwnd_ = nullptr;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace nova
