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

struct Ent { HBITMAP b; int w, h; int rot = 0; unsigned long long t; };   // rot: the Transform
                                           // quarter-turns + flip bits baked in

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
    std::vector<float> ix, iy;             // ink: the polyline (DIPs; closed shapes repeat the first point)
    float width = 3;                       // ink stroke width (DIPs) - KillerPDF's _drawWidth 3
    float fsize = 24;                      // text: KillerPDF's _textFontSize 24
    COLORREF color = RGB(0xff, 0x00, 0x00);
    BYTE alpha = 255;
    std::wstring text;                     // AT_TEXT content
    // KillerPDF's fill/typography state, carried per annotation so the Select
    // tool can re-style it and the bar can sync FROM it (SetFillColor /
    // _textFontName / the B I S U toggles):
    COLORREF fill = RGB(255, 255, 255);    // closed ink / text box background
    BYTE fillA = 0;                        // 0 = no fill (KillerPDF's default A=0)
    std::wstring font = L"Segoe UI";       // _textFontName
    bool bold = false, italic = false, strike = false, under = false;
    float spacing = 0;                     // _textLetterSpacing (DIPs)
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
    // so pane A's and pane B's bars each live here now
    bool vHot = false, vDrag = false, hHot = false, hDrag = false;   // overlay bar states
    int vGrab = 0, hGrab = 0;
    // the pane's annotations and its own current tool - KillerPDF keeps the
    // editing state per viewer instance (PdfViewer.Bridge2: "Annotations, ...
    // and the current tool remain independent between panes"), so each pane
    // draws with the tool the reader left IT on
    std::unordered_map<int, std::vector<Annot>> annos;
    int tool = 0;                       // TOOL_SELECT .. TOOL_DRAW (the toolbar's tool buttons)
    // the Transform popup's page-level session transforms (KillerPDF rasterizes
    // the page - text becomes unselectable there; docsy rotates the render and
    // the page's own DIP space, so annotations rotate with it):
    std::unordered_map<int, int> prot;  // quarter-turns CW the cache bakes (0-3)
    std::unordered_map<int, BYTE> pflip; // the Transform's flips: bit1 H, bit2 V
};
enum { TOOL_SELECT, TOOL_TEXT, TOOL_HIGHLIGHT, TOOL_LINE, TOOL_SHAPE, TOOL_DRAW,
       TOOL_COUNT };

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
    int tbHot = -1;                     // hovered toolbar button, -1 none (index into the row)
    int tbPressed = -1;                 // a toolbar button held down (capture-held, WPF's IsPressed)
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
    HBRUSH teBg = nullptr;              // the editor's sampled page-tone brush (KillerPDF's
                                        // TextEditBackground: fill when set, else page under a
                                        // 64-alpha gray)
    // the Select tool's corner-handle resize (KillerPDF's NW/NE/SE/SW handles)
    bool annResize = false;             // a handle drag is live
    int annHandle = 0;                  // which handle: 0 NW, 1 NE, 2 SE, 3 SW
    bool annMoved = false;              // the armed annotation actually moved (a click arms
                                        // it again, a drag moves it)
    // the Shapes tool's polygon session (KillerPDF's ShapeKind.Polygon): click
    // points, close on the first point / double-click / Enter, Esc cancels
    bool annPoly = false;
    std::vector<float> annPX, annPY;    // the vertices so far (page DIPs)
    // ---- shared tool defaults (KillerPDF's PdfViewer.Bridge2 state) ----
    COLORREF drawColor = RGB(0xff, 0x00, 0x00);   // _drawColor: Colors.Red
    BYTE drawOpacity = 255;                       // _drawOpacity: 100%
    float drawWidth = 3;                          // _drawWidth: 3
    bool lineLevel = true;                        // _lineLevel: the Line bar's Level checkbox, ON
    bool drawErase = false;                       // the Draw bar's Eraser checkbox (transient)
    bool hlErase = false;                         // the Highlight bar's Annotation eraser (transient)
    int shapeKind = 0;                            // _shapeKind: Rectangle, then Ellipse, Polygon
    bool shapeFill = true;                        // _shapeFill: the Fill checkbox, CHECKED
    COLORREF hlRGB = RGB(0xff, 0xff, 0x00);       // _highlightColor: yellow
    BYTE hlA = 80;                                // ... at alpha 80 (~31%)
    std::wstring textFont = L"Segoe UI";          // _textFontName
    float textSize = 24;                          // _textFontSize (pt)
    float textSpacing = 0;                        // _textLetterSpacing (pt)
    bool textBold = false, textItalic = false, textStrike = false, textUnder = false;
    COLORREF textRGB = RGB(0, 0, 0);              // _textColor
    BYTE textA = 255;                             // _textOpacity
    COLORREF textFillRGB = RGB(255, 255, 255);    // _textFillColor: fill NONE (A=0)
    BYTE textFillA = 0;
    // ---- undo / redo (KillerPDF's stack, snapshot granularity, 20 deep) ----
    struct UndoSnap {
        bool paneB = false;
        std::unordered_map<int, std::vector<Annot>> annos;
    };
    std::vector<UndoSnap> undo, redo;
    // ---- the floating annotation settings bar (KillerPDF's AnnotationBars) ----
    bool barOn = false;                 // a bar is on screen
    int barTool = -1;                   // the TOOL_* whose bar is up
    bool barMini = false;               // tucked to the 13px peek strip
    int barDx = 0;                      // slide offset from the right-anchored home
    int barHot = -1;                    // hovered cell, -1 none
    int barSlide = -1;                  // slider cell being dragged, -1 none
    int barSlideX0 = 0; float barSlideV0 = 0;   // drag anchor for the value map
    bool barDrag = false;               // the grip is sliding the bar
    int barDragX = 0, barDx0 = 0;
    int barW = 0, barH = 0;             // last laid-out size (the peek freeze + invalidation)
    // popups the bars open (the font list, the color picker, the Transform
    // menu, the size edit, the paragraph editor)
    HWND fontPop = nullptr, colorPop = nullptr, trPop = nullptr, sizeEdit = nullptr, paraDlg = nullptr;
    int colorTarget = 0;                // which color the picker edits (1 draw, 2 highlight, 3 text, 4 text fill)
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
static int PaneW(bool b);                         // a pane's full pass width (the bar's wrap cap)

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

// defined with the split's pane geometry and the annotation paint below - the
// tools' interaction layer (further down) reads both
static int PaneX0(bool b);                         // a pane's pass origin in view x
static int CardX(bool b);                          // the card inside the pass
static bool PageOrigin(const DocState& D, bool b, int i, int& ox, int& oy);
static void AnnotBounds(const Annot& a, float& x0, float& y0, float& x1, float& y1);
static bool AnnotHit(const Annot& a, float fx, float fy, float k);

// the sidebar rail's three buttons and the Document Info dialog, defined with
// the rail section below
static int RW();                        // the rail's column width (0 in full screen)
static void RailRepaint();              // the rail repaints when a lit state flips
static void ToggleSplit();
static void ToggleSide();               // defined with the main window (the Pages button)
static void InfoOpen();
static bool InfoIsOpen();
static void SetStatusHold(const std::wstring& t, unsigned ms = 2500);   // the footer's held line
static void SetTool(int t, bool b);     // the toolbar's tool buttons (KillerPDF's SetTool)
static void CommitTextEdit(bool commit = true, bool restore = false);   // the Text tool's box
static void NewBlank();                 // Ctrl+N: a blank document into the focused pane
static void CloseDoc();                 // the Close button: the focused pane's document leaves
static void SaveDoc(bool saveAs);       // the Save button + Ctrl+S / Ctrl+Shift+S
static bool WriteAnnotatedPdf(DocState& D, const std::wstring& file, std::wstring& err);
// ---- edit tools, the bar and its popups (defined with their sections) ----
static void UndoPush(bool b);           // snapshot the pane's annotations + marks before a change
static void UndoOp();                   // Ctrl+Z
static void RedoOp();                   // Ctrl+Y / Ctrl+Shift+Z
static void ClearAllAnnos();            // the Clear All button
static void TransformPopupOpen();       // the Transform button's menu
static void TransformApply(bool b, int page, int turns, bool flipH, bool flipV);
static void ApplyDrawStyleToSelection();    // bar color/size/opacity -> the selected annotation
static void ApplyTextStyleToSelection();    // text bar -> the selected text box
static void AnnBarSync(bool rebuild);   // repaint the floating bar (old ∪ new footprint)
static RECT AnnBarRect();               // the bar's on-screen rect in view coords
static void PolyCancel(HWND h);         // abandon the polygon session
static void PolyCommit(HWND h);         // close the polygon (repeat first point)
static void ListPopOpen(HWND owner, POINT p, std::vector<std::wstring> items, int who, int selRow);
static void OpenDlg();                   // the Open button's file dialog
static void PaintAnnots(HDC dc, const DocState& D, int page, int px, int py, float k);
// the footer's shared fills (defined with the status bar, used by the bars)
static HBRUSH PopBrush();
static HBRUSH PopHoverBrush();
static HBRUSH ComboBorderBrush();
static HBRUSH ComboFillBrush();

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
// GrainBrushShared overlay), holding ToolbarGrid (Margin 4,2). Its RightBar
// carries the editing tools; docsy takes the six its annotation engine owns,
// in ToolbarLayout's own default order - select, text, highlight, line, shape,
// draw - followed by the search unit, the same right-aligned cluster the
// reference bar ends with. Every button is the family ToolbarButton in the
// default Large + Under mode (Shell/ToolbarStyle.cs: Large icons, text under,
// the family default since 2026-07-30): glyph FontSize 20 over a Killer Sans
// 14 caption with a 2px gap, Padding 6,4, MinHeight 50, width auto = the
// wider of the glyph line box and the caption plus 12 - sitting directly on
// the chrome (no box at rest), a radius-4 ControlCornerRadius RowHoverBrush
// #404040 tile on hover, SurfaceBrush #333333 while pressed, Opacity 0.4 when
// no document is open (ToolbarAvailability's `_ => state.HasDocument` - the
// tools are not special-cased), the Hand cursor, and the content's icon shadow
// (black, blur 4, depth 2, IconShadowOpacity 0.9 on the dark themes) as the
// dark offset copy the caption's wordmark already uses. Click acts on release
// inside, with capture held - WPF button semantics.
//
// The tools' selected state is KillerPDF's SetTool (Shell/ToolSelection.cs):
// the active tool's button wears the SelectionBg gradient with the SelectionFg
// white glyph and caption, the icon shadow dropped (Tag="selected" clears the
// ContentPresenter effect), and the face stays SelectionBg under hover and
// press - the local Background beats the style triggers, exactly as WPF
// resolves it. A button with no document behind it dims instead; a click
// never lands on a dimmed button.

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

// The row's buttons, the trimmed set the user's reference shows: a LEFT
// cluster (Close, Open, Save) and the RIGHT cluster (the tools, Transform,
// Clear All, then the search unit) - the reference's New, Flatten, Print,
// Undo, Redo and the Open/Save split chevrons stay off docsy's row, their
// actions riding the keyboard shortcuts instead.
// Labels are the reference's own strings (Str_Lbl_*); glyphs are the buttons'
// own MDL2 codepoints at the runtime restyle's size (ToolbarStyle's
// BuildToolbarContent, GlyphLarge 20) - Transform draws its composed icon
// (the 20px frame + four vector arrows), DrawToolbarGlyph's TransformBoxMove.
enum { TB_CLOSE, TB_OPEN, TB_SAVE,   // the file cluster
       TB_SELECT, TB_TEXT, TB_HIGHLIGHT, TB_LINE, TB_SHAPE, TB_DRAW,
       TB_TRANSFORM, TB_CLEAR, TB_FIND, TB_COUNT };
static const wchar_t* const TbLabel[TB_COUNT] = {
    L"Close", L"Open", L"Save",
    L"Select", L"Text", L"Highlight", L"Line", L"Shapes", L"Draw",
    L"Transform", L"Clear All", L"Find Text"
};
static const wchar_t* const TbGlyph[TB_COUNT] = {
    L"\uE711", L"\uE8DA", L"\uE74E",
    L"\uE8B0", L"\uE8D2", L"\uE7E6", L"\uE738", L"\uE739", L"\uED63",
    L"", L"\uED62", L"\uE721"
};   // Transform carries its composed icon, drawn in BarDraw

// ToolbarAvailability: Open arms always (KillerPDF's XAML leaves it enabled
// with no document), Close/Save/tools/Transform/Search arm with a document;
// Clear All arms when there is anything to clear. Narrow windows shed
// RIGHT-cluster middle buttons first (ReflowToolbar's shed order, adapted) -
// Select and Find Text always stay.
static bool TbShed[TB_COUNT] = {};
static int TbToolOf(int i) {
    if (i >= TB_SELECT && i <= TB_DRAW) return i - TB_SELECT;
    return -1;                            // actions carry no selected face
}
static bool TbEnabled(int i) {
    bool doc = DVF().n != 0;
    if (i == TB_OPEN) return true;
    if (i == TB_CLEAR) {
        if (!doc) return false;
        for (auto& kv : DVF().annos) if (!kv.second.empty()) return true;
        return g.annInk || g.annPoly;      // a pending gesture counts, like HasPendingAnnotation
    }
    return doc;
}

// A button's auto width in Under mode: the wider of the glyph's 24px MDL2 line
// box (FontSize 20) and the caption, plus Padding 6,4's 12 horizontal pixels.
static int TbBtnW(HDC dc, int i) {
    RECT rc{ 0, 0, 0, 0 };
    HFONT of = (HFONT)SelectObject(dc, CaptionFont(g_tbcap, 14, false, L"Killer Sans"));
    DrawTextW(dc, TbLabel[i], -1, &rc, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, of);
    return max((int)(24 * g.dpi), (int)(rc.right - rc.left)) + (int)(12 * g.dpi);
}   // the caption width casts: MSVC's std::max cannot deduce across LONG and int

// The row inside the strip: ToolbarGrid's Margin 4,2 - 4 from the edges,
// 2 from the top, auto height (TbBtnH, MinHeight 50 floor), buttons touching
// (a StackPanel has no spacing; each button's own padding makes the gap).
// The LEFT cluster anchors to the left edge, the RIGHT cluster to the right;
// when the window is too narrow for both, right-cluster middle buttons shed
// (ReflowToolbar) until the two fit.
static void TbRects(int w, RECT out[TB_COUNT]) {
    HDC dc = GetDC(g.bar);
    int bh = TbBtnH();
    int y0 = (int)(2 * g.dpi);
    for (int i = 0; i < TB_COUNT; i++) TbShed[i] = false;
    // right cluster, right to left
    int x = w - (int)(4 * g.dpi);
    for (int i = TB_COUNT - 1; i >= TB_SELECT; i--) {
        int bw = TbBtnW(dc, i);
        x -= bw;
        out[i] = { x, y0, x + bw, y0 + bh };
    }
    // left cluster, left to right; shed until no overlap (the file basics stay
    // - they are KillerPDF's own left cluster)
    static const int shedOrder[] = { TB_TRANSFORM, TB_CLEAR,
                                     TB_DRAW, TB_SHAPE, TB_LINE, TB_HIGHLIGHT, TB_TEXT };
    int lx = (int)(4 * g.dpi);
    for (int i = TB_CLOSE; i < TB_SELECT; i++) {
        int bw = TbBtnW(dc, i);
        out[i] = { lx, y0, lx + bw, y0 + bh };
        lx += bw;
    }
    for (int s = 0; s < 7 && out[TB_SELECT - 1].right > out[TB_SELECT].left; s++) {
        int i = shedOrder[s];
        TbShed[i] = true;
        int bw = out[i].right - out[i].left;
        for (int k = TB_SELECT; k < TB_COUNT; k++) {   // the right cluster closes left
            if (out[k].left >= out[i].left) { out[k].left += bw; out[k].right += bw; }
        }
        out[i] = { 0, 0, 0, 0 };
    }
    ReleaseDC(g.bar, dc);
}

static int BarHit(POINT p, int w) {
    RECT bs[TB_COUNT]; TbRects(w, bs);
    for (int i = 0; i < TB_COUNT; i++)
        if (!TbShed[i] && p.x >= bs[i].left && p.x < bs[i].right && p.y >= bs[i].top && p.y < bs[i].bottom) return i;
    return -1;
}

// The composed Transform icon's arrows: DrawToolbarGlyph's vector strokes, not
// a font glyph - "The Segoe MDL2 move glyph shrunk to fit the box rendered its
// bottom arrowhead as a single pixel, so it read as cut off" is the reference's
// own reason for drawing them. The 12 segments live in the path's 11x11 unit
// space (center 8,8); the path box is 70% of the glyph size (14px at
// GlyphLarge 20), so units scale by 14/11 and center on the frame.
// StrokeThickness 1 stays 1 (Shape.Stretch scales the geometry, never the pen),
// square caps, miter joins - a 1px GDI line lands on the same pixels.
static void TfArrows(HDC dc, int cx, int cy, COLORREF c) {
    static const float seg[12][4] = {   // x0,y0,x1,y1 in the path's unit space:
        { 8, 2.5f, 8, 6 },      { 8, 10, 8, 13.5f },        // the four stems...
        { 2.5f, 8, 6, 8 },      { 10, 8, 13.5f, 8 },
        { 6, 4.5f, 8, 2.5f },   { 8, 2.5f, 10, 4.5f },      // ...the four heads,
        { 6, 11.5f, 8, 13.5f }, { 8, 13.5f, 10, 11.5f },    // two strokes each
        { 4.5f, 6, 2.5f, 8 },   { 2.5f, 8, 4.5f, 10 },      // meeting at the tip
        { 11.5f, 6, 13.5f, 8 }, { 13.5f, 8, 11.5f, 10 },
    };
    const float s = 14.0f / 11.0f * (float)g.dpi;   // 11x11 units -> the 14px path box
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ og = SelectObject(dc, p);
    for (auto& q : seg) {
        MoveToEx(dc, cx + (int)std::lround((q[0] - 8) * s), cy + (int)std::lround((q[1] - 8) * s), nullptr);
        LineTo(dc, cx + (int)std::lround((q[2] - 8) * s), cy + (int)std::lround((q[3] - 8) * s));
    }
    SelectObject(dc, og);
    DeleteObject(p);
}

static void BarDraw(HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    RECT bar{ 0, 0, w, h };
    FillRect(dc, &bar, GrainBrush(1));   // the shared dark chrome: title bar + toolbar + sidebar + footer
    RECT bs[TB_COUNT]; TbRects(w, bs);
    int capH = 0;                        // every caption shares one face, one size
    {   RECT mc{ 0, 0, 0, 0 };
        HFONT cf = CaptionFont(g_tbcap, 14, false, L"Killer Sans");
        HFONT of = (HFONT)SelectObject(dc, cf);
        DrawTextW(dc, L"Find Text", -1, &mc, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(dc, of);
        capH = mc.bottom - mc.top;       // one face, one size: every caption shares it
    }
    for (int i = 0; i < TB_COUNT; i++) {
        const RECT& b = bs[i];
        if (TbShed[i]) continue;
        bool en = TbEnabled(i);
        int tool = TbToolOf(i);
        bool sel = en && tool >= 0 && DVF().tool == tool;   // SetTool's SelectionBg face,
                                                            // lit on the focused pane's tool
        bool hot = en && !sel && g.tbHot == i;
        bool pr = en && !sel && g.tbPressed == i;
        if (sel || hot || pr) {              // a face under the content: the active tool's
            Gdiplus::Graphics gs(dc);        // SelectionBg, or the hover/pressed tile
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath p;
            float rr = (float)(4 * g.dpi);   // ToolbarButton's ControlCornerRadius is 4
            float fx = (float)b.left, fy = (float)b.top;
            float fw = (float)(b.right - b.left), fh = (float)(b.bottom - b.top);
            p.AddArc(fx, fy, 2 * rr, 2 * rr, 180, 90);
            p.AddArc(fx + fw - 2 * rr, fy, 2 * rr, 2 * rr, 270, 90);
            p.AddArc(fx + fw - 2 * rr, fy + fh - 2 * rr, 2 * rr, 2 * rr, 0, 90);
            p.AddArc(fx, fy + fh - 2 * rr, 2 * rr, 2 * rr, 90, 90);
            p.CloseFigure();
            if (sel) {   // SelectionBg's own #387552 -> #104e2b vertical gradient; it also
                         // holds under hover/press (the local value beats the triggers)
                Gdiplus::LinearGradientBrush gb(
                    Gdiplus::Rect(b.left, b.top, b.right - b.left, b.bottom - b.top),
                    Gdiplus::Color(255, 0x38, 0x75, 0x52),
                    Gdiplus::Color(255, 0x10, 0x4e, 0x2b), 90.0f);
                gs.FillPath(&gb, &p);
            } else {     // RowHoverBrush #404040, pressed SurfaceBrush #333333; disabled
                         // bakes the style's Opacity 0.4 over #1c1c1c: 0.4*#404040 +
                         // 0.6*#1c1c1c = #2a2a2a
                Gdiplus::Color c = pr ? Gdiplus::Color(255, 0x33, 0x33, 0x33)
                                      : Gdiplus::Color(255, 0x40, 0x40, 0x40);
                if (!en) c = Gdiplus::Color(255, 0x2a, 0x2a, 0x2a);
                Gdiplus::SolidBrush f(c);
                gs.FillPath(&f, &p);
            }
        }
        // the content column: the glyph at 20 with the caption 2px under, both
        // centered (ToolbarStyle.cs BuildToolbarContent, Under mode). White on
        // the active tool (SelectionFg), TextBrush otherwise, the 0.4 dim over
        // #1c1c1c with no document - and the icon shadow's dark copy under the
        // light pass except on the active tool (Tag="selected" clears the
        // ContentPresenter effect). Save's accent rides the
        // ToolbarSplitMainAccent's AccentLogo #1ea54c.
        bool accent = i == TB_SAVE;
        int gh = (int)(24 * g.dpi), gap = (int)(2 * g.dpi);   // the MDL2 line box at FontSize 20:
        int colH = gh + gap + capH;                            // a 20px cell would clip the glyph
        int avail = (int)(b.bottom - b.top) - colH;            // MSVC: RECT members are LONG - cast
        int y0 = b.top + max(0, avail) / 2;                    // before the std::max deduction
        RECT gr{ b.left, y0, b.right, y0 + gh };
        RECT cr{ b.left, y0 + gh + gap, b.right, y0 + gh + gap + capH };
        SetBkMode(dc, TRANSPARENT);
        // DangerRed #ef4444 rides the Clear All button (ToolbarButtonDanger);
        // disabled buttons bake Opacity 0.4 over #1c1c1c -> #6a6a6a-class text
        bool danger = i == TB_CLEAR;
        COLORREF fg = sel ? RGB(0xff, 0xff, 0xff)
                   : en ? (danger ? RGB(0xef, 0x44, 0x44)
                                  : accent ? RGB(0x1e, 0xa5, 0x4c) : RGB(0xe0, 0xe0, 0xe0))
                        : (danger ? RGB(0x77, 0x33, 0x33)
                                  : accent ? RGB(0x18, 0x63, 0x2e) : RGB(0x6a, 0x6a, 0x6a));
        if (!TbGlyph[i][0]) {                // the composed icon: DrawToolbarGlyph's
                                             // TransformBoxMove at GlyphLarge 20 - a 20x20
                                             // 1px Border whose four move arrows are VECTOR
                                             // STROKES (the inline XAML's E7C2 glyph is
                                             // overridden at runtime; the font glyph read as
                                             // cut off, the reference's own comment says so)
            int fw2 = (int)(20 * g.dpi), fh2 = (int)(20 * g.dpi);
            int gx = b.left + (b.right - b.left - fw2) / 2;
            int gy = gr.top + (gh - fh2) / 2;
            auto drawTf = [&](COLORREF c, int ox, int oy) {
                HPEN fr = CreatePen(PS_SOLID, 1, c);
                HGDIOBJ og = SelectObject(dc, fr);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));   // frame only
                RECT fr2{ gx + ox, gy + oy, gx + fw2 + ox, gy + fh2 + oy };
                Rectangle(dc, fr2.left, fr2.top, fr2.right, fr2.bottom);
                SelectObject(dc, ob);
                SelectObject(dc, og); DeleteObject(fr);
                TfArrows(dc, gx + ox + fw2 / 2, gy + oy + fh2 / 2, c);   // the vector arrows,
            };                                           // centered on the frame
            if (!sel) drawTf(RGB(16, 16, 16), 0, 1);   // Direction 270: the shadow hangs
            drawTf(fg, 0, 0);                          // BELOW the icon, never beside it
        } else {
        if (!sel) {                                            // the shadow copy
            SelectObject(dc, CaptionFont(g_tbglyph, 20, false, L"Segoe MDL2 Assets"));
            SetTextColor(dc, RGB(16, 16, 16));
            RECT gs1{ gr.left + 1, gr.top + 1, gr.right + 1, gr.bottom + 1 };
            DrawTextW(dc, TbGlyph[i], -1, &gs1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, CaptionFont(g_tbglyph, 20, false, L"Segoe MDL2 Assets"));
        SetTextColor(dc, fg);
        DrawTextW(dc, TbGlyph[i], -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (TbLabel[i][0]) {
        SelectObject(dc, CaptionFont(g_tbcap, 14, false, L"Killer Sans"));
        if (!sel) {                                            // the caption's shadow copy
            SetTextColor(dc, RGB(16, 16, 16));
            RECT cs1{ cr.left + 1, cr.top + 1, cr.right + 1, cr.bottom + 1 };
            DrawTextW(dc, TbLabel[i], -1, &cs1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SetTextColor(dc, fg);
        DrawTextW(dc, TbLabel[i], -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

static LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:                        // the row anchors to the right edge: a resize
        InvalidateRect(h, nullptr, FALSE);   // only dirties the exposed strip, so the
        return 0;                            // row must repaint whole or buttons
                                             // vanish and old copies linger
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
        int hit = BarHit(p, r.right);
        if (hit >= 0 && TbEnabled(hit)) {  // capture held: WPF's IsPressed story
            SetCapture(h);                 // (ToolbarAvailability's per-button rules)
            g.tbPressed = hit;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int was = g.tbPressed;
        if (was >= 0) {
            g.tbPressed = -1;
            if (GetCapture() == h) ReleaseCapture();
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT r; GetClientRect(h, &r);
            int hit = BarHit(p, r.right);
            if (hit >= 0 && hit == was && TbEnabled(hit)) {    // click on release inside one
                switch (hit) {                                 // button, KillerPDF's handlers
                case TB_CLOSE: CloseDoc(); break;
                case TB_OPEN: OpenDlg(); break;
                case TB_SAVE: SaveDoc(false); break;
                case TB_TRANSFORM: TransformPopupOpen(); break;
                case TB_CLEAR: ClearAllAnnos(); break;
                case TB_FIND: FindToggle(); break;
                default: {                                     // the tools: SetTool on the
                    int t = TbToolOf(hit);                     // active viewer, like the
                    if (t >= 0) SetTool(t, FocusB());          // ToolX_Click one-liners
                    break;
                }
                }
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:              // the press lost the mouse some other way
        if (g.tbPressed >= 0) { g.tbPressed = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(g.main, m, wp, lp);   // route like the footer does: the wheel scrolls
                                                  // the page from any chrome surface
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT r; GetClientRect(h, &r);
            SetCursor(LoadCursorW(nullptr, BarHit(p, r.right) >= 0 ? IDC_HAND : IDC_ARROW));
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
        // the Transform's quarter-turns and flips bake into the render: the page
        // rasterizes at its UNROTATED aspect, then GDI+ rotates/flips into the
        // caller's (already swapped) bitmap, so every downstream path - layout,
        // hit tests, annotation painting - reads ordinary w x h pixels
        int rot = D.prot.count(i) ? D.prot[i] : 0;
        int flip = D.pflip.count(i) ? D.pflip[i] : 0;
        int rw = (rot % 2 == 0) ? w : h, rh = (rot % 2 == 0) ? h : w;
        auto page = D.doc.GetPage((uint32_t)i);
        PdfPageRenderOptions o;
        o.DestinationWidth((uint32_t)max(1, rw));
        o.DestinationHeight((uint32_t)max(1, rh));
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
        if (rot == 0 && flip == 0) {
            bmp.GetHBITMAP(Gdiplus::Color(255, 255, 255), &hb);
            return hb;
        }
        Gdiplus::Bitmap* out = bmp.Clone(0, 0, bmp.GetWidth(), bmp.GetHeight(), PixelFormat32bppARGB);
        if (!out || out->GetLastStatus() != Gdiplus::Ok) { delete out; return nullptr; }
        // the compose order matches TransformApply's coordinate map exactly:
        // flips first, then the quarter-turns, one named call each - no enum
        // arithmetic to miscount
        if (flip & 1) out->RotateFlip(Gdiplus::RotateNoneFlipX);
        if (flip & 2) out->RotateFlip(Gdiplus::RotateNoneFlipY);
        if ((rot & 3) == 1) out->RotateFlip(Gdiplus::Rotate90FlipNone);
        else if ((rot & 3) == 2) out->RotateFlip(Gdiplus::Rotate180FlipNone);
        else if ((rot & 3) == 3) out->RotateFlip(Gdiplus::Rotate270FlipNone);
        out->GetHBITMAP(Gdiplus::Color(255, 255, 255), &hb);
        delete out;
        return hb;
    } catch (...) { return nullptr; }
}

static HBITMAP GetBmp(DocState& D, int i, int w, int h) {
    auto it = D.cache.find(i);
    int rot = (D.prot.count(i) ? D.prot[i] : 0) | (D.pflip.count(i) ? D.pflip[i] << 4 : 0);
    if (it != D.cache.end()) {
        if (it->second.w == w && it->second.h == h && it->second.rot == rot) { it->second.t = ++D.tick; return it->second.b; }
        DeleteObject(it->second.b); D.cache.erase(it);
    }
    HBITMAP b = Render(D, i, w, h);
    if (!b) return nullptr;
    D.cache[i] = Ent{ b, w, h, rot, ++D.tick };
    return b;
}

// The same render as Render(), decoded as a SoftwareBitmap - the input format the
// OS OCR engine consumes for the text-selection pass. Rotated/flipped pages bake
// their transform the same way, so a transformed page's OCR cells land in its
// new coordinate space and selection keeps working there.
static winrt::Windows::Graphics::Imaging::SoftwareBitmap RenderSoft(DocState& D, int i, int w, int h) {
    try {
        int rot = D.prot.count(i) ? D.prot[i] : 0;
        int flip = D.pflip.count(i) ? D.pflip[i] : 0;
        int rw = (rot % 2 == 0) ? w : h, rh = (rot % 2 == 0) ? h : w;
        auto page = D.doc.GetPage((uint32_t)i);
        PdfPageRenderOptions o;
        o.DestinationWidth((uint32_t)max(1, rw));
        o.DestinationHeight((uint32_t)max(1, rh));
        o.BackgroundColor(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
        o.BitmapEncoderId(winrt::Windows::Graphics::Imaging::BitmapEncoder::BmpEncoderId());
        InMemoryRandomAccessStream st;
        page.RenderToStreamAsync(st, o).get();
        st.Seek(0);
        winrt::com_ptr<IStream> is;
        winrt::check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(st), winrt::guid_of<IStream>(), is.put_void()));
        Gdiplus::Bitmap bmp(is.get());
        if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;
        if (rot == 0 && flip == 0) {
            auto dec = winrt::Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(st).get();
            return dec.GetSoftwareBitmapAsync().get();
        }
        Gdiplus::Bitmap* out = bmp.Clone(0, 0, bmp.GetWidth(), bmp.GetHeight(), PixelFormat32bppARGB);
        if (!out || out->GetLastStatus() != Gdiplus::Ok) { delete out; return nullptr; }
        if (flip & 1) out->RotateFlip(Gdiplus::RotateNoneFlipX);
        if (flip & 2) out->RotateFlip(Gdiplus::RotateNoneFlipY);
        if ((rot & 3) == 1) out->RotateFlip(Gdiplus::Rotate90FlipNone);
        else if ((rot & 3) == 2) out->RotateFlip(Gdiplus::Rotate180FlipNone);
        else if ((rot & 3) == 3) out->RotateFlip(Gdiplus::Rotate270FlipNone);
        // re-encode the rotated raster so the OCR path reads the same pixels
        CLSID bmpCls{}; UINT cnt = 0, bytes = 0;
        Gdiplus::GetImageEncodersSize(&cnt, &bytes);
        std::vector<BYTE> buf(bytes);
        auto* enc = (Gdiplus::ImageCodecInfo*)buf.data();
        Gdiplus::GetImageEncoders(cnt, bytes, enc);
        for (UINT e = 0; e < cnt; e++) if (!lstrcmpW(enc[e].MimeType, L"image/bmp")) bmpCls = enc[e].Clsid;
        InMemoryRandomAccessStream st2;
        winrt::com_ptr<IStream> os;
        winrt::check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(st2), winrt::guid_of<IStream>(), os.put_void()));
        out->Save(os.get(), &bmpCls, nullptr);
        delete out;
        LARGE_INTEGER z0{}; z0.QuadPart = 0;
        os->Seek(z0, STREAM_SEEK_SET, nullptr);
        auto dec2 = winrt::Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(st2).get();
        return dec2.GetSoftwareBitmapAsync().get();
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
// The band carries KillerPDF's own 12% line-height pad (SelectionBounds grows
// every quad by exactly that), because the OCR word boxes hug the glyphs: a
// press between two lines' boxes or on a tall letter's headroom is ON that text
// for the user, and the highlight must arm there - not hint "no text".
static bool TtOverText(const TxtPage& pt, float x, float y) {
    const float slop = 2.0f;   // DIPs
    for (const TxtLine& ln : pt.lines) {
        float pad = (ln.bottom - ln.top) * 0.12f;
        if (y <= ln.bottom + pad + slop && y >= ln.top - pad - slop &&
            x >= ln.left - slop && x <= ln.right + slop)
            return true;
    }
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
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // the toolbar's selected tool is
                                                        // the focused pane's tool
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
        D.prot.clear(); D.pflip.clear();
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

// ---------------- file cluster (KillerPDF's FileOperations.cs) ----------------
// Close / Open / Save, reduced to docsy's single-document model: a pane IS
// KillerPDF's tab.

// New_Click -> NewDocument: a blank one-page PDF opens temp-backed with the
// suggested name "Untitled.pdf" - the first Save asks where to keep it, which
// is SaveDoc's no-real-home -> Save As branch. The writer is the same minimal
// PDF the Flatten path emits; a blank page is its MediaBox and nothing else.
static void NewBlank() {
    static int seq = 0;
    wchar_t tmp[MAX_PATH * 2];
    GetTempPathW((DWORD)_countof(tmp), tmp);
    wchar_t file[MAX_PATH * 2];
    swprintf(file, MAX_PATH * 2, L"%sUntitled%d.pdf", tmp, ++seq);
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { SetStatusHold(L"New document failed: the file could not be written"); return; }
    std::string head = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    DWORD wr = 0;
    WriteFile(f, head.data(), (DWORD)head.size(), &wr, nullptr);
    long long pos = (long long)head.size();
    auto emit = [&](const std::string& s) { WriteFile(f, s.data(), (DWORD)s.size(), &wr, nullptr); pos += (long long)s.size(); };
    long long catOff = pos;
    emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    long long pagesOff = pos;
    emit("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    long long pageOff = pos;
    emit("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n");
    long long xref = pos;
    std::string xr = "xref\n0 4\n0000000000 65535 f \n";
    auto off10 = [](long long v) { char b[16]; snprintf(b, 16, "%010lld", v); return std::string(b); };
    xr += off10(catOff) + " 00000 n \n" + off10(pagesOff) + " 00000 n \n" + off10(pageOff) + " 00000 n \n";
    xr += "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    emit(xr);
    CloseHandle(f);
    if (Open(DVF(), file)) SetStatusHold(L"New blank document");
}

// CloseFile_Click -> CloseTab: the focused pane's document leaves, its
// annotations and find results die with it - the same teardown Open()
// gives a replaced document, landing on KillerPDF's empty state.
static void CloseDoc() {
    DocState& D = DVF();
    if (!D.n) return;
    if (g.tedit) CommitTextEdit();
    ClearCache(D);
    D.doc = nullptr; D.n = 0; D.pw.clear(); D.ph.clear();
    D.name.clear(); D.path.clear();
    D.sx = D.sy = 0; D.zoom = 1.0; D.fitMode = 2; D.cur = -1;
    D.txt.clear(); D.annos.clear(); D.prot.clear(); D.pflip.clear();
    D.tool = TOOL_SELECT;
    g.annSelPage = -1; g.annSelIdx = -1;
    SelClear(D);
    bool mine = fd.pane == (&D == &g.dB);
    if (mine && fd.open) {
        if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
        if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
        fd = Find{};
    }
    {                                          // the sidebar shows the focused pane: empty now
        for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
        sd.th.clear(); sd.scroll = 0; sd.cur = -1; sd.hold = 0; sd.tw = 0;
        sd.cw = 0; sd.ch = 0; sd.docH = 0;
    }
    LayoutMain();
    InvalidateRect(g.main, nullptr, FALSE);
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
    RailRepaint();
    SetStatusHold(L"Document closed");
}

// The shared write-out: every page renders at ~150dpi with the annotations
// composited - the same paint the screen shows - and lands as JPEG pages in a
// minimal PDF. SaveDoc's writer is the only emitter.
static bool WriteAnnotatedPdf(DocState& D, const std::wstring& file, std::wstring& err) {
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"the file could not be written"; return false; }
    auto pageJpeg = [&](int i, std::vector<BYTE>& out) -> bool {
        int w = std::clamp((int)std::lround(D.pw[i] * 2.0f), 64, 2400);
        int h = max(1, (int)std::lround((float)w * D.ph[i] / max(1.f, D.pw[i])));
        HBITMAP b = GetBmp(D, i, w, h);
        if (!b) return false;
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ ob = SelectObject(mdc, b);
        float k = (float)w / max(1.f, D.pw[i]);
        PaintAnnots(mdc, D, i, 0, 0, k);   // annotations burn in
        CLSID jpg{}; UINT cnt = 0, bytes = 0;
        Gdiplus::GetImageEncodersSize(&cnt, &bytes);
        std::vector<BYTE> buf(bytes);
        auto* enc = (Gdiplus::ImageCodecInfo*)buf.data();
        Gdiplus::GetImageEncoders(cnt, bytes, enc);
        for (UINT e = 0; e < cnt; e++) if (!lstrcmpW(enc[e].MimeType, L"image/jpeg")) jpg = enc[e].Clsid;
        Gdiplus::Bitmap bm(b, nullptr);
        IStream* os = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &os) != S_OK) { SelectObject(mdc, ob); DeleteDC(mdc); return false; }
        bool ok = false;
        if (bm.Save(os, &jpg, nullptr) == Gdiplus::Ok) {
            LARGE_INTEGER z1{}; z1.QuadPart = 0;
            os->Seek(z1, STREAM_SEEK_SET, nullptr);
            STATSTG st2{};
            os->Stat(&st2, STATFLAG_NONAME);
            ULARGE_INTEGER sz = st2.cbSize;
            out.resize((size_t)sz.QuadPart);
            ULONG rd = 0;
            ok = os->Read(out.data(), (ULONG)out.size(), &rd) == S_OK && rd == out.size();
        }
        os->Release();
        SelectObject(mdc, ob); DeleteDC(mdc);
        return ok && !out.empty();
    };
    std::string head = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    DWORD wr = 0;
    WriteFile(f, head.data(), (DWORD)head.size(), &wr, nullptr);
    long long pos = (long long)head.size();
    auto emit = [&](const std::string& s) { WriteFile(f, s.data(), (DWORD)s.size(), &wr, nullptr); pos += (long long)s.size(); };
    long long catOff = pos;
    emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    long long pagesOff = pos;
    std::string kids;
    for (int i = 0; i < D.n; i++) kids += std::to_string(3 + i * 2) + " 0 R ";
    emit("2 0 obj\n<< /Type /Pages /Kids [ " + kids + "] /Count " + std::to_string(D.n) + " >>\nendobj\n");
    std::vector<long long> pageOff(D.n), contOff(D.n), imgOff(D.n);
    bool failed = false;
    for (int i = 0; i < D.n && !failed; i++) {
        std::vector<BYTE> jpg;
        if (!pageJpeg(i, jpg)) { failed = true; break; }
        float wpt = D.pw[i] * 2.0f, hpt = D.ph[i] * 2.0f;   // the 2x raster's DIP size, PDF points
        char hb2[256];
        pageOff[i] = pos;
        snprintf(hb2, 256, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Contents %d 0 R /Resources << /XObject << /Im %d 0 R >> >> >>\nendobj\n",
                 3 + i * 2, wpt, hpt, 4 + i * 2, 5 + i * 2);
        emit(hb2);
        contOff[i] = pos;
        std::string cs = "q\n" + std::to_string(wpt).substr(0, 8) + " 0 0 " + std::to_string(hpt).substr(0, 8) + " 0 0 cm\n/Im Do\nQ\n";
        emit(std::to_string(4 + i * 2) + " 0 obj\n<< /Length " + std::to_string(cs.size()) + " >>\nstream\n" + cs + "endstream\nendobj\n");
        imgOff[i] = pos;
        std::string ih = std::to_string(5 + i * 2) + " 0 obj\n<< /Type /XObject /Subtype /Image /Width " +
            std::to_string(std::clamp((int)std::lround(D.pw[i] * 2.0f), 64, 2400)) +
            " /Height " + std::to_string(max(1, (int)std::lround((float)std::clamp((int)std::lround(D.pw[i] * 2.0f), 64, 2400) * D.ph[i] / max(1.f, D.pw[i])))) +
            " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length " + std::to_string(jpg.size()) + " >>\nstream\n";
        emit(ih);
        WriteFile(f, jpg.data(), (DWORD)jpg.size(), &wr, nullptr);
        pos += (long long)jpg.size();
        emit("\nendstream\nendobj\n");
    }
    if (failed) {
        CloseHandle(f);
        err = L"a page could not be rendered";
        return false;
    }
    long long xref = pos;
    std::string xr = "xref\n0 " + std::to_string(3 + D.n * 2) + "\n0000000000 65535 f \n";
    auto off10 = [](long long v) { char b[16]; snprintf(b, 16, "%010lld", v); return std::string(b); };
    xr += off10(catOff) + " 00000 n \n" + off10(pagesOff) + " 00000 n \n";
    for (int i = 0; i < D.n; i++)
        xr += off10(pageOff[i]) + " 00000 n \n" + off10(contOff[i]) + " 00000 n \n" + off10(imgOff[i]) + " 00000 n \n";
    xr += "trailer\n<< /Size " + std::to_string(3 + D.n * 2) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    emit(xr);
    CloseHandle(f);
    return true;
}

// Save_Click: no document -> "Open a file first"; a real path -> the same
// Yes/No/Cancel overwrite confirm (Yes saves in place, No falls to Save As);
// Save As (Ctrl+Shift+S or the chevron) always asks, and adopts the new path.
static void SaveDoc(bool saveAs) {
    DocState& D = DVF();
    if (!D.n) { SetStatusHold(L"Open a file first"); return; }
    if (g.tedit) CommitTextEdit();
    std::wstring file = D.path;
    if (saveAs || file.empty()) {
        wchar_t buf[MAX_PATH * 2] = L"";
        wcsncpy(buf, D.name.c_str(), MAX_PATH * 2 - 1);
        OPENFILENAMEW o{}; o.lStructSize = sizeof o; o.hwndOwner = g.main;
        o.lpstrFilter = L"PDF files (*.pdf)\0*.pdf\0All files\0*.*\0";
        o.lpstrFile = buf; o.nMaxFile = (DWORD)_countof(buf);
        o.lpstrTitle = L"Save PDF As"; o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        o.lpstrDefExt = L"pdf";
        if (!GetSaveFileNameW(&o)) { SetStatusHold(L"Save canceled (no file written)"); return; }
        file = buf;
    } else {
        int choice = MessageBoxW(g.main, (std::wstring(L"Overwrite ") + D.name + L"?").c_str(),
                                 L"Save", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL) { SetStatusHold(L"Save canceled"); return; }
        if (choice == IDNO) { SaveDoc(true); return; }
    }
    std::wstring err;
    if (!WriteAnnotatedPdf(D, file, err)) { SetStatusHold(std::wstring(L"Save failed: ") + err); return; }
    if (file != D.path) {                          // Save As adopts its file (KillerPDF's
        D.path = file;                             // SaveAsWithName lands the doc on it)
        size_t sl = file.find_last_of(L"\\/");
        D.name = sl == std::wstring::npos ? file : file.substr(sl + 1);
        UpdateTitle();
    }
    SetStatusHold(std::wstring(L"Saved to ") + file);
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
    sd.th[pick] = Ent{ b, sd.tw, STH(pick), 0, ++sd.tick };   // b may be null: stays a placeholder, no retry spin
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

// ---------------- annotation tools (the toolbar's tool buttons) ----------------
// KillerPDF's editing tools, in docsy's pane language. The Text tool commits
// through a real child EDIT (the Find query's WS_CLIPCHILDREN contract, at
// KillerPDF's 165pt box and 24pt Segoe UI); the Draw / Line / Shapes tools
// drag in page-DIP space and commit on release with KillerPDF's own discard
// rules (a line needs >3pt of length, a shape >3pt of both dimensions, a
// one-point Draw commits as a round dot); the Highlight tool reuses the
// flowing selection and converts the swept lines on release
// (CommitFlowingHighlight). Each pane keeps its own tool, exactly like
// KillerPDF keeps the editing state per viewer instance.

// The text box commits on focus loss, on any view motion, or on Ctrl+Enter -
// the text lands as an AT_TEXT annotation carrying the bar's full style
// (KillerPDF's BuildCommittedTextAnnotation); empty content discards, exactly
// like KillerPDF. Del inside the box deletes the WHOLE textbox - input and
// frame, nothing left, whatever the selection is (the user's contract; the
// character edits stay with Backspace). Escape backs out: a re-edited box is
// RESTORED, KillerPDF's CancelActiveTextEdit, so backing out never deletes.
static Annot g_teRestore;          // the re-edit's original, parked while the box is
static int g_teRestorePage = -1, g_teRestoreIdx = -1;   // open (CancelActiveTextEdit's
static bool g_teRestoreB = false;  // _reeditOriginal - Escape puts it back, Del drops it)
static void CommitTextEdit(bool commit, bool restore) {
    if (!g.tedit) return;                       // the default argument rides the forward
    HWND e = g.tedit;
    int len = GetWindowTextLengthW(e);
    std::wstring s;
    if (commit && len > 0) {
        s.resize(len);
        GetWindowTextW(e, s.data(), len + 1);
        if (s.find_first_not_of(L" \t\r\n") == std::wstring::npos) s.clear();
    }
    int fpg = g.tePage; bool fpaneB = g.tePaneB;
    g.tedit = nullptr;
    if (g.teBg) { DeleteObject(g.teBg); g.teBg = nullptr; }
    if (GetFocus() == e) SetFocus(g.main);   // the keys go home - parking focus on the view
                                             // starved the main window's shortcuts (Del on
                                             // the selected box included) after every commit
    DestroyWindow(e);
    if (!s.empty()) {
        DocState& D = DV(fpaneB);
        {
            UndoPush(fpaneB);
            Annot a;
            a.type = AT_TEXT;
            a.x = g.teX; a.y = g.teY; a.w = g.teW;
            a.fsize = g.textSize; a.color = g.textRGB; a.alpha = g.textA; a.text = s;
            a.font = g.textFont;
            a.bold = g.textBold; a.italic = g.textItalic;
            a.strike = g.textStrike; a.under = g.textUnder;
            a.spacing = g.textSpacing;
            a.fill = g.textFillRGB; a.fillA = g.textFillA;
            // the box's height, measured at the CURRENT scale (the wrapped line
            // count is what the box showed while editing), then clamped on-page
            float k = (float)(D.zoom * g.dpi);
            HDC m = GetDC(g.view);
            int hpx = max(8, (int)std::lround(a.fsize * k));
            HFONT f = CreateFontW(-hpx, 0, 0, 0, a.bold ? FW_BOLD : FW_NORMAL,
                                  a.italic, a.under, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, a.font.c_str());
            HGDIOBJ of = SelectObject(m, f);
            RECT mr{ 0, 0, (int)(g.teW * k), 10000 };
            DrawTextW(m, s.c_str(), -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            SelectObject(m, of); DeleteObject(f); ReleaseDC(g.view, m);
            a.h = max(24.0f, (mr.bottom - mr.top) / max(0.01f, k));
            a.x = std::clamp(a.x, 0.0f, max(0.0f, (D.n && fpg < (int)D.pw.size() ? D.pw[fpg] : 0) - a.w));
            a.y = std::clamp(a.y, 0.0f, max(0.0f, (D.n && fpg < (int)D.ph.size() ? D.ph[fpg] : 0) - a.h));
            D.annos[fpg].push_back(a);
        }
    }
    else if (restore && g_teRestoreIdx >= 0) {
        // KillerPDF's CancelActiveTextEdit: backing out of a re-edit puts the
        // original annotation back where it sat - Escape is a cancel, never
        // a delete; the snapshot UndoPush took at open stays the undo pair,
        // so no extra entry rides here
        DocState& D = DV(g_teRestoreB);
        std::vector<Annot>& list = D.annos[g_teRestorePage];
        list.insert(list.begin() + std::clamp(g_teRestoreIdx, 0, (int)list.size()), g_teRestore);
    }
    g_teRestoreIdx = -1;   // the stash is spent whichever way the box closed
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// The box's height contract: KillerPDF's TextBox starts one line tall
// (MinHeight 24) and auto-grows as lines wrap, until a corner handle drag
// makes the height free-form. The edit reports its wrapped line count through
// EM_GETLINECOUNT; the fit moves the child and repaints the composite's ring
// band (old ∪ new footprint, the FindPlace contract).
static int g_teLineH = 0;      // one line's px at the box's font
static int g_teMinH = 0;       // KillerPDF's MinHeight 24, scaled
static bool g_teFixedH = false;   // a handle drag made the height free-form
static void TextEditFit() {
    if (!g.tedit || g_teFixedH || g_teLineH <= 0) return;
    int lines = (int)SendMessageW(g.tedit, EM_GETLINECOUNT, 0, 0);
    int wantH = max(g_teMinH, lines * g_teLineH + (int)(4 * g.dpi));
    RECT r; GetWindowRect(g.tedit, &r);
    POINT p0{ r.left, r.top }, p1{ r.right, r.bottom };
    ScreenToClient(g.view, &p0); ScreenToClient(g.view, &p1);
    int curH = p1.y - p0.y;
    if (wantH == curH) return;
    RECT u{ p0.x - 8, p0.y - 8, p1.x + 8, max(p0.y + wantH, p1.y) + 8 };
    MoveWindow(g.tedit, p0.x, p0.y, p1.x - p0.x, wantH, TRUE);
    InvalidateRect(g.view, &u, FALSE);   // the ring + handles ride the composite
}

// The Text tool's box resize: dragging an editing handle reshapes the box
// (opposite corner pinned, KillerPDF's min 40x24); the commit takes the box
// the user left on screen.
static bool g_teResize = false;
static int g_teHandle = 0;
static POINT g_teAnchor{}; static RECT g_teStart{};
static COLORREF g_teTone = RGB(255, 255, 255);   // the editor's sampled tone (the CTLCOLOR brush)

// The live-resize frame paint. The stock EDIT repaints in two visible steps -
// WM_ERASEBKGND floods the client with the tone, WM_PAINT then draws the text
// over it - so at drag speed the text strobes (erase, text, erase, text...),
// and the caret churns on every re-wrap: the "flashing and freaking out" the
// stock control shows while a corner handle drags. KillerPDF's WPF TextBox has
// no such phases (retained composition, one frame per size), so while the
// drag is live this paints the frame itself - the tone fill and the wrapped
// text in ONE pass with the edit's own font and margins - and the caret stays
// hidden until the drag hands the box back. The text re-wraps live at the new
// width, exactly the reference's behavior; on release the real edit repaints
// and the caret returns (KillerPDF's _tehBox?.Focus()).
static void TextEditPaintFrame(HWND h, HDC dc) {
    RECT rc; GetClientRect(h, &rc);
    if (g.teBg) FillRect(dc, &rc, g.teBg);
    else { SetBkColor(dc, g_teTone); ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr); }
    int len = GetWindowTextLengthW(h);
    std::wstring s;
    if (len > 0) { s.resize(len); GetWindowTextW(h, s.data(), len + 1); }
    if (s.empty()) return;
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    HGDIOBJ of = f ? SelectObject(dc, f) : nullptr;
    SetBkColor(dc, g_teTone);
    SetTextColor(dc, g.textRGB);
    DWORD mar = (DWORD)SendMessageW(h, EM_GETMARGINS, 0, 0);
    RECT tr{ rc.left + (int)(short)LOWORD(mar), rc.top + 1,
             rc.right - (int)(short)HIWORD(mar), rc.bottom };
    DrawTextW(dc, s.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    if (f) SelectObject(dc, of);
}

static LRESULT CALLBACK TextEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (g_teResize && m == WM_ERASEBKGND) return 1;   // no erase phase mid-drag
    if (g_teResize && m == WM_PAINT) {                // one atomic frame paint
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        TextEditPaintFrame(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { CommitTextEdit(false, true); return 0; }   // backs out - a
                                     // re-edited box returns (CancelActiveTextEdit)
        if (wp == VK_DELETE) {      // Del deletes the WHOLE textbox - input and frame,
            CommitTextEdit(false);  // nothing left, whatever the selection is; letters
            return 0;               // are Backspace's business
        }
        if (wp == VK_RETURN && (GetKeyState(VK_CONTROL) < 0)) { CommitTextEdit(); return 0; }
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(h, EM_SETSEL, 0, -1); return 0;   // WPF TextBox's Ctrl+A SelectAll - a
        }                                                  // plain EDIT ships no such chord
        if (wp == 'B' && GetKeyState(VK_CONTROL) < 0) {
            g.textBold = !g.textBold;
            ApplyTextStyleToSelection();     // the live box restyles with the shared state
            return 0;
        }
        if (wp == 'I' && GetKeyState(VK_CONTROL) < 0) { g.textItalic = !g.textItalic; ApplyTextStyleToSelection(); return 0; }
        if (wp == 'U' && GetKeyState(VK_CONTROL) < 0) { g.textUnder = !g.textUnder; ApplyTextStyleToSelection(); return 0; }
    }
    return CallWindowProcW(g.teditDef, h, m, wp, lp);
}

// The Text tool's press: re-edit a placed text annotation under the click
// (it leaves the list and comes back when the box closes - and the bar SYNCs
// to the box's own look, KillerPDF's EditTextAtPosition), or open a fresh box
// centered on the hotspot (KillerPDF's PlaceTextBox: left edge on the click,
// first line's center on the I-beam).
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
            if (fx >= a.x - 8 && fx <= a.x + a.w + 8 && fy >= a.y - 10 && fy <= a.y + a.h + 10) {
                UndoPush(b);                 // the re-edit's commit replaces the original
                std::wstring text = a.text;
                float tx = a.x, ty = a.y, tw = a.w;
                // the bar takes the box's look as its new defaults (KillerPDF's sync)
                g.textFont = a.font; g.textSize = a.fsize; g.textSpacing = a.spacing;
                g.textBold = a.bold; g.textItalic = a.italic;
                g.textStrike = a.strike; g.textUnder = a.under;
                g.textRGB = a.color; g.textA = a.alpha;
                g.textFillRGB = a.fill; g.textFillA = a.fillA;
                g_teRestore = a;           // park the original: Escape puts it back,
                g_teRestorePage = pg;      // Del drops it for good (CommitTextEdit's
                g_teRestoreIdx = ai; g_teRestoreB = b;   // restore switch)
                it->second.erase(it->second.begin() + ai);
                TextEditOpen(b, pg, tx, ty, tw, text.c_str());   // defined below
                return;
            }
        }
    }
    // a fresh box: 165pt wide, the first line's center on the I-beam hotspot
    int lineH = (int)std::lround(g.textSize * k * 4.0 / 3.0);
    float half = (lineH / 2.0f) / max(0.01f, k);
    TextEditOpen(b, pg, fx, max(0.0f, fy - half), 165.0f, nullptr);
}

// The editor's background: KillerPDF's TextEditBackground - the box's own
// fill when one is set, else the page tone under a 64-alpha gray (so the
// empty box reads on light and dark pages). An EDIT is opaque, so the page
// pixels under the box are blitted into a tiny DIB and averaged, and the
// gray is blended into that average.
static HBRUSH TextEditBackground(DocState& D, int pg, int x, int y, int w, int h) {
    if (g.textFillA > 0) { g_teTone = g.textFillRGB; return CreateSolidBrush(g.textFillRGB); }
    COLORREF tone = RGB(255, 255, 255);
    HBITMAP b = GetBmp(D, pg, PW(D, pg), PH(D, pg));
    w = std::clamp(w, 1, 400); h = std::clamp(h, 1, 400);
    if (b && w > 0 && h > 0) {
        HDC sdc = CreateCompatibleDC(nullptr);
        HGDIOBJ oldb = SelectObject(sdc, b);
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        BYTE* bits = nullptr;
        HBITMAP tile = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (tile) {
            HDC tdc = CreateCompatibleDC(nullptr);
            HGDIOBJ ot = SelectObject(tdc, tile);
            BitBlt(tdc, 0, 0, w, h, sdc, x, y, SRCCOPY);
            SelectObject(tdc, ot); DeleteDC(tdc);
            long long r = 0, gg = 0, bl = 0; long long n = 0;
            for (int yy = 0; yy < h; yy += 2) {
                const BYTE* row = bits + (size_t)yy * ((w * 4 + 3) & ~3);
                for (int xx = 0; xx < w; xx += max(1, w / 24)) {
                    const BYTE* p2 = row + xx * 4;
                    bl += p2[0]; gg += p2[1]; r += p2[2]; n++;
                }
            }
            if (n > 0) tone = RGB((int)(r / n * 3 / 4 + 32), (int)(gg / n * 3 / 4 + 32),
                                  (int)(bl / n * 3 / 4 + 32));   // 25% of the 128-gray over the page
            DeleteObject(tile);
        }
        SelectObject(sdc, oldb); DeleteDC(sdc);
    }
    g_teTone = tone;
    return CreateSolidBrush(tone);
}

// Opens the box: a real child EDIT over its page position, one green ring +
// four corner handles painted by the composite around it (KillerPDF's
// PrimaryBrush border and resize handles).
static void TextEditOpen(bool b, int pg, float tx, float ty, float tw, const wchar_t* text) {
    CommitTextEdit();                       // one editor at a time (KillerPDF's _activeTextBox)
    DocState& D = DV(b);
    if (!D.n || pg < 0 || pg >= D.n) return;
    g.tePaneB = b; g.tePage = pg; g.teX = tx; g.teY = ty; g.teW = tw;
    float k = (float)(D.zoom * g.dpi);
    int ox, oy;
    if (!PageOrigin(D, b, pg, ox, oy)) return;
    int fontPx = max(8, (int)std::lround(g.textSize * k));
    int lineH = (int)std::lround(fontPx * 4.0 / 3.0);
    int boxW = max(24, (int)std::lround(tw * k));
    int x = PaneX0(b) + ox + (int)std::lround(tx * k);
    int y = oy + (int)std::lround(ty * k);
    // KillerPDF's box: MinHeight 24, ONE line tall at open, auto-grown by
    // TextEditFit as lines wrap - until a corner handle makes the height
    // free-form. ES_AUTOVSCROLL stays as the overflow safety net.
    g_teLineH = lineH;
    g_teMinH = max((int)(24 * g.dpi), lineH);
    g_teFixedH = false;
    int boxH = g_teMinH;
    // clamp into the pane's card so the editor never hides under the chrome
    int cardL = PaneX0(b) + CardX(b), cardR = cardL + CardW(b);
    x = std::clamp(x, cardL, max(cardL, cardR - boxW));
    y = std::clamp(y, 0, max(0, g.ch - boxH));
    HFONT f = CreateFontW(-fontPx, 0, 0, 0, g.textBold ? FW_BOLD : FW_NORMAL,
                          g.textItalic, g.textUnder, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, g.textFont.c_str());
    g.tedit = CreateWindowExW(0, L"EDIT", text ? text : L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                              x, y, boxW, boxH, g.view, nullptr, g.hinst, nullptr);
    if (!g.tedit) return;
    if (g.teBg) DeleteObject(g.teBg);
    g.teBg = TextEditBackground(D, pg, x - PaneX0(b) - ox, y - oy, boxW, boxH);
    SendMessageW(g.tedit, WM_SETFONT, (WPARAM)f, TRUE);   // the font slot rides the edit
    g.teditDef = (WNDPROC)SetWindowLongPtrW(g.tedit, GWLP_WNDPROC, (LONG_PTR)TextEditProc);
    SetFocus(g.tedit);
    SendMessageW(g.tedit, EM_SETSEL, 0, -1);   // KillerPDF opens every box select-all
                                               // (QueueTextEditorFocus selectAll: true) -
                                               // typing over it replaces the text at once
    SetStatusHold(L"Editing text. Change size/color above, Enter to save");
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

// ---------------- undo / redo (KillerPDF's stack) ----------------
// One snapshot per edit operation, bounded to 20 entries (UndoHistoryBudget's
// maximumEntries); redo clears on a fresh edit. A snapshot is the pane's whole
// annotation state - pages carry a handful of small vectors, so
// the copy is trivial next to a page render.
static App::UndoSnap SnapPane(bool b) {
    App::UndoSnap s; s.paneB = b;
    DocState& D = DV(b);
    for (auto& kv : D.annos) if (!kv.second.empty()) s.annos[kv.first] = kv.second;
    return s;
}
static void UndoPush(bool b) {
    g.undo.push_back(SnapPane(b));
    if (g.undo.size() > 20) g.undo.erase(g.undo.begin());
    g.redo.clear();                        // fresh edits invalidate the redo chain
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // Undo/Redo re-arm
}
static void UndoOp() {
    if (g.tedit) CommitTextEdit();
    if (g.undo.empty()) { SetStatusHold(L"Nothing to undo"); return; }
    App::UndoSnap e = g.undo.back();
    g.undo.pop_back();
    g.redo.push_back(SnapPane(e.paneB));
    if (g.redo.size() > 20) g.redo.erase(g.redo.begin());
    DocState& D = DV(e.paneB);
    D.annos = e.annos;
    g.annSelPage = -1; g.annSelIdx = -1;
    SetStatusHold(L"Undone");
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
}
static void RedoOp() {
    if (g.tedit) CommitTextEdit();
    if (g.redo.empty()) { SetStatusHold(L"Nothing to redo"); return; }
    App::UndoSnap e = g.redo.back();
    g.redo.pop_back();
    g.undo.push_back(SnapPane(e.paneB));
    if (g.undo.size() > 20) g.undo.erase(g.undo.begin());
    DocState& D = DV(e.paneB);
    D.annos = e.annos;
    g.annSelPage = -1; g.annSelIdx = -1;
    SetStatusHold(L"Redone");
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
}

// The Clear All button (ClearAllAnnotations_Click): a full snapshot lands on
// the undo stack, then every annotation leaves - marks and gestures untouched.
static void ClearAllAnnos() {
    DocState& D = DVF();
    if (!D.n) return;
    if (g.tedit) CommitTextEdit();
    int total = 0;
    for (auto& kv : D.annos) total += (int)kv.second.size();
    if (!total && !g.annInk && !g.annPoly) { SetStatusHold(L"No annotations to clear"); return; }
    UndoPush(FocusB());
    D.annos.clear();
    PolyCancel(g.view);
    g.annSelPage = -1; g.annSelIdx = -1;
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
    SetStatusHold(L"Cleared all annotations (" + std::to_wstring(total) + L")");
}

// ---- the Shapes tool's polygon session (ShapeKind.Polygon) ----
static void PolyCancel(HWND h) {
    if (!g.annPoly) return;
    g.annPoly = false;
    g.annPX.clear(); g.annPY.clear();
    if (h) InvalidateRect(h, nullptr, FALSE);
}
static void PolyCommit(HWND h) {
    if (!g.annPoly) return;
    DocState& D = DV(g.annPaneB);
    g.annPoly = false;
    if (g.annPX.size() >= 3) {
        UndoPush(g.annPaneB);
        Annot a;
        a.type = AT_INK; a.width = g.drawWidth; a.color = g.drawColor; a.alpha = g.drawOpacity;
        a.ix = g.annPX; a.iy = g.annPY;
        a.ix.push_back(g.annPX[0]); a.iy.push_back(g.annPY[0]);   // closed: first point repeats
        if (g.shapeFill) { a.fill = g.drawColor; a.fillA = (BYTE)max(20, g.drawOpacity / 2); }
        D.annos[g.annPage].push_back(a);
    }
    g.annPX.clear(); g.annPY.clear();
    if (h) InvalidateRect(h, nullptr, FALSE);
}

// ---- the erasers ----
// Rectangle subtraction, KillerPDF's SubtractRect: the parts of a NOT covered
// by b, as up to four strips; [a] when they miss, empty when b swallows a.
static void SubtractRect(const FRect& a, const FRect& bx, std::vector<FRect>& out) {
    float l = max(a.x, bx.x), r2 = min(a.x + a.w, bx.x + bx.w);
    float t = max(a.y, bx.y), b2 = min(a.y + a.h, bx.y + bx.h);
    if (r2 <= l || b2 <= t) { out.push_back(a); return; }
    if (t > a.y) out.push_back({ a.x, a.y, a.w, t - a.y });
    if (b2 < a.y + a.h) out.push_back({ a.x, b2, a.w, a.y + a.h - b2 });
    if (l > a.x) out.push_back({ a.x, t, l - a.x, b2 - t });
    if (r2 < a.x + a.w) out.push_back({ r2, t, a.x + a.w - r2, b2 - t });
}

// The Draw bar's brush eraser (KillerPDF's ErasePartial with a brush path):
// ink strokes SPLIT at the covered points (pieces keep their look, runs under
// two points drop), and highlights CARVE - the brush's circular footprint,
// widened around every gesture sample, is subtracted from the covered rects
// (HighlightErase's own smooth-hole mechanic, blocky at rect granularity).
// rad is the brush radius in page points.
static void EraseBrushAt(DocState& D, int page, float x, float y, float rad) {
    auto it = D.annos.find(page);
    if (it == D.annos.end()) return;
    bool changed = false;
    float r2 = rad * rad;
    std::vector<Annot> out;
    out.reserve(it->second.size());
    for (Annot& a : it->second) {
        if (a.type == AT_INK) {
            std::vector<float> rx, ry;
            auto flush = [&]() {
                if (rx.size() >= 2) { Annot n = a; n.ix = rx; n.iy = ry; out.push_back(n); }
                else if (!rx.empty()) changed = true;   // a lone survivor point drops
                rx.clear(); ry.clear();
            };
            for (size_t i = 0; i < a.ix.size(); i++) {
                float dx = a.ix[i] - x, dy = a.iy[i] - y;
                if (dx * dx + dy * dy <= r2) { changed = true; flush(); }
                else { rx.push_back(a.ix[i]); ry.push_back(a.iy[i]); }
            }
            flush();
        } else if (a.type == AT_HIGHLIGHT) {
            bool touch = false;
            for (const FRect& r : a.lines)
                if (x >= r.x - rad && x <= r.x + r.w + rad && y >= r.y - rad && y <= r.y + r.h + rad) { touch = true; break; }
            if (!touch) { out.push_back(a); continue; }
            changed = true;
            std::vector<FRect> pieces;
            for (const FRect& r : a.lines) {
                std::vector<FRect> cur{ r };
                // subtract this gesture's own footprint box first (the sample
                // that just landed), then the recent samples the stroke
                // passed - older ones were already carved out of this rect by
                // their own moves, so a bounded window keeps every pass O(1)
                std::vector<FRect> boxes;
                boxes.push_back({ x - rad, y - rad, rad * 2, rad * 2 });
                size_t n = g.annIX.size(), lo = n > 65 ? n - 65 : 0;
                for (size_t s2 = lo; s2 + 1 < n; s2++) {
                    FRect bx{ g.annIX[s2] - rad, g.annIY[s2] - rad, rad * 2, rad * 2 };
                    if (bx.x < r.x + r.w && bx.x + bx.w > r.x &&
                        bx.y < r.y + r.h && bx.y + bx.h > r.y) boxes.push_back(bx);
                }
                for (const FRect& bx : boxes) {
                    std::vector<FRect> next;
                    for (const FRect& c : cur) SubtractRect(c, bx, next);
                    cur = std::move(next);
                    if (cur.empty()) break;
                }
                for (const FRect& pc : cur)
                    if (pc.w >= 1 && pc.h >= 1) pieces.push_back(pc);
            }
            if (!pieces.empty()) {
                Annot n = a; n.lines = pieces;
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const FRect& r : pieces) {
                    x0 = min(x0, r.x); y0 = min(y0, r.y);
                    x1 = max(x1, r.x + r.w); y1 = max(y1, r.y + r.h);
                }
                n.x = x0; n.y = y0; n.w = x1 - x0; n.h = y1 - y0;
                out.push_back(n);
            }
        } else out.push_back(a);
    }
    if (changed) {
        it->second = std::move(out);
        if (it->second.empty()) D.annos.erase(it);
    }
}

// The Highlight bar's Annotation eraser (the drag box): highlights subtract
// the box (up to four strips per rect, pieces under 1x1 dropped) and ink
// strokes SPLIT at the covered points - KillerPDF's ErasePartial hands the
// region to both vector families.
static void EraseBox(HWND h, DocState& D, int page, FRect box) {
    auto it = D.annos.find(page);
    if (it == D.annos.end()) return;
    bool changed = false;
    std::vector<Annot> out;
    for (const Annot& a : it->second) {
        if (a.type == AT_HIGHLIGHT) {
            std::vector<FRect> pieces;
            for (const FRect& r : a.lines) SubtractRect(r, box, pieces);
            pieces.erase(std::remove_if(pieces.begin(), pieces.end(),
                         [](const FRect& q) { return q.w < 1 || q.h < 1; }), pieces.end());
            if (pieces.size() != a.lines.size()) changed = true;
            if (!pieces.empty()) {
                Annot n = a; n.lines = pieces;
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const FRect& r : pieces) {
                    x0 = min(x0, r.x); y0 = min(y0, r.y);
                    x1 = max(x1, r.x + r.w); y1 = max(y1, r.y + r.h);
                }
                n.x = x0; n.y = y0; n.w = x1 - x0; n.h = y1 - y0;
                out.push_back(n);
            }
        } else if (a.type == AT_INK) {
            std::vector<float> rx, ry;
            auto flush = [&]() {
                if (rx.size() >= 2) { Annot n = a; n.ix = rx; n.iy = ry; out.push_back(n); }
                else if (!rx.empty()) changed = true;   // a lone survivor point drops
                rx.clear(); ry.clear();
            };
            for (size_t i = 0; i < a.ix.size(); i++) {
                bool in = a.ix[i] >= box.x && a.ix[i] <= box.x + box.w &&
                          a.iy[i] >= box.y && a.iy[i] <= box.y + box.h;
                if (in) { changed = true; flush(); }
                else { rx.push_back(a.ix[i]); ry.push_back(a.iy[i]); }
            }
            flush();
        } else out.push_back(a);
    }
    if (changed) {
        it->second = std::move(out);
        if (it->second.empty()) D.annos.erase(it);
        InvalidateRect(h, nullptr, FALSE);
    }
}

// A selection-resize's new geometry, per type (KillerPDF's resize path: the
// opposite corner anchors, min 4 units; ink scales its points, a multi-line
// highlight scales its quads inside the new union, boxes take the new rect).
static void ResizeAnnotTo(Annot& a, float nx, float ny, float nw, float nh) {
    float bx0, by0, bx1, by1;
    AnnotBounds(a, bx0, by0, bx1, by1);
    float ow = max(1.0f, bx1 - bx0), oh = max(1.0f, by1 - by0);
    if (a.type == AT_HIGHLIGHT) {
        for (FRect& r : a.lines) {
            r.x = nx + (r.x - bx0) * nw / ow;
            r.y = ny + (r.y - by0) * nh / oh;
            r.w = r.w * nw / ow; r.h = r.h * nh / oh;
        }
        a.x = nx; a.y = ny; a.w = nw; a.h = nh;
    } else if (a.type == AT_INK) {
        for (size_t i = 0; i < a.ix.size(); i++) {
            a.ix[i] = nx + (a.ix[i] - bx0) * nw / ow;
            a.iy[i] = ny + (a.iy[i] - by0) * nh / oh;
        }
    } else { a.x = nx; a.y = ny; a.w = nw; a.h = nh; }
}

// The press under a drawing tool. Returns true when the tool consumed it.
static bool ToolDown(HWND h, bool b, POINT p) {
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
        // per-line annotations. The bar's Annotation eraser swaps the gesture
        // for a carve box. A press with no text under it hints instead of
        // silently laying down a rect - KillerPDF's own status line.
        if (g.hlErase) {
            if (!onPage) return true;
            g.annInk = true; g.annKind = -1; g.annArm = false;
            g.annPaneB = b; g.annPage = pg; g.annSX = D.sx; g.annSY = D.sy;
            g.annStart = g.annLast = p; g.annFX = fx; g.annFY = fy;
            g.annIX.assign(2, fx); g.annIY.assign(2, fy);
            return true;
        }
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
            SetStatusHold(L"No text here. Shapes is on O");
        }
        return true;
    }
    if (tool == TOOL_SHAPE && g.annPoly && g.annPaneB == b && onPage) {
        // the polygon session: a click near the first vertex closes (snap 8),
        // any other click drops a vertex
        float dx0 = fx - g.annPX[0], dy0 = fy - g.annPY[0];
        if (g.annPX.size() >= 3 && dx0 * dx0 + dy0 * dy0 <= 64.0f) { PolyCommit(h); return true; }
        g.annPX.push_back(fx); g.annPY.push_back(fy);
        g.annFX = fx; g.annFY = fy; g.annLast = p;
        InvalidateRect(h, nullptr, FALSE);
        return true;
    }
    if (!onPage) return true;                 // a drawing press off the page does nothing
    if (tool == TOOL_TEXT) { TextPlace(b, p); return true; }
    if (tool == TOOL_DRAW && g.drawErase) {
        g.annInk = true; g.annKind = -2; g.annArm = true;   // annArm: undo not pushed yet
        g.annPaneB = b; g.annPage = pg; g.annSX = D.sx; g.annSY = D.sy;
        g.annStart = g.annLast = p; g.annFX = fx; g.annFY = fy;
        g.annIX.clear(); g.annIY.clear();
        g.annIX.push_back(fx); g.annIY.push_back(fy);
        return true;
    }
    g.annInk = true; g.annKind = tool; g.annArm = true;
    g.annPaneB = b; g.annPage = pg; g.annSX = D.sx; g.annSY = D.sy;
    g.annStart = g.annLast = p; g.annFX = fx; g.annFY = fy;
    g.annIX.clear(); g.annIY.clear();
    g.annIX.push_back(fx); g.annIY.push_back(fy);
    if (tool != TOOL_DRAW) { g.annIX.push_back(fx); g.annIY.push_back(fy); }   // the tracked end
    if (tool == TOOL_SHAPE && g.shapeKind == 2) {
        // the polygon's first vertex lands on press; the drag tracks the rubber
        // band and the release keeps the session alive (KillerPDF's click flow)
        g.annPoly = true;
        g.annPX.assign(1, fx); g.annPY.assign(1, fy);
    }
    return true;
}

// The drag under a stroke or a move gesture. p is view pixels; the stroke
// lives in the press-time scroll's DIP space (a scroll mid-stroke cannot skew
// it, KillerPDF pins the gesture to its canvas the same way).
static void ToolMove(HWND h, POINT p) {
    DocState& D = DV(g.annPaneB);
    float k = (float)(D.zoom * g.dpi);
    if (g.annResize) {                        // a corner handle drags: opposite corner anchors
        auto it = D.annos.find(g.annPage);
        if (it != D.annos.end() && g.annSelIdx >= 0 && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            float ax0, ay0, ax1, ay1; AnnotBounds(a, ax0, ay0, ax1, ay1);
            RECT was{ PaneX0(g.annPaneB) + (int)(ax0 * k) - 4, (int)(ay0 * k) - 4,
                      (int)(ax1 * k) + 5, (int)(ay1 * k) + 5 };
            POINT q{ p.x - PaneX0(g.annPaneB) - CardX(g.annPaneB), p.y };
            int pg2; float fx, fy;
            if (ViewToPageAt(D, q, CardW(g.annPaneB), g.ch, g.annSX, g.annSY, pg2, fx, fy)) {
                float axc = (g.annHandle == 0 || g.annHandle == 3) ? ax1 : ax0;
                float ayc = (g.annHandle < 2) ? ay1 : ay0;
                float nw = max(4.0f, fabsf(fx - axc)), nh = max(4.0f, fabsf(fy - ayc));
                float nx0 = fx < axc ? axc - nw : axc, ny0 = fy < ayc ? ayc - nh : ayc;
                ResizeAnnotTo(a, nx0, ny0, nw, nh);
                g.annMoved = true;
            }
            float bx0, by0, bx1, by1; AnnotBounds(a, bx0, by0, bx1, by1);
            RECT now{ PaneX0(g.annPaneB) + (int)(bx0 * k) - 4, (int)(by0 * k) - 4,
                      (int)(bx1 * k) + 5, (int)(by1 * k) + 5 };
            RECT u{ min(was.left, now.left), min(was.top, now.top),
                    max(was.right, now.right), max(was.bottom, now.bottom) };
            InvalidateRect(h, &u, FALSE);
        }
        return;
    }
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
                g.annLast = p; g.annMoved = true;
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
    if (!g.annInk && !g.annPoly) return;
    if (g.annPoly) {                          // the rubber band tracks the pointer
        POINT q{ p.x - PaneX0(g.annPaneB) - CardX(g.annPaneB), p.y };
        int pg; float fx, fy;
        if (ViewToPageAt(D, q, CardW(g.annPaneB), g.ch, g.annSX, g.annSY, pg, fx, fy)) {
            g.annFX = fx; g.annFY = fy; g.annLast = p;
            InvalidateRect(h, nullptr, FALSE);
        }
        return;
    }
    POINT q{ p.x - PaneX0(g.annPaneB) - CardX(g.annPaneB), p.y };
    int pg; float fx, fy;
    if (!ViewToPageAt(D, q, CardW(g.annPaneB), g.ch, g.annSX, g.annSY, pg, fx, fy)) return;
    if (g.annKind == -2) {                    // the brush eraser erases as it moves
        if (g.annArm) { UndoPush(g.annPaneB); g.annArm = false; }   // one entry per gesture
        AnnInval(h);
        // Densify the segment KillerPDF's DensifyPolyline covers: the eraser
        // works on the PATH, not its samples, so a fast drag leaves no gaps.
        float rad = g.drawWidth * 0.5f;
        float lx = g.annIX.back(), ly = g.annIY.back();
        float dx = fx - lx, dy = fy - ly;
        float dist = sqrtf(dx * dx + dy * dy);
        float step = max(1.0f, rad * 0.5f);
        int steps = (int)(dist / step);
        for (int s2 = 1; s2 <= steps; s2++)
            EraseBrushAt(D, g.annPage, lx + dx * s2 / steps, ly + dy * s2 / steps, rad);
        g.annIX.push_back(fx); g.annIY.push_back(fy);
        EraseBrushAt(D, g.annPage, fx, fy, rad);
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    if (g.annKind == TOOL_DRAW) {
        size_t ln = g.annIX.size();
        if (!ln || fabsf(fx - g.annIX[ln - 1]) > 0.01f || fabsf(fy - g.annIY[ln - 1]) > 0.01f) {
            AnnInval(h);                      // old footprint first: bounds BEFORE the append
            g.annIX.push_back(fx); g.annIY.push_back(fy);
            AnnInval(h);                      // then the new one - the two regions join in
        }                                     // one paint, so the stroke never trails ghosts
        return;
    }
    // line / shape / eraser box: the second point tracks the
    // pointer (KillerPDF's preview). The Level checkbox snaps the line's end
    // to the nearest axis - horizontal when the drag is wider than tall.
    g.annArm = false;
    if (g.annKind == TOOL_LINE && g.lineLevel) {
        if (fabsf(fx - g.annIX[0]) >= fabsf(fy - g.annIY[0])) fy = g.annIY[0];
        else fx = g.annIX[0];
    }
    if (g.annIX.size() < 2) { g.annIX.push_back(fx); g.annIY.push_back(fy); }
    else { g.annIX[1] = fx; g.annIY[1] = fy; }
    AnnInval(h);
}

// The release: KillerPDF's commit rules, verbatim. A line needs >3pt of
// length, a shape >3pt of both dimensions (smaller drags
// cancel with KillerPDF's own status line), a one-point Draw commits as a dot
// (two points 0.25 apart meeting in the round caps). Shapes honor the bar's
// style picker: the filled box commits as a full-alpha block, outline shapes
// as closed ink strokes, the ellipse as KillerPDF's 64 segments with the
// stroke color at half alpha filling the inside when Fill is on.
static void ToolUp(HWND h) {
    DocState& D = DV(g.annPaneB);
    if (g.annResize) {                        // the resize applied live; text reflows once
        g.annResize = false;
        auto it = D.annos.find(g.annPage);
        if (it != D.annos.end() && g.annSelIdx >= 0 && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            if (a.type == AT_TEXT && a.text.size() > 0) {
                float k = (float)(D.zoom * g.dpi);
                HDC m = GetDC(g.view);
                int hpx = max(8, (int)std::lround(a.fsize * k));
                HFONT f = CreateFontW(-hpx, 0, 0, 0, a.bold ? FW_BOLD : FW_NORMAL,
                                      a.italic, a.under, 0, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, a.font.c_str());
                HGDIOBJ of = SelectObject(m, f);
                RECT mr{ 0, 0, (int)(a.w * k), 10000 };
                DrawTextW(m, a.text.c_str(), -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
                SelectObject(m, of); DeleteObject(f); ReleaseDC(g.view, m);
                a.h = max(24.0f, (mr.bottom - mr.top) / max(0.01f, k));
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return;
    }
    if (g.annMove) {
        g.annMove = false;
        return;
    }
    if (g.annPoly) return;                    // the polygon session waits for its closing click
    if (!g.annInk) return;
    g.annInk = false;
    int kind = g.annKind;
    auto& v = D.annos[g.annPage];
    if (kind == -2) {                         // the brush eraser: a click is a dot erase -
                                              // KillerPDF's Covered() with a one-point path
        if (g.annArm) {                       // no move fired: the drag path never erased
            UndoPush(g.annPaneB);
            g.annArm = false;
            EraseBrushAt(D, g.annPage, g.annIX[0], g.annIY[0], g.drawWidth * 0.5f);
            InvalidateRect(h, nullptr, FALSE);
        }
    } else if (kind == -1) {                  // the highlight eraser's carve box
        if (g.annIX.size() >= 2) {
            float x0 = min(g.annIX[0], g.annIX[1]), x1 = max(g.annIX[0], g.annIX[1]);
            float y0 = min(g.annIY[0], g.annIY[1]), y1 = max(g.annIY[0], g.annIY[1]);
            if (x1 - x0 > 3 && y1 - y0 > 3) {
                UndoPush(g.annPaneB);
                EraseBox(h, D, g.annPage, FRect{ x0, y0, x1 - x0, y1 - y0 });
            }
        }
    } else if (kind == TOOL_LINE) {
        if (g.annIX.size() >= 2 && (fabsf(g.annIX[1] - g.annIX[0]) > 3 || fabsf(g.annIY[1] - g.annIY[0]) > 3)) {
            UndoPush(g.annPaneB);
            Annot a; a.type = AT_INK;
            a.width = g.drawWidth; a.color = g.drawColor; a.alpha = g.drawOpacity;
            a.ix = g.annIX; a.iy = g.annIY;
            v.push_back(a);
        }
    } else if (kind == TOOL_SHAPE && g.annIX.size() >= 2) {
        float x0 = min(g.annIX[0], g.annIX[1]), x1 = max(g.annIX[0], g.annIX[1]);
        float y0 = min(g.annIY[0], g.annIY[1]), y1 = max(g.annIY[0], g.annIY[1]);
        if (x1 - x0 > 3 && y1 - y0 > 3) {
            UndoPush(g.annPaneB);
            if (g.shapeKind == 0 && g.shapeFill) {
                // the filled box commits as KillerPDF's HighlightAnnotation:
                // the stroke color at FULL alpha
                Annot a; a.type = AT_HIGHLIGHT; a.color = g.drawColor; a.alpha = 255;
                a.lines.push_back(FRect{ x0, y0, x1 - x0, y1 - y0 });
                a.x = x0; a.y = y0; a.w = x1 - x0; a.h = y1 - y0;
                v.push_back(a);
            } else {
                Annot a; a.type = AT_INK;
                a.width = g.drawWidth; a.color = g.drawColor; a.alpha = g.drawOpacity;
                if (g.shapeKind == 0) {           // the outline box: four corners, closed
                    a.ix = { x0, x1, x1, x0, x0 };
                    a.iy = { y0, y0, y1, y1, y0 };
                } else {                          // the ellipse: 64 segments, closed
                    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
                    float rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
                    for (int k2 = 0; k2 <= 64; k2++) {
                        float ang = k2 * 6.28318530717959f / 64.0f;
                        a.ix.push_back(cx + rx * cosf(ang));
                        a.iy.push_back(cy + ry * sinf(ang));
                    }
                    if (g.shapeFill) { a.fill = g.drawColor; a.fillA = (BYTE)max(20, g.drawOpacity / 2); }
                }
                v.push_back(a);
            }
        }
    } else if (kind == TOOL_DRAW) {
        if (!g.annIX.empty()) {
            if (g.annIX.size() == 1) {        // the dot: 0.25pt to the right, caps meet
                g.annIX.push_back(g.annIX[0] + 0.25f); g.annIY.push_back(g.annIY[0]);
            }
            UndoPush(g.annPaneB);
            Annot a; a.type = AT_INK;
            a.width = g.drawWidth; a.color = g.drawColor; a.alpha = g.drawOpacity;
            a.ix = g.annIX; a.iy = g.annIY;
            v.push_back(a);
        }
    }
    g.annIX.clear(); g.annIY.clear();
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // Clear All may have armed
    InvalidateRect(h, nullptr, FALSE);        // the commit repaints the page's overlay
}

// The Select tool's press: the LIVE selection's corner handles answer first
// (resize, KillerPDF's NW/NE/SE/SW squares), then the topmost annotation under
// the pointer is chosen (KillerPDF hit-tests top-down) and armed for the drag;
// a miss drops the selection and lets the flowing text path arm. Returns true
// on a hit.
static bool SelectAnnotAt(HWND h, bool b, POINT p) {
    DocState& D = DV(b);
    POINT q{ p.x - PaneX0(b) - CardX(b), p.y };
    int pg; float fx, fy;
    if (!ViewToPageAt(D, q, CardW(b), g.ch, D.sx, D.sy, pg, fx, fy)) return false;
    float k = (float)(D.zoom * g.dpi);
    if (g.annSelPage >= 0 && g.annSelB == b && g.annSelIdx >= 0) {
        auto it = D.annos.find(g.annSelPage);
        if (it != D.annos.end() && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            float x0, y0, x1, y1; AnnotBounds(a, x0, y0, x1, y1);
            int ox, oy;
            if (PageOrigin(D, b, g.annSelPage, ox, oy)) {
                int hs = (int)(14 * g.dpi);
                for (int hd = 0; hd < 4; hd++) {
                    float hx = (hd == 0 || hd == 3) ? x0 : x1;
                    float hy = (hd < 2) ? y0 : y1;
                    int cx2 = PaneX0(b) + ox + (int)(hx * k), cy2 = oy + (int)(hy * k);
                    if (p.x >= cx2 - hs / 2 && p.x < cx2 - hs / 2 + hs &&
                        p.y >= cy2 - hs / 2 && p.y < cy2 - hs / 2 + hs) {
                        UndoPush(b);       // one entry covers the whole resize gesture
                        g.annResize = true; g.annHandle = hd; g.annMoved = true;
                        g.annPaneB = b; g.annPage = g.annSelPage;
                        g.annSX = D.sx; g.annSY = D.sy;
                        g.annStart = g.annLast = p;
                        return true;
                    }
                }
            }
        }
    }
    auto it = D.annos.find(pg);
    if (it == D.annos.end()) {
        if (g.annSelPage >= 0) {
            g.annSelPage = -1; g.annSelIdx = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return false;
    }
    for (int ai = (int)it->second.size() - 1; ai >= 0; ai--) {
        if (AnnotHit(it->second[ai], fx, fy, k)) {
            UndoPush(b);                   // one entry covers the whole move gesture
            g.annSelB = b; g.annSelPage = pg; g.annSelIdx = ai;
            g.annMove = true; g.annMoved = false;
            g.annPaneB = b; g.annPage = pg;
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

// KillerPDF's SetTool (Shell/ToolSelection.cs), driving the toolbar's tool
// buttons: the tool lands on ONE pane's document state (KillerPDF keeps the
// editing state per viewer instance), a no-document pane re-lands on Select,
// a live text box commits first, an in-progress polygon is abandoned, and the
// view's cursor + brush ring follow on the repaint. The floating bar follows
// the FOCUSED pane's tool: re-clicking the tool that owns the visible bar
// tucks it to its peek strip (ToggleAnnotBarMinimized), picking another bar
// tool rebuilds the bar, Select takes it away. The toolbar repaints too: its
// selected face mirrors the focused pane's tool.
static bool BarToolOwns(int t) {
    return t == TOOL_DRAW || t == TOOL_LINE || t == TOOL_SHAPE || t == TOOL_HIGHLIGHT ||
           t == TOOL_TEXT;
}
static void SetTool(int t, bool b) {
    if (t < 0 || t >= TOOL_COUNT) return;
    DocState& D = DV(b);
    if (!D.n) t = TOOL_SELECT;              // KillerPDF's SetTool: no document, no editing
    CommitTextEdit();                       // an open box lands with the tool switch
    if (g.annPoly && t != TOOL_SHAPE) PolyCancel(g.view);
    RECT ringOff{ 0, 0, 0, 0 };
    if (t != TOOL_DRAW && g.brushOn) {
        ringOff = RECT{ g.brushPt.x - 12, g.brushPt.y - 12, g.brushPt.x + 12, g.brushPt.y + 12 };
        g.brushOn = false;
    }
    bool was = D.tool;
    D.tool = t;
    if (g.annSelPage >= 0) { g.annSelPage = -1; g.annSelIdx = -1; }   // the ring belongs to Select
    RECT oldBar{}; bool hadBar = g.barOn;
    if (hadBar) oldBar = AnnBarRect();
    if (b == FocusB()) {
        if (BarToolOwns(t) && g.barOn && g.barTool == t) {
            g.barMini = !g.barMini;         // re-click: tuck / bring back, no rebuild
        } else {
            g.barOn = BarToolOwns(t);
            g.barTool = t;
            g.barMini = false;
        }
    }
    if (g.bar && (was != t || b == FocusB())) InvalidateRect(g.bar, nullptr, FALSE);
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);   // cursors, ring and the bar ride
    (void)oldBar; (void)hadBar;             // the full-view repaint covers every footprint
}                                         // (a tool switch is a rare discrete click)

// ---------------- the floating annotation settings bar ----------------
// KillerPDF's AnnotationBars: a floating bar under the toolbar at the top of
// the FOCUSED pane's card, right-anchored (gap 8 + the scrollbar's 12px inset
// when the vertical bar shows), square top corners with a rounded bottom
// (0,0,4,4), MenuBackgroundBrush #141414, a 1px #404040 edge on the sides and
// bottom, a soft shadow, a grip of three 3px dots that slides it along the top
// (double-click or a tool re-click tucks it to a 13px peek strip of six dots).
// Content is a wrap of groups: "Color:" swatches (UiKit.DefaultSwatches +
// the rainbow "More colors..."), "Size:" and "Opacity:" sliders (90px track,
// 12x14 green #22bb56 thumb, alpha-byte opacity shown as %), then the tool's
// own toggle - Draw "Eraser", Line "Level" (on by default), Shapes the three
// kind buttons (Box / Ellipse / Freeform polygon) plus "Fill" (checked),
// Highlight "Annotation eraser". The Text tool swaps in its own groups: the
// font combo, B I S U, the Color row, the Fill row (None + swatches), Size
// with -/+ and an editable box, Spacing, Opacity, Fill Opacity, and Edit
// paragraph.

enum { AB_OUT = -2, AB_NONE = -1, AB_GRIP = 0,
       AB_RW_DRAW = 109, AB_SWC_DRAW = 100,        // 100..108 swatches, 109 rainbow
       AB_SWC_TEXT = 200, AB_RW_TEXT = 290,         // 200..208, 290 rainbow
       AB_SWC_FILL = 300, AB_RW_FILL = 390, AB_FILL_NONE = 399,
       AB_SIZE = 10, AB_OPACITY = 11, AB_LEVEL = 12, AB_ERASE = 13, AB_HLERASE = 14,
       AB_FILLCHK = 15, AB_KIND0 = 16, AB_KIND1 = 17, AB_KIND2 = 18,
       AB_FONT = 20, AB_BOLD = 21, AB_ITALIC = 22, AB_STRIKE = 23, AB_UNDER = 24,
       AB_TMINUS = 25, AB_TPLUS = 26, AB_TBOX = 27, AB_SPACING = 28,
       AB_TOACITY = 29, AB_FOACITY = 30, AB_PARA = 31, AB_TSIZE = 32 };

struct AbCell { int id; RECT r; const wchar_t* lab = nullptr; };   // lab: a -1 label cell's text
static std::vector<AbCell> g_ab;
static int g_abW = 0, g_abH = 0;
static int g_abKey = -1, g_abKeyW = -1;
static RECT g_barLast{};                     // the last painted bar rect (old ∪ new invalidation)

static const int AbSwatches[9] = { 0xE03C3C, 0xE87A1E, 0xF2C01E, 0x2EA54C, 0x2E86DE,
                                   0x8E5BD6, 0xE04A9A, 0x000000, 0xFFFFFF };
static HFONT AbFont11() {
    static HFONT f = nullptr; static double fd = 0;
    if (f && fd != g.dpi) { DeleteObject(f); f = nullptr; }   // a DPI hop re-faces the bars
    if (!f) f = CaptionFont(f, 11, false, L"Killer Sans UI");
    fd = g.dpi;
    return f;
}
static int AbTextW(HDC dc, const wchar_t* s) {
    HGDIOBJ of = SelectObject(dc, AbFont11());
    RECT m{ 0, 0, 0, 0 };
    DrawTextW(dc, s, -1, &m, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, of);
    return m.right - m.left;
}
static bool AbBarTool(int t) { return BarToolOwns(t); }

// One pass lays the tool's groups into g_ab (bar-local rects), wrapping when
// the pane's width runs out - WrapPanel's line breaking. The wrap cap is the
// document area's width less the bar chrome (KillerPDF's _barWidthInset: area
// - 28). The packing is the reference's own arithmetic: both wrap hosts carry
// Margin (8,2,8,2); the draw-family bars are the bare host (BarPadding 0,
// BarEdgeThickness 0 in Dark) while the text bar adds Padding 4 and its
// (1,0,1,1) PaneBorderBrush frame; a draw bar's line is content 20 + the 2+2
// group margins, the text bar's rows are content + the 3+3 Group margins; and
// the Opacity unit is ATOMIC - AnnotationBars.cs packs the Level/Eraser check
// (and the Shape tool's whole kind picker) into dOpacityUnit, so the wrap can
// never split a toggle from its slider.
static void AbLayout() {
    bool b = FocusB();
    DocState& D = DV(b);
    int tool = g.barTool;
    bool text = tool == TOOL_TEXT;
    // the text bar carries its own Padding 4 and (1,0,1,1) PaneBorderBrush
    // frame; the draw-family bars ride BarPadding 0 and BarEdgeThickness 0 in
    // Dark - the bare wrap host
    int pad = text ? (int)(5 * g.dpi) : 0;
    int host = (int)(8 * g.dpi);                // the wrap host's Margin (8,...,8)
    int area = max(160, PaneW(b) - (int)(28 * g.dpi));   // the host's MaxWidth: area - 28
    int avail = max(120, area - 2 * pad - 2 * host);     // the content's wrap cap inside it
    int key = (tool + 2) * 2 + (g.barMini ? 1 : 0) * 7 + (int)(g.dpi * 4) * 1000;
    if (key == g_abKey && g_abKeyW == avail) return;
    g_abKey = key; g_abKeyW = avail;
    g_ab.clear();
    if (g.barMini) {
        g_abW = (int)(64 * g.dpi); g_abH = (int)(13 * g.dpi);
        g_ab.push_back({ AB_GRIP, { 0, 0, g_abW, g_abH } });
        return;
    }
    HDC dc = GetDC(g.view);
    auto S = [&](double v) { return (int)std::lround(v * g.dpi); };
    int band = S(20);                       // the line's content band
    int rowH = band + S(4);
    int x = pad + host, y = pad + S(2);     // the host's Margin (8,2) - and the bar's chrome
    int x0 = x;
    int rowTop = y;                         // the row the cells are landing on (y only
    int maxW = 0;                           // moves when a unit wraps)
    // MakeBarGrip: Padding (1,0,10,0) around the 3px dots - one 14px strip,
    // stretched over the line (the text bar's MakeBarGrip(4) is the same
    // width). The cell is EnableBarSlide's drag zone.
    auto grip = [&]() {
        g_ab.push_back({ AB_GRIP, { x, y - S(2), x + S(14), y + rowH - S(2) } });
        x += S(14);
    };
    auto nl = [&]() { maxW = max(maxW, x); x = x0; y += rowH; rowTop = y; };
    auto need = [&](int w) { if (x + w > x0 + avail && x > x0) nl(); };
    auto put = [&](int id, int w, int h) {  // the cell centers in the line's content band
        int top = y + S(2) + max(0, (band - h)) / 2;
        g_ab.push_back({ id, { x, top, x + w, top + h } });
        x += w;
    };
    auto label = [&](const wchar_t* s) {
        int w = AbTextW(dc, s) + S(6);
        g_ab.push_back({ -1, { x, y + S(2), x + w, y + S(2) + band }, s });
        x += w;
    };
    auto swatchRow = [&](int base) {        // the Color group: label + 9 swatches + rainbow, +16
        need(S(42) + 10 * S(20));
        label(L"Color:");
        for (int i = 0; i < 9; i++) put(base + i, S(20), S(18));
        put(base == AB_SWC_DRAW ? AB_RW_DRAW : base == AB_SWC_TEXT ? AB_RW_TEXT : AB_RW_FILL, S(20), S(18));
        x += S(16);                         // the group's Margin (0,2,16,2)
    };
    auto slider = [&](int id, const wchar_t* lab, int valW) {
        need(S(6) + AbTextW(dc, lab) + S(90) + S(4) + valW + S(16));
        label(lab);
        put(id, S(90) + S(4) + valW, S(20));
        x += S(16);                         // the group's Margin (0,2,16,2)
    };
    // BarCheck: the 15px box (radius 3) and the 11px label, the unit centered
    // on the row's content band. lead/trail are the WPF margins around it.
    auto check = [&](int id, const wchar_t* lab, int lead, int trail) {
        int bw = S(15);
        int w = bw + S(5) + AbTextW(dc, lab);
        int top = y + S(2) + max(0, (band - bw)) / 2;
        g_ab.push_back({ id, { x + lead, top, x + lead + w, top + bw } });
        x += lead + w + trail;
    };
    // per-row cell emitters for the text bar's columns: (rh) the row's line
    // height, (ch) the row's content height - the cell centers in the content
    // band the way the WPF StackPanels center in their Group margins.
    auto rowPut = [&](int rx, int ry, int rh, int ch, int id, int w, int h) {
        int top = ry + (rh - ch) / 2 + max(0, (ch - h)) / 2;
        g_ab.push_back({ id, { rx, top, rx + w, top + h } });
        return rx + w;
    };
    auto rowLab = [&](int rx, int ry, int rh, const wchar_t* s) {
        int w = AbTextW(dc, s) + S(6);
        g_ab.push_back({ -1, { rx, ry + (rh - S(16)) / 2, rx + w, ry + (rh + S(16)) / 2 }, s });
        return rx + w;
    };
    if (text) {
        // KillerPDF's deliberate 1.7.5 layout, assembled as three vertical
        // pairs so related controls share an exact left edge: Font over Size
        // over Spacing, Color over Fill over "Edit paragraph", and Opacity
        // beside Fill Opacity. The outer WrapPanel moves a whole pair at
        // narrow widths instead of separating a label from its control. Each
        // row's line height is its content + the Group margin 3+3; each row
        // carries the group's right Margin 16 (the paragraph button's own
        // Margin (0,3,0,0) has none).
        int six = S(6);
        int rowF = six + AbTextW(dc, L"Font:") + S(132) + 4 * S(24);
        int rowS = six + AbTextW(dc, L"Size:") + S(90) + S(21) + S(21) + S(36) + S(16);
        int rowP = six + AbTextW(dc, L"Spacing:") + S(90) + S(4) + S(48);
        int colA = max(rowF, max(rowS, rowP)) + S(16);
        int hF = S(28), hS = S(26), hP = S(26);          // 22/20/20 content + 6
        int rowC = six + AbTextW(dc, L"Color:") + 10 * S(20);
        int rowFl = six + AbTextW(dc, L"Fill:") + S(20) + 10 * S(20);
        int paraW = AbTextW(dc, L"Edit paragraph") + S(20);
        int colB = max(rowC + S(16), max(rowFl + S(16), paraW));
        int hC = S(26), hFl = S(26), hPara = S(25);      // 20/20/22 content + margins
        int oLab = S(6) + AbTextW(dc, L"Opacity:") + S(94) + S(40) + S(16);
        int foLab = S(6) + AbTextW(dc, L"Fill Opacity:") + S(94) + S(40) + S(16);
        int colC = oLab + foLab;
        int hO = S(26);
        int colAh = hF + hS + hP, colBh = hC + hFl + hPara, colCh = hO;
        // the WrapPanel's line decisions: the grip leads line 1, a pair that
        // does not fit moves WHOLE to the next line, whose top rides the
        // tallest pair the line before it kept
        g_ab.push_back({ AB_GRIP, { x, y - S(2), x + S(14), y - S(2) + S(80) } });   // stretched; re-heighted below
        int ax = x + S(14), ay = y;
        int l1w = S(14) + colA, l1h = colAh;
        bool bWrap = l1w + colB > avail;
        if (!bWrap) { l1w += colB; l1h = max(l1h, colBh); }
        bool cWrap = l1w + colC > avail;
        if (!cWrap) l1h = max(l1h, colCh);
        // the grip stretched over line 1: its cell rides l1h (VerticalAlignment.Stretch)
        for (AbCell& c : g_ab) if (c.id == AB_GRIP) c.r.bottom = ay - S(2) + l1h;
        {   // column A rows
            int rx = rowLab(ax, ay, hF, L"Font:");
            rx = rowPut(rx, ay, hF, S(22), AB_FONT, S(132), S(22));
            const wchar_t* styles[4] = { L"B", L"I", L"S", L"U" };
            int sids[4] = { AB_BOLD, AB_ITALIC, AB_STRIKE, AB_UNDER };
            for (int i = 0; i < 4; i++) {
                int s2 = max(S(22), AbTextW(dc, styles[i]) + S(8));
                rowPut(rx + S(2), ay, hF, S(22), sids[i], s2, S(20));   // StyleToggle Margin (2,0,0,0)
                rx += S(2) + s2;
            }
            rx = rowLab(ax, ay + hF, hS, L"Size:");
            rx = rowPut(rx, ay + hF, hS, S(20), AB_TSIZE, S(94), S(20));
            rowPut(rx + S(3), ay + hF, hS, S(20), AB_TMINUS, S(18), S(20)); rx += S(21);
            rowPut(rx + S(3), ay + hF, hS, S(20), AB_TPLUS, S(18), S(20)); rx += S(21);
            rowPut(rx + S(4), ay + hF, hS, S(20), AB_TBOX, S(32), S(20)); rx += S(36);
            rowPut(rx + S(2), ay + hF, hS, S(20), -1, S(14), S(14));       // the pt label
            g_ab.back().lab = L"pt";
            rx = rowLab(ax, ay + hF + hS, hP, L"Spacing:");
            rowPut(rx, ay + hF + hS, hP, S(20), AB_SPACING, S(94) + S(48), S(20));
        }
        int bRight = 0;
        int bx = bWrap ? x0 : ax + colA, by = bWrap ? ay + colAh : ay;
        {   // column B rows
            int rx = rowLab(bx, by, hC, L"Color:");
            for (int i = 0; i < 9; i++) { rowPut(rx, by, hC, S(20), AB_SWC_TEXT + i, S(20), S(18)); rx += S(20); }
            rowPut(rx, by, hC, S(20), AB_RW_TEXT, S(20), S(18)); rx += S(20);
            bRight = max(bRight, rx + S(16));
            rx = rowLab(bx, by + hC, hFl, L"Fill:");
            rowPut(rx, by + hC, hFl, S(20), AB_FILL_NONE, S(18), S(18)); rx += S(20);
            for (int i = 0; i < 9; i++) { rowPut(rx, by + hC, hFl, S(20), AB_SWC_FILL + i, S(20), S(18)); rx += S(20); }
            rowPut(rx, by + hC, hFl, S(20), AB_RW_FILL, S(20), S(18)); rx += S(20);
            bRight = max(bRight, rx + S(16));
            // the paragraph button: Margin (0,3,0,0), MinHeight 22
            int py = by + hC + hFl + S(3);
            g_ab.push_back({ AB_PARA, { bx, py, bx + paraW, py + S(22) } });
            bRight = max(bRight, bx + paraW);
        }
        int cx = cWrap ? x0 : bx + colB;
        int cy = cWrap ? (bWrap ? by + colBh : max(ay + colAh, by + colBh)) : ay;
        {   // the opacity pair, one line (top-aligned like every wrap unit)
            int rx = rowLab(cx, cy, hO, L"Opacity:");
            rowPut(rx, cy, hO, S(20), AB_TOACITY, S(94) + S(40), S(20));
            int rx2 = rowLab(cx + oLab, cy, hO, L"Fill Opacity:");
            rowPut(rx2, cy, hO, S(20), AB_FOACITY, S(94) + S(40), S(20));
        }
        int bottom = max(ay + colAh, max(by + colBh, cy + colCh));
        int l2w = bWrap ? (colB + (cWrap ? 0 : colC)) : (cWrap ? colC : 0);
        maxW = max(l1w, l2w);
        g_abW = min(maxW + host + pad, area - S(28) + 2 * pad);   // + the chrome, capped
        g_abH = bottom + S(2) + pad;                      // + the host's bottom Margin 2 + chrome
        ReleaseDC(g.view, dc);
        for (AbCell& c : g_ab) if (c.r.right > g_abW) c.r.right = g_abW;
        return;
    }
    grip();
    {
        swatchRow(AB_SWC_DRAW);
        // KillerPDF's Width=34 size value and Width=40 opacity value, both
        // right-aligned - the reserves keep the slider pitch constant as the
        // number's digits change.
        if (tool != TOOL_HIGHLIGHT) {
            slider(AB_SIZE, L"Size:", S(34));
        }
        // The dOpacityUnit: Opacity + the Level/Eraser toggle (or the Shape
        // tool's kind picker row) is ONE wrap unit - AnnotationBars.cs packs
        // barCheck into dOpacityUnit, so the unit's width is measured before
        // the wrap test and the toggle can never land on a row by itself.
        {
            const wchar_t* cl = tool == TOOL_DRAW ? L"Eraser" : tool == TOOL_LINE ? L"Level"
                              : tool == TOOL_HIGHLIGHT ? L"Annotation eraser" : nullptr;
            int opW = S(6) + AbTextW(dc, L"Opacity:") + S(94) + S(40);
            int unit = opW + S(16);          // opacityGroup's Margin (0,2,16,2)
            if (cl) unit += S(15) + S(5) + AbTextW(dc, cl) + S(2);   // + barCheck Margin (0,2,2,2)
            if (tool == TOOL_SHAPE)          // ShapeKindPicker's row rides the unit's end
                unit += S(2) + 3 * S(28) + S(10) + S(15) + S(5) + AbTextW(dc, L"Fill") + S(18);
            need(unit);
            label(L"Opacity:");
            put(AB_OPACITY, S(94) + S(40), S(20));
            x += S(16);                      // opacityGroup's Margin (0,2,16,2)
            if (tool == TOOL_DRAW) check(AB_ERASE, L"Eraser", 0, S(2));
            else if (tool == TOOL_LINE) check(AB_LEVEL, L"Level", 0, S(2));
            else if (tool == TOOL_HIGHLIGHT) check(AB_HLERASE, L"Annotation eraser", 0, S(2));
            else if (tool == TOOL_SHAPE) {
                x += S(2);                   // the picker row's Margin (2,0,0,0)
                put(AB_KIND0, S(26), S(20)); x += S(2);   // KindBtn Margin (1,0,1,0)
                put(AB_KIND1, S(26), S(20)); x += S(2);
                x += S(1);                   // KindBtn Margin (1,0,1,0): the last kind's
                                             // right margin, then the check's own lead 10
                check(AB_FILLCHK, L"Fill", S(10), S(18)); // fillCheck Margin (10,0,18,0)
            } else x += S(16);
        }
    }
    maxW = max(maxW, x);
    ReleaseDC(g.view, dc);
    // the bar hugs its content: the host's Margin 8 on both sides (+ the
    // chrome), capped at the area - the same cap the WPF MaxWidth binding
    // puts on the wrap panel (area - 28) plus the chrome around it
    g_abW = min(maxW + host + pad, area - S(28) + 2 * pad);
    g_abH = rowTop + rowH + S(2) + pad;                   // the last row + the host's bottom 2
    for (AbCell& c : g_ab) if (c.r.right > g_abW) c.r.right = g_abW;
}

// The bar's rect in view coords: right-anchored on the FOCUSED pane's card
// (gap 8 + the scrollbar inset), slid by the grip's offset.
static RECT AnnBarRect() {
    AbLayout();
    bool b = FocusB();
    DocState& D = DV(b);
    int cardL = PaneX0(b) + CardX(b), cardR = cardL + CardW(b);
    int inset = (int)(8 * g.dpi) + (D.n && D.docH > g.ch ? SBar() : 0);
    int x0 = cardR - inset - g.barDx - g_abW;
    x0 = std::clamp(x0, cardL + (int)(2 * g.dpi), max(cardL, cardR - (int)(2 * g.dpi) - g_abW));
    return { x0, 0, x0 + g_abW, g_abH };
}

// State flips repaint the bar's own footprint only (old ∪ new) - the bar sits
// over pages, never recompositing the document under it. The shadow's 3px near
// field below the bottom edge rides along.
static void AnnBarSync(bool) {
    if (!g.view) return;
    // the shadow field: blur 6 (sigma 3) tails ~9px past the sides and depth
    // 3 + ~9px below - the tile AnnBarDraw blends, so old ∪ new must cover it
    int mx = (int)std::ceil(9.0 * g.dpi);
    int sh = (int)std::ceil((3.0 + 9.0) * g.dpi) + 1;
    RECT now = g.barOn ? AnnBarRect() : RECT{ 0, 0, 0, 0 };
    RECT old = g_barLast;
    bool hadOld = old.left < old.right;
    if (hadOld && g.barOn) {
        now.bottom += sh; old.bottom += sh;
        now.left -= mx; now.right += mx;
        old.left -= mx; old.right += mx;
        RECT u{ min(old.left, now.left), min(old.top, now.top),
                max(old.right, now.right), max(old.bottom, now.bottom) };
        InvalidateRect(g.view, &u, FALSE);
    } else if (hadOld) { old.bottom += sh; old.left -= mx; old.right += mx; InvalidateRect(g.view, &old, FALSE); }
    else if (g.barOn) { now.bottom += sh; now.left -= mx; now.right += mx; InvalidateRect(g.view, &now, FALSE); }
}

// Bar choices flow into the SELECTED annotation live (KillerPDF's
// ApplyDrawStyleToSelection) and set the defaults the next stroke commits.
static void ApplyDrawStyleToSelection() {
    if (g.annSelPage < 0 || g.annSelIdx < 0) return;
    DocState& D = DV(g.annSelB);
    auto it = D.annos.find(g.annSelPage);
    if (it == D.annos.end() || g.annSelIdx >= (int)it->second.size()) return;
    Annot& a = it->second[g.annSelIdx];
    if (a.type == AT_HIGHLIGHT) {
        if (g.barTool == TOOL_HIGHLIGHT) { a.color = g.hlRGB; a.alpha = g.hlA; }
        else { a.color = g.drawColor; a.alpha = 255; }   // a filled shape's block
    } else if (a.type == AT_INK) {
        a.color = g.drawColor; a.alpha = g.drawOpacity; a.width = g.drawWidth;
        if (a.fillA > 0) { a.fill = g.drawColor; a.fillA = (BYTE)max(20, g.drawOpacity / 2); }
    }
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// The text bar's funnel: the live editor restyles first, then every selected
// text box (KillerPDF's ApplyTextStyleToSelection), and the next placed box
// picks the same state up.
static void ApplyTextStyleToSelection() {
    if (g.tedit) {
        float k = (float)(DV(g.tePaneB).zoom * g.dpi);
        int fontPx = max(8, (int)std::lround(g.textSize * k));
        HFONT f = CreateFontW(-fontPx, 0, 0, 0, g.textBold ? FW_BOLD : FW_NORMAL,
                              g.textItalic, g.textUnder, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, g.textFont.c_str());
        SendMessageW(g.tedit, WM_SETFONT, (WPARAM)f, TRUE);
        RECT er; GetWindowRect(g.tedit, &er);
        POINT p0{ er.left, er.top }, p1{ er.right, er.bottom };
        ScreenToClient(g.view, &p0); ScreenToClient(g.view, &p1);
        RECT u{ p0.x - 8, p0.y - 8, p1.x + 8, p1.y + 8 };
        InvalidateRect(g.view, &u, FALSE);
    }
    if (g.annSelPage >= 0 && g.annSelIdx >= 0) {
        DocState& D = DV(g.annSelB);
        auto it = D.annos.find(g.annSelPage);
        if (it != D.annos.end() && g.annSelIdx < (int)it->second.size()) {
            Annot& a = it->second[g.annSelIdx];
            if (a.type == AT_TEXT) {
                a.font = g.textFont; a.fsize = g.textSize; a.spacing = g.textSpacing;
                a.bold = g.textBold; a.italic = g.textItalic;
                a.strike = g.textStrike; a.under = g.textUnder;
                a.color = g.textRGB; a.alpha = g.textA;
                a.fill = g.textFillRGB; a.fillA = g.textFillA;
                float k = (float)(D.zoom * g.dpi);
                HDC m = GetDC(g.view);
                int hpx = max(8, (int)std::lround(a.fsize * k));
                HFONT f = CreateFontW(-hpx, 0, 0, 0, a.bold ? FW_BOLD : FW_NORMAL,
                                      a.italic, a.under, 0, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, a.font.c_str());
                HGDIOBJ of = SelectObject(m, f);
                RECT mr{ 0, 0, (int)(a.w * k), 10000 };
                DrawTextW(m, a.text.c_str(), -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
                SelectObject(m, of); DeleteObject(f); ReleaseDC(g.view, m);
                a.h = max(24.0f, (mr.bottom - mr.top) / max(0.01f, k));
            }
        }
    }
    AnnBarSync(true);
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// ---- the bar's paint ----
static void AbRoundRect(Gdiplus::Graphics& gs, const RECT& r, float rad,
                        Gdiplus::Brush* fill, Gdiplus::Pen* pen = nullptr) {
    Gdiplus::GraphicsPath p;
    float fx = (float)r.left, fy = (float)r.top;
    float fw = (float)(r.right - r.left), fh = (float)(r.bottom - r.top);
    if (fw <= 0 || fh <= 0) return;
    p.AddArc(fx, fy, 2 * rad, 2 * rad, 180, 90);
    p.AddArc(fx + fw - 2 * rad, fy, 2 * rad, 2 * rad, 270, 90);
    p.AddArc(fx + fw - 2 * rad, fy + fh - 2 * rad, 2 * rad, 2 * rad, 0, 90);
    p.AddArc(fx, fy + fh - 2 * rad, 2 * rad, 2 * rad, 90, 90);
    p.CloseFigure();
    if (fill) gs.FillPath(fill, &p);
    if (pen) gs.DrawPath(pen, &p);
}
static HFONT AbMdlFont(int px) {
    static HFONT f = nullptr; static int fpx = 0; static double fd = 0;
    if (f && (fpx != px || fd != g.dpi)) { DeleteObject(f); f = nullptr; }
    if (!f) f = CaptionFont(f, px, false, L"Segoe MDL2 Assets");
    fpx = px; fd = g.dpi;
    return f;
}
static COLORREF AbCellColor(int id) {
    if (id >= AB_SWC_DRAW && id < AB_RW_DRAW) return RGB((AbSwatches[id - AB_SWC_DRAW] >> 16) & 0xff,
        (AbSwatches[id - AB_SWC_DRAW] >> 8) & 0xff, AbSwatches[id - AB_SWC_DRAW] & 0xff);
    if (id >= AB_SWC_TEXT && id < AB_RW_TEXT) { int v = AbSwatches[id - AB_SWC_TEXT]; return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff); }
    if (id >= AB_SWC_FILL && id < AB_RW_FILL) { int v = AbSwatches[id - AB_SWC_FILL]; return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff); }
    return 0;
}
static bool AbSwatchActive(int id) {
    COLORREF c = AbCellColor(id);
    if (id >= AB_SWC_DRAW && id < AB_RW_DRAW) {
        if (g.barTool == TOOL_HIGHLIGHT) return g.hlRGB == c;
        return g.drawColor == c;
    }
    if (id >= AB_SWC_TEXT && id < AB_RW_TEXT) return g.textRGB == c;
    return g.textFillA > 0 && g.textFillRGB == c;
}
static void AbSliderDraw(Gdiplus::Graphics& gs, const RECT& r, float vmin, float vmax, float v) {
    float trackY = (r.top + r.bottom) / 2.0f - 2.0f * (float)g.dpi;
    Gdiplus::SolidBrush track(Gdiplus::Color(255, 0x3f, 0x3f, 0x3f));
    AbRoundRect(gs, RECT{ r.left, (int)trackY, r.left + (int)(90 * g.dpi), (int)trackY + (int)(4 * g.dpi) },
                2.0f * (float)g.dpi, &track);
    float t = std::clamp((v - vmin) / max(0.001f, vmax - vmin), 0.0f, 1.0f);
    int tx = r.left + (int)(t * (90.0f * g.dpi - 12.0f * g.dpi));
    Gdiplus::SolidBrush thumb(Gdiplus::Color(255, 0x22, 0xbb, 0x56));
    AbRoundRect(gs, RECT{ tx, (int)trackY - (int)(5 * g.dpi), tx + (int)(12 * g.dpi), (int)trackY + (int)(9 * g.dpi) },
                2.0f * (float)g.dpi, &thumb);
}
// The tick itself. KillerPDF's BarCheck marks a literal "✓" (U+2713) TextBlock
// at FontSize 10, Center/Center in the 15px box (AnnotationBars.cs) - WPF
// centers the text line, and the glyph reads centered. GDI's DT_VCENTER
// centers the font's LINE BOX too, but the glyph's ink can sit off-center
// inside that line - the drift the user saw. So the tick is measured once per
// DPI here: drawn into a scratch 15px box exactly the way the real one draws,
// its ink bounding box scanned, and the real draw's text rect shifted by minus
// that ink-to-center offset - the ink lands dead center BY CONSTRUCTION,
// because the measurement and the draw share one placement path (DrawTextW's
// rect math; no second metric convention to disagree). ANTIALIASED_QUALITY
// matches WPF's grayscale anti-aliasing - ClearType's subpixel filter tinted
// the thin stroke with the background's chroma. Segoe UI owns U+2713 on every
// Windows this targets; Segoe UI Symbol, then MDL2's E73E, are the fallbacks.
static void AbCheckGlyph(HDC dc, const RECT& box) {
    static HFONT f = nullptr; static double fd = 0; static wchar_t ch = 0;
    static POINT off{ 0, 0 };                 // ink-center minus box-center at this dpi
    if (f && fd != g.dpi) { DeleteObject(f); f = nullptr; }
    if (!f) {
        const struct { const wchar_t* face; wchar_t ch; } tries[] = {
            { L"Segoe UI", L'\u2713' }, { L"Segoe UI Symbol", L'\u2713' },
            { L"Segoe MDL2 Assets", L'\uE73E' } };
        for (const auto& t : tries) {
            HFONT tf = CreateFontW(-(int)std::lround(10 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, t.face);
            HGDIOBJ of = SelectObject(dc, tf);
            WORD idx = 0xFFFF;
            GetGlyphIndicesW(dc, &t.ch, 1, &idx, 1);   // GGI_MARK_NONEXISTING_GLYPHS
            SelectObject(dc, of);
            if (idx != 0xFFFF) { f = tf; ch = t.ch; break; }
            DeleteObject(tf);
        }
        if (f) {
            int side = max(1, (int)(15 * g.dpi));
            HDC m = CreateCompatibleDC(dc);
            HBITMAP bm = CreateCompatibleBitmap(dc, side, side);
            HGDIOBJ ob = SelectObject(m, bm);
            RECT full{ 0, 0, side, side };
            HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(m, &full, bg); DeleteObject(bg);
            HGDIOBJ of2 = SelectObject(m, f);
            SetBkMode(m, TRANSPARENT);
            SetTextColor(m, RGB(255, 255, 255));
            DrawTextW(m, &ch, 1, &full, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(m, of2);
            int x0 = side, y0 = side, x1 = -1, y1 = -1;
            for (int y = 0; y < side; y++)
                for (int x = 0; x < side; x++) {
                    COLORREF c = GetPixel(m, x, y);
                    if (GetRValue(c) + GetGValue(c) + GetBValue(c) > 90) {   // ink over the black floor
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
                }
            SelectObject(m, ob); DeleteObject(bm); DeleteDC(m);
            off = (x1 >= x0 && y1 >= y0) ? POINT{ (x0 + x1) / 2 - side / 2, (y0 + y1) / 2 - side / 2 }
                                         : POINT{ 0, 0 };
        }
        fd = g.dpi;
    }
    if (!f || !ch) return;
    RECT cr{ box.left - off.x, box.top - off.y, box.right - off.x, box.bottom - off.y };
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, &ch, 1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}
static void AbCheckDraw(Gdiplus::Graphics& gs, HDC dc, const RECT& r, bool on) {
    RECT box{ r.left, r.top, r.left + (int)(15 * g.dpi), r.top + (int)(15 * g.dpi) };
    if (on) {
        Gdiplus::LinearGradientBrush gb(Gdiplus::Rect(box.left, box.top, box.right - box.left, box.bottom - box.top),
            Gdiplus::Color(255, 0x38, 0x75, 0x52), Gdiplus::Color(255, 0x10, 0x4e, 0x2b), 90.0f);
        AbRoundRect(gs, box, 3.0f * (float)g.dpi, &gb);
        SetTextColor(dc, RGB(255, 255, 255));   // SelectionFg on the SelectionBg fill
        AbCheckGlyph(dc, box);
    } else {
        Gdiplus::Pen pen(Gdiplus::Color(255, 0x44, 0x44, 0x44), 1.0f);
        AbRoundRect(gs, box, 3.0f * (float)g.dpi, nullptr, &pen);
    }
    SelectObject(dc, AbFont11());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
}
// The check's label rides its cell's band, vertically centered on the box -
// the whole BarCheck StackPanel is one centered line (AnnotationBars.cs).
static void AbCheckLabel(HDC dc, const RECT& r, const wchar_t* lab) {
    SelectObject(dc, AbFont11());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
    RECT lr{ r.left + (int)(20 * g.dpi), r.top, r.right, r.bottom };
    DrawTextW(dc, lab, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static double Phi(double z);   // the gaussian CDF, defined with the card's shadow
static Gdiplus::TextureBrush* GrainTex(int which);   // the grain tile as a GDI+ brush
static int g_grainPX = 0, g_grainPY = 0;   // GDI's brush-org phase, stashed by the paint

static void AnnBarDraw(HDC dc, int ox, int oy) {
    if (!g.barOn) return;
    AbLayout();
    g_barLast = RECT{ ox, oy, ox + g_abW, oy + g_abH };
    bool textBar = g.barTool == TOOL_TEXT;
    // the DropShadow: black, blur 6 (sigma 3), depth 3 down, opacity 0.38 -
    // AnnotBarShadow's exact recipe, rasterized as one cached gaussian tile
    // like the Find bar's (its blur 16 / 0.55 there, 6 / 0.38 here)
    {
        static HBITMAP shb = nullptr; static HDC shdc = nullptr;
        static int shw = 0, shh = 0, shmx = 0; static double shdpi = 0;
        double sig = 3.0 * g.dpi, depth = 3.0 * g.dpi;
        int mx = (int)std::ceil(3.0 * sig), my = (int)std::ceil(depth + 3.0 * sig);
        int tw = g_abW + 2 * mx, th = g_abH + my;
        if (!shb || shw != tw || shh != th || shdpi != g.dpi) {
            BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), tw, -th, 1, 32, BI_RGB };
            BYTE* px = nullptr;
            HBITMAP nb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
            if (px && nb) {
                for (int y = 0; y < th; y++) {
                    double ay = Phi((y - depth) / sig) - Phi((y - depth - g_abH) / sig);
                    BYTE* row = px + (size_t)y * tw * 4;
                    for (int x2 = 0; x2 < tw; x2++) {
                        double a = 0.38 * ay * (Phi((x2 - mx) / sig) - Phi((x2 - mx - g_abW) / sig));
                        BYTE* p = row + (size_t)x2 * 4;
                        p[0] = p[1] = p[2] = 0;
                        p[3] = (BYTE)std::lround(a * 255.0);
                    }
                }
                if (!shdc) shdc = CreateCompatibleDC(dc);
                if (shb) DeleteObject(shb);
                shb = nb;
                SelectObject(shdc, shb);
                shw = tw; shh = th; shmx = mx; shdpi = g.dpi;
            }
        }
        if (shb) {
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(dc, ox - shmx, oy, shw, shh, shdc, 0, 0, shw, shh, bf);
        }
    }
    // the body: the floating surface tier - MenuBackgroundBrush #141414 under
    // the shared grain (AnnotationBars.cs's BuildBarHost grain layer), square
    // top corners, the flyout's radius-4 bottom (0,0,4,4). The TEXT bar adds
    // its own (1,0,1,1) PaneBorderBrush #555555 frame - no top edge, the
    // toolbar above already separates (TextSettingsBar.cs); the draw-family
    // bars ride BarEdgeThickness 0 in Dark - no edge of their own, the shadow
    // is the only line.
    int spx = g_grainPX, spy = g_grainPY;
    g_grainPX += ox; g_grainPY += oy;
    {
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        float fx = (float)ox, fy = (float)oy;
        float fw = (float)g_abW, fh = (float)g_abH;
        float rad = 4.0f * (float)g.dpi;
        auto barPath = [&](float inset) {
            Gdiplus::GraphicsPath* p = new Gdiplus::GraphicsPath();
            float r = max(0.5f, rad - inset);
            p->AddLine(fx + inset, fy + inset, fx + fw - inset, fy + inset);
            p->AddArc(fx + fw - inset - 2 * r, fy + fh - inset - 2 * r, 2 * r, 2 * r, 0, 90);
            p->AddArc(fx + inset, fy + fh - inset - 2 * r, 2 * r, 2 * r, 90, 90);
            p->CloseFigure();
            return p;
        };
        if (textBar) {   // the 1px rim under the grain: fill the rim color,
                         // then the inset body over it - the top edge stays
                         // body (no top border)
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush rim(Gdiplus::Color(255, 0x55, 0x55, 0x55));
            Gdiplus::GraphicsPath* outer = barPath(0);
            gs.FillPath(&rim, outer);
            delete outer;
        }
        Gdiplus::GraphicsPath* inner = barPath(textBar ? 1.0f : 0.0f);
        if (Gdiplus::TextureBrush* t = GrainTex(2)) gs.FillPath(t, inner);
        else { Gdiplus::SolidBrush mb(Gdiplus::Color(255, 0x14, 0x14, 0x14)); gs.FillPath(&mb, inner); }
        delete inner;
    }
    g_grainPX = spx; g_grainPY = spy;
    // the grip: MakeBarGrip's dots - 3px, MutedTextBrush #a3a3a3, spaced 3px,
    // centered in the grip strip (three dots on the single-row bars, the text
    // bar's MakeBarGrip(4) carries four)
    {
        Gdiplus::Graphics gs(dc);
        for (const AbCell& c : g_ab) {
            if (c.id != AB_GRIP) continue;
            int n = textBar ? 4 : 3;
            float cx = (float)(ox + c.r.left) + 2.5f * (float)g.dpi;
            float cy = (float)(oy + c.r.top + c.r.bottom) / 2.0f;
            Gdiplus::SolidBrush dot(Gdiplus::Color(255, 0xa3, 0xa3, 0xa3));
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            for (int i = 0; i < n; i++)
                gs.FillEllipse(&dot, cx - 1.5f * (float)g.dpi, cy + (i - (n - 1) / 2.0f) * 6.0f * (float)g.dpi - 1.5f * (float)g.dpi,
                               3.0f * (float)g.dpi, 3.0f * (float)g.dpi);
            break;
        }
    }
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, AbFont11());
    wchar_t tmp[48];
    for (const AbCell& c : g_ab) {
        RECT r{ ox + c.r.left, oy + c.r.top, ox + c.r.right, oy + c.r.bottom };
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        int id = c.id;
        if (id == -1) {                          // the group's label: KillerPDF's DimLabel
            if (c.lab) {
                SelectObject(dc, AbFont11());
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
                DrawTextW(dc, c.lab, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            continue;
        }
        if ((id >= AB_SWC_DRAW && id < AB_RW_DRAW) || (id >= AB_SWC_TEXT && id < AB_RW_TEXT) ||
            (id >= AB_SWC_FILL && id < AB_RW_FILL)) {
            COLORREF col = AbCellColor(id);
            Gdiplus::SolidBrush f(Gdiplus::Color(255, GetRValue(col), GetGValue(col), GetBValue(col)));
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, &f);
            bool act = AbSwatchActive(id);
            Gdiplus::Pen pen(act ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0x44, 0x44, 0x44),
                             act ? 2.0f : 1.0f);
            AbRoundRect(gs, RECT{ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, 3.0f * (float)g.dpi, nullptr, &pen);
        } else if (id == AB_RW_DRAW || id == AB_RW_TEXT || id == AB_RW_FILL) {
            // the rainbow "More colors..." swatch: the picker's own gradient
            Gdiplus::Color cols[5] = { Gdiplus::Color(255, 255, 0, 0), Gdiplus::Color(255, 255, 255, 0),
                                       Gdiplus::Color(255, 0, 255, 0), Gdiplus::Color(255, 0, 255, 255),
                                       Gdiplus::Color(255, 0, 0, 255) };
            float pos[5] = { 0.0f, 0.25f, 0.5f, 0.7f, 1.0f };   // Gdiplus::REAL positions
            Gdiplus::LinearGradientBrush rb(Gdiplus::Rect(r.left, r.top, r.right - r.left, r.bottom - r.top),
                Gdiplus::Color(255, 255, 0, 0), Gdiplus::Color(255, 0, 0, 255), 0.0f, false);
            rb.SetInterpolationColors(cols, pos, 5);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, &rb);
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x44, 0x44, 0x44), 1.0f);
            AbRoundRect(gs, RECT{ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, 3.0f * (float)g.dpi, nullptr, &pen);
        } else if (id == AB_FILL_NONE) {
            Gdiplus::SolidBrush w(Gdiplus::Color(255, 255, 255, 255));
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, &w);
            Gdiplus::Pen red(Gdiplus::Color(255, 255, 0, 0), 1.5f);
            gs.DrawLine(&red, (float)r.left + 3, (float)r.bottom - 3, (float)r.right - 3, (float)r.top + 3);
            Gdiplus::Pen pen(g.textFillA == 0 ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0x44, 0x44, 0x44),
                             g.textFillA == 0 ? 2.0f : 1.0f);
            AbRoundRect(gs, RECT{ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, 3.0f * (float)g.dpi, nullptr, &pen);
        } else if (id == AB_SIZE) {
            AbSliderDraw(gs, r, 1, 60, g.drawWidth);
            swprintf(tmp, 48, L"%dpx", (int)g.drawWidth);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_OPACITY) {
            AbSliderDraw(gs, r, 10, 255, g.drawOpacity);
            swprintf(tmp, 48, L"%d%%", (int)(g.drawOpacity / 255.0 * 100));
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_TSIZE) {
            AbSliderDraw(gs, r, 8, 72, std::clamp(g.textSize, 8.0f, 72.0f));
        } else if (id == AB_SPACING) {
            AbSliderDraw(gs, r, -2, 20, g.textSpacing);
            swprintf(tmp, 48, L"%.2f pt", g.textSpacing);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_TOACITY) {
            AbSliderDraw(gs, r, 10, 255, g.textA);
            swprintf(tmp, 48, L"%d%%", (int)(g.textA / 255.0 * 100));
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_FOACITY) {
            AbSliderDraw(gs, r, 10, 255, g.textFillA == 0 ? 255 : g.textFillA);
            swprintf(tmp, 48, L"%d%%", (int)((g.textFillA == 0 ? 255 : g.textFillA) / 255.0 * 100));
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ r.left + (int)(94 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, tmp, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_ERASE) {
            AbCheckDraw(gs, dc, r, g.drawErase);
            AbCheckLabel(dc, r, L"Eraser");
        } else if (id == AB_LEVEL) {
            AbCheckDraw(gs, dc, r, g.lineLevel);
            AbCheckLabel(dc, r, L"Level");
        } else if (id == AB_HLERASE) {
            AbCheckDraw(gs, dc, r, g.hlErase);
            AbCheckLabel(dc, r, L"Annotation eraser");
        } else if (id == AB_FILLCHK) {
            AbCheckDraw(gs, dc, r, g.shapeFill);
            AbCheckLabel(dc, r, L"Fill");
        } else if (id >= AB_KIND0 && id <= AB_KIND2) {
            int k = id - AB_KIND0;
            bool act = g.shapeKind == k;
            Gdiplus::Pen pen(act ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0x44, 0x44, 0x44),
                             act ? 1.5f : 1.0f);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, nullptr, &pen);
            Gdiplus::Pen gl(act ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0xa3, 0xa3, 0xa3), 1.5f);
            float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
            float s = (float)g.dpi;
            if (k == 0) gs.DrawRectangle(&gl, cx - 6.5f * s, cy - 4.5f * s, 13 * s, 9 * s);
            else if (k == 1) gs.DrawEllipse(&gl, cx - 6.5f * s, cy - 4.5f * s, 13 * s, 9 * s);
            else {
                Gdiplus::PointF pts[5] = {
                    Gdiplus::PointF(cx, cy - 5.5f * s), Gdiplus::PointF(cx + 6.5f * s, cy - 1.0f * s),
                    Gdiplus::PointF(cx + 4.0f * s, cy + 5.5f * s), Gdiplus::PointF(cx - 4.0f * s, cy + 5.5f * s),
                    Gdiplus::PointF(cx - 6.5f * s, cy - 1.0f * s) };
                gs.DrawPolygon(&gl, pts, 5);
            }
        } else if (id == AB_FONT) {
            Gdiplus::SolidBrush f(Gdiplus::Color(255, 0x14, 0x14, 0x14));
            AbRoundRect(gs, r, 2.0f * (float)g.dpi, &f);
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x40, 0x40, 0x40), 1.0f);
            AbRoundRect(gs, r, 2.0f * (float)g.dpi, nullptr, &pen);
            SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
            RECT tr{ r.left + (int)(6 * g.dpi), r.top, r.right - (int)(18 * g.dpi), r.bottom };
            std::wstring nm = g.textFont;
            DrawTextW(dc, nm.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            SelectObject(dc, AbMdlFont(12));
            RECT cr2{ r.right - (int)(16 * g.dpi), r.top, r.right, r.bottom };
            DrawTextW(dc, L"\uE70D", -1, &cr2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, AbFont11());
        } else if (id >= AB_BOLD && id <= AB_UNDER) {
            bool act = (id == AB_BOLD && g.textBold) || (id == AB_ITALIC && g.textItalic) ||
                       (id == AB_STRIKE && g.textStrike) || (id == AB_UNDER && g.textUnder);
            if (act) {
                Gdiplus::SolidBrush bg(Gdiplus::Color(40, 0x22, 0xbb, 0x56));
                AbRoundRect(gs, r, 3.0f * (float)g.dpi, &bg);
            }
            Gdiplus::Pen pen(act ? Gdiplus::Color(255, 0x22, 0xbb, 0x56) : Gdiplus::Color(255, 0x44, 0x44, 0x44),
                             act ? 2.0f : 1.0f);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, nullptr, &pen);
            static HFONT fb = nullptr, fi = nullptr, fs = nullptr, fu = nullptr;
            HFONT f = id == AB_BOLD ? CaptionFont(fb, 12, true, L"Segoe UI")
                    : id == AB_ITALIC ? CaptionFont(fi, 12, false, L"Segoe UI")
                    : id == AB_UNDER ? CaptionFont(fu, 12, false, L"Segoe UI")
                    : CaptionFont(fs, 12, false, L"Segoe UI");
            if (id == AB_ITALIC && f) { LOGFONTW lf; GetObject(f, sizeof lf, &lf); lf.lfItalic = TRUE; HFONT f2 = CreateFontIndirectW(&lf); DeleteObject(fi); fi = f2; f = f2; }
            if (id == AB_UNDER && f) { LOGFONTW lf; GetObject(f, sizeof lf, &lf); lf.lfUnderline = TRUE; HFONT f2 = CreateFontIndirectW(&lf); DeleteObject(fu); fu = f2; f = f2; }
            if (id == AB_STRIKE && f) { LOGFONTW lf; GetObject(f, sizeof lf, &lf); lf.lfStrikeOut = TRUE; HFONT f2 = CreateFontIndirectW(&lf); DeleteObject(fs); fs = f2; f = f2; }
            SelectObject(dc, f);
            SetTextColor(dc, act ? RGB(255, 255, 255) : RGB(0xa3, 0xa3, 0xa3));
            DrawTextW(dc, id == AB_BOLD ? L"B" : id == AB_ITALIC ? L"I" : id == AB_STRIKE ? L"S" : L"U",
                      -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, AbFont11());
        } else if (id == AB_TMINUS || id == AB_TPLUS) {
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x44, 0x44, 0x44), 1.0f);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, nullptr, &pen);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            DrawTextW(dc, id == AB_TMINUS ? L"\u2212" : L"+", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_TBOX) {
            Gdiplus::SolidBrush f(Gdiplus::Color(255, 0x1c, 0x1c, 0x1c));
            AbRoundRect(gs, r, 2.0f * (float)g.dpi, &f);
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x40, 0x40, 0x40), 1.0f);
            AbRoundRect(gs, r, 2.0f * (float)g.dpi, nullptr, &pen);
            SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
            swprintf(tmp, 48, L"%d", (int)g.textSize);
            DrawTextW(dc, tmp, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (id == AB_PARA) {
            Gdiplus::Pen pen(Gdiplus::Color(255, 0x44, 0x44, 0x44), 1.0f);
            AbRoundRect(gs, r, 3.0f * (float)g.dpi, nullptr, &pen);
            SetTextColor(dc, g.barHot == AB_PARA ? RGB(255, 255, 255) : RGB(0xe0, 0xe0, 0xe0));
            DrawTextW(dc, L"Edit paragraph", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

// Hit-testing: -2 = not over the bar (the page under it gets the press), -1 =
// over the bar's padding (chrome: the press dies here), else the cell id.
static int AnnBarHit(POINT p) {
    if (!g.barOn) return AB_OUT;
    AbLayout();
    RECT r = AnnBarRect();
    if (p.x < r.left || p.x >= r.right || p.y < r.top || p.y >= r.bottom) return AB_OUT;
    for (const AbCell& c : g_ab)
        if (c.id != -1 && p.x >= r.left + c.r.left && p.x < r.left + c.r.right &&
            p.y >= r.top + c.r.top && p.y < r.top + c.r.bottom) return c.id;
    return AB_NONE;
}

// ---- the bar's popups: the font list, the color picker, the Transform menu,
// the size edit and the paragraph editor ----
static std::vector<std::wstring>* g_fonts = nullptr;
static int CALLBACK FontEnumProc(const ENUMLOGFONTEXW* lpelfe, const NEWTEXTMETRICW*, DWORD, LPARAM lp) {
    auto* v = (std::vector<std::wstring>*)lp;
    std::wstring n = lpelfe->elfLogFont.lfFaceName;
    if (!n.empty() && n[0] != '@') v->push_back(n);
    return 1;
}
static std::vector<std::wstring>& FontList() {
    if (!g_fonts) {
        g_fonts = new std::vector<std::wstring>();
        LOGFONTW lf{}; lf.lfCharSet = DEFAULT_CHARSET;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)FontEnumProc, (LPARAM)g_fonts, 0);
        ReleaseDC(nullptr, dc);
        std::sort(g_fonts->begin(), g_fonts->end());
        g_fonts->erase(std::unique(g_fonts->begin(), g_fonts->end()), g_fonts->end());
        auto it = std::find(g_fonts->begin(), g_fonts->end(), L"Segoe UI");
        if (it != g_fonts->end()) g_fonts->erase(it);
        g_fonts->insert(g_fonts->begin(), L"Segoe UI");
    }
    return *g_fonts;
}

// A small menu-like list popup (the font combo's dropdown, the Transform
// menu): CS_SAVEBITS rows over #141414, closes on deactivate like a menu.
struct ListPop { std::vector<std::wstring> items; int hot = -1, scroll = 0, who = 0; };
static ListPop g_lp;
static void ListPopClose() {
    if (g.fontPop) { DestroyWindow(g.fontPop); g.fontPop = nullptr; }
}
static LRESULT CALLBACK ListPopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) ListPopClose();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, PopBrush());
        SetBkMode(dc, TRANSPARENT);
        int rowH = (int)(22 * g.dpi);
        SelectObject(dc, AbFont11());
        for (int i = g_lp.scroll; i < (int)g_lp.items.size(); i++) {
            int y = (int)(4 * g.dpi) + (i - g_lp.scroll) * rowH;
            if (y + rowH > r.bottom) break;
            RECT rr{ 2, y, r.right - 2, y + rowH };
            if (i == g_lp.hot) FillRect(dc, &rr, PopHoverBrush());
            if (g_lp.who == 0 && g_lp.items[i] == g.textFont) {
                SetTextColor(dc, RGB(0x22, 0xbb, 0x56));
                RECT mr{ rr.right - (int)(20 * g.dpi), y, rr.right, y + rowH };
                SelectObject(dc, AbMdlFont(11));
                DrawTextW(dc, L"\uE73E", -1, &mr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, AbFont11());
            }
            SetTextColor(dc, i == g_lp.hot ? RGB(255, 255, 255) : RGB(0xe0, 0xe0, 0xe0));
            RECT tr{ rr.left + (int)(8 * g.dpi), y, rr.right - (int)(22 * g.dpi), y + rowH };
            DrawTextW(dc, g_lp.items[i].c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        FrameRect(dc, &r, ComboBorderBrush());
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int rowH = (int)(22 * g.dpi);
        int hot = -1;
        if (p.y >= (int)(4 * g.dpi)) {
            hot = g_lp.scroll + (p.y - (int)(4 * g.dpi)) / rowH;
            if (hot >= (int)g_lp.items.size()) hot = -1;
        }
        if (hot != g_lp.hot) { g_lp.hot = hot; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int rows = (int)g_lp.items.size();
        g_lp.scroll = std::clamp(g_lp.scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120 * 3, 0, max(0, rows - 1));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int rowH = (int)(22 * g.dpi);
        int hot = g_lp.scroll + max(0, (int)p.y - (int)(4 * g.dpi)) / rowH;
        if (hot < 0 || hot >= (int)g_lp.items.size()) { ListPopClose(); return 0; }
        if (g_lp.who == 0) {
            g.textFont = g_lp.items[hot];
            ApplyTextStyleToSelection();
        } else {
            bool b = FocusB();
            DocState& D = DV(b);
            int pg = PageAt(D, FocusSy() + g.ch / 3);
            if (D.n) {
                if (hot == 0) TransformApply(b, pg, 1, false, false);
                else if (hot == 1) TransformApply(b, pg, 3, false, false);
                else if (hot == 2) TransformApply(b, pg, 0, true, false);
                else if (hot == 3) TransformApply(b, pg, 0, false, true);
            }
        }
        ListPopClose();
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void ListPopOpen(HWND owner, POINT p, std::vector<std::wstring> items, int who, int selRow) {
    ListPopClose();
    g_lp.items = items; g_lp.hot = -1; g_lp.who = who;
    g_lp.scroll = std::clamp(selRow - 3, 0, max(0, (int)items.size() - 1));
    int rowH = (int)(22 * g.dpi);
    int w = (int)(200 * g.dpi);
    int rows = min(12, (int)items.size());
    int h2 = (int)(8 * g.dpi) + rows * rowH;
    g.fontPop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DocsyListPop", nullptr,
                                WS_POPUP | WS_BORDER, p.x, p.y, w, h2, owner, nullptr, g.hinst, nullptr);
    if (!g.fontPop) return;
    SetForegroundWindow(g.fontPop);
    ShowWindow(g.fontPop, SW_SHOWNA);
    InvalidateRect(g.fontPop, nullptr, FALSE);
}

// The color picker popup ("More colors..."): three sliders for R, G and B,
// the working color preview, Accept and Cancel - KillerPDF's picker reduced
// to the channels its bars actually set.
struct ColorPop { int r = 255, g = 0, b = 0, slide = -1; };
static ColorPop g_cp;
static void ColorPopClose(bool accept) {
    if (!g.colorPop) return;
    HWND h = g.colorPop; g.colorPop = nullptr;
    DestroyWindow(h);
    if (accept) {
        COLORREF c = RGB(g_cp.r, g_cp.g, g_cp.b);
        switch (g.colorTarget) {
        case 1: g.drawColor = c; ApplyDrawStyleToSelection(); break;
        case 2: g.hlRGB = c; ApplyDrawStyleToSelection(); break;
        case 3: g.textRGB = c; ApplyTextStyleToSelection(); break;
        case 4: g.textFillRGB = c; if (g.textFillA == 0) g.textFillA = 255; ApplyTextStyleToSelection(); break;
        }
    }
    AnnBarSync(true);
}
static LRESULT CALLBACK ColorPopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) ColorPopClose(false);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, PopBrush());
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, AbFont11());
        int pad = (int)(10 * g.dpi);
        COLORREF c = RGB(g_cp.r, g_cp.g, g_cp.b);
        RECT pv{ pad, pad, r.right - pad, pad + (int)(26 * g.dpi) };
        HBRUSH pb = CreateSolidBrush(c);
        FillRect(dc, &pv, pb); DeleteObject(pb);
        FrameRect(dc, &pv, ComboBorderBrush());
        const wchar_t* labs[3] = { L"R", L"G", L"B" };
        int vals[3] = { g_cp.r, g_cp.g, g_cp.b };
        for (int i = 0; i < 3; i++) {
            int y = pad + (int)(34 * g.dpi) + i * (int)(26 * g.dpi);
            SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
            RECT lr{ pad, y, pad + (int)(14 * g.dpi), y + (int)(18 * g.dpi) };
            DrawTextW(dc, labs[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            int tx = pad + (int)(20 * g.dpi);
            RECT tr{ tx, y + (int)(7 * g.dpi), tx + (int)(90 * g.dpi), y + (int)(11 * g.dpi) };
            HBRUSH tb = CreateSolidBrush(RGB(0x3f, 0x3f, 0x3f));
            FillRect(dc, &tr, tb); DeleteObject(tb);
            int hx = tx + (int)((vals[i] / 255.0) * (90.0 * g.dpi - 12.0 * g.dpi));
            RECT th{ hx, y, hx + (int)(12 * g.dpi), y + (int)(18 * g.dpi) };
            HBRUSH hb2 = CreateSolidBrush(RGB(0x22, 0xbb, 0x56));
            FillRect(dc, &th, hb2); DeleteObject(hb2);
            wchar_t vs[8]; swprintf(vs, 8, L"%d", vals[i]);
            SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
            RECT vr{ tx + (int)(96 * g.dpi), y, tx + (int)(130 * g.dpi), y + (int)(18 * g.dpi) };
            DrawTextW(dc, vs, -1, &vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        int by = pad + (int)(34 * g.dpi) + 3 * (int)(26 * g.dpi) + (int)(6 * g.dpi);
        RECT ok{ pad, by, pad + (int)(64 * g.dpi), by + (int)(24 * g.dpi) };
        RECT no{ pad + (int)(72 * g.dpi), by, pad + (int)(72 * g.dpi) + (int)(64 * g.dpi), by + (int)(24 * g.dpi) };
        FillRect(dc, &ok, g_cp.slide == 100 ? PopHoverBrush() : ComboFillBrush());
        FrameRect(dc, &ok, ComboBorderBrush());
        FillRect(dc, &no, g_cp.slide == 101 ? PopHoverBrush() : ComboFillBrush());
        FrameRect(dc, &no, ComboBorderBrush());
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        DrawTextW(dc, L"OK", -1, &ok, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(dc, L"Cancel", -1, &no, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        FrameRect(dc, &r, ComboBorderBrush());
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int pad = (int)(10 * g.dpi);
        int tx = pad + (int)(20 * g.dpi);
        bool changed = false;
        if (g_cp.slide >= 0 && g_cp.slide < 3 && p.x >= tx && p.x <= tx + (int)(90 * g.dpi)) {
            int v = (int)std::lround(std::clamp((p.x - tx) / (90.0 * g.dpi - 12.0 * g.dpi), 0.0, 1.0) * 255);
            int* ch[3] = { &g_cp.r, &g_cp.g, &g_cp.b };
            if (*ch[g_cp.slide] != v) { *ch[g_cp.slide] = v; changed = true; }
        }
        int hot = -1;
        if (p.y > r.bottom - (int)(34 * g.dpi)) hot = p.x < pad + (int)(68 * g.dpi) ? 100 : 101;
        if (hot != g_cp.slide && hot >= 0) g_cp.slide = hot;
        if (changed || hot >= 0) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int pad = (int)(10 * g.dpi);
        int tx = pad + (int)(20 * g.dpi);
        for (int i = 0; i < 3; i++) {
            int y = pad + (int)(34 * g.dpi) + i * (int)(26 * g.dpi);
            if (p.y >= y - 2 && p.y < y + (int)(20 * g.dpi) && p.x >= tx - 6 && p.x <= tx + (int)(92 * g.dpi)) {
                g_cp.slide = i;
                int v = (int)std::lround(std::clamp((p.x - tx) / (90.0 * g.dpi - 12.0 * g.dpi), 0.0, 1.0) * 255);
                int* ch[3] = { &g_cp.r, &g_cp.g, &g_cp.b };
                *ch[i] = v;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        if (p.y > r.bottom - (int)(34 * g.dpi))
            ColorPopClose(p.x < pad + (int)(68 * g.dpi));
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void ColorPickerOpen(int target, COLORREF cur) {
    if (g.colorPop) ColorPopClose(false);
    g.colorTarget = target;
    g_cp.r = GetRValue(cur); g_cp.g = GetGValue(cur); g_cp.b = GetBValue(cur);
    g_cp.slide = -1;
    int pad = (int)(10 * g.dpi);
    int w = pad * 2 + (int)(150 * g.dpi), h2 = (int)(34 * g.dpi) + 3 * (int)(26 * g.dpi) + (int)(38 * g.dpi) + pad;
    RECT br = AnnBarRect();
    g.colorPop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DocsyColorPop", nullptr,
                                 WS_POPUP | WS_BORDER, br.right - w, br.bottom + 4, w, h2,
                                 g.main, nullptr, g.hinst, nullptr);
    if (!g.colorPop) return;
    SetForegroundWindow(g.colorPop);
    ShowWindow(g.colorPop, SW_SHOWNA);
    InvalidateRect(g.colorPop, nullptr, FALSE);
}

// The Transform menu (the toolbar button): quarter turns and flips on the
// focused pane's current page - KillerPDF's rasterizing TransformWindow,
// reduced to the session transforms docsy can apply without re-encoding.
static void TransformPopupOpen() {
    DocState& D = DVF();
    if (!D.n) return;
    RECT wr; GetWindowRect(g.bar, &wr);
    RECT bs[TB_COUNT]; TbRects(wr.right - wr.left, bs);
    int x = bs[TB_TRANSFORM].left, y = bs[TB_TRANSFORM].bottom;
    ListPopOpen(g.main, { wr.left + x, wr.top + y + 2 },
                { L"Rotate 90\u00B0 clockwise", L"Rotate 90\u00B0 counter-clockwise",
                  L"Flip horizontal", L"Flip vertical" }, 1, -1);
}

// The size box: a tiny popup EDIT over the cell; Enter/focus-loss commits
// (clamped 1-400pt), Escape reverts - KillerPDF's CommitSizeBox. The subclass
// chains to the stock EDIT proc (g_sizeEditDef, the g.teditDef pattern):
// DefWindowProcW implements no editing at all, so the box once ate every key -
// typing, Backspace, Del, Ctrl+A all landed in the void.
static WNDPROC g_sizeEditDef = nullptr;
static LRESULT CALLBACK SizeEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(h, EM_SETSEL, 0, -1); return 0;   // the reference's SelectAll chord
        }
        if (wp == VK_RETURN) {
            wchar_t b[8] = L"";
            GetWindowTextW(h, b, 8);
            g.textSize = std::clamp((float)_wtof(b), 1.0f, 400.0f);
            ApplyTextStyleToSelection();
            DestroyWindow(h); g.sizeEdit = nullptr;
            return 0;
        }
        if (wp == VK_ESCAPE) { DestroyWindow(h); g.sizeEdit = nullptr; return 0; }
    }
    if (m == WM_KILLFOCUS) {
        wchar_t b[8] = L"";
        GetWindowTextW(h, b, 8);
        if (b[0]) {
            g.textSize = std::clamp((float)_wtof(b), 1.0f, 400.0f);
            ApplyTextStyleToSelection();
        }
        DestroyWindow(h); g.sizeEdit = nullptr;
        return 0;
    }
    return g_sizeEditDef ? CallWindowProcW(g_sizeEditDef, h, m, wp, lp)
                         : DefWindowProcW(h, m, wp, lp);
}
static void SizeEditOpen(POINT p) {
    if (g.sizeEdit) { DestroyWindow(g.sizeEdit); g.sizeEdit = nullptr; }
    g.sizeEdit = CreateWindowExW(WS_EX_TOPMOST, L"EDIT", nullptr,
                                 WS_POPUP | WS_BORDER | ES_NUMBER,
                                 p.x, p.y, (int)(32 * g.dpi), (int)(20 * g.dpi),
                                 g.main, nullptr, g.hinst, nullptr);
    if (!g.sizeEdit) return;
    wchar_t b[8]; swprintf(b, 8, L"%d", (int)g.textSize);
    SetWindowTextW(g.sizeEdit, b);
    SendMessageW(g.sizeEdit, EM_SETLIMITTEXT, 4, 0);
    g_sizeEditDef = (WNDPROC)GetWindowLongPtrW(g.sizeEdit, GWLP_WNDPROC);
    SetWindowLongPtrW(g.sizeEdit, GWLP_WNDPROC, (LONG_PTR)SizeEditProc);
    SendMessageW(g.sizeEdit, WM_SETFONT, (WPARAM)AbFont11(), TRUE);
    SetForegroundWindow(g.sizeEdit);
    SetFocus(g.sizeEdit);
    ShowWindow(g.sizeEdit, SW_SHOW);
}

// The paragraph editor ("Edit paragraph"): the selected text box's content in
// a multiline field - KillerPDF's ParagraphEditDialog reduced to what a
// session overlay can rewrite (the box's own text).
struct ParaDlg { int hot = -1; };
static ParaDlg g_pd;
static void ParaClose(bool apply) {
    if (!g.paraDlg) return;
    HWND h = g.paraDlg; g.paraDlg = nullptr;
    HWND ed = GetWindow(h, GW_CHILD);
    std::wstring s;
    if (ed) {
        int len = GetWindowTextLengthW(ed);
        s.resize(len); GetWindowTextW(ed, s.data(), len + 1);
    }
    DestroyWindow(h);
    if (apply && g.annSelPage >= 0 && g.annSelIdx >= 0) {
        DocState& D = DV(g.annSelB);
        auto it = D.annos.find(g.annSelPage);
        if (it != D.annos.end() && g.annSelIdx < (int)it->second.size() &&
            it->second[g.annSelIdx].type == AT_TEXT) {
            UndoPush(g.annSelB);
            Annot& a = it->second[g.annSelIdx];
            a.text = s;
            float k = (float)(D.zoom * g.dpi);
            HDC m = GetDC(g.view);
            int hpx = max(8, (int)std::lround(a.fsize * k));
            HFONT f = CreateFontW(-hpx, 0, 0, 0, a.bold ? FW_BOLD : FW_NORMAL,
                                  a.italic, a.under, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, a.font.c_str());
            HGDIOBJ of = SelectObject(m, f);
            RECT mr{ 0, 0, (int)(a.w * k), 10000 };
            DrawTextW(m, a.text.c_str(), -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            SelectObject(m, of); DeleteObject(f); ReleaseDC(g.view, m);
            a.h = max(24.0f, (mr.bottom - mr.top) / max(0.01f, k));
        }
    } else if (apply) SetStatusHold(L"Select a text box first");
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}
static LRESULT CALLBACK ParaDlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        SetBkColor(dc, RGB(0x1c, 0x1c, 0x1c));
        static HBRUSH b = CreateSolidBrush(RGB(0x1c, 0x1c, 0x1c));
        return (LRESULT)b;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_KILLFOCUS) return 0;   // clicking Apply must not kill the edit's text
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, PopBrush());
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, AbFont11());
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        RECT tr{ (int)(10 * g.dpi), (int)(8 * g.dpi), r.right, (int)(24 * g.dpi) };
        DrawTextW(dc, L"Edit paragraph", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int by = r.bottom - (int)(34 * g.dpi);
        RECT ok{ (int)(10 * g.dpi), by, (int)(10 * g.dpi) + (int)(64 * g.dpi), by + (int)(24 * g.dpi) };
        RECT no{ (int)(82 * g.dpi), by, (int)(82 * g.dpi) + (int)(64 * g.dpi), by + (int)(24 * g.dpi) };
        FillRect(dc, &ok, g_pd.hot == 1 ? PopHoverBrush() : ComboFillBrush());
        FrameRect(dc, &ok, ComboBorderBrush());
        FillRect(dc, &no, g_pd.hot == 2 ? PopHoverBrush() : ComboFillBrush());
        FrameRect(dc, &no, ComboBorderBrush());
        DrawTextW(dc, L"Apply", -1, &ok, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(dc, L"Close", -1, &no, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        FrameRect(dc, &r, ComboBorderBrush());
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int by = r.bottom - (int)(34 * g.dpi);
        int hot = p.y >= by && p.y < by + (int)(24 * g.dpi)
                ? (p.x < (int)(76 * g.dpi) ? 1 : p.x < (int)(150 * g.dpi) ? 2 : 0) : 0;
        if (hot != g_pd.hot) { g_pd.hot = hot; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int by = r.bottom - (int)(34 * g.dpi);
        if (p.y >= by && p.y < by + (int)(24 * g.dpi))
            ParaClose(p.x < (int)(76 * g.dpi));
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}
// The paragraph field's keys: the reference's WPF TextBox selects all on
// Ctrl+A - a plain EDIT ships no such chord (the find box's precedent) - and
// everything else stays the stock edit's own: Del, Backspace, caret moves.
static LRESULT CALLBACK ParaEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN && wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    WNDPROC orig = (WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA);
    return orig ? CallWindowProcW(orig, h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}
static void ParagraphOpen() {
    if (g.paraDlg) ParaClose(false);
    std::wstring cur;
    if (g.annSelPage >= 0 && g.annSelIdx >= 0) {
        DocState& D = DV(g.annSelB);
        auto it = D.annos.find(g.annSelPage);
        if (it != D.annos.end() && g.annSelIdx < (int)it->second.size() &&
            it->second[g.annSelIdx].type == AT_TEXT)
            cur = it->second[g.annSelIdx].text;
    }
    RECT br = AnnBarRect();
    int w = (int)(360 * g.dpi), h2 = (int)(220 * g.dpi);
    g.paraDlg = CreateWindowExW(WS_EX_TOOLWINDOW, L"DocsyPara", nullptr,
                                WS_POPUP | WS_BORDER, br.right - w, br.bottom + 4, w, h2,
                                g.main, nullptr, g.hinst, nullptr);
    if (!g.paraDlg) return;
    HWND ed = CreateWindowExW(0, L"EDIT", cur.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_MULTILINE |
                              ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
                              (int)(10 * g.dpi), (int)(28 * g.dpi), w - (int)(20 * g.dpi),
                              h2 - (int)(72 * g.dpi), g.paraDlg, nullptr, g.hinst, nullptr);
    SendMessageW(ed, WM_SETFONT, (WPARAM)UiFont(), TRUE);
    SendMessageW(ed, EM_SETLIMITTEXT, 10000, 0);
    SetWindowLongPtrW(ed, GWLP_USERDATA, GetWindowLongPtrW(ed, GWLP_WNDPROC));
    SetWindowLongPtrW(ed, GWLP_WNDPROC, (LONG_PTR)ParaEditProc);
    ShowWindow(g.paraDlg, SW_SHOWNA);
    InvalidateRect(g.paraDlg, nullptr, FALSE);
}

// ---- the bar's interactions, driven by the view's mouse messages ----
static int AnnBarSliderAt(int id) {   // the track's left edge, bar-local
    for (const AbCell& c : g_ab) if (c.id == id) return c.r.left;
    return 0;
}
static void AnnBarSlideValue(HWND h, POINT p) {
    RECT br = AnnBarRect();
    int lx = AnnBarSliderAt(g.barSlide);
    float t = std::clamp((p.x - br.left - lx) / (90.0f * (float)g.dpi), 0.0f, 1.0f);
    switch (g.barSlide) {
    case AB_SIZE: {
        int v = std::clamp(1 + (int)std::lround(t * 59), 1, 60);
        if (g.drawWidth != v) { g.drawWidth = (float)v; ApplyDrawStyleToSelection(); }
        break;
    }
    case AB_OPACITY: {
        int v = std::clamp(10 + (int)std::lround(t * 245), 10, 255);
        if (g.drawOpacity != v) { g.drawOpacity = (BYTE)v; ApplyDrawStyleToSelection(); }
        break;
    }
    case AB_SPACING: {
        float v = -2 + t * 22;
        v = std::round(v * 4) / 4;
        if (g.textSpacing != v) { g.textSpacing = v; ApplyTextStyleToSelection(); }
        break;
    }
    case AB_TOACITY: {
        int v = std::clamp(10 + (int)std::lround(t * 245), 10, 255);
        if (g.textA != v) { g.textA = (BYTE)v; ApplyTextStyleToSelection(); }
        break;
    }
    case AB_FOACITY: {
        int v = std::clamp(10 + (int)std::lround(t * 245), 10, 255);
        if (g.textFillA != v) { g.textFillA = (BYTE)v; ApplyTextStyleToSelection(); }
        break;
    }
    case AB_TSIZE: {
        float v = 8 + t * 64;
        if (g.textSize != v) { g.textSize = v; ApplyTextStyleToSelection(); }
        break;
    }
    }
    AnnBarSync(true);
}
static void AnnBarPress(HWND h, POINT p, int cell) {
    switch (cell) {
    case AB_GRIP:
        g.barDrag = true; g.barDragX = p.x; g.barDx0 = g.barDx;
        return;
    case AB_SIZE: case AB_OPACITY: case AB_SPACING: case AB_TOACITY: case AB_FOACITY: case AB_TSIZE:
        g.barSlide = cell; g.barSlideX0 = p.x;
        AnnBarSlideValue(h, p);
        return;
    case AB_RW_DRAW: ColorPickerOpen(1, g.drawColor); return;
    case AB_RW_TEXT: ColorPickerOpen(3, g.textRGB); return;
    case AB_RW_FILL: ColorPickerOpen(4, g.textFillRGB); return;
    case AB_FILL_NONE:
        g.textFillA = 0;
        ApplyTextStyleToSelection();
        return;
    case AB_LEVEL: g.lineLevel = !g.lineLevel; AnnBarSync(true); return;
    case AB_ERASE: g.drawErase = !g.drawErase; AnnBarSync(true); return;
    case AB_HLERASE: g.hlErase = !g.hlErase; AnnBarSync(true); return;
    case AB_FILLCHK: g.shapeFill = !g.shapeFill; AnnBarSync(true); return;
    case AB_KIND0: case AB_KIND1: case AB_KIND2:
        g.shapeKind = cell - AB_KIND0;
        AnnBarSync(true);
        return;
    case AB_FONT: {
        RECT br = AnnBarRect();
        for (const AbCell& c : g_ab) if (c.id == AB_FONT) {
            ListPopOpen(g.main, { br.left + c.r.left, br.bottom + 4 }, FontList(), 0, -1);
            break;
        }
        return;
    }
    case AB_BOLD: g.textBold = !g.textBold; ApplyTextStyleToSelection(); return;
    case AB_ITALIC: g.textItalic = !g.textItalic; ApplyTextStyleToSelection(); return;
    case AB_STRIKE: g.textStrike = !g.textStrike; ApplyTextStyleToSelection(); return;
    case AB_UNDER: g.textUnder = !g.textUnder; ApplyTextStyleToSelection(); return;
    case AB_TMINUS: g.textSize = std::clamp(g.textSize - 1, 1.0f, 400.0f); ApplyTextStyleToSelection(); return;
    case AB_TPLUS: g.textSize = std::clamp(g.textSize + 1, 1.0f, 400.0f); ApplyTextStyleToSelection(); return;
    case AB_TBOX: {
        RECT br = AnnBarRect();
        for (const AbCell& c : g_ab) if (c.id == AB_TBOX) {
            SizeEditOpen({ br.left + c.r.left, br.top + c.r.top });
            break;
        }
        return;
    }
    case AB_PARA: ParagraphOpen(); return;
    default:
        if (cell >= AB_SWC_DRAW && cell < AB_RW_DRAW) {
            COLORREF c = AbCellColor(cell);
            if (g.barTool == TOOL_HIGHLIGHT) g.hlRGB = c;   // the highlight bar retints its own
            else g.drawColor = c;
            ApplyDrawStyleToSelection();
            AnnBarSync(true);
        } else if (cell >= AB_SWC_TEXT && cell < AB_RW_TEXT) {
            g.textRGB = AbCellColor(cell);
            ApplyTextStyleToSelection();
        } else if (cell >= AB_SWC_FILL && cell < AB_RW_FILL) {
            g.textFillRGB = AbCellColor(cell);
            if (g.textFillA == 0) g.textFillA = 255;
            ApplyTextStyleToSelection();
        }
        return;
    }
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
    case WM_KEYDOWN:                         // keys live on the main window: forward, never
        SendMessageW(g.main, m, wp, lp);     // swallow - a stray focus here once ate Del and
        return 0;                            // every other shortcut until the next edit
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
        if (g.barHot != AB_OUT) { g.barHot = AB_OUT; AnnBarSync(true); }
        if (g.lineHot) { g.lineHot = false; InvalidateRect(h, nullptr, FALSE); }
        if (g.brushOn) {                    // the brush ring dies with the hover
            RECT u{ g.brushPt.x - 12, g.brushPt.y - 12, g.brushPt.x + 12, g.brushPt.y + 12 };
            g.brushOn = false;
            InvalidateRect(h, &u, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
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
    case WM_CAPTURECHANGED:
        fd.drag = false;                       // someone else took it (a modal loop, a close)
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
}

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
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(h, EM_SETSEL, 0, -1); return 0;   // the reference's SelectAll chord
        }
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

// KillerPDF's NudgeSelection: with a Select-tool annotation chosen, the arrow
// keys move it (Shift = 10pt steps) instead of scrolling the pane.
static bool NudgeSel(WPARAM wp, bool shift) {
    if (g.annSelPage < 0 || g.annSelIdx < 0) return false;
    DocState& D = DV(g.annSelB);
    auto it = D.annos.find(g.annSelPage);
    if (it == D.annos.end() || g.annSelIdx >= (int)it->second.size()) return false;
    UndoPush(g.annSelB);
    float step = shift ? 10.0f : 1.0f;
    float dx = wp == VK_LEFT ? -step : wp == VK_RIGHT ? step : 0;
    float dy = wp == VK_UP ? -step : wp == VK_DOWN ? step : 0;
    Annot& a = it->second[g.annSelIdx];
    if (a.type == AT_HIGHLIGHT) {
        for (FRect& r : a.lines) { r.x += dx; r.y += dy; }
        a.x += dx; a.y += dy;
    } else if (a.type == AT_INK) {
        for (size_t i = 0; i < a.ix.size(); i++) { a.ix[i] += dx; a.iy[i] += dy; }
    } else { a.x += dx; a.y += dy; }
    InvalidateRect(g.view, nullptr, FALSE);
    return true;
}

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
        case 'O':
            if (ctrl) OpenDlg();
            else if (shift && DVF().n) {           // KillerPDF's CycleShapeKind: Shift+O walks
                g.shapeKind = (g.shapeKind + 1) % 3;   // Box -> Ellipse -> Freeform -> Box
                if (DVF().tool == TOOL_SHAPE) AnnBarSync(true);
            } else if (DVF().n) SetTool(TOOL_SHAPE, FocusB());
            break;
        case 'N':                                  // KillerPDF's file cluster: Ctrl+N / Ctrl+W
            if (ctrl && !shift) NewBlank();
            break;
        case 'W':
            if (ctrl && !shift) CloseDoc();
            break;
        case 'S':
            if (ctrl) SaveDoc(shift);              // Ctrl+S saves in place, Ctrl+Shift+S asks
            break;
        // KillerPDF's one-letter tool keys (KeyboardShortcuts.cs TryToolShortcut):
        // one letter per tool, so a key keeps its tool however the bar grows -
        // Select is V (the Photoshop convention), Shapes O, Line also U. They
        // need an open document, exactly like the buttons they drive.
        case 'V': if (!ctrl && !shift && DVF().n) SetTool(TOOL_SELECT, FocusB()); break;
        case 'T': if (!ctrl && !shift && DVF().n) SetTool(TOOL_TEXT, FocusB()); break;
        case 'H': if (!ctrl && !shift && DVF().n) SetTool(TOOL_HIGHLIGHT, FocusB()); break;
        case 'L': case 'U':
            if (!ctrl && !shift && DVF().n) SetTool(TOOL_LINE, FocusB()); break;
        case 'D': if (!ctrl && !shift && DVF().n) SetTool(TOOL_DRAW, FocusB()); break;
        case VK_DOWN:
            if (NudgeSel(wp, shift)) break;
            ScrollTo(fx, fy + line);
            break;
        case VK_UP:
            if (NudgeSel(wp, shift)) break;
            ScrollTo(fx, fy - line);
            break;
        case VK_RIGHT:
            if (NudgeSel(wp, shift)) break;
            ScrollTo(fx + line, fy);
            break;
        case VK_LEFT:
            if (NudgeSel(wp, shift)) break;
            ScrollTo(fx - line, fy);
            break;
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
        case 'F':
            if (ctrl) FindToggle();
            break;
        case 'R':
            if (!ctrl && !shift && DVF().n) TransformPopupOpen();   // Transform (R)
            break;
        case 'Z':
            if (ctrl) { if (shift) RedoOp(); else UndoOp(); }   // Ctrl+Z / Ctrl+Shift+Z
            break;
        case 'Y':
            if (ctrl) RedoOp();
            break;
        case 'C': if (ctrl && !DVF().selText.empty()) CopyClipboard(DVF().selText); break;
        case 'A': if (ctrl) SelAll(DVF()); break;
        case VK_DELETE:
            // KillerPDF's DeleteSelected: the Select tool's chosen annotation
            // leaves its pane's document, one undo entry covers it
            if (g.annSelPage >= 0 && g.annSelIdx >= 0) {
                DocState& D2 = DV(g.annSelB);
                auto it = D2.annos.find(g.annSelPage);
                if (it != D2.annos.end() && g.annSelIdx < (int)it->second.size()) {
                    UndoPush(g.annSelB);
                    it->second.erase(it->second.begin() + g.annSelIdx);
                    if (it->second.empty()) D2.annos.erase(it);
                }
                g.annSelPage = -1; g.annSelIdx = -1;
                if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
                InvalidateRect(g.view, nullptr, FALSE);
            }
            break;
        case VK_F3:                                // Find next / previous; closed, F3 opens
            if (!fd.open) FindOpen();
            else if (!fd.hits.empty()) FindStep(shift ? -1 : 1);
            break;
        case VK_ESCAPE:
            if (g.annPoly) PolyCancel(h);          // the polygon session backs out first
            else if (g.tedit) CommitTextEdit(false, true);   // ESC backs the box out: a
                                                             // re-edit's original returns
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
        case VK_RETURN:
            if (g.annPoly) { PolyCommit(h); break; }   // the polygon closes on Enter
            break;
        case VK_BACK:
            if (g.annPoly && !g.annPX.empty()) {  // Backspace pops the last vertex
                g.annPX.pop_back(); g.annPY.pop_back();
                InvalidateRect(g.view, nullptr, FALSE);
            }
            break;
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

// ---------------- Transform (the toolbar's action) ----------------

// The Transform menu's apply: the page's session transform - KillerPDF
// rasterizes the page for these (its TransformWindow), and the annotations
// rotate with the raster; docsy rotates the RENDER and the page's own DIP
// space, so every downstream path (layout, hit tests, annotation paint, the
// OCR re-read) lands in the new space untouched. Flips apply first, then the
// quarter-turns - the same compose order Render() bakes.
static void TransformApply(bool b, int page, int turns, bool flipH, bool flipV) {
    DocState& D = DV(b);
    if (!D.n || page < 0 || page >= D.n) return;
    CommitTextEdit();
    float W = D.pw[page], H = D.ph[page];
    // flip the annotations into the new space first (flips ride the CURRENT dims)
    if (flipH || flipV) {
        D.pflip[page] = (BYTE)((D.pflip.count(page) ? D.pflip[page] : 0) ^ (flipH ? 1 : 0) ^ (flipV ? 2 : 0));
        auto it = D.annos.find(page);
        if (it != D.annos.end())
            for (Annot& a : it->second) {
                if (flipH) {
                    if (a.type == AT_HIGHLIGHT)
                        for (FRect& r : a.lines) r.x = W - r.x - r.w;
                    else if (a.type == AT_INK)
                        for (size_t i2 = 0; i2 < a.ix.size(); i2++) a.ix[i2] = W - a.ix[i2];
                    else { a.x = W - a.x - a.w; }
                }
                if (flipV) {
                    if (a.type == AT_HIGHLIGHT)
                        for (FRect& r : a.lines) r.y = H - r.y - r.h;
                    else if (a.type == AT_INK)
                        for (size_t i2 = 0; i2 < a.iy.size(); i2++) a.iy[i2] = H - a.iy[i2];
                    else { a.y = H - a.y - a.h; }
                }
            }
    }
    // then the quarter-turns, dims swapping each time
    int rot = ((D.prot.count(page) ? D.prot[page] : 0) + turns) & 3;
    for (int t = 0; t < (turns & 3); t++) {
        auto it = D.annos.find(page);
        if (it != D.annos.end())
            for (Annot& a : it->second) {
                if (a.type == AT_HIGHLIGHT) {
                    for (FRect& r : a.lines) {
                        float nx = H - r.y - r.h, ny = r.x;
                        r.x = nx; r.y = ny; std::swap(r.w, r.h);
                    }
                    float nx = H - a.y - a.h, ny = a.x;
                    a.x = nx; a.y = ny; std::swap(a.w, a.h);
                } else if (a.type == AT_INK) {
                    for (size_t i2 = 0; i2 < a.ix.size(); i2++) {
                        float nx = H - a.iy[i2], ny = a.ix[i2];
                        a.ix[i2] = nx; a.iy[i2] = ny;
                    }
                } else {
                    float nx = H - a.y - a.h, ny = a.x;
                    a.x = nx; a.y = ny; std::swap(a.w, a.h);
                }
            }
        std::swap(D.pw[page], D.ph[page]);   // the page's own DIP space turns with it
        std::swap(W, H);
    }
    D.prot[page] = rot;
    D.txt.clear();                           // the text layer re-reads the new raster
    D.cache.erase(page);                     // GetBmp re-renders with the new rot bits
    g.annSelPage = -1; g.annSelIdx = -1;
    SelClear(D);
    Layout(D, b);
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    SetStatusHold(L"Transformed page " + std::to_wstring(page + 1));
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
        if (g.barOn) {                                  // the floating settings bar rides the shot
            RECT br3 = AnnBarRect();                    // like the live composite paints it
            AnnBarDraw(m2, br3.left, br3.top);
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

    WNDCLASSEXW lc{ sizeof lc };                    // the bars' menu-list popup (fonts, Transform)
    lc.style = CS_SAVEBITS | CS_DROPSHADOW;
    lc.lpfnWndProc = ListPopProc; lc.hInstance = hi;
    lc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    lc.lpszClassName = L"DocsyListPop";
    RegisterClassExW(&lc);

    WNDCLASSEXW cc2{ sizeof cc2 };                  // the color picker popup ("More colors...")
    cc2.style = CS_SAVEBITS | CS_DROPSHADOW;
    cc2.lpfnWndProc = ColorPopProc; cc2.hInstance = hi;
    cc2.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cc2.lpszClassName = L"DocsyColorPop";
    RegisterClassExW(&cc2);

    WNDCLASSEXW pc2{ sizeof pc2 };                  // the paragraph editor ("Edit paragraph")
    pc2.lpfnWndProc = ParaDlgProc; pc2.hInstance = hi;
    pc2.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc2.lpszClassName = L"DocsyPara";
    RegisterClassExW(&pc2);

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
                    Annot sh; sh.type = AT_HIGHLIGHT; sh.color = RGB(0xff, 0x00, 0x00); sh.alpha = 255;
                    sh.lines.push_back(FRect{ 60, 150, 180, 60 });
                    sh.x = 60; sh.y = 150; sh.w = 180; sh.h = 60;
                    D.annos[0].push_back(sh);
                    Annot el; el.type = AT_INK; el.width = 3; el.color = RGB(0x2E, 0x86, 0xDE);
                    for (int k2 = 0; k2 <= 64; k2++) {   // the ellipse's 64 segments
                        float ang = k2 * 6.2831853f / 64;
                        el.ix.push_back(240 + 80 * cosf(ang)); el.iy.push_back(180 + 40 * sinf(ang));
                    }
                    el.fill = RGB(0x2E, 0x86, 0xDE); el.fillA = 64;
                    D.annos[0].push_back(el);
                    Annot tx; tx.type = AT_TEXT; tx.fsize = 24;
                    tx.x = 60; tx.y = 270; tx.w = 165; tx.h = 30;
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
