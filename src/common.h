#pragma once
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
static void WebCardRect(RECT& c, int w, int h);   // the card's rect: the reader's own container,
                                          // edge to edge - the webview rides inside it
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
            else if (hit >= 1000) WebNewTab(L"https://www.google.com/", true);
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
            if (hit < 0) WebNewTab(L"https://www.google.com/", true);
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

