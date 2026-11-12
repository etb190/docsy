// ---------------- page view ----------------

// KillerPDF's document card: the shared dark field fills the window and the
// canvas is a rounded card on it - RadCard 6 corners, a 1px PaneBorderBrush
// #555555 edge, inset 8px from the window's right edge, its other three edges
// flush against the band above, the sidebar left and the footer below, with
// every border pixel visible (WPF's BorderThickness draws inside the bounds).
// The field is painted by every pane from one tile, so the margins, the rail
// and the footer read as one container that no single component owns.
//
// The card is BAKED once per view size into a 32bpp backdrop DIB - field
// grain, KillerPDF's PaneShadow, the canvas interior - and a paint blits it
// under the pages. The corners were the sticking point: GDI regions stair-step
// (the "small cuts"), and bolting GDI+ corner patches over them left the arcs
// fighting the region edges and the top row clipped away. The bake composes
// the whole rim as one anti-aliased path set the way WPF's Border does, once,
// so a paint got cheaper (one BitBlt instead of fill + FrameRgn + four GDI+
// patches) and every corner, top included, is smooth.
// ---- the split's pane geometry ----
// splitX is the divider LINE's x. Pane A's pass spans [0, splitX): its card
// ends 4px short (its shadow room faces the gutter). Pane B's pass spans
// [splitX+4, g.cw): its card sits 4px in (left shadow room) and keeps the
// classic 8px window-edge gutter on its right. Unsplit, the single card is
// exactly the old geometry: full width minus the 8px window gutter.
static int PaneMin() { return (int)(320 * g.dpi); }   // KillerPDF's MinPaneWidth: neither pane goes
                                                      // under this, and the window's min track while
                                                      // split keeps room for both (WM_GETMINMAXINFO)
static int PaneW(bool b) { return b ? max(0, g.cw - g.splitX - SplitHalf())
                                    : (g.split ? g.splitX : g.cw); }  // a pane's pass width
static int PaneX0(bool b) { return b ? g.splitX + SplitHalf() : 0; }  // the pass's view x
static int CardX(bool b) { return b ? (int)(4 * g.dpi) : 0; }         // the card inside the pass
static int CardW(bool b) {
    if (!g.split) return max(0, g.cw - (int)(8 * g.dpi));
    return b ? max(0, PaneW(true) - (int)(12 * g.dpi))
             : max(0, PaneW(false) - (int)(4 * g.dpi));
}

static Gdiplus::GraphicsPath* CardPath(float x, float y, float w, float h, float rad, float inset) {
    Gdiplus::GraphicsPath* p = new Gdiplus::GraphicsPath();
    float r = max(0.5f, rad - inset);
    p->AddArc(x + inset, y + inset, 2 * r, 2 * r, 180, 90);
    p->AddArc(x + w - inset - 2 * r, y + inset, 2 * r, 2 * r, 270, 90);
    p->AddArc(x + w - inset - 2 * r, y + h - inset - 2 * r, 2 * r, 2 * r, 0, 90);
    p->AddArc(x + inset, y + h - inset - 2 * r, 2 * r, 2 * r, 90, 90);
    p->CloseFigure();
    return p;
}

static Gdiplus::TextureBrush* GrainTex(int which) {   // a grain tile as a GDI+ brush,
    static Gdiplus::TextureBrush* tb[3] = { nullptr, nullptr, nullptr };   // phased like GDI's own
    static float ox = -1, oy = -1;
    if (ox != (float)g_grainPX || oy != (float)g_grainPY) {   // the phase moves with the
        for (int i = 0; i < 3; i++) { delete tb[i]; tb[i] = nullptr; }   // pane layout
        ox = (float)g_grainPX; oy = (float)g_grainPY;
    }
    if (!tb[which]) {
        GrainBrush(which);               // make sure the DIB tile exists
        BITMAP gb{};
        if (!g_grainBmp[which] || !GetObject(g_grainBmp[which], sizeof gb, &gb) || !gb.bmBits) return nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), gb.bmWidth, gb.bmHeight, 1, (WORD)gb.bmBitsPixel, BI_RGB };
        Gdiplus::Bitmap* bm = Gdiplus::Bitmap::FromBITMAPINFO(&bi, gb.bmBits);   // copies the tile
        if (bm) {
            tb[which] = new Gdiplus::TextureBrush(bm, Gdiplus::WrapModeTile);
            tb[which]->TranslateTransform((float)((g_grainPX % 256 + 256) % 256),
                                          (float)((g_grainPY % 256 + 256) % 256));
        }
    }
    return tb[which];
}

// ---- the card's backdrop, baked once per view size -------------------------
//
// Dark.xaml's PaneShadowEffect: black, BlurRadius 16, ShadowDepth 5, Direction
// 270 (straight down), PaneShadowOpacity 0.6 - WPF blurs the card's rounded
// silhouette with a Gaussian of sigma BlurRadius/2 and blends it over the
// field before the card covers the middle. The exact signed distance to that
// silhouette (pushed 5dip down) indexes a precomputed erfc falloff, and the
// black multiplies into the baked field in place. Only the strip near the
// card's right edge is visited: the card is flush against the band, the
// sidebar and the footer (their panes sit above the shadow in KillerPDF's
// z-order, exactly why its reference render shows no shadow there), so the
// 8px window gutter is the only place the shadow can show - plus 4px under
// the ring's outer AA fringe, which must blend against darkened field.
static double Phi(double z);   // the footer's gaussian CDF, defined with it

static float SdRoundRect(float px, float py, float x0, float y0, float x1, float y1, float r) {
    float qx = fabsf(px - (x0 + x1) * 0.5f) - (x1 - x0) * 0.5f + r;
    float qy = fabsf(py - (y0 + y1) * 0.5f) - (y1 - y0) * 0.5f + r;
    float ax = max(qx, 0.f), ay = max(qy, 0.f);
    return sqrtf(ax * ax + ay * ay) + min(max(qx, qy), 0.f) - r;
}

static void CardShadowStrip(BYTE* bits, int w, int h, int cx0, int cx1, int rad, int ty = 0) {
    const double sig = 8.0 * g.dpi;
    const double depth = 5.0 * g.dpi;                     // ShadowDepth, Direction 270
    // the silhouette: the card pushed straight down, so the falloff reads
    // stronger along the gutter's lower half, like KillerPDF's render. The
    // card may sit inset in its DIB (pane B's left shadow room), so both
    // edges of the falloff are painted - each side lands only where the
    // gutter actually is. `ty` is the card's top row inside the DIB: the
    // tab band is a layout row, so the card - and its silhouette with it -
    // starts below the band (KillerPDF's card tucked under the strip).
    float sx0 = (float)cx0, sy0 = (float)ty + (float)depth, sx1 = (float)cx1, sy1 = (float)h + depth;
    // erfc falloff LUT: alpha(d) = 0.6 * Phi(-d / sig), d in [-3sig, 3sig]
    const int LUTN = 236;                // ~0.2px steps at sigma 8
    static float lut[LUTN]; static double lutSig = 0;
    if (lutSig != sig) {
        for (int i = 0; i < LUTN; i++) {
            double d = -3.0 * sig + (6.0 * sig) * i / (LUTN - 1);
            lut[i] = (float)(0.6 * Phi(-d / sig));
        }
        lutSig = sig;
    }
    int x0 = max(0, cx1 - 4), x1 = w;                 // the right falloff...
    for (int side = 0; side < 2; side++) {            // ...and, when the card is inset,
        if (side == 1) {                              // the left falloff too
            if (cx0 <= 0) break;
            x0 = 0; x1 = min(w, cx0 + 4);
        }
        for (int y = 0; y < h; y++) {
            BYTE* row = bits + (size_t)y * w * 4;
            for (int x = x0; x < x1; x++) {
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
}

// The ring, painted into the backdrop by pixel: GDI+ rasterizes with pixel
// CENTERS at integer coordinates, so a path edge at an integer lands exactly
// mid-pixel - the 1px even-odd band between two integer-edged paths smeared
// into two half-alpha columns (measured 60/67 where #555 should stand). The
// band's per-pixel coverage from the two signed distances has no such bias:
// a pixel is band where it sits inside the outer silhouette and outside the
// inner one, clamped over the half-pixel the center sample cannot see. The
// straights land full #555 on the outermost pixel, and the corner arcs get
// analytic anti-aliasing that beats a bezier arc's rasterization. `cx` is the
// card's left edge inside the DIB (pane B sits 4px in for its left shadow).
static void CardRingBake(BYTE* bits, int w, int h, int cw, int rad, int cx, int ty = 0) {
    float x1 = (float)(cx + cw) - 0.5f, y1 = (float)h - 0.5f;   // outer silhouette
    float ox0 = (float)cx - 0.5f, oy0 = (float)ty - 0.5f, orad = (float)rad;   //   in pixel-center space
    float ix0 = (float)cx + 0.5f, iy0 = (float)ty + 0.5f, irad = (float)rad - 1;  // inner silhouette
    int band2 = min(rad + 2, min(h, cw));
    auto ring = [&](int x, int y) {
        float f = -SdRoundRect((float)x, (float)y, ox0, oy0, x1, y1, orad);   // inside outer
        float gq = SdRoundRect((float)x, (float)y, ix0, iy0, x1 - 1, y1 - 1, irad);  // outside inner
        float a = min(f, gq) + 0.5f;
        if (a <= 0) return;
        if (a > 1) a = 1;
        BYTE* p = bits + ((size_t)y * w + x) * 4;
        int al = (int)(a * 255 + 0.5f), inv = 255 - al;
        p[0] = (BYTE)((0x55 * al + p[0] * inv) >> 8);
        p[1] = (BYTE)((0x55 * al + p[1] * inv) >> 8);
        p[2] = (BYTE)((0x55 * al + p[2] * inv) >> 8);
    };
    for (int y = ty; y < ty + band2 && y < h; y++)        // top strip, then bottom,
        for (int x = 0; x < w; x++) ring(x, y);
    for (int y = max(band2, h - band2); y < h; y++)
        for (int x = 0; x < w; x++) ring(x, y);
    for (int y = ty + band2; y < max(band2, h - band2); y++) {
        for (int x = 0; x < band2 + cx; x++) ring(x, y);       // left, then right - the straights'
        for (int x = max(band2, cx + cw - band2); x < w; x++) ring(x, y);   // mid pixels early-out inside
    }
}

// The backdrop DIBs: field grain + PaneShadow strip + canvas interior (the
// inner arc's fill, canvas grain phased to the window like every pane) + the
// #555555 ring. TWO slots, one per pane: the split paints two cards of two
// widths every frame, and re-baking a full-surface grain fill per pass would
// be the exact per-frame waste a 2,000-page book cannot afford. Each slot
// re-bakes only when its own size or the DPI changes.
static HBITMAP g_bdBmp[2] = { nullptr, nullptr };   // 32bpp top-down backdrops
static HDC g_bdDC[2] = { nullptr, nullptr };
static int g_bdW[2] = { 0, 0 }, g_bdH[2] = { 0, 0 };
static double g_bdDpi[2] = { 0, 0 }; // the bake key: size AND dpi (PerMonitorV2 hops)
static int g_bdIns[2] = { -1, -1 };  // ... and the card's top inset (the band's row)
static BYTE* g_bdBits[2] = { nullptr, nullptr };

static void CardBackdrop(HDC ref, int w, int h, bool b) {
    int slot = b ? 1 : 0;
    // the card's top row inside this DIB: the band is a layout row - KillerPDF's
    // card sits in the row BELOW its strip, its top border tucked 1px into the
    // band's foot so the active tab covers that pixel without a seam. One row
    // short of BANDH keeps that border visible under the inactive tabs.
    int ty = max(0, BandInset(b) - 1);
    if (g_bdBmp[slot] && g_bdW[slot] == w && g_bdH[slot] == h && g_bdDpi[slot] == g.dpi && g_bdIns[slot] == ty) return;
    if (g_bdDC[slot]) { DeleteDC(g_bdDC[slot]); g_bdDC[slot] = nullptr; }
    if (g_bdBmp[slot]) { DeleteObject(g_bdBmp[slot]); g_bdBmp[slot] = nullptr; }
    g_bdW[slot] = 0; g_bdH[slot] = 0;
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    BYTE* bits = nullptr;
    HBITMAP bm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    if (!bm || !bits) return;
    HDC bdc = CreateCompatibleDC(ref);
    HGDIOBJ ob = SelectObject(bdc, bm);
    // the card inside THIS DIB: pane B sits 4px in (its left shadow room), its
    // right edge keeps the window gutter; pane A and the unsplit card sit at 0
    int cx = b ? (int)(4 * g.dpi) : 0;
    int cw = CardW(b);                       // the card's own width inside this pass
    int rad = max(2, (int)(6 * g.dpi));
    bool card = cw > 2 * rad + 4 && h > 2 * rad + 2;
    // the DIB's left edge sits at the pane's view x, so the grain phase shifts
    // by exactly that - the field across both panes and the margins reads as
    // ONE surface, no seam at the gutter
    int xoffScreen = PaneX0(b);
    SetBrushOrgEx(bdc, g_grainPX - xoffScreen, g_grainPY, nullptr);
    RECT fr{ 0, 0, w, h };                                // the window's grain phase,
    FillRect(bdc, &fr, GrainBrush(1));                    // like every sibling pane
    if (card) {
        CardShadowStrip(bits, w, h, cx, cx + cw, rad, ty);
        Gdiplus::Graphics gs(bdc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // the canvas interior, half a pixel in: GDI+ centers pixels on integer
        // coordinates, so the path edge must sit at the boundary between the
        // ring's outermost pixel and the canvas's first one to fill it whole
        Gdiplus::GraphicsPath* inner = CardPath((float)cx + 0.5f, (float)ty + 0.5f, (float)cw - 1,
                                                (float)h - 1 - ty, (float)rad - 1, 0);
        if (Gdiplus::TextureBrush* c = GrainTex(0)) {     // the canvas, phased like GDI's
            c->TranslateTransform((float)-xoffScreen, 0.f);
            gs.FillPath(c, inner);
        }
        else {
            Gdiplus::SolidBrush cb(Gdiplus::Color(255, 58, 58, 58));
            gs.FillPath(&cb, inner);
        }
        delete inner;
        CardRingBake(bits, w, h, cw, rad, cx, ty);        // the #555 ring, exact per pixel
    }
    SelectObject(bdc, ob);
    DeleteDC(bdc);
    g_bdBmp[slot] = bm; g_bdDC[slot] = CreateCompatibleDC(ref);
    SelectObject(g_bdDC[slot], bm);
    g_bdBits[slot] = bits; g_bdW[slot] = w; g_bdH[slot] = h; g_bdDpi[slot] = g.dpi; g_bdIns[slot] = ty;
}

// Find highlights on one visible page (KillerPDF's AddSearchHighlight): a rounded
// box around every matched word of the page - dim orange fill, faint stroke - and
// the cursor match bright with the 3.5px accent ring; the box pads 30% of the
// shorter side and rounds at 60% of the pad, so the wrap breathes like the
// original's and stays consistent across font sizes.
static void FindPaintPage(HDC dc, const DocState& D, int page, int px, int py) {
    if (!fd.open || fd.hits.empty()) return;
    // The hits read in scan order - your page first, then forward, then the
    // pages before it - so the list is a rotation, not page-ascending. Every
    // page's matches are still one contiguous run (the pump scans a page
    // whole), so a linear walk finds it; lower_bound's sorted-list assumption
    // would miss the wrapped half.
    size_t i = 0, n = fd.hits.size();
    while (i < n && fd.hits[i].page != page) i++;
    if (i >= n) return;
    float k = (float)(D.zoom * g.dpi);
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush dim(Gdiplus::Color(70, 255, 165, 0));
    Gdiplus::SolidBrush hot(Gdiplus::Color(150, 255, 190, 0));
    Gdiplus::Pen dimPen(Gdiplus::Color(140, 255, 165, 0), 1.0f * (float)g.dpi);
    Gdiplus::Pen hotPen(Gdiplus::Color(255, 0x22, 0xbb, 0x56), (float)(3.5 * g.dpi));
    for (; i < n && fd.hits[i].page == page; ++i) {
        bool cur = (int)i == fd.cur;
        const FRect& r = fd.hits[i].r;
        float pad = min(r.w, r.h) * 0.30f;
        float x = px + (r.x - pad) * k, y = py + (r.y - pad) * k;
        float w = max((r.w + 2 * pad) * k, 4.0f), h = max((r.h + 2 * pad) * k, 4.0f);
        float rr = min(pad * 0.6f * k, h * 0.5f);
        if (rr < 0.5f) {
            Gdiplus::RectF rf(x, y, w, h);
            gs.FillRectangle(cur ? (Gdiplus::Brush*)&hot : (Gdiplus::Brush*)&dim, rf);
            if (cur) gs.DrawRectangle(&hotPen, rf);
            continue;
        }
        Gdiplus::GraphicsPath p;
        p.AddArc(x, y, 2 * rr, 2 * rr, 180, 90);
        p.AddArc(x + w - 2 * rr, y, 2 * rr, 2 * rr, 270, 90);
        p.AddArc(x + w - 2 * rr, y + h - 2 * rr, 2 * rr, 2 * rr, 0, 90);
        p.AddArc(x, y + h - 2 * rr, 2 * rr, 2 * rr, 90, 90);
        p.CloseFigure();
        gs.FillPath(cur ? (Gdiplus::Brush*)&hot : (Gdiplus::Brush*)&dim, &p);
        if (cur) gs.DrawPath(&hotPen, &p);
        else gs.DrawPath(&dimPen, &p);
    }
}

// ---- annotation geometry + paint (the six tools' committed state) ----

// The content's x inside the card: centered in the viewport when it fits,
// scrolled otherwise - the exact term Draw lays pages out with, shared with the
// gesture code so a stroke's invalidation rect lands where the paint will.
static int DocOffX(const DocState& D, int cw) {
    int gut = (D.n && D.docH > g.ch) ? SBar() : 0;   // the v-bar's reserved column
    return D.docW < cw - gut ? (cw - gut - D.docW) / 2 : -D.sx;
}

// A page's top-left in VIEW pixels (pass-relative for pane B; callers add
// PaneX0). Mirrors Draw's placement so events and paint agree per pixel.
static bool PageOrigin(const DocState& D, bool b, int i, int& ox, int& oy) {
    if (i < 0 || i >= D.n || (int)D.px.size() < D.n || D.top.empty() || D.rh.empty()) return false;
    int row = D.vm == 1 ? 0 : i / PerRow(D);
    if (row >= (int)D.rh.size()) return false;
    int cw = CardW(b);
    ox = CardX(b) + DocOffX(D, cw) + D.px[i];
    oy = D.top[row] - D.sy + (D.rh[row] - PH(D, i)) / 2;
    return true;
}

// An annotation's bounds in page DIPs: the line-quad union, the stroke's bbox
// widened by half the width, or the text box. One source for hit tests, the
// move gesture's old-∪-new invalidation and the commit paint.
static void AnnotBounds(const Annot& a, float& x0, float& y0, float& x1, float& y1) {
    x0 = y0 = 1e9f; x1 = y1 = -1e9f;
    if (a.type == AT_HIGHLIGHT) {
        for (const FRect& r : a.lines) {
            x0 = min(x0, r.x); y0 = min(y0, r.y);
            x1 = max(x1, r.x + r.w); y1 = max(y1, r.y + r.h);
        }
    } else if (a.type == AT_INK) {
        for (size_t i = 0; i < a.ix.size(); i++) {
            x0 = min(x0, a.ix[i]); x1 = max(x1, a.ix[i]);
            y0 = min(y0, a.iy[i]); y1 = max(y1, a.iy[i]);
        }
        float hw = a.width * 0.5f;
        x0 -= hw; y0 -= hw; x1 += hw; y1 += hw;
    } else {
        x0 = a.x; y0 = a.y; x1 = a.x + a.w; y1 = a.y + a.h;
    }
    if (x1 < x0) { x0 = x1 = 0; y0 = y1 = 0; }
}

// Point-to-segment distance (KillerPDF's DistPointToSegment): an ink stroke is
// hit along its path, not inside its bbox.
static float PtSegDist(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    float qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrtf(qx * qx + qy * qy);
}

// Does the page point hit the annotation? Padding is 2px worth of DIPs so a
// thin stroke is grabbable (KillerPDF's HitTestAnnotation pads the same way).
static bool AnnotHit(const Annot& a, float fx, float fy, float k) {
    float pad = 2.0f / max(0.01f, k);
    if (a.type == AT_HIGHLIGHT) {
        for (const FRect& r : a.lines)
            if (fx >= r.x - pad && fx <= r.x + r.w + pad && fy >= r.y - pad && fy <= r.y + r.h + pad) return true;
        return false;
    }
    if (a.type == AT_INK) {
        float reach = a.width * 0.5f + pad;
        if (a.ix.size() == 1) {
            float dx = fx - a.ix[0], dy = fy - a.iy[0];
            return dx * dx + dy * dy <= reach * reach;
        }
        for (size_t i = 0; i + 1 < a.ix.size(); i++)
            if (PtSegDist(fx, fy, a.ix[i], a.iy[i], a.ix[i + 1], a.iy[i + 1]) <= reach) return true;
        return false;
    }
    if (a.type == AT_TEXT) {   // KillerPDF's HitTestAnnotation grabs a text box with a
        return fx >= a.x - 8 && fx <= a.x + a.w + 8 &&    // forgiving margin - a one-line
               fy >= a.y - 10 && fy <= a.y + a.h + 10;    // box is a thin band, so clicks
    }                                                     // near it still select it
    return fx >= a.x - pad && fx <= a.x + a.w + pad && fy >= a.y - pad && fy <= a.y + a.h + pad;
}

static HFONT g_tafont = nullptr; static int g_tapx = 0; static std::wstring g_tafontName;

// One page's annotations, above the page and below the selection quads -
// KillerPDF's overlay order. Highlights are translucent line fills, ink rides
// GDI+ round caps at the stroke's own width (closed shapes fill their interior
// at the fill color when one is set), text renders in its own font and fill,
// and the Select tool's chosen annotation wears KillerPDF's accent ring with
// its four 14px corner handles.
static void PaintAnnots(HDC dc, const DocState& D, int page, int px, int py, float k) {
    auto it = D.annos.find(page);
    if (it == D.annos.end() || it->second.empty()) return;
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (size_t ai = 0; ai < it->second.size(); ai++) {
        const Annot& a = it->second[ai];
        Gdiplus::Color c(a.alpha, GetRValue(a.color), GetGValue(a.color), GetBValue(a.color));
        if (a.type == AT_HIGHLIGHT) {
            Gdiplus::SolidBrush br(c);
            for (const FRect& r : a.lines)
                gs.FillRectangle(&br, px + r.x * k, py + r.y * k, max(r.w * k, 2.0f), max(r.h * k, 2.0f));
        } else if (a.type == AT_INK) {
            if (a.ix.empty()) continue;
            std::vector<Gdiplus::PointF> pts;
            pts.reserve(a.ix.size());
            for (size_t i = 0; i < a.ix.size(); i++)
                pts.push_back(Gdiplus::PointF(px + a.ix[i] * k, py + a.iy[i] * k));
            if (a.fillA > 0 && a.ix.size() >= 3) {   // closed shapes fill their interior
                Gdiplus::Color fc(a.fillA, GetRValue(a.fill), GetGValue(a.fill), GetBValue(a.fill));
                Gdiplus::SolidBrush fb(fc);
                gs.FillPolygon(&fb, pts.data(), (INT)pts.size());
            }
            Gdiplus::Pen pen(c, max(1.0f, a.width * k));
            pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
            gs.DrawLines(&pen, pts.data(), (INT)pts.size());   // a dot: two points 0.25 apart,
                                                               // round caps meet as a disc
        } else {                                     // AT_TEXT
            if (a.fillA > 0) {
                Gdiplus::Color fc(a.fillA, GetRValue(a.fill), GetGValue(a.fill), GetBValue(a.fill));
                Gdiplus::SolidBrush fb(fc);
                gs.FillRectangle(&fb, px + a.x * k, py + a.y * k,
                                 max(a.w * k, 1.0f), max(a.h * k, 1.0f));
            }
            if (!a.text.empty()) {
                int hpx = max(6, (int)std::lround(a.fsize * k));
                // one cached face per (px, name, styles) - the usual case is a
                // single text style per document
                if (!g_tafont || g_tapx != hpx || g_tafontName != a.font) {
                    if (g_tafont) DeleteObject(g_tafont);
                    g_tafont = CreateFontW(-hpx, 0, 0, 0, a.bold ? FW_BOLD : FW_NORMAL,
                                           a.italic, a.under, 0, DEFAULT_CHARSET,
                                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_DONTCARE, a.font.c_str());
                    g_tapx = hpx; g_tafontName = a.font;
                }
                HGDIOBJ of = SelectObject(dc, g_tafont);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, a.color);
                RECT tr{ px + (int)(a.x * k) + 2, py + (int)(a.y * k) + 1,
                         px + (int)((a.x + a.w) * k), py + (int)((a.y + a.h) * k) };
                DrawTextW(dc, a.text.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
                SelectObject(dc, of);
            }
        }
        if (D.tool == TOOL_SELECT && g.annSelPage == page && g.annSelIdx == (int)ai &&
            g.annSelB == (&D == &g.dB)) {
            // KillerPDF's selection chrome: the accent border at 2px over a
            // 40-alpha wash, bounds + 8, plus the four 14px corner handles
            float x0, y0, x1, y1; AnnotBounds(a, x0, y0, x1, y1);
            float inv = 4.0f / max(0.01f, k);        // bounds + 8 px chrome, page DIPs
            float bx0 = x0 - inv, by0 = y0 - inv, bx1 = x1 + inv, by1 = y1 + inv;
            Gdiplus::Color acc(255, 0x22, 0xbb, 0x56);
            Gdiplus::SolidBrush wash(Gdiplus::Color(40, 0x22, 0xbb, 0x56));
            gs.FillRectangle(&wash, px + bx0 * k, py + by0 * k, (bx1 - bx0) * k, (by1 - by0) * k);
            Gdiplus::Pen sel(acc, 2.0f * (float)g.dpi);
            gs.DrawRectangle(&sel, px + bx0 * k, py + by0 * k,
                             max(1.0f, (bx1 - bx0) * k), max(1.0f, (by1 - by0) * k));
            float hs = 14.0f / max(0.01f, k) * 0.5f;
            float hxs[4] = { bx0, bx1, bx1, bx0 }, hys[4] = { by0, by0, by1, by1 };
            for (int hd = 0; hd < 4; hd++) {
                Gdiplus::SolidBrush hb(acc);
                gs.FillRectangle(&hb, px + (hxs[hd] - hs) * k, py + (hys[hd] - hs) * k,
                                 2 * hs * k, 2 * hs * k);
                Gdiplus::Pen hw(Gdiplus::Color(255, 255, 255, 255), 1.0f * (float)g.dpi);
                gs.DrawRectangle(&hw, px + (hxs[hd] - hs) * k, py + (hys[hd] - hs) * k,
                                 2 * hs * k, 2 * hs * k);
            }
        }
    }
}

// The live gesture's preview, painted exactly like the commit will read:
// Draw/Line ride the tool's own color/width, a filled box shows the full-alpha
// block it commits, an outline shape shows its stroke (the ellipse previews
// its fill at half alpha when Fill is on), the polygon draws its dashed rubber
// band with the snap ring, and the erasers show KillerPDF's red carve box /
// the brush path.
static void PaintLiveStroke(HDC dc, const DocState& D, int page, int px, int py, float k) {
    if (g.annPoly && g.annPage == page && g.annPaneB == (&D == &g.dB)) {
        if (!g.annPX.empty()) {
            Gdiplus::Graphics gs(dc);
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Color pc(max(60, g.drawOpacity / 2), GetRValue(g.drawColor),
                              GetGValue(g.drawColor), GetBValue(g.drawColor));
            Gdiplus::Pen pen(pc, max(1.0f, g.drawWidth / 2 * k));
            pen.SetDashStyle(Gdiplus::DashStyleDash);
            std::vector<Gdiplus::PointF> pts;
            for (size_t i = 0; i < g.annPX.size(); i++)
                pts.push_back(Gdiplus::PointF(px + g.annPX[i] * k, py + g.annPY[i] * k));
            pts.push_back(Gdiplus::PointF(px + g.annFX * k, py + g.annFY * k));   // the rubber band
            gs.DrawLines(&pen, pts.data(), (INT)pts.size());
            // the closing snap ring: 14px around the first vertex when near
            float dx0 = g.annFX - g.annPX[0], dy0 = g.annFY - g.annPY[0];
            if (g.annPX.size() >= 3 && dx0 * dx0 + dy0 * dy0 <= 64.0f) {
                Gdiplus::Pen ring(Gdiplus::Color(255, 0x22, 0xbb, 0x56), 2.0f);
                gs.DrawEllipse(&ring, px + g.annPX[0] * k - 7.0f, py + g.annPY[0] * k - 7.0f, 14.0f, 14.0f);
            }
        }
        return;
    }
    if (!g.annInk || g.annPage != page || g.annPaneB != (&D == &g.dB)) return;
    if (g.annIX.empty()) return;
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Color col(g.drawOpacity, GetRValue(g.drawColor), GetGValue(g.drawColor), GetBValue(g.drawColor));
    if (g.annKind == -1) {                       // the highlight eraser's carve box:
                                                 // red marching-ants fill
        float x0 = min(g.annIX[0], g.annIX[1]), x1 = max(g.annIX[0], g.annIX[1]);
        float y0 = min(g.annIY[0], g.annIY[1]), y1 = max(g.annIY[0], g.annIY[1]);
        Gdiplus::SolidBrush f(Gdiplus::Color(36, 255, 60, 60));
        gs.FillRectangle(&f, px + x0 * k, py + y0 * k, (x1 - x0) * k, (y1 - y0) * k);
        Gdiplus::Pen pe(Gdiplus::Color(235, 0xDC, 0x28, 0x28), 2.0f);
        pe.SetDashStyle(Gdiplus::DashStyleDash);
        gs.DrawRectangle(&pe, px + x0 * k, py + y0 * k, (x1 - x0) * k, (y1 - y0) * k);
        return;
    }
    if (g.annKind == -2) {                       // the brush eraser: a gray dashed trail
        std::vector<Gdiplus::PointF> pts;
        for (size_t i = 0; i < g.annIX.size(); i++)
            pts.push_back(Gdiplus::PointF(px + g.annIX[i] * k, py + g.annIY[i] * k));
        Gdiplus::Pen pen(Gdiplus::Color(120, 0xC8, 0xC8, 0xC8), max(1.0f, g.drawWidth * k));
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        if (pts.size() >= 2) gs.DrawLines(&pen, pts.data(), (INT)pts.size());
        return;
    }
    if (g.annKind == TOOL_SHAPE && g.annIX.size() >= 2) {
        float x0 = min(g.annIX[0], g.annIX[1]), x1 = max(g.annIX[0], g.annIX[1]);
        float y0 = min(g.annIY[0], g.annIY[1]), y1 = max(g.annIY[0], g.annIY[1]);
        if (g.shapeKind == 0 && g.shapeFill) {   // the filled box: the full-alpha block
            Gdiplus::SolidBrush br(col);
            gs.FillRectangle(&br, px + x0 * k, py + y0 * k, (x1 - x0) * k, (y1 - y0) * k);
            return;
        }
        if (g.shapeKind == 0) {                  // the outline box: the stroke only
            Gdiplus::Pen pen(col, max(1.0f, g.drawWidth * k));
            gs.DrawRectangle(&pen, px + x0 * k, py + y0 * k, (x1 - x0) * k, (y1 - y0) * k);
            return;
        }
        if (g.shapeKind == 1) {                  // the ellipse: 64 segments; fill at half alpha
            if (g.shapeFill) {
                Gdiplus::SolidBrush fb(Gdiplus::Color((BYTE)max(20, g.drawOpacity / 2),
                                      GetRValue(g.drawColor), GetGValue(g.drawColor), GetBValue(g.drawColor)));
                gs.FillEllipse(&fb, px + x0 * k, py + y0 * k, (x1 - x0) * k, (y1 - y0) * k);
            }
            Gdiplus::Pen pen(col, max(1.0f, g.drawWidth * k));
            gs.DrawEllipse(&pen, px + x0 * k, py + y0 * k, (x1 - x0) * k, (y1 - y0) * k);
            return;
        }
    }
    size_t n = g.annKind == TOOL_DRAW ? g.annIX.size() : 2;
    if (n < 2) n = min<size_t>(2, g.annIX.size());
    if (n < 2) return;
    std::vector<Gdiplus::PointF> pts;
    for (size_t i = 0; i < n; i++)
        pts.push_back(Gdiplus::PointF(px + g.annIX[i] * k, py + g.annIY[i] * k));
    Gdiplus::Pen pen(col, max(1.0f, g.drawWidth * k));
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    gs.DrawLines(&pen, pts.data(), (INT)pts.size());
}

// One pass paints ONE pane's document into the back buffer: its own backdrop
// card, its own layout, its own scroll. `extra` (optional) intersects the card
// clip with that pane's region, so the split's second pass reveals only its
// own strip - two panes, two documents, one frame.
static void Draw(HDC dc, int w, int h, int xoff, DocState& D, HRGN extra = nullptr) {
    bool thisIsB = &D == &g.dB;
    CardBackdrop(dc, w, h, thisIsB);     // baked field + PaneShadow + canvas, per pane and size
    int cx = CardX(thisIsB);             // the card inside this pass (pane B sits 4px in)
    int cw = CardW(thisIsB);
    if (g_bdBmp[thisIsB ? 1 : 0]) BitBlt(dc, xoff, 0, w, h, g_bdDC[thisIsB ? 1 : 0], 0, 0, SRCCOPY);
    else {
        RECT r{ xoff, 0, xoff + w, h };   // degenerate bake: the field alone
        FillRect(dc, &r, GrainBrush(1));
    }
    int rad = max(2, (int)(6 * g.dpi));
    int cardTop = max(0, BandInset(thisIsB) - 1);   // the card's top row: under the band
    bool card = cw > 2 * rad + 4 && h > 2 * rad + 2;
    HRGN clip = card ? CreateRoundRectRgn(xoff + cx + 2, cardTop + 2, xoff + cx + cw - 2, h - 2, 2 * (rad - 2), 2 * (rad - 2)) : nullptr;
    if (clip) SelectClipRgn(dc, clip);   // ContentClip: two pixels inside the card, past the
                                         // region's corner steps, so pages never nick the ring
    if (extra) ExtSelectClipRgn(dc, extra, RGN_AND);   // then the pane's strip, if splitting
    if (!D.n) {
        int sz = (int)(128 * g.dpi);
        if (!g.logo) g.logo = (HICON)LoadImageW(g.hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR);
        int cy = h / 2;
        if (g.logo) DrawIconEx(dc, xoff + (cw - sz) / 2, cy - sz - (int)(6 * g.dpi), g.logo, sz, sz, 0, nullptr, DI_NORMAL);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(200, 200, 205));
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT tr{ xoff, cy + (int)(8 * g.dpi), xoff + cw, h };
        DrawTextW(dc, L"Docsy  -  Ctrl+O or drop a PDF here", -1, &tr, DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (clip) { SelectClipRgn(dc, nullptr); DeleteObject(clip); }
        return;
    }
    HDC m = CreateCompatibleDC(dc);
    int offx = DocOffX(D, cw);           // the shared centering/scroll term: the v-bar's
                                         // reserved column keeps pages clear of the thumb
    bool selOn = D.selRange || (D.selArm && D.selDrag);   // quads paint live during the drag
    float kz = (float)(D.zoom * g.dpi);  // the page-DIP -> pixel scale for the overlay paints
                                         // (the loop below reuses k as its column index)
    int r0 = RowAt(D, D.sy), r1 = RowAt(D, D.sy + h - 1);
    int first = RowPage(D, r0, 0), last = first;
    int per = PerRow(D);
    for (int row = r0; row <= r1; row++) {
        int cnt = D.vm == 1 ? 1 : min(per, D.n - row * per);
        for (int k = 0; k < cnt; k++) {
            int i = RowPage(D, row, k);
            if (i < 0 || i >= D.n) continue;
            last = i;
            int pw = PW(D, i), ph = PH(D, i);
            int x = xoff + cx + offx + D.px[i];
            int y = D.top[row] - D.sy + (D.rh[row] - ph) / 2;   // pages ride centred in their row
            if (y + ph < 0) continue;
            int bw, bh;
            HBITMAP b = GetBmpEx(D, i, pw, ph, bw, bh, false);
            if (b) {
                HGDIOBJ o = SelectObject(m, b);
                // DocInvert (the footer moon): KillerPDF flips the document images to
                // their negative - the page rect is opaque, so NOTSRCCOPY inverts it
                // during the blit and the grain margins keep their tone.
                if (bw == pw && bh == ph)
                    BitBlt(dc, x, y, pw, ph, m, 0, 0, g.inv ? NOTSRCCOPY : SRCCOPY);
                else {                             // the worker's catch-up raster: the old
                    SetStretchBltMode(m, HALFTONE);   // zoom still speaks for the page -
                    SetStretchBltMode(dc, HALFTONE);  // stretched into the new rect until
                    SetBrushOrgEx(m, 0, 0, nullptr);  // the right-size render lands (both
                    StretchBlt(dc, x, y, pw, ph, m, 0, 0, bw, bh,   // DCs: drivers read the
                               g.inv ? NOTSRCCOPY : SRCCOPY);       // mode from either side)
                }
                SelectObject(m, o);
            } else {
                RECT pr{ x, y, x + pw, y + ph };
                FillRect(dc, &pr, (HBRUSH)GetStockObject(WHITE_BRUSH));
            }
            // the tools' committed annotations, above the page and below the
            // selection quads - KillerPDF's overlay order
            PaintAnnots(dc, D, i, x, y, kz);
            PaintLiveStroke(dc, D, i, x, y, kz);
            // flowing selection quads: KillerPDF paints one rect per selected line on an
            // overlay above the page - 60/255 of the dark theme's SelectionAccent #22bb56
            if (selOn) {
                int s, e; SelSliceForPage(D, i, s, e);
                if (s < e) {
                    TxtPage* pt = TtFor(D, i);
                    if (pt) {
                        std::vector<FRect> rs; TtRects(*pt, s, e, rs);
                        Gdiplus::Graphics gs(dc);
                        Gdiplus::SolidBrush br(Gdiplus::Color(60, 0x22, 0xbb, 0x56));
                        for (const FRect& q : rs)
                            gs.FillRectangle(&br, Gdiplus::RectF(x + q.x * kz, y + q.y * kz,
                                                                 max(q.w * kz, 2.0f), max(q.h * kz, 2.0f)));
                    }
                }
            }
            if (fd.open && fd.pane == thisIsB) FindPaintPage(dc, D, i, x, y);   // highlights ride the pane they scanned
        }
    }
    DeleteDC(m);
    if (!g.hint.empty() && GetTickCount64() < g.hintUntil) {   // the full-screen hint
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, UiFont());
        RECT hs{ xoff + 1, h - (int)(34 * g.dpi) + 1, xoff + cw + 1, h - (int)(12 * g.dpi) + 1 };
        SetTextColor(dc, RGB(10, 10, 10));
        DrawTextW(dc, g.hint.c_str(), -1, &hs, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT hn{ xoff, h - (int)(34 * g.dpi), xoff + cw, h - (int)(12 * g.dpi) };
        SetTextColor(dc, RGB(0xe8, 0xe8, 0xe8));
        DrawTextW(dc, g.hint.c_str(), -1, &hn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (clip) { SelectClipRgn(dc, nullptr); DeleteObject(clip); }
    D.lastVis = last;
    Evict(D, first, last);
}

static void Prefetch(DocState& D) {
    if (!D.n) return;
    int cand[2] = { D.lastVis + 1, PageAt(D, D.sy) - 1 };
    for (int i : cand)
        if (i >= 0 && i < D.n) {
            auto it = D.cache.find(i);
            if (it == D.cache.end() || it->second.w != PW(D, i)) { GetBmp(D, i, PW(D, i), PH(D, i)); }
        }
}
static void EnsureBack(HDC ref, int w, int h) {
    if (g.bbm && g.bw == w && g.bh == h) return;
    if (g.bbm) { SelectObject(g.bdc, g.bold); DeleteObject(g.bbm); }
    else g.bdc = CreateCompatibleDC(ref);
    g.bbm = CreateCompatibleBitmap(ref, w, h);
    g.bold = (HBITMAP)SelectObject(g.bdc, g.bbm);
    g.bw = w; g.bh = h;
}

// The page view's overlay bars. Like KillerPDF's document pane they live in a
// reserved 12px gutter (each bar's track stops short of the other's reserved
// strip, the WPF ScrollViewer's cross reservation), the track paints nothing,
// and the thumb is the only visible part: 4px and a third alpha at rest, 8px
// and solid under the cursor.
static void PaintViewBars(HDC dc, int w, int h, int xoff) {
    DocState& D = g.d;                   // the unsplit card is pane A's document surface
    if (!D.n) return;
    int gb = SBar(), r = (int)(3 * g.dpi);
    int cw = min(w, CardW(false));       // the bars ride inside the card, not on the window gutter
    bool vshow = D.docH > h;
    int track = cw - (vshow ? gb : 0);   // the content viewport: the v-bar's column is reserved
    bool hshow = D.docW > track;         // out of it, and the h-bar answers to THAT width
    int gutH = hshow ? gb : 0;
    int ins = BandInset(false);          // the band is a layout row: the track starts under it
    if (vshow) {
        // the thumb's length comes off the visible viewport (band out), its
        // travel maps sy over the REAL clamp range (the under-band model) -
        // mapped over the short range it hit its end ins pixels early; mapped
        // over the tall viewport the track itself overshot the h-bar
        int tl, tp; BarThumb(D.docH, h - ins - gutH, D.sy, tl, tp, D.docH - (h - gutH));
        tp += ins;
        bool hot = D.vHot || D.vDrag;
        int bw = hot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);
        RoundRectFill(dc, xoff + cw - gb + (gb - bw) / 2, tp, bw, tl, r, hot);
    }
    if (hshow) {
        int tl, tp; BarThumb(D.docW, track, D.sx, tl, tp);
        bool hot = D.hHot || D.hDrag;
        int bw = hot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);
        RoundRectFill(dc, xoff + tp, h - gb + (gb - bw) / 2, tl, bw, r, hot);
    }
}

// One split pane's overlay bars: the vertical thumb hugs THIS pane's right
// edge (pane A's parks against the divider, pane B's against the card's right
// edge) and the horizontal one spans the pane - each pane scrolls on its own,
// so each carries its own pair, same reserved-gutter look as PaintViewBars.
static void PaintPaneBars(HDC dc, DocState& D, bool b, bool vhot, bool hhot) {
    if (!D.n) return;
    int gb = SBar(), r = (int)(3 * g.dpi);
    int x0 = PaneX0(b) + CardX(b), x1 = x0 + CardW(b);   // the bars ride the CARD's own span -
    if (x1 - x0 <= SBar() * 2) return;                   // the same edges the clicks hit, so a
    bool vshow = D.docH > g.ch;                          // thumb is never 8px away from its zone
    int track = (x1 - x0) - (vshow ? gb : 0);            // the content viewport: the v-bar's
    bool hshow = D.docW > track;                         // column comes out of it first
    int gutH = hshow ? gb : 0;
    int ins = BandInset(b);                              // the track starts under the band
    if (vshow) {
        int tl, tp; BarThumb(D.docH, g.ch - ins - gutH, D.sy, tl, tp, D.docH - (g.ch - gutH));
        tp += ins;
        int bw = vhot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);
        RoundRectFill(dc, x1 - gb + (gb - bw) / 2, tp, bw, tl, r, vhot);
    }
    if (hshow) {
        int tl, tp; BarThumb(D.docW, track, D.sx, tl, tp);
        int bw = hhot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);
        RoundRectFill(dc, x0 + tp, g.ch - gb + (gb - bw) / 2, tl, bw, r, hhot);
    }
}

static HBRUSH SplitLineBrush(bool hot) {   // the divider: chrome gray, green while it
    static HBRUSH n = CreateSolidBrush(RGB(0x4a, 0x4a, 0x4a));   // means "grab me"
    static HBRUSH h = CreateSolidBrush(RGB(0x22, 0xbb, 0x56));
    return hot ? h : n;
}

static void ViewWheel(WPARAM wp, LPARAM lp) {
    int d = GET_WHEEL_DELTA_WPARAM(wp);
    POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(g.view, &p);                   // the pane under the wheel scrolls, focused
    bool b = PaneB(p.x);                          // or not - that is what a split means
    FocusPane(b);                                 // the wheel focuses too (KillerPDF's
                                                  // PreviewMouseWheel): zoom and flips must
                                                  // never land in the pane the reader left
    DocState& D = DV(b);                          // this pane's own document answers
    if (LOWORD(wp) & MK_CONTROL) {
        SetZoomAt(D.zoom * std::pow(1.12, d / 120.0), p.x, p.y, b);
    } else if (LOWORD(wp) & MK_SHIFT) {
        ScrollPane(b, D.sx - d * (int)(g.dpi * 80) / 120, D.sy);
    } else {
        // Single mode flips at the edges: scrolling past a page's end brings the
        // next one (KillerPDF's Single keeps one page up and navigates by flip).
        if (D.vm == 1) {
            if (d < 0 && D.sy >= max(0, D.docH - g.ch)) { SetPage(D.cur + 1); return; }
            if (d > 0 && D.sy <= 0) { SetPage(D.cur - 1); return; }
        }
        ScrollPane(b, D.sx, D.sy - d * (int)(g.dpi * 80) / 120);
    }
}

