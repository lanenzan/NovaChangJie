#pragma once

#include "SettingsWindow.g.h"

#include "Options.h"

namespace winrt::NovaSettings::implementation {

struct SettingsWindow : SettingsWindowT<SettingsWindow> {
    SettingsWindow() = default;
    void InitializeComponent();

    void Nav_SelectionChanged(Microsoft::UI::Xaml::Controls::NavigationView const&,
                              Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
    fire_and_forget ClearHistory_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenUserWords_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenDoc_Click(IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    void Load();
    void Bind();
    void Save();
    void UpdateDependents();
    void ShowStatus(hstring const& message, Microsoft::UI::Xaml::Controls::InfoBarSeverity severity);

    nova::Options options_;
    bool granted_ = false;  // App containers may read the settings.
};

}  // namespace winrt::NovaSettings::implementation

namespace winrt::NovaSettings::factory_implementation {

struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};

}  // namespace winrt::NovaSettings::factory_implementation
