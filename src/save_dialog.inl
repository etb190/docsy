// ---------------- the save prompts (KillerDialog's themed question box) ----------------
// KillerPDF never asks with a MessageBox: KillerDialog.Show is the app's own
// chrome at 390 DIP - the family caption (wordmark, qualifier, close glyph),
// the notice body (a 28px amber "!" ring in a 38px column beside the wrapped
// 13px message, margins 24/18) and the button row right-aligned with 8px gaps
// ("the way out first and the action last, at the right edge", 28px tall,
// >=80 wide, the accent hollow at rest and filled on hover, Enter on the
// accent, Esc on Cancel). Docsy's Document Info dialog already carries this
// chrome; the prompts are the same paint with three buttons:
//  * the Save click over changes - KillerPDF's Str_Dlg_OverwriteMsg
//    "Replace {name}?" answered Cancel | Save As | Overwrite, and
//  * the close prompts - Str_Dlg_UnsavedClose / Str_Dlg_UnsavedExit answered
//    Cancel | Don't Save | Save.
// Every caller gates on DocDirty: the prompt only exists over changes.
struct AskDlg {
    std::wstring qualifier;              // caption text after the wordmark
    std::wstring message;
    std::wstring btn[3];                 // visual order, left to right
    int bw[3] = {};                      // button widths: the label's own + 24, >=80
    int def = 2;                         // Enter's target = the accent, rightmost
    int result = -1;                     // -1 = dismissed (Esc / X / click-away)
    int hot = -1;                        // hovered: 0-2 buttons, 3 the caption close
    int msgH = 0;                        // the wrapped message's height, DIP pixels
};
static AskDlg g_ask;
static HWND g_askDlg = nullptr;
static bool g_askShots = false;          // the CI shot composites the same paint

static HFONT g_askCap = nullptr, g_askMsg = nullptr, g_askBtn = nullptr, g_askBang = nullptr;
static HFONT AskCapFont()  { return CaptionFont(g_askCap, 14, false, L"Killer Sans"); }
static HFONT AskMsgFont()  { return CaptionFont(g_askMsg, 13, false, L"Killer Sans UI"); }
static HFONT AskBtnFont()  { return CaptionFont(g_askBtn, 12, false, L"Killer Sans UI"); }
static HFONT AskBangFont() { return CaptionFont(g_askBang, 16, true, L"Killer Sans"); }

static int AskW() { return (int)(390 * g.dpi); }      // KillerDialog's Width=390
static int AskCapH() { return (int)(32 * g.dpi); }    // the family caption bar
// NoticeBody's grid: Margin 24,18,24,18, a 38px icon column, the 28px ring.
// The button row: Padding 16,0,24,20, buttons 28 tall with 8px gaps.
static int AskMsgTop() { return AskCapH() + (int)(18 * g.dpi); }
static int AskMsgW(int w) { return w - (int)(86 * g.dpi); }   // 24 margin + 38 column + 24
static int AskContentH() { return max((int)(28 * g.dpi), g_ask.msgH + (int)(5 * g.dpi)); }
static int AskH() {
    return AskCapH() + (int)(18 * g.dpi) + AskContentH() + (int)(18 * g.dpi)
         + (int)(28 * g.dpi) + (int)(20 * g.dpi);
}

// Button i's rect: the row walks in from the right edge, 8px between buttons.
static RECT AskBtnRect(int i, int w, int h) {
    int bh = (int)(28 * g.dpi), gap = (int)(8 * g.dpi);
    int x = w - (int)(24 * g.dpi), bottom = h - (int)(20 * g.dpi);
    for (int k = 2; k >= 0; k--) {
        if (k == i) return { x - g_ask.bw[k], bottom - bh, x, bottom };
        x -= g_ask.bw[k] + gap;
    }
    return { 0, 0, 0, 0 };
}

// the wrapped message's height and the three widths, measured once per ask
static void AskMeasure() {
    if (!g.main) return;
    HDC dc = GetDC(g.main);
    HFONT of = (HFONT)SelectObject(dc, AskMsgFont());
    RECT mr{ 0, 0, AskMsgW(AskW()), 1 << 15 };
    DrawTextW(dc, g_ask.message.c_str(), -1, &mr, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    g_ask.msgH = mr.bottom - mr.top;
    SelectObject(dc, AskBtnFont());
    for (int k = 0; k < 3; k++) {
        SIZE ts{};
        GetTextExtentPoint32W(dc, g_ask.btn[k].c_str(), (int)g_ask.btn[k].size(), &ts);
        g_ask.bw[k] = max((int)(80 * g.dpi), (int)ts.cx + (int)(24 * g.dpi));
    }
    SelectObject(dc, of);
    ReleaseDC(g.main, dc);
}

static HDC g_akdc = nullptr; static HBITMAP g_akbm = nullptr, g_akold = nullptr;
static int g_akw = 0, g_akh = 0;
static void EnsureAskBack(HDC ref, int w, int h) {   // the Info dialog's buffered present:
    if (g_akbm && g_akw == w && g_akh == h) return;  // a hover flip reblits an identical
    if (g_akbm) { SelectObject(g_akdc, g_akold); DeleteObject(g_akbm); }
    else g_akdc = CreateCompatibleDC(ref);
    g_akbm = CreateCompatibleBitmap(ref, w, h);
    g_akold = (HBITMAP)SelectObject(g_akdc, g_akbm);
    g_akw = w; g_akh = h;
}

// One paint, two hosts: the live dialog's WM_PAINT and the CI shot composite.
static void AskDialogDraw(HDC dc, int w, int h) {
    int capH = AskCapH();
    // the caption row: the family bar - the wordmark, the qualifier, the
    // red-hovered close; the whole bar drags the window (WM_NCHITTEST below)
    RECT cr{ 0, 0, w, capH };
    FillRect(dc, &cr, GrainBrush(2));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, AskCapFont());
    RECT tr{ (int)(12 * g.dpi), 0, w - (int)(44 * g.dpi), capH };
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
    DrawTextW(dc, L"Docsy", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SIZE ts{};
    GetTextExtentPoint32W(dc, L"Docsy", 5, &ts);
    RECT qr{ tr.left + ts.cx + (int)(4 * g.dpi), tr.top, tr.right, tr.bottom };
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
    DrawTextW(dc, g_ask.qualifier.c_str(), -1, &qr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    int bs = (int)(44 * g.dpi);
    RECT xr{ w - bs, 0, w, capH };
    if (g_ask.hot == 3) { RECT hr{ xr.left, 0, xr.right, capH }; FillRect(dc, &hr, RowHoverBrush()); }
    SelectObject(dc, StGlyph());
    SetTextColor(dc, g_ask.hot == 3 ? RGB(0xff, 0x67, 0x67) : RGB(0xa3, 0xa3, 0xa3));
    DrawTextW(dc, L"\xE8BB", -1, &xr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // the body: the shared grain, the amber ring, the message beside it
    RECT body{ 0, capH, w, h };
    FillRect(dc, &body, GrainBrush(1));
    SetBkMode(dc, TRANSPARENT);
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    int top = AskMsgTop();
    // the 28px ring, top-aligned in the 38px column - KillerDialog's Notice
    // Border, its amber darkening for the light theme never leaving dark here
    float inset = max(1.0f, (float)g.dpi);           // the 2px pen eats inward, keep the box
    Gdiplus::Pen ring(Gdiplus::Color((BYTE)255, 0xff, 0xbe, 0x50), 2.0f * (float)g.dpi);
    gs.DrawEllipse(&ring, (float)(24 * g.dpi) + inset, (float)top + inset,
                   (float)(28 * g.dpi) - 2 * inset, (float)(28 * g.dpi) - 2 * inset);
    RECT bang{ (int)(24 * g.dpi), top, (int)(24 * g.dpi) + (int)(28 * g.dpi), top + (int)(28 * g.dpi) };
    SelectObject(dc, AskBangFont());
    SetTextColor(dc, RGB(0xff, 0xbe, 0x50));
    DrawTextW(dc, L"!", -1, &bang, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // the message: column 1, its own 5px top margin, wrapping in 304 DIP
    RECT mr{ (int)(62 * g.dpi), top + (int)(5 * g.dpi), w - (int)(24 * g.dpi),
             top + (int)(5 * g.dpi) + max(g_ask.msgH, 1) };
    SelectObject(dc, AskMsgFont());
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
    DrawTextW(dc, g_ask.message.c_str(), -1, &mr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    // the three buttons: the way out first, the accent last, at the right edge
    auto boxPath = [&](const RECT& rc) {
        float r2 = (float)(4 * g.dpi);
        float x1 = (float)rc.left, y1 = (float)rc.top;
        float fw = (float)(rc.right - rc.left), fh = (float)(rc.bottom - rc.top);
        Gdiplus::GraphicsPath* pth = new Gdiplus::GraphicsPath();
        pth->AddArc(x1, y1, 2 * r2, 2 * r2, 180, 90);
        pth->AddArc(x1 + fw - 2 * r2, y1, 2 * r2, 2 * r2, 270, 90);
        pth->AddArc(x1 + fw - 2 * r2, y1 + fh - 2 * r2, 2 * r2, 2 * r2, 0, 90);
        pth->AddArc(x1, y1 + fh - 2 * r2, 2 * r2, 2 * r2, 90, 90);
        pth->CloseFigure();
        return pth;
    };
    for (int k = 0; k < 3; k++) {
        RECT br = AskBtnRect(k, w, h);
        bool accent = k == g_ask.def, hot = g_ask.hot == k;
        Gdiplus::GraphicsPath* pth = boxPath(br);
        if (accent && hot) {
            Gdiplus::SolidBrush gb(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56));
            gs.FillPath(&gb, pth);
        } else if (accent) {
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else if (hot) {
            Gdiplus::SolidBrush hb(Gdiplus::Color((BYTE)255, 0x40, 0x40, 0x40));
            gs.FillPath(&hb, pth);
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else {
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        }
        delete pth;
        SetTextColor(dc, accent ? (hot ? RGB(0x05, 0x24, 0x0f) : RGB(0x22, 0xbb, 0x56))
                                : RGB(0xe0, 0xe0, 0xe0));
        SelectObject(dc, AskBtnFont());
        DrawTextW(dc, g_ask.btn[k].c_str(), -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

static void AskClose(int r) {
    if (!g_askDlg) return;
    g_ask.result = r;
    HWND d = g_askDlg;
    g_askDlg = nullptr;
    DestroyWindow(d);
    RailRepaint();
    if (g.main) SetFocus(g.main);            // the keys go home with the dialog gone
}

static LRESULT CALLBACK AskDlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureAskBack(dc, r.right, r.bottom);
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
            SetBrushOrgEx(g_akdc, -po.x, -po.y, nullptr);   // the grain phases to the window,
            AskDialogDraw(g_akdc, r.right, r.bottom);       // one blit per hover flip
            BitBlt(dc, 0, 0, r.right, r.bottom, g_akdc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        // the popup contract (the Info dialog's): a click anywhere outside -
        // the main window, another app, the taskbar - answers Cancel
        if (LOWORD(wp) == WA_INACTIVE && g_askDlg == h) AskClose(-1);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { AskClose(-1); return 0; }          // IsCancel
        if (wp == VK_RETURN) { AskClose(g_ask.def); return 0; }   // IsDefault = the accent
        return 0;
    case WM_NCHITTEST: {
        // the caption DRAGS the window (DialogFrame's DragMove); the close
        // zone stays client so its click is ours
        POINT s{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT c = s; ScreenToClient(h, &c);
        RECT r; GetClientRect(h, &r);
        if (c.y >= 0 && c.y < AskCapH() && c.x < r.right - (int)(44 * g.dpi)) return HTCAPTION;
        break;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int hot = -1;
        for (int k = 0; k < 3; k++) {
            RECT br = AskBtnRect(k, r.right, r.bottom);
            if (PtInRect(&br, p)) { hot = k; break; }
        }
        if (hot < 0 && p.y < AskCapH() && p.x >= r.right - (int)(44 * g.dpi)) hot = 3;
        if (hot != g_ask.hot) {
            g_ask.hot = hot;
            if (hot >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_ask.hot >= 0) { g_ask.hot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        for (int k = 0; k < 3; k++) {
            RECT br = AskBtnRect(k, r.right, r.bottom);
            if (PtInRect(&br, p) && g_ask.hot == k) { AskClose(k); return 0; }
        }
        if (p.y < AskCapH() && p.x >= r.right - (int)(44 * g.dpi) && g_ask.hot == 3) AskClose(-1);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT r; GetClientRect(h, &r);
            bool over = false;
            for (int k = 0; k < 3 && !over; k++) {
                RECT br = AskBtnRect(k, r.right, r.bottom);
                over = PtInRect(&br, p) != FALSE;
            }
            if (over || (p.y < AskCapH() && p.x >= r.right - (int)(44 * g.dpi))) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
            }
            if (p.y < AskCapH()) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        }
        break;
    case WM_CLOSE:
        AskClose(-1);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// The ask itself: seed the state, open the owned popup centered on the main
// window and pump until it answers - the caller's flow stays linear, exactly
// like KillerDialog's ShowDialog. Returns the button's index (0 leftmost, 2
// the accent) or -1 for a dismissal, which every caller reads as Cancel.
static int AskPrompt3(const wchar_t* qualifier, const wchar_t* msg,
                      const wchar_t* b0, const wchar_t* b1, const wchar_t* b2) {
    static bool inFlight = false;            // a close click landing mid-ask reads Cancel
    if (!g.main || inFlight) return -1;
    inFlight = true;
    g_ask = AskDlg{};
    g_ask.qualifier = qualifier;
    g_ask.message = msg;
    g_ask.btn[0] = b0; g_ask.btn[1] = b1; g_ask.btn[2] = b2;
    g_ask.def = 2;
    AskMeasure();
    int w = AskW(), h = AskH();
    RECT mr{}; GetWindowRect(g.main, &mr);
    int x = mr.left + max(0, (int)((mr.right - mr.left) - w) / 2);
    int y = mr.top + max(0, (int)((mr.bottom - mr.top) - h) / 2);
    g_askDlg = CreateWindowExW(0, L"DocsyAskDlg", nullptr, WS_POPUP, x, y, w, h,
                               g.main, nullptr, g.hinst, nullptr);
    if (!g_askDlg) { inFlight = false; return -1; }
    DwmApply(g_askDlg);
    ShowWindow(g_askDlg, SW_SHOWNORMAL);
    SetFocus(g_askDlg);
    RailRepaint();
    MSG m2;
    while (g_askDlg && GetMessageW(&m2, nullptr, 0, 0) > 0) {   // the modal pump: eat
        TranslateMessage(&m2);                                 // messages until the
        DispatchMessageW(&m2);                                 // dialog answers itself
    }
    inFlight = false;
    return g_ask.result;
}

