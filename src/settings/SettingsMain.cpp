// NovaChangJie settings application.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "NovaIds.h"
#include "SettingsStore.h"
#include "UserWords.h"
#include "resource.h"

using namespace nova;

namespace {

constexpr uint32_t kPageSizes[] = {5, 6, 7, 8, 9};
constexpr uint32_t kFontSizes[] = {14, 16, 18, 20, 22, 24, 28, 32};

struct CheckBinding {
    int id;
    bool Options::*field;
};

constexpr CheckBinding kChecks[] = {
    {IDC_SENTENCE, &Options::sentenceMode},
    {IDC_CODECANDS, &Options::codeCandidates},
    {IDC_PREDICTION, &Options::prediction},
    {IDC_HINTS, &Options::showCodeHints},
    {IDC_AUTOUNIQUE, &Options::autoCommitUnique},
    {IDC_OVERFLOW, &Options::autoCommitOnOverflow},
    {IDC_PREFIXHINTS, &Options::prefixHints},
    {IDC_WORDPAIRS, &Options::wordPairs},
    {IDC_NEURAL, &Options::neuralRerank},
    {IDC_ASSOC, &Options::association},
    {IDC_LEARNING, &Options::learning},
    {IDC_SIMPLIFIED, &Options::outputSimplified},
    {IDC_LETTERS, &Options::showLetters},
    {IDC_SHIFT, &Options::shiftToggles},
    {IDC_STARTFULL, &Options::startFullShape},
    {IDC_PUNCT, &Options::chinesePunctuation},
};

constexpr wchar_t kTitle[] = L"NovaChangJie 星倉頡 設定";

// The options being edited. Each page loads its controls from it and writes
// them back when the sheet is applied; pages never opened keep their values.
Options g_options;
bool g_centered = false;

bool Has(HWND page, int id) { return GetDlgItem(page, id) != nullptr; }

void FillCombo(HWND dlg, int id, std::initializer_list<const wchar_t*> items, int selected) {
    HWND combo = GetDlgItem(dlg, id);
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const wchar_t* item : items) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

template <size_t N>
void FillNumberCombo(HWND dlg, int id, const uint32_t (&values)[N], uint32_t current) {
    HWND combo = GetDlgItem(dlg, id);
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int selected = 0;
    for (size_t i = 0; i < N; ++i) {
        std::wstring text = std::to_wstring(values[i]);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        if (values[i] <= current) selected = static_cast<int>(i);
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

int ComboIndex(HWND dlg, int id) {
    LRESULT i = SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0);
    return i == CB_ERR ? 0 : static_cast<int>(i);
}

void LoadPage(HWND page, const Options& o) {
    FillCombo(page, IDC_STARTMODE, {L"中文模式", L"英文模式"}, o.startInChinese ? 0 : 1);
    FillCombo(page, IDC_VERSION, {L"倉頡第五代", L"倉頡第三代", L"三五代混打"}, static_cast<int>(o.version));
    FillCombo(page, IDC_CHARSET, {L"Unicode 字元 (全部)", L"Big5 + 香港增補字符集", L"Big5 字元"},
              static_cast<int>(o.charset));
    FillCombo(page, IDC_LAYOUT, {L"直式", L"橫式"}, static_cast<int>(o.layout));
    FillNumberCombo(page, IDC_PAGESIZE, kPageSizes, o.pageSize);
    FillNumberCombo(page, IDC_FONTSIZE, kFontSizes, o.fontSize);
    if (Has(page, IDC_ORDER_FIXED))
        CheckRadioButton(page, IDC_ORDER_FIXED, IDC_ORDER_FREQ,
                         o.order == CandidateOrder::Traditional ? IDC_ORDER_FIXED : IDC_ORDER_FREQ);
    for (const CheckBinding& c : kChecks)
        if (Has(page, c.id)) CheckDlgButton(page, c.id, (o.*c.field) ? BST_CHECKED : BST_UNCHECKED);
}

// Only the controls on this page are read.
void ReadPage(HWND page, Options& o) {
    if (Has(page, IDC_STARTMODE)) o.startInChinese = ComboIndex(page, IDC_STARTMODE) == 0;
    if (Has(page, IDC_VERSION)) o.version = static_cast<CangjieVersion>(ComboIndex(page, IDC_VERSION));
    if (Has(page, IDC_CHARSET)) o.charset = static_cast<Charset>(ComboIndex(page, IDC_CHARSET));
    if (Has(page, IDC_LAYOUT)) o.layout = static_cast<CandidateLayout>(ComboIndex(page, IDC_LAYOUT));
    if (Has(page, IDC_PAGESIZE))
        o.pageSize = kPageSizes[std::min<size_t>(ComboIndex(page, IDC_PAGESIZE), std::size(kPageSizes) - 1)];
    if (Has(page, IDC_FONTSIZE))
        o.fontSize = kFontSizes[std::min<size_t>(ComboIndex(page, IDC_FONTSIZE), std::size(kFontSizes) - 1)];
    if (Has(page, IDC_ORDER_FIXED))
        o.order = IsDlgButtonChecked(page, IDC_ORDER_FIXED) == BST_CHECKED ? CandidateOrder::Traditional
                                                                          : CandidateOrder::Frequency;
    for (const CheckBinding& c : kChecks)
        if (Has(page, c.id)) o.*c.field = IsDlgButtonChecked(page, c.id) == BST_CHECKED;
}

// Section headings: start each etched line just after its heading text.
void FitHeadingLines(HWND page) {
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(page, WM_GETFONT, 0, 0));
    HDC dc = GetDC(page);
    HGDIOBJ old = SelectObject(dc, font);
    for (HWND child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if ((GetWindowLongW(child, GWL_STYLE) & SS_TYPEMASK) != SS_ETCHEDHORZ) continue;
        HWND heading = GetWindow(child, GW_HWNDPREV);
        if (!heading) continue;
        wchar_t text[64] = {};
        GetWindowTextW(heading, text, ARRAYSIZE(text));
        SIZE size = {};
        GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
        RECT h, line;
        GetWindowRect(heading, &h);
        GetWindowRect(child, &line);
        MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&h), 2);
        MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&line), 2);
        RECT gap = {0, 0, 6, 0};
        MapDialogRect(page, &gap);
        const int left = h.left + size.cx + gap.right;
        MoveWindow(child, left, line.top, std::max<int>(1, static_cast<int>(line.right) - left), 2, TRUE);
    }
    SelectObject(dc, old);
    ReleaseDC(page, dc);
}

void CenterSheet(HWND sheet) {
    RECT r, work;
    GetWindowRect(sheet, &r);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(sheet, MONITOR_DEFAULTTONEAREST), &mi);
    work = mi.rcWork;
    const int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(sheet, nullptr, work.left + (work.right - work.left - w) / 2,
                 work.top + (work.bottom - work.top - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

// Opens the 使用者造詞工具 (this program with /userwords).
void EditUserPhrases(HWND dlg) {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    ShellExecuteW(dlg, L"open", exe, L"/classic /userwords", nullptr, SW_SHOWNORMAL);
}

void ClearHistory(HWND dlg) {
    if (MessageBoxW(dlg, L"確定要清除所有學習記錄嗎？\n候選字將回復預設次序。", L"NovaChangJie",
                    MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    std::wstring dir = UserDataDirectory(false);
    if (!dir.empty()) DeleteFileW((dir + L"history.txt").c_str());
    BumpHistoryEpoch();
    MessageBoxW(dlg, L"學習記錄已清除。", L"NovaChangJie", MB_ICONINFORMATION);
}

INT_PTR CALLBACK PageProc(HWND page, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        LoadPage(page, g_options);
        FitHeadingLines(page);
        if (!g_centered) {
            HWND sheet = GetParent(page);
            // The sheet's buttons follow the Windows display language; keep
            // them Chinese like the rest of the dialog.
            SetDlgItemTextW(sheet, IDOK, L"確定");
            SetDlgItemTextW(sheet, IDCANCEL, L"取消");
            CenterSheet(sheet);
            g_centered = true;
        }
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_EDITPHRASES:
            EditUserPhrases(page);
            return TRUE;
        case IDC_CLEARHISTORY:
            ClearHistory(page);
            return TRUE;
        default:
            return FALSE;
        }
    case WM_NOTIFY: {
        const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lParam);
        if (hdr->code == PSN_KILLACTIVE) {
            ReadPage(page, g_options);
            SetWindowLongPtrW(page, DWLP_MSGRESULT, FALSE);
            return TRUE;
        }
        if (hdr->code == PSN_APPLY) {
            // Sent to every opened page; each saves the combined options.
            ReadPage(page, g_options);
            if (!SaveOptions(g_options)) {
                MessageBoxW(page, L"無法儲存設定。", L"NovaChangJie", MB_ICONERROR);
                SetWindowLongPtrW(page, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                return TRUE;
            }
            GrantAppContainerRead();
            SetWindowLongPtrW(page, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

void ShowSettings(HINSTANCE instance) {
    g_options = LoadOptions();
    const int pageIds[] = {IDD_PAGE_GENERAL, IDD_PAGE_ADVANCED, IDD_PAGE_DICTIONARY};
    PROPSHEETPAGEW pages[std::size(pageIds)] = {};
    for (size_t i = 0; i < std::size(pageIds); ++i) {
        pages[i].dwSize = sizeof(PROPSHEETPAGEW);
        pages[i].hInstance = instance;
        pages[i].pszTemplate = MAKEINTRESOURCEW(pageIds[i]);
        pages[i].pfnDlgProc = PageProc;
    }
    PROPSHEETHEADERW sheet = {};
    sheet.dwSize = sizeof(sheet);
    sheet.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP | PSH_USEHICON;
    sheet.hInstance = instance;
    sheet.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_NOVA), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    sheet.pszCaption = kTitle;
    sheet.nPages = static_cast<UINT>(std::size(pages));
    sheet.ppsp = pages;
    PropertySheetW(&sheet);
}

// Whether "0404:{clsid}{profile}" is already in one of the user's languages
// (HKCU\Control Panel\International\User Profile\<tag>, e.g. zh-Hant-TW).
bool InUserLanguageList(const std::wstring& item) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\International\\User Profile", 0, KEY_READ, &root) !=
        ERROR_SUCCESS)
        return false;
    bool found = false;
    wchar_t tag[128];
    for (DWORD i = 0; !found; ++i) {
        DWORD len = ARRAYSIZE(tag);
        if (RegEnumKeyExW(root, i, tag, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        found = RegGetValueW(root, tag, item.c_str(), RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    }
    RegCloseKey(root);
    return found;
}

// Adds NovaChangJie to (or removes it from) the current user's keyboard list
// through InstallLayoutOrTip in input.dll, the documented API for this. Used by
// the installer: "/add-keyboard" and "/remove-keyboard", optionally followed
// by language IDs such as "0404 0C04" (default: zh-TW only for adding, every
// registered language for removing).
int ManageKeyboard(bool add, int argc, wchar_t** argv) {
    using InstallLayoutOrTipFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
    constexpr DWORD kIlotUninstall = 0x00000001;
    HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto install = input ? reinterpret_cast<InstallLayoutOrTipFn>(GetProcAddress(input, "InstallLayoutOrTip"))
                         : nullptr;
    if (!install) {
        if (input) FreeLibrary(input);
        return 2;
    }
    wchar_t clsid[64], profile[64], quick[64];
    StringFromGUID2(kTextServiceClsid, clsid, ARRAYSIZE(clsid));
    StringFromGUID2(kProfileGuid, profile, ARRAYSIZE(profile));
    StringFromGUID2(kQuickProfileGuid, quick, ARRAYSIZE(quick));

    std::vector<std::wstring> langs;
    for (int i = 2; i < argc; ++i) langs.emplace_back(argv[i]);
    if (langs.empty()) {
        if (add) {
            langs.push_back(L"0404");
        } else {
            for (LANGID id : kLangIds) {
                wchar_t buf[8];
                swprintf_s(buf, L"%04X", id);
                langs.push_back(buf);
            }
        }
    }
    std::wstring spec;
    for (const std::wstring& lang : langs) {
        // Both input methods: 倉頡 and 速成.
        for (const wchar_t* p : {profile, quick}) {
            std::wstring item = lang + L":" + clsid + p;
            // Only add what is missing and remove what is there: removing an
            // item from a language the user does not have makes Windows add
            // that language (e.g. an empty zh-HK after uninstalling).
            if (InUserLanguageList(item) == add) continue;
            if (!spec.empty()) spec += L';';
            spec += item;
        }
    }
    BOOL ok = spec.empty() || install(spec.c_str(), add ? 0 : kIlotUninstall);
    FreeLibrary(input);
    return ok ? 0 : 1;
}

// Opens the WinUI settings app (<this folder>\ui\NovaChangJieSettingsUI.exe)
// with the same window; false if it is missing or does not start.
bool LaunchModernUi(bool userWords) {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring dir(exe, n);
    dir.resize(dir.find_last_of(L'\\') + 1);
    std::wstring app = dir + L"ui\\NovaChangJieSettingsUI.exe";
    if (GetFileAttributesW(app.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring cmd = L"\"" + app + L"\"" + (userWords ? L" /userwords" : L"");
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, (dir + L"ui").c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

// NovaChangJieSettings.exe [/userwords]           settings (WinUI app when installed)
// NovaChangJieSettings.exe /classic [/userwords]  the classic Win32 windows
// NovaChangJieSettings.exe /add-keyboard | /remove-keyboard [langid...]
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool classic = false;
    if (argv && argc >= 2 && std::wstring(argv[1]) == L"/classic") {
        classic = true;
        for (int i = 1; i + 1 < argc; ++i) argv[i] = argv[i + 1];
        --argc;
    }
    if (argv && argc >= 2) {
        std::wstring cmd = argv[1];
        if (cmd == L"/add-keyboard" || cmd == L"/remove-keyboard") {
            int rc = ManageKeyboard(cmd == L"/add-keyboard", argc, argv);
            LocalFree(argv);
            return rc;
        }
        if (cmd == L"/userwords" && !classic && LaunchModernUi(true)) {
            LocalFree(argv);
            return 0;
        }
        if (cmd == L"/userwords") {
            LocalFree(argv);
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            int rc = RunUserWordsTool(instance);
            CoUninitialize();
            return rc;
        }
    }
    if (argv) LocalFree(argv);
    if (!classic && LaunchModernUi(false)) return 0;

    // One settings window at a time.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\NovaChangJieSettings");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"#32770", kTitle);
        if (existing) SetForegroundWindow(existing);
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ShowSettings(instance);
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
