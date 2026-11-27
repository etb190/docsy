// ---------------- Find Text bar (KillerPDF's Shell/Search.cs) ----------------
// One floating bar over the page area, KillerPDF's fallback spot: the bar's top
// edge 6px under the view's top, its right edge 16px in from the view's right.
// The bar is NOT a window - it is composited INTO the view's back buffer on
// every paint (the same path the CI shot uses), because a floating child window
// over a GDI view is two surfaces that must be kept in step by hand: the view
// paints, the bar repaints over it, a moved bar leaves stale strips behind, and
// every one of those seams flashed under the mouse - the horror show. Composite
// pixels have no seams: pages, highlights, overlay bars, the bar and its
// DropShadow land in ONE buffer and ONE blit, and a drag is a coordinate change
// plus one invalidate - a single atomic frame per pointer move, WPF's single
// surface. The only real window is the query EDIT (a child of the view, parked
// inside its ring by FindPlace): the native control brings the caret, the IME,
// the clipboard and the selection for free, and the view's WS_CLIPCHILDREN
// keeps every composite blit out of its pixels - nothing can show through it,
// clobber it or turn it transparent.
// The visual is the original Border: CornerRadius 6, a 1px PaneBorderBrush
// #555555 frame, MenuBackgroundBrush #141414 + grain inside, and the DropShadow
// (black, blur 16, depth 3, down, opacity 0.55) - built as an ARGB tile and
// melted onto the view's own pixels. The row is KillerPDF's find row minus the
// replace gear (Windows.Data.Pdf cannot edit text): the drag grip (two columns
// of three 3px dots, MutedTextBrush - grabbing it moves the bar anywhere on the
// card, the offsets clamped and kept until the document changes), the magnifier
// glyph E721 (MutedTextBrush), the query edit (26px tall in a radius-3
// CardBorderBrush #4a4a4a box on BackgroundBrush #1c1c1c, UiFont 13), the
// Aa / ab toggles (SelectionBg's green gradient + SelectionFg white when on,
// RowHover #404040 + TextBrush on hover), the 96px counter cell (FontSize 11,
// MutedTextBrush, "12 / 73" or "No matches"), and ChevronUp / ChevronDown /
// Cancel 26x24 MDL2 buttons - ToolbarButton's TextBrush glyph over the
// RowHover fill, the close X the danger style's MutedText -> DangerRed glyph.
// The toolbar row above the view carries KillerPDF's search unit (BarDraw's
// E721-over-"Find Text" button, ToolbarButton's radius-3 RowHover hover) so
// the bar opens by mouse too, and the whole family - toolbar, bar, sidebar,
// footer - shares one grain, one border tone and one corner language.
// Logic is SearchController's: 250ms debounce, a two-character minimum,
// Enter/Shift+Enter and F3/Shift+F3 step the cursor with wrap, Escape closes,
// a toggle re-runs at once, results start at the first match on or after the
// page you are on, and the scan pumps page by page on a timer so the window
// never blocks on a long book.

static LRESULT CALLBACK FindEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static void FindPlace();
static void FindClampDrag();

static HFONT g_fntEdit = nullptr, g_fntOpt = nullptr, g_fntAb = nullptr,
              g_fntStat = nullptr, g_fntGlyph = nullptr;

static HFONT FdFontEdit()  { return CaptionFont(g_fntEdit,  13, false, L"Killer Sans UI"); }
static HFONT FdFontOpt()   { return CaptionFont(g_fntOpt,   12, false, L"Killer Sans UI"); }
static HFONT FdFontAb()    {            // "ab": KillerPDF's underlined Whole Word glyph
    if (!g_fntAb)
        g_fntAb = CreateFontW(-FdD(12), 0, 0, 0, FW_NORMAL, 0, TRUE, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Killer Sans UI");
    return g_fntAb;
}
static HFONT FdFontStat()  { return CaptionFont(g_fntStat,  11, false, L"Killer Sans UI"); }
static HFONT FdFontGlyph() { return CaptionFont(g_fntGlyph, 12, false, L"Segoe MDL2 Assets"); }

// The query edit's vertical placement. KillerPDF centres the text the WPF way:
// the box is 26px and the line rides its middle (VerticalContentAlignment
// Center + the template's centred PART_ContentHost). A single-line EDIT paints
// its line box at the TOP of its client (Wine's EDIT_AdjustFormatRect:
// "Windows doesn't care to fix text placement for SL controls"), so the window
// gets EXACTLY the font's line box and parks it centred in the box - a control
// the size of the line is centred whether the EDIT draws top or centre, and
// the glyph body lands mid-box on the real font's own metrics, no ratio guess.
static void FdEditGeom(int boxH, int& h, int& off) {
    HDC dc = GetDC(g_fedit ? g_fedit : (g.view ? g.view : nullptr));
    HFONT of = (HFONT)SelectObject(dc, FdFontEdit());
    TEXTMETRICW tm{}; GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    ReleaseDC(g_fedit ? g_fedit : (g.view ? g.view : nullptr), dc);
    h = min(FdD(FdEditH), (int)tm.tmHeight + 1);   // the line box + 1px of slack; the
                                                   // cast: tmHeight is LONG - MSVC's
                                                   // std::min cannot deduce across it
    off = max(0, (boxH - h) / 2);                  // the line box centred: WPF's
                                                   // VerticalContentAlignment Center
}

static void FindStatusRepaint() {
    if (g.view && fd.open) { RECT r = FdRect(); InvalidateRect(g.view, &r, FALSE); }
}

// Lands the cursor match: the counter, the repaint, and - when stepping - the
// scroll onto the match (Single mode flips pages first, the other modes scroll
// the match's line to the view's middle, horizontally centring the page too).
static void FindGoto(int idx, bool jump) {
    if (idx < 0 || idx >= (int)fd.hits.size()) return;
    fd.cur = idx;
    const FHit& hit = fd.hits[idx];
    if (jump) {
        DocState& D = fd.pane ? g.dB : g.d;   // the jump lands in the pane the scan reads -
        bool b = fd.pane;                     // the same rule KillerPDF's search binds by
        if (D.vm == 1 && hit.page != D.cur) SetPage(hit.page);
        int row = std::clamp(hit.page / PerRow(D), 0, max(0, (int)D.rh.size() - 1));
        int yDoc = PageTop(D, hit.page) + (D.rh[row] - PH(D, hit.page)) / 2
                 + (int)((hit.r.y + hit.r.h * 0.5f) * D.zoom * g.dpi);
        int xDoc = D.sx;
        if (D.docW > CardW(b) && hit.page < (int)D.px.size())
            xDoc = D.px[hit.page] + (int)((hit.r.x + hit.r.w * 0.5f) * D.zoom * g.dpi) - CardW(b) / 2;
        ScrollPane(b, xDoc, yDoc - g.ch / 2);
    }
    InvalidateRect(g.view, nullptr, FALSE);
    FindStatusRepaint();
}

// The scan just completed: KillerPDF's start rule - sit on the first match at or
// after the page you are on, else wrap to the first one - and the final counter.
static void FindFinish() {
    if (fd.hits.empty()) { fd.cur = -1; FindStatusRepaint(); return; }
    int start = max(0, (fd.pane ? g.dB : g.d).cur);   // the scan's own pane's page
    fd.cur = -1;
    for (int i = 0; i < (int)fd.hits.size(); i++)
        if (fd.hits[i].page >= start) { fd.cur = i; break; }
    if (fd.cur < 0) fd.cur = 0;
    FindGoto(fd.cur, true);
}

static void FindStep(int dir) {
    if (fd.hits.empty()) return;
    int n = (int)fd.hits.size();
    FindGoto(((fd.cur < 0 ? 0 : fd.cur) + dir + n) % n, true);
}

// A toggle flips and re-runs at once when there is a live query (KillerPDF's
// ToggleSearchOption); below two characters there is nothing to re-run.
static void FindOption(bool matchCase) {
    if (matchCase) fd.matchCase = !fd.matchCase;
    else fd.wholeWord = !fd.wholeWord;
    FindStatusRepaint();
    if (fd.query.size() >= 2 && fd.query.find_first_not_of(L" \t\n\r") != std::wstring::npos) {
        if (g.view) KillTimer(g.view, 7);
        fd.gen++; fd.scan = 0; fd.hits.clear(); fd.cur = -1; fd.done = false;
        fd.pane = FocusB();                       // a re-run re-pins to the focused pane
        fd.anchor = max(0, DVF().cur);
        if (g.view) SetTimer(g.view, 8, 30, nullptr);
    }
}

static void FindRun() {
    if (fd.query.size() < 2 || fd.query.find_first_not_of(L" \t\n\r") == std::wstring::npos) return;
    fd.gen++; fd.scan = 0; fd.hits.clear(); fd.cur = -1; fd.done = false; fd.pending = false;
    fd.pane = FocusB();                       // the scan pins to the pane the reader is on
    fd.anchor = max(0, DVF().cur);
    if (g.view) SetTimer(g.view, 8, 30, nullptr);
}

// The pump: scan pages until a small time budget is spent, then hand control
// back - a fresh OCR page costs tens of milliseconds, so the budget only trims
// the cheap pages; the heavy ones land one per tick and the window keeps its
// pulse. The run starts at the page you are ON (the anchor) and walks forward,
// wrapping to the pages before it last: the first OCR tick already covers what
// the user is looking at, and the counter climbs from that first tick instead
// of arriving only after the whole book. Every edit bumps gen, and a stale
// pump (this gen is no longer the state's) drops everything it has gathered.
static void FindPump() {
    if (fd.done) { if (g.view) KillTimer(g.view, 8); return; }
    unsigned my = fd.gen;
    unsigned long long t0 = GetTickCount64();
    std::vector<FRect> rs;
    int before = (int)fd.hits.size();
    DocState& D = fd.pane ? g.dB : g.d;           // the scan reads its pinned pane's document
    while (fd.scan < D.n && GetTickCount64() - t0 < 25) {
        int i = (fd.anchor + fd.scan) % D.n;      // the anchor page first, then forward
        TxtPage* pt = TtFor(D, i);
        if (pt && !pt->cells.empty() && FdMatch(*pt, fd.query, fd.matchCase, fd.wholeWord, rs) > 0)
            for (const FRect& r : rs) fd.hits.push_back({ i, r });
        fd.scan++;
    }
    if (fd.gen != my) return;                     // cancelled mid-tick: drop it all
    if ((int)fd.hits.size() != before) InvalidateRect(g.view, nullptr, FALSE);   // progressive
    FindStatusRepaint();                          // the counter climbs as pages land
    if (fd.scan >= D.n) {
        fd.done = true;
        if (g.view) KillTimer(g.view, 8);
        FindFinish();
    }
}

// EN_CHANGE: a new generation cancels the pump and the debounce, then the
// 250ms pause in typing starts the run - KillerPDF's SearchBox_TextChanged.
static void FindChanged() {
    if (!g_fedit) return;
    int len = GetWindowTextLengthW(g_fedit);
    std::wstring q(len, L'\0');
    if (len) GetWindowTextW(g_fedit, q.data(), len + 1);
    fd.gen++; fd.scan = 0; fd.hits.clear(); fd.cur = -1; fd.done = true;
    fd.query = q;
    fd.pending = q.size() >= 2 &&
                 q.find_first_not_of(L" \t\n\r") != std::wstring::npos;   // a run is coming
    if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
    if (q.size() < 2) {
        InvalidateRect(g.view, nullptr, FALSE);
        FindStatusRepaint();
        return;
    }
    if (g.view) SetTimer(g.view, 7, 250, nullptr);
}

static void FindOpen(bool create = true) {
    fd.open = true;
    fd.query.clear(); fd.hits.clear(); fd.cur = -1; fd.scan = 0; fd.gen++; fd.done = true; fd.hot = -1;
    fd.pending = false; fd.anchor = 0; fd.dx = fd.dy = 0;   // a fresh bar opens at home
    fd.pane = FocusB();                       // the bar serves the focused pane's document
    if (create && g.view) {
        if (!g_fedit) {
            g_fedit = CreateWindowExW(0, L"EDIT", nullptr,
                                      WS_CHILD | ES_AUTOHSCROLL,
                                      0, 0, 0, 0, g.view, nullptr, g.hinst, nullptr);
            if (!g_fedit) { fd.open = false; return; }
            SendMessageW(g_fedit, WM_SETFONT, (WPARAM)FdFontEdit(), TRUE);
            SendMessageW(g_fedit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(FdD(6), FdD(6)));
            g_feditDef = (WNDPROC)SetWindowLongPtrW(g_fedit, GWLP_WNDPROC, (LONG_PTR)FindEditProc);
        }
        FindPlace();                              // the edit rides its ring at the home spot
        ShowWindow(g_fedit, SW_SHOWNOACTIVATE);
        InvalidateRect(g_fedit, nullptr, TRUE);   // first light is a full opaque paint too:
                                                  // the child, not the composite, owns its
                                                  // pixels (WS_CLIPCHILDREN), so it must
                                                  // draw itself the frame it appears
        InvalidateRect(g.view, nullptr, FALSE);   // the bar enters the composite
    }
    if (g_fedit) { SetWindowTextW(g_fedit, L""); SetFocus(g_fedit); }   // open empty, focus ready
}

static void FindClose() {
    if (!fd.open) return;
    fd.open = false;
    fd.gen++; fd.done = true; fd.scan = 0;
    fd.hits.clear(); fd.cur = -1; fd.query.clear(); fd.hot = -1; fd.pending = false;
    if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
    if (g_fedit) ShowWindow(g_fedit, SW_HIDE);   // the bar was never a window: hide the edit
    if (g.main) SetFocus(g.main);                 // the keys go home (they live on the main window)
    InvalidateRect(g.view, nullptr, FALSE);       // the highlights and the bar leave with it
}

static void FindToggle() { fd.open ? FindClose() : FindOpen(); }

// The bar's paint, shared with the CI shot: (ox,oy) is the window's client
// origin in the destination DC - (0,0) on the window itself, the window's view
// offset in the shot. Assumes the view's pixels are already behind it.
static void FindDraw(HDC dc, int ox, int oy) {
    int editW = FdEditW(CardW(FocusB()));     // the bar rides the focused pane's card
    int bw = FdD(FdBarFix) + editW, bh = FdD(FdBarH);
    int w = bw + 2 * FdD(FdPadX), h = bh + FdD(FdPadT) + FdD(FdPadB);
    // the DropShadow: black, blur 16 (sigma 8), depth 3 down, opacity 0.55 - the
    // bar's blurred silhouette as an ARGB tile, cached per size/dpi
    static HBITMAP shb = nullptr; static HDC shdc = nullptr;
    static int shw = 0, shh = 0; static double shdpi = 0, shed = 0;
    if (!shb || shw != w || shh != h || shdpi != g.dpi || shed != editW) {
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        BYTE* px = nullptr;
        HBITMAP nb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
        if (px && nb) {
            const double sig = 8.0 * g.dpi, depth = 3.0 * g.dpi;
            double top = FdD(FdPadT) + depth, bottom = FdD(FdPadT) + bh + depth;
            double left = FdD(FdPadX), right = FdD(FdPadX) + bw;
            for (int y = 0; y < h; y++) {
                double ay = Phi((y - top) / sig) - Phi((y - bottom) / sig);
                BYTE* row = px + (size_t)y * w * 4;
                for (int x = 0; x < w; x++) {
                    double a = 0.55 * ay * (Phi((x - left) / sig) - Phi((x - right) / sig));
                    BYTE* p = row + (size_t)x * 4;
                    p[0] = p[1] = p[2] = 0;
                    p[3] = (BYTE)std::lround(a * 255.0);
                }
            }
            if (!shdc) shdc = CreateCompatibleDC(dc);
            if (shb) DeleteObject(shb);
            shb = nb;
            SelectObject(shdc, shb);
            shw = w; shh = h; shdpi = g.dpi; shed = editW;
        }
    }
    if (shb) {
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, ox, oy, w, h, shdc, 0, 0, w, h, bf);
    }
    // the panel: GDI+ radius-6 band (PaneBorderBrush #555555) over the
    // MenuBackgroundBrush #141414 + grain fill, corners anti-aliased - the
    // grain tile phases with the view's own so the texture reads continuous
    int spx = g_grainPX, spy = g_grainPY;
    g_grainPX += ox; g_grainPY += oy;
    {
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath* outer = CardPath((float)ox + FdD(FdPadX), (float)oy + FdD(FdPadT),
                                                (float)bw, (float)bh, (float)FdD(6), 0);
        Gdiplus::GraphicsPath* inner = CardPath((float)ox + FdD(FdPadX), (float)oy + FdD(FdPadT),
                                                (float)bw, (float)bh, (float)FdD(6), 1);
        if (Gdiplus::TextureBrush* t = GrainTex(2)) gs.FillPath(t, inner);
        else { Gdiplus::SolidBrush mb(Gdiplus::Color((BYTE)255, 0x14, 0x14, 0x14)); gs.FillPath(&mb, inner); }
        Gdiplus::GraphicsPath band;
        band.AddPath(outer, FALSE);
        band.AddPath(inner, FALSE);
        band.SetFillMode(Gdiplus::FillModeAlternate);
        Gdiplus::SolidBrush edge(Gdiplus::Color((BYTE)255, 0x55, 0x55, 0x55));
        gs.FillPath(&edge, &band);
        delete outer;
        delete inner;
    }
    g_grainPX = spx; g_grainPY = spy;

    FdRects R; FdLayout(editW, R);
    // every content rect rides the window's origin: (0,0) on the real window,
    // the window's view offset in the CI shot - the shadow and panel above
    // already add (ox,oy) themselves
    if (ox || oy) {
        RECT* fr[] = { &R.bar, &R.grip, &R.icon, &R.edit, &R.status,
                       &R.btn[FB_AA], &R.btn[FB_AB], &R.btn[FB_PREV], &R.btn[FB_NEXT], &R.btn[FB_CLOSE] };
        for (RECT* rp : fr) { rp->left += ox; rp->top += oy; rp->right += ox; rp->bottom += oy; }
    }
    SetBkMode(dc, TRANSPARENT);
    // the query edit: a radius-3 box on BackgroundBrush #1c1c1c with the
    // CardBorderBrush #4a4a4a ring; the real edit control covers the interior,
    // and with no control (the CI shot) the query text is drawn in its place
    {
        int x = R.edit.left - 1, y = R.edit.top - 1;
        int ew = R.edit.right - R.edit.left + 2, eh = R.edit.bottom - R.edit.top + 2;
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath* outer = CardPath((float)x, (float)y, (float)ew, (float)eh, (float)FdD(3), 0);
        Gdiplus::GraphicsPath* inner = CardPath((float)x, (float)y, (float)ew, (float)eh, (float)FdD(3), 1);
        Gdiplus::SolidBrush bg(Gdiplus::Color((BYTE)255, 0x1c, 0x1c, 0x1c));
        gs.FillPath(&bg, inner);
        Gdiplus::GraphicsPath band;
        band.AddPath(outer, FALSE);
        band.AddPath(inner, FALSE);
        band.SetFillMode(Gdiplus::FillModeAlternate);
        Gdiplus::SolidBrush ring(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a));   // CardBorderBrush (Dark)
        gs.FillPath(&ring, &band);
        delete outer;
        delete inner;
        if (!g_fedit && !fd.query.empty()) {
            RECT tr{ R.edit.left + FdD(6), R.edit.top, R.edit.right - FdD(6), R.edit.bottom };
            SelectObject(dc, FdFontEdit());
            SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
            DrawTextW(dc, fd.query.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    // the drag grip: KillerPDF's two columns of three 3px dots, MutedTextBrush,
    // the affordance the sidebar splitter and the annotate bars share - grabbing
    // it moves the whole bar (EnablePanelDrag)
    {
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush dot(Gdiplus::Color((BYTE)255, 0xa3, 0xa3, 0xa3));   // MutedTextBrush
        float gx = (float)R.grip.left + (float)std::lround(3.5 * g.dpi);  // the 1.5px dot
        float gy = (float)R.grip.top + (float)std::lround(5.5 * g.dpi);   // margins inside
        for (int c = 0; c < 2; c++)
            for (int ri = 0; ri < 3; ri++)
                gs.FillEllipse(&dot, gx + c * FdD(6), gy + ri * FdD(6),
                               (float)std::lround(3 * g.dpi), (float)std::lround(3 * g.dpi));
    }
    // the magnifier glyph
    SelectObject(dc, FdFontGlyph());
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));   // MutedTextBrush
    DrawTextW(dc, L"\uE721", -1, &R.icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // the option toggles: KillerPDF's UpdateSearchOptionButtons - the on state
    // fills with SelectionBg's green gradient and SelectionFg white text; the
    // off state is ToolbarButton's RowHover fill with the TextBrush glyph
    for (int b = FB_AA; b <= FB_AB; b++) {
        bool on = b == FB_AA ? fd.matchCase : fd.wholeWord;
        bool hot = fd.hot == b;
        if (on || hot) {
            Gdiplus::Graphics gs(dc);
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            RECT r = R.btn[b];
            Gdiplus::GraphicsPath p;
            float rr = (float)FdD(3);
            float fx = (float)r.left, fy = (float)r.top, fw = (float)(r.right - r.left), fh = (float)(r.bottom - r.top);
            p.AddArc(fx, fy, 2 * rr, 2 * rr, 180, 90);
            p.AddArc(fx + fw - 2 * rr, fy, 2 * rr, 2 * rr, 270, 90);
            p.AddArc(fx + fw - 2 * rr, fy + fh - 2 * rr, 2 * rr, 2 * rr, 0, 90);
            p.AddArc(fx, fy + fh - 2 * rr, 2 * rr, 2 * rr, 90, 90);
            p.CloseFigure();
            if (on) {
                Gdiplus::Rect gr(fx, fy, fw, fh);   // SelectionBg: #387552 -> #104e2b
                Gdiplus::LinearGradientBrush gb(gr, Gdiplus::Color((BYTE)255, 0x38, 0x75, 0x52),
                                                   Gdiplus::Color((BYTE)255, 0x10, 0x4e, 0x2b), 90.0f);
                gs.FillPath(&gb, &p);
            } else {
                Gdiplus::SolidBrush f(Gdiplus::Color((BYTE)255, 0x40, 0x40, 0x40));   // RowHoverBrush
                gs.FillPath(&f, &p);
            }
        }
        RECT r = R.btn[b];
        SelectObject(dc, b == FB_AA ? FdFontOpt() : FdFontAb());
        SetTextColor(dc, on ? RGB(0xff, 0xff, 0xff)      // SelectionFg
                            : RGB(0xe0, 0xe0, 0xe0));    // TextBrush
        DrawTextW(dc, b == FB_AA ? L"Aa" : L"ab", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // the counter cell: KillerPDF's compact "12 / 73" (current / total), or
    // "No matches" once the scan came back empty. While the scan is still
    // running the counter already shows the hits so far - the cursor follows
    // the start rule (first match at or after your page) on the partial list -
    // because the numbers are the answer to "is it finding anything" and they
    // land one OCR tick after typing stops, not after a whole-book crawl.
    std::wstring st;
    if (!fd.query.empty() && fd.query.size() >= 2) {
        if (fd.done) {
            if (!fd.pending)   // a finished scan speaks; a pending debounce says nothing -
                               // a flash of "No matches" between keystrokes is a lie
                st = fd.hits.empty() ? L"No matches"
                                     : std::to_wstring(fd.cur + 1) + L" / " + std::to_wstring(fd.hits.size());
        } else if (!fd.hits.empty()) {
            int start = max(0, (fd.pane ? g.dB : g.d).cur), pc = 0;
            for (int i = 0; i < (int)fd.hits.size(); i++)
                if (fd.hits[i].page >= start) { pc = i; break; }
            st = std::to_wstring(pc + 1) + L" / " + std::to_wstring(fd.hits.size());
        }
    }
    if (!st.empty()) {
        RECT r = R.status;
        SelectObject(dc, FdFontStat());
        SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));   // MutedTextBrush
        DrawTextW(dc, st.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // the nav buttons: ChevronUp / ChevronDown / Cancel. KillerPDF's ToolbarButton
    // keeps the TextBrush glyph over a RowHover fill; the close X is the shared
    // danger style - no fill at all, the MutedTextBrush glyph turns DangerRed.
    const wchar_t* glyph[3] = { L"\uE70E", L"\uE70D", L"\uE711" };
    for (int b = FB_PREV; b <= FB_CLOSE; b++) {
        bool hot = fd.hot == b;
        if (hot && b != FB_CLOSE) {
            RECT r = R.btn[b];
            Gdiplus::Graphics gs(dc);
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath p;
            float rr = (float)FdD(3);
            float fx = (float)r.left, fy = (float)r.top, fw = (float)(r.right - r.left), fh = (float)(r.bottom - r.top);
            p.AddArc(fx, fy, 2 * rr, 2 * rr, 180, 90);
            p.AddArc(fx + fw - 2 * rr, fy, 2 * rr, 2 * rr, 270, 90);
            p.AddArc(fx + fw - 2 * rr, fy + fh - 2 * rr, 2 * rr, 2 * rr, 0, 90);
            p.AddArc(fx, fy + fh - 2 * rr, 2 * rr, 2 * rr, 90, 90);
            p.CloseFigure();
            Gdiplus::SolidBrush f(Gdiplus::Color((BYTE)255, 0x40, 0x40, 0x40));   // RowHoverBrush
            gs.FillPath(&f, &p);
        }
        SelectObject(dc, FdFontGlyph());
        SetTextColor(dc, b == FB_CLOSE ? (hot ? RGB(0xE0, 0x44, 0x44)   // DangerRed
                                              : RGB(0xa3, 0xa3, 0xa3))   // MutedTextBrush
                                       : RGB(0xe0, 0xe0, 0xe0));         // TextBrush
        DrawTextW(dc, glyph[b - FB_PREV], -1, &R.btn[b], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// The edit's keys: Enter steps, Shift+Enter steps back, Escape closes, Ctrl+F
// closes (the toggle), Alt+C / Alt+W flip the option toggles from the box -
// KillerPDF's SearchBox_KeyDown + TrySearchOptionKey; everything else edits.
static LRESULT CALLBACK FindEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        if (wp == VK_RETURN) { FindStep(shift ? -1 : 1); return 0; }
        if (wp == VK_ESCAPE) { FindClose(); return 0; }
        if (wp == VK_F3) { FindStep(shift ? -1 : 1); return 0; }   // F3 steps from the box too
        if (wp == 'A' && ctrl) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }   // a single-line
        if (wp == 'F' && ctrl) { FindClose(); return 0; }   // EDIT has no built-in Ctrl+A
    } else if (m == WM_SYSKEYDOWN) {                   // the Alt+ chords land here
        if (wp == 'C') { FindOption(true); return 0; }     // Match Case
        if (wp == 'W') { FindOption(false); return 0; }    // Whole Word
    } else if (m == WM_MOUSEMOVE) {                    // the edit is a child: its moves leave the
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; // bar's hover tiles stale (a button under
        MapWindowPoints(h, g.view, &p, 1);             // the cursor stays lit) - hand them to the
        SendMessageW(g.view, WM_MOUSEMOVE, 0,          // view so the tiles clear, then let the
                     MAKELPARAM(p.x, p.y));            // edit still run its own drag-select
    } else if (m == WM_MOUSEWHEEL) {                   // the wheel scrolls the page from the box
        ViewWheel(wp, lp); return 0;                   // too, the way WPF bubbles it upward
    }
    return CallWindowProcW(g_feditDef, h, m, wp, lp);
}

// Park the edit inside its ring. The bar itself is composite pixels - there is
// no bar window to move, ever, so a drag, a resize or an open only re-parks
// this one control: the line-box window (FdEditGeom) centred in the 26px box,
// in VIEW coordinates at the bar's current origin. The composite repaints from
// the same invalidate that follows the move - one frame, no seams.
//
// The repaint contract is the whole fix for the transparency/artifacts horror:
// WS_CLIPCHILDREN clips this child's rect OUT of the view's paint, so the
// pixels under the edit are painted by the edit ALONE - nothing else will ever
// fill them. MoveWindow(.., FALSE) promised "no repainting of any kind", so
// after every park the control's new rect kept showing whatever was on screen
// before it (page pixels = "the input turns transparent"), until a caret blink
// or a keystroke repainted a sliver - the strobing patchwork. So: skip the
// move when the rect is already right (LayoutMain parks on every WM_SIZE),
// and on a real move repaint the child itself (bRepaint=TRUE) AND invalidate
// its full client for an erase+redraw in the same frame the view recomposites
// around it. The edit's WM_CTLCOLOREDIT brush is opaque #1c1c1c - the same
// tone the composite fills the box with - so edit and pixels meet seamlessly.
static void FindPlace() {
    if (!g_fedit || !g.view) return;
    FindClampDrag();                              // a resized card re-clamps the drag offset
    int bx, by; FdOrigin(bx, by);
    FdRects R; FdLayout(FdEditW(CardW(FocusB())), R);
    int eh, eoff; FdEditGeom(R.edit.bottom - R.edit.top, eh, eoff);
    RECT want{ bx + R.edit.left, by + R.edit.top + eoff,
               bx + R.edit.right, by + R.edit.top + eoff + eh };
    RECT have; GetWindowRect(g_fedit, &have);     // screen coords on both, cheap compare
    POINT o0{ want.left, want.top }, o1{ have.left, have.top };
    ScreenToClient(g.view, &o0); ScreenToClient(g.view, &o1);
    if (o0.x != o1.x || o0.y != o1.y ||           // POINT has no operator!= in the
        want.right - want.left != have.right - have.left ||   // Windows headers: field by field
        want.bottom - want.top != have.bottom - have.top) {
        MoveWindow(g_fedit, want.left, want.top, want.right - want.left,
                   want.bottom - want.top, TRUE);   // TRUE: the child repaints as it lands
        InvalidateRect(g_fedit, nullptr, TRUE);     // ...and one full erase+redraw on top:
                                                    // no stale rect, no patchwork, ever
    }
}

// The grip's drag bounds: the bar's visual rect (window minus the shadow
// margins) must stay fully on the card. A bar wider than the card collapses
// the range instead of letting std::clamp see lo > hi.
static void FindClampDrag() {
    int vw = CardW(FocusB());
    int bw = FdD(FdBarFix) + FdEditW(vw), bh = FdD(FdBarH);
    int loX = bw + FdD(16) - vw, hiX = FdD(16);
    fd.dx = std::clamp(fd.dx, min(loX, hiX), max(loX, hiX));
    int loY = -FdD(6), hiY = max(-FdD(6), g.ch - FdD(6) - bh);
    fd.dy = std::clamp(fd.dy, min(loY, hiY), max(loY, hiY));
}

