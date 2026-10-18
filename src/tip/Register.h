// NovaChangJie text service - COM and TSF registration.
#pragma once

#include "Globals.h"

namespace nova {

HRESULT RegisterServer();      // CLSID / InprocServer32 under HKLM.
void UnregisterServer();
HRESULT RegisterProfiles();    // Input profiles for zh-TW, zh-HK, zh-MO.
void UnregisterProfiles();
HRESULT RegisterCategories();  // Keyboard TIP + Windows 8+/11 capabilities.
void UnregisterCategories();

}  // namespace nova
