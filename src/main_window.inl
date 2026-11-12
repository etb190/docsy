// ---------------- main window ----------------

static void LayoutMain() {
    if (!g.main || !g.view) return;
    RECT r; GetClientRect(g.main, &r);
    int w = r.right, h = r.bottom;
    // The children's OLD rects, captured before the moves. When a child moves
    // or shrinks, the pixels it vacates are NOT repainted by anyone: the
    // window system hands them to whichever sibling now covers them, but that
    // sibling's own rect did not change, so it never repaints - the rail's
    // last frame then lives on as ghost buttons stacked over the sidebar, and
    // a resized sidebar's stale edge reads as a border that belongs to no one.
    // After the moves, every vacated strip is handed to the sibling that now
    // owns it (the parent for whatever no child covers). The strips are thin
    // bands - a layout pass moves edges, never documents - so this stays a
    // few InvalidateRects, not a repaint.
    struct Kid { HWND w; RECT was; } kids[7] = { { g.bar, {} }, { g_wtool, {} }, { g_wstrip, {} }, { g.side, {} }, { g.rail, {} }, { g.view, {} }, { g.status, {} } };
    for (auto& k : kids)
        if (k.w) {
            GetWindowRect(k.w, &k.was);
            ScreenToClient(g.main, (POINT*)&k.was);
            ScreenToClient(g.main, ((POINT*)&k.was) + 1);
        }
    int sw = g_webOn ? (g_webSide ? SW() : 0) : ((g.sideOn && DVF().n && !g.full) ? SW() : 0);   // the sidebar shows the FOCUSED
    int rw = RW();                              // pane's pages; the rail always rides between
    int cap0 = CAP();
    int tl = WEBTOOLH();                        // browser mode: the reader's toolbar row, one button
    int cap = cap0 + tl;                        // everything below sits under caption + toolbar
    int tb = g_webOn ? WebBandH() : TBH();      // browser mode: the tab band - there only
                                                // from the second tab, like the reader's
    int ws_ = WEBSTRIPH();                      // the browser's toolbar rides above it
    int sh = (g.full || g_webOn) ? 0 : SBH();   // full screen and the browser hide the footer
    int ch = max(0, h - cap - ws_ - tb - sh);   // the children sit below caption + strip + toolbar
    sd.w = sw;
    if (g_webOn) {
        if (g_wtool) MoveWindow(g_wtool, 0, cap0, w, tl, TRUE);
        if (g_wstrip) MoveWindow(g_wstrip, 0, cap, w, ws_, TRUE);
        if (g.bar) MoveWindow(g.bar, 0, cap + ws_, w, tb, TRUE);
    } else if (g.bar) {
        MoveWindow(g.bar, 0, cap, w, tb, TRUE);
    }
    bool sideResized = false;
    if (g.side) {
        MoveWindow(g.side, 0, cap + ws_ + tb, sw, ch, TRUE);
        if (sw) {
            RECT sr; GetClientRect(g.side, &sr);
            if (sr.right != sd.cw || sr.bottom != sd.ch) { sd.cw = sr.right; sd.ch = sr.bottom; sideResized = true; }
        }
    }
    if (g_webOn) {
        if (sw) { ws.cw = sd.cw; ws.ch = sd.ch; WebSideLayout(); }   // the rail's web state
    } else if (sideResized) SideLayout();                            // tracks the same window
    if (g.rail) {                               // the rail: between the sidebar and the page,
        MoveWindow(g.rail, sw, cap + ws_ + tb, rw, ch, TRUE);   // always there but in full screen
        ShowWindow(g.rail, rw > 0 ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    MoveWindow(g.view, sw + rw, cap + ws_ + tb, max(0, w - sw - rw), ch, TRUE);
    if (g.status) MoveWindow(g.status, 0, cap + ws_ + tb + ch, w, sh, TRUE);
    // Hand the vacated strips out. A moved child leaves up to four strips
    // (old ∖ new); each is intersected with every sibling's NEW rect - that
    // sibling repaints it in its own coords - and the parent mops up whatever
    // no child covers (WS_CLIPCHILDREN limits that paint to the exposed area,
    // so it is the grain fill and nothing else).
    for (auto& k : kids) {
        if (!k.w) continue;
        RECT now; GetWindowRect(k.w, &now); ScreenToClient(g.main, (POINT*)&now);
        ScreenToClient(g.main, ((POINT*)&now) + 1);
        if (EqualRect(&k.was, &now)) continue;
        RECT vac[4]; int nv = 0;
        if (now.left > k.was.left)
            vac[nv++] = { k.was.left, k.was.top, min(now.left, k.was.right), k.was.bottom };
        if (now.right < k.was.right)
            vac[nv++] = { max(now.right, k.was.left), k.was.top, k.was.right, k.was.bottom };
        if (now.top > k.was.top)
            vac[nv++] = { max(k.was.left, now.left), k.was.top,
                          min(k.was.right, now.right), min(now.top, k.was.bottom) };
        if (now.bottom < k.was.bottom)
            vac[nv++] = { max(k.was.left, now.left), max(now.bottom, k.was.top),
                          min(k.was.right, now.right), k.was.bottom };
        for (int i = 0; i < nv; i++) {
            // route the strip: every sibling whose CURRENT rect covers part of it
            RECT is;
            for (auto& o : kids) {
                if (!o.w || o.w == k.w) continue;
                RECT o2; GetWindowRect(o.w, &o2); ScreenToClient(g.main, (POINT*)&o2);
                ScreenToClient(g.main, ((POINT*)&o2) + 1);
                if (IntersectRect(&is, &vac[i], &o2)) InvalidateRect(o.w, &is, FALSE);
            }
            InvalidateRect(g.main, &vac[i], FALSE);   // the exposed remainder, if any
        }
    }
    if (g_fedit && fd.open) FindPlace();        // the edit tracks the focused card's top-right corner
    if (g.tedit) CommitTextEdit();              // the text box rides nothing: a relayout lands it,
                                                // its pixels belong to a page position, not a screen one
    RECT vr; GetClientRect(g.view, &vr);
    g.cw = vr.right; g.ch = vr.bottom;
    if (g.split) {   // a resize re-normalizes the split: the divider keeps KillerPDF's
        g.splitX = std::clamp(g.splitX, PaneMin(),   // minimum panes, not a span fraction
                              max(PaneMin(), g.cw - SplitHalf() - PaneMin()));
        UpdateScroll(g.d, false);               // each pane re-clamps into its own document
        UpdateScroll(g.dB, true);               // at its own card width
    } else {
        UpdateScroll(g.d, false);
    }
    if (!g.resizing && !g.busy && g.cw > 0 && g.ch > 0) {
        if (g_webOn) { WebBarLayout(); WebPlace(); WebResizeDeb(); }   // the browser parks its omnibox
        // each pane refits ITSELF on a resize, at its own card width and its
        // own fit mode - two documents do not share a scale
        for (int pass = 0; pass < (g.split ? 2 : 1); pass++) {
            bool b = pass == 1;
            DocState& D = DV(b);
            if (D.n && D.fitMode != 2) ApplyFit(D.fitMode == 1, b);
            else if (D.n) Layout(D, b);
        }
        UpdateTitle();
    }
}

static void ToggleSide() {
    if (!DVF().n) return;
    g.sideOn = !g.sideOn;
    ShowWindow(g.side, g.sideOn ? SW_SHOWNOACTIVATE : SW_HIDE);
    RailRepaint();                        // the Pages button lights and dims with the sidebar
    LayoutMain();
}

static void ZoomBy(double f) { SetZoom(DVF().zoom * f, g.cw / 2, g.ch / 2); }

// KillerPDF's NudgeSelection: with a Select-tool annotation chosen, the arrow
// keys move it (Shift = 10pt steps) instead of scrolling the pane.
static bool NudgeSel(WPARAM wp, bool shift) {
    if (g.annSelPage < 0 || g.annSelIdx < 0) return false;
    DocState& D = DV(g.annSelB);
    auto it = D.annos.find(g.annSelPage);
    if (it == D.annos.end() || g.annSelIdx >= (int)it->second.size()) return false;
    UndoPush(g.annSelB);
    float step = shift ? 10.0f : 1.0f;
    float dx = wp == VK_LEFT ? -step : wp == VK_RIGHT ? step : 0;
    float dy = wp == VK_UP ? -step : wp == VK_DOWN ? step : 0;
    Annot& a = it->second[g.annSelIdx];
    if (a.type == AT_HIGHLIGHT) {
        for (FRect& r : a.lines) { r.x += dx; r.y += dy; }
        a.x += dx; a.y += dy;
    } else if (a.type == AT_INK) {
        for (size_t i = 0; i < a.ix.size(); i++) { a.ix[i] += dx; a.iy[i] += dy; }
    } else { a.x += dx; a.y += dy; }
    InvalidateRect(g.view, nullptr, FALSE);
    return true;
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g.main = h; g.dpi = GetDpiForWindow(h) / 96.0;
        g.view = CreateWindowExW(0, L"DocsyView", nullptr,
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        g.side = CreateWindowExW(0, L"DocsySide", nullptr,
                                 WS_CHILD,
                                 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        g.status = CreateWindowExW(0, L"DocsyStatus", nullptr,
                                   WS_CHILD | WS_VISIBLE,
                                   0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        g.bar = CreateWindowExW(0, L"DocsyBar", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,   // CLIPCHILDREN: the omnibox
                                0, 0, 0, 0, h, nullptr, g.hinst, nullptr);   // edit lives on this row
        g.rail = CreateWindowExW(0, L"DocsyRail", nullptr,
                                 WS_CHILD | WS_VISIBLE,
                                 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        g_wtool = CreateWindowExW(0, L"DocsyWebTool", nullptr,     // browser mode's reader toolbar
                                  WS_CHILD, 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        g_wstrip = CreateWindowExW(0, L"DocsyWebStrip", nullptr,   // the browser's Safari toolbar,
                                   WS_CHILD, 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);   // hidden until Web
        DragAcceptFiles(h, TRUE); DragAcceptFiles(g.view, TRUE); DragAcceptFiles(g.side, TRUE);
        DwmApply(h);   // rounded corners + themed 1px border (Windows 11; no-op before)
        return 0;
    }
    case WM_SIZE:
        LayoutMain();
        InvalidateCap(h);   // the buttons anchor to the right edge: a resize only
                            // invalidates the exposed strip, so without this the
                            // cluster vanishes until a hover repaints it
        DwmApply(h);   // corners square off while maximized, round again on restore
        return 0;
    case WM_PAINT: {   // the custom caption row; the children paint their own areas
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT clip;                                   // a shrinking window exposes strips
        if (GetClipBox(dc, &clip) != NULLREGION) {   // the children vacated: fill them with
            FillRect(dc, &clip, GrainBrush(1));      // the shared grain or the last frame
        }                                            // lingers there (the ghost button)
        RECT r; GetClientRect(h, &r);
        PaintCaptionInto(dc, r.right);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {   // DWM re-materializes the surface (peek, taskbar thumbnails,
        HDC dc = (HDC)wp;    // occlusion, snap previews) WITHOUT invalidating first, which
        RECT r; GetClientRect(h, &r);   // blanked the band until the next re-hover
        PaintCaptionInto(dc, r.right);
        return 0;
    }
    case WM_NCACTIVATE: {    // activation flips are when DWM re-evaluates the frame; a
        LRESULT r = DefWindowProcW(h, m, wp, lp);   // band repaint keeps the glyphs fresh
        InvalidateCap(h);
        return r;
    }
    case WM_NCCALCSIZE: {
        // KillerPDF's frame: keep the real OS side and bottom borders, give the
        // caption row to the client. A maximized window clamps to the work area
        // so the stripped frame cannot overhang the taskbar or the screen edges.
        if (!wp || IsIconic(h)) break;
        NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
        RECT orig = p->rgrc[0];
        if (IsZoomed(h)) {
            HMONITOR mon = MonitorFromRect(&orig, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof mi };
            if (!mon || !GetMonitorInfoW(mon, &mi)) break;
            p->rgrc[0].left   = max(orig.left,   mi.rcWork.left);
            p->rgrc[0].top    = max(orig.top,    mi.rcWork.top);
            p->rgrc[0].right  = min(orig.right,  mi.rcWork.right);
            p->rgrc[0].bottom = min(orig.bottom, mi.rcWork.bottom);
            return 0;
        }
        DefWindowProcW(h, m, wp, lp);          // the OS lays out its full frame...
        g.osFrame = p->rgrc[0].left - orig.left;
        p->rgrc[0].top = orig.top;             // ...and only the caption is dropped
        return 0;
    }
    case WM_NCHITTEST: {
        // KillerPDF's classification order: caption controls first (they must
        // stay client input, and the maximize button answers HTMAXBUTTON so
        // Windows offers the Win11 snap-layout flyout), then the top resize
        // grip that replaced the removed OS border, then the drag row, and the
        // real side/bottom borders come from DefWindowProc.
        POINT s{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc; GetWindowRect(h, &rc);        // for the top-grip comparison (screen coords)
        RECT cc; GetClientRect(h, &cc);        // the buttons live in client coordinates
        POINT c = s; ScreenToClient(h, &c);
        int which;
        if (CapHit(c, cc.right, which)) return which == 1 ? HTMAXBUTTON : HTCLIENT;
        if (!IsZoomed(h) && g.osFrame > 0 && s.y < rc.top + g.osFrame) {
            if (s.x <  rc.left + g.osFrame) return HTTOPLEFT;
            if (s.x >= rc.right - g.osFrame) return HTTOPRIGHT;
            return HTTOP;
        }
        if (c.y >= 0 && c.y < CAP()) return HTCAPTION;
        break;
    }
    case WM_NCMOUSEMOVE: {                     // hover lives in the non-client zone too
        bool wasHot = g.capHot == 1;
        SetCapHot(h, wp == HTMAXBUTTON ? 1 : -1);   // repaints only on a hover flip
        if (wp == HTMAXBUTTON && !wasHot) {    // arm the leave tracker once per hover
            TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE | TME_NONCLIENT, h, 0 };   // session:
            TrackMouseEvent(&t);               // re-arming it on every move makes the
        }                                      // leave/enter churn that strobes the button
        break;
    }
    case WM_NCMOUSELEAVE:
        g.capPressed = false;
        SetCapHot(h, -1);
        break;
    case WM_NCLBUTTONDOWN:
        // Claim the press, or Windows runs its own caption-button loop and
        // paints a stock button over ours (SnapLayouts.cs does the same).
        if (wp == HTMAXBUTTON) { g.capPressed = true; InvalidateCap(h); return 0; }
        break;
    case WM_NCLBUTTONUP:
        if (wp == HTMAXBUTTON) {
            bool was = g.capPressed;
            g.capPressed = false; InvalidateCap(h);
            if (was) ToggleMax(h);
            return 0;
        }
        if (g.capPressed) { g.capPressed = false; InvalidateCap(h); }   // released off the button
        break;
    case WM_MOUSEMOVE: {                       // the client part of the bar: min and close
        POINT c{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int which;
        int hot = CapHit(c, r.right, which) ? which : -1;
        bool wasHot = g.capHot >= 0;
        SetCapHot(h, hot);                     // repaints only on a hover flip
        if (hot >= 0 && !wasHot) TrackLeave(h);   // arm leave-tracking once per hover
        break;
    }
    case WM_MOUSELEAVE:
        SetCapHot(h, -1);
        break;
    case WM_LBUTTONDOWN: {
        POINT c{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int which;
        if (CapHit(c, r.right, which)) {
            if (which == 0) ShowWindow(h, SW_MINIMIZE);
            else if (which == 2) PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    }
    case WM_SETCURSOR:
        // The caption buttons take KillerPDF's hand cursor. The maximize button
        // answers HTMAXBUTTON, not HTCLIENT, so the check must cover both hit
        // codes - otherwise Windows never sets a cursor over it and the
        // pointer vanishes on the middle button.
        if (LOWORD(lp) == HTCLIENT || LOWORD(lp) == HTMAXBUTTON) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT r; GetClientRect(h, &r);
            int which;
            if (CapHit(p, r.right, which)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        }
        break;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp) / 96.0;
        RECT* r = (RECT*)lp;
        ClearCache(g.d); ClearCache(g.dB);        // both panes re-render at the new scale
        if (g_uifont) { DeleteObject(g_uifont); g_uifont = nullptr; }
        if (g_sidefont) { DeleteObject(g_sidefont); g_sidefont = nullptr; }
        if (g_glyphfont) { DeleteObject(g_glyphfont); g_glyphfont = nullptr; }
        if (g_wordfont) { DeleteObject(g_wordfont); g_wordfont = nullptr; }
        if (g_tbglyph) { DeleteObject(g_tbglyph); g_tbglyph = nullptr; }
        if (g_tbcap) { DeleteObject(g_tbcap); g_tbcap = nullptr; }
        if (g_stfont) { DeleteObject(g_stfont); g_stfont = nullptr; }
        if (g_stfontL) { DeleteObject(g_stfontL); g_stfontL = nullptr; }
        if (g_stfontF) { DeleteObject(g_stfontF); g_stfontF = nullptr; }
        if (g_stcombo) { DeleteObject(g_stcombo); g_stcombo = nullptr; }
        if (g_stglyph) { DeleteObject(g_stglyph); g_stglyph = nullptr; }
        if (g_stchev) { DeleteObject(g_stchev); g_stchev = nullptr; }
        for (int slot = 0; slot < 2; slot++)      // both panes' backdrop bakes die: they
            if (g_bdBmp[slot]) { DeleteObject(g_bdBmp[slot]); g_bdBmp[slot] = nullptr;   // re-bake at the new DPI
                                 g_bdW[slot] = g_bdH[slot] = 0; g_bdDpi[slot] = 0; g_bdIns[slot] = -1; }
        if (g_fntEdit) { DeleteObject(g_fntEdit); g_fntEdit = nullptr; }
        if (g_fntOpt) { DeleteObject(g_fntOpt); g_fntOpt = nullptr; }
        if (g_fntAb) { DeleteObject(g_fntAb); g_fntAb = nullptr; }
        if (g_fntStat) { DeleteObject(g_fntStat); g_fntStat = nullptr; }
        if (g_fntGlyph) { DeleteObject(g_fntGlyph); g_fntGlyph = nullptr; }
        if (g_askCap) { DeleteObject(g_askCap); g_askCap = nullptr; }   // the ask dialog's four
        if (g_askMsg) { DeleteObject(g_askMsg); g_askMsg = nullptr; }   // faces re-land at the
        if (g_askBtn) { DeleteObject(g_askBtn); g_askBtn = nullptr; }   // new scale on next use
        if (g_askBang) { DeleteObject(g_askBang); g_askBang = nullptr; }
        if (g_fedit) SendMessageW(g_fedit, WM_SETFONT, (WPARAM)FdFontEdit(), TRUE);
        for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
        sd.th.clear(); sd.thBytes = 0; sd.tw = 0;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutMain();
        return 0;
    }
    case WM_GETMINMAXINFO: {   // the footer cluster needs its width; while the split is on,
        MINMAXINFO* mmi = (MINMAXINFO*)lp;   // both panes keep KillerPDF's 320px minimum -
        LONG minW = (LONG)(780 * g.dpi);     // the window's own min track enforces it
        if (g.split && !g.full) {
            LONG sides = (LONG)((g.sideOn && DVF().n ? SW() : 0) + RW());
            LONG panes = (LONG)(PaneMin() * 2 + SplitGap());
            minW = max(minW, sides + panes + (LONG)(2 * g.dpi));   // + the OS side borders
        }
        mmi->ptMinTrackSize.x = minW;
        mmi->ptMinTrackSize.y = (LONG)(380 * g.dpi);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEWHEEL: {
        if (g_webOn) return 0;               // the page scrolls itself: the wheel belongs to Chromium
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &p);
        if (g.sideOn && DVF().n && p.x < sd.w) {
            SideScrollTo(sd.scroll - GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120);
            return 0;
        }
        ViewWheel(wp, lp);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        if (g_webOn) return 0;
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(g.view, &p);
        bool b = PaneB(p.x);
        DocState& D = DV(b);
        ScrollPane(b, D.sx + GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 80) / 120, D.sy);
        return 0;
    }
    case WM_SYSKEYDOWN:                      // Alt+Left / Alt+Right: the browser's history
        if (g_webOn && WebSysKey(wp)) return 0;
        break;
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (g_webOn) { WebKey(wp, ctrl, shift); return 0; }   // the browser owns the keyboard
        int line = (int)(56 * g.dpi), pg = max(1, (int)(g.ch * 0.9));
        int& fx = PaneSx(FocusB()); int& fy = PaneSy(FocusB());   // the keys steer the focused pane
        switch (wp) {
        case 'O':
            if (ctrl) OpenDlg();
            else if (shift && DVF().n) {           // KillerPDF's CycleShapeKind: Shift+O walks
                g.shapeKind = (g.shapeKind + 1) % 3;   // Box -> Ellipse -> Freeform -> Box
                if (DVF().tool == TOOL_SHAPE) AnnBarSync(true);
            } else if (DVF().n) SetTool(TOOL_SHAPE, FocusB());
            break;
        case 'N':                                  // KillerPDF's file cluster: Ctrl+N / Ctrl+W
            if (ctrl && !shift) NewBlank();
            break;
        case 'W':
            if (ctrl && shift) CloseOtherTabs(FocusB(), Act(FocusB()));   // Close Others
            else if (ctrl) CloseDoc();
            break;
        case 'Q':
            if (ctrl && shift) CloseAllTabs(FocusB());   // Ctrl+Shift+Q: close every tab
            break;
        case VK_TAB:                               // Ctrl+Tab / Ctrl+Shift+Tab cycle the
            if (ctrl) CycleTab(FocusB(), shift ? -1 : 1);   // focused pane's document tabs
            break;
        case 'S':
            if (ctrl) SaveDoc(shift);              // Ctrl+S saves in place, Ctrl+Shift+S asks
            break;
        // KillerPDF's one-letter tool keys (KeyboardShortcuts.cs TryToolShortcut):
        // one letter per tool, so a key keeps its tool however the bar grows -
        // Select is V (the Photoshop convention), Shapes O, Line also U. They
        // need an open document, exactly like the buttons they drive.
        case 'V': if (!ctrl && !shift && DVF().n) SetTool(TOOL_SELECT, FocusB()); break;
        case 'T': if (!ctrl && !shift && DVF().n) SetTool(TOOL_TEXT, FocusB()); break;
        case 'H': if (!ctrl && !shift && DVF().n) SetTool(TOOL_HIGHLIGHT, FocusB()); break;
        case 'L': case 'U':
            if (!ctrl && !shift && DVF().n) SetTool(TOOL_LINE, FocusB()); break;
        case 'D': if (!ctrl && !shift && DVF().n) SetTool(TOOL_DRAW, FocusB()); break;
        case VK_DOWN:
            if (NudgeSel(wp, shift)) break;
            ScrollTo(fx, fy + line);
            break;
        case VK_UP:
            if (NudgeSel(wp, shift)) break;
            ScrollTo(fx, fy - line);
            break;
        case VK_RIGHT:
            if (NudgeSel(wp, shift)) break;
            ScrollTo(fx + line, fy);
            break;
        case VK_LEFT:
            if (NudgeSel(wp, shift)) break;
            ScrollTo(fx - line, fy);
            break;
        case VK_NEXT:
            if (DVF().vm == 1) SetPage(DVF().cur + 1);   // single mode: the flip keys
            else ScrollTo(fx, fy + pg);
            break;
        case VK_PRIOR:
            if (DVF().vm == 1) SetPage(DVF().cur - 1);
            else ScrollTo(fx, fy - pg);
            break;
        case VK_SPACE:
            if (DVF().vm == 1) SetPage(DVF().cur + (shift ? -1 : 1));
            else ScrollTo(fx, fy + (shift ? -pg : pg));
            break;
        case VK_HOME: ScrollTo(fx, 0); break;
        case VK_END: ScrollTo(fx, INT_MAX / 2); break;
        case VK_ADD: case VK_OEM_PLUS: ZoomBy(1.25); break;
        case VK_SUBTRACT: case VK_OEM_MINUS: ZoomBy(0.8); break;
        case '0': if (ctrl) ApplyFit(false, FocusB()); break;
        case '1': if (ctrl) SetZoom(1.0, g.cw / 2, g.ch / 2); break;
        case '2': if (ctrl) ApplyFit(true, FocusB()); break;
        case 'F':
            if (ctrl) FindToggle();
            break;
        case 'R':
            if (!ctrl && !shift && DVF().n) TransformPopupOpen();   // Transform (R)
            break;
        case 'Z':
            if (ctrl) { if (shift) RedoOp(); else UndoOp(); }   // Ctrl+Z / Ctrl+Shift+Z
            break;
        case 'Y':
            if (ctrl) RedoOp();
            break;
        case 'C': if (ctrl && !DVF().selText.empty()) CopyClipboard(DVF().selText); break;
        case 'A': if (ctrl) SelAll(DVF()); break;
        case VK_DELETE:
            // KillerPDF's DeleteSelected: the Select tool's chosen annotation
            // leaves its pane's document, one undo entry covers it
            if (g.annSelPage >= 0 && g.annSelIdx >= 0) {
                DocState& D2 = DV(g.annSelB);
                auto it = D2.annos.find(g.annSelPage);
                if (it != D2.annos.end() && g.annSelIdx < (int)it->second.size()) {
                    UndoPush(g.annSelB);
                    it->second.erase(it->second.begin() + g.annSelIdx);
                    if (it->second.empty()) D2.annos.erase(it);
                }
                g.annSelPage = -1; g.annSelIdx = -1;
                if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
                InvalidateRect(g.view, nullptr, FALSE);
            }
            break;
        case VK_F3:                                // Find next / previous; closed, F3 opens
            if (!fd.open) FindOpen();
            else if (!fd.hits.empty()) FindStep(shift ? -1 : 1);
            break;
        case VK_ESCAPE:
            if (g.annPoly) PolyCancel(h);          // the polygon session backs out first
            else if (g.tedit) CommitTextEdit(false, true);   // ESC backs the box out: a
                                                             // re-edit's original returns
            else if (fd.open) FindClose();         // the bar leaves next
            else if (g.full) ToggleFull(h);        // full screen leaves next
            else if (g.annSelPage >= 0) {          // then the annotation selection
                g.annSelPage = -1; g.annSelIdx = -1;
                InvalidateRect(g.view, nullptr, FALSE);
            }
            // and KillerPDF's Escape: an editing tool goes back to Select
            else if (DVF().n && DVF().tool != TOOL_SELECT) SetTool(TOOL_SELECT, FocusB());
            else SelClear(DVF());                  // then the focused pane's selection
            break;
        case VK_F5: SetViewMode(0); break;       // KillerPDF's mode keys
        case VK_F6: SetViewMode(1); break;
        case VK_F7: SetViewMode(2); break;
        case VK_F8: SetViewMode(3); break;
        case VK_F11: ToggleFull(h); break;
        case VK_F9: ToggleSide(); break;         // the reference's Pages shortcut stays
        case VK_F10: ToggleSplit(); break;       // KillerPDF's Split pane key
        case VK_F4: InfoOpen(); break;           // KillerPDF's Document info key
        case VK_RETURN:
            if (g.annPoly) { PolyCommit(h); break; }   // the polygon closes on Enter
            break;
        case VK_BACK:
            if (g.annPoly && !g.annPX.empty()) {  // Backspace pops the last vertex
                g.annPX.pop_back(); g.annPY.pop_back();
                InvalidateRect(g.view, nullptr, FALSE);
            }
            break;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 3) { InvalidateCap(h); return 0; }   // hovered-button keepalive repaint
        if (wp == 7) { WebCaptureTick(); return 0; }   // the web thumbs' capture pump
        break;                                 // (buffered, so an unchanged band reblits
                                               // identically and cannot strobe)
    case WM_DROPFILES: HandleDrop((HDROP)wp, h); return 0;   // the frame's drop: the focused pane
    case WM_APP + 5: WxInstallDone((WxJob*)lp); return 0;   // an extension download finished
    case WM_APP + 7: WxEngineRestart(); return 0;   // the deferred engine restart: a fresh
                                                    // browser process for extension operations
    case WM_APP + 3: {                        // the drop queue's drain: one document per
        if (!g_dropQ.empty()) {               // message, so the window paints between loads
            std::wstring p = std::move(g_dropQ.front());
            bool pb = g_dropPane.front();
            g_dropQ.erase(g_dropQ.begin());
            g_dropPane.erase(g_dropPane.begin());
            TabOpenInto(pb, p);
            if (!g_dropQ.empty()) PostMessageW(h, WM_APP + 3, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE: {
        // KillerPDF's quit prompt (Str_Dlg_UnsavedExit through KillerDialog.
        // AskToSave): unsaved work asks before the window dies. Save writes
        // every dirty tab of both panes in place - a session holds a live
        // engine handle, so its document saves without coming forward - and a
        // failed write keeps the window up.
        if (g.tedit) CommitTextEdit();
        auto paneDirty = [&](bool b) {
            std::vector<DocState>& T = Tabs(b);
            if (T.empty()) return DocDirty(DV(b));    // a pane that never tabbed: the live state alone
            for (int i = 0; i < (int)T.size(); i++)
                if (DocDirty(i == Act(b) ? DV(b) : T[i])) return true;
            return false;
        };
        bool aDirty = paneDirty(false), bDirty = paneDirty(true);
        if (!aDirty && !bDirty) { DestroyWindow(h); return 0; }
        int r = AskPrompt3(L"", L"You have unsaved changes. Save them before closing Docsy?",
                           L"Cancel", L"Don't Save", L"Save");
        if (r == 2) {
            auto savePane = [&](bool b) -> bool {
                std::vector<DocState>& T = Tabs(b);
                if (T.empty()) {
                    if (!DocDirty(DV(b))) return true;
                    std::wstring err;
                    if (!WriteAnnotatedPdf(DV(b), DV(b).path, err)) {
                        SetStatusHold(std::wstring(L"Save failed: ") + err);
                        return false;
                    }
                    SnapClean(DV(b));
                    return true;
                }
                for (int i = 0; i < (int)T.size(); i++) {
                    DocState& S = (i == Act(b)) ? DV(b) : T[i];
                    if (!DocDirty(S)) continue;
                    std::wstring err;
                    if (!WriteAnnotatedPdf(S, S.path, err)) {
                        SetStatusHold(std::wstring(L"Save failed: ") + err);
                        return false;
                    }
                    SnapClean(S);
                }
                return true;
            };
            if (!savePane(false)) return 0;          // a failed save is not a closed window
            if (!savePane(true)) return 0;
        }
        if (r == 2 || r == 1) DestroyWindow(h);   // Save / Don't Save close; Cancel stays
        return 0;
    }
    case WM_DESTROY:
        RqShutdown();                        // the render worker ends before the queues die
        WebShutdown();                       // the WebView2 controllers close with the window
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------- Transform (the toolbar's action) ----------------

// The Transform menu's apply: the page's session transform - KillerPDF
// rasterizes the page for these (its TransformWindow), and the annotations
// rotate with the raster; docsy rotates the RENDER and the page's own DIP
// space, so every downstream path (layout, hit tests, annotation paint, the
// OCR re-read) lands in the new space untouched. Flips apply first, then the
// quarter-turns - the same compose order Render() bakes.
static void TransformApply(bool b, int page, int turns, bool flipH, bool flipV) {
    DocState& D = DV(b);
    if (!D.n || page < 0 || page >= D.n) return;
    CommitTextEdit();
    float W = D.pw[page], H = D.ph[page];
    // flip the annotations into the new space first (flips ride the CURRENT dims)
    if (flipH || flipV) {
        D.pflip[page] = (BYTE)((D.pflip.count(page) ? D.pflip[page] : 0) ^ (flipH ? 1 : 0) ^ (flipV ? 2 : 0));
        auto it = D.annos.find(page);
        if (it != D.annos.end())
            for (Annot& a : it->second) {
                if (flipH) {
                    if (a.type == AT_HIGHLIGHT)
                        for (FRect& r : a.lines) r.x = W - r.x - r.w;
                    else if (a.type == AT_INK)
                        for (size_t i2 = 0; i2 < a.ix.size(); i2++) a.ix[i2] = W - a.ix[i2];
                    else { a.x = W - a.x - a.w; }
                }
                if (flipV) {
                    if (a.type == AT_HIGHLIGHT)
                        for (FRect& r : a.lines) r.y = H - r.y - r.h;
                    else if (a.type == AT_INK)
                        for (size_t i2 = 0; i2 < a.iy.size(); i2++) a.iy[i2] = H - a.iy[i2];
                    else { a.y = H - a.y - a.h; }
                }
            }
    }
    // then the quarter-turns, dims swapping each time
    int rot = ((D.prot.count(page) ? D.prot[page] : 0) + turns) & 3;
    for (int t = 0; t < (turns & 3); t++) {
        auto it = D.annos.find(page);
        if (it != D.annos.end())
            for (Annot& a : it->second) {
                if (a.type == AT_HIGHLIGHT) {
                    for (FRect& r : a.lines) {
                        float nx = H - r.y - r.h, ny = r.x;
                        r.x = nx; r.y = ny; std::swap(r.w, r.h);
                    }
                    float nx = H - a.y - a.h, ny = a.x;
                    a.x = nx; a.y = ny; std::swap(a.w, a.h);
                } else if (a.type == AT_INK) {
                    for (size_t i2 = 0; i2 < a.ix.size(); i2++) {
                        float nx = H - a.iy[i2], ny = a.ix[i2];
                        a.ix[i2] = nx; a.iy[i2] = ny;
                    }
                } else {
                    float nx = H - a.y - a.h, ny = a.x;
                    a.x = nx; a.y = ny; std::swap(a.w, a.h);
                }
            }
        std::swap(D.pw[page], D.ph[page]);   // the page's own DIP space turns with it
        std::swap(W, H);
    }
    D.prot[page] = rot;
    D.txt.clear();                           // the text layer re-reads the new raster
    CacheDrop(D, page);                      // GetBmp re-renders with the new rot bits
    g.annSelPage = -1; g.annSelIdx = -1;
    SelClear(D);
    Layout(D, b);
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    SetStatusHold(L"Transformed page " + std::to_wstring(page + 1));
}

// CI preview: render the first view offscreen to a PNG. Returns 0 on success, else 10+GDI+ status.
static int Shot(const wchar_t* out, int w, int h) {
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP b = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!b) return 4;
    HGDIOBJ old = SelectObject(dc, b);
    int sw = g_webOn ? sd.w : ((g.sideOn && DVF().n && !g.full) ? sd.w : 0);   // web: the rail
    int rw = RW();                                  // always shows the OPEN WEB TABS
    int cap0 = CAP(), cap = cap0 + WEBTOOLH(), sh = g_webOn ? 0 : SBH();
    int tb = g_webOn ? WebBandH() : TBH();          // browser mode: the tab band
    int ws_ = WEBSTRIPH();                          // the nav row rides between caption and tabs
    int ch = max(1, h - cap - ws_ - tb - sh);
    // the children render above the caption row and below it, so composite them shifted
    if (sw) {
        HDC m1 = CreateCompatibleDC(dc);
        HBITMAP b1 = CreateCompatibleBitmap(dc, sw, ch);
        HGDIOBJ o1 = SelectObject(m1, b1);
        SetBrushOrgEx(m1, 0, -(cap + ws_ + tb), nullptr);   // the pane's grain phase, as on screen
        if (g_webOn) WebSideDraw(m1, sw, ch);               // the OPEN WEB TABS rail
        else SideDraw(m1, sw, ch, 0);
        BitBlt(dc, 0, cap + ws_ + tb, sw, ch, m1, 0, 0, SRCCOPY);
        SelectObject(m1, o1); DeleteObject(b1); DeleteDC(m1);
    }
    {
        HDC m2 = CreateCompatibleDC(dc);
        int vw = max(1, w - sw - rw);
        HBITMAP b2 = CreateCompatibleBitmap(dc, vw, ch);
        HGDIOBJ o2 = SelectObject(m2, b2);
        SetBrushOrgEx(m2, -sw - rw, -(cap + ws_ + tb), nullptr);
        g_grainPX = -sw - rw; g_grainPY = -(cap + ws_ + tb);   // the view's grain phase, as on screen
        if (g_webOn) {
            WebCardPaint(m2, vw, ch);       // the browser's card: field, rounded face, ring
        } else if (g.split) {               // the split: two per-pane passes, the
            int wA = PaneW(false), xB = PaneX0(true), wB = PaneW(true);   // same paint the live
            for (int pass = 0; pass < 2; pass++) {      // view runs - focused pane LAST
                bool drawB = g.splitA ? pass == 0 : pass == 1;
                if (drawB) {
                    RECT pr{ xB, 0, vw, ch };
                    HRGN rg = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
                    Draw(m2, wB, ch, xB, g.dB, rg);
                    DeleteObject(rg);
                    PaintPaneBars(m2, g.dB, true, g.dB.vHot || g.dB.vDrag, g.dB.hHot || g.dB.hDrag);
                    TabBandPaint(m2, true);
                } else {
                    RECT pr{ 0, 0, wA, ch };
                    HRGN rg = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
                    Draw(m2, wA, ch, 0, g.d, rg);
                    DeleteObject(rg);
                    PaintPaneBars(m2, g.d, false, g.d.vHot || g.d.vDrag, g.d.hHot || g.d.hDrag);
                    TabBandPaint(m2, false);
                }
            }
            RECT band{ g.splitX + 1, 0, min(vw, g.splitX + SplitHalf()), ch };
            FillRect(m2, &band, GrainBrush(1));   // the gutter's own 3px, exactly like the
                                                  // live composite - never a stale strip
            RECT dl{ g.splitX, 2, g.splitX + 1, ch - 2 };
            FillRect(m2, &dl, SplitLineBrush(g.lineHot || g.splitDrag));
        } else {
            Draw(m2, vw, ch, 0, g.d);
            PaintViewBars(m2, vw, ch, 0);
            TabBandPaint(m2, false);
        }
        TabDragPaint(m2);                               // the cross-pane drag's ghost + caret
        if (g.barOn) {                                  // the floating settings bar rides the shot
            RECT br3 = AnnBarRect();                    // like the live composite paints it
            AnnBarDraw(m2, br3.left, br3.top);
        }
        if (fd.open) {                                  // the floating Find bar, over its pixels -
            FindDraw(m2, FdRect().left, FdRect().top);  // the same FdRect the live view paints,
        }                                               // so the shot IS the app's composite
        BitBlt(dc, sw + rw, cap + ws_ + tb, vw, ch, m2, 0, 0, SRCCOPY);
        SelectObject(m2, o2); DeleteObject(b2); DeleteDC(m2);
    }
    if (rw > 0) {                                       // the rail strip between sidebar and view
        HDC m5 = CreateCompatibleDC(dc);
        HBITMAP b5 = CreateCompatibleBitmap(dc, rw, ch);
        HGDIOBJ o5 = SelectObject(m5, b5);
        SetBrushOrgEx(m5, -sw, -(cap + ws_ + tb), nullptr);
        RailDraw(m5, rw, ch);
        BitBlt(dc, sw, cap + ws_ + tb, rw, ch, m5, 0, 0, SRCCOPY);
        SelectObject(m5, o5); DeleteObject(b5); DeleteDC(m5);
    }
    if (InfoIsOpen() || g_idlgShots) {                  // the Document Info dialog composites
        HDC m6 = CreateCompatibleDC(dc);                // over the view, its paint shared with
        HBITMAP b6 = CreateCompatibleBitmap(dc, IdW(), IdH());   // the live window's WM_PAINT
        HGDIOBJ o6 = SelectObject(m6, b6);
        SetBrushOrgEx(m6, 0, 0, nullptr);
        g_grainPX = 0; g_grainPY = 0;
        InfoDialogDraw(m6, IdW(), IdH());
        BitBlt(dc, max(0, (w - IdW()) / 2), max(cap, (h - IdH()) / 2), IdW(), IdH(), m6, 0, 0, SRCCOPY);
        SelectObject(m6, o6); DeleteObject(b6); DeleteDC(m6);
    }
    if (g_askShots) {                                   // the save prompt composites the
        HDC m7 = CreateCompatibleDC(dc);                // same paint the live window runs
        HBITMAP b7 = CreateCompatibleBitmap(dc, AskW(), AskH());
        HGDIOBJ o7 = SelectObject(m7, b7);
        SetBrushOrgEx(m7, 0, 0, nullptr);
        g_grainPX = 0; g_grainPY = 0;
        AskDialogDraw(m7, AskW(), AskH());
        BitBlt(dc, max(0, (w - AskW()) / 2), max(cap, (h - AskH()) / 2), AskW(), AskH(), m7, 0, 0, SRCCOPY);
        SelectObject(m7, o7); DeleteObject(b7); DeleteDC(m7);
    }
    if (!g.full && sh > 0) {                   // the footer strip
        HDC m3 = CreateCompatibleDC(dc);
        HBITMAP b3 = CreateCompatibleBitmap(dc, w, sh);
        HGDIOBJ o3 = SelectObject(m3, b3);
        SetBrushOrgEx(m3, 0, -(cap + ws_ + tb + ch), nullptr);
        StatusDraw(m3, w, sh);
        BitBlt(dc, 0, cap + ws_ + tb + ch, w, sh, m3, 0, 0, SRCCOPY);
        SelectObject(m3, o3); DeleteObject(b3); DeleteDC(m3);
    }
    if (!g.full && tb > 0) {                   // the row under the caption: reader tools,
        HDC m4 = CreateCompatibleDC(dc);       // or - one nav band lower - the tab row
        HBITMAP b4 = CreateCompatibleBitmap(dc, w, tb);
        HGDIOBJ o4 = SelectObject(m4, b4);
        SetBrushOrgEx(m4, 0, -(cap + ws_), nullptr);
        BarDraw(m4, w, tb);
        BitBlt(dc, 0, cap + ws_, w, tb, m4, 0, 0, SRCCOPY);
        SelectObject(m4, o4); DeleteObject(b4); DeleteDC(m4);
    }
    if (!g.full && g_webOn && cap > cap0) {    // the reader's toolbar row, one PDF button
        HDC m9 = CreateCompatibleDC(dc);
        HBITMAP b9 = CreateCompatibleBitmap(dc, w, cap - cap0);
        HGDIOBJ o9 = SelectObject(m9, b9);
        SetBrushOrgEx(m9, 0, -cap0, nullptr);
        WebToolDraw(m9, w, cap - cap0);
        BitBlt(dc, 0, cap0, w, cap - cap0, m9, 0, 0, SRCCOPY);
        SelectObject(m9, o9); DeleteObject(b9); DeleteDC(m9);
    }
    if (!g.full && ws_ > 0) {                  // the browser's Safari toolbar, topmost
        HDC m8 = CreateCompatibleDC(dc);
        HBITMAP b8 = CreateCompatibleBitmap(dc, w, ws_);
        HGDIOBJ o8 = SelectObject(m8, b8);
        SetBrushOrgEx(m8, 0, -cap, nullptr);
        WebStripPaint(m8, w, ws_);
        BitBlt(dc, 0, cap, w, ws_, m8, 0, 0, SRCCOPY);
        SelectObject(m8, o8); DeleteObject(b8); DeleteDC(m8);
    }
    DrawCaptionBar(dc, w);
    GdiFlush();
    SelectObject(dc, old);
    wchar_t full[MAX_PATH * 2];
    GetFullPathNameW(out, (DWORD)_countof(full), full, nullptr);
    Gdiplus::Bitmap* bm = Gdiplus::Bitmap::FromHBITMAP(b, nullptr);
    CLSID png{}; UINT cnt = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&cnt, &bytes);
    std::vector<BYTE> buf(bytes);
    auto* enc = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(cnt, bytes, enc);
    for (UINT i = 0; i < cnt; i++) if (!lstrcmpW(enc[i].MimeType, L"image/png")) png = enc[i].Clsid;
    Gdiplus::Status st = bm->Save(full, &png, nullptr);
    delete bm;
    return st == Gdiplus::Ok ? 0 : 10 + (int)st;
}

