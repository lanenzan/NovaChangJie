// NovaChangJie text service - DLL entry points and class factory.
#include "Globals.h"

#include "CandidateUI.h"
#include "Register.h"
#include "TextService.h"

namespace nova {
namespace {

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // Static object: lifetime is tied to the DLL lock count instead.
    STDMETHODIMP_(ULONG) AddRef() override {
        DllAddRef();
        return 2;
    }
    STDMETHODIMP_(ULONG) Release() override {
        DllRelease();
        return 1;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        return TextService::CreateInstance(outer, riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock)
            DllAddRef();
        else
            DllRelease();
        return S_OK;
    }
};

ClassFactory g_factory;

}  // namespace
}  // namespace nova

using namespace nova;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_hInstance = instance;
        DisableThreadLibraryCalls(instance);
        break;
    case DLL_PROCESS_DETACH:
        if (reserved == nullptr) {  // FreeLibrary, not process exit.
            UnregisterCandidateWindowClass();
            UnregisterClassW(L"NovaChangJieMenuOwner", instance);
        }
        break;
    default:
        break;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_NovaTextService)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_dllRefCount.load() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    HRESULT hr = RegisterServer();
    if (SUCCEEDED(hr)) hr = RegisterProfiles();
    if (SUCCEEDED(hr)) hr = RegisterCategories();
    if (FAILED(hr)) {
        UnregisterCategories();
        UnregisterProfiles();
        UnregisterServer();
    }
    return hr;
}

STDAPI DllUnregisterServer() {
    UnregisterCategories();
    UnregisterProfiles();
    UnregisterServer();
    return S_OK;
}
