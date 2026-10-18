#include "pch.h"

#include "WindowHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI;
using namespace winrt::Microsoft::UI::Windowing;
using namespace winrt::Microsoft::UI::Xaml;

namespace NovaUi {

std::wstring AppDir() {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring dir(path, n < ARRAYSIZE(path) ? n : 0);
    return dir.substr(0, dir.find_last_of(L'\\') + 1);
}

std::wstring InstallDir() {
    std::wstring dir = AppDir();  // ...\ui\  ->  ...\ .
    if (dir.size() > 1) {
        size_t slash = dir.find_last_of(L'\\', dir.size() - 2);
        if (slash != std::wstring::npos) return dir.substr(0, slash + 1);
    }
    return dir;
}

HWND WindowHandle(Window const& window) {
    HWND hwnd = nullptr;
    window.as<::IWindowNative>()->get_WindowHandle(&hwnd);
    return hwnd;
}

void SetupWindow(Window const& window, UIElement const& titleBar, int width, int height) {
    window.ExtendsContentIntoTitleBar(true);
    window.SetTitleBar(titleBar);
    window.SystemBackdrop(Media::MicaBackdrop());

    AppWindow appWindow = window.AppWindow();
    std::wstring icon = AppDir() + L"Assets\\NovaChangJie.ico";
    if (GetFileAttributesW(icon.c_str()) != INVALID_FILE_ATTRIBUTES) appWindow.SetIcon(icon);

    HWND hwnd = WindowHandle(window);
    const double scale = GetDpiForWindow(hwnd) / 96.0;
    const int w = static_cast<int>(width * scale), h = static_cast<int>(height * scale);
    DisplayArea area = DisplayArea::GetFromWindowId(appWindow.Id(), DisplayAreaFallback::Nearest);
    auto work = area.WorkArea();
    appWindow.MoveAndResize({work.X + std::max(0, (work.Width - w) / 2), work.Y + std::max(0, (work.Height - h) / 2),
                             std::min(w, work.Width), std::min(h, work.Height)});
}

void OpenWithShell(std::wstring const& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool ActivateExisting(wchar_t const* title) {
    HWND existing = FindWindowW(nullptr, title);
    if (!existing) return false;
    if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
    SetForegroundWindow(existing);
    return true;
}

}  // namespace NovaUi
