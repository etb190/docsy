// ---- small helpers the Safari chrome shares ----

static HFONT WebGlyphFont(int px) {                // Segoe MDL2 at px (dpi-scaled), cached per size:
    static HFONT f[3] = {}; static int sz[3] = {}; static double fd = 0;   // CaptionFont's slot keeps
    if (fd != g.dpi) {                             // the FIRST size it was asked for, so the
        for (auto& hf : f) if (hf) { DeleteObject(hf); hf = nullptr; }     // browser keeps its own
        fd = g.dpi;
    }
    for (int i = 0; i < 3; i++) if (f[i] && sz[i] == px) return f[i];
    for (int i = 0; i < 3; i++) if (!f[i]) {
        f[i] = CreateFontW(-(int)std::lround(px * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
        sz[i] = px;
        return f[i];
    }
    return f[0];
}

// What the idle address field shows: Safari prints the site, not the URL -
// "google.com", no scheme, no "www.", no path. Anything that is not a
// plain web address (about:, edge:) is shown whole.
static std::wstring WebHostOf(const std::wstring& u) {
    if (u.rfind(L"https://", 0) != 0 && u.rfind(L"http://", 0) != 0) return u;
    std::wstring r = u.substr(u.find(L"://") + 3);
    size_t e = r.find_first_of(L"/?#");
    if (e != std::wstring::npos) r.resize(e);
    size_t at = r.find(L'@');
    if (at != std::wstring::npos) r.erase(0, at + 1);
    if (r.rfind(L"www.", 0) == 0) r.erase(0, 4);
    return r;
}

// ---- the tab band: the reader's own tab strip, on a flat Safari bar ----
// The tab faces are the reader's (TabBandPaint): 3px inset, 29px faces, the
// 1px dividers, the muted label that brightens on the open tab, the close x
// that turns danger-red, the green rule over the open tab. What changes is the
// ground: a flat darker bar instead of grain, equal cells that fill the whole
// width the way Safari's do, titles centered in them, the open tab raised to
// the toolbar's own sheet. The band exists only from the SECOND tab - exactly
// like the reader's strip - so a single page gets the full height.

static int WebBandH() { return (g_webOn && g_wt.size() > 1) ? WEBTABH() : 0; }

struct WebTabGeo { int x0, cell, limit, faceTop, faceH, n, vis, start; };
static WebTabGeo WebTabLayout(int w) {
    WebTabGeo G;
    G.n = (int)g_wt.size();
    G.faceTop = TABF(3); G.faceH = TABF(29);
    G.x0 = 0; G.limit = w;
    int avail = max(0, G.limit - G.x0);
    G.cell = G.n > 0 ? max(TABF(120), avail / G.n) : 0;
    G.vis = G.n > 0 ? max(1, min(G.n, avail / max(1, G.cell))) : 0;
    G.start = 0;
    if (G.vis < G.n && g_wtCur >= G.vis) G.start = min(g_wtCur - G.vis + 1, G.n - G.vis);
    return G;
}

// the tab row's hit test: a tab index, -1 for bare sheet; `close` is the
// reader's 24px close-x zone at the face's right edge
static int WebTabRowHit(POINT p, int w, int h, bool& close) {
    (void)h;
    close = false;
    WebTabGeo G = WebTabLayout(w);
    if (p.y < 0 || p.y >= h) return -1;
    for (int k = 0; k < G.vis; k++) {
        int x = G.x0 + k * G.cell;
        if (p.x >= x && p.x < x + G.cell) {
            int xr = x + G.cell - TABF(2);
            close = p.x >= xr - TABF(24) && p.x < xr && p.y >= G.faceTop && p.y < G.faceTop + G.faceH;
            return G.start + k;
        }
    }
    return -1;
}

// the nav row's buttons, in strip coords; empty rects (the + no longer rides
// this row) never match
static int WebNavHit(POINT p) {
    for (int i = 0; i < WB_COUNT; i++) {
        if (g_wbR[i].right <= g_wbR[i].left) continue;
        if (p.x >= g_wbR[i].left && p.x < g_wbR[i].right && p.y >= g_wbR[i].top && p.y < g_wbR[i].bottom) return i;
    }
    return -1;
}

static void WebRound(Gdiplus::GraphicsPath& p, float l, float t, float r, float b, float rr) {
    rr = std::min(rr, std::min((r - l) / 2.0f, (b - t) / 2.0f));   // an arc wider than its box overshoots
    if (rr < 0.5f) { p.AddRectangle(Gdiplus::RectF(l, t, r - l, b - t)); return; }   // (the focus ring's stray edges)
    p.AddArc(l, t, 2 * rr, 2 * rr, 180, 90);
    p.AddArc(r - 2 * rr, t, 2 * rr, 2 * rr, 270, 90);
    p.AddArc(r - 2 * rr, b - 2 * rr, 2 * rr, 2 * rr, 0, 90);
    p.AddArc(l, b - 2 * rr, 2 * rr, 2 * rr, 90, 90);
    p.CloseFigure();
}

// a rounded, antialiased fill; `rad` is in dp
static void WebTileC(HDC dc, const RECT& r, Gdiplus::Color c, float rad) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush b(c);
    Gdiplus::GraphicsPath p;
    WebRound(p, (float)r.left, (float)r.top, (float)r.right, (float)r.bottom, rad * (float)g.dpi);
    gs.FillPath(&b, &p);
}

// a toolbar button's hover/pressed layer: a soft 6dp tile
static void WebTile(HDC dc, const RECT& r, COLORREF c) {
    WebTileC(dc, r, Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)), 8.0f);
}

// Safari's focus ring: a 2dp blue outline hugging the address field
static void WebRing(HDC dc, const RECT& r, float rad, float wdp, Gdiplus::Color c) {
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float wpx = max(1.0f, wdp * (float)g.dpi), hw = wpx / 2;
    Gdiplus::GraphicsPath p;
    WebRound(p, (float)r.left + hw, (float)r.top + hw, (float)r.right - hw, (float)r.bottom - hw,
             rad * (float)g.dpi);
    Gdiplus::Pen pen(c, wpx);
    gs.DrawPath(&pen, &p);
}

// ---- the toolbar: Safari's unified bar. Caption row and toolbar are ONE flat
// sheet. Left: sidebar toggle, then the back/forward chevrons. Center: the
// address field, a capsule a step lighter than the sheet - the site name and
// its padlock centered while idle, the full address with a blue focus ring
// once clicked, reload/stop inside its right edge, a blue progress line along
// its foot while a page loads. Right: back to the PDF reader, new tab, menu. ----

// the sidebar toggle's icon: a rounded window outline with a pane on the left
static void WebIconSidebar(HDC dc, const RECT& r, COLORREF c) {
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
    float hw = 8.5f * (float)g.dpi, hh = 6.5f * (float)g.dpi;
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)),
                     1.4f * (float)g.dpi);
    Gdiplus::GraphicsPath p;
    WebRound(p, cx - hw, cy - hh, cx + hw, cy + hh, 3.0f * (float)g.dpi);
    gs.DrawPath(&pen, &p);
    float dx = cx - hw + 6.0f * (float)g.dpi;
    gs.DrawLine(&pen, dx, cy - hh, dx, cy + hh);
}

static void WebStripPaint(HDC dc, int w, int h) {
    RECT bar{ 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(wvb::StripBg);
    FillRect(dc, &bar, bg); DeleteObject(bg);          // one flat sheet, no grain
    {   // the sheet's foot fade: the reader's TabStripFade recipe - transparent to
        // 45%, #59000000 at the foot, the whole gradient at BarShadowOpacity 0.38 -
        // the lifted chrome's shadow over the page it rides above. The card's pixels
        // are the webview's own, so the shadow lives on the sheet's bottom rows.
        Gdiplus::Graphics gfade(dc);
        Gdiplus::Color c0((BYTE)0, (BYTE)0, (BYTE)0, (BYTE)0);
        Gdiplus::Color c1((BYTE)0x22, (BYTE)0, (BYTE)0, (BYTE)0);
        Gdiplus::RectF fadeR(0.f, h * 0.45f, (float)w, h * 0.55f);
        Gdiplus::LinearGradientBrush fadeBr(fadeR, c0, c1, Gdiplus::LinearGradientModeVertical);
        gfade.FillRectangle(&fadeBr, fadeR);
    }
    RECT edge{ 0, h - 1, w, h };                       // the hairline under the toolbar
    HBRUSH eb = CreateSolidBrush(wvb::Edge);
    FillRect(dc, &edge, eb); DeleteObject(eb);
    SetBkMode(dc, TRANSPARENT);
    HFONT gf = WebGlyphFont(16);
    static const wchar_t* const gl[WB_COUNT] = {
        L"\uE8A5", L"\uE76B", L"\uE76C", nullptr, L"\uE710", L"\uE712", nullptr, L"\uE7B8"
    };                                                 // reload rides the field, the sidebar icon is drawn
    for (int i = 0; i < WB_COUNT; i++) {
        if (g_wbR[i].right <= g_wbR[i].left) continue;
        if (i == WB_RELOAD) continue;                  // its tile draws inside the field
        bool en = WebBtnEnabled(i);
        bool hot = en && g.tbTabHot == i, pr = en && g.tbPressed == i;
        bool on = i == WB_SIDE && g_webSide;           // the sidebar button stays lit while it is open
        if (hot || pr || on) WebTile(dc, g_wbR[i], pr ? wvb::Pressed : (hot ? wvb::Hover : wvb::Omnibox));
        COLORREF ink = !en ? wvb::InkOff : wvb::Ink;
        if (i == WB_SIDE) { WebIconSidebar(dc, g_wbR[i], ink); continue; }
        SelectObject(dc, gf);
        SetTextColor(dc, ink);
        RECT rc = g_wbR[i];
        DrawTextW(dc, gl[i], -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    for (size_t k = 0; k < g_wxR.size() && k < g_wxIdx.size(); k++) {   // the extensions' toolbar icons
        const WebExt& e = g_wx[g_wxIdx[k]];
        RECT r = g_wxR[k];
        bool hot = (int)k == g_wxHot, pr = hot && g_wxPress == (int)k;
        if (hot || pr) WebTile(dc, r, pr ? wvb::Pressed : wvb::Hover);
        int is = (int)std::lround(18 * g.dpi);
        int ix = (int)(r.left + r.right - is) / 2, iy = (int)(r.top + r.bottom - is) / 2;
        Gdiplus::Graphics gx(dc);
        gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (e.icon) {
            gx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            gx.DrawImage(e.icon, ix, iy, is, is);
        } else {                                       // no icon: the first letter in a disc
            Gdiplus::SolidBrush db(Gdiplus::Color(255, 0x55, 0x55, 0x58));
            gx.FillEllipse(&db, ix, iy, is, is);
            RECT lr{ ix, iy, ix + is, iy + is };
            wchar_t ch[2] = { e.name.empty() ? L'?' : (wchar_t)towupper(e.name[0]), 0 };
            SelectObject(dc, g_webEditFont ? g_webEditFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(dc, wvb::Ink);
            DrawTextW(dc, ch, 1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    // the address field: a capsule
    float capR = (float)((g_webOmni.bottom - g_webOmni.top) / 2) / (float)g.dpi;
    WebTileC(dc, g_webOmni, Gdiplus::Color(255, GetRValue(wvb::Omnibox), GetGValue(wvb::Omnibox),
                                           GetBValue(wvb::Omnibox)), capR);
    bool editFocus = g_wedit && GetFocus() == g_wedit;
    if (editFocus)                                     // Safari's focus story: a blue ring
        WebRing(dc, g_webOmni, capR, 2.0f, Gdiplus::Color(235, 0x0a, 0x84, 0xff));
    if (g_webFail) {                                   // no engine: the field explains itself
        SelectObject(dc, g_webEditFont ? g_webEditFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, wvb::InkDim);
        RECT tr = g_webOmni;
        DrawTextW(dc, L"web engine missing", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    WebTab* t = WebCur();
    // the padlock: hugging the centered site name while idle, pinned to the
    // field's left edge while it is being edited; only secure pages wear one
    bool secure = t && t->url.rfind(L"https://", 0) == 0;
    if (secure) {
        int lockX = (int)g_webOmni.left + TABF(14);
        if (g_wedit) {                                 // the lock follows the centered text, idle or editing
            wchar_t tx[512] = L"";
            int len = GetWindowTextW(g_wedit, tx, 512);
            if (len > 0) {
                HGDIOBJ of0 = SelectObject(dc, g_webEditFont);
                SIZE sz{}; GetTextExtentPoint32W(dc, tx, len, &sz);
                SelectObject(dc, of0);
                int cx = (int)(g_webOmni.left + g_webOmni.right) / 2;
                lockX = max((int)g_webOmni.left + TABF(14), cx - (int)sz.cx / 2 - TABF(18));
            }
        }
        SelectObject(dc, WebGlyphFont(11));
        SetTextColor(dc, wvb::InkDim);
        RECT lk{ lockX, g_webOmni.top, lockX + TABF(14), g_webOmni.bottom };
        DrawTextW(dc, L"\uE72E", -1, &lk, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // reload at rest, stop while loading - a round tile under the cursor
    RECT rf = g_wbR[WB_RELOAD];
    if (g.tbTabHot == WB_RELOAD)
        WebTileC(dc, rf, Gdiplus::Color(255, GetRValue(wvb::Hover), GetGValue(wvb::Hover),
                                        GetBValue(wvb::Hover)), 12.0f);
    SelectObject(dc, WebGlyphFont(13));
    SetTextColor(dc, wvb::Ink);
    DrawTextW(dc, (t && t->loading) ? L"\uE711" : L"\uE72C", -1, &rf,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (t && t->loading) {                             // the progress line: a blue segment sweeping
        int inset = TABF(16);                          // along the capsule's foot
        int pw = (int)(g_webOmni.right - g_webOmni.left) - 2 * inset;
        if (pw > 8) {
            int seg2 = max(8, pw / 4);
            int ph = (int)(GetTickCount64() % 900);
            int sx = (int)g_webOmni.left + inset + (int)((long long)(pw + seg2) * ph / 900) - seg2;
            int x0 = max(sx, (int)g_webOmni.left + inset);
            int x1 = min(sx + seg2, (int)g_webOmni.left + inset + pw);
            if (x1 > x0) {
                Gdiplus::Graphics gp(dc);
                Gdiplus::SolidBrush pb(Gdiplus::Color(255, 0x0a, 0x84, 0xff));
                gp.FillRectangle(&pb, x0, (int)g_webOmni.bottom - max(2, TABF(2)) - TABF(1),
                                 x1 - x0, max(2, TABF(2)));
            }
        }
    }
    // the placeholder, centered like the idle site name
    wchar_t has[8] = L"";
    if (g_wedit && !editFocus && GetWindowTextW(g_wedit, has, 2) == 0) {
        SelectObject(dc, g_webEditFont);
        SetTextColor(dc, wvb::InkDim);
        RECT pr{ g_webOmni.left + TABF(30), g_webOmni.top, g_webOmni.right - TABF(30), g_webOmni.bottom };
        DrawTextW(dc, L"Search or enter website name", -1, &pr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void WebStripSpin() {
    bool any = false;
    for (auto& t : g_wt) if (t.loading) any = true;
    if (!g_wstrip) return;
    if (any) SetTimer(g_wstrip, 3, 16, nullptr);   // 60fps: the sweep is time-based, so the
                                                   // timer only sets the frame rate - 90ms
                                                   // read as a thing jumping under the bar
    else KillTimer(g_wstrip, 3);
}

// the toolbar's window: Safari's unified bar. The buttons press with the
// toolbar's own capture-held story (g.tbTabHot / g.tbPressed - the tab row
// under this window keeps its hover in ws.sHot instead, so the two never
// collide), the pill answers a click by focusing the omnibox edit, and the
// edit's WM_CTLCOLOREDIT lands here now that the edit is this window's child.
static LRESULT CALLBACK WebStripProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureBarBack(dc, r.right, r.bottom);
            WebStripPaint(g_bardc, r.right, r.bottom);   // shares the toolbar's one buffer:
            BitBlt(dc, 0, 0, r.right, r.bottom, g_bardc, 0, 0, SRCCOPY);   // an atomic frame
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 3) {                             // the loading line sweeps the pill's foot:
            if (!IsRectEmpty(&g_webOmni))          // only the pill repaints - the toolbar's
                InvalidateRect(h, &g_webOmni, FALSE);   // buttons hold still at 60fps, and
            if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // the tabs' spinners ride the band
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hot = WebNavHit(p), xh = WxHit(p);
        if (hot != g.tbTabHot || xh != g_wxHot) {
            g.tbTabHot = hot; g_wxHot = xh;
            if (hot >= 0 || xh >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.tbTabHot != -1 || g_wxHot != -1) { g.tbTabHot = -1; g_wxHot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hit = WebNavHit(p), xh = WxHit(p);
        if (hit >= 0) {                            // capture held: the toolbar's press story
            SetCapture(h);
            g.tbPressed = hit;
            InvalidateRect(h, nullptr, FALSE);
        } else if (xh >= 0) {                      // an extension's icon: same press story
            SetCapture(h);
            g_wxPress = xh;
            InvalidateRect(h, nullptr, FALSE);
        } else if (PtInRect(&g_webOmni, p) && g_wedit && !g_webFail) {
            SetFocus(g_wedit);                     // a click on the pill focuses the URL
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_wxPress >= 0) {
            int pr = g_wxPress; g_wxPress = -1;
            if (GetCapture() == h) ReleaseCapture();
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            InvalidateRect(h, nullptr, FALSE);
            if (WxHit(p) == pr) WxOpenPopupFor(pr);
            return 0;
        }
        int was = g.tbPressed;
        if (was >= 0) {
            g.tbPressed = -1;
            if (GetCapture() == h) ReleaseCapture();
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int hit = WebNavHit(p);
            if (hit >= 0 && hit == was && WebBtnEnabled(hit)) {
                if (hit == WB_RELOAD) {            // the pill's refresh doubles as stop
                    WebTab* t = WebCur();
                    if (t && t->web) { if (t->loading) t->web->Stop(); else t->web->Reload(); }
                } else WebBarClick(hit);
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g.tbPressed >= 0 || g_wxPress >= 0) { g.tbPressed = -1; g_wxPress = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_CONTEXTMENU: {                         // right-click opens the extensions' menu: the
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   // manager's door, and the diagnostics with it
        if (p.x != -1 || p.y != -1) {              // (keyboard-open passes -1,-1; no anchor to offer)
            ScreenToClient(h, &p);
            WxMenu(p);
        }
        return 0;
    }
    case WM_CTLCOLOREDIT:                          // the omnibox edit is this window's child
        return (LRESULT)WebEditColors((HDC)wp, (HWND)lp);
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            int hit = WebNavHit(p);
            bool pill = PtInRect(&g_webOmni, p);
            bool xi = WxHit(p) >= 0;
            SetCursor(LoadCursorW(nullptr, (hit >= 0 || xi) ? IDC_HAND : (pill ? IDC_IBEAM : IDC_ARROW)));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// the nav row's geometry: the buttons and the pill live in the strip window's
// coords now (Safari's order - toolbar on top, tabs under). The reload button IS
// the pill's right zone, so its hit rect lands inside the pill; the + rides
// the tab row below and its nav rect stays empty.
static void WebBarLayout() {
    if (!g_wstrip || !g_webOn) return;
    RECT r; GetClientRect(g_wstrip, &r);
    int d = TABF(30), cy = (r.bottom - 1 - d) / 2, pitch = TABF(32);
    int ph = TABF(30), py = (r.bottom - 1 - ph) / 2;
    int x = TABF(10);
    g_wbR[WB_SIDE] = { x, cy, x + d, cy + d };
    x += pitch + TABF(10); g_wbR[WB_BACK] = { x, cy, x + d, cy + d };
    x += pitch; g_wbR[WB_FWD] = { x, cy, x + d, cy + d };
    int mx = (int)r.right - TABF(10);
    g_wbR[WB_MENU] = { 0, 0, 0, 0 };               // the 3-dot is gone: the toolbar keeps only
    g_wbR[WB_NEWTAB] = { mx - d, cy, mx, cy + d }; // the + and, left of it, each extension's icon
    mx -= pitch;
    g_wbR[WB_READER] = { 0, 0, 0, 0 };             // the PDF button lives on the top toolbar now
    int leftEdge = (int)g_wbR[WB_NEWTAB].left;
    g_wbR[WB_EXT] = { 0, 0, 0, 0 };                // the puzzle button is gone too - extensions
    g_wxR.clear(); g_wxIdx.clear();                // work from their own icons, and the pill
    std::vector<int> vis;                          // gains the room they used to bracket. The
    for (int i = 0; i < (int)g_wx.size() && vis.size() < 5; i++)   // icons ride g_wx alone now:
        if (g_wx[i].enabled && !g_wx[i].popup.empty()) vis.push_back(i);   // g_wxOk was the listing
    int n = (int)vis.size();                       // call's verdict, and a listing that fails
    int right0 = (int)g_wbR[WB_NEWTAB].left - (pitch - d);   // while the folders still run must
    for (int k = 0; k < n; k++) {                  // never take the faces with it
        int right = right0 - (n - 1 - k) * pitch;
        g_wxR.push_back(RECT{ right - d, cy, right, cy + d });
        g_wxIdx.push_back(vis[k]);
    }
    if (n) leftEdge = (int)g_wxR[0].left;
    int lo = (int)g_wbR[WB_FWD].right + TABF(18), hi = leftEdge - TABF(18);
    int pw = max(10, min(hi - lo, TABF(560)));     // the capsule: centered in the window, capped
    int pl = std::clamp((int)r.right / 2 - pw / 2, lo, max(lo, hi - pw));
    g_webOmni = { pl, py, pl + pw, py + ph };
    g_wbR[WB_RELOAD] = { g_webOmni.right - TABF(30), g_webOmni.top + TABF(3),
                         g_webOmni.right - TABF(6), g_webOmni.bottom - TABF(3) };
    if (!g_webEditFont) g_webEditFont = CreateFontW(-(int)std::lround(13 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!g_wedit) {
        g_wedit = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | ES_CENTER,
                                  0, 0, 0, 0, g_wstrip, (HMENU)(INT_PTR)1001, g.hinst, nullptr);
        if (g_wedit) {
            SendMessageW(g_wedit, WM_SETFONT, (WPARAM)g_webEditFont, TRUE);
            g_weditPrev = (WNDPROC)SetWindowLongPtrW(g_wedit, GWLP_WNDPROC, (LONG_PTR)WebEditProc);
        }
    }
    if (g_wedit) {
        int eh = max(4, TABF(18));                 // one text line, centered in the capsule
        MoveWindow(g_wedit, g_webOmni.left + TABF(34), g_webOmni.top + (ph - eh) / 2,
                   max(10, (int)g_webOmni.right - (int)g_webOmni.left - TABF(68)), eh, TRUE);
        ShowWindow(g_wedit, g_webFail ? SW_HIDE : SW_SHOW);
    }
    WebSyncOmnibox();
}

static void WebSyncOmnibox() {
    if (!g_wedit || GetFocus() == g_wedit) return;
    WebTab* t = WebCur();
    SetWindowTextW(g_wedit, t ? WebHostOf(t->url).c_str() : L"");   // idle: the site, like Safari
    if (g_wstrip) InvalidateRect(g_wstrip, nullptr, FALSE);         // the padlock hugs the text
}

static LRESULT CALLBACK WebEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            wchar_t buf[2048]; GetWindowTextW(h, buf, 2048);
            WebGo(buf);
            SetFocus(g.main);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            WebSyncOmniboxForce();
            SetFocus(g.main);
            return 0;
        }
    }
    if (m == WM_SETFOCUS) {                        // focus opens the field to the full address,
        WebTab* ft = WebCur();                     // selects all, and left-aligns it
        SetWindowTextW(h, ft ? ft->url.c_str() : L"");
        LRESULT r = CallWindowProcW(g_weditPrev, h, m, wp, lp);   // (stays centered, like Safari)
        PostMessageW(h, EM_SETSEL, 0, -1);
        InvalidateRect(h, nullptr, TRUE);
        if (g_wstrip) InvalidateRect(g_wstrip, nullptr, FALSE);   // the ring + padlock follow
        return r;
    }
    if (m == WM_KILLFOCUS) {                       // the blur puts the pill back the way
        LRESULT r = CallWindowProcW(g_weditPrev, h, m, wp, lp);   // the page left it
        WebSyncOmniboxForce();
        InvalidateRect(h, nullptr, TRUE);
        return r;
    }
    return CallWindowProcW(g_weditPrev, h, m, wp, lp);
}

static void WebSyncOmniboxForce() {
    if (!g_wedit) return;
    WebTab* t = WebCur();
    SetWindowTextW(g_wedit, t ? WebHostOf(t->url).c_str() : L"");
    if (g_wstrip) InvalidateRect(g_wstrip, nullptr, FALSE);
}

static HBRUSH WebEditColors(HDC dc, HWND ctl) {
    if (!g_webEditBrush) g_webEditBrush = CreateSolidBrush(wvb::Omnibox);
    SetBkColor(dc, wvb::Omnibox);
    SetTextColor(dc, wvb::Ink);
    return g_webEditBrush;
}

static void WebTabX(HDC dc, int xr, int cy, bool hot) {   // the reader's close x
    HPEN xp = CreatePen(PS_SOLID, max(1, (int)std::lround(1.2 * g.dpi)),
                        hot ? RGB(0xE0, 0x44, 0x44) : RGB(0xa3, 0xa3, 0xa3));
    HGDIOBJ op = SelectObject(dc, xp);
    int g2 = TABF(4);                              // the 8px cross inside the 24px zone
    MoveToEx(dc, xr - g2, cy - g2, nullptr); LineTo(dc, xr + g2, cy + g2);
    MoveToEx(dc, xr + g2, cy - g2, nullptr); LineTo(dc, xr - g2, cy + g2);
    SelectObject(dc, op);
    DeleteObject(xp);
}

// ---- the tab band: flat bar, the reader's tab faces, Safari's proportions ----
static void WebBarDraw(HDC dc, int w, int h) {
    RECT bar{ 0, 0, w, h };
    HBRUSH bb = CreateSolidBrush(wvb::Band);
    FillRect(dc, &bar, bb); DeleteObject(bb);
    {   // the band's foot fade: the reader band's own TabStripFade, over the card
        // tucked under this row - the last of the chrome's shadow before the page
        Gdiplus::Graphics gfade(dc);
        Gdiplus::Color c0((BYTE)0, (BYTE)0, (BYTE)0, (BYTE)0);
        Gdiplus::Color c1((BYTE)0x22, (BYTE)0, (BYTE)0, (BYTE)0);
        Gdiplus::RectF fadeR(0.f, h * 0.45f, (float)w, h * 0.55f);
        Gdiplus::LinearGradientBrush fadeBr(fadeR, c0, c1, Gdiplus::LinearGradientModeVertical);
        gfade.FillRectangle(&fadeBr, fadeR);
    }
    RECT edge{ 0, h - 1, w, h };                       // the hairline above the page
    HBRUSH eb = CreateSolidBrush(wvb::Edge);
    FillRect(dc, &edge, eb); DeleteObject(eb);
    SetBkMode(dc, TRANSPARENT);
    WebTabGeo G = WebTabLayout(w);
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    auto drawTab = [&](int i) {
        int k = i - G.start;
        const WebTab& t = g_wt[i];
        bool act = i == g_wtCur;
        int fl = G.x0 + k * G.cell, fr = fl + G.cell;
        int ft = G.faceTop, fb = ft + G.faceH;
        if (act) {                                     // the open tab is raised to the toolbar's sheet
            RECT face{ fl, 0, fr, h - 1 };
            HBRUSH fbr = CreateSolidBrush(wvb::StripBg);
            FillRect(dc, &face, fbr); DeleteObject(fbr);
        } else {
            if (i == ws.sHot && !g_webFail) {      // the hovered face: no floating tile - the
                                                   // tab itself turns one step lighter, the
                                                   // band's own color warmed, and that is all
                COLORREF bc = wvb::Band;
                HBRUSH hb = CreateSolidBrush(RGB(min(255, GetRValue(bc) + 14),
                                                 min(255, GetGValue(bc) + 14),
                                                 min(255, GetBValue(bc) + 14)));
                RECT face{ fl, 0, fr, h - 1 };
                FillRect(dc, &face, hb); DeleteObject(hb);
            }
            if (k != G.vis - 1 && i + 1 != g_wtCur) {  // the divider, none beside the open tab
                Gdiplus::SolidBrush db(Gdiplus::Color(255, 0x55, 0x55, 0x55));
                gs.FillRectangle(&db, Gdiplus::RectF((float)fr - 1.f, (float)ft, 1.f, (float)G.faceH));
            }
        }
        if (t.loading) {                               // the green spinner at the face's left
            float rr = 5.0f * (float)g.dpi;
            float cx = (float)(fl + TABF(12)) + rr, cy = (float)ft + G.faceH / 2.0f;
            int a0 = (int)((GetTickCount64() / 6) % 360);
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x22, 0xbb, 0x56), 1.6f * (float)g.dpi);
            gs.DrawArc(&pen, cx - rr, cy - rr, 2 * rr, 2 * rr, (Gdiplus::REAL)a0, 270.0f);
        }
        std::wstring ttl = t.title;
        if (ttl.empty()) ttl = WebHostOf(t.url);
        if (ttl.empty()) ttl = L"New Tab";
        RECT tr{ fl + TABF(30), ft, fr - TABF(30), fb };   // centered between the spinner and the x
        TabLabel(dc, ttl, tr, act ? RGB(0xe0, 0xe0, 0xe0) : RGB(0xa3, 0xa3, 0xa3), true);
        WebTabX(dc, fr - TABF(2) - TABF(12), ft + G.faceH / 2, i == ws.sHot && ws.sHotX);
        if (act) {                                     // the green rule over the open tab
            Gdiplus::SolidBrush acc(Gdiplus::Color(255, 0x22, 0xbb, 0x56));
            gs.FillRectangle(&acc, Gdiplus::RectF((float)fl, 0.f, (float)(fr - fl),
                                                  (float)max(2, TABF(2))));
        }
    };
    for (int k = 0; k < G.vis; k++) if (G.start + k != g_wtCur) drawTab(G.start + k);   // inactive first,
    if (g_wtCur >= G.start && g_wtCur < G.start + G.vis) drawTab(g_wtCur);             // the open tab last
}

static void WebBarClick(int id) {
    WebTab* t = WebCur();
    switch (id) {
    case WB_READER: WebToggle(false); return;
    case WB_EXT: { POINT z{ g_wbR[WB_EXT].left, g_wbR[WB_EXT].bottom }; WxMenu(z); } return;
    case WB_SIDE:
        g_webSide = !g_webSide;
        if (g.side) ShowWindow(g.side, g_webSide ? SW_SHOWNOACTIVATE : SW_HIDE);
        LayoutMain(); WebRepaintChrome();
        return;
    case WB_BACK: if (t && t->web && t->canBack) t->web->GoBack(); return;
    case WB_FWD: if (t && t->web && t->canFwd) t->web->GoForward(); return;
    case WB_RELOAD:
        if (t && t->web) { if (t->loading) t->web->Stop(); else t->web->Reload(); }
        return;
    case WB_NEWTAB: WebNewTab(L"https://www.google.com/", true); return;
    case WB_MENU: {
        if (!g_wstrip) return;
        POINT pt{ g_wbR[WB_MENU].left, g_wbR[WB_MENU].bottom };
        ClientToScreen(g_wstrip, &pt);
        HMENU m = CreatePopupMenu();
        if (!m) return;
        AppendMenuW(m, MF_STRING, 1, L"New tab\tCtrl+T");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 2, L"Zoom in");
        AppendMenuW(m, MF_STRING, 3, L"Zoom out");
        AppendMenuW(m, MF_STRING, 4, L"Actual size");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 5, L"Reload page\tF5");
        AppendMenuW(m, MF_STRING, 7, g_webSide ? L"Hide sidebar" : L"Show sidebar");
        AppendMenuW(m, MF_STRING, 6, L"Back to the reader");
        SetForegroundWindow(g.main);
        int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.main, nullptr);
        DestroyMenu(m);
        switch (c) {
        case 1: WebNewTab(L"https://www.google.com/", true); break;
        case 2: WebZoomBy(1.25); break;
        case 3: WebZoomBy(0.8); break;
        case 4: WebZoomBy(0); break;               // 0 = reset to 100%
        case 5: if (t && t->web) t->web->Reload(); break;
        case 6: WebToggle(false); break;
        case 7: WebBarClick(WB_SIDE); break;
        }
        return;
    }
    }
}

// ---- navigation, tabs, the engine's lifecycle ----

static void WebGo(const std::wstring& what) {
    WebTab* t = WebCur();
    if (!t || !t->web) return;
    std::wstring s = what;
    while (!s.empty() && (s[0] == L' ' || s[0] == L'\t')) s.erase(0, 1);
    while (!s.empty() && (s[s.size() - 1] == L' ' || s[s.size() - 1] == L'\t')) s.pop_back();
    if (s.empty()) return;
    std::wstring url;
    if (s.find(L"://") != std::wstring::npos || s.rfind(L"about:", 0) == 0 || s.rfind(L"edge:", 0) == 0) {
        url = s;
    } else if (s.find(L'.') == std::wstring::npos || s.find(L' ') != std::wstring::npos) {
        // a search, percent-encoded the way an address bar owes the engine:
        // Google is the default search engine
        url = L"https://www.google.com/search?q=";
        char hex[] = "0123456789ABCDEF";
        for (wchar_t c : s) {
            if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
                || c == L'-' || c == L'_' || c == L'.' || c == L'~') url += c;
            else if (c == L' ') url += L'+';
            else {
                unsigned int u = c;
                if (u < 0x80) {
                    url += L'%'; url += hex[u >> 4]; url += hex[u & 15];
                } else {
                    // UTF-8 encode, then escape each byte
                    wchar_t enc[4] = {};
                    int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, nullptr, 0, nullptr, nullptr);
                    if (n > 0 && n < 8) {
                        char b[8];
                        WideCharToMultiByte(CP_UTF8, 0, &c, 1, b, n, nullptr, nullptr);
                        for (int k = 0; k < n; k++) {
                            unsigned char uc = (unsigned char)b[k];
                            url += L'%'; url += hex[uc >> 4]; url += hex[uc & 15];
                        }
                    }
                }
            }
        }
    } else {
        url = L"https://" + s;
    }
    t->web->Navigate(url.c_str());
}

static void WebEnsurePump() {
    if (!g.main) return;
    bool need = false;
    for (auto& t : g_wt)                   // a navigation in flight rides the pump too: the
        if (t.captureDue || t.capturePending || t.loading) need = true;   // watchdog lives there,
    if (need) SetTimer(g.main, 7, 200, nullptr);   // and a stuck load is the blackscreen's
    else KillTimer(g.main, 7);                     // last standing shape
}

static const wchar_t* WebProcName(COREWEBVIEW2_PROCESS_FAILED_KIND k) {   // the failure's kind, in words
    switch (k) {
    case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED: return L"browser process exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED: return L"render process exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE: return L"render process unresponsive";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_FRAME_RENDER_PROCESS_EXITED: return L"frame render process exited";
    case COREWEBVIEW2_PROCESS_FAILED_KIND_UTILITY_PROCESS_EXITED: return L"utility process exited";
    default: return L"process failure";
    }
}

static const wchar_t* WebProcReason(COREWEBVIEW2_PROCESS_FAILED_REASON r) {   // why it died, in words
    switch (r) {
    case COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED: return L"unexpected";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_UNRESPONSIVE: return L"unresponsive";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_TERMINATED: return L"terminated";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_CRASHED: return L"crashed";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_LAUNCH_FAILED: return L"launch failed";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_OUT_OF_MEMORY: return L"out of memory";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_PROFILE_DELETED: return L"profile deleted";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_NORMAL_EXIT: return L"normal exit";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_ABNORMAL_EXIT: return L"abnormal exit";
    default: return L"other";
    }
}

static const wchar_t* WebErrName(COREWEBVIEW2_WEB_ERROR_STATUS es) {      // the load's verdict, in words
    switch (es) {
    case COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT: return L"cannot connect";
    case COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED: return L"name not resolved";
    case COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT: return L"timeout";
    case COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE: return L"server unreachable";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED: return L"connection aborted";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET: return L"connection reset";
    case COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED: return L"offline";
    case COREWEBVIEW2_WEB_ERROR_STATUS_ERROR_HTTP_INVALID_SERVER_RESPONSE: return L"bad server response";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID: return L"invalid certificate";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED: return L"certificate expired";
    case COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED: return L"canceled";
    case COREWEBVIEW2_WEB_ERROR_STATUS_REDIRECT_FAILED: return L"redirect failed";
    default: break;
    }
    return es == COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN ? L"unknown" : L"error";
}

// the stuck-navigation self-heal, run from the pump: a navigation that left and
// never lands (the DirectComposition stall of WebView2Feedback 5574, a hung
// renderer, a dead network service) gets one presentation kick - a 1px bounds
// jog forces the swap-chain to commit again, the exact move that showed content
// in that thread's repros - and one reload. Each stage lands in webext.log, so
// the next "blackscreen" report arrives with times and URLs, not adjectives.
static void WebNavWatchdog() {
    unsigned long long now = GetTickCount64();
    for (auto& t : g_wt) {
        if (!t.loading || !t.navStart) continue;
        unsigned long long age = now - t.navStart;
        if (age > 25000 && !t.navReloaded) {
            t.navReloaded = true;
            WxLogf(L"nav: watchdog reloads after %us - %s", (unsigned)(age / 1000), t.url.c_str());
            if (t.web) t.web->Reload();
        } else if (age > 12000 && !t.navKicked) {
            t.navKicked = true;
            WxLogf(L"nav: watchdog kicks the surface after %us - %s", (unsigned)(age / 1000), t.url.c_str());
            if (t.ctr) {                   // one pixel further in than WebPlace's own inset: the size
                RECT c; WebCardRect(c, g.cw, g.ch);   // change itself is what forces the fresh commit
                c.left++; c.top++; c.right -= 2; c.bottom -= 2;
                if (c.right > c.left && c.bottom > c.top) t.ctr->put_Bounds(c);
            }
        } else if (t.navKicked && age > 12400) {
            WebPlace();                    // the kick's bounds go back; idempotent afterwards
        }
    }
}

static void WebCaptureTab(int id);
static void WebThumbDecode(int id, IStream* st);

static void WebCaptureTick() {                     // the main window's pump (timer 7)
    WebNavWatchdog();                              // the stuck-navigation self-heal rides along
    unsigned long long now = GetTickCount64();
    for (auto& t : g_wt) {
        if (t.captureDue && now >= t.captureDue && t.web) {
            t.captureDue = 0;
            WebCaptureTab(t.id);
        }
    }
    WebEnsurePump();
}

static void WebCaptureTab(int id) {
    WebTab* t = WebTabById(id);
    if (!t || !t->web) return;
    if (t->capturing) { t->capturePending = true; return; }
    t->capturing = true;
    wrl::ComPtr<IStream> st;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &st))) { t->capturing = false; return; }
    wrl::ComPtr<ICoreWebView2CapturePreviewCompletedHandler> cb =
        Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [id, st](HRESULT hr) -> HRESULT {
                WebTab* t = WebTabById(id);
                if (t) {
                    t->capturing = false;
                    if (SUCCEEDED(hr)) WebThumbDecode(id, st.Get());
                    if (t && t->capturePending) { t->capturePending = false; WebCaptureTab(id); }
                }
                return S_OK;
            });
    t->web->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, st.Get(), cb.Get());
}

static void WebThumbDecode(int id, IStream* st) {
    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(st, &hg)) || !hg) return;
    SIZE_T sz = GlobalSize(hg);
    void* p = GlobalLock(hg);
    if (!p || !sz) { if (p) GlobalUnlock(hg); return; }
    wrl::ComPtr<IStream> st2;
    bool ok = SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st2));
    ULONG wr = 0;
    if (ok) ok = SUCCEEDED(st2->Write(p, (ULONG)sz, &wr)) && wr == sz;
    GlobalUnlock(hg);
    if (!ok) return;
    LARGE_INTEGER z{}; st2->Seek(z, STREAM_SEEK_SET, nullptr);
    Gdiplus::Bitmap* bm = Gdiplus::Bitmap::FromStream(st2.Get());
    if (!bm) return;
    int bw = (int)bm->GetWidth(), bh = (int)bm->GetHeight();
    if (!bw || !bh) { delete bm; return; }
    HBITMAP full = nullptr;
    bm->GetHBITMAP(Gdiplus::Color(255, 0x1c, 0x1c, 0x1c), &full);   // the card's face behind
    delete bm;
    if (!full) return;
    int TW2 = (int)(240 * g.dpi), TH2 = max(1, (int)((long long)TW2 * bh / bw));   // 240dp: the rail
                              // stretches thumbs past 96dp now, so the capture keeps the headroom
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), TW2, -TH2, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP scaled = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (scaled) {
        HDC m = CreateCompatibleDC(nullptr), s = CreateCompatibleDC(nullptr);
        HGDIOBJ o1 = SelectObject(m, scaled), o2 = SelectObject(s, full);
        SetStretchBltMode(m, HALFTONE); SetBrushOrgEx(m, 0, 0, nullptr);
        StretchBlt(m, 0, 0, TW2, TH2, s, 0, 0, bw, bh, SRCCOPY);
        SelectObject(m, o1); SelectObject(s, o2); DeleteDC(m); DeleteDC(s);
    }
    DeleteObject(full);
    if (!scaled) return;
    WebTab* t = WebTabById(id);
    if (!t) { DeleteObject(scaled); return; }
    if (t->thumb) DeleteObject(t->thumb);
    t->thumb = scaled; t->thumbW = TW2; t->thumbH = TH2;
    t->ar = (double)bh / max(1, bw);
    if (g_webOn) { WebSideLayout(); InvalidateRect(g.side, nullptr, FALSE); }
}

static void WebShowCurrent() {
    for (int i = 0; i < (int)g_wt.size(); i++)
        if (g_wt[i].ctr)   // the current tab's surface shows at once: its DefaultBackgroundColor
            g_wt[i].ctr->put_IsVisible((i == g_wtCur) ? TRUE : FALSE);   // is the pages' canvas, so the engine's
    WebPlace();        // spin-up reads as the card, never as black glass or a page that never lands
}

static void WebHistorySync(int id) {
    WebTab* t = WebTabById(id);
    if (!t || !t->web) return;
    BOOL b = FALSE;
    t->web->get_CanGoBack(&b); t->canBack = b != 0;
    t->web->get_CanGoForward(&b); t->canFwd = b != 0;
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
}

static void WebEnsureController(int i) {
    if (!g_wvEnv || i < 0 || i >= (int)g_wt.size()) return;
    WebTab* t = &g_wt[i];
    if (t->ctr || t->building) return;
    t->building = true;
    unsigned long long id = t->id;
    HRESULT hr = g_wvEnv->CreateCoreWebView2Controller(g.view,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [id](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                WebTab* t = WebTabById(id);
                if (t) t->building = false;
                if (FAILED(hr) || !c) { g_webFail = true; WebRepaintChrome(); return S_OK; }
                if (!t) { c->Close(); return S_OK; }
                t->ctr = c;
                c->get_CoreWebView2(&t->web);
                wrl::ComPtr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2)
                    c2->put_DefaultBackgroundColor({ 255, 0x3a, 0x3a, 0x3a });   // the pages' canvas
                                                                                    // (#3a3a3a, PaneBrush), so the
                                                                                    // engine's spin-up reads as the
                                                                                    // card's own surface, never as
                                                                                    // Chromium's white or black glass
                t->web->AddScriptToExecuteOnDocumentCreated(kWxStoreScript, nullptr);   // the store's "Add to Docsy"
                {   EventRegistrationToken tkm;
                    t->web->add_WebMessageReceived(
                        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                LPWSTR src = nullptr; std::wstring from;
                                if (SUCCEEDED(a->get_Source(&src)) && src) { from = src; CoTaskMemFree(src); }
                                if (!WxStoreHost(from, nullptr)) return S_OK;      // only the store may ask
                                LPWSTR msg = nullptr;
                                if (SUCCEEDED(a->TryGetWebMessageAsString(&msg)) && msg) {
                                    std::wstring m = msg; CoTaskMemFree(msg);
                                    if (m.rfind(L"docsy-install:", 0) == 0) WxInstall(m.substr(14));
                                }
                                return S_OK;
                            }).Get(), &tkm); }
                // a .crx arriving in the webview is an install click that got as
                // far as the update service: docsy takes the download's id, cancels
                // it, and runs its own installer - one more road that cannot be
                // blocked by page UI or script
                {   wrl::ComPtr<ICoreWebView2_4> w4;
                    if (SUCCEEDED(t->web->QueryInterface(IID_PPV_ARGS(&w4))) && w4) {
                        w4->add_DownloadStarting(
                            Microsoft::WRL::Callback<ICoreWebView2DownloadStartingEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* a) -> HRESULT {
                                    wrl::ComPtr<ICoreWebView2DownloadOperation> op;
                                    if (FAILED(a->get_DownloadOperation(&op)) || !op) return S_OK;
                                    LPWSTR u = nullptr;
                                    if (SUCCEEDED(op->get_Uri(&u)) && u) {
                                        std::wstring low = u; CoTaskMemFree(u);
                                        for (auto& c : low) c = (wchar_t)towlower(c);
                                        std::wstring sid;              // x=id%3D<32 a-p> in the update URL
                                        size_t p = low.find(L"id%3d");
                                        if (p != std::wstring::npos) {
                                            p += 5;
                                            while (p < low.size() && sid.size() < 32)
                                                if (low[p] >= L'a' && low[p] <= L'p') sid += low[p++];
                                                else break;
                                        }
                                        if (!sid.empty()) {
                                            a->put_Handled(TRUE); a->put_Cancel(TRUE);
                                            WxInstall(sid);
                                        }
                                    }
                                    return S_OK;
                                }).Get(), &t->tkDl);
                    }
                }
                t->web->add_NavigationStarting(
                    Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [id](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                            WebTab* t = WebTabById(id);
                            if (!t) return S_OK;
                            LPWSTR nu = nullptr;
                            if (SUCCEEDED(a->get_Uri(&nu)) && nu) { WxLogf(L"nav: start %s", nu); CoTaskMemFree(nu); }
                            t->loading = true;
                            t->navStart = GetTickCount64();   // the watchdog's clock starts here
                            t->navKicked = false; t->navReloaded = false;
                            WebEnsurePump();                  // the pump lives while a load is out
                            WebRepaintChrome(); WebStripSpin();
                            return S_OK;
                        }).Get(), &t->tkNavS);
                t->web->add_NavigationCompleted(
                    Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [id](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                            WebTab* t = WebTabById(id);
                            if (!t) return S_OK;
                            t->loading = false;
                            t->navStart = 0;                           // the watchdog stands down
                            BOOL navOk = FALSE;
                            if (a) a->get_IsSuccess(&navOk);
                            if (navOk) WxLogf(L"nav: done %s", t->url.c_str());
                            else {
                                COREWEBVIEW2_WEB_ERROR_STATUS es = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                if (a) a->get_WebErrorStatus(&es);
                                WxLogf(L"nav: failed %s (%s)", t->url.c_str(), WebErrName(es));
                            }
                            WebPlace();                                // a watchdog kick returns to the card
                            WxPushState(t->web.Get());                 // the store button learns what is installed
                            t->captureDue = GetTickCount64() + 1200;   // let the page settle, then
                            WebEnsurePump();                           // the rail gets its thumb
                            WebHistorySync(id);
                            WebRepaintChrome(); WebStripSpin(); WebSyncOmnibox();
                            return S_OK;
                        }).Get(), &t->tkNavC);
                t->web->add_SourceChanged(
                    Microsoft::WRL::Callback<ICoreWebView2SourceChangedEventHandler>(
                        [id](ICoreWebView2* s, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                            WebTab* t = WebTabById(id);
                            if (!t || !s) return S_OK;
                            LPWSTR u = nullptr;
                            if (SUCCEEDED(s->get_Source(&u)) && u) {
                                t->url = u; CoTaskMemFree(u);
                            }
                            WebRepaintChrome(); WebSyncOmnibox();
                            return S_OK;
                        }).Get(), &t->tkSrc);
                t->web->add_DocumentTitleChanged(
                    Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                        [id](ICoreWebView2* s, IUnknown*) -> HRESULT {
                            WebTab* t = WebTabById(id);
                            if (!t || !s) return S_OK;
                            LPWSTR ti = nullptr;
                            if (SUCCEEDED(s->get_DocumentTitle(&ti)) && ti) {
                                t->title = ti; CoTaskMemFree(ti);
                            }
                            WebRepaintChrome();
                            if (g.side) InvalidateRect(g.side, nullptr, FALSE);
                            return S_OK;
                        }).Get(), &t->tkTitle);
                t->web->add_HistoryChanged(    // HistoryChanged rides the base interface
                    Microsoft::WRL::Callback<ICoreWebView2HistoryChangedEventHandler>(
                        [id](ICoreWebView2*, IUnknown*) -> HRESULT {
                            WebHistorySync(id);
                            return S_OK;
                        }).Get(), &t->tkHist);
                {   // the processes' own verdicts: a render death reloads the tab once, a
                    // second one cold-restarts the engine - deferred through WM_APP+7,
                    // never re-entrant in an event frame. The handler rides the base
                    // interface, where ProcessFailed has lived since the first SDK.
                    t->web->add_ProcessFailed(
                        Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
                            [id](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
                                WebTab* t = WebTabById(id);   // resolve the tab fresh: the vector may
                                if (!t) return S_OK;          // have moved since the handler was born
                                COREWEBVIEW2_PROCESS_FAILED_KIND k{};
                                if (a) a->get_ProcessFailedKind(&k);
                                wrl::ComPtr<ICoreWebView2ProcessFailedEventArgs2> a2;
                                COREWEBVIEW2_PROCESS_FAILED_REASON r = COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED;
                                if (a && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&a2))) && a2) a2->get_Reason(&r);
                                WxLogf(L"proc: %s (%s) on tab %llu", WebProcName(k), WebProcReason(r), (unsigned long long)id);
                                if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                                    if (g.main) PostMessageW(g.main, WM_APP + 7, 0, 0);
                                    return S_OK;
                                }
                                bool render = k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED
                                    || k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE
                                    || k == COREWEBVIEW2_PROCESS_FAILED_KIND_FRAME_RENDER_PROCESS_EXITED;
                                if (render && ++t->renderDeaths <= 1) {
                                    if (t->web) t->web->Reload();   // one silent recovery per tab
                                } else if (render && g.main) {
                                    PostMessageW(g.main, WM_APP + 7, 0, 0);
                                }
                                return S_OK;
                            }).Get(), &t->tkProc);
                }
                WebShowCurrent();                  // the controller shows THE MOMENT it exists: a surface
                                                   // held invisible until its first NavigationCompleted
                                                   // never got there on real machines - the hidden widget
                                                   // throttles its renderer and the first navigation stalls
                                                   // behind it. The DefaultBackgroundColor set above IS the
                                                   // pages' canvas, so the visible spin-up reads as the card
                                                   // itself - and the page lands over it; a load that never
                                                   // lands at all meets the watchdog further down
                WebRepaintChrome();
                WxLogf(L"ctrl: tab %llu is up, bounds placed", (unsigned long long)id);
                if (t && !t->pending.empty()) {    // a tab born before its engine navigates the moment
                    t->web->Navigate(t->pending.c_str());   // it exists. The page is the product: extensions
                    t->pending.clear();            // already registered in the profile ride the browser
                }                                  // process from its birth, and WxInit below only asks the
                WxInit();                          // listing - a stall there can never hold the page back.
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        WebTab* t = WebTabById(id);
        if (t) t->building = false;
        g_webFail = true;
        WebRepaintChrome();
    }
}

static void WebEnsureEnv() {
    if (g_wvEnv || g_webFail || g_wvEnvBuilding) return;
    if (!g_webChecked) {
        g_webChecked = true;
        WxCleanWebViewEnvVars();       // inherited WEBVIEW2_* variables beat every option below
        LPWSTR ver = nullptr;
        if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver)) && ver) {
            g_webVer = ver;
            CoTaskMemFree(ver);
            g_webAvail = true;
        }
    }
    if (!g_webAvail) { g_webFail = true; WebRepaintChrome(); if (g.view) InvalidateRect(g.view, nullptr, FALSE); return; }
    WxLogOtherDocsy();                 // a second docsy.exe may have built this profile's engine
    WxLogPolicies();                   // Extension*/Experiment* policies refuse installs machine-wide
    PWSTR la = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);
    std::wstring udf = (la ? std::wstring(la) : std::wstring(L".")) + L"\\Docsy\\WebView2";
    if (la) CoTaskMemFree(la);
    g_wvEnv = nullptr;
    auto envOpts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();   // extensions on
    bool extOn = false;                                     // did the switch actually land?
    wrl::ComPtr<ICoreWebView2EnvironmentOptions6> envOpts6;
    if (envOpts && SUCCEEDED(envOpts.As(&envOpts6)) && envOpts6)
        extOn = SUCCEEDED(envOpts6->put_AreBrowserExtensionsEnabled(TRUE));
    // the engine is in the middle of retiring Manifest V2 (Chrome hard-killed it in
    // 139; Edge's own cutoff lands through 2026/27), and nearly every extension the
    // store offers is still MV2 - Tampermonkey, uBlock Origin, Dark Reader. These
    // two switches hold the old manifest alive; unknown or already-flipped features
    // are ignored harmlessly by the runtime, so this costs nothing when not needed.
    wrl::ComPtr<ICoreWebView2EnvironmentOptions> envOptsBase;      // the arguments ride
    if (envOpts && SUCCEEDED(envOpts.As(&envOptsBase)) && envOptsBase) {   // the base interface
        // GPU compositing stays off, on purpose. WebView2Feedback 5574 is the
        // proof this machine class exists: on it, the DirectComposition
        // presentation path dies while every DOM event keeps firing, and the
        // view holds ONE uniform dark frame forever - the launch blackscreen
        // this browser carried for weeks, screenshots and all. Microsoft's own
        // committed workaround for that thread routes compositing through the
        // CPU; the cost is a slower scroll on huge pages, and the payment is
        // that the page SHOWS. The --disable-features list merges with the
        // runtime's own (a union, per the AdditionalBrowserArguments docs), so
        // the MV2 keep-alive and Edge's internal disabled features both survive.
        envOptsBase->put_AdditionalBrowserArguments(
            L"--disable-features=ExtensionManifestV2Unsupported,ExtensionManifestV2Disabled"
            L" --disable-gpu-compositing");
    }
    WxLogf(L"env: mv2 keep-alive arguments set");
    // extension installs only work in a profile that was born with them on: a
    // user data folder provisioned by an older build - extensions off - keeps
    // that state forever, and every AddBrowserExtension fails inside it, no
    // matter how correct the package is. A marker file tells the two eras
    // apart, so the first extension-capable run rebuilds the profile once;
    // installed extensions live outside the profile and are re-added from
    // their folders on startup, so nothing the user keeps is lost.
    if (extOn && GetFileAttributesW((udf + L"\\.docsy-ext-on-2").c_str()) == INVALID_FILE_ATTRIBUTES) {
        WxLogf(L"env: profile predates the cleaned extension environment - rebuilding it once so installs stop failing");
        WxRmTree(udf);
        WxMkdirs(udf);
        HANDLE mf = CreateFileW((udf + L"\\.docsy-ext-on-2").c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (mf != INVALID_HANDLE_VALUE) CloseHandle(mf);
    }
    WxLogf(L"env: extensions flag %s, runtime %s", extOn ? L"on" : L"OFF", g_webVer.c_str());
    g_wvEnvBuilding = true;
    HRESULT hrE = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), envOpts.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                g_wvEnvBuilding = false;
                if (FAILED(hr) || !env) { g_webFail = true; WxLogf(L"env: creation failed 0x%08lX", (unsigned long)hr); WebRepaintChrome(); return S_OK; }
                g_wvEnv = env;
                for (int i = 0; i < (int)g_wt.size(); i++) WebEnsureController(i);
                WebRepaintChrome();
                return S_OK;
            }).Get());
    if (FAILED(hrE)) {
        g_wvEnvBuilding = false;
        g_webFail = true;
        WxLogf(L"env: creation refused synchronously 0x%08lX", (unsigned long)hrE);
        WebRepaintChrome();
    }
}

static void WebNewTab(std::wstring url, bool makeCurrent) {
    bool band0 = WebBandH() > 0;
    WebTab t; t.id = g_wtIds++; t.url = url; t.pending = url;
    g_wt.push_back(std::move(t));
    if ((WebBandH() > 0) != band0) LayoutMain();       // the tab band appears with the second tab
    int i = (int)g_wt.size() - 1;
    if (makeCurrent || g_wtCur < 0) g_wtCur = i;
    WebEnsureEnv();
    WebEnsureController(i);
    WebShowCurrent();                // the outgoing tab's surface hides at once: the new tab is
                                     // current now, and the old page's frozen pixels must not read
                                     // through the engine's build window like a wrong tab
    WebSideLayout();
    WebRepaintChrome();
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

static void WebCloseTabAt(int i) {
    if (i < 0 || i >= (int)g_wt.size()) return;
    bool band0 = WebBandH() > 0;
    if (g_wt[i].ctr) g_wt[i].ctr->Close();
    g_wt.erase(g_wt.begin() + i);
    if (g_wt.empty()) {                            // Chrome closes the window; docsy's mode
        g_wtCur = -1;                              // stays and opens a fresh tab instead
        WebNewTab(L"https://www.google.com/", true);
        return;
    }
    if (g_wtCur >= (int)g_wt.size()) g_wtCur = (int)g_wt.size() - 1;
    else if (g_wtCur > i) g_wtCur--;
    if ((WebBandH() > 0) != band0) LayoutMain();       // ...and goes again with the last but one
    WebShowCurrent();
    WebBarLayout();
    WebSideLayout(); WebSideSync();
    WebRepaintChrome();
}

static void WebSwitchTo(int i) {
    if (i < 0 || i >= (int)g_wt.size() || i == g_wtCur) return;
    if (g_wtCur >= 0 && g_wtCur < (int)g_wt.size())      // the outgoing tab's thumb refreshes
        g_wt[g_wtCur].captureDue = GetTickCount64();
    g_wtCur = i;
    WebShowCurrent();
    WebBarLayout();
    WebSideSync();
    WebRepaintChrome();
}

// the page: the reader's own container, edge to edge. The webview rides in
// the same rounded card the pages sit in - RadCard 6 corners, the 1px #555
// ring - and the card now fills the view on every side: no gutter anywhere,
// the window IS the container. The webview's bounds stop one pixel shy of
// the card's edge (WebPlace), so the ring still draws the container's edge,
// and a GDI region carves the webview's own window to the same arc
// (WebRoundWebView): the browser window is rounded exactly like the card it
// lives in, the ring's arc showing through every carved corner. A fresh
// tab's spin-up is the pages' canvas tone, never a black slab.
static void WebCardRect(RECT& c, int w, int h) {
    c = { 0, 0, max(10, w), max(10, h) };
}

// the reader's PaneShadow (black, BlurRadius 16, ShadowDepth 5, Direction 270,
// Opacity 0.6) around a card that fills its DIB edge to edge: the silhouette
// touches all four sides, so the erfc falloff only escapes inside the four
// corner notches the webview's rounded region leaves open - the same shadow
// the pages' card casts into its gutter, living where the rounding is.
static void WebCardShadow(BYTE* bits, int w, int h, int x0, int y0, int x1, int y1, int rad) {
    const double sig = 8.0 * g.dpi;
    const double depth = 5.0 * g.dpi;
    float sx0 = (float)x0, sy0 = (float)y0 + (float)depth, sx1 = (float)x1, sy1 = (float)y1 + (float)depth;
    const int LUTN = 236;
    static float lut[LUTN]; static double lutSig = 0;
    if (lutSig != sig) {
        for (int i = 0; i < LUTN; i++) {
            double d = -3.0 * sig + (6.0 * sig) * i / (LUTN - 1);
            lut[i] = (float)(0.6 * Phi(-d / sig));
        }
        lutSig = sig;
    }
    int yA = max(0, y0 - (int)(3 * sig) - 4), yB = min(h, y1 + (int)depth + (int)(3 * sig) + 4);
    for (int y = yA; y < yB; y++) {
        BYTE* row = bits + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            float d = SdRoundRect(x + 0.5f, y + 0.5f, sx0, sy0, sx1, sy1, (float)rad);
            int i = (int)((d / sig + 3.0) * (LUTN - 1) / 6.0);
            int a = i < 0 ? (int)(0.6 * 255 + 0.5f) : (i >= LUTN ? 0 : (int)(lut[i] * 255 + 0.5f));
            if (a <= 0) continue;
            BYTE* p = row + (size_t)x * 4;
            int inv = 255 - a;
            p[0] = (BYTE)((p[0] * inv) >> 8);
            p[1] = (BYTE)((p[1] * inv) >> 8);
            p[2] = (BYTE)((p[2] * inv) >> 8);
        }
    }
}

// the #555 ring for the floating card: CardRingBake's own per-pixel band with
// its analytic corner AA, all four edges free instead of flush to the foot.
static void WebCardRing(BYTE* bits, int w, int h, int x0, int y0, int x1, int y1, int rad) {
    float ox0 = (float)x0 - 0.5f, oy0 = (float)y0 - 0.5f, ox1 = (float)x1 - 0.5f, oy1 = (float)y1 - 0.5f;
    float ix0 = (float)x0 + 0.5f, iy0 = (float)y0 + 0.5f, ix1 = ox1 - 1.0f, iy1 = oy1 - 1.0f;
    int band2 = min(rad + 2, min(y1 - y0, x1 - x0));
    auto ring = [&](int x, int y) {
        float f = -SdRoundRect((float)x, (float)y, ox0, oy0, ox1, oy1, (float)rad);   // inside outer
        float gq = SdRoundRect((float)x, (float)y, ix0, iy0, ix1, iy1, (float)rad - 1);   // outside inner
        float a = min(f, gq) + 0.5f;
        if (a <= 0) return;
        if (a > 1) a = 1;
        BYTE* p = bits + ((size_t)y * w + x) * 4;
        int al = (int)(a * 255 + 0.5f), inv = 255 - al;
        p[0] = (BYTE)((0x55 * al + p[0] * inv) >> 8);
        p[1] = (BYTE)((0x55 * al + p[1] * inv) >> 8);
        p[2] = (BYTE)((0x55 * al + p[2] * inv) >> 8);
    };
    for (int y = max(0, y0 - 1); y < min(h, y0 + band2); y++)          // top strip, then bottom,
        for (int x = max(0, x0 - 1); x < min(w, x1 + 1); x++) ring(x, y);
    for (int y = max(y0 + band2, max(0, y1 - band2)); y < min(h, y1 + 1); y++)
        for (int x = max(0, x0 - 1); x < min(w, x1 + 1); x++) ring(x, y);
    for (int y = max(0, y0 + band2); y < max(y0 + band2, min(h, y1 - band2)); y++) {
        for (int x = max(0, x0 - 1); x < min(w, x0 + band2); x++) ring(x, y);   // left straight,
        for (int x = max(x0 + band2, min(w, x1 - band2)); x < min(w, x1 + 1); x++) ring(x, y);   // right
    }
}

// the view's whole paint in browser mode: the reader's OWN backdrop recipe,
// run verbatim - the shared chrome grain as the field, the PaneShadow at the
// rounded corners, the canvas grain inside the ring and the #555 edge (the
// exact recipes the reader's CardBackdrop bakes, so the browser's card is
// the pages' card to the pixel) - plus the no-engine card when the runtime
// is missing. The webview itself lands on the card's rect the moment its
// controller exists, its own background the card's dark and its own window
// carved to the card's arc (WebRoundWebView), so an open and a fresh tab
// show the card's room until the page composites - never black glass, never
// a placeholder word. One body for the live view's WM_PAINT and the shot
// composite, so the two can never drift.
static HBITMAP g_wcbm = nullptr; static HDC g_wcdc = nullptr;
static int g_wcw = 0, g_wch = 0; static double g_wcdpi = 0;

static void WebCardPaint(HDC dc, int w, int h) {
    RECT vr{ 0, 0, w, h };
    if (!g_wcbm || g_wcw != w || g_wch != h || g_wcdpi != g.dpi) {
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        BYTE* bits = nullptr;
        HBITMAP bm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (bm && bits) {
            HDC bdc = CreateCompatibleDC(dc);
            HGDIOBJ ob = SelectObject(bdc, bm);
            POINT po{ 0, 0 };                                 // the grain phase: the live view's
            if (WindowFromDC(dc) == g.view)                   // window offset - the shot composite
                MapWindowPoints(g.view, g.main, &po, 1);      // phases itself into the globals
            else { po.x = g_grainPX; po.y = g_grainPY; }
            SetBrushOrgEx(bdc, po.x, po.y, nullptr);
            RECT fr{ 0, 0, w, h };
            FillRect(bdc, &fr, GrainBrush(1));                // the one dark chrome field
            RECT cc; WebCardRect(cc, w, h);                   // the card inside this DIB: the
            int m = cc.left, R = cc.right, B = cc.bottom;     // whole view, the reader's own
            int rad = max(2, (int)(6 * g.dpi));               // corners
            if (R - m > 2 * rad + 4 && B - m > 2 * rad + 2) {
                WebCardShadow(bits, w, h, m, m, R, B, rad);   // the reader's PaneShadow, alive in
                                                              // the corner notches: black,
                                                              // BlurRadius 16, ShadowDepth 5,
                                                              // Direction 270, Opacity 0.6 - the
                                                              // webview covers the middle exactly
                                                              // as pages cover it
                Gdiplus::Graphics gs(bdc);
                gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::GraphicsPath* inner = CardPath((float)m + 0.5f, (float)m + 0.5f,
                                                        (float)(R - m) - 1, (float)(B - m) - 1,
                                                        (float)rad - 1, 0);   // the canvas interior,
                if (Gdiplus::TextureBrush* c = GrainTex(0))          // half a pixel in, the reader's
                    gs.FillPath(c, inner);                           // own edge rule
                else {
                    Gdiplus::SolidBrush cb(Gdiplus::Color(255, 58, 58, 58));
                    gs.FillPath(&cb, inner);
                }
                delete inner;
                WebCardRing(bits, w, h, m, m, R, B, rad);         // the #555 ring, exact per pixel
            }
            SelectObject(bdc, ob);
            DeleteDC(bdc);
            if (g_wcbm) DeleteObject(g_wcbm);
            if (g_wcdc) DeleteDC(g_wcdc);
            g_wcbm = bm;
            g_wcdc = CreateCompatibleDC(dc);
            SelectObject(g_wcdc, bm);
            g_wcw = w; g_wch = h; g_wcdpi = g.dpi;
        }
    }
    if (g_wcbm) BitBlt(dc, 0, 0, w, h, g_wcdc, 0, 0, SRCCOPY);
    else {
        HBRUSH pb = CreateSolidBrush(wvb::Page);
        FillRect(dc, &vr, pb); DeleteObject(pb);
    }
    if (g_webFail) { WebFailCard(dc, w, h); return; }
}

// the browser window gets the card's rounding too: the webview is a square
// child HWND, and a square page corner would sit on the ring's arc like a
// sticker on a curved frame. A GDI region on that child - the ring's own
// inner silhouette, the radius the container rounds by - carves the page's
// corners to the card's arc, so the glass is rounded exactly where the card
// is and the #555 ring draws the frame all the way around. Every
// Chrome_WidgetWin child of the view is carved (one per controller, all
// riding the card's bounds), so a background tab arrives rounded when its
// turn on top comes; the region rides the window's own size, and WebPlace
// re-cuts it on every resize and every freshly born controller.
static void WebRoundWebView() {
    if (!g.view) return;
    int ew = 2 * (max(2, (int)(6 * g.dpi)) - 1);       // the ring's inner arc: the same
    if (ew < 4) ew = 4;                                // radius the container rounds by
    HWND hw = nullptr;
    while ((hw = FindWindowExW(g.view, hw, nullptr, nullptr)) != nullptr) {
        WCHAR cls[32]{};
        if (!GetClassNameW(hw, cls, 32) || wcsncmp(cls, L"Chrome_WidgetWin", 16) != 0) continue;
        RECT rc{};
        if (!GetWindowRect(hw, &rc)) continue;
        int vw = rc.right - rc.left, vh = rc.bottom - rc.top;
        if (vw < 16 || vh < 16) continue;
        SetWindowRgn(hw, CreateRoundRectRgn(0, 0, vw, vh, ew, ew), TRUE);   // the system owns
    }                                                   // the region from here on
}

static void WebPlace() {
    if (!g.view) return;
    RECT c; WebCardRect(c, g.cw, g.ch);
    if (c.right <= c.left || c.bottom <= c.top) return;
    c.left++; c.top++; c.right--; c.bottom--;   // one pixel in: the #555 ring stays visible the
    for (auto& t : g_wt)                        // whole way around, and the page sits INSIDE the
        if (t.ctr) t.ctr->put_Bounds(c);        // card exactly where the PDF pages sit in theirs -
    WebRoundWebView();                          // and the glass itself is carved to the card's
}                                               // arc - none is left at the parent's raw default

static void WebResizeDeb() {                       // a resize re-captures the visible thumb
    WebTab* t = WebCur();
    if (!t) return;
    t->captureDue = GetTickCount64() + 900;
    WebEnsurePump();
}

static void WebToggle(bool on) {
    if (on == g_webOn) return;
    if (on && g_sync) return;                      // the CI shots never browse
    g_webOn = on;
    if (g.side)                                    // the rail is the browser's own, closed until asked
        ShowWindow(g.side, on ? (g_webSide ? SW_SHOWNOACTIVATE : SW_HIDE) : (g.sideOn ? SW_SHOWNOACTIVATE : SW_HIDE));
    if (on) {
        WxLoadDisk();
        if (g_wt.empty()) WebNewTab(L"https://www.google.com/", true);
        else { WebEnsureEnv(); WebShowCurrent(); }
        if (g_wtool) { ShowWindow(g_wtool, SW_SHOWNOACTIVATE); InvalidateRect(g_wtool, nullptr, FALSE); }
        if (g_wstrip) { ShowWindow(g_wstrip, SW_SHOWNOACTIVATE); InvalidateRect(g_wstrip, nullptr, FALSE); }
        if (g_webFail && g.view) InvalidateRect(g.view, nullptr, FALSE);
    } else {
        WebTab* t = WebCur();
        if (t) t->captureDue = GetTickCount64() + 250;   // the parting thumb
        WebEnsurePump();
        if (g_wstrip) ShowWindow(g_wstrip, SW_HIDE);
        if (g_wtool) ShowWindow(g_wtool, SW_HIDE);
        g_wtoolHot = g_wtoolDown = false;
        for (auto& w : g_wt) if (w.ctr) w.ctr->put_IsVisible(FALSE);
        if (g_wedit) { ShowWindow(g_wedit, SW_HIDE); SetFocus(g.main); }
        WxPopClose();
    }
    LayoutMain();                                  // the strip band appears or collapses
    WebBarLayout();
    WebRepaintChrome();
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    RailRepaint();
    UpdateTitle();
}

static void WebShutdown() {
    WxPopClose();
    for (auto& e : g_wx) { if (e.icon) { delete e.icon; e.icon = nullptr; } e.ext.Reset(); }
    g_wx.clear(); g_wxProf.Reset();
    for (auto& t : g_wt) {
        if (t.ctr) t.ctr->Close();
        t.ctr.Reset(); t.web.Reset();
        if (t.thumb) { DeleteObject(t.thumb); t.thumb = nullptr; }
    }
    g_wt.clear(); g_wtCur = -1;
    g_wvEnv.Reset();
    if (g_wcbm) { DeleteObject(g_wcbm); g_wcbm = nullptr; }   // the card's baked backdrop
    if (g_wcdc) { DeleteDC(g_wcdc); g_wcdc = nullptr; }
    g_wcw = g_wch = 0;
}

// ---- the sidebar: the reader's page rail, showing the web tabs ----

static int WebSideTH(int i) {
    const WebTab& t = g_wt[i];
    if (t.thumb && t.thumbW > 0) return max(1, (int)std::lround((double)ws.tw * t.thumbH / t.thumbW));
    return max(24, ws.tw * 2 / 3);                 // a viewport-ish placeholder until the
}                                                  // first capture lands

static void WebSideLayout() {
    int n = (int)g_wt.size();
    if (!n || ws.cw <= 0) { ws.docH = 0; return; }
    ws.tw = max(1, (int)std::lround((ws.cw - SBar()) * 0.95));   // 95% of the item's container:
                                                                 // KillerPDF's stretch-to-width card,
                                                                 // the last 5% left as its margin
    int padT = (int)(8 * g.dpi), below = (int)(28 * g.dpi);
    ws.top.assign((size_t)n + 1, 0);
    int y = (int)(8 * g.dpi);
    for (int i = 0; i < n; i++) { ws.top[i] = y + padT; y = ws.top[i] + WebSideTH(i) + below; }
    ws.top[n] = y;
    ws.docH = y + (int)(8 * g.dpi);
    ws.scroll = std::clamp(ws.scroll, 0, max(0, ws.docH - ws.ch));
    if (g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE)) {
        InvalidateRect(g.side, nullptr, FALSE);
        SetTimer(g.side, 2, 25, nullptr);
    }
}

static void WebSideSync() {                        // the highlight follows the visible tab
    if (!g.side || g_wt.empty()) { ws.cur = -1; return; }
    if (g_wtCur == ws.cur) return;
    ws.cur = g_wtCur;
    if (g_wtCur >= 0 && ws.docH > 0) {
        int y0 = ws.top[g_wtCur] - (int)(8 * g.dpi);
        int y1 = ws.top[g_wtCur] + WebSideTH(g_wtCur) + (int)(28 * g.dpi);
        if (y1 > ws.scroll + ws.ch) WebSideScrollTo(y0 - (int)(4 * g.dpi));
        else if (y0 < ws.scroll) WebSideScrollTo(y1 - ws.ch + (int)(4 * g.dpi));
    }
    InvalidateRect(g.side, nullptr, FALSE);
}

static void WebSideScrollTo(int y) {
    y = std::clamp(y, 0, max(0, ws.docH - ws.ch));
    if (y == ws.scroll) return;
    ws.scroll = y;
    if (g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE))
        InvalidateRect(g.side, nullptr, FALSE);
}

static int WebSideAt(int y) {                      // the tab under a rail y, like SidePageAt
    for (int i = 0; i < (int)g_wt.size(); i++)
        if (y >= ws.top[i] && y < ws.top[i] + WebSideTH(i) + (int)(30 * g.dpi)) return i;
    return -1;
}

static void WebSideDraw(HDC dc, int w, int h) {
    RECT r{ 0, 0, w, h };
    FillRect(dc, &r, GrainBrush(1));               // the reader's own field: the shared dark grain
    int n = (int)g_wt.size();
    if (!n || ws.cw <= 0 || ws.top.empty()) return;
    int avail = ws.cw - SBar();
    HGDIOBJ of = SelectObject(dc, SideFont());
    SetBkMode(dc, TRANSPARENT);
    HDC m = CreateCompatibleDC(dc);
    int padT = (int)(8 * g.dpi), labH = (int)(20 * g.dpi), padB = (int)(6 * g.dpi), gap = (int)(2 * g.dpi);
    for (int i = 0; i < n && ws.top[i] < ws.scroll + h; i++) {
        int th = WebSideTH(i);
        int x = (avail - ws.tw) / 2;
        int y = ws.top[i] - ws.scroll;
        if (y + th + gap + labH < 0) continue;
        int btop = y - padT, bbot = y + th + gap + labH + padB;
        if ((i == ws.cur || i == ws.hover) && bbot > 0) {   // the reader's item band: the green
            RECT band{ 0, max(0, btop), avail, min(h, bbot) };   // SelectionBg gradient behind the
            if (i == ws.cur) {                                   // open tab, RowHoverBrush under the cursor
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
        RECT tr{ x, y, x + ws.tw, y + th };
        const WebTab& t = g_wt[i];
        if (t.thumb) {                             // the live capture, stretched the way Draw
            HDC s = CreateCompatibleDC(dc);        // stretches a stale page raster
            HGDIOBJ o2 = SelectObject(s, t.thumb);
            SetStretchBltMode(dc, HALFTONE);   // the reader's own stretch for a stale raster
            StretchBlt(dc, x, y, ws.tw, th, s, 0, 0, t.thumbW, t.thumbH, SRCCOPY);
            SelectObject(s, o2); DeleteDC(s);
            FrameRect(dc, &tr, ThumbEdgeBrush());
        } else {                                   // the placeholder, exactly the reader's
            FillRect(dc, &tr, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
            FrameRect(dc, &tr, ThumbEdgeBrush());
        }
        RECT nr{ TABF(6), y + th + gap, avail - TABF(6), y + th + gap + labH };
        SetTextColor(dc, i == ws.cur ? RGB(0xff, 0xff, 0xff) : RGB(0xa3, 0xa3, 0xa3));
        std::wstring lab = t.title.empty() ? t.url : t.title;
        DrawTextW(dc, lab.c_str(), -1, &nr, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    DeleteDC(m);
    SelectObject(dc, of);
    if (ws.docH > ws.ch && ws.ch > 0) {            // the reader's own overlay bar
        int tl, tp; BarThumb(ws.docH, ws.ch, ws.scroll, tl, tp);
        bool hot = ws.barHot || ws.barDrag;
        int bw = hot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);
        RoundRectFill(dc, w - SBar() + (SBar() - bw) / 2, tp, bw, tl, (int)(3 * g.dpi), hot);
    }
    if (g_webOn)                                   // the card's left halo lands here, over the
        DrawRailShadow(dc, w, h, 0);               // thumbs the way the reader's rail receives
                                                   // its share - the shadow's pane paints last,
                                                   // KillerPDF's own z-order
}

static bool WebSidePump() { return false; }        // thumbs arrive through CapturePreview

static void WebSideDown(const POINT& p) {
    SetCapture(g.side);
    ws.dp = p; ws.hit = WebSideAt(p.y + ws.scroll); ws.moved = 0;
    int gb = SBar();
    if (ws.docH > ws.ch && p.x >= ws.cw - gb && p.x < ws.cw) {
        int tl, tp; BarThumb(ws.docH, ws.ch, ws.scroll, tl, tp);
        if (p.y >= tp && p.y < tp + tl) { ws.barDrag = true; ws.barGrab = p.y - tp; }
        else WebSideScrollTo(ws.scroll + (p.y < tp ? -(int)(ws.ch * 0.9) : (int)(ws.ch * 0.9)));
    }
}
static void WebSideMove(HWND h, const POINT& p) {
    if (ws.barDrag) {
        int tl, tp; BarThumb(ws.docH, ws.ch, ws.scroll, tl, tp);
        WebSideScrollTo((int)((long long)(p.y - ws.barGrab) * max(0, ws.docH - ws.ch) / max(1, ws.ch - tl)));
        return;
    }
    bool hot = ws.docH > ws.ch && p.x >= ws.cw - SBar() && p.x < ws.cw;
    if (hot) { int tl, tp; BarThumb(ws.docH, ws.ch, ws.scroll, tl, tp); hot = p.y >= tp && p.y < tp + tl; }
    int hov = (!ws.barDrag && ws.docH > 0 && p.x < ws.cw - SBar() && p.y + ws.scroll < ws.docH)
              ? WebSideAt(p.y + ws.scroll) : -1;
    if (hov != ws.hover || hot != ws.barHot) {
        ws.hover = hov; ws.barHot = hot;
        if (hov >= 0 || hot) TrackLeave(h);
        InvalidateRect(h, nullptr, FALSE);
    }
    if (GetCapture() == h) {
        int dy = p.y - ws.dp.y; ws.dp = p;
        if (dy) ws.moved += abs(dy);
    }
}
static void WebSideUp(HWND h, const POINT& p) {
    bool bar = ws.barDrag;
    ws.barDrag = false;
    if (GetCapture() == h) ReleaseCapture();
    if (!bar && ws.moved <= (int)(4 * g.dpi) && ws.hit >= 0 && ws.hit < (int)g_wt.size())
        WebSwitchTo(ws.hit);                       // a rail card switches tabs, like a page jump
    ws.hit = -1;
}
static void WebSideLeave() {
    if (ws.barHot) { ws.barHot = false; InvalidateRect(g.side, nullptr, FALSE); }
    if (ws.hover != -1) { ws.hover = -1; InvalidateRect(g.side, nullptr, FALSE); }
}
static void WebSideWheel(int delta) { WebSideScrollTo(ws.scroll + delta); }

// ---- zoom, status line, keys, the no-engine card ----

static void WebZoomBy(double f) {
    WebTab* t = WebCur();
    if (!t || !t->ctr) return;
    if (f == 0) t->zoom = 1.0;
    else t->zoom = std::clamp(t->zoom * f, 0.25, 5.0);
    t->ctr->put_ZoomFactor(t->zoom);
    StatusRepaint();
}
static int WebZoomCurrent() {                      // the nearest preset, in the 2..7 space
    WebTab* t = WebCur();
    double pct = t ? t->zoom * 100 : 100;
    int best = 4; double bd = 1e9;
    for (int i = 0; i < 6; i++) {
        double d = fabs(ZoomPcts[i] * 100 - pct);
        if (d < bd) { bd = d; best = i + 2; }
    }
    return best;
}
static void WebZoomApplyItem(int i) { WebZoomBy(i == 2 ? 0 : ZoomPcts[i] / std::max(0.01, WebCur() ? WebCur()->zoom : 1.0)); }
static const wchar_t* WebZoomLabel(int i) { return ZoomLabel(i); }

static std::wstring WebStatusLine() {
    if (GetTickCount64() < g.statusHoldUntil) return g.statusHoldText;
    WebTab* t = WebCur();
    if (!t) return L"Browser";
    if (t->loading) return L"Loading " + t->url + L"\u2026";
    if (!t->title.empty()) return t->title + L"  -  " + t->url;
    if (!t->url.empty()) return t->url;
    return L"Browser";
}

static bool WebKey(WPARAM wp, bool ctrl, bool shift) {
    if (ctrl && wp == 'T') { WebNewTab(L"https://www.google.com/", true); return true; }
    if (ctrl && wp == 'W') { WebCloseTabAt(g_wtCur); return true; }
    if (ctrl && wp == 'L') {
        if (g_wedit && !g_webFail) { SetFocus(g_wedit); PostMessageW(g_wedit, EM_SETSEL, 0, -1); }
        return true;
    }
    if (ctrl && wp == 'R') { WebTab* t = WebCur(); if (t && t->web) t->web->Reload(); return true; }
    if (wp == VK_F5) { WebTab* t = WebCur(); if (t && t->web) t->web->Reload(); return true; }
    if (ctrl && wp == VK_TAB) {
        int n = (int)g_wt.size();
        if (n > 1) WebSwitchTo((g_wtCur + (shift ? n - 1 : 1)) % n);
        return true;
    }
    if (ctrl && (wp == VK_ADD || wp == VK_OEM_PLUS)) { WebZoomBy(1.25); return true; }
    if (ctrl && (wp == VK_SUBTRACT || wp == VK_OEM_MINUS)) { WebZoomBy(0.8); return true; }
    if (ctrl && wp == '0') { WebZoomBy(0); return true; }
    if (wp == VK_ESCAPE) {
        WebTab* t = WebCur();
        if (t && t->loading && t->web) t->web->Stop();
        else if (GetFocus() == g_wedit) WebSyncOmniboxForce(), SetFocus(g.main);
        return true;
    }
    return true;                                   // browser mode: the reader's keys sit out
}

static bool WebSysKey(WPARAM wp) {
    WebTab* t = WebCur();
    if (wp == VK_LEFT) { if (t && t->web && t->canBack) t->web->GoBack(); return true; }
    if (wp == VK_RIGHT) { if (t && t->web && t->canFwd) t->web->GoForward(); return true; }
    return false;
}

static void WebFailCard(HDC dc, int w, int h) {
    int cw = (int)(420 * g.dpi), chh = (int)(190 * g.dpi);
    int x = (w - cw) / 2, y = (h - chh) / 2;
    RECT card{ x, y, x + cw, y + chh };
    HBRUSH b = CreateSolidBrush(wvb::Omnibox);
    FillRect(dc, &card, b); DeleteObject(b);
    HBRUSH f = CreateSolidBrush(wvb::Border);
    FrameRect(dc, &card, f); DeleteObject(f);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, CaptionFont(g_tbcap, 17, false, L"Killer Sans"));
    SetTextColor(dc, wvb::Ink);
    RECT t1{ x, y + (int)(24 * g.dpi), x + cw, y + (int)(52 * g.dpi) };
    DrawTextW(dc, L"The web engine is missing", -1, &t1, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, CaptionFont(g_tbcap, 13, false, L"Killer Sans"));
    SetTextColor(dc, wvb::InkDim);
    RECT t2{ x + (int)(30 * g.dpi), y + (int)(60 * g.dpi), x + cw - (int)(30 * g.dpi), y + (int)(118 * g.dpi) };
    DrawTextW(dc,
        L"Docsy's browser rides Windows' own WebView2, which this machine "
        L"does not have yet. Your PDFs are untouched - the Reader button "
        L"brings them straight back.", -1, &t2, DT_CENTER | DT_WORDBREAK);
    int bw = (int)(240 * g.dpi), bh = (int)(34 * g.dpi);
    g_webFailBtn = { x + (cw - bw) / 2, y + chh - (int)(58 * g.dpi), x + (cw + bw) / 2, y + chh - (int)(24 * g.dpi) };
    HBRUSH gb = CreateSolidBrush(RGB(0x22, 0xbb, 0x56));
    FillRect(dc, &g_webFailBtn, gb); DeleteObject(gb);
    SelectObject(dc, CaptionFont(g_tbcap, 14, false, L"Killer Sans"));
    SetTextColor(dc, RGB(0xff, 0xff, 0xff));
    DrawTextW(dc, L"Get the WebView2 runtime", -1, &g_webFailBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void WebViewClick(POINT p) {
    if (g_webFail && PtInRect(&g_webFailBtn, p))
        ShellExecuteW(g.main, L"open",
                      L"https://go.microsoft.com/fwlink/p/?LinkId=2124703",
                      nullptr, nullptr, SW_SHOWNORMAL);
}

static void StatusDraw(HDC dc, int w, int h) {
    RECT bar{ 0, 0, w, h };
    FillRect(dc, &bar, GrainBrush(1));   // the dark chrome: sidebar + footer + title = one container
    DrawPaneShadow(dc, w, h);            // the document card's shadow falls on the bar's top rows
    if (w < (int)(200 * g.dpi)) return;
    StatusRects(w);
    SetBkMode(dc, TRANSPARENT);
    int dpi = g.dpi;
    RECT tr = g_stR[ST_STATUS];          // the status line, ellipsized before the tools cell
    tr.left = (int)(16 * dpi); tr.right -= (int)(4 * dpi);   // FooterStatusInset 16
    std::wstring t = g_webOn ? WebStatusLine() : StatusTextNow();
    if (LatinText(t.c_str())) StText(dc, tr, t.c_str(), DT_LEFT | DT_END_ELLIPSIS,
                                     StFont(), 16 * g.dpi, StCapK, RGB(0xe0, 0xe0, 0xe0));
    else {                               // a held file name with script the subset drops:
        SelectObject(dc, StFontF());     // Segoe UI fallback, cell-centered as before
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        DrawTextW(dc, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    DrawVersion(dc);
    if (g_webOn) {                         // browser mode: status line + zoom + fullscreen,
        auto div = [&](int ax, int bx) {   // the reader's cells sit the mode out
            int mx = (ax + bx) / 2;
            RECT d{ mx, (SBH() - (int)(14 * dpi)) / 2, mx + 1, (SBH() + (int)(14 * dpi)) / 2 };
            FillRect(dc, &d, DividerBrush());
        };
        div(g_stR[ST_ZOUT].left, g_stR[ST_FULL].right);
        DrawGlyph(dc, ST_FULL, L"\uE740", false);
        DrawGlyph(dc, ST_ZOUT, L"\uECC9", false);
        DrawGlyph(dc, ST_ZIN, L"\uECC8", false);
        RECT cb = g_stR[ST_COMBO];
        FillRect(dc, &cb, ComboFillBrush());
        FrameRect(dc, &cb, ComboBorderBrush());
        WebTab* wt = WebCur();
        wchar_t zl[32];
        swprintf_s(zl, L"%d%%", (int)std::lround((wt ? wt->zoom : 1.0) * 100));
        RECT ct = cb; ct.left += (int)(6 * dpi); ct.right -= (int)(16 * dpi);
        StText(dc, ct, zl, DT_LEFT | DT_END_ELLIPSIS, StCombo(), 10 * g.dpi, StCapU, RGB(0xe0, 0xe0, 0xe0));
        RECT chv{ cb.right - (int)(14 * dpi), cb.top, cb.right, cb.bottom };
        SelectObject(dc, StChevron());
        SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
        DrawTextW(dc, L"\uE70D", -1, &chv, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (!DVF().n) return;                  // no focused document: status + version cells only
    std::wstring lbl, det; PaperLabel(lbl, det);
    if (!lbl.empty()) {
        RECT sr = g_stR[ST_SIZE];
        StText(dc, sr, lbl.c_str(), DT_CENTER | DT_END_ELLIPSIS, StFont(), 16 * g.dpi, StCapK,
               g.stHot == ST_SIZE ? RGB(0x22, 0xbb, 0x56) : RGB(0xe0, 0xe0, 0xe0));
    }
    auto div = [&](int ax, int bx) {     // a card-border line centred in the gap
        int mx = (ax + bx) / 2;
        RECT d{ mx, (SBH() - (int)(14 * dpi)) / 2, mx + 1, (SBH() + (int)(14 * dpi)) / 2 };
        FillRect(dc, &d, DividerBrush());
    };
    if (g_stR[ST_SIZE].right) div(g_stR[ST_MCONT].left, g_stR[ST_SIZE].right);
    div(g_stR[ST_MGRID].right, g_stR[ST_FULL].left);
    div(g_stR[ST_ZOUT].left, g_stR[ST_INV].right);
    DrawModeIcon(dc, ST_MCONT, DVF().vm == 0);
    DrawModeIcon(dc, ST_MSINGLE, DVF().vm == 1);
    DrawModeIcon(dc, ST_MTWO, DVF().vm == 2);
    DrawModeIcon(dc, ST_MGRID, DVF().vm == 3);
    DrawGlyph(dc, ST_FULL, L"\uE740", false);
    DrawGlyph(dc, ST_INV, L"\uE708", g.inv);
    DrawGlyph(dc, ST_ZOUT, L"\uECC9", false);
    DrawGlyph(dc, ST_ZIN, L"\uECC8", false);
    RECT cb = g_stR[ST_COMBO];           // the closed combo
    FillRect(dc, &cb, ComboFillBrush());
    FrameRect(dc, &cb, ComboBorderBrush());
    wchar_t zl[32];
    if (DVF().fitMode == 1) wcscpy_s(zl, L"Fit Page");
    else if (DVF().fitMode == 0) wcscpy_s(zl, L"Fit Width");
    else swprintf_s(zl, L"%d%%", (int)std::lround(DVF().zoom * 100));
    RECT ct = cb; ct.left += (int)(6 * dpi); ct.right -= (int)(16 * dpi);
    StText(dc, ct, zl, DT_LEFT | DT_END_ELLIPSIS, StCombo(), 10 * g.dpi, StCapU, RGB(0xe0, 0xe0, 0xe0));
    RECT chv{ cb.right - (int)(14 * dpi), cb.top, cb.right, cb.bottom };
    SelectObject(dc, StChevron());
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
    DrawTextW(dc, L"\uE70D", -1, &chv, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---- the zoom dropdown popup ----
// A borderless popup that keeps mouse capture for its whole life, the way menus
// do: moves update the highlight, a press on an item applies it, a press
// anywhere else dismisses, and a stolen capture dismisses via a poll timer.

static RECT g_zoomComboScr{};            // the closed combo, screen coords, while open
static int g_zoomItemH = 0, g_zoomPopW = 0;

// The popup's frame, drawn by WM_PAINT through one buffer - the items are
// buttons, so the same buffered-present rule as the toolbar applies: a direct
// paint shows the fill and the rows in two visible steps and strobes on every
// highlight flip.
static void ZoomPopDraw(HDC dc, int w, int h) {
    RECT r{ 0, 0, w, h };
    FillRect(dc, &r, PopBrush());
    FrameRect(dc, &r, ComboBorderBrush());
    SelectObject(dc, StCombo());
    SetBkMode(dc, TRANSPARENT);
    int cur = ZoomCurrent();
    int n = g_webOn ? 6 : 8;                       // the web page has no fit modes
    for (int i = 0; i < n; i++) {
        RECT it{ 1, 1 + i * g_zoomItemH, r.right - 1, 1 + (i + 1) * g_zoomItemH };
        if (i == g.zoomHot) FillRect(dc, &it, PopHoverBrush());
        RECT tx = it; tx.left += (int)(8 * g.dpi);
        StText(dc, tx, ZoomLabel(g_webOn ? i + 2 : i), DT_LEFT, StCombo(), 10 * g.dpi, StCapU,
               i == cur ? RGB(0x22, 0xbb, 0x56) : RGB(0xe0, 0xe0, 0xe0));
    }
}
static HDC g_zdc = nullptr; static HBITMAP g_zbm = nullptr, g_zold = nullptr;
static int g_zw = 0, g_zh = 0;
static void EnsureZoomBack(HDC ref, int w, int h) {
    if (g_zbm && g_zw == w && g_zh == h) return;
    if (g_zbm) { SelectObject(g_zdc, g_zold); DeleteObject(g_zbm); }
    else g_zdc = CreateCompatibleDC(ref);
    g_zbm = CreateCompatibleBitmap(ref, w, h);
    g_zold = (HBITMAP)SelectObject(g_zdc, g_zbm);
    g_zw = w; g_zh = h;
}

static void ZoomPopupClose() {
    if (!g.zoomOpen) return;
    g.zoomOpen = false; g.zoomHot = -1;
    if (g.zoomPop) {
        KillTimer(g.zoomPop, 1);
        if (GetCapture() == g.zoomPop) ReleaseCapture();
        ShowWindow(g.zoomPop, SW_HIDE);
    }
    StatusRepaint();
}

static void ZoomPopupOpen() {
    if (!g.status || (!DVF().n && !g_webOn)) return;
    RECT cr; GetClientRect(g.status, &cr);
    StatusRects(cr.right);
    RECT cb = g_stR[ST_COMBO];
    POINT tl{ cb.left, cb.bottom }, br{ cb.right, cb.bottom };
    ClientToScreen(g.status, &tl); ClientToScreen(g.status, &br);
    g_zoomComboScr = { tl.x, tl.y, br.x, br.y };
    g_zoomItemH = (int)(22 * g.dpi);
    g_zoomPopW = max((int)(cb.right - cb.left), (int)(96 * g.dpi));
    int popH = (g_webOn ? 6 : 8) * g_zoomItemH + 2;
    if (!g.zoomPop) {
        g.zoomPop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DocsyZoomPop", nullptr,
                                    WS_POPUP, tl.x, tl.y, g_zoomPopW, popH,
                                    g.main, nullptr, g.hinst, nullptr);
        if (!g.zoomPop) return;
    }
    MONITORINFO mi{ sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(g.status, MONITOR_DEFAULTTONEAREST), &mi);
    int y = tl.y;
    if (y + popH > mi.rcWork.bottom) y = tl.y - SBH() - popH;   // flip above when it spills
    SetWindowPos(g.zoomPop, HWND_TOPMOST, tl.x, y, g_zoomPopW, popH, SWP_NOACTIVATE);
    ShowWindow(g.zoomPop, SW_SHOWNOACTIVATE);
    InvalidateRect(g.zoomPop, nullptr, FALSE);
    SetTimer(g.zoomPop, 1, 250, nullptr);
    SetCapture(g.zoomPop);
    g.zoomOpen = true; g.zoomHot = ZoomCurrent();
    StatusRepaint();
}

static LRESULT CALLBACK ZoomPopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        EnsureZoomBack(dc, r.right, r.bottom);   // buffered like every hover surface: the
        ZoomPopDraw(g_zdc, r.right, r.bottom);   // items are buttons and a direct paint
        BitBlt(dc, 0, 0, r.right, r.bottom, g_zdc, 0, 0, SRCCOPY);   // strobes on a flip
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int last = g_webOn ? 5 : 7;
        int it = (p.y - 1) / max(1, g_zoomItemH);
        if (it < 0 || it > last || p.x < 0 || p.x >= g_zoomPopW) it = -1;
        if (it != g.zoomHot) { g.zoomHot = it; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int last = g_webOn ? 5 : 7;
        int it = (p.y - 1) / max(1, g_zoomItemH);
        if (p.x >= 0 && p.x < g_zoomPopW && it >= 0 && it <= last) {
            int pick = it;
            ZoomPopupClose();
            ZoomApply(pick);
        } else {
            ZoomPopupClose();            // a press on the combo toggles; elsewhere dismisses
        }
        return 0;
    }
    case WM_LBUTTONUP: return 0;         // the opening click's release lands here
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        int last = g_webOn ? 5 : 7;
        if (g.zoomHot < 0) g.zoomHot = ZoomCurrent();
        g.zoomHot = std::clamp(g.zoomHot - (d > 0 ? 1 : -1), 0, last);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1 && GetCapture() != h) ZoomPopupClose();   // capture was stolen: dismiss
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---- the footer window ----

static LRESULT CALLBACK StatusProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:                        // the pager/zoom cluster anchors to the right
        InvalidateRect(h, nullptr, FALSE);   // edge: a resize must repaint the whole strip
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureStatusBack(dc, r.right, r.bottom);
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // same grain phase as every pane
            SetBrushOrgEx(g_stbdc, -po.x, -po.y, nullptr);
            StatusDraw(g_stbdc, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, g_stbdc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {               // mirrors/hover heals, like the caption band
        RECT r; GetClientRect(h, &r);
        POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
        SetBrushOrgEx((HDC)wp, -po.x, -po.y, nullptr);
        StatusDraw((HDC)wp, r.right, r.bottom);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {                   // a held status expired: restore the routine line
            if (GetTickCount64() >= g.statusHoldUntil) { KillTimer(h, 1); StatusRepaint(); }
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hot = StatusHit(p);
        if (hot != g.stHot) {            // repaints only on a hover flip
            g.stHot = hot;
            if (hot >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.stHot != -1) { g.stHot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        StatusAction(StatusHit(p));
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (DVF().n) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (StatusHit(p) == ST_COMBO) { ZoomCycle(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1); return 0; }
        }
        return SendMessageW(g.main, m, wp, lp);   // route like the main window does
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            SetCursor(LoadCursorW(nullptr, StatusHit(p) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

