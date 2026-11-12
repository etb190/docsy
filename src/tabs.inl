// ============================================================
//  Tabs - KillerPDF's DocumentSession model (Controls/Viewer/PdfViewer.Tabs.cs)
// ============================================================
// One window, many documents: every open PDF is a session in its pane's strip,
// the front tab's fields are the live working set, and switching tabs captures
// the live fields into the outgoing session and applies the incoming session's
// fields, then re-renders. The strip itself is per pane (KillerPDF keeps
// _sessions/_active on the viewer, not the window), the tab band appears only
// from the second document of that pane, and every open/close/switch route
// below mirrors its namesake there: TabOpenInto is OpenInNewTab + BeginTabLoad,
// SwitchTab is SwitchToTab, CloseTabAt is CloseTabCore, CloseOtherTabs and
// CloseAllTabs keep their prompts, CycleTab keeps its wrap, and TabMoveToPane
// is DetachSessionExt + AdoptSessionExt. KillerPDF's lazy deferred tabs and
// restart restore are the one behavior docsy does not carry: it keeps no
// settings store at all, so there is no OpenTabs key to restore from.

static std::vector<DocState>& Tabs(bool b) { return b ? g.tabs[1] : g.tabs[0]; }
static int& Act(bool b) { return b ? g.act[1] : g.act[0]; }
static unsigned long long g_tabTick = 0;

static DocState* DocById(int id) {          // the render results' owner lookup: the tabs
    if (!id) return nullptr;                // vector moves, so the drain re-resolves by id
    if (g.d.id == id) return &g.d;          // the front document lives in the working set,
    if (g.dB.id == id) return &g.dB;        // OUTSIDE the tab vectors, until a switch parks
    for (int p = 0; p < 2; p++)             // it there - the drain must find it in g.d/g.dB
        for (auto& t : Tabs(p != 0))        // or every on-screen page renders white forever
            if (t.id == id) return &t;
    return nullptr;
}

// The worker's finished renders land here on the UI thread: each result
// re-validated against what the owner still wants (the page may have been
// zoomed, rotated or closed meanwhile) and inserted into the very cache the
// sync path used. Pages repaint the view; thumbs repaint just their own strip.
static void RqDrain() {
    std::vector<RRes> done;
    { std::lock_guard<std::mutex> lk(g_rqM); done.swap(g_rqDone); }
    if (done.empty()) return;
    bool viewInv = false;
    for (auto& r : done) {
        if (r.kind == RJ_PAGE) {
            DocState* D = DocById(r.docId);
            if (!D || r.page < 0 || r.page >= D->n) { if (r.b) DeleteObject(r.b); continue; }
            int want = (D->prot.count(r.page) ? D->prot[r.page] : 0) |
                       (D->pflip.count(r.page) ? D->pflip[r.page] << 4 : 0);
            if (r.w != PW(*D, r.page) || r.h != PH(*D, r.page) || r.rot != want) {
                if (r.b) DeleteObject(r.b);            // stale size or transform: the next
                continue;                              // paint re-asked at the right shape
            }
            CacheDrop(*D, r.page);
            if (!r.b) continue;            // a failed page render stays uncached and unpainted:
                                           // a null entry would make every later paint a hit
                                           // that returns nothing, the page white for good -
                                           // the next paint asks again, the sync path's own
                                           // behavior. Thumbs keep their deliberate placeholder
            D->cache[r.page] = Ent{ r.b, r.w, r.h, r.rot, ++D->tick };
            D->cacheBytes += (long long)r.w * r.h * 4;
            Evict(*D, r.page, r.page);
            viewInv = true;
        } else {
            DocState* D = DocById(r.docId);
            if (!D || D != &DVF() || r.page < 0 || r.page >= D->n || r.w != sd.tw || r.h != STH(r.page)) {
                if (r.b) DeleteObject(r.b);
                continue;                              // the strip moved to another document
            }
            sd.th[r.page] = Ent{ r.b, r.w, r.h, 0, ++sd.tick };
            if (r.b) sd.thBytes += (long long)r.w * r.h * 4;
            SideThEvict(SidePageAt(sd.scroll), SidePageAt(sd.scroll + sd.ch - 1));
            if (g.side && sd.top.size() > (size_t)r.page) {
                RECT tr{ 0, sd.top[r.page] - sd.scroll, max(1, sd.cw),
                         sd.top[r.page] - sd.scroll + STH(r.page) + (int)(36 * g.dpi) };
                InvalidateRect(g.side, &tr, FALSE);    // just the card that arrived
            }
        }
    }
    if (viewInv && g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// The strip counts DOCUMENTS, not slots: a pane's blank start state is not a
// tab, and the front slot is a stale husk while its document is live, so the
// count reads the live state there (KillerPDF counts Doc/DeferredPath).
static int TabDocCount(bool b) {
    std::vector<DocState>& T = Tabs(b);
    if (T.empty()) return DV(b).n > 0 ? 1 : 0;
    int c = 0;
    for (int i = 0; i < (int)T.size(); i++)
        if ((i == Act(b) ? DV(b).n : T[i].n) > 0) c++;
    return c;
}
static bool BandShown(bool b) {   // RebuildTabStrip: the strip exists from the second
    return !g.full && TabDocCount(b) > 1;   // document, and full screen gives the row to the page
}
static int BANDH() {              // the band: 3px top inset + 29px face + 1px foot
    return (int)(33 * g.dpi);     // (KillerPDF's TabMargin 0,3,0,1 around the 29px face)
}
static int BandInset(bool b) { return BandShown(b) ? BANDH() : 0; }

// Make sure the pane HAS a session list, adopting the live working set as its
// first session's document on first use (EnsureInitialSession, minus the
// capture: the front slot stays a husk until a switch parks the live state).
static void TabEnsure(bool b) {
    if (!Tabs(b).empty()) return;
    Tabs(b).push_back(DocState{});
    g.tabUndo[b].push_back({});
    g.tabRedo[b].push_back({});
    Act(b) = 0;
}

// Copy the live working set INTO the front session (CaptureSessionState). The
// move is the capture: the bitmaps, annotations, transforms, checkpoint and
// undo pair all ride the DocState. The live fields are left a husk that the
// very next apply overwrites - nothing may read them in between.
static void TabCaptureLive(bool b) {
    TabEnsure(b);
    int a = Act(b);
    Tabs(b)[a] = std::move(DV(b));
    g.tabUndo[b][a] = std::move(g.undo);
    g.tabRedo[b][a] = std::move(g.redo);
    DV(b) = DocState{};
}

// KillerPDF's CancelTransientForSwitch: the live interactions die before the
// working set changes, so nothing bleeds onto the next document.
static void TabCancelTransients(bool b) {
    CommitTextEdit();
    if (g.annPaneB == b) {                        // the pane's own gestures die with it
        PolyCancel(g.view);
        g.annInk = false; g.annArm = false; g.annMove = false; g.annResize = false;
    }
    if (g.annSelB == b) { g.annSelPage = -1; g.annSelIdx = -1; }
    g.brushOn = false;
    if (g.barOn) {                                // the floating bar belongs to the outgoing tool
        g.barOn = false; g.barTool = -1;
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    }
    if (fd.open && fd.pane == b) {                // the find is pinned to ITS pane (CloseSearchBar)
        if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
        if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
        fd = Find{};
    }
    SelClear(DV(b));                              // ClearSelection + ClearTextSelection
}

// Point the live working set AT the session (ApplySessionState), mark it the
// front tab, and hand the window-level surfaces their new document.
static void TabApply(bool b, int idx) {
    DV(b) = std::move(Tabs(b)[idx]);
    g.undo = std::move(g.tabUndo[b][idx]);
    g.redo = std::move(g.tabRedo[b][idx]);
    Act(b) = idx;
    DV(b).tabTick = ++g_tabTick;
}

// TouchRenderLru: three tabs keep their rendered page bitmaps no matter what
// (KillerPDF's RenderCacheTabCap window), and MORE tabs stay warm as long as
// the pane's whole strip fits the byte budget - ten light documents come back
// instantly, memory still has a hard ceiling. ClearCache runs per session,
// exactly the per-tab cache KillerPDF clears when a tab falls out of the LRU.
static void TabTrimCaches(bool b) {
    std::vector<DocState>& T = Tabs(b);
    std::vector<std::pair<unsigned long long, int>> v;
    for (int i = 0; i < (int)T.size(); i++)
        if (i != Act(b) && !T[i].cache.empty()) v.push_back({ T[i].tabTick, i });
    std::sort(v.begin(), v.end(), [](auto& a, auto& c) { return a.first > c.first; });
    long long total = 0;
    for (auto& t : T) total += t.cacheBytes;
    for (int k = 3; k < (int)v.size() && total > TAB_CACHE_BUDGET; k++) {
        total -= T[v[k].second].cacheBytes;
        ClearCache(T[v[k].second]); T[v[k].second].txt.clear();
    }
}

// The surfaces that read the FOCUSED pane's document after its identity
// changed: the sidebar's ring, the footer's page/zoom, the rail's light and
// the toolbar's tool - FocusPane's choreography, reused for a tab switch.
static void TabChrome(bool b) {
    if (b != FocusB()) {                          // an unfocused pane's switch touches only itself
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);
        return;
    }
    for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
    sd.th.clear(); sd.thBytes = 0; sd.scroll = 0; sd.cur = -1; sd.hold = 0; sd.tw = 0;
    // sd.cw/sd.ch stay: zeroing them made the SideLayout below early-return on
    // cw <= 0, which killed the thumbnail pump, froze the strip on the outgoing
    // document's pixels and left sd.top describing the OTHER document - the
    // next paint then indexed the focused document against it
    SideLayout();
    UpdateTitle();
    SideSync();
    StatusRepaint();
    RailRepaint();
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// SwitchToTab: make session idx the pane's front tab.
static void SwitchTab(bool b, int idx) {
    TabEnsure(b);
    if (idx < 0 || idx >= (int)Tabs(b).size() || idx == Act(b)) return;
    TabCancelTransients(b);
    TabCaptureLive(b);                            // the outgoing tab parks with its state
    TabApply(b, idx);                             // ... and the target becomes live
    TabTrimCaches(b);
    LayoutMain();                                 // the band may appear or collapse; reflow
    TabChrome(b);
}

// FindOpenSession: an open session for this exact file (case-insensitive full
// path), or -1. The front slot reads the live state, the only place its
// current path is guaranteed current.
static int TabFindOpen(bool b, const std::wstring& p) {
    wchar_t want[MAX_PATH * 4];
    GetFullPathNameW(p.c_str(), (DWORD)_countof(want), want, nullptr);
    std::vector<DocState>& T = Tabs(b);
    for (int i = 0; i < (int)T.size(); i++) {
        const DocState& s = (i == Act(b)) ? DV(b) : T[i];
        if (!s.n || s.path.empty()) continue;
        wchar_t have[MAX_PATH * 4];
        GetFullPathNameW(s.path.c_str(), (DWORD)_countof(have), have, nullptr);
        if (!lstrcmpiW(have, want)) return i;
    }
    return -1;
}

// OpenInNewTab: the dialog, a drop and the command line all land here. An
// already-open CLEAN tab wins over a duplicate (FindOpenSession's short
// circuit, "Already open" in the footer); an empty front tab is reused
// (BeginTabLoad's reuse rule); a busy one spawns a new session at the strip's
// end that inherits the current view mode, so a fresh document does not snap
// back to Continuous when the reader lives in Single. A failed open rolls the
// strip back exactly (AbortTabLoad).
static bool TabOpenInto(bool b, const std::wstring& p) {
    int found = TabFindOpen(b, p);
    if (found >= 0) {
        const DocState& s = (found == Act(b)) ? DV(b) : Tabs(b)[found];
        if (!DocDirty(s)) {
            SwitchTab(b, found);                  // a no-op when it is already the front tab
            SetStatusHold(std::wstring(L"Already open: ") + s.name);
            return true;
        }
    }
    TabEnsure(b);
    int prev = Act(b);
    bool wasEmpty = !DV(b).n;                     // the front tab is blank: reuse it
    int created = -1;
    if (!wasEmpty) {
        int vm = DV(b).vm, fit = DV(b).fitMode;   // the view mode the new tab inherits
        TabCancelTransients(b);
        TabCaptureLive(b);                        // the current document parks in its slot
        Tabs(b).push_back(DocState{});            // the new session rides the strip's end
        g.tabUndo[b].push_back({});
        g.tabRedo[b].push_back({});
        Act(b) = (int)Tabs(b).size() - 1;
        DV(b) = DocState{};
        DV(b).vm = vm; DV(b).fitMode = fit;
        created = Act(b);
    } else {
        TabCancelTransients(b);
    }
    if (!Open(DV(b), p)) {
        if (created >= 0) {                       // AbortTabLoad: the strip never saw it
            Tabs(b).pop_back();
            g.tabUndo[b].pop_back();
            g.tabRedo[b].pop_back();
            DV(b) = std::move(Tabs(b)[prev]);
            g.undo = std::move(g.tabUndo[b][prev]);
            g.redo = std::move(g.tabRedo[b][prev]);
            Act(b) = prev;
        }
        return false;
    }
    return true;
}

// CloseTabCore. Returns false when a prompt was canceled (the tab stays), so
// the sweep helpers can stop or skip. An INACTIVE dirty tab is brought forward
// first - a save works on the live document - exactly KillerPDF's route.
static bool CloseTabAt(bool b, int idx) {
    TabEnsure(b);
    if (idx < 0 || idx >= (int)Tabs(b).size()) return false;
    bool live = idx == Act(b);
    if (!(live ? DV(b).n : Tabs(b)[idx].n)) return false;   // the blank start tab is not a document
    if (!live && DocDirty(Tabs(b)[idx])) SwitchTab(b, idx);
    if (idx != Act(b)) {                          // a clean inactive tab: release it where it sits
        ClearCache(Tabs(b)[idx]);
        Tabs(b).erase(Tabs(b).begin() + idx);
        g.tabUndo[b].erase(g.tabUndo[b].begin() + idx);
        g.tabRedo[b].erase(g.tabRedo[b].begin() + idx);
        if (idx < Act(b)) Act(b)--;
        LayoutMain();
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);
        StatusRepaint();
        return true;
    }
    // the front tab: capture nothing - the live fields ARE the tab
    if (g.tedit) CommitTextEdit();
    TabCancelTransients(b);
    if (DocDirty(DV(b))) {                        // KillerPDF's tab close asks first
        int r = AskPrompt3(L"", L"This file has unsaved changes. Save them before closing it?",
                           L"Cancel", L"Don't Save", L"Save");
        if (r == 0) return false;                 // Cancel: the document stays open
        if (r == 2) {                             // Save: a failed write keeps it open too
            std::wstring err;
            if (!WriteAnnotatedPdf(DV(b), DV(b).path, err)) {
                SetStatusHold(std::wstring(L"Save failed: ") + err);
                return false;
            }
        }
    }
    ClearCache(DV(b));                            // the document's bitmaps die with it
    if (fd.open && fd.pane == b) {                // the find dies only when ITS document dies
        if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
        if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
        fd = Find{};
    }
    Tabs(b).erase(Tabs(b).begin() + idx);
    g.tabUndo[b].erase(g.tabUndo[b].begin() + idx);
    g.tabRedo[b].erase(g.tabRedo[b].begin() + idx);
    if (Tabs(b).empty()) {                        // the last tab: a blank session and the
        Tabs(b).push_back(DocState{});            // empty drop-zone state (CloseTabCore's blank)
        g.tabUndo[b].push_back({});
        g.tabRedo[b].push_back({});
        Act(b) = 0;
        DV(b) = DocState{};
        g.undo.clear(); g.redo.clear();
    } else {                                      // the neighbor takes the front (idx clamped)
        int next = min(idx, (int)Tabs(b).size() - 1);
        DV(b) = std::move(Tabs(b)[next]);
        g.undo = std::move(g.tabUndo[b][next]);
        g.redo = std::move(g.tabRedo[b][next]);
        Act(b) = next;
        DV(b).tabTick = ++g_tabTick;
    }
    g.annSelPage = -1; g.annSelIdx = -1;
    LayoutMain();
    TabChrome(b);
    SetStatusHold(L"Document closed");
    return true;
}

// CloseOtherTabs: every other document tab closes, each dirty one asking like
// a manual close; a canceled tab is skipped and the sweep moves on. The keeper
// is followed by its path - closes above it shift the indices under it.
static void CloseOtherTabs(bool b, int keep) {
    TabEnsure(b);
    if (keep < 0 || keep >= (int)Tabs(b).size()) return;
    std::wstring keepPath = (keep == Act(b)) ? DV(b).path : Tabs(b)[keep].path;
    std::vector<std::wstring> skipped;
    for (;;) {
        int k = -1;
        for (int i = 0; i < (int)Tabs(b).size(); i++)
            if (((i == Act(b)) ? DV(b).path : Tabs(b)[i].path) == keepPath) { k = i; break; }
        if (k < 0) return;
        int hit = -1;
        std::wstring hitPath;
        for (int i = 0; i < (int)Tabs(b).size(); i++) {
            if (i == k) continue;
            bool live = i == Act(b);
            if (!((live ? DV(b).n : Tabs(b)[i].n) > 0)) continue;
            const std::wstring& p2 = live ? DV(b).path : Tabs(b)[i].path;
            bool gone = false;
            for (auto& s2 : skipped) if (s2 == p2) { gone = true; break; }
            if (gone) continue;
            hit = i; hitPath = p2;
            break;
        }
        if (hit < 0) return;
        if (!CloseTabAt(b, hit)) skipped.push_back(hitPath);
    }
}

// CloseAllTabs (Ctrl+Shift+Q): ONE combined ask for every dirty tab rather
// than a prompt each, then the pane resets to a single blank session.
static void CloseAllTabs(bool b) {
    TabEnsure(b);
    bool any = false, anyDirty = false;
    for (int i = 0; i < (int)Tabs(b).size(); i++) {
        DocState& S = (i == Act(b)) ? DV(b) : Tabs(b)[i];
        if (!S.n) continue;
        any = true;
        if (DocDirty(S)) anyDirty = true;
    }
    if (!any) return;
    if (g.tedit) CommitTextEdit();
    if (anyDirty) {
        int r = AskPrompt3(L"", L"You have unsaved changes. Save them before closing all tabs?",
                           L"Cancel", L"Don't Save", L"Save");
        if (r == 0) return;
        if (r == 2) {
            for (int i = 0; i < (int)Tabs(b).size(); i++) {
                DocState& S = (i == Act(b)) ? DV(b) : Tabs(b)[i];
                if (!S.n || !DocDirty(S)) continue;
                std::wstring err;
                if (!WriteAnnotatedPdf(S, S.path, err)) {
                    SetStatusHold(std::wstring(L"Save failed: ") + err);
                    return;                       // a failed save stops the sweep, window up
                }
                SnapClean(S);
            }
        }
    }
    for (int i = 0; i < (int)Tabs(b).size(); i++) ClearCache(Tabs(b)[i]);
    if (fd.open && fd.pane == b) {
        if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
        if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
        fd = Find{};
    }
    Tabs(b).clear();
    g.tabUndo[b].clear();
    g.tabRedo[b].clear();
    Tabs(b).push_back(DocState{});
    g.tabUndo[b].push_back({});
    g.tabRedo[b].push_back({});
    Act(b) = 0;
    DV(b) = DocState{};
    g.undo.clear(); g.redo.clear();
    g.annSelPage = -1; g.annSelIdx = -1;
    LayoutMain();
    TabChrome(b);
    SetStatusHold(L"All tabs closed");
}

// CycleTab: Ctrl+Tab walks the document tabs forward, Ctrl+Shift+Tab backward,
// wrapping, blanks skipped.
static void CycleTab(bool b, int dir) {
    if (TabDocCount(b) < 2) return;
    int cnt = (int)Tabs(b).size();
    for (int k = 1; k <= cnt; k++) {
        int i = ((Act(b) + dir * k) % cnt + cnt) % cnt;
        if ((i == Act(b) ? DV(b).n : Tabs(b)[i].n) > 0) { SwitchTab(b, i); return; }
    }
}

// DetachSessionExt + AdoptSessionExt: a session is self-contained - document,
// annotations, transforms, checkpoint, undo pair, render cache and view state
// all ride the value - so a move between panes is a move, nothing reloads.
// The tab you just dragged is the one you are looking at.
static void TabMoveToPane(bool from, int idx, bool to, int insertAt) {
    if (from == to) return;
    TabEnsure(from); TabEnsure(to);
    if (idx < 0 || idx >= (int)Tabs(from).size()) return;
    if (!(idx == Act(from) ? DV(from).n : Tabs(from)[idx].n)) return;
    bool wasLive = idx == Act(from);
    DocState s;
    std::vector<App::UndoSnap> us, rs;
    if (wasLive) {
        s = std::move(DV(from));
        us = std::move(g.undo); rs = std::move(g.redo);
    } else {
        s = std::move(Tabs(from)[idx]);
        us = std::move(g.tabUndo[from][idx]); rs = std::move(g.tabRedo[from][idx]);
    }
    Tabs(from).erase(Tabs(from).begin() + idx);
    g.tabUndo[from].erase(g.tabUndo[from].begin() + idx);
    g.tabRedo[from].erase(g.tabRedo[from].begin() + idx);
    if (wasLive) {
        if (Tabs(from).empty()) {
            Tabs(from).push_back(DocState{});
            g.tabUndo[from].push_back({});
            g.tabRedo[from].push_back({});
            Act(from) = 0;
            DV(from) = DocState{};
            g.undo.clear(); g.redo.clear();
        } else {
            int next = min(idx, (int)Tabs(from).size() - 1);
            DV(from) = std::move(Tabs(from)[next]);
            g.undo = std::move(g.tabUndo[from][next]);
            g.redo = std::move(g.tabRedo[from][next]);
            Act(from) = next;
            DV(from).tabTick = ++g_tabTick;
        }
    } else if (idx < Act(from)) Act(from)--;
    if (fd.open && fd.pane == from && wasLive) {  // the scan's document left this pane:
        if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }   // the find dies with it
        if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
        fd = Find{};
    }
    s.tabTick = ++g_tabTick;
    for (auto& sn : us) sn.paneB = to;            // the snapshots now belong to the other pane
    for (auto& sn : rs) sn.paneB = to;
    int at = std::clamp(insertAt, 0, (int)Tabs(to).size());
    Tabs(to).insert(Tabs(to).begin() + at, std::move(s));
    g.tabUndo[to].insert(g.tabUndo[to].begin() + at, std::move(us));
    g.tabRedo[to].insert(g.tabRedo[to].begin() + at, std::move(rs));
    if (at <= Act(to)) Act(to)++;
    TabTrimCaches(from); TabTrimCaches(to);
    SwitchTab(to, at);
    LayoutMain();                                 // both strips may have grown or collapsed
    TabChrome(from); TabChrome(to);
}

static void EnsureSideBack(HDC ref, int w, int h) {
    if (sd.bbm && sd.bw == w && sd.bh == h) return;
    if (sd.bbm) { SelectObject(sd.bdc, sd.bold); DeleteObject(sd.bbm); }
    else sd.bdc = CreateCompatibleDC(ref);
    sd.bbm = CreateCompatibleBitmap(ref, w, h);
    sd.bold = (HBITMAP)SelectObject(sd.bdc, sd.bbm);
    sd.bw = w; sd.bh = h;
}

// Overlay-bar geometry, ported from KillerPDF: the thumb keeps the viewport's
// proportional size but never shrinks below a 64px grab floor (ThumbViewportFloor,
// floored at half the track), and its position maps the scroll linearly over the
// free track. The track itself paints nothing - the bar is the only visible part.
static void BarThumb(int docLen, int viewLen, int scroll, int& tlen, int& tpos, int range = -1) {
    int R = range >= 0 ? range : docLen - viewLen;   // the scroll's real travel; the v-thumbs
    if (docLen <= viewLen || docLen <= 0 || viewLen <= 0 || R <= 0) { tlen = 0; tpos = 0; return; }
    int t = (int)((long long)viewLen * viewLen / docLen);
    int fl = (int)min(64.0 * g.dpi, viewLen * 0.5);
    tlen = std::clamp(t, fl, viewLen);
    tpos = std::clamp((int)((long long)(viewLen - tlen) * max(0, scroll) / R), 0, viewLen - tlen);
}

// The KillerPDF thumb: a slim rounded bar in the theme green, a third alpha at
// rest and solid under the cursor. GDI+ blends it straight onto the back buffer.
static void RoundRectFill(HDC dc, int x, int y, int w, int h, int r, bool hot) {
    if (w <= 0 || h <= 0) return;
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;
    r = max(1, min(r, min(w, h) / 2));
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.CloseFigure();
    Gdiplus::SolidBrush br(Gdiplus::Color(hot ? (BYTE)255 : (BYTE)0x55, (BYTE)0x22, (BYTE)0xBB, (BYTE)0x56));
    gs.FillPath(&br, &p);
}

static HFONT SideFont() {   // the page label: BrandSansFont ("Killer Sans") at FontSize 14
    if (!g_sidefont)
        g_sidefont = CreateFontW(-(int)(14 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Killer Sans");
    return g_sidefont;
}

static HBRUSH ThumbEdgeBrush()   { static HBRUSH b = CreateSolidBrush(RGB(0x33, 0x33, 0x33)); return b; }  // SurfaceBrush
static HBRUSH RowHoverBrush()    { static HBRUSH b = CreateSolidBrush(RGB(0x40, 0x40, 0x40)); return b; }  // RowHoverBrush

static void TrackLeave(HWND h) { TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t); }

// Paints the sidebar rail: the shared dark noisy field (the same surface the
// window margins and footer paint, so the rail belongs to no one component),
// KillerPDF's page items - a full-width band that lights RowHoverBrush under
// the cursor and the SelectionBg green gradient behind the current page, a
// "Page N" label in Killer Sans 14 - and the rail's own overlay bar inside the
// reserved 12px gutter on the right.
static void SideDraw(HDC dc, int w, int h, int xoff) {
    RECT r{ xoff, 0, xoff + w, h };
    FillRect(dc, &r, GrainBrush(1));
    DocState& D = DVF();                    // the sidebar paints the FOCUSED pane's pages
    if (!D.n || sd.cw <= 0 || sd.top.empty()) return;
    int avail = sd.cw - SBar();
    HGDIOBJ of = SelectObject(dc, SideFont());
    SetBkMode(dc, TRANSPARENT);
    HDC m = CreateCompatibleDC(dc);
    int padT = (int)(8 * g.dpi), labH = (int)(20 * g.dpi), padB = (int)(6 * g.dpi), gap = (int)(2 * g.dpi);
    int first = SidePageAt(sd.scroll);
    for (int i = first; i < D.n && sd.top[i] < sd.scroll + h; i++) {
        int th = STH(i);
        int x = xoff + (avail - sd.tw) / 2;
        int y = sd.top[i] - sd.scroll;
        int btop = y - padT, bbot = y + th + gap + labH + padB;   // the item band
        if (i == sd.cur || i == sd.hover) {
            RECT band{ xoff, max(0, btop), xoff + avail, min(h, bbot) };
            if (i == sd.cur) {                                    // SelectionBg, top #387552 -> bottom #104E2B
                Gdiplus::Graphics gs(dc);
                Gdiplus::LinearGradientBrush gb(Gdiplus::Rect(band.left, band.top - 1, band.right - band.left, band.bottom - band.top + 2),
                                                Gdiplus::Color(255, 0x38, 0x75, 0x52),
                                                Gdiplus::Color(255, 0x10, 0x4e, 0x2b), 90.0f);
                gs.FillRectangle(&gb, (int)band.left, (int)band.top,
                                 (int)(band.right - band.left), (int)(band.bottom - band.top));
            } else {
                FillRect(dc, &band, RowHoverBrush());
            }
        }
        RECT tr{ x, y, x + sd.tw, y + th };
        auto it = sd.th.find(i);
        if (it != sd.th.end() && it->second.b) {
            HGDIOBJ o = SelectObject(m, it->second.b);
            BitBlt(dc, x, y, sd.tw, th, m, 0, 0, SRCCOPY);
            SelectObject(m, o);
            FrameRect(dc, &tr, ThumbEdgeBrush());
        } else {
            FillRect(dc, &tr, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
            FrameRect(dc, &tr, ThumbEdgeBrush());
        }
        wchar_t num[24];
        swprintf_s(num, L"Page %d", i + 1);
        RECT nr{ xoff, y + th + gap, xoff + avail, y + th + gap + labH };
        SetTextColor(dc, i == sd.cur ? RGB(0xff, 0xff, 0xff) : RGB(0xa3, 0xa3, 0xa3));
        DrawTextW(dc, num, -1, &nr, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    DeleteDC(m);
    SelectObject(dc, of);
    if (sd.docH > sd.ch && sd.ch > 0) {
        int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp);
        bool hot = sd.barHot || sd.barDrag;
        int bw = hot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);
        RoundRectFill(dc, xoff + w - SBar() + (SBar() - bw) / 2, tp, bw, tl, (int)(3 * g.dpi), hot);
    }
}

static LRESULT CALLBACK SideProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    // WM_MOUSEACTIVATE keeps its default (MA_ACTIVATE): a click anywhere on the
    // rail must raise and focus the window. The old MA_NOACTIVATE was an
    // anti-flash band-aid from when the frame still carried WS_CAPTION
    // (activating made DWM repaint the native caption); with the style gone
    // there is nothing left to flash, and swallowing activation kept clicks on
    // the sidebar from bringing Docsy to the foreground.
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (sd.cw > 0 && sd.ch > 0) {
            EnsureSideBack(dc, sd.cw, sd.ch);
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // the grain tile phase:
            SetBrushOrgEx(sd.bdc, -po.x, -po.y, nullptr);           // anchored to the window, like
            if (g_webOn) WebSideDraw(sd.bdc, sd.cw, sd.ch);         // browser mode: the rail is
            else SideDraw(sd.bdc, sd.cw, sd.ch, 0);                 // the web tabs' thumbnails
            BitBlt(dc, 0, 0, sd.cw, sd.ch, sd.bdc, 0, 0, SRCCOPY);  // shared field runs unbroken
            SetTimer(h, 2, 25, nullptr);   // finish any pending thumbs
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 2 && (g_webOn ? !WebSidePump() : !SidePump())) KillTimer(h, 2);
        return 0;
    case WM_SIZE:                        // the cards re-flow to the width: a full
        InvalidateRect(h, nullptr, FALSE);   // repaint leaves no stale column behind
        return 0;
    case WM_MOUSEWHEEL:
        if (g_webOn) { WebSideWheel(-GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120); return 0; }
        SideScrollTo(sd.scroll - GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_webOn) { WebSideDown(p); return 0; }
        SetCapture(h);
        int gb = SBar();
        if (DVF().n && sd.docH > sd.ch && p.x >= sd.cw - gb && p.x < sd.cw) {
            int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp);
            if (p.y >= tp && p.y < tp + tl) { sd.barDrag = true; sd.barGrab = p.y - tp; }
            else SideScrollTo(sd.scroll + (p.y < tp ? -(int)(sd.ch * 0.9) : (int)(sd.ch * 0.9)));
            return 0;
        }
        sd.dp = p;
        sd.hit = SidePageAt(sd.dp.y + sd.scroll);
        sd.moved = 0;
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_webOn) { WebSideMove(h, p); return 0; }
        if (sd.barDrag) {
            int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp);
            SideScrollTo((int)((long long)(p.y - sd.barGrab) * max(0, sd.docH - sd.ch) / max(1, sd.ch - tl)));
            return 0;
        }
        bool hot = DVF().n && sd.docH > sd.ch && p.x >= sd.cw - SBar() && p.x < sd.cw;
        if (hot) { int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp); hot = p.y >= tp && p.y < tp + tl; }
        int hov = (!sd.barDrag && DVF().n && sd.docH > 0 && p.x < sd.cw - SBar()
                   && p.y + sd.scroll < sd.docH) ? SidePageAt(p.y + sd.scroll) : -1;
        if (hov != sd.hover) {
            sd.hover = hov;
            if (hov >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (hot != sd.barHot) {
            sd.barHot = hot;
            if (hot) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (GetCapture() == h) {
            int dy = p.y - sd.dp.y;
            sd.dp = p;
            if (dy) sd.moved += abs(dy);   // a drag only cancels the click: like the page
        }                                  // view, the wheel and the thumb own scrolling
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_webOn) { WebSideLeave(); return 0; }
        if (sd.barHot) { sd.barHot = false; InvalidateRect(h, nullptr, FALSE); }
        if (sd.hover != -1) { sd.hover = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_webOn) { WebSideUp(h, p); return 0; }
        bool bar = sd.barDrag;
        sd.barDrag = false;
        if (GetCapture() == h) ReleaseCapture();
        if (!bar && sd.moved <= (int)(4 * g.dpi) && sd.hit >= 0 && sd.hit < DVF().n) {
            sd.hold = GetTickCount64() + 600;   // the jump must not scroll the sidebar back
            if (DVF().vm == 1) SetPage(sd.hit);     // single mode: a thumb click flips to that page
            else ScrollTo(DVF().sx, max(0, PageTop(DVF(), sd.hit) - Gap() / 2));
        }
        sd.moved = 0; sd.hit = -1;
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (g_webOn) {                 // the web rail's cards take the hand: a rail card
                                           // switches tabs, the same affordance the page
                                           // thumbs get when the reader rides the window
                bool over = p.x < sd.cw - SBar() && ws.docH > 0 && WebSideAt(p.y + ws.scroll) >= 0;
                SetCursor(LoadCursorW(nullptr, over ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            bool edge = g.sideOn && DVF().n && sd.cw > 0 && p.x >= sd.cw - 1;
            SetCursor(LoadCursorW(nullptr, edge ? IDC_SIZEWE : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DROPFILES: HandleDrop((HDROP)wp, h); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

