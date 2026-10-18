#include "Globals.h"

#include <shellapi.h>

namespace nova {

HINSTANCE g_hInstance = nullptr;
std::atomic<long> g_dllRefCount{0};

const CLSID CLSID_NovaTextService = kTextServiceClsid;
const GUID GUID_NovaProfile = kProfileGuid;
const GUID GUID_NovaQuickProfile = kQuickProfileGuid;
const GUID GUID_NovaDisplayAttributeInput = {
    0x098237a7, 0x192c, 0x4df7, {0xb5, 0xe2, 0x7a, 0xcd, 0x84, 0x4e, 0x20, 0xa9}};
const GUID GUID_NovaPreservedKeyFullShape = {
    0xdee969e4, 0x5b3e, 0x43ad, {0xa1, 0x07, 0xcb, 0x36, 0xff, 0x74, 0x6d, 0xcf}};
const GUID GUID_NovaPreservedKeyPunct = {
    0xd00f01ed, 0x05c4, 0x42e4, {0x84, 0x3b, 0x54, 0xe8, 0xe1, 0x0b, 0x14, 0x61}};
const GUID GUID_NovaCandidateUIElement = {
    0x1928d036, 0xb557, 0x46a4, {0x8c, 0x80, 0xd9, 0xe4, 0x72, 0x90, 0x12, 0x52}};

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(g_hInstance, path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) return {};
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
}

std::wstring FindInstalledFile(const wchar_t* name) {
    std::wstring dir = ModuleDirectory();
    if (dir.empty()) return {};
    std::wstring candidate = dir + name;
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
    // One level up: <install>\x86\NovaChangJie.dll -> <install>\name
    size_t slash = dir.find_last_of(L'\\', dir.size() - 2);
    if (slash != std::wstring::npos) {
        candidate = dir.substr(0, slash + 1) + name;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return {};
}

void LaunchSettings(HWND owner, const wchar_t* args) {
    std::wstring exe = FindInstalledFile(L"NovaChangJieSettings.exe");
    if (exe.empty()) return;
    ShellExecuteW(owner, L"open", exe.c_str(), args, nullptr, SW_SHOWNORMAL);
}

}  // namespace nova
