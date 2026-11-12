// ============================================================
//  The tab strip - KillerPDF's band (PdfViewer.TabStrip.cs + PdfViewer.xaml)
// ============================================================
// The band is a layout row at the top of the pane's own pass: the window's
// grainy field shows through where no tab paints, the active tab carries the
// menu tier (#141414) and joins the card below it, the inactive tabs are
// transparent with a 1px #555 divider apiece, and the card's tucked top
// border is the hairline at the band's foot. Equal cells between a 160px
// floor and a 260px ceiling, the count capped at the floor, the overflow a
// chevron that lists the collapsed tabs, and the "+" that opens a blank
// document - every number below is KillerPDF's, at the window's DPI.

static const int TAB_FLOOR = 160, TAB_CEIL = 260;   // TabFloorWidth / TabCeilingWidth
static const int TAB_BTN = 22, TAB_BTN_GAP = 4;     // the + and chevron cells (26px pitch)
static int TABF(int v) { return (int)(v * g.dpi); }

static HFONT TabFont() {                 // the GDI emergency face: TabTitleFontSize 12,
    static HFONT f = nullptr; static double fd = 0;   // KillerPDF's MonoFont family list is
    if (f && fd != g.dpi) { DeleteObject(f); f = nullptr; }   // "Killer Sans UI, Segoe UI" -
    if (!f) f = CaptionFont(f, 12, false, L"Segoe UI");       // Segoe UI is its own fallback
    fd = g.dpi;
    return f;
}

// The tab label, drawn with GDI+ so the face renders like KillerPDF's WPF text:
// GDI's rasterizer grid-snaps the bundled subsets' unhinted 12px stems into fat
// blur, which the reader read as broken. The collection carries the UI face
// ALONE (a shared collection pairs it with the slanted brand face). The Font's
// argument order is the SDK header's own, not the docs': Font(family, emSize,
// STYLE, UNIT) - builds 97/98 passed UnitPixel(2) as the third argument and the
// rasterizer obeyed it as FontStyleItalic, shearing a perfectly upright face
// into a synthetic oblique at the Point default's 16px. Style and unit are now
// named explicitly and in the header's order: Regular, pixel-sized at 12*dpi.
static Gdiplus::PrivateFontCollection* TabFaces() {
    static Gdiplus::PrivateFontCollection* pc = nullptr;
    if (!pc) {
        pc = new Gdiplus::PrivateFontCollection();
        HRSRC hs = FindResourceW(g.hinst, MAKEINTRESOURCEW(202), (LPCWSTR)RT_RCDATA);
        HGLOBAL hg = hs ? LoadResource(g.hinst, hs) : nullptr;
        const void* data = hg ? LockResource(hg) : nullptr;
        DWORD len = hs ? SizeofResource(g.hinst, hs) : 0;
        if (data && len) pc->AddMemoryFont(data, len);   // KillerSansUI-sub.ttf
    }
    return pc;
}

// The label: the dirty bullet TabTitle prefixes gets its own ink - brand green
// #22bb56, the tab's unsaved-changes dot - and the title after it draws in the
// label's own color. GDI+ measures the bullet's advance so the title never
// overlaps it; the GDI fallback measures the same way with GetTextExtentPoint32.
static void TabLabel(HDC dc, const std::wstring& s, const RECT& r, COLORREF c, bool center = false) {
    bool dot = s.size() >= 2 && s[0] == L'\x2022' && s[1] == L' ';
    const wchar_t* txt = dot ? s.c_str() + 2 : s.c_str();   // past the bullet and its space
    Gdiplus::PrivateFontCollection* pc = TabFaces();
    Gdiplus::FontFamily fam;                        // the collection's only family
    INT got = 0; pc->GetFamilies(1, &fam, &got);
    Gdiplus::Font f(&fam, (float)(12 * g.dpi), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    if (!got || fam.GetLastStatus() != Gdiplus::Ok || f.GetLastStatus() != Gdiplus::Ok) {
        SetBkMode(dc, TRANSPARENT);                 // no face: GDI's Segoe UI
        HFONT gf = TabFont();
        HGDIOBJ of = SelectObject(dc, gf);
        RECT tr = r;
        if (dot) {                                  // the green bullet, then the title
            SIZE bs; GetTextExtentPoint32W(dc, L"\x2022", 1, &bs);
            RECT br{ r.left, r.top, r.left + bs.cx + TABF(6), r.bottom };
            SetTextColor(dc, RGB(0x22, 0xbb, 0x56));
            DrawTextW(dc, L"\x2022", -1, &br, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            tr.left = br.right;
        }
        SetTextColor(dc, c);
        DrawTextW(dc, txt, -1, &tr, (center ? DT_CENTER : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);
        return;
    }
    Gdiplus::Graphics gs(dc);
    gs.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    Gdiplus::StringFormat sf;
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoFitBlackBox);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    if (center) sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    RECT tr = r;
    if (dot) {                                      // the green bullet, then the title
        Gdiplus::StringFormat mf(Gdiplus::StringFormat::GenericTypographic());
        mf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF rf((float)r.left, (float)r.top,
                          (float)(r.right - r.left), (float)(r.bottom - r.top));
        Gdiplus::RectF bb;
        gs.MeasureString(L"\x2022", 1, &f, rf, &mf, &bb);
        Gdiplus::SolidBrush gb(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56));
        gs.DrawString(L"\x2022", 1, &f, rf, &mf, &gb);
        tr.left += (int)std::ceil(bb.Width) + TABF(4);
    }
    Gdiplus::RectF rf2((float)tr.left, (float)tr.top,
                       (float)(tr.right - tr.left), (float)(tr.bottom - tr.top));
    gs.DrawString(txt, -1, &f, rf2, &sf, &br);
}

// The tab's title: the file name without its extension (KillerPDF's Title),
// with the dirty dot KillerPDF's RefreshTabLabel prefixes. The front slot
// reads the live state - the only place its dirtiness is current.
static std::wstring TabTitle(bool b, int idx) {
    const DocState& s = (idx == Act(b)) ? DV(b) : Tabs(b)[idx];
    const std::wstring& nm = s.name;
    std::wstring t = nm;
    size_t dot = nm.find_last_of(L'.');
    size_t sl = nm.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (sl == std::wstring::npos || dot > sl) && dot > 0) t = nm.substr(0, dot);
    return DocDirty(s) ? L"\x2022 " + t : t;   // IsDirty ? "\u2022 " + Title : Title
}

// ApplyTabWindow, at docsy's own band: which tabs fit at this width, the
// contiguous window that shifts the least to keep the active tab on screen,
// and whether the chevron shows. `start`/`cap` are VISIBLE slots; the caller
// maps them to sessions.
struct TabStripGeo {
    int x0, cell, start, cap, hostW;    // the strip's left edge, the equal cell width,
    bool chev;                          // the window's first slot, its count, the chevron
    int faceTop, faceH, actH;           // the faces' geometry (inactive 29, active 30)
};
static void TabStripLayout(bool b, TabStripGeo& G) {
    int cardL = PaneX0(b) + CardX(b), cardR = cardL + CardW(b);
    G.faceTop = TABF(3); G.faceH = TABF(29); G.actH = TABF(30);
    G.start = 0; G.cap = 0; G.chev = false; G.hostW = 0; G.cell = 0; G.x0 = cardL;
    // visible sessions: the doc tabs, in strip order (a blank start slot is
    // not a tab - UniformGrid ignores a collapsed child, so docsy skips it)
    std::vector<int> vis;
    for (int i = 0; i < (int)Tabs(b).size(); i++)
        if ((i == Act(b) ? DV(b).n : Tabs(b)[i].n) > 0) vis.push_back(i);
    int n = (int)vis.size();
    if (n == 0) return;
    int btnR = cardR - TABF(2);                       // the + hugs the card's right inset
    int plusL = btnR - TABF(TAB_BTN);
    int avail = plusL - TABF(TAB_BTN_GAP) - cardL;    // the strip measures against the
                                                      // band's remaining width
    int cap = avail / TABF(TAB_FLOOR);
    bool overflow = cap < n;
    if (overflow) {
        cap = max(1, (avail - TABF(TAB_BTN)) / TABF(TAB_FLOOR));
        if (cap >= n) overflow = false;
    }
    G.chev = overflow;
    int stripAvail = max(0, avail - (overflow ? TABF(TAB_BTN) : 0));
    int visCount = overflow ? cap : n;
    G.hostW = max(TABF(TAB_FLOOR), min(stripAvail, visCount * TABF(TAB_CEIL)));
    G.cap = visCount;
    G.cell = G.hostW / max(1, visCount);
    if (overflow) {
        int start = std::clamp(g.tabWin[b], 0, n - cap);
        int actVis = -1;                              // the active tab's visible slot
        for (int k = 0; k < n; k++)
            if (vis[k] == Act(b)) { actVis = k; break; }
        if (actVis >= 0) {
            if (actVis < start) start = actVis;
            else if (actVis > start + cap - 1) start = actVis - cap + 1;
        }
        g.tabWin[b] = start;
        G.start = start;
    } else {
        g.tabWin[b] = 0;
        G.cap = n;
    }
}

// The one geometry both the paint and the hit test share: a VISIBLE index's
// face rect (k counts through the full visible list; the window's first slot
// sits at the strip's left edge).
static void TabSlotRect(const TabStripGeo& G, int k, RECT& r) {
    int x = G.x0 + (k - G.start) * G.cell;
    r = { x, G.faceTop, x + G.cell, G.faceTop + G.faceH };
}

// The strip's hit test, the paint's own geometry: 0 field, 1 tab, 2 close x,
// 3 the +, 4 the chevron. `vis` maps a visible slot back to its session.
static int TabBandHitAt(bool b, const TabStripGeo& G, const std::vector<int>& vis, POINT p, int& slot) {
    slot = -1;
    if (!BandShown(b)) return 0;
    if (p.y < 0 || p.y >= BANDH()) return 0;
    int cardL = PaneX0(b) + CardX(b), cardR = cardL + CardW(b);
    if (p.x < cardL || p.x >= cardR) return 0;
    int btnR = cardR - TABF(2);
    int plusL = btnR - TABF(TAB_BTN);
    if (p.x >= plusL && p.x < btnR && p.y >= TABF(4) && p.y < TABF(24)) return 3;
    if (G.chev) {
        int chevL = plusL - TABF(TAB_BTN_GAP) - TABF(TAB_BTN);
        if (p.x >= chevL && p.x < chevL + TABF(TAB_BTN) && p.y >= TABF(4) && p.y < TABF(24)) return 4;
    }
    for (int k = G.start; k < G.start + G.cap && k < (int)vis.size(); k++) {
        RECT r; TabSlotRect(G, k, r);
        if (p.x >= r.left && p.x < r.right && p.y >= G.faceTop && p.y < G.faceTop + G.faceH) {
            slot = k;
            int xr = r.right - TABF(2);               // the close x owns its own 24px zone -
            if (p.x >= xr - TABF(24) && p.x < xr && p.y >= G.faceTop + TABF(2) && p.y < G.faceTop + G.faceH - TABF(2))
                return 2;                             // the active tab's included: its x closes it
            return 1;
        }
    }
    return 0;
}

static void TabBandInvalidate(bool b) {   // the band's own strip, nothing else
    if (!g.view || !BandShown(b)) return;
    RECT r{ PaneX0(b), 0, PaneX0(b) + PaneW(b), BANDH() };
    InvalidateRect(g.view, &r, FALSE);
}

// The band's paint. Draw order is KillerPDF's z-order: the field, the foot
// fade, then the tabs - the active tab last among its neighbors so its face is
// never overpainted - then the + and the chevron. The card's own ring lands at
// the band's foot (the tucked top border CardBackdrop bakes), so the band
// paints no hairline of its own and stops a row short of it.
static void TabBandPaint(HDC dc, bool b) {
    if (!BandShown(b)) return;
    TabEnsure(b);
    TabStripGeo G; TabStripLayout(b, G);
    std::vector<int> vis;
    for (int i = 0; i < (int)Tabs(b).size(); i++)
        if ((i == Act(b) ? DV(b).n : Tabs(b)[i].n) > 0) vis.push_back(i);
    int cardL = PaneX0(b) + CardX(b), cardR = cardL + CardW(b);
    int BH = BANDH();
    // the field: the band paints no fill of its own - the window's grainy
    // background shows through, exactly KillerPDF's transparent strip
    int bx0 = PaneX0(b), bw0 = PaneW(b);
    RECT bandR{ bx0, 0, bx0 + bw0, BH - 1 };   // the field stops at the band's foot: the card's
    FillRect(dc, &bandR, GrainBrush(1));       // tucked ring owns the foot's last row
    // the foot fade (TabStripFade): transparent to 45%, #59000000 at the foot,
    // the whole gradient at BarShadowOpacity 0.38
    {
        Gdiplus::Graphics gfade(dc);
        Gdiplus::Color c0((BYTE)0, (BYTE)0, (BYTE)0, (BYTE)0);
        Gdiplus::Color c1((BYTE)0x22, (BYTE)0, (BYTE)0, (BYTE)0);
        Gdiplus::RectF fadeR((float)bx0, (float)BH * 0.45f, (float)bw0, (float)BH * 0.55f);
        Gdiplus::LinearGradientBrush fadeBr(fadeR, c0, c1, Gdiplus::LinearGradientModeVertical);
        gfade.FillRectangle(&fadeBr, fadeR);
    }
    // the active tab's visible slot, for the paint order and the drag ghost
    int actVis = -1;
    for (int k = 0; k < (int)vis.size(); k++) if (vis[k] == Act(b)) { actVis = k; break; }
    int r3 = (int)(3 * g.dpi);
    // the tabs. Painted inactive-first, active last; a grabbed tab sits above
    // everything, glued to the cursor. The active tab wears no fill - the
    // field shows through it like through an inactive face, and the only
    // accent it carries is a thin green rule along its top edge, the mark of
    // the tab the reader is in.
    int hotK = (g.tbTabHotPane == b) ? g.tbTabHot : -1;
    int kEnd = min(G.start + G.cap, (int)vis.size());
    auto glideDx = [&](int k) -> int {           // a swapping neighbor or a released
        if (g.tbGlideIdx == k && g.tbGlidePane == b && g.tbGlideT0) {   // tab easing home
            double t = min(1.0, (GetTickCount64() - g.tbGlideT0) / 140.0);
            if (t >= 1) { g.tbGlideIdx = -1; return 0; }
            return (int)std::lround(g.tbGlideDX * (1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t)));
        }
        if (g.tbSettleIdx == k && g.tbSettlePane == b && g.tbSettleT0) {
            double t = min(1.0, (GetTickCount64() - g.tbSettleT0) / 120.0);
            if (t >= 1) { g.tbSettleIdx = -1; return 0; }
            return (int)std::lround(g.tbSettleDX * (1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t)));
        }
        return 0;
    };
    auto drawCloseX = [&](int xr, int cy, bool xhot) {
        HPEN xp = CreatePen(PS_SOLID, max(1, (int)std::lround(1.2 * g.dpi)),
                            xhot ? RGB(0xE0, 0x44, 0x44) : RGB(0xa3, 0xa3, 0xa3));
        HGDIOBJ op = SelectObject(dc, xp);
        int g2 = TABF(4);                        // the 8px cross inside the 24px zone
        MoveToEx(dc, xr - g2, cy - g2, nullptr); LineTo(dc, xr + g2, cy + g2);
        MoveToEx(dc, xr + g2, cy - g2, nullptr); LineTo(dc, xr - g2, cy + g2);
        SelectObject(dc, op);
        DeleteObject(xp);
    };
    for (int k = G.start; k < kEnd; k++) {          // the inactive faces first
        int si = vis[k];
        if (si == Act(b)) continue;                 // the active tab draws below, last
        if (g.tbDragging && g.tbDragPane == b && k == g.tbDragIdx) continue;   // glued: the ghost
        RECT r; TabSlotRect(G, k, r);
        int dx = glideDx(k);
        int fl = r.left + dx, fr = r.right + dx;
        // the divider: an inactive tab's 1px right border, dropped on the
        // strip's last slot while the chevron is hidden (IsLast)
        bool isLastVis = k == kEnd - 1;
        if (!G.chev && isLastVis) { /* the band's edge: no stray rule */ }
        else {
            Gdiplus::Graphics gs(dc);
            Gdiplus::SolidBrush db(Gdiplus::Color((BYTE)255, 0x55, 0x55, 0x55));
            gs.FillRectangle(&db, Gdiplus::RectF((float)fr - 1.f, (float)G.faceTop, 1.f, (float)G.faceH));
        }
        // the label, dirty dot included, ellipsized before the close x
        RECT tr{ fl + TABF(10), G.faceTop, fr - TABF(24) - TABF(6), G.faceTop + G.faceH };
        TabLabel(dc, TabTitle(b, si), tr, RGB(0xa3, 0xa3, 0xa3));
        // the close x: muted, danger red under the cursor (TabCloseButton)
        int xr = fr - TABF(2) - TABF(12), cy = G.faceTop + G.faceH / 2;
        drawCloseX(xr, cy, hotK == k && g.tbHotBtn == 1);
    }
    if (actVis >= G.start && actVis < kEnd &&
        !(g.tbDragging && g.tbDragPane == b && actVis == g.tbDragIdx)) {
        // the ACTIVE tab: no face of its own - the field shows through and
        // the brighter label carries the emphasis, KillerPDF's TextBrush
        // over MutedText - and a thin green rule (PrimaryBrush #22bb56)
        // runs along the top edge, the stripe the reader asked for
        RECT r; TabSlotRect(G, actVis, r);
        int dx = glideDx(actVis);
        int fl = r.left + dx, fr = r.right + dx;
        int ft = G.faceTop, fb2 = ft + G.actH;  // the label keeps the flush geometry
        Gdiplus::Graphics gs(dc);
        Gdiplus::SolidBrush acc(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56));
        gs.FillRectangle(&acc, Gdiplus::RectF((float)fl, (float)ft,
                                              (float)(fr - fl), (float)max(1, TABF(1))));
        // the label and the x on the active tab
        RECT tr{ fl + TABF(10), ft, fr - TABF(24) - TABF(6), fb2 };
        TabLabel(dc, TabTitle(b, vis[actVis]), tr, RGB(0xe0, 0xe0, 0xe0));
        int xr = fr - TABF(2) - TABF(12), cy = ft + G.faceH / 2;
        drawCloseX(xr, cy, hotK == actVis && g.tbHotBtn == 1);
    }
    if (g.tbDragging && g.tbDragPane == b && g.tbDragIdx >= G.start && g.tbDragIdx < kEnd) {
        // the grabbed tab rides the cursor, clamped into its strip (Tab_DragMove)
        RECT r; TabSlotRect(G, g.tbDragIdx, r);
        int rel = std::clamp(g.tbDragX - g.tbDragGrab - G.x0, 0, max(0, G.hostW - G.cell));
        int fl = G.x0 + rel, fr = fl + (r.right - r.left);
        bool active = vis[g.tbDragIdx] == Act(b);
        int ft = G.faceTop, fb2 = ft + (active ? G.actH : G.faceH);
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath face;
        float rr = (float)r3;
        face.AddArc((float)fl, (float)ft, 2 * rr, 2 * rr, 180, 90);
        face.AddArc((float)fr - 2 * rr, (float)ft, 2 * rr, 2 * rr, 270, 90);
        face.AddLine((float)fr, (float)ft + rr, (float)fr, (float)fb2);
        face.AddLine((float)fr, (float)fb2, (float)fl, (float)fb2);
        face.CloseFigure();
        Gdiplus::SolidBrush faceBr(Gdiplus::Color((BYTE)255, 0x14, 0x14, 0x14));
        gs.FillPath(&faceBr, &face);
        RECT tr{ fl + TABF(10), ft, fr - TABF(24) - TABF(6), fb2 };
        TabLabel(dc, TabTitle(b, vis[g.tbDragIdx]), tr, RGB(0xe0, 0xe0, 0xe0));
        int xr = fr - TABF(2) - TABF(12), cy = ft + G.faceH / 2;
        drawCloseX(xr, cy, false);
    }
    // the "+" (NewTabBtn): a new blank document in this pane, RowHover under
    // the cursor
    int btnR = cardR - TABF(2);
    int plusL = btnR - TABF(TAB_BTN);
    bool plusHot = g.tbTabHotPane == b && g.tbHotBtn == 2;
    if (plusHot) {
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush hb(Gdiplus::Color((BYTE)255, 0x40, 0x40, 0x40));   // RowHoverBrush
        Gdiplus::GraphicsPath hp;
        float hr = (float)TABF(3);
        hp.AddArc((float)plusL, (float)TABF(4), 2 * hr, 2 * hr, 180, 90);
        hp.AddArc((float)plusL + TABF(TAB_BTN) - 2 * hr, (float)TABF(4), 2 * hr, 2 * hr, 270, 90);
        hp.AddArc((float)plusL + TABF(TAB_BTN) - 2 * hr, (float)TABF(24) - 2 * hr, 2 * hr, 2 * hr, 0, 90);
        hp.AddArc((float)plusL, (float)TABF(24) - 2 * hr, 2 * hr, 2 * hr, 90, 90);
        hp.CloseFigure();
        gs.FillPath(&hb, &hp);
    }
    {
        int cx = plusL + TABF(TAB_BTN) / 2, cy = (TABF(4) + TABF(24)) / 2;
        int g2 = TABF(5);                        // the 11px plus glyph
        HPEN xp = CreatePen(PS_SOLID, 1, plusHot ? RGB(0xe0, 0xe0, 0xe0) : RGB(0xa3, 0xa3, 0xa3));
        HGDIOBJ op = SelectObject(dc, xp);
        MoveToEx(dc, cx - g2, cy, nullptr); LineTo(dc, cx + g2, cy);
        MoveToEx(dc, cx, cy - g2, nullptr); LineTo(dc, cx, cy + g2);
        SelectObject(dc, op);
        DeleteObject(xp);
    }
    // the overflow chevron, only while tabs are collapsed out of the strip
    if (G.chev) {
        int chevL = plusL - TABF(TAB_BTN_GAP) - TABF(TAB_BTN);
        bool chevHot = g.tbTabHotPane == b && g.tbHotBtn == 3;
        if (chevHot) {
            Gdiplus::Graphics gs(dc);
            Gdiplus::SolidBrush hb(Gdiplus::Color((BYTE)255, 0x40, 0x40, 0x40));
            gs.FillRectangle(&hb, Gdiplus::RectF((float)chevL, (float)TABF(4),
                                                 (float)TABF(TAB_BTN), (float)TABF(20)));
        }
        int cx = chevL + TABF(TAB_BTN) / 2, cy = (TABF(4) + TABF(24)) / 2;
        int g2 = TABF(4);
        HPEN xp = CreatePen(PS_SOLID, 1, chevHot ? RGB(0xe0, 0xe0, 0xe0) : RGB(0xa3, 0xa3, 0xa3));
        HGDIOBJ op = SelectObject(dc, xp);
        MoveToEx(dc, cx - g2, cy - g2 / 2, nullptr);   // ChevronDown E70D
        LineTo(dc, cx, cy + g2 / 2 + (g2 - g2 / 2));
        LineTo(dc, cx + g2, cy - g2 / 2);
        SelectObject(dc, op);
        DeleteObject(xp);
    }
}

// The drag's window-level feedback (Shell/PaneDrag.cs): over the OTHER pane
// the real tab cannot follow the hand, so a translucent ghost takes over and
// a 2px accent caret marks the insertion slot. The ghost is the dragged tab's
// own face - its title from the source pane, its width from the source cell.
static void TabDragPaint(HDC dc) {
    if (!g.tbDragging || g.tbGhostPane < 0) return;
    bool from = g.tbDragPane != 0;
    bool b = g.tbGhostPane != 0;
    TabStripGeo Gs; TabStripLayout(from, Gs);       // the dragged face's own width
    std::vector<int> visFrom;
    for (int i = 0; i < (int)Tabs(from).size(); i++)
        if ((i == Act(from) ? DV(from).n : Tabs(from)[i].n) > 0) visFrom.push_back(i);
    if (g.tbDragIdx < 0 || g.tbDragIdx >= (int)visFrom.size()) return;
    int wsi = visFrom[g.tbDragIdx];
    std::wstring title = TabTitle(from, wsi);
    int w = std::clamp(Gs.cell, TABF(TAB_FLOOR), TABF(TAB_CEIL));
    if (!BandShown(b)) return;
    TabStripGeo G; TabStripLayout(b, G);
    int gl = g.tbDragX - g.tbDragGrab, gr = gl + w;
    if (gr < PaneX0(b) || gl > PaneX0(b) + PaneW(b)) return;   // not over this pane
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath face;
    float rr = (float)TABF(3);
    face.AddArc((float)gl, (float)Gs.faceTop, 2 * rr, 2 * rr, 180, 90);
    face.AddArc((float)gr - 2 * rr, (float)Gs.faceTop, 2 * rr, 2 * rr, 270, 90);
    face.AddLine((float)gr, (float)Gs.faceTop + rr, (float)gr, (float)Gs.faceTop + Gs.actH);
    face.AddLine((float)gr, (float)Gs.faceTop + Gs.actH, (float)gl, (float)Gs.faceTop + Gs.actH);
    face.CloseFigure();
    Gdiplus::SolidBrush fb(Gdiplus::Color((BYTE)210, 0x14, 0x14, 0x14));   // TabDragGhost, Opacity .82
    gs.FillPath(&fb, &face);
    RECT tr{ gl + TABF(10), Gs.faceTop, gr - TABF(24), Gs.faceTop + Gs.faceH };
    TabLabel(dc, title, tr, RGB(0xe0, 0xe0, 0xe0));
    // the caret: 2px rule at the insertion slot's leading edge - neutral chrome
    // gray, the strip carries no accent of its own any more
    if (g.tbCaret >= 0) {
        int cx = G.x0 + g.tbCaret * G.cell;
        cx = std::clamp(cx, G.x0, G.x0 + G.hostW);
        Gdiplus::SolidBrush cb(Gdiplus::Color((BYTE)255, 0x8a, 0x8a, 0x8a));
        gs.FillRectangle(&cb, Gdiplus::RectF((float)cx, (float)G.faceTop, 2.f, (float)G.faceH));
    }
}

// ---- the strip's pointer gestures (Tab_MouseDown / Tab_DragDown/Move/Up) ----

struct TabPopCtx { int kind = -1; bool pane = 0; int tab = 0; std::vector<int> map; };
static TabPopCtx g_tabPop;
static void ListPopClose();   // defined with the list popup below

// The tab menu and the overflow chevron share the list popup; the rows carry
// their own action table here. KillerPDF's menu: Open Containing Folder,
// Close Tab, Close Others, Close All Tabs (the tear-out entry is the one
// docsy cannot offer - it is a one-window reader).
static void TabPopPick(int row) {
    if (g_tabPop.kind == 1) {                     // the chevron's hidden tabs
        if (row >= 0 && row < (int)g_tabPop.map.size()) SwitchTab(g_tabPop.pane, g_tabPop.map[row]);
        return;
    }
    if (g_tabPop.kind != 0 || row < 0) return;
    bool b = g_tabPop.pane;
    if (row == 0) {                               // Open Containing Folder
        int si = g_tabPop.tab;
        const std::wstring& p = (si == Act(b)) ? DV(b).path : Tabs(b)[si].path;
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            ShellExecuteW(g.main, L"open", L"explorer.exe",
                          (std::wstring(L"/select,\"") + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    } else if (row == 1) {
        CloseTabAt(b, g_tabPop.tab);
    } else if (row == 2) {
        CloseOtherTabs(b, g_tabPop.tab);
    } else if (row == 3) {
        CloseAllTabs(b);
    }
}

// A press on the strip: pending until the mouse-up, so a press can begin a
// drag without switching first (KillerPDF's left-click-switches-on-UP rule).
// A press on the + or the chevron arms those; the field focuses the pane.
static bool TabStripDown(HWND h, POINT p) {
    bool b = g.split ? (p.x >= g.splitX + SplitHalf()) : false;
    if (p.y >= BANDH() || !BandShown(b)) return false;
    TabEnsure(b);
    TabStripGeo G; TabStripLayout(b, G);
    std::vector<int> vis;
    for (int i = 0; i < (int)Tabs(b).size(); i++)
        if ((i == Act(b) ? DV(b).n : Tabs(b)[i].n) > 0) vis.push_back(i);
    int slot;
    int what = TabBandHitAt(b, G, vis, p, slot);
    FocusPane(b);                                 // a strip click claims the pane first
    if (what == 3) { g.tbDragPane = b; g.tbPending = -2; return true; }   // the +
    if (what == 4) { g.tbDragPane = b; g.tbPending = -3; return true; }   // the chevron
    if (what == 1 || what == 2) {
        g.tbPending = slot;                       // the visible slot, session resolved on UP
        g.tbPendingX = what == 2;
        g.tbDragPane = b;
        g.tbDragIdx = slot;
        g.tbDragGrab = p.x - (G.x0 + (slot - G.start) * G.cell);
        g.tbDragX = p.x;
        g.tbDragging = false;
        return true;
    }
    g.tbPending = -1;                             // the field: the focus was the whole action
    return true;
}

static bool TabStripMove(HWND h, POINT p) {
    // hover first (the +, the chevron, the tabs and their close x)
    bool b = g.split ? (p.x >= g.splitX + SplitHalf()) : false;
    int hot = -1, hotPane = b, hotBtn = 0;
    if (p.y < BANDH()) {
        bool hb = g.split ? (p.x >= g.splitX + SplitHalf()) : false;
        if (BandShown(hb)) {
            TabEnsure(hb);
            TabStripGeo G; TabStripLayout(hb, G);
            std::vector<int> vis;
            for (int i = 0; i < (int)Tabs(hb).size(); i++)
                if ((i == Act(hb) ? DV(hb).n : Tabs(hb)[i].n) > 0) vis.push_back(i);
            int slot;
            int what = TabBandHitAt(hb, G, vis, p, slot);
            if (what == 1 || what == 2) { hot = slot; hotBtn = what; hotPane = hb; }
            else if (what == 3) { hot = -1; hotBtn = 2; hotPane = hb; }
            else if (what == 4) { hot = -1; hotBtn = 3; hotPane = hb; }
        }
    }
    if (hot != g.tbTabHot || hotPane != g.tbTabHotPane || hotBtn != g.tbHotBtn) {
        bool wasPane = g.tbTabHotPane;               // the old and new faces repaint: a hover
        bool wasAny = g.tbTabHot >= 0 || g.tbHotBtn >= 2;   // flip is two invalidations
        bool isAny = hot >= 0 || hotBtn >= 2;
        g.tbTabHot = hot; g.tbTabHotPane = hotPane; g.tbHotBtn = hotBtn;
        if (isAny) TrackLeave(h);
        if (wasAny) TabBandInvalidate(wasPane);
        if (isAny) TabBandInvalidate(hotPane);
    }
    // the drag physics: past the system's 4px drag threshold the grabbed tab
    // glues to the cursor and swaps when its advancing edge crosses a
    // neighbor's layout-slot midpoint (edge against midpoint gives the
    // hysteresis, so a tab parked on a boundary does not bounce)
    if (g.tbPending >= 0 && !g.tbPendingX) {
        bool pb = g.tbDragPane;
        if (!g.tbDragging && abs(p.x - g.tbDragX) >= 4) g.tbDragging = true;   // tbDragX
        if (g.tbDragging) {                       // still holds the press point until now
            g.tbDragX = p.x;
            g.tbGhostPane = -1; g.tbCaret = -1;
            if (g.split && pb != PaneB(p.x)) {    // over the OTHER pane: the ghost has it
                g.tbGhostPane = PaneB(p.x);
                // the caret slot: the strip boundary the cursor sits before
                bool tb = g.tbGhostPane != 0;
                if (BandShown(tb)) {
                    TabStripGeo G2; TabStripLayout(tb, G2);
                    int cnt = TabDocCount(tb);
                    int rel = p.x - G2.x0;
                    g.tbCaret = std::clamp((rel + G2.cell / 2) / max(1, G2.cell), 0, cnt);
                } else g.tbCaret = TabDocCount(tb);
                TabBandInvalidate(pb);
                TabBandInvalidate(tb);
                return true;
            }
            // reorder: swap with a neighbor when an edge crosses its midpoint
            TabEnsure(pb);
            TabStripGeo G; TabStripLayout(pb, G);
            std::vector<int> vis;
            for (int i = 0; i < (int)Tabs(pb).size(); i++)
                if ((i == Act(pb) ? DV(pb).n : Tabs(pb)[i].n) > 0) vis.push_back(i);
            int cur = g.tbDragIdx;
            if (cur < 0 || cur >= (int)vis.size()) return true;
            int slide = G.cell;
            double rawLeft = p.x - g.tbDragGrab;
            double leftEdge = rawLeft, rightEdge = rawLeft + slide;
            auto midX = [&](int k) { return G.x0 + (k - G.start) * G.cell + G.cell / 2.0; };
            if (cur + 1 < (int)vis.size() && cur + G.start + 1 < G.start + G.cap &&
                rightEdge > midX(cur + 1)) {
                int a = vis[cur], c = vis[cur + 1];
                std::swap(Tabs(pb)[a], Tabs(pb)[c]);
                std::swap(g.tabUndo[pb][a], g.tabUndo[pb][c]);
                std::swap(g.tabRedo[pb][a], g.tabRedo[pb][c]);
                if (Act(pb) == a) Act(pb) = c; else if (Act(pb) == c) Act(pb) = a;
                g.tbGlidePane = pb; g.tbGlideIdx = cur; g.tbGlideDX = slide;    // it jumped left:
                g.tbGlideT0 = GetTickCount64();                 // from one cell right of its new slot
                g.tbDragIdx = cur + 1;
                SetTimer(h, 9, 16, nullptr);
            } else if (cur - 1 >= G.start && leftEdge < midX(cur - 1)) {
                int a = vis[cur], c = vis[cur - 1];
                std::swap(Tabs(pb)[a], Tabs(pb)[c]);
                std::swap(g.tabUndo[pb][a], g.tabUndo[pb][c]);
                std::swap(g.tabRedo[pb][a], g.tabRedo[pb][c]);
                if (Act(pb) == a) Act(pb) = c; else if (Act(pb) == c) Act(pb) = a;
                g.tbGlidePane = pb; g.tbGlideIdx = cur; g.tbGlideDX = -slide;   // it jumped right
                g.tbGlideT0 = GetTickCount64();
                g.tbDragIdx = cur - 1;
                SetTimer(h, 9, 16, nullptr);
            }
            TabBandInvalidate(pb);
            return true;
        }
    }
    return g.tbDragging;
}

static bool TabStripUp(HWND h, POINT p) {
    if (g.tbPending == -2) {                      // the +: NewDocument into this pane
        g.tbPending = -1;
        NewBlank();                               // the pane was focused at the press
        return true;
    }
    if (g.tbPending == -3) {                      // the chevron lists the collapsed tabs
        g.tbPending = -1;
        bool b = g.tbDragPane;
        TabStripGeo G; TabStripLayout(b, G);
        std::vector<int> vis;
        for (int i = 0; i < (int)Tabs(b).size(); i++)
            if ((i == Act(b) ? DV(b).n : Tabs(b)[i].n) > 0) vis.push_back(i);
        g_tabPop = TabPopCtx{ 1, b, -1, {} };
        std::vector<std::wstring> items;
        for (int k = 0; k < (int)vis.size(); k++) {
            if (k >= G.start && k < min(G.start + G.cap, (int)vis.size())) continue;
            g_tabPop.map.push_back(vis[k]);
            std::wstring t = TabTitle(b, vis[k]);
            if (vis[k] == Act(b)) t = L"\x2022 " + t;   // bold reads as a bullet here
            items.push_back(t);
        }
        if (!items.empty()) {
            int cardR = G.x0 + CardW(b);
            int chevL = cardR - TABF(2) - TABF(TAB_BTN) - TABF(TAB_BTN_GAP) - TABF(TAB_BTN);
            POINT pt{ chevL, BANDH() + 2 };
            ClientToScreen(h, &pt);
            ListPopOpen(g.main, pt, items, 2, -1);
        }
        return true;
    }
    if (g.tbPending >= 0) {
        int slot = g.tbPending;
        bool b = g.tbDragPane;
        bool wasX = g.tbPendingX;
        bool wasDragging = g.tbDragging;
        g.tbPending = -1; g.tbPendingX = false;
        if (!wasDragging) {
            TabEnsure(b);
            TabStripGeo G; TabStripLayout(b, G);
            std::vector<int> vis;
            for (int i = 0; i < (int)Tabs(b).size(); i++)
                if ((i == Act(b) ? DV(b).n : Tabs(b)[i].n) > 0) vis.push_back(i);
            if (slot < G.start || slot >= min(G.start + G.cap, (int)vis.size())) return true;
            int si = vis[slot];
            if (wasX) CloseTabAt(b, si);          // the close x answers on the release
            else SwitchTab(b, si);                // a plain click switches on UP
            return true;
        }
        // the drag ends. Over the other pane: a move, checked on RELEASE (the
        // capture would otherwise be pulled out from under the running drag)
        g.tbDragging = false;
        if (g.tbGhostPane >= 0) {
            bool to = g.tbGhostPane != 0;
            TabStripGeo G2; TabStripLayout(to, G2);
            std::vector<int> visTo;
            for (int i = 0; i < (int)Tabs(to).size(); i++)
                if ((i == Act(to) ? DV(to).n : Tabs(to)[i].n) > 0) visTo.push_back(i);
            int from = g.tbDragPane;
            std::vector<int> visFrom;
            for (int i = 0; i < (int)Tabs(from).size(); i++)
                if ((i == Act(from) ? DV(from).n : Tabs(from)[i].n) > 0) visFrom.push_back(i);
            int si = g.tbDragIdx >= 0 && g.tbDragIdx < (int)visFrom.size() ? visFrom[g.tbDragIdx] : -1;
            int at = g.tbCaret >= 0 && g.tbCaret <= (int)visTo.size()
                   ? (g.tbCaret < (int)visTo.size() ? visTo[g.tbCaret] : (int)Tabs(to).size())
                   : (int)Tabs(to).size();
            g.tbGhostPane = -1; g.tbCaret = -1;
            if (si >= 0) TabMoveToPane(from, si, to, at);
            return true;
        }
        // a reorder settles: the grabbed tab glides from its dragged position
        // into its final slot (the neighbors already swapped under it)
        TabStripGeo G; TabStripLayout(b, G);
        int home = G.x0 + (g.tbDragIdx - G.start) * G.cell;
        g.tbSettlePane = b; g.tbSettleIdx = g.tbDragIdx;
        g.tbSettleDX = (g.tbDragX - g.tbDragGrab) - home;
        g.tbSettleT0 = GetTickCount64();
        g.tbGlideIdx = -1;
        SetTimer(h, 9, 16, nullptr);
        TabBandInvalidate(b);
        return true;
    }
    return false;
}

static bool TabStripMid(POINT p) {                // middle-click closes, like KillerPDF's
    if (p.y >= BANDH()) return false;
    bool b = g.split ? (p.x >= g.splitX + SplitHalf()) : false;
    if (!BandShown(b)) return false;
    TabEnsure(b);
    TabStripGeo G; TabStripLayout(b, G);
    std::vector<int> vis;
    for (int i = 0; i < (int)Tabs(b).size(); i++)
        if ((i == Act(b) ? DV(b).n : Tabs(b)[i].n) > 0) vis.push_back(i);
    int slot;
    int what = TabBandHitAt(b, G, vis, p, slot);
    if (what == 1 || what == 2) { CloseTabAt(b, vis[slot]); return true; }
    return false;
}

static bool TabStripRClick(HWND h, POINT p) {     // the tab's context menu
    if (p.y >= BANDH()) return false;
    bool b = g.split ? (p.x >= g.splitX + SplitHalf()) : false;
    if (!BandShown(b)) return false;
    TabEnsure(b);
    TabStripGeo G; TabStripLayout(b, G);
    std::vector<int> vis;
    for (int i = 0; i < (int)Tabs(b).size(); i++)
        if ((i == Act(b) ? DV(b).n : Tabs(b)[i].n) > 0) vis.push_back(i);
    int slot;
    int what = TabBandHitAt(b, G, vis, p, slot);
    if (what != 1 && what != 2) return false;
    int si = vis[slot];
    g_tabPop = TabPopCtx{ 0, b, si, {} };
    std::vector<std::wstring> items{ L"Open Containing Folder", L"Close Tab",
                                     L"Close Other Tabs", L"Close All Tabs" };
    POINT pt{ p.x, BANDH() + 2 };
    ClientToScreen(h, &pt);
    ListPopOpen(g.main, pt, items, 2, -1);
    return true;
}

static void TabStripAnimTick(HWND h) {            // the glide/settle pump (timer 9)
    unsigned long long now = GetTickCount64();
    bool glide = g.tbGlideIdx >= 0 && g.tbGlideT0 && now - g.tbGlideT0 < 140;
    bool settle = g.tbSettleIdx >= 0 && g.tbSettleT0 && now - g.tbSettleT0 < 120;
    if (glide) TabBandInvalidate(g.tbGlidePane != 0);
    if (settle) TabBandInvalidate(g.tbSettlePane != 0);
    if (!glide && !settle) {
        if (g.tbGlideIdx >= 0) { g.tbGlideIdx = -1; TabBandInvalidate(g.tbGlidePane != 0); }
        if (g.tbSettleIdx >= 0) { g.tbSettleIdx = -1; TabBandInvalidate(g.tbSettlePane != 0); }
        KillTimer(h, 9);
        return;
    }
    SetTimer(h, 9, 16, nullptr);
}

static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:                        // the overlay bars anchor to the card's edges:
        InvalidateRect(h, nullptr, FALSE);   // a resize must repaint the whole view
        return 0;
    // WM_MOUSEACTIVATE keeps its default (MA_ACTIVATE): clicking the document
    // must raise and focus the window. MA_NOACTIVATE here was a leftover
    // anti-flash band-aid from the WS_CAPTION era - the style is gone, nothing
    // native can flash on activation any more, and the swallow only meant
    // clicks on the pages could not bring Docsy to the foreground.
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (g_webOn) {                       // the browser owns the card: the view paints the
            WebCardPaint(dc, g.cw, g.ch);    // shared field, the rounded card's face and
            EndPaint(h, &ps);                // ring; the controller HWND renders the page,
            return 0;                        // or the no-engine card when the runtime is missing
        }
        if (g.cw > 0 && g.ch > 0) {
            EnsureBack(dc, g.cw, g.ch);
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // the grain tile phase is the
            SetBrushOrgEx(g.bdc, -po.x, -po.y, nullptr);            // window's, so the field the
            g_grainPX = -po.x; g_grainPY = -po.y;   // corner patches phase their tiles to it
            if (g.split) {                          // two panes, two documents: each pass
                int wA = PaneW(false), xB = PaneX0(true), wB = PaneW(true);   // paints its own card,
                for (int pass = 0; pass < 2; pass++) {   // its own layout, its own scroll - the
                    bool drawB = g.splitA ? pass == 0 : pass == 1;   // focused pane draws LAST so
                    if (drawB) {                    // Evict/lastVis/prefetch follow the focus
                        RECT pr{ xB, 0, g.cw, g.ch };
                        HRGN rg = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
                        Draw(g.bdc, wB, g.ch, xB, g.dB, rg);
                        DeleteObject(rg);
                        PaintPaneBars(g.bdc, g.dB, true, g.dB.vHot || g.dB.vDrag, g.dB.hHot || g.dB.hDrag);
                        TabBandPaint(g.bdc, true);   // the pane's own tab band, over its card's top
                    } else {
                        RECT pr{ 0, 0, wA, g.ch };
                        HRGN rg = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
                        Draw(g.bdc, wA, g.ch, 0, g.d, rg);
                        DeleteObject(rg);
                        PaintPaneBars(g.bdc, g.d, false, g.d.vHot || g.d.vDrag, g.d.hHot || g.d.hDrag);
                        TabBandPaint(g.bdc, false);
                    }
                }
                RECT band{ g.splitX + 1, 0, min(g.cw, g.splitX + SplitHalf()), g.ch };
                FillRect(g.bdc, &band, GrainBrush(1));   // the gutter's own 3px: pane A's pass
                                                         // ends at the line, pane B's starts 4px
                                                         // past it - without this fill the strip
                                                         // between them never repaints and stale
                                                         // fragments pile up as ghosts on a drag
                RECT dl{ g.splitX, 2, g.splitX + 1, g.ch - 2 };   // the divider: one pixel of
                FillRect(g.bdc, &dl, SplitLineBrush(g.lineHot || g.splitDrag));   // chrome that turns
            } else {                                // green when it means "grab me"
                Draw(g.bdc, g.cw, g.ch, 0, g.d);                    // card floats on never shows a
                PaintViewBars(g.bdc, g.cw, g.ch, 0);                // seam at a pane boundary
                TabBandPaint(g.bdc, false);
            }
            TabDragPaint(g.bdc);                    // the cross-pane drag's ghost + caret
            if (g.brushOn) {                        // the Draw tool's brush ring: the ink
                DocState& DC = DV(PaneB(g.brushPt.x));   // color at the brush's own size,
                float k = (float)(DC.zoom * g.dpi);      // KillerPDF's ShowBrushPreview
                Gdiplus::Graphics gs(g.bdc);
                gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                float d = max(4.0f, 3.0f * k);
                Gdiplus::Pen pen(Gdiplus::Color((BYTE)255, 0xff, 0x00, 0x00), 1.0f * (float)g.dpi);
                gs.DrawEllipse(&pen, g.brushPt.x - d / 2, g.brushPt.y - d / 2, d, d);
            }
            if (g.tedit) {                          // the text box's ring + corner handles: the
                RECT er; GetWindowRect(g.tedit, &er);   // pixels the WS_CLIPCHILDREN punch-out
                ScreenToClient(h, (POINT*)&er);         // exposes, KillerPDF's PrimaryBrush border
                ScreenToClient(h, ((POINT*)&er) + 1);
                HPEN gp = CreatePen(PS_SOLID, 1, RGB(0x22, 0xbb, 0x56));
                HGDIOBJ og = SelectObject(g.bdc, gp);
                HGDIOBJ ob = SelectObject(g.bdc, GetStockObject(NULL_BRUSH));
                RECT er2{ er.left - 1, er.top - 1, er.right + 1, er.bottom + 1 };
                Rectangle(g.bdc, er2.left, er2.top, er2.right, er2.bottom);
                SelectObject(g.bdc, ob); SelectObject(g.bdc, og); DeleteObject(gp);
                {                                   // the editor's own 12px resize handles
                    int hs = (int)(12 * g.dpi);
                    HBRUSH hb = CreateSolidBrush(RGB(0x22, 0xbb, 0x56));
                    POINT hpt[4] = { { er.left, er.top }, { er.right, er.top },
                                     { er.right, er.bottom }, { er.left, er.bottom } };
                    for (POINT& hp : hpt) {
                        RECT hr{ hp.x - hs / 2, hp.y - hs / 2, hp.x - hs / 2 + hs, hp.y - hs / 2 + hs };
                        FillRect(g.bdc, &hr, hb);
                        FrameRect(g.bdc, &hr, (HBRUSH)GetStockObject(WHITE_BRUSH));
                    }
                    DeleteObject(hb);
                }
            }
            if (g.barOn) {                          // the floating settings bar, over its pixels
                RECT br2 = AnnBarRect();            // (the Find bar paints later, so it wins the
                AnnBarDraw(g.bdc, br2.left, br2.top);   // top-right corner when both are open)
            }
            if (fd.open) {                          // the Find bar is composite pixels, not a
                int bx, by; FdOrigin(bx, by);       // window: pages, highlights, overlay bars,
                FindDraw(g.bdc, bx, by);            // the bar and its shadow land in ONE buffer,
            }                                       // and the blit below is ONE atomic frame -
            BitBlt(dc, 0, 0, g.cw, g.ch, g.bdc, 0, 0, SRCCOPY);   // frame, no second surface to fall out of
            SetTimer(h, 1, 5, nullptr);             // sync with the edit (WS_CLIPCHILDREN
        }                                           // already guards the edit's own pixels)
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 9) { TabStripAnimTick(h); return 0; }   // the strip's glide/settle pump
        if (wp == 6) {                         // the full-screen hint's expiry repaint
            KillTimer(h, 6);
            if (GetTickCount64() >= g.hintUntil) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (wp == 7) {                         // the find bar's debounce fired
            KillTimer(h, 7);
            FindRun();
            return 0;
        }
        if (wp == 8) { FindPump(); return 0; }   // the find scan's pump tick
        if (wp == 4) {                         // deferred text pre-warm: OCR the settled
            KillTimer(h, 4);                   // page once it exists and scrolling has been
            DocState& D = DVF();               // quiet for a moment - off the click path;
            if (D.n && !D.selArm) {            // the FOCUSED pane's document is the one read
                if (GetTickCount64() - D.scrollTick > 250) {
                    int pg = PageAt(D, FocusSy() + g.ch / 3);
                    if (D.txt.find(pg) == D.txt.end()) TtFor(D, pg);
                } else {
                    SetTimer(h, 4, 160, nullptr);   // still moving: try again after it settles
                }
            }
            return 0;
        }
        KillTimer(h, 1); Prefetch(DVF()); return 0;
    case WM_MOUSEWHEEL: ViewWheel(wp, lp); return 0;
    case WM_APP + 2: RqDrain(); return 0;   // the render worker's finished pages and thumbs
    case WM_MOUSEHWHEEL: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(g.view, &p);
        bool b = PaneB(p.x);
        DocState& D = DV(b);
        ScrollPane(b, D.sx + GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 80) / 120, D.sy);
        return 0;
    }
    case WM_KEYDOWN:                         // keys live on the main window: forward, never
        SendMessageW(g.main, m, wp, lp);     // swallow - a stray focus here once ate Del and
        return 0;                            // every other shortcut until the next edit
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_webOn) { WebViewClick(p); return 0; }   // the fail card's download button, if any
        SetCapture(h);
        if (TabStripDown(h, p)) return 0;       // the strip owns its band first: a press
                                                // arms a switch/drag pending the release
        if (fd.open) {                           // the floating bar owns its pixels first: the
            int bx, by; FdOrigin(bx, by);        // grip drags, the ring focuses the edit, the
            POINT q{ p.x - bx, p.y - by };       // buttons act - a press on the bar never falls
            int ht = FdHit(q);                   // through to the page underneath, and a press
            if (ht == FB_GRIP) {                 // in the shadow margins does (WPF's DropShadow
                fd.drag = true; fd.dragX = p.x; fd.dragY = p.y;   // never extends hit testing)
                fd.hot = -1;
                return 0;
            }
            FdRects R; FdLayout(FdEditW(CardW(FocusB())), R);   // a press anywhere in the query box -
            if (q.x >= R.edit.left - 1 && q.x < R.edit.right + 1 &&   // the edit only owns its
                q.y >= R.edit.top - 1 && q.y < R.edit.bottom + 1) {   // line-box window - focuses it
                SetFocus(g_fedit);
                return 0;
            }
            if (ht == FB_CLOSE) { FindClose(); return 0; }
            else if (ht == FB_PREV) { FindStep(-1); return 0; }
            else if (ht == FB_NEXT) { FindStep(1); return 0; }
            else if (ht == FB_AA) { FindOption(true); return 0; }
            else if (ht == FB_AB) { FindOption(false); return 0; }
            if (FdOverBar(p)) return 0;          // the card's frame and padding: chrome, not a page press
        }
        if (g.barOn) {                           // the settings bar owns its pixels next: swatches,
            int cell = AnnBarHit(p);             // sliders, checks and buttons answer here, and a
            if (cell != AB_OUT) {                // press on the bar's padding never falls through
                AnnBarPress(h, p, cell);
                return 0;
            }
        }
        if (g.tedit) {                           // the editor's corner handles resize the box
            RECT er; GetWindowRect(g.tedit, &er);
            ScreenToClient(h, (POINT*)&er); ScreenToClient(h, ((POINT*)&er) + 1);
            int hs = (int)(12 * g.dpi);
            for (int hd = 0; hd < 4; hd++) {
                int hx = (hd == 0 || hd == 3) ? er.left : er.right;
                int hy = (hd < 2) ? er.top : er.bottom;
                if (p.x >= hx - hs / 2 && p.x < hx - hs / 2 + hs &&
                    p.y >= hy - hs / 2 && p.y < hy - hs / 2 + hs) {
                    g_teResize = true;
                    HideCaret(g.tedit);              // the caret sits the drag out (the frame
                                                     // paint owns the pixels until release)
                    g_teAnchor = POINT{ (hd == 0 || hd == 3) ? er.right : er.left,
                                        (hd < 2) ? er.bottom : er.top };
                    g_teStart = er; g_teHandle = hd;
                    return 0;
                }
            }
        }
        int gb = SBar();
        if (g.split) {                           // the split owns its chrome first: the divider
            if (abs(p.x - g.splitX) <= SplitHalf()) { g.splitDrag = true; return 0; }   // grabs,
            bool inB = PaneB(p.x);               // a click focuses the pane under it, and each
            FocusPane(inB);                      // pane's own overlay bars answer there
            DocState& D = DV(inB);
            bool vshow = D.n && D.docH > g.ch;
            int track = ViewW(D, inB);           // the content viewport: the h-bar answers to it
            bool hshow = D.n && D.docW > track;
            int gutH = hshow ? gb : 0;
            int cardR = PaneX0(inB) + CardX(inB) + CardW(inB);   // this pane's card's right edge
            int cardL = PaneX0(inB) + CardX(inB);
            if (vshow && p.x >= cardR - gb && p.x < cardR) { // the v-bar hugs the card's edge
                int ins = BandInset(inB);
                int tl, tp; BarThumb(D.docH, g.ch - ins - gutH, D.sy, tl, tp,
                                     D.docH - (g.ch - gutH)); tp += ins;   // under the band
                if (p.y >= tp && p.y < tp + tl) { D.vDrag = true; D.vGrab = p.y - tp; }   // short
                else ScrollPane(inB, D.sx, D.sy + (p.y < tp ? -(int)(g.ch * 0.9) : (int)(g.ch * 0.9)));
                return 0;
            }
            if (hshow && p.y >= g.ch - gb && p.x >= cardL && p.x < cardR - (vshow ? gb : 0)) {
                int tl, tp; BarThumb(D.docW, track, D.sx, tl, tp);
                if (p.x >= cardL + tp && p.x < cardL + tp + tl) { D.hDrag = true; D.hGrab = p.x - (cardL + tp); }
                else ScrollPane(inB, D.sx + (p.x < cardL + tp ? -(int)(track * 0.9) : (int)(track * 0.9)), D.sy);
                return 0;
            }
        } else {
        DocState& D = g.d;
        int cw = CardW(false);
        bool vshow = D.n && D.docH > g.ch;
        int track = cw - (vshow ? gb : 0);
        bool hshow = D.n && D.docW > track;
        int gutH = hshow ? gb : 0;
        if (vshow && p.x >= cw - gb && p.x < cw) {   // the vertical bar: inside the card's edge
            int ins = BandInset(false);
            int tl, tp; BarThumb(D.docH, g.ch - ins - gutH, D.sy, tl, tp,
                                 D.docH - (g.ch - gutH)); tp += ins;   // under the band
            if (p.y >= tp && p.y < tp + tl) { D.vDrag = true; D.vGrab = p.y - tp; }
            else ScrollTo(D.sx, D.sy + (p.y < tp ? -(int)(g.ch * 0.9) : (int)(g.ch * 0.9)));
            return 0;
        }
        if (hshow && p.y >= g.ch - gb && p.x < cw - (vshow ? gb : 0)) {
            int tl, tp; BarThumb(D.docW, track, D.sx, tl, tp);
            if (p.x >= tp && p.x < tp + tl) { D.hDrag = true; D.hGrab = p.x - tp; }
            else ScrollTo(D.sx + (p.x < tp ? -(int)(track * 0.9) : (int)(track * 0.9)), D.sy);
            return 0;
        }
        }
        // the tools: every press but Select's own routes here first (KillerPDF's
        // Canvas_MouseLeftButtonDown dispatch). A tool press also commits a live
        // text box first, exactly like its CommitActiveTextBox.
        bool pbT = PaneB(p.x);
        if (DV(pbT).tool != TOOL_SELECT) {
            CommitTextEdit();
            if (ToolDown(h, pbT, p)) return 0;
        }
        // flowing text selection: a press ON text starts KillerPDF's caret drag. A press
        // anywhere else starts nothing - the wheel and the keys own scrolling, so the
        // old grab-and-pan gesture (which fought the selection drag) is gone. The press
        // maps through the pane it landed in, so each pane selects in its own document.
        // With the Select tool an annotation under the press wins first: it is chosen,
        // armed for the drag, and the text path never sees the press.
        int pg; float fx, fy;
        bool pb = PaneB(p.x);
        DocState& D = DV(pb);
        if (D.tool == TOOL_SELECT && SelectAnnotAt(h, pb, p)) return 0;
        POINT q{ p.x - PaneX0(pb) - CardX(pb), p.y };    // the press in the pane's own card coords
        if (ViewToPageAt(D, q, CardW(pb), g.ch, D.sx, D.sy, pg, fx, fy)) {
            TxtPage* pt = TtFor(D, pg);   // builds once if the pre-warm has not reached it yet
            if (pt && TtOverText(*pt, fx, fy)) {
                SelClear(D);
                D.selArm = true; D.selDrag = false;
                D.selAPage = D.selFPage = pg;
                D.selACaret = D.selFCaret = TtCaret(*pt, fx, fy);
                D.selDownPos = p;
                return 0;
            }
            if (D.selRange || D.selArm) SelClear(D);   // a plain click off text drops it
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (TabStripMove(h, p)) return 0;       // the strip's hover tiles + the drag physics
        if (fd.drag) {                           // the grip is moving the bar: the offsets update,
            RECT was = FdRect();                 // the edit rides along, and the frame lands in
            fd.dx += p.x - fd.dragX;             // the back buffer as one atomic blit. Only the
            fd.dy += p.y - fd.dragY;             // bar's own footprint changes - old ∪ new, and
            fd.dragX = p.x; fd.dragY = p.y;      // FindDraw paints exactly FdRect - so a full-view
            FindPlace();                         // recomposite (every cached page, every frame)
            RECT now = FdRect();                 // would be pure waste on a 2000-page book. The
            RECT u{ min(was.left, now.left), min(was.top, now.top),   // edit's own park inside the
                    max(was.right, now.right), max(was.bottom, now.bottom) };   // union is covered:
            InvalidateRect(h, &u, FALSE);        // FindPlace repaints the child, the union paints
            return 0;                            // the composite around it - no ghosts, no waste
        }
        if (fd.open) {                           // the bar's hover tiles, the same flip-only
            int bx, by; FdOrigin(bx, by);        // repaint the scrollbars use
            POINT q{ p.x - bx, p.y - by };
            int ht = FdHit(q);
            if (ht != fd.hot) {
                RECT r = FdRect();
                fd.hot = ht;
                if (ht >= 0) TrackLeave(h);
                InvalidateRect(h, &r, FALSE);
            }
            if (ht >= 0 || FdOverBar(p)) return 0;   // over the chrome: no page hover work
        }
        if (g.barDrag) {                         // the grip slides the bar along the top:
            RECT was = AnnBarRect();             // only the bar's own footprint repaints
            g.barDx = g.barDx0 + (g.barDragX - p.x);
            RECT now = AnnBarRect();
            RECT u{ min(was.left, now.left), min(was.top, now.top),
                    max(was.right, now.right), max(was.bottom, now.bottom) };
            InvalidateRect(h, &u, FALSE);
            return 0;
        }
        if (g.barSlide >= 0) { AnnBarSlideValue(h, p); return 0; }
        if (g_teResize) {                        // the editor's box follows the dragged handle:
            int minW = (int)(40 * g.dpi), minH = (int)(24 * g.dpi);   // KillerPDF's min 40x24
            // KillerPDF's PdfViewer.Annotations live resize: the dragged
            // corner takes the pointer (clamped to the page card), the size is
            // the pointer's ABSOLUTE distance from the opposite-corner anchor
            // clamped to the minimum, and the box's origin derives from the
            // anchor MINUS that size on the dragged sides - so a left/top
            // handle grows the box leftward/upward with the anchor pinned,
            // and crossing the anchor flips the box to the other side without
            // ever producing an inverted rect.
            DocState& ED = DV(g.tePaneB);
            int cardL = PaneX0(g.tePaneB) + CardX(g.tePaneB), cardR = cardL + CardW(g.tePaneB);
            int qx = std::clamp((int)p.x, cardL, max(cardL, cardR));   // the pointer in the card
            int qy = std::clamp((int)p.y, 0, g.ch);
            int newW = max(minW, abs((int)(qx - g_teAnchor.x)));
            int newH = max(minH, abs((int)(qy - g_teAnchor.y)));
            int nx = (g_teHandle == 0 || g_teHandle == 3) ? g_teAnchor.x - newW : g_teAnchor.x;
            int ny = (g_teHandle == 0 || g_teHandle == 1) ? g_teAnchor.y - newH : g_teAnchor.y;
            if (nx != g_teStart.left || ny != g_teStart.top ||
                newW != g_teStart.right - g_teStart.left || newH != g_teStart.bottom - g_teStart.top) {
                RECT was{ g_teStart.left - 8, g_teStart.top - 8, g_teStart.right + 8, g_teStart.bottom + 8 };
                // FALSE + one explicit paint: the stock TRUE lets the edit's erase-then-text
                // strobe run at drag speed even under the WM_PAINT override - the synchronous
                // UpdateWindow lands the frame's single pass before the parent's band blit.
                MoveWindow(g.tedit, nx, ny, newW, newH, FALSE);
                InvalidateRect(g.tedit, nullptr, TRUE);
                UpdateWindow(g.tedit);
                g_teStart = RECT{ nx, ny, nx + newW, ny + newH };
                float k = (float)(ED.zoom * g.dpi);
                // the box's page-DIP origin and width ride the drag, so the
                // commit lands the annotation where the box was left
                int ox, oy;
                if (PageOrigin(ED, g.tePaneB, g.tePage, ox, oy)) {
                    g.teX = (nx - PaneX0(g.tePaneB) - ox) / max(0.01f, k);
                    g.teY = (ny - oy) / max(0.01f, k);
                }
                g.teW = max(24.0f, newW / max(0.01f, k));
                g_teFixedH = true;   // a handle drag makes the height free-form (KillerPDF's rule)
                RECT now{ nx - 8, ny - 8, nx + newW + 8, ny + newH + 8 };
                RECT u{ min(was.left, now.left), min(was.top, now.top),
                        max(was.right, now.right), max(was.bottom, now.bottom) };
                InvalidateRect(h, &u, FALSE);   // the ring + handles' own band, not the view
            }
            return 0;
        }
        if (g.barOn) {                           // the bar's hover tile, flip-only
            int cell = AnnBarHit(p);
            if (cell != g.barHot) { g.barHot = cell; AnnBarSync(true); }
            if (cell != AB_OUT) return 0;        // over the bar: no page hover work
        }
        if (g.splitDrag) {                       // the divider follows the pointer, clamped so
            int lo = PaneMin();                  // neither pane under KillerPDF's 320px minimum
            int hi = max(lo, g.cw - SplitHalf() - PaneMin());
            int nl = std::clamp((int)p.x, lo, hi);
            if (nl != g.splitX) {
                g.splitX = nl;
                // BOTH panes re-anchor to the line - pane B's origin is the
                // line's x, pane A's card ends at it - so every pixel of both
                // documents shifts with it. A band around the line alone would
                // leave the rest of each pane at its last frame: the page
                // reading twice, fragments stacked on fragments, one ghost per
                // drag frame. The frame composite is cached-bitmap blits (no
                // page decodes - GetBmp answers from the cache), so a full-view
                // invalidate per move is the cheap part; KillerPDF's own drag
                // relayouts both panes on every move the same way.
                RECT u{ 0, 0, g.cw, g.ch };
                InvalidateRect(h, &u, FALSE);
            }
            return 0;
        }
        if (g.dB.vDrag) {
            int tl, tp; BarThumb(g.dB.docH, g.ch, g.dB.sy, tl, tp);
            ScrollPane(true, g.dB.sx, (int)((long long)(p.y - g.dB.vGrab) * max(0, g.dB.docH - g.ch) / max(1, g.ch - tl)));
            return 0;
        }
        if (g.dB.hDrag) {
            int x0 = PaneX0(true) + CardX(true), x1 = x0 + CardW(true);
            int track = x1 - x0 - ((g.dB.n && g.dB.docH > g.ch) ? SBar() : 0);
            int tl, tp; BarThumb(g.dB.docW, track, g.dB.sx, tl, tp);
            // the grab was measured from the thumb's absolute x, so the map back
            // into scroll space subtracts the pane's track origin first
            ScrollPane(true, (int)((long long)(p.x - x0 - g.dB.hGrab) * max(0, g.dB.docW - CardW(true)) / max(1, track - tl)), g.dB.sy);
            return 0;
        }
        if (g.resizing) {
            int w = g.rsW0 + (p.x - g.rsX);
            if (w != g.sideW) { g.sideW = w; LayoutMain(); }   // thumbs rebuild once, on release
            return 0;
        }
        if (g.d.vDrag) {
            int tl, tp; BarThumb(g.d.docH, g.ch, g.d.sy, tl, tp);
            ScrollTo(g.d.sx, (int)((long long)(p.y - g.d.vGrab) * max(0, g.d.docH - g.ch) / max(1, g.ch - tl)));
            return 0;
        }
        if (g.d.hDrag) {
            int cw = CardW(false);
            int tl, tp; BarThumb(g.d.docW, cw - ((g.d.n && g.d.docH > g.ch) ? SBar() : 0), g.d.sx, tl, tp);
            ScrollTo((int)((long long)(p.x - g.d.hGrab) * max(0, g.d.docW - cw) / max(1, cw - (g.d.docH > g.ch ? SBar() : 0) - tl)), g.d.sy);
            return 0;
        }
        // the tools' live gestures: a stroke tracks its own footprint (old ∪ new
        // per frame, no full-view recomposite), and the Select tool's drag moves
        // the chosen annotation inside its page.
        if (g.annMove || g.annInk) { ToolMove(h, p); return 0; }
        // live selection drag: the focus caret tracks the pointer (cross-page, like
        // KillerPDF's Continuous mode); ViewToPage clamps into the page so dragging
        // past an edge parks the caret at the nearest line's start/end. The pane
        // whose press armed the drag owns it, whichever pane the pointer crosses.
        if (g.d.selArm || g.dB.selArm) {
            DocState& D = g.d.selArm ? g.d : g.dB;
            bool pb = &D == &g.dB;
            if (!D.selDrag) {
                if (abs(p.x - D.selDownPos.x) < (int)(4 * g.dpi) &&
                    abs(p.y - D.selDownPos.y) < (int)(4 * g.dpi)) return 0;   // still a click
                D.selDrag = true;
            }
            int pg; float fx, fy;
            POINT q{ p.x - PaneX0(pb) - CardX(pb), p.y };
            if (ViewToPageAt(D, q, CardW(pb), g.ch, D.sx, D.sy, pg, fx, fy)) {
                TxtPage* pt = TtFor(D, pg);
                if (pt) {
                    int caret = TtCaret(*pt, fx, fy);
                    if (pg != D.selFPage || caret != D.selFCaret) {
                        D.selFPage = pg; D.selFCaret = caret;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        // the Draw tool's brush ring follows the cursor while no gesture holds
        // the capture (KillerPDF's ShowBrushPreview): the ring's old ∪ new moves,
        // never the whole view
        if (!g.annInk && !g.annMove && !g.d.selArm && !g.dB.selArm &&
            !g.d.vDrag && !g.d.hDrag && !g.dB.vDrag && !g.dB.hDrag && !g.splitDrag && !g.resizing) {
            bool bNow = PaneB(p.x);
            DocState& DC = DV(bNow);
            bool ring = DC.n && DC.tool == TOOL_DRAW;
            if (ring != g.brushOn || (ring && (p.x != g.brushPt.x || p.y != g.brushPt.y))) {
                RECT u{ min(p.x, g.brushPt.x) - 12, min(p.y, g.brushPt.y) - 12,
                        max(p.x, g.brushPt.x) + 12, max(p.y, g.brushPt.y) + 12 };
                if (!g.brushOn) u = RECT{ p.x - 12, p.y - 12, p.x + 12, p.y + 12 };
                g.brushOn = ring; g.brushPt = p;
                InvalidateRect(h, &u, FALSE);
            }
        }
        if (g.split) {                           // the split's hover work: the divider's hot
            bool lineHot = abs(p.x - g.splitX) <= SplitHalf();   // flip, then each pane's bars
            if (lineHot != g.lineHot) {          // over their own tracks
                g.lineHot = lineHot;
                RECT u{ max(0, g.splitX - SplitHalf() - SBar()), 0,
                        min(g.cw, g.splitX + SplitHalf() + SBar() + 1), g.ch };
                InvalidateRect(h, &u, FALSE);
                if (lineHot) TrackLeave(h);
            }
            bool inB = PaneB(p.x);
            bool vhot = false, hhot = false;     // each pane's bars over their own card edges
            int gb = SBar();
            DocState& D = DV(inB);
            int cardR = PaneX0(inB) + CardX(inB) + CardW(inB);
            int cardL = PaneX0(inB) + CardX(inB);
            if (lineHot) return 0;               // the grab zone: no bar hover underneath
            if (D.n && D.docH > g.ch && p.x >= cardR - gb && p.x < cardR) {
                int gutH = (D.n && D.docW > ViewW(D, inB)) ? gb : 0;
                int ins = BandInset(inB);
                int tl, tp; BarThumb(D.docH, g.ch - ins - gutH, D.sy, tl, tp,
                                     D.docH - (g.ch - gutH)); tp += ins;
                vhot = p.y >= tp && p.y < tp + tl;
            }
            if (D.n && D.docW > ViewW(D, inB) && p.y >= g.ch - gb && p.x >= cardL && p.x < cardR - ((D.docH > g.ch) ? gb : 0)) {
                int track = CardW(inB) - ((D.docH > g.ch) ? gb : 0);
                int tl, tp; BarThumb(D.docW, track, D.sx, tl, tp); hhot = p.x >= cardL + tp && p.x < cardL + tp + tl;
            }
            DocState& Do = DV(!inB);         // crossing panes clears the far pane's hots -
            if (vhot != D.vHot || hhot != D.hHot || Do.vHot || Do.hHot) {   // no MOUSELEAVE
                D.vHot = vhot; D.hHot = hhot; Do.vHot = false; Do.hHot = false;   // fires inside
                if (vhot || hhot) TrackLeave(h);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        DocState& D = g.d;
        int cw = CardW(false);
        bool vhot = D.n && D.docH > g.ch && p.x >= cw - SBar() && p.x < cw;
        if (vhot) {
            int gutH = (D.n && D.docW > cw - SBar()) ? SBar() : 0;
            int ins = BandInset(false);
            int tl, tp; BarThumb(D.docH, g.ch - ins - gutH, D.sy, tl, tp,
                                 D.docH - (g.ch - gutH)); tp += ins;
            vhot = p.y >= tp && p.y < tp + tl;
        }
        bool hhot = !vhot && D.n && D.docW > cw - (D.docH > g.ch ? SBar() : 0) && p.y >= g.ch - SBar() && p.x < cw - (vhot ? SBar() : 0);
        if (hhot) { int tl, tp; BarThumb(D.docW, cw - (vhot ? SBar() : 0), D.sx, tl, tp); hhot = p.x >= tp && p.x < tp + tl; }
        if (vhot != D.vHot || hhot != D.hHot) {
            D.vHot = vhot; D.hHot = hhot;
            if (vhot || hhot) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MBUTTONUP: {                     // middle-click closes the tab under it
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (TabStripMid(p)) return 0;
        break;
    }
    case WM_RBUTTONUP: {                     // the tab's context menu
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (TabStripRClick(h, p)) return 0;
        break;
    }
    case WM_MOUSELEAVE:
        if (g.tbTabHot >= 0 || g.tbHotBtn >= 2) {   // the strip's hover tiles clear on leave
            g.tbTabHot = -1; g.tbHotBtn = 0;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (g.d.vHot || g.d.hHot || g.dB.vHot || g.dB.hHot || fd.hot >= 0) {
            g.d.vHot = g.d.hHot = g.dB.vHot = g.dB.hHot = false; fd.hot = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (g.barHot != AB_OUT) { g.barHot = AB_OUT; AnnBarSync(true); }
        if (g.lineHot) { g.lineHot = false; InvalidateRect(h, nullptr, FALSE); }
        if (g.brushOn) {                    // the brush ring dies with the hover
            RECT u{ g.brushPt.x - 12, g.brushPt.y - 12, g.brushPt.x + 12, g.brushPt.y + 12 };
            g.brushOn = false;
            InvalidateRect(h, &u, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g.tbPending >= -3 && (g.tbPending != -1 || g.tbDragging)) {
            g.tbOwnRelease = true;               // the release is ours: CAPTURECHANGED fires
            ReleaseCapture();                    // synchronously inside it and must not eat
            g.tbOwnRelease = false;              // the press this UP is about to answer
            TabStripUp(h, p);
            return 0;
        }
        if (g_teResize) {                        // the drag ends: the real edit takes the pixels
            g_teResize = false;                  // back and the caret returns - KillerPDF hands
            ReleaseCapture();                    // focus back so typing continues (_tehBox?.Focus())
            if (g.tedit) {
                ShowCaret(g.tedit);
                InvalidateRect(g.tedit, nullptr, TRUE);
                SetFocus(g.tedit);
            }
            return 0;
        }
        if (g.barDrag) { g.barDrag = false; return 0; }   // the grip releases
        if (g.barSlide >= 0) { g.barSlide = -1; return 0; }   // the slider releases
        if (fd.drag) { fd.drag = false; fd.hot = -1; }   // the grip releases; capture goes below
        if (g.resizing) { g.resizing = false; ReleaseCapture(); LayoutMain(); return 0; }
        if (g.annMove || g.annInk) {        // the tool gesture releases: commit or discard
            ToolUp(h);
            ReleaseCapture();
            return 0;
        }
        if (g.d.selArm || g.dB.selArm) {         // whichever pane's press armed the drag owns it
            DocState& D = g.d.selArm ? g.d : g.dB;
            D.selArm = false;
            bool dragged = D.selDrag;
            D.selDrag = false;
            ReleaseCapture();
            if (!dragged || (D.selAPage == D.selFPage && D.selACaret == D.selFCaret)) {
                SelClear(D);                      // plain click collapses the selection
            } else if (g.selCommit == TOOL_HIGHLIGHT) {
                // KillerPDF's CommitFlowingHighlight: the swept lines leave as
                // one per-line highlight annotation set in the drag's own color
                // (the bar's Color row and Opacity slider), one undo entry
                UndoPush(&D == &g.dB);
                int sp, sc, ep, ec; SelOrdered(D, sp, sc, ep, ec);
                int total = 0;
                for (int pg = sp; pg <= ep && pg < D.n; pg++) {
                    int s, e; SelSliceForPage(D, pg, s, e);
                    if (s >= e) continue;
                    TxtPage* pt = TtFor(D, pg);
                    if (!pt) continue;
                    std::vector<FRect> rs; TtRects(*pt, s, e, rs);
                    if (rs.empty()) continue;
                    Annot a;
                    a.type = AT_HIGHLIGHT;
                    a.color = g.hlRGB; a.alpha = g.hlA;   // _highlightColor: yellow @ 31%
                    a.lines = rs;
                    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                    for (const FRect& r : rs) {
                        x0 = min(x0, r.x); y0 = min(y0, r.y);
                        x1 = max(x1, r.x + r.w); y1 = max(y1, r.y + r.h);
                    }
                    a.x = x0; a.y = y0; a.w = x1 - x0; a.h = y1 - y0;
                    D.annos[pg].push_back(a);
                    total += (int)rs.size();
                }
                SelClear(D);
                if (total) SetStatusHold(std::wstring(L"Highlighted ") + std::to_wstring(total) + L" line(s)");
                if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // Clear All arms with the page's first highlight
                InvalidateRect(h, nullptr, FALSE);
            } else {
                D.selRange = true;
                D.selText = SelBuildText(D, nullptr);
                if (D.selText.find_first_not_of(L" \n\r\t") == std::wstring::npos)
                    SelClear(D);                  // swept a page with no text layer
                else CopyClipboard(D.selText);    // select copies immediately, KillerPDF-style
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        g.d.vDrag = g.d.hDrag = g.dB.vDrag = g.dB.hDrag = g.splitDrag = false;
        ReleaseCapture();
        return 0;
        }
    case WM_CAPTURECHANGED:
        fd.drag = false;                       // someone else took it (a modal loop, a close)
        if (g.tbOwnRelease) {                  // our own LBUTTONUP release: the pending press
            g.tbOwnRelease = false;            // survives it - TabStripUp answers right after
        } else if (g.tbPending >= -3) {        // the strip's pending press dies with it:
            g.tbPending = -1; g.tbPendingX = false;   // a stolen capture is a click off the band
            g.tbDragging = false; g.tbGhostPane = -1; g.tbCaret = -1;
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        // double-click word selection, copied at once (KillerPDF's TrySelectWordAtPosition)
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g.barOn && AnnBarHit(p) >= AB_NONE) {   // a bar double-click tucks / expands it
            g.barMini = !g.barMini;
            g.barHot = AB_OUT;
            AnnBarSync(true);
            return 0;
        }
        if (fd.open && FdOverBar(p)) return 0;   // the second click on the bar is not a page
                                                 // double-click (the first press was the bar's)
        if (g.split) {                           // a divider double-click snaps the split back
            if (abs(p.x - g.splitX) <= SplitHalf()) {   // to the even 50/50 it opened at
                g.splitX = max(1, g.cw / 2);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        int pg; float fx, fy;
        bool pb = PaneB(p.x);
        DocState& D = DV(pb);                    // the double-click selects in ITS pane's document
        POINT q{ p.x - PaneX0(pb) - CardX(pb), p.y };
        if (ViewToPageAt(D, q, CardW(pb), g.ch, D.sx, D.sy, pg, fx, fy)) {
            TxtPage* pt = TtFor(D, pg);
            int s, e;
            if (pt && TtWordRange(*pt, fx, fy, s, e)) {
                D.selArm = D.selDrag = false;
                D.selAPage = D.selFPage = pg;
                D.selACaret = s; D.selFCaret = e;
                D.selRange = true;
                D.selText = TtText(*pt, s, e, nullptr);
                CopyClipboard(D.selText);
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            {   // the strip's pointers first (KillerPDF's Cursor=Hand on the tab border
                // and the ToolbarButton style): the hand over every tab, its close x,
                // the + and the chevron; the arrow over the band's own field - and
                // never the page tools' cursors, which would peek through the chrome
                POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
                if (p.y < BANDH() && !g.full) {
                    bool hb = g.split ? PaneB(p.x) : false;
                    if (BandShown(hb)) {
                        TabEnsure(hb);
                        TabStripGeo G; TabStripLayout(hb, G);
                        std::vector<int> vis;
                        for (int i = 0; i < (int)Tabs(hb).size(); i++)
                            if ((i == Act(hb) ? DV(hb).n : Tabs(hb)[i].n) > 0) vis.push_back(i);
                        int slot;
                        SetCursor(LoadCursorW(nullptr,
                                   TabBandHitAt(hb, G, vis, p, slot) ? IDC_HAND : IDC_ARROW));
                        return TRUE;
                    }
                }
            }
            if (g.barOn) {                     // the bar's cells take the hand (the bar wins
                POINT p; GetCursorPos(&p); ScreenToClient(h, &p);   // its pixels in the press
                if (AnnBarHit(p) != AB_OUT) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }   // path too)
            }
            if (g.tedit) {                     // the editor's corner handles say the drag
                POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
                RECT er; GetWindowRect(g.tedit, &er);
                ScreenToClient(h, (POINT*)&er); ScreenToClient(h, ((POINT*)&er) + 1);
                int hs = (int)(12 * g.dpi);
                for (int hd = 0; hd < 4; hd++) {
                    int hx = (hd == 0 || hd == 3) ? er.left : er.right;
                    int hy = (hd < 2) ? er.top : er.bottom;
                    if (p.x >= hx - hs / 2 && p.x < hx - hs / 2 + hs &&
                        p.y >= hy - hs / 2 && p.y < hy - hs / 2 + hs) {
                        // KillerPDF's handle cursors: NW/SE the NWSE arrow, NE/SW the NESW one
                        SetCursor(LoadCursorW(nullptr, (hd == 0 || hd == 2) ? IDC_SIZENWSE : IDC_SIZENESW));
                        return TRUE;
                    }
                }
            }
            if (fd.open) {                     // the bar's cells take the hand (ToolbarButton's
                POINT p; GetCursorPos(&p);     // Cursor=Hand); over the chrome an arrow - the
                ScreenToClient(h, &p);         // page's I-beam must not peek through the card
                int ht = fd.drag ? FB_GRIP : FdHitView(p);
                if (fd.drag || ht >= 0) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
                if (FdOverBar(p)) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
            }
            if (g.d.n || g.dB.n || g.split) {
            if (g.d.vDrag || g.d.hDrag || g.d.vHot || g.d.hHot || g.dB.vDrag || g.dB.hDrag || g.dB.vHot || g.dB.hHot)
                { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (g.split && abs(p.x - g.splitX) <= SplitHalf()) {   // the divider drags horizontally:
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE;   // the cursor says so
            }
            // the tools' cursors (KillerPDF's CursorForTool): Text I-beam, the
            // drag tools the cross (Draw pairs it with the brush ring for the
            // pen WPF shows; GDI has no stock pen cursor). Select keeps the
            // I-beam-over-text peek below.
            {
                bool pbT = PaneB(p.x);
                int tool = DV(pbT).tool;
                if (DV(pbT).n && tool == TOOL_TEXT) { SetCursor(LoadCursorW(nullptr, IDC_IBEAM)); return TRUE; }
                if (DV(pbT).n && (tool == TOOL_HIGHLIGHT || tool == TOOL_LINE || tool == TOOL_SHAPE || tool == TOOL_DRAW)) {
                    SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE;
                }
            }
            // I-beam over selectable text (KillerPDF's SelectableTextCursor), peeked
            // without building: the cursor advertises text only once the page's OCR
            // pass has landed, and the arrow elsewhere says a press does nothing
            int pg; float fx, fy;
            bool pb = PaneB(p.x);
            DocState& D = DV(pb);
            POINT q{ p.x - PaneX0(pb) - CardX(pb), p.y };
            if (ViewToPageAt(D, q, CardW(pb), g.ch, D.sx, D.sy, pg, fx, fy)) {
                TxtPage* pt = TtPeek(D, pg);
                if (pt && TtOverText(*pt, fx, fy)) { SetCursor(LoadCursorW(nullptr, IDC_IBEAM)); return TRUE; }
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
            }
        }
        break;
    case WM_DROPFILES: HandleDrop((HDROP)wp, h); return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g_fedit) FindChanged();   // the query edit
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g.tedit) TextEditFit();   // the text box grows with its lines
        if (HIWORD(wp) == EN_KILLFOCUS && (HWND)lp == g.tedit) CommitTextEdit();   // the text
        return 0;                                // tool's box lands when the focus leaves it
    case WM_CTLCOLOREDIT: {                    // the dark query edit; the text tool's box rides
        HDC dc = (HDC)wp;                      // its own sampled tone, KillerPDF's editor
        if ((HWND)lp == g.tedit) {
            SetTextColor(dc, g.textRGB);
            SetBkColor(dc, g_teTone);
            if (g.teBg) return (LRESULT)g.teBg;
            static HBRUSH w = CreateSolidBrush(RGB(0xff, 0xff, 0xff));
            return (LRESULT)w;
        }
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        SetBkColor(dc, RGB(0x1c, 0x1c, 0x1c));
        static HBRUSH b = CreateSolidBrush(RGB(0x1c, 0x1c, 0x1c));
        return (LRESULT)b;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

