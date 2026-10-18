// NovaChangJie - Options persistence in HKCU\Software\NovaChangJie.
// Shared by the text service and the settings application.
#pragma once

#include <windows.h>

#include <string>

#include "Options.h"

namespace nova {

constexpr wchar_t kSettingsKey[] = L"Software\\NovaChangJie";

// Missing or unreadable values keep their defaults. Never fails: inside an
// AppContainer the key may be unreadable and defaults are used.
Options LoadOptions();
bool SaveOptions(const Options& options);
// Incremented on every save so running text services can reload cheaply.
DWORD OptionsRevision();

// %APPDATA%\NovaChangJie\ with trailing backslash (created if missing), or
// empty when unavailable (for example inside an AppContainer).
std::wstring UserDataDirectory(bool create);

// Grants ALL APPLICATION PACKAGES read access to the settings key and the
// user data directory so Store / UWP apps can use the same settings.
void GrantAppContainerRead();

// %APPDATA%\NovaChangJie\user_phrases.txt; with `create`, a commented
// template is written when the file does not exist yet.
std::wstring UserPhrasesFile(bool create);

// "Clear learning data" from the settings app bumps this counter; every
// running text service drops its in-memory history when it changes.
DWORD HistoryEpoch();
void BumpHistoryEpoch();

bool ReadFileUtf8(const std::wstring& path, std::string& out);
// Writes through a temporary file and replaces atomically.
bool WriteFileUtf8(const std::wstring& path, const std::string& data);

}  // namespace nova
