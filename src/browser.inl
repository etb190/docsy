// ---- browsing history forward declarations ----
static void HistAdd(const std::wstring& url, const std::wstring& title);
static void HistUpdateTitle(const std::wstring& url, const std::wstring& title);
static void HistDropdownToggle();
static void HistFullOpen();

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

static int g_wbHot = -1;

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
        float fh = (float)(int)(44 * g.dpi);       // the fade rides the nav band's own foot - the
        Gdiplus::RectF fadeR(0.f, fh * 0.45f, (float)w, fh * 0.55f);   // bookmarks bar below is a flat sheet, so the ramp must end at the band's top, not the strip's
        Gdiplus::LinearGradientBrush fadeBr(fadeR, c0, c1, Gdiplus::LinearGradientModeVertical);
        gfade.FillRectangle(&fadeBr, fadeR);
    }
    SetBkMode(dc, TRANSPARENT);
    HFONT gf = WebGlyphFont(16);
    static const wchar_t* const gl[WB_COUNT] = {
        L"\uE8A5", L"\uE76B", L"\uE76C", nullptr, L"\uE8A5", L"\uE710", L"\uE712", nullptr, L"\uE7B8", nullptr, L"\uE81C", L"\uE8F1", L"\uE8BD"
    };                                                 // reload and star ride beside the field; the reader hand, hist, the
                                                       // summary navigator and the AI robot at far right
    for (int i = 0; i < WB_COUNT; i++) {
        if (g_wbR[i].right <= g_wbR[i].left) continue;
        if (i == WB_RELOAD || i == WB_STAR) continue;  // custom tiles outside the field
        bool en = WebBtnEnabled(i);
        bool hot = en && g_wbHot == i, pr = en && g.tbPressed == i;
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
    // the favicon at the capsule's left
    if (t && !t->url.empty() && t->url != L"about:blank" && t->url.rfind(L"about:", 0) != 0) {
        int is = (int)(16 * g.dpi);
        int ix = (int)g_webOmni.left + TABF(10);
        int iy = (int)g_webOmni.top + ((int)(g_webOmni.bottom - g_webOmni.top) - is) / 2;
        RECT ir{ ix, iy, ix + is, iy + is };
        Gdiplus::Graphics gx(dc);
        gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        BmDrawFavicon(gx, dc, ir, t->url, t->title);
    }
    // the padlock: hugging the centered site name while idle, pinned to the
    // field's left edge while it is being edited; only secure pages wear one
    bool secure = t && t->url.rfind(L"https://", 0) == 0;
    if (secure) {
        int lockX = (int)g_webOmni.left + TABF(32);
        if (g_wedit) {                                 // the lock follows the centered text, idle or editing
            wchar_t tx[512] = L"";
            int len = GetWindowTextW(g_wedit, tx, 512);
            if (len > 0) {
                HGDIOBJ of0 = SelectObject(dc, g_webEditFont);
                SIZE sz{}; GetTextExtentPoint32W(dc, tx, len, &sz);
                SelectObject(dc, of0);
                int cx = (int)(g_webOmni.left + g_webOmni.right) / 2;
                lockX = max((int)g_webOmni.left + TABF(32), cx - (int)sz.cx / 2 - TABF(18));
            }
        }
        SelectObject(dc, WebGlyphFont(12));
        SetTextColor(dc, wvb::InkDim);
        RECT lk{ lockX, g_webOmni.top, lockX + TABF(16), g_webOmni.bottom };
        DrawTextW(dc, L"\uE72E", -1, &lk, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // reload at rest, stop/reset while loading - to the right of the omnibox outside
    RECT rf = g_wbR[WB_RELOAD];
    if (rf.right > rf.left) {
        bool en = WebBtnEnabled(WB_RELOAD);
        bool hot = en && (g_wbHot == WB_RELOAD);
        bool pr = en && (g.tbPressed == WB_RELOAD);
        if (hot || pr) WebTile(dc, rf, pr ? wvb::Pressed : wvb::Hover);
        bool loading = (t && t->loading);
        // Refresh (\uE72C) and Reset/Cancel (\uE711) rendered with matching visual optical size:
        SelectObject(dc, WebGlyphFont(loading ? 11 : 12));
        SetTextColor(dc, !en ? wvb::InkOff : wvb::Ink);
        DrawTextW(dc, loading ? L"\uE711" : L"\uE72C", -1, &rf,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // the bookmark star, to the left of the omnibox outside:
    RECT st = g_wbR[WB_STAR];
    if (st.right > st.left) {
        bool en = !g_webFail;
        bool hot = en && (g_wbHot == WB_STAR);
        bool pr = en && (g.tbPressed == WB_STAR);
        if (hot || pr) WebTile(dc, st, pr ? wvb::Pressed : wvb::Hover);
        bool saved = t && BmFindUrl(t->url) >= 0;
        SelectObject(dc, WebGlyphFont(14));
        SetTextColor(dc, g_webFail ? wvb::InkOff : (saved ? wvb::Blue : wvb::Ink));
        RECT stg = st;
        stg.top += 1; stg.bottom += 1;
        DrawTextW(dc, saved ? L"\uE735" : L"\uE734", -1, &stg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
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
        RECT pr{ g_webOmni.left + TABF(34), g_webOmni.top, g_webOmni.right - TABF(34), g_webOmni.bottom };   // symmetric centered ground
        DrawTextW(dc, L"Search or enter website name", -1, &pr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // the bookmarks bar rides the strip's lower band: painted last, so it
    // covers the toolbar's foot fade in its own rows and restores the hairline
    // the fade covered - the band's top edge stays clean, the card's boundary
    // (the hairline at h-1) belongs to the bar's own draw
    BmBarDraw(dc, w, h);
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
static HDC g_stripdc = nullptr;
static HBITMAP g_stripbm = nullptr;
static int g_stripw = 0, g_striph = 0;

static void EnsureStripBack(HDC ref, int w, int h) {
    if (g_stripdc && g_stripbm && g_stripw == w && g_striph == h) return;
    if (g_stripbm) DeleteObject(g_stripbm);
    if (!g_stripdc) g_stripdc = CreateCompatibleDC(ref);
    g_stripbm = CreateCompatibleBitmap(ref, max(1, w), max(1, h));
    SelectObject(g_stripdc, g_stripbm);
    g_stripw = w; g_striph = h;
}

static LRESULT CALLBACK WebStripProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureStripBack(dc, r.right, r.bottom);
            WebStripPaint(g_stripdc, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, g_stripdc, 0, 0, SRCCOPY);
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
        int hot = WebNavHit(p), xh = WxHit(p), bh = BmHit(p);
        if (hot != g_wbHot || xh != g_wxHot || bh != g_bmHot) {
            g_wbHot = hot; g_wxHot = xh; g_bmHot = bh;
            if (hot >= 0 || xh >= 0 || bh >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_wbHot != -1 || g_wxHot != -1 || g_bmHot != -1) {
            g_wbHot = -1; g_wxHot = -1; g_bmHot = -1; InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hit = WebNavHit(p), xh = WxHit(p), bh = BmHit(p);
        if (hit >= 0) {                            // capture held: the toolbar's press story
            SetCapture(h);
            g.tbPressed = hit;
            InvalidateRect(h, nullptr, FALSE);
        } else if (xh >= 0) {                      // an extension's icon: same press story
            SetCapture(h);
            g_wxPress = xh;
            InvalidateRect(h, nullptr, FALSE);
        } else if (bh >= 0) {                      // a bookmark row: the press story once more
            SetCapture(h);
            g_bmPress = bh;
            InvalidateRect(h, nullptr, FALSE);
        } else if (PtInRect(&g_webOmni, p) && g_wedit && !g_webFail) {
            SetFocus(g_wedit);                     // a click on the pill focuses the URL
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MBUTTONDOWN: {                         // a middle click on a bookmark row: the
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   // row opens in a tab of its own
        int mb = BmHit(p);
        if (mb >= 0) BmClick(mb, true);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_bmPress >= 0) {                      // a bookmark row's release answers where it pressed
            int pr = g_bmPress; g_bmPress = -1;
            if (GetCapture() == h) ReleaseCapture();
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            InvalidateRect(h, nullptr, FALSE);
            if (BmHit(p) == pr) BmClick(pr, false);
            return 0;
        }
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
        if (g.tbPressed >= 0 || g_wxPress >= 0 || g_bmPress >= 0) {
            g.tbPressed = -1; g_wxPress = -1; g_bmPress = -1; InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CONTEXTMENU: {                         // right-click: a bookmark row answers with its own
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   // menu (edit, remove, open in a tab); the bare
        if (p.x != -1 || p.y != -1) {              // strip keeps the extensions' menu it always had
            ScreenToClient(h, &p);
            int rh = BmHit(p);
            if (rh >= 0 && g_bmRows[rh].kind != 2) { BmRowMenu(rh); return 0; }
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
            bool bi = BmHit(p) >= 0;
            SetCursor(LoadCursorW(nullptr, (hit >= 0 || xi || bi) ? IDC_HAND : (pill ? IDC_IBEAM : IDC_ARROW)));
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
    int navH = (int)(44 * g.dpi);              // the nav row centers in its own 44-band - the
                                               // bookmarks bar's ground rides below it, and centering
                                               // in the whole strip slid the pill under the bar
    int d = TABF(30), cy = (navH - 1 - d) / 2, pitch = TABF(30);   // the reader toolbar's own
               // spacing: buttons touch (each face's padding makes the gap), not 2dp apart
    int ph = TABF(30), py = (navH - 1 - ph) / 2;
    int x = TABF(10);
    g_wbR[WB_SIDE] = { x, cy, x + d, cy + d };
    x += pitch + TABF(10); g_wbR[WB_BACK] = { x, cy, x + d, cy + d };
    x += pitch; g_wbR[WB_FWD] = { x, cy, x + d, cy + d };
    int mx = (int)r.right - TABF(10);
    g_wbR[WB_MENU] = { 0, 0, 0, 0 };               // the 3-dot is gone: the toolbar keeps only
    g_wbR[WB_HIST] = { mx - d, cy, mx, cy + d };   // history button at the far right
    mx -= pitch;
    g_wbR[WB_SUM] = { mx - d, cy, mx, cy + d };    // the summary navigator beside it:
                                                   // the browser's own web seat
    mx -= pitch;
    g_wbR[WB_AI] = { mx - d, cy, mx, cy + d };     // the AI robot beside it: the panel's
                                                   // toggle in browser mode, like the
                                                   // reader toolbar's and editor's
    mx -= pitch;
    g_wbR[WB_NEWTAB] = { mx - d, cy, mx, cy + d }; // the + and, left of it, each extension's icon
    mx -= pitch;
    g_wbR[WB_PDF] = { mx - d, cy, mx, cy + d };    // the worst-case road: a page sitting in the
                                                   // engine's own pdf viewer goes to the reader by hand
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
    int gap = TABF(6);
    int lo = (int)g_wbR[WB_FWD].right + TABF(10) + d + gap;
    int hi = leftEdge - TABF(10) - d - gap;
    int pw = max(10, min(hi - lo, TABF(560)));     // the capsule: centered in the window, capped
    int pl = std::clamp((int)r.right / 2 - pw / 2, lo, max(lo, hi - pw));
    g_webOmni = { pl, py, pl + pw, py + ph };
    g_wbR[WB_STAR] = { pl - gap - d, cy, pl - gap, cy + d };      // star to the left outside omnibox
    g_wbR[WB_RELOAD] = { pl + pw + gap, cy, pl + pw + gap + d, cy + d }; // reload to the right outside omnibox
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
        int ew = max(10, (int)(g_webOmni.right - g_webOmni.left) - 2 * TABF(34)); // symmetric padding
        MoveWindow(g_wedit, g_webOmni.left + TABF(34), g_webOmni.top + (ph - eh) / 2, ew, eh, TRUE);
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
        } else {
            int is = (int)(16 * g.dpi);
            int ix = fl + TABF(10);
            int iy = ft + (G.faceH - is) / 2;
            RECT ir{ ix, iy, ix + is, iy + is };
            if (t.url.empty() || t.url == L"about:blank") {
                SelectObject(dc, WebGlyphFont(12));
                SetTextColor(dc, wvb::InkDim);
                DrawTextW(dc, L"\uE774", -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                BmDrawFavicon(gs, dc, ir, t.url, t.title);
            }
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

// the worst-case road: a document that slipped past every capture and now sits
// in the engine's own pdf viewer - or any address the reader should own - goes
// to the reader BY HAND: the same credentialed fetch the automatic roads use,
// the same endgame, the same hand-off and step-back home (the definition rides
// below the note globals it words)
static void WebForceReaderOpen();

static void WebBarClick(int id) {
    WebTab* t = WebCur();
    switch (id) {
    case WB_READER: WebToggle(false); return;
    case WB_PDF: WebForceReaderOpen(); return;    // the hand road, for the page the
                                                  // automatic capture never caught
    case WB_EXT: { POINT z{ g_wbR[WB_EXT].left, g_wbR[WB_EXT].bottom }; WxMenu(z); } return;
    case WB_SIDE:
        g_webSide = !g_webSide;
        if (g.side) ShowWindow(g.side, g_webSide ? SW_SHOWNOACTIVATE : SW_HIDE);
        LayoutMain(); WebPlace(); WebRepaintChrome();
        return;
    case WB_BACK: if (t && t->web && t->canBack) t->web->GoBack(); return;
    case WB_FWD: if (t && t->web && t->canFwd) t->web->GoForward(); return;
    case WB_RELOAD:
        if (t && t->web) { if (t->loading) t->web->Stop(); else t->web->Reload(); }
        return;
    case WB_NEWTAB: WebNewTab(L"https://www.google.com/", true); return;
    case WB_HIST: HistDropdownToggle(); return;
    case WB_SUM: SumToggle(1); return;             // the browser's own web summary seat
    case WB_AI: AiChatToggle(); return;            // the browser sidechat's own toggle
    case WB_STAR: BmStarClick(); return;           // the pill's star: saved page back off the list,
                                                   // stranger to the bubble (bookmarks.inl)
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
    if (s == L"docsy://history" || s == L"about:history" || s == L"history") {
        HistFullOpen();
        return;
    }
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

// the pdf roads' transient word (Avalanche's status strip, drawn in the view
// itself): a fetch in flight, a site that refused, a link that never was one
static std::wstring g_webNote;
static unsigned long long g_webNoteUntil = 0;

// the hand road itself: the toolbar's WB_PDF
static void WebPdfFetch(const std::wstring& url, const std::wstring& alt, int ix);
static int WebTabIdx(unsigned long long id);
static void WebForceReaderOpen() {
    WebTab* t = WebCur();
    if (!t || t->url.empty()) return;
    WxLogf(L"pdf: the hand road takes the page to the reader - %s", t->url.c_str());
    g_webNote = L"Fetching the PDF for the reader...";
    g_webNoteUntil = GetTickCount64() + 150000;        // the fetch's own timeouts bound the wait
    WebPdfFetch(t->url, t->url, WebTabIdx(t->id));
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

static void WebEnsurePump() {
    if (!g.main) return;
    bool need = false;
    for (auto& t : g_wt)                   // a navigation in flight rides the pump too: the
        if (t.captureDue || t.capturePending || t.loading
            || t.pdfCaptureBusy || t.pdfFetching.size()) need = true;   // watchdog lives there,
    if (g_webNoteUntil) need = true;       // and a standing word needs its take-down beat
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
static int WebTabIdx(unsigned long long id);      // the definition rides below, with the
                                                  // pdf roads: the pump's stall branch and
static void WebPdfEscalate(WebTab* t, int ix);    // the net's own walk both need it early
static void WebThumbDecode(int id, IStream* st);
static void WebSideSync();                        // defined below WebSideCardRect; the
                                                  // NavigationCompleted bridge runs ahead of it

static void WebSwitchSyncSoon() {                 // the tab switch's sidebar beat: the rail's
    if (!g.main) return;                          // highlight repaints on a one-shot 60ms
    KillTimer(g.main, 4735);                      // timer, and the new tab's own
    SetTimer(g.main, 4735, 60, nullptr);          // NavigationCompleted snaps it in the
}                                                 // moment the page actually presents - the
                                                  // rail changes WITH the view's frame, never
                                                  // a visible beat ahead of it. The old switch
                                                  // repainted the rail instantly while the
                                                  // webview was still waking the tab's hidden
                                                  // renderer, so the sidebars always led.

static void WebCaptureTick() {                     // the main window's pump (timer 7)
    WebNavWatchdog();                              // the stuck-navigation self-heal rides along
    if (g_webNoteUntil && GetTickCount64() >= g_webNoteUntil) {   // the word's take-down beat
        g_webNote.clear();
        g_webNoteUntil = 0;
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    }
    unsigned long long now = GetTickCount64();
    for (auto& t : g_wt) {
        if (t.pdfCaptureBusy && t.pdfCaptureStart
            && now - t.pdfCaptureStart > 20000) {      // a capture that never lands frees
            t.pdfCaptureBusy = false;                  // its own slot and escalates: the
            WxLogf(L"pdf: the capture stalled 20s - the credentialed fetch takes over");
            WebPdfEscalate(&t, WebTabIdx(t.id));       // fetch gets one clean try instead
        }
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

// the thumb's raster lands OFF the UI thread now. A full-page capture is a
// multi-megapixel PNG: Gdiplus' decode plus GetHBITMAP plus the HALFTONE
// downscale were 100-300ms of synchronous work on the UI thread, and they ran
// exactly when the rail or the view was mid-move (a resize re-captures, a
// switch re-captures) - the whole frame seized, then everything landed at once,
// and the reader read that as the open tearing and glitching. The worker turns
// the PNG into the 240dp DIB and posts the finished HBITMAP home (WM_APP + 10);
// the UI thread only stores it and repaints the one card band. No GDI+ object
// crosses the thread line: the worker owns its Bitmap end to end, the UI owns
// only the HBITMAP. dpi is copied into the job before the spawn - g.dpi moves.
struct WebThumbDone { int id; HBITMAP bm; int w, h; double ar; unsigned long long seq; };
static unsigned long long g_thumbSeq = 0;

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
    auto* job = new WebThumbDone{ id, nullptr, (int)(240 * g.dpi), 0, 0.0, ++g_thumbSeq };
    std::thread([job, st2]() {
        LARGE_INTEGER z{}; st2->Seek(z, STREAM_SEEK_SET, nullptr);
        Gdiplus::Bitmap* bm = Gdiplus::Bitmap::FromStream(st2.Get());
        int bw = bm ? (int)bm->GetWidth() : 0, bh = bm ? (int)bm->GetHeight() : 0;
        if (!bw || !bh) { delete bm; delete job; return; }
        HBITMAP full = nullptr;
        bm->GetHBITMAP(Gdiplus::Color(255, 0x1c, 0x1c, 0x1c), &full);   // the card's face behind
        delete bm;
        if (!full) { delete job; return; }
        int TW2 = job->w > 0 ? job->w : 240, TH2 = max(1, (int)((long long)TW2 * bh / bw));
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
        if (!scaled) { delete job; return; }
        job->bm = scaled; job->h = TH2; job->ar = (double)bh / max(1, bw);
        if (g.main) PostMessageW(g.main, WM_APP + 10, 0, (LPARAM)job);
        else { DeleteObject(scaled); delete job; }   // the frame died mid-flight: nobody
    }).detach();                                     // is coming to free the DIB for us
}

static void WebThumbArrive(WebThumbDone* d) {          // WM_APP + 10: the worker's DIB is
    if (!d) return;                                    // home - the UI thread stores it and
    WebTab* t = WebTabById(d->id);                     // repaints just the card band. A stale
    if (!t || d->seq < t->thumbSeq) {                  // seq loses the race to a newer capture
        if (d->bm) DeleteObject(d->bm);                // of the same tab: the older thread's
        delete d; return;                              // pixels must never overwrite the newer
    }
    t->thumbSeq = d->seq;
    if (t->thumb) DeleteObject(t->thumb);
    t->thumb = d->bm; t->thumbW = d->w; t->thumbH = d->h;
    t->ar = d->ar;
    int id = d->id;                                // the band lookup runs after the job is freed
    delete d;
    if (g_webOn && g_webSide && g.side) {      // only a SHOWN rail repaints: hidden, ws.cw is
                                               // still 0 and ws.top is empty - the card rect
                                               // below indexed a vector that never laid out,
                                               // and the first browser flip died the moment
                                               // the tab's thumb landed
        WebSideLayout();                           // the card heights are width-fixed, so the
        int ix = -1;                               // rows never move: just the card that
        for (int i2 = 0; i2 < (int)g_wt.size(); i2++)   // arrived repaints - a burst of
            if (g_wt[i2].id == id) { ix = i2; break; }   // captures no longer strobes the rail
        RECT sr; GetClientRect(g.side, &sr);
        RECT cr = WebSideCardRect(ix, sr.bottom);
        if (cr.right > cr.left) InvalidateRect(g.side, &cr, FALSE);
    }
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
    BmStarSync();                                  // a navigation may cross the star's line: saved
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // or not, the pill's fill follows the URL
}

// ---- The PDF hand-off (Avalanche's roads, ported verbatim) ---------------
// A PDF is never rendered by the browser, and the browser's own request is
// never cancelled: navigations and downloads ride Chromium's network stack,
// the only one a CDN or an anti-bot wall will trust. A download streams
// through DownloadStarting straight into the temp area; a document the engine
// receives itself is copied out of the response event's own stream; the
// bundled helper's button hands the embedded document's address over for the
// pages the host cannot see into - and every candidate is checked for a real
// %PDF- header before the reader ever hears of it. Every road ends at
// WebHandPdfToReader, which is the window's HideWebPane + OpenInNewTab: the
// browser steps aside and the document opens as an ordinary reader tab.

static int WebTabIdx(unsigned long long id) {
    for (int i = 0; i < (int)g_wt.size(); i++)
        if (g_wt[i].id == id) return i;
    return -1;
}

static void WebPdfGuardOn(WebTab* t, int ix) {         // the early visual guard: blank the
    if (!t || t->pdfGuard) return;                     // view the beat before the headers
    t->pdfGuard = true;                                // speak - a false positive costs one
    if (t->ctr && ix == g_wtCur) t->ctr->put_IsVisible(FALSE);   // blanked moment, never a
}                                                      // cancelled navigation

static void WebPdfGuardOff(WebTab* t, int ix) {        // the guard's stand-down: only the
    if (!t) return;                                    // view on screen gets its paint back
    t->pdfGuard = false;
    if (t->ctr && ix == g_wtCur && g_webOn) t->ctr->put_IsVisible(TRUE);
}

static void WebPdfNote(const wchar_t* msg) {
    g_webNote = msg;
    g_webNoteUntil = GetTickCount64() + 3000;
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// The pdf-shaped-address test for the EARLY VISUAL GUARD only - it never
// cancels a navigation. A .pdf suffix or a publisher route (/pdf/<id>, a
// trailing /pdf, /getpdf?id=) raises the guard a beat before the response
// headers speak; a false positive costs one blanked moment, and the
// header-driven stand-down gives the page straight back.
static bool WebLooksLikePdf(const std::wstring& url) {
    if (url.size() < 9) return false;
    wchar_t sch = 0;
    if (_wcsnicmp(url.c_str(), L"http://", 7) == 0) sch = 1;
    else if (_wcsnicmp(url.c_str(), L"https://", 8) == 0) sch = 1;
    if (!sch) return false;
    std::wstring path = url;
    {   size_t q = path.find_first_of(L"?#");          // the query never names the document
        if (q != std::wstring::npos) path = path.substr(0, q);
    }
    size_t at = path.find(L"/", 8);                    // the authority ends at the first slash
    path = at != std::wstring::npos ? path.substr(at) : L"/";
    for (auto& c : path) c = (wchar_t)towlower(c);
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, L".pdf") == 0) return true;
    std::vector<std::wstring> segs;
    {   std::wstring cur;
        for (wchar_t c : path) {
            if (c == L'/') { if (!cur.empty()) segs.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) segs.push_back(cur);
    }
    for (size_t i = 0; i + 1 < segs.size(); i++)
        if (segs[i] == L"pdf") return true;            // /pdf/<id>
    if (!segs.empty() && (segs.back() == L"pdf" || segs.back() == L"getpdf")) return true;
    return false;
}

// A real PDF announces itself with %PDF- within the first kilobyte - the
// window every reader scans. Anything else never reaches the reader.
static bool WebHasPdfHeaderFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    unsigned char head[1024];
    DWORD got = 0;
    ReadFile(f, head, sizeof(head), &got, nullptr);
    CloseHandle(f);
    if (got < 100) return false;
    int scan = (int)(got >= 1024 ? 1024u : got) - 4;
    for (int i = 0; i < scan; i++)
        if (head[i] == 0x25 && head[i + 1] == 0x50 && head[i + 2] == 0x44
            && head[i + 3] == 0x46 && head[i + 4] == 0x2D)
            return true;
    return false;
}

static std::wstring WebPdfTempDir() {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = (n ? std::wstring(tmp) : std::wstring(L".")) + L"docsy_pdf";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring WebPdfSafeName(std::wstring name) {   // a path or a bare URL reduced
    size_t q = name.find_first_of(L"?#");                 // to a clean file name
    if (q != std::wstring::npos) name = name.substr(0, q);
    size_t at = name.find_last_of(L"/\\");
    if (at != std::wstring::npos) name = name.substr(at + 1);
    if (name.empty()) name = L"document.pdf";
    static const wchar_t* bad = L"<>:\"|?*";
    for (wchar_t& c : name) if (c < 32 || wcschr(bad, c)) c = L'_';
    if (name.size() > 64) name = name.substr(name.size() - 64);   // keep the tail: the
                                                                  // extension usually lives there
    if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".pdf") != 0) name += L".pdf";
    return name;
}

static std::wstring WebPdfTempPath(const std::wstring& name) {   // one unique temp file
    std::wstring dir = WebPdfTempDir();
    std::wstring p = dir + L"\\" + name;
    wchar_t alt[MAX_PATH];
    for (int k = 2; k < 1000; k++) {
        HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); return p; }
        swprintf_s(alt, L"%s\\%04d_%s", dir.c_str(), k, name.c_str());
        p = alt;
    }
    return p;
}

static void WebHandPdfToReader(const std::wstring& path) {   // every road's one ending:
    if (!g_webOn || g_sync) return;                          // the pane steps aside, and the
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;   // document is an
    for (auto& t : g_wt) {                                   // ordinary reader tab
        t.pdfGuard = false;
        t.pdfCaptureBusy = false;
        t.pdfFetching.clear();
    }
    WxLogf(L"pdf: the document hands itself to the reader - %s", path.c_str());
    g_webNote.clear(); g_webNoteUntil = 0;   // the fetching word's job is done
    WebToggle(false);
    TabOpenInto(false, path);
    // the browser steps back on its own: the document left it, and the road
    // home is the one the user came on - the page that offered the link. The
    // next flip to the browser lands on the search results, not on a dead
    // viewer tab, and the browsing continues from where the click began.
    WebTab* back = WebCur();
    if (back && back->web) {
        BOOL canBack = FALSE;
        if (SUCCEEDED(back->web->get_CanGoBack(&canBack)) && canBack)
            back->web->GoBack();
    }
}

// A tiny quoted-string field out of the helper's JSON word - no parser, one
// shape the content script posts, {"type":"docsy-open-pdf","url":...,"alt":...}
static std::wstring WebJsonField(const std::wstring& m, const wchar_t* key) {
    std::wstring needle = std::wstring(L"\"") + key + L"\"";
    size_t p = m.find(needle);
    if (p == std::wstring::npos) return L"";
    p = m.find(L'"', p + needle.size());               // the colon, then the opening quote
    if (p == std::wstring::npos) return L"";
    p++;
    std::wstring out;
    while (p < m.size() && m[p] != L'"') {
        if (m[p] == L'\\' && p + 1 < m.size()) { p++; }  // the escapes a URL can carry ride raw
        out += m[p++];
    }
    return out;
}

// The response event's capture: the engine's own bytes are already copying -
// the worker drains the stream the runtime hands the handler, while Chromium
// keeps writing it, and the endgame lands on the UI thread (WM_APP + 12) to
// check the header, hand the reader the document or give the page straight
// back. Cache-served answers hide their bytes; the later roads fetch for those.
static void WebPdfResponseSeen(unsigned long long id, ICoreWebView2WebResourceResponseReceivedEventArgs* a) {
    int ix = WebTabIdx(id);
    WebTab* t = WebTabById(id);
    if (!t || ix != g_wtCur || !a) return;             // the view on screen owns the roads
    wrl::ComPtr<ICoreWebView2WebResourceRequest> req;
    wrl::ComPtr<ICoreWebView2WebResourceResponseView> resp;
    if (FAILED(a->get_Request(&req)) || FAILED(a->get_Response(&resp)) || !req || !resp) return;
    LPWSTR ur = nullptr;
    std::wstring uri;
    if (SUCCEEDED(req->get_Uri(&ur)) && ur) { uri = ur; CoTaskMemFree(ur); }
    std::wstring mime;
    wrl::ComPtr<ICoreWebView2HttpResponseHeaders> rh;
    if (SUCCEEDED(resp->get_Headers(&rh)) && rh) {
        BOOL has = FALSE;
        if (SUCCEEDED(rh->Contains(L"Content-Type", &has)) && has) {
            LPWSTR ct = nullptr;
            if (SUCCEEDED(rh->GetHeader(L"Content-Type", &ct)) && ct) { mime = ct; CoTaskMemFree(ct); }
        }
    }
    size_t cut = mime.find(L';');
    if (cut != std::wstring::npos) mime = mime.substr(0, cut);
    while (!mime.empty() && (mime.front() == L' ' || mime.front() == L'\t')) mime.erase(mime.begin());
    while (!mime.empty() && (mime.back() == L' ' || mime.back() == L'\t')) mime.pop_back();
    bool pdfShaped = WebLooksLikePdf(uri);
    // the mime the road accepts: the honest two, plus the shells publishers wrap
    // pdfs in (octet-stream, binary, force-download) and the absent header - for
    // a pdf-shaped address; the worker's %PDF- check keeps every lie harmless
    bool isPdf = _wcsicmp(mime.c_str(), L"application/pdf") == 0
              || _wcsicmp(mime.c_str(), L"application/x-pdf") == 0
              || (pdfShaped && (_wcsicmp(mime.c_str(), L"application/octet-stream") == 0
                             || _wcsicmp(mime.c_str(), L"application/binary") == 0
                             || _wcsicmp(mime.c_str(), L"application/force-download") == 0
                             || _wcsicmp(mime.c_str(), L"application/download") == 0
                             || mime.empty()));
    int status = 0;
    resp->get_StatusCode(&status);
    if (isPdf || pdfShaped || t->pdfGuard)             // the roads speak in webext.log:
        WxLogf(L"pdf: response %s (%d) - %s",          // mime, verdict, address - the next
               mime.empty() ? L"(no type)" : mime.c_str(), status, uri.c_str());   // report has numbers
    if (!isPdf) {
        // a pdf-shaped address that delivered an ordinary page gets the engine
        // straight back - an HTML answer (a Cloudflare challenge included) is
        // a page the reader must see and solve, WHATEVER status code it carries
        bool html = mime.rfind(L"text/html", 0) == 0;
        if (html && status >= 200 && status < 300 && uri == t->url) t->pdfSpokeHtml = true;
        if (t->pdfGuard && (html || (status >= 200 && status < 300 && pdfShaped)))
            WebPdfGuardOff(t, ix);
        return;
    }
    if (t->pdfCaptureBusy) return;                     // one capture at a time per tab
    t->pdfCaptureBusy = true;
    t->pdfCaptureStart = GetTickCount64();             // the pump's stall clock starts here
    WebPdfGuardOn(t, ix);
    WxLogf(L"pdf: the capture starts - %s", uri.c_str());
    // the view's GetContent hands the stream over at completion - but the Invoke
    // parameter is an [in] lease that dies the moment the handler returns, so the
    // worker holds its own reference: a captured raw pointer is a use-after-free
    // the moment the reader is about to take the document (the first crash)
    HRESULT gcHr = resp->GetContent(
        Microsoft::WRL::Callback<ICoreWebView2WebResourceResponseViewGetContentCompletedHandler>(
            [id](HRESULT hr, IStream* st) -> HRESULT {
                if (FAILED(hr) || !st) {               // no bytes: the page stands back up,
                    if (g.main) PostMessageW(g.main, WM_APP + 12, (WPARAM)id, (LPARAM)nullptr);   // the later roads take their turn
                    return S_OK;
                }
                wrl::ComPtr<IStream> keep = st;        // the worker's own lease on the bytes
                std::thread([id, keep]() {
                    std::wstring path = WebPdfTempPath(WebPdfSafeName(L"document.pdf"));
                    bool ok = false;
                    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (f != INVALID_HANDLE_VALUE) {
                        unsigned char buf[65536];
                        ULONG got = 0;
                        bool err = false;
                        for (;;) {
                            if (FAILED(keep->Read(buf, sizeof(buf), &got))) { err = true; break; }
                            if (!got) break;
                            DWORD wr = 0;
                            if (!WriteFile(f, buf, got, &wr, nullptr) || wr != got) { err = true; break; }
                        }
                        CloseHandle(f);
                        ok = !err;
                    }
                    bool pdf = ok && WebHasPdfHeaderFile(path);
                    if (ok && !pdf) DeleteFileW(path.c_str());     // temp litter is harmless but rude
                    if (g.main) PostMessageW(g.main, WM_APP + 12, (WPARAM)id,
                                             (LPARAM)(pdf ? new std::wstring(path) : (std::wstring*)nullptr));
                    else if (pdf) DeleteFileW(path.c_str());
                }).detach();
                return S_OK;
            }).Get());
    if (FAILED(gcHr)) {                                // a refused capture frees the slot
        WxLogf(L"pdf: the capture was refused (hr 0x%08X) - the fetch takes over", (unsigned)gcHr);
        t->pdfCaptureBusy = false;
        WebPdfEscalate(t, ix);                         // and the credentialed fetch gets
    }                                                  // one clean try, no button press
}

// The helper button's road: the credentialed out-of-process fetch - the
// session's own cookies (HttpOnly ones too, straight from the engine's own
// jar), the engine's real user agent, the page that offered the link as
// referrer, and honest decompression. Sites that answered an anonymous fetch
// with a wall answer this one the way they answer the user's own browser.
static void WebPdfFetch(const std::wstring& url, const std::wstring& alt, int ix) {
    if (ix < 0 || ix >= (int)g_wt.size()) return;
    WebTab* t = &g_wt[ix];
    if (!t->web) return;
    if (t->pdfFetching == url) return;                 // one flight per address
    t->pdfFetching = url;
    WebPdfNote(L"Fetching the PDF for the reader...");
    std::wstring ua = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
    {   wrl::ComPtr<ICoreWebView2Settings> stg;        // the engine's own UA, when it has one
        wrl::ComPtr<ICoreWebView2Settings2> stg2;
        if (SUCCEEDED(t->web->get_Settings(&stg)) && stg
            && SUCCEEDED(stg->QueryInterface(IID_PPV_ARGS(&stg2))) && stg2) {
            LPWSTR u = nullptr;
            if (SUCCEEDED(stg2->get_UserAgent(&u)) && u && *u) { ua = u; CoTaskMemFree(u); }
            else if (u) CoTaskMemFree(u);
        }
    }
    wrl::ComPtr<ICoreWebView2_2> w2;
    if (FAILED(t->web->QueryInterface(IID_PPV_ARGS(&w2))) || !w2) { t->pdfFetching.clear(); return; }
    wrl::ComPtr<ICoreWebView2CookieManager> cm;
    if (FAILED(w2->get_CookieManager(&cm)) || !cm) { t->pdfFetching.clear(); return; }
    unsigned long long tid = t->id;
    HWND main = g.main;
    HRESULT hr = cm->GetCookies(url.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [tid, url, alt, ua, main](HRESULT hr, ICoreWebView2CookieList* list) -> HRESULT {
                std::wstring cookieHdr;
                if (SUCCEEDED(hr) && list) {
                    UINT32 n = 0;
                    if (SUCCEEDED(list->get_Count(&n)))
                        for (UINT32 i = 0; i < n; i++) {
                            wrl::ComPtr<ICoreWebView2Cookie> ck;
                            if (FAILED(list->GetValueAtIndex(i, &ck)) || !ck) continue;
                            LPWSTR nm = nullptr, vl = nullptr;
                            std::wstring a, b;
                            if (SUCCEEDED(ck->get_Name(&nm)) && nm) { a = nm; CoTaskMemFree(nm); }
                            if (SUCCEEDED(ck->get_Value(&vl)) && vl) { b = vl; CoTaskMemFree(vl); }
                            if (!a.empty()) {
                                if (!cookieHdr.empty()) cookieHdr += L"; ";
                                cookieHdr += a + L"=" + b;
                            }
                        }
                }
                // the fetch itself lives on a worker: WinHttp, the store download's
                // own road, carrying the cookies and the referrer
                std::thread([tid, url, alt, ua, cookieHdr, main]() {
                    bool ok = false;
                    std::wstring path;
                    wchar_t host[256] = L"", pth[2048] = L"", extra[2048] = L"";
                    URL_COMPONENTS uc{};
                    uc.dwStructSize = sizeof uc;
                    uc.lpszHostName = host; uc.dwHostNameLength = 256;
                    uc.lpszUrlPath = pth; uc.dwUrlPathLength = 2048;
                    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
                    if (WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
                        HINTERNET s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
                        if (s) {
                            WinHttpSetTimeouts(s, 10000, 10000, 30000, 120000);
                            HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
                            if (c) {
                                std::wstring full = std::wstring(pth) + extra;
                                HINTERNET rq = WinHttpOpenRequest(c, L"GET", full.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
                                if (rq) {
                                    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
                                    WinHttpSetOption(rq, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof redir);
                                    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
                                    WinHttpSetOption(rq, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof dec);
                                    std::wstring hdrs = L"Accept: application/pdf,application/octet-stream,*/*;q=0.8\r\n";
                                    hdrs += L"User-Agent: " + ua + L"\r\n";
                                    if (_wcsnicmp(alt.c_str(), L"https://", 8) == 0)
                                        hdrs += L"Referer: " + alt + L"\r\n";   // the page that offered the link
                                    if (!cookieHdr.empty()) hdrs += L"Cookie: " + cookieHdr + L"\r\n";
                                    if (WinHttpSendRequest(rq, hdrs.c_str(), (DWORD)hdrs.size(),
                                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                                        && WinHttpReceiveResponse(rq, nullptr)) {
                                        DWORD code = 0, sz = sizeof code;
                                        WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                                                            WINHTTP_NO_HEADER_INDEX);
                                        if (code >= 200 && code < 300) {
                                            path = WebPdfTempPath(WebPdfSafeName(url));
                                            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                                            if (f != INVALID_HANDLE_VALUE) {
                                                ok = true;
                                                for (;;) {
                                                    DWORD avail = 0;
                                                    if (!WinHttpQueryDataAvailable(rq, &avail)) { ok = false; break; }
                                                    if (!avail) break;
                                                    std::vector<unsigned char> chunk(avail);
                                                    DWORD got = 0;
                                                    if (!WinHttpReadData(rq, chunk.data(), avail, &got)) { ok = false; break; }
                                                    DWORD wr = 0;
                                                    if (got && (!WriteFile(f, chunk.data(), got, &wr, nullptr) || wr != got)) { ok = false; break; }
                                                }
                                                CloseHandle(f);
                                                if (!ok) DeleteFileW(path.c_str());
                                            }
                                        }
                                    }
                                    WinHttpCloseHandle(rq);
                                }
                                WinHttpCloseHandle(c);
                            }
                            WinHttpCloseHandle(s);
                        }
                    }
                    bool pdf = ok && !path.empty() && WebHasPdfHeaderFile(path);
                    if (ok && !pdf) DeleteFileW(path.c_str());
                    if (main) PostMessageW(main, WM_APP + 14, (WPARAM)tid,
                                           (LPARAM)(pdf ? new std::wstring(path) : (std::wstring*)nullptr));
                    else if (pdf) DeleteFileW(path.c_str());
                }).detach();
                return S_OK;
            }).Get());
    if (FAILED(hr)) t->pdfFetching.clear();            // a refused query frees the slot
}

// The net under the capture road: when the response event's own bytes never
// arrive - a cache-served answer, a refused capture, a main document the event
// never spoke for - the credentialed fetch takes the road BY ITSELF. It is the
// helper button's exact walk (the session's cookies, the engine's own agent,
// the offering page as referrer), just without waiting for a button press, and
// the guard holds the view blank until the endgame lands the document in the
// reader or the honest blocked word comes home. One flight per address, like
// every other road - the fetch's own dedup keeps a second caller out.
static void WebPdfEscalate(WebTab* t, int ix) {
    if (!t || ix < 0 || ix >= (int)g_wt.size()) return;
    if (!g_webOn || g_sync) { WebPdfGuardOff(t, ix); return; }
    std::wstring u = t->url;
    if (u.empty()) { WebPdfGuardOff(t, ix); return; }
    if (t->pdfFetching == u) return;                   // a flight is already out: its
                                                       // endgame owns the view from here
    WxLogf(L"pdf: the capture came up empty - the credentialed fetch takes the road: %s", u.c_str());
    WebPdfFetch(u, t->pdfReferrer.empty() ? u : t->pdfReferrer, ix);
    g_webNote = L"Fetching the PDF for the reader..."; // the word rides until the endgame
    g_webNoteUntil = GetTickCount64() + 150000;        // rewrites it - the fetch's own
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);   // timeouts bound the wait
}

static void WebSessionSave();                      // the session's deck write; the
                                                   // definition rides behind WebNewTab,
                                                   // the NavigationCompleted bridge
                                                   // below calls it on every landing
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
                {   // the built-in viewer's toolbar is pinned off: a document that still
                    // reaches the viewer is the hand-off's failure, not a promised
                    // surface - the reader is the only PDF face this app shows
                    wrl::ComPtr<ICoreWebView2Settings> stg;
                    if (SUCCEEDED(t->web->get_Settings(&stg)) && stg) {
                        wrl::ComPtr<ICoreWebView2Settings7> stg7;
                        if (SUCCEEDED(stg->QueryInterface(IID_PPV_ARGS(&stg7))) && stg7)
                            stg7->put_HiddenPdfToolbarItems(COREWEBVIEW2_PDF_TOOLBAR_ITEMS_NONE);
                    }
                }
                t->web->AddScriptToExecuteOnDocumentCreated(kWxStoreScript, nullptr);   // the store's "Add to Docsy"
                WxEnsurePdfHelper();               // the bundled pdf helper rides once per session:
                                                   // the "open in docsy" button is the profile's own
                {   EventRegistrationToken tkm;
                    t->web->add_WebMessageReceived(
                        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [id](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                LPWSTR msg = nullptr;
                                if (SUCCEEDED(a->TryGetWebMessageAsString(&msg)) && msg) {
                                    std::wstring m = msg; CoTaskMemFree(msg);
                                    if (m.find(L"docsy-open-pdf") != std::wstring::npos) {
                                        // the bundled helper's word: the floating button hands
                                        // the document's address over (the embedded one first,
                                        // the page's own as alt) and the credentialed fetch
                                        // takes it from here - any page may ask, the reader
                                        // renders the verdict itself
                                        std::wstring url = WebJsonField(m, L"url");
                                        std::wstring alt = WebJsonField(m, L"alt");
                                        if (!url.empty()) WebPdfFetch(url, alt, WebTabIdx(id));
                                        return S_OK;
                                    }
                                    LPWSTR src = nullptr; std::wstring from;
                                    if (SUCCEEDED(a->get_Source(&src)) && src) { from = src; CoTaskMemFree(src); }
                                    if (!WxStoreHost(from, nullptr)) return S_OK;      // only the store may ask
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
                                [id](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* a) -> HRESULT {
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
                                            return S_OK;
                                        }
                                    }
                                    // the pdf download road (Avalanche's OnDownloadStarting):
                                    // a document the engine delivers as a download streams into
                                    // the temp area under our handling - no Edge flyout for a
                                    // file the reader is about to eat - and a completed %PDF-
                                    // file hands itself to the reader, while a refused one is
                                    // deleted and the page stands back up
                                    {
                                        LPWSTR dm = nullptr; std::wstring mime;
                                        if (SUCCEEDED(op->get_MimeType(&dm)) && dm) { mime = dm; CoTaskMemFree(dm); }
                                        LPWSTR rp = nullptr; std::wstring suggested;
                                        if (SUCCEEDED(a->get_ResultFilePath(&rp)) && rp) { suggested = rp; CoTaskMemFree(rp); }
                                        LPWSTR du = nullptr; std::wstring dlUrl;
                                        if (SUCCEEDED(op->get_Uri(&du)) && du) { dlUrl = du; CoTaskMemFree(du); }
                                        bool isPdf = _wcsicmp(mime.c_str(), L"application/pdf") == 0
                                                  || _wcsicmp(mime.c_str(), L"application/x-pdf") == 0
                                                  || ((_wcsicmp(mime.c_str(), L"application/octet-stream") == 0
                                                       || _wcsicmp(mime.c_str(), L"application/binary") == 0
                                                       || _wcsicmp(mime.c_str(), L"application/force-download") == 0
                                                       || _wcsicmp(mime.c_str(), L"application/download") == 0)
                                                      && ((suggested.size() >= 4
                                                           && _wcsicmp(suggested.c_str() + suggested.size() - 4, L".pdf") == 0)
                                                          || WebLooksLikePdf(dlUrl)));
                                        if (isPdf) {
                                            WxLogf(L"pdf: the download road takes it - %s (%s)",
                                                   dlUrl.empty() ? suggested.c_str() : dlUrl.c_str(), mime.c_str());
                                            std::wstring target = WebPdfTempPath(
                                                WebPdfSafeName(suggested.empty() ? std::wstring(L"download.pdf") : suggested));
                                            a->put_Handled(TRUE);
                                            a->put_ResultFilePath(target.c_str());
                                            static EventRegistrationToken tkPdfDl;   // one download road
                                                                                     // at a time: the
                                            op->add_StateChanged(                    // registration
                                                Microsoft::WRL::Callback<ICoreWebView2StateChangedEventHandler>(   // overwrites,
                                                    [id, target](ICoreWebView2DownloadOperation* o,               // nothing unregisters
                                                                 IUnknown*) -> HRESULT {
                                                        COREWEBVIEW2_DOWNLOAD_STATE ds = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
                                                        if (o) o->get_State(&ds);   // the state rides the
                                                        if (ds == COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS) return S_OK;   // sender
                                                        // the endgame rides the UI thread: the header
                                                        // check, the hand-off, the stand-down
                                                        if (g.main) PostMessageW(g.main, WM_APP + 13, (WPARAM)id,
                                                            (LPARAM)(ds == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED
                                                                ? new std::wstring(target) : (std::wstring*)nullptr));
                                                        return S_OK;
                                                    }).Get(), &tkPdfDl);
                                            return S_OK;
                                        }
                                    }
                                    return S_OK;
                                }).Get(), &t->tkDl);
                    }
                }
                {   // the pdf capture road (Avalanche's WebResourceResponseReceived): the
                    // engine's own response event - the headers speak here first, and a
                    // document answer's bytes copy straight out of the response's own
                    // stream while Chromium is still writing them
                    wrl::ComPtr<ICoreWebView2_2> w2;
                    if (SUCCEEDED(t->web->QueryInterface(IID_PPV_ARGS(&w2))) && w2) {
                        w2->add_WebResourceResponseReceived(
                            Microsoft::WRL::Callback<ICoreWebView2WebResourceResponseReceivedEventHandler>(
                                [id](ICoreWebView2*, ICoreWebView2WebResourceResponseReceivedEventArgs* a) -> HRESULT {
                                    WebPdfResponseSeen(id, a);
                                    return S_OK;
                                }).Get(), &t->tkPdfR);
                    }
                }
                t->web->add_NavigationStarting(
                    Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [id](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                            WebTab* t = WebTabById(id);
                            if (!t) return S_OK;
                            LPWSTR nu = nullptr;
                            std::wstring navUrl;
                            if (SUCCEEDED(a->get_Uri(&nu)) && nu) {
                                navUrl = nu;
                                WxLogf(L"nav: start %s", nu);
                                CoTaskMemFree(nu);
                            }
                            // the early visual guard: a pdf-shaped address blanks the view a
                            // beat before the response headers speak, and any other fresh
                            // navigation stands a stale guard down - nothing is cancelled,
                            // the hand-off catches the document wherever it lands
                            t->pdfReferrer = t->url;       // the offering page: the fetch's
                                                           // referrer, before the url turns over
                            t->pdfSpokeHtml = false;       // a fresh navigation speaks anew
                            if (WebLooksLikePdf(navUrl)) {
                                WxLogf(L"pdf: guard up - %s", navUrl.c_str());
                                WebPdfGuardOn(t, WebTabIdx(id));
                            } else if (t->pdfGuard) WebPdfGuardOff(t, WebTabIdx(id));
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
                            if (navOk) {
                                WxLogf(L"nav: done %s", t->url.c_str());
                                WebSessionSave();    // the deck keeps the address the tab
                                                     // settled on: a relaunch reopens it
                                HistAdd(t->url, t->title);
                                std::wstring host = BmHostOf(t->url);
                                if (!host.empty()) EnsureHostFavicon(host);
                            } else {
                                COREWEBVIEW2_WEB_ERROR_STATUS es = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                if (a) a->get_WebErrorStatus(&es);
                                WxLogf(L"nav: failed %s (%s)", t->url.c_str(), WebErrName(es));
                            }
                            // the guard's stand-down, then the net's second strand: a
                            // navigation that landed pdf-shaped without a capture
                            // reading gets the credentialed fetch BY ITSELF - the
                            // event never spoke, or its bytes refused - and the
                            // fetch's endgame owns the view from there. An html
                            // verdict for the document itself (a challenge, a shell)
                            // stands the guard down: the page IS the answer.
                            int pIx = WebTabIdx(id);
                            if (navOk && !t->pdfCaptureBusy && !t->pdfSpokeHtml
                                && pIx == g_wtCur && g_webOn && !g_sync
                                && WebLooksLikePdf(t->url) && t->pdfFetching != t->url)
                                WebPdfEscalate(t, pIx);
                            else if (t->pdfGuard && !t->pdfCaptureBusy)
                                WebPdfGuardOff(t, pIx);
                            WebPlace();                                // a watchdog kick returns to the card
                            WxPushState(t->web.Get());                 // the store button learns what is installed
                            t->captureDue = GetTickCount64() + 1200;   // let the page settle, then
                            WebEnsurePump();                           // the rail gets its thumb
                            WebHistorySync(id);
                            if (id == (unsigned long long)g_wtCur) {   // the page just presented:
                                KillTimer(g.main, 4735);               // a pending switch beat
                                WebSideSync();                         // lands now, not a beat
                            }                                          // before the view's frame
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
                                std::wstring prev = t->url;
                                t->url = u; CoTaskMemFree(u);
                                if (!t->url.empty() && t->url != prev) {
                                    HistAdd(t->url, t->title);
                                    std::wstring host = BmHostOf(t->url);
                                    if (!host.empty()) EnsureHostFavicon(host);
                                }
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
                                HistUpdateTitle(t->url, t->title);
                            }
                            WebRepaintChrome();
                            if (g.side) InvalidateRect(g.side, nullptr, FALSE);
                            return S_OK;
                        }).Get(), &t->tkTitle);
                t->web->add_HistoryChanged(    // HistoryChanged rides the base interface
                    Microsoft::WRL::Callback<ICoreWebView2HistoryChangedEventHandler>(
                        [id](ICoreWebView2* s, IUnknown*) -> HRESULT {
                            WebTab* t = WebTabById(id);
                            if (t && s) {
                                LPWSTR u = nullptr;
                                if (SUCCEEDED(s->get_Source(&u)) && u) {
                                    std::wstring curUrl = u; CoTaskMemFree(u);
                                    if (!curUrl.empty() && curUrl != t->url) {
                                        t->url = curUrl;
                                        HistAdd(t->url, t->title);
                                        std::wstring host = BmHostOf(t->url);
                                        if (!host.empty()) EnsureHostFavicon(host);
                                    }
                                }
                            }
                            WebHistorySync(id);
                            return S_OK;
                        }).Get(), &t->tkHist);
                t->web->add_NewWindowRequested(
                    Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                            if (!a) return S_OK;
                            a->put_Handled(TRUE);
                            LPWSTR u = nullptr;
                            if (SUCCEEDED(a->get_Uri(&u)) && u) {
                                std::wstring s = u; CoTaskMemFree(u);
                                if (!s.empty() && s != L"about:blank") {
                                    WebNewTab(s, true);
                                }
                            }
                            return S_OK;
                        }).Get(), &t->tkNewWin);
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
                if (g_editOn) EditorEnsureInit();
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

// ---- the browser's session: the tabs and their addresses ride settings.json ----
// Every tab opening, closing, switch and committed navigation writes the deck
// (capped at 20 tabs) plus the front tab's index, and a launch that comes back
// in browser mode reopens the same pages on the same tab - the session the
// user closed is the session they meet. The restore path flags itself so the
// tab births don't each rewrite the deck they are mid-writing.
static std::string WxJsonStr(const std::wstring& w);   // the JSON string literal; its
                                                       // definition rides ai_core.inl,
                                                       // below this module
static bool g_webRestoring = false;
static void WebSessionSave() {
    if (g_sync || g_webRestoring || g_wt.empty()) return;
    std::string j = "[";
    for (size_t i = 0; i < g_wt.size() && i < 20; i++) {
        if (i) j += ",";
        j += WxJsonStr(g_wt[i].url);
    }
    j += "]";
    AiSetSetting("web.session.urls", j);
    AiSetSetting("web.session.cur", std::to_string(g_wtCur < 0 ? 0 : g_wtCur));
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
    SumTabStand(1);                  // the deck's seat moved to the fresh tab: a new
                                     // tab never stood, so a standing card folds
                                     // with the move, like any tab switch
    WebSessionSave();                // the deck remembers the tab the moment it exists
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
    SumTabStand(1);                  // the neighbor took the seat without a
                                     // WebSwitchTo - the card answers to it,
                                     // open or folded, like any other deck move
    WebSessionSave();                // the closed tab is gone from the deck the moment it dies
}

static void WebSwitchTo(int i) {
    if (i < 0 || i >= (int)g_wt.size() || i == g_wtCur) return;
    if (g_wtCur >= 0 && g_wtCur < (int)g_wt.size())      // the outgoing tab's thumb refreshes
        g_wt[g_wtCur].captureDue = GetTickCount64() + 250;   // AFTER the beat: CapturePreview
                                                             // against a tab that just went
                                                             // hidden forces a render burst that
                                                             // starved the incoming tab's first
                                                             // present - the rail swapped while
                                                             // the view froze, one more shape of
                                                             // the switch lag (WebToggle's parting
                                                             // thumb already waits 250ms for the
                                                             // same reason)
    g_wtCur = i;
    WebShowCurrent();
    WebBarLayout();
    WebSwitchSyncSoon();
    WebRepaintChrome();
    SumTabStand(1);     // SUM_WEB: the deck moved - the card shows the new
                        // tab's own stand, open or folded, not the old one's
    AiModeChanged();                             // the sidechat follows the focused tab
    WebSessionSave();                            // the front tab's seat rides the deck too
}

// the word plate's Search road: every lookup rides ONE dedicated tab - the
// same tab all the time - and the browser itself takes the stage to show it.
// The tab is found by its id and rebuilt when closed; its query pages never
// enter the session deck (the restore must not resurrect a search the plate
// made, not the user).
static unsigned long long g_lookupTab = 0;
static void WebToggle(bool on);                    // the stage's own switch, defined below
                                                   // with the mode's other machinery
static void WebWordLookup(const std::wstring& word) {
    std::wstring q = word;
    for (auto& c : q) if (c == L' ') c = L'+';   // Google's own query shape
    std::wstring url = L"https://www.google.com/search?q=" + q;
    int hit = -1;
    if (g_lookupTab)
        for (int i = 0; i < (int)g_wt.size(); i++)
            if (g_wt[i].id == g_lookupTab) { hit = i; break; }
    if (hit < 0) {                               // the lookup tab died with its last
        g_webRestoring = true;                   // run: a new one wears the seat
        WebNewTab(url, false);
        g_webRestoring = false;
        hit = (int)g_wt.size() - 1;
        g_lookupTab = g_wt[hit].id;
    } else if (g_wt[hit].web) {
        g_wt[hit].web->Navigate(url.c_str());    // the same tab, a fresh query
        g_wt[hit].url = url;
    } else {
        g_wt[hit].pending = url;                 // the controller is still being born
        g_wt[hit].url = url;
    }
    g_wtCur = hit;                               // the seat lands before the stage shows
    if (!g_webOn) WebToggle(true);
    else { WebShowCurrent(); WebBarLayout(); WebSwitchSyncSoon(); WebRepaintChrome(); AiModeChanged(); }
    SumTabStand(1);                  // the lookup tab owns the seat now: the word-search
                                     // road already folds the issuing card, and this
                                     // tab never stood - the fold stays, like a switch
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
            POINT po{ 0, 0 };
            if (WindowFromDC(dc) == g.view) {
                MapWindowPoints(g.view, g.main, &po, 1);
                SetBrushOrgEx(bdc, -po.x, -po.y, nullptr);
            } else {
                SetBrushOrgEx(bdc, g_grainPX, g_grainPY, nullptr);
            }
            RECT fr{ 0, 0, w, h };
            FillRect(bdc, &fr, GrainBrush(0));
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
    if (!g_webNote.empty()) {                      // the pdf roads' word: one dark chip
        int nh = (int)(34 * g.dpi);                // near the card's foot, gone in 3s
        SetBkMode(dc, TRANSPARENT);
        HFONT of = (HFONT)SelectObject(dc, CaptionFont(g_tbcap, 14, false, L"Killer Sans"));
        RECT mc{ 0, 0, 0, 0 };
        DrawTextW(dc, g_webNote.c_str(), -1, &mc, DT_CALCRECT | DT_SINGLELINE);
        int nw = (int)(mc.right - mc.left) + (int)(28 * g.dpi);
        int ny = h - nh - (int)(18 * g.dpi);
        RECT nr{ max(0, (w - nw) / 2), max(0, ny), max(0, (w - nw) / 2) + nw, max(0, ny) + nh };
        {
            Gdiplus::Graphics gs(dc);
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath p;
            float rr = (float)(8 * g.dpi);
            float fx = (float)nr.left, fy = (float)nr.top;
            float fw = (float)(nr.right - nr.left), fh = (float)(nr.bottom - nr.top);
            p.AddArc(fx, fy, 2 * rr, 2 * rr, 180, 90);
            p.AddArc(fx + fw - 2 * rr, fy, 2 * rr, 2 * rr, 270, 90);
            p.AddArc(fx + fw - 2 * rr, fy + fh - 2 * rr, 2 * rr, 2 * rr, 0, 90);
            p.AddArc(fx, fy + fh - 2 * rr, 2 * rr, 2 * rr, 90, 90);
            p.CloseFigure();
            Gdiplus::SolidBrush fill(Gdiplus::Color(235, 0x1c, 0x1c, 0x1c));
            Gdiplus::Pen edge(Gdiplus::Color(255, 0x3a, 0x3a, 0x3a), 1.0f);
            gs.FillPath(&fill, &p);
            gs.DrawPath(&edge, &p);
        }
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        RECT tr{ nr.left + (int)(14 * g.dpi), nr.top, nr.right - (int)(14 * g.dpi), nr.bottom };
        DrawTextW(dc, g_webNote.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }
}

static void WebRoundWebView() {
    static bool once = false;                      // the clear is idempotent - nothing ever
    if (once || !g.view) return;                   // sets a region on the engine's children,
    once = true;                                   // so re-running it on every layout pass
    HWND hw = nullptr;                             // only force-invalidated their whole
    while ((hw = FindWindowExW(g.view, hw, nullptr, nullptr)) != nullptr) {   // surface and
        WCHAR cls[32]{};                           // flashed the page mid-move - the rail's
        if (!GetClassNameW(hw, cls, 32) || wcsncmp(cls, L"Chrome_WidgetWin", 16) != 0) continue;   // open read as tear
        SetWindowRgn(hw, nullptr, TRUE);
    }
}

static void WebPlace() {
    if (!g.view) return;
    RECT c; WebCardRect(c, g.cw, g.ch);
    if (c.right <= c.left || c.bottom <= c.top) return;
    for (auto& t : g_wt)
        if (t.ctr) t.ctr->put_Bounds(c);
    WebRoundWebView();
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
    if (on && g_editOn) EditorToggle(false);
    g_webOn = on;
    // (the panel rebind fires BELOW, after the web side actually exists: the
    // old call ran before the first tab was created, AiBindContext found no
    // g_wt[g_wtCur] and bailed - the flip to the browser left the PDF's
    // sidechat on screen and the reader had to close the panel by hand)
    if (g.side)                                    // the rail is the browser's own, closed until asked
        ShowWindow(g.side, on ? (g_webSide ? SW_SHOWNOACTIVATE : SW_HIDE) : (g.sideOn ? SW_SHOWNOACTIVATE : SW_HIDE));
    if (on) {
        WxLoadDisk();
        BmLoad();                                  // the list is home before the bar first paints
        if (g_wt.empty()) {
            // the session comes home: the same tab deck the last run wore, the
            // same front tab - a launch lands where the last one left off, not
            // on a fresh google page the user never asked for
            std::string urls = AiGetSetting("web.session.urls", "");
            crxk::J sj;
            std::vector<std::wstring> ru;
            if (!urls.empty() && crxk::JParse(urls, sj) && sj.t == 'a')
                for (auto& e : sj.a)
                    if (e.t == 's' && !e.s.empty()) ru.push_back(WxW(e.s));
            if (!ru.empty()) {
                g_webRestoring = true;
                for (auto& u : ru) WebNewTab(u, false);
                g_webRestoring = false;
                int c = max(0, min((int)ru.size() - 1,
                                   atoi(AiGetSetting("web.session.cur", "0").c_str())));
                if (c < (int)g_wt.size()) WebSwitchTo(c);
            } else {
                WebNewTab(L"https://www.google.com/", true);
            }
        }
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
        BmPopDestroy();                            // the star's bubble has no business over the reader
        g_bmHot = g_bmPress = -1;
    }
    AiModeChanged();                               // the panel rebinds: browser <-> reader -
                                                   // after the tab setup above, so the first
                                                   // flip into web (no tabs yet) finds a tab
    LayoutMain();                                  // the strip band appears or collapses
    WebBarLayout();
    WebRepaintChrome();
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    RailRepaint();
    UpdateTitle();
    // the session remembers the view: a launch with no file restores this exact face
    AiSetSetting("app.view", on ? "web" : (g_editOn ? "editor" : "pdf"));
    SumSearchReveal();   // the stage landed: a card a word-search folded pops back
    SumStageSync();      // the face flip: each seat parks when its stage leaves and
                         // comes back when the stage returns - the reader's card
                         // folds over the browser, the browser's card over the reader
}

static void WebShutdown() {
    WebSessionSave();                  // the deck rides out with the window, even on a hard exit
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

static int WebSideTH(int /*i*/) {
    return max(24, (int)std::lround(ws.tw * 0.625));
}

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
    // (no whole-rail invalidate here: this runs on every layout pass and on
    // every thumb arrival, and a full repaint each time is what strobed the
    // rail while captures landed - the callers that change geometry repaint,
    // and WebThumbDecode below repaints just the card that arrived)
}

static RECT WebSideCardRect(int i, int h) {        // a card's band in rail client coords:
    if (i < 0 || i >= (int)g_wt.size() || i >= (int)ws.top.size())
        return RECT{ 0, 0, 0, 0 };                 // thumb + label + pads, the same band the
                                                   // hover paints. ws.top exists only once the
                                                   // rail laid out (WebSideLayout, ws.cw > 0) -
                                                   // the index guard rides ws.top's own size, or
                                                   // a card rect for a never-laid-out rail reads
                                                   // out of bounds and takes the app down
    int padT = (int)(8 * g.dpi), labH = (int)(20 * g.dpi),
        padB = (int)(6 * g.dpi), gap = (int)(2 * g.dpi);
    int th = WebSideTH(i);
    int btop = ws.top[i] - padT - ws.scroll, bbot = ws.top[i] + th + gap + labH + padB - ws.scroll;
    if (bbot <= 0 || btop >= h) return RECT{ 0, 0, 0, 0 };
    int avail = ws.cw - SBar();
    return RECT{ 0, max(0, btop), max(1, avail), min(h, bbot) };
}

static void WebSideSync() {                        // the highlight follows the visible tab
    if (!g.side || g_wt.empty()) { ws.cur = -1; return; }
    if (g_wtCur == ws.cur) return;
    ws.cur = g_wtCur;
    if (g_wtCur >= 0 && g_wtCur < (int)ws.top.size() && ws.docH > 0) {   // the index rides
        int y0 = ws.top[g_wtCur] - (int)(8 * g.dpi);               // ws.top's OWN size: a tab
        int y1 = ws.top[g_wtCur] + WebSideTH(g_wtCur) + (int)(28 * g.dpi);   // count that
        if (y1 > ws.scroll + ws.ch) WebSideScrollTo(y0 - (int)(4 * g.dpi));   // outgrew the
        else if (y0 < ws.scroll) WebSideScrollTo(y1 - ws.ch + (int)(4 * g.dpi));   // last layout
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
    for (int i = 0; i < (int)g_wt.size() && i < (int)ws.top.size(); i++)
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
    if (g_webOn) {                                 // the card's left halo lands here, over the
        DrawRailShadow(dc, w, h, 0);               // thumbs the way the reader's rail receives
        RECT divLine{ w - 1, 0, w, h };            // divider border on the sidebar edge only
        HBRUSH divBr = CreateSolidBrush(RGB(0x48, 0x48, 0x48));
        FillRect(dc, &divLine, divBr);
        DeleteObject(divBr);
    }
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
        RECT sr; GetClientRect(h, &sr);
        RECT ro = WebSideCardRect(ws.hover, sr.bottom);   // the old band sheds its hover
        int barPrev = ws.barHot;                          // row and the new one gains it:
        ws.hover = hov; ws.barHot = hot;                  // both repaint, never the whole
        if (hov >= 0 || hot) TrackLeave(h);               // rail on every cursor crossing
        RECT rn = WebSideCardRect(hov, sr.bottom);
        if (ro.right > ro.left) InvalidateRect(h, &ro, FALSE);
        if (rn.right > rn.left && !EqualRect(&ro, &rn)) InvalidateRect(h, &rn, FALSE);
        if (hot != barPrev) {
            RECT br{ sr.right - SBar(), 0, sr.right, sr.bottom };
            InvalidateRect(h, &br, FALSE);
        }
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
    if (ctrl && wp == 'H') { HistFullOpen(); return true; }
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
    std::wstring t;
    if (g_editOn) {                      // the editor's own line: the one Letter sheet,
                                         // the page zoom - or a held message riding over
        if (GetTickCount64() < g.statusHoldUntil) t = g.statusHoldText;
        else {
            wchar_t eb[64];
            swprintf_s(eb, L"Page 1 of 1, %d%%", (int)std::lround(g_edZoom * 100));
            t = eb;
        }
    }
    else t = g_webOn ? WebStatusLine() : StatusTextNow();
    if (LatinText(t.c_str())) StText(dc, tr, t.c_str(), DT_LEFT | DT_END_ELLIPSIS,
                                     StFont(), 16 * g.dpi, StCapK, RGB(0xe0, 0xe0, 0xe0));
    else {                               // a held file name with script the subset drops:
        SelectObject(dc, StFontF());     // Segoe UI fallback, cell-centered as before
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        DrawTextW(dc, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    DrawVersion(dc);
    if (g_editOn) {                        // the text editor's footer: the browser's own
        auto div = [&](int ax, int bx) {   // cell set - status, moon, fullscreen, zoom -
            int mx = (ax + bx) / 2;        // plus the Letter sheet's fixed size cell
            RECT d{ mx, (SBH() - (int)(14 * dpi)) / 2, mx + 1, (SBH() + (int)(14 * dpi)) / 2 };
            FillRect(dc, &d, DividerBrush());
        };
        if (g_stR[ST_SIZE].right) div(g_stR[ST_FULL].left, g_stR[ST_SIZE].right);
        div(g_stR[ST_ZOUT].left, g_stR[ST_INV].right);
        RECT sr = g_stR[ST_SIZE];
        StText(dc, sr, L"Letter  8.5 x 11 in", DT_CENTER | DT_END_ELLIPSIS, StFont(), 16 * dpi, StCapK,
               g.stHot == ST_SIZE ? RGB(0x22, 0xbb, 0x56) : RGB(0xe0, 0xe0, 0xe0));
        DrawGlyph(dc, ST_FULL, L"\uE740", false);
        DrawGlyph(dc, ST_INV, L"\uE708", g_edInvert);
        DrawGlyph(dc, ST_ZOUT, L"\uECC9", false);
        DrawGlyph(dc, ST_ZIN, L"\uECC8", false);
        RECT cb = g_stR[ST_COMBO];
        FillRect(dc, &cb, ComboFillBrush());
        FrameRect(dc, &cb, ComboBorderBrush());
        wchar_t zl[32];
        swprintf_s(zl, L"%d%%", (int)std::lround(g_edZoom * 100));
        RECT ct = cb; ct.left += (int)(6 * dpi); ct.right -= (int)(16 * dpi);
        StText(dc, ct, zl, DT_LEFT | DT_END_ELLIPSIS, StCombo(), 10 * g.dpi, StCapU, RGB(0xe0, 0xe0, 0xe0));
        RECT chv{ cb.right - (int)(14 * dpi), cb.top, cb.right, cb.bottom };
        SelectObject(dc, StChevron());
        SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
        DrawTextW(dc, L"\uE70D", -1, &chv, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
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
    int n = (g_webOn || g_editOn) ? 6 : 8;         // the web page and the editor have no fit modes
    for (int i = 0; i < n; i++) {
        RECT it{ 1, 1 + i * g_zoomItemH, r.right - 1, 1 + (i + 1) * g_zoomItemH };
        if (i == g.zoomHot) FillRect(dc, &it, PopHoverBrush());
        RECT tx = it; tx.left += (int)(8 * g.dpi);
        StText(dc, tx, ZoomLabel((g_webOn || g_editOn) ? i + 2 : i), DT_LEFT, StCombo(), 10 * g.dpi, StCapU,
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
    if (!g.status || (!g_editOn && !g_webOn && !DVF().n)) return;
    RECT cr; GetClientRect(g.status, &cr);
    StatusRects(cr.right);
    RECT cb = g_stR[ST_COMBO];
    POINT tl{ cb.left, cb.bottom }, br{ cb.right, cb.bottom };
    ClientToScreen(g.status, &tl); ClientToScreen(g.status, &br);
    g_zoomComboScr = { tl.x, tl.y, br.x, br.y };
    g_zoomItemH = (int)(22 * g.dpi);
    g_zoomPopW = max((int)(cb.right - cb.left), (int)(96 * g.dpi));
    int popH = ((g_webOn || g_editOn) ? 6 : 8) * g_zoomItemH + 2;
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
        int last = (g_webOn || g_editOn) ? 5 : 7;
        int it = (p.y - 1) / max(1, g_zoomItemH);
        if (it < 0 || it > last || p.x < 0 || p.x >= g_zoomPopW) it = -1;
        if (it != g.zoomHot) { g.zoomHot = it; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int last = (g_webOn || g_editOn) ? 5 : 7;
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
        int last = (g_webOn || g_editOn) ? 5 : 7;
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
        if (DVF().n || g_editOn) {
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

