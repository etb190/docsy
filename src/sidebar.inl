// ---------------- the sidebar rail: Split pane / Pages / Document Info ----------------
// KillerPDF's SidebarToggleStrip, in docsy's container language: a 24px strip
// that always sits between the sidebar and the page (the sidebar collapses TO
// it, so a closed sidebar comes back from the same place), the shared grain
// field, and a bottom-anchored button stack in the reference's locked order -
// Split pane, Pages, Document Info. The editing tools are NOT rail buttons in
// KillerPDF: they live in the toolbar's RightBar, and that is where docsy
// carries them (the toolbar row above). The rail is exactly the reference's
// three buttons, with their logic and nothing else: no outline, comments,
// attachments or theme buttons come over. Each vector icon reads at 16px and
// never depends on a glyph being present in the OS font.

enum { RB_SPLIT, RB_PAGES, RB_INFO, RB_COUNT };

static int RW() { return (g.full || g_webOn) ? 0 : (int)(24 * g.dpi); }   // full screen gives the column to the page

static void RailRects(RECT r[RB_COUNT], int h) {   // 24px cells, 6px gaps, 6px off the bottom
    int bw = (int)(24 * g.dpi), gap = (int)(6 * g.dpi);
    int x = max(0, (RW() - bw) / 2);
    int y = max(0, h - (int)(6 * g.dpi) - RB_COUNT * bw - (RB_COUNT - 1) * gap);
    for (int i = 0; i < RB_COUNT; i++) {
        r[i] = { x, y, x + bw, y + bw };
        y += bw + gap;
    }
}

// The green state: the feature a button toggles is on - the split, the
// sidebar (KillerPDF lights a panel tab only while its sidebar shows it). The
// info button never lights: KillerPDF's DocInfoRailBtn has no "on" trigger,
// it is a momentary action, not a toggle. Pages needs a document; Split does
// not (KillerPDF's split acts on the WINDOW - pane B simply opens on its start
// screen).
static bool RailOn(int id) {
    return id == RB_SPLIT ? g.split
         : id == RB_PAGES ? (g.sideOn && DVF().n)
         : false;
}
static bool RailEnabled(int id) {
    return id == RB_SPLIT ? true : (DVF().n > 0);
}

// KillerPDF centers the rail glyphs with GlyphInk.Center (Controls/GlyphInk.cs):
// icon fonts draw glyphs off-center inside their advance box, so the text is
// shifted by the distance between the advance-box center and the INK center on
// both axes. The GDI twin reads the same numbers from the font - the ink box
// and origin from GetGlyphOutlineW's metrics, the advance box and the line
// box from the text metrics DrawTextW centers by - and shifts the draw rect by
// that distance, so the glyph lands with the same space on every side.
static void InkCenterRect(HDC dc, const RECT& cell, const wchar_t* glyph, RECT& out) {
    out = cell;
    TEXTMETRICW tm{};
    if (!GetTextMetricsW(dc, &tm)) return;
    GLYPHMETRICS gm{}; MAT2 mat{ {0, 1}, {0, 0}, {0, 0}, {0, 1} };
    if (GetGlyphOutlineW(dc, (UINT)glyph[0], GGO_METRICS, &gm, 0, nullptr, &mat) == GDI_ERROR)
        return;
    // vertical: the baseline sits tmAscent under the line box's top (DrawTextW
    // centers the tmHeight line box, no external leading), the ink's top sits
    // gmptGlyphOrigin.y above the baseline, the box runs gmBlackBoxY down
    double inkC = tm.tmAscent - gm.gmptGlyphOrigin.y + gm.gmBlackBoxY / 2.0;
    double dy = tm.tmHeight / 2.0 - inkC;
    SIZE adv{}; GetTextExtentPoint32W(dc, glyph, 1, &adv);
    double dx = adv.cx / 2.0 - gm.gmptGlyphOrigin.x - gm.gmBlackBoxX / 2.0;
    OffsetRect(&out, (int)std::lround(dx), (int)std::lround(dy));
}

// The glyphs are KillerPDF's own, drawn from the same font and codepoints the
// reference buttons use - Segoe MDL2 Assets, the system icon font Windows 10+
// always ships (the toolbar's E721 and the caption's E921/E922/E923/E8BB
// already ride it): Split pane swaps E89F (a pane pushed out - "open one")
// while closed for E8A0 (a pane pulled in) while open at FontSize 14, Pages
// shows E8A9 and Document Info E946 at FontSize 15 (the RailPanelBtn and
// DocInfoRailBtn templates). Under each ink pass sits the reference's icon
// shadow - black, ShadowDepth 2, Direction 270 (straight down), 0.9 in the
// dark theme - which GDI speaks as the app's one dark copy under the light
// pass, the same treatment the toolbar's E721 and the caption glyphs get.
static void RailIcon(HDC dc, int id, const RECT& rc, COLORREF c, bool shadow = true) {
    struct Spec { const wchar_t* closed; const wchar_t* open; HFONT* font; };
    static HFONT f14 = nullptr, f15 = nullptr;
    static const Spec specs[RB_COUNT] = {
        { L"\uE89F", L"\uE8A0", &f14 },   // Split pane, FontSize 14, glyph swaps on state
        { L"\uE8A9", L"\uE8A9", &f15 },   // Pages (RailPanelBtn), FontSize 15
        { L"\uE946", L"\uE946", &f15 },   // Document Info, FontSize 15
    };
    const Spec& sp = specs[id];
    const wchar_t* glyph = (id == RB_SPLIT && g.split) ? sp.open : sp.closed;
    int px = id == RB_SPLIT ? 14 : 15;
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, CaptionFont(px == 14 ? f14 : f15, px, false, L"Segoe MDL2 Assets"));
    RECT in; InkCenterRect(dc, rc, glyph, in);   // GlyphInk.Center's shift, both passes
    if (shadow) {                           // the shadow pass: depth 2, straight down -
        RECT sh = in;
        OffsetRect(&sh, 0, (int)std::lround(2 * g.dpi));
        SetTextColor(dc, RGB(16, 16, 16));
        DrawTextW(dc, glyph, -1, &sh, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(dc, c);
    DrawTextW(dc, glyph, -1, &in, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---- KillerPDF's PaneShadow, landing on the rail ----
// The card butts the rail and casts its PaneShadow on all four sides (SidebarLayout.cs:
// the hand-rolled SidebarShadow gradient died the day DocPaneBorder carried the real
// one, because the real one already darkens the surface beside the card). Bottom over
// the footer, right over the window gutter - and the LEFT halo over the rail's field,
// which KillerPDF's own dark-theme render reads as a 28->20 dip across the rail's last
// ~8px. The top shows nothing anywhere: the card tucks under the opaque tab band. The
// rail is a separate child window, so its share of the halo lands here - the same
// falloff the footer paints (black, blur 16, depth 5 down, opacity 0.6), factored for
// the card's LEFT edge: the vertical term fades in across the silhouette's pushed-down
// top edge, the horizontal term runs the gaussian leftward from the rail's right edge,
// and the corner pull-in c recedes near the card's rounded top-left corner the way the
// footer's does around its bottom ones. (The bottom corner wraps below the rail's last
// row - the silhouette reaches 5px past it - so only the top needs the pull-in.) Baked
// once per height/dpi into a black alpha strip whose right edge abuts the card and
// AlphaBlended over whatever the rail has drawn - both paint paths, icons included,
// which is KillerPDF's own z-order: the shadow's pane renders above the sidebar's.
static HBITMAP g_rshbm = nullptr; static HDC g_rshdc = nullptr;
static int g_rshw = 0, g_rshh = 0, g_rshins = -1; static double g_rshdpi = 0;

static void DrawRailShadow(HDC dc, int w, int h, int ins2) {
    if (w <= 0 || h <= 0) return;
    int sw = min(w, (int)(20 * g.dpi) + 2);   // the visible reach: ~2.5 sigma, past this
                                              // the falloff is under one percent of ink
    int ins = ins2 >= 0 ? ins2                // an explicit card-top row: the browser's rail
              : max(0, BandInset(false) - 1); // borrows the painter with its own geometry
                                              // (the card's top row: the halo fades in where
                                              // the card starts, under the band when one shows)
    if (!g_rshbm || g_rshw != sw || g_rshh != h || g_rshdpi != g.dpi || g_rshins != ins) {
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), sw, -h, 1, 32, BI_RGB };
        BYTE* px = nullptr;
        HBITMAP bm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
        if (px && bm) {
            const double sig = 8.0 * g.dpi, depth = 5.0 * g.dpi;
            int rad = max(2, (int)(6 * g.dpi));
            for (int y = 0; y < h; y++) {
                double vy = Phi((y - ins - depth) / sig);   // the silhouette starts 5px down
                                                            // of the card's top: the halo
                                                            // fades in over its top edge
                double cy = max(0.0, (double)y - ins - depth);   // height past that top edge
                double c = cy < rad ? rad - std::sqrt((double)rad * rad - (rad - cy) * (rad - cy))
                                    : 0.0;               // the corner's pull-in, footer-style
                BYTE* row = px + (size_t)y * sw * 4;
                for (int x = 0; x < sw; x++) {
                    // the card's left edge abuts the strip's right edge (x = sw): the
                    // gaussian runs leftward from it, pulled in by the corner above
                    double a = 0.6 * vy * Phi((x - sw - c) / sig);
                    BYTE* p = row + (size_t)x * 4;
                    p[0] = p[1] = p[2] = 0;
                    p[3] = (BYTE)std::lround(a * 255.0);
                }
            }
            if (!g_rshdc) g_rshdc = CreateCompatibleDC(dc);
            if (g_rshbm) DeleteObject(g_rshbm);
            g_rshbm = bm;
            SelectObject(g_rshdc, g_rshbm);
            g_rshw = sw; g_rshh = h; g_rshdpi = g.dpi; g_rshins = ins;
        }
    }
    if (g_rshbm) {
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, w - sw, 0, sw, h, g_rshdc, 0, 0, sw, h, bf);
    }
}

static void RailDraw(HDC dc, int w, int h) {
    RECT r{ 0, 0, w, h };
    FillRect(dc, &r, GrainBrush(1));        // the shared field, so the rail reads as part
                                            // of the one container wrapping the page. No
                                            // seam of its own: KillerPDF's rail is the
                                            // SidebarRailBrush field and nothing else - the
                                            // card's #555 ring at the view's edge is the only
                                            // line there, the same one every other boundary
                                            // shows, and the page shadow paints over the field
                                            // exactly like it does on the sidebar and footer.
    RECT bs[RB_COUNT]; RailRects(bs, h);
    for (int i = 0; i < RB_COUNT; i++) {
        bool en = RailEnabled(i), on = en && RailOn(i), hot = en && g.railHot == i;
        // KillerPDF's RailPanelBtn template: the glyph IS the button - no face,
        // no fill, no pressed tile. Rest TextBrush #e0e0e0, hover and lit
        // PrimaryBrush #22bb56, disabled the glyph at Opacity 0.35: 0.35 of
        // #e0e0e0 over the #1c1c1c field is #616161, not the #4e4e4e the first
        // port guessed with.
        COLORREF c = !en ? RGB(0x61, 0x61, 0x61)
                   : (on || hot) ? RGB(0x22, 0xbb, 0x56)
                   : RGB(0xe0, 0xe0, 0xe0);
        RailIcon(dc, i, bs[i], c);
    }
    DrawRailShadow(dc, w, h);               // the card's left halo, over the icons the way
}                                           // KillerPDF's z-order paints it - last ink down

static void RailRepaint() { if (g.rail) InvalidateRect(g.rail, nullptr, FALSE); }

// The hover/click map over the bottom stack. Returns the rail id, -1 for nothing.
static int RailHitAt(POINT p, int hh) {
    RECT bs[RB_COUNT]; RailRects(bs, hh);
    for (int i = 0; i < RB_COUNT; i++) if (PtInRect(&bs[i], p)) return i;
    return -1;
}

static void RailActivate(int id) {
    if (id == RB_SPLIT) ToggleSplit();      // KillerPDF's SplitPaneRailBtn_Click = F10
    else if (id == RB_PAGES) ToggleSide();  // the Pages tab opens the sidebar's pages panel
    else InfoOpen();                        // KillerPDF's DocInfo_Click: the button stays
}                                           // enabled; a no-document click is told to the reader

static LRESULT CALLBACK RailProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureRailBack(dc, r.right, r.bottom);   // one buffered frame per hover flip:
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // a direct paint lands in
            SetBrushOrgEx(g_raildc, -po.x, -po.y, nullptr);   // two visible steps (the fill
            RailDraw(g_raildc, r.right, r.bottom);   // erases the icons, then they redraw)
            BitBlt(dc, 0, 0, r.right, r.bottom, g_raildc, 0, 0, SRCCOPY);   // and strobes -
        }                                            // the blit shows one state, like the bar
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {               // mirrors/hover heals, like the toolbar's
        RECT r; GetClientRect(h, &r);
        POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
        SetBrushOrgEx((HDC)wp, -po.x, -po.y, nullptr);
        RailDraw((HDC)wp, r.right, r.bottom);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g.resizing) {                     // the resize drag rides the capture: the sidebar
            POINT q{ p.x, p.y };              // width follows the pointer wherever it crosses
            MapWindowPoints(h, g.view, &q, 1);
            int w = g.rsW0 + (q.x - g.rsX);
            if (w != g.sideW) { g.sideW = w; LayoutMain(); }
            return 0;
        }
        RECT rr; GetClientRect(h, &rr);
        int hot = RailHitAt(p, rr.bottom);
        if (hot != g.railHot) {                 // repaint on a hover flip, never per move
            g.railHot = hot;
            if (hot >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.railHot >= 0 || g.railPressed) {
            g.railHot = -1; g.railPressed = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        // the rail's left edge IS the sidebar's resize handle now: the view no
        // longer borders the sidebar, this 24px strip does - capture-held, so
        // the drag keeps counting wherever the pointer crosses
        if (g.sideOn && DVF().n && p.x < (int)(5 * g.dpi)) {
            g.railHot = -1;
            g.resizing = true;
            POINT q{ p.x, p.y }; MapWindowPoints(h, g.view, &q, 1);
            g.rsX = q.x; g.rsW0 = SW();
            SetCapture(h);
            return 0;
        }
        RECT rr; GetClientRect(h, &rr);
        if (RailEnabled(RailHitAt(p, rr.bottom)) && RailHitAt(p, rr.bottom) >= 0) {
            g.railPressed = true;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g.resizing) {                     // the sidebar drag ends: one relayout, thumbs rebuild
            g.resizing = false;
            if (GetCapture() == h) ReleaseCapture();
            LayoutMain();
            return 0;
        }
        if (!g.railPressed) return 0;
        g.railPressed = false;
        if (GetCapture() == h) ReleaseCapture();
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rr; GetClientRect(h, &rr);
        int hit = RailHitAt(p, rr.bottom);
        if (hit >= 0 && g.railHot == hit) RailActivate(hit);   // press + release on one button
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        g.resizing = false; g.railPressed = false;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (g.sideOn && DVF().n && p.x < (int)(5 * g.dpi)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE;   // the sidebar's edge
            }
            RECT rr; GetClientRect(h, &rr);
            int hit = RailHitAt(p, rr.bottom);
            if (hit >= 0 && RailEnabled(hit)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;   // ToolbarButton's Cursor=Hand
            }
        }
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---- Split pane: KillerPDF's two-document toggle ----
// Opening: pane A keeps the width it already has and the WINDOW grows to make
// room for pane B, which opens matching it - halving pane A instead would
// resize the document you are reading just to put a second one beside it. The
// window grows as far as the work area allows (sliding left first when the
// right edge is out of room, KillerPDF's ShiftAndResizeWindow); a window that
// cannot grow that far splits what it already has evenly instead. Closing
// hands back exactly what was grown, so F10 round-trips to where it started.
// Pane B starts on its own start screen - a fresh pane has no document -
// and a re-split resumes whatever pane B held last (KillerPDF's kept session).
static void ToggleSplit() {
    if (!g.view) return;
    if (!g.split) {
        int aNow = max(PaneMin(), g.cw);      // pane A keeps the width it already has
        int gut = SplitGap();
        int want = aNow + gut;                // pane B opens matching it, gutter between
        MONITORINFO mi{ sizeof mi };
        HMONITOR mon = MonitorFromWindow(g.main, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(mon, &mi);
        RECT rc{}; GetWindowRect(g.main, &rc);
        int roomRight = max(0, (int)(mi.rcWork.right - rc.right));
        int roomLeft = max(0, (int)(rc.left - mi.rcWork.left));
        // maximized or snapped: the window stays where Windows put it and
        // splits what it already has (KillerPDF's evenSplit rule)
        if (IsZoomed(g.main)) { roomRight = 0; roomLeft = 0; }
        // not enough room to the right: slide the window left first, as far as
        // its own monitor allows, so pane A can keep its width
        int shift = roomRight < want ? min(want - roomRight, roomLeft) : 0;
        if (shift > 0)
            SetWindowPos(g.main, nullptr, rc.left - shift, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        int grow = min(want, roomRight + shift);   // pane B's final width is whatever
        bool even = grow + 1 < want;               // the window could actually give
        g.split = true;
        g.splitGrow = even ? 0 : grow;             // remembered so closing gives back only
        if (even) {                                // what the window grew (0: closes in place)
            g.splitX = std::clamp(g.cw / 2, PaneMin(), max(PaneMin(), g.cw - SplitHalf() - PaneMin()));
        } else {
            g.splitX = aNow - SplitHalf();         // the line lands just past pane A's card
        }
        g.splitA = true;                           // the reader keeps the pane they were reading
        g.splitDrag = false; g.lineHot = false;
        g.dB.vHot = g.dB.hHot = g.dB.vDrag = g.dB.hDrag = false;
        if (!even)                                  // the window grows onto pane B: WM_SIZE
            SetWindowPos(g.main, nullptr, 0, 0,     // relayouts, refits and repaints both panes
                         (rc.right - rc.left) + grow, 0, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        else
            LayoutMain();
    } else {
        g.split = false;
        int grow = g.splitGrow;               // hand back exactly what opening grew
        g.splitGrow = 0;
        RECT rc{}; GetWindowRect(g.main, &rc);
        if (grow > 0)
            SetWindowPos(g.main, nullptr, 0, 0, (rc.right - rc.left) - grow, 0,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        bool wasB = !g.splitA;                // the focused pane at the moment of closing
        g.splitA = true;                      // the window-level surfaces read pane A again
        if (wasB && g.dB.path != g.d.path) {
            // The sidebar described pane B's document: its thumbs, selection
            // and scroll belong to that file and go with it. Closing a split
            // whose panes share one document (or whose pane B is empty) keeps
            // the sidebar exactly where it was - KillerPDF's close does not
            // touch it either.
            for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
            sd.th.clear(); sd.thBytes = 0; sd.scroll = 0; sd.cur = -1; sd.hold = 0;   // cw/ch stay - zeroing
                                                                                      // them dead-returns SideLayout
        }
        SideLayout();
        UpdateTitle();
        StatusRepaint();
        LayoutMain();
    }
    RailRepaint();
    InvalidateRect(g.view, nullptr, FALSE);
}

