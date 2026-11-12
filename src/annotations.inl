// ---------------- annotation tools (the toolbar's tool buttons) ----------------
// KillerPDF's editing tools, in docsy's pane language. The Text tool commits
// through a real child EDIT (the Find query's WS_CLIPCHILDREN contract, at
// KillerPDF's 165pt box and 24pt Segoe UI); the Draw / Line / Shapes tools
// drag in page-DIP space and commit on release with KillerPDF's own discard
// rules (a line needs >3pt of length, a shape >3pt of both dimensions, a
// one-point Draw commits as a round dot); the Highlight tool reuses the
// flowing selection and converts the swept lines on release
// (CommitFlowingHighlight). Each pane keeps its own tool, exactly like
// KillerPDF keeps the editing state per viewer instance.

// The text box commits on focus loss, on any view motion, or on Ctrl+Enter -
// the text lands as an AT_TEXT annotation carrying the bar's full style
// (KillerPDF's BuildCommittedTextAnnotation); empty content discards, exactly
// like KillerPDF. Del inside the box deletes the WHOLE textbox - input and
// frame, nothing left, whatever the selection is (the user's contract; the
// character edits stay with Backspace). Escape backs out: a re-edited box is
// RESTORED, KillerPDF's CancelActiveTextEdit, so backing out never deletes.
static Annot g_teRestore;          // the re-edit's original, parked while the box is
static int g_teRestorePage = -1, g_teRestoreIdx = -1;   // open (CancelActiveTextEdit's
static bool g_teRestoreB = false;  // _reeditOriginal - Escape puts it back, Del drops it)
static void CommitTextEdit(bool commit, bool restore) {
    if (!g.tedit) return;                       // the default argument rides the forward
    HWND e = g.tedit;
    int len = GetWindowTextLengthW(e);
    std::wstring s;
    if (commit && len > 0) {
        s.resize(len);
        GetWindowTextW(e, s.data(), len + 1);
        if (s.find_first_not_of(L" \t\r\n") == std::wstring::npos) s.clear();
    }
    int fpg = g.tePage; bool fpaneB = g.tePaneB;
    g.tedit = nullptr;
    if (g.teBg) { DeleteObject(g.teBg); g.teBg = nullptr; }
    if (GetFocus() == e) SetFocus(g.main);   // the keys go home - parking focus on the view
                                             // starved the main window's shortcuts (Del on
                                             // the selected box included) after every commit
    DestroyWindow(e);
    if (!s.empty()) {
        DocState& D = DV(fpaneB);
        {
            UndoPush(fpaneB);
            Annot a;
            a.type = AT_TEXT;
            a.x = g.teX; a.y = g.teY; a.w = g.teW;
            a.fsize = g.textSize; a.color = g.textRGB; a.alpha = g.textA; a.text = s;
            a.font = g.textFont;
            a.bold = g.textBold; a.italic = g.textItalic;
            a.strike = g.textStrike; a.under = g.textUnder;
            a.spacing = g.textSpacing;
            a.fill = g.textFillRGB; a.fillA = g.textFillA;
            // the box's height, measured at the CURRENT scale (the wrapped line
            // count is what the box showed while editing), then clamped on-page
            float k = (float)(D.zoom * g.dpi);
            HDC m = GetDC(g.view);
            int hpx = max(8, (int)std::lround(a.fsize * k));
            HFONT f = CreateFontW(-hpx, 0, 0, 0, a.bold ? FW_BOLD : FW_NORMAL,
                                  a.italic, a.under, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, a.font.c_str());
            HGDIOBJ of = SelectObject(m, f);
            RECT mr{ 0, 0, (int)(g.teW * k), 10000 };
            DrawTextW(m, s.c_str(), -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            SelectObject(m, of); DeleteObject(f); ReleaseDC(g.view, m);
            a.h = max(24.0f, (mr.bottom - mr.top) / max(0.01f, k));
            a.x = std::clamp(a.x, 0.0f, max(0.0f, (D.n && fpg < (int)D.pw.size() ? D.pw[fpg] : 0) - a.w));
            a.y = std::clamp(a.y, 0.0f, max(0.0f, (D.n && fpg < (int)D.ph.size() ? D.ph[fpg] : 0) - a.h));
            D.annos[fpg].push_back(a);
        }
    }
    else if (restore && g_teRestoreIdx >= 0) {
        // KillerPDF's CancelActiveTextEdit: backing out of a re-edit puts the
        // original annotation back where it sat - Escape is a cancel, never
        // a delete; the snapshot UndoPush took at open stays the undo pair,
        // so no extra entry rides here
        DocState& D = DV(g_teRestoreB);
        std::vector<Annot>& list = D.annos[g_teRestorePage];
        list.insert(list.begin() + std::clamp(g_teRestoreIdx, 0, (int)list.size()), g_teRestore);
    }
    g_teRestoreIdx = -1;   // the stash is spent whichever way the box closed
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// The box's height contract: KillerPDF's TextBox starts one line tall
// (MinHeight 24) and auto-grows as lines wrap, until a corner handle drag
// makes the height free-form. The edit reports its wrapped line count through
// EM_GETLINECOUNT; the fit moves the child and repaints the composite's ring
// band (old ∪ new footprint, the FindPlace contract).
static int g_teLineH = 0;      // one line's px at the box's font
static int g_teMinH = 0;       // KillerPDF's MinHeight 24, scaled
static bool g_teFixedH = false;   // a handle drag made the height free-form
static void TextEditFit() {
    if (!g.tedit || g_teFixedH || g_teLineH <= 0) return;
    int lines = (int)SendMessageW(g.tedit, EM_GETLINECOUNT, 0, 0);
    int wantH = max(g_teMinH, lines * g_teLineH + (int)(4 * g.dpi));
    RECT r; GetWindowRect(g.tedit, &r);
    POINT p0{ r.left, r.top }, p1{ r.right, r.bottom };
    ScreenToClient(g.view, &p0); ScreenToClient(g.view, &p1);
    int curH = p1.y - p0.y;
    if (wantH == curH) return;
    RECT u{ p0.x - 8, p0.y - 8, p1.x + 8, max(p0.y + wantH, p1.y) + 8 };
    MoveWindow(g.tedit, p0.x, p0.y, p1.x - p0.x, wantH, TRUE);
    InvalidateRect(g.view, &u, FALSE);   // the ring + handles ride the composite
}

// The Text tool's box resize: dragging an editing handle reshapes the box
// (opposite corner pinned, KillerPDF's min 40x24); the commit takes the box
// the user left on screen.
static bool g_teResize = false;
static int g_teHandle = 0;
static POINT g_teAnchor{}; static RECT g_teStart{};
static COLORREF g_teTone = RGB(255, 255, 255);   // the editor's sampled tone (the CTLCOLOR brush)

// The live-resize frame paint. The stock EDIT repaints in two visible steps -
// WM_ERASEBKGND floods the client with the tone, WM_PAINT then draws the text
// over it - so at drag speed the text strobes (erase, text, erase, text...),
// and the caret churns on every re-wrap: the "flashing and freaking out" the
// stock control shows while a corner handle drags. KillerPDF's WPF TextBox has
// no such phases (retained composition, one frame per size), so while the
// drag is live this paints the frame itself - the tone fill and the wrapped
// text in ONE pass with the edit's own font and margins - and the caret stays
// hidden until the drag hands the box back. The text re-wraps live at the new
// width, exactly the reference's behavior; on release the real edit repaints
// and the caret returns (KillerPDF's _tehBox?.Focus()).
static void TextEditPaintFrame(HWND h, HDC dc) {
    RECT rc; GetClientRect(h, &rc);
    if (g.teBg) FillRect(dc, &rc, g.teBg);
    else { SetBkColor(dc, g_teTone); ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr); }
    int len = GetWindowTextLengthW(h);
    std::wstring s;
    if (len > 0) { s.resize(len); GetWindowTextW(h, s.data(), len + 1); }
    if (s.empty()) return;
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    HGDIOBJ of = f ? SelectObject(dc, f) : nullptr;
    SetBkColor(dc, g_teTone);
    SetTextColor(dc, g.textRGB);
    DWORD mar = (DWORD)SendMessageW(h, EM_GETMARGINS, 0, 0);
    RECT tr{ rc.left + (int)(short)LOWORD(mar), rc.top + 1,
             rc.right - (int)(short)HIWORD(mar), rc.bottom };
    DrawTextW(dc, s.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    if (f) SelectObject(dc, of);
}

static LRESULT CALLBACK TextEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (g_teResize && m == WM_ERASEBKGND) return 1;   // no erase phase mid-drag
    if (g_teResize && m == WM_PAINT) {                // one atomic frame paint
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        TextEditPaintFrame(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { CommitTextEdit(false, true); return 0; }   // backs out - a
                                     // re-edited box returns (CancelActiveTextEdit)
        if (wp == VK_DELETE) {      // Del deletes the WHOLE textbox - input and frame,
            CommitTextEdit(false);  // nothing left, whatever the selection is; letters
            return 0;               // are Backspace's business
        }
        if (wp == VK_RETURN && (GetKeyState(VK_CONTROL) < 0)) { CommitTextEdit(); return 0; }
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(h, EM_SETSEL, 0, -1); return 0;   // WPF TextBox's Ctrl+A SelectAll - a
        }                                                  // plain EDIT ships no such chord
        if (wp == 'B' && GetKeyState(VK_CONTROL) < 0) {
            g.textBold = !g.textBold;
            ApplyTextStyleToSelection();     // the live box restyles with the shared state
            return 0;
        }
        if (wp == 'I' && GetKeyState(VK_CONTROL) < 0) { g.textItalic = !g.textItalic; ApplyTextStyleToSelection(); return 0; }
        if (wp == 'U' && GetKeyState(VK_CONTROL) < 0) { g.textUnder = !g.textUnder; ApplyTextStyleToSelection(); return 0; }
    }
    return CallWindowProcW(g.teditDef, h, m, wp, lp);
}

// The Text tool's press: re-edit a placed text annotation under the click
// (it leaves the list and comes back when the box closes - and the bar SYNCs
// to the box's own look, KillerPDF's EditTextAtPosition), or open a fresh box
// centered on the hotspot (KillerPDF's PlaceTextBox: left edge on the click,
// first line's center on the I-beam).
static void TextEditOpen(bool b, int pg, float tx, float ty, float tw, const wchar_t* text);
static void TextPlace(bool b, POINT p) {
    DocState& D = DV(b);
    POINT q{ p.x - PaneX0(b) - CardX(b), p.y };
    int pg; float fx, fy;
    if (!ViewToPageAt(D, q, CardW(b), g.ch, D.sx, D.sy, pg, fx, fy)) return;
    auto it = D.annos.find(pg);
    float k = (float)(D.zoom * g.dpi);
    if (it != D.annos.end()) {
        for (int ai = (int)it->second.size() - 1; ai >= 0; ai--) {
            Annot& a = it->second[ai];
            if (a.type != AT_TEXT) continue;
            if (fx >= a.x - 8 && fx <= a.x + a.w + 8 && fy >= a.y - 10 && fy <= a.y + a.h + 10) {
                UndoPush(b);                 // the re-edit's commit replaces the original
                std::wstring text = a.text;
                float tx = a.x, ty = a.y, tw = a.w;
                // the bar takes the box's look as its new defaults (KillerPDF's sync)
                g.textFont = a.font; g.textSize = a.fsize; g.textSpacing = a.spacing;
                g.textBold = a.bold; g.textItalic = a.italic;
                g.textStrike = a.strike; g.textUnder = a.under;
                g.textRGB = a.color; g.textA = a.alpha;
                g.textFillRGB = a.fill; g.textFillA = a.fillA;
                g_teRestore = a;           // park the original: Escape puts it back,
                g_teRestorePage = pg;      // Del drops it for good (CommitTextEdit's
                g_teRestoreIdx = ai; g_teRestoreB = b;   // restore switch)
                it->second.erase(it->second.begin() + ai);
                TextEditOpen(b, pg, tx, ty, tw, text.c_str());   // defined below
                return;
            }
        }
    }
    // a fresh box: 165pt wide, the first line's center on the I-beam hotspot
    int lineH = (int)std::lround(g.textSize * k * 4.0 / 3.0);
    float half = (lineH / 2.0f) / max(0.01f, k);
    TextEditOpen(b, pg, fx, max(0.0f, fy - half), 165.0f, nullptr);
}

// The editor's background: KillerPDF's TextEditBackground - the box's own
// fill when one is set, else the page tone under a 64-alpha gray (so the
// empty box reads on light and dark pages). An EDIT is opaque, so the page
// pixels under the box are blitted into a tiny DIB and averaged, and the
// gray is blended into that average.
static HBRUSH TextEditBackground(DocState& D, int pg, int x, int y, int w, int h) {
    if (g.textFillA > 0) { g_teTone = g.textFillRGB; return CreateSolidBrush(g.textFillRGB); }
    COLORREF tone = RGB(255, 255, 255);
    HBITMAP b = GetBmp(D, pg, PW(D, pg), PH(D, pg));
    w = std::clamp(w, 1, 400); h = std::clamp(h, 1, 400);
    if (b && w > 0 && h > 0) {
        HDC sdc = CreateCompatibleDC(nullptr);
        HGDIOBJ oldb = SelectObject(sdc, b);
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        BYTE* bits = nullptr;
        HBITMAP tile = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (tile) {
            HDC tdc = CreateCompatibleDC(nullptr);
            HGDIOBJ ot = SelectObject(tdc, tile);
            BitBlt(tdc, 0, 0, w, h, sdc, x, y, SRCCOPY);
            SelectObject(tdc, ot); DeleteDC(tdc);
            long long r = 0, gg = 0, bl = 0; long long n = 0;
            for (int yy = 0; yy < h; yy += 2) {
                const BYTE* row = bits + (size_t)yy * ((w * 4 + 3) & ~3);
                for (int xx = 0; xx < w; xx += max(1, w / 24)) {
                    const BYTE* p2 = row + xx * 4;
                    bl += p2[0]; gg += p2[1]; r += p2[2]; n++;
                }
            }
            if (n > 0) tone = RGB((int)(r / n * 3 / 4 + 32), (int)(gg / n * 3 / 4 + 32),
                                  (int)(bl / n * 3 / 4 + 32));   // 25% of the 128-gray over the page
            DeleteObject(tile);
        }
        SelectObject(sdc, oldb); DeleteDC(sdc);
    }
    g_teTone = tone;
    return CreateSolidBrush(tone);
}

// Opens the box: a real child EDIT over its page position, one green ring +
// four corner handles painted by the composite around it (KillerPDF's
// PrimaryBrush border and resize handles).
static void TextEditOpen(bool b, int pg, float tx, float ty, float tw, const wchar_t* text) {
    CommitTextEdit();                       // one editor at a time (KillerPDF's _activeTextBox)
    DocState& D = DV(b);
    if (!D.n || pg < 0 || pg >= D.n) return;
    g.tePaneB = b; g.tePage = pg; g.teX = tx; g.teY = ty; g.teW = tw;
    float k = (float)(D.zoom * g.dpi);
    int ox, oy;
    if (!PageOrigin(D, b, pg, ox, oy)) return;
    int fontPx = max(8, (int)std::lround(g.textSize * k));
    int lineH = (int)std::lround(fontPx * 4.0 / 3.0);
    int boxW = max(24, (int)std::lround(tw * k));
    int x = PaneX0(b) + ox + (int)std::lround(tx * k);
    int y = oy + (int)std::lround(ty * k);
    // KillerPDF's box: MinHeight 24, ONE line tall at open, auto-grown by
    // TextEditFit as lines wrap - until a corner handle makes the height
    // free-form. ES_AUTOVSCROLL stays as the overflow safety net.
    g_teLineH = lineH;
    g_teMinH = max((int)(24 * g.dpi), lineH);
    g_teFixedH = false;
    int boxH = g_teMinH;
    // clamp into the pane's card so the editor never hides under the chrome
    int cardL = PaneX0(b) + CardX(b), cardR = cardL + CardW(b);
    x = std::clamp(x, cardL, max(cardL, cardR - boxW));
    y = std::clamp(y, 0, max(0, g.ch - boxH));
    HFONT f = CreateFontW(-fontPx, 0, 0, 0, g.textBold ? FW_BOLD : FW_NORMAL,
                          g.textItalic, g.textUnder, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, g.textFont.c_str());
    g.tedit = CreateWindowExW(0, L"EDIT", text ? text : L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                              x, y, boxW, boxH, g.view, nullptr, g.hinst, nullptr);
    if (!g.tedit) return;
    if (g.teBg) DeleteObject(g.teBg);
    g.teBg = TextEditBackground(D, pg, x - PaneX0(b) - ox, y - oy, boxW, boxH);
    SendMessageW(g.tedit, WM_SETFONT, (WPARAM)f, TRUE);   // the font slot rides the edit
    g.teditDef = (WNDPROC)SetWindowLongPtrW(g.tedit, GWLP_WNDPROC, (LONG_PTR)TextEditProc);
    SetFocus(g.tedit);
    SendMessageW(g.tedit, EM_SETSEL, 0, -1);   // KillerPDF opens every box select-all
                                               // (QueueTextEditorFocus selectAll: true) -
                                               // typing over it replaces the text at once
    SetStatusHold(L"Editing text. Change size/color above, Enter to save");
    InvalidateRect(g.view, nullptr, FALSE);   // the composite paints the ring the edit's
                                              // punch-out exposes (WS_CLIPCHILDREN owns
                                              // the box's pixels from here)
}

// A stroke's invalidation: the gesture's footprint so far, in view pixels -
// the drag path never recomposites anything beyond its own ink (call it
// before and after a mutation; the invalid regions accumulate into one paint).
static void AnnInval(HWND h) {
    DocState& D = DV(g.annPaneB);
    int ox, oy;
    if (!PageOrigin(D, g.annPaneB, g.annPage, ox, oy)) { InvalidateRect(h, nullptr, FALSE); return; }
    float k = (float)(D.zoom * g.dpi);
    float x0, y0, x1, y1;
    Annot a;                                  // bounds of the live points
    a.type = AT_INK; a.width = 3;
    a.ix = g.annIX; a.iy = g.annIY;
    AnnotBounds(a, x0, y0, x1, y1);
    RECT r{ PaneX0(g.annPaneB) + ox + (int)floorf(x0 * k) - 3, oy + (int)floorf(y0 * k) - 3,
            PaneX0(g.annPaneB) + ox + (int)ceilf(x1 * k) + 4, oy + (int)ceilf(y1 * k) + 4 };
    InvalidateRect(h, &r, FALSE);
}

// ---------------- undo / redo (KillerPDF's stack) ----------------
// One snapshot per edit operation, bounded to 20 entries (UndoHistoryBudget's
// maximumEntries); redo clears on a fresh edit. A snapshot is the pane's whole
// annotation state - pages carry a handful of small vectors, so
// the copy is trivial next to a page render.
static App::UndoSnap SnapPane(bool b) {
    App::UndoSnap s; s.paneB = b;
    DocState& D = DV(b);
    for (auto& kv : D.annos) if (!kv.second.empty()) s.annos[kv.first] = kv.second;
    return s;
}
static void UndoPush(bool b) {
    g.undo.push_back(SnapPane(b));
    if (g.undo.size() > 20) g.undo.erase(g.undo.begin());
    g.redo.clear();                        // fresh edits invalidate the redo chain
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // Undo/Redo re-arm
}
static void UndoOp() {
    if (g.tedit) CommitTextEdit();
    if (g.undo.empty()) { SetStatusHold(L"Nothing to undo"); return; }
    App::UndoSnap e = g.undo.back();
    g.undo.pop_back();
    g.redo.push_back(SnapPane(e.paneB));
    if (g.redo.size() > 20) g.redo.erase(g.redo.begin());
    DocState& D = DV(e.paneB);
    D.annos = e.annos;
    g.annSelPage = -1; g.annSelIdx = -1;
    SetStatusHold(L"Undone");
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
}
static void RedoOp() {
    if (g.tedit) CommitTextEdit();
    if (g.redo.empty()) { SetStatusHold(L"Nothing to redo"); return; }
    App::UndoSnap e = g.redo.back();
    g.redo.pop_back();
    g.undo.push_back(SnapPane(e.paneB));
    if (g.undo.size() > 20) g.undo.erase(g.undo.begin());
    DocState& D = DV(e.paneB);
    D.annos = e.annos;
    g.annSelPage = -1; g.annSelIdx = -1;
    SetStatusHold(L"Redone");
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
}

// The Clear All button (ClearAllAnnotations_Click): a full snapshot lands on
// the undo stack, then every annotation leaves - marks and gestures untouched.
static void ClearAllAnnos() {
    DocState& D = DVF();
    if (!D.n) return;
    if (g.tedit) CommitTextEdit();
    int total = 0;
    for (auto& kv : D.annos) total += (int)kv.second.size();
    if (!total && !g.annInk && !g.annPoly) { SetStatusHold(L"No annotations to clear"); return; }
    UndoPush(FocusB());
    D.annos.clear();
    PolyCancel(g.view);
    g.annSelPage = -1; g.annSelIdx = -1;
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
    SetStatusHold(L"Cleared all annotations (" + std::to_wstring(total) + L")");
}

// ---- the Shapes tool's polygon session (ShapeKind.Polygon) ----
static void PolyCancel(HWND h) {
    if (!g.annPoly) return;
    g.annPoly = false;
    g.annPX.clear(); g.annPY.clear();
    if (h) InvalidateRect(h, nullptr, FALSE);
}
static void PolyCommit(HWND h) {
    if (!g.annPoly) return;
    DocState& D = DV(g.annPaneB);
    g.annPoly = false;
    if (g.annPX.size() >= 3) {
        UndoPush(g.annPaneB);
        Annot a;
        a.type = AT_INK; a.width = g.drawWidth; a.color = g.drawColor; a.alpha = g.drawOpacity;
        a.ix = g.annPX; a.iy = g.annPY;
        a.ix.push_back(g.annPX[0]); a.iy.push_back(g.annPY[0]);   // closed: first point repeats
        if (g.shapeFill) { a.fill = g.drawColor; a.fillA = (BYTE)max(20, g.drawOpacity / 2); }
        D.annos[g.annPage].push_back(a);
    }
    g.annPX.clear(); g.annPY.clear();
    if (h) InvalidateRect(h, nullptr, FALSE);
}

// ---- the erasers ----
// Rectangle subtraction, KillerPDF's SubtractRect: the parts of a NOT covered
// by b, as up to four strips; [a] when they miss, empty when b swallows a.
static void SubtractRect(const FRect& a, const FRect& bx, std::vector<FRect>& out) {
    float l = max(a.x, bx.x), r2 = min(a.x + a.w, bx.x + bx.w);
    float t = max(a.y, bx.y), b2 = min(a.y + a.h, bx.y + bx.h);
    if (r2 <= l || b2 <= t) { out.push_back(a); return; }
    if (t > a.y) out.push_back({ a.x, a.y, a.w, t - a.y });
    if (b2 < a.y + a.h) out.push_back({ a.x, b2, a.w, a.y + a.h - b2 });
    if (l > a.x) out.push_back({ a.x, t, l - a.x, b2 - t });
    if (r2 < a.x + a.w) out.push_back({ r2, t, a.x + a.w - r2, b2 - t });
}

// The Draw bar's brush eraser (KillerPDF's ErasePartial with a brush path):
// ink strokes SPLIT at the covered points (pieces keep their look, runs under
// two points drop), and highlights CARVE - the brush's circular footprint,
// widened around every gesture sample, is subtracted from the covered rects
// (HighlightErase's own smooth-hole mechanic, blocky at rect granularity).
// rad is the brush radius in page points.
static void EraseBrushAt(DocState& D, int page, float x, float y, float rad) {
    auto it = D.annos.find(page);
    if (it == D.annos.end()) return;
    bool changed = false;
    float r2 = rad * rad;
    std::vector<Annot> out;
    out.reserve(it->second.size());
    for (Annot& a : it->second) {
        if (a.type == AT_INK) {
            std::vector<float> rx, ry;
            auto flush = [&]() {
                if (rx.size() >= 2) { Annot n = a; n.ix = rx; n.iy = ry; out.push_back(n); }
                else if (!rx.empty()) changed = true;   // a lone survivor point drops
                rx.clear(); ry.clear();
            };
            for (size_t i = 0; i < a.ix.size(); i++) {
                float dx = a.ix[i] - x, dy = a.iy[i] - y;
                if (dx * dx + dy * dy <= r2) { changed = true; flush(); }
                else { rx.push_back(a.ix[i]); ry.push_back(a.iy[i]); }
            }
            flush();
        } else if (a.type == AT_HIGHLIGHT) {
            bool touch = false;
            for (const FRect& r : a.lines)
                if (x >= r.x - rad && x <= r.x + r.w + rad && y >= r.y - rad && y <= r.y + r.h + rad) { touch = true; break; }
            if (!touch) { out.push_back(a); continue; }
            changed = true;
            std::vector<FRect> pieces;
            for (const FRect& r : a.lines) {
                std::vector<FRect> cur{ r };
                // subtract this gesture's own footprint box first (the sample
                // that just landed), then the recent samples the stroke
                // passed - older ones were already carved out of this rect by
                // their own moves, so a bounded window keeps every pass O(1)
                std::vector<FRect> boxes;
                boxes.push_back({ x - rad, y - rad, rad * 2, rad * 2 });
                size_t n = g.annIX.size(), lo = n > 65 ? n - 65 : 0;
                for (size_t s2 = lo; s2 + 1 < n; s2++) {
                    FRect bx{ g.annIX[s2] - rad, g.annIY[s2] - rad, rad * 2, rad * 2 };
                    if (bx.x < r.x + r.w && bx.x + bx.w > r.x &&
                        bx.y < r.y + r.h && bx.y + bx.h > r.y) boxes.push_back(bx);
                }
                for (const FRect& bx : boxes) {
                    std::vector<FRect> next;
                    for (const FRect& c : cur) SubtractRect(c, bx, next);
                    cur = std::move(next);
                    if (cur.empty()) break;
                }
                for (const FRect& pc : cur)
                    if (pc.w >= 1 && pc.h >= 1) pieces.push_back(pc);
            }
            if (!pieces.empty()) {
                Annot n = a; n.lines = pieces;
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const FRect& r : pieces) {
                    x0 = min(x0, r.x); y0 = min(y0, r.y);
                    x1 = max(x1, r.x + r.w); y1 = max(y1, r.y + r.h);
                }
                n.x = x0; n.y = y0; n.w = x1 - x0; n.h = y1 - y0;
                out.push_back(n);
            }
        } else out.push_back(a);
    }
    if (changed) {
        it->second = std::move(out);
        if (it->second.empty()) D.annos.erase(it);
    }
}

// The Highlight bar's Annotation eraser (the drag box): highlights subtract
// the box (up to four strips per rect, pieces under 1x1 dropped) and ink
// strokes SPLIT at the covered points - KillerPDF's ErasePartial hands the
// region to both vector families.
static void EraseBox(HWND h, DocState& D, int page, FRect box) {
    auto it = D.annos.find(page);
    if (it == D.annos.end()) return;
    bool changed = false;
    std::vector<Annot> out;
    for (const Annot& a : it->second) {
        if (a.type == AT_HIGHLIGHT) {
            std::vector<FRect> pieces;
            for (const FRect& r : a.lines) SubtractRect(r, box, pieces);
            pieces.erase(std::remove_if(pieces.begin(), pieces.end(),
                         [](const FRect& q) { return q.w < 1 || q.h < 1; }), pieces.end());
            if (pieces.size() != a.lines.size()) changed = true;
            if (!pieces.empty()) {
                Annot n = a; n.lines = pieces;
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const FRect& r : pieces) {
                    x0 = min(x0, r.x); y0 = min(y0, r.y);
                    x1 = max(x1, r.x + r.w); y1 = max(y1, r.y + r.h);
                }
                n.x = x0; n.y = y0; n.w = x1 - x0; n.h = y1 - y0;
                out.push_back(n);
            }
        } else if (a.type == AT_INK) {
            std::vector<float> rx, ry;
            auto flush = [&]() {
                if (rx.size() >= 2) { Annot n = a; n.ix = rx; n.iy = ry; out.push_back(n); }
                else if (!rx.empty()) changed = true;   // a lone survivor point drops
                rx.clear(); ry.clear();
            };
            for (size_t i = 0; i < a.ix.size(); i++) {
                bool in = a.ix[i] >= box.x && a.ix[i] <= box.x + box.w &&
                          a.iy[i] >= box.y && a.iy[i] <= box.y + box.h;
                if (in) { changed = true; flush(); }
                else { rx.push_back(a.ix[i]); ry.push_back(a.iy[i]); }
            }
            flush();
        } else out.push_back(a);
    }
    if (changed) {
        it->second = std::move(out);
        if (it->second.empty()) D.annos.erase(it);
        InvalidateRect(h, nullptr, FALSE);
    }
}

// A selection-resize's new geometry, per type (KillerPDF's resize path: the
// opposite corner anchors, min 4 units; ink scales its points, a multi-line
// highlight scales its quads inside the new union, boxes take the new rect).
static void ResizeAnnotTo(Annot& a, float nx, float ny, float nw, float nh) {
    float bx0, by0, bx1, by1;
    AnnotBounds(a, bx0, by0, bx1, by1);
    float ow = max(1.0f, bx1 - bx0), oh = max(1.0f, by1 - by0);
    if (a.type == AT_HIGHLIGHT) {
        for (FRect& r : a.lines) {
            r.x = nx + (r.x - bx0) * nw / ow;
            r.y = ny + (r.y - by0) * nh / oh;
            r.w = r.w * nw / ow; r.h = r.h * nh / oh;
        }
        a.x = nx; a.y = ny; a.w = nw; a.h = nh;
    } else if (a.type == AT_INK) {
        for (size_t i = 0; i < a.ix.size(); i++) {
            a.ix[i] = nx + (a.ix[i] - bx0) * nw / ow;
            a.iy[i] = ny + (a.iy[i] - by0) * nh / oh;
        }
    } else { a.x = nx; a.y = ny; a.w = nw; a.h = nh; }
}

// The press under a drawing tool. Returns true when the tool consumed it.
static bool ToolDown(HWND h, bool b, POINT p) {
    DocState& D = DV(b);
    if (!D.n) return false;
    int tool = D.tool;
    if (tool == TOOL_SELECT) return false;    // the classic selection path owns Select
    POINT q{ p.x - PaneX0(b) - CardX(b), p.y };
    int pg; float fx, fy;
    bool onPage = ViewToPageAt(D, q, CardW(b), g.ch, D.sx, D.sy, pg, fx, fy);
    if (tool == TOOL_HIGHLIGHT) {
        // KillerPDF #127 Phase 2: on text the highlight FLOWS along the runs
        // exactly like text selection; the release turns the lines into
        // per-line annotations. The bar's Annotation eraser swaps the gesture
        // for a carve box. A press with no text under it hints instead of
        // silently laying down a rect - KillerPDF's own status line.
        if (g.hlErase) {
            if (!onPage) return true;
            g.annInk = true; g.annKind = -1; g.annArm = false;
            g.annPaneB = b; g.annPage = pg; g.annSX = D.sx; g.annSY = D.sy;
            g.annStart = g.annLast = p; g.annFX = fx; g.annFY = fy;
            g.annIX.assign(2, fx); g.annIY.assign(2, fy);
            return true;
        }
        TxtPage* pt = onPage ? TtFor(D, pg) : nullptr;
        if (pt && TtOverText(*pt, fx, fy)) {
            SelClear(D);
            g.selCommit = TOOL_HIGHLIGHT;
            D.selArm = true; D.selDrag = false;
            D.selAPage = D.selFPage = pg;
            D.selACaret = D.selFCaret = TtCaret(*pt, fx, fy);
            D.selDownPos = p;
        } else {
            if (D.selRange || D.selArm) SelClear(D);
            SetStatusHold(L"No text here. Shapes is on O");
        }
        return true;
    }
    if (tool == TOOL_SHAPE && g.annPoly && g.annPaneB == b && onPage) {
        // the polygon session: a click near the first vertex closes (snap 8),
        // any other click drops a vertex
        float dx0 = fx - g.annPX[0], dy0 = fy - g.annPY[0];
        if (g.annPX.size() >= 3 && dx0 * dx0 + dy0 * dy0 <= 64.0f) { PolyCommit(h); return true; }
        g.annPX.push_back(fx); g.annPY.push_back(fy);
        g.annFX = fx; g.annFY = fy; g.annLast = p;
        InvalidateRect(h, nullptr, FALSE);
        return true;
    }
    if (!onPage) return true;                 // a drawing press off the page does nothing
    if (tool == TOOL_TEXT) { TextPlace(b, p); return true; }
    if (tool == TOOL_DRAW && g.drawErase) {
        g.annInk = true; g.annKind = -2; g.annArm = true;   // annArm: undo not pushed yet
        g.annPaneB = b; g.annPage = pg; g.annSX = D.sx; g.annSY = D.sy;
        g.annStart = g.annLast = p; g.annFX = fx; g.annFY = fy;
        g.annIX.clear(); g.annIY.clear();
        g.annIX.push_back(fx); g.annIY.push_back(fy);
        return true;
    }
    g.annInk = true; g.annKind = tool; g.annArm = true;
    g.annPaneB = b; g.annPage = pg; g.annSX = D.sx; g.annSY = D.sy;
    g.annStart = g.annLast = p; g.annFX = fx; g.annFY = fy;
    g.annIX.clear(); g.annIY.clear();
    g.annIX.push_back(fx); g.annIY.push_back(fy);
    if (tool != TOOL_DRAW) { g.annIX.push_back(fx); g.annIY.push_back(fy); }   // the tracked end
    if (tool == TOOL_SHAPE && g.shapeKind == 2) {
        // the polygon's first vertex lands on press; the drag tracks the rubber
        // band and the release keeps the session alive (KillerPDF's click flow)
        g.annPoly = true;
        g.annPX.assign(1, fx); g.annPY.assign(1, fy);
    }
    return true;
}

// The drag under a stroke or a move gesture. p is view pixels; the stroke
// lives in the press-time scroll's DIP space (a scroll mid-stroke cannot skew
// it, KillerPDF pins the gesture to its canvas the same way).
static void ToolMove(HWND h, POINT p) {
    DocState& D = DV(g.annPaneB);
    float k = (float)(D.zoom * g.dpi);
    if (g.annResize) {                        // a corner handle drags: opposite corner anchors
        auto it = D.annos.find(g.annPage);
        if (it != D.annos.end() && g.annSelIdx >= 0 && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            float ax0, ay0, ax1, ay1; AnnotBounds(a, ax0, ay0, ax1, ay1);
            RECT was{ PaneX0(g.annPaneB) + (int)(ax0 * k) - 4, (int)(ay0 * k) - 4,
                      (int)(ax1 * k) + 5, (int)(ay1 * k) + 5 };
            POINT q{ p.x - PaneX0(g.annPaneB) - CardX(g.annPaneB), p.y };
            int pg2; float fx, fy;
            if (ViewToPageAt(D, q, CardW(g.annPaneB), g.ch, g.annSX, g.annSY, pg2, fx, fy)) {
                float axc = (g.annHandle == 0 || g.annHandle == 3) ? ax1 : ax0;
                float ayc = (g.annHandle < 2) ? ay1 : ay0;
                float nw = max(4.0f, fabsf(fx - axc)), nh = max(4.0f, fabsf(fy - ayc));
                float nx0 = fx < axc ? axc - nw : axc, ny0 = fy < ayc ? ayc - nh : ayc;
                ResizeAnnotTo(a, nx0, ny0, nw, nh);
                g.annMoved = true;
            }
            float bx0, by0, bx1, by1; AnnotBounds(a, bx0, by0, bx1, by1);
            RECT now{ PaneX0(g.annPaneB) + (int)(bx0 * k) - 4, (int)(by0 * k) - 4,
                      (int)(bx1 * k) + 5, (int)(by1 * k) + 5 };
            RECT u{ min(was.left, now.left), min(was.top, now.top),
                    max(was.right, now.right), max(was.bottom, now.bottom) };
            InvalidateRect(h, &u, FALSE);
        }
        return;
    }
    if (g.annMove) {                          // the Select tool drags the chosen annotation
        auto it = D.annos.find(g.annPage);
        if (it != D.annos.end() && g.annSelIdx >= 0 && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            float bx0, by0, bx1, by1; AnnotBounds(a, bx0, by0, bx1, by1);
            float ddx = (p.x - g.annLast.x) / max(0.01f, k);
            float ddy = (p.y - g.annLast.y) / max(0.01f, k);
            if (ddx != 0 || ddy != 0) {
                // clamp the drag into the page (KillerPDF's ClampAnnotPos)
                float pw = g.annPage < (int)D.pw.size() ? D.pw[g.annPage] : 0;
                float phh = g.annPage < (int)D.ph.size() ? D.ph[g.annPage] : 0;
                float nx0 = bx0 + ddx, ny0 = by0 + ddy;
                float w = bx1 - bx0, hh = by1 - by0;
                nx0 = std::clamp(nx0, 0.0f, max(0.0f, pw - w));
                ny0 = std::clamp(ny0, 0.0f, max(0.0f, phh - hh));
                ddx = nx0 - bx0; ddy = ny0 - by0;
                if (a.type == AT_HIGHLIGHT) {
                    for (FRect& r : a.lines) { r.x += ddx; r.y += ddy; }
                    a.x = nx0; a.y = ny0;
                } else if (a.type == AT_INK) {
                    for (size_t i = 0; i < a.ix.size(); i++) { a.ix[i] += ddx; a.iy[i] += ddy; }
                } else { a.x = nx0; a.y = ny0; }
                g.annLast = p; g.annMoved = true;
                RECT now{ 0, 0, 0, 0 };
                float ax0, ay0, ax1, ay1; AnnotBounds(a, ax0, ay0, ax1, ay1);
                int ox, oy;
                if (PageOrigin(D, g.annPaneB, g.annPage, ox, oy)) {
                    now = RECT{ PaneX0(g.annPaneB) + ox + (int)(ax0 * k) - 3, oy + (int)(ay0 * k) - 3,
                                PaneX0(g.annPaneB) + ox + (int)(ax1 * k) + 4, oy + (int)(ay1 * k) + 4 };
                }
                InvalidateRect(h, &now, FALSE);   // the bounds ride the annotation: one rect
                                                  // covers the pixels it left and entered
            }
        }
        return;
    }
    if (!g.annInk && !g.annPoly) return;
    if (g.annPoly) {                          // the rubber band tracks the pointer
        POINT q{ p.x - PaneX0(g.annPaneB) - CardX(g.annPaneB), p.y };
        int pg; float fx, fy;
        if (ViewToPageAt(D, q, CardW(g.annPaneB), g.ch, g.annSX, g.annSY, pg, fx, fy)) {
            g.annFX = fx; g.annFY = fy; g.annLast = p;
            InvalidateRect(h, nullptr, FALSE);
        }
        return;
    }
    POINT q{ p.x - PaneX0(g.annPaneB) - CardX(g.annPaneB), p.y };
    int pg; float fx, fy;
    if (!ViewToPageAt(D, q, CardW(g.annPaneB), g.ch, g.annSX, g.annSY, pg, fx, fy)) return;
    if (g.annKind == -2) {                    // the brush eraser erases as it moves
        if (g.annArm) { UndoPush(g.annPaneB); g.annArm = false; }   // one entry per gesture
        AnnInval(h);
        // Densify the segment KillerPDF's DensifyPolyline covers: the eraser
        // works on the PATH, not its samples, so a fast drag leaves no gaps.
        float rad = g.drawWidth * 0.5f;
        float lx = g.annIX.back(), ly = g.annIY.back();
        float dx = fx - lx, dy = fy - ly;
        float dist = sqrtf(dx * dx + dy * dy);
        float step = max(1.0f, rad * 0.5f);
        int steps = (int)(dist / step);
        for (int s2 = 1; s2 <= steps; s2++)
            EraseBrushAt(D, g.annPage, lx + dx * s2 / steps, ly + dy * s2 / steps, rad);
        g.annIX.push_back(fx); g.annIY.push_back(fy);
        EraseBrushAt(D, g.annPage, fx, fy, rad);
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    if (g.annKind == TOOL_DRAW) {
        size_t ln = g.annIX.size();
        if (!ln || fabsf(fx - g.annIX[ln - 1]) > 0.01f || fabsf(fy - g.annIY[ln - 1]) > 0.01f) {
            AnnInval(h);                      // old footprint first: bounds BEFORE the append
            g.annIX.push_back(fx); g.annIY.push_back(fy);
            AnnInval(h);                      // then the new one - the two regions join in
        }                                     // one paint, so the stroke never trails ghosts
        return;
    }
    // line / shape / eraser box: the second point tracks the
    // pointer (KillerPDF's preview). The Level checkbox snaps the line's end
    // to the nearest axis - horizontal when the drag is wider than tall.
    g.annArm = false;
    if (g.annKind == TOOL_LINE && g.lineLevel) {
        if (fabsf(fx - g.annIX[0]) >= fabsf(fy - g.annIY[0])) fy = g.annIY[0];
        else fx = g.annIX[0];
    }
    if (g.annIX.size() < 2) { g.annIX.push_back(fx); g.annIY.push_back(fy); }
    else { g.annIX[1] = fx; g.annIY[1] = fy; }
    AnnInval(h);
}

// The release: KillerPDF's commit rules, verbatim. A line needs >3pt of
// length, a shape >3pt of both dimensions (smaller drags
// cancel with KillerPDF's own status line), a one-point Draw commits as a dot
// (two points 0.25 apart meeting in the round caps). Shapes honor the bar's
// style picker: the filled box commits as a full-alpha block, outline shapes
// as closed ink strokes, the ellipse as KillerPDF's 64 segments with the
// stroke color at half alpha filling the inside when Fill is on.
static void ToolUp(HWND h) {
    DocState& D = DV(g.annPaneB);
    if (g.annResize) {                        // the resize applied live; text reflows once
        g.annResize = false;
        auto it = D.annos.find(g.annPage);
        if (it != D.annos.end() && g.annSelIdx >= 0 && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            if (a.type == AT_TEXT && a.text.size() > 0) {
                float k = (float)(D.zoom * g.dpi);
                HDC m = GetDC(g.view);
                int hpx = max(8, (int)std::lround(a.fsize * k));
                HFONT f = CreateFontW(-hpx, 0, 0, 0, a.bold ? FW_BOLD : FW_NORMAL,
                                      a.italic, a.under, 0, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, a.font.c_str());
                HGDIOBJ of = SelectObject(m, f);
                RECT mr{ 0, 0, (int)(a.w * k), 10000 };
                DrawTextW(m, a.text.c_str(), -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
                SelectObject(m, of); DeleteObject(f); ReleaseDC(g.view, m);
                a.h = max(24.0f, (mr.bottom - mr.top) / max(0.01f, k));
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return;
    }
    if (g.annMove) {
        g.annMove = false;
        return;
    }
    if (g.annPoly) return;                    // the polygon session waits for its closing click
    if (!g.annInk) return;
    g.annInk = false;
    int kind = g.annKind;
    auto& v = D.annos[g.annPage];
    if (kind == -2) {                         // the brush eraser: a click is a dot erase -
                                              // KillerPDF's Covered() with a one-point path
        if (g.annArm) {                       // no move fired: the drag path never erased
            UndoPush(g.annPaneB);
            g.annArm = false;
            EraseBrushAt(D, g.annPage, g.annIX[0], g.annIY[0], g.drawWidth * 0.5f);
            InvalidateRect(h, nullptr, FALSE);
        }
    } else if (kind == -1) {                  // the highlight eraser's carve box
        if (g.annIX.size() >= 2) {
            float x0 = min(g.annIX[0], g.annIX[1]), x1 = max(g.annIX[0], g.annIX[1]);
            float y0 = min(g.annIY[0], g.annIY[1]), y1 = max(g.annIY[0], g.annIY[1]);
            if (x1 - x0 > 3 && y1 - y0 > 3) {
                UndoPush(g.annPaneB);
                EraseBox(h, D, g.annPage, FRect{ x0, y0, x1 - x0, y1 - y0 });
            }
        }
    } else if (kind == TOOL_LINE) {
        if (g.annIX.size() >= 2 && (fabsf(g.annIX[1] - g.annIX[0]) > 3 || fabsf(g.annIY[1] - g.annIY[0]) > 3)) {
            UndoPush(g.annPaneB);
            Annot a; a.type = AT_INK;
            a.width = g.drawWidth; a.color = g.drawColor; a.alpha = g.drawOpacity;
            a.ix = g.annIX; a.iy = g.annIY;
            v.push_back(a);
        }
    } else if (kind == TOOL_SHAPE && g.annIX.size() >= 2) {
        float x0 = min(g.annIX[0], g.annIX[1]), x1 = max(g.annIX[0], g.annIX[1]);
        float y0 = min(g.annIY[0], g.annIY[1]), y1 = max(g.annIY[0], g.annIY[1]);
        if (x1 - x0 > 3 && y1 - y0 > 3) {
            UndoPush(g.annPaneB);
            if (g.shapeKind == 0 && g.shapeFill) {
                // the filled box commits as KillerPDF's HighlightAnnotation:
                // the stroke color at FULL alpha
                Annot a; a.type = AT_HIGHLIGHT; a.color = g.drawColor; a.alpha = 255;
                a.lines.push_back(FRect{ x0, y0, x1 - x0, y1 - y0 });
                a.x = x0; a.y = y0; a.w = x1 - x0; a.h = y1 - y0;
                v.push_back(a);
            } else {
                Annot a; a.type = AT_INK;
                a.width = g.drawWidth; a.color = g.drawColor; a.alpha = g.drawOpacity;
                if (g.shapeKind == 0) {           // the outline box: four corners, closed
                    a.ix = { x0, x1, x1, x0, x0 };
                    a.iy = { y0, y0, y1, y1, y0 };
                } else {                          // the ellipse: 64 segments, closed
                    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
                    float rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
                    for (int k2 = 0; k2 <= 64; k2++) {
                        float ang = k2 * 6.28318530717959f / 64.0f;
                        a.ix.push_back(cx + rx * cosf(ang));
                        a.iy.push_back(cy + ry * sinf(ang));
                    }
                    if (g.shapeFill) { a.fill = g.drawColor; a.fillA = (BYTE)max(20, g.drawOpacity / 2); }
                }
                v.push_back(a);
            }
        }
    } else if (kind == TOOL_DRAW) {
        if (!g.annIX.empty()) {
            if (g.annIX.size() == 1) {        // the dot: 0.25pt to the right, caps meet
                g.annIX.push_back(g.annIX[0] + 0.25f); g.annIY.push_back(g.annIY[0]);
            }
            UndoPush(g.annPaneB);
            Annot a; a.type = AT_INK;
            a.width = g.drawWidth; a.color = g.drawColor; a.alpha = g.drawOpacity;
            a.ix = g.annIX; a.iy = g.annIY;
            v.push_back(a);
        }
    }
    g.annIX.clear(); g.annIY.clear();
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // Clear All may have armed
    InvalidateRect(h, nullptr, FALSE);        // the commit repaints the page's overlay
}

// The Select tool's press: the LIVE selection's corner handles answer first
// (resize, KillerPDF's NW/NE/SE/SW squares), then the topmost annotation under
// the pointer is chosen (KillerPDF hit-tests top-down) and armed for the drag;
// a miss drops the selection and lets the flowing text path arm. Returns true
// on a hit.
static bool SelectAnnotAt(HWND h, bool b, POINT p) {
    DocState& D = DV(b);
    POINT q{ p.x - PaneX0(b) - CardX(b), p.y };
    int pg; float fx, fy;
    if (!ViewToPageAt(D, q, CardW(b), g.ch, D.sx, D.sy, pg, fx, fy)) return false;
    float k = (float)(D.zoom * g.dpi);
    if (g.annSelPage >= 0 && g.annSelB == b && g.annSelIdx >= 0) {
        auto it = D.annos.find(g.annSelPage);
        if (it != D.annos.end() && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            float x0, y0, x1, y1; AnnotBounds(a, x0, y0, x1, y1);
            int ox, oy;
            if (PageOrigin(D, b, g.annSelPage, ox, oy)) {
                int hs = (int)(14 * g.dpi);
                for (int hd = 0; hd < 4; hd++) {
                    float hx = (hd == 0 || hd == 3) ? x0 : x1;
                    float hy = (hd < 2) ? y0 : y1;
                    int cx2 = PaneX0(b) + ox + (int)(hx * k), cy2 = oy + (int)(hy * k);
                    if (p.x >= cx2 - hs / 2 && p.x < cx2 - hs / 2 + hs &&
                        p.y >= cy2 - hs / 2 && p.y < cy2 - hs / 2 + hs) {
                        UndoPush(b);       // one entry covers the whole resize gesture
                        g.annResize = true; g.annHandle = hd; g.annMoved = true;
                        g.annPaneB = b; g.annPage = g.annSelPage;
                        g.annSX = D.sx; g.annSY = D.sy;
                        g.annStart = g.annLast = p;
                        return true;
                    }
                }
            }
        }
    }
    auto it = D.annos.find(pg);
    if (it == D.annos.end()) {
        if (g.annSelPage >= 0) {
            g.annSelPage = -1; g.annSelIdx = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return false;
    }
    for (int ai = (int)it->second.size() - 1; ai >= 0; ai--) {
        if (AnnotHit(it->second[ai], fx, fy, k)) {
            UndoPush(b);                   // one entry covers the whole move gesture
            g.annSelB = b; g.annSelPage = pg; g.annSelIdx = ai;
            g.annMove = true; g.annMoved = false;
            g.annPaneB = b; g.annPage = pg;
            g.annSX = D.sx; g.annSY = D.sy;
            g.annStart = g.annLast = p;
            InvalidateRect(h, nullptr, FALSE);   // the ring lands
            return true;
        }
    }
    if (g.annSelPage >= 0) {
        g.annSelPage = -1; g.annSelIdx = -1;
        InvalidateRect(h, nullptr, FALSE);
    }
    return false;
}

// KillerPDF's SetTool (Shell/ToolSelection.cs), driving the toolbar's tool
// buttons: the tool lands on ONE pane's document state (KillerPDF keeps the
// editing state per viewer instance), a no-document pane re-lands on Select,
// a live text box commits first, an in-progress polygon is abandoned, and the
// view's cursor + brush ring follow on the repaint. The floating bar follows
// the FOCUSED pane's tool: re-clicking the tool that owns the visible bar
// tucks it to its peek strip (ToggleAnnotBarMinimized), picking another bar
// tool rebuilds the bar, Select takes it away. The toolbar repaints too: its
// selected face mirrors the focused pane's tool.
static bool BarToolOwns(int t) {
    return t == TOOL_DRAW || t == TOOL_LINE || t == TOOL_SHAPE || t == TOOL_HIGHLIGHT ||
           t == TOOL_TEXT;
}
static void SetTool(int t, bool b) {
    if (t < 0 || t >= TOOL_COUNT) return;
    DocState& D = DV(b);
    if (!D.n) t = TOOL_SELECT;              // KillerPDF's SetTool: no document, no editing
    CommitTextEdit();                       // an open box lands with the tool switch
    if (g.annPoly && t != TOOL_SHAPE) PolyCancel(g.view);
    RECT ringOff{ 0, 0, 0, 0 };
    if (t != TOOL_DRAW && g.brushOn) {
        ringOff = RECT{ g.brushPt.x - 12, g.brushPt.y - 12, g.brushPt.x + 12, g.brushPt.y + 12 };
        g.brushOn = false;
    }
    bool was = D.tool;
    D.tool = t;
    if (g.annSelPage >= 0) { g.annSelPage = -1; g.annSelIdx = -1; }   // the ring belongs to Select
    RECT oldBar{}; bool hadBar = g.barOn;
    if (hadBar) oldBar = AnnBarRect();
    if (b == FocusB()) {
        if (BarToolOwns(t) && g.barOn && g.barTool == t) {
            g.barMini = !g.barMini;         // re-click: tuck / bring back, no rebuild
        } else {
            g.barOn = BarToolOwns(t);
            g.barTool = t;
            g.barMini = false;
        }
    }
    if (g.bar && (was != t || b == FocusB())) InvalidateRect(g.bar, nullptr, FALSE);
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);   // cursors, ring and the bar ride
    (void)oldBar; (void)hadBar;             // the full-view repaint covers every footprint
}                                         // (a tool switch is a rare discrete click)

// ---------------- the floating annotation settings bar ----------------
// KillerPDF's AnnotationBars: a floating bar under the toolbar at the top of
// the FOCUSED pane's card, right-anchored (gap 8 + the scrollbar's 12px inset
// when the vertical bar shows), square top corners with a rounded bottom
// (0,0,4,4), MenuBackgroundBrush #141414, a 1px #404040 edge on the sides and
// bottom, a soft shadow, a grip of three 3px dots that slides it along the top
// (double-click or a tool re-click tucks it to a 13px peek strip of six dots).
// Content is a wrap of groups: "Color:" swatches (UiKit.DefaultSwatches +
// the rainbow "More colors..."), "Size:" and "Opacity:" sliders (90px track,
// 12x14 green #22bb56 thumb, alpha-byte opacity shown as %), then the tool's
// own toggle - Draw "Eraser", Line "Level" (on by default), Shapes the three
// kind buttons (Box / Ellipse / Freeform polygon) plus "Fill" (checked),
// Highlight "Annotation eraser". The Text tool swaps in its own groups: the
// font combo, B I S U, the Color row, the Fill row (None + swatches), Size
// with -/+ and an editable box, Spacing, Opacity, Fill Opacity, and Edit
// paragraph.

enum { AB_OUT = -2, AB_NONE = -1, AB_GRIP = 0,
       AB_RW_DRAW = 109, AB_SWC_DRAW = 100,        // 100..108 swatches, 109 rainbow
       AB_SWC_TEXT = 200, AB_RW_TEXT = 290,         // 200..208, 290 rainbow
       AB_SWC_FILL = 300, AB_RW_FILL = 390, AB_FILL_NONE = 399,
       AB_SIZE = 10, AB_OPACITY = 11, AB_LEVEL = 12, AB_ERASE = 13, AB_HLERASE = 14,
       AB_FILLCHK = 15, AB_KIND0 = 16, AB_KIND1 = 17, AB_KIND2 = 18,
       AB_FONT = 20, AB_BOLD = 21, AB_ITALIC = 22, AB_STRIKE = 23, AB_UNDER = 24,
       AB_TMINUS = 25, AB_TPLUS = 26, AB_TBOX = 27, AB_SPACING = 28,
       AB_TOACITY = 29, AB_FOACITY = 30, AB_PARA = 31, AB_TSIZE = 32 };

struct AbCell { int id; RECT r; const wchar_t* lab = nullptr; };   // lab: a -1 label cell's text
static std::vector<AbCell> g_ab;
static int g_abW = 0, g_abH = 0;
static int g_abKey = -1, g_abKeyW = -1;
static RECT g_barLast{};                     // the last painted bar rect (old ∪ new invalidation)

static const int AbSwatches[9] = { 0xE03C3C, 0xE87A1E, 0xF2C01E, 0x2EA54C, 0x2E86DE,
                                   0x8E5BD6, 0xE04A9A, 0x000000, 0xFFFFFF };
static HFONT AbFont11() {
    static HFONT f = nullptr; static double fd = 0;
    if (f && fd != g.dpi) { DeleteObject(f); f = nullptr; }   // a DPI hop re-faces the bars
    if (!f) f = CaptionFont(f, 11, false, L"Killer Sans UI");
    fd = g.dpi;
    return f;
}
static int AbTextW(HDC dc, const wchar_t* s) {
    HGDIOBJ of = SelectObject(dc, AbFont11());
    RECT m{ 0, 0, 0, 0 };
    DrawTextW(dc, s, -1, &m, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, of);
    return m.right - m.left;
}
static bool AbBarTool(int t) { return BarToolOwns(t); }

// One pass lays the tool's groups into g_ab (bar-local rects), wrapping when
// the pane's width runs out - WrapPanel's line breaking. The wrap cap is the
// document area's width less the bar chrome (KillerPDF's _barWidthInset: area
// - 28). The packing is the reference's own arithmetic: both wrap hosts carry
// Margin (8,2,8,2); the draw-family bars are the bare host (BarPadding 0,
// BarEdgeThickness 0 in Dark) while the text bar adds Padding 4 and its
// (1,0,1,1) PaneBorderBrush frame; a draw bar's line is content 20 + the 2+2
// group margins, the text bar's rows are content + the 3+3 Group margins; and
// the Opacity unit is ATOMIC - AnnotationBars.cs packs the Level/Eraser check
// (and the Shape tool's whole kind picker) into dOpacityUnit, so the wrap can
// never split a toggle from its slider.
static void AbLayout() {
    bool b = FocusB();
    DocState& D = DV(b);
    int tool = g.barTool;
    bool text = tool == TOOL_TEXT;
    // the text bar carries its own Padding 4 and (1,0,1,1) PaneBorderBrush
    // frame; the draw-family bars ride BarPadding 0 and BarEdgeThickness 0 in
    // Dark - the bare wrap host
    int pad = text ? (int)(5 * g.dpi) : 0;
    int host = (int)(8 * g.dpi);                // the wrap host's Margin (8,...,8)
    int area = max(160, PaneW(b) - (int)(28 * g.dpi));   // the host's MaxWidth: area - 28
    int avail = max(120, area - 2 * pad - 2 * host);     // the content's wrap cap inside it
    int key = (tool + 2) * 2 + (g.barMini ? 1 : 0) * 7 + (int)(g.dpi * 4) * 1000;
    if (key == g_abKey && g_abKeyW == avail) return;
    g_abKey = key; g_abKeyW = avail;
    g_ab.clear();
    if (g.barMini) {
        g_abW = (int)(64 * g.dpi); g_abH = (int)(13 * g.dpi);
        g_ab.push_back({ AB_GRIP, { 0, 0, g_abW, g_abH } });
        return;
    }
    HDC dc = GetDC(g.view);
    auto S = [&](double v) { return (int)std::lround(v * g.dpi); };
    int band = S(20);                       // the line's content band
    int rowH = band + S(4);
    int x = pad + host, y = pad + S(2);     // the host's Margin (8,2) - and the bar's chrome
    int x0 = x;
    int rowTop = y;                         // the row the cells are landing on (y only
    int maxW = 0;                           // moves when a unit wraps)
    // MakeBarGrip: Padding (1,0,10,0) around the 3px dots - one 14px strip,
    // stretched over the line (the text bar's MakeBarGrip(4) is the same
    // width). The cell is EnableBarSlide's drag zone.
    auto grip = [&]() {
        g_ab.push_back({ AB_GRIP, { x, y - S(2), x + S(14), y + rowH - S(2) } });
        x += S(14);
    };
    auto nl = [&]() { maxW = max(maxW, x); x = x0; y += rowH; rowTop = y; };
    auto need = [&](int w) { if (x + w > x0 + avail && x > x0) nl(); };
    auto put = [&](int id, int w, int h) {  // the cell centers in the line's content band
        int top = y + S(2) + max(0, (band - h)) / 2;
        g_ab.push_back({ id, { x, top, x + w, top + h } });
        x += w;
    };
    auto label = [&](const wchar_t* s) {
        int w = AbTextW(dc, s) + S(6);
        g_ab.push_back({ -1, { x, y + S(2), x + w, y + S(2) + band }, s });
        x += w;
    };
    auto swatchRow = [&](int base) {        // the Color group: label + 9 swatches + rainbow, +16
        need(S(42) + 10 * S(20));
        label(L"Color:");
        for (int i = 0; i < 9; i++) put(base + i, S(20), S(18));
        put(base == AB_SWC_DRAW ? AB_RW_DRAW : base == AB_SWC_TEXT ? AB_RW_TEXT : AB_RW_FILL, S(20), S(18));
        x += S(16);                         // the group's Margin (0,2,16,2)
    };
    auto slider = [&](int id, const wchar_t* lab, int valW) {
        need(S(6) + AbTextW(dc, lab) + S(90) + S(4) + valW + S(16));
        label(lab);
        put(id, S(90) + S(4) + valW, S(20));
        x += S(16);                         // the group's Margin (0,2,16,2)
    };
    // BarCheck: the 15px box (radius 3) and the 11px label, the unit centered
    // on the row's content band. lead/trail are the WPF margins around it.
    auto check = [&](int id, const wchar_t* lab, int lead, int trail) {
        int bw = S(15);
        int w = bw + S(5) + AbTextW(dc, lab);
        int top = y + S(2) + max(0, (band - bw)) / 2;
        g_ab.push_back({ id, { x + lead, top, x + lead + w, top + bw } });
        x += lead + w + trail;
    };
    // per-row cell emitters for the text bar's columns: (rh) the row's line
    // height, (ch) the row's content height - the cell centers in the content
    // band the way the WPF StackPanels center in their Group margins.
    auto rowPut = [&](int rx, int ry, int rh, int ch, int id, int w, int h) {
        int top = ry + (rh - ch) / 2 + max(0, (ch - h)) / 2;
        g_ab.push_back({ id, { rx, top, rx + w, top + h } });
        return rx + w;
    };
    auto rowLab = [&](int rx, int ry, int rh, const wchar_t* s) {
        int w = AbTextW(dc, s) + S(6);
        g_ab.push_back({ -1, { rx, ry + (rh - S(16)) / 2, rx + w, ry + (rh + S(16)) / 2 }, s });
        return rx + w;
    };
    if (text) {
        // KillerPDF's deliberate 1.7.5 layout, assembled as three vertical
        // pairs so related controls share an exact left edge: Font over Size
        // over Spacing, Color over Fill over "Edit paragraph", and Opacity
        // beside Fill Opacity. The outer WrapPanel moves a whole pair at
        // narrow widths instead of separating a label from its control. Each
        // row's line height is its content + the Group margin 3+3; each row
        // carries the group's right Margin 16 (the paragraph button's own
        // Margin (0,3,0,0) has none).
        int six = S(6);
        int rowF = six + AbTextW(dc, L"Font:") + S(132) + 4 * S(24);
        int rowS = six + AbTextW(dc, L"Size:") + S(90) + S(21) + S(21) + S(36) + S(16);
        int rowP = six + AbTextW(dc, L"Spacing:") + S(90) + S(4) + S(48);
        int colA = max(rowF, max(rowS, rowP)) + S(16);
        int hF = S(28), hS = S(26), hP = S(26);          // 22/20/20 content + 6
        int rowC = six + AbTextW(dc, L"Color:") + 10 * S(20);
        int rowFl = six + AbTextW(dc, L"Fill:") + S(20) + 10 * S(20);
        int paraW = AbTextW(dc, L"Edit paragraph") + S(20);
        int colB = max(rowC + S(16), max(rowFl + S(16), paraW));
        int hC = S(26), hFl = S(26), hPara = S(25);      // 20/20/22 content + margins
        int oLab = S(6) + AbTextW(dc, L"Opacity:") + S(94) + S(40) + S(16);
        int foLab = S(6) + AbTextW(dc, L"Fill Opacity:") + S(94) + S(40) + S(16);
        int colC = oLab + foLab;
        int hO = S(26);
        int colAh = hF + hS + hP, colBh = hC + hFl + hPara, colCh = hO;
        // the WrapPanel's line decisions: the grip leads line 1, a pair that
        // does not fit moves WHOLE to the next line, whose top rides the
        // tallest pair the line before it kept
        g_ab.push_back({ AB_GRIP, { x, y - S(2), x + S(14), y - S(2) + S(80) } });   // stretched; re-heighted below
        int ax = x + S(14), ay = y;
        int l1w = S(14) + colA, l1h = colAh;
        bool bWrap = l1w + colB > avail;
        if (!bWrap) { l1w += colB; l1h = max(l1h, colBh); }
        bool cWrap = l1w + colC > avail;
        if (!cWrap) l1h = max(l1h, colCh);
        // the grip stretched over line 1: its cell rides l1h (VerticalAlignment.Stretch)
        for (AbCell& c : g_ab) if (c.id == AB_GRIP) c.r.bottom = ay - S(2) + l1h;
        {   // column A rows
            int rx = rowLab(ax, ay, hF, L"Font:");
            rx = rowPut(rx, ay, hF, S(22), AB_FONT, S(132), S(22));
            const wchar_t* styles[4] = { L"B", L"I", L"S", L"U" };
            int sids[4] = { AB_BOLD, AB_ITALIC, AB_STRIKE, AB_UNDER };
            for (int i = 0; i < 4; i++) {
                int s2 = max(S(22), AbTextW(dc, styles[i]) + S(8));
                rowPut(rx + S(2), ay, hF, S(22), sids[i], s2, S(20));   // StyleToggle Margin (2,0,0,0)
                rx += S(2) + s2;
            }
            rx = rowLab(ax, ay + hF, hS, L"Size:");
            rx = rowPut(rx, ay + hF, hS, S(20), AB_TSIZE, S(94), S(20));
            rowPut(rx + S(3), ay + hF, hS, S(20), AB_TMINUS, S(18), S(20)); rx += S(21);
            rowPut(rx + S(3), ay + hF, hS, S(20), AB_TPLUS, S(18), S(20)); rx += S(21);
            rowPut(rx + S(4), ay + hF, hS, S(20), AB_TBOX, S(32), S(20)); rx += S(36);
            rowPut(rx + S(2), ay + hF, hS, S(20), -1, S(14), S(14));       // the pt label
            g_ab.back().lab = L"pt";
            rx = rowLab(ax, ay + hF + hS, hP, L"Spacing:");
            rowPut(rx, ay + hF + hS, hP, S(20), AB_SPACING, S(94) + S(48), S(20));
        }
        int bRight = 0;
        int bx = bWrap ? x0 : ax + colA, by = bWrap ? ay + colAh : ay;
        {   // column B rows
            int rx = rowLab(bx, by, hC, L"Color:");
            for (int i = 0; i < 9; i++) { rowPut(rx, by, hC, S(20), AB_SWC_TEXT + i, S(20), S(18)); rx += S(20); }
            rowPut(rx, by, hC, S(20), AB_RW_TEXT, S(20), S(18)); rx += S(20);
            bRight = max(bRight, rx + S(16));
            rx = rowLab(bx, by + hC, hFl, L"Fill:");
            rowPut(rx, by + hC, hFl, S(20), AB_FILL_NONE, S(18), S(18)); rx += S(20);
            for (int i = 0; i < 9; i++) { rowPut(rx, by + hC, hFl, S(20), AB_SWC_FILL + i, S(20), S(18)); rx += S(20); }
            rowPut(rx, by + hC, hFl, S(20), AB_RW_FILL, S(20), S(18)); rx += S(20);
            bRight = max(bRight, rx + S(16));
            // the paragraph button: Margin (0,3,0,0), MinHeight 22
            int py = by + hC + hFl + S(3);
            g_ab.push_back({ AB_PARA, { bx, py, bx + paraW, py + S(22) } });
            bRight = max(bRight, bx + paraW);
        }
        int cx = cWrap ? x0 : bx + colB;
        int cy = cWrap ? (bWrap ? by + colBh : max(ay + colAh, by + colBh)) : ay;
        {   // the opacity pair, one line (top-aligned like every wrap unit)
            int rx = rowLab(cx, cy, hO, L"Opacity:");
            rowPut(rx, cy, hO, S(20), AB_TOACITY, S(94) + S(40), S(20));
            int rx2 = rowLab(cx + oLab, cy, hO, L"Fill Opacity:");
            rowPut(rx2, cy, hO, S(20), AB_FOACITY, S(94) + S(40), S(20));
        }
        int bottom = max(ay + colAh, max(by + colBh, cy + colCh));
        int l2w = bWrap ? (colB + (cWrap ? 0 : colC)) : (cWrap ? colC : 0);
        maxW = max(l1w, l2w);
        g_abW = min(maxW + host + pad, area - S(28) + 2 * pad);   // + the chrome, capped
        g_abH = bottom + S(2) + pad;                      // + the host's bottom Margin 2 + chrome
        ReleaseDC(g.view, dc);
        for (AbCell& c : g_ab) if (c.r.right > g_abW) c.r.right = g_abW;
        return;
    }
    grip();
    {
        swatchRow(AB_SWC_DRAW);
        // KillerPDF's Width=34 size value and Width=40 opacity value, both
        // right-aligned - the reserves keep the slider pitch constant as the
        // number's digits change.
        if (tool != TOOL_HIGHLIGHT) {
            slider(AB_SIZE, L"Size:", S(34));
        }
        // The dOpacityUnit: Opacity + the Level/Eraser toggle (or the Shape
        // tool's kind picker row) is ONE wrap unit - AnnotationBars.cs packs
        // barCheck into dOpacityUnit, so the unit's width is measured before
        // the wrap test and the toggle can never land on a row by itself.
        {
            const wchar_t* cl = tool == TOOL_DRAW ? L"Eraser" : tool == TOOL_LINE ? L"Level"
                              : tool == TOOL_HIGHLIGHT ? L"Annotation eraser" : nullptr;
            int opW = S(6) + AbTextW(dc, L"Opacity:") + S(94) + S(40);
            int unit = opW + S(16);          // opacityGroup's Margin (0,2,16,2)
            if (cl) unit += S(15) + S(5) + AbTextW(dc, cl) + S(2);   // + barCheck Margin (0,2,2,2)
            if (tool == TOOL_SHAPE)          // ShapeKindPicker's row rides the unit's end
                unit += S(2) + 3 * S(28) + S(10) + S(15) + S(5) + AbTextW(dc, L"Fill") + S(18);
            need(unit);
            label(L"Opacity:");
            put(AB_OPACITY, S(94) + S(40), S(20));
            x += S(16);                      // opacityGroup's Margin (0,2,16,2)
            if (tool == TOOL_DRAW) check(AB_ERASE, L"Eraser", 0, S(2));
            else if (tool == TOOL_LINE) check(AB_LEVEL, L"Level", 0, S(2));
            else if (tool == TOOL_HIGHLIGHT) check(AB_HLERASE, L"Annotation eraser", 0, S(2));
            else if (tool == TOOL_SHAPE) {
                x += S(2);                   // the picker row's Margin (2,0,0,0)
                put(AB_KIND0, S(26), S(20)); x += S(2);   // KindBtn Margin (1,0,1,0)
                put(AB_KIND1, S(26), S(20)); x += S(2);
                x += S(1);                   // KindBtn Margin (1,0,1,0): the last kind's
                                             // right margin, then the check's own lead 10
                check(AB_FILLCHK, L"Fill", S(10), S(18)); // fillCheck Margin (10,0,18,0)
            } else x += S(16);
        }
    }
    maxW = max(maxW, x);
    ReleaseDC(g.view, dc);
    // the bar hugs its content: the host's Margin 8 on both sides (+ the
    // chrome), capped at the area - the same cap the WPF MaxWidth binding
    // puts on the wrap panel (area - 28) plus the chrome around it
    g_abW = min(maxW + host + pad, area - S(28) + 2 * pad);
    g_abH = rowTop + rowH + S(2) + pad;                   // the last row + the host's bottom 2
    for (AbCell& c : g_ab) if (c.r.right > g_abW) c.r.right = g_abW;
}

// The bar's rect in view coords: right-anchored on the FOCUSED pane's card
// (gap 8 + the scrollbar inset), slid by the grip's offset.
static RECT AnnBarRect() {
    AbLayout();
    bool b = FocusB();
    DocState& D = DV(b);
    int cardL = PaneX0(b) + CardX(b), cardR = cardL + CardW(b);
    int inset = (int)(8 * g.dpi) + (D.n && D.docH > g.ch ? SBar() : 0);
    int x0 = cardR - inset - g.barDx - g_abW;
    x0 = std::clamp(x0, cardL + (int)(2 * g.dpi), max(cardL, cardR - (int)(2 * g.dpi) - g_abW));
    int y0 = BandInset(b);               // KillerPDF's bar anchors to the card's top - under
    return { x0, y0, x0 + g_abW, y0 + g_abH };   // the tab strip's row, never over its tabs
}

// State flips repaint the bar's own footprint only (old ∪ new) - the bar sits
// over pages, never recompositing the document under it. The shadow's 3px near
// field below the bottom edge rides along.
static void AnnBarSync(bool) {
    if (!g.view) return;
    // the shadow field: blur 6 (sigma 3) tails ~9px past the sides and depth
    // 3 + ~9px below - the tile AnnBarDraw blends, so old ∪ new must cover it
    int mx = (int)std::ceil(9.0 * g.dpi);
    int sh = (int)std::ceil((3.0 + 9.0) * g.dpi) + 1;
    RECT now = g.barOn ? AnnBarRect() : RECT{ 0, 0, 0, 0 };
    RECT old = g_barLast;
    bool hadOld = old.left < old.right;
    if (hadOld && g.barOn) {
        now.bottom += sh; old.bottom += sh;
        now.left -= mx; now.right += mx;
        old.left -= mx; old.right += mx;
        RECT u{ min(old.left, now.left), min(old.top, now.top),
                max(old.right, now.right), max(old.bottom, now.bottom) };
        InvalidateRect(g.view, &u, FALSE);
    } else if (hadOld) { old.bottom += sh; old.left -= mx; old.right += mx; InvalidateRect(g.view, &old, FALSE); }
    else if (g.barOn) { now.bottom += sh; now.left -= mx; now.right += mx; InvalidateRect(g.view, &now, FALSE); }
}

// Bar choices flow into the SELECTED annotation live (KillerPDF's
// ApplyDrawStyleToSelection) and set the defaults the next stroke commits.
static void ApplyDrawStyleToSelection() {
    if (g.annSelPage < 0 || g.annSelIdx < 0) return;
    DocState& D = DV(g.annSelB);
    auto it = D.annos.find(g.annSelPage);
    if (it == D.annos.end() || g.annSelIdx >= (int)it->second.size()) return;
    Annot& a = it->second[g.annSelIdx];
    if (a.type == AT_HIGHLIGHT) {
        if (g.barTool == TOOL_HIGHLIGHT) { a.color = g.hlRGB; a.alpha = g.hlA; }
        else { a.color = g.drawColor; a.alpha = 255; }   // a filled shape's block
    } else if (a.type == AT_INK) {
        a.color = g.drawColor; a.alpha = g.drawOpacity; a.width = g.drawWidth;
        if (a.fillA > 0) { a.fill = g.drawColor; a.fillA = (BYTE)max(20, g.drawOpacity / 2); }
    }
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// The text bar's funnel: the live editor restyles first, then every selected
// text box (KillerPDF's ApplyTextStyleToSelection), and the next placed box
// picks the same state up.
static void ApplyTextStyleToSelection() {
    if (g.tedit) {
        float k = (float)(DV(g.tePaneB).zoom * g.dpi);
        int fontPx = max(8, (int)std::lround(g.textSize * k));
        HFONT f = CreateFontW(-fontPx, 0, 0, 0, g.textBold ? FW_BOLD : FW_NORMAL,
                              g.textItalic, g.textUnder, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, g.textFont.c_str());
        SendMessageW(g.tedit, WM_SETFONT, (WPARAM)f, TRUE);
        RECT er; GetWindowRect(g.tedit, &er);
        POINT p0{ er.left, er.top }, p1{ er.right, er.bottom };
        ScreenToClient(g.view, &p0); ScreenToClient(g.view, &p1);
        RECT u{ p0.x - 8, p0.y - 8, p1.x + 8, p1.y + 8 };
        InvalidateRect(g.view, &u, FALSE);
    }
    if (g.annSelPage >= 0 && g.annSelIdx >= 0) {
        DocState& D = DV(g.annSelB);
        auto it = D.annos.find(g.annSelPage);
        if (it != D.annos.end() && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            if (a.type == AT_TEXT) {
                a.font = g.textFont; a.fsize = g.textSize; a.spacing = g.textSpacing;
                a.bold = g.textBold; a.italic = g.textItalic;
                a.strike = g.textStrike; a.under = g.textUnder;
                a.color = g.textRGB; a.alpha = g.textA;
                a.fill = g.textFillRGB; a.fillA = g.textFillA;
                float k = (float)(D.zoom * g.dpi);
                HDC m = GetDC(g.view);
                int hpx = max(8, (int)std::lround(a.fsize * k));
                HFONT f = CreateFontW(-hpx, 0, 0, 0, a.bold ? FW_BOLD : FW_NORMAL,
                                      a.italic, a.under, 0, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, a.font.c_str());
                HGDIOBJ of = SelectObject(m, f);
                RECT mr{ 0, 0, (int)(a.w * k), 10000 };
                DrawTextW(m, a.text.c_str(), -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
                SelectObject(m, of); DeleteObject(f); ReleaseDC(g.view, m);
                a.h = max(24.0f, (mr.bottom - mr.top) / max(0.01f, k));
            }
        }
    }
    AnnBarSync(true);
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// ---- the bar's paint ----
static void AbRoundRect(Gdiplus::Graphics& gs, const RECT& r, float rad,
                        Gdiplus::Brush* fill, Gdiplus::Pen* pen = nullptr) {
    Gdiplus::GraphicsPath p;
    float fx = (float)r.left, fy = (float)r.top;
    float fw = (float)(r.right - r.left), fh = (float)(r.bottom - r.top);
    if (fw <= 0 || fh <= 0) return;
    p.AddArc(fx, fy, 2 * rad, 2 * rad, 180, 90);
    p.AddArc(fx + fw - 2 * rad, fy, 2 * rad, 2 * rad, 270, 90);
    p.AddArc(fx + fw - 2 * rad, fy + fh - 2 * rad, 2 * rad, 2 * rad, 0, 90);
    p.AddArc(fx, fy + fh - 2 * rad, 2 * rad, 2 * rad, 90, 90);
    p.CloseFigure();
    if (fill) gs.FillPath(fill, &p);
    if (pen) gs.DrawPath(pen, &p);
}
static HFONT AbMdlFont(int px) {
    static HFONT f = nullptr; static int fpx = 0; static double fd = 0;
    if (f && (fpx != px || fd != g.dpi)) { DeleteObject(f); f = nullptr; }
    if (!f) f = CaptionFont(f, px, false, L"Segoe MDL2 Assets");
    fpx = px; fd = g.dpi;
    return f;
}
static COLORREF AbCellColor(int id) {
    if (id >= AB_SWC_DRAW && id < AB_RW_DRAW) return RGB((AbSwatches[id - AB_SWC_DRAW] >> 16) & 0xff,
        (AbSwatches[id - AB_SWC_DRAW] >> 8) & 0xff, AbSwatches[id - AB_SWC_DRAW] & 0xff);
    if (id >= AB_SWC_TEXT && id < AB_RW_TEXT) { int v = AbSwatches[id - AB_SWC_TEXT]; return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff); }
    if (id >= AB_SWC_FILL && id < AB_RW_FILL) { int v = AbSwatches[id - AB_SWC_FILL]; return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff); }
    return 0;
}
static bool AbSwatchActive(int id) {
    COLORREF c = AbCellColor(id);
    if (id >= AB_SWC_DRAW && id < AB_RW_DRAW) {
        if (g.barTool == TOOL_HIGHLIGHT) return g.hlRGB == c;
        return g.drawColor == c;
    }
    if (id >= AB_SWC_TEXT && id < AB_RW_TEXT) return g.textRGB == c;
    return g.textFillA > 0 && g.textFillRGB == c;
}
static void AbSliderDraw(Gdiplus::Graphics& gs, const RECT& r, float vmin, float vmax, float v) {
    float trackY = (r.top + r.bottom) / 2.0f - 2.0f * (float)g.dpi;
    Gdiplus::SolidBrush track(Gdiplus::Color(255, 0x3f, 0x3f, 0x3f));
    AbRoundRect(gs, RECT{ r.left, (int)trackY, r.left + (int)(90 * g.dpi), (int)trackY + (int)(4 * g.dpi) },
                2.0f * (float)g.dpi, &track);
    float t = std::clamp((v - vmin) / max(0.001f, vmax - vmin), 0.0f, 1.0f);
    int tx = r.left + (int)(t * (90.0f * g.dpi - 12.0f * g.dpi));
    Gdiplus::SolidBrush thumb(Gdiplus::Color(255, 0x22, 0xbb, 0x56));
    AbRoundRect(gs, RECT{ tx, (int)trackY - (int)(5 * g.dpi), tx + (int)(12 * g.dpi), (int)trackY + (int)(9 * g.dpi) },
                2.0f * (float)g.dpi, &thumb);
}
// The tick itself. KillerPDF's BarCheck marks a literal "✓" (U+2713) TextBlock
// at FontSize 10, Center/Center in the 15px box (AnnotationBars.cs) - WPF
// centers the text line, and the glyph reads centered. GDI's DT_VCENTER
// centers the font's LINE BOX too, but the glyph's ink can sit off-center
// inside that line - the drift the user saw. So the tick is measured once per
// DPI here: drawn into a scratch 15px box exactly the way the real one draws,
// its ink bounding box scanned, and the real draw's text rect shifted by minus
// that ink-to-center offset - the ink lands dead center BY CONSTRUCTION,
// because the measurement and the draw share one placement path (DrawTextW's
// rect math; no second metric convention to disagree). ANTIALIASED_QUALITY
// matches WPF's grayscale anti-aliasing - ClearType's subpixel filter tinted
// the thin stroke with the background's chroma. Segoe UI owns U+2713 on every
// Windows this targets; Segoe UI Symbol, then MDL2's E73E, are the fallbacks.
static void AbCheckGlyph(HDC dc, const RECT& box) {
    static HFONT f = nullptr; static double fd = 0; static wchar_t ch = 0;
    static POINT off{ 0, 0 };                 // ink-center minus box-center at this dpi
    if (f && fd != g.dpi) { DeleteObject(f); f = nullptr; }
    if (!f) {
        const struct { const wchar_t* face; wchar_t ch; } tries[] = {
            { L"Segoe UI", L'\u2713' }, { L"Segoe UI Symbol", L'\u2713' },
            { L"Segoe MDL2 Assets", L'\uE73E' } };
        for (const auto& t : tries) {
            HFONT tf = CreateFontW(-(int)std::lround(10 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, t.face);
            HGDIOBJ of = SelectObject(dc, tf);
            WORD idx = 0xFFFF;
            GetGlyphIndicesW(dc, &t.ch, 1, &idx, 1);   // GGI_MARK_NONEXISTING_GLYPHS
            SelectObject(dc, of);
            if (idx != 0xFFFF) { f = tf; ch = t.ch; break; }
            DeleteObject(tf);
        }
        if (f) {
            int side = max(1, (int)(15 * g.dpi));
            HDC m = CreateCompatibleDC(dc);
            HBITMAP bm = CreateCompatibleBitmap(dc, side, side);
            HGDIOBJ ob = SelectObject(m, bm);
            RECT full{ 0, 0, side, side };
            HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(m, &full, bg); DeleteObject(bg);
            HGDIOBJ of2 = SelectObject(m, f);
            SetBkMode(m, TRANSPARENT);
            SetTextColor(m, RGB(255, 255, 255));
            DrawTextW(m, &ch, 1, &full, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(m, of2);
            int x0 = side, y0 = side, x1 = -1, y1 = -1;
            for (int y = 0; y < side; y++)
                for (int x = 0; x < side; x++) {
                    COLORREF c = GetPixel(m, x, y);
                    if (GetRValue(c) + GetGValue(c) + GetBValue(c) > 90) {   // ink over the black floor
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
                }
            SelectObject(m, ob); DeleteObject(bm); DeleteDC(m);
            off = (x1 >= x0 && y1 >= y0) ? POINT{ (x0 + x1) / 2 - side / 2, (y0 + y1) / 2 - side / 2 }
                                         : POINT{ 0, 0 };
        }
        fd = g.dpi;
    }
    if (!f || !ch) return;
    RECT cr{ box.left - off.x, box.top - off.y, box.right - off.x, box.bottom - off.y };
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, &ch, 1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}
static void AbCheckDraw(Gdiplus::Graphics& gs, HDC dc, const RECT& r, bool on) {
    RECT box{ r.left, r.top, r.left + (int)(15 * g.dpi), r.top + (int)(15 * g.dpi) };
    if (on) {
        Gdiplus::LinearGradientBrush gb(Gdiplus::Rect(box.left, box.top, box.right - box.left, box.bottom - box.top),
            Gdiplus::Color(255, 0x38, 0x75, 0x52), Gdiplus::Color(255, 0x10, 0x4e, 0x2b), 90.0f);
        AbRoundRect(gs, box, 3.0f * (float)g.dpi, &gb);
        SetTextColor(dc, RGB(255, 255, 255));   // SelectionFg on the SelectionBg fill
        AbCheckGlyph(dc, box);
    } else {
        Gdiplus::Pen pen(Gdiplus::Color(255, 0x44, 0x44, 0x44), 1.0f);
        AbRoundRect(gs, box, 3.0f * (float)g.dpi, nullptr, &pen);
    }
    SelectObject(dc, AbFont11());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
}
// The check's label rides its cell's band, vertically centered on the box -
// the whole BarCheck StackPanel is one centered line (AnnotationBars.cs).
static void AbCheckLabel(HDC dc, const RECT& r, const wchar_t* lab) {
    SelectObject(dc, AbFont11());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
    RECT lr{ r.left + (int)(20 * g.dpi), r.top, r.right, r.bottom };
    DrawTextW(dc, lab, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static double Phi(double z);   // the gaussian CDF, defined with the card's shadow
static Gdiplus::TextureBrush* GrainTex(int which);   // the grain tile as a GDI+ brush
static int g_grainPX = 0, g_grainPY = 0;   // GDI's brush-org phase, stashed by the paint

static void AnnBarDraw(HDC dc, int ox, int oy) {
    if (!g.barOn) return;
    AbLayout();
    g_barLast = RECT{ ox, oy, ox + g_abW, oy + g_abH };
    bool textBar = g.barTool == TOOL_TEXT;
    // the DropShadow: black, blur 6 (sigma 3), depth 3 down, opacity 0.38 -
    // AnnotBarShadow's exact recipe, rasterized as one cached gaussian tile
    // like the Find bar's (its blur 16 / 0.55 there, 6 / 0.38 here)
    {
        static HBITMAP shb = nullptr; static HDC shdc = nullptr;
        static int shw = 0, shh = 0, shmx = 0; static double shdpi = 0;
        double sig = 3.0 * g.dpi, depth = 3.0 * g.dpi;
        int mx = (int)std::ceil(3.0 * sig), my = (int)std::ceil(depth + 3.0 * sig);
        int tw = g_abW + 2 * mx, th = g_abH + my;
        if (!shb || shw != tw || shh != th || shdpi != g.dpi) {
            BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), tw, -th, 1, 32, BI_RGB };
            BYTE* px = nullptr;
            HBITMAP nb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
            if (px && nb) {
                for (int y = 0; y < th; y++) {
                    double ay = Phi((y - depth) / sig) - Phi((y - depth - g_abH) / sig);
                    BYTE* row = px + (size_t)y * tw * 4;
                    for (int x2 = 0; x2 < tw; x2++) {
                        double a = 0.38 * ay * (Phi((x2 - mx) / sig) - Phi((x2 - mx - g_abW) / sig));
                        BYTE* p = row + (size_t)x2 * 4;
                        p[0] = p[1] = p[2] = 0;
                        p[3] = (BYTE)std::lround(a * 255.0);
                    }
                }
                if (!shdc) shdc = CreateCompatibleDC(dc);
                if (shb) DeleteObject(shb);
                shb = nb;
                SelectObject(shdc, shb);
                shw = tw; shh = th; shmx = mx; shdpi = g.dpi;
            }
        }
        if (shb) {
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(dc, ox - shmx, oy, shw, shh, shdc, 0, 0, shw, shh, bf);
        }
    }
    // the body: the floating surface tier - MenuBackgroundBrush #141414 under
    // the shared grain (AnnotationBars.cs's BuildBarHost grain layer), square
    // top corners, the flyout's radius-4 bottom (0,0,4,4). The TEXT bar adds
    // its own (1,0,1,1) PaneBorderBrush #555555 frame - no top edge, the
    // toolbar above already separates (TextSettingsBar.cs); the draw-family
    // bars ride BarEdgeThickness 0 in Dark - no edge of their own, the shadow
    // is the only line.
    int spx = g_grainPX, spy = g_grainPY;
    g_grainPX += ox; g_grainPY += oy;
    {
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        float fx = (float)ox, fy = (float)oy;
        float fw = (float)g_abW, fh = (float)g_abH;
        float rad = 4.0f * (float)g.dpi;
        auto barPath = [&](float inset) {
            Gdiplus::GraphicsPath* p = new Gdiplus::GraphicsPath();
            float r = max(0.5f, rad - inset);
            p->AddLine(fx + inset, fy + inset, fx + fw - inset, fy + inset);
            p->AddArc(fx + fw - inset - 2 * r, fy + fh - inset - 2 * r, 2 * r, 2 * r, 0, 90);
            p->AddArc(fx + inset, fy + fh - inset - 2 * r, 2 * r, 2 * r, 90, 90);
            p->CloseFigure();
            return p;
        };
        if (textBar) {   // the 1px rim under the grain: fill the rim color,
                         // then the inset body over it - the top edge stays
                         // body (no top border)
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush rim(Gdiplus::Color(255, 0x55, 0x55, 0x55));
            Gdiplus::GraphicsPath* outer = barPath(0);
            gs.FillPath(&rim, outer);
            delete outer;
        }
        Gdiplus::GraphicsPath* inner = barPath(textBar ? 1.0f : 0.0f);
        if (Gdiplus::TextureBrush* t = GrainTex(2)) gs.FillPath(t, inner);
        else { Gdiplus::SolidBrush mb(Gdiplus::Color(255, 0x14, 0x14, 0x14)); gs.FillPath(&mb, inner); }
        delete inner;
    }
    g_grainPX = spx; g_grainPY = spy;
    // the grip: MakeBarGrip's dots - 3px, MutedTextBrush #a3a3a3, spaced 3px,
    // centered in the grip strip (three dots on the single-row bars, the text
    // bar's MakeBarGrip(4) carries four)
    {
        Gdiplus::Graphics gs(dc);
        for (const AbCell& c : g_ab) {
            if (c.id != AB_GRIP) continue;
            int n = textBar ? 4 : 3;
            float cx = (float)(ox + c.r.left) + 2.5f * (float)g.dpi;
            float cy = (float)(oy + c.r.top + c.r.bottom) / 2.0f;
            Gdiplus::SolidBrush dot(Gdiplus::Color(255, 0xa3, 0xa3, 0xa3));
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            for (int i = 0; i < n; i++)
                gs.FillEllipse(&dot, cx - 1.5f * (float)g.dpi, cy + (i - (n - 1) / 2.0f) * 6.0f * (float)g.dpi - 1.5f * (float)g.dpi,
                               3.0f * (float)g.dpi, 3.0f * (float)g.dpi);
            break;
        }
    }
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, AbFont11());
    wchar_t tmp[48];
    for (const AbCell& c : g_ab) {
        RECT r{ ox + c.r.left, oy + c.r.top, ox + c.r.right, oy + c.r.bottom };
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        int id = c.id;
        if (id == -1) {                          // the group's label: KillerPDF's DimLabel
            if (c.lab) {
                SelectObject(dc, AbFont11());
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
                DrawTextW(dc, c.lab, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            continue;
        }
        if ((id >= AB_SWC_DRAW && id < AB_RW_DRAW) || (id >= AB_SWC_TEXT && id < AB_RW_TEXT) ||
            (id >= AB_SWC_FILL && id < AB_RW_FILL)) {
            COLORREF col = AbCellColor(id);
            Gdiplus::SolidBrush f(Gdiplus::Color(255, GetRValue(col), GetGValue(col), GetBValue(col)));
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, &f);
            bool act = AbSwatchActive(id);
            Gdiplus::Pen pen(act ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0x44, 0x44, 0x44),
                             act ? 2.0f : 1.0f);
            AbRoundRect(gs, RECT{ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, 3.0f * (float)g.dpi, nullptr, &pen);
        } else if (id == AB_RW_DRAW || id == AB_RW_TEXT || id == AB_RW_FILL) {
            // the rainbow "More colors..." swatch: the picker's own gradient
            Gdiplus::Color cols[5] = { Gdiplus::Color(255, 255, 0, 0), Gdiplus::Color(255, 255, 255, 0),
                                       Gdiplus::Color(255, 0, 255, 0), Gdiplus::Color(255, 0, 255, 255),
                                       Gdiplus::Color(255, 0, 0, 255) };
            float pos[5] = { 0.0f, 0.25f, 0.5f, 0.7f, 1.0f };   // Gdiplus::REAL positions
            Gdiplus::LinearGradientBrush rb(Gdiplus::Rect(r.left, r.top, r.right - r.left, r.bottom - r.top),
                Gdiplus::Color(255, 255, 0, 0), Gdiplus::Color(255, 0, 0, 255), 0.0f, false);
            rb.SetInterpolationColors(cols, pos, 5);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, &rb);
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x44, 0x44, 0x44), 1.0f);
            AbRoundRect(gs, RECT{ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, 3.0f * (float)g.dpi, nullptr, &pen);
        } else if (id == AB_FILL_NONE) {
            Gdiplus::SolidBrush w(Gdiplus::Color(255, 255, 255, 255));
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, &w);
            Gdiplus::Pen red(Gdiplus::Color(255, 255, 0, 0), 1.5f);
            gs.DrawLine(&red, (float)r.left + 3, (float)r.bottom - 3, (float)r.right - 3, (float)r.top + 3);
            Gdiplus::Pen pen(g.textFillA == 0 ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0x44, 0x44, 0x44),
                             g.textFillA == 0 ? 2.0f : 1.0f);
            AbRoundRect(gs, RECT{ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, 3.0f * (float)g.dpi, nullptr, &pen);
        } else if (id == AB_SIZE) {
            AbSliderDraw(gs, r, 1, 60, g.drawWidth);
            swprintf(tmp, 48, L"%dpx", (int)g.drawWidth);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_OPACITY) {
            AbSliderDraw(gs, r, 10, 255, g.drawOpacity);
            swprintf(tmp, 48, L"%d%%", (int)(g.drawOpacity / 255.0 * 100));
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_TSIZE) {
            AbSliderDraw(gs, r, 8, 72, std::clamp(g.textSize, 8.0f, 72.0f));
        } else if (id == AB_SPACING) {
            AbSliderDraw(gs, r, -2, 20, g.textSpacing);
            swprintf(tmp, 48, L"%.2f pt", g.textSpacing);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_TOACITY) {
            AbSliderDraw(gs, r, 10, 255, g.textA);
            swprintf(tmp, 48, L"%d%%", (int)(g.textA / 255.0 * 100));
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_FOACITY) {
            AbSliderDraw(gs, r, 10, 255, g.textFillA == 0 ? 255 : g.textFillA);
            swprintf(tmp, 48, L"%d%%", (int)((g.textFillA == 0 ? 255 : g.textFillA) / 255.0 * 100));
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_ERASE) {
            AbCheckDraw(gs, dc, r, g.drawErase);
            AbCheckLabel(dc, r, L"Eraser");
        } else if (id == AB_LEVEL) {
            AbCheckDraw(gs, dc, r, g.lineLevel);
            AbCheckLabel(dc, r, L"Level");
        } else if (id == AB_HLERASE) {
            AbCheckDraw(gs, dc, r, g.hlErase);
            AbCheckLabel(dc, r, L"Annotation eraser");
        } else if (id == AB_FILLCHK) {
            AbCheckDraw(gs, dc, r, g.shapeFill);
            AbCheckLabel(dc, r, L"Fill");
        } else if (id >= AB_KIND0 && id <= AB_KIND2) {
            int k = id - AB_KIND0;
            bool act = g.shapeKind == k;
            Gdiplus::Pen pen(act ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0x44, 0x44, 0x44),
                             act ? 1.5f : 1.0f);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, nullptr, &pen);
            Gdiplus::Pen gl(act ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0xa3, 0xa3, 0xa3), 1.5f);
            float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
            float s = (float)g.dpi;
            if (k == 0) gs.DrawRectangle(&gl, cx - 6.5f * s, cy - 4.5f * s, 13 * s, 9 * s);
            else if (k == 1) gs.DrawEllipse(&gl, cx - 6.5f * s, cy - 4.5f * s, 13 * s, 9 * s);
            else {
                Gdiplus::PointF pts[5] = {
                    Gdiplus::PointF(cx, cy - 5.5f * s), Gdiplus::PointF(cx + 6.5f * s, cy - 1.0f * s),
                    Gdiplus::PointF(cx + 4.0f * s, cy + 5.5f * s), Gdiplus::PointF(cx - 4.0f * s, cy + 5.5f * s),
                    Gdiplus::PointF(cx - 6.5f * s, cy - 1.0f * s) };
                gs.DrawPolygon(&gl, pts, 5);
            }
        } else if (id == AB_FONT) {
            Gdiplus::SolidBrush f(Gdiplus::Color(255, 0x14, 0x14, 0x14));
            AbRoundRect(gs, r, 2.0f * (float)g.dpi, &f);
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x40, 0x40, 0x40), 1.0f);
            AbRoundRect(gs, r, 2.0f * (float)g.dpi, nullptr, &pen);
            SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
            RECT tr{ r.left + (int)(6 * g.dpi), r.top, r.right - (int)(18 * g.dpi), r.bottom };
            std::wstring nm = g.textFont;
            DrawTextW(dc, nm.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            SelectObject(dc, AbMdlFont(12));
            RECT cr2{ r.right - (int)(16 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, L"\uE70D", -1, &cr2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, AbFont11());
        } else if (id >= AB_BOLD && id <= AB_UNDER) {
            bool act = (id == AB_BOLD && g.textBold) || (id == AB_ITALIC && g.textItalic) ||
                       (id == AB_STRIKE && g.textStrike) || (id == AB_UNDER && g.textUnder);
            if (act) {
                Gdiplus::SolidBrush bg(Gdiplus::Color(40, 0x22, 0xbb, 0x56));
                AbRoundRect(gs, r, 3.0f * (float)g.dpi, &bg);
            }
            Gdiplus::Pen pen(act ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0x44, 0x44, 0x44),
                             act ? 2.0f : 1.0f);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, nullptr, &pen);
            static HFONT fb = nullptr, fi = nullptr, fs = nullptr, fu = nullptr;
            HFONT f = id == AB_BOLD ? CaptionFont(fb, 12, true, L"Segoe UI")
                    : id == AB_ITALIC ? CaptionFont(fi, 12, false, L"Segoe UI")
                    : id == AB_UNDER ? CaptionFont(fu, 12, false, L"Segoe UI")
                    : CaptionFont(fs, 12, false, L"Segoe UI");
            if (id == AB_ITALIC && f) { LOGFONTW lf; GetObject(f, sizeof lf, &lf); lf.lfItalic = TRUE; HFONT f2 = CreateFontIndirectW(&lf); DeleteObject(fi); fi = f2; f = f2; }
            if (id == AB_UNDER && f) { LOGFONTW lf; GetObject(f, sizeof lf, &lf); lf.lfUnderline = TRUE; HFONT f2 = CreateFontIndirectW(&lf); DeleteObject(fu); fu = f2; f = f2; }
            if (id == AB_STRIKE && f) { LOGFONTW lf; GetObject(f, sizeof lf, &lf); lf.lfStrikeOut = TRUE; HFONT f2 = CreateFontIndirectW(&lf); DeleteObject(fs); fs = f2; f = f2; }
            SelectObject(dc, f);
            SetTextColor(dc, act ? RGB(255, 255, 255) : RGB(0xa3, 0xa3, 0xa3));
            DrawTextW(dc, id == AB_BOLD ? L"B" : id == AB_ITALIC ? L"I" : id == AB_STRIKE ? L"S" : L"U",
                      -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, AbFont11());
        } else if (id == AB_TMINUS || id == AB_TPLUS) {
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x44, 0x44, 0x44), 1.0f);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, nullptr, &pen);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            DrawTextW(dc, id == AB_TMINUS ? L"\u2212" : L"+", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_TBOX) {
            Gdiplus::SolidBrush f(Gdiplus::Color(255, 0x1c, 0x1c, 0x1c));
            AbRoundRect(gs, r, 2.0f * (float)g.dpi, &f);
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x40, 0x40, 0x40), 1.0f);
            AbRoundRect(gs, r, 2.0f * (float)g.dpi, nullptr, &pen);
            SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
            swprintf(tmp, 48, L"%d", (int)g.textSize);
            DrawTextW(dc, tmp, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_PARA) {
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x44, 0x44, 0x44), 1.0f);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, nullptr, &pen);
            SetTextColor(dc, g.barHot == AB_PARA ? RGB(255, 255, 255) : RGB(0xe0, 0xe0, 0xe0));
            DrawTextW(dc, L"Edit paragraph", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

// Hit-testing: -2 = not over the bar (the page under it gets the press), -1 =
// over the bar's padding (chrome: the press dies here), else the cell id.
static int AnnBarHit(POINT p) {
    if (!g.barOn) return AB_OUT;
    AbLayout();
    RECT r = AnnBarRect();
    if (p.x < r.left || p.x >= r.right || p.y < r.top || p.y >= r.bottom) return AB_OUT;
    for (const AbCell& c : g_ab)
        if (c.id != -1 && p.x >= r.left + c.r.left && p.x < r.left + c.r.right &&
            p.y >= r.top + c.r.top && p.y < r.top + c.r.bottom) return c.id;
    return AB_NONE;
}

// ---- the bar's popups: the font list, the color picker, the Transform menu,
// the size edit and the paragraph editor ----
static std::vector<std::wstring>* g_fonts = nullptr;
static int CALLBACK FontEnumProc(const ENUMLOGFONTEXW* lpelfe, const NEWTEXTMETRICW*, DWORD, LPARAM lp) {
    auto* v = (std::vector<std::wstring>*)lp;
    std::wstring n = lpelfe->elfLogFont.lfFaceName;
    if (!n.empty() && n[0] != '@') v->push_back(n);
    return 1;
}
static std::vector<std::wstring>& FontList() {
    if (!g_fonts) {
        g_fonts = new std::vector<std::wstring>();
        LOGFONTW lf{}; lf.lfCharSet = DEFAULT_CHARSET;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)FontEnumProc, (LPARAM)g_fonts, 0);
        ReleaseDC(nullptr, dc);
        std::sort(g_fonts->begin(), g_fonts->end());
        g_fonts->erase(std::unique(g_fonts->begin(), g_fonts->end()), g_fonts->end());
        auto it = std::find(g_fonts->begin(), g_fonts->end(), L"Segoe UI");
        if (it != g_fonts->end()) g_fonts->erase(it);
        g_fonts->insert(g_fonts->begin(), L"Segoe UI");
    }
    return *g_fonts;
}

// A small menu-like list popup (the font combo's dropdown, the Transform
// menu): CS_SAVEBITS rows over #141414, closes on deactivate like a menu.
struct ListPop { std::vector<std::wstring> items; int hot = -1, scroll = 0, who = 0; };
static ListPop g_lp;
static void ListPopClose() {
    if (g.fontPop) { DestroyWindow(g.fontPop); g.fontPop = nullptr; }
}
static LRESULT CALLBACK ListPopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) ListPopClose();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, PopBrush());
        SetBkMode(dc, TRANSPARENT);
        int rowH = (int)(22 * g.dpi);
        SelectObject(dc, AbFont11());
        for (int i = g_lp.scroll; i < (int)g_lp.items.size(); i++) {
            int y = (int)(4 * g.dpi) + (i - g_lp.scroll) * rowH;
            if (y + rowH > r.bottom) break;
            RECT rr{ 2, y, r.right - 2, y + rowH };
            if (i == g_lp.hot) FillRect(dc, &rr, PopHoverBrush());
            if (g_lp.who == 0 && g_lp.items[i] == g.textFont) {
                SetTextColor(dc, RGB(0x22, 0xbb, 0x56));
                RECT mr{ rr.right - (int)(20 * g.dpi), y, rr.right, y + rowH };
                SelectObject(dc, AbMdlFont(11));
                DrawTextW(dc, L"\uE73E", -1, &mr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, AbFont11());
            }
            SetTextColor(dc, i == g_lp.hot ? RGB(255, 255, 255) : RGB(0xe0, 0xe0, 0xe0));
            RECT tr{ rr.left + (int)(8 * g.dpi), y, rr.right - (int)(22 * g.dpi), y + rowH };
            DrawTextW(dc, g_lp.items[i].c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        FrameRect(dc, &r, ComboBorderBrush());
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int rowH = (int)(22 * g.dpi);
        int hot = -1;
        if (p.y >= (int)(4 * g.dpi)) {
            hot = g_lp.scroll + (p.y - (int)(4 * g.dpi)) / rowH;
            if (hot >= (int)g_lp.items.size()) hot = -1;
        }
        if (hot != g_lp.hot) { g_lp.hot = hot; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int rows = (int)g_lp.items.size();
        g_lp.scroll = std::clamp(g_lp.scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120 * 3, 0, max(0, rows - 1));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int rowH = (int)(22 * g.dpi);
        int hot = g_lp.scroll + max(0, (int)p.y - (int)(4 * g.dpi)) / rowH;
        if (hot < 0 || hot >= (int)g_lp.items.size()) { ListPopClose(); return 0; }
        if (g_lp.who == 0) {
            g.textFont = g_lp.items[hot];
            ApplyTextStyleToSelection();
        } else if (g_lp.who == 2) {              // the tab strip's menus (TabPopPick)
            TabPopPick(hot);
        } else {
            bool b = FocusB();
            DocState& D = DV(b);
            int pg = PageAt(D, FocusSy() + g.ch / 3);
            if (D.n) {
                if (hot == 0) TransformApply(b, pg, 1, false, false);
                else if (hot == 1) TransformApply(b, pg, 3, false, false);
                else if (hot == 2) TransformApply(b, pg, 0, true, false);
                else if (hot == 3) TransformApply(b, pg, 0, false, true);
            }
        }
        ListPopClose();
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void ListPopOpen(HWND owner, POINT p, std::vector<std::wstring> items, int who, int selRow) {
    ListPopClose();
    g_lp.items = items; g_lp.hot = -1; g_lp.who = who;
    g_lp.scroll = std::clamp(selRow - 3, 0, max(0, (int)items.size() - 1));
    int rowH = (int)(22 * g.dpi);
    int w = (int)(200 * g.dpi);
    int rows = min(12, (int)items.size());
    int h2 = (int)(8 * g.dpi) + rows * rowH;
    g.fontPop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DocsyListPop", nullptr,
                                WS_POPUP | WS_BORDER, p.x, p.y, w, h2, owner, nullptr, g.hinst, nullptr);
    if (!g.fontPop) return;
    SetForegroundWindow(g.fontPop);
    ShowWindow(g.fontPop, SW_SHOWNA);
    InvalidateRect(g.fontPop, nullptr, FALSE);
}

// The color picker popup ("More colors..."): three sliders for R, G and B,
// the working color preview, Accept and Cancel - KillerPDF's picker reduced
// to the channels its bars actually set.
struct ColorPop { int r = 255, g = 0, b = 0, slide = -1; };
static ColorPop g_cp;
static void ColorPopClose(bool accept) {
    if (!g.colorPop) return;
    HWND h = g.colorPop; g.colorPop = nullptr;
    DestroyWindow(h);
    if (accept) {
        COLORREF c = RGB(g_cp.r, g_cp.g, g_cp.b);
        switch (g.colorTarget) {
        case 1: g.drawColor = c; ApplyDrawStyleToSelection(); break;
        case 2: g.hlRGB = c; ApplyDrawStyleToSelection(); break;
        case 3: g.textRGB = c; ApplyTextStyleToSelection(); break;
        case 4: g.textFillRGB = c; if (g.textFillA == 0) g.textFillA = 255; ApplyTextStyleToSelection(); break;
        }
    }
    AnnBarSync(true);
}
static LRESULT CALLBACK ColorPopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) ColorPopClose(false);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, PopBrush());
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, AbFont11());
        int pad = (int)(10 * g.dpi);
        COLORREF c = RGB(g_cp.r, g_cp.g, g_cp.b);
        RECT pv{ pad, pad, r.right - pad, pad + (int)(26 * g.dpi) };
        HBRUSH pb = CreateSolidBrush(c);
        FillRect(dc, &pv, pb); DeleteObject(pb);
        FrameRect(dc, &pv, ComboBorderBrush());
        const wchar_t* labs[3] = { L"R", L"G", L"B" };
        int vals[3] = { g_cp.r, g_cp.g, g_cp.b };
        for (int i = 0; i < 3; i++) {
            int y = pad + (int)(34 * g.dpi) + i * (int)(26 * g.dpi);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ pad, y, pad + (int)(14 * g.dpi), y + (int)(18 * g.dpi) };
            DrawTextW(dc, labs[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            int tx = pad + (int)(20 * g.dpi);
            RECT tr{ tx, y + (int)(7 * g.dpi), tx + (int)(90 * g.dpi), y + (int)(11 * g.dpi) };
            HBRUSH tb = CreateSolidBrush(RGB(0x3f, 0x3f, 0x3f));
            FillRect(dc, &tr, tb); DeleteObject(tb);
            int hx = tx + (int)((vals[i] / 255.0) * (90.0 * g.dpi - 12.0 * g.dpi));
            RECT th{ hx, y, hx + (int)(12 * g.dpi), y + (int)(18 * g.dpi) };
            HBRUSH hb2 = CreateSolidBrush(RGB(0x22, 0xbb, 0x56));
            FillRect(dc, &th, hb2); DeleteObject(hb2);
            wchar_t vs[8]; swprintf(vs, 8, L"%d", vals[i]);
            SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
            RECT vr{ tx + (int)(96 * g.dpi), y, tx + (int)(130 * g.dpi), y + (int)(18 * g.dpi) };
            DrawTextW(dc, vs, -1, &vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        int by = pad + (int)(34 * g.dpi) + 3 * (int)(26 * g.dpi) + (int)(6 * g.dpi);
        RECT ok{ pad, by, pad + (int)(64 * g.dpi), by + (int)(24 * g.dpi) };
        RECT no{ pad + (int)(72 * g.dpi), by, pad + (int)(72 * g.dpi) + (int)(64 * g.dpi), by + (int)(24 * g.dpi) };
        FillRect(dc, &ok, g_cp.slide == 100 ? PopHoverBrush() : ComboFillBrush());
        FrameRect(dc, &ok, ComboBorderBrush());
        FillRect(dc, &no, g_cp.slide == 101 ? PopHoverBrush() : ComboFillBrush());
        FrameRect(dc, &no, ComboBorderBrush());
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        DrawTextW(dc, L"OK", -1, &ok, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(dc, L"Cancel", -1, &no, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        FrameRect(dc, &r, ComboBorderBrush());
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int pad = (int)(10 * g.dpi);
        int tx = pad + (int)(20 * g.dpi);
        bool changed = false;
        if (g_cp.slide >= 0 && g_cp.slide < 3 && p.x >= tx && p.x <= tx + (int)(90 * g.dpi)) {
            int v = (int)std::lround(std::clamp((p.x - tx) / (90.0 * g.dpi - 12.0 * g.dpi), 0.0, 1.0) * 255);
            int* ch[3] = { &g_cp.r, &g_cp.g, &g_cp.b };
            if (*ch[g_cp.slide] != v) { *ch[g_cp.slide] = v; changed = true; }
        }
        int hot = -1;
        if (p.y > r.bottom - (int)(34 * g.dpi)) hot = p.x < pad + (int)(68 * g.dpi) ? 100 : 101;
        if (hot != g_cp.slide && hot >= 0) g_cp.slide = hot;
        if (changed || hot >= 0) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int pad = (int)(10 * g.dpi);
        int tx = pad + (int)(20 * g.dpi);
        for (int i = 0; i < 3; i++) {
            int y = pad + (int)(34 * g.dpi) + i * (int)(26 * g.dpi);
            if (p.y >= y - 2 && p.y < y + (int)(20 * g.dpi) && p.x >= tx - 6 && p.x <= tx + (int)(92 * g.dpi)) {
                g_cp.slide = i;
                int v = (int)std::lround(std::clamp((p.x - tx) / (90.0 * g.dpi - 12.0 * g.dpi), 0.0, 1.0) * 255);
                int* ch[3] = { &g_cp.r, &g_cp.g, &g_cp.b };
                *ch[i] = v;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        if (p.y > r.bottom - (int)(34 * g.dpi))
            ColorPopClose(p.x < pad + (int)(68 * g.dpi));
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void ColorPickerOpen(int target, COLORREF cur) {
    if (g.colorPop) ColorPopClose(false);
    g.colorTarget = target;
    g_cp.r = GetRValue(cur); g_cp.g = GetGValue(cur); g_cp.b = GetBValue(cur);
    g_cp.slide = -1;
    int pad = (int)(10 * g.dpi);
    int w = pad * 2 + (int)(150 * g.dpi), h2 = (int)(34 * g.dpi) + 3 * (int)(26 * g.dpi) + (int)(38 * g.dpi) + pad;
    RECT br = AnnBarRect();
    g.colorPop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DocsyColorPop", nullptr,
                                 WS_POPUP | WS_BORDER, br.right - w, br.bottom + 4, w, h2,
                                 g.main, nullptr, g.hinst, nullptr);
    if (!g.colorPop) return;
    SetForegroundWindow(g.colorPop);
    ShowWindow(g.colorPop, SW_SHOWNA);
    InvalidateRect(g.colorPop, nullptr, FALSE);
}

// The Transform menu (the toolbar button): quarter turns and flips on the
// focused pane's current page - KillerPDF's rasterizing TransformWindow,
// reduced to the session transforms docsy can apply without re-encoding.
static void TransformPopupOpen() {
    DocState& D = DVF();
    if (!D.n) return;
    RECT wr; GetWindowRect(g.bar, &wr);
    RECT bs[TB_COUNT]; TbRects(wr.right - wr.left, bs);
    int x = bs[TB_TRANSFORM].left, y = bs[TB_TRANSFORM].bottom;
    ListPopOpen(g.main, { wr.left + x, wr.top + y + 2 },
                { L"Rotate 90\u00B0 clockwise", L"Rotate 90\u00B0 counter-clockwise",
                  L"Flip horizontal", L"Flip vertical" }, 1, -1);
}

// The size box: a tiny popup EDIT over the cell; Enter/focus-loss commits
// (clamped 1-400pt), Escape reverts - KillerPDF's CommitSizeBox. The subclass
// chains to the stock EDIT proc (g_sizeEditDef, the g.teditDef pattern):
// DefWindowProcW implements no editing at all, so the box once ate every key -
// typing, Backspace, Del, Ctrl+A all landed in the void.
static WNDPROC g_sizeEditDef = nullptr;
static LRESULT CALLBACK SizeEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(h, EM_SETSEL, 0, -1); return 0;   // the reference's SelectAll chord
        }
        if (wp == VK_RETURN) {
            wchar_t b[8] = L"";
            GetWindowTextW(h, b, 8);
            g.textSize = std::clamp((float)_wtof(b), 1.0f, 400.0f);
            ApplyTextStyleToSelection();
            DestroyWindow(h); g.sizeEdit = nullptr;
            return 0;
        }
        if (wp == VK_ESCAPE) { DestroyWindow(h); g.sizeEdit = nullptr; return 0; }
    }
    if (m == WM_KILLFOCUS) {
        wchar_t b[8] = L"";
        GetWindowTextW(h, b, 8);
        if (b[0]) {
            g.textSize = std::clamp((float)_wtof(b), 1.0f, 400.0f);
            ApplyTextStyleToSelection();
        }
        DestroyWindow(h); g.sizeEdit = nullptr;
        return 0;
    }
    return g_sizeEditDef ? CallWindowProcW(g_sizeEditDef, h, m, wp, lp)
                         : DefWindowProcW(h, m, wp, lp);
}
static void SizeEditOpen(POINT p) {
    if (g.sizeEdit) { DestroyWindow(g.sizeEdit); g.sizeEdit = nullptr; }
    g.sizeEdit = CreateWindowExW(WS_EX_TOPMOST, L"EDIT", nullptr,
                                 WS_POPUP | WS_BORDER | ES_NUMBER,
                                 p.x, p.y, (int)(32 * g.dpi), (int)(20 * g.dpi),
                                 g.main, nullptr, g.hinst, nullptr);
    if (!g.sizeEdit) return;
    wchar_t b[8]; swprintf(b, 8, L"%d", (int)g.textSize);
    SetWindowTextW(g.sizeEdit, b);
    SendMessageW(g.sizeEdit, EM_SETLIMITTEXT, 4, 0);
    g_sizeEditDef = (WNDPROC)GetWindowLongPtrW(g.sizeEdit, GWLP_WNDPROC);
    SetWindowLongPtrW(g.sizeEdit, GWLP_WNDPROC, (LONG_PTR)SizeEditProc);
    SendMessageW(g.sizeEdit, WM_SETFONT, (WPARAM)AbFont11(), TRUE);
    SetForegroundWindow(g.sizeEdit);
    SetFocus(g.sizeEdit);
    ShowWindow(g.sizeEdit, SW_SHOW);
}

// The paragraph editor ("Edit paragraph"): the selected text box's content in
// a multiline field - KillerPDF's ParagraphEditDialog reduced to what a
// session overlay can rewrite (the box's own text).
struct ParaDlg { int hot = -1; };
static ParaDlg g_pd;
static void ParaClose(bool apply) {
    if (!g.paraDlg) return;
    HWND h = g.paraDlg; g.paraDlg = nullptr;
    HWND ed = GetWindow(h, GW_CHILD);
    std::wstring s;
    if (ed) {
        int len = GetWindowTextLengthW(ed);
        s.resize(len); GetWindowTextW(ed, s.data(), len + 1);
    }
    DestroyWindow(h);
    if (apply && g.annSelPage >= 0 && g.annSelIdx >= 0) {
        DocState& D = DV(g.annSelB);
        auto it = D.annos.find(g.annSelPage);
        if (it != D.annos.end() && g.annSelIdx < (int)it->second.size() &&
            it->second[g.annSelIdx].type == AT_TEXT) {
            UndoPush(g.annSelB);
            Annot& a = it->second[g.annSelIdx];
            a.text = s;
            float k = (float)(D.zoom * g.dpi);
            HDC m = GetDC(g.view);
            int hpx = max(8, (int)std::lround(a.fsize * k));
            HFONT f = CreateFontW(-hpx, 0, 0, 0, a.bold ? FW_BOLD : FW_NORMAL,
                                  a.italic, a.under, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, a.font.c_str());
            HGDIOBJ of = SelectObject(m, f);
            RECT mr{ 0, 0, (int)(a.w * k), 10000 };
            DrawTextW(m, a.text.c_str(), -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            SelectObject(m, of); DeleteObject(f); ReleaseDC(g.view, m);
            a.h = max(24.0f, (mr.bottom - mr.top) / max(0.01f, k));
        }
    } else if (apply) SetStatusHold(L"Select a text box first");
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}
static LRESULT CALLBACK ParaDlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        SetBkColor(dc, RGB(0x1c, 0x1c, 0x1c));
        static HBRUSH b = CreateSolidBrush(RGB(0x1c, 0x1c, 0x1c));
        return (LRESULT)b;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_KILLFOCUS) return 0;   // clicking Apply must not kill the edit's text
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, PopBrush());
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, AbFont11());
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        RECT tr{ (int)(10 * g.dpi), (int)(8 * g.dpi), r.right, (int)(24 * g.dpi) };
        DrawTextW(dc, L"Edit paragraph", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int by = r.bottom - (int)(34 * g.dpi);
        RECT ok{ (int)(10 * g.dpi), by, (int)(10 * g.dpi) + (int)(64 * g.dpi), by + (int)(24 * g.dpi) };
        RECT no{ (int)(82 * g.dpi), by, (int)(82 * g.dpi) + (int)(64 * g.dpi), by + (int)(24 * g.dpi) };
        FillRect(dc, &ok, g_pd.hot == 1 ? PopHoverBrush() : ComboFillBrush());
        FrameRect(dc, &ok, ComboBorderBrush());
        FillRect(dc, &no, g_pd.hot == 2 ? PopHoverBrush() : ComboFillBrush());
        FrameRect(dc, &no, ComboBorderBrush());
        DrawTextW(dc, L"Apply", -1, &ok, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(dc, L"Close", -1, &no, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        FrameRect(dc, &r, ComboBorderBrush());
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int by = r.bottom - (int)(34 * g.dpi);
        int hot = p.y >= by && p.y < by + (int)(24 * g.dpi)
                ? (p.x < (int)(76 * g.dpi) ? 1 : p.x < (int)(150 * g.dpi) ? 2 : 0) : 0;
        if (hot != g_pd.hot) { g_pd.hot = hot; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int by = r.bottom - (int)(34 * g.dpi);
        if (p.y >= by && p.y < by + (int)(24 * g.dpi))
            ParaClose(p.x < (int)(76 * g.dpi));
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}
// The paragraph field's keys: the reference's WPF TextBox selects all on
// Ctrl+A - a plain EDIT ships no such chord (the find box's precedent) - and
// everything else stays the stock edit's own: Del, Backspace, caret moves.
static LRESULT CALLBACK ParaEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN && wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    WNDPROC orig = (WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA);
    return orig ? CallWindowProcW(orig, h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}
static void ParagraphOpen() {
    if (g.paraDlg) ParaClose(false);
    std::wstring cur;
    if (g.annSelPage >= 0 && g.annSelIdx >= 0) {
        DocState& D = DV(g.annSelB);
        auto it = D.annos.find(g.annSelPage);
        if (it != D.annos.end() && g.annSelIdx < (int)it->second.size() &&
            it->second[g.annSelIdx].type == AT_TEXT)
            cur = it->second[g.annSelIdx].text;
    }
    RECT br = AnnBarRect();
    int w = (int)(360 * g.dpi), h2 = (int)(220 * g.dpi);
    g.paraDlg = CreateWindowExW(WS_EX_TOOLWINDOW, L"DocsyPara", nullptr,
                                WS_POPUP | WS_BORDER, br.right - w, br.bottom + 4, w, h2,
                                g.main, nullptr, g.hinst, nullptr);
    if (!g.paraDlg) return;
    HWND ed = CreateWindowExW(0, L"EDIT", cur.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_MULTILINE |
                              ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
                              (int)(10 * g.dpi), (int)(28 * g.dpi), w - (int)(20 * g.dpi),
                              h2 - (int)(72 * g.dpi), g.paraDlg, nullptr, g.hinst, nullptr);
    SendMessageW(ed, WM_SETFONT, (WPARAM)UiFont(), TRUE);
    SendMessageW(ed, EM_SETLIMITTEXT, 10000, 0);
    SetWindowLongPtrW(ed, GWLP_USERDATA, GetWindowLongPtrW(ed, GWLP_WNDPROC));
    SetWindowLongPtrW(ed, GWLP_WNDPROC, (LONG_PTR)ParaEditProc);
    ShowWindow(g.paraDlg, SW_SHOWNA);
    InvalidateRect(g.paraDlg, nullptr, FALSE);
}

// ---- the bar's interactions, driven by the view's mouse messages ----
static int AnnBarSliderAt(int id) {   // the track's left edge, bar-local
    for (const AbCell& c : g_ab) if (c.id == id) return c.r.left;
    return 0;
}
static void AnnBarSlideValue(HWND h, POINT p) {
    RECT br = AnnBarRect();
    int lx = AnnBarSliderAt(g.barSlide);
    float t = std::clamp((p.x - br.left - lx) / (90.0f * (float)g.dpi), 0.0f, 1.0f);
    switch (g.barSlide) {
    case AB_SIZE: {
        int v = std::clamp(1 + (int)std::lround(t * 59), 1, 60);
        if (g.drawWidth != v) { g.drawWidth = (float)v; ApplyDrawStyleToSelection(); }
        break;
    }
    case AB_OPACITY: {
        int v = std::clamp(10 + (int)std::lround(t * 245), 10, 255);
        if (g.drawOpacity != v) { g.drawOpacity = (BYTE)v; ApplyDrawStyleToSelection(); }
        break;
    }
    case AB_SPACING: {
        float v = -2 + t * 22;
        v = std::round(v * 4) / 4;
        if (g.textSpacing != v) { g.textSpacing = v; ApplyTextStyleToSelection(); }
        break;
    }
    case AB_TOACITY: {
        int v = std::clamp(10 + (int)std::lround(t * 245), 10, 255);
        if (g.textA != v) { g.textA = (BYTE)v; ApplyTextStyleToSelection(); }
        break;
    }
    case AB_FOACITY: {
        int v = std::clamp(10 + (int)std::lround(t * 245), 10, 255);
        if (g.textFillA != v) { g.textFillA = (BYTE)v; ApplyTextStyleToSelection(); }
        break;
    }
    case AB_TSIZE: {
        float v = 8 + t * 64;
        if (g.textSize != v) { g.textSize = v; ApplyTextStyleToSelection(); }
        break;
    }
    }
    AnnBarSync(true);
}
static void AnnBarPress(HWND h, POINT p, int cell) {
    switch (cell) {
    case AB_GRIP:
        g.barDrag = true; g.barDragX = p.x; g.barDx0 = g.barDx;
        return;
    case AB_SIZE: case AB_OPACITY: case AB_SPACING: case AB_TOACITY: case AB_FOACITY: case AB_TSIZE:
        g.barSlide = cell; g.barSlideX0 = p.x;
        AnnBarSlideValue(h, p);
        return;
    case AB_RW_DRAW: ColorPickerOpen(1, g.drawColor); return;
    case AB_RW_TEXT: ColorPickerOpen(3, g.textRGB); return;
    case AB_RW_FILL: ColorPickerOpen(4, g.textFillRGB); return;
    case AB_FILL_NONE:
        g.textFillA = 0;
        ApplyTextStyleToSelection();
        return;
    case AB_LEVEL: g.lineLevel = !g.lineLevel; AnnBarSync(true); return;
    case AB_ERASE: g.drawErase = !g.drawErase; AnnBarSync(true); return;
    case AB_HLERASE: g.hlErase = !g.hlErase; AnnBarSync(true); return;
    case AB_FILLCHK: g.shapeFill = !g.shapeFill; AnnBarSync(true); return;
    case AB_KIND0: case AB_KIND1: case AB_KIND2:
        g.shapeKind = cell - AB_KIND0;
        AnnBarSync(true);
        return;
    case AB_FONT: {
        RECT br = AnnBarRect();
        for (const AbCell& c : g_ab) if (c.id == AB_FONT) {
            ListPopOpen(g.main, { br.left + c.r.left, br.bottom + 4 }, FontList(), 0, -1);
            break;
        }
        return;
    }
    case AB_BOLD: g.textBold = !g.textBold; ApplyTextStyleToSelection(); return;
    case AB_ITALIC: g.textItalic = !g.textItalic; ApplyTextStyleToSelection(); return;
    case AB_STRIKE: g.textStrike = !g.textStrike; ApplyTextStyleToSelection(); return;
    case AB_UNDER: g.textUnder = !g.textUnder; ApplyTextStyleToSelection(); return;
    case AB_TMINUS: g.textSize = std::clamp(g.textSize - 1, 1.0f, 400.0f); ApplyTextStyleToSelection(); return;
    case AB_TPLUS: g.textSize = std::clamp(g.textSize + 1, 1.0f, 400.0f); ApplyTextStyleToSelection(); return;
    case AB_TBOX: {
        RECT br = AnnBarRect();
        for (const AbCell& c : g_ab) if (c.id == AB_TBOX) {
            SizeEditOpen({ br.left + c.r.left, br.top + c.r.top });
            break;
        }
        return;
    }
    case AB_PARA: ParagraphOpen(); return;
    default:
        if (cell >= AB_SWC_DRAW && cell < AB_RW_DRAW) {
            COLORREF c = AbCellColor(cell);
            if (g.barTool == TOOL_HIGHLIGHT) g.hlRGB = c;   // the highlight bar retints its own
            else g.drawColor = c;
            ApplyDrawStyleToSelection();
            AnnBarSync(true);
        } else if (cell >= AB_SWC_TEXT && cell < AB_RW_TEXT) {
            g.textRGB = AbCellColor(cell);
            ApplyTextStyleToSelection();
        } else if (cell >= AB_SWC_FILL && cell < AB_RW_FILL) {
            g.textFillRGB = AbCellColor(cell);
            if (g.textFillA == 0) g.textFillA = 255;
            ApplyTextStyleToSelection();
        }
        return;
    }
}

