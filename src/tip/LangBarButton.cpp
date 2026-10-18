#include "LangBarButton.h"

#include <oleauto.h>
#include <olectl.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "TextService.h"

namespace nova {

namespace {

constexpr DWORD kSinkCookie = 0x4E434A31;  // Only one sink is supported.
constexpr wchar_t kMenuOwnerClass[] = L"NovaChangJieMenuOwner";

enum MenuId : UINT {
    kCmdChinese = 1,
    kCmdFullShape,
    kCmdPunct,
    kCmdSimplified,
    kCmdUserPhrases,
    kCmdSettings,
};

bool TaskbarIsLight() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value != 0;
    return false;
}

// Coverage (0..1) of a moon badge at pixel (x, y), 4x4 supersampled. A full
// moon is a disc; the crescent is the lit left-hand sliver left when a second
// disc, shifted right, is taken away (a mirrored waxing crescent).
float MoonCoverage(int x, int y, float cx, float cy, float r, bool full) {
    int hits = 0;
    for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
            float px = x + (sx + 0.5f) / 4.0f, py = y + (sy + 0.5f) / 4.0f;
            float dx = px - cx, dy = py - cy;
            bool in = dx * dx + dy * dy <= r * r;
            if (in && !full) {
                float ox = px - (cx + r * 0.62f);
                in = ox * ox + dy * dy > r * r;
            }
            hits += in ? 1 : 0;
        }
    }
    return hits / 16.0f;
}

// Renders the mode glyph (中 / 簡 / 英) with a moon badge in the top-right
// corner: a crescent (lit on the left) for 半形, a full moon for 全形.
HICON CreateModeIcon(const wchar_t* glyph, bool fullMoon, COLORREF color) {
    UINT dpi = GetDpiForSystem();
    int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (size <= 0) size = 16;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;  // Top-down.
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color32 = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color32 || !bits) return nullptr;

    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldBmp = SelectObject(dc, color32);
    std::memset(bits, 0, static_cast<size_t>(size) * size * 4);
    // The glyph fills the lower-left, leaving the corner for the moon.
    const int glyphSize = size * 15 / 16;
    HFONT font = CreateFontW(-glyphSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                             L"Microsoft JhengHei UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc = {0, size - glyphSize - 1, glyphSize + 1, size};
    DrawTextW(dc, glyph, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    GdiFlush();

    const float r = size * 0.19f;
    const float cx = size - r - 0.25f, cy = r + 0.25f;
    const float gap = r + std::max(1.0f, size / 16.0f);  // Clear space around the moon.
    auto* px = static_cast<BYTE*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x, px += 4) {
            float glyphA = px[2] / 255.0f;  // White-on-black coverage.
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy < gap * gap) glyphA = 0;
            float a = std::max(glyphA, MoonCoverage(x, y, cx, cy, r, fullMoon));
            BYTE alpha = static_cast<BYTE>(a * 255.0f + 0.5f);
            // Premultiplied colour, as icons expect.
            px[0] = static_cast<BYTE>(GetBValue(color) * alpha / 255);
            px[1] = static_cast<BYTE>(GetGValue(color) * alpha / 255);
            px[2] = static_cast<BYTE>(GetRValue(color) * alpha / 255);
            px[3] = alpha;
        }
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteDC(dc);

    std::vector<BYTE> maskBits(static_cast<size_t>((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color32;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color32);
    return icon;
}

}  // namespace

LangBarButton::LangBarButton(TextService* owner) : owner_(owner) { DllAddRef(); }

LangBarButton::~LangBarButton() {
    if (menuOwner_) DestroyWindow(menuOwner_);
    DllRelease();
}

void LangBarButton::Detach() {
    owner_ = nullptr;
    sink_.Reset();
    if (menuOwner_) {
        DestroyWindow(menuOwner_);
        menuOwner_ = nullptr;
    }
}

void LangBarButton::Update() {
    if (sink_) sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP);
}

STDMETHODIMP LangBarButton::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfLangBarItem || riid == IID_ITfLangBarItemButton)
        *ppv = static_cast<ITfLangBarItemButton*>(this);
    else if (riid == IID_ITfSource)
        *ppv = static_cast<ITfSource*>(this);
    if (!*ppv) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) LangBarButton::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) LangBarButton::Release() {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
}

// 中 (or 簡 for simplified output) in Chinese mode, 英 in English mode.
const wchar_t* LangBarButton::ModeGlyph() const {
    if (!owner_) return L"中";
    const Engine& e = owner_->GetEngine();
    if (!e.Chinese()) return L"英";
    return e.GetOptions().outputSimplified ? L"簡" : L"中";
}

// 全形 shows a full moon, 半形 a crescent, in Chinese and English mode alike.
bool LangBarButton::FullMoon() const { return owner_ && owner_->GetEngine().FullShape(); }

STDMETHODIMP LangBarButton::GetInfo(TF_LANGBARITEMINFO* pInfo) {
    if (!pInfo) return E_INVALIDARG;
    pInfo->clsidService = CLSID_NovaTextService;
    pInfo->guidItem = GUID_LBI_INPUTMODE;
    pInfo->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
    pInfo->ulSort = 0;
    wcscpy_s(pInfo->szDescription, L"NovaChangJie 輸入模式");
    return S_OK;
}

STDMETHODIMP LangBarButton::GetStatus(DWORD* pdwStatus) {
    if (!pdwStatus) return E_INVALIDARG;
    *pdwStatus = 0;
    return S_OK;
}

STDMETHODIMP LangBarButton::Show(BOOL) { return E_NOTIMPL; }

STDMETHODIMP LangBarButton::GetTooltipString(BSTR* pbstrToolTip) {
    if (!pbstrToolTip) return E_INVALIDARG;
    std::wstring tip = owner_ && owner_->IsQuick() ? L"Nova Quick 星速成 - " : L"NovaChangJie 星倉頡 - ";
    if (owner_) {
        const Engine& e = owner_->GetEngine();
        tip += !e.Chinese() ? L"英文" : e.GetOptions().outputSimplified ? L"簡體中文" : L"正體中文";
        tip += FullMoon() ? L" / 全形" : L" / 半形";
        tip += e.ChinesePunct() ? L" / 中文標點" : L" / 英文標點";
    }
    *pbstrToolTip = SysAllocString(tip.c_str());
    return *pbstrToolTip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarButton::OnClick(TfLBIClick click, POINT pt, const RECT*) {
    if (!owner_) return S_OK;
    ComPtr<LangBarButton> keep(this);
    if (click == TF_LBI_CLK_LEFT)
        owner_->ToggleChinese();
    else if (click == TF_LBI_CLK_RIGHT)
        ShowMenu(pt);
    return S_OK;
}

STDMETHODIMP LangBarButton::InitMenu(ITfMenu*) { return S_OK; }
STDMETHODIMP LangBarButton::OnMenuSelect(UINT wID) {
    RunCommand(wID);
    return S_OK;
}

STDMETHODIMP LangBarButton::GetIcon(HICON* phIcon) {
    if (!phIcon) return E_INVALIDARG;
    COLORREF color = TaskbarIsLight() ? RGB(0, 0, 0) : RGB(255, 255, 255);
    *phIcon = CreateModeIcon(ModeGlyph(), FullMoon(), color);  // Caller destroys it.
    return *phIcon ? S_OK : E_FAIL;
}

STDMETHODIMP LangBarButton::GetText(BSTR* pbstrText) {
    if (!pbstrText) return E_INVALIDARG;
    *pbstrText = SysAllocString(ModeGlyph());
    return *pbstrText ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarButton::AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) {
    if (!punk || !pdwCookie) return E_INVALIDARG;
    if (riid != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
    if (sink_) return CONNECT_E_ADVISELIMIT;
    if (FAILED(punk->QueryInterface(IID_PPV_ARGS(&sink_)))) {
        sink_.Reset();
        return E_NOINTERFACE;
    }
    *pdwCookie = kSinkCookie;
    return S_OK;
}

STDMETHODIMP LangBarButton::UnadviseSink(DWORD dwCookie) {
    if (dwCookie != kSinkCookie || !sink_) return CONNECT_E_NOCONNECTION;
    sink_.Reset();
    return S_OK;
}

void LangBarButton::ShowMenu(POINT pt) {
    if (!owner_) return;
    if (!menuOwner_) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = g_hInstance;
        wc.lpszClassName = kMenuOwnerClass;
        RegisterClassExW(&wc);  // Fails harmlessly if already registered.
        menuOwner_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMenuOwnerClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                     g_hInstance, nullptr);
        if (!menuOwner_) return;
    }
    const Engine& e = owner_->GetEngine();
    const bool secure = owner_->IsSecureMode();
    HMENU menu = CreatePopupMenu();
    auto add = [&](UINT id, const wchar_t* text, bool checked, bool enabled = true) {
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, text);
    };
    add(kCmdChinese, L"中文輸入\tShift", e.Chinese());
    add(kCmdFullShape, L"全形\tShift+Space", e.FullShape());
    add(kCmdPunct, L"中文標點\tCtrl+.", e.ChinesePunct());
    add(kCmdSimplified, L"輸出簡體中文", e.GetOptions().outputSimplified);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(kCmdUserPhrases, L"使用者造詞…", false, !secure);
    add(kCmdSettings, L"設定…", false, !secure);
    SetForegroundWindow(menuOwner_);
    UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON |
                                                            TPM_BOTTOMALIGN,
                                                  pt.x, pt.y, menuOwner_, nullptr));
    DestroyMenu(menu);
    PostMessageW(menuOwner_, WM_NULL, 0, 0);
    RunCommand(cmd);
}

void LangBarButton::RunCommand(UINT id) {
    if (!owner_) return;
    switch (id) {
    case kCmdChinese: owner_->ToggleChinese(); break;
    case kCmdFullShape: owner_->ToggleFullShape(); break;
    case kCmdPunct: owner_->ToggleChinesePunct(); break;
    case kCmdSimplified: owner_->ToggleSimplified(); break;
    case kCmdUserPhrases: owner_->OpenUserPhrases(); break;
    case kCmdSettings: owner_->OpenSettings(); break;
    default: break;
    }
}

}  // namespace nova
