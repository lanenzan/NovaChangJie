#include "pch.h"

#include "App.xaml.h"
#include "SettingsWindow.xaml.h"
#include "UserWordsWindow.xaml.h"
#include "WindowHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::NovaSettings::implementation {

namespace {

Window g_userWords{nullptr};

}  // namespace

App::App() {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        if (IsDebuggerPresent()) {
            auto message = e.Message();
            __debugbreak();
        }
    });
#endif
}

// NovaChangJieSettingsUI.exe              the settings window
// NovaChangJieSettingsUI.exe /userwords   the 使用者造詞工具
void App::OnLaunched(LaunchActivatedEventArgs const&) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool userWords = argv && argc >= 2 && _wcsicmp(argv[1], L"/userwords") == 0;
    if (argv) LocalFree(argv);

    // One window of each kind: bring an open one to the front instead.
    if (NovaUi::ActivateExisting(userWords ? NovaUi::kUserWordsTitle : NovaUi::kSettingsTitle)) {
        Exit();
        return;
    }
    if (userWords) {
        ShowUserWords();
    } else {
        window_ = make<SettingsWindow>();
        window_.Activate();
    }
}

void App::ShowUserWords() {
    if (!g_userWords) {
        g_userWords = make<UserWordsWindow>();
        g_userWords.Closed([](auto&&, auto&&) { g_userWords = nullptr; });
    }
    g_userWords.Activate();
}

}  // namespace winrt::NovaSettings::implementation
