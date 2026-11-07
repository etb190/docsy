// Docsy - minimal, fast, native Win32 PDF reader.
// Uses the PDF engine built into Windows (Windows.Data.Pdf): no bundled renderer.
// The main window hosts two children: the page view (right) and the thumbnail
// sidebar (left; drag the splitter between them to resize it). The sidebar is
// fully virtualized: only thumbnails actually in view are rendered (a few per
// timer tick) and off-screen ones are evicted, so a 2,000-page book scrolls as
// light as a 10-page one. No stock scrollbars anywhere: scrolling is wheel,
// drag-pan and keys, and the only bar on screen is a slim KillerPDF-style
// overlay thumb (4px wide, 8px under the cursor) owning an invisible 12px gutter.
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
using std::min; using std::max;
#include <gdiplus.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.UI.h>

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
    std::unordered_map<int, Ent> cache;
    unsigned long long tick = 0;
    HDC bdc = nullptr; HBITMAP bbm = nullptr, bold = nullptr; int bw = 0, bh = 0;
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

static int PW(int i) { return max(1, (int)std::lround(g.pw[i] * g.zoom * g.dpi)); }
static int PH(int i) { return max(1, (int)std::lround(g.ph[i] * g.zoom * g.dpi)); }
static int Gap() { return max(4, (int)(10 * g.dpi)); }
static int SBar() { return (int)(12 * g.dpi); }   // overlay bar gutter: KillerPDF's ScrollBarThickness

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
// Two surfaces share the tile: the page margins (lighter grey) and the sidebar (darker).
static HBRUSH g_grainBrush[2] = { nullptr, nullptr };
static HBITMAP g_grainBmp[2] = { nullptr, nullptr };

static HBRUSH GrainBrush(int which) {
    if (g_grainBrush[which]) return g_grainBrush[which];
    const int S = 256;
    const double Spread = 150.0, Soften = 0.10, Contrast = 1.35;
    const double Alpha = 70.0 / 255.0 * 0.24;   // tile alpha 70 times the dark-theme GrainOpacity
    const BYTE back[2][3] = { { 60, 63, 65 }, { 38, 40, 43 } };  // margins, sidebar
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

static void SideSync();
static void LayoutMain();

static void UpdateTitle() {
    static wchar_t last[600];
    int c = g.n ? PageAt(g.sy + g.ch / 3) : -1;
    if (c != g.cur) { g.cur = c; SideSync(); }
    wchar_t t[600];
    if (!g.n) lstrcpyW(t, L"Docsy");
    else swprintf_s(t, L"%s  -  %d / %d  -  %d%%  -  Docsy", g.name.c_str(), c + 1, g.n, (int)std::lround(g.zoom * 100));
    if (g.main && lstrcmpW(t, last)) { lstrcpyW(last, t); SetWindowTextW(g.main, t); }
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
    int hi = (int)(234 * g.dpi);                                      // nMaxPages: 200px thumb + margins + bar
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
    int avail = sd.cw - SBar();                    // the bar's gutter is reserved, KillerPDF-style
    if (!g.resizing) {                             // splitter drags keep the old pitch and thumbs;
        int tw = std::clamp(avail - 2 * (int)(16 * g.dpi), 1, (int)(200 * g.dpi));  // 8+8 margins, 200px cap
        if (sd.tw != tw) {                         // the release rebuilds at the final width
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

// Keeps the sidebar highlight in step with the view and follows the current page.
static void SideSync() {
    if (!g.side || !g.n) { sd.cur = -1; return; }
    int c = g.cur;
    if (c == sd.cur) return;
    sd.cur = c;
    if (c >= 0 && sd.docH > 0 && GetTickCount64() >= sd.hold) {
        int y0 = sd.top[c], y1 = sd.top[c + 1];
        if (y0 < sd.scroll) SideScrollTo(y0 - (int)(4 * g.dpi));
        else if (y1 > sd.scroll + sd.ch) SideScrollTo(y1 - sd.ch + (int)(4 * g.dpi));
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
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
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
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
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
        g.vDrag = g.hDrag = g.drag = false;
        ReleaseCapture();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g.n) {
            if (g.vDrag || g.hDrag || g.vHot || g.hHot) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (g.sideOn && p.x < (int)(5 * g.dpi)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
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
    sd.w = sw;
    if (g.side) {
        MoveWindow(g.side, 0, 0, sw, h, TRUE);
        if (sw) {
            RECT sr; GetClientRect(g.side, &sr);
            if (sr.right != sd.cw || sr.bottom != sd.ch) { sd.cw = sr.right; sd.ch = sr.bottom; SideLayout(); }
        }
    }
    MoveWindow(g.view, sw, 0, w - sw, h, TRUE);
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
        return 0;
    }
    case WM_SIZE:
        LayoutMain();
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp) / 96.0;
        RECT* r = (RECT*)lp;
        ClearCache();
        if (g_uifont) { DeleteObject(g_uifont); g_uifont = nullptr; }
        if (g_sidefont) { DeleteObject(g_sidefont); g_sidefont = nullptr; }
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
        case VK_F9: ToggleSide(); break;
        }
        return 0;
    }
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
    if (sw) SideDraw(dc, sw, h, 0);
    Draw(dc, w - sw, h, sw);
    PaintViewBars(dc, w - sw, h, sw);
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
    vc.lpfnWndProc = ViewProc; vc.hInstance = hi;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.lpszClassName = L"DocsyView";
    RegisterClassExW(&vc);

    WNDCLASSEXW sc{ sizeof sc };
    sc.lpfnWndProc = SideProc; sc.hInstance = hi;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.lpszClassName = L"DocsySide";
    RegisterClassExW(&sc);

    DWORD style = WS_OVERLAPPEDWINDOW;
    CreateWindowExW(0, L"Docsy", L"Docsy", style, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                    nullptr, nullptr, hi, nullptr);
    if (!g.main) return 1;

    if (shot) {
        if (lstrcmpW(argv[3], L"-") && !Open(argv[3])) return 2;
        RECT r{ 0, 0, 880, 680 };
        AdjustWindowRectEx(&r, style, FALSE, 0);
        SetWindowPos(g.main, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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
