#include "Register.h"

#include "resource.h"

namespace nova {

namespace {

const GUID* const kCategories[] = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,       // Candidate list as a UI element.
    &GUID_TFCAT_TIPCAP_SECUREMODE,             // Usable on the logon screen.
    &GUID_TFCAT_TIPCAP_COMLESS,                // Loadable without COM activation.
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,   // Mode via conversion compartment.
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,       // Store / UWP / WinUI apps.
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,         // Taskbar input indicator.
};

std::wstring ClsidKeyPath() {
    wchar_t clsid[64];
    StringFromGUID2(CLSID_NovaTextService, clsid, ARRAYSIZE(clsid));
    return std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
}

std::wstring ModulePath() {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(g_hInstance, path, ARRAYSIZE(path));
    return (n == 0 || n >= ARRAYSIZE(path)) ? std::wstring() : std::wstring(path, n);
}

bool SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

}  // namespace

HRESULT RegisterServer() {
    std::wstring module = ModulePath();
    if (module.empty()) return E_FAIL;
    std::wstring path = ClsidKeyPath();
    HKEY clsidKey = nullptr, inproc = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &clsidKey, nullptr) !=
        ERROR_SUCCESS)
        return E_ACCESSDENIED;
    bool ok = SetString(clsidKey, nullptr, kServiceName) &&
              RegCreateKeyExW(clsidKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr) ==
                  ERROR_SUCCESS &&
              SetString(inproc, nullptr, module) && SetString(inproc, L"ThreadingModel", L"Apartment");
    if (inproc) RegCloseKey(inproc);
    RegCloseKey(clsidKey);
    return ok ? S_OK : E_FAIL;
}

void UnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKeyPath().c_str());
}

HRESULT RegisterProfiles() {
    ComPtr<ITfInputProcessorProfileMgr> mgr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr));
    if (FAILED(hr)) return hr;
    std::wstring icon = ModulePath();
    struct Profile { const GUID* guid; const wchar_t* name; int iconId; };
    const Profile profiles[] = {{&GUID_NovaProfile, kServiceName, IDI_NOVA},
                                {&GUID_NovaQuickProfile, kQuickServiceName, IDI_NOVA_QUICK}};
    for (const Profile& p : profiles) {
        for (LANGID lang : kLangIds) {
            // Not enabled by default: Windows would then add hidden zh-HK and
            // zh-MO keyboards for every user and list each input method once
            // per language. The installer (or Settings > Add a keyboard)
            // enables it for the user instead.
            hr = mgr->RegisterProfile(CLSID_NovaTextService, lang, *p.guid, p.name, static_cast<ULONG>(wcslen(p.name)),
                                      icon.c_str(), static_cast<ULONG>(icon.size()), static_cast<UINT>(-p.iconId),
                                      nullptr, 0, FALSE, 0);
            if (FAILED(hr)) return hr;
        }
    }
    return S_OK;
}

void UnregisterProfiles() {
    ComPtr<ITfInputProcessorProfileMgr> mgr;
    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr))))
        return;
    for (LANGID lang : kLangIds) {
        mgr->UnregisterProfile(CLSID_NovaTextService, lang, GUID_NovaProfile, 0);
        mgr->UnregisterProfile(CLSID_NovaTextService, lang, GUID_NovaQuickProfile, 0);
    }
}

HRESULT RegisterCategories() {
    ComPtr<ITfCategoryMgr> mgr;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr));
    if (FAILED(hr)) return hr;
    for (const GUID* cat : kCategories) {
        hr = mgr->RegisterCategory(CLSID_NovaTextService, *cat, CLSID_NovaTextService);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

void UnregisterCategories() {
    ComPtr<ITfCategoryMgr> mgr;
    if (FAILED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr)))) return;
    for (const GUID* cat : kCategories) mgr->UnregisterCategory(CLSID_NovaTextService, *cat, CLSID_NovaTextService);
}

}  // namespace nova
