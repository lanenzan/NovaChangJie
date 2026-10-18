// NovaChangJie text service - the TSF text input processor.
#pragma once

#include "Globals.h"

#include <ctffunc.h>

#include "Engine.h"
#include "LmClient.h"

namespace nova {

class CandidateUI;
class LangBarButton;

class TextService final : public ITfTextInputProcessorEx,
                          public ITfThreadMgrEventSink,
                          public ITfThreadFocusSink,
                          public ITfKeyEventSink,
                          public ITfCompositionSink,
                          public ITfDisplayAttributeProvider,
                          public ITfCompartmentEventSink,
                          public ITfTextLayoutSink,
                          public ITfFunctionProvider,
                          public ITfFnConfigure,
                          public ITfInputProcessorProfileActivationSink {
public:
    static HRESULT CreateInstance(IUnknown* outer, REFIID riid, void** ppv);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor(Ex)
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;
    STDMETHODIMP Deactivate() override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr* previous) override;
    STDMETHODIMP OnPushContext(ITfContext*) override { return S_OK; }
    STDMETHODIMP OnPopContext(ITfContext*) override { return S_OK; }

    // ITfThreadFocusSink
    STDMETHODIMP OnSetThreadFocus() override;
    STDMETHODIMP OnKillThreadFocus() override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guidInfo, ITfDisplayAttributeInfo** ppInfo) override;

    // ITfCompartmentEventSink
    STDMETHODIMP OnChange(REFGUID rguid) override;

    // ITfTextLayoutSink
    STDMETHODIMP OnLayoutChange(ITfContext* pic, TfLayoutCode lcode, ITfContextView* pView) override;

    // ITfFunctionProvider
    STDMETHODIMP GetType(GUID* pguid) override;
    STDMETHODIMP GetDescription(BSTR* pbstrDesc) override;
    STDMETHODIMP GetFunction(REFGUID rguid, REFIID riid, IUnknown** ppunk) override;

    // ITfFnConfigure (ITfFunction::GetDisplayName + Show)
    STDMETHODIMP GetDisplayName(BSTR* pbstrName) override;
    STDMETHODIMP Show(HWND hwndParent, LANGID langid, REFGUID rguidProfile) override;

    // ITfInputProcessorProfileActivationSink: 倉頡 and 速成 are two profiles of
    // this text service; switching between them does not reactivate it.
    STDMETHODIMP OnActivated(DWORD dwProfileType, LANGID langid, REFCLSID clsid, REFGUID catid,
                             REFGUID guidProfile, HKL hkl, DWORD dwFlags) override;

    // Used by the candidate window and the language bar button.
    void SelectCandidate(size_t index);
    void SetCandidateSelection(size_t index);
    void ChangePage(long delta);
    void CancelComposition();
    void ToggleChinese();
    void ToggleFullShape();
    void ToggleChinesePunct();
    void ToggleSimplified();
    void OpenSettings();
    void OpenUserPhrases();
    const Engine& GetEngine() const { return engine_; }
    bool IsQuick() const { return quick_; }
    bool IsSecureMode() const { return (activateFlags_ & TF_TMAE_SECUREMODE) != 0; }
    ITfThreadMgr* ThreadMgr() const { return threadMgr_.Get(); }
    TfClientId ClientId() const { return clientId_; }

private:
    TextService();
    ~TextService();

    bool ShouldHandle(WPARAM vk) const;
    KeyInput TranslateKey(WPARAM vk, LPARAM lParam) const;
    HRESULT HandleKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, bool test, BOOL* pfEaten);
    HRESULT HandleShiftUp(ITfContext* pic, WPARAM wParam, BOOL* pfEaten);
    // What the application's input scope says about the focused field.
    enum class FieldKind { Normal, Private, Password };
    FieldKind ReadFieldKind(ITfContext* pic);

    // Applies engine output to the document: commit, then composition text,
    // then repositions the candidate window.
    void ApplyOutput(ITfContext* pic, const EngineOutput& out);
    ComPtr<ITfContext> FocusContext() const;

    // Edit-session helpers; `ec` must be a read/write cookie.
    bool StartComposition(TfEditCookie ec, ITfContext* pic);
    void SetCompositionText(TfEditCookie ec, ITfContext* pic, const std::u16string& text, size_t caret);
    void CommitText(TfEditCookie ec, ITfContext* pic, const std::u16string& text);
    void EndComposition(TfEditCookie ec, bool clearText);
    // Clears and ends the composition from outside an edit session.
    void AbandonComposition();
    bool GetAnchorRect(TfEditCookie ec, ITfContext* pic, RECT* rc);
    void UpdateCandidateWindow(const RECT* anchor);
    void RepositionCandidateWindow(ITfContext* pic);

    void InitCompartments();
    void UninitCompartments();
    DWORD ReadCompartment(REFGUID guid) const;
    void WriteCompartment(REFGUID guid, DWORD value);
    void SyncCompartmentsFromEngine();
    void ApplyModeFromCompartments();
    void ModeChanged();

    void AdviseLayoutSink(ITfContext* pic);
    void UnadviseLayoutSink();

    void ReloadOptionsIfChanged(bool force);
    void SetQuick(bool quick);
    void SaveUserData(bool force);

    std::atomic<ULONG> refs_{1};
    ComPtr<ITfThreadMgr> threadMgr_;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    DWORD activateFlags_ = 0;
    DWORD threadMgrSinkCookie_ = TF_INVALID_COOKIE;
    DWORD threadFocusSinkCookie_ = TF_INVALID_COOKIE;
    DWORD profileSinkCookie_ = TF_INVALID_COOKIE;
    DWORD openCloseCookie_ = TF_INVALID_COOKIE;
    DWORD conversionCookie_ = TF_INVALID_COOKIE;
    DWORD layoutCookie_ = TF_INVALID_COOKIE;
    ComPtr<ITfContext> layoutContext_;
    bool keySinkAdvised_ = false;
    bool functionProviderAdvised_ = false;
    bool updatingCompartments_ = false;

    ComPtr<ITfComposition> composition_;
    ComPtr<ITfContext> compositionContext_;
    TfGuidAtom inputAttributeAtom_ = TF_INVALID_GUIDATOM;

    Engine engine_;
    LmClient lm_;  // Neural re-ranking by the NovaChangJieLM helper, if running.
    Options options_;
    DWORD optionsRevision_ = static_cast<DWORD>(-1);

    ComPtr<CandidateUI> candidateUI_;
    ComPtr<LangBarButton> langBarButton_;

    bool quick_ = false;  // The 速成 profile is active.
    bool shiftPending_ = false;
    ULONGLONG shiftDownTick_ = 0;
};

}  // namespace nova
