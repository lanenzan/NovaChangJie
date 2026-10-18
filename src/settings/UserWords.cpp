// NovaChangJie user words tool (使用者造詞工具), laid out like the Microsoft
// ChangJie user phrase tool: a menu, a toolbar, a grid of words with each
// character's radicals and the shortcut, view tabs at the bottom
// (使用者造詞 / 常用新詞) and a status bar with the word count.
//
// The files and their formats are handled by UserWordsData.h, shared with
// the WinUI settings app.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "UserWords.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Dictionary.h"
#include "SettingsStore.h"
#include "Unicode.h"
#include "UserWordsData.h"
#include "resource.h"

namespace nova {
namespace {

constexpr wchar_t kClassName[] = L"NovaChangJieUserWords";
constexpr wchar_t kAppTitle[] = L"NovaChangJie 使用者造詞工具";
constexpr COLORREF kGridColor = RGB(255, 255, 204);

using Word = UserWord;

enum class View { User, Learned };
enum class Sort { None, Phrase, Reading, Shortcut };


std::wstring Wide(std::u16string_view s) { return std::wstring(s.begin(), s.end()); }
std::u16string Narrow16(std::wstring_view s) { return std::u16string(s.begin(), s.end()); }
std::wstring WideAscii(std::string_view s) { return std::wstring(s.begin(), s.end()); }

std::wstring Radicals(std::string_view code) { return Wide(RadicalsOf(code)); }


// ---------------------------------------------------------------------------

class UserWordsWindow {
public:
    explicit UserWordsWindow(HINSTANCE instance) : instance_(instance) {}

    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
    void Create();
    void CreateFonts();
    void Layout();
    void LoadData();
    bool Save();
    bool ConfirmClose();
    void Refresh(int selectWord = -1);
    void UpdateStatus();
    void UpdateCommands();
    void SetView(View view);
    void SortBy(Sort sort);
    std::vector<Word>& Current() { return view_ == View::User ? user_ : learned_; }
    std::vector<int> SelectedWords() const;
    void Command(int id);
    void Insert();
    void Modify();
    void Delete();
    void Undelete();
    void MoveToUser();
    void Find();
    void Import();
    void Export();
    void OnNotify(NMHDR* hdr, LRESULT& result, bool& handled);
    bool Cj3() const { return version_ == CangjieVersion::V3; }
    std::wstring RadicalsFor(const Word& w) const { return Wide(WordRadicals(w, table_, Cj3())); }
    std::string ReadingOf(const Word& w) const { return WordReading(w, table_, Cj3()); }
    const std::vector<std::string>& Chosen(const Word& w) const { return Cj3() ? w.chosen3 : w.chosen5; }
    bool EditWord(Word& word, int ignoreIndex);
    static INT_PTR CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK FindProc(HWND, UINT, WPARAM, LPARAM);

    HINSTANCE instance_;
    HWND hwnd_ = nullptr, list_ = nullptr, toolbar_ = nullptr, tabs_ = nullptr, status_ = nullptr;
    HFONT font_ = nullptr;
    HACCEL accel_ = nullptr;
    HIMAGELIST tabImages_ = nullptr;
    CodeTable table_;
    CangjieVersion version_ = CangjieVersion::V5;
    View view_ = View::User;
    Sort sort_ = Sort::None;
    std::vector<Word> user_, learned_;
    std::vector<std::u16string> removedLearned_;
    std::vector<std::pair<View, std::vector<Word>>> undo_;
    bool userDirty_ = false, learnedDirty_ = false;
    bool showToolbar_ = true, showStatus_ = true;
    std::wstring findText_;

    // Edit dialog state.
    struct EditState {
        UserWordsWindow* self;
        Word* word;
        int ignoreIndex;
        size_t chars = 0;
    };
};

UserWordsWindow* g_window = nullptr;

int UserWordsWindow::Run() {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
    version_ = LoadOptions().version;
    // nova.dict sits next to this program.
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n < MAX_PATH ? n : 0);
    table_.Load(dir.substr(0, dir.find_last_of(L'\\') + 1) + L"nova.dict");
    LoadData();
    Create();
    if (!hwnd_) return 1;
    if (!table_.Loaded())
        MessageBoxW(hwnd_, L"找不到 nova.dict，無法顯示字根。", kAppTitle, MB_ICONWARNING);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(hwnd_, accel_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void UserWordsWindow::Create() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance_;
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_NOVA));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_NOVA), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_USERWORDS_MENU);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    accel_ = LoadAcceleratorsW(instance_, MAKEINTRESOURCEW(IDR_USERWORDS_ACCEL));
    g_window = this;
    hwnd_ = CreateWindowExW(0, kClassName, kAppTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return;
    // Size for the monitor's DPI.
    UINT dpi = GetDpiForWindow(hwnd_);
    SetWindowPos(hwnd_, nullptr, 0, 0, MulDiv(960, dpi, 96), MulDiv(520, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
}

void UserWordsWindow::CreateFonts() {
    if (font_) DeleteObject(font_);
    UINT dpi = GetDpiForWindow(hwnd_);
    font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                        L"Microsoft JhengHei UI");
    for (HWND w : {list_, tabs_, status_})
        if (w) SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

LRESULT CALLBACK UserWordsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UserWordsWindow* self = g_window;
    if (msg == WM_NCCREATE) self->hwnd_ = hwnd;
    return self ? self->Handle(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT UserWordsWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        UINT dpi = GetDpiForWindow(hwnd_);
        // Toolbar with the standard common-control images.
        toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                                   WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP, 0, 0, 0, 0,
                                   hwnd_, nullptr, instance_, nullptr);
        SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        const bool large = dpi >= 144;
        SendMessageW(toolbar_, TB_LOADIMAGES, large ? IDB_STD_LARGE_COLOR : IDB_STD_SMALL_COLOR,
                     reinterpret_cast<LPARAM>(HINST_COMMCTRL));
        const TBBUTTON buttons[] = {
            {STD_FILEOPEN, ID_UW_IMPORT, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
            {STD_PRINTPRE, ID_UW_EXPORT, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
            {STD_FILESAVE, ID_UW_SAVE, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
            {0, 0, 0, BTNS_SEP, {}, 0, 0},
            {STD_FILENEW, ID_UW_INSERT, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
            {STD_DELETE, ID_UW_DELETE, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
            {STD_UNDO, ID_UW_UNDELETE, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
            {STD_PROPERTIES, ID_UW_MODIFY, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
            {STD_PASTE, ID_UW_MOVE, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
            {0, 0, 0, BTNS_SEP, {}, 0, 0},
            {STD_FIND, ID_UW_FIND, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
            {0, 0, 0, BTNS_SEP, {}, 0, 0},
            {STD_HELP, ID_UW_ABOUT, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        };
        SendMessageW(toolbar_, TB_ADDBUTTONS, std::size(buttons), reinterpret_cast<LPARAM>(buttons));
        SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);

        list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 0, 0,
                                hwnd_, nullptr, instance_, nullptr);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(list_, kGridColor);
        ListView_SetTextBkColor(list_, kGridColor);
        struct Col { const wchar_t* title; int width; int fmt; };
        const Col cols[] = {{L"", 44, LVCFMT_CENTER}, {L"使用者造詞", 200, LVCFMT_LEFT},
                            {L"字根", 360, LVCFMT_LEFT}, {L"捷徑", 140, LVCFMT_LEFT}};
        for (int i = 0; i < 4; ++i) {
            LVCOLUMNW c = {LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
            c.pszText = const_cast<wchar_t*>(cols[i].title);
            c.cx = MulDiv(cols[i].width, dpi, 96);
            c.fmt = cols[i].fmt;
            ListView_InsertColumn(list_, i, &c);
        }

        tabs_ = CreateWindowExW(0, WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | TCS_BOTTOM | TCS_TABS, 0, 0, 0,
                                0, hwnd_, nullptr, instance_, nullptr);
        const wchar_t* tabNames[] = {L"使用者造詞", L"常用新詞"};
        for (int i = 0; i < 2; ++i) {
            TCITEMW item = {TCIF_TEXT};
            item.pszText = const_cast<wchar_t*>(tabNames[i]);
            TabCtrl_InsertItem(tabs_, i, &item);
        }
        status_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                  hwnd_, nullptr, instance_, nullptr);
        CreateFonts();
        SetView(View::User);
        SetFocus(list_);
        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        CreateFonts();
        Layout();
        return 0;
    }
    case WM_SETFOCUS:
        if (list_) SetFocus(list_);
        return 0;
    case WM_INITMENUPOPUP:
        UpdateCommands();
        return 0;
    case WM_COMMAND:
        Command(LOWORD(wParam));
        return 0;
    case WM_NOTIFY: {
        LRESULT result = 0;
        bool handled = false;
        OnNotify(reinterpret_cast<NMHDR*>(lParam), result, handled);
        if (handled) return result;
        break;
    }
    case WM_CLOSE:
        if (ConfirmClose()) DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        if (font_) DeleteObject(font_);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void UserWordsWindow::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int top = 0, bottom = rc.bottom;
    if (showToolbar_) {
        SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
        RECT tr;
        GetWindowRect(toolbar_, &tr);
        top = tr.bottom - tr.top;
    }
    if (showStatus_) {
        SendMessageW(status_, WM_SIZE, 0, 0);
        RECT sr;
        GetWindowRect(status_, &sr);
        bottom -= sr.bottom - sr.top;
    }
    // Tab strip height: one tab row.
    RECT tabRect = {0, 0, rc.right, 0};
    TabCtrl_AdjustRect(tabs_, TRUE, &tabRect);
    const int tabHeight = std::max<int>(static_cast<int>(tabRect.bottom - tabRect.top),
                                        MulDiv(28, GetDpiForWindow(hwnd_), 96));
    MoveWindow(tabs_, 0, bottom - tabHeight, rc.right, tabHeight, TRUE);
    MoveWindow(list_, 0, top, rc.right, std::max(0, bottom - tabHeight - top), TRUE);
}

void UserWordsWindow::LoadData() {
    user_ = LoadUserWords();
    learned_ = LoadLearnedWords();
    removedLearned_.clear();
    userDirty_ = learnedDirty_ = false;
}

bool UserWordsWindow::Save() {
    if (userDirty_) {
        if (!SaveUserWords(user_)) {
            MessageBoxW(hwnd_, L"無法儲存使用者造詞。", kAppTitle, MB_ICONERROR);
            return false;
        }
        userDirty_ = false;
    }
    if (learnedDirty_) {
        if (!RemoveLearnedWords(removedLearned_)) {
            MessageBoxW(hwnd_, L"無法儲存常用新詞。", kAppTitle, MB_ICONERROR);
            return false;
        }
        removedLearned_.clear();
        learnedDirty_ = false;
    }
    UpdateCommands();
    return true;
}

bool UserWordsWindow::ConfirmClose() {
    if (!userDirty_ && !learnedDirty_) return true;
    int r = MessageBoxW(hwnd_, L"詞彙已變更，是否儲存？", kAppTitle, MB_ICONQUESTION | MB_YESNOCANCEL);
    if (r == IDCANCEL) return false;
    return r == IDNO || Save();
}

void UserWordsWindow::Refresh(int selectWord) {
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    std::vector<Word>& words = Current();
    for (size_t i = 0; i <= words.size(); ++i) {
        const bool newRow = i == words.size();  // The last row adds a word, as in Microsoft's tool.
        if (newRow && view_ != View::User) break;
        std::wstring number = std::to_wstring(i + 1);
        LVITEMW item = {LVIF_TEXT | LVIF_PARAM};
        item.iItem = static_cast<int>(i);
        item.pszText = number.data();
        item.lParam = newRow ? -1 : static_cast<LPARAM>(i);
        ListView_InsertItem(list_, &item);
        if (newRow) {
            ListView_SetItemText(list_, i, 1, const_cast<wchar_t*>(L"（按兩下新增詞彙）"));
            continue;
        }
        std::wstring text = Wide(words[i].text), radicals = RadicalsFor(words[i]);
        std::wstring third = view_ == View::User ? WideAscii(words[i].shortcut) : std::to_wstring(words[i].count);
        ListView_SetItemText(list_, i, 1, text.data());
        ListView_SetItemText(list_, i, 2, radicals.data());
        ListView_SetItemText(list_, i, 3, third.data());
    }
    if (selectWord >= 0 && selectWord < static_cast<int>(words.size())) {
        ListView_SetItemState(list_, selectWord, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, selectWord, FALSE);
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
    UpdateStatus();
    UpdateCommands();
}

void UserWordsWindow::UpdateStatus() {
    std::wstring text = L"詞彙總數:" + std::to_wstring(Current().size());
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void UserWordsWindow::UpdateCommands() {
    const bool user = view_ == View::User;
    const bool any = !SelectedWords().empty();
    const bool one = SelectedWords().size() == 1;
    struct State { int id; bool enabled; };
    const State states[] = {
        {ID_UW_UNDELETE, !undo_.empty()}, {ID_UW_INSERT, user},       {ID_UW_DELETE, any},
        {ID_UW_MODIFY, user && one},      {ID_UW_MOVE, !user && any}, {ID_UW_SAVE, userDirty_ || learnedDirty_},
        {ID_UW_EXPORT, user},             {ID_UW_IMPORT, user},
    };
    HMENU menu = GetMenu(hwnd_);
    for (const State& s : states) {
        EnableMenuItem(menu, s.id, MF_BYCOMMAND | (s.enabled ? MF_ENABLED : MF_GRAYED));
        SendMessageW(toolbar_, TB_ENABLEBUTTON, s.id, MAKELPARAM(s.enabled, 0));
    }
    CheckMenuRadioItem(menu, ID_UW_VIEW_USER, ID_UW_VIEW_LEARNED, user ? ID_UW_VIEW_USER : ID_UW_VIEW_LEARNED,
                       MF_BYCOMMAND);
    CheckMenuItem(menu, ID_UW_SORT_PHRASE, MF_BYCOMMAND | (sort_ == Sort::Phrase ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_UW_SORT_READING, MF_BYCOMMAND | (sort_ == Sort::Reading ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_UW_SORT_SHORTCUT, MF_BYCOMMAND | (sort_ == Sort::Shortcut ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_UW_VIEW_TOOLBAR, MF_BYCOMMAND | (showToolbar_ ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, ID_UW_VIEW_STATUS, MF_BYCOMMAND | (showStatus_ ? MF_CHECKED : MF_UNCHECKED));
}

void UserWordsWindow::SetView(View view) {
    view_ = view;
    TabCtrl_SetCurSel(tabs_, view == View::User ? 0 : 1);
    LVCOLUMNW c = {LVCF_TEXT};
    c.pszText = const_cast<wchar_t*>(view == View::User ? L"使用者造詞" : L"常用新詞");
    ListView_SetColumn(list_, 1, &c);
    c.pszText = const_cast<wchar_t*>(view == View::User ? L"捷徑" : L"使用次數");
    ListView_SetColumn(list_, 3, &c);
    std::wstring title = std::wstring(kAppTitle) + (view == View::User ? L" - [使用者造詞]" : L" - [常用新詞]");
    SetWindowTextW(hwnd_, title.c_str());
    Refresh(0);
}

void UserWordsWindow::SortBy(Sort sort) {
    sort_ = sort;
    auto& words = Current();
    std::stable_sort(words.begin(), words.end(), [&](const Word& a, const Word& b) {
        switch (sort) {
        case Sort::Phrase: return a.text < b.text;
        case Sort::Reading: return ReadingOf(a) < ReadingOf(b);
        case Sort::Shortcut:
            if (a.shortcut.empty() != b.shortcut.empty()) return !a.shortcut.empty();
            return a.shortcut < b.shortcut;
        default: return false;
        }
    });
    Refresh(0);
}

std::vector<int> UserWordsWindow::SelectedWords() const {
    std::vector<int> out;
    if (!list_) return out;
    for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0;
         i = ListView_GetNextItem(list_, i, LVNI_SELECTED)) {
        LVITEMW item = {LVIF_PARAM};
        item.iItem = i;
        ListView_GetItem(list_, &item);
        if (item.lParam >= 0) out.push_back(static_cast<int>(item.lParam));
    }
    return out;
}

void UserWordsWindow::Command(int id) {
    switch (id) {
    case ID_UW_IMPORT: Import(); break;
    case ID_UW_EXPORT: Export(); break;
    case ID_UW_SAVE: Save(); break;
    case ID_UW_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case ID_UW_UNDELETE: Undelete(); break;
    case ID_UW_INSERT: Insert(); break;
    case ID_UW_DELETE: Delete(); break;
    case ID_UW_MODIFY: Modify(); break;
    case ID_UW_MOVE: MoveToUser(); break;
    case ID_UW_SELECTALL:
        for (int i = 0; i < static_cast<int>(Current().size()); ++i)
            ListView_SetItemState(list_, i, LVIS_SELECTED, LVIS_SELECTED);
        break;
    case ID_UW_FIND: Find(); break;
    case ID_UW_VIEW_USER: SetView(View::User); break;
    case ID_UW_VIEW_LEARNED: SetView(View::Learned); break;
    case ID_UW_SORT_PHRASE: SortBy(Sort::Phrase); break;
    case ID_UW_SORT_READING: SortBy(Sort::Reading); break;
    case ID_UW_SORT_SHORTCUT: SortBy(Sort::Shortcut); break;
    case ID_UW_VIEW_TOOLBAR:
        showToolbar_ = !showToolbar_;
        ShowWindow(toolbar_, showToolbar_ ? SW_SHOW : SW_HIDE);
        Layout();
        break;
    case ID_UW_VIEW_STATUS:
        showStatus_ = !showStatus_;
        ShowWindow(status_, showStatus_ ? SW_SHOW : SW_HIDE);
        Layout();
        break;
    case ID_UW_ABOUT:
        MessageBoxW(hwnd_,
                    L"NovaChangJie 使用者造詞工具\n\n"
                    L"使用者造詞：自訂詞彙，以詞語碼 (例如 倉頡 = orgc) 或捷徑鍵輸入。\n"
                    L"常用新詞：輸入法從您的輸入中學到的詞彙，可刪除或移至使用者造詞。\n\n"
                    L"變更在按「儲存」後生效。",
                    kAppTitle, MB_ICONINFORMATION);
        break;
    }
    UpdateCommands();
}

void UserWordsWindow::OnNotify(NMHDR* hdr, LRESULT& result, bool& handled) {
    if (hdr->hwndFrom == list_) {
        switch (hdr->code) {
        case NM_DBLCLK:
        case NM_RETURN: {
            int focused = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
            LVITEMW item = {LVIF_PARAM};
            item.iItem = focused;
            if (focused >= 0 && ListView_GetItem(list_, &item) && item.lParam < 0) Insert();
            else if (view_ == View::User) Modify();
            handled = true;
            break;
        }
        case LVN_ITEMCHANGED:
            UpdateCommands();
            break;
        case NM_CUSTOMDRAW: {
            auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                result = CDRF_NOTIFYITEMDRAW;
                handled = true;
            } else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                if (static_cast<LPARAM>(cd->nmcd.lItemlParam) < 0) cd->clrText = GetSysColor(COLOR_GRAYTEXT);
                result = CDRF_DODEFAULT;
                handled = true;
            }
            break;
        }
        }
    } else if (hdr->hwndFrom == tabs_ && hdr->code == TCN_SELCHANGE) {
        SetView(TabCtrl_GetCurSel(tabs_) == 0 ? View::User : View::Learned);
        handled = true;
    } else if (hdr->code == TTN_GETDISPINFOW) {
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(hdr);
        static const std::pair<UINT_PTR, const wchar_t*> tips[] = {
            {ID_UW_IMPORT, L"匯入"},   {ID_UW_EXPORT, L"匯出"}, {ID_UW_SAVE, L"儲存"},
            {ID_UW_INSERT, L"新增"},   {ID_UW_DELETE, L"刪除"}, {ID_UW_UNDELETE, L"復原刪除"},
            {ID_UW_MODIFY, L"修改"},   {ID_UW_MOVE, L"移至使用者造詞"},
            {ID_UW_FIND, L"尋找"},     {ID_UW_ABOUT, L"說明"},
        };
        for (const auto& [id, text] : tips)
            if (hdr->idFrom == id) info->lpszText = const_cast<wchar_t*>(text);
        handled = true;
    }
}

void UserWordsWindow::Insert() {
    if (view_ != View::User) return;
    Word w;
    if (!EditWord(w, -1)) return;
    user_.push_back(std::move(w));
    userDirty_ = true;
    Refresh(static_cast<int>(user_.size()) - 1);
}

void UserWordsWindow::Modify() {
    std::vector<int> sel = SelectedWords();
    if (view_ != View::User || sel.size() != 1) return;
    Word w = user_[sel[0]];
    if (!EditWord(w, sel[0])) return;
    user_[sel[0]] = std::move(w);
    userDirty_ = true;
    Refresh(sel[0]);
}

void UserWordsWindow::Delete() {
    std::vector<int> sel = SelectedWords();
    if (sel.empty()) return;
    std::vector<Word>& words = Current();
    std::vector<Word> removed;
    std::sort(sel.rbegin(), sel.rend());
    for (int i : sel) {
        removed.push_back(words[i]);
        if (view_ == View::Learned) removedLearned_.push_back(words[i].text);
        words.erase(words.begin() + i);
    }
    std::reverse(removed.begin(), removed.end());
    undo_.emplace_back(view_, std::move(removed));
    (view_ == View::User ? userDirty_ : learnedDirty_) = true;
    Refresh(std::min(sel.back(), static_cast<int>(words.size()) - 1));
}

void UserWordsWindow::Undelete() {
    if (undo_.empty()) return;
    auto [view, words] = std::move(undo_.back());
    undo_.pop_back();
    if (view != view_) SetView(view);
    std::vector<Word>& target = Current();
    for (Word& w : words) {
        if (view == View::Learned)
            removedLearned_.erase(std::remove(removedLearned_.begin(), removedLearned_.end(), w.text),
                                  removedLearned_.end());
        target.push_back(std::move(w));
    }
    (view == View::User ? userDirty_ : learnedDirty_) = true;
    Refresh(static_cast<int>(target.size()) - 1);
}

void UserWordsWindow::MoveToUser() {
    if (view_ != View::Learned) return;
    std::vector<int> sel = SelectedWords();
    if (sel.empty()) return;
    std::sort(sel.rbegin(), sel.rend());
    size_t moved = 0;
    for (int i : sel) {
        const Word& w = learned_[i];
        bool exists = std::any_of(user_.begin(), user_.end(), [&](const Word& u) { return u.text == w.text; });
        if (!exists) {
            Word copy;
            copy.text = w.text;
            user_.push_back(std::move(copy));
            ++moved;
        }
        removedLearned_.push_back(w.text);
        learned_.erase(learned_.begin() + i);
    }
    userDirty_ = learnedDirty_ = true;
    Refresh(0);
    std::wstring msg = L"已將 " + std::to_wstring(moved) + L" 個詞彙移至使用者造詞。";
    MessageBoxW(hwnd_, msg.c_str(), kAppTitle, MB_ICONINFORMATION);
}

INT_PTR CALLBACK UserWordsWindow::FindProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    static std::wstring* text = nullptr;
    switch (msg) {
    case WM_INITDIALOG:
        text = reinterpret_cast<std::wstring*>(lParam);
        SetDlgItemTextW(dlg, IDC_FIND_TEXT, text->c_str());
        SendDlgItemMessageW(dlg, IDC_FIND_TEXT, EM_SETSEL, 0, -1);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buf[256] = {};
            GetDlgItemTextW(dlg, IDC_FIND_TEXT, buf, 256);
            *text = buf;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

void UserWordsWindow::Find() {
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_WORD_FIND), hwnd_, FindProc,
                        reinterpret_cast<LPARAM>(&findText_)) != IDOK || findText_.empty())
        return;
    const std::u16string needle = Narrow16(findText_);
    const auto& words = Current();
    std::vector<int> sel = SelectedWords();
    const int n = static_cast<int>(words.size());
    const int from = sel.empty() ? 0 : sel.front() + 1;
    for (int k = 0; k < n; ++k) {
        int i = (from + k) % n;
        if (words[i].text.find(needle) != std::u16string::npos ||
            (!words[i].shortcut.empty() && Narrow16(WideAscii(words[i].shortcut)) == needle)) {
            ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(list_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list_, i, FALSE);
            SetFocus(list_);
            return;
        }
    }
    MessageBoxW(hwnd_, (L"找不到「" + findText_ + L"」。").c_str(), kAppTitle, MB_ICONINFORMATION);
}

void UserWordsWindow::Import() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"文字檔 (*.txt)\0*.txt\0所有檔案 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string data;
    if (!ReadFileUtf8(file, data)) {
        MessageBoxW(hwnd_, L"無法讀取檔案。", kAppTitle, MB_ICONERROR);
        return;
    }
    size_t added = 0;
    for (Word& w : ParseUserWords(data)) {
        bool exists = std::any_of(user_.begin(), user_.end(), [&](const Word& u) { return u.text == w.text; });
        if (!exists) {
            user_.push_back(std::move(w));
            ++added;
        }
    }
    if (added) userDirty_ = true;
    if (view_ != View::User) SetView(View::User); else Refresh(0);
    std::wstring msg = L"已匯入 " + std::to_wstring(added) + L" 個詞彙。";
    MessageBoxW(hwnd_, msg.c_str(), kAppTitle, MB_ICONINFORMATION);
}

void UserWordsWindow::Export() {
    wchar_t file[MAX_PATH] = L"user_phrases.txt";
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"文字檔 (*.txt)\0*.txt\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    if (!WriteFileUtf8(file, SerializeUserWords(user_)))
        MessageBoxW(hwnd_, L"無法寫入檔案。", kAppTitle, MB_ICONERROR);
}

// ---------------------------------------------------------------------------
// 詞彙編輯 dialog: the word, each character's radicals (a dropdown when the
// character has more than one code) and an optional shortcut.

bool UserWordsWindow::EditWord(Word& word, int ignoreIndex) {
    EditState state{this, &word, ignoreIndex};
    return DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_WORD_EDIT), hwnd_, EditProc,
                           reinterpret_cast<LPARAM>(&state)) == IDOK;
}

INT_PTR CALLBACK UserWordsWindow::EditProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<EditState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    auto readText = [&]() {
        int len = GetWindowTextLengthW(GetDlgItem(dlg, IDC_WE_TEXT));
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        GetDlgItemTextW(dlg, IDC_WE_TEXT, text.data(), len + 1);
        text.resize(static_cast<size_t>(len));
        std::wstring clean;
        for (wchar_t c : text)
            if (c != L'\r' && c != L'\n' && c != L' ' && c != L'\t' && c != L'　') clean += c;
        return Narrow16(clean);
    };
    auto readShortcut = [&]() {
        wchar_t buf[32] = {};
        GetDlgItemTextW(dlg, IDC_WE_SHORTCUT, buf, 32);
        std::string s;
        for (const wchar_t* p = buf; *p; ++p) s += static_cast<char>(*p < 128 ? *p : '?');
        return s;
    };
    // Rebuilds the character rows; returns whether every character has a code.
    auto fillRows = [&](const std::vector<std::string>& keep) {
        std::vector<char32_t> cps = CodePoints(readText());
        bool allCoded = !cps.empty();
        st->chars = cps.size();
        for (size_t i = 0; i < kMaxCodedChars; ++i) {
            HWND label = GetDlgItem(dlg, IDC_WE_CHAR0 + static_cast<int>(i));
            HWND combo = GetDlgItem(dlg, IDC_WE_CODE0 + static_cast<int>(i));
            std::u16string ch;
            if (i < cps.size()) AppendCodePoint(ch, cps[i]);
            SetWindowTextW(label, Wide(ch).c_str());
            // Keep the user's pick if the character and its list are unchanged.
            int previous = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
            std::wstring oldFirst;
            if (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0) {
                wchar_t buf[64] = {};
                SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(buf));
                oldFirst = buf;
            }
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            const std::vector<std::string>* codes = i < cps.size() ? st->self->table_.Codes(cps[i], st->self->Cj3()) : nullptr;
            if (i < cps.size() && !codes) allCoded = false;
            int select = 0;
            if (codes) {
                for (size_t k = 0; k < codes->size(); ++k) {
                    std::wstring item = Radicals((*codes)[k]) + L"  (" + WideAscii((*codes)[k]) + L")";
                    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
                    if (i < keep.size() && keep[i] == (*codes)[k]) select = static_cast<int>(k);
                }
                wchar_t first[64] = {};
                SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(first));
                if (keep.empty() && oldFirst == first && previous >= 0) select = previous;
                SendMessageW(combo, CB_SETCURSEL, select, 0);
            }
            EnableWindow(combo, codes && codes->size() > 1);
        }
        if (cps.size() > kMaxCodedChars) allCoded = false;
        return allCoded;
    };
    auto validate = [&]() {
        std::vector<char32_t> cps = CodePoints(readText());
        bool allCoded = !cps.empty() && cps.size() <= kMaxCodedChars;
        for (char32_t cp : cps)
            if (!st->self->table_.Codes(cp, st->self->Cj3())) allCoded = false;
        std::string sc = readShortcut();
        bool scOk = sc.empty() || IsCodeText(sc);
        bool ok = !cps.empty() && scOk && (allCoded || !sc.empty());
        EnableWindow(GetDlgItem(dlg, IDOK), ok);
    };

    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<EditState*>(lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        SendDlgItemMessageW(dlg, IDC_WE_SHORTCUT, EM_LIMITTEXT, kCodeBytes - 1, 0);
        SetDlgItemTextW(dlg, IDC_WE_TEXT, Wide(st->word->text).c_str());
        SetDlgItemTextW(dlg, IDC_WE_SHORTCUT, WideAscii(st->word->shortcut).c_str());
        fillRows(st->self->Chosen(*st->word));
        validate();
        SetWindowTextW(dlg, st->ignoreIndex < 0 ? L"詞彙編輯 - 新增" : L"詞彙編輯 - 修改");
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == IDC_WE_TEXT && code == EN_CHANGE) {
            fillRows({});
            validate();
        } else if (id == IDC_WE_SHORTCUT && code == EN_CHANGE) {
            validate();
        } else if (id == IDOK) {
            std::u16string text = readText();
            std::string sc = readShortcut();
            const auto& words = st->self->user_;
            for (size_t i = 0; i < words.size(); ++i) {
                if (static_cast<int>(i) == st->ignoreIndex) continue;
                if (words[i].text == text) {
                    MessageBoxW(dlg, L"此詞彙已經存在。", L"詞彙編輯", MB_ICONWARNING);
                    return TRUE;
                }
                if (!sc.empty() && words[i].shortcut == sc) {
                    MessageBoxW(dlg, L"此捷徑鍵已被其他詞彙使用。", L"詞彙編輯", MB_ICONWARNING);
                    return TRUE;
                }
            }
            Word& w = *st->word;
            w.text = text;
            w.shortcut = sc;
            // Store non-primary choices only; the other version's choices stay.
            std::vector<std::string> chosen;
            bool any = false;
            std::vector<char32_t> cps = CodePoints(text);
            for (size_t i = 0; i < cps.size() && i < kMaxCodedChars; ++i) {
                HWND combo = GetDlgItem(dlg, IDC_WE_CODE0 + static_cast<int>(i));
                int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
                const auto* codes = st->self->table_.Codes(cps[i], st->self->Cj3());
                std::string c = codes && sel > 0 && sel < static_cast<int>(codes->size()) ? (*codes)[sel] : "";
                any = any || !c.empty();
                chosen.push_back(c);
            }
            (st->self->Cj3() ? w.chosen3 : w.chosen5) = any ? chosen : std::vector<std::string>();
            EndDialog(dlg, IDOK);
        } else if (id == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
        }
        return TRUE;
    }
    }
    return FALSE;
}

}  // namespace

int RunUserWordsTool(HINSTANCE instance) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\NovaChangJieUserWords");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kClassName, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    UserWordsWindow window(instance);
    int rc = window.Run();
    if (mutex) CloseHandle(mutex);
    return rc;
}

}  // namespace nova
