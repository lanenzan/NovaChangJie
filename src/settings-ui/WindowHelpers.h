// NovaChangJie settings (WinUI) - helpers shared by the windows.
#pragma once

#include <string>

namespace NovaUi {

inline constexpr wchar_t kSettingsTitle[] = L"NovaChangJie 星倉頡 設定";
inline constexpr wchar_t kUserWordsTitle[] = L"NovaChangJie 使用者造詞工具";

// This program's folder and <install dir>\ (this program is in <install dir>\ui\).
std::wstring AppDir();
std::wstring InstallDir();
HWND WindowHandle(winrt::Microsoft::UI::Xaml::Window const& window);
// Icon, Mica backdrop, content in the title bar (titleBar is the drag area),
// size in DIPs, centred on the work area.
void SetupWindow(winrt::Microsoft::UI::Xaml::Window const& window,
                 winrt::Microsoft::UI::Xaml::UIElement const& titleBar, int width, int height);
// Opens a file with its default program (e.g. a .md licence file).
void OpenWithShell(std::wstring const& path);
// Brings an already open window of another instance to the front.
bool ActivateExisting(wchar_t const* title);

inline winrt::hstring H(std::u16string const& s) {
    return winrt::hstring(reinterpret_cast<wchar_t const*>(s.data()), static_cast<uint32_t>(s.size()));
}
inline std::u16string U16(winrt::hstring const& s) { return std::u16string(s.begin(), s.end()); }

}  // namespace NovaUi
