// ---------------- docsy AI: the workers' event pump ----------------
// The one channel every AI worker talks back through: heap events posted to
// the main frame, whose proc (main_window.inl) forwards WM_APP + 71 into
// ai_ui3.inl's AiHandleEvent on the UI thread. The first cut routed them to a
// message-only window whose wndproc was DefWindowProc - it swallowed every
// WM_APP + 71, so no chat answer, no summary, no notes card ever came home
// (and the pump window itself was only born when the chat panel toggled, so
// the summary window's whole chain died in silence before it even started).
// Declared this early because the editor's own page messages join the flow.
#pragma once

enum AiEvKind {
    AIEV_DELTA = 1,       // utf8 visible answer so far (accumulated raw)
    AIEV_DONE,            // a: answer; b: sources blob; d: duration ms
    AIEV_ERROR,           // a: friendly text
    AIEV_CANCEL,          // the reply was stopped
    AIEV_IDX_STATUS,      // indexing line + progress
    AIEV_SEM_STATUS,      // the semantic line for the current doc
    AIEV_NOTES_STATUS,    // notes busy/status line
    AIEV_NOTES_CARDS,     // b: cards blob
    AIEV_SUM_INIT, AIEV_SUM_PROGRESS, AIEV_SUM_DELTA, AIEV_SUM_DONE,
    AIEV_SUM_ERROR, AIEV_SUM_NOTEXT, AIEV_EXTRACT_DONE, AIEV_WEB_TEXT,
    AIEV_ED_TEXT, AIEV_TEST_OUT, AIEV_AXO_DONE
};
struct AiEv {
    AiEvKind kind;
    unsigned long long gen = 0;
    std::string key;                     // the context the event belongs to
    std::string a, b;                    // utf8 payloads
    double d = 0;
    bool flag = false;
};
static void AiPost(AiEv* e) {
    // the main frame owns the AI event queue: its proc answers WM_APP + 71
    // with AiHandleEvent, on the UI thread, whether or not any AI surface is
    // open - the summary window's chain needs no other seat to live
    if (g.main) PostMessageW(g.main, WM_APP + 71, 0, (LPARAM)e);
    else delete e;
}
static AiEv* AiEvMake(AiEvKind k) { AiEv* e = new AiEv(); e->kind = k; return e; }

// the UI-side entry points the early modules call (defined in ai_ui*.inl)
static void AiChatToggle();
static void AiModeChanged();
static void AiOverlayLayout();
static void SumToggle(int mode);
static void SumOwnerMoved();   // the frame moved: the summary popup rides along
