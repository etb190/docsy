// Docsy - minimal, fast, native Win32 PDF reader.
// Uses the PDF engine built into Windows (Windows.Data.Pdf): no bundled renderer.
// The main window hosts two children: the page view (right) and the thumbnail
// sidebar (left; drag the splitter between them to resize it). The sidebar is
// fully virtualized: only thumbnails actually in view are rendered (a few per
// timer tick) and off-screen ones are evicted, so a 2,000-page book scrolls as
// light as a 10-page one. No stock scrollbars anywhere: scrolling is wheel and
// keys, and the only bar on screen is a slim KillerPDF-style overlay thumb (4px
// wide, 8px under the cursor) owning an invisible 12px gutter. A drag on the
// rail only cancels a click - like the page view, it never pans (no grab-to-scroll).
// Text selection is KillerPDF's flowing model too: a press on text drags a caret
// pair of individual characters along the page's reading order (column-aware,
// line-banded cells: the built-in OCR pass over the page supplies word boxes, each
// sliced into per-character cells the way KillerPDF's engine supplies letters) and
// paints one 60/255 accent quad per selected line; select or double-click copies
// at once. The page never pans under the mouse - the wheel and the keys own
// scrolling, so a grab always means text selection, never a scroll.
// Find Text is KillerPDF's search bar too (Shell/Search.cs + SearchController +
// SearchService): Ctrl+F - or the toolbar row's Find Text button - floats one
// rounded grain bar over the page area (top right, KillerPDF's fallback spot)
// with KillerPDF's DropShadow melted onto the live view pixels. The bar is not
// a window: it is composited INTO the view's back buffer every paint (pages,
// highlights, overlay bars, the bar and its shadow land in one buffer and one
// blit), so a drag or a repaint is one atomic frame - WPF's single surface.
// The only real window is the query EDIT, a child of the view parked inside its
// ring. The row is the drag grip (two
// columns of three dots; grabbing it moves the bar anywhere on the card), the
// magnifier glyph (E721), the query edit, the Aa / ab option toggles
// (SelectionBg gradient + white when on), the "12 / 73" counter cell and
// ChevronUp / ChevronDown / Cancel buttons with the danger red close glyph.
// 250ms debounce, a two-character minimum, Enter / Shift+Enter / F3 / Shift+F3
// step, Escape closes. The scan walks the document page by page on a pump timer
// (the OCR text layer is built page-wise, so a 2,000-page book streams its
// matches instead of blocking), matches fold case unless Match Case, Whole Word
// rejects letter/digit/underscore neighbours, and every match paints one rounded
// box per touched word - dim orange for the field, bright with the accent ring
// for the cursor match - and navigation lands on the first match at or after
// the page you are on.
// The frame is KillerPDF's too: the real OS side borders stay, but the window
// is created without WS_CAPTION (and WM_NCCALCSIZE gives the top row to the
// client), so the native caption can never appear - not even on the first
// paint. A custom 32px title bar (grain, wordmark, the find tool button and
// 44x32 min/max/close buttons - the find one KillerPDF's ToolbarButton hover,
// the others the green/red hovers, the Win11 snap flyout on the maximize
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

// the info dialog's edit margins - the SDK's own value, guarded because some
// header sets stop short of the edit-margin block
#ifndef EM_SETMARGIN
#define EM_SETMARGIN 0x00D3
#endif
#ifndef EC_LEFTMARGIN
#define EC_LEFTMARGIN 0x0001
#endif
#ifndef EC_RIGHTMARGIN
#define EC_RIGHTMARGIN 0x0002
#endif

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

// ---- annotation model (KillerPDF's page annotations, the three primitives ----
// the six tool buttons commit). Coordinates are PAGE DIPs (PDF points) so an
// annotation rides its page through zoom and scroll exactly like the selection
// quads do. KillerPDF reduces every editing tool to these: the Highlight tool
// commits one HighlightAnnotation per selected line (CommitFlowingHighlight),
// and Draw / Line / Shapes all commit InkAnnotation strokes - a straight line
// is a 2-point stroke, a rectangle a closed 4-corner stroke, an ellipse a
// closed 64-segment stroke (Shell/Shapes.cs CommitShapeDrag) - and the Text
// tool commits a TextAnnotation (24pt, Segoe UI, 165pt box). Session state:
// KillerPDF writes annotations into the saved PDF; docsy's engine is the
// read-only Windows renderer, so they live with the open document.
enum { AT_HIGHLIGHT, AT_INK, AT_TEXT };
struct Annot {
    int type = AT_INK;
    float x = 0, y = 0, w = 0, h = 0;      // text: box pos+width (DIPs); highlight: line-quad union
    std::vector<FRect> lines;              // highlight: one quad per selected line
    std::vector<float> ix, iy;             // ink: the polyline (DIPs)
    float width = 3;                       // ink stroke width (DIPs) - KillerPDF's _drawWidth 3
    float fsize = 24;                      // text: KillerPDF's _textFontSize 24
    COLORREF color = RGB(0xff, 0x00, 0x00);
    BYTE alpha = 255;
    std::wstring text;                     // AT_TEXT content
};

// Everything one open document owns: its engine handle, its page geometry, its
// layout at the pane's own card width, its zoom/scroll/view-mode state, its
// rendered-page cache, its OCR text geometry and its flowing selection. The
// split carries TWO of these - pane A's in App::d, pane B's in App::dB - so a
// pane is a real document surface (KillerPDF's PdfViewer per pane), not a
// viewport carved out of a shared flow. Every helper below either takes an
// explicit DocState& (the render path, which paints both panes in one frame)
// or acts on the focused pane through DVF() (the interaction path, which
// always names the pane the user is working in).
struct DocState {
    PdfDocument doc{ nullptr };
    std::wstring name;
    std::wstring path;                  // full path of the open file (the status click's size flash)
    int n = 0;
    std::vector<float> pw, ph;          // page size in DIPs (PDF points)
    std::vector<int> top;               // pixel top of each ROW (a row holds 1-3 pages)
    std::vector<int> px;                // pixel x of each page inside its row
    std::vector<int> rh;                // pixel height of each row (the tallest page in it)
    double zoom = 1.0;                  // zoom: pixels per DIP (x dpi scale)
    int vm = 0;                         // KillerPDF's view modes: 0 continuous, 1 single, 2 two-page, 3 grid
    int fitMode = 2;                    // KillerPDF's FitMode: 0 FitWidth, 1 FitPage, 2 none - a
                                        // document opens at 100% zoom, not fit-width
    int sx = 0, sy = 0;                 // this pane's scroll position
    int docW = 0, docH = 0, gap = 10, maxw = 0, lastVis = 0;
    int cur = -1;                       // current page (view centre), drives the sidebar highlight
    unsigned long long scrollTick = 0;  // last scroll stamp: defers the text pre-warm until settling
    std::unordered_map<int, Ent> cache;
    unsigned long long tick = 0;
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
    // this pane's overlay bars: KillerPDF's document pane carries its own pair,
    // so pane A's classic fields and pane B's old "2" twins both live here now
    bool vHot = false, vDrag = false, hHot = false, hDrag = false;   // overlay bar states
    int vGrab = 0, hGrab = 0;
    // the pane's annotations and its own current tool - KillerPDF keeps the
    // editing state per viewer instance (PdfViewer.Bridge2: "Annotations, ...
    // and the current tool remain independent between panes"), so each pane
    // draws with the tool the reader left IT on
    std::unordered_map<int, std::vector<Annot>> annos;
    int tool = 0;                       // TOOL_SELECT .. TOOL_DRAW (the rail's top stack)
};
enum { TOOL_SELECT, TOOL_TEXT, TOOL_HIGHLIGHT, TOOL_LINE, TOOL_SHAPE, TOOL_DRAW, TOOL_COUNT };

struct App {
    HWND main = nullptr, view = nullptr, side = nullptr, status = nullptr;
    HWND bar = nullptr, zoomPop = nullptr;      // bar: KillerPDF's toolbar row (the Find Text button)
    HINSTANCE hinst = nullptr;
    HICON logo = nullptr;
    DocState d;                         // pane A's document (the classic state, every pre-split path)
    DocState dB;                        // pane B's document - a second real document, KillerPDF's
                                        // split pane: it starts EMPTY (its own start screen) and
                                        // loads what the user opens or drops onto it
    double dpi = 1.0;                   // pixels per DIP of the OS scale factor
    bool inv = false;                   // DocInvert (the footer moon): page bitmaps blitted inverted
    bool full = false;                  // F11 full screen: chrome gone, window over the monitor
    WINDOWPLACEMENT fsPlace{};          // pre-fullscreen placement
    LONG fsStyle = 0;
    std::wstring hint;                  // transient on-page hint (full screen entry)
    unsigned long long hintUntil = 0;
    bool quiet = false, busy = false, sideOn = false;
    int cw = 0, ch = 0;                 // the view's client size (both panes share the height)
    int sideW = 0;                      // user-sized sidebar width (0 = KillerPDF's 180px default)
    bool resizing = false;              // splitter drag: defer refits and thumb rebuilds
    int rsX = 0, rsW0 = 0;
    int osFrame = 0;                    // OS side-border width from WM_NCCALCSIZE (the top resize grip)
    int capHot = -1;                    // hovered caption button: 0 min, 1 max, 2 close, -1 none
    bool capPressed = false;            // the maximize button held down (non-client, SnapLayouts-style)
    int tbHot = -1;                     // hovered toolbar button: 0 the Find Text button, -1 none
    bool tbPressed = false;             // the Find Text button held down (capture-held, WPF's IsPressed)
    HDC bdc = nullptr; HBITMAP bbm = nullptr, bold = nullptr; int bw = 0, bh = 0;
    HDC capdc = nullptr; HBITMAP capbm = nullptr, capold = nullptr; int capw = 0, caph = 0;
    // the footer (KillerPDF's status bar): a held message briefly wins over the
    // routine page/zoom line, exactly like SetStatusHeld in MainWindow.xaml.cs
    std::wstring statusHoldText;
    unsigned long long statusHoldUntil = 0;
    int stHot = -1;                     // hovered footer control, -1 none
    bool zoomOpen = false;              // the footer zoom dropdown is on screen
    int zoomHot = -1;                   // highlighted item inside the open dropdown
    // the sidebar rail (the reference app's SidebarToggleStrip): a 24px strip
    // that always sits between the sidebar and the page - the sidebar collapses
    // TO it, so it is how a closed sidebar comes back. Bottom stack top to
    // bottom: Split pane, Pages, Document Info - the three buttons asked for,
    // gray at rest, green while their feature is on, RowHover under the cursor
    HWND rail = nullptr;
    int railHot = -1;                   // hovered rail button, -1 none
    bool railPressed = false;           // a rail button is held down
    // split view: TWO documents side by side (KillerPDF's split pane, F10).
    // Pane A keeps its width and the window grows to make room for pane B,
    // which opens matching it; closing shrinks the window back by the same
    // amount. Each pane is one DocState above; the focused pane receives the
    // wheel, the keys, the find bar, the sidebar and the footer's page/zoom.
    bool split = false;                 // split view is on
    int splitX = 0;                     // divider x in view coords; pane A is [0, splitX)
    bool splitA = true;                 // the focused pane: true = A, false = B
    bool splitDrag = false;             // the divider is being dragged
    bool lineHot = false;               // the divider is hovered
    int splitGrow = 0;                  // how much OpenSplit grew the window, so CloseSplit
                                        // hands back exactly that (0: an even split, closes in place)
    // annotation gestures (KillerPDF's _isDrawing / _activeInk / drag group):
    // one live gesture at a time, pinned to the pane and page it armed on, in
    // that page's DIP space at the press-time scroll - a scroll during a stroke
    // cannot skew it (KillerPDF pins the gesture to its canvas the same way)
    bool annInk = false;                // Draw/Line/Shapes stroke is live
    int annKind = 0;                    // TOOL_LINE / TOOL_SHAPE / TOOL_DRAW of the live stroke
    bool annArm = false;                // press landed, waiting for movement (line/shape)
    bool annMove = false;               // Select tool: dragging a selected annotation
    int annPaneB = 0, annPage = -1;     // the gesture's pane and page
    int annSX = 0, annSY = 0;           // the pane's scroll at press time
    POINT annStart{}, annLast{};        // press / last pointer, view pixels
    float annFX = 0, annFY = 0;         // press point, page DIPs
    std::vector<float> annIX, annIY;    // the stroke so far (page DIPs)
    int annSelB = 0, annSelPage = -1, annSelIdx = -1;   // Select tool's chosen annotation
    int selCommit = 0;                  // flowing selection's tool: 0 copy, TOOL_HIGHLIGHT commits
    // the Draw tool's brush ring (KillerPDF's ShowBrushPreview): a circle the
    // size of the brush at the cursor, so the footprint shows before a stroke
    bool brushOn = false;
    POINT brushPt{};
    // the Text tool's edit box: a real child EDIT (the Find query's contract),
    // committed into an AT_TEXT annotation when it loses focus or the view moves
    HWND tedit = nullptr;
    WNDPROC teditDef = nullptr;
    int tePaneB = 0, tePage = -1;       // the box's pane and page
    float teX = 0, teY = 0, teW = 0;    // box origin + width, page DIPs
};
static App g;

// Sidebar state. Thumbs are small page renders, made lazily and LRU-capped at
// 256, so the sidebar never renders more than the few pages you can see.
struct Side {
    int w = 0, cw = 0, ch = 0, scroll = 0, docH = 0, tw = 0, cur = -1;
    int hover = -1;                     // the item under the cursor: KillerPDF's IsMouseOver row
    int hit = -1, moved = 0;            // click-vs-drag: a drag cancels the jump
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
    std::vector<FHit> hits;           // scan order: from the anchor page forward, then
                                      // wrapping to the pages before it
    int cur = -1;                     // the emphasized match (cursor)
    int scan = 0;                     // how many pages of this run have been read
    int anchor = 0;                   // the page the run starts at - the page you are
                                      // on first, so the counter's numbers land in one
                                      // OCR tick instead of a whole-book crawl
    unsigned gen = 0;                 // bumped on every cancel; a stale pump quits
    bool done = true;                 // the whole document has been scanned
    bool pending = false;             // a query sits in the debounce: no verdict yet, so
                                      // the cell shows nothing instead of a false "No matches"
    int hot = -1;                     // hovered bar button
    int dx = 0, dy = 0;               // drag offset from the default top-right spot
    bool drag = false;                // the grip is dragging the bar
    int dragX = 0, dragY = 0;         // the drag's last pointer position
    bool pane = false;                // which pane's document the scan reads: pinned at
                                      // open/query time, so refocusing the other pane
                                      // never hijacks a running scan (KillerPDF's search
                                      // binds to the viewer it started on)
};
static Find fd;
static HWND g_fedit = nullptr;                     // the query EDIT: the bar's only real
                                                   // window, a child of the view (its pixels
                                                   // are composite; see the Find section)
static WNDPROC g_feditDef = nullptr;               // the edit's stock proc (subclassed for keys)

static HFONT g_uifont = nullptr, g_sidefont = nullptr;
static HFONT g_glyphfont = nullptr, g_wordfont = nullptr;
static HFONT g_tbglyph = nullptr, g_tbcap = nullptr; // toolbar: E721 at 20, the caption at Killer Sans 14

static int PW(const DocState& D, int i) { return max(1, (int)std::lround(D.pw[i] * D.zoom * g.dpi)); }
static int PH(const DocState& D, int i) { return max(1, (int)std::lround(D.ph[i] * D.zoom * g.dpi)); }
static int Gap() { return max(4, (int)(10 * g.dpi)); }
static int SBar() { return (int)(12 * g.dpi); }   // overlay bar gutter: KillerPDF's ScrollBarThickness
static int CardW(bool b);                         // a pane's card width: its column minus the 8px
                                                  // inset on the edge that faces away (the window
                                                  // edge for B, the divider for A - the unsplit
                                                  // card keeps its classic right-edge inset)

// ---- split-view pane helpers ----
// Two documents side by side (KillerPDF's split pane): pane A is the classic
// App::d state, pane B the second App::dB. Each pane lays out its own document
// at its own card width, scrolls and zooms on its own, and the focused pane
// receives the wheel, the keys, the find bar, the sidebar and the footer.
// splitX is the divider LINE's x: pane A's pass spans [0, splitX), pane B's
// [splitX+SplitHalf(), cw), and the 8px gutter straddles the line, 4px a side.
static int SplitGap() { return (int)(8 * g.dpi); }   // KillerPDF's SplitGutter: the channel
                                                     // between the two cards reads the same
                                                     // whether the neighbor is the other pane
                                                     // or the window edge
static int SplitHalf() { return SplitGap() / 2; }    // 4px: each pane's shadow room beside the line
static bool PaneB(int x) { return g.split && x >= g.splitX + SplitHalf(); }   // a view x is in pane B:
                                                                              // the grab zone ends 4px
                                                                              // past the line
static bool FocusB() { return g.split && !g.splitA; }                        // the focused pane is B
static DocState& DV(bool b) { return b ? g.dB : g.d; }      // a pane's document state
static DocState& DVF() { return DV(FocusB()); }             // the focused pane's document state
static int FocusSy() { return DVF().sy; }                   // its vertical scroll
static int& PaneSx(bool b) { return DV(b).sx; }
static int& PaneSy(bool b) { return DV(b).sy; }

// ---- the content viewport (KillerPDF's ScrollViewer contract) ----
// A visible overlay bar is RESERVED, not overlaid: the WPF ScrollViewer lays
// the content out in what is left after the 12px bar column (and the 12px bar
// row), and clamps the scroll so the content's edge stops at the bar - pages
// never flow under the thumb. docsy's old math only reserved the column while
// centering; on any horizontal overflow the pages ran under the bar and the
// third-alpha thumb washed out over the page - the "transparent" split
// scrollbar. ViewW/ViewH are the one source of truth for the viewport every
// clamp, thumb map, hit test and centering term shares.
static int ViewW(const DocState& D, bool b) {
    int gut = (D.n && D.docH > g.ch) ? SBar() : 0;   // the vertical bar's reserved column
    return max(0, CardW(b) - gut);
}
static int ViewH(const DocState& D, bool b) {
    int gut = (D.n && D.docW > ViewW(D, b)) ? SBar() : 0;   // the horizontal bar's reserved row
    return max(0, g.ch - gut);
}

// the sidebar rail's three buttons and the Document Info dialog, defined with
// the rail section below
static int RW();                        // the rail's column width (0 in full screen)
static void RailRepaint();              // the rail repaints when a lit state flips
static void ToggleSplit();
static void ToggleSide();               // defined with the main window (the Pages button)
static void InfoOpen();
static bool InfoIsOpen();
static void SetStatusHold(const std::wstring& t, unsigned ms = 2500);   // the footer's held line
static void SetTool(int t, bool b);     // the rail's tool stack (KillerPDF's SetTool)
static void CommitTextEdit(bool commit = true);   // the Text tool's box (defined with the tools)

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

// 0 min, 1 max, 2 close - right-aligned, KillerPDF's whole caption row. The
// search button lives one row down in the toolbar (BarDraw), so the caption
// stays exactly KillerPDF's three window buttons and the app carries exactly
// one search button.
static void CapRects(RECT r[3], int w) {
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

// ---------------- toolbar row (KillerPDF's Row 1) ----------------
// Under the caption KillerPDF runs a full-width toolbar border: BackgroundBrush
// #1c1c1c under the shared grain (MainWindow.xaml's ToolbarRowBorder + the
// GrainBrushShared overlay), holding ToolbarGrid (Margin 4,2) with the tool
// buttons. docsy is a viewer, so its bar carries the one tool that exists here:
// ToolbarLayout's "search" unit - the magnifier E721 with Str_Lbl_Search "Find
// Text" and the Ctrl:F caption (Shell/ToolbarLayout.cs), whose Search_Click
// toggles the find bar. The button is the family ToolbarButton in the default
// Large + Under mode (Shell/ToolbarStyle.cs): glyph FontSize 20 over a Killer
// Sans 14 caption with a 2px gap, MinHeight 50, Padding 6,4 - sitting directly
// on the chrome (no box at rest), a radius-3 SmallCornerRadius RowHoverBrush
// #404040 tile on hover, SurfaceBrush #333333 while pressed, Opacity 0.4 when
// no document is open, the Hand cursor, and the content's icon shadow (black,
// blur 4, depth 2, IconShadowOpacity 0.9 on the dark themes) as the 1px dark
// offset copy the caption's wordmark already uses. Click acts on release inside,
// with capture held - WPF button semantics - and opens the same find bar as
// Ctrl+F and the caption button.

// The button's auto height, the way WPF's Under mode sizes it: the content
// column (the MDL2 line box for FontSize 20 is ~23.5px, the Killer Sans 14
// caption's cell follows the font) plus Padding 6,4, with MinHeight 50 as the
// floor. Measured once per DPI from the real font; the row is the button plus
// ToolbarGrid's Margin 4,2. KillerPDF's own render (the user's screenshot,
// 100% scale) reads caption 32 + row ~55-56, which is this math.
static int TbBtnH() {
    static int h = 0; static double d = 0;
    if (!h || d != g.dpi) {
        HDC dc = GetDC(g.bar);
        RECT rc{ 0, 0, 0, 0 };
        HFONT of = (HFONT)SelectObject(dc, CaptionFont(g_tbcap, 14, false, L"Killer Sans"));
        DrawTextW(dc, L"Find Text", -1, &rc, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(dc, of);
        ReleaseDC(g.bar, dc);
        int capH = rc.bottom - rc.top;
        h = max((int)(50 * g.dpi), (int)(24 * g.dpi) + (int)(2 * g.dpi) + capH + (int)(8 * g.dpi));
        d = g.dpi;
    }
    return h;
}
static int TBH() { return g.full ? 0 : TbBtnH() + (int)(4 * g.dpi); }   // + ToolbarGrid Margin 4,2

static void FindToggle();                        // the find bar's own section, below
static void TrackLeave(HWND h);                  // defined with the view's hover helpers
static int FdD(int v) { return (int)std::lround(v * g.dpi); }

// Metrics (dpi units): shadow margins around the visual bar, then the row.
// The bar is KillerPDF's Border to the pixel: 1px PaneBorderBrush frame, 4px
// padding, the content grid's own 8/6/8/6 margin, then the 26px row -> a 48px
// card. Bar width = the fixed cells + the edit; the edit shrinks (60..200) so
// the whole bar always fits the card, FitSearchBox-style.
static const int FdPadX = 24, FdPadT = 14, FdPadB = 28;   // window margins for the shadow
static const int FdBarH = 48, FdEditH = 26, FdBtnW = 26, FdBtnH = 24, FdStatW = 96;
static const int FdGripW = 20;   // the drag grip: margin 2 + two 3px dot columns + margin 6
// left border+pad+grid margin 1+4+8 | grip 20 | icon 12+6 | Aa | ab | status 2+96+2 | prev | next | close | grid margin+pad+border 8+4+1
static const int FdBarFix = 13 + FdGripW + 12 + 6 + FdBtnW * 2 + 2 + FdStatW + 2 + FdBtnW * 3 + 13;

enum { FB_GRIP, FB_AA, FB_AB, FB_PREV, FB_NEXT, FB_CLOSE, FB_COUNT };

static int FdEditW(int vw) {
    return std::clamp(vw - FdD(FdBarFix) - FdD(16) - 2 * FdD(FdPadX), FdD(60), FdD(200));
}

struct FdRects {
    RECT bar, grip, icon, edit, status, btn[FB_COUNT];
};

static void FdLayout(int editW, FdRects& R) {
    int bw = FdD(FdBarFix) + editW;
    R.bar = { FdD(FdPadX), FdD(FdPadT), FdD(FdPadX) + bw, FdD(FdPadT) + FdD(FdBarH) };
    R.btn[FB_GRIP] = { 0, 0, 0, 0 };                          // the grip is its own cell
    int top = R.bar.top + FdD(11);                            // inside border 1 + padding 4 +
                                                              // the content grid's top margin 6
    int x = R.bar.left + FdD(13);                             // the grid's left margin 8
    R.grip = { x, top, x + FdD(FdGripW), top + FdD(FdEditH) };   // KillerPDF's drag grip cell
    x += FdD(FdGripW);
    R.icon = { x, top, x + FdD(12), top + FdD(FdEditH) };
    x += FdD(12) + FdD(6);
    R.edit = { x, top, x + editW, top + FdD(FdEditH) };
    x += editW;
    for (int i = FB_AA; i < FB_COUNT; i++) {
        if (i == FB_PREV) {                                   // the status cell sits before prev,
            R.status = { x + FdD(2), top,                     // centred on the 26px row like the
                         x + FdD(2) + FdD(FdStatW), top + FdD(FdEditH) };   // WPF TextBlock is
            x += FdD(2) + FdD(FdStatW) + FdD(2);
        }
        R.btn[i] = { x, top + FdD(1), x + FdD(FdBtnW), top + FdD(1) + FdD(FdBtnH) };
        x += FdD(FdBtnW);
    }
}

static int FdHit(POINT p) {
    FdRects R; FdLayout(FdEditW(CardW(FocusB())), R);
    if (p.x >= R.grip.left && p.x < R.grip.right && p.y >= R.grip.top && p.y < R.grip.bottom)
        return FB_GRIP;
    for (int i = FB_AA; i < FB_COUNT; i++)
        if (p.x >= R.btn[i].left && p.x < R.btn[i].right && p.y >= R.btn[i].top && p.y < R.btn[i].bottom)
            return i;
    return -1;
}

// The bar's geometry in VIEW coordinates. The old design moved a child window
// and re-derived everything from it; now the origin is pure arithmetic - the
// home spot (KillerPDF's fallback placement) plus the grip's clamped drag
// offset - and paint, hit tests and invalidations all ride it.
static void FdOrigin(int& bx, int& by) {     // the bar anchors over the FOCUSED pane's card:
    int bw = FdD(FdBarFix) + FdEditW(CardW(FocusB()));   // the surface its scan reads
    bx = CardW(FocusB()) - FdD(16) - bw - FdD(FdPadX) + fd.dx;
    by = FdD(6) - FdD(FdPadT) + fd.dy;
}
static RECT FdRect() {                       // the full shadow-padded window rect
    int bx, by; FdOrigin(bx, by);
    int bw = FdD(FdBarFix) + FdEditW(CardW(FocusB()));
    return { bx, by, bx + bw + 2 * FdD(FdPadX), by + FdD(FdBarH) + FdD(FdPadT) + FdD(FdPadB) };
}
static int FdHitView(POINT p) {              // a cell hit in view coordinates
    int bx, by; FdOrigin(bx, by);
    p.x -= bx; p.y -= by;
    return FdHit(p);
}
static bool FdOverBar(POINT p) {             // over the visual card (frame, not shadow)
    int bx, by; FdOrigin(bx, by);
    FdRects R; FdLayout(FdEditW(CardW(FocusB())), R);
    p.x -= bx; p.y -= by;
    return p.x >= R.bar.left && p.x < R.bar.right && p.y >= R.bar.top && p.y < R.bar.bottom;
}

// The find bar composites into the view's back buffer, so the view's proc needs
// the bar's geometry and cell actions (defined with the find section, below):
static void FindPlace();                         // park the edit inside its ring
static void FindStatusRepaint();                 // repaint the counter cell
static void FindDraw(HDC dc, int ox, int oy);    // the bar's paint (composite pixels)
static void FindStep(int dir);                   // prev / next
static void FindOption(bool matchCase);          // Aa / ab
static void FindClose();                         // the bar leaves
static void FindRun();                           // the debounce fired: start the scan
static void FindPump();                          // the scan's timer tick
static void FindChanged();                       // the query edit's EN_CHANGE

static HDC g_bardc = nullptr; static HBITMAP g_barbm = nullptr, g_barold = nullptr;
static int g_barw = 0, g_barh = 0;
static void EnsureBarBack(HDC ref, int w, int h) {
    if (g_barbm && g_barw == w && g_barh == h) return;
    if (g_barbm) { SelectObject(g_bardc, g_barold); DeleteObject(g_barbm); }
    else g_bardc = CreateCompatibleDC(ref);
    g_barbm = CreateCompatibleBitmap(ref, w, h);
    g_barold = (HBITMAP)SelectObject(g_bardc, g_barbm);
    g_barw = w; g_barh = h;
}

// The rail and the Document Info dialog paint through the same one-buffer
// contract as the toolbar above: draw the frame into memory, present it as one
// BitBlt. They are the two surfaces the new buttons live on, and painting them
// straight to the screen is what made hovering ANY of those buttons flash -
// a direct paint lands in two visible steps (the field fill erases the icons,
// then the icons redraw over it), so every hover flip strobed the whole strip.
// A buffered present is atomic: the same flip costs one blit and shows one
// state, exactly like the toolbar, the footer, the sidebar and the view.
static HDC g_raildc = nullptr; static HBITMAP g_railbm = nullptr, g_railold = nullptr;
static int g_railw = 0, g_railh = 0;
static void EnsureRailBack(HDC ref, int w, int h) {
    if (g_railbm && g_railw == w && g_railh == h) return;
    if (g_railbm) { SelectObject(g_raildc, g_railold); DeleteObject(g_railbm); }
    else g_raildc = CreateCompatibleDC(ref);
    g_railbm = CreateCompatibleBitmap(ref, w, h);
    g_railold = (HBITMAP)SelectObject(g_raildc, g_railbm);
    g_railw = w; g_railh = h;
}
static HDC g_iddc = nullptr; static HBITMAP g_idbm = nullptr, g_idold = nullptr;
static int g_idw = 0, g_idh = 0;
static void EnsureInfoBack(HDC ref, int w, int h) {
    if (g_idbm && g_idw == w && g_idh == h) return;
    if (g_idbm) { SelectObject(g_iddc, g_idold); DeleteObject(g_idbm); }
    else g_iddc = CreateCompatibleDC(ref);
    g_idbm = CreateCompatibleBitmap(ref, w, h);
    g_idold = (HBITMAP)SelectObject(g_iddc, g_idbm);
    g_idw = w; g_idh = h;
}

// The button's width: the wider of the 20px glyph and the "Find Text" caption,
// plus Padding 6,4's 12 horizontal pixels (auto width in Under mode).
static int BarBtnW(HDC dc) {
    RECT rc{ 0, 0, 0, 0 };
    HFONT of = (HFONT)SelectObject(dc, CaptionFont(g_tbcap, 14, false, L"Killer Sans"));
    DrawTextW(dc, L"Find Text", -1, &rc, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, of);
    int capW = rc.right - rc.left;
    return max((int)(20 * g.dpi), capW) + (int)(12 * g.dpi);
}

// The button box inside the row: ToolbarGrid's Margin 4,2 - 4 from the right
// edge, 2 from the top, auto height (TbBtnH, MinHeight 50 floor).
static RECT BarBtnRect(int w) {
    HDC dc = GetDC(g.bar);
    int bw = BarBtnW(dc);
    ReleaseDC(g.bar, dc);
    int bh = TbBtnH();
    int right = w - (int)(4 * g.dpi);
    return { right - bw, (int)(2 * g.dpi), right, (int)(2 * g.dpi) + bh };
}

static int BarHit(POINT p, int w) {
    RECT b = BarBtnRect(w);
    return (p.x >= b.left && p.x < b.right && p.y >= b.top && p.y < b.bottom) ? 0 : -1;
}

static void BarDraw(HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    RECT bar{ 0, 0, w, h };
    FillRect(dc, &bar, GrainBrush(1));   // the shared dark chrome: title bar + toolbar + sidebar + footer
    RECT b = BarBtnRect(w);
    bool en = DVF().n != 0;                // ToolbarAvailability: search needs an open document
    bool hot = g.tbHot == 0, pr = g.tbPressed && en;
    if (hot || pr) {                     // the hover/pressed tile: radius-3 SmallCornerRadius
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath p;
        float rr = (float)(3 * g.dpi);
        float fx = (float)b.left, fy = (float)b.top;
        float fw = (float)(b.right - b.left), fh = (float)(b.bottom - b.top);
        p.AddArc(fx, fy, 2 * rr, 2 * rr, 180, 90);
        p.AddArc(fx + fw - 2 * rr, fy, 2 * rr, 2 * rr, 270, 90);
        p.AddArc(fx + fw - 2 * rr, fy + fh - 2 * rr, 2 * rr, 2 * rr, 0, 90);
        p.AddArc(fx, fy + fh - 2 * rr, 2 * rr, 2 * rr, 90, 90);
        p.CloseFigure();
        // RowHoverBrush #404040, pressed SurfaceBrush #333333; disabled bakes the
        // style's Opacity 0.4 over #1c1c1c: 0.4*#404040 + 0.6*#1c1c1c = #2a2a2a
        Gdiplus::Color c = pr ? Gdiplus::Color(255, 0x33, 0x33, 0x33)
                              : Gdiplus::Color(255, 0x40, 0x40, 0x40);
        if (!en) c = Gdiplus::Color(255, 0x2a, 0x2a, 0x2a);
        Gdiplus::SolidBrush f(c);
        gs.FillPath(&f, &p);
    }
    // the content column: E721 at 20 with the caption 2px under, both centered
    // (ToolbarStyle.cs BuildToolbarContent, Under mode), and the icon shadow's
    // 1px dark copy under the light pass - the wordmark's treatment
    RECT mc{ 0, 0, 0, 0 };
    HFONT cf = CaptionFont(g_tbcap, 14, false, L"Killer Sans");
    HFONT of = (HFONT)SelectObject(dc, cf);
    DrawTextW(dc, L"Find Text", -1, &mc, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, of);
    int capH = mc.bottom - mc.top;
    int gh = (int)(24 * g.dpi), gap = (int)(2 * g.dpi);   // the MDL2 line box at FontSize 20 (~23.5px):
    int colH = gh + gap + capH;                            // a 20px cell would clip the glyph
    int avail = (int)(b.bottom - b.top) - colH;            // MSVC: RECT members are LONG - cast
    int y0 = b.top + max(0, avail) / 2;                    // before the std::max deduction
    RECT gr{ b.left, y0, b.right, y0 + gh };
    RECT cr{ b.left, y0 + gh + gap, b.right, y0 + gh + gap + capH };
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, CaptionFont(g_tbglyph, 20, false, L"Segoe MDL2 Assets"));
    SetTextColor(dc, RGB(16, 16, 16));                             // the shadow copy
    RECT gs1{ gr.left + 1, gr.top + 1, gr.right + 1, gr.bottom + 1 };
    DrawTextW(dc, L"\uE721", -1, &gs1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT cs1{ cr.left + 1, cr.top + 1, cr.right + 1, cr.bottom + 1 };
    SelectObject(dc, cf);
    DrawTextW(dc, L"Find Text", -1, &cs1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, en ? RGB(0xe0, 0xe0, 0xe0) : RGB(0x6a, 0x6a, 0x6a));   // TextBrush, 0.4 when dimmed
    SelectObject(dc, CaptionFont(g_tbglyph, 20, false, L"Segoe MDL2 Assets"));
    DrawTextW(dc, L"\uE721", -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, cf);
    DrawTextW(dc, L"Find Text", -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:                        // the Find Text button anchors to the right
        InvalidateRect(h, nullptr, FALSE);   // edge: a resize only dirties the exposed
        return 0;                            // strip, so the row must repaint whole or
                                             // the button vanishes and old copies linger
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureBarBack(dc, r.right, r.bottom);
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // same grain phase as every pane
            SetBrushOrgEx(g_bardc, -po.x, -po.y, nullptr);
            BarDraw(g_bardc, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, g_bardc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {               // mirrors/hover heals, like the caption band
        RECT r; GetClientRect(h, &r);
        POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
        SetBrushOrgEx((HDC)wp, -po.x, -po.y, nullptr);
        BarDraw((HDC)wp, r.right, r.bottom);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int hot = BarHit(p, r.right);
        if (hot != g.tbHot) {            // repaints only on a hover flip
            g.tbHot = hot;
            if (hot >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.tbHot != -1) { g.tbHot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        if (BarHit(p, r.right) == 0 && DVF().n) {   // capture held: WPF's IsPressed story
            SetCapture(h);
            g.tbPressed = true;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        bool was = g.tbPressed;
        if (was) {
            g.tbPressed = false;
            if (GetCapture() == h) ReleaseCapture();
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT r; GetClientRect(h, &r);
            if (BarHit(p, r.right) == 0 && DVF().n) FindToggle();   // click on release inside
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:              // the press lost the mouse some other way
        if (g.tbPressed) { g.tbPressed = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(g.main, m, wp, lp);   // route like the footer does: the wheel scrolls
                                                  // the page from any chrome surface
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT r; GetClientRect(h, &r);
            SetCursor(LoadCursorW(nullptr, BarHit(p, r.right) == 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
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

static void ClearCache(DocState& D) {
    for (auto& kv : D.cache) DeleteObject(kv.second.b);
    D.cache.clear();
}

static HFONT UiFont() {
    if (!g_uifont) {
        g_uifont = CreateFontW(-MulDiv(9, (int)(g.dpi * 96), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    return g_uifont;
}

static HBITMAP Render(DocState& D, int i, int w, int h) {
    try {
        auto page = D.doc.GetPage((uint32_t)i);
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

static HBITMAP GetBmp(DocState& D, int i, int w, int h) {
    auto it = D.cache.find(i);
    if (it != D.cache.end()) {
        if (it->second.w == w && it->second.h == h) { it->second.t = ++D.tick; return it->second.b; }
        DeleteObject(it->second.b); D.cache.erase(it);
    }
    HBITMAP b = Render(D, i, w, h);
    if (!b) return nullptr;
    D.cache[i] = Ent{ b, w, h, ++D.tick };
    return b;
}

// The same render as Render(), decoded as a SoftwareBitmap - the input format the
// OS OCR engine consumes for the text-selection pass.
static winrt::Windows::Graphics::Imaging::SoftwareBitmap RenderSoft(DocState& D, int i, int w, int h) {
    try {
        auto page = D.doc.GetPage((uint32_t)i);
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

static void Evict(DocState& D, int lo, int hi) {
    while (D.cache.size() > 24) {
        int k = -1; unsigned long long best = ~0ULL;
        for (auto& kv : D.cache)
            if ((kv.first < lo - 1 || kv.first > hi + 1) && kv.second.t < best) { best = kv.second.t; k = kv.first; }
        if (k < 0) break;
        DeleteObject(D.cache[k].b); D.cache.erase(k);
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
static int PerRow(const DocState& D) { return D.vm == 2 ? 2 : D.vm == 3 ? 3 : 1; }
static int RowCount(const DocState& D) {
    if (!D.n) return 0;
    if (D.vm == 1) return 1;                          // single: one row holding the current page
    return (D.n + PerRow(D) - 1) / PerRow(D);
}
static int RowPage(const DocState& D, int r, int k) { return D.vm == 1 ? D.cur : r * PerRow(D) + k; }
static int RowAt(const DocState& D, int y) { return PageIn(D.top, RowCount(D), y); }
static int PageTop(const DocState& D, int i) {       // page's row top (pixel)
    if (!D.n || D.top.empty()) return 0;
    int r = std::clamp(i / PerRow(D), 0, (int)D.top.size() - 2);
    return D.top[r];
}
static int PageAt(const DocState& D, int y) {        // primary page at a viewport y
    if (!D.n) return 0;
    int r = RowAt(D, y);
    return D.vm == 1 ? D.cur : min(D.n - 1, r * PerRow(D));
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
static TxtPage* TtFor(DocState& D, int i) {
    if (!D.n || i < 0 || i >= D.n) return nullptr;
    auto it = D.txt.find(i);
    if (it != D.txt.end()) { it->second.tick = ++D.tick; return &it->second; }
    TxtPage pt; pt.w = D.pw[i]; pt.h = D.ph[i];
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
            int ow = std::clamp((int)std::lround(D.pw[i] * 2), 800, 1600);
            int oh = max(1, (int)std::lround(ow * D.ph[i] / max(1.f, D.pw[i])));
            auto soft = RenderSoft(D, i, ow, oh);
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
    pt.tick = ++D.tick;
    D.txt[i] = std::move(pt);
    while (D.txt.size() > 48) {             // LRU: drop the stalest page's geometry
        auto oldest = D.txt.end();
        for (auto x = D.txt.begin(); x != D.txt.end(); ++x)
            if (x->first != i && (oldest == D.txt.end() || x->second.tick < oldest->second.tick)) oldest = x;
        if (oldest == D.txt.end()) break;
        D.txt.erase(oldest);
    }
    return &D.txt[i];
}

// Peek at already-built text geometry without triggering the OCR build - the
// cursor only advertises text that is ready, and the build itself happens off
// the click path (the pre-warm timer, or the first press as a last resort).
static TxtPage* TtPeek(DocState& D, int i) {
    auto it = D.txt.find(i);
    return it != D.txt.end() ? &it->second : nullptr;
}

static void SelClear(DocState& D) {
    bool had = D.selRange || D.selArm || D.selDrag || !D.selText.empty();
    D.selArm = D.selDrag = D.selRange = false;
    D.selAPage = D.selFPage = -1;
    D.selACaret = D.selFCaret = 0;
    D.selText.clear();
    g.selCommit = 0;                       // the flowing highlight's tool dies with the
    if (had && g.view) InvalidateRect(g.view, nullptr, FALSE);   // selection it rode
}

// The anchor->focus pair in reading order.
static void SelOrdered(const DocState& D, int& sp, int& sc, int& ep, int& ec) {
    bool aFirst = D.selAPage < D.selFPage || (D.selAPage == D.selFPage && D.selACaret <= D.selFCaret);
    if (aFirst) { sp = D.selAPage; sc = D.selACaret; ep = D.selFPage; ec = D.selFCaret; }
    else { sp = D.selFPage; sc = D.selFCaret; ep = D.selAPage; ec = D.selACaret; }
}

// The caret slice of the selection that falls on one page, or (0,0) when none.
static void SelSliceForPage(DocState& D, int page, int& start, int& end) {
    start = end = 0;
    int sp, sc, ep, ec;
    SelOrdered(D, sp, sc, ep, ec);
    if (page < sp || page > ep) return;
    TxtPage* pt = TtFor(D, page);
    int n = pt ? (int)pt->cells.size() : 0;
    start = page == sp ? sc : 0;
    end = page == ep ? ec : n;
    start = std::clamp(start, 0, n);
    end = std::clamp(end, 0, n);
    if (start > end) std::swap(start, end);
}

// Cross-page text: one page's slice per line group, pages joined with '\n'
// (KillerPDF's BuildSelectedText).
static std::wstring SelBuildText(DocState& D, int* words) {
    if (words) *words = 0;
    int sp, sc, ep, ec;
    SelOrdered(D, sp, sc, ep, ec);
    std::wstring out;
    for (int p = sp; p <= ep; p++) {
        TxtPage* pt = TtFor(D, p);
        if (!pt || pt->cells.empty()) continue;
        int s, e;
        SelSliceForPage(D, p, s, e);
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
static void SelAll(DocState& D) {
    if (!D.n) return;
    int pg = PageAt(D, D.sy + g.ch / 3);
    TxtPage* pt = TtFor(D, pg);
    if (!pt || pt->cells.empty()) return;
    SelClear(D);
    D.selAPage = D.selFPage = pg;
    D.selACaret = 0;
    D.selFCaret = (int)pt->cells.size();
    D.selRange = true;
    D.selText = TtText(*pt, 0, (int)pt->cells.size(), nullptr);
    if (!D.selText.empty()) CopyClipboard(D.selText);
    InvalidateRect(g.view, nullptr, FALSE);
}

// The view->page transform, mirroring Draw()'s page placement exactly: which page a
// view point falls on and where it sits inside that page, in DIPs. The point is
// clamped into the page, so dragging past an edge parks the caret at the start or
// end of the nearest line instead of losing the selection (KillerPDF clamps the
// drag point into the page canvas the same way). Multi-column rows resolve the
// page by x (a point in the gap clamps to the nearest page, so a drag across a
// spread flows through both pages like KillerPDF's Continuous does).
// Pane-aware core: the caller names the scroll state the point lives in (pane
// B passes its own pair). Everything else is the classic mapping.
static bool ViewToPageAt(DocState& D, POINT v, int w, int h, int sx, int sy, int& page, float& dx, float& dy) {
    if (!D.n) return false;
    int gut = D.docH > h ? SBar() : 0;
    int offx = D.docW < w - gut ? (w - gut - D.docW) / 2 : -sx;
    int r = RowAt(D, v.y + sy);
    if (r < 0 || r >= RowCount(D)) return false;
    int cnt = D.vm == 1 ? 1 : min(PerRow(D), D.n - r * PerRow(D));
    int best = -1; float bestD = FLT_MAX;
    for (int k = 0; k < cnt; k++) {
        int i = RowPage(D, r, k);
        int x0 = offx + D.px[i], x1 = x0 + PW(D, i);
        float d = v.x < x0 ? (float)(x0 - v.x) : v.x > x1 ? (float)(v.x - x1) : 0;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return false;
    float k = (float)(D.zoom * g.dpi);
    if (k <= 0) return false;
    page = best;
    int py = D.top[r] - sy + (D.rh[r] - PH(D, best)) / 2;
    dx = (v.x - (offx + D.px[best])) / k;
    dy = (v.y - py) / k;
    dx = std::clamp(dx, 0.0f, (float)PW(D, best) / k);
    dy = std::clamp(dy, 0.0f, (float)PH(D, best) / k);
    return true;
}

static bool ViewToPage(DocState& D, POINT v, int w, int h, int& page, float& dx, float& dy) {
    return ViewToPageAt(D, v, w, h, D.sx, D.sy, page, dx, dy);
}

static void SideSync();
static void SideLayout();
static void LayoutMain();
static void StatusRepaint();

static void UpdateTitle() {   // the sidebar highlight follows the focused pane's
    DocState& D = DVF();                             // page under its upper third. The
    int c = D.n ? PageAt(D, FocusSy() + g.ch / 3) : -1;   // window text stays "Docsy" - the
    if (c != D.cur) {                             // open file's name shows nowhere.
        D.cur = c;
        if (g.view) SetTimer(g.view, 4, 160, nullptr);   // pre-warm the page's text once
        SideSync();                           // the scroll settles (never mid-gesture)
        StatusRepaint();                      // "Page N of M" moved
    }
}

static void UpdateScroll(DocState& D, bool b) {   // one pane's scroll clamps into ITS
    g.busy = true;                                // document's layout at ITS viewport: the
    D.sy = std::clamp(D.sy, 0, max(0, D.docH - ViewH(D, b)));   // reserved bar rows are part
    D.sx = std::clamp(D.sx, 0, max(0, D.docW - ViewW(D, b)));   // of the viewport now, so the
    g.busy = false;                               // content stops AT the bar, never under it
    D.scrollTick = GetTickCount64();
}

static void Layout(DocState& D, bool b) {   // one pane's document at the pane's own
    D.gap = Gap();                          // metrics: two panes hold two documents, so
    int per = PerRow(D);                    // two layouts - each reflows at its own card
    int rows = RowCount(D);                 // width and clamps at its own scroll range
    D.top.assign((size_t)rows + 1, 0);
    D.px.assign((size_t)max(D.n, 1), 0);
    D.rh.assign((size_t)max(rows, 1), 0);
    int y = D.gap; D.maxw = 0;
    for (int r = 0; r < rows; r++) {                 // pass 1: row heights and the widest row
        int i0 = D.vm == 1 ? D.cur : r * per;
        int cnt = D.vm == 1 ? 1 : min(per, D.n - r * per);
        int wsum = 0, hmax = 0;
        for (int k = 0; k < cnt; k++) { wsum += PW(D, i0 + k); hmax = max(hmax, PH(D, i0 + k)); }
        D.maxw = max(D.maxw, wsum + (cnt - 1) * D.gap);
        D.top[r] = y; D.rh[r] = hmax;
        y += hmax + D.gap;
    }
    D.top[rows] = y;
    D.docH = rows ? y : 0;
    D.docW = rows ? D.maxw + 2 * D.gap : 0;
    for (int r = 0; r < rows; r++) {                 // pass 2: each row centred on the widest one
        int i0 = D.vm == 1 ? D.cur : r * per;
        int cnt = D.vm == 1 ? 1 : min(per, D.n - r * per);
        int wsum = 0;
        for (int k = 0; k < cnt; k++) wsum += PW(D, i0 + k);
        int x = D.gap + (D.maxw - (wsum + (cnt - 1) * D.gap)) / 2;
        for (int k = 0; k < cnt; k++) { D.px[i0 + k] = x; x += PW(D, i0 + k) + D.gap; }
    }
    UpdateScroll(D, b);
    InvalidateRect(g.view, nullptr, FALSE);
    StatusRepaint();
    UpdateTitle();
}

static void SetZoomAt(double z, int ax, int ay, bool b) {
    DocState& D = DV(b);                          // the anchor names its pane: pane B zooms
    CommitTextEdit();                             // a zoom lands the open text box
    int& sxr = D.sx; int& syr = D.sy;             // around its own viewport point, in its
    z = std::clamp(z, 0.1, 4.0);                  // own document's layout
    double ry = D.docH ? (double)(syr + ay) / D.docH : 0, rx = D.docW ? (double)(sxr + ax) / D.docW : 0;
    if (z != D.zoom) ClearCache(D);
    D.zoom = z; D.fitMode = 2;              // an explicit zoom leaves the fit modes
    Layout(D, b);
    syr = (int)(ry * D.docH) - ay;
    if (D.docW > CardW(b)) sxr = (int)(rx * D.docW) - ax;
    UpdateScroll(D, b);
    InvalidateRect(g.view, nullptr, FALSE);
    UpdateTitle();
    StatusRepaint();
}

static void SetZoom(double z, int ax, int ay) { SetZoomAt(z, ax, ay, FocusB()); }

// KillerPDF's FitToWidth / FitToPage, mode-aware: the fit target is the widest
// ROW (a spread in TwoPage, 3 columns in Grid), and Page additionally height-
// limits the current row. The zoom% the status line and the combo show derives
// from the resulting scale.
static void ApplyFit(bool pageFit, bool b) {   // one pane's fit, at that pane's own
    DocState& D = DV(b);                       // card width - two panes fit independently
    if (!D.n || g.cw <= 0) return;
    int W = CardW(b) - SBar();   // the card's inner width minus the reserved overlay-bar column
    int per = PerRow(D);
    int c = PageAt(D, D.sy + g.ch / 3);
    int r = D.vm == 1 ? 0 : c / per;
    double z;
    if (D.vm == 2) {                         // the current spread's two pages share the row
        int a = c - c % 2, bb = min(D.n - 1, a + 1);
        z = (W - 3.0 * Gap()) / (max(1.f, D.pw[a] + D.pw[bb]) * g.dpi);
    } else if (D.vm == 3) {
        float mw = 0; for (float w : D.pw) mw = max(mw, w);
        z = (W - 4.0 * Gap()) / (per * mw * g.dpi);
    } else {
        int i = D.vm == 1 ? max(0, D.cur) : 0;
        float mw = D.vm == 1 ? D.pw[i] : 0;
        if (D.vm == 0) for (float w : D.pw) mw = max(mw, w);
        z = (W - 2.0 * Gap()) / (max(1.f, mw) * g.dpi);
    }
    if (pageFit) {                           // the current row must also fit vertically
        int i0 = D.vm == 1 ? max(0, D.cur) : r * per;
        int cnt = D.vm == 1 ? 1 : min(per, D.n - i0);
        float rh = 0; for (int k = 0; k < cnt; k++) rh = max(rh, D.ph[i0 + k]);
        z = min(z, (g.ch - 2.0 * Gap()) / (max(1.f, rh) * g.dpi));
    }
    SetZoomAt(z, 0, 0, b);
    D.fitMode = pageFit ? 1 : 0;
    StatusRepaint();
}

// One pane's scroll: clamped into the shared layout, invalidated over THAT
// pane's pixels only, and the sidebar/status follow just the focused pane (a
// background pane may scroll without dragging the page ring along).
static void ScrollPane(bool b, int x, int y) {
    DocState& D = DV(b);                        // this pane's own document and viewport: the
    CommitTextEdit();                           // the box rides nothing: a scroll lands it
    x = std::clamp(x, 0, max(0, D.docW - ViewW(D, b)));   // clamps stop the content AT the
    y = std::clamp(y, 0, max(0, D.docH - ViewH(D, b)));   // reserved bars, KillerPDF's ScrollViewer
    if (x == D.sx && y == D.sy) return;
    D.sx = x; D.sy = y;
    D.scrollTick = GetTickCount64();
    if (g.view) {
        RECT r{ 0, 0, g.split ? g.splitX : g.cw, g.ch };   // pane A ends at the divider line,
        if (b) r = { g.splitX, 0, g.cw, g.ch };            // pane B starts at it - the gutter
        InvalidateRect(g.view, &r, FALSE);                 // straddles the line, 4px a side
    }
    if (b == FocusB()) UpdateTitle();
}

static void ScrollTo(int x, int y) { ScrollPane(FocusB(), x, y); }

// Single mode's page flip (KillerPDF's F6 view): the document IS one page, so
// navigation replaces it and parks at the top. The sidebar's current-page ring
// and the footer's "Page N of M" follow.
static void SetPage(int i) {
    DocState& D = DVF();                       // the flip acts on the focused pane's document
    if (!D.n) return;
    i = std::clamp(i, 0, D.n - 1);
    D.cur = i;
    if (D.fitMode != 2) ApplyFit(D.fitMode == 1, FocusB()); else Layout(D, FocusB());
    D.sx = 0; D.sy = 0;                        // the flip parks the focused pane at the top
    UpdateScroll(D, FocusB());
    if (g.view) SetTimer(g.view, 4, 160, nullptr);   // pre-warm the new page's text
    SideSync();
    StatusRepaint();
}

// Click/wheel focus (KillerPDF's FocusPaneFromPointer): the pane under the
// pointer becomes the one every window-level surface serves - the sidebar's
// ring retargets to its document, the footer's page/zoom reads it, the find
// bar anchors over its card. One switch, one repaint; no per-surface plumbing.
static void FocusPane(bool b) {
    if (!g.split || b == FocusB()) return;
    g.splitA = !b;
    for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);   // the sidebar's thumbs
    sd.th.clear(); sd.scroll = 0; sd.cur = -1; sd.hold = 0;              // belong to the focused
    sd.cw = 0; sd.ch = 0;                                                // pane's document now
    SideLayout();
    UpdateTitle();
    SideSync();
    StatusRepaint();
    RailRepaint();                        // the Pages tab's light rides the focused pane too
    InvalidateRect(g.view, nullptr, FALSE);
}

static bool Open(DocState& D, const std::wstring& p) {   // opens into ONE pane: Ctrl+O and
    try {                                                // the toolbar act on the focused pane,
        wchar_t full[MAX_PATH * 4];                      // a drop loads the pane it landed on
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
        ClearCache(D);
        D.doc = d; D.n = (int)n; D.pw = w; D.ph = h; D.name = nm; D.path = full;
        D.sx = D.sy = 0; D.zoom = 1.0; D.fitMode = 2; D.cur = -1;   // every document opens
                                                      // at 100%; -1 so LayoutMain's UpdateTitle
                                                      // sees the change and syncs the sidebar highlight
        D.txt.clear();               // the new document's text geometry starts fresh
        D.annos.clear();             // the tools' marks die with their document - KillerPDF's
                                     // annotations live in the PDF; docsy's live with it
        D.tool = TOOL_SELECT;        // the pane's tool re-lands on Select, like KillerPDF's
                                     // open path restores the toolbar
        g.annSelPage = -1; g.annSelIdx = -1;   // and the Select tool's ring has nothing to hold
        SelClear(D);
        bool mine = fd.pane == (&D == &g.dB);        // the find dies only when ITS document
        if (mine && fd.open) {                       // dies; the other pane's scan keeps going
            if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
            if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
            fd = Find{};                             // the Find bar's results die with the old document
        }
        if (&D == &DVF()) {                          // the sidebar shows the FOCUSED pane's pages:
            for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);   // a drop or open
            sd.th.clear(); sd.scroll = 0; sd.cur = -1; sd.hold = 0; sd.tw = 0;   // into the pane
            sd.cw = 0; sd.ch = 0;                    // you read rebuilds the ring around it
            g.sideOn = true;
            if (g.side) ShowWindow(g.side, SW_SHOWNOACTIVATE);
        }
        LayoutMain();
        InvalidateRect(g.main, nullptr, FALSE);
        if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // the Find Text button undims (Opacity 0.4 -> 1)
        RailRepaint();                                      // Pages/Info undim, the Pages light lands
        return true;
    } catch (...) {
        if (!g.quiet) MessageBoxW(g.main, L"Could not open this file (invalid, encrypted or unsupported PDF).", L"Docsy", MB_ICONWARNING);
        return false;
    }
}

static void OpenDlg() {                          // the dialog opens into the FOCUSED pane
    wchar_t f[MAX_PATH * 2] = L"";
    OPENFILENAMEW o{}; o.lStructSize = sizeof o; o.hwndOwner = g.main;
    o.lpstrFilter = L"PDF files\0*.pdf\0All files\0*.*\0";
    o.lpstrFile = f; o.nMaxFile = (DWORD)_countof(f);
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) Open(DVF(), f);
}

static void HandleDrop(HDROP d, HWND h) {        // the pane under the drop takes the file:
    wchar_t f[MAX_PATH * 2];                     // the view maps the point to its pane, a
    if (DragQueryFileW(d, 0, f, (UINT)_countof(f))) {   // drop on the sidebar or the frame goes
        POINT p; DragQueryPoint(d, &p);          // to the pane the reader is working in
        bool b = FocusB();
        if (h == g.view) b = PaneB(p.x);
        Open(DV(b), f);
    }
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
static int STH(int i) {                 // sidebar thumb height: the FOCUSED pane's pages,
    DocState& D = DVF();                // the sidebar is a window-level surface like KillerPDF's
    return max(1, (int)std::lround(sd.tw * D.ph[i] / max(1.f, D.pw[i])));
}
static int SidePageAt(int y) { return PageIn(sd.top, DVF().n, y); }

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
    DocState& D = DVF();                    // the sidebar lays out the FOCUSED pane's pages
    if (!D.n || sd.cw <= 0) { sd.docH = 0; return; }
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
        sd.top.assign((size_t)D.n + 1, 0);
        int y = (int)(8 * g.dpi);                       // the first band's top
        for (int i = 0; i < D.n; i++) { sd.top[i] = y + padT; y = sd.top[i] + STH(i) + below; }
        sd.top[D.n] = y;
        sd.docH = y + (int)(8 * g.dpi);
    }
    sd.scroll = std::clamp(sd.scroll, 0, max(0, sd.docH - sd.ch));
    InvalidateRect(g.side, nullptr, FALSE);
    SetTimer(g.side, 2, 25, nullptr);
}

// Renders the visible thumbnail closest to the viewport centre. Returns true
// while visible thumbs are still missing, so a 25 ms timer can pump the rest.
static bool SidePump() {
    DocState& D = DVF();                    // thumbs render from the FOCUSED pane's document
    if (!D.n || sd.ch <= 0 || sd.docH <= 0) return false;
    if (!(g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE))) return false;   // nothing to pump while hidden (shot mode shows it first)
    int first = SidePageAt(sd.scroll), last = first;
    while (last + 1 < D.n && sd.top[last + 1] < sd.scroll + sd.ch) last++;
    int pick = -1, bestd = INT_MAX, center = (first + last) / 2;
    for (int i = max(0, first - 1); i <= min(D.n - 1, last + 1); i++) {
        if (sd.th.find(i) != sd.th.end()) continue;
        int d = abs(i - center);
        if (d < bestd) { bestd = d; pick = i; }
    }
    if (pick < 0) return false;
    HBITMAP b = Render(D, pick, sd.tw, STH(pick));
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
    DocState& D = DVF();                    // the ring follows the focused pane's page
    if (!g.side || !D.n) { sd.cur = -1; return; }
    int c = D.cur;
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
    DocState& D = DVF();                    // the sidebar paints the FOCUSED pane's pages
    if (!D.n || sd.cw <= 0 || sd.top.empty()) return;
    int avail = sd.cw - SBar();
    HGDIOBJ of = SelectObject(dc, SideFont());
    SetBkMode(dc, TRANSPARENT);
    HDC m = CreateCompatibleDC(dc);
    int padT = (int)(8 * g.dpi), labH = (int)(20 * g.dpi), padB = (int)(6 * g.dpi), gap = (int)(2 * g.dpi);
    int first = SidePageAt(sd.scroll);
    for (int i = first; i < D.n && sd.top[i] < sd.scroll + h; i++) {
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
    case WM_SIZE:                        // the cards re-flow to the width: a full
        InvalidateRect(h, nullptr, FALSE);   // repaint leaves no stale column behind
        return 0;
    case WM_MOUSEWHEEL:
        SideScrollTo(sd.scroll - GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetCapture(h);
        int gb = SBar();
        if (DVF().n && sd.docH > sd.ch && p.x >= sd.cw - gb && p.x < sd.cw) {
            int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp);
            if (p.y >= tp && p.y < tp + tl) { sd.barDrag = true; sd.barGrab = p.y - tp; }
            else SideScrollTo(sd.scroll + (p.y < tp ? -(int)(sd.ch * 0.9) : (int)(sd.ch * 0.9)));
            return 0;
        }
        sd.dp = p;
        sd.hit = SidePageAt(sd.dp.y + sd.scroll);
        sd.moved = 0;
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (sd.barDrag) {
            int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp);
            SideScrollTo((int)((long long)(p.y - sd.barGrab) * max(0, sd.docH - sd.ch) / max(1, sd.ch - tl)));
            return 0;
        }
        bool hot = DVF().n && sd.docH > sd.ch && p.x >= sd.cw - SBar() && p.x < sd.cw;
        if (hot) { int tl, tp; BarThumb(sd.docH, sd.ch, sd.scroll, tl, tp); hot = p.y >= tp && p.y < tp + tl; }
        int hov = (!sd.barDrag && DVF().n && sd.docH > 0 && p.x < sd.cw - SBar()
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
            if (dy) sd.moved += abs(dy);   // a drag only cancels the click: like the page
        }                                  // view, the wheel and the thumb own scrolling
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
        if (!bar && sd.moved <= (int)(4 * g.dpi) && sd.hit >= 0 && sd.hit < DVF().n) {
            sd.hold = GetTickCount64() + 600;   // the jump must not scroll the sidebar back
            if (DVF().vm == 1) SetPage(sd.hit);     // single mode: a thumb click flips to that page
            else ScrollTo(DVF().sx, max(0, PageTop(DVF(), sd.hit) - Gap() / 2));
        }
        sd.moved = 0; sd.hit = -1;
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            bool edge = g.sideOn && DVF().n && sd.cw > 0 && p.x >= sd.cw - 1;
            SetCursor(LoadCursorW(nullptr, edge ? IDC_SIZEWE : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DROPFILES: HandleDrop((HDROP)wp, h); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------- annotation tools (the rail's top stack) ----------------
// KillerPDF's editing tools, in docsy's pane language. The Text tool commits
// through a real child EDIT (the Find query's WS_CLIPCHILDREN contract, at
// KillerPDF's 165pt box and 24pt Segoe UI); the Draw / Line / Shapes tools
// drag in page-DIP space and commit on release with KillerPDF's own discard
// rules (a line needs >3pt of length, a shape >3pt of both dimensions, a
// one-point Draw commits as a round dot); the Highlight tool reuses the
// flowing selection and converts the swept lines on release
// (CommitFlowingHighlight). Each pane keeps its own tool, exactly like
// KillerPDF keeps the editing state per viewer instance.

// The text box commits on focus loss, on any view motion, or on Escape -
// Escape discards, everything else lands as an AT_TEXT annotation in the
// pane's own document.
static void CommitTextEdit(bool commit = true) {
    if (!g.tedit) return;
    HWND e = g.tedit;
    int len = GetWindowTextLengthW(e);
    std::wstring s;
    if (commit && len > 0) {
        s.resize(len);
        GetWindowTextW(e, s.data(), len + 1);
        if (s.find_first_not_of(L" \t\r\n") == std::wstring::npos) s.clear();
    }
    g.tedit = nullptr;
    if (GetFocus() == e) SetFocus(g.view);
    DestroyWindow(e);
    if (!s.empty()) {
        DocState& D = DV(g.tePaneB);
        Annot a;
        a.type = AT_TEXT;
        a.x = g.teX; a.y = g.teY; a.w = g.teW;
        a.fsize = 24; a.color = RGB(0, 0, 0); a.alpha = 255; a.text = s;
        // the box's height, measured at the CURRENT scale (the wrapped line
        // count is what the box showed while editing)
        float k = (float)(D.zoom * g.dpi);
        HDC m = GetDC(g.view);
        int hpx = (int)std::lround(24 * k);
        HFONT f = CreateFontW(-hpx, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ of = SelectObject(m, f);
        RECT mr{ 0, 0, (int)(g.teW * k), 10000 };
        DrawTextW(m, s.c_str(), -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        SelectObject(m, of); DeleteObject(f); ReleaseDC(g.view, m);
        a.h = max(24.0f, (mr.bottom - mr.top) / max(0.01f, k));
        D.annos[g.tePage].push_back(a);
    }
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

static LRESULT CALLBACK TextEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) { CommitTextEdit(false); return 0; }
    return CallWindowProcW(g.teditDef, h, m, wp, lp);
}

// The Text tool's press: re-edit a placed text annotation under the click
// (it leaves the list and comes back when the box closes), or open a fresh
// box centered on the hotspot (KillerPDF's PlaceTextBox).
static void TextEditOpen(bool b, int pg, float tx, float ty, float tw, const wchar_t* text);
static void TextPlace(bool b, POINT p) {
    DocState& D = DV(b);
    POINT q{ p.x - PaneX0(b) - CardX(b), p.y };
    int pg; float fx, fy;
    if (!ViewToPageAt(D, q, CardW(b), g.ch, D.sx, D.sy, pg, fx, fy)) return;
    auto it = D.annos.find(pg);
    float k = (float)(D.zoom * g.dpi);
    if (it != D.annos.end()) {
        for (int ai = (int)it->second.size() - 1; ai >= 0; ai--) {
            Annot& a = it->second[ai];
            if (a.type != AT_TEXT) continue;
            if (fx >= a.x && fx <= a.x + a.w && fy >= a.y && fy <= a.y + a.h) {
                std::wstring text = a.text;
                float tx = a.x, ty = a.y, tw = a.w;
                it->second.erase(it->second.begin() + ai);
                TextEditOpen(b, pg, tx, ty, tw, text.c_str());   // defined below
                return;
            }
        }
    }
    // a fresh box: 165pt wide, the first line's center on the I-beam hotspot
    int lineH = (int)std::lround(24 * k * 4.0 / 3.0);
    float half = (lineH / 2.0f) / max(0.01f, k);
    TextEditOpen(b, pg, fx, max(0.0f, fy - half), 165.0f, nullptr);
}

// Opens the box: a real child EDIT over its page position, one dark ring
// painted by the composite around it, KillerPDF's PrimaryBrush border.
static void TextEditOpen(bool b, int pg, float tx, float ty, float tw, const wchar_t* text) {
    CommitTextEdit();                       // one editor at a time (KillerPDF's _activeTextBox)
    DocState& D = DV(b);
    if (!D.n || pg < 0 || pg >= D.n) return;
    g.tePaneB = b; g.tePage = pg; g.teX = tx; g.teY = ty; g.teW = tw;
    float k = (float)(D.zoom * g.dpi);
    int ox, oy;
    if (!PageOrigin(D, b, pg, ox, oy)) return;
    int fontPx = max(8, (int)std::lround(24 * k));
    int lineH = (int)std::lround(fontPx * 4.0 / 3.0);
    int boxW = max(24, (int)std::lround(tw * k));
    int x = PaneX0(b) + ox + (int)std::lround(tx * k);
    int y = oy + (int)std::lround(ty * k);
    int boxH = lineH * 3;                   // room for three lines; ES_AUTOVSCROLL beyond
    // clamp into the pane's card so the editor never hides under the chrome
    int cardL = PaneX0(b) + CardX(b), cardR = cardL + CardW(b);
    x = std::clamp(x, cardL, max(cardL, cardR - boxW));
    y = std::clamp(y, 0, max(0, g.ch - boxH));
    HFONT f = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g.tedit = CreateWindowExW(0, L"EDIT", text ? text : L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                              x, y, boxW, boxH, g.view, nullptr, g.hinst, nullptr);
    if (!g.tedit) return;
    SendMessageW(g.tedit, WM_SETFONT, (WPARAM)f, TRUE);   // the font slot rides the edit
    g.teditDef = (WNDPROC)SetWindowLongPtrW(g.tedit, GWLP_WNDPROC, (LONG_PTR)TextEditProc);
    SetFocus(g.tedit);
    InvalidateRect(g.view, nullptr, FALSE);   // the composite paints the ring the edit's
                                              // punch-out exposes (WS_CLIPCHILDREN owns
                                              // the box's pixels from here)
}

// A stroke's invalidation: the gesture's footprint so far, in view pixels -
// the drag path never recomposites anything beyond its own ink (call it
// before and after a mutation; the invalid regions accumulate into one paint).
static void AnnInval(HWND h) {
    DocState& D = DV(g.annPaneB);
    int ox, oy;
    if (!PageOrigin(D, g.annPaneB, g.annPage, ox, oy)) { InvalidateRect(h, nullptr, FALSE); return; }
    float k = (float)(D.zoom * g.dpi);
    float x0, y0, x1, y1;
    Annot a;                                  // bounds of the live points
    a.type = AT_INK; a.width = 3;
    a.ix = g.annIX; a.iy = g.annIY;
    AnnotBounds(a, x0, y0, x1, y1);
    RECT r{ PaneX0(g.annPaneB) + ox + (int)floorf(x0 * k) - 3, oy + (int)floorf(y0 * k) - 3,
            PaneX0(g.annPaneB) + ox + (int)ceilf(x1 * k) + 4, oy + (int)ceilf(y1 * k) + 4 };
    InvalidateRect(h, &r, FALSE);
}

// The press under a drawing tool. Returns true when the tool consumed it.
static bool ToolDown(bool b, POINT p) {
    DocState& D = DV(b);
    if (!D.n) return false;
    int tool = D.tool;
    if (tool == TOOL_SELECT) return false;    // the classic selection path owns Select
    POINT q{ p.x - PaneX0(b) - CardX(b), p.y };
    int pg; float fx, fy;
    bool onPage = ViewToPageAt(D, q, CardW(b), g.ch, D.sx, D.sy, pg, fx, fy);
    if (tool == TOOL_HIGHLIGHT) {
        // KillerPDF #127 Phase 2: on text the highlight FLOWS along the runs
        // exactly like text selection; the release turns the lines into
        // per-line annotations. A press with no text under it hints instead
        // of silently laying down a rect.
        TxtPage* pt = onPage ? TtFor(D, pg) : nullptr;
        if (pt && TtOverText(*pt, fx, fy)) {
            SelClear(D);
            g.selCommit = TOOL_HIGHLIGHT;
            D.selArm = true; D.selDrag = false;
            D.selAPage = D.selFPage = pg;
            D.selACaret = D.selFCaret = TtCaret(*pt, fx, fy);
            D.selDownPos = p;
        } else {
            if (D.selRange || D.selArm) SelClear(D);
            SetStatusHold(L"No text here - drag over text to highlight it");
        }
        return true;
    }
    if (!onPage) return true;                 // a drawing press off the page does nothing
    if (tool == TOOL_TEXT) { TextPlace(b, p); return true; }
    g.annInk = true; g.annKind = tool; g.annArm = true;
    g.annPaneB = b; g.annPage = pg; g.annSX = D.sx; g.annSY = D.sy;
    g.annStart = g.annLast = p; g.annFX = fx; g.annFY = fy;
    g.annIX.clear(); g.annIY.clear();
    g.annIX.push_back(fx); g.annIY.push_back(fy);
    if (tool != TOOL_DRAW) { g.annIX.push_back(fx); g.annIY.push_back(fy); }   // the tracked end
    return true;
}

// The drag under a stroke or a move gesture. p is view pixels; the stroke
// lives in the press-time scroll's DIP space (a scroll mid-stroke cannot skew
// it, KillerPDF pins the gesture to its canvas the same way).
static void ToolMove(HWND h, POINT p) {
    DocState& D = DV(g.annPaneB);
    float k = (float)(D.zoom * g.dpi);
    if (g.annMove) {                          // the Select tool drags the chosen annotation
        auto it = D.annos.find(g.annPage);
        if (it != D.annos.end() && g.annSelIdx >= 0 && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            float bx0, by0, bx1, by1; AnnotBounds(a, bx0, by0, bx1, by1);
            float ddx = (p.x - g.annLast.x) / max(0.01f, k);
            float ddy = (p.y - g.annLast.y) / max(0.01f, k);
            if (ddx != 0 || ddy != 0) {
                // clamp the drag into the page (KillerPDF's ClampAnnotPos)
                float pw = g.annPage < (int)D.pw.size() ? D.pw[g.annPage] : 0;
                float phh = g.annPage < (int)D.ph.size() ? D.ph[g.annPage] : 0;
                float nx0 = bx0 + ddx, ny0 = by0 + ddy;
                float w = bx1 - bx0, hh = by1 - by0;
                nx0 = std::clamp(nx0, 0.0f, max(0.0f, pw - w));
                ny0 = std::clamp(ny0, 0.0f, max(0.0f, phh - hh));
                ddx = nx0 - bx0; ddy = ny0 - by0;
                if (a.type == AT_HIGHLIGHT) {
                    for (FRect& r : a.lines) { r.x += ddx; r.y += ddy; }
                    a.x = nx0; a.y = ny0;
                } else if (a.type == AT_INK) {
                    for (size_t i = 0; i < a.ix.size(); i++) { a.ix[i] += ddx; a.iy[i] += ddy; }
                } else { a.x = nx0; a.y = ny0; }
                g.annLast = p;
                RECT now{ 0, 0, 0, 0 };
                float ax0, ay0, ax1, ay1; AnnotBounds(a, ax0, ay0, ax1, ay1);
                int ox, oy;
                if (PageOrigin(D, g.annPaneB, g.annPage, ox, oy)) {
                    now = RECT{ PaneX0(g.annPaneB) + ox + (int)(ax0 * k) - 3, oy + (int)(ay0 * k) - 3,
                                PaneX0(g.annPaneB) + ox + (int)(ax1 * k) + 4, oy + (int)(ay1 * k) + 4 };
                }
                InvalidateRect(h, &now, FALSE);   // the bounds ride the annotation: one rect
                                                  // covers the pixels it left and entered
            }
        }
        return;
    }
    if (!g.annInk) return;
    POINT q{ p.x - PaneX0(g.annPaneB) - CardX(g.annPaneB), p.y };
    int pg; float fx, fy;
    if (!ViewToPageAt(D, q, CardW(g.annPaneB), g.ch, g.annSX, g.annSY, pg, fx, fy)) return;
    if (g.annKind == TOOL_DRAW) {
        size_t ln = g.annIX.size();
        if (!ln || fabsf(fx - g.annIX[ln - 1]) > 0.01f || fabsf(fy - g.annIY[ln - 1]) > 0.01f) {
            AnnInval(h);                      // old footprint first: bounds BEFORE the append
            g.annIX.push_back(fx); g.annIY.push_back(fy);
            AnnInval(h);                      // then the new one - the two regions join in
        }                                     // one paint, so the stroke never trails ghosts
        return;
    }
    // line / shape: the second point tracks the pointer (KillerPDF's preview)
    g.annArm = false;
    if (g.annIX.size() < 2) { g.annIX.push_back(fx); g.annIY.push_back(fy); }
    else { g.annIX[1] = fx; g.annIY[1] = fy; }
    AnnInval(h);
}

// The release: KillerPDF's commit rules, verbatim. A line needs >3pt of
// length, a shape >3pt of both dimensions, a one-point Draw commits as a dot
// (two points 0.25 apart meeting in the round caps).
static void ToolUp(HWND h) {
    DocState& D = DV(g.annPaneB);
    if (g.annMove) {
        g.annMove = false;
        return;
    }
    if (!g.annInk) return;
    g.annInk = false;
    int kind = g.annKind;
    auto& v = D.annos[g.annPage];
    if (kind == TOOL_LINE) {
        if (g.annIX.size() >= 2 && (fabsf(g.annIX[1] - g.annIX[0]) > 3 || fabsf(g.annIY[1] - g.annIY[0]) > 3)) {
            Annot a; a.type = AT_INK; a.width = 3; a.color = RGB(0xff, 0x00, 0x00);
            a.ix = g.annIX; a.iy = g.annIY;
            v.push_back(a);
        }
    } else if (kind == TOOL_SHAPE) {
        float x0 = min(g.annIX[0], g.annIX[1]), x1 = max(g.annIX[0], g.annIX[1]);
        float y0 = min(g.annIY[0], g.annIY[1]), y1 = max(g.annIY[0], g.annIY[1]);
        if (x1 - x0 > 3 && y1 - y0 > 3) {     // the filled rectangle: KillerPDF's default
            Annot a; a.type = AT_HIGHLIGHT; a.color = RGB(0xff, 0x00, 0x00); a.alpha = 80;
            a.lines.push_back(FRect{ x0, y0, x1 - x0, y1 - y0 });
            a.x = x0; a.y = y0; a.w = x1 - x0; a.h = y1 - y0;
            v.push_back(a);
        }
    } else {                                  // TOOL_DRAW
        if (!g.annIX.empty()) {
            if (g.annIX.size() == 1) {        // the dot: 0.25pt to the right, caps meet
                g.annIX.push_back(g.annIX[0] + 0.25f); g.annIY.push_back(g.annIY[0]);
            }
            Annot a; a.type = AT_INK; a.width = 3; a.color = RGB(0xff, 0x00, 0x00);
            a.ix = g.annIX; a.iy = g.annIY;
            v.push_back(a);
        }
    }
    g.annIX.clear(); g.annIY.clear();
    InvalidateRect(h, nullptr, FALSE);        // the commit repaints the page's overlay
}

// The Select tool's press: the topmost annotation under the pointer is chosen
// (KillerPDF hit-tests top-down) and armed for the drag; a miss drops the
// selection and lets the flowing text path arm. Returns true on a hit.
static bool SelectAnnotAt(HWND h, bool b, POINT p) {
    DocState& D = DV(b);
    POINT q{ p.x - PaneX0(b) - CardX(b), p.y };
    int pg; float fx, fy;
    if (!ViewToPageAt(D, q, CardW(b), g.ch, D.sx, D.sy, pg, fx, fy)) return false;
    auto it = D.annos.find(pg);
    if (it == D.annos.end()) {
        if (g.annSelPage >= 0) {
            g.annSelPage = -1; g.annSelIdx = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return false;
    }
    float k = (float)(D.zoom * g.dpi);
    for (int ai = (int)it->second.size() - 1; ai >= 0; ai--) {
        if (AnnotHit(it->second[ai], fx, fy, k)) {
            g.annSelB = b; g.annSelPage = pg; g.annSelIdx = ai;
            g.annMove = true; g.annPaneB = b; g.annPage = pg;
            g.annSX = D.sx; g.annSY = D.sy;
            g.annStart = g.annLast = p;
            InvalidateRect(h, nullptr, FALSE);   // the ring lands
            return true;
        }
    }
    if (g.annSelPage >= 0) {
        g.annSelPage = -1; g.annSelIdx = -1;
        InvalidateRect(h, nullptr, FALSE);
    }
    return false;
}

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

static void CardShadowStrip(BYTE* bits, int w, int h, int cx0, int cx1, int rad) {
    const double sig = 8.0 * g.dpi;
    const double depth = 5.0 * g.dpi;                     // ShadowDepth, Direction 270
    // the silhouette: the card pushed straight down, so the falloff reads
    // stronger along the gutter's lower half, like KillerPDF's render. The
    // card may sit inset in its DIB (pane B's left shadow room), so both
    // edges of the falloff are painted - each side lands only where the
    // gutter actually is.
    float sx0 = (float)cx0, sy0 = (float)depth, sx1 = (float)cx1, sy1 = (float)h + depth;
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
static void CardRingBake(BYTE* bits, int w, int h, int cw, int rad, int cx) {
    float x1 = (float)(cx + cw) - 0.5f, y1 = (float)h - 0.5f;   // outer silhouette
    float ox0 = (float)cx - 0.5f, oy0 = -0.5f, orad = (float)rad;   //   in pixel-center space
    float ix0 = (float)cx + 0.5f, iy0 = 0.5f, irad = (float)rad - 1;  // inner silhouette
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
    for (int y = 0; y < band2; y++)                       // top strip, then bottom,
        for (int x = 0; x < w; x++) ring(x, y);
    for (int y = max(band2, h - band2); y < h; y++)
        for (int x = 0; x < w; x++) ring(x, y);
    for (int y = band2; y < max(band2, h - band2); y++) {
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
static BYTE* g_bdBits[2] = { nullptr, nullptr };

static void CardBackdrop(HDC ref, int w, int h, bool b) {
    int slot = b ? 1 : 0;
    if (g_bdBmp[slot] && g_bdW[slot] == w && g_bdH[slot] == h && g_bdDpi[slot] == g.dpi) return;
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
        CardShadowStrip(bits, w, h, cx, cx + cw, rad);
        Gdiplus::Graphics gs(bdc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // the canvas interior, half a pixel in: GDI+ centers pixels on integer
        // coordinates, so the path edge must sit at the boundary between the
        // ring's outermost pixel and the canvas's first one to fill it whole
        Gdiplus::GraphicsPath* inner = CardPath((float)cx + 0.5f, 0.5f, (float)cw - 1, (float)h - 1,
                                                (float)rad - 1, 0);
        if (Gdiplus::TextureBrush* c = GrainTex(0)) {     // the canvas, phased like GDI's
            c->TranslateTransform((float)-xoffScreen, 0.f);
            gs.FillPath(c, inner);
        }
        else {
            Gdiplus::SolidBrush cb(Gdiplus::Color(255, 58, 58, 58));
            gs.FillPath(&cb, inner);
        }
        delete inner;
        CardRingBake(bits, w, h, cw, rad, cx);            // the #555 ring, exact per pixel
    }
    SelectObject(bdc, ob);
    DeleteDC(bdc);
    g_bdBmp[slot] = bm; g_bdDC[slot] = CreateCompatibleDC(ref);
    SelectObject(g_bdDC[slot], bm);
    g_bdBits[slot] = bits; g_bdW[slot] = w; g_bdH[slot] = h; g_bdDpi[slot] = g.dpi;
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
    return fx >= a.x - pad && fx <= a.x + a.w + pad && fy >= a.y - pad && fy <= a.y + a.h + pad;
}

static HFONT g_tafont = nullptr; static int g_tapx = 0;   // the text annotation's Segoe UI, cached per px

// One page's annotations, above the page and below the selection quads -
// KillerPDF's overlay order. Highlights are translucent line fills, ink rides
// GDI+ round caps at the stroke's own width, text wraps in the annotation's
// box, and the Select tool's chosen annotation wears the accent ring.
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
            Gdiplus::Pen pen(c, max(1.0f, a.width * k));
            pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
            gs.DrawLines(&pen, pts.data(), (INT)pts.size());   // a dot: two points 0.25 apart,
                                                               // round caps meet as a disc
        } else {
            int hpx = (int)std::lround(a.fsize * k);
            if (hpx > 0) {
                if (!g_tafont || g_tapx != hpx) {
                    if (g_tafont) DeleteObject(g_tafont);
                    g_tafont = CreateFontW(-hpx, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                    g_tapx = hpx;
                }
                HGDIOBJ of = SelectObject(dc, g_tafont);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(0, 0, 0));
                RECT tr{ px + (int)(a.x * k), py + (int)(a.y * k),
                         px + (int)((a.x + a.w) * k), py + (int)((a.y + a.h) * k) };
                DrawTextW(dc, a.text.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
                SelectObject(dc, of);
            }
        }
        if (D.tool == TOOL_SELECT && g.annSelPage == page && g.annSelIdx == (int)ai &&
            g.annSelB == (&D == &g.dB)) {
            float x0, y0, x1, y1; AnnotBounds(a, x0, y0, x1, y1);
            Gdiplus::Pen sel(Gdiplus::Color(255, 0x22, 0xbb, 0x56), 1.0f * (float)g.dpi);
            gs.DrawRectangle(&sel, Gdiplus::RectF(px + x0 * k, py + y0 * k,
                                                  max(1.0f, (x1 - x0) * k), max(1.0f, (y1 - y0) * k)));
        }
    }
}

// The live gesture's preview, painted exactly like the commit will read:
// Draw/Line ride the red round-cap stroke, Shapes drags a translucent fill
// (KillerPDF's filled-rectangle look, committed as a highlight-style fill).
static void PaintLiveStroke(HDC dc, const DocState& D, int page, int px, int py, float k) {
    if (!g.annInk || g.annPage != page || g.annPaneB != (&D == &g.dB)) return;
    if (g.annIX.empty()) return;
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (g.annKind == TOOL_SHAPE && g.annIX.size() >= 2) {
        float x0 = min(g.annIX[0], g.annIX[1]), x1 = max(g.annIX[0], g.annIX[1]);
        float y0 = min(g.annIY[0], g.annIY[1]), y1 = max(g.annIY[0], g.annIY[1]);
        Gdiplus::SolidBrush br(Gdiplus::Color(80, 0xff, 0x00, 0x00));
        gs.FillRectangle(&br, px + x0 * k, py + y0 * k, (x1 - x0) * k, (y1 - y0) * k);
        return;
    }
    size_t n = g.annKind == TOOL_DRAW ? g.annIX.size() : 2;
    if (n < 2) n = min<size_t>(2, g.annIX.size());
    if (n < 2) return;
    std::vector<Gdiplus::PointF> pts;
    for (size_t i = 0; i < n; i++)
        pts.push_back(Gdiplus::PointF(px + g.annIX[i] * k, py + g.annIY[i] * k));
    Gdiplus::Pen pen(Gdiplus::Color(255, 0xff, 0x00, 0x00), max(1.0f, 3.0f * k));
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
    bool card = cw > 2 * rad + 4 && h > 2 * rad + 2;
    HRGN clip = card ? CreateRoundRectRgn(xoff + cx + 2, 2, xoff + cx + cw - 2, h - 2, 2 * (rad - 2), 2 * (rad - 2)) : nullptr;
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
    float k = (float)(D.zoom * g.dpi);
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
            HBITMAP b = GetBmp(D, i, pw, ph);
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
            // the tools' committed annotations, above the page and below the
            // selection quads - KillerPDF's overlay order
            PaintAnnots(dc, D, i, x, y, k);
            PaintLiveStroke(dc, D, i, x, y, k);
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
                            gs.FillRectangle(&br, Gdiplus::RectF(x + q.x * k, y + q.y * k,
                                                                 max(q.w * k, 2.0f), max(q.h * k, 2.0f)));
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
    if (vshow) {
        int tl, tp; BarThumb(D.docH, h - gutH, D.sy, tl, tp);   // the v-track stops short of
        bool hot = D.vHot || D.vDrag;                           // the h-bar's reserved row
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
    if (vshow) {
        int tl, tp; BarThumb(D.docH, g.ch - gutH, D.sy, tl, tp);   // the v-track stops short
        int bw = vhot ? (int)(8 * g.dpi) : (int)(4 * g.dpi);       // of the h-bar's reserved row
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
                    } else {
                        RECT pr{ 0, 0, wA, g.ch };
                        HRGN rg = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
                        Draw(g.bdc, wA, g.ch, 0, g.d, rg);
                        DeleteObject(rg);
                        PaintPaneBars(g.bdc, g.d, false, g.d.vHot || g.d.vDrag, g.d.hHot || g.d.hDrag);
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
            }
            if (g.brushOn) {                        // the Draw tool's brush ring: the ink
                DocState& DC = DV(PaneB(g.brushPt.x));   // color at the brush's own size,
                float k = (float)(DC.zoom * g.dpi);      // KillerPDF's ShowBrushPreview
                Gdiplus::Graphics gs(g.bdc);
                gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                float d = max(4.0f, 3.0f * k);
                Gdiplus::Pen pen(Gdiplus::Color(255, 0xff, 0x00, 0x00), 1.0f * (float)g.dpi);
                gs.DrawEllipse(&pen, g.brushPt.x - d / 2, g.brushPt.y - d / 2, d, d);
            }
            if (g.tedit) {                          // the text box's ring: the one pixel the
                RECT er; GetWindowRect(g.tedit, &er);   // WS_CLIPCHILDREN punch-out exposes,
                ScreenToClient(h, (POINT*)&er);         // KillerPDF's PrimaryBrush border
                ScreenToClient(h, ((POINT*)&er) + 1);
                HPEN gp = CreatePen(PS_SOLID, 1, RGB(0x22, 0xbb, 0x56));
                HGDIOBJ og = SelectObject(g.bdc, gp);
                HGDIOBJ ob = SelectObject(g.bdc, GetStockObject(NULL_BRUSH));
                RECT er2{ er.left - 1, er.top - 1, er.right + 1, er.bottom + 1 };
                Rectangle(g.bdc, er2.left, er2.top, er2.right, er2.bottom);
                SelectObject(g.bdc, ob); SelectObject(g.bdc, og); DeleteObject(gp);
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
    case WM_MOUSEHWHEEL: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(g.view, &p);
        bool b = PaneB(p.x);
        DocState& D = DV(b);
        ScrollPane(b, D.sx + GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 80) / 120, D.sy);
        return 0;
    }
    case WM_KEYDOWN: return 0;   // keys live on the main window
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetCapture(h);
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
                int tl, tp; BarThumb(D.docH, g.ch - gutH, D.sy, tl, tp);   // the track stops
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
            int tl, tp; BarThumb(D.docH, g.ch - gutH, D.sy, tl, tp);
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
            if (ToolDown(pbT, p)) return 0;
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
                int tl, tp; BarThumb(D.docH, g.ch - gutH, D.sy, tl, tp); vhot = p.y >= tp && p.y < tp + tl;
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
            int tl, tp; BarThumb(D.docH, g.ch - gutH, D.sy, tl, tp); vhot = p.y >= tp && p.y < tp + tl;
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
    case WM_MOUSELEAVE:
        if (g.d.vHot || g.d.hHot || g.dB.vHot || g.dB.hHot || fd.hot >= 0) {
            g.d.vHot = g.d.hHot = g.dB.vHot = g.dB.hHot = false; fd.hot = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (g.lineHot) { g.lineHot = false; InvalidateRect(h, nullptr, FALSE); }
        if (g.brushOn) {                    // the brush ring dies with the hover
            RECT u{ g.brushPt.x - 12, g.brushPt.y - 12, g.brushPt.x + 12, g.brushPt.y + 12 };
            g.brushOn = false;
            InvalidateRect(h, &u, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
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
                    a.color = RGB(0xff, 0xff, 0x00); a.alpha = 80;   // _highlightColor
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
    case WM_CAPTURECHANGED:
        fd.drag = false;                       // someone else took it (a modal loop, a close)
        return 0;
    case WM_LBUTTONDBLCLK: {
        // double-click word selection, copied at once (KillerPDF's TrySelectWordAtPosition)
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
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
        if (HIWORD(wp) == EN_KILLFOCUS && (HWND)lp == g.tedit) CommitTextEdit();   // the text
        return 0;                                // tool's box lands when the focus leaves it
    case WM_CTLCOLOREDIT: {                    // the dark query edit; the text tool's box is
        HDC dc = (HDC)wp;                      // white over the page, KillerPDF's editor
        if ((HWND)lp == g.tedit) {
            SetTextColor(dc, RGB(0, 0, 0));
            SetBkColor(dc, RGB(0xff, 0xff, 0xff));
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

// Only the two fit modes have a name; a free zoom prints its own percentage
// alone - the zoom dropdown already reads that way ("35%"), and "Custom"
// next to the number said the same thing twice.
static const wchar_t* FitName() {
    return DVF().fitMode == 1 ? L"Fit Page" : L"Fit Width";
}

static std::wstring StatusRoutine() {   // the FOCUSED pane's page and zoom: the footer
    DocState& D = DVF();                // serves the pane the reader is working in
    if (!D.n) return L"Docsy  -  Ctrl+O or drop a PDF here";
    int c = D.cur >= 0 ? D.cur : 0;
    int pct = (int)std::lround(D.zoom * 100);
    wchar_t b[160];
    if (D.fitMode == 2)   // a free zoom: the number speaks for itself
        swprintf_s(b, L"Page %d of %d, %d%%", c + 1, D.n, pct);
    else
        swprintf_s(b, L"Page %d of %d, %s (%d%%)", c + 1, D.n, FitName(), pct);
    return b;
}

static void StatusRepaint() { if (g.status) InvalidateRect(g.status, nullptr, FALSE); }

static void SetStatusHold(const std::wstring& t, unsigned ms) {   // the default rides the
    g.statusHoldText = t;                                         // forward declaration above
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
    DocState& D = DVF();
    if (!D.n || D.cur < 0) return;
    int i = D.cur;
    double wpt = D.pw[i], hpt = D.ph[i];
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
    DocState& D = DVF();                     // the mode keys steer the focused pane's document
    if (!D.n || m == D.vm || m < 0 || m > 3) return;
    int c = PageAt(D, D.sy + g.ch / 3);
    D.vm = m;
    if (D.fitMode != 2) ApplyFit(D.fitMode == 1, FocusB()); else Layout(D, FocusB());
    ScrollTo(D.sx, max(0, PageTop(D, c) - Gap() / 2));   // the current page's row stays in sight
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
    if (i == 0) ApplyFit(true, FocusB());
    else if (i == 1) ApplyFit(false, FocusB());
    else if (i >= 2 && i <= 7) SetZoom(ZoomPcts[i - 2], g.cw / 2, g.ch / 2);
}
static int ZoomCurrent() {                       // which item the closed box shows
    DocState& D = DVF();
    if (D.fitMode == 1) return 0;
    if (D.fitMode == 0) return 1;
    double pct = D.zoom * 100;
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
            return (!DVF().n && i != ST_STATUS && i != ST_VER) ? -1 : i;
    }
    return -1;
}

static void ZoomBy(double f);   // defined with the main window's keys
static void ZoomPopupOpen();
static void ZoomPopupClose();

static void StatusAction(int id) {
    switch (id) {
    case ST_STATUS: {   // StatusText_Click: the FOCUSED pane's file, held while read
        if (DVF().path.empty()) break;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(DVF().path.c_str(), GetFileExInfoStandard, &fa)) {
            long long bytes = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            wchar_t sz[40];
            if (bytes >= 1LL << 20) FmtNum(sz, 40, bytes / 1048576.0, 2), wcscat_s(sz, L" MB");
            else if (bytes >= 1LL << 10) FmtNum(sz, 40, bytes / 1024.0, 1), wcscat_s(sz, L" KB");
            else swprintf_s(sz, L"%lld B", bytes);
            SetStatusHold(DVF().name + L" - " + sz);
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
    // the card edges in footer coords: past the sidebar AND the rail now, and
    // with the split on there are TWO cards - each casts its own shadow strip
    int vx0 = (g.sideOn && DVF().n && !g.full ? SW() : 0) + RW();
    int L = vx0;
    int keyL = vx0 * 4 + (g.split ? g.splitX : -1);
    if (!g_shbm || g_shw != w || g_shh != h || g_shdpi != g.dpi || g_shL != keyL) {
        int rad = max(2, (int)(6 * g.dpi));
        int R = w - (int)(8 * g.dpi);                  // the card's right edge: the 8px window gutter
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        BYTE* px = nullptr;
        HBITMAP bm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
        if (px && bm) {
            const double sig = 8.0 * g.dpi, depth = 5.0 * g.dpi;
            // the card spans: the one unsplit card, or pane A and pane B's
            double sp[2][2] = { { (double)L, (double)R }, { 0, 0 } };
            int spans = 1;
            if (g.split) {
                sp[0][1] = vx0 + g.splitX - (int)(4 * g.dpi);       // pane A's card ends at its shadow room
                sp[1][0] = vx0 + g.splitX + (int)(4 * g.dpi) + (int)(4 * g.dpi);   // pane B's begins past its own
                sp[1][1] = (double)R;
                spans = 2;
            }
            for (int y = 0; y < h; y++) {
                double ay = 0.6 * Phi((depth - y) / sig);          // the card's shadow, 5px down
                double c = y < rad ? rad - std::sqrt((double)rad * rad - (double)(rad - y) * (rad - y)) : 0.0;
                BYTE* row = px + (size_t)y * w * 4;
                for (int x = 0; x < w; x++) {
                    double a = 0;
                    for (int k = 0; k < spans; k++) {   // each card's falloff, corners pulled in
                        double lEdge = sp[k][0] + c - 2.0, rEdge = sp[k][1] - c + 2.0;
                        a = max(a, ay * Phi((x - lEdge) / sig) * Phi((rEdge - x) / sig));
                    }
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
    for (int i = 0; i < 8; i++) {
        RECT it{ 1, 1 + i * g_zoomItemH, r.right - 1, 1 + (i + 1) * g_zoomItemH };
        if (i == g.zoomHot) FillRect(dc, &it, PopHoverBrush());
        RECT tx = it; tx.left += (int)(8 * g.dpi);
        StText(dc, tx, ZoomLabel(i), DT_LEFT, StCombo(), 10 * g.dpi, StCapU,
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
    if (!g.status || !DVF().n) return;
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
        EnsureZoomBack(dc, r.right, r.bottom);   // buffered like every hover surface: the
        ZoomPopDraw(g_zdc, r.right, r.bottom);   // items are buttons and a direct paint
        BitBlt(dc, 0, 0, r.right, r.bottom, g_zdc, 0, 0, SRCCOPY);   // strobes on a flip
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
        Gdiplus::SolidBrush bg(Gdiplus::Color(255, 0x1c, 0x1c, 0x1c));
        gs.FillPath(&bg, inner);
        Gdiplus::GraphicsPath band;
        band.AddPath(outer, FALSE);
        band.AddPath(inner, FALSE);
        band.SetFillMode(Gdiplus::FillModeAlternate);
        Gdiplus::SolidBrush ring(Gdiplus::Color(255, 0x4a, 0x4a, 0x4a));   // CardBorderBrush (Dark)
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
        Gdiplus::SolidBrush dot(Gdiplus::Color(255, 0xa3, 0xa3, 0xa3));   // MutedTextBrush
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
                Gdiplus::LinearGradientBrush gb(gr, Gdiplus::Color(255, 0x38, 0x75, 0x52),
                                                   Gdiplus::Color(255, 0x10, 0x4e, 0x2b), 90.0f);
                gs.FillPath(&gb, &p);
            } else {
                Gdiplus::SolidBrush f(Gdiplus::Color(255, 0x40, 0x40, 0x40));   // RowHoverBrush
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
            Gdiplus::SolidBrush f(Gdiplus::Color(255, 0x40, 0x40, 0x40));   // RowHoverBrush
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

// ---------------- the sidebar rail: tools + Split pane / Pages / Document Info ----------------
// KillerPDF's SidebarToggleStrip, in docsy's container language: a 24px strip
// that always sits between the sidebar and the page (the sidebar collapses TO
// it, so a closed sidebar comes back from the same place), the shared grain
// field, a 1px seam against the view, a TOP-anchored tool stack and a
// bottom-anchored reference stack. The tools are the six edit buttons the
// screenshot asked for, in KillerPDF's own toolbar order - Select, Text,
// Highlight, Line, Shapes, Draw - with their ToolbarButton styling: hover
// paints the RowHoverBrush #404040 rounded face, the active tool wears the
// SelectionBg gradient with the white glyph, the rest ride the shadowed glyph
// at rest. The reference trio keeps the locked bottom order - Split pane,
// Pages, Document Info. Each vector icon reads at 16px and never depends on a
// glyph being present in the OS font.

enum { RB_SPLIT, RB_PAGES, RB_INFO, RB_COUNT };
enum { RT_SELECT = 100, RT_TEXT, RT_HIGHLIGHT, RT_LINE, RT_SHAPE, RT_DRAW, RT_COUNT_ };
static int RailId(int tool) { return RT_SELECT + tool; }   // tool index -> rail id
static int RailTool(int id) { return id >= RT_SELECT ? id - RT_SELECT : -1; }

static int RW() { return g.full ? 0 : (int)(24 * g.dpi); }   // full screen gives the column to the page

static void RailRects(RECT r[RB_COUNT], int h) {   // 24px cells, 6px gaps, 6px off the bottom
    int bw = (int)(24 * g.dpi), gap = (int)(6 * g.dpi);
    int x = max(0, (RW() - bw) / 2);
    int y = max(0, h - (int)(6 * g.dpi) - RB_COUNT * bw - (RB_COUNT - 1) * gap);
    for (int i = 0; i < RB_COUNT; i++) {
        r[i] = { x, y, x + bw, y + bw };
        y += bw + gap;
    }
}

// The tool stack hangs from the TOP edge (6px in), one 24px cell per tool in
// the toolbar's order, so the pointer reaches Select - the resting tool -
// without crossing the reference trio's zone.
static void ToolRects(RECT r[TOOL_COUNT], int h) {
    int bw = (int)(24 * g.dpi), gap = (int)(6 * g.dpi);
    int x = max(0, (RW() - bw) / 2);
    int y = (int)(6 * g.dpi);
    for (int i = 0; i < TOOL_COUNT; i++) {
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
    if (id >= RT_SELECT) {                              // a tool is lit while ITS pane is the
        if (!DVF().n) return false;                     // focused one and holds the tool -
        return DVF().tool == RailTool(id);              // KillerPDF's toolbar mirrors the
    }                                                   // active viewer's CurrentTool
    return id == RB_SPLIT ? g.split
         : id == RB_PAGES ? (g.sideOn && DVF().n)
         : false;
}
static bool RailEnabled(int id) {
    if (id >= RT_SELECT) return true;                   // KillerPDF's tool buttons are always
    return id == RB_SPLIT ? true : (DVF().n > 0);       // live; a no-document click re-lands
}                                                       // on Select inside SetTool

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
    // the tools: the toolbar's own glyphs at the ToolbarButton's FontSize 14
    static const wchar_t* tools[TOOL_COUNT] = {
        L"\uE8B0",   // Select  (ToolSelectBtn)
        L"\uE8D2",   // Text    (ToolTextBtn)
        L"\uE7E6",   // Highlight (ToolHighlightBtn)
        L"\uE738",   // Line    (ToolUnderlineBtn - the old Underline button IS the Line tool)
        L"\uE739",   // Shapes  (ToolShapeBtn)
        L"\uED63",   // Draw    (ToolDrawBtn)
    };
    int tool = RailTool(id);
    const wchar_t* glyph;
    int px;
    if (tool >= 0) { glyph = tools[tool]; px = 14; }
    else {
        const Spec& sp = specs[id];
        glyph = (id == RB_SPLIT && g.split) ? sp.open : sp.closed;
        px = id == RB_SPLIT ? 14 : 15;
    }
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, CaptionFont(px == 14 ? f14 : f15, px, false, L"Segoe MDL2 Assets"));
    RECT in; InkCenterRect(dc, rc, glyph, in);   // GlyphInk.Center's shift, both passes
    if (shadow) {                           // the shadow pass: depth 2, straight down -
        RECT sh = in;                       // the selected tool drops it (ToolbarButton's
        OffsetRect(&sh, 0, (int)std::lround(2 * g.dpi));   // Tag=selected trigger clears
        SetTextColor(dc, RGB(16, 16, 16));                 // the ContentPresenter effect)
        DrawTextW(dc, glyph, -1, &sh, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(dc, c);
    DrawTextW(dc, glyph, -1, &in, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
    RECT ts[TOOL_COUNT]; ToolRects(ts, h);
    for (int t = 0; t < TOOL_COUNT; t++) {
        int id = RailId(t);
        bool en = RailEnabled(id), on = en && RailOn(id), hot = en && g.railHot == id;
        if (on) {
            // the active tool wears KillerPDF's ToolbarButton selected state:
            // the SelectionBg gradient face and the white glyph, the icon
            // shadow dropped (the Tag=selected trigger clears the effect)
            Gdiplus::Graphics gs(dc);
            Gdiplus::LinearGradientBrush gb(Gdiplus::Rect(ts[t].left, ts[t].top - 1, ts[t].right - ts[t].left, ts[t].bottom - ts[t].top + 2),
                                            Gdiplus::Color(255, 0x38, 0x75, 0x52),
                                            Gdiplus::Color(255, 0x10, 0x4e, 0x2b), 90.0f);
            Gdiplus::GraphicsPath p;
            int rad = (int)(4 * g.dpi);
            p.AddArc((REAL)ts[t].left, (REAL)ts[t].top, 2.0f * rad, 2.0f * rad, 180, 90);
            p.AddArc((REAL)(ts[t].right - 2 * rad), (REAL)ts[t].top, 2.0f * rad, 2.0f * rad, 270, 90);
            p.AddArc((REAL)(ts[t].right - 2 * rad), (REAL)(ts[t].bottom - 2 * rad), 2.0f * rad, 2.0f * rad, 0, 90);
            p.AddArc((REAL)ts[t].left, (REAL)(ts[t].bottom - 2 * rad), 2.0f * rad, 2.0f * rad, 90, 90);
            p.CloseFigure();
            gs.FillPath(&gb, &p);
            RailIcon(dc, id, ts[t], RGB(0xff, 0xff, 0xff), false);   // white, no shadow pass
            continue;
        }
        if (hot) {                          // ToolbarButton's IsMouseOver trigger: the
            Gdiplus::Graphics gs(dc);       // RowHoverBrush #404040 rounded face under
            Gdiplus::GraphicsPath p;        // the unlit glyph
            int rad = (int)(4 * g.dpi);
            p.AddArc((REAL)ts[t].left, (REAL)ts[t].top, 2.0f * rad, 2.0f * rad, 180, 90);
            p.AddArc((REAL)(ts[t].right - 2 * rad), (REAL)ts[t].top, 2.0f * rad, 2.0f * rad, 270, 90);
            p.AddArc((REAL)(ts[t].right - 2 * rad), (REAL)(ts[t].bottom - 2 * rad), 2.0f * rad, 2.0f * rad, 0, 90);
            p.AddArc((REAL)ts[t].left, (REAL)(ts[t].bottom - 2 * rad), 2.0f * rad, 2.0f * rad, 90, 90);
            p.CloseFigure();
            Gdiplus::SolidBrush hb(Gdiplus::Color(255, 0x40, 0x40, 0x40));
            gs.FillPath(&hb, &p);
        }
        RailIcon(dc, id, ts[t], RGB(0xe0, 0xe0, 0xe0));
    }
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
}

static void RailRepaint() { if (g.rail) InvalidateRect(g.rail, nullptr, FALSE); }

// One hover/click map for both stacks: the tool group first, the reference
// trio second (their zones never overlap - one hangs from the top, one from
// the bottom). Returns the coded rail id, -1 for nothing.
static int RailHitAt(POINT p, int hh) {
    RECT ts[TOOL_COUNT]; ToolRects(ts, hh);
    for (int t = 0; t < TOOL_COUNT; t++) if (PtInRect(&ts[t], p)) return RailId(t);
    RECT bs[RB_COUNT]; RailRects(bs, hh);
    for (int i = 0; i < RB_COUNT; i++) if (PtInRect(&bs[i], p)) return i;
    return -1;
}

static void RailActivate(int id) {
    if (id >= RT_SELECT) { SetTool(RailTool(id), FocusB()); return; }   // KillerPDF's ToolX_Click
    if (id == RB_SPLIT) ToggleSplit();      // = SetTool on the ACTIVE viewer; docsy's rail
    else if (id == RB_PAGES) ToggleSide();  // serves the focused pane
    else InfoOpen();                        // KillerPDF's DocInfo_Click: the button stays
}                                           // enabled; a no-document click is told to the reader

// the SetTool behind the tool stack: the tool lands on ONE pane's document
// state (KillerPDF keeps the editing state per viewer instance), a no-document
// pane re-lands on Select, a live text box commits first, and the view's
// cursor + brush ring follow on the repaint.
static void SetTool(int t, bool b) {
    if (t < 0 || t >= TOOL_COUNT) return;
    DocState& D = DV(b);
    if (!D.n) t = TOOL_SELECT;              // KillerPDF's SetTool: no document, no editing
    CommitTextEdit();                       // an open box lands with the tool switch
    if (D.tool == t) { RailRepaint(); return; }   // re-click: the tool stays (its bar-tuck
    D.tool = t;                             // toggle rides the settings bar we don't carry)
    if (g.annSelPage >= 0) { g.annSelPage = -1; g.annSelIdx = -1; }   // the ring belongs to Select
    RailRepaint();
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);   // cursors and ring states ride the
}                                                         // tool; the switch is a rare click

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
            sd.th.clear(); sd.scroll = 0; sd.cur = -1; sd.hold = 0; sd.cw = 0; sd.ch = 0;
        }
        SideLayout();
        UpdateTitle();
        StatusRepaint();
        LayoutMain();
    }
    RailRepaint();
    InvalidateRect(g.view, nullptr, FALSE);
}

// ---------------- the Document Info dialog ----------------
// KillerPDF's DocumentInfoDialog (F4), in docsy's chrome: a real modal window
// owned by the main one, the find bar's container language for the body - the
// soft DropShadow, the radius-6 grain panel, the #555555 ring - its caption
// "Docsy - Document Info", the field list Title / Author / Subject / Keywords
// / Creator and the mono summary block (Producer when the file has one,
// Pages, the PDF version off the header, the file's KB). docsy is a reader:
// the fields read, they do not edit - there is no Save pair, the dialog
// closes with Close, Enter or Esc.

// ---- the PDF's own Document Info dictionary ----
// The same source KillerPDF's engine reads: the trailer's /Info reference is
// plain text in every PDF (classic trailer dictionaries and xref-stream
// dictionaries alike), the object's strings are decoded literal or hex, and
// an encrypted file - whose strings are ciphered - answers empty fields
// instead of garbage. Best effort by design: a file with no Info dictionary,
// or one this reader cannot reach, simply shows "-" per field while the
// summary's Pages / version / KB stay exact (they never come from the dict).
struct PdfInfo {
    std::wstring title, author, subject, keywords, creator, producer;
    std::wstring version;               // "1.7" - straight off the "%PDF-" header
};

static std::wstring PdfDecodeString(const std::wstring& raw, bool hex) {
    std::string bytes;
    if (hex) {
        int nib = -1;
        for (wchar_t c : raw) {
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) continue;
            if (nib < 0) nib = v;
            else { bytes.push_back((char)(nib * 16 + v)); nib = -1; }
        }
        if (nib >= 0) bytes.push_back((char)(nib * 16));   // odd digit count pads 0
    } else {
        for (size_t i = 0; i < raw.size(); i++) {
            wchar_t c = raw[i];
            if (c == L'\\' && i + 1 < raw.size()) {
                wchar_t e = raw[++i];
                switch (e) {
                case L'n': bytes.push_back('\n'); break;
                case L'r': bytes.push_back('\r'); break;
                case L't': bytes.push_back('\t'); break;
                case L'b': bytes.push_back('\b'); break;
                case L'f': bytes.push_back('\f'); break;
                default:
                    if (e >= L'0' && e <= L'7') {           // up to three octal digits
                        int v = e - L'0', k = 0;
                        while (k < 2 && i + 1 < raw.size() && raw[i + 1] >= L'0' && raw[i + 1] <= L'7')
                            { v = v * 8 + (raw[++i] - L'0'); k++; }
                        bytes.push_back((char)v);
                    } else bytes.push_back((char)e);        // \\ ( ) and anything else: the char itself
                    break;
                }
            } else if (c >= 0 && c <= 255) bytes.push_back((char)c);
        }
    }
    if (bytes.size() >= 2 && (BYTE)bytes[0] == 0xFE && (BYTE)bytes[1] == 0xFF) {
        std::wstring out;                                    // UTF-16BE, the BOM says so
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            out.push_back((wchar_t)((BYTE)bytes[i] << 8 | (BYTE)bytes[i + 1]));
        return out;
    }
    if (bytes.size() >= 3 && (BYTE)bytes[0] == 0xEF && (BYTE)bytes[1] == 0xBB && (BYTE)bytes[2] == 0xBF) {
        return std::wstring(bytes.begin() + 3, bytes.end()); // UTF-8 without re-encoding: the
    }                                                        // subsets this dialog shows are ascii
    std::wstring out;                                        // PDFDocEncoding: latin-1 covers it
    for (unsigned char c : bytes) out.push_back((wchar_t)c);
    return out;
}

static bool PdfInfoRead(const std::wstring& path, PdfInfo& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    long long size = sz.QuadPart;
    char head[16]{};
    DWORD got = 0;
    ReadFile(f, head, 15, &got, nullptr);
    if (got >= 8 && !memcmp(head, "%PDF-", 5)) {             // the version is the header's
        const char* v = head + 5;
        std::wstring ver;
        while ((*v >= '0' && *v <= '9') || *v == '.') ver.push_back(*v++);
        out.version = ver;
    }
    // the tail holds the trailer: the last /Info reference and any /Encrypt mark
    long long tailOff = max(0LL, size - 16384);
    DWORD tailLen = (DWORD)(size - tailOff);
    std::string tail((size_t)tailLen, '\0');
    SetFilePointer(f, (LONG)tailOff, nullptr, FILE_BEGIN);
    if (!ReadFile(f, tail.data(), tailLen, &got, nullptr)) { CloseHandle(f); return false; }
    tail.resize(got);
    CloseHandle(f);
    if (tail.find("/Encrypt") != std::string::npos) return true;   // ciphered strings: fields stay empty
    size_t info = tail.rfind("/Info");
    if (info == std::string::npos) return out.version.size() > 0;
    long long objNum = -1;
    {   // "/Info N G R" - the object number follows the key
        size_t p = info + 5;
        while (p < tail.size() && (tail[p] == ' ' || tail[p] == '\t' || tail[p] == '\r' || tail[p] == '\n')) p++;
        objNum = _atoi64(tail.c_str() + p);
    }
    if (objNum < 0 || objNum > 99999999) return out.version.size() > 0;
    // the object itself: scanned from the file's start - "N 0 obj" is plain
    // text everywhere it is real; a stream's binary noise can imitate one, so
    // each candidate is validated by its keys and the next one tried
    HANDLE f2 = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f2 == INVALID_HANDLE_VALUE) return out.version.size() > 0;
    std::string pat = std::to_string(objNum) + " 0 obj";
    long long probe = 0;
    const long long WINDOW = 1 << 20;
    for (int attempt = 0; attempt < 8 && probe < size; attempt++) {
        long long span = min((long long)tailOff + (long long)tail.size() - probe, (long long)(WINDOW + pat.size() * 2));
        std::string buf((size_t)max(0LL, span), '\0');
        DWORD rd = 0;
        SetFilePointer(f2, (LONG)probe, nullptr, FILE_BEGIN);
        if (!ReadFile(f2, buf.data(), (DWORD)buf.size(), &rd, nullptr) || rd < pat.size()) break;
        buf.resize(rd);
        size_t at = buf.find(pat);
        if (at == std::string::npos) { probe += rd - (long long)pat.size() + 1; continue; }
        size_t obj = at;
        size_t dict = buf.find("<<", obj);
        size_t end = buf.find("endobj", obj);
        if (dict == std::string::npos || end == std::string::npos) { probe += obj + pat.size(); continue; }
        std::string dictText = buf.substr(dict, min(end - dict, (size_t)65536));
        // the dict must mention at least one Info key, else this "N 0 obj" was noise
        if (dictText.find("/Title") == std::string::npos && dictText.find("/Author") == std::string::npos &&
            dictText.find("/Subject") == std::string::npos && dictText.find("/Keywords") == std::string::npos &&
            dictText.find("/Creator") == std::string::npos && dictText.find("/Producer") == std::string::npos) {
            probe += obj + pat.size();
            continue;
        }
        auto value = [&](const char* key) -> std::wstring {
            size_t k = dictText.find(key);
            if (k == std::string::npos) return L"";
            size_t p = k + strlen(key);
            while (p < dictText.size() && isspace((unsigned char)dictText[p])) p++;
            if (p >= dictText.size()) return L"";
            if (dictText[p] == '(') {                       // literal string, balanced parens
                int depth = 1; size_t q = p + 1; std::wstring raw;
                while (q < dictText.size() && depth > 0) {
                    char c = dictText[q];
                    if (c == '\\' && q + 1 < dictText.size()) { raw += dictText[q]; raw += dictText[q + 1]; q += 2; continue; }
                    if (c == '(') depth++;
                    if (c == ')') { depth--; if (!depth) break; }
                    raw += c; q++;
                }
                return PdfDecodeString(std::wstring(raw.begin(), raw.end()), false);
            }
            if (dictText[p] == '<' && p + 1 < dictText.size() && dictText[p + 1] != '<') {
                size_t e = dictText.find('>', p);
                if (e == std::string::npos) return L"";
                return PdfDecodeString(std::wstring(dictText.begin() + p + 1, dictText.begin() + e), true);
            }
            return L"";                                     // names/refs/numbers: not shown
        };
        out.title    = value("/Title");
        out.author   = value("/Author");
        out.subject  = value("/Subject");
        out.keywords = value("/Keywords");
        out.creator  = value("/Creator");
        out.producer = value("/Producer");
        return true;
    }
    return out.version.size() > 0;
}

// Save edited metadata into the PDF the way KillerPDF's save does - an
// incremental update: the original bytes are never touched, the new /Info
// object plus a fresh xref table and trailer are appended, and readers take
// the newest section (a classic trailer after an xref-stream update is legal
// mixing). The Info object keeps its existing number when the file has one -
// the new xref entry overrides the old - otherwise it takes /Size and the
// size grows by one. Strings go out as PDFDocEncoding literals when they are
// ASCII and UTF-16BE hex strings when they are not, escaped per the spec.
// A file whose trailer cannot be parsed, or an encrypted one (its strings
// are ciphered), is refused with a message instead of being damaged.
static bool PdfInfoWrite(const std::wstring& path, const PdfInfo& in, std::wstring& err) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"The file could not be opened for writing."; return false; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    long long size = sz.QuadPart;
    long long tailOff = max(0LL, size - 16384);
    std::string tail((size_t)(size - tailOff), '\0');
    DWORD got = 0;
    SetFilePointer(f, (LONG)tailOff, nullptr, FILE_BEGIN);
    if (!ReadFile(f, tail.data(), (DWORD)tail.size(), &got, nullptr)) {
        CloseHandle(f);
        err = L"The file could not be read.";
        return false;
    }
    tail.resize(got);
    if (tail.find("/Encrypt") != std::string::npos) {
        CloseHandle(f);
        err = L"Encrypted PDFs stay read-only here.";
        return false;
    }
    // the keys the appended trailer must carry forward: /Root and /Size come
    // from a classic trailer, or - in an xref-stream file - from the xref
    // object the last startxref points at
    auto numAfter = [&](const std::string& hay, size_t at) -> long long {
        size_t p = at;
        while (p < hay.size() && (hay[p] == ' ' || hay[p] == '\t' || hay[p] == '\r' || hay[p] == '\n')) p++;
        return _atoi64(hay.c_str() + p);
    };
    size_t sx = tail.rfind("startxref");
    if (sx == std::string::npos) { CloseHandle(f); err = L"This PDF's structure could not be updated."; return false; }
    long long prev = numAfter(tail, sx + 9);
    long long rootObj = -1, rootGen = 0, sizeVal = -1, infoObj = -1;
    std::string td;                                       // trailer dict text, either flavor
    size_t tr = tail.rfind("trailer");
    if (tr != std::string::npos) td = tail.substr(tr, min(tail.size() - tr, (size_t)2048));
    if (td.find("/Root") == std::string::npos && prev > 0 && prev < size) {
        std::string xb(2048, '\0'); DWORD xr = 0;
        SetFilePointer(f, (LONG)prev, nullptr, FILE_BEGIN);
        if (ReadFile(f, xb.data(), (DWORD)xb.size(), &xr, nullptr)) {
            xb.resize(xr);
            size_t d = xb.find("<<");
            if (d != std::string::npos) td = xb.substr(d, min(xb.size() - d, (size_t)2048));
        }
    }
    CloseHandle(f);
    size_t k = td.find("/Root");
    if (k == std::string::npos) { err = L"This PDF's structure could not be updated."; return false; }
    rootObj = numAfter(td, k + 5);
    {   // the generation is the second integer of the reference
        const char* q = td.c_str() + k + 5;
        while (*q == ' ') q++;
        while (*q >= '0' && *q <= '9') q++;
        while (*q == ' ') q++;
        rootGen = _atoi64(q);
    }
    k = td.find("/Size");
    if (k != std::string::npos) sizeVal = numAfter(td, k + 5);
    k = td.find("/Info");
    if (k != std::string::npos) infoObj = numAfter(td, k + 5);
    if (rootObj <= 0 || rootObj > 99999999 || sizeVal <= 0 || sizeVal > 99999999) {
        err = L"This PDF's structure could not be updated.";
        return false;
    }
    long long obj = infoObj > 0 ? infoObj : sizeVal;
    long long newSize = infoObj > 0 ? sizeVal : sizeVal + 1;
    // the strings: ASCII (with escapes) as a literal, anything else UTF-16BE hex
    auto pdfstr = [](const std::wstring& s) -> std::string {
        bool ascii = true;
        for (wchar_t c : s) if (c < 32 || c > 126) { ascii = false; break; }
        std::string out;
        if (ascii) {
            out += '(';
            for (wchar_t c : s) {
                if (c == L'(' || c == L')' || c == L'\\') out += '\\';
                if (c == L'\r') { out += "\\r"; continue; }
                if (c == L'\n') { out += "\\n"; continue; }
                out += (char)c;
            }
            out += ')';
            return out;
        }
        out += "<FEFF";
        const char* hx = "0123456789ABCDEF";
        for (wchar_t c : s) {
            out += hx[(c >> 12) & 15]; out += hx[(c >> 8) & 15];
            out += hx[(c >> 4) & 15];  out += hx[c & 15];
        }
        out += '>';
        return out;
    };
    std::string body = "\r\n";
    long long objOff = size + (long long)body.size();
    body += std::to_string(obj) + " 0 obj\n<< ";
    auto add = [&](const char* key, const std::wstring& v) {
        if (!v.empty()) body += std::string(key) + " " + pdfstr(v) + " ";
    };
    add("/Title", in.title);       add("/Author", in.author);
    add("/Subject", in.subject);   add("/Keywords", in.keywords);
    add("/Creator", in.creator);   add("/Producer", in.producer);
    body += ">>\nendobj\r\n";
    long long xrefOff = objOff + (long long)body.size() - 2;
    char ent[32];
    snprintf(ent, sizeof ent, "%010lld 00000 n \n", objOff);
    body += std::string("xref\n") + std::to_string(obj) + " 1\n" + ent;
    body += "trailer\n<< /Size " + std::to_string(newSize) +
            " /Root " + std::to_string(rootObj) + " " + std::to_string(rootGen) + " R" +
            " /Info " + std::to_string(obj) + " 0 R";
    if (prev > 0 && prev < size) body += " /Prev " + std::to_string(prev);
    body += " >>\nstartxref\n" + std::to_string(xrefOff) + "\n%%EOF\n";
    HANDLE w = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) {
        err = L"The file could not be opened for writing.";
        return false;
    }
    SetFilePointer(w, (LONG)size, nullptr, FILE_BEGIN);
    DWORD wrote = 0;
    bool ok = WriteFile(w, body.data(), (DWORD)body.size(), &wrote, nullptr) && wrote == body.size();
    CloseHandle(w);
    if (!ok) { err = L"The file could not be written."; return false; }
    // verify by reading back, so "saved" never lies
    PdfInfo back;
    PdfInfoRead(path, back);
    if (back.title != in.title || back.author != in.author || back.subject != in.subject ||
        back.keywords != in.keywords || back.creator != in.creator) {
        err = L"The update could not be verified.";
        return false;
    }
    return true;
}

// ---- the dialog itself ----
// KillerPDF's DocumentInfoDialog, in docsy's chrome. The reference's shape:
// a 460-wide window sized to its content, a caption that DRAGS the window,
// five EDITABLE fields (Title / Author / Subject / Keywords / Creator - real
// text boxes with a caret and a selection, Keywords wrapping like the
// reference's wrap:true field), the mono summary block (Producer when the
// file has one, Pages, the PDF version off the header, the file's KB) and a
// right-aligned Cancel / Save pair - Esc cancels, Enter saves. The window is
// owned, not app-modal: the main window stays live, a click anywhere outside
// the dialog dismisses it (the reference's dialog is draggable chrome; the
// click-outside dismissal is the popup contract this port follows), and Save
// writes the metadata into the PDF itself - the reference applies the edit
// to the file, so the words must survive the dialog.
struct InfoDlg {
    std::wstring title, author, subject, keywords, creator, producer, ver;
    long long bytes = 0;
    int pages = 0;
    int hot = -1;                        // hovered control: 0 Cancel, 1 Save, 2 the caption close
};
static InfoDlg g_idlg;
static HWND g_infoDlg = nullptr;
static HWND g_idEdits[5] = {};       // the five live fields (nullptr in the CI shot host)
static bool g_idlgShots = false;     // the CI shot renders the dialog's paint without the
                                     // live window - one paint path, two hosts

static bool InfoIsOpen() { return g_infoDlg != nullptr; }

static HFONT g_idCap = nullptr, g_idLab = nullptr, g_idVal = nullptr, g_idMono = nullptr;
static HFONT IdCapFont()  { return CaptionFont(g_idCap, 14, false, L"Killer Sans"); }
static HFONT IdLabFont()  { return CaptionFont(g_idLab, 11, false, L"Killer Sans UI"); }
static HFONT IdValFont()  { return CaptionFont(g_idVal, 12, false, L"Killer Sans UI"); }
static HFONT IdMonoFont() { return CaptionFont(g_idMono, 11, false, L"Consolas"); }

static int IdW() { return (int)(460 * g.dpi); }          // KillerPDF's Width=460
static int IdCapH() { return (int)(32 * g.dpi); }

// the dialog's natural height from its own metrics, so SizeToContent.Height's
// behavior is arithmetic, not a magic number
static int IdFieldH(int i) { return (int)((i == 3 ? 44 : 26) * g.dpi); }   // Keywords wraps
static int IdH() {
    int labH = (int)(14 * g.dpi), gap = (int)(8 * g.dpi);
    int fields = 0;
    for (int i = 0; i < 5; i++) fields += labH + (int)(2 * g.dpi) + IdFieldH(i) + gap;
    int sumH = (int)(15 * g.dpi) * 4;
    int btnH = (int)(28 * g.dpi);
    return IdCapH() + (int)(6 * g.dpi) + fields + (int)(12 * g.dpi) + sumH +
           (int)(16 * g.dpi) + btnH + (int)(16 * g.dpi);
}

// Each field's label and box rect, in dialog client coords - shared by the
// paint, the edit placement and the shot seed. KillerPDF's stack: GroupLabel
// (11px, 2px under), the field, 8px after.
static void IdFieldRect(int i, int w, RECT& lab, RECT& box) {
    int pad = (int)(20 * g.dpi);
    int y = IdCapH() + (int)(6 * g.dpi);
    for (int k = 0; k < i; k++)
        y += (int)(14 * g.dpi) + (int)(2 * g.dpi) + IdFieldH(k) + (int)(8 * g.dpi);
    lab = { pad, y, w - pad, y + (int)(14 * g.dpi) };
    box = { pad, y + (int)(14 * g.dpi) + (int)(2 * g.dpi), w - pad,
            y + (int)(14 * g.dpi) + (int)(2 * g.dpi) + IdFieldH(i) };
}

// The button pair, right-aligned with an 8px gap (UiKit.ButtonRow): which 0 =
// Cancel, 1 = Save (the accent, rightmost - the reference's IsDefault).
static RECT IdBtnRect(int which, int w, int h) {
    int saveW = (int)(64 * g.dpi), cancelW = (int)(84 * g.dpi), bh = (int)(28 * g.dpi);
    int right = w - (int)(20 * g.dpi) - (which ? 0 : saveW + (int)(8 * g.dpi));
    return { right - (which ? saveW : cancelW), h - (int)(16 * g.dpi) - bh,
             right, h - (int)(16 * g.dpi) };
}

// One paint, two hosts: the live dialog's WM_PAINT and the CI shot composite.
// The field boxes are painted here; the text inside them belongs to the live
// EDIT children - values are drawn only when there are no edits (the shot
// host), so the two never double-render a string.
static void InfoDialogDraw(HDC dc, int w, int h) {
    int capH = IdCapH();
    // the caption row: DialogFrame's bar - the wordmark, the qualifier, the
    // red-hovered close. The whole bar drags the window (WM_NCHITTEST below).
    RECT cr{ 0, 0, w, capH };
    FillRect(dc, &cr, GrainBrush(2));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, IdCapFont());
    RECT tr{ (int)(12 * g.dpi), 0, w - (int)(44 * g.dpi), capH };
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
    DrawTextW(dc, L"Docsy", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SIZE ts{};
    GetTextExtentPoint32W(dc, L"Docsy", 5, &ts);
    RECT qr{ tr.left + ts.cx + (int)(4 * g.dpi), tr.top, tr.right, tr.bottom };
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
    DrawTextW(dc, L"- Document Info", -1, &qr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    int bs = (int)(44 * g.dpi);
    RECT xr{ w - bs, 0, w, capH };
    if (g_idlg.hot == 2) { RECT hr{ xr.left, 0, xr.right, capH }; FillRect(dc, &hr, RowHoverBrush()); }
    SelectObject(dc, StGlyph());
    SetTextColor(dc, g_idlg.hot == 2 ? RGB(0xff, 0x67, 0x67) : RGB(0xa3, 0xa3, 0xa3));
    DrawTextW(dc, L"\xE8BB", -1, &xr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // the body field: the shared grain
    RECT body{ 0, capH, w, h };
    FillRect(dc, &body, GrainBrush(1));
    SetBkMode(dc, TRANSPARENT);
    const wchar_t* labels[5] = { L"Title", L"Author", L"Subject", L"Keywords", L"Creator" };
    const std::wstring* vals[5] = { &g_idlg.title, &g_idlg.author, &g_idlg.subject,
                                    &g_idlg.keywords, &g_idlg.creator };
    bool live = g_infoDlg != nullptr;
    // the field boxes: KillerPDF's UiKit.Field - TextFieldBrush #101010 inside
    // a 1px #4a4a4a CardBorder, radius 4
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    auto boxPath = [&](const RECT& rc) {
        float rr = (float)(4 * g.dpi);
        float fx = (float)rc.left, fy = (float)rc.top;
        float fw = (float)(rc.right - rc.left), fh = (float)(rc.bottom - rc.top);
        Gdiplus::GraphicsPath* pth = new Gdiplus::GraphicsPath();
        pth->AddArc(fx, fy, 2 * rr, 2 * rr, 180, 90);
        pth->AddArc(fx + fw - 2 * rr, fy, 2 * rr, 2 * rr, 270, 90);
        pth->AddArc(fx + fw - 2 * rr, fy + fh - 2 * rr, 2 * rr, 2 * rr, 0, 90);
        pth->AddArc(fx, fy + fh - 2 * rr, 2 * rr, 2 * rr, 90, 90);
        pth->CloseFigure();
        return pth;
    };
    int lastBoxBottom = 0;
    for (int i = 0; i < 5; i++) {
        RECT lab, box;
        IdFieldRect(i, w, lab, box);
        lastBoxBottom = box.bottom;
        SelectObject(dc, IdLabFont());
        SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
        DrawTextW(dc, labels[i], -1, &lab, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        Gdiplus::GraphicsPath* pth = boxPath(box);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 0x10, 0x10, 0x10));
        gs.FillPath(&fill, pth);
        Gdiplus::Pen edge(Gdiplus::Color(255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
        gs.DrawPath(&edge, pth);
        delete pth;
        if (!live) {                       // the shot host speaks for the edits
            RECT vr{ box.left + (int)(6 * g.dpi), box.top + (int)(2 * g.dpi),
                     box.right - (int)(6 * g.dpi), box.bottom - (int)(2 * g.dpi) };
            SelectObject(dc, IdValFont());
            SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
            SetBkColor(dc, RGB(0x10, 0x10, 0x10));
            DrawTextW(dc, vals[i]->c_str(), -1, &vr,
                      DT_LEFT | (i == 3 ? DT_WORDBREAK | DT_EDITCONTROL : DT_VCENTER | DT_SINGLELINE) | DT_END_ELLIPSIS);
            SetBkMode(dc, TRANSPARENT);
        }
    }
    // the summary block, mono and muted - KillerPDF's BuildSummary
    std::wstring sum;
    if (!g_idlg.producer.empty()) sum += L"Producer: " + g_idlg.producer + L"\n";
    sum += L"Pages: " + std::to_wstring(g_idlg.pages) + L"\n";
    sum += L"PDF " + (g_idlg.ver.empty() ? std::wstring(L"1.?") : g_idlg.ver) + L"\n";
    wchar_t kb[40];
    if (g_idlg.bytes >= 1LL << 20) swprintf_s(kb, L"%.0f KB", g_idlg.bytes / 1024.0);
    else swprintf_s(kb, L"%lld KB", g_idlg.bytes / 1024);
    sum += kb;
    int sy = lastBoxBottom + (int)(12 * g.dpi);
    SelectObject(dc, IdMonoFont());
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
    RECT sr{ (int)(24 * g.dpi), sy, w - (int)(20 * g.dpi), sy + (int)(15 * g.dpi) * 4 };
    DrawTextW(dc, sum.c_str(), -1, &sr, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);
    // the pair, right-aligned: Cancel the secondary, Save the accent -
    // hollow in the accent at rest, filled with it on hover
    for (int wh = 0; wh < 2; wh++) {
        RECT br = IdBtnRect(wh, w, h);
        bool accent = wh == 1, hot = g_idlg.hot == wh;
        Gdiplus::GraphicsPath* pth = boxPath(br);
        if (accent && hot) {
            Gdiplus::SolidBrush gb(Gdiplus::Color(255, 0x22, 0xbb, 0x56));
            gs.FillPath(&gb, pth);
        } else if (accent) {
            Gdiplus::Pen pb(Gdiplus::Color(255, 0x22, 0xbb, 0x56), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else if (hot) {
            Gdiplus::SolidBrush hb(Gdiplus::Color(255, 0x40, 0x40, 0x40));
            gs.FillPath(&hb, pth);
            Gdiplus::Pen pb(Gdiplus::Color(255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else {
            Gdiplus::Pen pb(Gdiplus::Color(255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        }
        delete pth;
        SetTextColor(dc, accent ? (hot ? RGB(0x05, 0x24, 0x0f) : RGB(0x22, 0xbb, 0x56))
                                : RGB(0xe0, 0xe0, 0xe0));
        SelectObject(dc, IdValFont());
        DrawTextW(dc, accent ? L"Save" : L"Cancel", -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void InfoClose() {
    if (!g_infoDlg) return;
    HWND d = g_infoDlg;
    g_infoDlg = nullptr;
    for (HWND& e : g_idEdits) e = nullptr;
    DestroyWindow(d);
    RailRepaint();
    if (g.main) SetFocus(g.main);            // the keys go home with the dialog gone
}

// Save: gather the five fields and apply them the way KillerPDF's save does -
// the metadata goes into the file itself (an incremental update; the original
// bytes stay untouched). A failure reports in the footer and leaves the
// dialog open over an unchanged file.
static void InfoSave() {
    if (!g_infoDlg) return;
    DocState& D = DVF();
    if (!D.n) { InfoClose(); return; }
    wchar_t buf[4096]{};
    auto grab = [&](int i) {
        if (!g_idEdits[i]) return i == 0 ? g_idlg.title : i == 1 ? g_idlg.author
                              : i == 2 ? g_idlg.subject : i == 3 ? g_idlg.keywords : g_idlg.creator;
        GetWindowTextW(g_idEdits[i], buf, 4096);
        return std::wstring(buf);
    };
    PdfInfo meta;
    meta.title = grab(0);       meta.author = grab(1);
    meta.subject = grab(2);     meta.keywords = grab(3);
    meta.creator = grab(4);
    meta.producer = g_idlg.producer;         // the producer rides through unchanged
    meta.version = g_idlg.ver;
    std::wstring err;
    if (PdfInfoWrite(D.path, meta, err)) {
        InfoClose();
        SetStatusHold(L"Document Info updated.");
    } else {
        SetStatusHold(err.c_str());
    }
}

// The fields' keys: Enter saves, Esc cancels, Tab walks the five boxes -
// the reference's IsDefault / IsCancel / tab order, which a plain EDIT child
// would otherwise swallow.
static LRESULT CALLBACK IdEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    WNDPROC orig = (WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_KEYDOWN) {
        if (wp == VK_RETURN) { InfoSave(); return 0; }
        if (wp == VK_ESCAPE) { InfoClose(); return 0; }
        if (wp == VK_TAB) {
            int i = 0;
            while (i < 5 && g_idEdits[i] != h) i++;
            bool back = GetKeyState(VK_SHIFT) < 0;
            for (int step = 0; step < 5; step++) {
                i = (i + (back ? -1 : 1) + 5) % 5;
                if (g_idEdits[i]) { SetFocus(g_idEdits[i]); break; }
            }
            return 0;
        }
    }
    return orig ? CallWindowProcW(orig, h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}

static void InfoOpen() {
    if (g_infoDlg) { SetFocus(g_infoDlg); return; }
    DocState& D = DVF();                     // the FOCUSED pane's document, KillerPDF's active viewer
    if (!D.n) {                              // KillerPDF's Str_Msg_OpenFirst, in the footer's
        SetStatusHold(L"Open a PDF first."); // language (the app has no message boxes)
        return;
    }
    PdfInfo pi;
    g_idlg = InfoDlg{};
    PdfInfoRead(D.path, pi);
    g_idlg.title = pi.title;                 // raw values: the reference shows what the
    g_idlg.author = pi.author;               // dictionary holds, and empty stays empty
    g_idlg.subject = pi.subject;
    g_idlg.keywords = pi.keywords;
    g_idlg.creator = pi.creator;
    g_idlg.producer = pi.producer;
    g_idlg.ver = pi.version;
    g_idlg.pages = D.n;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    g_idlg.bytes = GetFileAttributesExW(D.path.c_str(), GetFileExInfoStandard, &fa)
                 ? (long long)(((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) : 0;
    // a borderless OWNED popup at the reference's Width=460: the main window
    // stays live - NOT app-modal - and a click outside dismisses the dialog
    // without saving (the popup contract; the reference's own dialog is
    // dragged by its caption and answered with Cancel or Save)
    int w = IdW(), h = IdH();
    RECT mr{}; GetWindowRect(g.main, &mr);
    int x = mr.left + max(0, (int)((mr.right - mr.left) - w) / 2);
    int y = mr.top + max(0, (int)((mr.bottom - mr.top) - h) / 2);
    g_infoDlg = CreateWindowExW(0, L"DocsyInfoDlg", nullptr,
                                WS_POPUP | WS_CLIPCHILDREN, x, y, w, h,
                                g.main, nullptr, g.hinst, nullptr);
    if (!g_infoDlg) return;
    // the five live fields, KillerPDF's UiKit.Field as a child EDIT: 12px
    // Killer Sans UI on #101010 (WM_CTLCOLOREDIT below), the reference's
    // 6,4 padding via the edit margins. Keywords is the wrap:true field:
    // multiline so long lists wrap, and Enter is never a newline - it saves.
    std::wstring init[5] = { pi.title, pi.author, pi.subject, pi.keywords, pi.creator };
    for (int i = 0; i < 5; i++) {
        RECT lab, box;
        IdFieldRect(i, w, lab, box);
        DWORD es = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT |
                   (i == 3 ? ES_MULTILINE : ES_AUTOHSCROLL);
        g_idEdits[i] = CreateWindowExW(0, L"EDIT", nullptr, es,
            box.left + 2, box.top + 2, box.right - box.left - 4, box.bottom - box.top - 4,
            g_infoDlg, (HMENU)(intptr_t)(100 + i), g.hinst, nullptr);
        if (g_idEdits[i]) {
            SetWindowLongPtrW(g_idEdits[i], GWLP_USERDATA,
                              GetWindowLongPtrW(g_idEdits[i], GWLP_WNDPROC));
            SetWindowLongPtrW(g_idEdits[i], GWLP_WNDPROC, (LONG_PTR)IdEditProc);
            SendMessageW(g_idEdits[i], WM_SETFONT, (WPARAM)IdValFont(), FALSE);
            SendMessageW(g_idEdits[i], EM_SETMARGIN, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM((int)(4 * g.dpi), (int)(4 * g.dpi)));
            SetWindowTextW(g_idEdits[i], init[i].c_str());
        }
    }
    DwmApply(g_infoDlg);
    ShowWindow(g_infoDlg, SW_SHOWNORMAL);
    if (g_idEdits[0]) SetFocus(g_idEdits[0]);   // the reference: Loaded focuses _title
    RailRepaint();
}

static LRESULT CALLBACK InfoDlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureInfoBack(dc, r.right, r.bottom);   // the same buffered present as the rail:
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // the dialog used to paint
            SetBrushOrgEx(g_iddc, -po.x, -po.y, nullptr);   // straight to the screen, so every
            InfoDialogDraw(g_iddc, r.right, r.bottom);   // hover flip over Cancel/Save/the close
            BitBlt(dc, 0, 0, r.right, r.bottom, g_iddc, 0, 0, SRCCOPY);   // strobed the window
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        // the popup contract: a click anywhere outside the dialog - the main
        // window, another app, the taskbar - dismisses it the way KillerPDF's
        // flyouts close on deactivation. Cancel semantics: what was typed is
        // simply not saved.
        if (LOWORD(wp) == WA_INACTIVE && g_infoDlg == h) InfoClose();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { InfoClose(); return 0; }   // IsCancel
        if (wp == VK_RETURN) { InfoSave(); return 0; }    // IsDefault
        return 0;
    case WM_CTLCOLOREDIT: {               // UiKit.Field: the text #e0e0e0 on #101010
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        SetBkColor(dc, RGB(0x10, 0x10, 0x10));
        static HBRUSH b = CreateSolidBrush(RGB(0x10, 0x10, 0x10));
        return (LRESULT)b;
    }
    case WM_NCHITTEST: {
        // the caption DRAGS the window (DialogFrame's DragMove); the close
        // zone stays client so its click is ours
        POINT s{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT c = s; ScreenToClient(h, &c);
        RECT r; GetClientRect(h, &r);
        if (c.y >= 0 && c.y < IdCapH() && c.x < r.right - (int)(44 * g.dpi)) return HTCAPTION;
        break;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        RECT b0 = IdBtnRect(0, r.right, r.bottom), b1 = IdBtnRect(1, r.right, r.bottom);
        int hot = -1;
        if (PtInRect(&b1, p)) hot = 1;
        else if (PtInRect(&b0, p)) hot = 0;
        else if (p.y < IdCapH() && p.x >= r.right - (int)(44 * g.dpi)) hot = 2;
        if (hot != g_idlg.hot) {
            g_idlg.hot = hot;
            if (hot >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_idlg.hot >= 0) { g_idlg.hot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        RECT b0 = IdBtnRect(0, r.right, r.bottom), b1 = IdBtnRect(1, r.right, r.bottom);
        if (PtInRect(&b0, p) && g_idlg.hot == 0) { InfoClose(); return 0; }   // Cancel
        if (PtInRect(&b1, p) && g_idlg.hot == 1) { InfoSave(); return 0; }    // Save
        if (p.y < IdCapH() && p.x >= r.right - (int)(44 * g.dpi) && g_idlg.hot == 2) InfoClose();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT r; GetClientRect(h, &r);
            RECT b0 = IdBtnRect(0, r.right, r.bottom), b1 = IdBtnRect(1, r.right, r.bottom);
            if (PtInRect(&b0, p) || PtInRect(&b1, p) ||
                (p.y < IdCapH() && p.x >= r.right - (int)(44 * g.dpi))) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
            }
            if (p.y < IdCapH()) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        }
        break;
    case WM_CLOSE:
        InfoClose();
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

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
    struct Kid { HWND w; RECT was; } kids[5] = { { g.bar, {} }, { g.side, {} }, { g.rail, {} }, { g.view, {} }, { g.status, {} } };
    for (auto& k : kids)
        if (k.w) {
            GetWindowRect(k.w, &k.was);
            ScreenToClient(g.main, (POINT*)&k.was);
            ScreenToClient(g.main, ((POINT*)&k.was) + 1);
        }
    int sw = (g.sideOn && DVF().n && !g.full) ? SW() : 0;   // the sidebar shows the FOCUSED
    int rw = RW();                              // pane's pages; the rail always rides between
    int cap = CAP();
    int tb = TBH();                             // the toolbar row under the caption
    int sh = g.full ? 0 : SBH();                // full screen hides the footer too
    int ch = max(0, h - cap - tb - sh);         // the children sit below caption + toolbar
    sd.w = sw;
    if (g.bar) MoveWindow(g.bar, 0, cap, w, tb, TRUE);
    if (g.side) {
        MoveWindow(g.side, 0, cap + tb, sw, ch, TRUE);
        if (sw) {
            RECT sr; GetClientRect(g.side, &sr);
            if (sr.right != sd.cw || sr.bottom != sd.ch) { sd.cw = sr.right; sd.ch = sr.bottom; SideLayout(); }
        }
    }
    if (g.rail) {                               // the rail: between the sidebar and the page,
        MoveWindow(g.rail, sw, cap + tb, rw, ch, TRUE);   // always there but in full screen
        ShowWindow(g.rail, rw > 0 ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    MoveWindow(g.view, sw + rw, cap + tb, max(0, w - sw - rw), ch, TRUE);
    if (g.status) MoveWindow(g.status, 0, cap + tb + ch, w, sh, TRUE);
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
                                WS_CHILD | WS_VISIBLE,
                                0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        g.rail = CreateWindowExW(0, L"DocsyRail", nullptr,
                                 WS_CHILD | WS_VISIBLE,
                                 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
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
                                 g_bdW[slot] = g_bdH[slot] = 0; g_bdDpi[slot] = 0; }
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
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &p);
        if (g.sideOn && DVF().n && p.x < sd.w) {
            SideScrollTo(sd.scroll - GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120);
            return 0;
        }
        ViewWheel(wp, lp);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(g.view, &p);
        bool b = PaneB(p.x);
        DocState& D = DV(b);
        ScrollPane(b, D.sx + GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 80) / 120, D.sy);
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        int line = (int)(56 * g.dpi), pg = max(1, (int)(g.ch * 0.9));
        int& fx = PaneSx(FocusB()); int& fy = PaneSy(FocusB());   // the keys steer the focused pane
        switch (wp) {
        case 'O': if (ctrl) OpenDlg(); break;
        case VK_DOWN: ScrollTo(fx, fy + line); break;
        case VK_UP: ScrollTo(fx, fy - line); break;
        case VK_RIGHT: ScrollTo(fx + line, fy); break;
        case VK_LEFT: ScrollTo(fx - line, fy); break;
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
        case 'F': if (ctrl) FindToggle(); else ApplyFit(false, FocusB()); break;   // Ctrl+F find, plain F fit
        case 'C': if (ctrl && !DVF().selText.empty()) CopyClipboard(DVF().selText); break;
        case 'A': if (ctrl) SelAll(DVF()); break;
        case VK_DELETE:
            // KillerPDF's DeleteSelected: the Select tool's chosen annotation
            // leaves its pane's document, topmost index last-in first-out safe
            if (g.annSelPage >= 0 && g.annSelIdx >= 0) {
                DocState& D2 = DV(g.annSelB);
                auto it = D2.annos.find(g.annSelPage);
                if (it != D2.annos.end() && g.annSelIdx < (int)it->second.size()) {
                    it->second.erase(it->second.begin() + g.annSelIdx);
                    if (it->second.empty()) D2.annos.erase(it);
                }
                g.annSelPage = -1; g.annSelIdx = -1;
                InvalidateRect(g.view, nullptr, FALSE);
            }
            break;
        case VK_F3:                                // Find next / previous; closed, F3 opens
            if (!fd.open) FindOpen();
            else if (!fd.hits.empty()) FindStep(shift ? -1 : 1);
            break;
        case VK_ESCAPE:
            if (g.tedit) CommitTextEdit(false);    // the open text box discards first
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
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 3) { InvalidateCap(h); return 0; }   // hovered-button keepalive repaint
        break;                                 // (buffered, so an unchanged band reblits
                                               // identically and cannot strobe)
    case WM_DROPFILES: HandleDrop((HDROP)wp, h); return 0;   // the frame's drop: the focused pane
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
    int sw = (g.sideOn && DVF().n && !g.full) ? sd.w : 0;
    int rw = RW();                                  // the rail's column rides the shot too
    int cap = CAP(), tb = TBH(), sh = SBH();
    int ch = max(1, h - cap - tb - sh);
    // the children render above the caption row and below it, so composite them shifted
    if (sw) {
        HDC m1 = CreateCompatibleDC(dc);
        HBITMAP b1 = CreateCompatibleBitmap(dc, sw, ch);
        HGDIOBJ o1 = SelectObject(m1, b1);
        SetBrushOrgEx(m1, 0, -(cap + tb), nullptr);      // the pane's grain phase, as on screen
        SideDraw(m1, sw, ch, 0);
        BitBlt(dc, 0, cap + tb, sw, ch, m1, 0, 0, SRCCOPY);
        SelectObject(m1, o1); DeleteObject(b1); DeleteDC(m1);
    }
    {
        HDC m2 = CreateCompatibleDC(dc);
        int vw = max(1, w - sw - rw);
        HBITMAP b2 = CreateCompatibleBitmap(dc, vw, ch);
        HGDIOBJ o2 = SelectObject(m2, b2);
        SetBrushOrgEx(m2, -sw - rw, -(cap + tb), nullptr);
        g_grainPX = -sw - rw; g_grainPY = -(cap + tb);   // the view's grain phase, as on screen
        if (g.split) {                                  // the split: two per-pane passes, the
            int wA = PaneW(false), xB = PaneX0(true), wB = PaneW(true);   // same paint the live
            for (int pass = 0; pass < 2; pass++) {      // view runs - focused pane LAST
                bool drawB = g.splitA ? pass == 0 : pass == 1;
                if (drawB) {
                    RECT pr{ xB, 0, vw, ch };
                    HRGN rg = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
                    Draw(m2, wB, ch, xB, g.dB, rg);
                    DeleteObject(rg);
                    PaintPaneBars(m2, g.dB, true, g.dB.vHot || g.dB.vDrag, g.dB.hHot || g.dB.hDrag);
                } else {
                    RECT pr{ 0, 0, wA, ch };
                    HRGN rg = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
                    Draw(m2, wA, ch, 0, g.d, rg);
                    DeleteObject(rg);
                    PaintPaneBars(m2, g.d, false, g.d.vHot || g.d.vDrag, g.d.hHot || g.d.hDrag);
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
        }
        if (fd.open) {                                  // the floating Find bar, over its pixels -
            FindDraw(m2, FdRect().left, FdRect().top);  // the same FdRect the live view paints,
        }                                               // so the shot IS the app's composite
        BitBlt(dc, sw + rw, cap + tb, vw, ch, m2, 0, 0, SRCCOPY);
        SelectObject(m2, o2); DeleteObject(b2); DeleteDC(m2);
    }
    if (rw > 0) {                                       // the rail strip between sidebar and view
        HDC m5 = CreateCompatibleDC(dc);
        HBITMAP b5 = CreateCompatibleBitmap(dc, rw, ch);
        HGDIOBJ o5 = SelectObject(m5, b5);
        SetBrushOrgEx(m5, -sw, -(cap + tb), nullptr);
        RailDraw(m5, rw, ch);
        BitBlt(dc, sw, cap + tb, rw, ch, m5, 0, 0, SRCCOPY);
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
    if (!g.full && sh > 0) {                   // the footer strip
        HDC m3 = CreateCompatibleDC(dc);
        HBITMAP b3 = CreateCompatibleBitmap(dc, w, sh);
        HGDIOBJ o3 = SelectObject(m3, b3);
        SetBrushOrgEx(m3, 0, -(cap + tb + ch), nullptr);
        StatusDraw(m3, w, sh);
        BitBlt(dc, 0, cap + tb + ch, w, sh, m3, 0, 0, SRCCOPY);
        SelectObject(m3, o3); DeleteObject(b3); DeleteDC(m3);
    }
    if (!g.full && tb > 0) {                   // the toolbar strip under the caption
        HDC m4 = CreateCompatibleDC(dc);
        HBITMAP b4 = CreateCompatibleBitmap(dc, w, tb);
        HGDIOBJ o4 = SelectObject(m4, b4);
        SetBrushOrgEx(m4, 0, -cap, nullptr);
        BarDraw(m4, w, tb);
        BitBlt(dc, 0, cap, w, tb, m4, 0, 0, SRCCOPY);
        SelectObject(m4, o4); DeleteObject(b4); DeleteDC(m4);
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

    WNDCLASSEXW bc{ sizeof bc };                    // the toolbar row: the Find Text button
    bc.lpfnWndProc = BarProc; bc.hInstance = hi;
    bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    bc.lpszClassName = L"DocsyBar";
    RegisterClassExW(&bc);

    WNDCLASSEXW rc{ sizeof rc };                    // the sidebar rail: Split / Pages / Info
    rc.lpfnWndProc = RailProc; rc.hInstance = hi;
    rc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    rc.lpszClassName = L"DocsyRail";
    RegisterClassExW(&rc);

    WNDCLASSEXW ic{ sizeof ic };                    // the Document Info dialog (F4)
    ic.lpfnWndProc = InfoDlgProc; ic.hInstance = hi;
    ic.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ic.lpszClassName = L"DocsyInfoDlg";
    RegisterClassExW(&ic);

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
    DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_CAPTION) | WS_CLIPCHILDREN;
    CreateWindowExW(0, L"Docsy", L"Docsy", style, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                    nullptr, nullptr, hi, nullptr);
    if (!g.main) return 1;
    SetWindowPos(g.main, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (shot) {
        if (lstrcmpW(argv[3], L"-") && !Open(g.d, argv[3])) return 2;
        // CI preview seed: a committed selection over one page's per-character carets
        // (sel=page:c0:c1), so the release screenshot shows the selection quads
        if (argc >= 5 && !wcsncmp(argv[4], L"sel=", 4)) {
            const wchar_t* q = argv[4] + 4;
            const wchar_t* c1p = wcschr(q, L':');
            const wchar_t* c2p = c1p ? wcschr(c1p + 1, L':') : nullptr;
            if (c1p && c2p) {
                int pg = _wtoi(q), c0 = _wtoi(c1p + 1), c1 = _wtoi(c2p + 1);
                TxtPage* pt = TtFor(g.d, pg);
                if (pt && !pt->cells.empty()) {
                    int n = (int)pt->cells.size();
                    g.d.selAPage = g.d.selFPage = pg;
                    g.d.selACaret = std::clamp(c0, 0, n);
                    g.d.selFCaret = std::clamp(c1, 0, n);
                    if (g.d.selACaret != g.d.selFCaret) g.d.selRange = true;
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
                DocState& D = DVF();
                while (fd.scan < D.n) {
                    TxtPage* pt = TtFor(D, fd.scan);
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
        // CI preview seed 3: zoom=NN pins the zoom to NN percent - Fit Width
        // rides the page edge-to-edge, which hides every card corner, so the
        // release screenshot zooms out and shows the margins, the ring, the
        // four corners and the gutter's shadow the way a reader at 35-56% sees
        // them.
        if (argc >= 7 && !wcsncmp(argv[6], L"zoom=", 5))
            SetZoom(_wtof(argv[6] + 5) / 100.0, g.cw / 2, g.ch / 2);
        // CI preview seed 4: split=on runs the real toggle - pane A keeps its
        // document, pane B shows its fresh start screen (KillerPDF's split opens
        // empty), and the window grows by pane B's column exactly as it does
        // live; the shot re-reads the client rect so the wider frame is honest
        for (int a = 3; a < argc; a++)
            if (!wcscmp(argv[a], L"split=on")) ToggleSplit();
        // CI preview seed 5: info=open renders the Document Info dialog's paint
        // over the view - the same InfoDialogDraw the live modal window runs
        for (int a = 3; a < argc; a++)
            if (!wcscmp(argv[a], L"info=open")) {
                PdfInfo pi;
                PdfInfoRead(DVF().path, pi);
                g_idlg = InfoDlg{};
                g_idlg.title = pi.title;      // raw values, like the live dialog
                g_idlg.author = pi.author;
                g_idlg.subject = pi.subject;
                g_idlg.keywords = pi.keywords;
                g_idlg.creator = pi.creator;
                g_idlg.producer = pi.producer;
                g_idlg.ver = pi.version;
                g_idlg.pages = DVF().n;
                WIN32_FILE_ATTRIBUTE_DATA fa;
                g_idlg.bytes = GetFileAttributesExW(DVF().path.c_str(), GetFileExInfoStandard, &fa)
                             ? (long long)(((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) : 0;
                g_idlgShots = true;
            }
        // CI preview seed 6: fileB=<path> loads pane B so the split's scrollbar
        // math can be verified against a REAL second document - pane B's own
        // pair of overlay bars rides its own card edges, pane A's hugs the
        // divider, and neither thumb may wash out over a page or a stale strip
        for (int a = 3; a < argc; a++)
            if (!wcsncmp(argv[a], L"fileB=", 6)) Open(g.dB, argv[a] + 6);
        // CI preview seed 7: tool=N switches the focused pane's tool through the
        // real SetTool, so the rail's selected-state face and the tool's cursor
        // state are what the shot shows
        for (int a = 3; a < argc; a++)
            if (!wcsncmp(argv[a], L"tool=", 5)) SetTool(_wtoi(argv[a] + 5), FocusB());
        // CI preview seed 8: annot=demo seeds one committed annotation of each
        // primitive KillerPDF's tools produce - a yellow per-line highlight, a
        // red ink stroke (Draw/Line's commit) and a filled red rectangle
        // (Shapes' commit) - over page 1's own DIP space, so the render path
        // and its overlay order are pixel-verifiable
        for (int a = 3; a < argc; a++)
            if (!wcscmp(argv[a], L"annot=demo")) {
                DocState& D = DVF();
                if (D.n) {
                    TtFor(D, 0);   // the OCR layer warms the same way a live drag would
                    Annot hl; hl.type = AT_HIGHLIGHT;
                    hl.color = RGB(0xff, 0xff, 0x00); hl.alpha = 80;
                    hl.lines.push_back(FRect{ 60, 60, 200, 16 });
                    hl.x = 60; hl.y = 60; hl.w = 200; hl.h = 16;
                    D.annos[0].push_back(hl);
                    Annot ink; ink.type = AT_INK; ink.width = 3;
                    ink.color = RGB(0xff, 0x00, 0x00);
                    ink.ix = { 60, 120, 180, 240 }; ink.iy = { 110, 90, 130, 100 };
                    D.annos[0].push_back(ink);
                    Annot sh; sh.type = AT_HIGHLIGHT; sh.color = RGB(0xff, 0x00, 0x00); sh.alpha = 80;
                    sh.lines.push_back(FRect{ 60, 150, 180, 60 });
                    sh.x = 60; sh.y = 150; sh.w = 180; sh.h = 60;
                    D.annos[0].push_back(sh);
                    Annot tx; tx.type = AT_TEXT; tx.fsize = 24;
                    tx.x = 60; tx.y = 230; tx.w = 165; tx.h = 30;
                    tx.color = RGB(0, 0, 0); tx.text = L"Docsy";
                    D.annos[0].push_back(tx);
                }
            }
        GetClientRect(g.main, &c);             // the seeds may have resized (split=on grows
        for (int k = 0; k <= DVF().n; k++) if (!SidePump()) break;   // the window like the real
        return Shot(argv[2], c.right, c.bottom);                     // toggle does) - re-read it
    }

    ShowWindow(g.main, show);
    UpdateWindow(g.main);
    if (argc > 1) Open(g.d, argv[1]);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
