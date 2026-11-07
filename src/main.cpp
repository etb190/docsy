// Docsy - minimal, fast, native Win32 PDF reader.
// Uses the PDF engine built into Windows (Windows.Data.Pdf): no bundled renderer.
// The main window hosts two children: the page view (right) and the thumbnail
// sidebar (left; drag the splitter between them to resize it). The sidebar is
// fully virtualized: only thumbnails actually in view are rendered (a few per
// timer tick) and off-screen ones are evicted, so a 2,000-page book scrolls as
// light as a 10-page one. No stock scrollbars anywhere: scrolling is wheel,
// drag-pan and keys, and the only bar on screen is a slim KillerPDF-style
// overlay thumb (4px wide, 8px under the cursor) owning an invisible 12px gutter.
// Text selection is KillerPDF's flowing model too: a press on text drags a caret
// pair along the page's reading order (column-aware, line-banded cells from the
// built-in OCR pass over the page) and paints one 60/255 accent quad per
// selected line; select or double-click copies at once; empty page areas still pan.
// The frame is KillerPDF's too: the real OS side borders stay, but the window
// is created without WS_CAPTION (and WM_NCCALCSIZE gives the top row to the
// client), so the native caption can never appear - not even on the first
// paint. A custom 32px title bar (grain, wordmark, 44x32 min/max/close
// buttons with the green/red hovers, the Win11 snap flyout on the maximize
// button and DWM rounded corners) owns the top row; nothing read from the
// document (no file name) is drawn anywhere on it.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <unknwn.h>
#include <objidl.h>
#include <shellapi.h>
#include <shcore.h>
#include <commdlg.h>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <string>
#include <cmath>
#include <climits>
#include <cfloat>
using std::min; using std::max;
#include <gdiplus.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.Media.Ocr.h>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "shcore")
#pragma comment(lib, "gdiplus")
#pragma comment(lib, "comdlg32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "ole32")

using winrt::Windows::Data::Pdf::PdfDocument;
using winrt::Windows::Data::Pdf::PdfPageRenderOptions;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;

struct Ent { HBITMAP b; int w, h; unsigned long long t; };

// ---- flowing text selection (port of KillerPDF's TextRunService + #127) ----
// A page's selectable units - here the OS OCR's per-word boxes over a fresh page
// render (Windows.Data.Pdf exposes no text layer to read directly), in KillerPDF
// the engine's word letters - are grouped into visual lines by vertical band overlap,
// read in column-aware order, and a drag tracks the run of units between two carets
// (page, index) in that reading order. Geometry lives in page-DIP space with the
// top-left origin the OCR boxes report, so painting is the same scale the page
// bitmap already uses. Empty page areas stay with the pan gesture, exactly like
// KillerPDF keeps its marquee for empty space.
struct TxtCell {                      // one selectable unit: an OCR word box
    std::wstring text;
    float l = 0, r = 0, t = 0, b = 0; // page DIPs, top-left origin, y down
    int word = 0, line = 0;           // ordinals assigned when the page flattens
};
struct TxtLine {                      // a contiguous slice of the flattened cells
    int start = 0, count = 0;
    float top = 0, bottom = 0, left = 0, right = 0;
    int end() const { return start + count; }
};
struct TxtPage {
    float w = 0, h = 0;               // page size in DIPs
    unsigned long long tick = 0;      // LRU stamp
    std::vector<TxtCell> cells;
    std::vector<TxtLine> lines;
};
struct FRect { float x, y, w, h; };   // selection quad in page DIPs

struct App {
    HWND main = nullptr, view = nullptr, side = nullptr;
    HINSTANCE hinst = nullptr;
    HICON logo = nullptr;
    PdfDocument doc{ nullptr };
    std::wstring name;
    int n = 0;
    std::vector<float> pw, ph;          // page size in DIPs
    std::vector<int> top;               // pixel top of each page
    double zoom = 1.0, dpi = 1.0;       // zoom: pixels per DIP (x dpi scale)
    bool fit = true, quiet = false, drag = false, busy = false, sideOn = false;
    int sx = 0, sy = 0, cw = 0, ch = 0, docW = 0, docH = 0, gap = 10, maxw = 0, lastVis = 0;
    int cur = -1;                       // current page (view centre), drives the sidebar highlight
    POINT dp{};
    bool vHot = false, vDrag = false, hHot = false, hDrag = false;   // overlay bar states
    int vGrab = 0, hGrab = 0;
    int sideW = 0;                      // user-sized sidebar width (0 = KillerPDF's 180px default)
    bool resizing = false;              // splitter drag: defer refits and thumb rebuilds
    int rsX = 0, rsW0 = 0;
    int osFrame = 0;                    // OS side-border width from WM_NCCALCSIZE (the top resize grip)
    int capHot = -1;                    // hovered caption button: 0 min, 1 max, 2 close, -1 none
    bool capPressed = false;            // the maximize button held down (non-client, SnapLayouts-style)
    std::unordered_map<int, Ent> cache;
    unsigned long long tick = 0;
    HDC bdc = nullptr; HBITMAP bbm = nullptr, bold = nullptr; int bw = 0, bh = 0;
    // flowing text selection: carets over the flattened text cells of a page,
    // plus the lazily built per-page text geometry (LRU-capped like the thumbs)
    std::unordered_map<int, TxtPage> txt;
    bool selArm = false;                // press started on text; the drag is live
    bool selDrag = false;               // past the 4px click-vs-drag threshold
    bool selRange = false;              // a committed selection is on screen
    int selAPage = -1, selACaret = 0;   // anchor caret (page, cell index)
    int selFPage = -1, selFCaret = 0;   // focus caret
    POINT selDownPos{};                 // press point, view pixels
    std::wstring selText;               // committed selection text
};
static App g;

// Sidebar state. Thumbs are small page renders, made lazily and LRU-capped at
// 256, so the sidebar never renders more than the few pages you can see.
struct Side {
    int w = 0, cw = 0, ch = 0, scroll = 0, docH = 0, tw = 0, cur = -1;
    int hit = -1, moved = 0, pend = 0;  // drag-scroll state
    bool barHot = false, barDrag = false;   // the sidebar's own overlay bar
    int barGrab = 0;
    POINT dp{};                         // last drag position
    unsigned long long hold = 0, tick = 0;
    std::vector<int> top;               // pixel top of each sidebar item
    std::unordered_map<int, Ent> th;
    HDC bdc = nullptr; HBITMAP bbm = nullptr, bold = nullptr; int bw = 0, bh = 0;
};
static Side sd;
static HFONT g_uifont = nullptr, g_sidefont = nullptr;
static HFONT g_glyphfont = nullptr, g_wordfont = nullptr;

static int PW(int i) { return max(1, (int)std::lround(g.pw[i] * g.zoom * g.dpi)); }
static int PH(int i) { return max(1, (int)std::lround(g.ph[i] * g.zoom * g.dpi)); }
static int Gap() { return max(4, (int)(10 * g.dpi)); }
static int SBar() { return (int)(12 * g.dpi); }   // overlay bar gutter: KillerPDF's ScrollBarThickness

static HBRUSH GrainBrush(int which);   // defined with the grain tile below

// ---------------- custom caption (KillerPDF's frame) ----------------
// The window keeps WS_THICKFRAME (so Windows still owns the resize borders,
// the drag, the double-click maximize, Aero Snap and the snap flyout) but is
// created WITHOUT WS_CAPTION. A window that still carries WS_CAPTION gets its
// stock title bar painted by DWM on the first activation until a
// minimize/restore forces a frame recalc - the exact launch glitch this
// removes: with no caption style there is nothing left to paint, ever.
// WM_NCCALCSIZE gives the caption row to the client, where the app paints its
// own bar. KillerPDF's tokens: TitleBarHeight 32, buttons 44x32 with a
// transparent face, green #22bb56 hover on min/max and #E04444 on close with
// white glyphs, MDL2 E921/E922/E923/E8BB at 10px.
static int CAP() { return (int)(32 * g.dpi); }

static void CapRects(RECT r[3], int w) {              // 0 min, 1 max, 2 close - right-aligned
    int bw = (int)(44 * g.dpi), bh = CAP();
    r[2] = { w - bw, 0, w, bh };
    r[1] = { w - 2 * bw, 0, w - bw, bh };
    r[0] = { w - 3 * bw, 0, w - 2 * bw, bh };
}

static bool CapHit(POINT c, int w, int& which) {      // c: client coords
    if (c.y < 0 || c.y >= CAP()) return false;
    RECT r[3]; CapRects(r, w);
    for (int i = 0; i < 3; i++)
        if (c.x >= r[i].left && c.x < r[i].right) { which = i; return true; }
    return false;
}

static void InvalidateCap(HWND h) {
    RECT r; GetClientRect(h, &r);
    RECT band{ 0, 0, r.right, CAP() };
    InvalidateRect(h, &band, FALSE);
}

static void SetCapHot(HWND h, int hot) {
    if (g.capHot == hot) return;
    g.capHot = hot;
    if (hot >= 0) SetTimer(h, 3, 250, nullptr);   // while a button is hovered the band also
    else KillTimer(h, 3);                          // repaints on a slow tick, so a surface
    InvalidateCap(h);                              // that dropped it heals without re-hover
}

static void ToggleMax(HWND h) {
    ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
}

// DWM frame attributes (Controls/DwmChrome.cs): rounded corners that square off
// when maximized, and the 1px system border tinted the theme's AppBorderBrush
// #3f3f3f. Both are no-ops before Windows 11 - failures are swallowed there.
// Each attribute is only pushed when its state actually flips: re-setting them
// on every WM_SIZE tick makes DWM re-tessellate the window region mid-resize,
// which can drop freshly painted pixels (the vanishing caption buttons).
static void DwmApply(HWND h) {
    using DwmSetFn = HRESULT (WINAPI*)(HWND, DWORD, const void*, UINT);
    static DwmSetFn pfn = nullptr; static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE d = LoadLibraryW(L"dwmapi.dll");
        if (d) pfn = (DwmSetFn)(void*)GetProcAddress(d, "DwmSetWindowAttribute");
    }
    if (!pfn) return;
    static int s_round = -1;
    int roundc = IsZoomed(h) ? 1 /* DWMWCP_DONOTROUND */ : 2 /* DWMWCP_ROUND */;
    if (roundc != s_round) {
        s_round = roundc;
        pfn(h, 33, &roundc, sizeof roundc);           // DWMWA_WINDOW_CORNER_PREFERENCE
    }
    static bool s_border = false;
    if (!s_border) {
        s_border = true;
        COLORREF border = RGB(0x3f, 0x3f, 0x3f);      // DWMWA_BORDER_COLOR, 0x00BBGGRR
        pfn(h, 34, &border, sizeof border);
    }
}

static HFONT CaptionFont(HFONT& slot, int px, bool bold, const wchar_t* face) {
    if (!slot)
        slot = CreateFontW(-(int)std::lround(px * g.dpi), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                           0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    return slot;
}

// The caption row: grain over #1c1c1c, icon + wordmark at the left ("Docsy"
// in one light tone over its soft dark shadow - KillerPDF's two-tone
// AccentLogo green was dropped), and the three window buttons at the right -
// and nothing read from the document: the file name is deliberately not shown
// anywhere (bar or window text). The bar spans both children, so the main
// window paints it.
static void DrawCaptionBar(HDC dc, int w) {
    RECT bar{ 0, 0, w, CAP() };
    FillRect(dc, &bar, GrainBrush(2));
    SetBkMode(dc, TRANSPARENT);

    // icon + wordmark
    int isz = (int)(16 * g.dpi);
    static HICON s_ic = nullptr; static int s_icsz = 0;   // loaded once per DPI, kept for repaints
    if (s_ic && s_icsz != isz) { DestroyIcon(s_ic); s_ic = nullptr; }
    if (!s_ic) s_ic = (HICON)LoadImageW(g.hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, isz, isz, LR_DEFAULTCOLOR);
    s_icsz = isz;
    if (s_ic) DrawIconEx(dc, (int)(8 * g.dpi), (CAP() - isz) / 2, s_ic, isz, isz, 0, nullptr, DI_NORMAL);
    HFONT of = (HFONT)SelectObject(dc, CaptionFont(g_wordfont, 12, true, L"Segoe UI"));
    int tx = (int)(8 * g.dpi) + isz + (int)(5 * g.dpi);
    SetTextColor(dc, RGB(16, 16, 16));                        // the crisp copy's soft dark shadow
    RECT sh1{ tx + 1, 1, tx + (int)(96 * g.dpi) + 1, CAP() + 1 };
    DrawTextW(dc, L"Docsy", -1, &sh1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    RECT wr{ tx, 0, tx + (int)(96 * g.dpi), CAP() };
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));                  // one flat light tone - the
    DrawTextW(dc, L"Docsy", -1, &wr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    // two-tone AccentLogo colouring (KillerPDF's green "sy") was dropped

    // window buttons
    SelectObject(dc, CaptionFont(g_glyphfont, 10, false, L"Segoe MDL2 Assets"));
    RECT r[3]; CapRects(r, w);
    const wchar_t* glyph[3] = { L"\uE921", IsZoomed(g.main) ? L"\uE923" : L"\uE922", L"\uE8BB" };
    for (int i = 0; i < 3; i++) {
        bool hot = g.capHot == i;
        if (hot) {
            if (i == 2) {                                     // close: solid danger red
                HBRUSH rb = CreateSolidBrush(RGB(0xE0, 0x44, 0x44));
                RECT rr = r[i]; FillRect(dc, &rr, rb); DeleteObject(rb);
            } else {                                          // min/max: the accent green,
                Gdiplus::Rect gr(r[i].left, r[i].top,         // KillerPDF's ButtonFill gradient
                                 r[i].right - r[i].left, r[i].bottom - r[i].top);
                Gdiplus::Graphics gs(dc);
                Gdiplus::LinearGradientBrush gb(gr,
                                                   Gdiplus::Color(255, 0x22, 0xbb, 0x56),
                                                   Gdiplus::Color(255, 0x1d, 0x9d, 0x48), 90.0f);
                gs.FillRectangle(&gb, gr);
            }
        }
        SetTextColor(dc, hot ? RGB(255, 255, 255)
                             : i == 2 ? RGB(0xb6, 0xb6, 0xb6) : RGB(0xe0, 0xe0, 0xe0));
        DrawTextW(dc, glyph[i], -1, &r[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, of);
}

// ---- film-grain margins (port of KillerPDF's procedural GrainTexture) ----
// One seamless 256x256 tile of gray specks: Gaussian around 128, softened with wrapping
// neighbours so the tile repeats without a seam, then contrast-boosted to keep dark and
// bright specks distinct. KillerPDF paints this tile at alpha 70/255 and then scales the
// whole overlay down by its theme resource GrainOpacity - 0.24 on the dark themes - so
// the effective blend is (70/255) * 0.24 ~= 0.066. Baking that product in reproduces the
// rendered frost exactly; painting at the raw alpha 70 reads as harsh TV static. The
// docsy greys are neutral, so the theme-tinted "lean" specks of the original never
// apply. The seed is fixed so every build shows the identical pattern, and the tile stays
// fixed to the window while pages scroll, matching KillerPDF's viewport-fixed overlay.
// Two page-side surfaces share the tile - the page margins (lighter grey) and
// the sidebar (darker) - plus the third, darkest one for the custom title bar
// (KillerPDF's TitleBarBrush #1c1c1c, which carries the same grain overlay).
static HBRUSH g_grainBrush[3] = { nullptr, nullptr, nullptr };
static HBITMAP g_grainBmp[3] = { nullptr, nullptr, nullptr };

static HBRUSH GrainBrush(int which) {
    if (g_grainBrush[which]) return g_grainBrush[which];
    const int S = 256;
    const double Spread = 150.0, Soften = 0.10, Contrast = 1.35;
    const double Alpha = 70.0 / 255.0 * 0.24;   // tile alpha 70 times the dark-theme GrainOpacity
    const BYTE back[3][3] = { { 60, 63, 65 }, { 38, 40, 43 }, { 28, 28, 28 } };  // margins, sidebar, title bar
    auto cl = [](double v) { return v < 0 ? 0.0 : v > 255 ? 255.0 : v; };
    unsigned long long st = 1337;  // splitmix64: deterministic stream, same fixed-seed intent
    auto rnd = [&st]() {
        st += 0x9E3779B97F4A7C15ULL;
        unsigned long long z = st;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return ((z ^ (z >> 31)) >> 11) * (1.0 / 9007199254740992.0);  // [0,1)
    };
    std::vector<double> raw((size_t)S * S);
    for (double& v : raw) {
        double u1 = 1.0 - rnd(), u2 = rnd();
        v = cl(128.0 + Spread * std::sqrt(-2.0 * std::log(u1)) * std::cos(2 * 3.14159265358979323846 * u2));
    }
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), S, S, 1, 32, BI_RGB };
    BYTE* px = nullptr;
    g_grainBmp[which] = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    if (!px || !g_grainBmp[which]) return (HBRUSH)GetStockObject(GRAY_BRUSH);
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            double around = (raw[((y + S - 1) % S) * S + x] + raw[((y + 1) % S) * S + x]
                           + raw[y * S + (x + S - 1) % S] + raw[y * S + (x + 1) % S]) / 4.0;
            double soft = raw[y * S + x] * (1.0 - Soften) + around * Soften;
            double shade = cl(128.0 + (soft - 128.0) * Contrast);
            BYTE* p = px + ((size_t)y * S + x) * 4;
            for (int c = 0; c < 3; c++) p[c] = (BYTE)std::lround(back[which][c] * (1.0 - Alpha) + shade * Alpha);
            p[3] = 255;
        }
    }
    g_grainBrush[which] = CreatePatternBrush(g_grainBmp[which]);
    if (!g_grainBrush[which]) return (HBRUSH)GetStockObject(GRAY_BRUSH);
    return g_grainBrush[which];
}

static void ClearCache() {
    for (auto& kv : g.cache) DeleteObject(kv.second.b);
    g.cache.clear();
}

static HFONT UiFont() {
    if (!g_uifont) {
        g_uifont = CreateFontW(-MulDiv(9, (int)(g.dpi * 96), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    return g_uifont;
}

static HBITMAP Render(int i, int w, int h) {
    try {
        auto page = g.doc.GetPage((uint32_t)i);
        PdfPageRenderOptions o;
        o.DestinationWidth((uint32_t)w);
        o.DestinationHeight((uint32_t)h);
        o.BackgroundColor(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
        o.BitmapEncoderId(winrt::Windows::Graphics::Imaging::BitmapEncoder::BmpEncoderId()); // raw BMP: no PNG cost
        InMemoryRandomAccessStream st;
        page.RenderToStreamAsync(st, o).get();
        st.Seek(0);
        winrt::com_ptr<IStream> is;
        winrt::check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(st), winrt::guid_of<IStream>(), is.put_void()));
        Gdiplus::Bitmap bmp(is.get());
        if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;
        HBITMAP hb = nullptr;
        bmp.GetHBITMAP(Gdiplus::Color(255, 255, 255), &hb);
        return hb;
    } catch (...) { return nullptr; }
}

static HBITMAP GetBmp(int i, int w, int h) {
    auto it = g.cache.find(i);
    if (it != g.cache.end()) {
        if (it->second.w == w && it->second.h == h) { it->second.t = ++g.tick; return it->second.b; }
        DeleteObject(it->second.b); g.cache.erase(it);
    }
    HBITMAP b = Render(i, w, h);
    if (!b) return nullptr;
    g.cache[i] = Ent{ b, w, h, ++g.tick };
    return b;
}

// The same render as Render(), decoded as a SoftwareBitmap - the input format the
// OS OCR engine consumes for the text-selection pass.
static winrt::Windows::Graphics::Imaging::SoftwareBitmap RenderSoft(int i, int w, int h) {
    try {
        auto page = g.doc.GetPage((uint32_t)i);
        PdfPageRenderOptions o;
        o.DestinationWidth((uint32_t)w);
        o.DestinationHeight((uint32_t)h);
        o.BackgroundColor(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
        o.BitmapEncoderId(winrt::Windows::Graphics::Imaging::BitmapEncoder::BmpEncoderId());
        InMemoryRandomAccessStream st;
        page.RenderToStreamAsync(st, o).get();
        st.Seek(0);
        auto dec = winrt::Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(st).get();
        return dec.GetSoftwareBitmapAsync().get();
    } catch (...) { return nullptr; }
}

static void Evict(int lo, int hi) {
    while (g.cache.size() > 24) {
        int k = -1; unsigned long long best = ~0ULL;
        for (auto& kv : g.cache)
            if ((kv.first < lo - 1 || kv.first > hi + 1) && kv.second.t < best) { best = kv.second.t; k = kv.first; }
        if (k < 0) break;
        DeleteObject(g.cache[k].b); g.cache.erase(k);
    }
}

static int PageIn(const std::vector<int>& top, int n, int y) {
    if (n <= 0 || top.empty()) return 0;
    auto it = std::upper_bound(top.begin(), top.begin() + n, y);
    return std::clamp((int)(it - top.begin()) - 1, 0, n - 1);
}
static int PageAt(int y) { return PageIn(g.top, g.n, y); }

// ---------------- text selection core (verbatim-portable geometry) ----------------
// Everything in this block is pure std:: - the same functions are compiled by
// scripts/text_check.cpp on Linux against synthetic pages.

// Group cells into visual lines: a cell joins a line when its vertical band overlaps
// the line's band by at least half the smaller height, and bands grow as members join.
// Then KillerPDF's #185 column-aware reading order: each band is split at gutter-sized
// gaps into segments; narrow segments cluster into columns by X-interval overlap and
// whole columns emit left-to-right (top-to-bottom inside each), while wide segments -
// titles and footers spanning the text width - flush the open column first. Windows.Data.Pdf
// exposes no writing directions, so the original's vertical-text ordering and RTL mirroring
// have nothing to key on and are not ported; lines read left-to-right.
static void TtLines(TxtPage& pt) {
    if (pt.cells.empty()) return;
    struct Band { std::vector<int> idx; float top, bottom; };
    std::vector<Band> bands;
    for (int i = 0; i < (int)pt.cells.size(); i++) {
        const TxtCell& c = pt.cells[i];
        int found = -1;
        for (int k = 0; k < (int)bands.size() && found < 0; k++) {
            float ov = min(bands[k].bottom, c.b) - max(bands[k].top, c.t);
            float mh = min(bands[k].bottom - bands[k].top, c.b - c.t);
            if (mh > 0 && ov >= mh * 0.5f) found = k;
        }
        if (found < 0) bands.push_back({ {i}, c.t, c.b });
        else {
            bands[found].idx.push_back(i);
            bands[found].top = min(bands[found].top, c.t);
            bands[found].bottom = max(bands[found].bottom, c.b);
        }
    }
    std::sort(bands.begin(), bands.end(), [](const Band& a, const Band& b) { return a.top < b.top; });

    float textL = FLT_MAX, textR = -FLT_MAX;
    for (const TxtCell& c : pt.cells) { textL = min(textL, c.l); textR = max(textR, c.r); }
    float wideW = (textR - textL) * 0.62f;   // spans most of the text width = not a column line

    std::vector<Band> ordered;
    struct Seg { std::vector<int> idx; float top, bottom, l, r; };
    std::vector<Seg> pending;
    auto flush = [&]() {
        if (pending.empty()) return;
        struct Col { float l, r; std::vector<int> idx; };
        std::vector<Col> cols;
        std::vector<int> byLeft(pending.size());
        for (int i = 0; i < (int)pending.size(); i++) byLeft[i] = i;
        std::sort(byLeft.begin(), byLeft.end(), [&](int a, int b) { return pending[a].l < pending[b].l; });
        for (int i : byLeft) {
            int hit = -1;
            for (int c = 0; c < (int)cols.size() && hit < 0; c++) {
                float ov = min(cols[c].r, pending[i].r) - max(cols[c].l, pending[i].l);
                float mw = min(cols[c].r - cols[c].l, pending[i].r - pending[i].l);
                if (mw > 0 && ov >= mw * 0.5f) hit = c;
            }
            if (hit < 0) cols.push_back({ pending[i].l, pending[i].r, {i} });
            else {
                cols[hit].idx.push_back(i);
                cols[hit].l = min(cols[hit].l, pending[i].l);
                cols[hit].r = max(cols[hit].r, pending[i].r);
            }
        }
        std::sort(cols.begin(), cols.end(), [](const Col& a, const Col& b) { return a.l < b.l; });
        for (Col& col : cols) {
            std::sort(col.idx.begin(), col.idx.end(), [&](int a, int b) { return pending[a].top < pending[b].top; });
            for (int i : col.idx) ordered.push_back({ pending[i].idx, pending[i].top, pending[i].bottom });
        }
        pending.clear();
    };
    for (const Band& band : bands) {
        std::vector<int> sorted = band.idx;
        std::sort(sorted.begin(), sorted.end(), [&](int a, int b) { return pt.cells[a].l < pt.cells[b].l; });
        // split threshold: well past a word space (~0.25em) but below a column gutter
        float tw = 0; int tn = 0;
        for (int i : sorted) { tw += pt.cells[i].r - pt.cells[i].l; tn += max(1, (int)pt.cells[i].text.size()); }
        float gapT = max(10.0f, (tn > 0 ? tw / tn : 5.0f) * 3);
        std::vector<Seg> segs;
        segs.push_back({ {sorted[0]}, 0, 0, 0, 0 });
        for (size_t k = 1; k < sorted.size(); k++) {
            if (pt.cells[sorted[k]].l - pt.cells[sorted[k - 1]].r > gapT) segs.push_back({ {}, 0, 0, 0, 0 });
            segs.back().idx.push_back(sorted[k]);
        }
        for (Seg& seg : segs) {
            float sT = FLT_MAX, sB = -FLT_MAX, sL = FLT_MAX, sR = -FLT_MAX;
            for (int i : seg.idx) {
                const TxtCell& c = pt.cells[i];
                sT = min(sT, c.t); sB = max(sB, c.b); sL = min(sL, c.l); sR = max(sR, c.r);
            }
            seg.top = sT; seg.bottom = sB; seg.l = sL; seg.r = sR;
            if (sR - sL >= wideW) { flush(); ordered.push_back({ seg.idx, sT, sB }); }
            else pending.push_back(seg);
        }
    }
    flush();

    // flatten in reading order; cells inside a line read left-to-right, word ordinals
    // restart where a space sits on either side of a join or the gap is a real word
    // space (a narrow gap with no spaces is one word split across range boxes)
    std::vector<TxtCell> flat; flat.reserve(pt.cells.size());
    int lineNo = 0, wordNo = 0;
    for (const Band& band : ordered) {
        std::vector<int> mem = band.idx;
        std::sort(mem.begin(), mem.end(), [&](int a, int b) { return pt.cells[a].l < pt.cells[b].l; });
        float bandH = max(1.0f, band.bottom - band.top);
        TxtLine ln; ln.start = (int)flat.size();
        float lt = FLT_MAX, lb = -FLT_MAX, ll = FLT_MAX, lr = -FLT_MAX;
        for (int i : mem) {
            TxtCell f = pt.cells[i];
            if (ln.count == 0) f.word = wordNo++;                    // a new line starts a word
            else {
                const TxtCell& p = flat.back();
                bool sp = (!p.text.empty() && p.text.back() == L' ') || (!f.text.empty() && f.text.front() == L' ');
                if (!sp && f.l - p.r <= bandH * 0.3f) f.word = p.word;
                else f.word = wordNo++;
            }
            f.line = lineNo;
            lt = min(lt, f.t); lb = max(lb, f.b); ll = min(ll, f.l); lr = max(lr, f.r);
            flat.push_back(std::move(f));
            ln.count++;
        }
        ln.top = lt; ln.bottom = lb; ln.left = ll; ln.right = lr;
        pt.lines.push_back(ln);
        lineNo++;
    }
    pt.cells = std::move(flat);
}

// True when the point sits ON text: inside a line's vertical band and within its
// horizontal extent (small slop). This is the gate that decides flowing selection
// vs the pan gesture - empty page areas must keep the pan.
static bool TtOverText(const TxtPage& pt, float x, float y) {
    const float slop = 2.0f;   // DIPs
    for (const TxtLine& ln : pt.lines)
        if (y <= ln.bottom + slop && y >= ln.top - slop &&
            x >= ln.left - slop && x <= ln.right + slop)
            return true;
    return false;
}

// Caret (0..cells) nearest a point, browser-style clamping: above the first line
// selects from the page start, below the last to the page end, between lines snaps
// to the closer line, beyond a line's ends clamps to its ends. (KillerPDF's
// CaretFromPoint at cell granularity; y grows downward in DIP space.)
static int TtCaret(const TxtPage& pt, float x, float y) {
    if (pt.lines.empty()) return 0;
    const TxtLine* target = nullptr;
    float bestV = FLT_MAX, bestH = FLT_MAX;
    for (const TxtLine& ln : pt.lines) {
        float v = y < ln.top ? ln.top - y : y > ln.bottom ? y - ln.bottom : 0;
        float h = x < ln.left ? ln.left - x : x > ln.right ? x - ln.right : 0;
        if (v < bestV || (v == bestV && h < bestH)) { bestV = v; bestH = h; target = &ln; }
    }
    if (!target) return 0;
    const TxtLine& first = pt.lines.front();
    const TxtLine& last = pt.lines.back();
    if (y < first.top && target == &first && x < first.left) return 0;
    if (y > last.bottom && target == &last && x > last.right) return (int)pt.cells.size();
    if (x <= target->left) return target->start;
    if (x >= target->right) return target->end();
    for (int i = target->start; i < target->end(); i++) {
        const TxtCell& c = pt.cells[i];
        if (x < (c.l + c.r) * 0.5f) return i;
    }
    return target->end();
}

// Caret range of the word (cell) under a point, or false when the point is outside
// selectable text - the double-click word selection.
static bool TtWordRange(const TxtPage& pt, float x, float y, int& s, int& e) {
    if (!TtOverText(pt, x, y)) return false;
    int best = -1; float bestD = FLT_MAX;
    for (int i = 0; i < (int)pt.cells.size(); i++) {
        const TxtCell& c = pt.cells[i];
        const TxtLine& ln = pt.lines[c.line];
        if (y < ln.top - 2 || y > ln.bottom + 2 || x < ln.left - 2 || x > ln.right + 2) continue;
        float d = x < c.l ? c.l - x : x > c.r ? x - c.r : 0;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return false;
    int word = pt.cells[best].word;
    s = best; e = best + 1;
    while (s > 0 && pt.cells[s - 1].word == word) s--;
    while (e < (int)pt.cells.size() && pt.cells[e].word == word) e++;
    return true;
}

// Per-line selection rects covering carets [start, end): one rect per line, first
// selected cell to last, padded 12% of the line height - KillerPDF's SelectionBounds.
static void TtRects(const TxtPage& pt, int start, int end, std::vector<FRect>& out) {
    int i = max(0, start);
    end = min(end, (int)pt.cells.size());
    while (i < end) {
        const TxtLine& ln = pt.lines[pt.cells[i].line];
        int segEnd = min(end, ln.end());
        float left = FLT_MAX, right = -FLT_MAX;
        for (int k = i; k < segEnd; k++) { left = min(left, pt.cells[k].l); right = max(right, pt.cells[k].r); }
        float pad = (ln.bottom - ln.top) * 0.12f;
        out.push_back({ left, ln.top - pad, right - left, (ln.bottom + pad) - (ln.top - pad) });
        i = segEnd;
    }
}

// Text for the caret range [start, end): newlines between lines, single spaces
// between words; a cell's own text (which may contain spaces) is appended verbatim.
// Also reports how many distinct words the range touches.
static std::wstring TtText(const TxtPage& pt, int start, int end, int* words) {
    if (words) *words = 0;
    std::wstring out;
    int lastWord = -1, lastLine = -1;
    for (int i = max(0, start); i < min(end, (int)pt.cells.size()); i++) {
        const TxtCell& c = pt.cells[i];
        if (lastLine >= 0 && c.line != lastLine) out += L'\n';
        else if (lastWord >= 0 && c.word != lastWord) out += L' ';
        if (c.word != lastWord && words) (*words)++;
        out += c.text;
        lastWord = c.word; lastLine = c.line;
    }
    return out;
}

// ---------------- text selection app glue ----------------

// Per-page text geometry, built lazily on first touch (cursor, press or a selection
// slice) and LRU-capped at 48 pages, so a 2,000-page book never pre-extracts.
static TxtPage* TtFor(int i) {
    if (!g.n || i < 0 || i >= g.n) return nullptr;
    auto it = g.txt.find(i);
    if (it != g.txt.end()) { it->second.tick = ++g.tick; return &it->second; }
    TxtPage pt; pt.w = g.pw[i]; pt.h = g.ph[i];
    try {
        // Windows.Data.Pdf exposes no text layer, so the selectable cells come from the
        // built-in OCR pass (Windows.Media.Ocr) over a fresh 2x render: one box per
        // recognized word, scaled back to page DIPs, feeding KillerPDF's exact cell
        // pipeline below. Built lazily on first touch and LRU-capped, so a 2,000-page
        // book never OCRs what you never touch; no OCR language pack means selection
        // is quietly off and the page keeps its pan gesture.
        static winrt::Windows::Media::Ocr::OcrEngine s_ocr = nullptr;
        static bool s_tried = false;
        if (!s_tried) {
            s_tried = true;
            s_ocr = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
        }
        if (s_ocr) {
            int ow = std::clamp((int)std::lround(g.pw[i] * 2), 800, 1600);
            int oh = max(1, (int)std::lround(ow * g.ph[i] / max(1.f, g.pw[i])));
            auto soft = RenderSoft(i, ow, oh);
            if (soft) {
                auto res = s_ocr.RecognizeAsync(soft).get();
                float kx = pt.w / ow, ky = pt.h / oh;
                for (winrt::Windows::Media::Ocr::OcrLine line : res.Lines())
                    for (winrt::Windows::Media::Ocr::OcrWord wd : line.Words()) {
                        auto r = wd.BoundingRect();
                        if (r.Width <= 0 || r.Height <= 0 || wd.Text().empty()) continue;
                        TxtCell c;
                        c.text = std::wstring(wd.Text());
                        c.l = r.X * kx; c.t = r.Y * ky;
                        c.r = c.l + r.Width * kx; c.b = c.t + r.Height * ky;
                        pt.cells.push_back(std::move(c));
                    }
            }
        }
    } catch (...) { /* no OCR pack / render failure: selection is just not offered here */ }
    TtLines(pt);
    pt.tick = ++g.tick;
    g.txt[i] = std::move(pt);
    while (g.txt.size() > 48) {           // LRU: drop the stalest page's geometry
        auto oldest = g.txt.end();
        for (auto x = g.txt.begin(); x != g.txt.end(); ++x)
            if (x->first != i && (oldest == g.txt.end() || x->second.tick < oldest->second.tick)) oldest = x;
        if (oldest == g.txt.end()) break;
        g.txt.erase(oldest);
    }
    return &g.txt[i];
}

static void SelClear() {
    bool had = g.selRange || g.selArm || g.selDrag || !g.selText.empty();
    g.selArm = g.selDrag = g.selRange = false;
    g.selAPage = g.selFPage = -1;
    g.selACaret = g.selFCaret = 0;
    g.selText.clear();
    if (had && g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// The anchor->focus pair in reading order.
static void SelOrdered(int& sp, int& sc, int& ep, int& ec) {
    bool aFirst = g.selAPage < g.selFPage || (g.selAPage == g.selFPage && g.selACaret <= g.selFCaret);
    if (aFirst) { sp = g.selAPage; sc = g.selACaret; ep = g.selFPage; ec = g.selFCaret; }
    else { sp = g.selFPage; sc = g.selFCaret; ep = g.selAPage; ec = g.selACaret; }
}

// The caret slice of the selection that falls on one page, or (0,0) when none.
static void SelSliceForPage(int page, int& start, int& end) {
    start = end = 0;
    int sp, sc, ep, ec;
    SelOrdered(sp, sc, ep, ec);
    if (page < sp || page > ep) return;
    TxtPage* pt = TtFor(page);
    int n = pt ? (int)pt->cells.size() : 0;
    start = page == sp ? sc : 0;
    end = page == ep ? ec : n;
    start = std::clamp(start, 0, n);
    end = std::clamp(end, 0, n);
    if (start > end) std::swap(start, end);
}

// Cross-page text: one page's slice per line group, pages joined with '\n'
// (KillerPDF's BuildSelectedText).
static std::wstring SelBuildText(int* words) {
    if (words) *words = 0;
    int sp, sc, ep, ec;
    SelOrdered(sp, sc, ep, ec);
    std::wstring out;
    for (int p = sp; p <= ep; p++) {
        TxtPage* pt = TtFor(p);
        if (!pt || pt->cells.empty()) continue;
        int s, e;
        SelSliceForPage(p, s, e);
        if (s >= e) continue;
        int w = 0;
        std::wstring t = TtText(*pt, s, e, &w);
        if (t.empty()) continue;
        if (!out.empty()) out += L'\n';
        out += t;
        if (words) *words += w;
    }
    return out;
}

static void CopyClipboard(const std::wstring& s) {
    if (!OpenClipboard(g.main)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        void* m = GlobalLock(hg);
        if (m) {
            memcpy(m, s.c_str(), bytes);
            GlobalUnlock(hg);
            if (!SetClipboardData(CF_UNICODETEXT, hg)) GlobalFree(hg);
        } else GlobalFree(hg);
    }
    CloseClipboard();
}

// Ctrl+A: flowing select-all on the current page and copy (KillerPDF's SelectAllText).
static void SelAll() {
    if (!g.n) return;
    int pg = PageAt(g.sy + g.ch / 3);
    TxtPage* pt = TtFor(pg);
    if (!pt || pt->cells.empty()) return;
    SelClear();
    g.selAPage = g.selFPage = pg;
    g.selACaret = 0;
    g.selFCaret = (int)pt->cells.size();
    g.selRange = true;
    g.selText = TtText(*pt, 0, (int)pt->cells.size(), nullptr);
    if (!g.selText.empty()) CopyClipboard(g.selText);
    InvalidateRect(g.view, nullptr, FALSE);
}

// The view->page transform, mirroring Draw()'s page placement exactly: which page a
// view point falls on and where it sits inside that page, in DIPs. The point is
// clamped into the page, so dragging past an edge parks the caret at the start or
// end of the nearest line instead of losing the selection (KillerPDF clamps the
// drag point into the page canvas the same way).
static bool ViewToPage(POINT v, int w, int h, int& page, float& dx, float& dy) {
    if (!g.n) return false;
    int gut = g.docH > h ? SBar() : 0;
    int offx = g.docW < w - gut ? (w - gut - g.docW) / 2 : -g.sx;
    int i = PageAt(v.y + g.sy);
    if (i < 0 || i >= g.n) return false;
    float k = (float)(g.zoom * g.dpi);
    if (k <= 0) return false;
    page = i;
    dx = (v.x - (offx + g.gap + (g.maxw - PW(i)) / 2)) / k;
    dy = (v.y - (g.top[i] - g.sy)) / k;
    dx = std::clamp(dx, 0.0f, (float)PW(i) / k);
    dy = std::clamp(dy, 0.0f, (float)PH(i) / k);
    return true;
}

static void SideSync();
static void LayoutMain();

static void UpdateTitle() {   // sidebar highlight follows the page under the upper
    int c = g.n ? PageAt(g.sy + g.ch / 3) : -1;   // third. The window text stays "Docsy"
    if (c != g.cur) { g.cur = c; SideSync(); }    // - the open file's name shows nowhere.
}

static void UpdateScroll() {
    g.busy = true;
    g.sy = std::clamp(g.sy, 0, max(0, g.docH - g.ch));
    g.sx = std::clamp(g.sx, 0, max(0, g.docW - g.cw));
    g.busy = false;
}

static void Layout() {
    g.gap = Gap();
    g.top.assign((size_t)g.n + 1, 0);
    int y = g.gap; g.maxw = 0;
    for (int i = 0; i < g.n; i++) { g.top[i] = y; y += PH(i) + g.gap; g.maxw = max(g.maxw, PW(i)); }
    g.top[g.n] = y;
    g.docH = g.n ? y : 0;
    g.docW = g.n ? g.maxw + 2 * g.gap : 0;
    UpdateScroll();
    InvalidateRect(g.view, nullptr, FALSE);
    UpdateTitle();
}

static void SetZoom(double z, bool fit, int ax, int ay) {
    z = std::clamp(z, 0.1, 4.0);
    double ry = g.docH ? (double)(g.sy + ay) / g.docH : 0, rx = g.docW ? (double)(g.sx + ax) / g.docW : 0;
    if (z != g.zoom) ClearCache();
    g.zoom = z; g.fit = fit;
    Layout();
    g.sy = (int)(ry * g.docH) - ay;
    if (g.docW > g.cw) g.sx = (int)(rx * g.docW) - ax;
    UpdateScroll();
    InvalidateRect(g.view, nullptr, FALSE);
    UpdateTitle();
}

static void ApplyFit(bool pageFit = false) {
    if (!g.n || g.cw <= 0) return;
    float mw = 0; for (float w : g.pw) mw = max(mw, w);
    int W = g.cw - SBar();   // the reserved overlay-bar column, as the old scrollbar subtracted itself
    double z = (W - 2.0 * Gap()) / (mw * g.dpi);
    if (pageFit) {
        int c = PageAt(g.sy + g.ch / 3);
        z = min(z, (g.ch - 2.0 * Gap()) / (g.ph[c] * g.dpi));
    }
    SetZoom(z, !pageFit, 0, 0);
}

static void ScrollTo(int x, int y) {
    x = std::clamp(x, 0, max(0, g.docW - g.cw));
    y = std::clamp(y, 0, max(0, g.docH - g.ch));
    if (x == g.sx && y == g.sy) return;
    g.sx = x; g.sy = y;
    InvalidateRect(g.view, nullptr, FALSE);
    UpdateTitle();
}

static bool Open(const std::wstring& p) {
    try {
        wchar_t full[MAX_PATH * 4];
        GetFullPathNameW(p.c_str(), (DWORD)_countof(full), full, nullptr);
        auto f = StorageFile::GetFileFromPathAsync(full).get();
        auto d = PdfDocument::LoadFromFileAsync(f).get();
        uint32_t n = d.PageCount();
        if (!n) throw std::runtime_error("empty");
        std::vector<float> w(n, 612.f), h(n, 792.f);
        for (uint32_t i = 0; i < n; i++) {
            try { auto s = d.GetPage(i).Size(); w[i] = s.Width; h[i] = s.Height; } catch (...) {}
        }
        std::wstring nm = full;
        size_t sl = nm.find_last_of(L"\\/");
        if (sl != std::wstring::npos) nm = nm.substr(sl + 1);
        ClearCache();
        g.doc = d; g.n = (int)n; g.pw = w; g.ph = h; g.name = nm;
        g.sx = g.sy = 0; g.fit = true;
        // fresh sidebar for the new document
        for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
        sd.th.clear(); sd.scroll = 0; sd.cur = -1; sd.hold = 0; sd.tw = 0; sd.cw = 0; sd.ch = 0;
        g.txt.clear();               // the new document's text geometry starts fresh
        SelClear();
        g.sideOn = true;
        if (g.side) ShowWindow(g.side, SW_SHOWNOACTIVATE);
        LayoutMain();
        InvalidateRect(g.main, nullptr, FALSE);
        return true;
    } catch (...) {
        if (!g.quiet) MessageBoxW(g.main, L"Could not open this file (invalid, encrypted or unsupported PDF).", L"Docsy", MB_ICONWARNING);
        return false;
    }
}

static void OpenDlg() {
    wchar_t f[MAX_PATH * 2] = L"";
    OPENFILENAMEW o{}; o.lStructSize = sizeof o; o.hwndOwner = g.main;
    o.lpstrFilter = L"PDF files\0*.pdf\0All files\0*.*\0";
    o.lpstrFile = f; o.nMaxFile = (DWORD)_countof(f);
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) Open(f);
}

static void HandleDrop(HDROP d) {
    wchar_t f[MAX_PATH * 2];
    if (DragQueryFileW(d, 0, f, (UINT)_countof(f))) Open(f);
    DragFinish(d);
}

// ---------------- sidebar ----------------

static int SW() {                                                     // sidebar window width
    int w = g.sideW ? g.sideW : (int)(180 * g.dpi);                   // KillerPDF's pages-panel default
    int lo = (int)(120 * g.dpi);                                      // nMinColumn
    int hi = (int)(234 * g.dpi);                                      // nMaxPages (KillerPDF's pages cap)
    RECT r;
    if (g.main && GetClientRect(g.main, &r) && r.right > 0)
        hi = max(lo, min(hi, (int)r.right - (int)(160 * g.dpi)));
    return std::clamp(w, lo, hi);
}
static int STH(int i) { return max(1, (int)std::lround(sd.tw * g.ph[i] / max(1.f, g.pw[i]))); }
static int SidePageAt(int y) { return PageIn(sd.top, g.n, y); }

static void SideScrollTo(int y) {
    y = std::clamp(y, 0, max(0, sd.docH - sd.ch));
    if (y == sd.scroll) return;
    sd.scroll = y;
    if (g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE)) {
        InvalidateRect(g.side, nullptr, FALSE);
        SetTimer(g.side, 2, 25, nullptr);   // pump newly visible thumbs
    }
}

static void SideLayout() {
    if (!g.n || sd.cw <= 0) { sd.docH = 0; return; }
    if (!g.resizing) {                             // splitter drags keep the old pitch and thumbs;
        int tw = (int)(96 * g.dpi);                // fixed-size thumbs, centred: smaller than
        if (sd.tw != tw) {                         // KillerPDF's ~134px stretch-to-width card
            sd.tw = tw;
            for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
            sd.th.clear();
        }
        int lab = (int)(20 * g.dpi), gp = (int)(8 * g.dpi);   // 2px gap + label, 8px tail: the 8,6 / 0,2 rhythm
        sd.top.assign((size_t)g.n + 1, 0);
        int y = (int)(8 * g.dpi);
        for (int i = 0; i < g.n; i++) { sd.top[i] = y; y += STH(i) + lab + gp; }
        sd.top[g.n] = y;
        sd.docH = y - gp + (int)(8 * g.dpi);
    }
    sd.scroll = std::clamp(sd.scroll, 0, max(0, sd.docH - sd.ch));
    InvalidateRect(g.side, nullptr, FALSE);
    SetTimer(g.side, 2, 25, nullptr);
}

// Renders the visible thumbnail closest to the viewport centre. Returns true
// while visible thumbs are still missing, so a 25 ms timer can pump the rest.
static bool SidePump() {
    if (!g.n || sd.ch <= 0 || sd.docH <= 0) return false;
    if (!(g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE))) return false;   // nothing to pump while hidden (shot mode shows it first)
    int first = SidePageAt(sd.scroll), last = first;
    while (last + 1 < g.n && sd.top[last + 1] < sd.scroll + sd.ch) last++;
    int pick = -1, bestd = INT_MAX, center = (first + last) / 2;
    for (int i = max(0, first - 1); i <= min(g.n - 1, last + 1); i++) {
        if (sd.th.find(i) != sd.th.end()) continue;
        int d = abs(i - center);
        if (d < bestd) { bestd = d; pick = i; }
    }
    if (pick < 0) return false;
    HBITMAP b = Render(pick, sd.tw, STH(pick));
    sd.th[pick] = Ent{ b, sd.tw, STH(pick), ++sd.tick };   // b may be null: stays a placeholder, no retry spin
    while (sd.th.size() > 256) {                            // LRU: drop farthest off-screen, oldest first
        int k = -1; unsigned long long best = ~0ULL;
        for (auto& kv : sd.th)
            if ((kv.first < first - 2 || kv.first > last + 2) && kv.second.t < best) { best = kv.second.t; k = kv.first; }
        if (k < 0) break;
        if (sd.th[k].b) DeleteObject(sd.th[k].b);
        sd.th.erase(k);
    }
    InvalidateRect(g.side, nullptr, FALSE);
    return true;
}

// Keeps the sidebar highlight in step with the view and follows the current
// page. The follow is edge-reversed on purpose: a selection that runs off the
// bottom of the rail is scrolled flush to the TOP edge (the pages after it
// stay in sight below), and one that runs off the top lands flush at the
// bottom edge - instead of the usual ensure-visible, which anchored it the
// other way around and parked the selected card at the bottom of the rail.
static void SideSync() {
    if (!g.side || !g.n) { sd.cur = -1; return; }
    int c = g.cur;
    if (c == sd.cur) return;
    sd.cur = c;
    if (c >= 0 && sd.docH > 0 && GetTickCount64() >= sd.hold) {
        int y0 = sd.top[c], y1 = sd.top[c + 1];
        if (y1 > sd.scroll + sd.ch) SideScrollTo(y0 - (int)(4 * g.dpi));          // off the bottom: card at the top
        else if (y0 < sd.scroll) SideScrollTo(y1 - sd.ch + (int)(4 * g.dpi));     // off the top: card at the bottom
    }
    if (g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE)) InvalidateRect(g.side, nullptr, FALSE);
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
static void BarThumb(int docLen, int viewLen, int scroll, int& tlen, int& tpos) {
    if (docLen <= viewLen || docLen <= 0 || viewLen <= 0) { tlen = 0; tpos = 0; return; }
    int t = (int)((long long)viewLen * viewLen / docLen);
    int fl = (int)min(64.0 * g.dpi, viewLen * 0.5);
    tlen = std::clamp(t, fl, viewLen);
    tpos = std::clamp((int)((long long)(viewLen - tlen) * max(0, scroll) / (docLen - viewLen)), 0, viewLen - tlen);
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

static HFONT SideFont() {   // KillerPDF's page labels: 14px
    if (!g_sidefont)
        g_sidefont = CreateFontW(-(int)(14 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    return g_sidefont;
}

static void TrackLeave(HWND h) { TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t); }

// Paints the sidebar rail: darker noisy background, thumbs (or page-shaped
// placeholders while they render), page numbers, current-page accent ring, and
// the rail's own overlay bar inside the reserved 12px gutter on the right.
static void SideDraw(HDC dc, int w, int h, int xoff) {
    RECT r{ xoff, 0, xoff + w, h };
    FillRect(dc, &r, GrainBrush(1));
    if (!g.n || sd.cw <= 0 || sd.top.empty()) return;
    int avail = sd.cw - SBar();
    COLORREF accent = RGB(139, 92, 246);
    HGDIOBJ of = SelectObject(dc, SideFont());
    SetBkMode(dc, TRANSPARENT);
    HDC m = CreateCompatibleDC(dc);
    int first = SidePageAt(sd.scroll);
    for (int i = first; i < g.n && sd.top[i] < sd.scroll + h; i++) {
        int th = STH(i);
        int x = xoff + (avail - sd.tw) / 2;
        int y = sd.top[i] - sd.scroll;
        RECT tr{ x, y, x + sd.tw, y + th };
        auto it = sd.th.find(i);
        if (it != sd.th.end() && it->second.b) {
            HGDIOBJ o = SelectObject(m, it->second.b);
            BitBlt(dc, x, y, sd.tw, th, m, 0, 0, SRCCOPY);
            SelectObject(m, o);
            FrameRect(dc, &tr, (HBRUSH)GetStockObject(BLACK_BRUSH));
        } else {
            FillRect(dc, &tr, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
            FrameRect(dc, &tr, (HBRUSH)GetStockObject(GRAY_BRUSH));
        }
        if (i == sd.cur) {
            RECT a{ x - 2, y - 2, x + sd.tw + 2, y + th + 2 };
            HBRUSH ab = CreateSolidBrush(accent);
            FrameRect(dc, &a, ab);
            InflateRect(&a, -1, -1);
            FrameRect(dc, &a, ab);
            DeleteObject(ab);
        }
        wchar_t num[16];
        swprintf_s(num, L"%d", i + 1);
        RECT nr{ xoff, y + th + 2, xoff + avail, y + th + 2 + (int)(18 * g.dpi) };
        SetTextColor(dc, i == sd.cur ? RGB(178, 146, 255) : RGB(176, 178, 184));
        DrawTextW(dc, num, -1, &nr, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    DeleteDC(m);
    SelectObject(dc, of);
    RECT div{ xoff + w - 1, 0, xoff + w, h };
    FillRect(dc, &div, (HBRUSH)GetStockObject(BLACK_BRUSH));
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
            SideDraw(sd.bdc, sd.cw, sd.ch, 0);
            BitBlt(dc, 0, 0, sd.cw, sd.ch, sd.bdc, 0, 0, SRCCOPY);
            SetTimer(h, 2, 25, nullptr);   // finish any pending thumbs
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 2 && !SidePump()) KillTimer(h, 2);
        return 0;
    case WM_SIZE: return 0;
    case WM_MOUSEWHEEL:
        SideScrollTo(sd.scroll - GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetCapture(h);
        int gb = SBar();
        if (g.n && sd.docH > sd.ch && p.x >= sd.cw - gb && p.x < sd.cw - 1) {
            int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp);
            if (p.y >= tp && p.y < tp + tl) { sd.barDrag = true; sd.barGrab = p.y - tp; }
            else SideScrollTo(sd.scroll + (p.y < tp ? -(int)(sd.ch * 0.9) : (int)(sd.ch * 0.9)));
            return 0;
        }
        sd.dp = p;
        sd.hit = SidePageAt(sd.dp.y + sd.scroll);
        sd.moved = 0; sd.pend = 0;
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (sd.barDrag) {
            int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp);
            SideScrollTo((int)((long long)(p.y - sd.barGrab) * max(0, sd.docH - sd.ch) / max(1, sd.ch - tl)));
            return 0;
        }
        bool hot = g.n && sd.docH > sd.ch && p.x >= sd.cw - SBar() && p.x < sd.cw - 1;
        if (hot) { int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp); hot = p.y >= tp && p.y < tp + tl; }
        if (hot != sd.barHot) {
            sd.barHot = hot;
            if (hot) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (GetCapture() == h) {
            int dy = p.y - sd.dp.y;
            sd.dp = p;
            if (dy) {
                sd.moved += abs(dy);
                sd.pend -= dy;
                if (sd.moved > (int)(4 * g.dpi)) { SideScrollTo(sd.scroll + sd.pend); sd.pend = 0; }
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (sd.barHot) { sd.barHot = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        bool bar = sd.barDrag;
        sd.barDrag = false;
        if (GetCapture() == h) ReleaseCapture();
        if (!bar && sd.moved <= (int)(4 * g.dpi) && sd.hit >= 0 && sd.hit < g.n) {
            sd.hold = GetTickCount64() + 600;   // the jump must not scroll the sidebar back
            ScrollTo(g.sx, max(0, g.top[sd.hit] - Gap() / 2));
        }
        sd.moved = 0; sd.hit = -1; sd.pend = 0;
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            bool edge = g.sideOn && g.n && sd.cw > 0 && p.x >= sd.cw - 1;
            SetCursor(LoadCursorW(nullptr, edge ? IDC_SIZEWE : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DROPFILES: HandleDrop((HDROP)wp); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------- page view ----------------

static void Draw(HDC dc, int w, int h, int xoff) {
    RECT r{ xoff, 0, xoff + w, h };
    FillRect(dc, &r, GrainBrush(0));   // textured margins instead of flat grey
    if (!g.n) {
        int sz = (int)(128 * g.dpi);
        if (!g.logo) g.logo = (HICON)LoadImageW(g.hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR);
        int cy = h / 2;
        if (g.logo) DrawIconEx(dc, xoff + (w - sz) / 2, cy - sz - (int)(6 * g.dpi), g.logo, sz, sz, 0, nullptr, DI_NORMAL);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(200, 200, 205));
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT tr{ xoff, cy + (int)(8 * g.dpi), xoff + w, h };
        DrawTextW(dc, L"Docsy  -  Ctrl+O or drop a PDF here", -1, &tr, DT_CENTER | DT_TOP | DT_SINGLELINE);
        return;
    }
    HDC m = CreateCompatibleDC(dc);
    int first = PageAt(g.sy), last = first;
    int gut = g.docH > h ? SBar() : 0;   // the reserved bar column, like KillerPDF's Grid layout
    int offx = g.docW < w - gut ? (w - gut - g.docW) / 2 : -g.sx;
    bool selOn = g.selRange || (g.selArm && g.selDrag);   // quads paint live during the drag
    for (int i = first; i < g.n && g.top[i] < g.sy + h; i++) {
        last = i;
        int pw = PW(i), ph = PH(i);
        int x = xoff + offx + g.gap + (g.maxw - pw) / 2, y = g.top[i] - g.sy;
        if (y + ph < 0) continue;
        HBITMAP b = GetBmp(i, pw, ph);
        if (b) {
            HGDIOBJ o = SelectObject(m, b);
            BitBlt(dc, x, y, pw, ph, m, 0, 0, SRCCOPY);
            SelectObject(m, o);
        } else {
            RECT pr{ x, y, x + pw, y + ph };
            FillRect(dc, &pr, (HBRUSH)GetStockObject(WHITE_BRUSH));
        }
        // flowing selection quads: KillerPDF paints one rect per selected line on an
        // overlay above the page - 60/255 of the dark theme's SelectionAccent #22bb56
        if (selOn) {
            int s, e; SelSliceForPage(i, s, e);
            if (s < e) {
                TxtPage* pt = TtFor(i);
                if (pt) {
                    std::vector<FRect> rs; TtRects(*pt, s, e, rs);
                    float k = (float)(g.zoom * g.dpi);
                    Gdiplus::Graphics gs(dc);
                    Gdiplus::SolidBrush br(Gdiplus::Color(60, 0x22, 0xbb, 0x56));
                    for (const FRect& q : rs)
                        gs.FillRectangle(&br, Gdiplus::RectF(x + q.x * k, y + q.y * k,
                                                             max(q.w * k, 2.0f), max(q.h * k, 2.0f)));
                }
            }
        }
    }
    DeleteDC(m);
    g.lastVis = last;
    Evict(first, last);
}

static void Prefetch() {
    if (!g.n) return;
    int cand[2] = { g.lastVis + 1, PageAt(g.sy) - 1 };
    for (int i : cand)
        if (i >= 0 && i < g.n) {
            auto it = g.cache.find(i);
            if (it == g.cache.end() || it->second.w != PW(i)) { GetBmp(i, PW(i), PH(i)); }
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
// reserved 12px gutter (the vertical one spans the full height, the horizontal
// one stops short of it), the track paints nothing, and the thumb is the only
// visible part: 4px and a third alpha at rest, 8px and solid under the cursor.
static void PaintViewBars(HDC dc, int w, int h, int xoff) {
    if (!g.n) return;
    int gb = SBar(), r = (int)(3 * g.dpi);
    bool vshow = g.docH > h;
    if (vshow) {
        int tl, tp; BarThumb(g.docH, h, g.sy, tl, tp);
        bool hot = g.vHot || g.vDrag;
        int bw = hot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);
        RoundRectFill(dc, xoff + w - gb + (gb - bw) / 2, tp, bw, tl, r, hot);
    }
    if (g.docW > w) {
        int track = w - (vshow ? gb : 0);
        int tl, tp; BarThumb(g.docW, track, g.sx, tl, tp);
        bool hot = g.hHot || g.hDrag;
        int bw = hot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);
        RoundRectFill(dc, xoff + tp, h - gb + (gb - bw) / 2, tl, bw, r, hot);
    }
}

static void ViewWheel(WPARAM wp, LPARAM lp) {
    int d = GET_WHEEL_DELTA_WPARAM(wp);
    if (LOWORD(wp) & MK_CONTROL) {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(g.view, &p);
        SetZoom(g.zoom * std::pow(1.12, d / 120.0), false, p.x, p.y);
    } else if (LOWORD(wp) & MK_SHIFT) ScrollTo(g.sx - d * (int)(g.dpi * 80) / 120, g.sy);
    else ScrollTo(g.sx, g.sy - d * (int)(g.dpi * 80) / 120);
}

static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    // WM_MOUSEACTIVATE keeps its default (MA_ACTIVATE): clicking the document
    // must raise and focus the window. MA_NOACTIVATE here was a leftover
    // anti-flash band-aid from the WS_CAPTION era - the style is gone, nothing
    // native can flash on activation any more, and the swallow only meant
    // clicks on the pages could not bring Docsy to the foreground.
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (g.cw > 0 && g.ch > 0) {
            EnsureBack(dc, g.cw, g.ch);
            Draw(g.bdc, g.cw, g.ch, 0);
            PaintViewBars(g.bdc, g.cw, g.ch, 0);
            BitBlt(dc, 0, 0, g.cw, g.ch, g.bdc, 0, 0, SRCCOPY);
            SetTimer(h, 1, 5, nullptr);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        KillTimer(h, 1); Prefetch(); return 0;
    case WM_MOUSEWHEEL: ViewWheel(wp, lp); return 0;
    case WM_MOUSEHWHEEL:
        ScrollTo(g.sx + GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 80) / 120, g.sy); return 0;
    case WM_KEYDOWN: return 0;   // keys live on the main window
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetCapture(h);
        int gb = SBar();
        bool vshow = g.n && g.docH > g.ch;
        if (vshow && p.x >= g.cw - gb) {   // the vertical bar: drag the thumb, page on the track
            int tl, tp; BarThumb(g.docH, g.ch, g.sy, tl, tp);
            if (p.y >= tp && p.y < tp + tl) { g.vDrag = true; g.vGrab = p.y - tp; }
            else ScrollTo(g.sx, g.sy + (p.y < tp ? -(int)(g.ch * 0.9) : (int)(g.ch * 0.9)));
            return 0;
        }
        if (g.n && g.docW > g.cw && p.y >= g.ch - gb && p.x < g.cw - (vshow ? gb : 0)) {
            int tl, tp; BarThumb(g.docW, g.cw - (vshow ? gb : 0), g.sx, tl, tp);
            if (p.x >= tp && p.x < tp + tl) { g.hDrag = true; g.hGrab = p.x - tp; }
            else ScrollTo(g.sx + (p.x < tp ? -(int)(g.cw * 0.9) : (int)(g.cw * 0.9)), g.sy);
            return 0;
        }
        if (g.sideOn && g.n && p.x < (int)(5 * g.dpi)) {   // the splitter: drag to resize the sidebar
            g.resizing = true; g.rsX = p.x; g.rsW0 = SW();
            return 0;
        }
        // flowing text selection: a press ON text starts KillerPDF's caret drag; a press
        // on empty page keeps the pan (KillerPDF keeps its marquee there instead)
        int pg; float fx, fy;
        if (ViewToPage(p, g.cw, g.ch, pg, fx, fy)) {
            TxtPage* pt = TtFor(pg);
            if (pt && TtOverText(*pt, fx, fy)) {
                SelClear();
                g.selArm = true; g.selDrag = false;
                g.selAPage = g.selFPage = pg;
                g.selACaret = g.selFCaret = TtCaret(*pt, fx, fy);
                g.selDownPos = p;
                return 0;
            }
            if (g.selRange || g.selArm) SelClear();   // a plain click off text drops it
        }
        g.drag = true; g.dp = p;
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g.resizing) {
            int w = g.rsW0 + (p.x - g.rsX);
            if (w != g.sideW) { g.sideW = w; LayoutMain(); }   // thumbs rebuild once, on release
            return 0;
        }
        if (g.vDrag) {
            int tl, tp; BarThumb(g.docH, g.ch, g.sy, tl, tp);
            ScrollTo(g.sx, (int)((long long)(p.y - g.vGrab) * max(0, g.docH - g.ch) / max(1, g.ch - tl)));
            return 0;
        }
        if (g.hDrag) {
            int tl, tp; BarThumb(g.docW, g.cw - ((g.n && g.docH > g.ch) ? SBar() : 0), g.sx, tl, tp);
            ScrollTo((int)((long long)(p.x - g.hGrab) * max(0, g.docW - g.cw) / max(1, g.cw - (g.docH > g.ch ? SBar() : 0) - tl)), g.sy);
            return 0;
        }
        if (g.drag) {
            ScrollTo(g.sx - (p.x - g.dp.x), g.sy - (p.y - g.dp.y)); g.dp = p;
        }
        // live selection drag: the focus caret tracks the pointer (cross-page, like
        // KillerPDF's Continuous mode); ViewToPage clamps into the page so dragging
        // past an edge parks the caret at the nearest line's start/end
        if (g.selArm) {
            if (!g.selDrag) {
                if (abs(p.x - g.selDownPos.x) < (int)(4 * g.dpi) &&
                    abs(p.y - g.selDownPos.y) < (int)(4 * g.dpi)) return 0;   // still a click
                g.selDrag = true;
            }
            int pg; float fx, fy;
            if (ViewToPage(p, g.cw, g.ch, pg, fx, fy)) {
                TxtPage* pt = TtFor(pg);
                if (pt) {
                    int caret = TtCaret(*pt, fx, fy);
                    if (pg != g.selFPage || caret != g.selFCaret) {
                        g.selFPage = pg; g.selFCaret = caret;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        bool vhot = g.n && g.docH > g.ch && p.x >= g.cw - SBar();
        if (vhot) { int tl, tp; BarThumb(g.docH, g.ch, g.sy, tl, tp); vhot = p.y >= tp && p.y < tp + tl; }
        bool hhot = !vhot && g.n && g.docW > g.cw && p.y >= g.ch - SBar() && p.x < g.cw - (vhot ? SBar() : 0);
        if (hhot) { int tl, tp; BarThumb(g.docW, g.cw - (vhot ? SBar() : 0), g.sx, tl, tp); hhot = p.x >= tp && p.x < tp + tl; }
        if (vhot != g.vHot || hhot != g.hHot) {
            g.vHot = vhot; g.hHot = hhot;
            if (vhot || hhot) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.vHot || g.hHot) { g.vHot = g.hHot = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (g.resizing) { g.resizing = false; ReleaseCapture(); LayoutMain(); return 0; }
        if (g.selArm) {
            g.selArm = false;
            bool dragged = g.selDrag;
            g.selDrag = false;
            ReleaseCapture();
            if (!dragged || (g.selAPage == g.selFPage && g.selACaret == g.selFCaret)) {
                SelClear();                       // plain click collapses the selection
            } else {
                g.selRange = true;
                g.selText = SelBuildText(nullptr);
                if (g.selText.find_first_not_of(L" \n\r\t") == std::wstring::npos)
                    SelClear();                   // swept a page with no text layer
                else CopyClipboard(g.selText);    // select copies immediately, KillerPDF-style
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        g.vDrag = g.hDrag = g.drag = false;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK: {
        // double-click word selection, copied at once (KillerPDF's TrySelectWordAtPosition)
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int pg; float fx, fy;
        if (ViewToPage(p, g.cw, g.ch, pg, fx, fy)) {
            TxtPage* pt = TtFor(pg);
            int s, e;
            if (pt && TtWordRange(*pt, fx, fy, s, e)) {
                g.selArm = g.selDrag = false;
                g.selAPage = g.selFPage = pg;
                g.selACaret = s; g.selFCaret = e;
                g.selRange = true;
                g.selText = TtText(*pt, s, e, nullptr);
                CopyClipboard(g.selText);
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g.n) {
            if (g.vDrag || g.hDrag || g.vHot || g.hHot) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (g.sideOn && p.x < (int)(5 * g.dpi)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            // I-beam over selectable text (KillerPDF's SelectableTextCursor): the cursor
            // also says which gesture a press will start - select here, pan elsewhere
            int pg; float fx, fy;
            if (!g.drag && ViewToPage(p, g.cw, g.ch, pg, fx, fy)) {
                TxtPage* pt = TtFor(pg);
                if (pt && TtOverText(*pt, fx, fy)) { SetCursor(LoadCursorW(nullptr, IDC_IBEAM)); return TRUE; }
            }
            SetCursor(LoadCursorW(nullptr, g.drag ? IDC_SIZEALL : IDC_HAND)); return TRUE;
        }
        break;
    case WM_DROPFILES: HandleDrop((HDROP)wp); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------- main window ----------------

static void LayoutMain() {
    if (!g.main || !g.view) return;
    RECT r; GetClientRect(g.main, &r);
    int w = r.right, h = r.bottom;
    int sw = (g.sideOn && g.n) ? SW() : 0;
    int cap = CAP(), ch = max(0, h - cap);      // both children sit below the custom caption row
    sd.w = sw;
    if (g.side) {
        MoveWindow(g.side, 0, cap, sw, ch, TRUE);
        if (sw) {
            RECT sr; GetClientRect(g.side, &sr);
            if (sr.right != sd.cw || sr.bottom != sd.ch) { sd.cw = sr.right; sd.ch = sr.bottom; SideLayout(); }
        }
    }
    MoveWindow(g.view, sw, cap, w - sw, ch, TRUE);
    RECT vr; GetClientRect(g.view, &vr);
    g.cw = vr.right; g.ch = vr.bottom;
    g.sx = std::clamp(g.sx, 0, max(0, g.docW - g.cw));
    g.sy = std::clamp(g.sy, 0, max(0, g.docH - g.ch));
    if (!g.resizing && !g.busy && g.cw > 0 && g.ch > 0) {
        if (g.fit) ApplyFit(); else Layout();
        UpdateTitle();
    }
}

static void ToggleSide() {
    if (!g.n) return;
    g.sideOn = !g.sideOn;
    ShowWindow(g.side, g.sideOn ? SW_SHOWNOACTIVATE : SW_HIDE);
    LayoutMain();
}

static void ZoomBy(double f) { SetZoom(g.zoom * f, false, g.cw / 2, g.ch / 2); }

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g.main = h; g.dpi = GetDpiForWindow(h) / 96.0;
        g.view = CreateWindowExW(0, L"DocsyView", nullptr,
                                 WS_CHILD | WS_VISIBLE,
                                 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        g.side = CreateWindowExW(0, L"DocsySide", nullptr,
                                 WS_CHILD,
                                 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        DragAcceptFiles(h, TRUE); DragAcceptFiles(g.view, TRUE); DragAcceptFiles(g.side, TRUE);
        DwmApply(h);   // rounded corners + themed 1px border (Windows 11; no-op before)
        return 0;
    }
    case WM_SIZE:
        LayoutMain();
        DwmApply(h);   // corners square off while maximized, round again on restore
        return 0;
    case WM_PAINT: {   // the custom caption row; the children paint their own areas
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        DrawCaptionBar(dc, r.right);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {   // DWM re-materializes the surface (peek, taskbar thumbnails,
        HDC dc = (HDC)wp;    // occlusion, snap previews) WITHOUT invalidating first, which
        RECT r; GetClientRect(h, &r);   // blanked the band until the next re-hover
        DrawCaptionBar(dc, r.right);
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
        SetCapHot(h, wp == HTMAXBUTTON ? 1 : -1);
        if (wp == HTMAXBUTTON) {
            TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE | TME_NONCLIENT, h, 0 };
            TrackMouseEvent(&t);
            InvalidateCap(h);                  // repaint on every hover move: a dropped
        }                                      // surface heals without leaving and re-entering
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
        if (hot >= 0) { TrackLeave(h); InvalidateCap(h); }   // repaint on every hover move
        SetCapHot(h, hot);
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
        ClearCache();
        if (g_uifont) { DeleteObject(g_uifont); g_uifont = nullptr; }
        if (g_sidefont) { DeleteObject(g_sidefont); g_sidefont = nullptr; }
        if (g_glyphfont) { DeleteObject(g_glyphfont); g_glyphfont = nullptr; }
        if (g_wordfont) { DeleteObject(g_wordfont); g_wordfont = nullptr; }
        for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
        sd.th.clear(); sd.tw = 0;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutMain();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEWHEEL: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &p);
        if (g.sideOn && g.n && p.x < sd.w) {
            SideScrollTo(sd.scroll - GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120);
            return 0;
        }
        ViewWheel(wp, lp);
        return 0;
    }
    case WM_MOUSEHWHEEL:
        ScrollTo(g.sx + GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 80) / 120, g.sy); return 0;
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        int line = (int)(56 * g.dpi), pg = max(1, (int)(g.ch * 0.9));
        switch (wp) {
        case 'O': if (ctrl) OpenDlg(); break;
        case VK_DOWN: ScrollTo(g.sx, g.sy + line); break;
        case VK_UP: ScrollTo(g.sx, g.sy - line); break;
        case VK_RIGHT: ScrollTo(g.sx + line, g.sy); break;
        case VK_LEFT: ScrollTo(g.sx - line, g.sy); break;
        case VK_NEXT: ScrollTo(g.sx, g.sy + pg); break;
        case VK_PRIOR: ScrollTo(g.sx, g.sy - pg); break;
        case VK_SPACE: ScrollTo(g.sx, g.sy + (shift ? -pg : pg)); break;
        case VK_HOME: ScrollTo(g.sx, 0); break;
        case VK_END: ScrollTo(g.sx, INT_MAX / 2); break;
        case VK_ADD: case VK_OEM_PLUS: ZoomBy(1.25); break;
        case VK_SUBTRACT: case VK_OEM_MINUS: ZoomBy(0.8); break;
        case '0': if (ctrl) ApplyFit(); break;
        case '1': if (ctrl) SetZoom(1.0, false, g.cw / 2, g.ch / 2); break;
        case '2': if (ctrl) ApplyFit(true); break;
        case 'F': ApplyFit(); break;
        case 'C': if (ctrl && !g.selText.empty()) CopyClipboard(g.selText); break;
        case 'A': if (ctrl) SelAll(); break;
        case VK_ESCAPE: SelClear(); break;
        case VK_F9: ToggleSide(); break;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 3) { InvalidateCap(h); return 0; }   // hovered-button keepalive repaint
        break;
    case WM_DROPFILES: HandleDrop((HDROP)wp); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// CI preview: render the first view offscreen to a PNG. Returns 0 on success, else 10+GDI+ status.
static int Shot(const wchar_t* out, int w, int h) {
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP b = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!b) return 4;
    HGDIOBJ old = SelectObject(dc, b);
    int sw = (g.sideOn && g.n) ? sd.w : 0;
    int cap = CAP(), ch = max(1, h - cap);
    // the children render above the caption row, so composite them shifted
    if (sw) {
        HDC m1 = CreateCompatibleDC(dc);
        HBITMAP b1 = CreateCompatibleBitmap(dc, sw, ch);
        HGDIOBJ o1 = SelectObject(m1, b1);
        SideDraw(m1, sw, ch, 0);
        BitBlt(dc, 0, cap, sw, ch, m1, 0, 0, SRCCOPY);
        SelectObject(m1, o1); DeleteObject(b1); DeleteDC(m1);
    }
    {
        HDC m2 = CreateCompatibleDC(dc);
        HBITMAP b2 = CreateCompatibleBitmap(dc, max(1, w - sw), ch);
        HGDIOBJ o2 = SelectObject(m2, b2);
        Draw(m2, w - sw, ch, 0);
        PaintViewBars(m2, w - sw, ch, 0);
        BitBlt(dc, sw, cap, w - sw, ch, m2, 0, 0, SRCCOPY);
        SelectObject(m2, o2); DeleteObject(b2); DeleteDC(m2);
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

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Gdiplus::GdiplusStartupInput gi; ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gi, nullptr);
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool shot = argc >= 4 && !lstrcmpW(argv[1], L"--shot");
    g.quiet = shot; g.hinst = hi;

    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = Proc; wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"Docsy";
    RegisterClassExW(&wc);

    WNDCLASSEXW vc{ sizeof vc };
    vc.style = CS_DBLCLKS;               // double-click word selection
    vc.lpfnWndProc = ViewProc; vc.hInstance = hi;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.lpszClassName = L"DocsyView";
    RegisterClassExW(&vc);

    WNDCLASSEXW sc{ sizeof sc };
    sc.lpfnWndProc = SideProc; sc.hInstance = hi;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.lpszClassName = L"DocsySide";
    RegisterClassExW(&sc);

    // WS_CAPTION is dropped (see the chrome notes above): a captioned window
    // has DWM paint its stock title bar on the first activation until a
    // minimize/restore forces a frame recalc - the "minimize then maximize
    // once on launch" glitch. Without the style there is no native caption to
    // paint at all; the thickframe still owns resize borders, Aero Snap, the
    // system menu and the Win11 snap flyout. SWP_FRAMECHANGED syncs the final
    // frame before the first present.
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_CAPTION;
    CreateWindowExW(0, L"Docsy", L"Docsy", style, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                    nullptr, nullptr, hi, nullptr);
    if (!g.main) return 1;
    SetWindowPos(g.main, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (shot) {
        if (lstrcmpW(argv[3], L"-") && !Open(argv[3])) return 2;
        // CI preview seed: a committed selection over one page's cells (sel=page:c0:c1),
        // so the release screenshot shows the selection quads
        if (argc >= 5 && !wcsncmp(argv[4], L"sel=", 4)) {
            const wchar_t* q = argv[4] + 4;
            const wchar_t* c1p = wcschr(q, L':');
            const wchar_t* c2p = c1p ? wcschr(c1p + 1, L':') : nullptr;
            if (c1p && c2p) {
                int pg = _wtoi(q), c0 = _wtoi(c1p + 1), c1 = _wtoi(c2p + 1);
                TxtPage* pt = TtFor(pg);
                if (pt && !pt->cells.empty()) {
                    int n = (int)pt->cells.size();
                    g.selAPage = g.selFPage = pg;
                    g.selACaret = std::clamp(c0, 0, n);
                    g.selFCaret = std::clamp(c1, 0, n);
                    if (g.selACaret != g.selFCaret) g.selRange = true;
                }
            }
        }
        // Size the window so the client is exactly 880x680: WM_NCCALCSIZE gives
        // the caption back to the client, so only the side borders remain.
        SetWindowPos(g.main, nullptr, 0, 0, 880, 680, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (int pass = 0; pass < 2; pass++) {
            RECT c; GetClientRect(g.main, &c);
            if (c.right == 880 && c.bottom == 680) break;
            SetWindowPos(g.main, nullptr, 0, 0, 880 * 2 - c.right, 680 * 2 - c.bottom,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT c; GetClientRect(g.main, &c);
        for (int k = 0; k <= g.n; k++) if (!SidePump()) break;   // render the visible thumbs now
        return Shot(argv[2], c.right, c.bottom);
    }

    ShowWindow(g.main, show);
    UpdateWindow(g.main);
    if (argc > 1) Open(argv[1]);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
