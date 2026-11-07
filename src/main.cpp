// Docsy - minimal, fast, native Win32 PDF reader.
// Uses the PDF engine built into Windows (Windows.Data.Pdf): no bundled renderer.
// The main window hosts two children: the page view (right) and the thumbnail
// sidebar (left; drag the splitter between them to resize it). The sidebar is
// fully virtualized: only thumbnails actually in view are rendered (a few per
// timer tick) and off-screen ones are evicted, so a 2,000-page book scrolls as
// light as a 10-page one. No stock scrollbars anywhere: scrolling is wheel and
// keys, and the only bar on screen is a slim KillerPDF-style overlay thumb (4px
// wide, 8px under the cursor) owning an invisible 12px gutter.
// Text selection is KillerPDF's flowing model too: a press on text drags a caret
// pair of individual characters along the page's reading order (column-aware,
// line-banded cells: the built-in OCR pass over the page supplies word boxes, each
// sliced into per-character cells the way KillerPDF's engine supplies letters) and
// paints one 60/255 accent quad per selected line; select or double-click copies
// at once. The page never pans under the mouse - the wheel and the keys own
// scrolling, so a grab always means text selection, never a scroll.
// Find Text is KillerPDF's search bar too (Shell/Search.cs + SearchController +
// SearchService): Ctrl+F floats one rounded grain bar over the page area (top
// right, KillerPDF's fallback spot) with KillerPDF's DropShadow melted onto the
// live view pixels; the row is the magnifier glyph (E721), the query edit, the
// Aa / ab option toggles (accent fill when on), the "12 / 73" counter cell and
// ChevronUp / ChevronDown / Cancel buttons with the red close hover. 250ms
// debounce, a two-character minimum, Enter / Shift+Enter / F3 / Shift+F3 step,
// Escape closes. The scan walks the document page by page on a pump timer (the
// OCR text layer is built page-wise, so a 2,000-page book streams its matches
// instead of blocking), matches fold case unless Match Case, Whole Word rejects
// letter/digit/underscore neighbours, and every match paints one rounded box per
// touched word - dim orange for the field, bright with the accent ring for the
// cursor match - and navigation lands on the first match at or after the page
// you are on.
// The frame is KillerPDF's too: the real OS side borders stay, but the window
// is created without WS_CAPTION (and WM_NCCALCSIZE gives the top row to the
// client), so the native caption can never appear - not even on the first
// paint. A custom 32px title bar (grain, wordmark, 44x32 min/max/close
// buttons with the green/red hovers, the Win11 snap flyout on the maximize
// button and DWM rounded corners) owns the top row; nothing read from the
// document (no file name) is drawn anywhere on it.
// The bottom row is KillerPDF's footer status bar, one full-width child painted
// in the same #1c1c1c + shared grain tone as the sidebar and title bar (the
// original paints all three BackgroundBrush under one tile), so they read as
// ONE container wrapping the document pane on two sides. Its cells port the
// original's logic: the status line "Page N of M, Fit Page (Z%)" with the
// click-for-file-size flash (held messages beat routine updates,
// SetStatusHeld-style), the paper-size label from KillerPDF's PageSizeFormatter
// (known A/US sizes matched at +/-3pt, mm for A-series, inches for US), the
// F5-F8 view-mode radios (continuous / single / two-page / 3-column grid), the
// full screen and doc-invert (moon) toggles, the zoom out / fit dropdown /
// zoom in cluster over a real dropdown popup, and the version cell. The text
// is the original's bundled Killer Sans (OFL; subset, embedded in the exe,
// process-private via AddFontMemResourceEx) at its FontSize 16, on a 24px bar.
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
#include <cwctype>
#include <cmath>
#include <climits>
#include <cfloat>
using std::min; using std::max;
#include <gdiplus.h>
#pragma comment(lib, "msimg32.lib")   // AlphaBlend: the pane shadow's premultiplied ARGB tile
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
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
// bitmap already uses. Empty page areas are dead to the mouse - the wheel and the
// keys own scrolling, so a grab on the page always means selection, never a scroll.
struct TxtCell {                      // one selectable unit: a character sliced out
    std::wstring text;                // of an OCR word box (word-granular boxes are
    float l = 0, r = 0, t = 0, b = 0; // what the OS OCR reports; TtSplitChars cuts
    int word = 0, line = 0;           // them into KillerPDF's letter-granular cells)
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
    HWND main = nullptr, view = nullptr, side = nullptr, status = nullptr, zoomPop = nullptr;
    HINSTANCE hinst = nullptr;
    HICON logo = nullptr;
    PdfDocument doc{ nullptr };
    std::wstring name;
    std::wstring path;                  // full path of the open file (the status click's size flash)
    int n = 0;
    std::vector<float> pw, ph;          // page size in DIPs (PDF points)
    std::vector<int> top;               // pixel top of each ROW (a row holds 1-3 pages)
    std::vector<int> px;                // pixel x of each page inside its row
    std::vector<int> rh;                // pixel height of each row (the tallest page in it)
    double zoom = 1.0, dpi = 1.0;       // zoom: pixels per DIP (x dpi scale)
    int vm = 0;                         // KillerPDF's view modes: 0 continuous, 1 single, 2 two-page, 3 grid
    int fitMode = 0;                    // KillerPDF's FitMode: 0 FitWidth, 1 FitPage, 2 none (free zoom)
    bool inv = false;                   // DocInvert (the footer moon): page bitmaps blitted inverted
    bool full = false;                  // F11 full screen: chrome gone, window over the monitor
    WINDOWPLACEMENT fsPlace{};          // pre-fullscreen placement
    LONG fsStyle = 0;
    std::wstring hint;                  // transient on-page hint (full screen entry)
    unsigned long long hintUntil = 0;
    bool quiet = false, busy = false, sideOn = false;
    int sx = 0, sy = 0, cw = 0, ch = 0, docW = 0, docH = 0, gap = 10, maxw = 0, lastVis = 0;
    int cur = -1;                       // current page (view centre), drives the sidebar highlight
    unsigned long long scrollTick = 0;  // last scroll stamp: defers the text pre-warm until settling
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
    HDC capdc = nullptr; HBITMAP capbm = nullptr, capold = nullptr; int capw = 0, caph = 0;
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
    // the footer (KillerPDF's status bar): a held message briefly wins over the
    // routine page/zoom line, exactly like SetStatusHeld in MainWindow.xaml.cs
    std::wstring statusHoldText;
    unsigned long long statusHoldUntil = 0;
    int stHot = -1;                     // hovered footer control, -1 none
    bool zoomOpen = false;              // the footer zoom dropdown is on screen
    int zoomHot = -1;                   // highlighted item inside the open dropdown
};
static App g;

// Sidebar state. Thumbs are small page renders, made lazily and LRU-capped at
// 256, so the sidebar never renders more than the few pages you can see.
struct Side {
    int w = 0, cw = 0, ch = 0, scroll = 0, docH = 0, tw = 0, cur = -1;
    int hover = -1;                     // the item under the cursor: KillerPDF's IsMouseOver row
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

// Find Text state (KillerPDF's SearchController fields, flat): the bar's query
// and option toggles, the reading-ordered match list (one rect per touched
// word), the cursor match, and the pump's progress. The pump scans page by
// page on a timer so a 2,000-page book streams matches without holding the
// UI; every edit bumps gen, and a stale pump's results are dropped on sight.
struct FHit { int page; FRect r; };   // one matched word's rect, page DIPs
struct Find {
    bool open = false;                // the bar is on screen
    std::wstring query;               // the box's text
    bool matchCase = false, wholeWord = false;
    std::vector<FHit> hits;           // page asc, then reading order per page
    int cur = -1;                     // the emphasized match (cursor)
    int scan = 0;                     // next page the pump reads
    unsigned gen = 0;                 // bumped on every cancel; a stale pump quits
    bool done = true;                 // the whole document has been scanned
    int hot = -1;                     // hovered bar button
};
static Find fd;
static HWND g_find = nullptr, g_fedit = nullptr;
static WNDPROC g_feditDef = nullptr;               // the edit's stock proc (subclassed for keys)

static HFONT g_uifont = nullptr, g_sidefont = nullptr;
static HFONT g_glyphfont = nullptr, g_wordfont = nullptr;

static int PW(int i) { return max(1, (int)std::lround(g.pw[i] * g.zoom * g.dpi)); }
static int PH(int i) { return max(1, (int)std::lround(g.ph[i] * g.zoom * g.dpi)); }
static int Gap() { return max(4, (int)(10 * g.dpi)); }
static int SBar() { return (int)(12 * g.dpi); }   // overlay bar gutter: KillerPDF's ScrollBarThickness
static int CardW();                               // the document card's width: view minus the 8px window gutter

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
static int CAP() { return g.full ? 0 : (int)(32 * g.dpi); }   // full screen gives the row to the page

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
    if (g.capHot == hot) return;                     // repaints only on a hover flip -
    g.capHot = hot;                                  // never per mouse move
    if (hot >= 0) SetTimer(h, 3, 250, nullptr);      // while a button is hovered the band also
    else KillTimer(h, 3);                            // repaints on a slow tick, so a surface
    InvalidateCap(h);                                // that dropped it heals without re-hover
    // (the tick repaint is buffered now, so an unchanged band reblits identically
    // and cannot strobe the way the old direct paint did)
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
    int roundc = (IsZoomed(h) || g.full) ? 1 /* DWMWCP_DONOTROUND */ : 2 /* DWMWCP_ROUND */;
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
// window paints it. Painted through a memory bitmap and blitted in one call:
// a direct paint showed every intermediate step (grain fill, wordmark, button
// gradient) on each repaint, which read as the buttons flashing under the cursor.
static void DrawCaptionBar(HDC dc, int w) {
    if (g.full || w <= 0) return;
    RECT bar{ 0, 0, w, CAP() };
    FillRect(dc, &bar, GrainBrush(1));   // the title bar stays the dark chrome tile
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

// The band's own back buffer, the way the view and the sidebar already paint.
static void EnsureCapBack(HDC ref, int w) {
    int caph = CAP();
    if (g.capbm && g.capw == w && g.caph == caph) return;
    if (g.capbm) { SelectObject(g.capdc, g.capold); DeleteObject(g.capbm); }
    else g.capdc = CreateCompatibleDC(ref);
    g.capbm = CreateCompatibleBitmap(ref, w, caph);
    g.capold = (HBITMAP)SelectObject(g.capdc, g.capbm);
    g.capw = w; g.caph = caph;
}

// Draw the caption band into dst with a single atomic blit.
static void PaintCaptionInto(HDC dst, int w) {
    if (!dst || w <= 0 || CAP() <= 0) return;
    EnsureCapBack(dst, w);
    DrawCaptionBar(g.capdc, w);
    BitBlt(dst, 0, 0, w, CAP(), g.capdc, 0, 0, SRCCOPY);
}

// ---- film-grain margins (port of KillerPDF's procedural GrainTexture) ----
// One seamless 256x256 tile of gray specks: Gaussian around 128 (sigma 150),
// softened with wrapping neighbours so the tile repeats without a seam, then
// contrast-boosted to keep dark and bright specks distinct. KillerPDF paints
// this tile at alpha 70/255 and then scales the whole overlay down by its
// theme resource GrainOpacity - 0.24 on the dark themes - so the effective
// blend is (70/255) * 0.24 ~= 0.066. Baking that product in reproduces the
// rendered frost exactly; painting at the raw alpha 70 reads as harsh TV
// static. The seed is fixed so every build shows the identical pattern, and
// the tile stays fixed to the window while pages scroll, matching KillerPDF's
// viewport-fixed overlay.
//
// Two KillerPDF details this port now keeps exactly:
//  * the lean specks. 40% of the specks lean toward the theme's secondary
//    color - the complement of the accent hue (LeanFor/Generate in
//    Controls/GrainTexture.cs). The green #22bb56 accent sits at hue 132, the
//    complement 312 lands on pink, and the family rule leans greens to purple
//    (285) instead: FromHsv(285, 0.55, 0.85) = RGB(187, 98, 217). A leaning
//    speck keeps its own lightness (k = 0.35 + 1.1*shade/255) and mixes 80% of
//    the way over, so the tint reads as a cool grey, never a color wash.
//  * the surfaces. The dark theme paints the title bar, the sidebar and the
//    footer the SAME BackgroundBrush #1c1c1c under one shared tile
//    (GrainBrushShared), which is why KillerPDF's frame reads as one
//    continuous container; only the document card is lighter (PaneBrush
//    #3a3a3a). docsy now bakes the same neutrals: slot 0 is the document
//    canvas, slot 1 the shared dark chrome (sidebar + footer + title bar all
//    return it), so the two frame the page area on two sides seamlessly.
static HBRUSH g_grainBrush[3] = { nullptr, nullptr, nullptr };
static HBITMAP g_grainBmp[3] = { nullptr, nullptr, nullptr };

static HBRUSH GrainBrush(int which) {
    if (which > 2) which = 2;
    if (g_grainBrush[which]) return g_grainBrush[which];
    const int S = 256;
    const double Spread = 150.0, Soften = 0.10, Contrast = 1.35;
    const double LeanShare = 0.40, LeanMix = 0.80;
    const double Alpha = 70.0 / 255.0 * 0.24;   // tile alpha 70 times the dark-theme GrainOpacity
    const BYTE back[3][3] = {
        { 58, 58, 58 },   // document canvas  - KillerPDF's BgCanvas/PaneBrush #3a3a3a
        { 28, 28, 28 },   // dark chrome      - BackgroundBrush/TitleBarBrush/FooterBackgroundBrush #1c1c1c
        { 20, 20, 20 },   // floating surface - MenuBackgroundBrush #141414 (the Find bar)
    };                                    // stored B,G,R; all neutral greys, exactly as the theme states them
    const double leanC[3] = { 217.0, 98.0, 187.0 };   // the purple lean, B,G,R
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
    std::vector<BYTE> lean((size_t)S * S);
    for (size_t i = 0; i < raw.size(); i++) {
        double u1 = 1.0 - rnd(), u2 = rnd();
        raw[i] = cl(128.0 + Spread * std::sqrt(-2.0 * std::log(u1)) * std::cos(2 * 3.14159265358979323846 * u2));
        lean[i] = rnd() < LeanShare;
    }
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), S, S, 1, 32, BI_RGB };
    BYTE* px = nullptr;
    g_grainBmp[which] = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    if (!px || !g_grainBmp[which]) return (HBRUSH)GetStockObject(GRAY_BRUSH);
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            size_t i = (size_t)y * S + x;
            double around = (raw[((y + S - 1) % S) * S + x] + raw[((y + 1) % S) * S + x]
                           + raw[y * S + (x + S - 1) % S] + raw[y * S + (x + 1) % S]) / 4.0;
            double soft = raw[i] * (1.0 - Soften) + around * Soften;
            double shade = cl(128.0 + (soft - 128.0) * Contrast);
            double speck[3] = { shade, shade, shade };
            if (lean[i]) {
                double k = 0.35 + 1.1 * shade / 255.0;      // keeps the speck's own lightness
                for (int c = 0; c < 3; c++)
                    speck[c] = shade * (1.0 - LeanMix) + cl(leanC[c] * k) * LeanMix;
            }
            BYTE* p = px + i * 4;
            for (int c = 0; c < 3; c++) p[c] = (BYTE)std::lround(back[which][c] * (1.0 - Alpha) + speck[c] * Alpha);
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

// ---- view modes (KillerPDF's F5-F8) over a row-based layout ----
// Continuous keeps the classic single column; Single lays out ONE page (the
// current one) and flips pages at the wheel's edges; TwoPage pairs pages into
// spreads ((0,1), (2,3) ... - KillerPDF's default pairing, book mode is its
// opt-in setting); Grid flows 3 columns (KillerPDF's default _gridColumns).
// top[] becomes the ROW tops, px[] each page's x inside its row, rh[] the row
// heights - the single column is just the 1-page-per-row case.
static int PerRow() { return g.vm == 2 ? 2 : g.vm == 3 ? 3 : 1; }
static int RowCount() {
    if (!g.n) return 0;
    if (g.vm == 1) return 1;                          // single: one row holding the current page
    return (g.n + PerRow() - 1) / PerRow();
}
static int RowPage(int r, int k) { return g.vm == 1 ? g.cur : r * PerRow() + k; }
static int RowAt(int y) { return PageIn(g.top, RowCount(), y); }
static int PageTop(int i) {                          // page's row top (pixel)
    if (!g.n || g.top.empty()) return 0;
    int r = std::clamp(i / PerRow(), 0, (int)g.top.size() - 2);
    return g.top[r];
}
static int PageAt(int y) {                           // primary page at a viewport y
    if (!g.n) return 0;
    int r = RowAt(y);
    return g.vm == 1 ? g.cur : min(g.n - 1, r * PerRow());
}

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

// KillerPDF's cells are the engine's LETTERS; ours start as OCR word boxes. Slice
// every word box into one cell per character - a uniform advance across the box,
// which is what hit-testing needs since the caret only compares mid-points - so
// drags move character by character instead of snapping whole words, exactly at
// KillerPDF's granularity. Every character keeps the word's ordinal, so the space
// between words, the double-click word range and the copied text are unchanged.
static void TtSplitChars(TxtPage& pt) {
    if (pt.cells.empty() || pt.lines.empty()) return;
    std::vector<TxtCell> out; out.reserve(pt.cells.size() * 4);
    std::vector<TxtLine> lines; lines.reserve(pt.lines.size());
    int lineStart = 0;
    for (const TxtLine& ln : pt.lines) {
        for (int i = ln.start; i < ln.end(); i++) {
            const TxtCell& w = pt.cells[i];
            int n = max(1, (int)w.text.size());
            float wl = (w.r - w.l) / n;
            for (int j = 0; j < n; j++) {
                TxtCell c;
                c.text.assign(1, w.text[j]);
                c.l = w.l + wl * j; c.r = w.l + wl * (j + 1);
                c.t = w.t; c.b = w.b;
                c.word = w.word; c.line = (int)lines.size();
                out.push_back(std::move(c));
            }
        }
        TxtLine nl = ln;                       // the band is the word band: contiguous
        nl.start = lineStart;                  // character tiles span the same extent
        nl.count = (int)out.size() - lineStart;
        lineStart = (int)out.size();
        lines.push_back(nl);
    }
    pt.cells = std::move(out);
    pt.lines = std::move(lines);
}

// True when the point sits ON text: inside a line's vertical band and within its
// horizontal extent (small slop). This is the gate that decides flowing selection
// vs doing nothing - a press on text selects, a press anywhere else is dead.
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

// ---------------- Find Text core (pure std:: - scripts/find_check.cpp) ----------------

// KillerPDF's PdfSearchText.Fold, reduced to what our OCR cells need: 1:1
// lower-case folding (a length-changing fold would break the char->cell map).
static std::wstring FdLower(const std::wstring& s) {
    std::wstring t(s);
    for (wchar_t& c : t) c = (wchar_t)towlower(c);
    return t;
}

// SearchService over the cell stream: the page flattens to a string (cells of
// one word join with no separator, a word gap or a line break one space), every
// occurrence is folded to lower case unless Match Case, Whole Word rejects
// letter/digit/underscore neighbours (KillerPDF's \p{L}\p{Nd}_ lookarounds), and
// each match lands as ONE RECT PER TOUCHED WORD - the union of that word's
// matched cell boxes, FindExactMatchesOnPage's word split. Occurrences scan
// forward without overlap, like the engine's. Returns the match count; the
// per-word rects come back through out.
static int FdMatch(const TxtPage& pt, const std::wstring& query, bool matchCase,
                   bool wholeWord, std::vector<FRect>& out) {
    out.clear();
    if (pt.cells.empty() || query.empty()) return 0;
    std::wstring text; text.reserve(pt.cells.size() * 3);
    std::vector<int> cellOf; cellOf.reserve(pt.cells.size() * 3);
    int lastWord = -1, lastLine = -1;
    for (int i = 0; i < (int)pt.cells.size(); i++) {
        const TxtCell& c = pt.cells[i];
        if (lastLine >= 0 && c.line != lastLine) { text += L' '; cellOf.push_back(-1); }
        else if (lastWord >= 0 && c.word != lastWord) { text += L' '; cellOf.push_back(-1); }
        for (wchar_t ch : c.text) { text += ch; cellOf.push_back(i); }
        lastWord = c.word; lastLine = c.line;
    }
    std::wstring hay = matchCase ? text : FdLower(text);
    std::wstring needle = matchCase ? query : FdLower(query);
    int n = (int)needle.length(), N = (int)hay.length();
    if (n == 0 || n > N) return 0;
    auto wordy = [](wchar_t c) { return iswalnum(c) || c == L'_'; };
    int matches = 0;
    for (int p = 0; p + n <= N; p++) {
        if (hay.compare(p, n, needle) != 0) continue;
        if (wholeWord && ((p > 0 && wordy(hay[p - 1])) || (p + n < N && wordy(hay[p + n])))) continue;
        int s = p, e = p + n;
        while (s < e && cellOf[s] < 0) s++;          // a query never starts or ends on a
        while (e > s && cellOf[e - 1] < 0) e--;      // separator; trim the join spaces
        if (s >= e) continue;
        int i = s;
        while (i < e) {                              // one rect per touched word
            if (cellOf[i] < 0) { i++; continue; }    // the join space between words
            int w = pt.cells[cellOf[i]].word;
            float l = FLT_MAX, r = -FLT_MAX, t = FLT_MAX, b = -FLT_MAX;
            while (i < e && pt.cells[cellOf[i]].word == w) {
                const TxtCell& c = pt.cells[cellOf[i]];
                l = min(l, c.l); r = max(r, c.r); t = min(t, c.t); b = max(b, c.b);
                i++;
            }
            out.push_back({ l, t, r - l, b - t });
        }
        matches++;
        p += n - 1;
    }
    return matches;
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
    TtSplitChars(pt);   // word boxes -> KillerPDF's letter-granular cells
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

// Peek at already-built text geometry without triggering the OCR build - the
// cursor only advertises text that is ready, and the build itself happens off
// the click path (the pre-warm timer, or the first press as a last resort).
static TxtPage* TtPeek(int i) {
    auto it = g.txt.find(i);
    return it != g.txt.end() ? &it->second : nullptr;
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
// drag point into the page canvas the same way). Multi-column rows resolve the
// page by x (a point in the gap clamps to the nearest page, so a drag across a
// spread flows through both pages like KillerPDF's Continuous does).
static bool ViewToPage(POINT v, int w, int h, int& page, float& dx, float& dy) {
    if (!g.n) return false;
    int gut = g.docH > h ? SBar() : 0;
    int offx = g.docW < w - gut ? (w - gut - g.docW) / 2 : -g.sx;
    int r = RowAt(v.y + g.sy);
    if (r < 0 || r >= RowCount()) return false;
    int cnt = g.vm == 1 ? 1 : min(PerRow(), g.n - r * PerRow());
    int best = -1; float bestD = FLT_MAX;
    for (int k = 0; k < cnt; k++) {
        int i = RowPage(r, k);
        int x0 = offx + g.px[i], x1 = x0 + PW(i);
        float d = v.x < x0 ? (float)(x0 - v.x) : v.x > x1 ? (float)(v.x - x1) : 0;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return false;
    float k = (float)(g.zoom * g.dpi);
    if (k <= 0) return false;
    page = best;
    int py = g.top[r] - g.sy + (g.rh[r] - PH(best)) / 2;
    dx = (v.x - (offx + g.px[best])) / k;
    dy = (v.y - py) / k;
    dx = std::clamp(dx, 0.0f, (float)PW(best) / k);
    dy = std::clamp(dy, 0.0f, (float)PH(best) / k);
    return true;
}

static void SideSync();
static void LayoutMain();
static void StatusRepaint();

static void UpdateTitle() {   // sidebar highlight follows the page under the upper
    int c = g.n ? PageAt(g.sy + g.ch / 3) : -1;   // third. The window text stays "Docsy"
    if (c != g.cur) {                             // - the open file's name shows nowhere.
        g.cur = c;
        if (g.view) SetTimer(g.view, 4, 160, nullptr);   // pre-warm the page's text once
        SideSync();                           // the scroll settles (never mid-gesture)
        StatusRepaint();                      // "Page N of M" moved
    }
}

static void UpdateScroll() {
    g.busy = true;
    g.sy = std::clamp(g.sy, 0, max(0, g.docH - g.ch));
    g.sx = std::clamp(g.sx, 0, max(0, g.docW - CardW()));
    g.busy = false;
    g.scrollTick = GetTickCount64();
}

static void Layout() {
    g.gap = Gap();
    int per = PerRow();
    int rows = RowCount();
    g.top.assign((size_t)rows + 1, 0);
    g.px.assign((size_t)max(g.n, 1), 0);
    g.rh.assign((size_t)max(rows, 1), 0);
    int y = g.gap; g.maxw = 0;
    for (int r = 0; r < rows; r++) {                 // pass 1: row heights and the widest row
        int i0 = g.vm == 1 ? g.cur : r * per;
        int cnt = g.vm == 1 ? 1 : min(per, g.n - r * per);
        int wsum = 0, hmax = 0;
        for (int k = 0; k < cnt; k++) { wsum += PW(i0 + k); hmax = max(hmax, PH(i0 + k)); }
        g.maxw = max(g.maxw, wsum + (cnt - 1) * g.gap);
        g.top[r] = y; g.rh[r] = hmax;
        y += hmax + g.gap;
    }
    g.top[rows] = y;
    g.docH = rows ? y : 0;
    g.docW = rows ? g.maxw + 2 * g.gap : 0;
    for (int r = 0; r < rows; r++) {                 // pass 2: each row centred on the widest one
        int i0 = g.vm == 1 ? g.cur : r * per;
        int cnt = g.vm == 1 ? 1 : min(per, g.n - r * per);
        int wsum = 0;
        for (int k = 0; k < cnt; k++) wsum += PW(i0 + k);
        int x = g.gap + (g.maxw - (wsum + (cnt - 1) * g.gap)) / 2;
        for (int k = 0; k < cnt; k++) { g.px[i0 + k] = x; x += PW(i0 + k) + g.gap; }
    }
    UpdateScroll();
    InvalidateRect(g.view, nullptr, FALSE);
    StatusRepaint();
    UpdateTitle();
}

static void SetZoom(double z, int ax, int ay) {
    z = std::clamp(z, 0.1, 4.0);
    double ry = g.docH ? (double)(g.sy + ay) / g.docH : 0, rx = g.docW ? (double)(g.sx + ax) / g.docW : 0;
    if (z != g.zoom) ClearCache();
    g.zoom = z; g.fitMode = 2;              // an explicit zoom leaves the fit modes
    Layout();
    g.sy = (int)(ry * g.docH) - ay;
    if (g.docW > CardW()) g.sx = (int)(rx * g.docW) - ax;
    UpdateScroll();
    InvalidateRect(g.view, nullptr, FALSE);
    UpdateTitle();
    StatusRepaint();
}

// KillerPDF's FitToWidth / FitToPage, mode-aware: the fit target is the widest
// ROW (a spread in TwoPage, 3 columns in Grid), and Page additionally height-
// limits the current row. The zoom% the status line and the combo show derives
// from the resulting scale.
static void ApplyFit(bool pageFit = false) {
    if (!g.n || g.cw <= 0) return;
    int W = CardW() - SBar();   // the card's inner width minus the reserved overlay-bar column
    int per = PerRow();
    int c = PageAt(g.sy + g.ch / 3);
    int r = g.vm == 1 ? 0 : c / per;
    double z;
    if (g.vm == 2) {                         // the current spread's two pages share the row
        int a = c - c % 2, b = min(g.n - 1, a + 1);
        z = (W - 3.0 * Gap()) / (max(1.f, g.pw[a] + g.pw[b]) * g.dpi);
    } else if (g.vm == 3) {
        float mw = 0; for (float w : g.pw) mw = max(mw, w);
        z = (W - 4.0 * Gap()) / (per * mw * g.dpi);
    } else {
        int i = g.vm == 1 ? max(0, g.cur) : 0;
        float mw = g.vm == 1 ? g.pw[i] : 0;
        if (g.vm == 0) for (float w : g.pw) mw = max(mw, w);
        z = (W - 2.0 * Gap()) / (max(1.f, mw) * g.dpi);
    }
    if (pageFit) {                           // the current row must also fit vertically
        int i0 = g.vm == 1 ? max(0, g.cur) : r * per;
        int cnt = g.vm == 1 ? 1 : min(per, g.n - i0);
        float rh = 0; for (int k = 0; k < cnt; k++) rh = max(rh, g.ph[i0 + k]);
        z = min(z, (g.ch - 2.0 * Gap()) / (max(1.f, rh) * g.dpi));
    }
    SetZoom(z, 0, 0);
    g.fitMode = pageFit ? 1 : 0;
    StatusRepaint();
}

static void ScrollTo(int x, int y) {
    x = std::clamp(x, 0, max(0, g.docW - CardW()));
    y = std::clamp(y, 0, max(0, g.docH - g.ch));
    if (x == g.sx && y == g.sy) return;
    g.sx = x; g.sy = y;
    g.scrollTick = GetTickCount64();
    InvalidateRect(g.view, nullptr, FALSE);
    UpdateTitle();
}

// Single mode's page flip (KillerPDF's F6 view): the document IS one page, so
// navigation replaces it and parks at the top. The sidebar's current-page ring
// and the footer's "Page N of M" follow.
static void SetPage(int i) {
    if (!g.n) return;
    i = std::clamp(i, 0, g.n - 1);
    g.cur = i;
    if (g.fitMode != 2) ApplyFit(g.fitMode == 1); else Layout();
    g.sx = 0; g.sy = 0;
    UpdateScroll();
    if (g.view) SetTimer(g.view, 4, 160, nullptr);   // pre-warm the new page's text
    SideSync();
    StatusRepaint();
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
        g.doc = d; g.n = (int)n; g.pw = w; g.ph = h; g.name = nm; g.path = full;
        g.sx = g.sy = 0; g.fitMode = 0; g.cur = -1;   // -1 so LayoutMain's UpdateTitle sees the
                                                      // change and syncs the sidebar highlight
        // fresh sidebar for the new document
        for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
        sd.th.clear(); sd.scroll = 0; sd.cur = -1; sd.hold = 0; sd.tw = 0; sd.cw = 0; sd.ch = 0;
        g.txt.clear();               // the new document's text geometry starts fresh
        SelClear();
        if (g_find) { KillTimer(g_find, 1); KillTimer(g_find, 2); ShowWindow(g_find, SW_HIDE); }
        fd = Find{};                 // the Find bar's results die with the old document
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
        // KillerPDF's page item: a full-width ListBoxItem band (Padding 8,6) around a
        // StackPanel (Margin 8,2) holding the thumb and the label line. Consecutive
        // bands touch, the way list items stack - a selected band reads as one card.
        int padT = (int)(8 * g.dpi);                    // 6 item pad + 2 stack margin above the thumb
        int below = (int)(28 * g.dpi);                  // 2 gap + ~20 label line + 6 pad below it
        sd.top.assign((size_t)g.n + 1, 0);
        int y = (int)(8 * g.dpi);                       // the first band's top
        for (int i = 0; i < g.n; i++) { sd.top[i] = y + padT; y = sd.top[i] + STH(i) + below; }
        sd.top[g.n] = y;
        sd.docH = y + (int)(8 * g.dpi);
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
        int y0 = sd.top[c] - (int)(8 * g.dpi);                        // the item band's edges
        int y1 = sd.top[c] + STH(c) + (int)(28 * g.dpi);
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

static HFONT SideFont() {   // the page label: BrandSansFont ("Killer Sans") at FontSize 14
    if (!g_sidefont)
        g_sidefont = CreateFontW(-(int)(14 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Killer Sans");
    return g_sidefont;
}

static HBRUSH ThumbEdgeBrush()   { static HBRUSH b = CreateSolidBrush(RGB(0x33, 0x33, 0x33)); return b; }  // SurfaceBrush
static HBRUSH RowHoverBrush()    { static HBRUSH b = CreateSolidBrush(RGB(0x40, 0x40, 0x40)); return b; }  // RowHoverBrush

static void TrackLeave(HWND h) { TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t); }

// Paints the sidebar rail: the shared dark noisy field (the same surface the
// window margins and footer paint, so the rail belongs to no one component),
// KillerPDF's page items - a full-width band that lights RowHoverBrush under
// the cursor and the SelectionBg green gradient behind the current page, a
// "Page N" label in Killer Sans 14 - and the rail's own overlay bar inside the
// reserved 12px gutter on the right.
static void SideDraw(HDC dc, int w, int h, int xoff) {
    RECT r{ xoff, 0, xoff + w, h };
    FillRect(dc, &r, GrainBrush(1));
    if (!g.n || sd.cw <= 0 || sd.top.empty()) return;
    int avail = sd.cw - SBar();
    HGDIOBJ of = SelectObject(dc, SideFont());
    SetBkMode(dc, TRANSPARENT);
    HDC m = CreateCompatibleDC(dc);
    int padT = (int)(8 * g.dpi), labH = (int)(20 * g.dpi), padB = (int)(6 * g.dpi), gap = (int)(2 * g.dpi);
    int first = SidePageAt(sd.scroll);
    for (int i = first; i < g.n && sd.top[i] < sd.scroll + h; i++) {
        int th = STH(i);
        int x = xoff + (avail - sd.tw) / 2;
        int y = sd.top[i] - sd.scroll;
        int btop = y - padT, bbot = y + th + gap + labH + padB;   // the item band
        if (i == sd.cur || i == sd.hover) {
            RECT band{ xoff, max(0, btop), xoff + avail, min(h, bbot) };
            if (i == sd.cur) {                                    // SelectionBg, top #387552 -> bottom #104E2B
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
        RECT tr{ x, y, x + sd.tw, y + th };
        auto it = sd.th.find(i);
        if (it != sd.th.end() && it->second.b) {
            HGDIOBJ o = SelectObject(m, it->second.b);
            BitBlt(dc, x, y, sd.tw, th, m, 0, 0, SRCCOPY);
            SelectObject(m, o);
            FrameRect(dc, &tr, ThumbEdgeBrush());
        } else {
            FillRect(dc, &tr, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
            FrameRect(dc, &tr, ThumbEdgeBrush());
        }
        wchar_t num[24];
        swprintf_s(num, L"Page %d", i + 1);
        RECT nr{ xoff, y + th + gap, xoff + avail, y + th + gap + labH };
        SetTextColor(dc, i == sd.cur ? RGB(0xff, 0xff, 0xff) : RGB(0xa3, 0xa3, 0xa3));
        DrawTextW(dc, num, -1, &nr, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    DeleteDC(m);
    SelectObject(dc, of);
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
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // the grain tile phase:
            SetBrushOrgEx(sd.bdc, -po.x, -po.y, nullptr);           // anchored to the window, like
            SideDraw(sd.bdc, sd.cw, sd.ch, 0);                      // every other pane's, so the
            BitBlt(dc, 0, 0, sd.cw, sd.ch, sd.bdc, 0, 0, SRCCOPY);  // shared field runs unbroken
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
        if (g.n && sd.docH > sd.ch && p.x >= sd.cw - gb && p.x < sd.cw) {
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
        bool hot = g.n && sd.docH > sd.ch && p.x >= sd.cw - SBar() && p.x < sd.cw;
        if (hot) { int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp); hot = p.y >= tp && p.y < tp + tl; }
        int hov = (!sd.barDrag && g.n && sd.docH > 0 && p.x < sd.cw - SBar()
                   && p.y + sd.scroll < sd.docH) ? SidePageAt(p.y + sd.scroll) : -1;
        if (hov != sd.hover) {
            sd.hover = hov;
            if (hov >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
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
        if (sd.hover != -1) { sd.hover = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        bool bar = sd.barDrag;
        sd.barDrag = false;
        if (GetCapture() == h) ReleaseCapture();
        if (!bar && sd.moved <= (int)(4 * g.dpi) && sd.hit >= 0 && sd.hit < g.n) {
            sd.hold = GetTickCount64() + 600;   // the jump must not scroll the sidebar back
            if (g.vm == 1) SetPage(sd.hit);     // single mode: a thumb click flips to that page
            else ScrollTo(g.sx, max(0, PageTop(sd.hit) - Gap() / 2));
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

// KillerPDF's document card: the shared dark field fills the window and the
// canvas is a rounded card on it - RadCard 6 corners, a 1px PaneBorderBrush
// #555555 edge, inset 8px from the window's right edge, tucked -1 under the
// band above (so its top border line hides exactly like KillerPDF's) and
// sitting on the footer below. The field is painted by every pane from one
// tile, so the margins, the rail and the footer read as one container that no
// single component owns.
//
// The corners are WPF arcs there, and GDI regions here can only step them -
// the "small cuts" the reference never shows. The split that keeps the best of
// both renderers: GDI owns the straight edges (its region fill and FrameRgn
// ring are pixel-exact, which GDI+ pens are not), and GDI+ repaints just the
// four corner squares the way the Border composes them - field grain outside
// the outer RadCard arc, canvas grain inside its radius-1 inner arc, the 1px
// PaneBorderBrush #555555 band between, all anti-aliased, on grain tiles
// translated to GDI's own brush-org phase. The pages keep the cheap region
// clip.
static int CardW() { return max(0, g.cw - (int)(8 * g.dpi)); }

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

static int g_grainPX = 0, g_grainPY = 0;   // GDI's brush-org phase, stashed by the paint

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

// GDI's rounded regions can only step around the corners - the "small cuts"
// KillerPDF never shows. So after the GDI fill and ring (whose straight edges
// are pixel-exact and stay), the four corner squares are repainted by GDI+ the
// way the WPF Border composes them: field grain outside the outer RadCard arc,
// canvas grain inside its radius-1 inner arc, the 1px PaneBorderBrush #555555
// band between - every edge anti-aliased. The grain tiles are translated to
// GDI's brush-org phase, so the patches melt into the pane without a seam, and
// the band meets FrameRgn's straight ring flush at the arc tangents.
static HBRUSH CardEdgeBrush()   { static HBRUSH b = CreateSolidBrush(RGB(0x55, 0x55, 0x55)); return b; }  // PaneBorderBrush

static void CardCorners(HDC dc, int x, int y, int w, int h, int rad) {
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath* outer = CardPath((float)x, (float)y, (float)w, (float)h, (float)rad, 0);
    Gdiplus::GraphicsPath* inner = CardPath((float)x, (float)y, (float)w, (float)h, (float)rad, 1);
    Gdiplus::GraphicsPath band;          // the ring: even-odd between the two arcs
    band.AddPath(outer, FALSE);
    band.AddPath(inner, FALSE);
    band.SetFillMode(Gdiplus::FillModeAlternate);
    Gdiplus::SolidBrush edge(Gdiplus::Color(255, 0x55, 0x55, 0x55));
    int sq = rad + 2;                    // the arc plus its AA tails, per corner
    RECT cs[4] = {
        { x, y, x + sq, y + sq },                    // top-left (its top row stays tucked)
        { x + w - sq, y, x + w, y + sq },            // top-right
        { x, y + h - sq, x + sq, y + h },            // bottom-left
        { x + w - sq, y + h - sq, x + w, y + h },    // bottom-right
    };
    for (int i = 0; i < 4; i++) {
        gs.SetClip(Gdiplus::Rect(cs[i].left, cs[i].top, sq, sq), Gdiplus::CombineModeReplace);
        if (Gdiplus::TextureBrush* f = GrainTex(1))   // the field the card floats on
            gs.FillRectangle(f, Gdiplus::Rect(cs[i].left, cs[i].top, sq, sq));
        if (Gdiplus::TextureBrush* c = GrainTex(0))   // the canvas, inside the inner arc
            gs.FillPath(c, inner);
        else { Gdiplus::SolidBrush cb(Gdiplus::Color(255, 58, 58, 58)); gs.FillPath(&cb, inner); }
        gs.FillPath(&edge, &band);                    // the PaneBorder band, arcs blended
    }
    delete outer;
    delete inner;
}

// Find highlights on one visible page (KillerPDF's AddSearchHighlight): a rounded
// box around every matched word of the page - dim orange fill, faint stroke - and
// the cursor match bright with the 3.5px accent ring; the box pads 30% of the
// shorter side and rounds at 60% of the pad, so the wrap breathes like the
// original's and stays consistent across font sizes.
static void FindPaintPage(HDC dc, int page, int px, int py) {
    if (!fd.open || fd.hits.empty()) return;
    auto it = std::lower_bound(fd.hits.begin(), fd.hits.end(), page,
                               [](const FHit& h, int p) { return h.page < p; });
    if (it == fd.hits.end() || it->page != page) return;
    float k = (float)(g.zoom * g.dpi);
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush dim(Gdiplus::Color(70, 255, 165, 0));
    Gdiplus::SolidBrush hot(Gdiplus::Color(150, 255, 190, 0));
    Gdiplus::Pen dimPen(Gdiplus::Color(140, 255, 165, 0), 1.0f * (float)g.dpi);
    Gdiplus::Pen hotPen(Gdiplus::Color(255, 0x22, 0xbb, 0x56), (float)(3.5 * g.dpi));
    for (; it != fd.hits.end() && it->page == page; ++it) {
        bool cur = (int)(it - fd.hits.begin()) == fd.cur;
        const FRect& r = it->r;
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

static void Draw(HDC dc, int w, int h, int xoff) {
    RECT r{ xoff, 0, xoff + w, h };
    FillRect(dc, &r, GrainBrush(1));   // the field the card floats on
    int cw = min(w, CardW());
    int rad = max(2, (int)(6 * g.dpi));
    bool card = cw > 2 * rad + 4 && h > 2 * rad + 2;
    HRGN clip = card ? CreateRoundRectRgn(xoff + 1, -1, xoff + cw - 1, h - 1, 2 * rad, 2 * rad) : nullptr;
    if (clip) SelectClipRgn(dc, clip);   // ContentClip: the card's inside, 1px inside its border
    HRGN canvas = card ? CreateRoundRectRgn(xoff, -1, xoff + cw + 1, h + 1, 2 * rad, 2 * rad) : nullptr;
    if (canvas) SelectClipRgn(dc, canvas);   // CanvasClip: the fill and the frame share ONE
    RECT cr{ xoff, 0, xoff + cw, h };        // region, so no bare field ring can open between
    FillRect(dc, &cr, GrainBrush(0));        // the card's canvas and its edge at any DPI.
                                             // GDI's rounded regions give up their last row
                                             // and column, so the rect runs one past the
                                             // card and the canvas still reaches the edge
    if (canvas) SelectClipRgn(dc, clip);     // the pages stay 1px inside the frame
    if (!g.n) {
        int sz = (int)(128 * g.dpi);
        if (!g.logo) g.logo = (HICON)LoadImageW(g.hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR);
        int cy = h / 2;
        if (g.logo) DrawIconEx(dc, xoff + (cw - sz) / 2, cy - sz - (int)(6 * g.dpi), g.logo, sz, sz, 0, nullptr, DI_NORMAL);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(200, 200, 205));
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT tr{ xoff, cy + (int)(8 * g.dpi), xoff + cw, h };
        DrawTextW(dc, L"Docsy  -  Ctrl+O or drop a PDF here", -1, &tr, DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (clip) { SelectClipRgn(dc, nullptr); DeleteObject(clip); }
        if (canvas) { FrameRgn(dc, canvas, CardEdgeBrush(), 1, 1); DeleteObject(canvas); }
        if (card) CardCorners(dc, xoff, -1, cw, h + 1, rad);
        return;
    }
    HDC m = CreateCompatibleDC(dc);
    int gut = g.docH > h ? SBar() : 0;   // the reserved bar column, like KillerPDF's Grid layout
    int offx = g.docW < cw - gut ? (cw - gut - g.docW) / 2 : -g.sx;
    bool selOn = g.selRange || (g.selArm && g.selDrag);   // quads paint live during the drag
    int r0 = RowAt(g.sy), r1 = RowAt(g.sy + h - 1);
    int first = RowPage(r0, 0), last = first;
    int per = PerRow();
    for (int row = r0; row <= r1; row++) {
        int cnt = g.vm == 1 ? 1 : min(per, g.n - row * per);
        for (int k = 0; k < cnt; k++) {
            int i = RowPage(row, k);
            if (i < 0 || i >= g.n) continue;
            last = i;
            int pw = PW(i), ph = PH(i);
            int x = xoff + offx + g.px[i];
            int y = g.top[row] - g.sy + (g.rh[row] - ph) / 2;   // pages ride centred in their row
            if (y + ph < 0) continue;
            HBITMAP b = GetBmp(i, pw, ph);
            if (b) {
                HGDIOBJ o = SelectObject(m, b);
                // DocInvert (the footer moon): KillerPDF flips the document images to
                // their negative - the page rect is opaque, so NOTSRCCOPY inverts it
                // during the blit and the grain margins keep their tone.
                BitBlt(dc, x, y, pw, ph, m, 0, 0, g.inv ? NOTSRCCOPY : SRCCOPY);
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
            if (fd.open) FindPaintPage(dc, i, x, y);
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
    if (canvas) {   // the 1px card edge, KillerPDF's PaneBorderBrush #555555: pixel-exact
        FrameRgn(dc, canvas, CardEdgeBrush(), 1, 1);   // on the straights, drawn before the
        DeleteObject(canvas);                          // corners so the AA arcs land on top
    }
    if (card) CardCorners(dc, xoff, -1, cw, h + 1, rad);   // WPF-smooth corner arcs over the
                                                           // steps the regions leave behind
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
    int cw = min(w, CardW());            // the bars ride inside the card, not on the window gutter
    bool vshow = g.docH > h;
    if (vshow) {
        int tl, tp; BarThumb(g.docH, h, g.sy, tl, tp);
        bool hot = g.vHot || g.vDrag;
        int bw = hot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);
        RoundRectFill(dc, xoff + cw - gb + (gb - bw) / 2, tp, bw, tl, r, hot);
    }
    if (g.docW > cw) {
        int track = cw - (vshow ? gb : 0);
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
        SetZoom(g.zoom * std::pow(1.12, d / 120.0), p.x, p.y);
    } else if (LOWORD(wp) & MK_SHIFT) ScrollTo(g.sx - d * (int)(g.dpi * 80) / 120, g.sy);
    else {
        // Single mode flips at the edges: scrolling past a page's end brings the
        // next one (KillerPDF's Single keeps one page up and navigates by flip).
        if (g.vm == 1) {
            if (d < 0 && g.sy >= max(0, g.docH - g.ch)) { SetPage(g.cur + 1); return; }
            if (d > 0 && g.sy <= 0) { SetPage(g.cur - 1); return; }
        }
        ScrollTo(g.sx, g.sy - d * (int)(g.dpi * 80) / 120);
    }
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
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // the grain tile phase is the
            SetBrushOrgEx(g.bdc, -po.x, -po.y, nullptr);            // window's, so the field the
            g_grainPX = -po.x; g_grainPY = -po.y;   // corner patches phase their tiles to it
            Draw(g.bdc, g.cw, g.ch, 0);                             // card floats on never shows a
            PaintViewBars(g.bdc, g.cw, g.ch, 0);                    // seam at a pane boundary
            BitBlt(dc, 0, 0, g.cw, g.ch, g.bdc, 0, 0, SRCCOPY);
            if (g_find && fd.open) InvalidateRect(g_find, nullptr, FALSE);   // the bar's backdrop
            SetTimer(h, 1, 5, nullptr);                              // rides the view's own pixels
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 6) {                         // the full-screen hint's expiry repaint
            KillTimer(h, 6);
            if (GetTickCount64() >= g.hintUntil) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (wp == 4) {                         // deferred text pre-warm: OCR the settled
            KillTimer(h, 4);                   // page once it exists and scrolling has been
            if (g.n && !g.selArm) {            // quiet for a moment - off the click path
                if (GetTickCount64() - g.scrollTick > 250) {
                    int pg = PageAt(g.sy + g.ch / 3);
                    if (g.txt.find(pg) == g.txt.end()) TtFor(pg);
                } else {
                    SetTimer(h, 4, 160, nullptr);   // still moving: try again after it settles
                }
            }
            return 0;
        }
        KillTimer(h, 1); Prefetch(); return 0;
    case WM_MOUSEWHEEL: ViewWheel(wp, lp); return 0;
    case WM_MOUSEHWHEEL:
        ScrollTo(g.sx + GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 80) / 120, g.sy); return 0;
    case WM_KEYDOWN: return 0;   // keys live on the main window
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetCapture(h);
        int gb = SBar();
        int cw = CardW();
        bool vshow = g.n && g.docH > g.ch;
        if (vshow && p.x >= cw - gb && p.x < cw) {   // the vertical bar: inside the card's edge
            int tl, tp; BarThumb(g.docH, g.ch, g.sy, tl, tp);
            if (p.y >= tp && p.y < tp + tl) { g.vDrag = true; g.vGrab = p.y - tp; }
            else ScrollTo(g.sx, g.sy + (p.y < tp ? -(int)(g.ch * 0.9) : (int)(g.ch * 0.9)));
            return 0;
        }
        if (g.n && g.docW > cw && p.y >= g.ch - gb && p.x < cw - (vshow ? gb : 0)) {
            int tl, tp; BarThumb(g.docW, cw - (vshow ? gb : 0), g.sx, tl, tp);
            if (p.x >= tp && p.x < tp + tl) { g.hDrag = true; g.hGrab = p.x - tp; }
            else ScrollTo(g.sx + (p.x < tp ? -(int)(cw * 0.9) : (int)(cw * 0.9)), g.sy);
            return 0;
        }
        if (g.sideOn && g.n && p.x < (int)(5 * g.dpi)) {   // the splitter: drag to resize the sidebar
            g.resizing = true; g.rsX = p.x; g.rsW0 = SW();
            return 0;
        }
        // flowing text selection: a press ON text starts KillerPDF's caret drag. A press
        // anywhere else starts nothing - the wheel and the keys own scrolling, so the
        // old grab-and-pan gesture (which fought the selection drag) is gone.
        int pg; float fx, fy;
        if (ViewToPage(p, CardW(), g.ch, pg, fx, fy)) {
            TxtPage* pt = TtFor(pg);   // builds once if the pre-warm has not reached it yet
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
            int cw = CardW();
            int tl, tp; BarThumb(g.docW, cw - ((g.n && g.docH > g.ch) ? SBar() : 0), g.sx, tl, tp);
            ScrollTo((int)((long long)(p.x - g.hGrab) * max(0, g.docW - cw) / max(1, cw - (g.docH > g.ch ? SBar() : 0) - tl)), g.sy);
            return 0;
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
            if (ViewToPage(p, CardW(), g.ch, pg, fx, fy)) {
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
        bool vhot = g.n && g.docH > g.ch && p.x >= CardW() - SBar() && p.x < CardW();
        if (vhot) { int tl, tp; BarThumb(g.docH, g.ch, g.sy, tl, tp); vhot = p.y >= tp && p.y < tp + tl; }
        bool hhot = !vhot && g.n && g.docW > CardW() && p.y >= g.ch - SBar() && p.x < CardW() - (vhot ? SBar() : 0);
        if (hhot) { int tl, tp; BarThumb(g.docW, CardW() - (vhot ? SBar() : 0), g.sx, tl, tp); hhot = p.x >= tp && p.x < tp + tl; }
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
        g.vDrag = g.hDrag = false;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK: {
        // double-click word selection, copied at once (KillerPDF's TrySelectWordAtPosition)
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int pg; float fx, fy;
        if (ViewToPage(p, CardW(), g.ch, pg, fx, fy)) {
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
            // I-beam over selectable text (KillerPDF's SelectableTextCursor), peeked
            // without building: the cursor advertises text only once the page's OCR
            // pass has landed, and the arrow elsewhere says a press does nothing
            int pg; float fx, fy;
            if (ViewToPage(p, CardW(), g.ch, pg, fx, fy)) {
                TxtPage* pt = TtPeek(pg);
                if (pt && TtOverText(*pt, fx, fy)) { SetCursor(LoadCursorW(nullptr, IDC_IBEAM)); return TRUE; }
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
        }
        break;
    case WM_DROPFILES: HandleDrop((HDROP)wp); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------- footer status bar (KillerPDF's status bar) ----------------
// One full-width child under the document pane, painted in the same #1c1c1c +
// grain tone as the sidebar and title bar (KillerPDF paints the footer
// BackgroundBrush under the shared tile), so the rail + footer read as one
// container wrapping the page area on two sides. Cells right-to-left,
// KillerPDF's own arrangement: version | zoom out / fit dropdown / zoom in |
// fullscreen + doc-invert | the F5-F8 view-mode radios | paper size | the
// status line. Every metric below is the original's: FooterBorder MinHeight
// 24, 24x20 nav buttons, 25x20 mode radios, a fixed 78x20 zoom box, dividers
// 1x14 with their stated margins, the 16px FooterStatusInset on the status
// line, 20px of trailing margin on the copyright, and text in the bundled
// Killer Sans at FontSize 16 (the combo uses Killer Sans UI at 10).

#define APP_VER L"1.0"

static int SBH() { return (int)(24 * g.dpi); }        // FooterBorder MinHeight=24

static HFONT g_stfont = nullptr, g_stfontL = nullptr, g_stfontF = nullptr,
              g_stcombo = nullptr, g_stglyph = nullptr, g_stchev = nullptr;
static HFONT StFont()    { return CaptionFont(g_stfont, 16, false, L"Killer Sans"); }        // BrandSansFont 16
static HFONT StFontL()   { return CaptionFont(g_stfontL, 16, false, L"Killer Sans"); }       // version cell 16
static HFONT StFontF()   { return CaptionFont(g_stfontF, 16, false, L"Segoe UI"); }          // non-latin status fallback
static HFONT StCombo()   { return CaptionFont(g_stcombo, 10, false, L"Killer Sans UI"); }    // UiFont 10
static HFONT StGlyph()   { return CaptionFont(g_stglyph, 13, false, L"Segoe MDL2 Assets"); }
static HFONT StChevron() { return CaptionFont(g_stchev, 7, false, L"Segoe MDL2 Assets"); }

// The embedded Killer Sans subsets cover latin-1 plus the dashes/quotes/dots
// (see src/fonts): the routine UI strings all live there. A held status can
// echo a file NAME with any script in it, so such strings draw in Segoe UI
// instead of showing .notdef boxes for the glyphs the subset drops.
static bool LatinText(const wchar_t* s) {
    for (const wchar_t* p = s; *p; p++) {
        if (*p < 0x0100) continue;
        if (*p >= 0x2013 && *p <= 0x2026) continue;
        return false;
    }
    return true;
}

static HBRUSH DividerBrush()     { static HBRUSH b = CreateSolidBrush(RGB(0x4a, 0x4a, 0x4a)); return b; }
static HBRUSH AccentBrush()      { static HBRUSH b = CreateSolidBrush(RGB(0x22, 0xbb, 0x56)); return b; }
static HBRUSH ComboFillBrush()   { static HBRUSH b = CreateSolidBrush(RGB(0x14, 0x14, 0x14)); return b; }  // MenuBackgroundBrush
static HBRUSH ComboBorderBrush() { static HBRUSH b = CreateSolidBrush(RGB(0x40, 0x40, 0x40)); return b; }  // MenuBorderBrush
static HBRUSH PopBrush()         { static HBRUSH b = CreateSolidBrush(RGB(0x14, 0x14, 0x14)); return b; }
static HBRUSH PopHoverBrush()    { static HBRUSH b = CreateSolidBrush(RGB(0x40, 0x40, 0x40)); return b; }  // RowHoverBrush

static HDC s_measdc = nullptr;
static int TextW(HFONT f, const wchar_t* s) {          // extent at the current font
    if (!s_measdc) s_measdc = GetDC(nullptr);
    HGDIOBJ o = SelectObject(s_measdc, f);
    SIZE sz{}; GetTextExtentPoint32W(s_measdc, s, (int)wcslen(s), &sz);
    SelectObject(s_measdc, o);
    return sz.cx;
}

// ---- the status line, SetStatusHeld semantics ----
// A held message (file size flash, about) briefly wins over the routine
// page/zoom line and expires on its own; routine updates cannot stomp it
// (MainWindow.xaml.cs's _statusHoldUntil, timed here instead ofDispatcher).

static const wchar_t* FitName() {
    return g.fitMode == 1 ? L"Fit Page" : g.fitMode == 0 ? L"Fit Width" : L"Custom";
}

static std::wstring StatusRoutine() {
    if (!g.n) return L"Docsy  -  Ctrl+O or drop a PDF here";
    int c = g.cur >= 0 ? g.cur : 0;
    wchar_t b[160];
    swprintf_s(b, L"Page %d of %d, %s (%d%%)", c + 1, g.n, FitName(), (int)std::lround(g.zoom * 100));
    return b;
}

static void StatusRepaint() { if (g.status) InvalidateRect(g.status, nullptr, FALSE); }

static void SetStatusHold(const std::wstring& t, unsigned ms = 2500) {
    g.statusHoldText = t;
    g.statusHoldUntil = GetTickCount64() + ms;
    if (g.status) SetTimer(g.status, 1, 250, nullptr);
    StatusRepaint();
}

static std::wstring StatusTextNow() {
    return GetTickCount64() < g.statusHoldUntil ? g.statusHoldText : StatusRoutine();
}

// ---- KillerPDF's PageSizeFormatter ----
// Known sizes live in PDF points and match on (short, long) sides at +/-3pt so
// orientation never hides the name; Tabloid landscape reports as Ledger. The
// primary unit is millimetres for the A series, inches for the US sizes, and
// inches for anything unknown (useMetric = known?.Metric ?? false).
struct KnownSize { const wchar_t* name; double w, h; bool metric; };
static const KnownSize KSizes[] = {
    { L"A0", 2384, 3370, true },  { L"A1", 1684, 2384, true },  { L"A2", 1191, 1684, true },
    { L"A3", 842, 1191, true },   { L"A4", 595, 842, true },    { L"A5", 420, 595, true },
    { L"A6", 298, 420, true },    { L"Letter", 612, 792, false }, { L"Legal", 612, 1008, false },
    { L"Tabloid", 792, 1224, false }, { L"ANSI C", 1224, 1584, false },
    { L"ANSI D", 1584, 2448, false }, { L"ANSI E", 2448, 3168, false },
};

// .NET's "0.#" / "0.##": round to the decimals, then trim a bare ".0"/".00".
static void FmtNum(wchar_t* out, size_t n, double v, int dec) {
    swprintf_s(out, n, dec == 1 ? L"%.1f" : L"%.2f", v);
    std::wstring s = out;
    size_t dot = s.find(L'.');
    if (dot != std::wstring::npos) {
        size_t last = s.find_last_not_of(L'0');
        if (last == dot) last = dot - 1;               // all decimals were zeros
        s.erase(last + 1);
    }
    wcscpy_s(out, n, s.c_str());
}

// The current page's size: the footer label ("A4  210 x 297 mm") and the full
// px | in | mm | pt breakdown (the original shows it in the crop dialog; docsy
// flashes it as a held status on click).
static void PaperLabel(std::wstring& label, std::wstring& details) {
    label.clear(); details.clear();
    if (!g.n || g.cur < 0) return;
    int i = g.cur;
    double wpt = g.pw[i], hpt = g.ph[i];
    double inW = wpt / 72.0, inH = hpt / 72.0;
    double mmW = inW * 25.4, mmH = inH * 25.4;
    double sh = min(wpt, hpt), lo = max(wpt, hpt);
    const KnownSize* known = nullptr;
    for (const KnownSize& s : KSizes)
        if (fabs(sh - s.w) <= 3 && fabs(lo - s.h) <= 3) { known = &s; break; }
    wchar_t name[24] = L"";
    if (known) {
        wcscpy_s(name, known->name);
        if (!wcscmp(name, L"Tabloid") && wpt > hpt) wcscpy_s(name, L"Ledger");
    }
    bool metric = known && known->metric;
    wchar_t a[32], b[32], prim[80];
    FmtNum(a, 32, metric ? mmW : inW, metric ? 1 : 2);
    FmtNum(b, 32, metric ? mmH : inH, metric ? 1 : 2);
    swprintf_s(prim, L"%s x %s %s", a, b, metric ? L"mm" : L"in");
    label = name[0] ? std::wstring(name) + L"  " + prim : prim;
    wchar_t pxw[32], pxh[32], c[32], d[32], e[32], f[32], dt[192];
    swprintf_s(pxw, L"%d", (int)std::lround(inW * 150));
    swprintf_s(pxh, L"%d", (int)std::lround(inH * 150));
    FmtNum(c, 32, inW, 2); FmtNum(d, 32, inH, 2);
    FmtNum(e, 32, mmW, 1); FmtNum(f, 32, mmH, 1);
    swprintf_s(dt, L"%s x %s px at 150 DPI | %s x %s in | %s x %s mm | %.1f x %.1f pt",
               pxw, pxh, c, d, e, f, wpt, hpt);
    details = dt;
}

// ---- footer actions ----

static void SetViewMode(int m) {
    if (!g.n || m == g.vm || m < 0 || m > 3) return;
    int c = PageAt(g.sy + g.ch / 3);
    g.vm = m;
    if (g.fitMode != 2) ApplyFit(g.fitMode == 1); else Layout();
    ScrollTo(g.sx, max(0, PageTop(c) - Gap() / 2));   // the current page's row stays in sight
    InvalidateRect(g.view, nullptr, FALSE);
    StatusRepaint();
}

// KillerPDF's F11 (Shell/FullScreen.cs): the chrome collapses and the window
// covers the monitor; F11 or Esc brings it back. A short hint on the page
// stands in for the original's ShowFullScreenHint overlay.
static void ToggleFull(HWND h) {
    g.full = !g.full;
    if (g.full) {
        g.fsStyle = GetWindowLongW(h, GWL_STYLE);
        WINDOWPLACEMENT wp{ sizeof wp };
        GetWindowPlacement(h, &wp);
        g.fsPlace = wp;
        MONITORINFO mi{ sizeof mi };
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, (g.fsStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g.hint = L"Esc or F11 exits full screen";
        g.hintUntil = GetTickCount64() + 2200;
        if (g.view) SetTimer(g.view, 6, 250, nullptr);
    } else {
        SetWindowLongW(h, GWL_STYLE, g.fsStyle);
        SetWindowPlacement(h, &g.fsPlace);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    LayoutMain();
    DwmApply(h);
    InvalidateRect(h, nullptr, FALSE);
}

// The moon button: KillerPDF's DocInvert flips the document images to their
// negative (dark reading for light documents). docsy inverts during the blit.
static void ToggleInv() {
    g.inv = !g.inv;
    InvalidateRect(g.view, nullptr, FALSE);
    StatusRepaint();
}

// ---- the zoom dropdown ----
// The combo's items are KillerPDF's exactly: Fit Page, Fit Width, then the
// fixed presets. Wheel over the closed box cycles them (the original's
// PreviewMouseWheel), the popup is a borderless owner-drawn menu.
static const double ZoomPcts[6] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
static const wchar_t* ZoomLabel(int i) {
    static const wchar_t* ls[8] = { L"Fit Page", L"Fit Width", L"50%", L"75%", L"100%", L"125%", L"150%", L"200%" };
    return ls[i];
}
static void ZoomApply(int i) {
    if (i == 0) ApplyFit(true);
    else if (i == 1) ApplyFit();
    else if (i >= 2 && i <= 7) SetZoom(ZoomPcts[i - 2], g.cw / 2, g.ch / 2);
}
static int ZoomCurrent() {                       // which item the closed box shows
    if (g.fitMode == 1) return 0;
    if (g.fitMode == 0) return 1;
    double pct = g.zoom * 100;
    int best = 4; double bd = 1e9;
    for (int i = 0; i < 6; i++) {
        double d = fabs(ZoomPcts[i] * 100 - pct);
        if (d < bd) { bd = d; best = i + 2; }
    }
    return best;
}
static void ZoomCycle(int dir) { ZoomApply(std::clamp(ZoomCurrent() + dir, 0, 7)); }

// ---- footer geometry: KillerPDF's FooterToolsPanel, measured right-to-left ----
// The version cell ends 20px short of the right edge (copyright margin); the
// tools cell stacks against it with no extra padding (FooterCellPadding = 0 on
// the dark theme): zoom in | combo 78 wide with 1px margins | zoom out |
// divider (1x14, m 5,0) | invert + fullscreen, butted together | divider |
// four 25px mode radios, butted | divider (m 2,0,5,0) | paper size, MinWidth
// 90, margins 4,0,5,0. The status line takes the rest, its text inset 16px.

enum { ST_STATUS, ST_SIZE, ST_MCONT, ST_MSINGLE, ST_MTWO, ST_MGRID,
       ST_FULL, ST_INV, ST_ZOUT, ST_COMBO, ST_ZIN, ST_VER, ST_COUNT };
static RECT g_stR[ST_COUNT];

static void StatusRects(int w) {
    int dpi = g.dpi;
    int btnH = (int)(20 * dpi), by = (SBH() - btnH) / 2;
    for (auto& r : g_stR) r = { 0, 0, 0, 0 };
    int x = w;
    int wC = TextW(StFontL(), L"\u00A9 2026 Docsy"), wV = TextW(StFontL(), L"v" APP_VER);
    int vw = (int)(20 * dpi) + wC + (int)(12 * dpi) + wV + (int)(8 * dpi) + 1 + (int)(5 * dpi);
    g_stR[ST_VER] = { x - vw, 0, x, SBH() };
    x -= vw;
    int bw24 = (int)(24 * dpi);
    x -= bw24;  g_stR[ST_ZIN] = { x, by, x + bw24, by + btnH };
    x -= (int)(1 * dpi);                                     // ZoomBox Margin="1,0"
    int cw = (int)(78 * dpi);                                // ZoomBox Width=78
    x -= cw;    g_stR[ST_COMBO] = { x, by, x + cw, by + btnH };
    x -= (int)(1 * dpi);
    x -= bw24;  g_stR[ST_ZOUT] = { x, by, x + bw24, by + btnH };
    x -= (int)(5 * dpi) + 1 + (int)(5 * dpi);                // divider Margin="5,0"
    x -= bw24;  g_stR[ST_INV] = { x, by, x + bw24, by + btnH };
    x -= bw24;  g_stR[ST_FULL] = { x, by, x + bw24, by + btnH };
    x -= (int)(5 * dpi) + 1 + (int)(5 * dpi);                // divider Margin="5,0"
    int mw = (int)(25 * dpi);                                // FooterModeButton 25x20
    x -= mw;    g_stR[ST_MGRID] = { x, by, x + mw, by + btnH };
    x -= mw;    g_stR[ST_MTWO] = { x, by, x + mw, by + btnH };
    x -= mw;    g_stR[ST_MSINGLE] = { x, by, x + mw, by + btnH };
    x -= mw;    g_stR[ST_MCONT] = { x, by, x + mw, by + btnH };
    std::wstring lbl, det; PaperLabel(lbl, det);
    if (!lbl.empty()) {
        x -= (int)(5 * dpi) + 1 + (int)(2 * dpi);            // first divider Margin="2,0,5,0"
        int sw2 = max((int)(90 * dpi), TextW(StFont(), lbl.c_str()) + (int)(6 * dpi));
        x -= (int)(5 * dpi);                                 // PageSizeLabel right margin
        g_stR[ST_SIZE] = { x - sw2, 0, x, SBH() };
        x -= sw2 + (int)(4 * dpi);                           // PageSizeLabel left margin
    }
    g_stR[ST_STATUS] = { 0, 0, max(x, (int)(40 * dpi)), SBH() };
}

static int StatusHit(POINT p) {
    RECT r;
    if (!g.status || !GetClientRect(g.status, &r) || r.right <= 0) return -1;
    StatusRects(r.right);
    for (int i = 0; i < ST_COUNT; i++) {
        if (p.x >= g_stR[i].left && p.x < g_stR[i].right && p.y >= g_stR[i].top && p.y < g_stR[i].bottom)
            return (!g.n && i != ST_STATUS && i != ST_VER) ? -1 : i;
    }
    return -1;
}

static void ZoomBy(double f);   // defined with the main window's keys
static void ZoomPopupOpen();
static void ZoomPopupClose();

static void StatusAction(int id) {
    switch (id) {
    case ST_STATUS: {   // StatusText_Click: the open file's size, held while read
        if (g.path.empty()) break;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(g.path.c_str(), GetFileExInfoStandard, &fa)) {
            long long bytes = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            wchar_t sz[40];
            if (bytes >= 1LL << 20) FmtNum(sz, 40, bytes / 1048576.0, 2), wcscat_s(sz, L" MB");
            else if (bytes >= 1LL << 10) FmtNum(sz, 40, bytes / 1024.0, 1), wcscat_s(sz, L" KB");
            else swprintf_s(sz, L"%lld B", bytes);
            SetStatusHold(g.name + L" - " + sz);
        }
        break;
    }
    case ST_SIZE: {     // the crop-size button: docsy flashes the full breakdown
        std::wstring lbl, det; PaperLabel(lbl, det);
        if (!det.empty()) SetStatusHold(det);
        break;
    }
    case ST_MCONT: SetViewMode(0); break;
    case ST_MSINGLE: SetViewMode(1); break;
    case ST_MTWO: SetViewMode(2); break;
    case ST_MGRID: SetViewMode(3); break;
    case ST_FULL: ToggleFull(g.main); break;
    case ST_INV: ToggleInv(); break;
    case ST_ZOUT: ZoomBy(0.8); break;
    case ST_ZIN: ZoomBy(1.25); break;
    case ST_COMBO: if (g.zoomOpen) ZoomPopupClose(); else ZoomPopupOpen(); break;
    case ST_VER: SetStatusHold(L"Docsy " APP_VER L"  -  tiny native Win32 PDF reader"); break;
    }
}

// ---- footer painting ----

static void DrawModeIcon(HDC dc, int id, bool checked) {
    // KillerPDF's FooterModeButton glyphs, drawn with the same vector prims the
    // XAML uses: bars / rectangles, accent on hover, accent + 2px underline
    // while the mode is the checked one.
    bool hot = g.stHot == id;
    HBRUSH b = CreateSolidBrush((hot || checked) ? RGB(0x22, 0xbb, 0x56) : RGB(0xd6, 0xd6, 0xd6));
    RECT r = g_stR[id];
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    if (checked) cy -= (int)(1 * g.dpi);               // the underline lifts the glyph a hair
    if (id == ST_MCONT) {                              // three bars, 13x3 at 5px pitch
        int lw = (int)(13 * g.dpi), lh = max(1, (int)(3 * g.dpi)), pitch = (int)(5 * g.dpi);
        for (int k = -1; k <= 1; k++) {
            RECT l{ cx - lw / 2, cy + k * pitch - lh / 2, cx + lw / 2, cy + k * pitch - lh / 2 + lh };
            FillRect(dc, &l, b);
        }
    } else if (id == ST_MSINGLE) {                     // one 10x13 outline
        RECT o{ cx - (int)(5 * g.dpi), cy - (int)(6 * g.dpi), cx + (int)(5 * g.dpi), cy + (int)(7 * g.dpi) };
        FrameRect(dc, &o, b);
    } else if (id == ST_MTWO) {                        // two 7x13 outlines, 2px apart
        int w7 = (int)(7 * g.dpi);
        RECT a{ cx - w7 - (int)(1 * g.dpi), cy - (int)(6 * g.dpi), cx - (int)(1 * g.dpi), cy + (int)(7 * g.dpi) };
        RECT c2{ cx + (int)(1 * g.dpi), cy - (int)(6 * g.dpi), cx + (int)(1 * g.dpi) + w7, cy + (int)(7 * g.dpi) };
        FrameRect(dc, &a, b); FrameRect(dc, &c2, b);
    } else {                                           // grid: four 6x6 outlines, 1px apart
        int s6 = (int)(6 * g.dpi), s7 = (int)(7 * g.dpi);
        RECT q[4] = {
            { cx - s7, cy - s7, cx - s7 + s6, cy - s7 + s6 },
            { cx + 1,  cy - s7, cx + 1 + s6,  cy - s7 + s6 },
            { cx - s7, cy + 1,  cx - s7 + s6, cy + 1 + s6 },
            { cx + 1,  cy + 1,  cx + 1 + s6,  cy + 1 + s6 },
        };
        for (const RECT& q4 : q) FrameRect(dc, &q4, b);
    }
    if (checked) {
        RECT u{ r.left, r.bottom - max(2, (int)(2 * g.dpi)), r.right, r.bottom };
        FillRect(dc, &u, b);
    }
    DeleteObject(b);
}

static void DrawGlyph(HDC dc, int id, const wchar_t* glyph, bool on) {
    bool hot = g.stHot == id;
    SelectObject(dc, StGlyph());
    SetTextColor(dc, (hot || on) ? RGB(0x22, 0xbb, 0x56) : RGB(0xd6, 0xd6, 0xd6));
    RECT r = g_stR[id];
    DrawTextW(dc, glyph, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// KillerPDF centers a footer text's cap body on the bar (WPF VerticalAlignment=Center on
// BrandSansFont 16). GDI's DT_VCENTER centers the line CELL, and the rasterized ascent
// rounds up, so Killer Sans rides ~1px low on the 24px bar. Compute the DT_TOP origin
// that lands the baseline at barH/2 + capHeight/2 instead, from each face's true cap
// height (the bundled subsets: Killer Sans sCapHeight 649/1324 em, Killer Sans UI 876).
static const double StCapK = 649.0 / 1324.0;   // Killer Sans
static const double StCapU = 876.0 / 1324.0;   // Killer Sans UI

static int StCapTop(HDC dc, int barH, double emPx, double capRatio) {
    TEXTMETRICW tm{}; GetTextMetricsW(dc, &tm);
    return (int)std::lround(barH / 2.0 + capRatio * emPx / 2.0 - tm.tmAscent);
}

static void StText(HDC dc, RECT r, const wchar_t* s, UINT flags, HFONT f,
                   double emPx, double capRatio, COLORREF c) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    r.top += StCapTop(dc, r.bottom - r.top, emPx, capRatio);
    DrawTextW(dc, s, -1, &r, flags | DT_SINGLELINE | DT_TOP);
}

static void DrawVersion(HDC dc) {
    // The version cell: divider (1x14, Margin="5,0,8,0") | "v1.0" (Margin="0,0,12,0")
    // | "(c) 2026 Docsy" (Margin="0,0,20,0"). KillerPDF draws the cell at
    // Opacity 0.75 - TextFooter #858585 over the #1c1c1c footer bakes to #6b6b6b.
    int dpi = g.dpi;
    RECT vr = g_stR[ST_VER];
    RECT d{ vr.left + (int)(5 * dpi), (SBH() - (int)(14 * dpi)) / 2,
            vr.left + (int)(5 * dpi) + 1, (SBH() + (int)(14 * dpi)) / 2 };
    FillRect(dc, &d, DividerBrush());
    COLORREF vcol = g.stHot == ST_VER ? RGB(0x90, 0x90, 0x90) : RGB(0x6b, 0x6b, 0x6b);
    RECT t1 = vr; t1.left = d.right + (int)(8 * dpi); t1.right = t1.left + TextW(StFontL(), L"v" APP_VER);
    StText(dc, t1, L"v" APP_VER, DT_LEFT, StFontL(), 16 * g.dpi, StCapK, vcol);
    RECT t2 = vr; t2.right -= (int)(20 * dpi);
    t2.left = t2.right - TextW(StFontL(), L"\u00A9 2026 Docsy");
    StText(dc, t2, L"\u00A9 2026 Docsy", DT_LEFT, StFontL(), 16 * g.dpi, StCapK, vcol);
}

static HDC g_stbdc = nullptr; static HBITMAP g_stbm = nullptr, g_stbold = nullptr;
static int g_stbw = 0, g_stbh = 0;
static void EnsureStatusBack(HDC ref, int w, int h) {
    if (g_stbm && g_stbw == w && g_stbh == h) return;
    if (g_stbm) { SelectObject(g_stbdc, g_stbold); DeleteObject(g_stbm); }
    else g_stbdc = CreateCompatibleDC(ref);
    g_stbm = CreateCompatibleBitmap(ref, w, h);
    g_stbold = (HBITMAP)SelectObject(g_stbdc, g_stbm);
    g_stbw = w; g_stbh = h;
}

// ---- KillerPDF's PaneShadow, landing on the footer ----
// The document card is a lifted card and casts a soft black drop shadow DOWN over the
// footer (PaneShadowEffect: black, BlurRadius 16, ShadowDepth 5, Direction 270, Opacity
// 0.6; the footer sits at ZIndex -1 underneath it). Docsy's footer is a separate child
// window, so the shadow lands here: the same gaussian falloff over the bar's top rows -
// KillerPDF's own dark-theme render reads ~0.44 alpha on the first row, half by row 5 and
// gone by row 15 - spanning the card's width, wrapping its rounded bottom corners and
// fading out horizontally at the card's ends. Built once per size/dpi into an ARGB tile
// and AlphaBlended, so the footer's hover repaints stay cheap.
static HBITMAP g_shbm = nullptr; static HDC g_shdc = nullptr;
static int g_shw = 0, g_shh = 0, g_shL = -1; static double g_shdpi = 0;

static double Phi(double z) { return 0.5 * std::erfc(-z * 0.70710678118654752440); }

static void DrawPaneShadow(HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int L = (g.sideOn && g.n && !g.full) ? SW() : 0;   // the card's left edge in footer coords
    if (!g_shbm || g_shw != w || g_shh != h || g_shdpi != g.dpi || g_shL != L) {
        int rad = max(2, (int)(6 * g.dpi));
        int R = w - (int)(8 * g.dpi);                  // the card's right edge: the 8px window gutter
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        BYTE* px = nullptr;
        HBITMAP bm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
        if (px && bm) {
            const double sig = 8.0 * g.dpi, depth = 5.0 * g.dpi;
            for (int y = 0; y < h; y++) {
                double ay = 0.6 * Phi((depth - y) / sig);          // the card's shadow, 5px down
                double c = y < rad ? rad - std::sqrt((double)rad * rad - (double)(rad - y) * (rad - y)) : 0.0;
                double lEdge = L + c - 2.0, rEdge = R - c + 2.0;   // the corner pulls the ends in
                BYTE* row = px + (size_t)y * w * 4;
                for (int x = 0; x < w; x++) {
                    double a = ay * Phi((x - lEdge) / sig) * Phi((rEdge - x) / sig);
                    BYTE* p = row + (size_t)x * 4;
                    p[0] = p[1] = p[2] = 0;
                    p[3] = (BYTE)std::lround(a * 255.0);
                }
            }
            if (!g_shdc) g_shdc = CreateCompatibleDC(dc);
            if (g_shbm) DeleteObject(g_shbm);
            g_shbm = bm;
            SelectObject(g_shdc, g_shbm);
            g_shw = w; g_shh = h; g_shdpi = g.dpi; g_shL = L;
        }
    }
    if (g_shbm) {
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, 0, 0, w, h, g_shdc, 0, 0, w, h, bf);
    }
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
    std::wstring t = StatusTextNow();
    if (LatinText(t.c_str())) StText(dc, tr, t.c_str(), DT_LEFT | DT_END_ELLIPSIS,
                                     StFont(), 16 * g.dpi, StCapK, RGB(0xe0, 0xe0, 0xe0));
    else {                               // a held file name with script the subset drops:
        SelectObject(dc, StFontF());     // Segoe UI fallback, cell-centered as before
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        DrawTextW(dc, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    DrawVersion(dc);
    if (!g.n) return;                    // no document: status + version cells only
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
    DrawModeIcon(dc, ST_MCONT, g.vm == 0);
    DrawModeIcon(dc, ST_MSINGLE, g.vm == 1);
    DrawModeIcon(dc, ST_MTWO, g.vm == 2);
    DrawModeIcon(dc, ST_MGRID, g.vm == 3);
    DrawGlyph(dc, ST_FULL, L"\uE740", false);
    DrawGlyph(dc, ST_INV, L"\uE708", g.inv);
    DrawGlyph(dc, ST_ZOUT, L"\uECC9", false);
    DrawGlyph(dc, ST_ZIN, L"\uECC8", false);
    RECT cb = g_stR[ST_COMBO];           // the closed combo
    FillRect(dc, &cb, ComboFillBrush());
    FrameRect(dc, &cb, ComboBorderBrush());
    wchar_t zl[32];
    if (g.fitMode == 1) wcscpy_s(zl, L"Fit Page");
    else if (g.fitMode == 0) wcscpy_s(zl, L"Fit Width");
    else swprintf_s(zl, L"%d%%", (int)std::lround(g.zoom * 100));
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
    if (!g.status || !g.n) return;
    RECT cr; GetClientRect(g.status, &cr);
    StatusRects(cr.right);
    RECT cb = g_stR[ST_COMBO];
    POINT tl{ cb.left, cb.bottom }, br{ cb.right, cb.bottom };
    ClientToScreen(g.status, &tl); ClientToScreen(g.status, &br);
    g_zoomComboScr = { tl.x, tl.y, br.x, br.y };
    g_zoomItemH = (int)(22 * g.dpi);
    g_zoomPopW = max((int)(cb.right - cb.left), (int)(96 * g.dpi));
    int popH = 8 * g_zoomItemH + 2;
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
        FillRect(dc, &r, PopBrush());
        FrameRect(dc, &r, ComboBorderBrush());
        SelectObject(dc, StCombo());
        SetBkMode(dc, TRANSPARENT);
        int cur = ZoomCurrent();
        for (int i = 0; i < 8; i++) {
            RECT it{ 1, 1 + i * g_zoomItemH, r.right - 1, 1 + (i + 1) * g_zoomItemH };
            if (i == g.zoomHot) FillRect(dc, &it, PopHoverBrush());
            RECT tx = it; tx.left += (int)(8 * g.dpi);
            StText(dc, tx, ZoomLabel(i), DT_LEFT, StCombo(), 10 * g.dpi, StCapU,
                   i == cur ? RGB(0x22, 0xbb, 0x56) : RGB(0xe0, 0xe0, 0xe0));
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int it = (p.y - 1) / max(1, g_zoomItemH);
        if (it < 0 || it > 7 || p.x < 0 || p.x >= g_zoomPopW) it = -1;
        if (it != g.zoomHot) { g.zoomHot = it; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int it = (p.y - 1) / max(1, g_zoomItemH);
        if (p.x >= 0 && p.x < g_zoomPopW && it >= 0 && it <= 7) {
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
        if (g.zoomHot < 0) g.zoomHot = ZoomCurrent();
        g.zoomHot = std::clamp(g.zoomHot - (d > 0 ? 1 : -1), 0, 7);
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
        if (g.n) {
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

// ---------------- Find Text bar (KillerPDF's Shell/Search.cs) ----------------
// One floating child of the page view, KillerPDF's fallback spot: the bar's top
// edge 6px under the view's top, its right edge 16px in from the view's right.
// The visual is the original Border: CornerRadius 6, a 1px PaneBorderBrush
// #555555 frame, MenuBackgroundBrush #141414 + grain inside, and the DropShadow
// (black, blur 16, depth 3, down, opacity 0.55) - built as an ARGB tile and
// melted onto the view's own pixels: the bar's window copies the view's back
// buffer behind itself first, so the shadow lands on the live page content and
// the margins disappear into it. The row is KillerPDF's find row minus the
// replace gear (Windows.Data.Pdf cannot edit text): magnifier glyph E721, the
// query edit (26px tall in a radius-3 CardBorderBrush #404040 box on
// BackgroundBrush #1c1c1c, UiFont 13), the Aa / ab toggles (accent fill when
// on, SelectionFg text), the 96px counter cell (FontSize 11, MutedTextBrush,
// "12 / 73" or "No matches"), and ChevronUp / ChevronDown / Cancel 26x24 MDL2
// buttons with RowHoverBrush hovers and the danger red on close.
// Logic is SearchController's: 250ms debounce, a two-character minimum,
// Enter/Shift+Enter and F3/Shift+F3 step the cursor with wrap, Escape closes,
// a toggle re-runs at once, results start at the first match on or after the
// page you are on, and the scan pumps page by page on a timer so the window
// never blocks on a long book.

static int FdD(int v) { return (int)std::lround(v * g.dpi); }

static LRESULT CALLBACK FindProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK FindEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static void FindPlace();

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

// Metrics (dpi units): shadow margins around the visual bar, then the row.
// Bar width = the fixed cells + the edit; the edit shrinks (60..200) so the
// whole bar always fits the card, FitSearchBox-style.
static const int FdPadX = 24, FdPadT = 14, FdPadB = 28;   // window margins for the shadow
static const int FdBarH = 36, FdEditH = 26, FdBtnW = 26, FdBtnH = 24, FdStatW = 96;
// left border+pad 1+4 | icon 12+6 | Aa | ab | status 2+96+2 | prev | next | close | pad+border 4+1
static const int FdBarFix = 1 + 4 + 12 + 6 + FdBtnW * 2 + 2 + FdStatW + 2 + FdBtnW * 3 + 4 + 1;

enum { FB_AA, FB_AB, FB_PREV, FB_NEXT, FB_CLOSE, FB_COUNT };

static int FdEditW(int vw) {
    return std::clamp(vw - FdD(FdBarFix) - FdD(16) - 2 * FdD(FdPadX), FdD(60), FdD(200));
}

struct FdRects {
    RECT bar, icon, edit, status, btn[FB_COUNT];
};

static void FdLayout(int editW, FdRects& R) {
    int bw = FdD(FdBarFix) + editW;
    R.bar = { FdD(FdPadX), FdD(FdPadT), FdD(FdPadX) + bw, FdD(FdPadT) + FdD(FdBarH) };
    int top = R.bar.top + FdD(1) + FdD(4);                    // inside border + padding
    int x = R.bar.left + FdD(1) + FdD(4);
    R.icon = { x, top, x + FdD(12), top + FdD(FdEditH) };
    x += FdD(12) + FdD(6);
    R.edit = { x, top, x + editW, top + FdD(FdEditH) };
    x += editW;
    for (int i = 0; i < FB_COUNT; i++) {
        if (i == FB_PREV) {                                   // the status cell sits before prev
            R.status = { x + FdD(2), R.bar.top + 1, x + FdD(2) + FdD(FdStatW), R.bar.bottom - 1 };
            x += FdD(2) + FdD(FdStatW) + FdD(2);
        }
        R.btn[i] = { x, top + FdD(1), x + FdD(FdBtnW), top + FdD(1) + FdD(FdBtnH) };
        x += FdD(FdBtnW);
    }
}

static int FdHit(POINT p) {
    FdRects R; FdLayout(FdEditW(CardW()), R);
    for (int i = 0; i < FB_COUNT; i++)
        if (p.x >= R.btn[i].left && p.x < R.btn[i].right && p.y >= R.btn[i].top && p.y < R.btn[i].bottom)
            return i;
    return -1;
}

static void FindStatusRepaint() { if (g_find && fd.open) InvalidateRect(g_find, nullptr, FALSE); }

// Lands the cursor match: the counter, the repaint, and - when stepping - the
// scroll onto the match (Single mode flips pages first, the other modes scroll
// the match's line to the view's middle, horizontally centring the page too).
static void FindGoto(int idx, bool jump) {
    if (idx < 0 || idx >= (int)fd.hits.size()) return;
    fd.cur = idx;
    const FHit& hit = fd.hits[idx];
    if (jump) {
        if (g.vm == 1 && hit.page != g.cur) SetPage(hit.page);
        int row = std::clamp(hit.page / PerRow(), 0, max(0, (int)g.rh.size() - 1));
        int yDoc = PageTop(hit.page) + (g.rh[row] - PH(hit.page)) / 2
                 + (int)((hit.r.y + hit.r.h * 0.5f) * g.zoom * g.dpi);
        int xDoc = g.sx;
        if (g.docW > CardW() && hit.page < (int)g.px.size())
            xDoc = g.px[hit.page] + (int)((hit.r.x + hit.r.w * 0.5f) * g.zoom * g.dpi) - CardW() / 2;
        ScrollTo(xDoc, yDoc - g.ch / 2);
    }
    InvalidateRect(g.view, nullptr, FALSE);
    FindStatusRepaint();
}

// The scan just completed: KillerPDF's start rule - sit on the first match at or
// after the page you are on, else wrap to the first one - and the final counter.
static void FindFinish() {
    if (fd.hits.empty()) { fd.cur = -1; FindStatusRepaint(); return; }
    int start = max(0, g.cur);
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
        if (g_find) KillTimer(g_find, 1);
        fd.gen++; fd.scan = 0; fd.hits.clear(); fd.cur = -1; fd.done = false;
        if (g_find) SetTimer(g_find, 2, 30, nullptr);
    }
}

static void FindRun() {
    if (fd.query.size() < 2 || fd.query.find_first_not_of(L" \t\n\r") == std::wstring::npos) return;
    fd.gen++; fd.scan = 0; fd.hits.clear(); fd.cur = -1; fd.done = false;
    if (g_find) SetTimer(g_find, 2, 30, nullptr);
}

// The pump: scan pages until a small time budget is spent, then hand control
// back - a fresh OCR page costs tens of milliseconds, so the budget only trims
// the cheap pages; the heavy ones land one per tick and the window keeps its
// pulse. Every edit bumps gen, and a stale pump (this gen is no longer the
// state's) drops everything it has gathered.
static void FindPump() {
    if (!g_find || fd.done) { if (g_find) KillTimer(g_find, 2); return; }
    unsigned my = fd.gen;
    unsigned long long t0 = GetTickCount64();
    std::vector<FRect> rs;
    int before = (int)fd.hits.size();
    while (fd.scan < g.n && GetTickCount64() - t0 < 12) {
        TxtPage* pt = TtFor(fd.scan);
        if (pt && !pt->cells.empty() && FdMatch(*pt, fd.query, fd.matchCase, fd.wholeWord, rs) > 0)
            for (const FRect& r : rs) fd.hits.push_back({ fd.scan, r });
        fd.scan++;
    }
    if (fd.gen != my) return;                     // cancelled mid-tick: drop it all
    if ((int)fd.hits.size() != before) InvalidateRect(g.view, nullptr, FALSE);   // progressive
    if (fd.scan >= g.n) {
        fd.done = true;
        KillTimer(g_find, 2);
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
    if (g_find) { KillTimer(g_find, 1); KillTimer(g_find, 2); }
    if (q.size() < 2) {
        InvalidateRect(g.view, nullptr, FALSE);
        FindStatusRepaint();
        return;
    }
    if (g_find) SetTimer(g_find, 1, 250, nullptr);
}

static void FindOpen(bool create = true) {
    fd.open = true;
    if (create && g.view) {
        if (!g_find) {
            WNDCLASSEXW fc{ sizeof fc };
            fc.lpfnWndProc = FindProc; fc.hInstance = g.hinst;
            fc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            fc.lpszClassName = L"DocsyFind";
            RegisterClassExW(&fc);
            g_find = CreateWindowExW(0, L"DocsyFind", nullptr, WS_CHILD | WS_CLIPCHILDREN,
                                     0, 0, 0, 0, g.view, nullptr, g.hinst, nullptr);
            if (!g_find) { fd.open = false; return; }
            g_fedit = CreateWindowExW(0, L"EDIT", nullptr,
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 0, 0, g_find, nullptr, g.hinst, nullptr);
            SendMessageW(g_fedit, WM_SETFONT, (WPARAM)FdFontEdit(), TRUE);
            SendMessageW(g_fedit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(FdD(6), FdD(6)));
            g_feditDef = (WNDPROC)SetWindowLongPtrW(g_fedit, GWLP_WNDPROC, (LONG_PTR)FindEditProc);
        }
        FindPlace();
        ShowWindow(g_find, SW_SHOWNOACTIVATE);
    }
    fd.query.clear(); fd.hits.clear(); fd.cur = -1; fd.scan = 0; fd.gen++; fd.done = true; fd.hot = -1;
    if (g_fedit) { SetWindowTextW(g_fedit, L""); SetFocus(g_fedit); }   // open empty, focus ready
}

static void FindClose() {
    if (!fd.open) return;
    fd.open = false;
    fd.gen++; fd.done = true; fd.scan = 0;
    fd.hits.clear(); fd.cur = -1; fd.query.clear(); fd.hot = -1;
    if (g_find) { KillTimer(g_find, 1); KillTimer(g_find, 2); ShowWindow(g_find, SW_HIDE); }
    if (g.main) SetFocus(g.main);                 // the keys go home (they live on the main window)
    InvalidateRect(g.view, nullptr, FALSE);       // the highlights leave with the bar
}

static void FindToggle() { fd.open ? FindClose() : FindOpen(); }

// The bar's paint, shared with the CI shot: (ox,oy) is the window's client
// origin in the destination DC - (0,0) on the window itself, the window's view
// offset in the shot. Assumes the view's pixels are already behind it.
static void FindDraw(HDC dc, int ox, int oy) {
    int editW = FdEditW(CardW());
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
        else { Gdiplus::SolidBrush mb(Gdiplus::Color(255, 0x14, 0x14, 0x14)); gs.FillPath(&mb, inner); }
        Gdiplus::GraphicsPath band;
        band.AddPath(outer, FALSE);
        band.AddPath(inner, FALSE);
        band.SetFillMode(Gdiplus::FillModeAlternate);
        Gdiplus::SolidBrush edge(Gdiplus::Color(255, 0x55, 0x55, 0x55));
        gs.FillPath(&edge, &band);
        delete outer;
        delete inner;
    }
    g_grainPX = spx; g_grainPY = spy;

    FdRects R; FdLayout(editW, R);
    SetBkMode(dc, TRANSPARENT);
    // the query edit: a radius-3 box on BackgroundBrush #1c1c1c with the
    // CardBorderBrush #404040 ring; the real edit control covers the interior,
    // and with no control (the CI shot) the query text is drawn in its place
    {
        int x = R.edit.left - 1, y = R.edit.top - 1;
        int ew = R.edit.right - R.edit.left + 2, eh = R.edit.bottom - R.edit.top + 2;
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath* outer = CardPath((float)x, (float)y, (float)ew, (float)eh, (float)FdD(3), 0);
        Gdiplus::GraphicsPath* inner = CardPath((float)x, (float)y, (float)ew, (float)eh, (float)FdD(3), 1);
        Gdiplus::SolidBrush bg(Gdiplus::Color(255, 0x1c, 0x1c, 0x1c));
        gs.FillPath(&bg, inner);
        Gdiplus::GraphicsPath band;
        band.AddPath(outer, FALSE);
        band.AddPath(inner, FALSE);
        band.SetFillMode(Gdiplus::FillModeAlternate);
        Gdiplus::SolidBrush ring(Gdiplus::Color(255, 0x40, 0x40, 0x40));
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
    // the magnifier glyph
    SelectObject(dc, FdFontGlyph());
    SetTextColor(dc, RGB(0x8a, 0x8a, 0x8a));
    DrawTextW(dc, L"\uE721", -1, &R.icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // the option toggles: accent fill + dark text when on, RowHover on hover
    for (int b = 0; b <= 1; b++) {
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
            Gdiplus::SolidBrush f(on ? Gdiplus::Color(255, 0x22, 0xbb, 0x56)
                                     : Gdiplus::Color(255, 0x40, 0x40, 0x40));
            gs.FillPath(&f, &p);
        }
        RECT r = R.btn[b];
        SelectObject(dc, b == FB_AA ? FdFontOpt() : FdFontAb());
        SetTextColor(dc, on ? RGB(0x0d, 0x0d, 0x0d) : RGB(0xc8, 0xc8, 0xc8));
        DrawTextW(dc, b == FB_AA ? L"Aa" : L"ab", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // the counter cell: KillerPDF's compact "12 / 73" (current / total), or
    // "No matches" once the scan came back empty
    std::wstring st;
    if (!fd.query.empty() && fd.query.size() >= 2) {
        if (fd.done) st = fd.hits.empty() ? L"No matches"
                                          : std::to_wstring(fd.cur + 1) + L" / " + std::to_wstring(fd.hits.size());
    }
    if (!st.empty()) {
        RECT r = R.status;
        SelectObject(dc, FdFontStat());
        SetTextColor(dc, RGB(0x8a, 0x8a, 0x8a));
        DrawTextW(dc, st.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // the nav buttons: ChevronUp / ChevronDown / Cancel, red close hover
    const wchar_t* glyph[3] = { L"\uE70E", L"\uE70D", L"\uE711" };
    for (int b = FB_PREV; b <= FB_CLOSE; b++) {
        bool hot = fd.hot == b;
        if (hot) {
            RECT r = R.btn[b];
            if (b == FB_CLOSE) {
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
                Gdiplus::SolidBrush f(Gdiplus::Color(255, 0xE0, 0x44, 0x44));
                gs.FillPath(&f, &p);
            } else {
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
                Gdiplus::SolidBrush f(Gdiplus::Color(255, 0x40, 0x40, 0x40));
                gs.FillPath(&f, &p);
            }
        }
        SelectObject(dc, FdFontGlyph());
        SetTextColor(dc, hot ? RGB(255, 255, 255) : RGB(0xc8, 0xc8, 0xc8));
        DrawTextW(dc, glyph[b - FB_PREV], -1, &R.btn[b], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// The bar's back buffer (one blit: a direct paint showed every intermediate
// step - backdrop copy, shadow, panel - which is exactly the strobing the
// caption buttons once did under the cursor).
static HDC g_fdc = nullptr; static HBITMAP g_fbm = nullptr, g_fold = nullptr;
static int g_fw = 0, g_fh = 0;
static void EnsureFindBack(HDC ref, int w, int h) {
    if (g_fbm && g_fw == w && g_fh == h) return;
    if (g_fbm) { SelectObject(g_fdc, g_fold); DeleteObject(g_fbm); }
    else g_fdc = CreateCompatibleDC(ref);
    g_fbm = CreateCompatibleBitmap(ref, w, h);
    g_fold = (HBITMAP)SelectObject(g_fdc, g_fbm);
    g_fw = w; g_fh = h;
}

static LRESULT CALLBACK FindProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureFindBack(dc, r.right, r.bottom);
            // the view's live pixels behind us: the shadow melts onto the pages,
            // the margins disappear into the view - the bar floats, WPF-style
            POINT po{ 0, 0 }; MapWindowPoints(h, g.view, &po, 1);
            RECT vr{ po.x, po.y, po.x + r.right, po.y + r.bottom };
            FillRect(g_fdc, &r, GrainBrush(1));
            RECT ix{ max(0, (int)vr.left), max(0, (int)vr.top),
                     min(g.bw, (int)vr.right), min(g.bh, (int)vr.bottom) };
            if (g.bdc && ix.right > ix.left && ix.bottom > ix.top)
                BitBlt(g_fdc, ix.left - vr.left, ix.top - vr.top, ix.right - ix.left, ix.bottom - ix.top,
                       g.bdc, ix.left, ix.top, SRCCOPY);
            POINT pm{ 0, 0 }; MapWindowPoints(h, g.main, &pm, 1);   // the shared grain phase
            SetBrushOrgEx(g_fdc, -pm.x, -pm.y, nullptr);
            FindDraw(g_fdc, 0, 0);
            BitBlt(dc, 0, 0, r.right, r.bottom, g_fdc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) { KillTimer(h, 1); FindRun(); return 0; }    // the debounce fired
        if (wp == 2) { FindPump(); return 0; }
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g_fedit) FindChanged();
        return 0;
    case WM_CTLCOLOREDIT: {                            // the dark edit: light text on chrome
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        SetBkColor(dc, RGB(0x1c, 0x1c, 0x1c));
        static HBRUSH b = CreateSolidBrush(RGB(0x1c, 0x1c, 0x1c));
        return (LRESULT)b;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int ht = FdHit(p);
        if (ht != fd.hot) {                            // repaints only on a hover flip
            fd.hot = ht;
            if (ht >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (fd.hot != -1) { fd.hot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        int ht = FdHit(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        if (ht == FB_CLOSE) FindClose();
        else if (ht == FB_PREV) FindStep(-1);
        else if (ht == FB_NEXT) FindStep(1);
        else if (ht == FB_AA) FindOption(true);
        else if (ht == FB_AB) FindOption(false);
        return 0;
    }
    case WM_MOUSEWHEEL: ViewWheel(wp, lp); return 0;   // the bar floats; the wheel still scrolls
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {                  // the bar is buttons and a text box: arrow
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// The edit's keys: Enter steps, Shift+Enter steps back, Escape closes, Ctrl+F
// closes (the toggle), everything else edits - KillerPDF's SearchBox_KeyDown.
static LRESULT CALLBACK FindEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        if (wp == VK_RETURN) { FindStep(shift ? -1 : 1); return 0; }
        if (wp == VK_ESCAPE) { FindClose(); return 0; }
        if (wp == VK_F3) { FindStep(shift ? -1 : 1); return 0; }   // F3 steps from the box too
        if (wp == 'F' && ctrl) { FindClose(); return 0; }
    }
    return CallWindowProcW(g_feditDef, h, m, wp, lp);
}

// Size and spot the bar: the edit shrinks to fit (60..200), the bar's right
// edge rides 16px in from the view's right, top 6px under the view's top.
static void FindPlace() {
    if (!g_find || !g_fedit || !g.view) return;
    int vw = CardW();
    if (vw <= 0) return;
    int editW = FdEditW(vw);
    int bw = FdD(FdBarFix) + editW, bh = FdD(FdBarH);
    MoveWindow(g_find, vw - FdD(16) + FdD(FdPadX) - bw, FdD(6) - FdD(FdPadT),
               bw + 2 * FdD(FdPadX), bh + FdD(FdPadT) + FdD(FdPadB), TRUE);
    FdRects R; FdLayout(editW, R);
    MoveWindow(g_fedit, R.edit.left, R.edit.top,
               R.edit.right - R.edit.left, R.edit.bottom - R.edit.top, FALSE);
}

// ---------------- main window ----------------

static void LayoutMain() {
    if (!g.main || !g.view) return;
    RECT r; GetClientRect(g.main, &r);
    int w = r.right, h = r.bottom;
    int sw = (g.sideOn && g.n && !g.full) ? SW() : 0;
    int cap = CAP();
    int sh = g.full ? 0 : SBH();                // full screen hides the footer too
    int ch = max(0, h - cap - sh);              // the children sit below the caption row
    sd.w = sw;
    if (g.side) {
        MoveWindow(g.side, 0, cap, sw, ch, TRUE);
        if (sw) {
            RECT sr; GetClientRect(g.side, &sr);
            if (sr.right != sd.cw || sr.bottom != sd.ch) { sd.cw = sr.right; sd.ch = sr.bottom; SideLayout(); }
        }
    }
    MoveWindow(g.view, sw, cap, w - sw, ch, TRUE);
    if (g.status) MoveWindow(g.status, 0, cap + ch, w, sh, TRUE);
    if (g_find && fd.open) FindPlace();          // the bar tracks the card's top-right corner
    RECT vr; GetClientRect(g.view, &vr);
    g.cw = vr.right; g.ch = vr.bottom;
    g.sx = std::clamp(g.sx, 0, max(0, g.docW - CardW()));
    g.sy = std::clamp(g.sy, 0, max(0, g.docH - g.ch));
    if (!g.resizing && !g.busy && g.cw > 0 && g.ch > 0) {
        if (g.fitMode != 2) ApplyFit(g.fitMode == 1); else Layout();   // fit modes re-run on resize
        UpdateTitle();
    }
}

static void ToggleSide() {
    if (!g.n) return;
    g.sideOn = !g.sideOn;
    ShowWindow(g.side, g.sideOn ? SW_SHOWNOACTIVATE : SW_HIDE);
    LayoutMain();
}

static void ZoomBy(double f) { SetZoom(g.zoom * f, g.cw / 2, g.ch / 2); }

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
        g.status = CreateWindowExW(0, L"DocsyStatus", nullptr,
                                   WS_CHILD | WS_VISIBLE,
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
        ClearCache();
        if (g_uifont) { DeleteObject(g_uifont); g_uifont = nullptr; }
        if (g_sidefont) { DeleteObject(g_sidefont); g_sidefont = nullptr; }
        if (g_glyphfont) { DeleteObject(g_glyphfont); g_glyphfont = nullptr; }
        if (g_wordfont) { DeleteObject(g_wordfont); g_wordfont = nullptr; }
        if (g_stfont) { DeleteObject(g_stfont); g_stfont = nullptr; }
        if (g_stfontL) { DeleteObject(g_stfontL); g_stfontL = nullptr; }
        if (g_stfontF) { DeleteObject(g_stfontF); g_stfontF = nullptr; }
        if (g_stcombo) { DeleteObject(g_stcombo); g_stcombo = nullptr; }
        if (g_stglyph) { DeleteObject(g_stglyph); g_stglyph = nullptr; }
        if (g_stchev) { DeleteObject(g_stchev); g_stchev = nullptr; }
        if (g_fntEdit) { DeleteObject(g_fntEdit); g_fntEdit = nullptr; }
        if (g_fntOpt) { DeleteObject(g_fntOpt); g_fntOpt = nullptr; }
        if (g_fntAb) { DeleteObject(g_fntAb); g_fntAb = nullptr; }
        if (g_fntStat) { DeleteObject(g_fntStat); g_fntStat = nullptr; }
        if (g_fntGlyph) { DeleteObject(g_fntGlyph); g_fntGlyph = nullptr; }
        if (g_fedit) SendMessageW(g_fedit, WM_SETFONT, (WPARAM)FdFontEdit(), TRUE);
        for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
        sd.th.clear(); sd.tw = 0;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutMain();
        return 0;
    }
    case WM_GETMINMAXINFO: {   // the footer cluster needs its width; the rail needs some height
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = (LONG)(780 * g.dpi);
        mmi->ptMinTrackSize.y = (LONG)(380 * g.dpi);
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
        case VK_NEXT:
            if (g.vm == 1) SetPage(g.cur + 1);   // single mode: the flip keys
            else ScrollTo(g.sx, g.sy + pg);
            break;
        case VK_PRIOR:
            if (g.vm == 1) SetPage(g.cur - 1);
            else ScrollTo(g.sx, g.sy - pg);
            break;
        case VK_SPACE:
            if (g.vm == 1) SetPage(g.cur + (shift ? -1 : 1));
            else ScrollTo(g.sx, g.sy + (shift ? -pg : pg));
            break;
        case VK_HOME: ScrollTo(g.sx, 0); break;
        case VK_END: ScrollTo(g.sx, INT_MAX / 2); break;
        case VK_ADD: case VK_OEM_PLUS: ZoomBy(1.25); break;
        case VK_SUBTRACT: case VK_OEM_MINUS: ZoomBy(0.8); break;
        case '0': if (ctrl) ApplyFit(); break;
        case '1': if (ctrl) SetZoom(1.0, g.cw / 2, g.ch / 2); break;
        case '2': if (ctrl) ApplyFit(true); break;
        case 'F': if (ctrl) FindToggle(); else ApplyFit(); break;   // Ctrl+F find, plain F fit
        case 'C': if (ctrl && !g.selText.empty()) CopyClipboard(g.selText); break;
        case 'A': if (ctrl) SelAll(); break;
        case VK_F3:                                // Find next / previous; closed, F3 opens
            if (!fd.open) FindOpen();
            else if (!fd.hits.empty()) FindStep(shift ? -1 : 1);
            break;
        case VK_ESCAPE:
            if (fd.open) FindClose();              // the bar leaves first
            else if (g.full) ToggleFull(h);        // full screen leaves next
            else SelClear();
            break;
        case VK_F5: SetViewMode(0); break;       // KillerPDF's mode keys
        case VK_F6: SetViewMode(1); break;
        case VK_F7: SetViewMode(2); break;
        case VK_F8: SetViewMode(3); break;
        case VK_F11: ToggleFull(h); break;
        case VK_F9: ToggleSide(); break;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 3) { InvalidateCap(h); return 0; }   // hovered-button keepalive repaint
        break;                                 // (buffered, so an unchanged band reblits
                                               // identically and cannot strobe)
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
    int sw = (g.sideOn && g.n && !g.full) ? sd.w : 0;
    int cap = CAP(), sh = SBH();
    int ch = max(1, h - cap - sh);
    // the children render above the caption row and below it, so composite them shifted
    if (sw) {
        HDC m1 = CreateCompatibleDC(dc);
        HBITMAP b1 = CreateCompatibleBitmap(dc, sw, ch);
        HGDIOBJ o1 = SelectObject(m1, b1);
        SetBrushOrgEx(m1, 0, -cap, nullptr);            // the pane's grain phase, as on screen
        SideDraw(m1, sw, ch, 0);
        BitBlt(dc, 0, cap, sw, ch, m1, 0, 0, SRCCOPY);
        SelectObject(m1, o1); DeleteObject(b1); DeleteDC(m1);
    }
    {
        HDC m2 = CreateCompatibleDC(dc);
        HBITMAP b2 = CreateCompatibleBitmap(dc, max(1, w - sw), ch);
        HGDIOBJ o2 = SelectObject(m2, b2);
        SetBrushOrgEx(m2, -sw, -cap, nullptr);
        g_grainPX = -sw; g_grainPY = -cap;              // the view's grain phase, as on screen
        Draw(m2, w - sw, ch, 0);
        PaintViewBars(m2, w - sw, ch, 0);
        if (fd.open) {                                  // the floating Find bar, over its pixels
            int editW = FdEditW(w - sw);
            int bw = FdD(FdBarFix) + editW;
            FindDraw(m2, (w - sw) - FdD(16) + FdD(FdPadX) - bw, FdD(6) - FdD(FdPadT));
        }
        BitBlt(dc, sw, cap, w - sw, ch, m2, 0, 0, SRCCOPY);
        SelectObject(m2, o2); DeleteObject(b2); DeleteDC(m2);
    }
    if (!g.full && sh > 0) {                   // the footer strip
        HDC m3 = CreateCompatibleDC(dc);
        HBITMAP b3 = CreateCompatibleBitmap(dc, w, sh);
        HGDIOBJ o3 = SelectObject(m3, b3);
        SetBrushOrgEx(m3, 0, -(cap + ch), nullptr);
        StatusDraw(m3, w, sh);
        BitBlt(dc, 0, cap + ch, w, sh, m3, 0, 0, SRCCOPY);
        SelectObject(m3, o3); DeleteObject(b3); DeleteDC(m3);
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

    // The bundled Killer Sans faces (KillerPDF's BrandSansFont / UiFont, OFL,
    // subset to latin-1 + dashes in src/fonts). Loading them from memory keeps
    // them process-private: no install, no file next to the exe.
    for (int id : { 201, 202 }) {
        HRSRC hs = FindResourceW(hi, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
        HGLOBAL hg = hs ? LoadResource(hi, hs) : nullptr;
        const void* data = hg ? LockResource(hg) : nullptr;
        DWORD len = hs ? SizeofResource(hi, hs) : 0;
        if (data && len) {
            DWORD n = 0;
            AddFontMemResourceEx(const_cast<void*>(data), len, nullptr, &n);
        }
    }

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

    WNDCLASSEXW tc{ sizeof tc };                    // the footer status bar
    tc.lpfnWndProc = StatusProc; tc.hInstance = hi;
    tc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tc.lpszClassName = L"DocsyStatus";
    RegisterClassExW(&tc);

    WNDCLASSEXW zc{ sizeof zc };                    // the zoom dropdown popup
    zc.style = CS_SAVEBITS | CS_DROPSHADOW;         // menu-like: saved bits + a soft shadow
    zc.lpfnWndProc = ZoomPopProc; zc.hInstance = hi;
    zc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    zc.lpszClassName = L"DocsyZoomPop";
    RegisterClassExW(&zc);

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
        // CI preview seed: a committed selection over one page's per-character carets
        // (sel=page:c0:c1), so the release screenshot shows the selection quads
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
        // CI preview seed 2: find=<query> opens the Find bar with the query and
        // scans synchronously (no windows here - the bar paints through Shot), so
        // the release screenshot shows the bar, the counter and the highlights
        if (argc >= 6 && !wcsncmp(argv[5], L"find=", 5)) {
            FindOpen(false);
            fd.query = argv[5] + 5;
            if (fd.query.size() >= 2) {
                fd.done = false;
                std::vector<FRect> rs;
                while (fd.scan < g.n) {
                    TxtPage* pt = TtFor(fd.scan);
                    if (pt && !pt->cells.empty() &&
                        FdMatch(*pt, fd.query, fd.matchCase, fd.wholeWord, rs) > 0)
                        for (const FRect& r : rs) fd.hits.push_back({ fd.scan, r });
                    fd.scan++;
                }
                fd.done = true;
                FindFinish();
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
