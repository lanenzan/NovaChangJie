// NovaChangJie text service - process-wide globals.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>

#include <atomic>
#include <string>

#include "NovaIds.h"

using Microsoft::WRL::ComPtr;

namespace nova {

extern HINSTANCE g_hInstance;
extern std::atomic<long> g_dllRefCount;

inline void DllAddRef() { ++g_dllRefCount; }
inline void DllRelease() { --g_dllRefCount; }

// {ECC8B4F9-F45D-46AE-A362-524ED69A98CA}
extern const CLSID CLSID_NovaTextService;
// {E57D6684-5925-4A4F-BE4C-3C365A51D55A}
extern const GUID GUID_NovaProfile;
extern const GUID GUID_NovaQuickProfile;  // 速成
// {098237A7-192C-4DF7-B5E2-7ACD844E20A9}
extern const GUID GUID_NovaDisplayAttributeInput;
// {DEE969E4-5B3E-43AD-A107-CB36FF746DCF}
extern const GUID GUID_NovaPreservedKeyFullShape;
// {D00F01ED-05C4-42E4-843B-54E8E10B1461}
extern const GUID GUID_NovaPreservedKeyPunct;
// {1928D036-B557-46A4-8C80-D9E472901252}
extern const GUID GUID_NovaCandidateUIElement;

constexpr wchar_t kServiceName[] = L"NovaChangJie 星倉頡";
constexpr wchar_t kQuickServiceName[] = L"Nova Quick 星速成";
constexpr wchar_t kProductName[] = L"NovaChangJie";

// Directory containing this DLL, with a trailing backslash.
std::wstring ModuleDirectory();
// Locates a file installed next to the DLL or one directory up (the x86 DLL
// lives in a subdirectory). Returns an empty string when not found.
std::wstring FindInstalledFile(const wchar_t* name);
// Starts the settings application.
// Starts NovaChangJieSettings.exe; args "/userwords" opens the user words tool.
void LaunchSettings(HWND owner, const wchar_t* args = nullptr);

}  // namespace nova
