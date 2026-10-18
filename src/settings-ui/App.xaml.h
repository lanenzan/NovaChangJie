#pragma once

#include "App.xaml.g.h"

namespace winrt::NovaSettings::implementation {

struct App : AppT<App> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    // Opens (or brings to the front) the 使用者造詞工具 window.
    static void ShowUserWords();

private:
    Microsoft::UI::Xaml::Window window_{nullptr};
};

}  // namespace winrt::NovaSettings::implementation
