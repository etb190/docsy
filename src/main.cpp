// Docsy - minimal, fast, native Win32 PDF reader.
// Uses the PDF engine built into Windows (Windows.Data.Pdf): no bundled renderer.
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
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;
    HICON logo = nullptr;
    PdfDocument doc{ nullptr };
    std::wstring name;
    int n = 0;
    std::vector<float> pw, ph;          // page size in DIPs
    std::vector<int> top;               // pixel top of each page
    double zoom = 1.0, dpi = 1.0;       // zoom: pixels per DIP (x dpi scale)
    bool fit = true, quiet = false, drag = false, busy = false;
    int sx = 0, sy = 0, cw = 0, ch = 0, docW = 0, docH = 0, gap = 10, maxw = 0, lastVis = 0;
    POINT dp{};
    std::unordered_map<int, Ent> cache;
    unsigned long long tick = 0;
    HDC bdc = nullptr; HBITMAP bbm = nullptr, bold = nullptr; int bw = 0, bh = 0;
};
static App g;

static int PW(int i) { return max(1, (int)std::lround(g.pw[i] * g.zoom * g.dpi)); }
static int PH(int i) { return max(1, (int)std::lround(g.ph[i] * g.zoom * g.dpi)); }
static int Gap() { return max(4, (int)(10 * g.dpi)); }

static void ClearCache() {
    for (auto& kv : g.cache) DeleteObject(kv.second.b);
    g.cache.clear();
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

static int PageAt(int y) {
    if (!g.n) return 0;
    auto it = std::upper_bound(g.top.begin(), g.top.begin() + g.n, y);
    return std::clamp((int)(it - g.top.begin()) - 1, 0, g.n - 1);
}

static void UpdateTitle() {
    static wchar_t last[600];
    wchar_t t[600];
    if (!g.n) lstrcpyW(t, L"Docsy");
    else swprintf_s(t, L"%s  -  %d / %d  -  %d%%  -  Docsy", g.name.c_str(),
                    PageAt(g.sy + g.ch / 3) + 1, g.n, (int)std::lround(g.zoom * 100));
    if (lstrcmpW(t, last)) { lstrcpyW(last, t); SetWindowTextW(g.hwnd, t); }
}

static void UpdateScroll() {
    g.busy = true;
    for (int k = 0; k < 3; k++) {
        g.sy = std::clamp(g.sy, 0, max(0, g.docH - g.ch));
        g.sx = std::clamp(g.sx, 0, max(0, g.docW - g.cw));
        SCROLLINFO si{ sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS };
        si.nMin = 0; si.nMax = max(0, g.docH - 1); si.nPage = (UINT)g.ch; si.nPos = g.sy;
        SetScrollInfo(g.hwnd, SB_VERT, &si, TRUE);
        si.nMax = max(0, g.docW - 1); si.nPage = (UINT)g.cw; si.nPos = g.sx;
        SetScrollInfo(g.hwnd, SB_HORZ, &si, TRUE);
        RECT r; GetClientRect(g.hwnd, &r);
        if (r.right == g.cw && r.bottom == g.ch) break;
        g.cw = r.right; g.ch = r.bottom;
    }
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
    InvalidateRect(g.hwnd, nullptr, FALSE);
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
    InvalidateRect(g.hwnd, nullptr, FALSE);
    UpdateTitle();
}

static void ApplyFit(bool pageFit = false) {
    if (!g.n || g.cw <= 0) return;
    float mw = 0; for (float w : g.pw) mw = max(mw, w);
    int W = g.cw;
    if (!(GetWindowLongW(g.hwnd, GWL_STYLE) & WS_VSCROLL)) W -= GetSystemMetricsForDpi(SM_CXVSCROLL, (UINT)(g.dpi * 96));
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
    SetScrollPos(g.hwnd, SB_VERT, y, TRUE);
    SetScrollPos(g.hwnd, SB_HORZ, x, TRUE);
    InvalidateRect(g.hwnd, nullptr, FALSE);
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
        if (g.cw > 0) ApplyFit(); else Layout();
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return true;
    } catch (...) {
        if (!g.quiet) MessageBoxW(g.hwnd, L"Could not open this file (invalid, encrypted or unsupported PDF).", L"Docsy", MB_ICONWARNING);
        return false;
    }
}

static void OpenDlg() {
    wchar_t f[MAX_PATH * 2] = L"";
    OPENFILENAMEW o{}; o.lStructSize = sizeof o; o.hwndOwner = g.hwnd;
    o.lpstrFilter = L"PDF files\0*.pdf\0All files\0*.*\0";
    o.lpstrFile = f; o.nMaxFile = (DWORD)_countof(f);
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) Open(f);
}

static void Draw(HDC dc, int w, int h) {
    HBRUSH br = CreateSolidBrush(RGB(60, 63, 65));
    RECT r{ 0, 0, w, h };
    FillRect(dc, &r, br); DeleteObject(br);
    if (!g.n) {
        int sz = (int)(128 * g.dpi);
        if (!g.logo) g.logo = (HICON)LoadImageW(g.hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR);
        int cy = h / 2;
        if (g.logo) DrawIconEx(dc, (w - sz) / 2, cy - sz - (int)(6 * g.dpi), g.logo, sz, sz, 0, nullptr, DI_NORMAL);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(200, 200, 205));
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        RECT tr{ 0, cy + (int)(8 * g.dpi), w, h };
        DrawTextW(dc, L"Docsy  -  Ctrl+O or drop a PDF here", -1, &tr, DT_CENTER | DT_TOP | DT_SINGLELINE);
        return;
    }
    HDC m = CreateCompatibleDC(dc);
    int first = PageAt(g.sy), last = first;
    int offx = g.docW < w ? (w - g.docW) / 2 : -g.sx;
    for (int i = first; i < g.n && g.top[i] < g.sy + h; i++) {
        last = i;
        int pw = PW(i), ph = PH(i);
        int x = offx + g.gap + (g.maxw - pw) / 2, y = g.top[i] - g.sy;
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

static int ScrollMsg(int bar, WPARAM wp) {
    SCROLLINFO si{ sizeof si, SIF_ALL };
    GetScrollInfo(g.hwnd, bar, &si);
    int p = si.nPos, line = (int)(40 * g.dpi);
    switch (LOWORD(wp)) {
    case SB_LINEUP: p -= line; break;
    case SB_LINEDOWN: p += line; break;
    case SB_PAGEUP: p -= (int)si.nPage; break;
    case SB_PAGEDOWN: p += (int)si.nPage; break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: p = si.nTrackPos; break;
    case SB_TOP: p = 0; break;
    case SB_BOTTOM: p = INT_MAX / 2; break;
    }
    return p;
}

static void ZoomBy(double f) { SetZoom(g.zoom * f, false, g.cw / 2, g.ch / 2); }

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        g.hwnd = h; g.dpi = GetDpiForWindow(h) / 96.0;
        DragAcceptFiles(h, TRUE);
        return 0;
    case WM_SIZE: {
        RECT r; GetClientRect(h, &r);
        g.cw = r.right; g.ch = r.bottom;
        if (g.busy || g.cw <= 0 || g.ch <= 0) return 0;
        if (g.fit) ApplyFit(); else Layout();
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp) / 96.0;
        RECT* r = (RECT*)lp;
        ClearCache();
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g.fit) ApplyFit(); else Layout();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (g.cw > 0 && g.ch > 0) {
            EnsureBack(dc, g.cw, g.ch);
            Draw(g.bdc, g.cw, g.ch);
            BitBlt(dc, 0, 0, g.cw, g.ch, g.bdc, 0, 0, SRCCOPY);
            SetTimer(h, 1, 5, nullptr);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        KillTimer(h, 1); Prefetch(); return 0;
    case WM_VSCROLL: ScrollTo(g.sx, ScrollMsg(SB_VERT, wp)); return 0;
    case WM_HSCROLL: ScrollTo(ScrollMsg(SB_HORZ, wp), g.sy); return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        if (LOWORD(wp) & MK_CONTROL) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &p);
            SetZoom(g.zoom * std::pow(1.12, d / 120.0), false, p.x, p.y);
        } else if (LOWORD(wp) & MK_SHIFT) ScrollTo(g.sx - d * (int)(g.dpi * 80) / 120, g.sy);
        else ScrollTo(g.sx, g.sy - d * (int)(g.dpi * 80) / 120);
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
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(h); g.drag = true; g.dp = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; return 0;
    case WM_MOUSEMOVE:
        if (g.drag) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScrollTo(g.sx - (p.x - g.dp.x), g.sy - (p.y - g.dp.y)); g.dp = p;
        }
        return 0;
    case WM_LBUTTONUP:
        g.drag = false; ReleaseCapture(); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g.n) { SetCursor(LoadCursorW(nullptr, g.drag ? IDC_SIZEALL : IDC_HAND)); return TRUE; }
        break;
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp; wchar_t f[MAX_PATH * 2];
        if (DragQueryFileW(d, 0, f, (UINT)_countof(f))) Open(f);
        DragFinish(d);
        return 0;
    }
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
    Draw(dc, w, h); GdiFlush();
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
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL;
    CreateWindowExW(0, L"Docsy", L"Docsy", style, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                    nullptr, nullptr, hi, nullptr);
    if (!g.hwnd) return 1;

    if (shot) {
        if (lstrcmpW(argv[3], L"-") && !Open(argv[3])) return 2;
        RECT r{ 0, 0, 880, 680 };
        AdjustWindowRectEx(&r, style, FALSE, 0);
        SetWindowPos(g.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT c; GetClientRect(g.hwnd, &c); g.cw = c.right; g.ch = c.bottom;
        ApplyFit();
        return Shot(argv[2], g.cw, g.ch);
    }

    ShowWindow(g.hwnd, show);
    UpdateWindow(g.hwnd);
    if (argc > 1) Open(argv[1]);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
