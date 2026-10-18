#include "pch.h"

#include "SettingsWindow.xaml.h"
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

#include <array>

#include "App.xaml.h"
#include "SettingsStore.h"
#include "WindowHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

#ifndef NOVA_VERSION
#define NOVA_VERSION L"0.9.2"
#endif

namespace winrt::NovaSettings::implementation {

namespace {

constexpr std::array<uint32_t, 5> kPageSizes = {5, 6, 7, 8, 9};
constexpr std::array<uint32_t, 8> kFontSizes = {14, 16, 18, 20, 22, 24, 28, 32};

template <size_t N>
int IndexOf(const std::array<uint32_t, N>& values, uint32_t v) {
    int index = 0;
    for (size_t i = 0; i < N; ++i)
        if (values[i] <= v) index = static_cast<int>(i);
    return index;
}

Media::Imaging::BitmapImage AppIcon() {
    std::wstring path = NovaUi::AppDir() + L"Assets\\NovaChangJie.ico";
    return Media::Imaging::BitmapImage(Windows::Foundation::Uri(L"file:///" + path));
}

}  // namespace

void SettingsWindow::InitializeComponent() {
    SettingsWindowT::InitializeComponent();
    Title(NovaUi::kSettingsTitle);
    NovaUi::SetupWindow(*this, TitleBar(), 1000, 760);
    TitleIcon().Source(AppIcon());
    AboutIcon().Source(AppIcon());
    VersionText().Text(hstring(L"版本 ") + NOVA_VERSION);
    Load();
    Bind();
}

void SettingsWindow::Load() {
    options_ = nova::LoadOptions();
    StartMode().SelectedIndex(options_.startInChinese ? 0 : 1);
    CharsetBox().SelectedIndex(static_cast<int>(options_.charset));
    VersionBox().SelectedIndex(static_cast<int>(options_.version));
    LayoutBox().SelectedIndex(static_cast<int>(options_.layout));
    PageSizeBox().SelectedIndex(IndexOf(kPageSizes, options_.pageSize));
    FontSizeBox().SelectedIndex(IndexOf(kFontSizes, options_.fontSize));
    OrderBox().SelectedIndex(options_.order == nova::CandidateOrder::Traditional ? 1 : 0);
    UpdateDependents();
}

// Every control saves as soon as it changes, like Windows Settings; the text
// service picks the change up the next time it is used.
void SettingsWindow::Bind() {
    const std::pair<ToggleSwitch, bool nova::Options::*> toggles[] = {
        {ShiftToggles(), &nova::Options::shiftToggles},
        {StartFullShape(), &nova::Options::startFullShape},
        {ChinesePunctuation(), &nova::Options::chinesePunctuation},
        {OutputSimplified(), &nova::Options::outputSimplified},
        {SentenceMode(), &nova::Options::sentenceMode},
        {CodeCandidates(), &nova::Options::codeCandidates},
        {Prediction(), &nova::Options::prediction},
        {ShowCodeHints(), &nova::Options::showCodeHints},
        {Association(), &nova::Options::association},
        {AutoCommitUnique(), &nova::Options::autoCommitUnique},
        {AutoCommitOnOverflow(), &nova::Options::autoCommitOnOverflow},
        {PrefixHints(), &nova::Options::prefixHints},
        {ShowLetters(), &nova::Options::showLetters},
        {Learning(), &nova::Options::learning},
        {WordPairs(), &nova::Options::wordPairs},
        {NeuralRerank(), &nova::Options::neuralRerank},
    };
    for (const auto& [toggle, field] : toggles) {
        toggle.IsOn(options_.*field);
        toggle.Toggled([this, toggle, field](auto&&, auto&&) {
            options_.*field = toggle.IsOn();
            UpdateDependents();
            Save();
        });
    }
    auto onCombo = [this](ComboBox const& box, auto apply) {
        box.SelectionChanged([this, box, apply](auto&&, auto&&) {
            int i = box.SelectedIndex();
            if (i < 0) return;
            apply(i);
            Save();
        });
    };
    onCombo(StartMode(), [this](int i) { options_.startInChinese = i == 0; });
    onCombo(CharsetBox(), [this](int i) { options_.charset = static_cast<nova::Charset>(i); });
    onCombo(VersionBox(), [this](int i) { options_.version = static_cast<nova::CangjieVersion>(i); });
    onCombo(LayoutBox(), [this](int i) { options_.layout = static_cast<nova::CandidateLayout>(i); });
    onCombo(PageSizeBox(), [this](int i) { options_.pageSize = kPageSizes[i]; });
    onCombo(FontSizeBox(), [this](int i) { options_.fontSize = kFontSizes[i]; });
    onCombo(OrderBox(), [this](int i) {
        options_.order = i == 1 ? nova::CandidateOrder::Traditional : nova::CandidateOrder::Frequency;
    });
}

void SettingsWindow::Save() {
    if (!nova::SaveOptions(options_)) {
        ShowStatus(L"無法儲存設定。", InfoBarSeverity::Error);
        return;
    }
    if (!granted_) {
        nova::GrantAppContainerRead();
        granted_ = true;
    }
}

// Options of an option are only available while it is on.
void SettingsWindow::UpdateDependents() {
    CodeCandidates().IsEnabled(options_.sentenceMode);
    ShowCodeHints().IsEnabled(options_.prediction);
    OrderBox().IsEnabled(options_.learning);
    NeuralRerank().IsEnabled(options_.sentenceMode);
    WordPairs().IsEnabled(options_.sentenceMode);
}

void SettingsWindow::ShowStatus(hstring const& message, InfoBarSeverity severity) {
    Status().Message(message);
    Status().Severity(severity);
    Status().IsOpen(true);
}

void SettingsWindow::Nav_SelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args) {
    auto item = args.SelectedItem().try_as<NavigationViewItem>();
    if (!item) return;
    const hstring tag = unbox_value<hstring>(item.Tag());
    const std::pair<const wchar_t*, StackPanel> pages[] = {
        {L"General", GeneralPage()}, {L"Input", InputPage()}, {L"Candidates", CandidatesPage()},
        {L"Dictionary", DictionaryPage()}, {L"About", AboutPage()},
    };
    for (const auto& [name, page] : pages) page.Visibility(tag == name ? Visibility::Visible : Visibility::Collapsed);
    PageTitle().Text(unbox_value<hstring>(item.Content()));
    Status().IsOpen(false);
    Scroller().ChangeView(nullptr, 0.0, nullptr, true);
}

fire_and_forget SettingsWindow::ClearHistory_Click(IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    ContentDialog dialog;
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(L"清除學習記錄"));
    dialog.Content(box_value(L"確定要清除所有學習記錄嗎？候選字將回復預設次序。"));
    dialog.PrimaryButtonText(L"清除");
    dialog.CloseButtonText(L"取消");
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
    std::wstring dir = nova::UserDataDirectory(false);
    if (!dir.empty()) DeleteFileW((dir + L"history.txt").c_str());
    nova::BumpHistoryEpoch();
    ShowStatus(L"學習記錄已清除。", InfoBarSeverity::Success);
}

void SettingsWindow::OpenUserWords_Click(IInspectable const&, RoutedEventArgs const&) { App::ShowUserWords(); }

void SettingsWindow::OpenDoc_Click(IInspectable const& sender, RoutedEventArgs const&) {
    std::wstring name(unbox_value<hstring>(sender.as<Button>().Tag()));
    std::wstring path = NovaUi::InstallDir() + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && name.size() > 4 &&
        name.compare(name.size() - 4, 4, L".txt") == 0)
        path.resize(path.size() - 4);  // "LICENSE" in a build folder.
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShowStatus(hstring(L"找不到 ") + hstring(name) + L"。", InfoBarSeverity::Warning);
        return;
    }
    NovaUi::OpenWithShell(path);
}

}  // namespace winrt::NovaSettings::implementation
