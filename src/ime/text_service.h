#pragma once

#include "../engine/pinyin_engine.h"
#include "../engine/shared_engine.h"
#include "../common/typing_stats.h"
#include "ui/candidate_window.h"
#include "ui/ime_ui_logic.h"
#include "ui/shared_status_ui.h"
#include "activation_state.h"
#include "input_policy.h"
#include "punctuation_state.h"

#include <msctf.h>

#include <string>

namespace shuru {

class TextService :
    public ITfTextInputProcessorEx,
    public ITfThreadMgrEventSink,
    public ITfKeyEventSink,
    public ITfTextEditSink,
    public ITfTextLayoutSink,
    public ITfCompositionSink,
    public ITfDisplayAttributeProvider {
public:
    TextService();
    virtual ~TextService();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    bool TryAddRefForUiCallback() noexcept;

    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext* pic) override;
    STDMETHODIMP OnPopContext(ITfContext* pic) override;

    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    STDMETHODIMP OnEndEdit(ITfContext* pic, TfEditCookie ecReadOnly, ITfEditRecord* pEditRecord) override;
    STDMETHODIMP OnLayoutChange(ITfContext* pic, TfLayoutCode lcode, ITfContextView* view) override;
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) override;

    // SharedStatusUi 回调（每个 TSF UI 线程共享一套）
    void OnStatusToggleEnglish();
    void OnStatusToggleSchema();
    void OnStatusToggleKeyboard();
    void OnStatusSoftKey(wchar_t ch, bool is_special);

private:
    LONG ref_ = 1;

    ITfThreadMgr* thread_mgr_ = nullptr;
    TfClientId client_id_ = TF_CLIENTID_NULL;
    DWORD thread_mgr_cookie_ = TF_INVALID_COOKIE;
    DWORD text_edit_cookie_ = TF_INVALID_COOKIE;
    DWORD text_layout_cookie_ = TF_INVALID_COOKIE;
    ITfContext* edit_context_ = nullptr;

    ITfComposition* composition_ = nullptr;
    bool english_mode_ = false;
    bool shuangpin_mode_ = false;  // 小鹤双拼
    SchemaSyncState schema_sync_;
    ShiftTapState shift_tap_;
    ShortcutModifierDecisionCache shortcut_modifier_cache_;
    TfGuidAtom display_atom_ = TF_INVALID_GUIDATOM;
    DWORD owner_thread_id_ = 0;
    bool key_sink_advised_ = false;
    ActivationState activation_state_;
    ChinesePunctuationState punctuation_state_;
    std::wstring recent_committed_text_;

    std::string composing_pinyin_;
    CandidatePageState candidate_state_;
    EngineQueryResult current_result_;
    std::wstring candidate_display_;
    std::wstring candidate_display_fallback_;
    bool composition_edit_in_progress_ = false;
    // 用户拖动候选窗后的固定位置；仅当前组合会话内有效，组合结束即恢复跟随光标。
    bool candidate_pos_overridden_ = false;
    POINT candidate_override_pos_ {};
    // 上屏后联想：最近一次上屏的候选文本作为上下文；联想激活期间
    // 候选窗展示 bigram 后继，数字键直接上屏。
    std::wstring last_committed_word_;
    bool association_active_ = false;
    std::vector<Candidate> association_candidates_;

    PinyinEngine* engine_ = nullptr;  // 进程内共享
    CandidateWindow candidate_window_;
    TypingStatsStore typing_stats_;
    bool status_ui_acquired_ = false;
    DWORD status_ui_thread_id_ = 0;
    bool clipboard_monitor_checked_ = false;

    HRESULT InitEngine();
    void EnsureUiWindows();
    void RollbackActivation() noexcept;
    bool IsOwnerThread() const noexcept;
    HRESULT AdviseThreadMgrEventSink();
    HRESULT UnadviseThreadMgrEventSink();
    HRESULT AdviseTextEditSink(ITfDocumentMgr* doc_mgr);
    HRESULT UnadviseTextEditSink();
    HRESULT AdviseKeyEventSink();
    HRESULT UnadviseKeyEventSink();

    bool HandleKeyDown(ITfContext* context, WPARAM wparam, LPARAM lparam, bool* eaten);
    void RefreshCandidates();
    void LearnCandidate(ITfContext* context, const Candidate& candidate,
                        const std::string& learned_pinyin, const std::string& learned_input);
    void ShowAssociation(ITfContext* context);
    void DismissAssociation();
    void SyncCandidateWindowCandidates();
    void UpdateCandidateWindow(ITfContext* context);
    bool GetCaretScreenRect(ITfContext* context, RECT* rect);
    void ResetCandidateAnchor() noexcept;
    void ClearCompositionState();
    void ToggleEnglishMode();
    void SyncStatusUi();
    void ToggleSoftKeyboard();
    void OnSoftKey(wchar_t ch, bool is_special);
    void OnCandidateSelected(size_t index);
    bool CommitCandidate(ITfContext* context, const Candidate& candidate);
    bool CommitRawComposition(ITfContext* context);
    static void SendVirtualKey(WORD vk);

    HRESULT EndComposition();
    HRESULT CommitText(ITfContext* context, const std::wstring& text, bool count_typing_stats = true);
    HRESULT SetCompositionString(ITfContext* context, const std::wstring& text);

    bool ShortcutModifierForKey(
        WPARAM wparam, LPARAM lparam, bool test_callback);
    bool IsKeyEaten(
        ITfContext* context, WPARAM wparam, bool shortcut_modifier) const;
    bool IsPasswordContext(ITfContext* context) const;
    static bool IsAsciiPrintable(WPARAM wparam);
};

} // namespace shuru
