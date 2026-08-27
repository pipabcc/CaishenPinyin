#pragma once

#include "../engine/pinyin_engine.h"
#include "../engine/shared_engine.h"
#include "../common/typing_stats.h"
#include "ui/candidate_window.h"
#include "ui/ime_ui_logic.h"
#include "ui/shared_status_ui.h"
#include "activation_state.h"
#include "input_policy.h"
#include "langbar_item.h"
#include "punctuation_state.h"
#include "direct_text_commit_request.h"

#include <msctf.h>

#include <cstdint>
#include <string>
#include <vector>

namespace shuru {

enum class ExistingTextCompositionResult;

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
    ShortcutModifierState shortcut_modifier_state_;
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
    // 候选窗保留最近一次稳定确认的组合末端。新文本写入或布局变化后先
    // 保留旧位置，等当前布局通知经过防抖再一次性更新，避免旧矩形曝光。
    bool has_candidate_anchor_ = false;
    RECT candidate_anchor_rect_ {};
    bool candidate_position_pending_ = false;
    bool candidate_layout_notified_ = false;
    // 该宿主是否发送过 ITfTextLayoutSink 通知。发送布局通知的宿主必须
    // 等到当前文本世代的布局到达后才允许解析锚点，否则 Chromium 系宿主
    // 在渲染器重排完成前会返回组合起点的旧矩形，造成候选窗左闪。
    bool candidate_layout_sink_seen_ = false;
    // 锚点确认时的拼音长度。组合只增不减（正常输入）时，向左且未换行
    // 的测量结果视为旧布局残留，拒绝更新锚点。
    std::size_t candidate_anchor_pinyin_len_ = 0;
    std::uint64_t candidate_layout_generation_ = 0;
    std::uint64_t candidate_layout_serial_ = 0;
    unsigned candidate_position_attempts_ = 0;
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
    LangBarItemButton* langbar_item_ = nullptr;
    ITfLangBarItemMgr* langbar_item_mgr_ = nullptr;
    bool status_ui_acquired_ = false;
    DWORD status_ui_thread_id_ = 0;
    bool clipboard_monitor_checked_ = false;
    std::uint64_t first_key_copy_tick_ = 0;
    std::uint64_t first_key_focus_tick_ = 0;
    ITfContext* first_key_copy_context_ = nullptr;
    HWND first_key_copy_window_ = nullptr;
    // 武装触发器的快捷键（'C'/'V'/'X'；0 表示焦点触发）。V 触发器后面
    // 预期紧跟宿主自己的整段粘贴写入，不能据此解除触发器。
    wchar_t first_key_trigger_key_ = 0;
    // 触发器存活期间观察到的"整段写入"末端（ACP）。粘贴正文末尾可能恰
    // 好是孤立字母，清扫网只回收该边界之后新出现的字母，防止误吃正文。
    bool first_key_boundary_valid_ = false;
    LONG first_key_boundary_acp_ = 0;
    ITfContext* first_key_boundary_context_ = nullptr;
    std::uint64_t first_key_recovery_generation_ = 0;
    std::uint64_t pending_first_key_generation_ = 0;
    std::uint64_t pending_first_key_started_tick_ = 0;
    ITfContext* pending_first_key_context_ = nullptr;
    wchar_t pending_first_key_character_ = 0;
    ITfRange* recovered_composition_start_ = nullptr;
    struct BufferedFirstKeyInput {
        WPARAM wparam = 0;
        LPARAM lparam = 0;
    };
    std::vector<BufferedFirstKeyInput> pending_first_key_inputs_;
    bool first_key_recovery_pending_ = false;
    bool first_key_recovery_adopted_ = false;

    // 独立搜索窗口文本直达会话。目标上下文由创建会话的 TSF 线程持有，
    // 设置程序只拿到不可预测令牌，不能自行选择其他输入框上屏。
    std::wstring direct_commit_token_;
    ITfContext* direct_commit_context_ = nullptr;
    HWND direct_commit_window_ = nullptr;
    std::uint64_t direct_commit_started_tick_ = 0;

    HRESULT InitEngine();
    void EnsureUiWindows();
    void RollbackActivation() noexcept;
    bool IsOwnerThread() const noexcept;
    HRESULT AdviseThreadMgrEventSink();
    HRESULT UnadviseThreadMgrEventSink();
    HRESULT BindEditContext(ITfContext* context);
    HRESULT AdviseTextEditSink(ITfDocumentMgr* doc_mgr);
    HRESULT UnadviseTextEditSink();
    static void UnadviseContextSinks(
        ITfContext* context, DWORD text_cookie, DWORD layout_cookie);
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
    void ScheduleCandidateWindowUpdate();
    void TryResolveCandidateAnchor(
        std::uint64_t generation, std::uint64_t layout_serial);
    bool GetCaretScreenRect(ITfContext* context, RECT* rect);
    bool TryGetHostFallbackRect(ITfContext* context, RECT* rect);
    void StartShiftReleasePolling();
    void StopShiftReleasePolling();
    void StartShortcutReleasePolling();
    void StopShortcutReleasePolling();
    void RecordShortcutForFirstKeyRecovery(
        ITfContext* context, WPARAM wparam,
        bool shortcut_modifier) noexcept;
    void TryRecoverExternalFirstKey(
        ITfContext* context, TfEditCookie read_cookie,
        ITfEditRecord* edit_record);
    bool TrySweepFallenFirstKey(ITfContext* context);
    bool BeginFirstKeyRecovery(
        ITfContext* context, ITfRange* range, wchar_t character);
    bool CanAdoptFirstKeyRecovery(
        std::uint64_t generation, ITfContext* context) const noexcept;
    void CompleteFirstKeyRecovery(
        std::uint64_t generation, ExistingTextCompositionResult result);
    void CancelFirstKeyRecovery(bool disarm_trigger) noexcept;
    void DisarmFirstKeyRecoveryTrigger() noexcept;
    void ArmFirstKeyRecoveryFocus() noexcept;
    void ExpireFirstKeyRecovery() noexcept;
    bool FirstKeyTriggerScopeMatches(
        ITfContext* context, std::uint64_t now,
        bool* focus_rebuilt_after_copy = nullptr) const noexcept;
    void RecordFirstKeyEditBoundary(
        ITfContext* context, bool acp_known, LONG boundary_acp) noexcept;
    bool HandlePendingFirstKeyInput(
        ITfContext* context, WPARAM wparam, LPARAM lparam, bool shortcut_modifier,
        bool test_callback, bool* eaten);
    void StartVModeWindowTimer();
    bool StartDirectQuickWindow(ITfContext* context, bool phrases);
    bool LaunchQuickWindowWithFallback(
        ITfContext* context, bool phrases, bool clear_composition);
    bool PollDirectTextCommit();
    void FinishDirectTextCommit(DirectTextCommitResult result);
    void CancelDirectTextCommit() noexcept;
    void CompleteShiftTap(ITfContext* context);
    void ResetCandidateAnchor() noexcept;
    void ClearCompositionState();
    void ResetForContextTransition();
    bool RebindFocusedContext();
    bool IsCurrentTopContext(ITfContext* context) const;
    void AbortRejectedComposition(HRESULT reason, ITfContext* attempted_context);
    void ToggleEnglishMode();
    void SyncStatusUi();
    void SyncLangBarItemEnglishMode();
    HRESULT SyncInputModeCompartments() noexcept;
    HRESULT InitLangBarItem();
    void UninitLangBarItem() noexcept;
    void ToggleSoftKeyboard();
    void OnSoftKey(wchar_t ch, bool is_special);
    void OnCandidateSelected(size_t index);
    void OnCandidatePinToggled(size_t index);
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
