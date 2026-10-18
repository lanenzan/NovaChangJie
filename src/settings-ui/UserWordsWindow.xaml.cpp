#include "pch.h"

#include "UserWordsWindow.xaml.h"
#if __has_include("UserWordsWindow.g.cpp")
#include "UserWordsWindow.g.cpp"
#endif
#if __has_include("WordItem.g.cpp")
#include "WordItem.g.cpp"
#endif

#include <algorithm>

#include "SettingsStore.h"
#include "Unicode.h"
#include "WindowHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Windows::Foundation;
using NovaUi::H;
using NovaUi::U16;
using nova::AppendCodePoint;
using nova::CodePoints;
using nova::Utf8ToUtf16;

namespace winrt::NovaSettings::implementation {

namespace {

hstring Ascii(std::string const& s) { return hstring(std::wstring(s.begin(), s.end())); }

std::string FromAscii(hstring const& s) {
    std::string out;
    for (wchar_t c : s) out += c < 128 ? static_cast<char>(c) : '?';
    return out;
}

// The word typed in the editor without spaces and line breaks.
std::u16string CleanWord(hstring const& text) {
    std::u16string out;
    for (wchar_t c : text)
        if (c != L'\r' && c != L'\n' && c != L' ' && c != L'\t' && c != L'\x3000') out += static_cast<char16_t>(c);
    return out;
}

// Common item dialog for 匯入 / 匯出; empty when cancelled.
std::wstring PickFile(HWND owner, bool save) {
    com_ptr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put()))))
        return {};
    const COMDLG_FILTERSPEC types[] = {{L"文字檔 (*.txt)", L"*.txt"}, {L"所有檔案 (*.*)", L"*.*"}};
    dialog->SetFileTypes(save ? 1 : 2, types);
    dialog->SetDefaultExtension(L"txt");
    if (save) dialog->SetFileName(L"user_phrases.txt");
    if (FAILED(dialog->Show(owner))) return {};
    com_ptr<IShellItem> item;
    wil::unique_cotaskmem_string path;
    if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    return path.get();
}

}  // namespace

void UserWordsWindow::InitializeComponent() {
    UserWordsWindowT::InitializeComponent();
    Title(NovaUi::kUserWordsTitle);
    NovaUi::SetupWindow(*this, TitleBar(), 1000, 680);
    TitleIcon().Source(Media::Imaging::BitmapImage(Uri(L"file:///" + NovaUi::AppDir() + L"Assets\\NovaChangJie.ico")));
    Words().ItemsSource(items_);

    cj3_ = nova::LoadOptions().version == nova::CangjieVersion::V3;
    if (!table_.Load(NovaUi::InstallDir() + L"nova.dict"))
        ShowStatus(L"找不到 nova.dict，無法顯示字根。", InfoBarSeverity::Warning);
    user_ = nova::LoadUserWords();
    learned_ = nova::LoadLearnedWords();

    // Unsaved changes: ask before closing.
    AppWindow().Closing([this](auto&&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
        if (closing_ || (!userDirty_ && !learnedDirty_)) return;
        args.Cancel(true);
        ConfirmClose();
    });
    SetView(View::User);
}

// ---------------------------------------------------------------------------
// List

void UserWordsWindow::Refresh(int selectWord) {
    items_.Clear();
    auto& words = Current();
    int shown = 0, selectRow = -1;
    for (size_t i = 0; i < words.size(); ++i) {
        const nova::UserWord& w = words[i];
        if (!filter_.empty() && w.text.find(filter_) == std::u16string::npos &&
            !(view_ == View::User && !w.shortcut.empty() && Utf8ToUtf16(w.shortcut) == filter_))
            continue;
        hstring extra = view_ == View::User ? Ascii(w.shortcut) : hstring(std::to_wstring(w.count));
        items_.Append(make<WordItem>(hstring(std::to_wstring(shown + 1)), H(w.text),
                                     H(nova::WordRadicals(w, table_, Cj3())), extra, static_cast<int32_t>(i)));
        if (static_cast<int>(i) == selectWord) selectRow = shown;
        ++shown;
    }
    if (selectRow >= 0) {
        Words().SelectedIndex(selectRow);
        Words().ScrollIntoView(items_.GetAt(selectRow));
    }
    const bool empty = items_.Size() == 0;
    EmptyHint().Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
    EmptyText().Text(!filter_.empty()       ? L"找不到符合的詞彙。"
                     : view_ == View::User ? L"還沒有使用者造詞。按「新增」建立第一個詞彙。"
                                           : L"還沒有學到的詞彙。");
    std::wstring count = L"詞彙總數：" + std::to_wstring(words.size());
    if (!filter_.empty()) count += L"，符合：" + std::to_wstring(shown);
    CountText().Text(count);
    UpdateCommands();
}

std::vector<int> UserWordsWindow::SelectedWords() {
    std::vector<int> out;
    for (auto const& item : Words().SelectedItems())
        if (auto w = item.try_as<NovaSettings::WordItem>()) out.push_back(w.Index());
    return out;
}

void UserWordsWindow::UpdateCommands() {
    const bool user = view_ == View::User;
    const size_t selected = SelectedWords().size();
    AddButton().IsEnabled(user);
    EditButton().IsEnabled(user && selected == 1);
    DeleteButton().IsEnabled(selected > 0);
    UndoButton().IsEnabled(!undo_.empty());
    MoveButton().Visibility(user ? Visibility::Collapsed : Visibility::Visible);
    MoveButton().IsEnabled(!user && selected > 0);
    SaveButton().IsEnabled(userDirty_ || learnedDirty_);
    ImportButton().IsEnabled(user);
    ExportButton().IsEnabled(user);
    SortShortcutButton().IsEnabled(user);
    Title(hstring(NovaUi::kUserWordsTitle) + (userDirty_ || learnedDirty_ ? L" *" : L""));
    TitleText().Text(Title());
}

void UserWordsWindow::SetView(View view) {
    view_ = view;
    WordHeader().Text(view == View::User ? L"使用者造詞" : L"常用新詞");
    ExtraHeader().Text(view == View::User ? L"捷徑" : L"使用次數");
    Refresh(0);
}

void UserWordsWindow::Views_SelectionChanged(SelectorBar const& sender, SelectorBarSelectionChangedEventArgs const&) {
    SetView(sender.SelectedItem() == LearnedViewItem() ? View::Learned : View::User);
}

void UserWordsWindow::Search_TextChanged(AutoSuggestBox const& sender, AutoSuggestBoxTextChangedEventArgs const&) {
    filter_ = CleanWord(sender.Text());
    Refresh();
}

void UserWordsWindow::Sort_Click(IInspectable const& sender, RoutedEventArgs const&) {
    const hstring by = unbox_value<hstring>(sender.as<AppBarButton>().Tag());
    auto& words = Current();
    std::stable_sort(words.begin(), words.end(), [&](const nova::UserWord& a, const nova::UserWord& b) {
        if (by == L"Reading")
            return nova::WordReading(a, table_, Cj3()) < nova::WordReading(b, table_, Cj3());
        if (by == L"Shortcut") {
            if (a.shortcut.empty() != b.shortcut.empty()) return !a.shortcut.empty();
            return a.shortcut < b.shortcut;
        }
        return a.text < b.text;
    });
    if (view_ == View::User) userDirty_ = true;  // The order is saved.
    Refresh(0);
}

void UserWordsWindow::Words_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) { UpdateCommands(); }

fire_and_forget UserWordsWindow::Words_DoubleTapped(IInspectable const&, DoubleTappedRoutedEventArgs const&) {
    if (view_ == View::User) co_await EditSelected();
}

fire_and_forget UserWordsWindow::Words_KeyDown(IInspectable const&, KeyRoutedEventArgs const& args) {
    if (args.Key() == Windows::System::VirtualKey::Delete) {
        args.Handled(true);
        DeleteSelected();
    } else if (args.Key() == Windows::System::VirtualKey::Enter && view_ == View::User) {
        args.Handled(true);
        co_await EditSelected();
    }
}

// ---------------------------------------------------------------------------
// Commands

fire_and_forget UserWordsWindow::Add_Click(IInspectable const&, RoutedEventArgs const&) { co_await AddWord(); }
fire_and_forget UserWordsWindow::Edit_Click(IInspectable const&, RoutedEventArgs const&) { co_await EditSelected(); }
void UserWordsWindow::Delete_Click(IInspectable const&, RoutedEventArgs const&) { DeleteSelected(); }

IAsyncAction UserWordsWindow::AddWord() {
    auto strong = get_strong();
    if (view_ != View::User || dialogOpen_) co_return;
    nova::UserWord w;
    if (!co_await EditWordAsync(&w, -1)) co_return;
    user_.push_back(std::move(w));
    userDirty_ = true;
    filter_.clear();
    SearchBox().Text(L"");
    Refresh(static_cast<int>(user_.size()) - 1);
}

IAsyncAction UserWordsWindow::EditSelected() {
    auto strong = get_strong();
    std::vector<int> sel = SelectedWords();
    if (view_ != View::User || sel.size() != 1 || dialogOpen_) co_return;
    const int index = sel[0];
    nova::UserWord w = user_[index];
    if (!co_await EditWordAsync(&w, index)) co_return;
    user_[index] = std::move(w);
    userDirty_ = true;
    Refresh(index);
}

void UserWordsWindow::DeleteSelected() {
    std::vector<int> sel = SelectedWords();
    if (sel.empty()) return;
    auto& words = Current();
    std::vector<nova::UserWord> removed;
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
    ShowStatus(hstring(L"已刪除 " + std::to_wstring(sel.size()) + L" 個詞彙。按 Ctrl+Z 可復原。"),
               InfoBarSeverity::Informational);
}

void UserWordsWindow::Undo_Click(IInspectable const&, RoutedEventArgs const&) {
    if (undo_.empty()) return;
    auto [view, words] = std::move(undo_.back());
    undo_.pop_back();
    if (view != view_) (view == View::User ? UserViewItem() : LearnedViewItem()).IsSelected(true);
    view_ = view;
    auto& target = Current();
    for (nova::UserWord& w : words) {
        if (view == View::Learned)
            removedLearned_.erase(std::remove(removedLearned_.begin(), removedLearned_.end(), w.text),
                                  removedLearned_.end());
        target.push_back(std::move(w));
    }
    (view == View::User ? userDirty_ : learnedDirty_) = true;
    SetView(view);
    Refresh(static_cast<int>(target.size()) - 1);
}

void UserWordsWindow::Move_Click(IInspectable const&, RoutedEventArgs const&) {
    if (view_ != View::Learned) return;
    std::vector<int> sel = SelectedWords();
    if (sel.empty()) return;
    std::sort(sel.rbegin(), sel.rend());
    size_t moved = 0;
    for (int i : sel) {
        const nova::UserWord& w = learned_[i];
        if (std::none_of(user_.begin(), user_.end(), [&](const nova::UserWord& u) { return u.text == w.text; })) {
            nova::UserWord copy;
            copy.text = w.text;
            user_.push_back(std::move(copy));
            ++moved;
        }
        removedLearned_.push_back(w.text);
        learned_.erase(learned_.begin() + i);
    }
    userDirty_ = learnedDirty_ = true;
    Refresh(0);
    ShowStatus(hstring(L"已將 " + std::to_wstring(moved) + L" 個詞彙移至使用者造詞。"), InfoBarSeverity::Success);
}

bool UserWordsWindow::Save() {
    if (userDirty_) {
        if (!nova::SaveUserWords(user_)) {
            ShowStatus(L"無法儲存使用者造詞。", InfoBarSeverity::Error);
            return false;
        }
        userDirty_ = false;
    }
    if (learnedDirty_) {
        if (!nova::RemoveLearnedWords(removedLearned_)) {
            ShowStatus(L"無法儲存常用新詞。", InfoBarSeverity::Error);
            return false;
        }
        removedLearned_.clear();
        learnedDirty_ = false;
    }
    UpdateCommands();
    ShowStatus(L"已儲存。", InfoBarSeverity::Success);
    return true;
}

void UserWordsWindow::Save_Click(IInspectable const&, RoutedEventArgs const&) { Save(); }

void UserWordsWindow::Import_Click(IInspectable const&, RoutedEventArgs const&) {
    std::wstring path = PickFile(NovaUi::WindowHandle(*this), false);
    if (path.empty()) return;
    std::string data;
    if (!nova::ReadFileUtf8(path, data)) {
        ShowStatus(L"無法讀取檔案。", InfoBarSeverity::Error);
        return;
    }
    size_t added = 0;
    for (nova::UserWord& w : nova::ParseUserWords(data)) {
        if (std::any_of(user_.begin(), user_.end(), [&](const nova::UserWord& u) { return u.text == w.text; }))
            continue;
        user_.push_back(std::move(w));
        ++added;
    }
    if (added) userDirty_ = true;
    Refresh(0);
    ShowStatus(hstring(L"已匯入 " + std::to_wstring(added) + L" 個詞彙。"), InfoBarSeverity::Success);
}

void UserWordsWindow::Export_Click(IInspectable const&, RoutedEventArgs const&) {
    std::wstring path = PickFile(NovaUi::WindowHandle(*this), true);
    if (path.empty()) return;
    if (nova::WriteFileUtf8(path, nova::SerializeUserWords(user_)))
        ShowStatus(L"已匯出。", InfoBarSeverity::Success);
    else
        ShowStatus(L"無法寫入檔案。", InfoBarSeverity::Error);
}

void UserWordsWindow::Save_Accelerator(KeyboardAccelerator const&, KeyboardAcceleratorInvokedEventArgs const& args) {
    args.Handled(true);
    if (userDirty_ || learnedDirty_) Save();
}

void UserWordsWindow::Undo_Accelerator(KeyboardAccelerator const&, KeyboardAcceleratorInvokedEventArgs const& args) {
    args.Handled(true);
    Undo_Click(nullptr, nullptr);
}

fire_and_forget UserWordsWindow::Add_Accelerator(KeyboardAccelerator const&,
                                                 KeyboardAcceleratorInvokedEventArgs const& args) {
    args.Handled(true);
    co_await AddWord();
}

void UserWordsWindow::Find_Accelerator(KeyboardAccelerator const&, KeyboardAcceleratorInvokedEventArgs const& args) {
    args.Handled(true);
    SearchBox().Focus(FocusState::Keyboard);
}

void UserWordsWindow::ShowStatus(hstring const& message, InfoBarSeverity severity) {
    Status().Message(message);
    Status().Severity(severity);
    Status().IsOpen(true);
}

fire_and_forget UserWordsWindow::ConfirmClose() {
    auto strong = get_strong();
    if (dialogOpen_) co_return;
    dialogOpen_ = true;
    ContentDialog dialog;
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(L"使用者造詞工具"));
    dialog.Content(box_value(L"詞彙已變更，是否儲存？"));
    dialog.PrimaryButtonText(L"儲存");
    dialog.SecondaryButtonText(L"不儲存");
    dialog.CloseButtonText(L"取消");
    dialog.DefaultButton(ContentDialogButton::Primary);
    ContentDialogResult result = co_await dialog.ShowAsync();
    dialogOpen_ = false;
    if (result == ContentDialogResult::None) co_return;
    if (result == ContentDialogResult::Primary && !Save()) co_return;
    closing_ = true;
    Close();
}

// ---------------------------------------------------------------------------
// 詞彙編輯: the word, each character's code (a choice when it has more than
// one) and an optional shortcut.

IAsyncOperation<bool> UserWordsWindow::EditWordAsync(nova::UserWord* word, int ignoreIndex) {
    auto strong = get_strong();
    dialogOpen_ = true;

    StackPanel panel;
    panel.Spacing(12);
    panel.MinWidth(440);
    TextBlock info;
    info.TextWrapping(TextWrapping::Wrap);
    info.Foreground(Application::Current().Resources().Lookup(box_value(L"TextFillColorSecondaryBrush")).as<Media::Brush>());
    info.Text(L"詞彙以每個字的字根組成詞語碼輸入。超過 9 個字或含有沒有字根的字時，請指派捷徑鍵 (例如 sck)，"
              L"以 ` P 加捷徑鍵輸入。");
    TextBox wordBox;
    wordBox.Header(box_value(L"詞彙"));
    wordBox.Text(H(word->text));
    StackPanel rows;
    rows.Spacing(6);
    TextBlock rowsHeader;
    rowsHeader.Text(L"字根");
    TextBox shortcutBox;
    shortcutBox.Header(box_value(L"捷徑鍵 (可留空)"));
    shortcutBox.CharacterCasing(CharacterCasing::Lower);
    shortcutBox.MaxLength(nova::kCodeBytes - 1);
    shortcutBox.Text(Ascii(word->shortcut));
    InfoBar error;
    error.Severity(InfoBarSeverity::Error);
    error.IsClosable(false);
    for (UIElement e : {UIElement(info), UIElement(wordBox), UIElement(rowsHeader), UIElement(rows),
                        UIElement(shortcutBox), UIElement(error)})
        panel.Children().Append(e);
    ScrollViewer scroller;
    scroller.Content(panel);

    ContentDialog dialog;
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(ignoreIndex < 0 ? L"新增詞彙" : L"修改詞彙"));
    dialog.Content(scroller);
    dialog.PrimaryButtonText(L"確定");
    dialog.CloseButtonText(L"取消");
    dialog.DefaultButton(ContentDialogButton::Primary);

    // Character rows: a code choice per character.
    std::vector<std::u16string> rowChars;
    std::vector<ComboBox> rowBoxes;
    const std::vector<std::string> initialChosen = Cj3() ? word->chosen3 : word->chosen5;
    auto fillRows = [&, this](bool keepInitial) {
        std::vector<char32_t> cps = CodePoints(CleanWord(wordBox.Text()));
        // Keep a pick when the character at that position is unchanged.
        std::vector<std::u16string> oldChars = rowChars;
        std::vector<int> oldPicks;
        for (auto const& box : rowBoxes) oldPicks.push_back(box.SelectedIndex());
        rows.Children().Clear();
        rowChars.clear();
        rowBoxes.clear();
        for (size_t i = 0; i < cps.size() && i < nova::kMaxCodedChars; ++i) {
            std::u16string ch;
            AppendCodePoint(ch, cps[i]);
            Grid row;
            row.ColumnSpacing(12);
            ColumnDefinition c0, c1;
            c0.Width(GridLengthHelper::FromPixels(32));
            c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            TextBlock label;
            label.Text(H(ch));
            label.FontSize(18);
            label.VerticalAlignment(VerticalAlignment::Center);
            ComboBox box;
            box.HorizontalAlignment(HorizontalAlignment::Stretch);
            Grid::SetColumn(box, 1);
            const std::vector<std::string>* codes = table_.Codes(cps[i], Cj3());
            if (codes) {
                int select = 0;
                for (size_t k = 0; k < codes->size(); ++k) {
                    box.Items().Append(box_value(H(nova::RadicalsOf((*codes)[k])) + L"  (" + Ascii((*codes)[k]) + L")"));
                    if (keepInitial && i < initialChosen.size() && initialChosen[i] == (*codes)[k])
                        select = static_cast<int>(k);
                }
                if (!keepInitial && i < oldChars.size() && oldChars[i] == ch && oldPicks[i] >= 0) select = oldPicks[i];
                box.SelectedIndex(select);
                box.IsEnabled(codes->size() > 1);
            } else {
                box.Items().Append(box_value(L"？ (沒有字根)"));
                box.SelectedIndex(0);
                box.IsEnabled(false);
            }
            row.Children().Append(label);
            row.Children().Append(box);
            rows.Children().Append(row);
            rowChars.push_back(ch);
            rowBoxes.push_back(box);
        }
    };
    auto validate = [&, this]() {
        std::vector<char32_t> cps = CodePoints(CleanWord(wordBox.Text()));
        bool allCoded = !cps.empty() && cps.size() <= nova::kMaxCodedChars;
        for (char32_t cp : cps)
            if (!table_.Codes(cp, Cj3())) allCoded = false;
        std::string sc = FromAscii(shortcutBox.Text());
        const bool scOk = sc.empty() || nova::IsCodeText(sc);
        dialog.IsPrimaryButtonEnabled(!cps.empty() && scOk && (allCoded || !sc.empty()));
        error.IsOpen(false);
        if (!scOk) {
            error.Severity(InfoBarSeverity::Error);
            error.Message(L"捷徑鍵只能使用英文字母 a-z。");
            error.IsOpen(true);
        } else if (!cps.empty() && !allCoded && sc.empty()) {
            error.Message(L"這個詞彙需要捷徑鍵才能輸入。");
            error.Severity(InfoBarSeverity::Warning);
            error.IsOpen(true);
        }
    };
    fillRows(true);
    validate();
    wordBox.TextChanged([&](auto&&, auto&&) {
        fillRows(false);
        validate();
    });
    shortcutBox.TextChanged([&](auto&&, auto&&) { validate(); });
    // Duplicates keep the dialog open.
    dialog.PrimaryButtonClick([&, this](ContentDialog const&, ContentDialogButtonClickEventArgs const& args) {
        std::u16string text = CleanWord(wordBox.Text());
        std::string sc = FromAscii(shortcutBox.Text());
        for (size_t i = 0; i < user_.size(); ++i) {
            if (static_cast<int>(i) == ignoreIndex) continue;
            const wchar_t* problem = user_[i].text == text                      ? L"此詞彙已經存在。"
                                     : !sc.empty() && user_[i].shortcut == sc ? L"此捷徑鍵已被其他詞彙使用。"
                                                                              : nullptr;
            if (problem) {
                error.Severity(InfoBarSeverity::Error);
                error.Message(problem);
                error.IsOpen(true);
                args.Cancel(true);
                return;
            }
        }
    });

    const ContentDialogResult result = co_await dialog.ShowAsync();
    dialogOpen_ = false;
    if (result != ContentDialogResult::Primary) co_return false;

    word->text = CleanWord(wordBox.Text());
    word->shortcut = FromAscii(shortcutBox.Text());
    // Store non-primary choices only; the other Cangjie version's choices stay.
    std::vector<std::string> chosen;
    bool any = false;
    std::vector<char32_t> cps = CodePoints(word->text);
    for (size_t i = 0; i < cps.size() && i < rowBoxes.size(); ++i) {
        const auto* codes = table_.Codes(cps[i], Cj3());
        const int sel = rowBoxes[i].SelectedIndex();
        std::string c = codes && sel > 0 && sel < static_cast<int>(codes->size()) ? (*codes)[sel] : std::string();
        any = any || !c.empty();
        chosen.push_back(c);
    }
    (Cj3() ? word->chosen3 : word->chosen5) = any ? chosen : std::vector<std::string>();
    co_return true;
}

}  // namespace winrt::NovaSettings::implementation
