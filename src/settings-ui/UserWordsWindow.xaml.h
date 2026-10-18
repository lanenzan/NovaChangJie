#pragma once

#include "UserWordsWindow.g.h"
#include "WordItem.g.h"

#include "UserWordsData.h"

namespace winrt::NovaSettings::implementation {

struct WordItem : WordItemT<WordItem> {
    WordItem(hstring number, hstring text, hstring radicals, hstring extra, int32_t index)
        : number_(std::move(number)), text_(std::move(text)), radicals_(std::move(radicals)),
          extra_(std::move(extra)), index_(index) {}
    hstring Number() const { return number_; }
    hstring Text() const { return text_; }
    hstring Radicals() const { return radicals_; }
    hstring Extra() const { return extra_; }
    int32_t Index() const { return index_; }

private:
    hstring number_, text_, radicals_, extra_;
    int32_t index_;
};

struct UserWordsWindow : UserWordsWindowT<UserWordsWindow> {
    UserWordsWindow() = default;
    void InitializeComponent();

    fire_and_forget Add_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    fire_and_forget Edit_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Delete_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Undo_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Move_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Save_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Import_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Export_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Sort_Click(IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Search_TextChanged(Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
                            Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const&);
    void Views_SelectionChanged(Microsoft::UI::Xaml::Controls::SelectorBar const&,
                                Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
    fire_and_forget Words_DoubleTapped(IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
    void Words_SelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    fire_and_forget Words_KeyDown(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void Save_Accelerator(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                          Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
    void Undo_Accelerator(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                          Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
    fire_and_forget Add_Accelerator(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                                    Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
    void Find_Accelerator(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                          Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);

private:
    enum class View { User, Learned };

    std::vector<nova::UserWord>& Current() { return view_ == View::User ? user_ : learned_; }
    std::vector<int> SelectedWords();
    void Refresh(int selectWord = -1);
    void UpdateCommands();
    void SetView(View view);
    bool Save();
    Windows::Foundation::IAsyncAction AddWord();
    Windows::Foundation::IAsyncAction EditSelected();
    void DeleteSelected();
    // The 詞彙編輯 dialog; true if the word was changed. `ignoreIndex` is the
    // word's own position (duplicates are checked against all others).
    Windows::Foundation::IAsyncOperation<bool> EditWordAsync(nova::UserWord* word, int ignoreIndex);
    fire_and_forget ConfirmClose();
    void ShowStatus(hstring const& message, Microsoft::UI::Xaml::Controls::InfoBarSeverity severity);
    bool Cj3() const { return cj3_; }

    nova::CodeTable table_;
    bool cj3_ = false;
    View view_ = View::User;
    std::vector<nova::UserWord> user_, learned_;
    std::vector<std::u16string> removedLearned_;
    std::vector<std::pair<View, std::vector<nova::UserWord>>> undo_;
    bool userDirty_ = false, learnedDirty_ = false;
    bool dialogOpen_ = false;
    bool closing_ = false;
    std::u16string filter_;
    Windows::Foundation::Collections::IObservableVector<IInspectable> items_ =
        single_threaded_observable_vector<IInspectable>();
};

}  // namespace winrt::NovaSettings::implementation

namespace winrt::NovaSettings::factory_implementation {

struct UserWordsWindow : UserWordsWindowT<UserWordsWindow, implementation::UserWordsWindow> {};

}  // namespace winrt::NovaSettings::factory_implementation
