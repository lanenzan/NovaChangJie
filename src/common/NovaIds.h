// NovaChangJie - identifiers shared by the text service and the settings app.
// The installer (installer/msi) refers to the same values; keep them in sync.
#pragma once

#include <windows.h>

namespace nova {

// {ECC8B4F9-F45D-46AE-A362-524ED69A98CA}
inline constexpr GUID kTextServiceClsid = {
    0xecc8b4f9, 0xf45d, 0x46ae, {0xa3, 0x62, 0x52, 0x4e, 0xd6, 0x9a, 0x98, 0xca}};
// {E57D6684-5925-4A4F-BE4C-3C365A51D55A}
inline constexpr GUID kProfileGuid = {
    0xe57d6684, 0x5925, 0x4a4f, {0xbe, 0x4c, 0x3c, 0x36, 0x5a, 0x51, 0xd5, 0x5a}};
// Nova Quick 星速成 profile of the same text service.
// {D4395F42-D822-4DF8-984D-4DF0C2F144E9}
inline constexpr GUID kQuickProfileGuid = {
    0xd4395f42, 0xd822, 0x4df8, {0x98, 0x4d, 0x4d, 0xf0, 0xc2, 0xf1, 0x44, 0xe9}};

// Named pipe of the NovaChangJieLM helper, followed by the user's SID string.
inline constexpr wchar_t kLmPipePrefix[] = L"\\\\.\\pipe\\NovaChangJieLM-";

inline constexpr LANGID kLangIds[] = {
    MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL),  // zh-TW (0x0404)
    MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_HONGKONG),     // zh-HK (0x0C04)
    MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_MACAU),        // zh-MO (0x1404)
};

}  // namespace nova
