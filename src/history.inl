#pragma once
#include <string>
#include <vector>
#include <ctime>
#include <algorithm>
#include <cstdint>

// ---- browsing history data structures & persistence ----

struct HistEntry {
    unsigned long long id = 0;
    std::wstring url;
    std::wstring title;
    time_t time = 0;
};

static std::vector<HistEntry> g_history;
static bool g_histLoaded = false;
static unsigned long long g_histNextId = 1;

static std::wstring HistFilePath() {
    PWSTR la = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);
    std::wstring r = (la ? std::wstring(la) : std::wstring(L".")) + L"\\Docsy\\history.txt";
    if (la) CoTaskMemFree(la);
    return r;
}

static void HistSave() {
    std::string txt;
    // Keep up to 5000 items
    size_t start = (g_history.size() > 5000) ? (g_history.size() - 5000) : 0;
    for (size_t i = start; i < g_history.size(); i++) {
        const auto& e = g_history[i];
        if (e.url.empty()) continue;
        txt += std::to_string((long long)e.time) + "\t" + WxU(BmEsc(e.url)) + "\t" + WxU(BmEsc(e.title)) + "\n";
    }
    std::wstring p = HistFilePath();
    std::wstring tmp = p + L".tmp";
    WxMkdirs(p.substr(0, p.find_last_of(L'\\')));
    if (WxWriteFile(tmp, (const unsigned char*)txt.data(), txt.size()))
        MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

static void HistLoad() {
    if (g_histLoaded) return;
    g_histLoaded = true;
    std::string raw;
    if (!WxReadFile(HistFilePath(), raw) || raw.empty()) return;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t e = raw.find('\n', pos);
        if (e == std::string::npos) e = raw.size();
        std::string line = raw.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;

        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        std::string stime = line.substr(0, t1);
        std::string surl = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - (t1 + 1));
        std::string stitle = (t2 == std::string::npos) ? "" : line.substr(t2 + 1);

        HistEntry item;
        item.id = g_histNextId++;
        item.time = (time_t)_strtoi64(stime.c_str(), nullptr, 10);
        item.url = BmUnesc(BmWide(surl));
        item.title = BmUnesc(BmWide(stitle));
        if (!item.url.empty()) g_history.push_back(item);
    }
    std::sort(g_history.begin(), g_history.end(), [](const HistEntry& a, const HistEntry& b) {
        return a.time > b.time;
    });
}

static void HistAdd(const std::wstring& url, const std::wstring& title) {
    if (url.empty() || url.rfind(L"about:", 0) == 0 || url.rfind(L"edge:", 0) == 0) return;
    HistLoad();
    time_t now = time(nullptr);
    if (!g_history.empty() && g_history[0].url == url && (now - g_history[0].time < 10)) {
        if (!title.empty()) g_history[0].title = title;
        g_history[0].time = now;
        HistSave();
        return;
    }
    HistEntry item;
    item.id = g_histNextId++;
    item.url = url;
    item.title = title.empty() ? BmHostOf(url) : title;
    item.time = now;
    g_history.insert(g_history.begin(), item);
    HistSave();
}

static void HistUpdateTitle(const std::wstring& url, const std::wstring& title) {
    if (url.empty() || title.empty()) return;
    HistLoad();
    for (size_t i = 0; i < g_history.size() && i < 10; i++) {
        if (g_history[i].url == url) {
            g_history[i].title = title;
            HistSave();
            break;
        }
    }
}

static void HistClear() {
    HistLoad();
    g_history.clear();
    HistSave();
}

static void HistDelete(unsigned long long id) {
    HistLoad();
    for (size_t i = 0; i < g_history.size(); i++) {
        if (g_history[i].id == id) {
            g_history.erase(g_history.begin() + i);
            HistSave();
            break;
        }
    }
}

static std::wstring HistFormatRelative(time_t diff) {
    if (diff < 60) return L"Just now";
    if (diff < 3600) {
        wchar_t b[32]; swprintf_s(b, L"%dm ago", (int)(diff / 60)); return b;
    }
    wchar_t b[32]; swprintf_s(b, L"%dh ago", (int)(diff / 3600)); return b;
}

static void HistDrawIcon(Gdiplus::Graphics& gx, HDC dc, const RECT& r, const std::wstring& url, const std::wstring& title) {
    std::wstring host = BmHostOf(url);
    if (!host.empty()) {
        std::wstring p = BmFavPath(host);
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            Gdiplus::Bitmap* ico = WxLoadIcon(p);
            if (ico) {
                gx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                gx.DrawImage(ico, (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top));
                delete ico;
                return;
            }
        }
    }
    BmTileDisc(gx, dc, r, title, host.empty() ? title : host);
}

// Forward declaration
static void HistFullOpen();

// ---- 1. Recent History Dropdown (Last 2 hours) ----

struct HistPopState {
    HWND h = nullptr;
    std::vector<HistEntry> recent;
    int hoverIdx = -1; // >=0: item index, -2: view all button
    RECT viewAllR{};
    std::vector<RECT> rowR;
};
static HistPopState g_hpop;
static HDC g_hpopdc = nullptr;
static HBITMAP g_hpopbm = nullptr, g_hpopold = nullptr;
static int g_hpopw = 0, g_hpoph = 0;

static void EnsureHistPopBack(HDC ref, int w, int h) {
    if (g_hpopbm && g_hpopw == w && g_hpoph == h) return;
    if (g_hpopbm) { SelectObject(g_hpopdc, g_hpopold); DeleteObject(g_hpopbm); }
    else g_hpopdc = CreateCompatibleDC(ref);
    g_hpopbm = CreateCompatibleBitmap(ref, max(1, w), max(1, h));
    g_hpopold = (HBITMAP)SelectObject(g_hpopdc, g_hpopbm);
    g_hpopw = w; g_hpoph = h;
}

static void HistPopPaint(HDC dc, int w, int h) {
    RECT full{ 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(RGB(0x20, 0x20, 0x22));
    FillRect(dc, &full, bg); DeleteObject(bg);

    HPEN ep = CreatePen(PS_SOLID, 1, RGB(0x42, 0x42, 0x44));
    HGDIOBJ op = SelectObject(dc, ep);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, w, h);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(ep);

    SetBkMode(dc, TRANSPARENT);
    Gdiplus::Graphics gx(dc);
    gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    HFONT fh = CaptionFont(g_wordfont, 12, true, L"Segoe UI");
    HFONT ft = CreateFontW(-(int)std::lround(13 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT fs = CreateFontW(-(int)std::lround(11 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    int headerH = (int)(36 * g.dpi);
    // Header icon & text
    SelectObject(dc, WebGlyphFont(13));
    SetTextColor(dc, wvb::InkDim);
    RECT icr{ (int)(12 * g.dpi), 0, (int)(32 * g.dpi), headerH };
    DrawTextW(dc, L"\uE81C", -1, &icr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, fh);
    SetTextColor(dc, wvb::Ink);
    RECT htr{ (int)(34 * g.dpi), 0, w - (int)(12 * g.dpi), headerH };
    DrawTextW(dc, L"Recent History (Last 2 hours)", -1, &htr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Divider under header
    HPEN dp = CreatePen(PS_SOLID, 1, RGB(0x32, 0x32, 0x34));
    HGDIOBJ odp = SelectObject(dc, dp);
    MoveToEx(dc, 0, headerH, nullptr); LineTo(dc, w, headerH);
    SelectObject(dc, odp); DeleteObject(dp);

    time_t now = time(nullptr);
    if (g_hpop.recent.empty()) {
        SelectObject(dc, ft);
        SetTextColor(dc, wvb::InkDim);
        RECT nr{ 0, headerH, w, h - (int)(48 * g.dpi) };
        DrawTextW(dc, L"No browsing history in the last 2 hours", -1, &nr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        int is = (int)(16 * g.dpi);
        for (size_t i = 0; i < g_hpop.rowR.size() && i < g_hpop.recent.size(); i++) {
            const RECT& rc = g_hpop.rowR[i];
            const auto& e = g_hpop.recent[i];
            bool hot = (g_hpop.hoverIdx == (int)i);
            if (hot) {
                Gdiplus::GraphicsPath p;
                WebRound(p, (float)rc.left + 4.f, (float)rc.top + 1.f, (float)rc.right - 4.f, (float)rc.bottom - 1.f, 5.0f * (float)g.dpi);
                Gdiplus::SolidBrush hb(Gdiplus::Color(255, 0x30, 0x30, 0x34));
                gx.FillPath(&hb, &p);
            }
            int ix = rc.left + (int)(12 * g.dpi);
            int iy = rc.top + ((rc.bottom - rc.top) - is) / 2;
            RECT ir{ ix, iy, ix + is, iy + is };
            HistDrawIcon(gx, dc, ir, e.url, e.title);

            int tx = ix + is + (int)(10 * g.dpi);
            int tright = rc.right - (int)(12 * g.dpi);
            RECT titR{ tx, rc.top + (int)(4 * g.dpi), tright, rc.top + (int)(22 * g.dpi) };
            SelectObject(dc, ft);
            SetTextColor(dc, wvb::Ink);
            DrawTextW(dc, e.title.c_str(), -1, &titR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT subR{ tx, rc.top + (int)(22 * g.dpi), tright, rc.bottom - (int)(3 * g.dpi) };
            SelectObject(dc, fs);
            SetTextColor(dc, wvb::InkDim);
            std::wstring sub = BmHostOf(e.url) + L" \u2022 " + HistFormatRelative(now - e.time);
            DrawTextW(dc, sub.c_str(), -1, &subR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    // Divider above footer
    int footTop = g_hpop.viewAllR.top - (int)(7 * g.dpi);
    HPEN fdp = CreatePen(PS_SOLID, 1, RGB(0x32, 0x32, 0x34));
    HGDIOBJ ofdp = SelectObject(dc, fdp);
    MoveToEx(dc, 0, footTop, nullptr); LineTo(dc, w, footTop);
    SelectObject(dc, ofdp); DeleteObject(fdp);

    // Footer button: "View all history"
    RECT vr = g_hpop.viewAllR;
    bool vhot = (g_hpop.hoverIdx == -2);
    Gdiplus::GraphicsPath vp;
    WebRound(vp, (float)vr.left, (float)vr.top, (float)vr.right, (float)vr.bottom, 5.0f * (float)g.dpi);
    Gdiplus::SolidBrush vb(Gdiplus::Color(255, vhot ? 0x38 : 0x2a, vhot ? 0x38 : 0x2a, vhot ? 0x3c : 0x2c));
    gx.FillPath(&vb, &vp);
    Gdiplus::Pen vpen(Gdiplus::Color(255, 0x48, 0x48, 0x4c), 1.0f);
    gx.DrawPath(&vpen, &vp);

    SelectObject(dc, WebGlyphFont(13));
    SetTextColor(dc, wvb::Ink);
    RECT vir{ vr.left + (int)(10 * g.dpi), vr.top, vr.left + (int)(30 * g.dpi), vr.bottom };
    DrawTextW(dc, L"\uE81C", -1, &vir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, ft);
    SetTextColor(dc, wvb::Ink);
    RECT vtr{ vr.left + (int)(32 * g.dpi), vr.top, vr.right - (int)(10 * g.dpi), vr.bottom };
    DrawTextW(dc, L"View all history", -1, &vtr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DeleteObject(ft); DeleteObject(fs);
}

static void HistDropdownClose() {
    if (g_hpop.h) {
        DestroyWindow(g_hpop.h);
        g_hpop.h = nullptr;
    }
}

static LRESULT CALLBACK HistPopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureHistPopBack(dc, r.right, r.bottom);
            HistPopPaint(g_hpopdc, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, g_hpopdc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hot = -1;
        if (PtInRect(&g_hpop.viewAllR, p)) hot = -2;
        else {
            for (size_t i = 0; i < g_hpop.rowR.size(); i++) {
                if (PtInRect(&g_hpop.rowR[i], p)) { hot = (int)i; break; }
            }
        }
        if (hot != g_hpop.hoverIdx) {
            g_hpop.hoverIdx = hot;
            if (hot != -1) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hpop.hoverIdx != -1) {
            g_hpop.hoverIdx = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&g_hpop.viewAllR, p)) {
            HistDropdownClose();
            HistFullOpen();
            return 0;
        }
        for (size_t i = 0; i < g_hpop.rowR.size() && i < g_hpop.recent.size(); i++) {
            if (PtInRect(&g_hpop.rowR[i], p)) {
                std::wstring u = g_hpop.recent[i].url;
                HistDropdownClose();
                WebGo(u);
                return 0;
            }
        }
        return 0;
    }
    case WM_MBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        for (size_t i = 0; i < g_hpop.rowR.size() && i < g_hpop.recent.size(); i++) {
            if (PtInRect(&g_hpop.rowR[i], p)) {
                std::wstring u = g_hpop.recent[i].url;
                HistDropdownClose();
                WebNewTab(u, true);
                return 0;
            }
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) HistDropdownClose();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { HistDropdownClose(); return 0; }
        break;
    case WM_DESTROY: {
        if (g_hpopbm) {
            SelectObject(g_hpopdc, g_hpopold);
            DeleteObject(g_hpopbm);
            g_hpopbm = nullptr;
            g_hpopold = nullptr;
        }
        if (g_hpopdc) {
            DeleteDC(g_hpopdc);
            g_hpopdc = nullptr;
        }
        g_hpopw = g_hpoph = 0;
        g_hpop.h = nullptr;
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void HistDropdownOpen() {
    if (g_hpop.h) { HistDropdownClose(); return; }
    HistLoad();
    time_t now = time(nullptr);
    time_t cutoff = now - 7200; // 2 hours
    g_hpop.recent.clear();
    for (const auto& e : g_history) {
        if (e.time >= cutoff) g_hpop.recent.push_back(e);
    }

    int W = (int)std::lround(350 * g.dpi);
    int headerH = (int)(36 * g.dpi);
    int rowH = (int)(44 * g.dpi);
    int footerH = (int)(46 * g.dpi);
    int n = (int)g_hpop.recent.size();
    int visN = std::min(n, 7);
    int bodyH = (visN == 0) ? (int)(50 * g.dpi) : (visN * rowH);
    int H = headerH + bodyH + footerH + (int)(6 * g.dpi);

    POINT a{ g_wbR[WB_HIST].left, g_wbR[WB_HIST].bottom + TABF(4) };
    if (g_wstrip) ClientToScreen(g_wstrip, &a);
    int x = a.x + (g_wbR[WB_HIST].right - g_wbR[WB_HIST].left) - W;
    int y = a.y;

    MONITORINFO mi{ sizeof mi };
    GetMonitorInfoW(MonitorFromPoint(POINT{ x, y }, MONITOR_DEFAULTTONEAREST), &mi);
    x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - W));

    g_hpop.rowR.clear();
    int curY = headerH + (int)(3 * g.dpi);
    for (int i = 0; i < visN; i++) {
        g_hpop.rowR.push_back(RECT{ 0, curY, W, curY + rowH });
        curY += rowH;
    }
    g_hpop.viewAllR = RECT{ (int)(10 * g.dpi), H - footerH + (int)(5 * g.dpi),
                           W - (int)(10 * g.dpi), H - (int)(9 * g.dpi) };
    g_hpop.hoverIdx = -1;

    g_hpop.h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"DocsyHistPop", L"",
                               WS_POPUP, x, y, W, H, g.main, nullptr, g.hinst, nullptr);
    if (!g_hpop.h) return;
    ShowWindow(g_hpop.h, SW_SHOW);
    SetForegroundWindow(g_hpop.h);
}

static void HistDropdownToggle() {
    if (g_hpop.h) HistDropdownClose();
    else HistDropdownOpen();
}

// ---- 2. Full History View with Real-time Search ----

struct HistWindowState {
    HWND h = nullptr;
    HWND editSearch = nullptr;
    WNDPROC editSearchPrev = nullptr;
    std::wstring searchStr;
    std::vector<HistEntry> filtered;
    int scrollY = 0;
    int totalH = 0;
    int hoverIdx = -1;
    bool hoverDel = false;
    bool clearBtnHot = false;
    RECT clearBtnR{};
    RECT searchCapsuleR{};
    std::vector<RECT> rowRects;
    std::vector<RECT> delRects;
};
static HistWindowState g_hwin;
static HDC g_hwindc = nullptr;
static HBITMAP g_hwinbm = nullptr, g_hwinold = nullptr;
static int g_hwinw = 0, g_hwinh = 0;

static void EnsureHistWinBack(HDC ref, int w, int h) {
    if (g_hwinbm && g_hwinw == w && g_hwinh == h) return;
    if (g_hwinbm) { SelectObject(g_hwindc, g_hwinold); DeleteObject(g_hwinbm); }
    else g_hwindc = CreateCompatibleDC(ref);
    g_hwinbm = CreateCompatibleBitmap(ref, max(1, w), max(1, h));
    g_hwinold = (HBITMAP)SelectObject(g_hwindc, g_hwinbm);
    g_hwinw = w; g_hwinh = h;
}

static void HistFilter() {
    HistLoad();
    g_hwin.filtered.clear();
    std::wstring q = g_hwin.searchStr;
    for (auto& c : q) c = towlower(c);
    while (!q.empty() && (q[0] == L' ' || q[0] == L'\t')) q.erase(0, 1);
    while (!q.empty() && (q.back() == L' ' || q.back() == L'\t')) q.pop_back();

    for (const auto& e : g_history) {
        if (q.empty()) {
            g_hwin.filtered.push_back(e);
        } else {
            std::wstring t = e.title, u = e.url;
            for (auto& c : t) c = towlower(c);
            for (auto& c : u) c = towlower(c);
            if (t.find(q) != std::wstring::npos || u.find(q) != std::wstring::npos) {
                g_hwin.filtered.push_back(e);
            }
        }
    }
}

static std::wstring HistDateHeader(time_t t) {
    time_t now = time(nullptr);
    struct tm tmNow{}, tmItem{};
    localtime_s(&tmNow, &now);
    localtime_s(&tmItem, &t);

    if (tmNow.tm_year == tmItem.tm_year && tmNow.tm_yday == tmItem.tm_yday)
        return L"Today";
    if (now - t < 172800 && ((tmNow.tm_yday == tmItem.tm_yday + 1) || (tmNow.tm_yday == 0 && tmItem.tm_yday >= 364)))
        return L"Yesterday";

    wchar_t buf[128];
    wcsftime(buf, 128, L"%A, %B %d, %Y", &tmItem);
    return buf;
}

static LRESULT CALLBACK HistSearchEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            if (g_hwin.h) PostMessageW(g_hwin.h, WM_CLOSE, 0, 0);
            return 0;
        }
    }
    return CallWindowProcW(g_hwin.editSearchPrev, h, m, wp, lp);
}

static void HistWinLayout(int w, int h) {
    g_hwin.rowRects.clear();
    g_hwin.delRects.clear();

    int topBarH = (int)(106 * g.dpi);
    int curY = topBarH - g_hwin.scrollY;
    int rowH = (int)(38 * g.dpi);
    int dateH = (int)(36 * g.dpi);

    std::wstring lastDate;
    for (size_t i = 0; i < g_hwin.filtered.size(); i++) {
        std::wstring d = HistDateHeader(g_hwin.filtered[i].time);
        if (d != lastDate) {
            lastDate = d;
            curY += dateH;
        }
        RECT r{ (int)(20 * g.dpi), curY, w - (int)(20 * g.dpi), curY + rowH };
        RECT delR{ r.right - (int)(32 * g.dpi), curY + (rowH - (int)(24 * g.dpi)) / 2,
                    r.right - (int)(8 * g.dpi), curY + (rowH + (int)(24 * g.dpi)) / 2 };
        g_hwin.rowRects.push_back(r);
        g_hwin.delRects.push_back(delR);
        curY += rowH;
    }
    g_hwin.totalH = (curY + g_hwin.scrollY) - topBarH;

    // Update scroll info
    SCROLLINFO si{ sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin = 0;
    si.nMax = std::max(0, g_hwin.totalH);
    si.nPage = std::max(1, h - topBarH);
    si.nPos = g_hwin.scrollY;
    SetScrollInfo(g_hwin.h, SB_VERT, &si, TRUE);
}

static void HistWinPaint(HDC dc, int w, int h) {
    RECT full{ 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(RGB(0x19, 0x19, 0x1b));
    FillRect(dc, &full, bg); DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    Gdiplus::Graphics gx(dc);
    gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    HFONT titleF = CreateFontW(-(int)std::lround(18 * g.dpi), 0, 0, 0, FW_BOLD, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT dateF = CreateFontW(-(int)std::lround(13 * g.dpi), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT rowF = CreateFontW(-(int)std::lround(13 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT smallF = CreateFontW(-(int)std::lround(11 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    int topBarH = (int)(106 * g.dpi);

    // Save DC and set clipping region for scrollable items
    HRGN listClip = CreateRectRgn(0, topBarH, w, h);
    SelectClipRgn(dc, listClip);

    // Draw date headers and rows
    int rowH = (int)(38 * g.dpi);
    int dateH = (int)(36 * g.dpi);
    int curY = topBarH - g_hwin.scrollY;
    std::wstring lastDate;

    for (size_t i = 0; i < g_hwin.filtered.size() && i < g_hwin.rowRects.size(); i++) {
        const auto& e = g_hwin.filtered[i];
        std::wstring d = HistDateHeader(e.time);
        if (d != lastDate) {
            lastDate = d;
            if (curY + dateH >= topBarH && curY <= h) {
                SelectObject(dc, dateF);
                SetTextColor(dc, RGB(0x38, 0x84, 0xff)); // Clean accent
                RECT dtr{ (int)(24 * g.dpi), curY + (int)(10 * g.dpi), w - (int)(24 * g.dpi), curY + dateH };
                DrawTextW(dc, d.c_str(), -1, &dtr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                HPEN dp = CreatePen(PS_SOLID, 1, RGB(0x2a, 0x2a, 0x2e));
                HGDIOBJ odp = SelectObject(dc, dp);
                MoveToEx(dc, (int)(24 * g.dpi), curY + dateH - 2, nullptr);
                LineTo(dc, w - (int)(24 * g.dpi), curY + dateH - 2);
                SelectObject(dc, odp); DeleteObject(dp);
            }
            curY += dateH;
        }

        const RECT& rc = g_hwin.rowRects[i];
        if (rc.bottom >= topBarH && rc.top <= h) {
            bool hot = (g_hwin.hoverIdx == (int)i);
            if (hot) {
                Gdiplus::GraphicsPath p;
                WebRound(p, (float)rc.left, (float)rc.top, (float)rc.right, (float)rc.bottom, 4.0f * (float)g.dpi);
                Gdiplus::SolidBrush hb(Gdiplus::Color(255, 0x28, 0x28, 0x2c));
                gx.FillPath(&hb, &p);
            }

            // Time HH:MM
            struct tm tmi{};
            localtime_s(&tmi, &e.time);
            wchar_t tb[32]; swprintf_s(tb, L"%02d:%02d", tmi.tm_hour, tmi.tm_min);
            SelectObject(dc, smallF);
            SetTextColor(dc, wvb::InkDim);
            RECT timeR{ rc.left + (int)(12 * g.dpi), rc.top, rc.left + (int)(54 * g.dpi), rc.bottom };
            DrawTextW(dc, tb, -1, &timeR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Favicon
            int is = (int)(16 * g.dpi);
            int ix = rc.left + (int)(62 * g.dpi);
            int iy = rc.top + (rowH - is) / 2;
            RECT ir{ ix, iy, ix + is, iy + is };
            HistDrawIcon(gx, dc, ir, e.url, e.title);

            // Title and URL
            int tx = ix + is + (int)(12 * g.dpi);
            int tright = rc.right - (hot ? (int)(40 * g.dpi) : (int)(14 * g.dpi));
            int avail = tright - tx;

            SelectObject(dc, rowF);
            SIZE sz{};
            GetTextExtentPoint32W(dc, e.title.c_str(), (int)e.title.size(), &sz);
            int titleW = std::min((int)sz.cx, (int)(avail * 0.55));
            RECT titR{ tx, rc.top, tx + titleW, rc.bottom };
            SetTextColor(dc, wvb::Ink);
            DrawTextW(dc, e.title.c_str(), -1, &titR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            int ux = tx + titleW + (int)(14 * g.dpi);
            if (ux < tright) {
                SelectObject(dc, smallF);
                SetTextColor(dc, wvb::InkDim);
                RECT urlR{ ux, rc.top, tright, rc.bottom };
                DrawTextW(dc, e.url.c_str(), -1, &urlR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            // Delete (X) button on hover
            if (hot) {
                const RECT& dr = g_hwin.delRects[i];
                if (g_hwin.hoverDel) {
                    Gdiplus::SolidBrush db(Gdiplus::Color(255, 0x48, 0x22, 0x22));
                    Gdiplus::GraphicsPath dp;
                    WebRound(dp, (float)dr.left, (float)dr.top, (float)dr.right, (float)dr.bottom, 4.0f * (float)g.dpi);
                    gx.FillPath(&db, &dp);
                }
                SelectObject(dc, WebGlyphFont(11));
                SetTextColor(dc, g_hwin.hoverDel ? RGB(0xff, 0x66, 0x66) : wvb::InkDim);
                RECT drc = dr;
                DrawTextW(dc, L"\uE711", -1, &drc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        curY += rowH;
    }

    if (g_hwin.filtered.empty()) {
        SelectObject(dc, rowF);
        SetTextColor(dc, wvb::InkDim);
        RECT er{ 0, topBarH + (int)(60 * g.dpi), w, topBarH + (int)(120 * g.dpi) };
        if (!g_hwin.searchStr.empty()) {
            std::wstring msg = L"No history matching \"" + g_hwin.searchStr + L"\"";
            DrawTextW(dc, msg.c_str(), -1, &er, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            DrawTextW(dc, L"No browsing history yet", -1, &er, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // Restore DC clipping
    SelectClipRgn(dc, nullptr);
    DeleteObject(listClip);

    // Top Bar Background (pinned, covers scrolled content underneath)
    RECT topR{ 0, 0, w, topBarH };
    HBRUSH tbg = CreateSolidBrush(RGB(0x19, 0x19, 0x1b));
    FillRect(dc, &topR, tbg); DeleteObject(tbg);

    HPEN tbp = CreatePen(PS_SOLID, 1, RGB(0x32, 0x32, 0x36));
    HGDIOBJ otbp = SelectObject(dc, tbp);
    MoveToEx(dc, 0, topBarH - 1, nullptr); LineTo(dc, w, topBarH - 1);
    SelectObject(dc, otbp); DeleteObject(tbp);

    // Title + Icon
    SelectObject(dc, WebGlyphFont(16));
    SetTextColor(dc, wvb::Blue);
    RECT icr{ (int)(24 * g.dpi), (int)(14 * g.dpi), (int)(48 * g.dpi), (int)(42 * g.dpi) };
    DrawTextW(dc, L"\uE81C", -1, &icr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, titleF);
    SetTextColor(dc, wvb::Ink);
    RECT titR{ (int)(52 * g.dpi), (int)(14 * g.dpi), (int)(240 * g.dpi), (int)(42 * g.dpi) };
    DrawTextW(dc, L"History", -1, &titR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // "Clear History" button at right
    RECT cr = g_hwin.clearBtnR;
    bool chot = g_hwin.clearBtnHot;
    Gdiplus::GraphicsPath cp;
    WebRound(cp, (float)cr.left, (float)cr.top, (float)cr.right, (float)cr.bottom, 4.0f * (float)g.dpi);
    Gdiplus::SolidBrush cb(Gdiplus::Color(255, chot ? 0x38 : 0x26, chot ? 0x22 : 0x26, chot ? 0x22 : 0x2a));
    gx.FillPath(&cb, &cp);
    Gdiplus::Pen cpen(Gdiplus::Color(255, chot ? 0xaa : 0x44, chot ? 0x44 : 0x44, chot ? 0x44 : 0x48), 1.0f);
    gx.DrawPath(&cpen, &cp);

    SelectObject(dc, WebGlyphFont(11));
    SetTextColor(dc, chot ? RGB(0xff, 0x66, 0x66) : wvb::Ink);
    RECT cir{ cr.left + (int)(8 * g.dpi), cr.top, cr.left + (int)(26 * g.dpi), cr.bottom };
    DrawTextW(dc, L"\uE74D", -1, &cir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, rowF);
    SetTextColor(dc, chot ? RGB(0xff, 0x66, 0x66) : wvb::Ink);
    RECT ctr{ cr.left + (int)(28 * g.dpi), cr.top, cr.right - (int)(8 * g.dpi), cr.bottom };
    DrawTextW(dc, L"Clear history", -1, &ctr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Search Capsule
    RECT sr = g_hwin.searchCapsuleR;
    Gdiplus::GraphicsPath sp;
    WebRound(sp, (float)sr.left, (float)sr.top, (float)sr.right, (float)sr.bottom, 6.0f * (float)g.dpi);
    Gdiplus::SolidBrush sb(Gdiplus::Color(255, 0x12, 0x12, 0x14));
    gx.FillPath(&sb, &sp);
    Gdiplus::Pen spen(Gdiplus::Color(255, 0x3c, 0x3c, 0x40), 1.0f);
    gx.DrawPath(&spen, &sp);

    SelectObject(dc, WebGlyphFont(12));
    SetTextColor(dc, wvb::InkDim);
    RECT sic{ sr.left + (int)(10 * g.dpi), sr.top, sr.left + (int)(32 * g.dpi), sr.bottom };
    DrawTextW(dc, L"\uE721", -1, &sic, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Placeholder if search edit is empty
    if (g_hwin.searchStr.empty() && g_hwin.editSearch && GetFocus() != g_hwin.editSearch) {
        SelectObject(dc, rowF);
        SetTextColor(dc, wvb::InkDim);
        RECT phr{ sr.left + (int)(34 * g.dpi), sr.top, sr.right - (int)(10 * g.dpi), sr.bottom };
        DrawTextW(dc, L"Search history", -1, &phr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    DeleteObject(titleF); DeleteObject(dateF); DeleteObject(rowF); DeleteObject(smallF);
}

static LRESULT CALLBACK HistWinProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: {
        int w = LOWORD(lp), h_ = HIWORD(lp);
        if (w > 0 && h_ > 0) {
            int topBarH = (int)(106 * g.dpi);
            int capH = (int)(32 * g.dpi);
            int capW = std::min(w - (int)(48 * g.dpi), (int)(520 * g.dpi));
            g_hwin.searchCapsuleR = RECT{ (int)(24 * g.dpi), (int)(54 * g.dpi),
                                          (int)(24 * g.dpi) + capW, (int)(54 * g.dpi) + capH };
            g_hwin.clearBtnR = RECT{ w - (int)(140 * g.dpi), (int)(16 * g.dpi),
                                     w - (int)(24 * g.dpi), (int)(16 * g.dpi) + (int)(30 * g.dpi) };
            if (g_hwin.editSearch) {
                int eh = (int)(20 * g.dpi);
                MoveWindow(g_hwin.editSearch,
                           g_hwin.searchCapsuleR.left + (int)(32 * g.dpi),
                           g_hwin.searchCapsuleR.top + (capH - eh) / 2,
                           capW - (int)(44 * g.dpi), eh, TRUE);
            }
            HistWinLayout(w, h_);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureHistWinBack(dc, r.right, r.bottom);
            HistWinPaint(g_hwindc, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, g_hwindc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wp) == 2001 && HIWORD(wp) == EN_CHANGE) {
            wchar_t buf[512]{};
            GetWindowTextW(g_hwin.editSearch, buf, 512);
            g_hwin.searchStr = buf;
            HistFilter();
            RECT r; GetClientRect(h, &r);
            g_hwin.scrollY = 0;
            HistWinLayout(r.right, r.bottom);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, RGB(0x12, 0x12, 0x14));
        SetTextColor(dc, wvb::Ink);
        static HBRUSH s_edbr = CreateSolidBrush(RGB(0x12, 0x12, 0x14));
        return (LRESULT)s_edbr;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        bool clHot = PtInRect(&g_hwin.clearBtnR, p) != 0;
        int hot = -1;
        bool hDel = false;
        for (size_t i = 0; i < g_hwin.rowRects.size(); i++) {
            if (PtInRect(&g_hwin.rowRects[i], p)) {
                hot = (int)i;
                if (PtInRect(&g_hwin.delRects[i], p)) hDel = true;
                break;
            }
        }
        if (hot != g_hwin.hoverIdx || clHot != g_hwin.clearBtnHot || hDel != g_hwin.hoverDel) {
            g_hwin.hoverIdx = hot;
            g_hwin.clearBtnHot = clHot;
            g_hwin.hoverDel = hDel;
            TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_hwin.hoverIdx != -1 || g_hwin.clearBtnHot || g_hwin.hoverDel) {
            g_hwin.hoverIdx = -1;
            g_hwin.clearBtnHot = false;
            g_hwin.hoverDel = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&g_hwin.clearBtnR, p)) {
            if (MessageBoxW(h, L"Are you sure you want to clear your entire browsing history?",
                            L"Clear Browsing History", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                HistClear();
                HistFilter();
                RECT r; GetClientRect(h, &r);
                g_hwin.scrollY = 0;
                HistWinLayout(r.right, r.bottom);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        for (size_t i = 0; i < g_hwin.rowRects.size() && i < g_hwin.filtered.size(); i++) {
            if (PtInRect(&g_hwin.delRects[i], p)) {
                HistDelete(g_hwin.filtered[i].id);
                HistFilter();
                RECT r; GetClientRect(h, &r);
                HistWinLayout(r.right, r.bottom);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&g_hwin.rowRects[i], p)) {
                std::wstring u = g_hwin.filtered[i].url;
                if (!g_webOn) WebToggle(true);
                WebGo(u);
                SetForegroundWindow(g.main);
                return 0;
            }
        }
        return 0;
    }
    case WM_MBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        for (size_t i = 0; i < g_hwin.rowRects.size() && i < g_hwin.filtered.size(); i++) {
            if (PtInRect(&g_hwin.rowRects[i], p)) {
                std::wstring u = g_hwin.filtered[i].url;
                if (!g_webOn) WebToggle(true);
                WebNewTab(u, true);
                SetForegroundWindow(g.main);
                return 0;
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT cl = p; ScreenToClient(h, &cl);
        for (size_t i = 0; i < g_hwin.rowRects.size() && i < g_hwin.filtered.size(); i++) {
            if (PtInRect(&g_hwin.rowRects[i], cl)) {
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, 1, L"Open");
                AppendMenuW(m, MF_STRING, 2, L"Open in new tab");
                AppendMenuW(m, MF_STRING, 3, L"Copy link address");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, 4, L"Delete from history");
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
                DestroyMenu(m);
                if (cmd == 1) {
                    if (!g_webOn) WebToggle(true);
                    WebGo(g_hwin.filtered[i].url);
                    SetForegroundWindow(g.main);
                } else if (cmd == 2) {
                    if (!g_webOn) WebToggle(true);
                    WebNewTab(g_hwin.filtered[i].url, true);
                    SetForegroundWindow(g.main);
                } else if (cmd == 3) {
                    if (OpenClipboard(h)) {
                        EmptyClipboard();
                        const std::wstring& u = g_hwin.filtered[i].url;
                        size_t sz = (u.size() + 1) * sizeof(wchar_t);
                        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sz);
                        if (hg) {
                            memcpy(GlobalLock(hg), u.c_str(), sz);
                            GlobalUnlock(hg);
                            SetClipboardData(CF_UNICODETEXT, hg);
                        }
                        CloseClipboard();
                    }
                } else if (cmd == 4) {
                    HistDelete(g_hwin.filtered[i].id);
                    HistFilter();
                    RECT r; GetClientRect(h, &r);
                    HistWinLayout(r.right, r.bottom);
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
        }
        return 0;
    }
    case WM_VSCROLL: {
        RECT r; GetClientRect(h, &r);
        int topBarH = (int)(106 * g.dpi);
        int maxScroll = std::max(0, g_hwin.totalH - (int)(r.bottom - topBarH));
        int cur = g_hwin.scrollY;
        switch (LOWORD(wp)) {
        case SB_LINEUP: cur -= (int)(38 * g.dpi); break;
        case SB_LINEDOWN: cur += (int)(38 * g.dpi); break;
        case SB_PAGEUP: cur -= (int)(r.bottom - topBarH); break;
        case SB_PAGEDOWN: cur += (int)(r.bottom - topBarH); break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: {
            SCROLLINFO si{ sizeof si, SIF_TRACKPOS };
            if (GetScrollInfo(h, SB_VERT, &si)) cur = si.nTrackPos;
            else cur = HIWORD(wp);
            break;
        }
        }
        cur = std::clamp(cur, 0, maxScroll);
        if (cur != g_hwin.scrollY) {
            g_hwin.scrollY = cur;
            HistWinLayout(r.right, r.bottom);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        RECT r; GetClientRect(h, &r);
        int topBarH = (int)(106 * g.dpi);
        int maxScroll = std::max(0, g_hwin.totalH - (int)(r.bottom - topBarH));
        int step = (int)(40 * g.dpi);
        int cur = g_hwin.scrollY - (delta / WHEEL_DELTA) * step;
        cur = std::clamp(cur, 0, maxScroll);
        if (cur != g_hwin.scrollY) {
            g_hwin.scrollY = cur;
            HistWinLayout(r.right, r.bottom);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_DESTROY: {
        if (g_hwinbm) {
            SelectObject(g_hwindc, g_hwinold);
            DeleteObject(g_hwinbm);
            g_hwinbm = nullptr;
            g_hwinold = nullptr;
        }
        if (g_hwindc) {
            DeleteDC(g_hwindc);
            g_hwindc = nullptr;
        }
        g_hwinw = g_hwinh = 0;
        g_hwin.h = nullptr;
        g_hwin.editSearch = nullptr;
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void HistFullOpen() {
    if (g_hwin.h) {
        ShowWindow(g_hwin.h, SW_RESTORE);
        SetForegroundWindow(g_hwin.h);
        if (g_hwin.editSearch) SetFocus(g_hwin.editSearch);
        return;
    }

    int W = (int)std::lround(720 * g.dpi);
    int H = (int)std::lround(560 * g.dpi);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (g.main) {
        RECT mr; GetWindowRect(g.main, &mr);
        x = mr.left + (mr.right - mr.left - W) / 2;
        y = mr.top + (mr.bottom - mr.top - H) / 2;
    }

    g_hwin.searchStr.clear();
    g_hwin.scrollY = 0;
    g_hwin.hoverIdx = -1;
    g_hwin.hoverDel = false;
    g_hwin.clearBtnHot = false;
    HistFilter();

    HWND hw = CreateWindowExW(WS_EX_APPWINDOW, L"DocsyHistWin", L"History - Docsy",
                              WS_OVERLAPPEDWINDOW | WS_VSCROLL, x, y, W, H, nullptr, nullptr, g.hinst, nullptr);
    if (!hw) return;
    g_hwin.h = hw;
    DwmApply(hw);

    g_hwin.editSearch = CreateWindowExW(0, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        0, 0, 0, 0, hw, (HMENU)2001, g.hinst, nullptr);
    if (g_hwin.editSearch) {
        HFONT edF = CreateFontW(-(int)std::lround(13 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SendMessageW(g_hwin.editSearch, WM_SETFONT, (WPARAM)edF, TRUE);
        g_hwin.editSearchPrev = (WNDPROC)SetWindowLongPtrW(g_hwin.editSearch, GWLP_WNDPROC, (LONG_PTR)HistSearchEditProc);
    }

    RECT cr; GetClientRect(hw, &cr);
    SendMessageW(hw, WM_SIZE, 0, MAKELPARAM(cr.right, cr.bottom));

    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);
    SetForegroundWindow(hw);
    if (g_hwin.editSearch) SetFocus(g_hwin.editSearch);
}

static void HistRegister(HINSTANCE hi) {
    WNDCLASSEXW hpc{ sizeof hpc };
    hpc.style = CS_DROPSHADOW | CS_SAVEBITS;
    hpc.lpfnWndProc = HistPopProc;
    hpc.hInstance = hi;
    hpc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hpc.lpszClassName = L"DocsyHistPop";
    RegisterClassExW(&hpc);

    WNDCLASSEXW hwc{ sizeof hwc };
    hwc.style = CS_HREDRAW | CS_VREDRAW;
    hwc.lpfnWndProc = HistWinProc;
    hwc.hInstance = hi;
    hwc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hwc.lpszClassName = L"DocsyHistWin";
    RegisterClassExW(&hwc);
}
