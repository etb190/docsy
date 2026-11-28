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
    AIEV_SEM_INDEX,       // heavy: the finished embedding pass's index - the
                          // vectors ride home to the LIVE seat here (the old
                          // pass built them into a local copy, saved the file
                          // and dropped the memory: the sidechat's retrieval
                          // stayed keyword-only until the app restarted)
    AIEV_NOTES_STATUS,    // notes busy/status line
    AIEV_NOTES_CARDS,     // b: cards blob
    AIEV_SUM_INIT, AIEV_SUM_PROGRESS, AIEV_SUM_DELTA, AIEV_SUM_DONE,
    AIEV_SUM_ERROR, AIEV_SUM_NOTEXT, AIEV_EXTRACT_DONE, AIEV_WEB_TEXT,
    AIEV_ED_TEXT, AIEV_TEST_OUT, AIEV_AXO_DONE,
    AIEV_SUM_PREFETCH,    // a: buffer digest (empty = failed); key: first page
    AIEV_SUM_TESTOUT,     // a: beaker probe output; flag: ok; gen: the run
    AIEV_WORD_OUT         // the word plate's Define/Explain answer: a: text;
                          // b: action ("define"/"explain"); key: the seat id;
                          // flag: ok
};
struct AiDocIndex;                       // complete in ai_index.inl - the heavy
                                         // rider below only moves through here,
                                         // and shared_ptr manages an incomplete
                                         // type once make_shared captured its
                                         // deleter (ai_ui2.inl, complete there)
struct AiEv {
    AiEvKind kind;
    unsigned long long gen = 0;
    std::string key;                     // the context the event belongs to
    std::string a, b;                    // utf8 payloads
    double d = 0;
    bool flag = false;
    std::shared_ptr<AiDocIndex> heavy;   // the big riders (an index's vectors)
                                         // travel by pointer - copying them
                                         // through the pump would memcpy the
                                         // document twice per handback
};
struct AiOcrPayload {                    // one finished OCR page, marshaled home
                                         // on WM_APP + 72 (the async flight's
                                         // landing - the recognition ran on its
                                         // own async completion, never on the
                                         // thread that asked for the page)
    std::wstring path;                   // the document it belongs to: an
                                         // arrival whose document is gone dies
    int page = -1;                       // 0-based page index
    int ow = 0, oh = 0;                  // the OCR raster the boxes are native
                                         // to; the UI thread scales them into
                                         // page DIPs where D.pw/D.ph live
    bool ok = false;                     // false = the engine failed: the
                                         // arrival parks an empty page so the
                                         // waiting pump moves on instead of
                                         // re-asking forever
    std::vector<TxtCell> cells;          // boxes in OCR pixels
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
static void AiPanelEnsure();           // the panel's own builder: the env's
                                       // cold-start knock (4723) re-rings it
                                       // from the main frame's timer block
static void AiModeChanged();
static void AiOverlayLayout(bool early);   // the default rides common.h's prototype
static void SumToggle(int mode);
static void SumOwnerMoved();   // the frame moved: the summary popup rides along
static void SumOutsideClick(long x, long y);   // the pump's verdict on an outside click
static void SumDragTick(long x, long y);       // the popup's drag/resize, one tick per move
static void SumDragEnd();                      // the drag's button-up: placement saved
static void SumEnsureRetry();                  // the env's retry knock, served on the pump
static void SumPrefetchKick();                 // the buffer's deferred kick, re-homed
static void SumSyncVisible();  // the frame's minimize/restore drives the seat's
                               // render - the controller never follows its host
