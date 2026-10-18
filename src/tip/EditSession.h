// NovaChangJie text service - an ITfEditSession that runs a callback.
#pragma once

#include "Globals.h"

#include <functional>
#include <new>
#include <utility>

namespace nova {

class EditSession final : public ITfEditSession {
public:
    using Callback = std::function<HRESULT(TfEditCookie)>;

    // Runs `fn` inside an edit session on `context`. TF_ES_SYNC is tried first;
    // when the application refuses a synchronous session the work is queued.
    static HRESULT Request(ITfContext* context, TfClientId clientId, DWORD flags, Callback fn) {
        if (!context) return E_INVALIDARG;
        ComPtr<EditSession> session;
        session.Attach(new (std::nothrow) EditSession(std::move(fn)));
        if (!session) return E_OUTOFMEMORY;
        HRESULT hrSession = E_FAIL;
        HRESULT hr = context->RequestEditSession(clientId, session.Get(), flags | TF_ES_SYNC, &hrSession);
        if (hr == TF_E_SYNCHRONOUS || hrSession == TF_E_SYNCHRONOUS)
            hr = context->RequestEditSession(clientId, session.Get(), flags | TF_ES_ASYNCDONTCARE, &hrSession);
        return FAILED(hr) ? hr : hrSession;
    }

    // Runs `fn` synchronously or not at all (for reads whose result is needed now).
    static HRESULT RequestSync(ITfContext* context, TfClientId clientId, DWORD flags, Callback fn) {
        if (!context) return E_INVALIDARG;
        ComPtr<EditSession> session;
        session.Attach(new (std::nothrow) EditSession(std::move(fn)));
        if (!session) return E_OUTOFMEMORY;
        HRESULT hrSession = E_FAIL;
        HRESULT hr = context->RequestEditSession(clientId, session.Get(), flags | TF_ES_SYNC, &hrSession);
        return FAILED(hr) ? hr : hrSession;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // ITfEditSession
    STDMETHODIMP DoEditSession(TfEditCookie ec) override { return fn_ ? fn_(ec) : S_OK; }

private:
    explicit EditSession(Callback fn) : fn_(std::move(fn)) { DllAddRef(); }
    ~EditSession() { DllRelease(); }

    std::atomic<ULONG> refs_{1};
    Callback fn_;
};

}  // namespace nova
