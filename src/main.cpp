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
#include <tlhelp32.h>   // Toolhelp32: is another docsy.exe (pid) sharing the profile's engine?
#include <shcore.h>
#include <shlobj_core.h>
#include <commdlg.h>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
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
#include <wrl/client.h>
#include <wrl/event.h>   // Microsoft::WRL::Callback: the WebView2 COM event lambdas
#include <webview2.h>    // the WebView2 SDK - CI fetches the Microsoft.Web.WebView2 package
#include <WebView2EnvironmentOptions.h>   // CoreWebView2EnvironmentOptions: switches extension support on
#include <winhttp.h>     // the Chrome Web Store download
#include <thread>
#include <memory>
#include <cstdarg>
#include <functional>
#include <cstring>
#include <cctype>
#include <cstdlib>
// WebView2's SDK header declares every interface and entry point at global scope;
// the  prefix the first draft used is gone - the names are simply global.
namespace wrl = Microsoft::WRL;                   // ComPtr + Callback: the event lambdas

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
#pragma comment(lib, "winhttp")
#pragma comment(lib, "shcore")
#pragma comment(lib, "gdiplus")
#pragma comment(lib, "comdlg32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "advapi32")   // RegOpenKeyExW/RegEnumValueW: the policy audit
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "ole32")

using winrt::Windows::Data::Pdf::PdfDocument;
using winrt::Windows::Data::Pdf::PdfPageRenderOptions;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;
using winrt::Windows::Storage::Streams::DataWriter;

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
    long long cacheBytes = 0;           // the page cache's byte weight (w*h*4 per entry):
                                        // the LRU evicts against this, not a page count, so
                                        // a 3,000-page doc keeps hundreds of light pages and
                                        // a giant zoom keeps only what memory allows
    int id = 0;                         // stable handle the render worker answers to (the
                                        // tabs vector reallocates, so jobs carry this, never
                                        // a DocState*)
    unsigned long long tabTick = 0;     // last time this session was the front tab: the
                                        // render-bitmap LRU keeps the three newest (PdfViewer.Tabs.cs
                                        // RenderCacheTabCap) and drops the rest's page bitmaps
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
    // the saved checkpoint (KillerPDF's CaptureSavedCheckpoint): the state a
    // save would write - the annotations plus the page transforms - copied at
    // open and after each successful save. Dirtiness is a compare against it
    // (DocDirty below), not an event flag, so no mutation site can be
    // forgotten and Undo back to the checkpoint reads clean again.
    std::unordered_map<int, std::vector<Annot>> savedAnnos;
    std::unordered_map<int, int> savedProt;
    std::unordered_map<int, BYTE> savedPflip;
};
enum { TOOL_SELECT, TOOL_TEXT, TOOL_HIGHLIGHT, TOOL_LINE, TOOL_SHAPE, TOOL_DRAW,
       TOOL_COUNT };

// ---- dirty tracking (KillerPDF's saved checkpoint, snapshot-based) ----
// The saved PDF is the pages' renders (the document plus its transforms) with
// the annotations composited, so "clean" is exactly annos + prot + pflip
// matching the checkpoint. AnnotEq walks every field a save burns.
static void SnapClean(DocState& D) {
    D.savedAnnos = D.annos;
    D.savedProt = D.prot;
    D.savedPflip = D.pflip;
}
static bool FRectEq(const FRect& a, const FRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
static bool FVecEq(const std::vector<FRect>& a, const std::vector<FRect>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (!FRectEq(a[i], b[i])) return false;
    return true;
}
static bool AnnotEq(const Annot& a, const Annot& b) {
    return a.type == b.type && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h
        && FVecEq(a.lines, b.lines) && a.ix == b.ix && a.iy == b.iy
        && a.width == b.width && a.fsize == b.fsize && a.color == b.color
        && a.alpha == b.alpha && a.text == b.text && a.fill == b.fill
        && a.fillA == b.fillA && a.font == b.font && a.bold == b.bold
        && a.italic == b.italic && a.strike == b.strike && a.under == b.under
        && a.spacing == b.spacing;
}
static bool AnnosDirty(const std::unordered_map<int, std::vector<Annot>>& s,
                       const std::unordered_map<int, std::vector<Annot>>& c) {
    if (s.size() != c.size()) return true;
    for (const auto& kv : c) {
        auto it = s.find(kv.first);
        if (it == s.end() || it->second.size() != kv.second.size()) return true;
        for (size_t i = 0; i < kv.second.size(); i++)
            if (!AnnotEq(kv.second[i], it->second[i])) return true;
    }
    return false;
}
static bool DocDirty(const DocState& D) {   // any byte the next save would write differently?
    return D.n && (AnnosDirty(D.savedAnnos, D.annos) || D.savedProt != D.prot
                   || D.savedPflip != D.pflip);
}

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
    int nextDocId = 0;                  // handed to each opened document for the render queue
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
    // ---- tabs (KillerPDF's DocumentSession model, PdfViewer.Tabs.cs) ----
    // Every open document is a session; a pane owns its own list and its own
    // front tab, exactly like KillerPDF's per-pane _sessions/_active. The LIVE
    // working set stays g.d / g.dB - the front slot holds a stale copy while
    // its document is live, and a switch captures the live fields into the
    // outgoing slot and moves the incoming slot into the live fields (the
    // CaptureSessionState / ApplySessionState pair, reduced to docsy's value
    // world: a DocState move IS the capture). A pane that never tabbed keeps
    // its list empty and reads the live state alone, so the pre-tab paths are
    // byte-identical.
    std::vector<DocState> tabs[2];      // each pane's sessions, in strip order
    int act[2] = { 0, 0 };              // each pane's front session index
    int tabWin[2] = { 0, 0 };           // leftmost tab in each strip (the overflow window)
    // the strip's gestures (KillerPDF's Tab_DragDown/Move/Up + Shell/PaneDrag.cs):
    int tbTabHot = -1, tbTabHotPane = 0;   // hovered tab (visible order), -1 none
    int tbHotBtn = 0;                   // hovered strip element: 0 tab, 1 close x, 2 new +, 3 chevron
    int tbPending = -1;                 // pressed tab awaiting the mouse-up (the switch happens
                                        // on UP so a press can begin a drag - KillerPDF's rule)
    bool tbPendingX = false;            // the press landed on a close x, not the tab face
    bool tbOwnRelease = false;          // the UP's own ReleaseCapture: CAPTURECHANGED fires
                                        // inside it, and without this flag the handler wiped
                                        // the pending press before TabStripUp could read it -
                                        // the bug that made every tab click land nowhere
    bool tbDragging = false;            // past the 4px threshold: the tab is glued to the cursor
    int tbDragPane = 0, tbDragIdx = 0;  // the dragged tab's pane and visible index at arm time
    int tbDragGrab = 0;                 // cursor offset inside the grabbed tab
    int tbDragX = 0;                    // cursor x in view coords while glued
    int tbGhostPane = -1;               // -1 none; else the OTHER pane under the drag: ghost + caret
    int tbCaret = -1;                   // the ghost's insertion slot in that pane's strip
    int tbGlidePane = 0, tbGlideIdx = -1;   // the displaced neighbor's glide (one swap at a time)
    int tbGlideDX = 0;                  // its pixel offset, easing back to 0
    unsigned long long tbGlideT0 = 0;   // the glide's start tick
    int tbSettlePane = 0, tbSettleIdx = -1; // the released tab's settle glide to its slot
    int tbSettleDX = 0;
    unsigned long long tbSettleT0 = 0;
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
    std::vector<std::vector<UndoSnap>> tabUndo[2], tabRedo[2];   // each pane's per-session stacks
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
    long long thBytes = 0;              // the thumb cache's byte weight, evicted like the
                                        // page cache: thousands of small thumbs stay, huge
                                        // dpi-scaled ones give way
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
static bool g_webOn = false;             // the browser owns the window chrome (declared early:
                                         // the caption painter and the layout branch on it)
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

// The tab band's reserved top inset for a pane's document layout (defined with
// the tab section below): KillerPDF's strip is a layout row the card tucks
// under, so a pane with two or more tabs starts its pages below the band -
// the band paints over the covered strip, which is exactly the clip the
// ScrollViewer row gives the same pixels in KillerPDF.
static int BandInset(bool b);

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
static int AskPrompt3(const wchar_t* qualifier, const wchar_t* msg,    // KillerDialog's themed
                      const wchar_t* b0, const wchar_t* b1, const wchar_t* b2);   // question box
static bool WriteAnnotatedPdf(DocState& D, const std::wstring& file, std::wstring& err);
static bool TabOpenInto(bool b, const std::wstring& p);   // the tabbed open (OpenInNewTab)
static bool CloseTabAt(bool b, int idx);                  // CloseTabCore, false on a cancel
static void TabPopPick(int row);                          // the tab menu / chevron popup's pick
static int& Act(bool b);                                  // a pane's front session index
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

// The browser's palette (Safari, dark appearance). Everything in web mode is a
// FLAT surface - no grain: the caption row and the toolbar are one sheet, the
// tab band sits a step darker, the address field a step lighter than the
// sheet, and the page runs edge to edge underneath.
namespace wvb {
constexpr COLORREF StripBg  = RGB(0x2b, 0x2b, 0x2d);   // caption + toolbar: one sheet
constexpr COLORREF Band     = RGB(0x1f, 0x1f, 0x21);   // the tab band
constexpr COLORREF Page     = RGB(0x1c, 0x1c, 0x1c);   // behind the page while it loads
constexpr COLORREF Card     = RGB(0x1c, 0x1c, 0x1c);   // the no-engine card's face
constexpr COLORREF Omnibox  = RGB(0x3a, 0x3a, 0x3c);   // the address field
constexpr COLORREF Hover    = RGB(0x44, 0x44, 0x46);   // a button's hover tile
constexpr COLORREF Pressed  = RGB(0x52, 0x52, 0x55);
constexpr COLORREF Ink      = RGB(0xeb, 0xeb, 0xf0);   // icons + the address
constexpr COLORREF InkDim   = RGB(0x98, 0x98, 0x9f);   // padlock, placeholder
constexpr COLORREF InkOff   = RGB(0x63, 0x63, 0x66);   // disabled arrows
constexpr COLORREF Border   = RGB(0x3a, 0x3a, 0x3c);
constexpr COLORREF Edge     = RGB(0x17, 0x17, 0x19);   // hairlines between the bands
constexpr COLORREF Blue     = RGB(0x0a, 0x84, 0xff);   // Safari's focus ring + progress
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
    if (g_webOn) {                       // browser: one flat sheet with the toolbar below
        HBRUSH fb = CreateSolidBrush(wvb::StripBg);
        FillRect(dc, &bar, fb); DeleteObject(fb);
    } else FillRect(dc, &bar, GrainBrush(1));   // the title bar stays the dark chrome tile
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
    if (!g_webOn) {                                           // (the browser shows the icon alone)
    SetTextColor(dc, RGB(16, 16, 16));                        // the crisp copy's soft dark shadow
    RECT sh1{ tx + 1, 1, tx + (int)(96 * g.dpi) + 1, CAP() + 1 };
    DrawTextW(dc, L"Docsy", -1, &sh1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    RECT wr{ tx, 0, tx + (int)(96 * g.dpi), CAP() };
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));                  // one flat light tone - the
    DrawTextW(dc, L"Docsy", -1, &wr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    }
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
            } else if (g_webOn) {                             // browser: a neutral hover tile
                HBRUSH hb = CreateSolidBrush(wvb::Hover);
                RECT rr = r[i]; FillRect(dc, &rr, hb); DeleteObject(hb);
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
    by = FdD(6) - FdD(FdPadT) + fd.dy + BandInset(FocusB());   // under the tab band when one shows
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
// ============================================================
//  The browser, dressed like Safari: a full web mode
//  beside the PDF reader, one toolbar button toggling between the two. The
//  engine is Windows' own WebView2 - Evergreen Chromium, preinstalled with
//  the OS and shared with Edge, so the exe carries zero bytes of browser and
//  only the loader glue. Safari's order holds: the toolbar (reader, back/forward,
//  the address field, the menu) rides the strip window on top, the tab band -
//  the reader's own tab strip - rides the toolbar window under it. The module
//  itself lives after the status bar; the toolbar row, the sidebar, the footer, the layout and the
//  main keys all branch on the mode, so their entry points are declared here.
// ============================================================
static bool g_webFail = false;           // the WebView2 runtime is missing (rare)
static bool g_webSide = false;           // the web-tab thumbnail sidebar (closed by default, like Safari)
static HWND g_wstrip = nullptr;          // the browser's Safari toolbar (on top)
static int WEBSTRIPH() { return g_webOn ? (int)(44 * g.dpi) : 0; }   // the toolbar band (Safari)
static HWND g_wtool = nullptr;           // the reader's own top toolbar, carried into browser mode
static int WEBTOOLH() { return g_webOn ? TBH() : 0; }   // ...with one button: back to the PDF reader
static int WEBTABH() { return (int)(34 * g.dpi); }   // the tab band: the reader's 3px inset + 29px face + 1px foot
static void WebToggle(bool on);           // the Web / Reader buttons
static bool WebBtnEnabled(int id);        // the web row's buttons arm without a document
static void WebNewTab(std::wstring url, bool makeCurrent);
static void WebCloseTabAt(int i);
static void WebSwitchTo(int i);
static void WebGo(const std::wstring& what);
static void WebBarLayout();               // park the omnibox edit inside its pill
static void WebBarDraw(HDC dc, int w, int h);   // the tab row, under the nav
static int  WebTabRowHit(POINT p, int w, int h, bool& close);   // tab, or 1000+n: the +
static void WebBarClick(int id);
static void WebStripPaint(HDC dc, int w, int h);   // the nav row, on top
static int  WebNavHit(POINT p);           // the nav row's buttons, strip coords
static void WebSideLayout();              // the sidebar's web-tab thumbs
static void WebSideSync();
static void WebSideDraw(HDC dc, int w, int h);
static void WebSideScrollTo(int y);
static void WebSideWheel(int delta);
static void WebSideDown(const POINT& p);
static void WebSideMove(HWND h, const POINT& p);
static void WebSideUp(HWND h, const POINT& p);
static void WebSideLeave();
static bool WebSidePump();
static int  WebSideAt(int y);             // the web tab under a rail y (the hand cursor)
static void DrawRailShadow(HDC dc, int w, int h, int ins2 = -1);   // the card's left halo,
                                          // the reader rail's painter - the web rail borrows it
static void WebPlace();                   // bounds the visible controller into the card
static void WebResizeDeb();               // a resize re-captures the visible thumb
static bool WebKey(WPARAM wp, bool ctrl, bool shift);   // true: swallowed
static bool WebSysKey(WPARAM wp);         // Alt+Left / Alt+Right
static void WebCaptureTick();             // the thumb capture pump (main timer 7)
static void WebShutdown();                // WM_DESTROY
static std::wstring WebStatusLine();      // the footer's left cell in browser mode
static void WebZoomBy(double f);
static int  WebZoomCurrent();             // the closed combo's nearest preset
static void WebZoomApplyItem(int i);      // popup item 0..5 -> 50%..200%
static HBRUSH WebEditColors(HDC dc, HWND ctl);   // WM_CTLCOLOREDIT
static void WebFailCard(HDC dc, int w, int h);   // the no-engine card in the view
static void WebViewClick(POINT p);        // the fail card's download button
static void WebRepaintChrome();           // nav + tabs + footer + rail in one call
static void WebCardRect(RECT& c, int w, int h);   // the card's rect: the webview rides inset
                                          // in the view, gutters around it for the shadow
static void WebCardPaint(HDC dc, int w, int h);   // the card's field, face and ring

// the sidebar's web state - the same fields sd carries, pointed at the tabs.
// It lives here, before the toolbar's proc, because the tab row (which the
// toolbar window becomes in browser mode) keeps its hover in ws.sHot - the
// nav row above owns g.tbTabHot, so the two rows never collide.
static struct {
    int cw = 0, ch = 0, tw = 0, docH = 0, scroll = 0, cur = -1, hover = -1;
    int hit = -1, moved = 0; POINT dp{}; bool barDrag = false, barHot = false, barGrab = 0;
    int sHot = -1;                         // the TAB ROW's hover (tab index or 1000+n for +)
    bool sHotX = false;                    // ...and whether the cursor is over that tab's close x
    std::vector<int> top;                  // separate from the nav row's hover above
} ws;

static int g_wtCur = -1;                 // the focused web tab; the tab row's proc reads it
                                         // for the press-to-switch rule, so it lives up here

// (the 20px frame + four vector arrows), DrawToolbarGlyph's TransformBoxMove.
enum { TB_CLOSE, TB_OPEN, TB_SAVE, TB_WEB,   // the file cluster; TB_WEB opens the browser
       TB_SELECT, TB_TEXT, TB_HIGHLIGHT, TB_LINE, TB_SHAPE, TB_DRAW,
       TB_TRANSFORM, TB_CLEAR, TB_FIND, TB_COUNT };
static const wchar_t* const TbLabel[TB_COUNT] = {
    L"Close", L"Open", L"Save", L"Web",
    L"Select", L"Text", L"Highlight", L"Line", L"Shapes", L"Draw",
    L"Transform", L"Clear All", L"Find Text"
};
static const wchar_t* const TbGlyph[TB_COUNT] = {
    L"\uE711", L"\uE8DA", L"\uE74E", L"\uE774",
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
    if (g_webOn) return WebBtnEnabled(i);  // the browser row answers for its own ids
    bool doc = DVF().n != 0;
    if (i == TB_OPEN) return true;
    if (i == TB_WEB) return true;         // the browser needs no document
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
    if (g_webOn) {                         // browser mode: the row IS the tab row - a
        bool cl = false;                   // tab index, 1000+n for the +, -1 bare sheet
        RECT r; GetClientRect(g.bar, &r);
        return WebTabRowHit(p, w, r.bottom, cl);
    }
    RECT bs[TB_COUNT]; TbRects(w, bs);
    for (int i = 0; i < TB_COUNT; i++)
        if (!TbShed[i] && p.x >= bs[i].left && p.x < bs[i].right && p.y >= bs[i].top && p.y < bs[i].bottom) return i;
    return -1;
}

// The composed Transform icon's arrows: DrawToolbarGlyph's vector strokes, not
// a font glyph - "The Segoe MDL2 move glyph shrunk to fit the box rendered its
// bottom arrowhead as a single pixel, so it read as cut off" is the reference's
// own reason for drawing them. The geometry lives in the path's 11x11 unit
// space (center 8,8); the path box is 70% of the glyph size (14px at
// GlyphLarge 20), so units scale by 14/11 and the box sits 3px inside the
// 20px frame - DrawToolbarGlyph's Grid(22) with the Border(20) centered and
// the Path(14) inside it. StrokeThickness 1 stays 1 (Shape.Stretch scales the
// geometry, never the pen).
// The strokes draw through GDI+, not GDI pens: the reference renders WPF, and
// the old integer LineTo port put every open stroke end ON its endpoint pixel,
// so the tip-to-tip ink read one pixel narrower than WPF's and the 2-vs-1
// empty columns at the frame's inner edges leaned the arrows right (and down).
// WPF strokes are antialiased at float positions with Square caps - each open
// end grows half a stroke width past its endpoint, a half-covered edge pixel
// at every tip - and PixelOffsetModeHalf is the WPF grid (integer coordinates
// are pixel corners, so a stroke at x.5 fills one full column). The four
// arrowheads ride one three-point subpath each, so the two strokes meeting at
// a tip miter-join there the way Geometry.Parse's own polylines do, with caps
// only at the free base ends.
static void TfArrows(HDC dc, float x0, float y0, COLORREF c) {
    static const float stem[4][4] = {   // x0,y0,x1,y1 in the path's unit space:
        { 8, 2.5f, 8, 6 },      { 8, 10, 8, 13.5f },        // the four stems,
        { 2.5f, 8, 6, 8 },      { 10, 8, 13.5f, 8 },        // capped at both ends
    };
    static const float head[4][3][2] = {    // the four heads, three points each:
        { { 6, 4.5f },  { 8, 2.5f },  { 10, 4.5f } },
        { { 6, 11.5f }, { 8, 13.5f }, { 10, 11.5f } },
        { { 4.5f, 6 },  { 2.5f, 8 },  { 4.5f, 10 } },   // miter-joined at the
        { { 11.5f, 6 }, { 13.5f, 8 }, { 11.5f, 10 } },  // tip, capped at the bases
    };
    const float s = 14.0f / 11.0f * (float)g.dpi;   // 11x11 units -> the 14px path box
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gs.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Pen p(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)), 1.0f);
    p.SetStartCap(Gdiplus::LineCapSquare);
    p.SetEndCap(Gdiplus::LineCapSquare);
    p.SetLineJoin(Gdiplus::LineJoinMiter);
    auto X = [&](float u) { return x0 + (u - 2.5f) * s; };
    auto Y = [&](float u) { return y0 + (u - 2.5f) * s; };
    Gdiplus::GraphicsPath gp;
    for (auto& q : stem) { gp.AddLine(X(q[0]), Y(q[1]), X(q[2]), Y(q[3])); gp.StartFigure(); }
    for (auto& h3 : head) {
        gp.AddLine(X(h3[0][0]), Y(h3[0][1]), X(h3[1][0]), Y(h3[1][1]));
        gp.AddLine(X(h3[1][0]), Y(h3[1][1]), X(h3[2][0]), Y(h3[2][1]));
        gp.StartFigure();
    }
    gs.DrawPath(&p, &gp);
}

static void BarDraw(HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g_webOn) { WebBarDraw(dc, w, h); return; }   // browser mode: Chrome's own toolbar
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
        bool hot = en && !sel && g.tbTabHot == i;
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
                TfArrows(dc, (float)(gx + ox) + 3.0f * (float)g.dpi,          // the vector arrows:
                         (float)(gy + oy) + 3.0f * (float)g.dpi, c);          // the 14px path box
            };                                           // 3px inside the 20px frame
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
        if (g_webOn) {                     // the tab row's hover lives in ws.sHot, never
            bool cl = false;               // in g.tbTabHot - the nav row above owns that
            int hot = WebTabRowHit(p, r.right, r.bottom, cl);
            if (hot != ws.sHot || cl != ws.sHotX) {
                ws.sHot = hot; ws.sHotX = cl;
                if (hot >= 0) TrackLeave(h);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        int hot = BarHit(p, r.right);
        if (hot != g.tbTabHot) {            // repaints only on a hover flip
            g.tbTabHot = hot;
            if (hot >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_webOn) {
            if (ws.sHot != -1 || ws.sHotX) { ws.sHot = -1; ws.sHotX = false; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        if (g.tbTabHot != -1) { g.tbTabHot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        if (g_webOn) {
            bool cl = false;
            int hit = WebTabRowHit(p, r.right, r.bottom, cl);
            if (hit >= 0 && hit < 1000 && hit != g_wtCur && !cl)
                WebSwitchTo(hit);          // Chrome switches on press
            return 0;
        }
        int hit = BarHit(p, r.right);
        if (hit >= 0 && TbEnabled(hit)) {  // capture held: WPF's IsPressed story
            SetCapture(h);                 // (ToolbarAvailability's per-button rules)
            g.tbPressed = hit;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_webOn) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT r; GetClientRect(h, &r);
            bool cl = false;
            int hit = WebTabRowHit(p, r.right, r.bottom, cl);
            if (hit >= 0 && hit < 1000 && cl) WebCloseTabAt(hit);
            else if (hit >= 1000) WebNewTab(L"https://duckduckgo.com/", true);
            return 0;
        }
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
                case TB_WEB: WebToggle(true); break;
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
    case WM_LBUTTONDBLCLK: {           // a double-click on bare tab row: a new tab
        if (g_webOn) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT r; GetClientRect(h, &r);
            bool cl = false;
            int hit = WebTabRowHit(p, r.right, r.bottom, cl);
            if (hit < 0) WebNewTab(L"https://duckduckgo.com/", true);
            return 0;
        }
        break;
    }
    case WM_MBUTTONUP: {               // middle-click closes, like the reader's strip
        if (g_webOn) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT r; GetClientRect(h, &r);
            bool cl = false;
            int hit = WebTabRowHit(p, r.right, r.bottom, cl);
            if (hit >= 0 && hit < 1000) WebCloseTabAt(hit);
            return 0;
        }
        break;
    }
    case WM_CAPTURECHANGED:              // the press lost the mouse some other way
        if (g.tbPressed >= 0) { g.tbPressed = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_CTLCOLOREDIT:                // the omnibox edit's colors (browser mode)
        return (LRESULT)WebEditColors((HDC)wp, (HWND)lp);
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

// ---- browser mode's top toolbar: the reader's own toolbar row (same grain,
// same button face, same glyph-over-caption layout, same height) carrying a
// single button that returns to the PDF reader ----
static bool g_wtoolHot = false, g_wtoolDown = false;

static RECT WebToolBtn() {
    HDC dc = GetDC(g_wtool);
    RECT rc{ 0, 0, 0, 0 };
    HFONT of = (HFONT)SelectObject(dc, CaptionFont(g_tbcap, 14, false, L"Killer Sans"));
    DrawTextW(dc, L"PDF", -1, &rc, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, of);
    ReleaseDC(g_wtool, dc);
    int bw = max((int)(24 * g.dpi), (int)(rc.right - rc.left)) + (int)(12 * g.dpi);
    int x = (int)(4 * g.dpi), y = (int)(2 * g.dpi);
    return RECT{ x, y, x + bw, y + TbBtnH() };
}

static void WebToolDraw(HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    RECT bar{ 0, 0, w, h };
    FillRect(dc, &bar, GrainBrush(1));
    RECT b = WebToolBtn();
    bool pr = g_wtoolDown && g_wtoolHot, hot = g_wtoolHot;
    if (hot || pr) {                                   // the reader's hover / pressed tile
        Gdiplus::Graphics gs(dc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath p;
        float rr = (float)(4 * g.dpi);
        float fx = (float)b.left, fy = (float)b.top;
        float fw = (float)(b.right - b.left), fh = (float)(b.bottom - b.top);
        p.AddArc(fx, fy, 2 * rr, 2 * rr, 180, 90);
        p.AddArc(fx + fw - 2 * rr, fy, 2 * rr, 2 * rr, 270, 90);
        p.AddArc(fx + fw - 2 * rr, fy + fh - 2 * rr, 2 * rr, 2 * rr, 0, 90);
        p.AddArc(fx, fy + fh - 2 * rr, 2 * rr, 2 * rr, 90, 90);
        p.CloseFigure();
        Gdiplus::SolidBrush f(pr ? Gdiplus::Color(255, 0x33, 0x33, 0x33)
                                 : Gdiplus::Color(255, 0x40, 0x40, 0x40));
        gs.FillPath(&f, &p);
    }
    int capH = 0;
    {   RECT mc{ 0, 0, 0, 0 };
        HFONT of = (HFONT)SelectObject(dc, CaptionFont(g_tbcap, 14, false, L"Killer Sans"));
        DrawTextW(dc, L"Find Text", -1, &mc, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(dc, of);
        capH = mc.bottom - mc.top;
    }
    int gh = (int)(24 * g.dpi), gap = (int)(2 * g.dpi);
    int colH = gh + gap + capH;
    int avail = (int)(b.bottom - b.top) - colH;
    int y0 = b.top + max(0, avail) / 2;
    RECT gr{ b.left, y0, b.right, y0 + gh };
    RECT cr{ b.left, y0 + gh + gap, b.right, y0 + gh + gap + capH };
    SetBkMode(dc, TRANSPARENT);
    const wchar_t* glyph = L"\uE8F1";                  // the library shelf - the reading room,
                                                       // not one more document among the tabs'
    SelectObject(dc, CaptionFont(g_tbglyph, 20, false, L"Segoe MDL2 Assets"));
    SetTextColor(dc, RGB(16, 16, 16));
    RECT gs1{ gr.left + 1, gr.top + 1, gr.right + 1, gr.bottom + 1 };
    DrawTextW(dc, glyph, -1, &gs1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
    DrawTextW(dc, glyph, -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, CaptionFont(g_tbcap, 14, false, L"Killer Sans"));
    SetTextColor(dc, RGB(16, 16, 16));
    RECT cs1{ cr.left + 1, cr.top + 1, cr.right + 1, cr.bottom + 1 };
    DrawTextW(dc, L"PDF", -1, &cs1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
    DrawTextW(dc, L"PDF", -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static LRESULT CALLBACK WebToolProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureBarBack(dc, r.right, r.bottom);
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // the shared grain phase
            SetBrushOrgEx(g_bardc, -po.x, -po.y, nullptr);
            WebToolDraw(g_bardc, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, g_bardc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT r; GetClientRect(h, &r);
        POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
        SetBrushOrgEx((HDC)wp, -po.x, -po.y, nullptr);
        WebToolDraw((HDC)wp, r.right, r.bottom);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT b = WebToolBtn();
        bool hot = PtInRect(&b, p) != 0;
        if (hot != g_wtoolHot) {
            g_wtoolHot = hot;
            if (hot) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_wtoolHot) { g_wtoolHot = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT b = WebToolBtn();
        if (PtInRect(&b, p)) {
            SetCapture(h);
            g_wtoolDown = true; g_wtoolHot = true;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_wtoolDown) {
            g_wtoolDown = false;
            if (GetCapture() == h) ReleaseCapture();
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT b = WebToolBtn();
            bool inside = PtInRect(&b, p) != 0;
            g_wtoolHot = false;
            InvalidateRect(h, nullptr, FALSE);
            if (inside) WebToggle(false);              // back to the PDF reader
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_wtoolDown) { g_wtoolDown = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(g.main, m, wp, lp);
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT b = WebToolBtn();
            SetCursor(LoadCursorW(nullptr, PtInRect(&b, p) ? IDC_HAND : IDC_ARROW));
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
    D.cacheBytes = 0;
}

static void CacheDrop(DocState& D, int i) {   // one entry out, byte weight with it
    auto it = D.cache.find(i);
    if (it == D.cache.end()) return;
    D.cacheBytes -= (long long)it->second.w * it->second.h * 4;
    DeleteObject(it->second.b);
    D.cache.erase(it);
}

static HFONT UiFont() {
    if (!g_uifont) {
        g_uifont = CreateFontW(-MulDiv(9, (int)(g.dpi * 96), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    return g_uifont;
}

// The BMP the engine encodes is a plain 32bpp file: parse its headers straight
// into a DIB section instead of paying a GDI+ decode AND a GetHBITMAP copy on
// every page - the pixel copy is the only work left. Anything unexpected
// (palette, RLE, odd masks, absurd sizes, a short read) answers nullptr and
// the caller falls back to the GDI+ path, so this can stay naive about
// everything it does not recognize.
static HBITMAP BmpStreamToDib(IStream* is) {
    BITMAPFILEHEADER fh{};
    ULONG got = 0;
    if (!is || FAILED(is->Read(&fh, sizeof fh, &got)) || got != sizeof fh || fh.bfType != 0x4D42)
        return nullptr;
    BITMAPINFOHEADER ih{};
    if (FAILED(is->Read(&ih, sizeof ih, &got)) || got != sizeof ih || ih.biSize < sizeof ih)
        return nullptr;
    if (ih.biSize > sizeof ih) {                        // V4/V5 header: the shape is the
        LARGE_INTEGER skip{};                           // same 40-byte base plus extra fields
        skip.QuadPart = (LONGLONG)(ih.biSize - sizeof ih);
        if (FAILED(is->Seek(skip, STREAM_SEEK_CUR, nullptr))) return nullptr;
    }
    if (ih.biPlanes != 1 || ih.biBitCount != 32) return nullptr;
    if (ih.biCompression != BI_RGB && ih.biCompression != BI_BITFIELDS) return nullptr;
    if (ih.biCompression == BI_BITFIELDS) {             // only the standard BGRx masks ride -
        DWORD m[3]{};                                   // they sit right after the base header
        if (FAILED(is->Read(m, sizeof m, &got)) || got != sizeof m) return nullptr;   // in both the
        if (m[0] != 0x00FF0000u || m[1] != 0x0000FF00u || m[2] != 0x000000FFu) return nullptr;   // 40-byte and V4/V5 shapes
    }
    if (ih.biWidth <= 0 || ih.biWidth > 30000 || ih.biHeight == 0 ||
        ih.biHeight > 30000 || ih.biHeight < -30000) return nullptr;
    int w = (int)ih.biWidth, hh = (int)labs((long)ih.biHeight);
    long long stride = (long long)w * 4, bytes = stride * (long long)hh;
    if (bytes <= 0 || bytes > (512LL << 20)) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;                        // top-down DIB: rows in scan order
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC tdc = CreateCompatibleDC(nullptr);
    HBITMAP hb = tdc ? CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (tdc) DeleteDC(tdc);
    if (!hb || !bits) { if (hb) DeleteObject(hb); return nullptr; }
    std::vector<BYTE> buf((size_t)bytes);
    if (FAILED(is->Read(buf.data(), (ULONG)bytes, &got)) || (long long)got != bytes) {
        DeleteObject(hb);
        return nullptr;
    }
    BYTE* dst = (BYTE*)bits;
    if (ih.biHeight < 0) memcpy(dst, buf.data(), (size_t)bytes);      // already top-down
    else for (int y = 0; y < hh; y++)                                 // bottom-up: flip
        memcpy(dst + (size_t)y * stride, buf.data() + (size_t)(hh - 1 - y) * stride, (size_t)stride);
    return hb;
}

// Every rasterizer of a PdfDocument - the worker's RenderCore and the UI
// thread's own renders (the OCR pass in RenderSoft, the save writer, shot
// mode) - queues behind this one gate: Windows.Data.Pdf does not survive two
// concurrent RenderToStreamAsync calls on one document, and before the gate a
// pre-warm OCR 250ms after a page settled raced the worker's render of the
// same page and took the process down. The wait is bounded by ONE page render
// (the lock is taken per call, never across the queue), and no path holds it
// together with the job-queue mutex, so the worker and the drain cannot cycle.
static std::mutex g_renderM;

static HBITMAP RenderCore(winrt::Windows::Data::Pdf::PdfDocument doc, int page, int w, int h,
                          int rot, int flip) {
    try {
        std::lock_guard<std::mutex> lk(g_renderM);
        // the Transform's quarter-turns and flips bake into the render: the page
        // rasterizes at its UNROTATED aspect, then GDI+ rotates/flips into the
        // caller's (already swapped) bitmap, so every downstream path - layout,
        // hit tests, annotation painting - reads ordinary w x h pixels
        int rw = (rot % 2 == 0) ? w : h, rh = (rot % 2 == 0) ? h : w;
        auto pageObj = doc.GetPage((uint32_t)page);
        PdfPageRenderOptions o;
        o.DestinationWidth((uint32_t)max(1, rw));
        o.DestinationHeight((uint32_t)max(1, rh));
        o.BackgroundColor(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
        o.BitmapEncoderId(winrt::Windows::Graphics::Imaging::BitmapEncoder::BmpEncoderId()); // raw BMP: no PNG cost
        InMemoryRandomAccessStream st;
        pageObj.RenderToStreamAsync(st, o).get();
        st.Seek(0);
        winrt::com_ptr<IStream> is;
        winrt::check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(st), winrt::guid_of<IStream>(), is.put_void()));
        if (rot == 0 && flip == 0) {
            HBITMAP fast = BmpStreamToDib(is.get());   // one copy, no GDI+ decode:
            if (fast) return fast;                     // misses fall through to GDI+
        }
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

static HBITMAP Render(DocState& D, int i, int w, int h) {
    int rot = D.prot.count(i) ? D.prot[i] : 0;
    int flip = D.pflip.count(i) ? D.pflip[i] : 0;
    return RenderCore(D.doc, i, w, h, rot, flip);
}

// ---- the async render pipeline ----
// Every page and thumbnail renders on one background worker: the UI thread
// asks (GetBmp / SidePump), gets a placeholder the same frame, and results
// land through a posted message whose drain inserts them into the same caches
// the sync path used. Shot mode (g_sync) keeps the old synchronous behavior -
// the CI previews composite one frame directly, so they must have every pixel
// now. Jobs carry the document's stable id and an agile PdfDocument ref, never
// a DocState*: the tabs vector reallocates, and the agile ref keeps the engine
// object alive even when the tab closes mid-render (the drain then finds no
// owner and frees the pixels).
static bool g_sync = false;                 // shot mode: render inside the asking call
static std::mutex g_rqM;
static std::condition_variable g_rqCv;
static bool g_rqQuit = false;
static HANDLE g_rqThread = nullptr;
static unsigned long long g_rqSeq = 0;      // bump on every ask; the worker answers the newest first
enum { RJ_PAGE, RJ_THUMB };
struct RJob {
    int docId, page, kind;
    int w, h, rot, flip;                    // rot: quarter-turns; flip: 1=X 2=Y (RenderCore's own bits)
    winrt::agile_ref<winrt::Windows::Data::Pdf::PdfDocument> doc;
    unsigned long long seq;
};
struct RRes {
    int docId, page, kind;
    int w, h, rot, flip;                    // pages: rot = the packed Ent key (quarter | flip<<4)
    HBITMAP b;                              // may be null: a failed thumb caches its null as a placeholder
    unsigned long long seq;
};
static std::vector<RJob> g_rq;              // pending, consumed by the worker
static std::vector<RRes> g_rqDone;          // finished, drained on the UI thread
static bool RqSame(const RJob& j, int docId, int page, int kind, int w, int h, int rot, int flip) {
    return j.docId == docId && j.page == page && j.kind == kind &&
           j.w == w && j.h == h && j.rot == rot && j.flip == flip;
}
static void RqEnqueue(DocState& D, int i, int kind, int w, int h, int rot, int flip) {
    if (!D.id || !D.doc) return;
    std::lock_guard<std::mutex> lk(g_rqM);
    for (size_t k = 0; k < g_rq.size(); k++) {          // supersede a stale-size job for
        if (g_rq[k].docId == D.id && g_rq[k].page == i && g_rq[k].kind == kind) {   // the same slot
            if (RqSame(g_rq[k], D.id, i, kind, w, h, rot, flip)) {
                g_rq[k].seq = ++g_rqSeq;                // already asked: just raise its turn
                return;
            }
            g_rq[k] = g_rq.back(); g_rq.pop_back(); k--;    // old size is wasted work now
        }
    }
    for (auto& r : g_rqDone)                            // a matching result is waiting to
        if (r.docId == D.id && r.page == i && r.kind == kind &&
            r.w == w && r.h == h && r.rot == rot && r.flip == flip) return;   // drain: no second render
    g_rq.push_back(RJob{ D.id, i, kind, w, h, rot, flip, winrt::make_agile(D.doc), ++g_rqSeq });
    g_rqCv.notify_one();
}
static void RqPurgeThumbs(int docId, int first, int last) {
    std::lock_guard<std::mutex> lk(g_rqM);
    for (size_t k = 0; k < g_rq.size(); k++) {          // thumb jobs for another document - the
        if (g_rq[k].kind != RJ_THUMB) continue;         // strip switched - or for pages the scroll
        if (g_rq[k].docId == docId && g_rq[k].page >= first - 1 && g_rq[k].page <= last + 1) continue;
        g_rq[k] = g_rq.back(); g_rq.pop_back(); k--;    // left behind: neither will ever be looked at,
    }                                                   // so the worker must not spend renders on them
}
static DWORD WINAPI RqWorker(LPVOID) {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    for (;;) {
        RJob j;
        {
            std::unique_lock<std::mutex> lk(g_rqM);
            g_rqCv.wait(lk, [] { return g_rqQuit || !g_rq.empty(); });
            if (g_rqQuit) break;
            size_t best = 0; bool have = false;         // PAGES before thumbs: every open and tab
            unsigned long long bs = 0;                  // switch asks the strip's fifteen thumbs a
            for (size_t k = 0; k < g_rq.size(); k++)    // tick after the pages, and pure recency then
                if (g_rq[k].kind == RJ_PAGE && (!have || g_rq[k].seq > bs)) { best = k; bs = g_rq[k].seq; have = true; }
            if (!have)                                  // held the eye's page hostage behind the whole
                for (size_t k = 0; k < g_rq.size(); k++)// card shelf - seconds of white pages on every
                    if (!have || g_rq[k].seq > bs) { best = k; bs = g_rq[k].seq; have = true; }
            j = g_rq[best];
            g_rq[best] = g_rq.back(); g_rq.pop_back();
        }
        HBITMAP b = nullptr;
        try { b = RenderCore(j.doc.get(), j.page, j.w, j.h, j.rot, j.flip); } catch (...) {}
        {
            std::lock_guard<std::mutex> lk(g_rqM);
            int protp = j.rot, flipp = j.flip;
            g_rqDone.push_back(RRes{ j.docId, j.page, j.kind, j.w, j.h,
                                     j.kind == RJ_PAGE ? (protp | (flipp << 4)) : 0, flipp, b, j.seq });
        }
        if (g.view) PostMessageW(g.view, WM_APP + 2, 0, 0);   // the drain paints the result
    }
    return 0;
}
static void RqEnsureThread() {
    if (!g_rqThread) g_rqThread = CreateThread(nullptr, 0, RqWorker, nullptr, 0, nullptr);
}
static void RqShutdown() {
    HANDLE t;
    { std::lock_guard<std::mutex> lk(g_rqM); g_rqQuit = true; t = g_rqThread; g_rqThread = nullptr; }
    g_rqCv.notify_all();
    if (t) { WaitForSingleObject(t, 3000); CloseHandle(t); }
}

// The paint ask. An exact hit answers itself. A miss enqueues the worker render
// and answers nothing (the frame paints a placeholder, the drain lands pixels).
// And a wrong-SIZE entry - a zoom the worker has not caught up with - keeps
// speaking for the page: it comes back with its true shape in bw/bh and Draw
// stretches it into the new rect, so a zoom gesture never blanks the pages it
// is resizing; the fresh render rides the queue the whole time. A transform
// mismatch still drops and goes white: sideways pixels stretched into an
// upright rect read broken, a placeholder reads pending. `exact` keeps the old
// contract for callers that sample page pixels at the CURRENT scale (the text
// box's tone sampler) - they must not be handed another zoom's raster.
static HBITMAP GetBmpEx(DocState& D, int i, int w, int h, int& bw, int& bh, bool exact) {
    bw = w; bh = h;
    auto it = D.cache.find(i);
    int rot = (D.prot.count(i) ? D.prot[i] : 0) | (D.pflip.count(i) ? D.pflip[i] << 4 : 0);
    if (it != D.cache.end()) {
        if (it->second.w == w && it->second.h == h && it->second.rot == rot) { it->second.t = ++D.tick; return it->second.b; }
        if (!exact && !g_sync && it->second.rot == rot) {   // stale size, same transform:
            it->second.t = ++D.tick;                        // the eye's page stays warm while
            RqEnqueue(D, i, RJ_PAGE, w, h, rot & 3, rot >> 4);   // the right-size render flies
            RqEnsureThread();
            bw = it->second.w; bh = it->second.h;
            return it->second.b;
        }
        CacheDrop(D, i);
    }
    if (!g_sync) {                                     // live: the worker owns the render -
        RqEnqueue(D, i, RJ_PAGE, w, h, rot & 3, rot >> 4);   // this frame paints the placeholder,
        RqEnsureThread();                              // the drain lands the pixels
        return nullptr;
    }
    HBITMAP b = Render(D, i, w, h);
    if (!b) return nullptr;
    D.cache[i] = Ent{ b, w, h, rot, ++D.tick };
    D.cacheBytes += (long long)w * h * 4;
    return b;
}
static HBITMAP GetBmp(DocState& D, int i, int w, int h) {
    int bw, bh;
    return GetBmpEx(D, i, w, h, bw, bh, true);
}

// The same render as Render(), decoded as a SoftwareBitmap - the input format the
// OS OCR engine consumes for the text-selection pass. Rotated/flipped pages bake
// their transform the same way, so a transformed page's OCR cells land in its
// new coordinate space and selection keeps working there.
static winrt::Windows::Graphics::Imaging::SoftwareBitmap RenderSoft(DocState& D, int i, int w, int h) {
    try {
        std::lock_guard<std::mutex> lk(g_renderM);   // the same gate: this render runs on the
        int rot = D.prot.count(i) ? D.prot[i] : 0;   // UI thread beside the worker's own
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

static const long long PAGE_CACHE_BUDGET = 160LL << 20;   // per document: hundreds of
                                          // light pages survive; giant zoomed pages cap it
static const long long TAB_CACHE_BUDGET = 640LL << 20;    // across a pane's whole tab strip
static void Evict(DocState& D, int lo, int hi) {
    while ((D.cacheBytes > PAGE_CACHE_BUDGET && D.cache.size() > 24) || D.cache.size() > 4096) {
        int k = -1; unsigned long long best = ~0ULL;
        for (auto& kv : D.cache)
            if ((kv.first < lo - 1 || kv.first > hi + 1) && kv.second.t < best) { best = kv.second.t; k = kv.first; }
        if (k < 0) break;
        D.cacheBytes -= (long long)D.cache[k].w * D.cache[k].h * 4;
        DeleteObject(D.cache[k].b); D.cache.erase(k);
    }
}

static int PageIn(const std::vector<int>& top, int n, int y) {
    if (n <= 0 || top.empty()) return 0;
    if (n > (int)top.size() - 1) n = (int)top.size() - 1;   // a stale layout (the focused document
    if (n <= 0) return 0;                                   // switched under it) must not walk off
    auto it = std::upper_bound(top.begin(), top.begin() + n, y);   // the array it still describes
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
    while (D.txt.size() > 256) {            // LRU: drop the stalest page's geometry (a
        auto oldest = D.txt.end();          // 2,000-cell page is ~100KB, so 256 pages ride
        for (auto x = D.txt.begin(); x != D.txt.end(); ++x)   // comfortably under 30MB)
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
    int y = D.gap + BandInset(b); D.maxw = 0;   // the tab band is a layout row: KillerPDF's card
                                                // starts below its strip, so the pages do too -
                                                // the covered strip scrolls under the opaque band,
                                                // which is the same clip the ScrollViewer row draws
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
    D.zoom = z; D.fitMode = 2;              // an explicit zoom leaves the fit modes - and keeps
                                            // the rasters: stale-size entries still paint
                                            // stretched while the worker catches up (Evict
                                            // keeps their weight inside the byte budget)
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
        z = min(z, (g.ch - BandInset(b) - 2.0 * Gap()) / (max(1.f, rh) * g.dpi));
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
    sd.th.clear(); sd.thBytes = 0; sd.scroll = 0; sd.cur = -1; sd.hold = 0;              // belong to the focused
                                                    // pane's document now - and cw/ch stay:
                                                    // zeroing them early-returned the SideLayout
                                                    // below and dead-pumped the strip
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
        // the file is read into memory and the engine chews on the copy:
        // LoadFromFileAsync pins the on-disk file for the PdfDocument's whole
        // life, so every later in-place save died on a sharing violation and
        // the disk never changed - the reader's "save does nothing". A memory
        // copy holds no handle; the path stays free for Save's overwrite.
        HANDLE fh = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) throw std::runtime_error("open");
        LARGE_INTEGER fsz{};
        if (!GetFileSizeEx(fh, &fsz) || fsz.QuadPart <= 0) { CloseHandle(fh); throw std::runtime_error("size"); }
        std::vector<BYTE> bytes((size_t)fsz.QuadPart);
        DWORD got = 0, rd = 0;
        while (got < bytes.size() &&
               ReadFile(fh, bytes.data() + got, (DWORD)(bytes.size() - got), &rd, nullptr) && rd) got += rd;
        CloseHandle(fh);                                 // the disk file is nobody's hostage now
        if (got != bytes.size()) throw std::runtime_error("read");
        InMemoryRandomAccessStream st;
        DataWriter dw(st);
        dw.WriteBytes(winrt::array_view<const BYTE>(bytes.data(), bytes.data() + bytes.size()));
        dw.StoreAsync().get();
        dw.FlushAsync().get();
        dw.DetachStream();                               // the writer lets go; the doc keeps the stream
        st.Seek(0);
        winrt::Windows::Data::Pdf::PdfDocument d{ nullptr };   // the projection has no default ctor
        {   // the load queues behind the render gate too: the worker may be inside a page render
            // of ANOTHER document right now, and the engine's loader rides the same machinery
            std::lock_guard<std::mutex> lk(g_renderM);
            d = PdfDocument::LoadFromStreamAsync(st).get();
        }
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
        D.id = ++g.nextDocId;               // the render queue's stable handle for this document
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
        SnapClean(D);                          // a fresh open IS the saved checkpoint
        bool mine = fd.pane == (&D == &g.dB);        // the find dies only when ITS document
        if (mine && fd.open) {                       // dies; the other pane's scan keeps going
            if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
            if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
            fd = Find{};                             // the Find bar's results die with the old document
        }
        if (&D == &DVF()) {                          // the sidebar shows the FOCUSED pane's pages:
            for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);   // a drop or open
            sd.th.clear(); sd.thBytes = 0; sd.scroll = 0; sd.cur = -1; sd.hold = 0; sd.tw = 0;   // into the pane
            sd.cw = 0; sd.ch = 0;                    // you read rebuilds the ring around it
            g.sideOn = true;
            if (g.side) ShowWindow(g.side, SW_SHOWNOACTIVATE);
        }
        LayoutMain();
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);   // the new document is the card now:
                                                              // WS_CLIPCHILDREN keeps the main
                                                              // window's repaint out of the view,
                                                              // and the band's arrival reflowed the
                                                              // pages - the card must not wait for a
                                                              // WM_SIZE or the render drain to show
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
    if (GetOpenFileNameW(&o)) TabOpenInto(FocusB(), f);   // OpenInNewTab: the dialog lands in
}

// The drop queue: every dropped file waits here and opens one per posted
// message, so a folder drop of tens of PDFs loads in sequence while the window
// keeps painting between loads - no single giant freeze.
static std::vector<std::wstring> g_dropQ;
static std::vector<bool> g_dropPane;

static void HandleDrop(HDROP d, HWND h) {        // the pane under the drop takes the files:
    UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);   // the view maps the point to its
    POINT p; DragQueryPoint(d, &p);              // pane, a drop on the sidebar or the frame
    bool b = FocusB();                           // goes to the pane the reader is working in
    if (h == g.view) b = PaneB(p.x);
    for (UINT i = 0; i < n; i++) {
        wchar_t f[MAX_PATH * 2];
        if (!DragQueryFileW(d, i, f, (UINT)_countof(f))) continue;
        g_dropQ.push_back(f);
        g_dropPane.push_back(b);
    }
    DragFinish(d);
    if (!g_dropQ.empty()) PostMessageW(g.main, WM_APP + 3, 0, 0);   // one open per message
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
    // NewDocument opens the blank in its OWN tab (BeginTabLoad): a busy front
    // tab keeps its state, an empty one is reused, so Ctrl+N never eats work
    if (TabOpenInto(FocusB(), file)) SetStatusHold(L"New blank document");
}

// CloseFile_Click -> CloseTab: the focused pane's front tab closes through the
// tab model's CloseTabCore - the neighbor takes the front, the last document
// lands on KillerPDF's empty state, and a dirty tab asks first.
static void CloseDoc() {
    CloseTabAt(FocusB(), Act(FocusB()));
}

// The shared write-out: every page renders at ~150dpi with the annotations
// composited - the same paint the screen shows - and lands as JPEG pages in a
// minimal PDF. SaveDoc's writer is the only emitter. The bytes land in a
// sibling temp first and REPLACE the target only once the whole PDF is out -
// a half-written save must never eat the file it was saving. SaveAs adopts
// the same rule: the copy is completed off to the side, then moved into place.
static bool WriteAnnotatedPdf(DocState& D, const std::wstring& file, std::wstring& err) {
    std::wstring tmp = file + L".docsy-tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"the file could not be written"; return false; }
    auto pageJpeg = [&](int i, std::vector<BYTE>& out) -> bool {
        int w = std::clamp((int)std::lround(D.pw[i] * 2.0f), 64, 2400);
        int h = max(1, (int)std::lround((float)w * D.ph[i] / max(1.f, D.pw[i])));
        HBITMAP b = Render(D, i, w, h);    // the writer renders synchronously: save needs
        if (!b) return false;              // every page's pixels now, not on the worker's
                                           // queue (and a save-sized 2x render never
                                           // pollutes the view cache the way GetBmp did)
        // the burn-in lands on a private 32bpp DIB section, and the JPEG encoder
        // reads the section's OWN memory (Bitmap over scan0). The old path handed
        // GDI+ an HBITMAP that was selected into a DC - GetDIBits, which GDI+
        // reads pixels through, is undefined for a selected bitmap and came back
        // all zeros: the saved PDF was white pages, and the encoder's walk over
        // the bad buffer took the app down. A DIB section has no DC coupling and
        // no HBITMAP wrap at all - the buffer is ours, the pixels are real.
        void* bits = nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;                    // top-down, like every consumer here
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) { if (dib) DeleteObject(dib); return false; }
        HDC ddc = CreateCompatibleDC(nullptr);
        HGDIOBJ od = SelectObject(ddc, dib);
        HDC sdc = CreateCompatibleDC(nullptr);         // the fresh render is a read-only
        HGDIOBJ osel = SelectObject(sdc, b);           // source: a plain blit, never drawn into
        BitBlt(ddc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
        SelectObject(sdc, osel); DeleteDC(sdc);
        DeleteObject(b);                   // the fresh render was this call's own
        float k = (float)w / max(1.f, D.pw[i]);
        PaintAnnots(ddc, D, i, 0, 0, k);               // annotations burn in
        CLSID jpg{}; UINT cnt = 0, bytes = 0;
        Gdiplus::GetImageEncodersSize(&cnt, &bytes);
        std::vector<BYTE> buf(bytes);
        auto* enc = (Gdiplus::ImageCodecInfo*)buf.data();
        Gdiplus::GetImageEncoders(cnt, bytes, enc);
        for (UINT e = 0; e < cnt; e++) if (!lstrcmpW(enc[e].MimeType, L"image/jpeg")) jpg = enc[e].Clsid;
        IStream* os = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &os) != S_OK) { SelectObject(ddc, od); DeleteDC(ddc); DeleteObject(dib); return false; }
        bool ok = false;
        {
            Gdiplus::Bitmap bm(w, h, w * 4, PixelFormat32bppRGB, (BYTE*)bits);   // wraps OUR
            ok = bm.Save(os, &jpg, nullptr) == Gdiplus::Ok;   // buffer - it stays alive
        }                                                     // until after this Save
        if (ok) {
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
        SelectObject(ddc, od); DeleteDC(ddc); DeleteObject(dib);   // the bitmap view is
        return ok && !out.empty();                                 // gone before the buffer
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
    // object numbering: catalog 1, pages 2, then THREE objects per page -
    // page 3+3i, contents 4+3i, image 5+3i. The old 2-per-page stride made
    // page i's number (3+2i) collide with page i-1's image (5+2(i-1)): two
    // "N 0 obj" bodies, an xref that pointed the Kids at image XObjects, and
    // a document whose pages rendered white everywhere but page one.
    for (int i = 0; i < D.n; i++) kids += std::to_string(3 + i * 3) + " 0 R ";
    emit("2 0 obj\n<< /Type /Pages /Kids [ " + kids + "] /Count " + std::to_string(D.n) + " >>\nendobj\n");
    std::vector<long long> pageOff(D.n), contOff(D.n), imgOff(D.n);
    bool failed = false;
    for (int i = 0; i < D.n && !failed; i++) {
        std::vector<BYTE> jpg;
        if (!pageJpeg(i, jpg)) { failed = true; break; }
        // Windows.Data.Pdf's Size() is DIPs (96/inch); the page box wants PDF
        // points (72/inch) - x0.75 lands the saved page at its true print size
        // (the old x2 wrote DIPs as points and bloated every page by a third)
        float wpt = D.pw[i] * 0.75f, hpt = D.ph[i] * 0.75f;
        char hb2[256];
        pageOff[i] = pos;
        snprintf(hb2, 256, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Contents %d 0 R /Resources << /XObject << /Im %d 0 R >> >> >>\nendobj\n",
                 3 + i * 3, wpt, hpt, 4 + i * 3, 5 + i * 3);
        emit(hb2);
        contOff[i] = pos;
        std::string cs = "q\n" + std::to_string(wpt).substr(0, 8) + " 0 0 " + std::to_string(hpt).substr(0, 8) + " 0 0 cm\n/Im Do\nQ\n";
        emit(std::to_string(4 + i * 3) + " 0 obj\n<< /Length " + std::to_string(cs.size()) + " >>\nstream\n" + cs + "endstream\nendobj\n");
        imgOff[i] = pos;
        std::string ih = std::to_string(5 + i * 3) + " 0 obj\n<< /Type /XObject /Subtype /Image /Width " +
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
        DeleteFileW(tmp.c_str());   // the temp dies alone; the real file never knew
        err = L"a page could not be rendered";
        return false;
    }
    long long xref = pos;
    std::string xr = "xref\n0 " + std::to_string(3 + D.n * 3) + "\n0000000000 65535 f \n";
    auto off10 = [](long long v) { char b[16]; snprintf(b, 16, "%010lld", v); return std::string(b); };
    xr += off10(catOff) + " 00000 n \n" + off10(pagesOff) + " 00000 n \n";
    for (int i = 0; i < D.n; i++)
        xr += off10(pageOff[i]) + " 00000 n \n" + off10(contOff[i]) + " 00000 n \n" + off10(imgOff[i]) + " 00000 n \n";
    xr += "trailer\n<< /Size " + std::to_string(3 + D.n * 3) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    emit(xr);
    CloseHandle(f);
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());   // the temp dies alone; the real file never knew
        err = L"the file could not be replaced";
        return false;
    }
    return true;
}

// Save_Click: no document -> "Open a file first"; a real path -> KillerPDF's
// save prompt, but only over CHANGES - a document matching its checkpoint has
// nothing to replace, so Ctrl+S stays quiet ("No changes to save"). The
// prompt is the themed KillerDialog box, "Replace {name}?" answered with
// Overwrite (saves in place), Save As (the file dialog, then the new path is
// adopted) or Cancel; Save As (Ctrl+Shift+S or the chevron) always asks and
// always writes - export semantics, no dirty gate.
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
        if (!DocDirty(D)) { SetStatusHold(L"No changes to save"); return; }
        int r = AskPrompt3(L" - Save", (std::wstring(L"Replace ") + D.name + L"?").c_str(),
                           L"Cancel", L"Save As", L"Overwrite");
        if (r == 1) { SaveDoc(true); return; }          // Save As asks for the new path
        if (r != 2) { SetStatusHold(L"Save canceled"); return; }   // Cancel or dismissed
    }
    std::wstring err;
    if (!WriteAnnotatedPdf(D, file, err)) { SetStatusHold(std::wstring(L"Save failed: ") + err); return; }
    SnapClean(D);                                // this state is now the checkpoint
    if (file != D.path) {                        // Save As adopts its file (KillerPDF's
        D.path = file;                           // SaveAsWithName lands the doc on it)
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
    if (i < 0 || i >= D.n) return 1;    // a stale index (the document switched mid-flight) reads nothing
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
            sd.th.clear(); sd.thBytes = 0;
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

// The rail's thumb LRU: evicts farthest-off-screen first, oldest first,
// against a byte budget and a hard entry ceiling - so a 2,000-page document
// keeps a couple thousand light thumbs resident while a giant-dpi strip gives
// memory back.
static const long long THUMB_CACHE_BUDGET = 96LL << 20;
static void SideThEvict(int first, int last) {
    for (;;) {
        if (sd.thBytes <= THUMB_CACHE_BUDGET && sd.th.size() <= 4096) return;
        int k = -1; unsigned long long best = ~0ULL;
        for (auto& kv : sd.th)
            if ((kv.first < first - 2 || kv.first > last + 2) && kv.second.t < best) { best = kv.second.t; k = kv.first; }
        if (k < 0) return;                       // everything left is on screen
        if (sd.th[k].b) { sd.thBytes -= (long long)sd.th[k].w * sd.th[k].h * 4; DeleteObject(sd.th[k].b); }
        sd.th.erase(k);
    }
}

// Asks for every visible thumbnail (the worker renders them in recency order,
// results land through the drain). Returns true while visible thumbs are still
// missing, so a 25 ms timer can re-pump the strip as the scroll reveals more.
// Shot mode keeps the old one-thumb-per-call contract - the startup loop
// expects to fill the rail synchronously before its one composite.
static bool SidePump() {
    DocState& D = DVF();                    // thumbs render from the FOCUSED pane's document
    if (!D.n || sd.ch <= 0 || sd.docH <= 0) return false;
    if (!(g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE))) return false;   // nothing to pump while hidden (shot mode shows it first)
    int first = SidePageAt(sd.scroll), last = first;
    while (last + 1 < D.n && sd.top[last + 1] < sd.scroll + sd.ch) last++;
    if (g_sync) {
        int pick = -1, bestd = INT_MAX, center = (first + last) / 2;
        for (int i = max(0, first - 1); i <= min(D.n - 1, last + 1); i++) {
            if (sd.th.find(i) != sd.th.end()) continue;
            int d = abs(i - center);
            if (d < bestd) { bestd = d; pick = i; }
        }
        if (pick < 0) return false;
        HBITMAP b = Render(D, pick, sd.tw, STH(pick));
        sd.th[pick] = Ent{ b, sd.tw, STH(pick), 0, ++sd.tick };   // b may be null: stays a placeholder, no retry spin
        if (b) sd.thBytes += (long long)sd.tw * STH(pick) * 4;
        SideThEvict(first, last);
        InvalidateRect(g.side, nullptr, FALSE);
        return true;
    }
    int missing = 0;
    RqPurgeThumbs(D.id, first, last);       // the strip's stale asks die here: another document's
    for (int i = max(0, first - 1); i <= min(D.n - 1, last + 1); i++) {   // thumbs, pages the scroll left
        if (sd.th.find(i) != sd.th.end()) continue;
        missing++;
        RqEnqueue(D, i, RJ_THUMB, sd.tw, STH(i), 0, 0);
    }
    return missing > 0;
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
    if (c >= 0 && (size_t)c + 1 < sd.top.size() && sd.docH > 0 && GetTickCount64() >= sd.hold) {
        int y0 = sd.top[c] - (int)(8 * g.dpi);                        // the item band's edges
        int y1 = sd.top[c] + STH(c) + (int)(28 * g.dpi);
        if (y1 > sd.scroll + sd.ch) SideScrollTo(y0 - (int)(4 * g.dpi));          // off the bottom: card at the top
        else if (y0 < sd.scroll) SideScrollTo(y1 - sd.ch + (int)(4 * g.dpi));     // off the top: card at the bottom
    }
    if (g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE)) InvalidateRect(g.side, nullptr, FALSE);
}

// ============================================================
//  Tabs - KillerPDF's DocumentSession model (Controls/Viewer/PdfViewer.Tabs.cs)
// ============================================================
// One window, many documents: every open PDF is a session in its pane's strip,
// the front tab's fields are the live working set, and switching tabs captures
// the live fields into the outgoing session and applies the incoming session's
// fields, then re-renders. The strip itself is per pane (KillerPDF keeps
// _sessions/_active on the viewer, not the window), the tab band appears only
// from the second document of that pane, and every open/close/switch route
// below mirrors its namesake there: TabOpenInto is OpenInNewTab + BeginTabLoad,
// SwitchTab is SwitchToTab, CloseTabAt is CloseTabCore, CloseOtherTabs and
// CloseAllTabs keep their prompts, CycleTab keeps its wrap, and TabMoveToPane
// is DetachSessionExt + AdoptSessionExt. KillerPDF's lazy deferred tabs and
// restart restore are the one behavior docsy does not carry: it keeps no
// settings store at all, so there is no OpenTabs key to restore from.

static std::vector<DocState>& Tabs(bool b) { return b ? g.tabs[1] : g.tabs[0]; }
static int& Act(bool b) { return b ? g.act[1] : g.act[0]; }
static unsigned long long g_tabTick = 0;

static DocState* DocById(int id) {          // the render results' owner lookup: the tabs
    if (!id) return nullptr;                // vector moves, so the drain re-resolves by id
    if (g.d.id == id) return &g.d;          // the front document lives in the working set,
    if (g.dB.id == id) return &g.dB;        // OUTSIDE the tab vectors, until a switch parks
    for (int p = 0; p < 2; p++)             // it there - the drain must find it in g.d/g.dB
        for (auto& t : Tabs(p != 0))        // or every on-screen page renders white forever
            if (t.id == id) return &t;
    return nullptr;
}

// The worker's finished renders land here on the UI thread: each result
// re-validated against what the owner still wants (the page may have been
// zoomed, rotated or closed meanwhile) and inserted into the very cache the
// sync path used. Pages repaint the view; thumbs repaint just their own strip.
static void RqDrain() {
    std::vector<RRes> done;
    { std::lock_guard<std::mutex> lk(g_rqM); done.swap(g_rqDone); }
    if (done.empty()) return;
    bool viewInv = false;
    for (auto& r : done) {
        if (r.kind == RJ_PAGE) {
            DocState* D = DocById(r.docId);
            if (!D || r.page < 0 || r.page >= D->n) { if (r.b) DeleteObject(r.b); continue; }
            int want = (D->prot.count(r.page) ? D->prot[r.page] : 0) |
                       (D->pflip.count(r.page) ? D->pflip[r.page] << 4 : 0);
            if (r.w != PW(*D, r.page) || r.h != PH(*D, r.page) || r.rot != want) {
                if (r.b) DeleteObject(r.b);            // stale size or transform: the next
                continue;                              // paint re-asked at the right shape
            }
            CacheDrop(*D, r.page);
            if (!r.b) continue;            // a failed page render stays uncached and unpainted:
                                           // a null entry would make every later paint a hit
                                           // that returns nothing, the page white for good -
                                           // the next paint asks again, the sync path's own
                                           // behavior. Thumbs keep their deliberate placeholder
            D->cache[r.page] = Ent{ r.b, r.w, r.h, r.rot, ++D->tick };
            D->cacheBytes += (long long)r.w * r.h * 4;
            Evict(*D, r.page, r.page);
            viewInv = true;
        } else {
            DocState* D = DocById(r.docId);
            if (!D || D != &DVF() || r.page < 0 || r.page >= D->n || r.w != sd.tw || r.h != STH(r.page)) {
                if (r.b) DeleteObject(r.b);
                continue;                              // the strip moved to another document
            }
            sd.th[r.page] = Ent{ r.b, r.w, r.h, 0, ++sd.tick };
            if (r.b) sd.thBytes += (long long)r.w * r.h * 4;
            SideThEvict(SidePageAt(sd.scroll), SidePageAt(sd.scroll + sd.ch - 1));
            if (g.side && sd.top.size() > (size_t)r.page) {
                RECT tr{ 0, sd.top[r.page] - sd.scroll, max(1, sd.cw),
                         sd.top[r.page] - sd.scroll + STH(r.page) + (int)(36 * g.dpi) };
                InvalidateRect(g.side, &tr, FALSE);    // just the card that arrived
            }
        }
    }
    if (viewInv && g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// The strip counts DOCUMENTS, not slots: a pane's blank start state is not a
// tab, and the front slot is a stale husk while its document is live, so the
// count reads the live state there (KillerPDF counts Doc/DeferredPath).
static int TabDocCount(bool b) {
    std::vector<DocState>& T = Tabs(b);
    if (T.empty()) return DV(b).n > 0 ? 1 : 0;
    int c = 0;
    for (int i = 0; i < (int)T.size(); i++)
        if ((i == Act(b) ? DV(b).n : T[i].n) > 0) c++;
    return c;
}
static bool BandShown(bool b) {   // RebuildTabStrip: the strip exists from the second
    return !g.full && TabDocCount(b) > 1;   // document, and full screen gives the row to the page
}
static int BANDH() {              // the band: 3px top inset + 29px face + 1px foot
    return (int)(33 * g.dpi);     // (KillerPDF's TabMargin 0,3,0,1 around the 29px face)
}
static int BandInset(bool b) { return BandShown(b) ? BANDH() : 0; }

// Make sure the pane HAS a session list, adopting the live working set as its
// first session's document on first use (EnsureInitialSession, minus the
// capture: the front slot stays a husk until a switch parks the live state).
static void TabEnsure(bool b) {
    if (!Tabs(b).empty()) return;
    Tabs(b).push_back(DocState{});
    g.tabUndo[b].push_back({});
    g.tabRedo[b].push_back({});
    Act(b) = 0;
}

// Copy the live working set INTO the front session (CaptureSessionState). The
// move is the capture: the bitmaps, annotations, transforms, checkpoint and
// undo pair all ride the DocState. The live fields are left a husk that the
// very next apply overwrites - nothing may read them in between.
static void TabCaptureLive(bool b) {
    TabEnsure(b);
    int a = Act(b);
    Tabs(b)[a] = std::move(DV(b));
    g.tabUndo[b][a] = std::move(g.undo);
    g.tabRedo[b][a] = std::move(g.redo);
    DV(b) = DocState{};
}

// KillerPDF's CancelTransientForSwitch: the live interactions die before the
// working set changes, so nothing bleeds onto the next document.
static void TabCancelTransients(bool b) {
    CommitTextEdit();
    if (g.annPaneB == b) {                        // the pane's own gestures die with it
        PolyCancel(g.view);
        g.annInk = false; g.annArm = false; g.annMove = false; g.annResize = false;
    }
    if (g.annSelB == b) { g.annSelPage = -1; g.annSelIdx = -1; }
    g.brushOn = false;
    if (g.barOn) {                                // the floating bar belongs to the outgoing tool
        g.barOn = false; g.barTool = -1;
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    }
    if (fd.open && fd.pane == b) {                // the find is pinned to ITS pane (CloseSearchBar)
        if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
        if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
        fd = Find{};
    }
    SelClear(DV(b));                              // ClearSelection + ClearTextSelection
}

// Point the live working set AT the session (ApplySessionState), mark it the
// front tab, and hand the window-level surfaces their new document.
static void TabApply(bool b, int idx) {
    DV(b) = std::move(Tabs(b)[idx]);
    g.undo = std::move(g.tabUndo[b][idx]);
    g.redo = std::move(g.tabRedo[b][idx]);
    Act(b) = idx;
    DV(b).tabTick = ++g_tabTick;
}

// TouchRenderLru: three tabs keep their rendered page bitmaps no matter what
// (KillerPDF's RenderCacheTabCap window), and MORE tabs stay warm as long as
// the pane's whole strip fits the byte budget - ten light documents come back
// instantly, memory still has a hard ceiling. ClearCache runs per session,
// exactly the per-tab cache KillerPDF clears when a tab falls out of the LRU.
static void TabTrimCaches(bool b) {
    std::vector<DocState>& T = Tabs(b);
    std::vector<std::pair<unsigned long long, int>> v;
    for (int i = 0; i < (int)T.size(); i++)
        if (i != Act(b) && !T[i].cache.empty()) v.push_back({ T[i].tabTick, i });
    std::sort(v.begin(), v.end(), [](auto& a, auto& c) { return a.first > c.first; });
    long long total = 0;
    for (auto& t : T) total += t.cacheBytes;
    for (int k = 3; k < (int)v.size() && total > TAB_CACHE_BUDGET; k++) {
        total -= T[v[k].second].cacheBytes;
        ClearCache(T[v[k].second]); T[v[k].second].txt.clear();
    }
}

// The surfaces that read the FOCUSED pane's document after its identity
// changed: the sidebar's ring, the footer's page/zoom, the rail's light and
// the toolbar's tool - FocusPane's choreography, reused for a tab switch.
static void TabChrome(bool b) {
    if (b != FocusB()) {                          // an unfocused pane's switch touches only itself
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);
        return;
    }
    for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
    sd.th.clear(); sd.thBytes = 0; sd.scroll = 0; sd.cur = -1; sd.hold = 0; sd.tw = 0;
    // sd.cw/sd.ch stay: zeroing them made the SideLayout below early-return on
    // cw <= 0, which killed the thumbnail pump, froze the strip on the outgoing
    // document's pixels and left sd.top describing the OTHER document - the
    // next paint then indexed the focused document against it
    SideLayout();
    UpdateTitle();
    SideSync();
    StatusRepaint();
    RailRepaint();
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
}

// SwitchToTab: make session idx the pane's front tab.
static void SwitchTab(bool b, int idx) {
    TabEnsure(b);
    if (idx < 0 || idx >= (int)Tabs(b).size() || idx == Act(b)) return;
    TabCancelTransients(b);
    TabCaptureLive(b);                            // the outgoing tab parks with its state
    TabApply(b, idx);                             // ... and the target becomes live
    TabTrimCaches(b);
    LayoutMain();                                 // the band may appear or collapse; reflow
    TabChrome(b);
}

// FindOpenSession: an open session for this exact file (case-insensitive full
// path), or -1. The front slot reads the live state, the only place its
// current path is guaranteed current.
static int TabFindOpen(bool b, const std::wstring& p) {
    wchar_t want[MAX_PATH * 4];
    GetFullPathNameW(p.c_str(), (DWORD)_countof(want), want, nullptr);
    std::vector<DocState>& T = Tabs(b);
    for (int i = 0; i < (int)T.size(); i++) {
        const DocState& s = (i == Act(b)) ? DV(b) : T[i];
        if (!s.n || s.path.empty()) continue;
        wchar_t have[MAX_PATH * 4];
        GetFullPathNameW(s.path.c_str(), (DWORD)_countof(have), have, nullptr);
        if (!lstrcmpiW(have, want)) return i;
    }
    return -1;
}

// OpenInNewTab: the dialog, a drop and the command line all land here. An
// already-open CLEAN tab wins over a duplicate (FindOpenSession's short
// circuit, "Already open" in the footer); an empty front tab is reused
// (BeginTabLoad's reuse rule); a busy one spawns a new session at the strip's
// end that inherits the current view mode, so a fresh document does not snap
// back to Continuous when the reader lives in Single. A failed open rolls the
// strip back exactly (AbortTabLoad).
static bool TabOpenInto(bool b, const std::wstring& p) {
    int found = TabFindOpen(b, p);
    if (found >= 0) {
        const DocState& s = (found == Act(b)) ? DV(b) : Tabs(b)[found];
        if (!DocDirty(s)) {
            SwitchTab(b, found);                  // a no-op when it is already the front tab
            SetStatusHold(std::wstring(L"Already open: ") + s.name);
            return true;
        }
    }
    TabEnsure(b);
    int prev = Act(b);
    bool wasEmpty = !DV(b).n;                     // the front tab is blank: reuse it
    int created = -1;
    if (!wasEmpty) {
        int vm = DV(b).vm, fit = DV(b).fitMode;   // the view mode the new tab inherits
        TabCancelTransients(b);
        TabCaptureLive(b);                        // the current document parks in its slot
        Tabs(b).push_back(DocState{});            // the new session rides the strip's end
        g.tabUndo[b].push_back({});
        g.tabRedo[b].push_back({});
        Act(b) = (int)Tabs(b).size() - 1;
        DV(b) = DocState{};
        DV(b).vm = vm; DV(b).fitMode = fit;
        created = Act(b);
    } else {
        TabCancelTransients(b);
    }
    if (!Open(DV(b), p)) {
        if (created >= 0) {                       // AbortTabLoad: the strip never saw it
            Tabs(b).pop_back();
            g.tabUndo[b].pop_back();
            g.tabRedo[b].pop_back();
            DV(b) = std::move(Tabs(b)[prev]);
            g.undo = std::move(g.tabUndo[b][prev]);
            g.redo = std::move(g.tabRedo[b][prev]);
            Act(b) = prev;
        }
        return false;
    }
    return true;
}

// CloseTabCore. Returns false when a prompt was canceled (the tab stays), so
// the sweep helpers can stop or skip. An INACTIVE dirty tab is brought forward
// first - a save works on the live document - exactly KillerPDF's route.
static bool CloseTabAt(bool b, int idx) {
    TabEnsure(b);
    if (idx < 0 || idx >= (int)Tabs(b).size()) return false;
    bool live = idx == Act(b);
    if (!(live ? DV(b).n : Tabs(b)[idx].n)) return false;   // the blank start tab is not a document
    if (!live && DocDirty(Tabs(b)[idx])) SwitchTab(b, idx);
    if (idx != Act(b)) {                          // a clean inactive tab: release it where it sits
        ClearCache(Tabs(b)[idx]);
        Tabs(b).erase(Tabs(b).begin() + idx);
        g.tabUndo[b].erase(g.tabUndo[b].begin() + idx);
        g.tabRedo[b].erase(g.tabRedo[b].begin() + idx);
        if (idx < Act(b)) Act(b)--;
        LayoutMain();
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);
        StatusRepaint();
        return true;
    }
    // the front tab: capture nothing - the live fields ARE the tab
    if (g.tedit) CommitTextEdit();
    TabCancelTransients(b);
    if (DocDirty(DV(b))) {                        // KillerPDF's tab close asks first
        int r = AskPrompt3(L"", L"This file has unsaved changes. Save them before closing it?",
                           L"Cancel", L"Don't Save", L"Save");
        if (r == 0) return false;                 // Cancel: the document stays open
        if (r == 2) {                             // Save: a failed write keeps it open too
            std::wstring err;
            if (!WriteAnnotatedPdf(DV(b), DV(b).path, err)) {
                SetStatusHold(std::wstring(L"Save failed: ") + err);
                return false;
            }
        }
    }
    ClearCache(DV(b));                            // the document's bitmaps die with it
    if (fd.open && fd.pane == b) {                // the find dies only when ITS document dies
        if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
        if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
        fd = Find{};
    }
    Tabs(b).erase(Tabs(b).begin() + idx);
    g.tabUndo[b].erase(g.tabUndo[b].begin() + idx);
    g.tabRedo[b].erase(g.tabRedo[b].begin() + idx);
    if (Tabs(b).empty()) {                        // the last tab: a blank session and the
        Tabs(b).push_back(DocState{});            // empty drop-zone state (CloseTabCore's blank)
        g.tabUndo[b].push_back({});
        g.tabRedo[b].push_back({});
        Act(b) = 0;
        DV(b) = DocState{};
        g.undo.clear(); g.redo.clear();
    } else {                                      // the neighbor takes the front (idx clamped)
        int next = min(idx, (int)Tabs(b).size() - 1);
        DV(b) = std::move(Tabs(b)[next]);
        g.undo = std::move(g.tabUndo[b][next]);
        g.redo = std::move(g.tabRedo[b][next]);
        Act(b) = next;
        DV(b).tabTick = ++g_tabTick;
    }
    g.annSelPage = -1; g.annSelIdx = -1;
    LayoutMain();
    TabChrome(b);
    SetStatusHold(L"Document closed");
    return true;
}

// CloseOtherTabs: every other document tab closes, each dirty one asking like
// a manual close; a canceled tab is skipped and the sweep moves on. The keeper
// is followed by its path - closes above it shift the indices under it.
static void CloseOtherTabs(bool b, int keep) {
    TabEnsure(b);
    if (keep < 0 || keep >= (int)Tabs(b).size()) return;
    std::wstring keepPath = (keep == Act(b)) ? DV(b).path : Tabs(b)[keep].path;
    std::vector<std::wstring> skipped;
    for (;;) {
        int k = -1;
        for (int i = 0; i < (int)Tabs(b).size(); i++)
            if (((i == Act(b)) ? DV(b).path : Tabs(b)[i].path) == keepPath) { k = i; break; }
        if (k < 0) return;
        int hit = -1;
        std::wstring hitPath;
        for (int i = 0; i < (int)Tabs(b).size(); i++) {
            if (i == k) continue;
            bool live = i == Act(b);
            if (!((live ? DV(b).n : Tabs(b)[i].n) > 0)) continue;
            const std::wstring& p2 = live ? DV(b).path : Tabs(b)[i].path;
            bool gone = false;
            for (auto& s2 : skipped) if (s2 == p2) { gone = true; break; }
            if (gone) continue;
            hit = i; hitPath = p2;
            break;
        }
        if (hit < 0) return;
        if (!CloseTabAt(b, hit)) skipped.push_back(hitPath);
    }
}

// CloseAllTabs (Ctrl+Shift+Q): ONE combined ask for every dirty tab rather
// than a prompt each, then the pane resets to a single blank session.
static void CloseAllTabs(bool b) {
    TabEnsure(b);
    bool any = false, anyDirty = false;
    for (int i = 0; i < (int)Tabs(b).size(); i++) {
        DocState& S = (i == Act(b)) ? DV(b) : Tabs(b)[i];
        if (!S.n) continue;
        any = true;
        if (DocDirty(S)) anyDirty = true;
    }
    if (!any) return;
    if (g.tedit) CommitTextEdit();
    if (anyDirty) {
        int r = AskPrompt3(L"", L"You have unsaved changes. Save them before closing all tabs?",
                           L"Cancel", L"Don't Save", L"Save");
        if (r == 0) return;
        if (r == 2) {
            for (int i = 0; i < (int)Tabs(b).size(); i++) {
                DocState& S = (i == Act(b)) ? DV(b) : Tabs(b)[i];
                if (!S.n || !DocDirty(S)) continue;
                std::wstring err;
                if (!WriteAnnotatedPdf(S, S.path, err)) {
                    SetStatusHold(std::wstring(L"Save failed: ") + err);
                    return;                       // a failed save stops the sweep, window up
                }
                SnapClean(S);
            }
        }
    }
    for (int i = 0; i < (int)Tabs(b).size(); i++) ClearCache(Tabs(b)[i]);
    if (fd.open && fd.pane == b) {
        if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
        if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
        fd = Find{};
    }
    Tabs(b).clear();
    g.tabUndo[b].clear();
    g.tabRedo[b].clear();
    Tabs(b).push_back(DocState{});
    g.tabUndo[b].push_back({});
    g.tabRedo[b].push_back({});
    Act(b) = 0;
    DV(b) = DocState{};
    g.undo.clear(); g.redo.clear();
    g.annSelPage = -1; g.annSelIdx = -1;
    LayoutMain();
    TabChrome(b);
    SetStatusHold(L"All tabs closed");
}

// CycleTab: Ctrl+Tab walks the document tabs forward, Ctrl+Shift+Tab backward,
// wrapping, blanks skipped.
static void CycleTab(bool b, int dir) {
    if (TabDocCount(b) < 2) return;
    int cnt = (int)Tabs(b).size();
    for (int k = 1; k <= cnt; k++) {
        int i = ((Act(b) + dir * k) % cnt + cnt) % cnt;
        if ((i == Act(b) ? DV(b).n : Tabs(b)[i].n) > 0) { SwitchTab(b, i); return; }
    }
}

// DetachSessionExt + AdoptSessionExt: a session is self-contained - document,
// annotations, transforms, checkpoint, undo pair, render cache and view state
// all ride the value - so a move between panes is a move, nothing reloads.
// The tab you just dragged is the one you are looking at.
static void TabMoveToPane(bool from, int idx, bool to, int insertAt) {
    if (from == to) return;
    TabEnsure(from); TabEnsure(to);
    if (idx < 0 || idx >= (int)Tabs(from).size()) return;
    if (!(idx == Act(from) ? DV(from).n : Tabs(from)[idx].n)) return;
    bool wasLive = idx == Act(from);
    DocState s;
    std::vector<App::UndoSnap> us, rs;
    if (wasLive) {
        s = std::move(DV(from));
        us = std::move(g.undo); rs = std::move(g.redo);
    } else {
        s = std::move(Tabs(from)[idx]);
        us = std::move(g.tabUndo[from][idx]); rs = std::move(g.tabRedo[from][idx]);
    }
    Tabs(from).erase(Tabs(from).begin() + idx);
    g.tabUndo[from].erase(g.tabUndo[from].begin() + idx);
    g.tabRedo[from].erase(g.tabRedo[from].begin() + idx);
    if (wasLive) {
        if (Tabs(from).empty()) {
            Tabs(from).push_back(DocState{});
            g.tabUndo[from].push_back({});
            g.tabRedo[from].push_back({});
            Act(from) = 0;
            DV(from) = DocState{};
            g.undo.clear(); g.redo.clear();
        } else {
            int next = min(idx, (int)Tabs(from).size() - 1);
            DV(from) = std::move(Tabs(from)[next]);
            g.undo = std::move(g.tabUndo[from][next]);
            g.redo = std::move(g.tabRedo[from][next]);
            Act(from) = next;
            DV(from).tabTick = ++g_tabTick;
        }
    } else if (idx < Act(from)) Act(from)--;
    if (fd.open && fd.pane == from && wasLive) {  // the scan's document left this pane:
        if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }   // the find dies with it
        if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
        fd = Find{};
    }
    s.tabTick = ++g_tabTick;
    for (auto& sn : us) sn.paneB = to;            // the snapshots now belong to the other pane
    for (auto& sn : rs) sn.paneB = to;
    int at = std::clamp(insertAt, 0, (int)Tabs(to).size());
    Tabs(to).insert(Tabs(to).begin() + at, std::move(s));
    g.tabUndo[to].insert(g.tabUndo[to].begin() + at, std::move(us));
    g.tabRedo[to].insert(g.tabRedo[to].begin() + at, std::move(rs));
    if (at <= Act(to)) Act(to)++;
    TabTrimCaches(from); TabTrimCaches(to);
    SwitchTab(to, at);
    LayoutMain();                                 // both strips may have grown or collapsed
    TabChrome(from); TabChrome(to);
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
static void BarThumb(int docLen, int viewLen, int scroll, int& tlen, int& tpos, int range = -1) {
    int R = range >= 0 ? range : docLen - viewLen;   // the scroll's real travel; the v-thumbs
    if (docLen <= viewLen || docLen <= 0 || viewLen <= 0 || R <= 0) { tlen = 0; tpos = 0; return; }
    int t = (int)((long long)viewLen * viewLen / docLen);
    int fl = (int)min(64.0 * g.dpi, viewLen * 0.5);
    tlen = std::clamp(t, fl, viewLen);
    tpos = std::clamp((int)((long long)(viewLen - tlen) * max(0, scroll) / R), 0, viewLen - tlen);
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
            if (g_webOn) WebSideDraw(sd.bdc, sd.cw, sd.ch);         // browser mode: the rail is
            else SideDraw(sd.bdc, sd.cw, sd.ch, 0);                 // the web tabs' thumbnails
            BitBlt(dc, 0, 0, sd.cw, sd.ch, sd.bdc, 0, 0, SRCCOPY);  // shared field runs unbroken
            SetTimer(h, 2, 25, nullptr);   // finish any pending thumbs
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 2 && (g_webOn ? !WebSidePump() : !SidePump())) KillTimer(h, 2);
        return 0;
    case WM_SIZE:                        // the cards re-flow to the width: a full
        InvalidateRect(h, nullptr, FALSE);   // repaint leaves no stale column behind
        return 0;
    case WM_MOUSEWHEEL:
        if (g_webOn) { WebSideWheel(-GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120); return 0; }
        SideScrollTo(sd.scroll - GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_webOn) { WebSideDown(p); return 0; }
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
        if (g_webOn) { WebSideMove(h, p); return 0; }
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
        if (g_webOn) { WebSideLeave(); return 0; }
        if (sd.barHot) { sd.barHot = false; InvalidateRect(h, nullptr, FALSE); }
        if (sd.hover != -1) { sd.hover = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_webOn) { WebSideUp(h, p); return 0; }
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
            if (g_webOn) {                 // the web rail's cards take the hand: a rail card
                                           // switches tabs, the same affordance the page
                                           // thumbs get when the reader rides the window
                bool over = p.x < sd.cw - SBar() && ws.docH > 0 && WebSideAt(p.y + ws.scroll) >= 0;
                SetCursor(LoadCursorW(nullptr, over ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
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
    int y0 = BandInset(b);               // KillerPDF's bar anchors to the card's top - under
    return { x0, y0, x0 + g_abW, y0 + g_abH };   // the tab strip's row, never over its tabs
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
        } else if (g_lp.who == 2) {              // the tab strip's menus (TabPopPick)
            TabPopPick(hot);
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
    if (g_webOn) { if (i >= 2 && i <= 7) WebZoomApplyItem(i - 2); return; }   // the web page's zoom
    if (i == 0) ApplyFit(true, FocusB());
    else if (i == 1) ApplyFit(false, FocusB());
    else if (i >= 2 && i <= 7) SetZoom(ZoomPcts[i - 2], g.cw / 2, g.ch / 2);
}
static int ZoomCurrent() {                       // which item the closed box shows
    if (g_webOn) return WebZoomCurrent();        // the web page's zoom, same 2..7 space
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
    if (!g_webOn) { x -= bw24; g_stR[ST_INV] = { x, by, x + bw24, by + btnH }; }   // invert is a reader thing
    x -= bw24;  g_stR[ST_FULL] = { x, by, x + bw24, by + btnH };
    if (!g_webOn) {
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
    }
    g_stR[ST_STATUS] = { 0, 0, max(x, (int)(40 * dpi)), SBH() };
}

static int StatusHit(POINT p) {
    RECT r;
    if (!g.status || !GetClientRect(g.status, &r) || r.right <= 0) return -1;
    StatusRects(r.right);
    for (int i = 0; i < ST_COUNT; i++) {
        if (p.x >= g_stR[i].left && p.x < g_stR[i].right && p.y >= g_stR[i].top && p.y < g_stR[i].bottom) {
            if (g_webOn && i != ST_FULL && (i == ST_SIZE || (i >= ST_MCONT && i <= ST_INV))) return -1;
            return (!DVF().n && i != ST_STATUS && i != ST_VER) ? -1 : i;
        }
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
    case ST_ZOUT: if (g_webOn) WebZoomBy(0.8); else ZoomBy(0.8); break;
    case ST_ZIN: if (g_webOn) WebZoomBy(1.25); else ZoomBy(1.25); break;
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
    int vx0 = ((g_webOn || (g.sideOn && DVF().n)) && !g.full ? SW() : 0) + RW();
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

// ============================================================
//  The browser. One toolbar button toggles between the PDF reader and a web
//  mode dressed like Safari (dark appearance): a flat unified toolbar on top
//  (reader button, back/forward chevrons, a centered address field that shows
//  the site while idle - padlock hugging the name - and opens to the full URL
//  with a blue focus ring, reload/stop inside its right edge, a progress line
//  along its foot while a page loads, the menu at the far right), and under
//  it the READER'S OWN tab strip, carried over unchanged for the web tabs:
//  the grainy field, #555 dividers, the 1px green rule over the open tab, the
//  muted close x that turns red, the + at the right. The WebView rides in the
//  same rounded card the reader's pages sit in. The engine is Windows' own
//  WebView2 - Evergreen Chromium, shared with Edge - so the exe carries no
//  browser bytes at all, only this glue. docsy's page-thumbnail rail keeps
//  showing the OPEN WEB TABS, captured live with CapturePreview. No runtime
//  (rare): the mode still toggles and explains itself, nothing else changes.
// ============================================================

struct WebTab {
    unsigned long long id = 0;
    std::wstring url, title;
    wrl::ComPtr<ICoreWebView2Controller> ctr;
    wrl::ComPtr<ICoreWebView2> web;
    double zoom = 1.0;
    bool loading = false, canBack = false, canFwd = false;
    bool shown = false;                    // the first frame has landed: a controller whose
                                           // engine has yet to composite stays invisible, so
                                           // a fresh tab shows the card's backdrop, never the
                                           // black glass the bare surface reads as
    bool capturing = false, capturePending = false;
    unsigned long long captureDue = 0;
    std::wstring pending;                  // the URL a brand-new tab opens once its
                                           // controller finishes building
    EventRegistrationToken tkNavS{}, tkNavC{}, tkSrc{}, tkTitle{}, tkHist{}, tkDl{};
    HBITMAP thumb = nullptr; int thumbW = 0, thumbH = 0; double ar = 0.0;
};
static std::vector<WebTab> g_wt;
static unsigned long long g_wtIds = 1;
static wrl::ComPtr<ICoreWebView2Environment> g_wvEnv;
static bool g_webChecked = false, g_webAvail = false;

// the sidebar's web state: the one definition lives up by the palette, where
// the toolbar's proc can see ws.sHot; the comment there tells why.

static HWND g_wedit = nullptr;             // the omnibox edit
static WNDPROC g_weditPrev = nullptr;
static std::wstring g_webVer;              // the runtime's version, for the fail card
static HFONT g_webEditFont = nullptr;
static HBRUSH g_webEditBrush = nullptr;
static RECT g_webOmni{};                   // the pill, in bar coords
static RECT g_webFailBtn{};                // the no-engine card's button, view coords

enum { WB_READER, WB_BACK, WB_FWD, WB_RELOAD, WB_NEWTAB, WB_MENU, WB_SIDE, WB_EXT, WB_COUNT };
static RECT g_wbR[WB_COUNT];

static WebTab* WebTabById(unsigned long long id) {
    for (auto& t : g_wt) if (t.id == id) return &t;
    return nullptr;
}
static WebTab* WebCur() { return g_wtCur >= 0 && g_wtCur < (int)g_wt.size() ? &g_wt[g_wtCur] : nullptr; }
static int WebCurIdx() { return g_wtCur; }

static void WebRepaintChrome() {
    if (g_wstrip) InvalidateRect(g_wstrip, nullptr, FALSE);
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
    if (g.side) InvalidateRect(g.side, nullptr, FALSE);
    StatusRepaint();
}
static void WebStripSpin();                // the loading spinner's timer, below
static void WebSyncOmnibox();              // module order: the omnibox helpers live
static void WebSyncOmniboxForce();         // below their first callers
static LRESULT CALLBACK WebEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp);

// ---------------------------------------------------------------------------
// Extension packages: everything between a Chrome Web Store download and an
// unpacked folder WebView2 can load, in plain C++ (no zlib, no tar, no
// PowerShell). A .crx is a small header in front of an ordinary zip; the
// header carries the developer's public key, and Chrome derives an
// extension's ID from that key - so the key is written back into the
// unpacked manifest.json, which makes the loaded extension keep its real
// store ID (extensions that name themselves in OAuth redirects, externally
// connectable pages or web-accessible URLs depend on it).
// ---------------------------------------------------------------------------
namespace crxk {

static void Sha256(const unsigned char* d, size_t n, unsigned char out[32]) {
    static const unsigned K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    unsigned h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    std::vector<unsigned char> m(d, d + n);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    unsigned long long bits = (unsigned long long)n * 8;
    for (int i = 7; i >= 0; i--) m.push_back((unsigned char)(bits >> (i * 8)));
    auto R = [](unsigned x, int k) { return (x >> k) | (x << (32 - k)); };
    for (size_t o = 0; o < m.size(); o += 64) {
        unsigned w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (unsigned)m[o + i*4] << 24 | (unsigned)m[o + i*4 + 1] << 16 | (unsigned)m[o + i*4 + 2] << 8 | m[o + i*4 + 3];
        for (int i = 16; i < 64; i++) {
            unsigned s0 = R(w[i-15], 7) ^ R(w[i-15], 18) ^ (w[i-15] >> 3);
            unsigned s1 = R(w[i-2], 17) ^ R(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g2 = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            unsigned S1 = R(e, 6) ^ R(e, 11) ^ R(e, 25), ch = (e & f) ^ (~e & g2);
            unsigned t1 = hh + S1 + ch + K[i] + w[i];
            unsigned S0 = R(a, 2) ^ R(a, 13) ^ R(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
            unsigned t2 = S0 + mj;
            hh = g2; g2 = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e; h[5] += f; h[6] += g2; h[7] += hh;
    }
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[i*4 + j] = (unsigned char)(h[i] >> (24 - j * 8));
}

static std::string B64(const unsigned char* d, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i+1] << 8 : 0) | (i + 2 < n ? d[i+2] : 0);
        r += T[v >> 18 & 63]; r += T[v >> 12 & 63];
        r += i + 1 < n ? T[v >> 6 & 63] : '='; r += i + 2 < n ? T[v & 63] : '=';
    }
    return r;
}

// ---- raw DEFLATE (RFC 1951): canonical-Huffman decoder in the puff.c shape ----
struct BitIn { const unsigned char* p; size_t n, pos = 0; unsigned long long buf = 0; int cnt = 0; bool err = false;
    int Bits(int k) {
        while (cnt < k) { if (pos >= n) { err = true; return 0; } buf |= (unsigned long long)p[pos++] << cnt; cnt += 8; }
        int v = (int)(buf & ((1ull << k) - 1)); buf >>= k; cnt -= k; return v; } };
struct Huff { short count[16], symbol[288]; };
static int HuffBuild(Huff& h, const short* len, int n) {
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= h.count[l]; if (left < 0) return left; }
    short offs[16]; offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h.count[l];
    for (int i = 0; i < n; i++) if (len[i]) h.symbol[offs[len[i]]++] = (short)i;
    return left;
}
static int HuffDecode(BitIn& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= b.Bits(1); if (b.err) return -1;
        int c = h.count[l];
        if (code - c < first) return h.symbol[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
}
static bool Inflate(const unsigned char* src, size_t n, std::vector<unsigned char>& out, size_t limit) {
    static const short lb[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short le[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short db[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short de[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    BitIn b{ src, n };
    int last;
    do {
        last = b.Bits(1); int type = b.Bits(2);
        if (b.err) return false;
        if (type == 0) {
            b.buf = 0; b.cnt = 0;
            if (b.pos + 4 > n) return false;
            unsigned len = src[b.pos] | src[b.pos + 1] << 8; b.pos += 4;
            if (b.pos + len > n || out.size() + len > limit) return false;
            out.insert(out.end(), src + b.pos, src + b.pos + len); b.pos += len;
        } else if (type == 1 || type == 2) {
            Huff lit, dist; short len[320];
            if (type == 1) {
                int i = 0;
                for (; i < 144; i++) len[i] = 8;
                for (; i < 256; i++) len[i] = 9;
                for (; i < 280; i++) len[i] = 7;
                for (; i < 288; i++) len[i] = 8;
                HuffBuild(lit, len, 288);
                for (i = 0; i < 30; i++) len[i] = 5;
                HuffBuild(dist, len, 30);
            } else {
                int nlen = b.Bits(5) + 257, ndist = b.Bits(5) + 1, ncode = b.Bits(4) + 4;
                if (b.err || nlen > 286 || ndist > 30) return false;
                static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                int i = 0;
                for (; i < ncode; i++) len[order[i]] = (short)b.Bits(3);
                for (; i < 19; i++) len[order[i]] = 0;
                Huff cl; if (HuffBuild(cl, len, 19) != 0) return false;
                i = 0;
                while (i < nlen + ndist) {
                    int sym = HuffDecode(b, cl); if (sym < 0) return false;
                    if (sym < 16) len[i++] = (short)sym;
                    else {
                        int rep, v = 0;
                        if (sym == 16) { if (i == 0) return false; v = len[i - 1]; rep = 3 + b.Bits(2); }
                        else if (sym == 17) rep = 3 + b.Bits(3); else rep = 11 + b.Bits(7);
                        if (i + rep > nlen + ndist) return false;
                        while (rep--) len[i++] = (short)v;
                    }
                }
                if (len[256] == 0) return false;
                int e1 = HuffBuild(lit, len, nlen); if (e1 < 0 || (e1 > 0 && nlen - lit.count[0] != 1)) return false;
                int e2 = HuffBuild(dist, len + nlen, ndist); if (e2 < 0 || (e2 > 0 && ndist - dist.count[0] != 1)) return false;
            }
            for (;;) {
                int sym = HuffDecode(b, lit); if (sym < 0) return false;
                if (sym < 256) { if (out.size() >= limit) return false; out.push_back((unsigned char)sym); }
                else if (sym == 256) break;
                else {
                    sym -= 257; if (sym >= 29) return false;
                    int l = lb[sym] + b.Bits(le[sym]);
                    int ds = HuffDecode(b, dist); if (ds < 0 || ds >= 30) return false;
                    size_t dd = (size_t)db[ds] + b.Bits(de[ds]);
                    if (b.err || dd > out.size() || out.size() + l > limit) return false;
                    for (int k = 0; k < l; k++) out.push_back(out[out.size() - dd]);
                }
            }
        } else return false;
    } while (!last);
    return !b.err;
}

// ---- zip: central directory, stored + deflated entries ----
struct ZipEnt { std::string name; unsigned method, csize, usize, off; };
static unsigned U16(const unsigned char* p) { return p[0] | p[1] << 8; }
static unsigned U32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static bool ZipList(const unsigned char* d, size_t n, std::vector<ZipEnt>& v) {
    if (n < 22) return false;
    size_t e = n - 22; bool found = false;
    size_t lim = n - 22 < 65557 ? n - 22 : 65557;
    for (size_t k = 0; k <= lim; k++, e--)
        if (U32(d + e) == 0x06054b50) { found = true; break; }
    if (!found) return false;
    unsigned cnt = U16(d + e + 10); size_t cd = U32(d + e + 16);
    for (unsigned i = 0; i < cnt; i++) {
        if (cd + 46 > n || U32(d + cd) != 0x02014b50) return false;
        ZipEnt z; z.method = U16(d + cd + 10); z.csize = U32(d + cd + 20); z.usize = U32(d + cd + 24);
        unsigned nl = U16(d + cd + 28), el = U16(d + cd + 30), cl = U16(d + cd + 32); z.off = U32(d + cd + 42);
        if (cd + 46 + nl > n) return false;
        z.name.assign((const char*)d + cd + 46, nl);
        v.push_back(z); cd += 46 + nl + el + cl;
    }
    return true;
}
static bool ZipRead(const unsigned char* d, size_t n, const ZipEnt& z, std::vector<unsigned char>& out) {
    if ((size_t)z.off + 30 > n || U32(d + z.off) != 0x04034b50) return false;
    size_t s = (size_t)z.off + 30 + U16(d + z.off + 26) + U16(d + z.off + 28);
    if (s + z.csize > n || z.usize > (256u << 20)) return false;
    out.clear();
    if (z.method == 0) { out.assign(d + s, d + s + z.csize); return true; }
    if (z.method == 8) return Inflate(d + s, z.csize, out, z.usize);
    return false;
}

// ---- the .crx header: where the zip starts, and the developer's public key ----
static bool Varint(const unsigned char* d, size_t n, size_t& p, unsigned long long& v) {
    v = 0; for (int s = 0; s < 64 && p < n; s += 7) { unsigned char c = d[p++]; v |= (unsigned long long)(c & 0x7f) << s; if (!(c & 0x80)) return true; }
    return false;
}
struct Field { unsigned long long num; const unsigned char* p; size_t len; };
static bool PbFields(const unsigned char* d, size_t n, std::vector<Field>& f) {
    size_t p = 0;
    while (p < n) {
        unsigned long long tag, v; if (!Varint(d, n, p, tag)) return false;
        int wt = (int)(tag & 7);
        if (wt == 0) { if (!Varint(d, n, p, v)) return false; }
        else if (wt == 2) { if (!Varint(d, n, p, v) || p + v > n) return false; f.push_back({ tag >> 3, d + p, (size_t)v }); p += (size_t)v; }
        else if (wt == 1) p += 8; else if (wt == 5) p += 4; else return false;
    }
    return true;
}
static bool CrxParse(const unsigned char* d, size_t n, size_t& zipOff, std::string& key) {
    key.clear();
    if (n >= 4 && d[0] == 'P' && d[1] == 'K') { zipOff = 0; return true; }     // a bare zip
    if (n < 16 || memcmp(d, "Cr24", 4) != 0) return false;
    unsigned ver = U32(d + 4);
    if (ver == 2) {
        size_t pk = U32(d + 8), sg = U32(d + 12);
        if (16 + pk + sg > n) return false;
        key.assign((const char*)d + 16, pk); zipOff = 16 + pk + sg; return true;
    }
    if (ver != 3) return false;
    size_t hl = U32(d + 8);
    if (12 + hl > n) return false;
    zipOff = 12 + hl;
    std::vector<Field> top; if (!PbFields(d + 12, hl, top)) return true;       // no key: still loadable
    std::vector<std::string> keys; std::string crxId;
    for (auto& f : top) {
        if (f.num == 2 || f.num == 3) {                                        // RSA / ECDSA proofs
            std::vector<Field> in; if (!PbFields(f.p, f.len, in)) continue;
            for (auto& g : in) if (g.num == 1) keys.emplace_back((const char*)g.p, g.len);
        } else if (f.num == 10000) {                                           // signed_header_data
            std::vector<Field> in; if (!PbFields(f.p, f.len, in)) continue;
            for (auto& g : in) if (g.num == 1) crxId.assign((const char*)g.p, g.len);
        }
    }
    for (auto& k : keys) {                                                     // the key the CRX id names
        unsigned char h[32]; Sha256((const unsigned char*)k.data(), k.size(), h);
        if (crxId.size() == 16 && memcmp(h, crxId.data(), 16) == 0) { key = k; return true; }
    }
    if (!keys.empty()) key = keys[0];
    return true;
}

// the manifest gets "key" as its first top-level member (skipped when it has one)
static bool JsonHasTopKey(const std::string& s, const char* name) {
    int depth = 0; size_t n = s.size();
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') {
            size_t j = i + 1; std::string str;
            while (j < n && s[j] != '"') { if (s[j] == '\\' && j + 1 < n) j++; str += s[j]; j++; }
            if (depth == 1) { size_t k = j + 1; while (k < n && isspace((unsigned char)s[k])) k++; if (k < n && s[k] == ':' && str == name) return true; }
            i = j;
        } else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
    }
    return false;
}
static std::string ManifestWithKey(const std::string& m, const std::string& keyDer) {
    if (keyDer.empty() || JsonHasTopKey(m, "key")) return m;
    size_t b = m.find('{'); if (b == std::string::npos) return m;
    std::string r = m;
    r.insert(b + 1, "\"key\":\"" + B64((const unsigned char*)keyDer.data(), keyDer.size()) + "\",");
    return r;
}
// the 32-letter extension id (a-p) Chrome derives from the key
static std::string IdFromKey(const std::string& keyDer) {
    unsigned char h[32]; Sha256((const unsigned char*)keyDer.data(), keyDer.size(), h);
    std::string id; for (int i = 0; i < 16; i++) { id += (char)('a' + (h[i] >> 4)); id += (char)('a' + (h[i] & 15)); }
    return id;
}

// ---- a small JSON reader for manifest.json / messages.json (comments tolerated) ----
struct J {
    char t = 'n';                                   // n null, b bool, d number, s string, a array, o object
    std::string s; double d = 0; bool b = false;
    std::vector<J> a; std::vector<std::pair<std::string, J>> o;
    const J* get(const char* k) const {
        if (t != 'o') return nullptr;
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    std::string str() const { return t == 's' ? s : std::string(); }
};
struct JP {
    const std::string& x; size_t i = 0;
    explicit JP(const std::string& s) : x(s) {}
    void Ws() {
        for (;;) {
            while (i < x.size() && (x[i] == ' ' || x[i] == '\t' || x[i] == '\r' || x[i] == '\n')) i++;
            if (i + 1 < x.size() && x[i] == '/' && x[i + 1] == '/') { while (i < x.size() && x[i] != '\n') i++; }
            else if (i + 1 < x.size() && x[i] == '/' && x[i + 1] == '*') {
                size_t e = x.find("*/", i + 2); i = e == std::string::npos ? x.size() : e + 2;
            } else break;
        }
    }
    static void Utf8(std::string& r, unsigned c) {
        if (c < 0x80) r += (char)c;
        else if (c < 0x800) { r += (char)(0xC0 | c >> 6); r += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { r += (char)(0xE0 | c >> 12); r += (char)(0x80 | (c >> 6 & 63)); r += (char)(0x80 | (c & 63)); }
        else { r += (char)(0xF0 | c >> 18); r += (char)(0x80 | (c >> 12 & 63)); r += (char)(0x80 | (c >> 6 & 63)); r += (char)(0x80 | (c & 63)); }
    }
    bool Hex4(unsigned& v) {
        if (i + 4 > x.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = x[i++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false;
        }
        return true;
    }
    bool Str(std::string& r) {
        if (i >= x.size() || x[i] != '"') return false;
        i++;
        while (i < x.size() && x[i] != '"') {
            char c = x[i++];
            if (c != '\\') { r += c; continue; }
            if (i >= x.size()) return false;
            char e = x[i++];
            switch (e) {
            case 'n': r += '\n'; break; case 't': r += '\t'; break; case 'r': r += '\r'; break;
            case 'b': r += '\b'; break; case 'f': r += '\f'; break;
            case 'u': {
                unsigned u; if (!Hex4(u)) return false;
                if (u >= 0xD800 && u < 0xDC00 && i + 1 < x.size() && x[i] == '\\' && x[i + 1] == 'u') {
                    i += 2; unsigned lo; if (!Hex4(lo)) return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                Utf8(r, u); break; }
            default: r += e; break;
            }
        }
        if (i >= x.size()) return false;
        i++; return true;
    }
    bool Val(J& v, int depth) {
        if (depth > 48) return false;
        Ws(); if (i >= x.size()) return false;
        char c = x[i];
        if (c == '{') {
            v.t = 'o'; i++; Ws();
            if (i < x.size() && x[i] == '}') { i++; return true; }
            for (;;) {
                Ws(); std::string k; if (!Str(k)) return false;
                Ws(); if (i >= x.size() || x[i] != ':') return false; i++;
                J c2; if (!Val(c2, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(c2));
                Ws(); if (i >= x.size()) return false;
                if (x[i] == ',') { i++; Ws(); if (i < x.size() && x[i] == '}') { i++; return true; } continue; }
                if (x[i] == '}') { i++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.t = 'a'; i++; Ws();
            if (i < x.size() && x[i] == ']') { i++; return true; }
            for (;;) {
                J c2; if (!Val(c2, depth + 1)) return false;
                v.a.push_back(std::move(c2));
                Ws(); if (i >= x.size()) return false;
                if (x[i] == ',') { i++; Ws(); if (i < x.size() && x[i] == ']') { i++; return true; } continue; }
                if (x[i] == ']') { i++; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = 's'; return Str(v.s); }
        if (x.compare(i, 4, "true") == 0) { v.t = 'b'; v.b = true; i += 4; return true; }
        if (x.compare(i, 5, "false") == 0) { v.t = 'b'; i += 5; return true; }
        if (x.compare(i, 4, "null") == 0) { i += 4; return true; }
        size_t st = i;
        while (i < x.size() && (isdigit((unsigned char)x[i]) || x[i] == '-' || x[i] == '+' || x[i] == '.' || x[i] == 'e' || x[i] == 'E')) i++;
        if (st == i) return false;
        v.t = 'd'; v.d = atof(x.substr(st, i - st).c_str()); return true;
    }
};
static bool JParse(const std::string& in, J& out) {
    size_t o = in.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    std::string s = in.substr(o);
    JP p(s); out = J();
    return p.Val(out, 0);
}
} // namespace crxk

// ---------------------------------------------------------------------------
//  Extensions. WebView2 can run Chrome extensions (content scripts, service
//  workers, declarative rules) from an unpacked folder, but ships no store, no
//  toolbar and no popup UI - so Docsy supplies all three:
//   * Chrome Web Store pages get a floating "Add to Docsy" button (the store's
//     own button insists on Chrome). Clicking it downloads the .crx from
//     Google's update endpoint on a worker thread, unpacks it with the code
//     above into %LOCALAPPDATA%\Docsy\Extensions\<id>, restores the store ID
//     and hands the folder to WebView2's AddBrowserExtension.
//   * Installed extensions with a popup show their icon on the toolbar; a
//     click opens the popup page (chrome-extension://<id>/<popup>) in a small
//     borderless window that sizes itself to the page, like Chrome's does and
//     closes when focus leaves it.
//   * The puzzle button's menu lists every extension (open, options, turn
//     off / on, remove) and links to the store.
//  They load before the first page navigates, so content scripts run on it.
// ---------------------------------------------------------------------------
struct WebExt {
    std::wstring id, storeId, name, version, dir, popup, options;   // storeId: the Web Store id it was fetched under
    Gdiplus::Bitmap* icon = nullptr;
    bool enabled = true;
    wrl::ComPtr<ICoreWebView2BrowserExtension> ext;
};
static std::vector<WebExt> g_wx;
static bool g_wxReady = false, g_wxBusy = false, g_wxOk = true;   // g_wxOk: the runtime can do extensions
static wrl::ComPtr<ICoreWebView2Profile7> g_wxProf;
static std::vector<RECT> g_wxR;            // the toolbar icons (strip coords) ...
static std::vector<int> g_wxIdx;           // ... and the g_wx entry each belongs to
static int g_wxHot = -1, g_wxPress = -1;
static std::vector<std::wstring> g_wxInstalling;
// one-shot engine restart: a browser process can come up refusing extension
// operations outright - another docsy.exe's engine owning the profile, inherited
// WEBVIEW2_* environment variables, a stale experiment payload, a runtime regression -
// and a cold relaunch is the only thing that re-reads all of it. The first
// INVALID_STATE schedules one; the install replays on the fresh engine.
static bool g_wxRestarted = false;                 // once per session, never a loop
static std::wstring g_wxRetryDir, g_wxRetryId;     // the install the old engine refused
static UINT_PTR g_wxRestartTimer = 0;
static void WebEnsureEnv();
static void WxEngineRestart();
static void WxFinishInstall(const std::wstring& id, const std::wstring& dir, HRESULT hr, ICoreWebView2BrowserExtension* ext);
static const wchar_t* WxHrName(HRESULT hr);
static void WebBarLayout();
static void WebNewTab(std::wstring url, bool makeCurrent);

static std::wstring WxW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string WxU(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring WxRoot() {
    PWSTR la = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);
    std::wstring r = (la ? std::wstring(la) : std::wstring(L".")) + L"\\Docsy\\Extensions";
    if (la) CoTaskMemFree(la);
    return r;
}
static void WxMkdirs(const std::wstring& p) {
    for (size_t i = 3; i <= p.size(); i++)
        if (i == p.size() || p[i] == L'\\') CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
}
static void WxRmTree(const std::wstring& p) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring c = p + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(c.c_str());
                else WxRmTree(c);
            } else {
                SetFileAttributesW(c.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(c.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(p.c_str());
}
// ---- install diagnostics. every step that can refuse an extension writes its
// reason here, to %LOCALAPPDATA%\Docsy\webext.log - a store-pill tooltip can
// never carry an HRESULT, and "it always fails" needs numbers to be fixed ----
static void WxLogf(const wchar_t* fmt, ...) {
    static bool dead = false;                       // the log is a luxury, never a roadblock
    if (dead) return;
    wchar_t msg[600];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[760];
    _snwprintf_s(line, _TRUNCATE, L"[%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    PWSTR la = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);
    std::wstring p = (la ? std::wstring(la) : std::wstring(L".")) + L"\\Docsy\\webext.log";
    if (la) CoTaskMemFree(la);
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { dead = true; return; }
    DWORD wrote = 0;
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &wrote, nullptr);
    CloseHandle(h);
    WIN32_FILE_ATTRIBUTE_DATA fa{};                 // over half a meg: the older half moves aside
    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) &&
        (((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) > (512ll << 10))
        MoveFileExW(p.c_str(), (p + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
}
// ---- the machine's own overrides. four WEBVIEW2_* environment variables beat
// every option this app passes (documented loader behavior): an inherited
// WEBVIEW2_USER_DATA_FOLDER silently points the engine at a foreign profile and
// WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS appends foreign switches - either reads
// exactly like "every extension install fails with ERROR_INVALID_STATE". They
// are reported once, then cleared: docsy owns its own engine configuration. ----
static void WxCleanWebViewEnvVars() {
    static const wchar_t* const names[] = {
        L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"WEBVIEW2_USER_DATA_FOLDER",
        L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", L"WEBVIEW2_RELEASE_CHANNEL_PREFERENCE" };
    for (const wchar_t* n : names) {
        DWORD need = GetEnvironmentVariableW(n, nullptr, 0);
        if (!need) {
            if (GetLastError() != ERROR_ENVVAR_NOT_FOUND)
                WxLogf(L"env: %s is set but empty - clearing it", n), SetEnvironmentVariableW(n, nullptr);
            continue;
        }
        std::wstring v(need, L'\0');
        need = GetEnvironmentVariableW(n, &v[0], need);
        v.resize(need);
        WxLogf(L"env: %s = '%s' was inherited - cleared (it overrides the app's own options)", n, v.c_str());
        SetEnvironmentVariableW(n, nullptr);
    }
}
// a second docsy.exe builds a second engine on the SAME profile: whichever
// process launched the browser process first decides its extension state, so
// an old window left running reads as "installs always fail" in the new one
static void WxLogOtherDocsy() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof pe };
    DWORD me = GetCurrentProcessId();
    if (Process32FirstW(s, &pe)) do {
        if (!_wcsicmp(pe.szExeFile, L"docsy.exe") && pe.th32ProcessID != me)
            WxLogf(L"env: another docsy.exe (pid %lu) is running - its engine may own the profile",
                   (unsigned long)pe.th32ProcessID);
    } while (Process32NextW(s, &pe));
    CloseHandle(s);
}
// machine-wide Edge/WebView2 policies are the third voice that can refuse every
// install before our options are ever read; only the interesting names are kept
static void WxLogPolicyKey(HKEY root, const wchar_t* rootName, const wchar_t* sub) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DWORD vals = 0, mx = 0;
    wchar_t nm[192]; DWORD nsz = 192;
    if (RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &vals, &mx, nullptr, nullptr, nullptr) == ERROR_SUCCESS && vals && mx < 192)
        for (DWORD i = 0; i < vals; i++) {
            nsz = 192;
            if (RegEnumValueW(k, i, nm, &nsz, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
                (wcsstr(nm, L"Extension") || wcsstr(nm, L"Experiment")))
                WxLogf(L"policy: %s\\%s\\%s is set", rootName, sub, nm);
        }
    RegCloseKey(k);
}
static void WxLogPolicies() {
    static const wchar_t* const subs[] = {
        L"Software\\Policies\\Microsoft\\Edge\\WebView2", L"Software\\Policies\\Microsoft\\Edge" };
    for (const wchar_t* s : subs) {
        WxLogPolicyKey(HKEY_LOCAL_MACHINE, L"HKLM", s);
        WxLogPolicyKey(HKEY_CURRENT_USER, L"HKCU", s);
    }
}
static bool WxReadFile(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (64ll << 20);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = sz.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart);
    }
    CloseHandle(h);
    return ok;
}
static bool WxWriteFile(const std::wstring& p, const unsigned char* d, size_t n) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true; size_t done = 0;
    while (ok && done < n) {
        DWORD w = 0, chunk = (DWORD)(n - done > (1u << 24) ? (1u << 24) : n - done);
        ok = WriteFile(h, d + done, chunk, &w, nullptr) && w > 0;
        done += w;
    }
    CloseHandle(h);
    return ok;
}
static bool WxSafeName(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos || n.find('\\') != std::string::npos) return false;
    size_t p = 0;
    while (p <= n.size()) {
        size_t e = n.find('/', p); if (e == std::string::npos) e = n.size();
        if (n.compare(p, e - p, "..") == 0) return false;
        p = e + 1;
    }
    return true;
}
static bool WxIsExtId(const std::wstring& s) {
    if (s.size() != 32) return false;
    for (wchar_t c : s) if (c < L'a' || c > L'p') return false;
    return true;
}
static bool WxStoreHost(const std::wstring& url, std::wstring* pathOut) {
    size_t p = url.find(L"://"); if (p == std::wstring::npos) return false;
    std::wstring rest = url.substr(p + 3);
    size_t sl = rest.find(L'/'); if (sl == std::wstring::npos) return false;
    std::wstring host = rest.substr(0, sl), path = rest.substr(sl);
    bool store = host == L"chromewebstore.google.com" || (host == L"chrome.google.com" && path.rfind(L"/webstore", 0) == 0);
    if (store && pathOut) *pathOut = path;
    return store;
}
static std::wstring WxStoreIdOf(const std::wstring& url) {   // the extension a store detail page is about
    std::wstring path;
    if (!WxStoreHost(url, &path)) return L"";
    size_t q = path.find_first_of(L"?#"); if (q != std::wstring::npos) path.resize(q);
    size_t pos = 0;
    while (pos < path.size()) {
        size_t e = path.find(L'/', pos); if (e == std::wstring::npos) e = path.size();
        std::wstring seg = path.substr(pos, e - pos);
        if (WxIsExtId(seg)) return seg;
        pos = e + 1;
    }
    return L"";
}

// ---- manifest -> name, popup, options page, toolbar icon ----
static std::wstring WxMsg(const std::wstring& dir, const crxk::J& man, const std::string& val) {
    if (val.size() > 8 && val.compare(0, 6, "__MSG_") == 0 && val.compare(val.size() - 2, 2, "__") == 0) {
        std::string key = val.substr(6, val.size() - 8), loc;
        if (const crxk::J* d = man.get("default_locale")) loc = d->str();
        const std::string cands[4] = { loc, "en", "en_US", "en_GB" };
        for (auto& c : cands) {
            if (c.empty()) continue;
            std::string txt; crxk::J m;
            if (!WxReadFile(dir + L"\\_locales\\" + WxW(c) + L"\\messages.json", txt) || !crxk::JParse(txt, m)) continue;
            for (auto& kv : m.o) {
                if (kv.first.size() != key.size()) continue;
                bool same = true;
                for (size_t i = 0; i < key.size(); i++) if (tolower((unsigned char)kv.first[i]) != tolower((unsigned char)key[i])) same = false;
                if (!same) continue;
                if (const crxk::J* mm = kv.second.get("message")) if (!mm->str().empty()) return WxW(mm->str());
            }
        }
    }
    return WxW(val);
}
static std::wstring WxPickIcon(const crxk::J* v) {          // a path, or a size->path map: the best for ~18dp
    if (!v) return L"";
    if (v->t == 's') return WxW(v->s);
    if (v->t != 'o') return L"";
    std::string best; int bestSz = 0; bool haveBig = false;
    for (auto& kv : v->o) {
        if (kv.second.t != 's') continue;
        int sz = atoi(kv.first.c_str());
        int want = (int)(18 * g.dpi + 0.5);
        bool big = sz >= want;
        if (best.empty() || (big && (!haveBig || sz < bestSz)) || (!big && !haveBig && sz > bestSz)) {
            best = kv.second.s; bestSz = sz; haveBig = big;
        }
    }
    return WxW(best);
}
static Gdiplus::Bitmap* WxLoadIcon(const std::wstring& path) {   // decoded into memory: no file lock left behind
    Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromFile(path.c_str());
    if (!src) return nullptr;
    if (src->GetLastStatus() != Gdiplus::Ok || src->GetWidth() == 0 || src->GetHeight() == 0) { delete src; return nullptr; }
    UINT w = src->GetWidth(), h = src->GetHeight();
    Gdiplus::Bitmap* c = new Gdiplus::Bitmap((INT)w, (INT)h, PixelFormat32bppPARGB);
    { Gdiplus::Graphics gs(c); gs.SetCompositingMode(Gdiplus::CompositingModeSourceCopy); gs.DrawImage(src, 0, 0, (INT)w, (INT)h); }
    delete src;
    return c;
}
static bool WxLoadInfo(WebExt& e) {
    std::string txt; crxk::J m;
    if (!WxReadFile(e.dir + L"\\manifest.json", txt) || !crxk::JParse(txt, m) || m.t != 'o') return false;
    if (const crxk::J* n = m.get("name")) e.name = WxMsg(e.dir, m, n->str());
    if (e.name.empty()) e.name = e.id;
    if (const crxk::J* v = m.get("version")) e.version = WxW(v->str());
    const crxk::J* act = m.get("action"); if (!act) act = m.get("browser_action");
    e.popup.clear(); e.options.clear();
    if (act) if (const crxk::J* p = act->get("default_popup")) e.popup = WxW(p->str());
    if (const crxk::J* ou = m.get("options_ui")) { if (const crxk::J* p = ou->get("page")) e.options = WxW(p->str()); }
    if (e.options.empty()) if (const crxk::J* op = m.get("options_page")) e.options = WxW(op->str());
    std::wstring ip = act ? WxPickIcon(act->get("default_icon")) : L"";
    if (ip.empty()) ip = WxPickIcon(m.get("icons"));
    if (e.icon) { delete e.icon; e.icon = nullptr; }
    if (!ip.empty()) {
        while (!ip.empty() && ip[0] == L'/') ip.erase(0, 1);
        for (auto& c : ip) if (c == L'/') c = L'\\';
        if (ip.find(L"..") == std::wstring::npos) e.icon = WxLoadIcon(e.dir + L"\\" + ip);
    }
    return true;
}
static std::wstring WxExtUrl(const WebExt& e, const std::wstring& page) {
    std::wstring p = page;
    while (!p.empty() && p[0] == L'/') p.erase(0, 1);
    return L"chrome-extension://" + e.id + L"/" + p;
}
static int WxFind(const std::wstring& id) {
    for (int i = 0; i < (int)g_wx.size(); i++) if (g_wx[i].id == id || (!g_wx[i].storeId.empty() && g_wx[i].storeId == id)) return i;
    return -1;
}

// ---- the profile, the existing extensions, loading before the first page ----
static wrl::ComPtr<ICoreWebView2Profile7> WxProfile() {
    if (g_wxProf) return g_wxProf;
    for (auto& t : g_wt) {
        if (!t.web) continue;
        wrl::ComPtr<ICoreWebView2_13> w13; wrl::ComPtr<ICoreWebView2Profile> p;
        if (SUCCEEDED(t.web.As(&w13)) && w13 && SUCCEEDED(w13->get_Profile(&p)) && p) {
            wrl::ComPtr<ICoreWebView2Profile7> p7;
            if (SUCCEEDED(p.As(&p7)) && p7) { g_wxProf = p7; break; }
        }
    }
    if (!g_wxProf) WxLogf(L"profile: no ICoreWebView2Profile7 on any tab - the runtime has no extension API");
    return g_wxProf;
}
static void WxAddFolder(const std::wstring& dir, std::function<void(HRESULT, ICoreWebView2BrowserExtension*)> done) {
    wrl::ComPtr<ICoreWebView2Profile7> p = WxProfile();
    if (!p) { WxLogf(L"add: no profile for %s", dir.c_str()); done(E_NOINTERFACE, nullptr); return; }
    HRESULT hr = p->AddBrowserExtension(dir.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
            [done](HRESULT r, ICoreWebView2BrowserExtension* ext) -> HRESULT { done(r, ext); return S_OK; }).Get());
    if (FAILED(hr)) { WxLogf(L"add: %s refused synchronously 0x%08lX", dir.c_str(), (unsigned long)hr); done(hr, nullptr); }
}
// a registration left by an earlier attempt (the profile remembers extensions across runs, and a
// failed install can leave one behind) makes AddBrowserExtension answer ERROR_INVALID_STATE: so any
// extension already registered under this id is removed first, then the folder is added fresh
static void WxAddFolderFresh(const std::wstring& dir, const std::wstring& id,
                             std::function<void(HRESULT, ICoreWebView2BrowserExtension*)> done) {
    wrl::ComPtr<ICoreWebView2Profile7> p = WxProfile();
    if (!p) { done(E_NOINTERFACE, nullptr); return; }
    HRESULT hr = p->GetBrowserExtensions(
        Microsoft::WRL::Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [dir, id, done](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                auto stale = std::make_shared<std::vector<wrl::ComPtr<ICoreWebView2BrowserExtension>>>();
                UINT n = 0;
                if (SUCCEEDED(hr) && list && SUCCEEDED(list->get_Count(&n)))
                    for (UINT i = 0; i < n; i++) {
                        wrl::ComPtr<ICoreWebView2BrowserExtension> e;
                        if (FAILED(list->GetValueAtIndex(i, &e)) || !e) continue;
                        LPWSTR rid = nullptr;
                        if (SUCCEEDED(e->get_Id(&rid)) && rid) {
                            if (id == rid) stale->push_back(e);
                            CoTaskMemFree(rid);
                        }
                    }
                if (stale->empty()) { WxLogf(L"add: no stale registration under %s", id.c_str()); WxAddFolder(dir, done); return S_OK; }
                WxLogf(L"add: %u stale registration(s) under %s - removing before the fresh add", (unsigned)stale->size(), id.c_str());
                auto left = std::make_shared<int>((int)stale->size());
                for (auto& e : *stale) {
                    HRESULT rh = e->Remove(
                        Microsoft::WRL::Callback<ICoreWebView2BrowserExtensionRemoveCompletedHandler>(
                            [left, stale, dir, done](HRESULT) -> HRESULT {
                                if (--*left == 0) WxAddFolder(dir, done);
                                return S_OK;
                            }).Get());
                    if (FAILED(rh) && --*left == 0) WxAddFolder(dir, done);
                }
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        WxLogf(L"add: pre-list GetBrowserExtensions refused synchronously 0x%08lX %s",
               (unsigned long)hr, WxHrName(hr));
        WxAddFolder(dir, done);
    }
}
static void WxNotifyTabs();
static void WxFinishInit() {
    g_wxReady = true; g_wxBusy = false;
    for (auto& t : g_wt)
        if (t.web && !t.pending.empty()) { t.web->Navigate(t.pending.c_str()); t.pending.clear(); }
    if (g_webOn) WebBarLayout();
    WebRepaintChrome();
    if (!g_wxRetryId.empty()) {                    // the engine that refused this install is
        std::wstring dir = g_wxRetryDir, id = g_wxRetryId;   // gone; the fresh one retries it
        g_wxRetryDir.clear(); g_wxRetryId.clear();
        WxAddFolderFresh(dir, id, [id, dir](HRESULT hr, ICoreWebView2BrowserExtension* ext) {
            WxFinishInstall(id, dir, hr, ext);
        });
    }
}
static void WxInit() {
    if (g_wxReady || g_wxBusy) return;
    wrl::ComPtr<ICoreWebView2Profile7> p = WxProfile();
    if (!p) { g_wxOk = false; WxFinishInit(); return; }
    g_wxBusy = true;
    HRESULT hr0 = p->GetBrowserExtensions(
        Microsoft::WRL::Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                g_wxOk = SUCCEEDED(hr);        // the fresh engine's own verdict - a restart
                std::vector<wrl::ComPtr<ICoreWebView2BrowserExtension>> have;   // resets this
                UINT n = 0;
                if (SUCCEEDED(hr) && list && SUCCEEDED(list->get_Count(&n)))
                    for (UINT i = 0; i < n; i++) {
                        wrl::ComPtr<ICoreWebView2BrowserExtension> e;
                        if (SUCCEEDED(list->GetValueAtIndex(i, &e)) && e) have.push_back(e);
                    }
                // our folders are the source of truth for names, icons and popups
                std::wstring root = WxRoot();
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                        WebExt e; e.id = fd.cFileName; e.storeId = e.id; e.dir = root + L"\\" + fd.cFileName;
                        if (WxLoadInfo(e)) g_wx.push_back(std::move(e));
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
                auto left = std::make_shared<int>(0);
                std::vector<int> toAdd;
                for (int i = 0; i < (int)g_wx.size(); i++) {
                    bool found = false;
                    for (auto& e : have) {
                        LPWSTR rid = nullptr;
                        if (SUCCEEDED(e->get_Id(&rid)) && rid) {
                            if (g_wx[i].id == rid) {
                                g_wx[i].ext = e; found = true;
                                BOOL en = TRUE; if (SUCCEEDED(e->get_IsEnabled(&en))) g_wx[i].enabled = en != FALSE;
                            }
                            CoTaskMemFree(rid);
                        }
                    }
                    if (!found) toAdd.push_back(i);
                }
                WxLogf(L"init: GetBrowserExtensions answered 0x%08lX, %u extension(s) registered",
                       (unsigned long)hr, (unsigned)have.size());
                if (FAILED(hr)) WxLogf(L"init: the listing itself failed - the engine has no working extension subsystem");
                WxLogf(L"init: %u installed, %zu folder(s), %zu to re-add", (unsigned)have.size(), g_wx.size(), toAdd.size());
                if (toAdd.empty()) { WxFinishInit(); return S_OK; }
                *left = (int)toAdd.size();
                for (int i : toAdd) {
                    std::wstring id = g_wx[i].id, dir = g_wx[i].dir;
                    WxAddFolder(dir, [id, left](HRESULT r, ICoreWebView2BrowserExtension* ext) {
                        WxLogf(L"init: re-add %s -> 0x%08lX", id.c_str(), (unsigned long)r);
                        int k = WxFind(id);
                        if (k >= 0) {
                            if (SUCCEEDED(r) && ext) {
                                g_wx[k].ext = ext;
                                LPWSTR rid = nullptr;
                                if (SUCCEEDED(ext->get_Id(&rid)) && rid) { g_wx[k].id = rid; CoTaskMemFree(rid); }
                            } else g_wx[k].enabled = false;     // it would not load: keep it listed, off
                        }
                        if (--*left == 0) WxFinishInit();
                    });
                }
                return S_OK;
            }).Get());
    if (FAILED(hr0)) {
        WxLogf(L"init: GetBrowserExtensions refused synchronously 0x%08lX %s",
               (unsigned long)hr0, WxHrName(hr0));
        g_wxBusy = false; g_wxOk = false;
        if (hr0 == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) && !g_wxRestarted)
            WxEngineRestart();                     // the whole engine refused: relaunch it once
        else WxFinishInit();
        return;
    }
}

// the engine cold-restart: release every controller and the environment, let the
// browser process exit, then rebuild it all on the same profile. The old engine's
// extension subsystem died with whatever it was born into - inherited switches, a
// profile engine owned by an older instance, a bad experiment payload - and only
// a fresh launch re-reads any of it. The first controller re-runs WxInit;
// WxFinishInit replays an install the old engine refused. Once per session.
static void CALLBACK WxEngineRestartTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    if (id != g_wxRestartTimer) return;
    g_wxRestartTimer = 0;
    WxLogf(L"env: the old engine is gone - building the new one");
    WebEnsureEnv();                        // its completion builds the controllers again
}
static void WxEngineRestart() {
    if (g_wxRestarted) return;
    g_wxRestarted = true;
    WxLogf(L"env: engine restart - releasing every controller and the environment");
    g_wxProf.Reset();
    g_wxReady = false; g_wxBusy = false;
    for (auto& e : g_wx) if (e.icon) { delete e.icon; e.icon = nullptr; }
    g_wx.clear();                          // WxInit rebuilds the list from the folders
    for (auto& t : g_wt) {
        if (t.ctr) t.ctr->Close();
        t.ctr.Reset(); t.web.Reset();
        if (t.pending.empty() && !t.url.empty()) t.pending = t.url;   // every live tab re-opens
        t.capturing = false; t.capturePending = false; t.captureDue = 0;
    }
    g_wvEnv.Reset();                       // the last host let go: the browser process exits
    UINT_PTR t = SetTimer(nullptr, 0, 1000, WxEngineRestartTimer);   // ...then the fresh one builds
    if (t) g_wxRestartTimer = t;
    else WebEnsureEnv();                   // no timer: rebuild immediately, best effort
}

// ---- installing from the Chrome Web Store ----
struct WxJob { std::wstring id, dir, err; bool ok = false; };
static bool WxHttpGet(const std::wstring& url, const std::wstring& ua, std::vector<unsigned char>& out, std::wstring& err) {
    wchar_t host[256] = L"", path[2048] = L"", extra[4096] = L"";
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = L"Bad download address."; return false; }
    HINTERNET s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = L"No network session."; return false; }
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = nullptr;
    bool ok = false;
    if (c) {
        std::wstring full = std::wstring(path) + extra;
        r = WinHttpOpenRequest(c, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    }
    if (r) {
        DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof redir);
        WinHttpSetTimeouts(r, 10000, 10000, 30000, 60000);
        if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            WxLogf(L"dl: HTTP %u", code);
            if (code == 200) {
                ok = true;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; break; }
                    if (avail == 0) break;
                    size_t at = out.size();
                    if (at + avail > (300u << 20)) { ok = false; break; }
                    out.resize(at + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(r, out.data() + at, avail, &got)) { ok = false; break; }
                    out.resize(at + got);
                }
                if (!ok) err = L"The download was interrupted.";
            } else err = code == 204 || code == 404 ? L"The Chrome Web Store has no download for this extension."
                                                    : L"The Chrome Web Store answered " + std::to_wstring(code) + L".";
        } else err = L"Could not reach the Chrome Web Store.";
    } else err = L"Could not reach the Chrome Web Store.";
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
static std::wstring WxCleanVer(const std::wstring& v) {     // "131.0.2903.112 beta" -> "131.0.2903.112"
    std::wstring r;
    for (wchar_t c : v) { if ((c >= L'0' && c <= L'9') || c == L'.') r += c; else break; }
    int dots = 0; for (wchar_t c : r) if (c == L'.') dots++;
    if (dots != 3 || r.front() == L'.' || r.back() == L'.') return L"131.0.0.0";
    return r;
}
static void WxFetch(WxJob& j, const std::wstring& rawVer, const std::wstring& root) {   // worker thread
    std::wstring ver = WxCleanVer(rawVer);
    WxLogf(L"fetch: id %s, prodver %s", j.id.c_str(), ver.c_str());
    std::vector<unsigned char> d;
    // the update service answers by the Chrome version it is told: the runtime's own first, then a
    // plain stable one - a 204/403 for one is often a 200 for the other
    const std::wstring tries[2] = { ver, L"131.0.0.0" };
    for (int a = 0; a < 2; a++) {
        if (a == 1 && tries[1] == tries[0]) break;
        std::wstring url = L"https://clients2.google.com/service/update2/crx?response=redirect&os=win&arch=x64&os_arch=x86_64"
                           L"&nacl_arch=x86-64&prod=chromecrx&prodchannel=stable&prodversion=" + tries[a] +
                           L"&lang=en-US&acceptformat=crx3&x=id%3D" + j.id + L"%26installsource%3Dondemand%26uc";
        std::wstring ua = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" + tries[a] + L" Safari/537.36";
        d.clear(); j.err.clear();
        if (WxHttpGet(url, ua, d, j.err) && d.size() >= 64) break;
        if (j.err.empty()) j.err = L"The Chrome Web Store has no download for this extension.";
        WxLogf(L"fetch: prodver %s refused: %s", tries[a].c_str(), j.err.c_str());
        d.clear();
    }
    if (d.empty()) { WxLogf(L"fetch: every version refused"); return; }
    if (d.size() < 64) { j.err = L"The Chrome Web Store has no download for this extension."; return; }
    size_t zo = 0; std::string key;
    if (!crxk::CrxParse(d.data(), d.size(), zo, key)) { WxLogf(L"fetch: not a crx (%zu bytes)", d.size()); j.err = L"That is not a valid extension package."; return; }
    std::vector<crxk::ZipEnt> zl;
    if (!crxk::ZipList(d.data() + zo, d.size() - zo, zl)) { j.err = L"The extension package is damaged."; return; }
    std::wstring dir = root + L"\\" + j.id;
    WxRmTree(dir); WxMkdirs(dir);
    bool man = false;
    for (auto& z : zl) {
        if (z.name.empty() || z.name.back() == '/') continue;
        if (!WxSafeName(z.name) || z.name.compare(0, 10, "_metadata/") == 0) continue;
        std::vector<unsigned char> body;
        if (!crxk::ZipRead(d.data() + zo, d.size() - zo, z, body)) { j.err = L"The extension package is damaged."; WxRmTree(dir); return; }
        std::wstring rel = WxW(z.name);
        for (auto& c : rel) if (c == L'/') c = L'\\';
        std::wstring full = dir + L"\\" + rel;
        WxMkdirs(full.substr(0, full.find_last_of(L'\\')));
        if (z.name == "manifest.json") {
            std::string t((const char*)body.data(), body.size());
            t = crxk::ManifestWithKey(t, key);                     // the store id survives the unpack
            body.assign(t.begin(), t.end()); man = true;
        }
        if (!WxWriteFile(full, body.data(), body.size())) { j.err = L"Could not write the extension to disk."; WxRmTree(dir); return; }
    }
    if (!man) { j.err = L"The package has no manifest."; WxRmTree(dir); return; }
    j.dir = dir; j.ok = true;
    WxLogf(L"fetch: unpacked %zu entries -> %s", zl.size(), dir.c_str());
}
static std::wstring WxJsStr(const std::wstring& s) {
    std::wstring r = L"\"";
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { r += L'\\'; r += c; }
        else if (c == L'\n' || c == L'\r') r += L' ';
        else r += c;
    }
    return r + L"\"";
}
static void WxPushState(ICoreWebView2* w) {             // the store button's state: which ids are installed
    if (!w) return;
    std::wstring js = L"window.__docsySet&&window.__docsySet([";
    bool first = true;
    for (size_t i = 0; i < g_wx.size(); i++) {          // the id WebView2 reports and the id the store knows;
        if (!g_wx[i].ext) continue;                     // one whose load failed stays off the pill, so it can be retried
        if (!first) js += L","; first = false; js += WxJsStr(g_wx[i].id);
        if (!g_wx[i].storeId.empty() && g_wx[i].storeId != g_wx[i].id) js += L"," + WxJsStr(g_wx[i].storeId);
    }
    js += L"])";
    w->ExecuteScript(js.c_str(), nullptr);
}
static void WxNotifyTabs() { for (auto& t : g_wt) if (t.web) WxPushState(t.web.Get()); }
static void WxNotifyFail(const std::wstring& msg) {
    std::wstring js = L"window.__docsyFail&&window.__docsyFail(" + WxJsStr(msg) + L")";
    bool storeOpen = false;
    for (auto& t : g_wt) {
        if (!t.web) continue;
        if (WxStoreHost(t.url, nullptr)) storeOpen = true;    // the page's own toast carries it
        t.web->ExecuteScript(js.c_str(), nullptr);
    }
    if (!storeOpen)                                           // an install started from the
        MessageBoxW(g.main, msg.c_str(), L"Docsy - extension install failed",  // menu or a
                    MB_OK | MB_ICONWARNING);                  // CRX download: say it natively
}
static void WxInstall(const std::wstring& id) {
    if (!WxIsExtId(id)) return;
    if (!g_wxOk) {
        WxNotifyFail(L"This WebView2 runtime is too old for extensions.");
        MessageBoxW(g.main, L"Extensions need a newer WebView2 runtime. Update Microsoft Edge / WebView2 and try again.",
                    L"Docsy", MB_OK | MB_ICONINFORMATION);
        return;
    }
    int have = WxFind(id);
    if (have >= 0 && g_wx[have].ext) { WxNotifyTabs(); return; }   // in and alive - the pill already knows
    // (a listed entry whose load failed once carries no ICoreWebView2BrowserExtension:
    //  it falls through here, gets reinstalled and WxAddFolderFresh clears the stale state)
    for (auto& s : g_wxInstalling) if (s == id) return;
    WxLogf(L"install: begin %s", id.c_str());
    g_wxInstalling.push_back(id);
    std::wstring ver = g_webVer.empty() ? std::wstring(L"131.0.0.0") : g_webVer;
    std::wstring root = WxRoot();
    HWND main = g.main;
    std::thread([id, ver, root, main]() {
        WxJob* j = new WxJob; j->id = id;
        WxFetch(*j, ver, root);
        if (!PostMessageW(main, WM_APP + 5, 0, (LPARAM)j)) delete j;
    }).detach();
}
// the shared tail of every install road (a store click, a .crx download, the
// self-test, a retry after an engine restart): the add answered - land it
static void WxFinishInstall(const std::wstring& id, const std::wstring& dir, HRESULT hr, ICoreWebView2BrowserExtension* ext) {
    WxLogf(L"install: AddBrowserExtension -> 0x%08lX", (unsigned long)hr);
    if (FAILED(hr) || !ext) {                          // the reason travels with the failure:
        if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) && !g_wxRestarted) {
            // the engine refused profile extension ops outright: every cause this can
            // have lives in the browser process, so the fix is a new one - the unpacked
            // folder stays, and WxFinishInit replays the add on the fresh engine
            WxLogf(L"install: INVALID_STATE - scheduling the one-shot engine restart; the install replays itself");
            g_wxRetryDir = dir; g_wxRetryId = id;
            // deferred one pump turn: this can be running inside a WebView2 completion
            // frame, and the engine must not be released re-entrantly - the message
            // unwinds every frame before WxEngineRestart touches anything
            PostMessageW(g.main, WM_APP + 7, 0, 0);
            return;
        }
        wchar_t why[512];                              // the toast spells out which one it hit
        if (hr == E_NOINTERFACE)
            swprintf_s(why, L"This WebView2 runtime has no extension API - update Microsoft Edge (WebView2 Runtime) and restart Docsy.");
        else if (hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))
            swprintf_s(why, L"The engine came up with extensions off. Restart Docsy once; if it still fails, the WebView2 runtime or a machine policy is blocking extensions (0x%08lX).",
                       (unsigned long)hr);
        else if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE))
            swprintf_s(why, L"The engine refused extension operations even on a fresh browser process (0x%08lX). A machine policy, an inherited WEBVIEW2_* environment variable or this runtime build is the cause - Extensions menu > Copy diagnostics carries the trail.",
                       (unsigned long)hr);
        else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            swprintf_s(why, L"The extension unpacked into a folder WebView2 rejects (0x%08lX). Try again.",
                       (unsigned long)hr);
        else if (hr == E_ACCESSDENIED)
            swprintf_s(why, L"The package carries a reserved system file name (0x%08lX).",
                       (unsigned long)hr);
        else
            swprintf_s(why, L"WebView2 refused this extension (0x%08lX). Extensions menu > Copy diagnostics carries the full trail, and its test extensions separate a Manifest V2 cutoff from a broken install.",
                       (unsigned long)hr);
        WxNotifyFail(why);
        WxRmTree(dir);
        return;
    }
    int old = WxFind(id);
    if (old >= 0) { if (g_wx[old].icon) delete g_wx[old].icon; g_wx.erase(g_wx.begin() + old); }
    WebExt e; e.id = id; e.storeId = id; e.dir = dir; e.ext = ext;
    LPWSTR rid = nullptr;
    if (SUCCEEDED(ext->get_Id(&rid)) && rid) { e.id = rid; CoTaskMemFree(rid); }
    BOOL en = TRUE; if (SUCCEEDED(ext->get_IsEnabled(&en))) e.enabled = en != FALSE;
    WxLoadInfo(e);
    g_wx.push_back(std::move(e));
    std::sort(g_wx.begin(), g_wx.end(), [](const WebExt& a, const WebExt& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    if (g_webOn) WebBarLayout();
    WebRepaintChrome();
    WxNotifyTabs();
}
static void WxInstallDone(WxJob* job) {                  // WM_APP + 5, on the UI thread
    std::unique_ptr<WxJob> j(job);
    for (size_t i = 0; i < g_wxInstalling.size(); i++) if (g_wxInstalling[i] == j->id) { g_wxInstalling.erase(g_wxInstalling.begin() + i); break; }
    if (!j->ok) { WxLogf(L"install: failed before load: %s", j->err.c_str()); WxNotifyFail(j->err); return; }
    std::wstring id = j->id, dir = j->dir;
    WxAddFolderFresh(dir, id, [id, dir](HRESULT hr, ICoreWebView2BrowserExtension* ext) {
        WxFinishInstall(id, dir, hr, ext);
    });
}
static void WxRemove(const std::wstring& id) {
    int i = WxFind(id); if (i < 0) return;
    wrl::ComPtr<ICoreWebView2BrowserExtension> ext = g_wx[i].ext;
    auto fin = [id]() {
        int k = WxFind(id); if (k < 0) return;
        std::wstring dir = g_wx[k].dir;
        if (g_wx[k].icon) { delete g_wx[k].icon; g_wx[k].icon = nullptr; }
        g_wx.erase(g_wx.begin() + k);
        WxRmTree(dir);
        if (g_webOn) WebBarLayout();
        WebRepaintChrome();
        WxNotifyTabs();
    };
    if (!ext) { fin(); return; }
    HRESULT hr = ext->Remove(Microsoft::WRL::Callback<ICoreWebView2BrowserExtensionRemoveCompletedHandler>(
        [fin](HRESULT) -> HRESULT { fin(); return S_OK; }).Get());
    if (FAILED(hr)) fin();
}
static void WxSetEnabled(const std::wstring& id, bool on) {
    int i = WxFind(id); if (i < 0 || !g_wx[i].ext) return;
    g_wx[i].ext->Enable(on ? TRUE : FALSE, Microsoft::WRL::Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
        [id, on](HRESULT hr) -> HRESULT {
            int k = WxFind(id);
            if (SUCCEEDED(hr) && k >= 0) { g_wx[k].enabled = on; if (g_webOn) WebBarLayout(); WebRepaintChrome(); }
            return S_OK;
        }).Get());
}

// the store page's floating button: the store's own insists on Chrome, so this one sits over it
static const wchar_t* const kWxStoreScript = LR"JS((function(){
  var h=location.hostname;
  if(window.top!==window)return;
  if(!(h==='chromewebstore.google.com'||(h==='chrome.google.com'&&location.pathname.indexOf('/webstore')===0)))return;
  var have=[],btn=null,busy=null,failed=false,why='';
  function cur(){var m=location.pathname.match(/\/detail\/(?:[^\/]+\/)?([a-p]{32})(?:[\/?#]|$)/);return m?m[1]:null;}
  function paint(){
    var id=cur();
    if(!id){if(btn){btn.remove();btn=null;}return;}
    if(!btn){
      btn=document.createElement('button');btn.type='button';
      btn.style.cssText='position:fixed;top:76px;right:28px;z-index:2147483647;padding:11px 20px;border:0;border-radius:999px;font:600 14px/1 "Segoe UI",system-ui,sans-serif;color:#fff;box-shadow:0 4px 16px rgba(0,0,0,.35);';
      btn.addEventListener('click',function(){
        var i=cur();if(!i||have.indexOf(i)>=0||busy)return;
        failed=false;busy=i;paint();
        try{window.chrome.webview.postMessage('docsy-install:'+i);}catch(e){busy=null;failed=true;paint();}
      });
      (document.body||document.documentElement).appendChild(btn);
    }
    var done=have.indexOf(id)>=0,work=busy===id;
    btn.textContent=done?'Added to Docsy \u2713':work?'Adding\u2026':failed?('Couldn\u2019t add'+(why?': '+(why.length>90?why.slice(0,87)+'\u2026':why):'')+' \u2014 click to retry'):'Add to Docsy';
    btn.title=failed?why:'';btn.style.maxWidth='420px';btn.style.textAlign='left';btn.style.lineHeight='1.3';
    btn.style.background=done?'#3a3a3c':failed?'#c0392b':'#22bb56';
    btn.style.cursor=(done||work)?'default':'pointer';
  }
  function toast(m){
    var t=document.getElementById('docsy-note');
    if(!t){t=document.createElement('div');t.id='docsy-note';
      t.style.cssText='position:fixed;left:50%;bottom:30px;transform:translateX(-50%);z-index:2147483647;padding:12px 18px;border-radius:12px;background:#1c1c1c;color:#eee;font:500 13px/1.45 "Segoe UI",system-ui,sans-serif;box-shadow:0 6px 24px rgba(0,0,0,.45);max-width:540px;opacity:0;transition:opacity .18s;';
      (document.body||document.documentElement).appendChild(t);}
    t.textContent=String(m);t.style.opacity='1';
    clearTimeout(toast._h);toast._h=setTimeout(function(){t.style.opacity='0';},8000);
  }
  window.__docsySet=function(l){var n=(have||[]).length;have=l||[];busy=null;failed=false;why='';
    if(have.length>n){var i2=cur();if(i2&&have.indexOf(i2)>=0)toast('Added to Docsy');}
    paint();};
  window.__docsyFail=function(m){busy=null;failed=true;why=m?String(m):'';if(m)toast(String(m));paint();};
  setInterval(paint,500);
  document.addEventListener('DOMContentLoaded',paint);
  // the store's own "Add to Chrome" dead-ends here (no chrome.webstore in a
  // webview) - a capture-phase click on it installs through docsy instead,
  // so the muscle-memory button does the right thing
  document.addEventListener('click',function(ev){
    var p=ev.target,txt='';
    for(var k=0;k<5&&p;k++,p=p.parentElement){
      txt=((p.textContent||'')+'').trim();
      if(txt.length>60)break;
      var on=p.getAttribute&&(p.getAttribute('aria-label')||'')+'';
      if(/^add to (chrome|edge|brave|firefox|browser)$/i.test(txt)||/^(zu chrome hinzuf\u00fcgen|a\u00f1adir a chrome|ajouter \u00e0 chrome|adicionar ao ?chrome|aggiungi a chrome|chrome\u306b\u8ffd\u52a0|添加至 ?chrome|添加到 ?chrome|chrome에 추가|установить в chrome)$/i.test(txt.trim())||/^add to (chrome|edge|brave|firefox|browser)$/i.test(on)){
        ev.preventDefault();ev.stopPropagation();
        var i=cur();if(!i||busy)return;
        if(have.indexOf(i)>=0)return;
        failed=false;busy=i;paint();
        try{window.chrome.webview.postMessage('docsy-install:'+i);}catch(e){busy=null;failed=true;paint();}
        return;
      }
    }
  },true);
})();)JS";

// ---- the popup window ----
static struct WxPopState {
    HWND h = nullptr;
    wrl::ComPtr<ICoreWebView2Controller> ctr;
    wrl::ComPtr<ICoreWebView2> web;
    int anchorR = 0, anchorB = 0, cssW = 300, cssH = 100;
    std::wstring extId;
} g_wp;
static std::wstring g_wpLastExt; static ULONGLONG g_wpLastClose = 0;

static void WxPopClose() {
    HWND h = g_wp.h;
    if (!h) return;
    g_wp.h = nullptr;                                   // first: nothing re-enters
    KillTimer(h, 1);
    g_wpLastExt = g_wp.extId; g_wpLastClose = GetTickCount64();
    if (g_wp.ctr) g_wp.ctr->Close();
    g_wp.ctr.Reset(); g_wp.web.Reset();
    DestroyWindow(h);
}
static void WxPopResize(int cw, int ch) {
    if (!g_wp.h) return;
    cw = std::clamp(cw, 25, 800); ch = std::clamp(ch, 25, 600);   // Chrome's own limits
    if (cw == g_wp.cssW && ch == g_wp.cssH) return;
    g_wp.cssW = cw; g_wp.cssH = ch;
    int w = (int)std::lround(cw * g.dpi), hh = (int)std::lround(ch * g.dpi);
    MONITORINFO mi{ sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(g_wp.h, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    int x = std::clamp(g_wp.anchorR - w, (int)wa.left, std::max((int)wa.left, (int)wa.right - w));
    int y = g_wp.anchorB;
    if (y + hh > wa.bottom) hh = std::max(50, (int)wa.bottom - y);
    SetWindowPos(g_wp.h, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
}
static LRESULT CALLBACK WxPopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (g_wp.h == h && g_wp.ctr) { RECT rc; GetClientRect(h, &rc); g_wp.ctr->put_Bounds(rc); }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && g_wp.h == h) PostMessageW(h, WM_CLOSE, 0, 0);   // focus left: it goes, like Chrome's
        return 0;
    case WM_TIMER:
        if (wp == 1 && g_wp.h == h && g_wp.web) {         // the page's size, polled: popups grow as they render
            g_wp.web->ExecuteScript(
                L"(function(){var d=document.documentElement,b=document.body;"
                L"return [Math.ceil(Math.max(d.scrollWidth,b?b.scrollWidth:0)),Math.ceil(Math.max(d.scrollHeight,b?b.scrollHeight:0))];})()",
                Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                    [h](HRESULT hr, LPCWSTR json) -> HRESULT {
                        if (FAILED(hr) || !json || g_wp.h != h) return S_OK;
                        int w = 0, hh = 0;
                        if (swscanf_s(json, L"[%d,%d]", &w, &hh) == 2) WxPopResize(w, hh);
                        return S_OK;
                    }).Get());
        }
        return 0;
    case WM_CLOSE: if (g_wp.h == h) WxPopClose(); else DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void WxOpenPopup(int ei, RECT anchor) {            // anchor: the icon, screen coords
    if (ei < 0 || ei >= (int)g_wx.size() || !g_wvEnv) return;
    const WebExt& e = g_wx[ei];
    if (e.popup.empty()) return;
    if (g_wp.h) { bool same = g_wp.extId == e.id; WxPopClose(); if (same) return; }
    else if (g_wpLastExt == e.id && GetTickCount64() - g_wpLastClose < 400) return;   // the click that dismissed it
    g_wp.extId = e.id;
    g_wp.cssW = 300; g_wp.cssH = 100;
    g_wp.anchorR = anchor.right; g_wp.anchorB = anchor.bottom + (int)(4 * g.dpi);
    int w = (int)std::lround(300 * g.dpi), hh = (int)std::lround(100 * g.dpi);
    MONITORINFO mi{ sizeof mi };
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
    int x = std::clamp(g_wp.anchorR - w, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - w));
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW, L"DocsyExtPopup", L"", WS_POPUP, x, g_wp.anchorB, w, hh, g.main, nullptr, g.hinst, nullptr);
    if (!hw) return;
    g_wp.h = hw;
    ShowWindow(hw, SW_SHOW);
    std::wstring url = WxExtUrl(e, e.popup);
    g_wvEnv->CreateCoreWebView2Controller(hw,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [hw, url](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                if (FAILED(hr) || !c || g_wp.h != hw) { if (c) c->Close(); if (g_wp.h == hw) WxPopClose(); return S_OK; }
                g_wp.ctr = c;
                c->get_CoreWebView2(&g_wp.web);
                RECT rc; GetClientRect(hw, &rc); c->put_Bounds(rc); c->put_IsVisible(TRUE);
                EventRegistrationToken tk;
                g_wp.web->add_NewWindowRequested(                     // a link out of a popup opens as a tab
                    Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [hw](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                            a->put_Handled(TRUE);
                            LPWSTR u = nullptr;
                            if (SUCCEEDED(a->get_Uri(&u)) && u) { std::wstring s = u; CoTaskMemFree(u); WebNewTab(s, true); }
                            PostMessageW(hw, WM_CLOSE, 0, 0);
                            return S_OK;
                        }).Get(), &tk);
                g_wp.web->add_WindowCloseRequested(                   // window.close() in the popup
                    Microsoft::WRL::Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
                        [hw](ICoreWebView2*, IUnknown*) -> HRESULT { PostMessageW(hw, WM_CLOSE, 0, 0); return S_OK; }).Get(), &tk);
                g_wp.web->Navigate(url.c_str());
                SetTimer(hw, 1, 250, nullptr);
                return S_OK;
            }).Get());
}

// ---- toolbar geometry + hits ----
static int WxHit(POINT p) {
    for (size_t k = 0; k < g_wxR.size(); k++)
        if (p.x >= g_wxR[k].left && p.x < g_wxR[k].right && p.y >= g_wxR[k].top && p.y < g_wxR[k].bottom) return (int)k;
    return -1;
}
static void WxOpenPopupFor(int k) {
    if (k < 0 || k >= (int)g_wxR.size() || !g_wstrip) return;
    RECT r = g_wxR[k];
    POINT a{ r.left, r.top }, b{ r.right, r.bottom };
    ClientToScreen(g_wstrip, &a); ClientToScreen(g_wstrip, &b);
    WxOpenPopup(g_wxIdx[k], RECT{ a.x, a.y, b.x, b.y });
}
static void WxOpenOptions(int ei) {
    if (ei < 0 || ei >= (int)g_wx.size() || g_wx[ei].options.empty()) return;
    WebNewTab(WxExtUrl(g_wx[ei], g_wx[ei].options), true);
}

// one command in the menu answers "it always fails": the clipboard carries the
// runtime, what the profile holds, and the log's tail - pasteable straight back
static void WxCopyDiag() {
    std::wstring r = L"Docsy extension diagnostics\r\n============================\r\n";
    r += L"runtime: " + (g_webVer.empty() ? std::wstring(L"(none)") : g_webVer) + L"\r\n";
    r += std::wstring(L"extension system: ") + (g_wxOk ? L"usable" : L"NOT usable on this runtime") + L"\r\n";
    wchar_t nb[32]; swprintf_s(nb, L"%u", (unsigned)g_wx.size());
    r += std::wstring(L"installed: ") + nb + L"\r\n";
    for (auto& e : g_wx)
        r += L"  " + e.name + L"  [" + e.id + (e.enabled ? L"] on" : L"] off") +
             (e.ext ? L"" : L"  NOT LOADED") + L"\r\n";
    r += L"\r\n---- webext.log (tail) ----\r\n";
    std::string raw;
    {   PWSTR la = nullptr; SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);
        if (la) { WxReadFile((std::wstring(la) + L"\\Docsy\\webext.log"), raw); CoTaskMemFree(la); } }
    if (raw.size() > 1) {
        size_t n = raw.size() / 2, from = n > 4000 ? n - 4000 : 0;   // UTF-16LE, tail only
        for (size_t i = from; i < n; i++)
            r += (wchar_t)((unsigned char)raw[i * 2] | ((wchar_t)(unsigned char)raw[i * 2 + 1] << 8));
    }
    if (!OpenClipboard(g.main)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (r.size() + 1) * sizeof(wchar_t));
    if (h) {
        void* p = GlobalLock(h);
        if (p) { memcpy(p, r.c_str(), (r.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
        else GlobalFree(h);
    }
    CloseClipboard();
}

static void WxMenu() {
    if (!g_wstrip) return;
    POINT pt{ g_wbR[WB_EXT].left, g_wbR[WB_EXT].bottom };
    ClientToScreen(g_wstrip, &pt);
    HMENU m = CreatePopupMenu();
    if (!m) return;
    WebTab* t = WebCur();
    std::wstring page = t ? WxStoreIdOf(t->url) : L"";
    if (!page.empty() && WxFind(page) < 0) {
        AppendMenuW(m, MF_STRING, 2, L"Add this extension to Docsy");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    if (g_wx.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No extensions installed");
    for (size_t i = 0; i < g_wx.size(); i++) {
        const WebExt& e = g_wx[i];
        HMENU sub = CreatePopupMenu();
        UINT base = 1000 + (UINT)i * 10;
        if (!e.popup.empty()) AppendMenuW(sub, MF_STRING | (e.enabled ? 0 : MF_GRAYED), base, L"Open");
        if (!e.options.empty()) AppendMenuW(sub, MF_STRING | (e.enabled ? 0 : MF_GRAYED), base + 2, L"Options");
        AppendMenuW(sub, MF_STRING | (e.ext ? 0 : MF_GRAYED), base + 1, e.enabled ? L"Turn off" : L"Turn on");
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, base + 3, L"Remove from Docsy");
        std::wstring label;
        for (wchar_t c : e.name) { label += c; if (c == L'&') label += c; }
        if (!e.enabled) label += L"  (off)";
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1, L"Get extensions from the Chrome Web Store");
    AppendMenuW(m, MF_STRING, 3, L"Copy diagnostics to the clipboard");
    AppendMenuW(m, MF_STRING, 4, L"Install test extension (Manifest V3)");
    AppendMenuW(m, MF_STRING, 5, L"Install test extension (Manifest V2)");
    SetForegroundWindow(g.main);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.main, nullptr);
    DestroyMenu(m);
    if (c == 1) { WebNewTab(L"https://chromewebstore.google.com/category/extensions", true); return; }
    if (c == 2) { WxInstall(page); return; }
    if (c == 3) { WxCopyDiag(); return; }
    if (c == 4) { WxInstall(L"ddkjiahejlhfcafbddmgiahcphecmpfh"); return; }   // uBlock Origin Lite, MV3
    if (c == 5) { WxInstall(L"dhdgffkkebhmkfjojejmpbldmpobfkfo"); return; }   // Tampermonkey, MV2
    if (c >= 1000) {
        int i = (c - 1000) / 10, k = (c - 1000) % 10;
        if (i < 0 || i >= (int)g_wx.size()) return;
        std::wstring id = g_wx[i].id;
        if (k == 0) {
            RECT r = g_wbR[WB_EXT];
            POINT a{ r.left, r.top }, b{ r.right, r.bottom };
            ClientToScreen(g_wstrip, &a); ClientToScreen(g_wstrip, &b);
            WxOpenPopup(i, RECT{ a.x, a.y, b.x, b.y });
        } else if (k == 1) WxSetEnabled(id, !g_wx[i].enabled);
        else if (k == 2) WxOpenOptions(i);
        else if (k == 3) WxRemove(id);
    }
}


static bool WebBtnEnabled(int id) {
    WebTab* t = WebCur();
    switch (id) {
    case WB_BACK: return t && t->canBack;
    case WB_FWD: return t && t->canFwd;
    case WB_RELOAD: case WB_READER: case WB_NEWTAB: case WB_MENU: case WB_SIDE: case WB_EXT: return true;
    }
    return false;
}

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
// "duckduckgo.com", no scheme, no "www.", no path. Anything that is not a
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
    case WM_CTLCOLOREDIT:                          // the omnibox edit is this window's child
        return (LRESULT)WebEditColors((HDC)wp, (HWND)lp);
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            int hit = WebNavHit(p);
            bool pill = PtInRect(&g_webOmni, p);
            bool xi = WxHit(p) >= 0;
            SetCursor(LoadCursorW(nullptr, ((hit >= 0 && hit != WB_RELOAD) || xi) ? IDC_HAND : (pill ? IDC_IBEAM : IDC_ARROW)));
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
    if (g_wxOk) {                                  // gains the room they used to bracket
        std::vector<int> vis;
        for (int i = 0; i < (int)g_wx.size() && vis.size() < 5; i++)
            if (g_wx[i].enabled && !g_wx[i].popup.empty()) vis.push_back(i);
        int n = (int)vis.size();
        int right0 = (int)g_wbR[WB_NEWTAB].left - (pitch - d);   // the row's own 2dp gap
        for (int k = 0; k < n; k++) {
            int right = right0 - (n - 1 - k) * pitch;
            g_wxR.push_back(RECT{ right - d, cy, right, cy + d });
            g_wxIdx.push_back(vis[k]);
        }
        if (n) leftEdge = (int)g_wxR[0].left;
    }
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
    case WB_EXT: WxMenu(); return;
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
    case WB_NEWTAB: WebNewTab(L"https://duckduckgo.com/", true); return;
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
        case 1: WebNewTab(L"https://duckduckgo.com/", true); break;
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
        // a search, percent-encoded the way an address bar owes the engine
        url = L"https://duckduckgo.com/?q=";
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
    for (auto& t : g_wt) if (t.captureDue || t.capturePending) need = true;
    if (need) SetTimer(g.main, 7, 200, nullptr);
    else KillTimer(g.main, 7);
}

static void WebCaptureTab(int id);
static void WebThumbDecode(int id, IStream* st);

static void WebCaptureTick() {                     // the main window's pump (timer 7)
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
    int TW2 = (int)(160 * g.dpi), TH2 = max(1, (int)((long long)TW2 * bh / bw));
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
        if (g_wt[i].ctr)   // a tab whose first frame never landed stays hidden: its surface
            g_wt[i].ctr->put_IsVisible((i == g_wtCur && g_wt[i].shown) ? TRUE : FALSE);   // is still black glass
    WebPlace();
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
    if (t->ctr) return;
    unsigned long long id = t->id;
    g_wvEnv->CreateCoreWebView2Controller(g.view,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [id](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                if (FAILED(hr) || !c) { g_webFail = true; WebRepaintChrome(); return S_OK; }
                WebTab* t = WebTabById(id);
                if (!t) { c->Close(); return S_OK; }
                t->ctr = c;
                c->get_CoreWebView2(&t->web);
                wrl::ComPtr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2)
                    c2->put_DefaultBackgroundColor({ 255, 0x1c, 0x1c, 0x1c });   // the card's dark,
                                                                                    // not Chromium's white
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
                        [id](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                            WebTab* t = WebTabById(id);
                            if (!t) return S_OK;
                            t->loading = true;
                            WebRepaintChrome(); WebStripSpin();
                            return S_OK;
                        }).Get(), &t->tkNavS);
                t->web->add_NavigationCompleted(
                    Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [id](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                            WebTab* t = WebTabById(id);
                            if (!t) return S_OK;
                            t->loading = false;
                            if (!t->shown) {           // the engine's first frame exists now: the
                                t->shown = true;       // card comes off the backdrop - this is the
                                if (g_webOn && g_wtCur >= 0 && g_wtCur < (int)g_wt.size()
                                    && &g_wt[g_wtCur] == t && t->ctr)
                                    t->ctr->put_IsVisible(TRUE);
                                if (g.view) InvalidateRect(g.view, nullptr, FALSE);
                            }
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
                c->put_IsVisible(FALSE);           // the surface stays dark until its first frame:
                                                   // a fresh controller's HWND reads as pure black
                                                   // glass before Chromium composites into it, which
                                                   // is the blackscreen on open and on a new tab -
                                                   // the card's backdrop shows instead, and the
                                                   // first NavigationCompleted raises the view
                WebPlace(); WebRepaintChrome();
                if (t && !t->pending.empty()) {    // a tab born before its engine navigates
                    if (g_wxReady) {                        // the moment it exists - once the extensions
                        t->web->Navigate(t->pending.c_str());   // are in, so content scripts hit page one
                        t->pending.clear();
                    }
                }
                WxInit();                          // once per session, pending or not: the listing
                return S_OK;                       // is the evidence - a refusal here condemns the
                                                   // whole engine, not just one install
            }).Get());
}

static void WebEnsureEnv() {
    if (g_wvEnv || g_webFail) return;
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
    if (envOpts && SUCCEEDED(envOpts.As(&envOptsBase)) && envOptsBase)   // the base interface
        envOptsBase->put_AdditionalBrowserArguments(
            L"--disable-features=ExtensionManifestV2Unsupported,ExtensionManifestV2Disabled");
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
    CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), envOpts.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) { g_webFail = true; WxLogf(L"env: creation failed 0x%08lX", (unsigned long)hr); WebRepaintChrome(); return S_OK; }
                g_wvEnv = env;
                for (int i = 0; i < (int)g_wt.size(); i++) WebEnsureController(i);
                WebRepaintChrome();
                return S_OK;
            }).Get());
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
        WebNewTab(L"https://duckduckgo.com/", true);
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

// the page: KillerPDF's floating card. The webview rides inset in the view -
// its top tucks under the strip the way the reader's card tucks under the tab
// band, an 8dp gutter runs down the right (the window gutter), the foot keeps
// 16dp (the shadow's room, the rows the reader's footer used to own), and the
// left edge either butts the thumbnails container - its halo falls on it, the
// rail's story - or, with the rail closed, keeps the same 8dp gutter. The
// gutters are where the card's PaneShadow shows.
static int WebGutL() { return g_webSide ? 0 : (int)(8 * g.dpi); }

static void WebCardRect(RECT& c, int w, int h) {
    int L = WebGutL();
    int R = max(L + 10, w - (int)(8 * g.dpi));
    int B = max(10, h - (int)(16 * g.dpi));
    c = { L, 0, R, B };
}

// the view's whole paint in browser mode: KillerPDF's backdrop - the shared
// chrome grain as the field, the card's PaneShadow falling into the gutters
// (black, BlurRadius 16, ShadowDepth 5, Direction 270, Opacity 0.6, the same
// blurred silhouette CardShadowStrip bakes for the reader's panes, evaluated
// over the three strips where the gutters are - the middle dies under the
// webview's own pixels) - plus the no-engine card when the runtime is missing
// and the New Tab word while the engine's first frame is still on its way, so
// an open and a fresh tab show the card's own room, never black glass. One
// body for the live view's WM_PAINT and the shot composite, so the two can
// never drift.
static HBITMAP g_wcbm = nullptr; static HDC g_wcdc = nullptr;
static int g_wcw = 0, g_wch = 0, g_wcside = -1; static double g_wcdpi = 0;

static void WebCardPaint(HDC dc, int w, int h) {
    RECT vr{ 0, 0, w, h };
    if (!g_wcbm || g_wcw != w || g_wch != h || g_wcdpi != g.dpi || g_wcside != (int)g_webSide) {
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
            int L = WebGutL();
            int R = max(L + 10, w - (int)(8 * g.dpi));
            int B = max(10, h - (int)(16 * g.dpi));
            const double sig = 8.0 * g.dpi, depth = 5.0 * g.dpi;   // BlurRadius 16, ShadowDepth 5
            int rad = max(2, (int)(6 * g.dpi));
            float sx0 = (float)L, sy0 = (float)depth, sx1 = (float)R, sy1 = (float)B + (float)depth;
            // the silhouette: the card pushed straight down, the reader's own recipe
            const int LUTN = 236;
            static float lut[LUTN]; static double lutSig = 0;
            if (lutSig != sig) {
                for (int i = 0; i < LUTN; i++) {
                    double d2 = -3.0 * sig + (6.0 * sig) * i / (LUTN - 1);
                    lut[i] = (float)(0.6 * Phi(-d2 / sig));
                }
                lutSig = sig;
            }
            for (int side = 0; side < 3; side++) {            // three disjoint strips: the right
                int x0, x1, y0, y1;                           // falloff, the left one when the
                if (side == 0) {                              // gutter exists, and the foot - the
                    x0 = max(0, R - 4); x1 = w; y0 = 0; y1 = h;    // corners wrap through the SDF
                } else if (side == 1) {
                    if (L <= 0) continue;                     // open rail: the halo falls on it,
                    x0 = 0; x1 = min(w, L + 4); y0 = 0; y1 = h;    // DrawRailShadow's, not this one's
                } else {
                    x0 = (L > 0) ? min(w, L + 4) : 0;   // open rail: the left column is the
                                                        // card's own - the strip starts at 0
                    x1 = max(x0, min(w, R - 4));
                    y0 = max(0, B - 4); y1 = h;
                }
                for (int y = y0; y < y1; y++) {
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
            SelectObject(bdc, ob);
            DeleteDC(bdc);
            if (g_wcbm) DeleteObject(g_wcbm);
            if (g_wcdc) DeleteDC(g_wcdc);
            g_wcbm = bm;
            g_wcdc = CreateCompatibleDC(dc);
            SelectObject(g_wcdc, bm);
            g_wcw = w; g_wch = h; g_wcdpi = g.dpi; g_wcside = (int)g_webSide;
        }
    }
    if (g_wcbm) BitBlt(dc, 0, 0, w, h, g_wcdc, 0, 0, SRCCOPY);
    else {
        HBRUSH pb = CreateSolidBrush(wvb::Page);
        FillRect(dc, &vr, pb); DeleteObject(pb);
    }
    if (g_webFail) { WebFailCard(dc, w, h); return; }
    WebTab* t = WebCur();
    if (t && !t->shown) {                        // the engine's first frame has not landed: the
        SetBkMode(dc, TRANSPARENT);              // card says what it is instead of black glass
        SelectObject(dc, CaptionFont(g_tbcap, 15, false, L"Killer Sans"));
        SetTextColor(dc, wvb::InkDim);
        RECT tr{ 0, h / 2 - (int)(12 * g.dpi), w, h / 2 + (int)(12 * g.dpi) };
        DrawTextW(dc, L"New Tab", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void WebPlace() {
    WebTab* t = WebCur();
    if (!g.view || !t || !t->ctr) return;
    RECT c; WebCardRect(c, g.cw, g.ch);
    RECT b = c;
    if (b.right <= b.left || b.bottom <= b.top) return;
    t->ctr->put_Bounds(b);
}

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
        if (g_wt.empty()) WebNewTab(L"https://duckduckgo.com/", true);
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
    g_wcw = g_wch = 0; g_wcside = -1;
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
    ws.tw = (int)(96 * g.dpi);                     // the reader's card width, the same figure
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
    if (ctrl && wp == 'T') { WebNewTab(L"https://duckduckgo.com/", true); return true; }
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
        Gdiplus::SolidBrush fill(Gdiplus::Color((BYTE)255, 0x10, 0x10, 0x10));
        gs.FillPath(&fill, pth);
        Gdiplus::Pen edge(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
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
            Gdiplus::SolidBrush gb(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56));
            gs.FillPath(&gb, pth);
        } else if (accent) {
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else if (hot) {
            Gdiplus::SolidBrush hb(Gdiplus::Color((BYTE)255, 0x40, 0x40, 0x40));
            gs.FillPath(&hb, pth);
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else {
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
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

// ---------------- the save prompts (KillerDialog's themed question box) ----------------
// KillerPDF never asks with a MessageBox: KillerDialog.Show is the app's own
// chrome at 390 DIP - the family caption (wordmark, qualifier, close glyph),
// the notice body (a 28px amber "!" ring in a 38px column beside the wrapped
// 13px message, margins 24/18) and the button row right-aligned with 8px gaps
// ("the way out first and the action last, at the right edge", 28px tall,
// >=80 wide, the accent hollow at rest and filled on hover, Enter on the
// accent, Esc on Cancel). Docsy's Document Info dialog already carries this
// chrome; the prompts are the same paint with three buttons:
//  * the Save click over changes - KillerPDF's Str_Dlg_OverwriteMsg
//    "Replace {name}?" answered Cancel | Save As | Overwrite, and
//  * the close prompts - Str_Dlg_UnsavedClose / Str_Dlg_UnsavedExit answered
//    Cancel | Don't Save | Save.
// Every caller gates on DocDirty: the prompt only exists over changes.
struct AskDlg {
    std::wstring qualifier;              // caption text after the wordmark
    std::wstring message;
    std::wstring btn[3];                 // visual order, left to right
    int bw[3] = {};                      // button widths: the label's own + 24, >=80
    int def = 2;                         // Enter's target = the accent, rightmost
    int result = -1;                     // -1 = dismissed (Esc / X / click-away)
    int hot = -1;                        // hovered: 0-2 buttons, 3 the caption close
    int msgH = 0;                        // the wrapped message's height, DIP pixels
};
static AskDlg g_ask;
static HWND g_askDlg = nullptr;
static bool g_askShots = false;          // the CI shot composites the same paint

static HFONT g_askCap = nullptr, g_askMsg = nullptr, g_askBtn = nullptr, g_askBang = nullptr;
static HFONT AskCapFont()  { return CaptionFont(g_askCap, 14, false, L"Killer Sans"); }
static HFONT AskMsgFont()  { return CaptionFont(g_askMsg, 13, false, L"Killer Sans UI"); }
static HFONT AskBtnFont()  { return CaptionFont(g_askBtn, 12, false, L"Killer Sans UI"); }
static HFONT AskBangFont() { return CaptionFont(g_askBang, 16, true, L"Killer Sans"); }

static int AskW() { return (int)(390 * g.dpi); }      // KillerDialog's Width=390
static int AskCapH() { return (int)(32 * g.dpi); }    // the family caption bar
// NoticeBody's grid: Margin 24,18,24,18, a 38px icon column, the 28px ring.
// The button row: Padding 16,0,24,20, buttons 28 tall with 8px gaps.
static int AskMsgTop() { return AskCapH() + (int)(18 * g.dpi); }
static int AskMsgW(int w) { return w - (int)(86 * g.dpi); }   // 24 margin + 38 column + 24
static int AskContentH() { return max((int)(28 * g.dpi), g_ask.msgH + (int)(5 * g.dpi)); }
static int AskH() {
    return AskCapH() + (int)(18 * g.dpi) + AskContentH() + (int)(18 * g.dpi)
         + (int)(28 * g.dpi) + (int)(20 * g.dpi);
}

// Button i's rect: the row walks in from the right edge, 8px between buttons.
static RECT AskBtnRect(int i, int w, int h) {
    int bh = (int)(28 * g.dpi), gap = (int)(8 * g.dpi);
    int x = w - (int)(24 * g.dpi), bottom = h - (int)(20 * g.dpi);
    for (int k = 2; k >= 0; k--) {
        if (k == i) return { x - g_ask.bw[k], bottom - bh, x, bottom };
        x -= g_ask.bw[k] + gap;
    }
    return { 0, 0, 0, 0 };
}

// the wrapped message's height and the three widths, measured once per ask
static void AskMeasure() {
    if (!g.main) return;
    HDC dc = GetDC(g.main);
    HFONT of = (HFONT)SelectObject(dc, AskMsgFont());
    RECT mr{ 0, 0, AskMsgW(AskW()), 1 << 15 };
    DrawTextW(dc, g_ask.message.c_str(), -1, &mr, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    g_ask.msgH = mr.bottom - mr.top;
    SelectObject(dc, AskBtnFont());
    for (int k = 0; k < 3; k++) {
        SIZE ts{};
        GetTextExtentPoint32W(dc, g_ask.btn[k].c_str(), (int)g_ask.btn[k].size(), &ts);
        g_ask.bw[k] = max((int)(80 * g.dpi), (int)ts.cx + (int)(24 * g.dpi));
    }
    SelectObject(dc, of);
    ReleaseDC(g.main, dc);
}

static HDC g_akdc = nullptr; static HBITMAP g_akbm = nullptr, g_akold = nullptr;
static int g_akw = 0, g_akh = 0;
static void EnsureAskBack(HDC ref, int w, int h) {   // the Info dialog's buffered present:
    if (g_akbm && g_akw == w && g_akh == h) return;  // a hover flip reblits an identical
    if (g_akbm) { SelectObject(g_akdc, g_akold); DeleteObject(g_akbm); }
    else g_akdc = CreateCompatibleDC(ref);
    g_akbm = CreateCompatibleBitmap(ref, w, h);
    g_akold = (HBITMAP)SelectObject(g_akdc, g_akbm);
    g_akw = w; g_akh = h;
}

// One paint, two hosts: the live dialog's WM_PAINT and the CI shot composite.
static void AskDialogDraw(HDC dc, int w, int h) {
    int capH = AskCapH();
    // the caption row: the family bar - the wordmark, the qualifier, the
    // red-hovered close; the whole bar drags the window (WM_NCHITTEST below)
    RECT cr{ 0, 0, w, capH };
    FillRect(dc, &cr, GrainBrush(2));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, AskCapFont());
    RECT tr{ (int)(12 * g.dpi), 0, w - (int)(44 * g.dpi), capH };
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
    DrawTextW(dc, L"Docsy", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SIZE ts{};
    GetTextExtentPoint32W(dc, L"Docsy", 5, &ts);
    RECT qr{ tr.left + ts.cx + (int)(4 * g.dpi), tr.top, tr.right, tr.bottom };
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
    DrawTextW(dc, g_ask.qualifier.c_str(), -1, &qr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    int bs = (int)(44 * g.dpi);
    RECT xr{ w - bs, 0, w, capH };
    if (g_ask.hot == 3) { RECT hr{ xr.left, 0, xr.right, capH }; FillRect(dc, &hr, RowHoverBrush()); }
    SelectObject(dc, StGlyph());
    SetTextColor(dc, g_ask.hot == 3 ? RGB(0xff, 0x67, 0x67) : RGB(0xa3, 0xa3, 0xa3));
    DrawTextW(dc, L"\xE8BB", -1, &xr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // the body: the shared grain, the amber ring, the message beside it
    RECT body{ 0, capH, w, h };
    FillRect(dc, &body, GrainBrush(1));
    SetBkMode(dc, TRANSPARENT);
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    int top = AskMsgTop();
    // the 28px ring, top-aligned in the 38px column - KillerDialog's Notice
    // Border, its amber darkening for the light theme never leaving dark here
    float inset = max(1.0f, (float)g.dpi);           // the 2px pen eats inward, keep the box
    Gdiplus::Pen ring(Gdiplus::Color((BYTE)255, 0xff, 0xbe, 0x50), 2.0f * (float)g.dpi);
    gs.DrawEllipse(&ring, (float)(24 * g.dpi) + inset, (float)top + inset,
                   (float)(28 * g.dpi) - 2 * inset, (float)(28 * g.dpi) - 2 * inset);
    RECT bang{ (int)(24 * g.dpi), top, (int)(24 * g.dpi) + (int)(28 * g.dpi), top + (int)(28 * g.dpi) };
    SelectObject(dc, AskBangFont());
    SetTextColor(dc, RGB(0xff, 0xbe, 0x50));
    DrawTextW(dc, L"!", -1, &bang, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // the message: column 1, its own 5px top margin, wrapping in 304 DIP
    RECT mr{ (int)(62 * g.dpi), top + (int)(5 * g.dpi), w - (int)(24 * g.dpi),
             top + (int)(5 * g.dpi) + max(g_ask.msgH, 1) };
    SelectObject(dc, AskMsgFont());
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
    DrawTextW(dc, g_ask.message.c_str(), -1, &mr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    // the three buttons: the way out first, the accent last, at the right edge
    auto boxPath = [&](const RECT& rc) {
        float r2 = (float)(4 * g.dpi);
        float x1 = (float)rc.left, y1 = (float)rc.top;
        float fw = (float)(rc.right - rc.left), fh = (float)(rc.bottom - rc.top);
        Gdiplus::GraphicsPath* pth = new Gdiplus::GraphicsPath();
        pth->AddArc(x1, y1, 2 * r2, 2 * r2, 180, 90);
        pth->AddArc(x1 + fw - 2 * r2, y1, 2 * r2, 2 * r2, 270, 90);
        pth->AddArc(x1 + fw - 2 * r2, y1 + fh - 2 * r2, 2 * r2, 2 * r2, 0, 90);
        pth->AddArc(x1, y1 + fh - 2 * r2, 2 * r2, 2 * r2, 90, 90);
        pth->CloseFigure();
        return pth;
    };
    for (int k = 0; k < 3; k++) {
        RECT br = AskBtnRect(k, w, h);
        bool accent = k == g_ask.def, hot = g_ask.hot == k;
        Gdiplus::GraphicsPath* pth = boxPath(br);
        if (accent && hot) {
            Gdiplus::SolidBrush gb(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56));
            gs.FillPath(&gb, pth);
        } else if (accent) {
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else if (hot) {
            Gdiplus::SolidBrush hb(Gdiplus::Color((BYTE)255, 0x40, 0x40, 0x40));
            gs.FillPath(&hb, pth);
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else {
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        }
        delete pth;
        SetTextColor(dc, accent ? (hot ? RGB(0x05, 0x24, 0x0f) : RGB(0x22, 0xbb, 0x56))
                                : RGB(0xe0, 0xe0, 0xe0));
        SelectObject(dc, AskBtnFont());
        DrawTextW(dc, g_ask.btn[k].c_str(), -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

static void AskClose(int r) {
    if (!g_askDlg) return;
    g_ask.result = r;
    HWND d = g_askDlg;
    g_askDlg = nullptr;
    DestroyWindow(d);
    RailRepaint();
    if (g.main) SetFocus(g.main);            // the keys go home with the dialog gone
}

static LRESULT CALLBACK AskDlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureAskBack(dc, r.right, r.bottom);
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
            SetBrushOrgEx(g_akdc, -po.x, -po.y, nullptr);   // the grain phases to the window,
            AskDialogDraw(g_akdc, r.right, r.bottom);       // one blit per hover flip
            BitBlt(dc, 0, 0, r.right, r.bottom, g_akdc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        // the popup contract (the Info dialog's): a click anywhere outside -
        // the main window, another app, the taskbar - answers Cancel
        if (LOWORD(wp) == WA_INACTIVE && g_askDlg == h) AskClose(-1);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { AskClose(-1); return 0; }          // IsCancel
        if (wp == VK_RETURN) { AskClose(g_ask.def); return 0; }   // IsDefault = the accent
        return 0;
    case WM_NCHITTEST: {
        // the caption DRAGS the window (DialogFrame's DragMove); the close
        // zone stays client so its click is ours
        POINT s{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT c = s; ScreenToClient(h, &c);
        RECT r; GetClientRect(h, &r);
        if (c.y >= 0 && c.y < AskCapH() && c.x < r.right - (int)(44 * g.dpi)) return HTCAPTION;
        break;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        int hot = -1;
        for (int k = 0; k < 3; k++) {
            RECT br = AskBtnRect(k, r.right, r.bottom);
            if (PtInRect(&br, p)) { hot = k; break; }
        }
        if (hot < 0 && p.y < AskCapH() && p.x >= r.right - (int)(44 * g.dpi)) hot = 3;
        if (hot != g_ask.hot) {
            g_ask.hot = hot;
            if (hot >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_ask.hot >= 0) { g_ask.hot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        for (int k = 0; k < 3; k++) {
            RECT br = AskBtnRect(k, r.right, r.bottom);
            if (PtInRect(&br, p) && g_ask.hot == k) { AskClose(k); return 0; }
        }
        if (p.y < AskCapH() && p.x >= r.right - (int)(44 * g.dpi) && g_ask.hot == 3) AskClose(-1);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT r; GetClientRect(h, &r);
            bool over = false;
            for (int k = 0; k < 3 && !over; k++) {
                RECT br = AskBtnRect(k, r.right, r.bottom);
                over = PtInRect(&br, p) != FALSE;
            }
            if (over || (p.y < AskCapH() && p.x >= r.right - (int)(44 * g.dpi))) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
            }
            if (p.y < AskCapH()) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        }
        break;
    case WM_CLOSE:
        AskClose(-1);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// The ask itself: seed the state, open the owned popup centered on the main
// window and pump until it answers - the caller's flow stays linear, exactly
// like KillerDialog's ShowDialog. Returns the button's index (0 leftmost, 2
// the accent) or -1 for a dismissal, which every caller reads as Cancel.
static int AskPrompt3(const wchar_t* qualifier, const wchar_t* msg,
                      const wchar_t* b0, const wchar_t* b1, const wchar_t* b2) {
    static bool inFlight = false;            // a close click landing mid-ask reads Cancel
    if (!g.main || inFlight) return -1;
    inFlight = true;
    g_ask = AskDlg{};
    g_ask.qualifier = qualifier;
    g_ask.message = msg;
    g_ask.btn[0] = b0; g_ask.btn[1] = b1; g_ask.btn[2] = b2;
    g_ask.def = 2;
    AskMeasure();
    int w = AskW(), h = AskH();
    RECT mr{}; GetWindowRect(g.main, &mr);
    int x = mr.left + max(0, (int)((mr.right - mr.left) - w) / 2);
    int y = mr.top + max(0, (int)((mr.bottom - mr.top) - h) / 2);
    g_askDlg = CreateWindowExW(0, L"DocsyAskDlg", nullptr, WS_POPUP, x, y, w, h,
                               g.main, nullptr, g.hinst, nullptr);
    if (!g_askDlg) { inFlight = false; return -1; }
    DwmApply(g_askDlg);
    ShowWindow(g_askDlg, SW_SHOWNORMAL);
    SetFocus(g_askDlg);
    RailRepaint();
    MSG m2;
    while (g_askDlg && GetMessageW(&m2, nullptr, 0, 0) > 0) {   // the modal pump: eat
        TranslateMessage(&m2);                                 // messages until the
        DispatchMessageW(&m2);                                 // dialog answers itself
    }
    inFlight = false;
    return g_ask.result;
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
    struct Kid { HWND w; RECT was; } kids[7] = { { g.bar, {} }, { g_wtool, {} }, { g_wstrip, {} }, { g.side, {} }, { g.rail, {} }, { g.view, {} }, { g.status, {} } };
    for (auto& k : kids)
        if (k.w) {
            GetWindowRect(k.w, &k.was);
            ScreenToClient(g.main, (POINT*)&k.was);
            ScreenToClient(g.main, ((POINT*)&k.was) + 1);
        }
    int sw = g_webOn ? (g_webSide ? SW() : 0) : ((g.sideOn && DVF().n && !g.full) ? SW() : 0);   // the sidebar shows the FOCUSED
    int rw = RW();                              // pane's pages; the rail always rides between
    int cap0 = CAP();
    int tl = WEBTOOLH();                        // browser mode: the reader's toolbar row, one button
    int cap = cap0 + tl;                        // everything below sits under caption + toolbar
    int tb = g_webOn ? WebBandH() : TBH();      // browser mode: the tab band - there only
                                                // from the second tab, like the reader's
    int ws_ = WEBSTRIPH();                      // the browser's toolbar rides above it
    int sh = (g.full || g_webOn) ? 0 : SBH();   // full screen and the browser hide the footer
    int ch = max(0, h - cap - ws_ - tb - sh);   // the children sit below caption + strip + toolbar
    sd.w = sw;
    if (g_webOn) {
        if (g_wtool) MoveWindow(g_wtool, 0, cap0, w, tl, TRUE);
        if (g_wstrip) MoveWindow(g_wstrip, 0, cap, w, ws_, TRUE);
        if (g.bar) MoveWindow(g.bar, 0, cap + ws_, w, tb, TRUE);
    } else if (g.bar) {
        MoveWindow(g.bar, 0, cap, w, tb, TRUE);
    }
    bool sideResized = false;
    if (g.side) {
        MoveWindow(g.side, 0, cap + ws_ + tb, sw, ch, TRUE);
        if (sw) {
            RECT sr; GetClientRect(g.side, &sr);
            if (sr.right != sd.cw || sr.bottom != sd.ch) { sd.cw = sr.right; sd.ch = sr.bottom; sideResized = true; }
        }
    }
    if (g_webOn) {
        if (sw) { ws.cw = sd.cw; ws.ch = sd.ch; WebSideLayout(); }   // the rail's web state
    } else if (sideResized) SideLayout();                            // tracks the same window
    if (g.rail) {                               // the rail: between the sidebar and the page,
        MoveWindow(g.rail, sw, cap + ws_ + tb, rw, ch, TRUE);   // always there but in full screen
        ShowWindow(g.rail, rw > 0 ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    MoveWindow(g.view, sw + rw, cap + ws_ + tb, max(0, w - sw - rw), ch, TRUE);
    if (g.status) MoveWindow(g.status, 0, cap + ws_ + tb + ch, w, sh, TRUE);
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
        if (g_webOn) { WebBarLayout(); WebPlace(); WebResizeDeb(); }   // the browser parks its omnibox
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
                                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,   // CLIPCHILDREN: the omnibox
                                0, 0, 0, 0, h, nullptr, g.hinst, nullptr);   // edit lives on this row
        g.rail = CreateWindowExW(0, L"DocsyRail", nullptr,
                                 WS_CHILD | WS_VISIBLE,
                                 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        g_wtool = CreateWindowExW(0, L"DocsyWebTool", nullptr,     // browser mode's reader toolbar
                                  WS_CHILD, 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);
        g_wstrip = CreateWindowExW(0, L"DocsyWebStrip", nullptr,   // the browser's Safari toolbar,
                                   WS_CHILD, 0, 0, 0, 0, h, nullptr, g.hinst, nullptr);   // hidden until Web
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
                                 g_bdW[slot] = g_bdH[slot] = 0; g_bdDpi[slot] = 0; g_bdIns[slot] = -1; }
        if (g_fntEdit) { DeleteObject(g_fntEdit); g_fntEdit = nullptr; }
        if (g_fntOpt) { DeleteObject(g_fntOpt); g_fntOpt = nullptr; }
        if (g_fntAb) { DeleteObject(g_fntAb); g_fntAb = nullptr; }
        if (g_fntStat) { DeleteObject(g_fntStat); g_fntStat = nullptr; }
        if (g_fntGlyph) { DeleteObject(g_fntGlyph); g_fntGlyph = nullptr; }
        if (g_askCap) { DeleteObject(g_askCap); g_askCap = nullptr; }   // the ask dialog's four
        if (g_askMsg) { DeleteObject(g_askMsg); g_askMsg = nullptr; }   // faces re-land at the
        if (g_askBtn) { DeleteObject(g_askBtn); g_askBtn = nullptr; }   // new scale on next use
        if (g_askBang) { DeleteObject(g_askBang); g_askBang = nullptr; }
        if (g_fedit) SendMessageW(g_fedit, WM_SETFONT, (WPARAM)FdFontEdit(), TRUE);
        for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
        sd.th.clear(); sd.thBytes = 0; sd.tw = 0;
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
        if (g_webOn) return 0;               // the page scrolls itself: the wheel belongs to Chromium
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &p);
        if (g.sideOn && DVF().n && p.x < sd.w) {
            SideScrollTo(sd.scroll - GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 240) / 120);
            return 0;
        }
        ViewWheel(wp, lp);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        if (g_webOn) return 0;
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(g.view, &p);
        bool b = PaneB(p.x);
        DocState& D = DV(b);
        ScrollPane(b, D.sx + GET_WHEEL_DELTA_WPARAM(wp) * (int)(g.dpi * 80) / 120, D.sy);
        return 0;
    }
    case WM_SYSKEYDOWN:                      // Alt+Left / Alt+Right: the browser's history
        if (g_webOn && WebSysKey(wp)) return 0;
        break;
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (g_webOn) { WebKey(wp, ctrl, shift); return 0; }   // the browser owns the keyboard
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
            if (ctrl && shift) CloseOtherTabs(FocusB(), Act(FocusB()));   // Close Others
            else if (ctrl) CloseDoc();
            break;
        case 'Q':
            if (ctrl && shift) CloseAllTabs(FocusB());   // Ctrl+Shift+Q: close every tab
            break;
        case VK_TAB:                               // Ctrl+Tab / Ctrl+Shift+Tab cycle the
            if (ctrl) CycleTab(FocusB(), shift ? -1 : 1);   // focused pane's document tabs
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
        if (wp == 7) { WebCaptureTick(); return 0; }   // the web thumbs' capture pump
        break;                                 // (buffered, so an unchanged band reblits
                                               // identically and cannot strobe)
    case WM_DROPFILES: HandleDrop((HDROP)wp, h); return 0;   // the frame's drop: the focused pane
    case WM_APP + 5: WxInstallDone((WxJob*)lp); return 0;   // an extension download finished
    case WM_APP + 7: WxEngineRestart(); return 0;   // the deferred engine restart: a fresh
                                                    // browser process for extension operations
    case WM_APP + 3: {                        // the drop queue's drain: one document per
        if (!g_dropQ.empty()) {               // message, so the window paints between loads
            std::wstring p = std::move(g_dropQ.front());
            bool pb = g_dropPane.front();
            g_dropQ.erase(g_dropQ.begin());
            g_dropPane.erase(g_dropPane.begin());
            TabOpenInto(pb, p);
            if (!g_dropQ.empty()) PostMessageW(h, WM_APP + 3, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE: {
        // KillerPDF's quit prompt (Str_Dlg_UnsavedExit through KillerDialog.
        // AskToSave): unsaved work asks before the window dies. Save writes
        // every dirty tab of both panes in place - a session holds a live
        // engine handle, so its document saves without coming forward - and a
        // failed write keeps the window up.
        if (g.tedit) CommitTextEdit();
        auto paneDirty = [&](bool b) {
            std::vector<DocState>& T = Tabs(b);
            if (T.empty()) return DocDirty(DV(b));    // a pane that never tabbed: the live state alone
            for (int i = 0; i < (int)T.size(); i++)
                if (DocDirty(i == Act(b) ? DV(b) : T[i])) return true;
            return false;
        };
        bool aDirty = paneDirty(false), bDirty = paneDirty(true);
        if (!aDirty && !bDirty) { DestroyWindow(h); return 0; }
        int r = AskPrompt3(L"", L"You have unsaved changes. Save them before closing Docsy?",
                           L"Cancel", L"Don't Save", L"Save");
        if (r == 2) {
            auto savePane = [&](bool b) -> bool {
                std::vector<DocState>& T = Tabs(b);
                if (T.empty()) {
                    if (!DocDirty(DV(b))) return true;
                    std::wstring err;
                    if (!WriteAnnotatedPdf(DV(b), DV(b).path, err)) {
                        SetStatusHold(std::wstring(L"Save failed: ") + err);
                        return false;
                    }
                    SnapClean(DV(b));
                    return true;
                }
                for (int i = 0; i < (int)T.size(); i++) {
                    DocState& S = (i == Act(b)) ? DV(b) : T[i];
                    if (!DocDirty(S)) continue;
                    std::wstring err;
                    if (!WriteAnnotatedPdf(S, S.path, err)) {
                        SetStatusHold(std::wstring(L"Save failed: ") + err);
                        return false;
                    }
                    SnapClean(S);
                }
                return true;
            };
            if (!savePane(false)) return 0;          // a failed save is not a closed window
            if (!savePane(true)) return 0;
        }
        if (r == 2 || r == 1) DestroyWindow(h);   // Save / Don't Save close; Cancel stays
        return 0;
    }
    case WM_DESTROY:
        RqShutdown();                        // the render worker ends before the queues die
        WebShutdown();                       // the WebView2 controllers close with the window
        PostQuitMessage(0); return 0;
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
    CacheDrop(D, page);                      // GetBmp re-renders with the new rot bits
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
    int sw = g_webOn ? sd.w : ((g.sideOn && DVF().n && !g.full) ? sd.w : 0);   // web: the rail
    int rw = RW();                                  // always shows the OPEN WEB TABS
    int cap0 = CAP(), cap = cap0 + WEBTOOLH(), sh = g_webOn ? 0 : SBH();
    int tb = g_webOn ? WebBandH() : TBH();          // browser mode: the tab band
    int ws_ = WEBSTRIPH();                          // the nav row rides between caption and tabs
    int ch = max(1, h - cap - ws_ - tb - sh);
    // the children render above the caption row and below it, so composite them shifted
    if (sw) {
        HDC m1 = CreateCompatibleDC(dc);
        HBITMAP b1 = CreateCompatibleBitmap(dc, sw, ch);
        HGDIOBJ o1 = SelectObject(m1, b1);
        SetBrushOrgEx(m1, 0, -(cap + ws_ + tb), nullptr);   // the pane's grain phase, as on screen
        if (g_webOn) WebSideDraw(m1, sw, ch);               // the OPEN WEB TABS rail
        else SideDraw(m1, sw, ch, 0);
        BitBlt(dc, 0, cap + ws_ + tb, sw, ch, m1, 0, 0, SRCCOPY);
        SelectObject(m1, o1); DeleteObject(b1); DeleteDC(m1);
    }
    {
        HDC m2 = CreateCompatibleDC(dc);
        int vw = max(1, w - sw - rw);
        HBITMAP b2 = CreateCompatibleBitmap(dc, vw, ch);
        HGDIOBJ o2 = SelectObject(m2, b2);
        SetBrushOrgEx(m2, -sw - rw, -(cap + ws_ + tb), nullptr);
        g_grainPX = -sw - rw; g_grainPY = -(cap + ws_ + tb);   // the view's grain phase, as on screen
        if (g_webOn) {
            WebCardPaint(m2, vw, ch);       // the browser's card: field, rounded face, ring
        } else if (g.split) {               // the split: two per-pane passes, the
            int wA = PaneW(false), xB = PaneX0(true), wB = PaneW(true);   // same paint the live
            for (int pass = 0; pass < 2; pass++) {      // view runs - focused pane LAST
                bool drawB = g.splitA ? pass == 0 : pass == 1;
                if (drawB) {
                    RECT pr{ xB, 0, vw, ch };
                    HRGN rg = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
                    Draw(m2, wB, ch, xB, g.dB, rg);
                    DeleteObject(rg);
                    PaintPaneBars(m2, g.dB, true, g.dB.vHot || g.dB.vDrag, g.dB.hHot || g.dB.hDrag);
                    TabBandPaint(m2, true);
                } else {
                    RECT pr{ 0, 0, wA, ch };
                    HRGN rg = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
                    Draw(m2, wA, ch, 0, g.d, rg);
                    DeleteObject(rg);
                    PaintPaneBars(m2, g.d, false, g.d.vHot || g.d.vDrag, g.d.hHot || g.d.hDrag);
                    TabBandPaint(m2, false);
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
            TabBandPaint(m2, false);
        }
        TabDragPaint(m2);                               // the cross-pane drag's ghost + caret
        if (g.barOn) {                                  // the floating settings bar rides the shot
            RECT br3 = AnnBarRect();                    // like the live composite paints it
            AnnBarDraw(m2, br3.left, br3.top);
        }
        if (fd.open) {                                  // the floating Find bar, over its pixels -
            FindDraw(m2, FdRect().left, FdRect().top);  // the same FdRect the live view paints,
        }                                               // so the shot IS the app's composite
        BitBlt(dc, sw + rw, cap + ws_ + tb, vw, ch, m2, 0, 0, SRCCOPY);
        SelectObject(m2, o2); DeleteObject(b2); DeleteDC(m2);
    }
    if (rw > 0) {                                       // the rail strip between sidebar and view
        HDC m5 = CreateCompatibleDC(dc);
        HBITMAP b5 = CreateCompatibleBitmap(dc, rw, ch);
        HGDIOBJ o5 = SelectObject(m5, b5);
        SetBrushOrgEx(m5, -sw, -(cap + ws_ + tb), nullptr);
        RailDraw(m5, rw, ch);
        BitBlt(dc, sw, cap + ws_ + tb, rw, ch, m5, 0, 0, SRCCOPY);
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
    if (g_askShots) {                                   // the save prompt composites the
        HDC m7 = CreateCompatibleDC(dc);                // same paint the live window runs
        HBITMAP b7 = CreateCompatibleBitmap(dc, AskW(), AskH());
        HGDIOBJ o7 = SelectObject(m7, b7);
        SetBrushOrgEx(m7, 0, 0, nullptr);
        g_grainPX = 0; g_grainPY = 0;
        AskDialogDraw(m7, AskW(), AskH());
        BitBlt(dc, max(0, (w - AskW()) / 2), max(cap, (h - AskH()) / 2), AskW(), AskH(), m7, 0, 0, SRCCOPY);
        SelectObject(m7, o7); DeleteObject(b7); DeleteDC(m7);
    }
    if (!g.full && sh > 0) {                   // the footer strip
        HDC m3 = CreateCompatibleDC(dc);
        HBITMAP b3 = CreateCompatibleBitmap(dc, w, sh);
        HGDIOBJ o3 = SelectObject(m3, b3);
        SetBrushOrgEx(m3, 0, -(cap + ws_ + tb + ch), nullptr);
        StatusDraw(m3, w, sh);
        BitBlt(dc, 0, cap + ws_ + tb + ch, w, sh, m3, 0, 0, SRCCOPY);
        SelectObject(m3, o3); DeleteObject(b3); DeleteDC(m3);
    }
    if (!g.full && tb > 0) {                   // the row under the caption: reader tools,
        HDC m4 = CreateCompatibleDC(dc);       // or - one nav band lower - the tab row
        HBITMAP b4 = CreateCompatibleBitmap(dc, w, tb);
        HGDIOBJ o4 = SelectObject(m4, b4);
        SetBrushOrgEx(m4, 0, -(cap + ws_), nullptr);
        BarDraw(m4, w, tb);
        BitBlt(dc, 0, cap + ws_, w, tb, m4, 0, 0, SRCCOPY);
        SelectObject(m4, o4); DeleteObject(b4); DeleteDC(m4);
    }
    if (!g.full && g_webOn && cap > cap0) {    // the reader's toolbar row, one PDF button
        HDC m9 = CreateCompatibleDC(dc);
        HBITMAP b9 = CreateCompatibleBitmap(dc, w, cap - cap0);
        HGDIOBJ o9 = SelectObject(m9, b9);
        SetBrushOrgEx(m9, 0, -cap0, nullptr);
        WebToolDraw(m9, w, cap - cap0);
        BitBlt(dc, 0, cap0, w, cap - cap0, m9, 0, 0, SRCCOPY);
        SelectObject(m9, o9); DeleteObject(b9); DeleteDC(m9);
    }
    if (!g.full && ws_ > 0) {                  // the browser's Safari toolbar, topmost
        HDC m8 = CreateCompatibleDC(dc);
        HBITMAP b8 = CreateCompatibleBitmap(dc, w, ws_);
        HGDIOBJ o8 = SelectObject(m8, b8);
        SetBrushOrgEx(m8, 0, -cap, nullptr);
        WebStripPaint(m8, w, ws_);
        BitBlt(dc, 0, cap, w, ws_, m8, 0, 0, SRCCOPY);
        SelectObject(m8, o8); DeleteObject(b8); DeleteDC(m8);
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

// ---------------------------------------------------------------------------
// Headless extension self-test: docsy --exttest <id|store-url> [out=path]
//                            docsy --extdiag
// Runs the exact road a store install runs in the GUI - engine options with
// extensions on, a live profile, the CRX download, the unpack, then
// AddBrowserExtension, then a listing that proves the extension is really
// there - with no window and no user, writes a report, and exits 0 (pass) /
// 1 (failed) / 2 (timed out). CI runs it on a real machine, so "extensions
// always fail" stops being one person's desktop and becomes a fact with an
// HRESULT; a user who can only say "fails" gets one command that says all.
// ---------------------------------------------------------------------------
static struct {
    HWND w = nullptr;
    wrl::ComPtr<ICoreWebView2Controller> ctr;
    std::wstring id, outPath, report;
    bool fin = false;
    int exitCode = 2;
} g_xt;

static const wchar_t* WxHrName(HRESULT hr) {
    if (hr == S_OK) return L"S_OK";
    if (hr == E_NOINTERFACE) return L"E_NOINTERFACE - this runtime has no extension API (update Microsoft Edge)";
    if (hr == E_ACCESSDENIED) return L"E_ACCESSDENIED - the package carries a '_' name the engine reserves";
    if (hr == E_INVALIDARG) return L"E_INVALIDARG";
    if (hr == E_FAIL) return L"E_FAIL - the engine refused the package (often the Manifest V2 cutoff)";
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) return L"ERROR_NOT_SUPPORTED - the engine came up with extensions off";
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return L"ERROR_FILE_NOT_FOUND - manifest.json missing or invalid";
    if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) return L"ERROR_INVALID_STATE - the browser process refused extension ops (another instance's engine, machine policy, inherited WEBVIEW2_* env vars, or this runtime build); one engine restart runs automatically";
    return L"";
}
static void WxTestRep(const wchar_t* fmt, ...) {
    wchar_t msg[700]; va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap); va_end(ap);
    g_xt.report += msg; g_xt.report += L"\r\n";
    WxLogf(L"test: %s", msg);
}
static void WxTestFinish(int code) {
    if (g_xt.fin) return;
    g_xt.fin = true;
    g_xt.exitCode = code;
    {   wchar_t n[64]; swprintf_s(n, L"exit code: %d\r\n", code); g_xt.report += n; }
    std::string raw;                                    // the fetch/download steps speak
    PWSTR la = nullptr;                                 // only in webext.log: the tail rides along
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);
    if (la) {
        WxReadFile((std::wstring(la) + L"\\Docsy\\webext.log"), raw);
        std::wstring p = g_xt.outPath.empty() ? (std::wstring(la) + L"\\Docsy\\exttest.txt") : g_xt.outPath;
        CoTaskMemFree(la);
        if (raw.size() > 1) {
            size_t n = raw.size() / 2, from = n > 5000 ? n - 5000 : 0;    // UTF-16LE tail
            g_xt.report += L"\r\n---- webext.log (tail) ----\r\n";
            for (size_t i = from; i < n; i++)
                g_xt.report += (wchar_t)((unsigned char)raw[i * 2] | ((wchar_t)(unsigned char)raw[i * 2 + 1] << 8));
        }
        size_t sl = p.find_last_of(L'\\');
        if (sl != std::wstring::npos) WxMkdirs(p.substr(0, sl));
        std::string u8 = WxU(g_xt.report);
        std::vector<unsigned char> d; const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        d.insert(d.end(), bom, bom + 3); d.insert(d.end(), u8.begin(), u8.end());
        WxWriteFile(p, d.data(), d.size());
    }
    if (g_xt.ctr) { g_xt.ctr->Close(); g_xt.ctr.Reset(); }
    PostMessageW(g_xt.w, WM_CLOSE, 0, 0);
}
static void WxTestList(ICoreWebView2Profile7* p, std::function<void(const std::vector<std::wstring>&)> done) {
    if (!p) { done({}); return; }
    HRESULT hr = p->GetBrowserExtensions(
        Microsoft::WRL::Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [done](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                std::vector<std::wstring> ids;
                UINT n = 0;
                if (SUCCEEDED(hr) && list && SUCCEEDED(list->get_Count(&n)))
                    for (UINT i = 0; i < n; i++) {
                        wrl::ComPtr<ICoreWebView2BrowserExtension> e;
                        if (SUCCEEDED(list->GetValueAtIndex(i, &e)) && e) {
                            LPWSTR id = nullptr;
                            if (SUCCEEDED(e->get_Id(&id)) && id) { ids.push_back(id); CoTaskMemFree(id); }
                        }
                    }
                if (FAILED(hr)) WxTestRep(L"GetBrowserExtensions: 0x%08lX %s", (unsigned long)hr, WxHrName(hr));
                done(ids);
                return S_OK;
            }).Get());
    if (FAILED(hr)) { WxTestRep(L"GetBrowserExtensions refused synchronously 0x%08lX", (unsigned long)hr); done({}); }
}
static LRESULT CALLBACK WxTestProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER:
        if (wp == 1 && !g_xt.fin) { WxTestRep(L"TIMEOUT after 120s - the engine stopped answering"); WxTestFinish(2); }
        return 0;
    case WM_APP + 5: {                        // the fetch thread is back: unpacked, or refused
        std::unique_ptr<WxJob> j((WxJob*)lp);
        if (!j || g_xt.fin) return 0;
        if (!j->ok) { WxTestRep(L"download/unpack: FAILED - %s", j->err.c_str()); WxTestFinish(1); return 0; }
        WxTestRep(L"download/unpack: ok -> %s", j->dir.c_str());
        std::wstring id = j->id;
        WxAddFolderFresh(j->dir, id, [id](HRESULT hr, ICoreWebView2BrowserExtension* ext) {
            WxTestRep(L"AddBrowserExtension: 0x%08lX %s", (unsigned long)hr, WxHrName(hr));
            if (FAILED(hr) || !ext) { WxTestFinish(1); return; }
            WxTestList(g_wxProf.Get(), [id](const std::vector<std::wstring>& ids) {
                bool on = false;
                for (auto& i : ids) if (i == id) on = true;
                if (on) { WxTestRep(L"VERDICT: PASS - %s is installed and listed", id.c_str()); WxTestFinish(0); }
                else { WxTestRep(L"VERDICT: FAIL - the add answered ok, but the profile does not list it"); WxTestFinish(1); }
            });
        });
        return 0; }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(g_xt.exitCode); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static int WxExtTestMain(const std::wstring& spec, const std::wstring& out) {
    g_xt.outPath = out;
    if (!spec.empty()) {
        g_xt.id = WxStoreIdOf(spec);
        if (g_xt.id.empty() && spec.find(L"/detail/") != std::wstring::npos) {
            size_t p = spec.rfind(L'/');
            if (p != std::wstring::npos && WxIsExtId(spec.substr(p + 1))) g_xt.id = spec.substr(p + 1);
        }
        if (g_xt.id.empty() && WxIsExtId(spec)) g_xt.id = spec;
    }
    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = WxTestProc; wc.hInstance = g.hinst; wc.lpszClassName = L"DocsyExtTest";
    RegisterClassExW(&wc);
    g_xt.w = CreateWindowExW(WS_EX_TOOLWINDOW, L"DocsyExtTest", L"", WS_POPUP, 0, 0, 8, 8,
                             nullptr, nullptr, g.hinst, nullptr);
    SetTimer(g_xt.w, 1, 120000, nullptr);
    SYSTEMTIME st; GetLocalTime(&st);
    WxTestRep(L"Docsy extension self-test  %04u-%02u-%02u %02u:%02u:%02u",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    WxTestRep(L"target: %s", g_xt.id.empty() ? L"(diagnose only)" : g_xt.id.c_str());
    WxCleanWebViewEnvVars();
    WxLogOtherDocsy();
    WxLogPolicies();
    LPWSTR ver = nullptr;
    if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver)) && ver) {
        g_webVer = ver; CoTaskMemFree(ver);
    }
    WxTestRep(L"runtime: %s", g_webVer.empty() ? L"(none - the WebView2 runtime is missing)" : g_webVer.c_str());
    if (g_webVer.empty()) { WxTestFinish(1); }
    else {
        PWSTR la = nullptr; SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);
        std::wstring udf = (la ? std::wstring(la) : std::wstring(L".")) + L"\\Docsy\\WebView2";
        if (la) CoTaskMemFree(la);
        WxTestRep(L"user data folder: %s", udf.c_str());
        auto envOpts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        bool extOn = false;
        wrl::ComPtr<ICoreWebView2EnvironmentOptions6> o6;
        if (envOpts && SUCCEEDED(envOpts.As(&o6)) && o6)
            extOn = SUCCEEDED(o6->put_AreBrowserExtensionsEnabled(TRUE));
        wrl::ComPtr<ICoreWebView2EnvironmentOptions> oBase;         // the base interface carries
        if (envOpts && SUCCEEDED(envOpts.As(&oBase)) && oBase)      // the additional arguments
            oBase->put_AdditionalBrowserArguments(
                L"--disable-features=ExtensionManifestV2Unsupported,ExtensionManifestV2Disabled");
        WxTestRep(L"extensions flag: %s, mv2 keep-alive arguments set", extOn ? L"on" : L"COULD NOT SET");
        HRESULT hrE = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), envOpts.Get(),
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(hr) || !env) { WxTestRep(L"environment: FAILED 0x%08lX %s", (unsigned long)hr, WxHrName(hr)); WxTestFinish(1); return S_OK; }
                    wrl::ComPtr<ICoreWebView2Environment> e = env;
                    e->CreateCoreWebView2Controller(g_xt.w,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                                if (FAILED(hr) || !c) { WxTestRep(L"controller: FAILED 0x%08lX %s", (unsigned long)hr, WxHrName(hr)); WxTestFinish(1); return S_OK; }
                                WxTestRep(L"controller: created");
                                g_xt.ctr = c;
                                c->put_IsVisible(FALSE);
                                wrl::ComPtr<ICoreWebView2> w;
                                c->get_CoreWebView2(&w);
                                wrl::ComPtr<ICoreWebView2_13> w13; wrl::ComPtr<ICoreWebView2Profile> pr;
                                wrl::ComPtr<ICoreWebView2Profile7> p7;
                                if (w && SUCCEEDED(w.As(&w13)) && w13 && SUCCEEDED(w13->get_Profile(&pr)) && pr &&
                                    SUCCEEDED(pr.As(&p7)) && p7) {
                                    g_wxProf = p7;               // the real install road uses this handle
                                    WxTestRep(L"profile: ICoreWebView2Profile7 ok");
                                } else {
                                    WxTestRep(L"profile: NO ICoreWebView2Profile7 - this runtime cannot install extensions");
                                    WxTestFinish(1); return S_OK;
                                }
                                WxTestList(p7.Get(), [](const std::vector<std::wstring>& ids) {
                                    WxTestRep(L"profile carries %u extension(s) already", (unsigned)ids.size());
                                    for (auto& i : ids) WxTestRep(L"  installed: %s", i.c_str());
                                    if (g_xt.id.empty()) { WxTestRep(L"VERDICT: engine ok (diagnose-only run)"); WxTestFinish(0); return; }
                                    std::thread([]() {          // the same thread-and-post the GUI install runs
                                        WxJob* j = new WxJob; j->id = g_xt.id;
                                        WxFetch(*j, g_webVer, WxRoot());
                                        PostMessageW(g_xt.w, WM_APP + 5, 0, (LPARAM)j);
                                    }).detach();
                                });
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());
        if (FAILED(hrE)) { WxTestRep(L"environment refused synchronously 0x%08lX", (unsigned long)hrE); WxTestFinish(1); }
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return g_xt.exitCode;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Gdiplus::GdiplusStartupInput gi; ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gi, nullptr);
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool shot = argc >= 4 && !lstrcmpW(argv[1], L"--shot");
    g.quiet = shot; g.hinst = hi; g_sync = shot;   // shots render synchronously: the
                                                   // preview composites one frame directly
    // --exttest <id|store-url> [out=path] / --extdiag: the headless extension
    // self-test - it builds the same environment the GUI builds, runs the same
    // install road a store click runs, and reports PASS/FAIL with an HRESULT.
    // CI runs it on a real machine; a mute "fails" report becomes a fact.
    if (argc >= 2 && (!lstrcmpW(argv[1], L"--exttest") || !lstrcmpW(argv[1], L"--extdiag"))) {
        std::wstring spec = (!lstrcmpW(argv[1], L"--extdiag") || argc < 3) ? std::wstring() : std::wstring(argv[2]);
        std::wstring out;
        for (int a = 2; a < argc; a++) if (wcsncmp(argv[a], L"out=", 4) == 0) out = argv[a] + 4;
        return WxExtTestMain(spec, out);
    }

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

    WNDCLASSEXW bc{ sizeof bc };                    // the toolbar row: reader tools, or the
    bc.style = CS_DBLCLKS;                          // browser's tab row (a double-click on
    bc.lpfnWndProc = BarProc; bc.hInstance = hi;    // bare sheet opens a tab, like the strip)
    bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    bc.lpszClassName = L"DocsyBar";
    RegisterClassExW(&bc);

    WNDCLASSEXW wtc{ sizeof wtc };                  // browser mode's top toolbar (one button: PDF)
    wtc.lpfnWndProc = WebToolProc; wtc.hInstance = hi;
    wtc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wtc.lpszClassName = L"DocsyWebTool";
    RegisterClassExW(&wtc);

    WNDCLASSEXW xpc{ sizeof xpc };                  // an extension's popup window
    xpc.style = CS_DROPSHADOW;
    xpc.lpfnWndProc = WxPopProc; xpc.hInstance = hi;
    xpc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    xpc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    xpc.lpszClassName = L"DocsyExtPopup";
    RegisterClassExW(&xpc);

    WNDCLASSEXW wbc{ sizeof wbc };                  // the browser's Safari toolbar (top): the
    wbc.style = CS_DBLCLKS;                         // logo, back/forward, the search pill,
    wbc.lpfnWndProc = WebStripProc; wbc.hInstance = hi;
    wbc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wbc.lpszClassName = L"DocsyWebStrip";
    RegisterClassExW(&wbc);

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

    WNDCLASSEXW qc{ sizeof qc };                    // the save/quit prompt (KillerDialog)
    qc.lpfnWndProc = AskDlgProc; qc.hInstance = hi;
    qc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    qc.lpszClassName = L"DocsyAskDlg";
    RegisterClassExW(&qc);

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
        if (lstrcmpW(argv[3], L"-") && !TabOpenInto(false, argv[3])) return 2;
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
            if (!wcsncmp(argv[a], L"fileB=", 6)) TabOpenInto(true, argv[a] + 6);
        // CI preview seed 6b: tab2=<path> opens a second document as a new tab
        // in pane A through the real TabOpenInto, so the release screenshot
        // shows the band KillerPDF shows from its second document - the front
        // tab with its accent stripe and dirty dot, the neighbor on the field,
        // the hairline and the + at the card's edge
        for (int a = 3; a < argc; a++)
            if (!wcsncmp(argv[a], L"tab2=", 5)) TabOpenInto(false, argv[a] + 5);
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
        // CI preview seed 9: save=<path> writes the CURRENT document - demo
        // marks and all - through the real writer, no dialogs (shot mode is
        // quiet). The release then carries a file docsy itself produced, and
        // the reopen shot next to it proves what a reader finds inside.
        std::wstring saveErr;
        for (int a = 3; a < argc; a++)
            if (!wcsncmp(argv[a], L"save=", 5)) {
                DocState& D = DVF();
                if (D.n && !WriteAnnotatedPdf(D, argv[a] + 5, saveErr))
                    OutputDebugStringW((L"save seed: " + saveErr + L"\n").c_str());
            }
        // CI preview seed 10: ask=save composites the save prompt's paint -
        // KillerDialog's chrome with the amber "!" ring, the Replace message
        // and the three-button row - over the view, the same AskDialogDraw the
        // live window runs. annot=demo before it gives the shot a dirty
        // document, the only state the prompt exists in.
        for (int a = 3; a < argc; a++)
            if (!wcscmp(argv[a], L"ask=save")) {
                DocState& D = DVF();
                g_ask = AskDlg{};
                g_ask.qualifier = L" - Save";
                g_ask.message = std::wstring(L"Replace ")
                              + (D.n ? D.name : std::wstring(L"sample.pdf")) + L"?";
                g_ask.btn[0] = L"Cancel"; g_ask.btn[1] = L"Save As"; g_ask.btn[2] = L"Overwrite";
                g_ask.def = 2;
                AskMeasure();
                g_askShots = true;
            }
        // CI preview seed 11: web=1 dresses the window in the browser mode's
        // chrome and nothing else - the Safari toolbar, the reader's tab strip with
        // the green rule on the front tab, and the empty rounded content card.
        // The two web tabs carry titles but NO controller, so no engine is
        // created, nothing loads and no network is touched - the frame is as
        // deterministic as the reader's own (WebToggle itself is barred in shot
        // mode for exactly that reason: a real tab builds the environment).
        for (int a = 3; a < argc; a++)
            if (!wcscmp(argv[a], L"web=1")) {
                WebTab t1; t1.id = g_wtIds++;
                t1.url = L"https://duckduckgo.com/"; t1.title = L"DuckDuckGo";
                WebTab t2; t2.id = g_wtIds++;
                t2.url = L"https://github.com/etb190/docsy";
                t2.title = L"GitHub - etb190/docsy";
                g_wt.push_back(std::move(t1)); g_wt.push_back(std::move(t2));
                g_wtCur = 0;
                g_webOn = true;
                if (g_wtool) ShowWindow(g_wtool, SW_SHOWNOACTIVATE);
                if (g_wstrip) ShowWindow(g_wstrip, SW_SHOWNOACTIVATE);
                LayoutMain();
                WebBarLayout();
                if (g_wedit) SetWindowTextW(g_wedit, L"");   // the edit's pixels are not part of
                                                             // the composite: an empty pill paints
                                                             // the field's centered placeholder instead
                WebSideLayout(); WebSideSync();
                WebRepaintChrome();
                if (g.view) InvalidateRect(g.view, nullptr, FALSE);
                UpdateTitle();
            }
        GetClientRect(g.main, &c);             // the seeds may have resized (split=on grows
        for (int k = 0; k <= DVF().n; k++) if (!SidePump()) break;   // the window like the real
        return Shot(argv[2], c.right, c.bottom);                     // toggle does) - re-read it
    }

    ShowWindow(g.main, show);
    UpdateWindow(g.main);
    if (argc > 1) TabOpenInto(false, argv[1]);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
