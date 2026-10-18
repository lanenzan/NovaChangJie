#include "TextService.h"

#include <InputScope.h>
#include <oleauto.h>
#include <shellapi.h>

#include <algorithm>
#include <ctime>

#include "CandidateUI.h"
#include "DisplayAttribute.h"
#include "EditSession.h"
#include "LangBarButton.h"
#include "SettingsStore.h"
#include "SharedData.h"

namespace nova {

namespace {

int64_t Now() { return static_cast<int64_t>(_time64(nullptr)); }

const wchar_t* AsWide(const std::u16string& s) { return reinterpret_cast<const wchar_t*>(s.c_str()); }

constexpr TF_PRESERVEDKEY kFullShapeKey = {VK_SPACE, TF_MOD_SHIFT};
constexpr TF_PRESERVEDKEY kPunctKey = {VK_OEM_PERIOD, TF_MOD_CONTROL};
constexpr ULONGLONG kShiftTapMaxMs = 800;

// GUID_PROP_INPUTSCOPE (InputScope.h), defined here so no extra library is needed.
constexpr GUID kPropInputScope = {0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

bool IsDown(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

// <install dir>\lm\NovaChangJieLM.exe; this DLL is in <install dir>\<arch>.
std::wstring HelperPath() {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(g_hInstance, path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) return {};
    std::wstring dir(path, n);
    for (int up = 0; up < 2; ++up) {
        size_t slash = dir.find_last_of(L'\\');
        if (slash == std::wstring::npos) return {};
        dir.resize(slash);
    }
    return dir + L"\\lm\\NovaChangJieLM.exe";
}

// Big5 code -> Unicode with Windows' code page 950 (for `B in the engine).
char32_t DecodeBig5(uint16_t big5) {
    const char bytes[2] = {static_cast<char>(big5 >> 8), static_cast<char>(big5 & 0xFF)};
    if (static_cast<unsigned char>(bytes[0]) < 0x81) return 0;  // Double-byte codes only.
    wchar_t wide[2] = {};
    int n = MultiByteToWideChar(950, MB_ERR_INVALID_CHARS, bytes, 2, wide, 2);
    return n == 1 ? static_cast<char32_t>(wide[0]) : 0;
}

}  // namespace

HRESULT TextService::CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* service = new (std::nothrow) TextService();
    if (!service) return E_OUTOFMEMORY;
    HRESULT hr = service->QueryInterface(riid, ppv);
    service->Release();
    return hr;
}

TextService::TextService() { DllAddRef(); }
TextService::~TextService() { DllRelease(); }

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor || riid == IID_ITfTextInputProcessorEx)
        *ppv = static_cast<ITfTextInputProcessorEx*>(this);
    else if (riid == IID_ITfThreadMgrEventSink)
        *ppv = static_cast<ITfThreadMgrEventSink*>(this);
    else if (riid == IID_ITfThreadFocusSink)
        *ppv = static_cast<ITfThreadFocusSink*>(this);
    else if (riid == IID_ITfKeyEventSink)
        *ppv = static_cast<ITfKeyEventSink*>(this);
    else if (riid == IID_ITfCompositionSink)
        *ppv = static_cast<ITfCompositionSink*>(this);
    else if (riid == IID_ITfDisplayAttributeProvider)
        *ppv = static_cast<ITfDisplayAttributeProvider*>(this);
    else if (riid == IID_ITfCompartmentEventSink)
        *ppv = static_cast<ITfCompartmentEventSink*>(this);
    else if (riid == IID_ITfTextLayoutSink)
        *ppv = static_cast<ITfTextLayoutSink*>(this);
    else if (riid == IID_ITfFunctionProvider)
        *ppv = static_cast<ITfFunctionProvider*>(this);
    else if (riid == IID_ITfFunction || riid == IID_ITfFnConfigure)
        *ppv = static_cast<ITfFnConfigure*>(this);
    else if (riid == IID_ITfInputProcessorProfileActivationSink)
        *ppv = static_cast<ITfInputProcessorProfileActivationSink*>(this);
    if (!*ppv) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) TextService::Release() {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
}

// ---------------------------------------------------------------------------
// Activation

STDMETHODIMP TextService::Activate(ITfThreadMgr* ptim, TfClientId tid) { return ActivateEx(ptim, tid, 0); }

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
    if (!ptim) return E_INVALIDARG;
    threadMgr_ = ptim;
    clientId_ = tid;
    activateFlags_ = dwFlags;

    // Which of our profiles (倉頡 / 速成) is being activated.
    ComPtr<ITfInputProcessorProfileMgr> profiles;
    TF_INPUTPROCESSORPROFILE active = {};
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&profiles))) &&
        SUCCEEDED(profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active)) &&
        IsEqualCLSID(active.clsid, CLSID_NovaTextService))
        quick_ = IsEqualGUID(active.guidProfile, GUID_NovaQuickProfile);

    ReloadOptionsIfChanged(true);
    SharedData& shared = SharedData::Get();
    engine_.SetDictionary(shared.Dict());
    engine_.SetUserModel(&shared.User());
    engine_.SetBig5Decoder(DecodeBig5);
    engine_.SetSentenceScorer(&lm_);
    lm_.SetHelperPath(HelperPath());
    engine_.SetOptions(options_);
    engine_.SetChinese(options_.startInChinese);
    engine_.SetFullShape(options_.startFullShape);
    engine_.SetChinesePunct(options_.chinesePunctuation);

    ComPtr<ITfSource> source;
    if (FAILED(threadMgr_.As(&source)) ||
        FAILED(source->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this),
                                  &threadMgrSinkCookie_)) ||
        FAILED(source->AdviseSink(IID_ITfThreadFocusSink, static_cast<ITfThreadFocusSink*>(this),
                                  &threadFocusSinkCookie_))) {
        Deactivate();
        return E_FAIL;
    }
    source->AdviseSink(IID_ITfInputProcessorProfileActivationSink,
                       static_cast<ITfInputProcessorProfileActivationSink*>(this), &profileSinkCookie_);

    ComPtr<ITfKeystrokeMgr> keystrokes;
    if (FAILED(threadMgr_.As(&keystrokes)) ||
        FAILED(keystrokes->AdviseKeyEventSink(clientId_, static_cast<ITfKeyEventSink*>(this), TRUE))) {
        Deactivate();
        return E_FAIL;
    }
    keySinkAdvised_ = true;
    static const wchar_t kFullDesc[] = L"Full / half shape";
    static const wchar_t kPunctDesc[] = L"Chinese / English punctuation";
    keystrokes->PreserveKey(clientId_, GUID_NovaPreservedKeyFullShape, &kFullShapeKey, kFullDesc,
                            ARRAYSIZE(kFullDesc) - 1);
    keystrokes->PreserveKey(clientId_, GUID_NovaPreservedKeyPunct, &kPunctKey, kPunctDesc, ARRAYSIZE(kPunctDesc) - 1);

    ComPtr<ITfCategoryMgr> categories;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&categories))))
        categories->RegisterGUID(GUID_NovaDisplayAttributeInput, &inputAttributeAtom_);

    ComPtr<ITfSourceSingle> single;
    if (SUCCEEDED(threadMgr_.As(&single)) &&
        SUCCEEDED(single->AdviseSingleSink(clientId_, IID_ITfFunctionProvider,
                                           static_cast<ITfFunctionProvider*>(this))))
        functionProviderAdvised_ = true;

    candidateUI_.Attach(new (std::nothrow) CandidateUI(this));
    if (candidateUI_) candidateUI_->SetOptions(options_);

    langBarButton_.Attach(new (std::nothrow) LangBarButton(this));
    ComPtr<ITfLangBarItemMgr> langBar;
    if (langBarButton_ && SUCCEEDED(threadMgr_.As(&langBar))) langBar->AddItem(langBarButton_.Get());

    InitCompartments();
    return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
    AbandonComposition();
    engine_.Reset();
    UnadviseLayoutSink();

    if (candidateUI_) {
        candidateUI_->Destroy();
        candidateUI_.Reset();
    }
    if (langBarButton_) {
        ComPtr<ITfLangBarItemMgr> langBar;
        if (threadMgr_ && SUCCEEDED(threadMgr_.As(&langBar))) langBar->RemoveItem(langBarButton_.Get());
        langBarButton_->Detach();
        langBarButton_.Reset();
    }
    UninitCompartments();

    if (threadMgr_) {
        ComPtr<ITfSourceSingle> single;
        if (functionProviderAdvised_ && SUCCEEDED(threadMgr_.As(&single)))
            single->UnadviseSingleSink(clientId_, IID_ITfFunctionProvider);
        functionProviderAdvised_ = false;

        ComPtr<ITfKeystrokeMgr> keystrokes;
        if (SUCCEEDED(threadMgr_.As(&keystrokes))) {
            keystrokes->UnpreserveKey(GUID_NovaPreservedKeyFullShape, &kFullShapeKey);
            keystrokes->UnpreserveKey(GUID_NovaPreservedKeyPunct, &kPunctKey);
            if (keySinkAdvised_) keystrokes->UnadviseKeyEventSink(clientId_);
        }
        keySinkAdvised_ = false;

        ComPtr<ITfSource> source;
        if (SUCCEEDED(threadMgr_.As(&source))) {
            if (threadMgrSinkCookie_ != TF_INVALID_COOKIE) source->UnadviseSink(threadMgrSinkCookie_);
            if (threadFocusSinkCookie_ != TF_INVALID_COOKIE) source->UnadviseSink(threadFocusSinkCookie_);
            if (profileSinkCookie_ != TF_INVALID_COOKIE) source->UnadviseSink(profileSinkCookie_);
        }
    }
    threadMgrSinkCookie_ = threadFocusSinkCookie_ = profileSinkCookie_ = TF_INVALID_COOKIE;

    SaveUserData(true);
    threadMgr_.Reset();
    clientId_ = TF_CLIENTID_NULL;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Focus

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr*) {
    ReloadOptionsIfChanged(false);
    SharedData::Get().RefreshUserPhrases();
    AbandonComposition();
    engine_.Reset();
    UnadviseLayoutSink();
    UpdateCandidateWindow(nullptr);
    if (!focus) SaveUserData(false);
    return S_OK;
}

STDMETHODIMP TextService::OnSetThreadFocus() {
    if (candidateUI_) candidateUI_->SetThreadFocus(true);
    return S_OK;
}

STDMETHODIMP TextService::OnKillThreadFocus() {
    if (candidateUI_) candidateUI_->SetThreadFocus(false);
    return S_OK;
}

STDMETHODIMP TextService::OnSetFocus(BOOL fForeground) {
    if (fForeground) ReloadOptionsIfChanged(false);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Keys

bool TextService::ShouldHandle(WPARAM vk) const {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
    case VK_PAUSE: case VK_SNAPSHOT: case VK_INSERT:
    case VK_PROCESSKEY: case VK_PACKET:
        return false;
    default:
        break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) return false;
    if (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2) return false;  // Media keys.
    return true;
}

KeyInput TextService::TranslateKey(WPARAM vk, LPARAM lParam) const {
    KeyInput k;
    k.shift = IsDown(VK_SHIFT);
    k.ctrl = IsDown(VK_CONTROL);
    k.alt = IsDown(VK_MENU);
    k.capsLock = (GetKeyState(VK_CAPITAL) & 1) != 0;

    switch (vk) {
    case VK_SPACE: k.key = Key::Space; return k;
    case VK_RETURN: k.key = Key::Enter; return k;
    case VK_ESCAPE: k.key = Key::Escape; return k;
    case VK_BACK: k.key = Key::Backspace; return k;
    case VK_TAB: k.key = Key::Tab; return k;
    case VK_UP: k.key = Key::Up; return k;
    case VK_DOWN: k.key = Key::Down; return k;
    case VK_LEFT: k.key = Key::Left; return k;
    case VK_RIGHT: k.key = Key::Right; return k;
    case VK_PRIOR: k.key = Key::PageUp; return k;
    case VK_NEXT: k.key = Key::PageDown; return k;
    case VK_HOME: k.key = Key::Home; return k;
    case VK_END: k.key = Key::End; return k;
    case VK_DELETE: k.key = Key::Delete; return k;
    default: break;
    }

    if (vk >= 'A' && vk <= 'Z') {
        k.key = Key::Char;
        k.letter = static_cast<char>('a' + (vk - 'A'));
        bool upper = k.shift != k.capsLock;
        k.ch = static_cast<char32_t>(upper ? vk : 'a' + (vk - 'A'));
        return k;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        k.key = Key::Char;
        k.digit = static_cast<int>(vk - VK_NUMPAD0);
        k.ch = static_cast<char32_t>('0' + k.digit);
        k.shift = false;  // Shift+numpad digit is a navigation key, not a symbol.
        return k;
    }
    if (vk >= '0' && vk <= '9') k.digit = static_cast<int>(vk - '0');

    BYTE state[256];
    if (GetKeyboardState(state)) {
        state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
        wchar_t buf[4] = {};
        UINT scan = (lParam >> 16) & 0xFF;
        // Flag 0x4: do not change the keyboard (dead key) state.
        int n = ToUnicodeEx(static_cast<UINT>(vk), scan, state, buf, ARRAYSIZE(buf), 0x4, GetKeyboardLayout(0));
        if (n == 1 && buf[0] >= 0x20) {
            k.key = Key::Char;
            k.ch = buf[0];
            return k;
        }
    }
    if (k.digit >= 0) {
        k.key = Key::Char;
        k.ch = static_cast<char32_t>('0' + k.digit);
    }
    return k;
}

HRESULT TextService::HandleKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, bool test, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    if (wParam == VK_SHIFT) {
        bool alone = !IsDown(VK_CONTROL) && !IsDown(VK_MENU) && !IsDown(VK_LWIN) && !IsDown(VK_RWIN);
        bool repeat = (lParam & 0x40000000) != 0;
        if (!repeat) {
            shiftPending_ = alone;
            shiftDownTick_ = GetTickCount64();
        }
        return S_OK;
    }
    shiftPending_ = false;
    if (!pic || !ShouldHandle(wParam) || !SharedData::Get().Dict()) return S_OK;

    // Respect read-only / disabled contexts (e.g. password boxes ask for this).
    ComPtr<ITfCompartmentMgr> ctxCompartments;
    if (SUCCEEDED(pic->QueryInterface(IID_PPV_ARGS(&ctxCompartments)))) {
        for (const GUID* g : {&GUID_COMPARTMENT_KEYBOARD_DISABLED, &GUID_COMPARTMENT_EMPTYCONTEXT}) {
            ComPtr<ITfCompartment> c;
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(ctxCompartments->GetCompartment(*g, &c)) && SUCCEEDED(c->GetValue(&v)) &&
                v.vt == VT_I4 && v.lVal != 0)
                return S_OK;
        }
    }

    // Password and PIN fields get no Chinese input: keys go straight to the
    // application, as in English mode. Private fields (InPrivate / incognito
    // windows) are typed in as usual but nothing is learned from them.
    if (!engine_.IsComposing()) {
        FieldKind field = ReadFieldKind(pic);
        engine_.SetPrivate(field != FieldKind::Normal);
        if (field == FieldKind::Password) return S_OK;
    }

    KeyInput key = TranslateKey(wParam, lParam);
    if (test) {
        *pfEaten = engine_.WouldConsume(key) ? TRUE : FALSE;
        // A key the IME passes through (e.g. Enter for a new line) never
        // reaches OnKeyDown, so dismiss the association list here.
        if (!*pfEaten && engine_.IsAssociation()) {
            engine_.Reset();
            UpdateCandidateWindow(nullptr);
        }
        return S_OK;
    }
    const bool hadCandidates = engine_.HasCandidates();
    EngineOutput out = engine_.Process(key, Now());
    *pfEaten = out.consumed ? TRUE : FALSE;
    if (out.beep) MessageBeep(MB_OK);
    if (out.consumed) {
        ApplyOutput(pic, out);
        if (!out.commit.empty()) SaveUserData(false);
    } else if (hadCandidates && !engine_.HasCandidates()) {
        UpdateCandidateWindow(nullptr);  // Association list was dismissed.
    }
    if (out.modeChanged) ModeChanged();
    return S_OK;
}

TextService::FieldKind TextService::ReadFieldKind(ITfContext* pic) {
    FieldKind kind = FieldKind::Normal;
    EditSession::RequestSync(pic, clientId_, TF_ES_READ, [&](TfEditCookie ec) {
        TF_SELECTION sel = {};
        ULONG fetched = 0;
        if (FAILED(pic->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) || fetched != 1) return S_OK;
        ComPtr<ITfRange> range;
        range.Attach(sel.range);
        ComPtr<ITfReadOnlyProperty> prop;
        if (FAILED(pic->GetAppProperty(kPropInputScope, &prop)) || !prop) return S_OK;
        VARIANT v;
        VariantInit(&v);
        ComPtr<ITfInputScope> scope;
        InputScope* scopes = nullptr;
        UINT count = 0;
        if (SUCCEEDED(prop->GetValue(ec, range.Get(), &v)) && v.vt == VT_UNKNOWN && v.punkVal &&
            SUCCEEDED(v.punkVal->QueryInterface(IID_PPV_ARGS(&scope))) &&
            SUCCEEDED(scope->GetInputScopes(&scopes, &count)) && scopes) {
            for (UINT i = 0; i < count; ++i) {
                switch (scopes[i]) {
                case IS_PASSWORD:
                case IS_NUMERIC_PASSWORD:
                case IS_NUMERIC_PIN:
                case IS_ALPHANUMERIC_PIN:
                case IS_ALPHANUMERIC_PIN_SET:
                    kind = FieldKind::Password;
                    break;
                case IS_PRIVATE:
                    if (kind == FieldKind::Normal) kind = FieldKind::Private;
                    break;
                default:
                    break;
                }
            }
            CoTaskMemFree(scopes);
        }
        VariantClear(&v);
        return S_OK;
    });
    return kind;
}

HRESULT TextService::HandleShiftUp(ITfContext*, WPARAM wParam, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    if (wParam != VK_SHIFT) return S_OK;
    bool toggle = shiftPending_ && options_.shiftToggles && GetTickCount64() - shiftDownTick_ < kShiftTapMaxMs;
    shiftPending_ = false;
    if (toggle) ToggleChinese();
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    return HandleKeyDown(pic, wParam, lParam, true, pfEaten);
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    return HandleKeyDown(pic, wParam, lParam, false, pfEaten);
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM, BOOL* pfEaten) {
    return HandleShiftUp(pic, wParam, pfEaten);
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM, BOOL* pfEaten) {
    return HandleShiftUp(pic, wParam, pfEaten);
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID rguid, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    // Shift+Space never reaches OnKeyDown, so releasing Shift afterwards must
    // not count as a lone Shift tap (which would also switch 中/英).
    shiftPending_ = false;
    if (IsEqualGUID(rguid, GUID_NovaPreservedKeyFullShape)) {
        ToggleFullShape();
        *pfEaten = TRUE;
    } else if (IsEqualGUID(rguid, GUID_NovaPreservedKeyPunct)) {
        ToggleChinesePunct();
        *pfEaten = TRUE;
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Document updates

ComPtr<ITfContext> TextService::FocusContext() const {
    ComPtr<ITfContext> ctx;
    if (!threadMgr_) return ctx;
    ComPtr<ITfDocumentMgr> doc;
    if (SUCCEEDED(threadMgr_->GetFocus(&doc)) && doc) doc->GetTop(&ctx);
    return ctx;
}

void TextService::ApplyOutput(ITfContext* pic, const EngineOutput& out) {
    if (!pic) return;
    ComPtr<TextService> self(this);
    ComPtr<ITfContext> ctx(pic);
    std::u16string commit = out.commit;
    EditSession::Request(pic, clientId_, TF_ES_READWRITE, [self, ctx, commit](TfEditCookie ec) {
        if (!commit.empty()) self->CommitText(ec, ctx.Get(), commit);
        if (self->engine_.IsComposing())
            self->SetCompositionText(ec, ctx.Get(), self->engine_.CompositionText(), self->engine_.CaretOffset());
        else if (self->composition_)
            self->EndComposition(ec, true);
        RECT rc;
        bool haveAnchor = self->GetAnchorRect(ec, ctx.Get(), &rc);
        self->UpdateCandidateWindow(haveAnchor ? &rc : nullptr);
        return S_OK;
    });
}

bool TextService::StartComposition(TfEditCookie ec, ITfContext* pic) {
    ComPtr<ITfInsertAtSelection> insert;
    ComPtr<ITfContextComposition> contextComposition;
    if (FAILED(pic->QueryInterface(IID_PPV_ARGS(&insert))) ||
        FAILED(pic->QueryInterface(IID_PPV_ARGS(&contextComposition))))
        return false;
    ComPtr<ITfRange> range;
    if (FAILED(insert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range)) || !range) return false;
    ComPtr<ITfComposition> composition;
    if (FAILED(contextComposition->StartComposition(ec, range.Get(), static_cast<ITfCompositionSink*>(this),
                                                    &composition)) ||
        !composition)
        return false;
    composition_ = composition;
    compositionContext_ = pic;

    TF_SELECTION sel = {};
    sel.range = range.Get();
    sel.style.ase = TF_AE_NONE;
    sel.style.fInterimChar = FALSE;
    pic->SetSelection(ec, 1, &sel);
    AdviseLayoutSink(pic);
    return true;
}

void TextService::SetCompositionText(TfEditCookie ec, ITfContext* pic, const std::u16string& text, size_t caretOffset) {
    if (!composition_ && !StartComposition(ec, pic)) return;
    ComPtr<ITfRange> range;
    if (FAILED(composition_->GetRange(&range))) return;
    range->SetText(ec, 0, AsWide(text), static_cast<LONG>(text.size()));

    ComPtr<ITfProperty> attr;
    if (inputAttributeAtom_ != TF_INVALID_GUIDATOM && SUCCEEDED(pic->GetProperty(GUID_PROP_ATTRIBUTE, &attr))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = static_cast<LONG>(inputAttributeAtom_);
        attr->SetValue(ec, range.Get(), &v);
    }

    // Caret inside the composition (sentence mode lets the user move it).
    ComPtr<ITfRange> caret;
    if (SUCCEEDED(range->Clone(&caret))) {
        caret->Collapse(ec, TF_ANCHOR_START);
        LONG moved = 0;
        LONG shift = static_cast<LONG>(std::min(caretOffset, text.size()));
        caret->ShiftEnd(ec, shift, &moved, nullptr);
        caret->ShiftStart(ec, shift, &moved, nullptr);
        TF_SELECTION sel = {};
        sel.range = caret.Get();
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        pic->SetSelection(ec, 1, &sel);
    }
}

void TextService::CommitText(TfEditCookie ec, ITfContext* pic, const std::u16string& text) {
    if (!composition_ && !StartComposition(ec, pic)) {
        // The document refused a composition; insert directly instead.
        ComPtr<ITfInsertAtSelection> insert;
        ComPtr<ITfRange> range;
        if (SUCCEEDED(pic->QueryInterface(IID_PPV_ARGS(&insert))) &&
            SUCCEEDED(insert->InsertTextAtSelection(ec, 0, AsWide(text), static_cast<LONG>(text.size()), &range)) &&
            range) {
            range->Collapse(ec, TF_ANCHOR_END);
            TF_SELECTION sel = {};
            sel.range = range.Get();
            sel.style.ase = TF_AE_NONE;
            pic->SetSelection(ec, 1, &sel);
        }
        return;
    }
    ComPtr<ITfRange> range;
    if (SUCCEEDED(composition_->GetRange(&range))) {
        range->SetText(ec, 0, AsWide(text), static_cast<LONG>(text.size()));
        ComPtr<ITfProperty> attr;
        if (SUCCEEDED(pic->GetProperty(GUID_PROP_ATTRIBUTE, &attr))) attr->Clear(ec, range.Get());
        ComPtr<ITfRange> caret;
        if (SUCCEEDED(range->Clone(&caret))) {
            caret->Collapse(ec, TF_ANCHOR_END);
            TF_SELECTION sel = {};
            sel.range = caret.Get();
            sel.style.ase = TF_AE_NONE;
            pic->SetSelection(ec, 1, &sel);
        }
    }
    EndComposition(ec, false);
}

void TextService::AbandonComposition() {
    if (!composition_ || !compositionContext_) {
        composition_.Reset();
        compositionContext_.Reset();
        return;
    }
    // Capture the composition itself: the session may run asynchronously,
    // after our own state has moved on.
    ComPtr<ITfComposition> composition = composition_;
    ComPtr<ITfContext> ctx = compositionContext_;
    composition_.Reset();
    compositionContext_.Reset();
    EditSession::Request(ctx.Get(), clientId_, TF_ES_READWRITE, [composition](TfEditCookie ec) {
        ComPtr<ITfRange> range;
        if (SUCCEEDED(composition->GetRange(&range))) range->SetText(ec, 0, L"", 0);
        composition->EndComposition(ec);
        return S_OK;
    });
}

void TextService::EndComposition(TfEditCookie ec, bool clearText) {
    if (!composition_) return;
    ComPtr<ITfComposition> composition = composition_;
    composition_.Reset();
    if (clearText) {
        ComPtr<ITfRange> range;
        if (SUCCEEDED(composition->GetRange(&range))) range->SetText(ec, 0, L"", 0);
    }
    composition->EndComposition(ec);
    compositionContext_.Reset();
}

bool TextService::GetAnchorRect(TfEditCookie ec, ITfContext* pic, RECT* rc) {
    ComPtr<ITfContextView> view;
    if (FAILED(pic->GetActiveView(&view)) || !view) return false;
    ComPtr<ITfRange> range;
    if (composition_) {
        composition_->GetRange(&range);
    } else {
        TF_SELECTION sel = {};
        ULONG fetched = 0;
        if (SUCCEEDED(pic->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) && fetched == 1)
            range.Attach(sel.range);
    }
    if (!range) return false;
    BOOL clipped = FALSE;
    RECT r = {};
    HRESULT hr = view->GetTextExt(ec, range.Get(), &r, &clipped);
    if (FAILED(hr) || hr == TF_E_NOLAYOUT) return false;
    if (r.left == 0 && r.top == 0 && r.right == 0 && r.bottom == 0) return false;
    *rc = r;
    return true;
}

void TextService::UpdateCandidateWindow(const RECT* anchor) {
    if (!candidateUI_) return;
    ComPtr<ITfContext> ctx = compositionContext_ ? compositionContext_ : FocusContext();
    candidateUI_->Update(engine_, anchor, ctx.Get());
}

void TextService::RepositionCandidateWindow(ITfContext* pic) {
    if (!pic || !candidateUI_ || !candidateUI_->IsVisible()) return;
    ComPtr<TextService> self(this);
    ComPtr<ITfContext> ctx(pic);
    EditSession::Request(pic, clientId_, TF_ES_READ, [self, ctx](TfEditCookie ec) {
        RECT rc;
        if (self->candidateUI_ && self->GetAnchorRect(ec, ctx.Get(), &rc)) self->candidateUI_->Move(rc);
        return S_OK;
    });
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) {
    // The application ended our composition (e.g. the user clicked away).
    // Drop the half-typed radicals rather than leaving them in the document.
    if (composition_ && pComposition) {
        ComPtr<ITfRange> range;
        if (SUCCEEDED(pComposition->GetRange(&range))) range->SetText(ecWrite, 0, L"", 0);
    }
    composition_.Reset();
    compositionContext_.Reset();
    engine_.Reset();
    UnadviseLayoutSink();
    UpdateCandidateWindow(nullptr);
    return S_OK;
}

void TextService::AdviseLayoutSink(ITfContext* pic) {
    if (layoutContext_.Get() == pic) return;
    UnadviseLayoutSink();
    ComPtr<ITfSource> source;
    if (SUCCEEDED(pic->QueryInterface(IID_PPV_ARGS(&source))) &&
        SUCCEEDED(source->AdviseSink(IID_ITfTextLayoutSink, static_cast<ITfTextLayoutSink*>(this), &layoutCookie_)))
        layoutContext_ = pic;
    else
        layoutCookie_ = TF_INVALID_COOKIE;
}

void TextService::UnadviseLayoutSink() {
    if (layoutContext_ && layoutCookie_ != TF_INVALID_COOKIE) {
        ComPtr<ITfSource> source;
        if (SUCCEEDED(layoutContext_.As(&source))) source->UnadviseSink(layoutCookie_);
    }
    layoutContext_.Reset();
    layoutCookie_ = TF_INVALID_COOKIE;
}

STDMETHODIMP TextService::OnLayoutChange(ITfContext* pic, TfLayoutCode lcode, ITfContextView*) {
    if (lcode == TF_LC_CHANGE) RepositionCandidateWindow(pic);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Commands from the candidate window and language bar

void TextService::SelectCandidate(size_t index) {
    EngineOutput out = engine_.Select(index, Now());
    ComPtr<ITfContext> ctx = compositionContext_ ? compositionContext_ : FocusContext();
    if (out.consumed && ctx) {
        ApplyOutput(ctx.Get(), out);
        SaveUserData(false);
    }
}

void TextService::SetCandidateSelection(size_t index) {
    engine_.SetSelection(index);
    UpdateCandidateWindow(nullptr);
}

void TextService::ChangePage(long delta) {
    long page = static_cast<long>(engine_.CurrentPage()) + delta;
    if (page < 0) return;
    engine_.SetPage(static_cast<size_t>(page));
    UpdateCandidateWindow(nullptr);
}

void TextService::CancelComposition() {
    engine_.Reset();
    ComPtr<ITfContext> ctx = compositionContext_ ? compositionContext_ : FocusContext();
    if (ctx)
        ApplyOutput(ctx.Get(), EngineOutput{});
    else
        UpdateCandidateWindow(nullptr);
}

void TextService::ToggleChinese() {
    EngineOutput out = engine_.SetChinese(!engine_.Chinese());
    ComPtr<ITfContext> ctx = compositionContext_ ? compositionContext_ : FocusContext();
    if (ctx)
        ApplyOutput(ctx.Get(), out);
    else
        UpdateCandidateWindow(nullptr);
    ModeChanged();
}

void TextService::ToggleFullShape() {
    engine_.SetFullShape(!engine_.FullShape());
    ModeChanged();
}

void TextService::ToggleChinesePunct() {
    engine_.SetChinesePunct(!engine_.ChinesePunct());
    ModeChanged();
}

void TextService::ToggleSimplified() {
    options_.outputSimplified = !options_.outputSimplified;
    engine_.SetOptions(options_);
    if (!IsSecureMode() && SaveOptions(options_)) optionsRevision_ = OptionsRevision();
    UpdateCandidateWindow(nullptr);
    if (langBarButton_) langBarButton_->Update();
}

void TextService::OpenSettings() {
    if (!IsSecureMode()) LaunchSettings(nullptr);
}

void TextService::OpenUserPhrases() {
    if (!IsSecureMode()) LaunchSettings(nullptr, L"/userwords");
}

void TextService::ModeChanged() {
    SyncCompartmentsFromEngine();
    if (langBarButton_) langBarButton_->Update();
}

// ---------------------------------------------------------------------------
// Compartments: keyboard open/close and input mode conversion. Windows 11
// reads these for the taskbar input indicator.

DWORD TextService::ReadCompartment(REFGUID guid) const {
    ComPtr<ITfCompartmentMgr> mgr;
    ComPtr<ITfCompartment> c;
    if (!threadMgr_ || FAILED(threadMgr_.As(&mgr)) || FAILED(mgr->GetCompartment(guid, &c))) return 0;
    VARIANT v;
    VariantInit(&v);
    if (FAILED(c->GetValue(&v)) || v.vt != VT_I4) return 0;
    return static_cast<DWORD>(v.lVal);
}

void TextService::WriteCompartment(REFGUID guid, DWORD value) {
    ComPtr<ITfCompartmentMgr> mgr;
    ComPtr<ITfCompartment> c;
    if (!threadMgr_ || FAILED(threadMgr_.As(&mgr)) || FAILED(mgr->GetCompartment(guid, &c))) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = static_cast<LONG>(value);
    c->SetValue(clientId_, &v);
}

void TextService::SyncCompartmentsFromEngine() {
    updatingCompartments_ = true;
    DWORD conversion = 0;
    if (engine_.Chinese()) conversion |= TF_CONVERSIONMODE_NATIVE;
    if (engine_.FullShape()) conversion |= TF_CONVERSIONMODE_FULLSHAPE;
    if (engine_.ChinesePunct()) conversion |= TF_CONVERSIONMODE_SYMBOL;
    WriteCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, 1);
    WriteCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, conversion);
    updatingCompartments_ = false;
}

void TextService::ApplyModeFromCompartments() {
    bool open = ReadCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) != 0;
    DWORD conversion = ReadCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    bool chinese = open && (conversion & TF_CONVERSIONMODE_NATIVE);
    if (chinese != engine_.Chinese()) {
        EngineOutput out = engine_.SetChinese(chinese);
        ComPtr<ITfContext> ctx = compositionContext_ ? compositionContext_ : FocusContext();
        if (ctx) ApplyOutput(ctx.Get(), out);
    }
    engine_.SetFullShape((conversion & TF_CONVERSIONMODE_FULLSHAPE) != 0);
    engine_.SetChinesePunct((conversion & TF_CONVERSIONMODE_SYMBOL) != 0);
    if (langBarButton_) langBarButton_->Update();
}

void TextService::InitCompartments() {
    SyncCompartmentsFromEngine();
    ComPtr<ITfCompartmentMgr> mgr;
    if (!threadMgr_ || FAILED(threadMgr_.As(&mgr))) return;
    auto advise = [&](REFGUID guid, DWORD* cookie) {
        ComPtr<ITfCompartment> c;
        ComPtr<ITfSource> source;
        if (SUCCEEDED(mgr->GetCompartment(guid, &c)) && SUCCEEDED(c.As(&source)))
            source->AdviseSink(IID_ITfCompartmentEventSink, static_cast<ITfCompartmentEventSink*>(this), cookie);
    };
    advise(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &openCloseCookie_);
    advise(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &conversionCookie_);
}

void TextService::UninitCompartments() {
    ComPtr<ITfCompartmentMgr> mgr;
    if (threadMgr_ && SUCCEEDED(threadMgr_.As(&mgr))) {
        auto unadvise = [&](REFGUID guid, DWORD cookie) {
            ComPtr<ITfCompartment> c;
            ComPtr<ITfSource> source;
            if (cookie != TF_INVALID_COOKIE && SUCCEEDED(mgr->GetCompartment(guid, &c)) && SUCCEEDED(c.As(&source)))
                source->UnadviseSink(cookie);
        };
        unadvise(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, openCloseCookie_);
        unadvise(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, conversionCookie_);
    }
    openCloseCookie_ = conversionCookie_ = TF_INVALID_COOKIE;
}

STDMETHODIMP TextService::OnChange(REFGUID rguid) {
    if (updatingCompartments_) return S_OK;
    if (IsEqualGUID(rguid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) ||
        IsEqualGUID(rguid, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION))
        ApplyModeFromCompartments();
    return S_OK;
}

// ---------------------------------------------------------------------------
// Display attributes

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum) return E_INVALIDARG;
    *ppEnum = new (std::nothrow) nova::EnumDisplayAttributeInfo();
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guidInfo, ITfDisplayAttributeInfo** ppInfo) {
    if (!ppInfo) return E_INVALIDARG;
    *ppInfo = nullptr;
    if (!IsEqualGUID(guidInfo, GUID_NovaDisplayAttributeInput)) return E_INVALIDARG;
    *ppInfo = new (std::nothrow) DisplayAttributeInfo();
    return *ppInfo ? S_OK : E_OUTOFMEMORY;
}

// ---------------------------------------------------------------------------
// Function provider: lets Windows open our settings ("Options" button).

STDMETHODIMP TextService::GetType(GUID* pguid) {
    if (!pguid) return E_INVALIDARG;
    *pguid = CLSID_NovaTextService;
    return S_OK;
}

STDMETHODIMP TextService::GetDescription(BSTR* pbstrDesc) {
    if (!pbstrDesc) return E_INVALIDARG;
    *pbstrDesc = SysAllocString(kServiceName);
    return *pbstrDesc ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetFunction(REFGUID rguid, REFIID riid, IUnknown** ppunk) {
    if (!ppunk) return E_INVALIDARG;
    *ppunk = nullptr;
    if (!IsEqualGUID(rguid, GUID_NULL)) return E_NOINTERFACE;
    if (riid == IID_ITfFnConfigure) return QueryInterface(riid, reinterpret_cast<void**>(ppunk));
    return E_NOINTERFACE;
}

STDMETHODIMP TextService::GetDisplayName(BSTR* pbstrName) {
    if (!pbstrName) return E_INVALIDARG;
    *pbstrName = SysAllocString(kServiceName);
    return *pbstrName ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::Show(HWND hwndParent, LANGID, REFGUID) {
    if (IsSecureMode()) return E_ACCESSDENIED;
    LaunchSettings(hwndParent);
    return S_OK;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Profiles: 倉頡 and 速成

STDMETHODIMP TextService::OnActivated(DWORD dwProfileType, LANGID, REFCLSID clsid, REFGUID, REFGUID guidProfile,
                                      HKL, DWORD dwFlags) {
    if (dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && (dwFlags & TF_IPSINK_FLAG_ACTIVE) &&
        IsEqualCLSID(clsid, CLSID_NovaTextService))
        SetQuick(IsEqualGUID(guidProfile, GUID_NovaQuickProfile));
    return S_OK;
}

void TextService::SetQuick(bool quick) {
    if (quick == quick_) return;
    quick_ = quick;
    AbandonComposition();
    engine_.Reset();
    options_.quick = quick_;
    engine_.SetOptions(options_);
    UpdateCandidateWindow(nullptr);
    if (langBarButton_) langBarButton_->Update();
}

void TextService::ReloadOptionsIfChanged(bool force) {
    DWORD rev = OptionsRevision();
    if (!force && rev == optionsRevision_) return;
    optionsRevision_ = rev;
    SharedData::Get().CheckHistoryEpoch();
    options_ = LoadOptions();
    options_.quick = quick_;
    engine_.SetOptions(options_);
    if (candidateUI_) candidateUI_->SetOptions(options_);
    if (langBarButton_) langBarButton_->Update();  // 中 / 簡 may have changed.
}

void TextService::SaveUserData(bool force) {
    if (IsSecureMode()) return;  // Never write learning data on the logon screen.
    SharedData::Get().SaveHistory(force);
}

}  // namespace nova
