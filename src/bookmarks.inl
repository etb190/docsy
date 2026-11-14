// ============================================================
//  Bookmarks. The bar rides INSIDE the browser's strip window -
//  WEBSTRIPH() grows by its band, so the tab row and the card flow
//  below it with no layout road of their own. One flat list, one
//  level of folders: a folder is a name items wear, a folder's own
//  seat on the bar exists while any item wears the name or while a
//  seat line survives in the file. Saved through the star in the
//  omnibox capsule - an unsaved page opens the small dark bubble
//  (name, folder choice, Done), a saved page is removed by the
//  same click. Favicons are the site's own /favicon.ico, fetched
//  once in the background (the store download's own WinHTTP road)
//  and cached in Docsy\Favicons\<hash>; a site without one wears
//  the letter tile, asked about once and never again. The whole
//  state is %LOCALAPPDATA%\Docsy\bookmarks.txt: one UTF-8 line per
//  row, written to a temp file and renamed over, so a kill mid-
//  write never takes the list's tail with it.
// ============================================================

static void WebGo(const std::wstring& what);
static void WebNewTab(std::wstring url, bool makeCurrent);
static WebTab* WebCur();
static void WebRound(Gdiplus::GraphicsPath& p, float l, float t, float r, float b, float rr);
static HFONT WebGlyphFont(int px);
static void BmBubbleOpen(unsigned long long id);   // the save bubble, at the foot of this file

struct Bm {
    unsigned long long id = 0;
    std::wstring url, title, folder;      // folder empty = the item sits on the bar itself;
                                          // url empty = the folder's own seat line
    Gdiplus::Bitmap* ico = nullptr;       // the site's favicon, decoded once - WxLoadIcon's road
    bool icoTried = false;                // the fetch road has run for this item, whatever it answered
};
static std::vector<Bm> g_bm;
static unsigned long long g_bmIds = 1;
static bool g_bmLoaded = false;
static int g_bmHot = -1, g_bmPress = -1;           // the bar row's hover and press, like g_wxHot
static std::vector<std::wstring> g_wxFavBusy;      // hosts a fetch thread is out for right now

// the save bubble's state - defined here so BmRepaint can see it, filled
// and driven by the roads at the foot of this file
static struct BmPopState {
    HWND h = nullptr;
    HWND edit = nullptr;               // the name field
    HWND fedit = nullptr;              // the new-folder field, only while naming a folder
    bool newFolder = false;
    unsigned long long target = 0;     // the item being edited; 0 = a brand-new save
    std::wstring url, title, folder;
    RECT folderR{}, doneR{}, remR{};
    int hot = 0;                       // 1 folder row, 2 Done, 3 Remove
    bool menuUp = false;               // a TrackPopupMenu is running from the bubble's own rows:
                                       // its SetForegroundWindow deactivates the bubble, and that
                                       // deactivation must never be read as a click-away save
} g_bmPop;

// ---- small string roads ------------------------------------------------

static std::wstring BmHostOf(const std::wstring& url) {
    size_t s = url.find(L"://");
    if (s == std::wstring::npos) return url;
    size_t a = s + 3, b = url.find_first_of(L"/?#", a);
    std::wstring h = url.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
    size_t at = h.rfind(L'@');                    // user@host - the user part is not the address
    if (at != std::wstring::npos) h = h.substr(at + 1);
    if (h.size() > 4 && h.rfind(L"www.", 0) == 0) h = h.substr(4);
    return h;
}
static std::wstring BmNormUrl(const std::wstring& url) {    // the dedup key: no fragment, no
    std::wstring u = url;                                   // trailing slash, one letter case
    size_t f = u.find(L'#');
    if (f != std::wstring::npos) u.erase(f);
    while (!u.empty() && u[u.size() - 1] == L'/') u.pop_back();
    for (wchar_t& c : u) if (c >= L'A' && c <= L'Z') c -= L'A' - L'a';
    return u;
}
static std::wstring BmEsc(std::wstring s) {                // the file's fields are tab-separated:
    std::wstring r;                                        // a literal tab or newline rides as an
    for (wchar_t c : s) {                                  // escape, a backslash doubles itself
        if (c == L'\\') r += L"\\\\";
        else if (c == L'\t') r += L"\\t";
        else if (c == L'\n') r += L"\\n";
        else if (c == L'\r') {}
        else r += c;
    }
    return r;
}
static std::wstring BmUnesc(const std::wstring& s) {
    std::wstring r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            i++;
            if (s[i] == L't') r += L'\t';
            else if (s[i] == L'n') r += L'\n';
            else if (s[i] == L'\\') r += L'\\';
            else r += s[i];
        } else r += s[i];
    }
    return r;
}
static std::wstring BmWide(const std::string& s) {         // the file is UTF-8, the app is UTF-16
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// ---- storage ------------------------------------------------------------

static std::wstring BmFilePath() {
    PWSTR la = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);
    std::wstring r = (la ? std::wstring(la) : std::wstring(L".")) + L"\\Docsy\\bookmarks.txt";
    if (la) CoTaskMemFree(la);
    return r;
}
static void BmSave() {
    std::string txt = "docsy-bookmarks 1\n";
    std::vector<std::wstring> seen;               // seat lines for folders the list no longer
    for (const Bm& b : g_bm)                      // names: an empty folder is a choice, kept
        if (b.url.empty() && !b.folder.empty() &&
            std::find(seen.begin(), seen.end(), b.folder) == seen.end())
            txt += "f\t" + WxU(BmEsc(b.folder)) + "\n", seen.push_back(b.folder);
    for (const Bm& b : g_bm) {
        if (b.url.empty()) continue;
        txt += "b\t" + WxU(BmEsc(b.url)) + "\t" + WxU(BmEsc(b.title)) + "\t" + WxU(BmEsc(b.folder)) + "\n";
    }
    std::wstring p = BmFilePath();
    std::wstring tmp = p + L".tmp";
    WxMkdirs(p.substr(0, p.find_last_of(L'\\')));
    if (WxWriteFile(tmp, (const unsigned char*)txt.data(), txt.size()))
        MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}
static void BmLoad() {
    if (g_bmLoaded) return;
    g_bmLoaded = true;
    std::string raw;
    if (!WxReadFile(BmFilePath(), raw) || raw.size() < 8) return;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t e = raw.find('\n', pos);
        if (e == std::string::npos) e = raw.size();
        std::string line = raw.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 2 || line[1] != '\t') continue;
        char kind = line[0];
        std::vector<std::wstring> f;
        {   std::wstring ln = BmWide(line.substr(2));   // one decode, then split on real tabs
            size_t a = 0;
            for (;;) {
                size_t t = ln.find(L'\t', a);
                f.push_back(ln.substr(a, t == std::wstring::npos ? std::wstring::npos : t - a));
                if (t == std::wstring::npos) break;
                a = t + 1;
            }
        }
        if (kind == 'f' && !f.empty() && !f[0].empty()) {
            Bm b; b.id = g_bmIds++; b.folder = BmUnesc(f[0]);   // a seat line becomes a Bm whose
            g_bm.push_back(std::move(b));                       // url stays empty - the bar reads
        } else if (kind == 'b' && f.size() >= 2) {              // it as the folder's own seat
            Bm b; b.id = g_bmIds++;
            b.url = BmUnesc(f[0]); b.title = BmUnesc(f[1]);
            if (f.size() >= 3) b.folder = BmUnesc(f[2]);
            if (!b.url.empty()) g_bm.push_back(std::move(b));
        }
    }
}
static void BmSeedDemo() {                        // the CI screenshot's list - never saved to
    Bm f; f.id = g_bmIds++; f.folder = L"Docs";   // disk, never fetched (shot mode builds no threads)
    g_bm.push_back(std::move(f));
    const wchar_t* d[][2] = {
        { L"https://www.google.com/", L"Google" },
        { L"https://github.com/etb190/docsy", L"GitHub - etb190/docsy" },
        { L"https://learn.microsoft.com/windows/win32/api/", L"Win32 API" },
    };
    for (int i = 0; i < 3; i++) {
        Bm b; b.id = g_bmIds++;
        if (i == 2) b.folder = L"Docs";
        b.url = d[i][0]; b.title = d[i][1];
        g_bm.push_back(std::move(b));
    }
}

// ---- the list's own roads ----------------------------------------------

static int BmFindUrl(const std::wstring& url) {
    std::wstring n = BmNormUrl(url);
    for (size_t i = 0; i < g_bm.size(); i++)
        if (!g_bm[i].url.empty() && BmNormUrl(g_bm[i].url) == n) return (int)i;
    return -1;
}
static void BmRepaint() {
    if (g_wstrip) InvalidateRect(g_wstrip, nullptr, FALSE);
    if (g_bmPop.h) InvalidateRect(g_bmPop.h, nullptr, FALSE);   // the bubble wears the star's state
}
static void BmStarSync() {                        // the current tab's URL changed: the star's
    if (g_wstrip) InvalidateRect(g_wstrip, nullptr, FALSE);   // fill follows, wherever the road came from
}
static void BmRemoveAt(int i) {
    if (i < 0 || i >= (int)g_bm.size()) return;
    if (g_bm[i].ico) { delete g_bm[i].ico; g_bm[i].ico = nullptr; }
    g_bm.erase(g_bm.begin() + i);
    BmSave();
    BmRepaint();
}
static void BmUpsert(unsigned long long id, const std::wstring& url, const std::wstring& title,
                     const std::wstring& folder) {
    int i = -1;
    for (size_t k = 0; k < g_bm.size(); k++) if (g_bm[k].id == id) { i = (int)k; break; }
    if (i < 0) {                                  // a brand-new bookmark joins at the list's tail:
        Bm b; b.id = g_bmIds++;                   // insertion order, the bar reads left to right
        b.url = url; b.title = title; b.folder = folder;
        g_bm.push_back(std::move(b));
    } else {
        g_bm[i].title = title; g_bm[i].folder = folder;
    }
    BmSave();
    BmRepaint();
}

// ---- favicons ------------------------------------------------------------
// The site's own /favicon.ico - no third-party service, no bytes the exe
// does not already pay. One background thread per host, the PNG/ICO lands
// in Docsy\Favicons\<hash>, and WM_APP+9 walks the list home on the UI
// thread. A miss is remembered as <hash>.png.none - a dead host is never
// re-asked, on this launch or any later one.

static std::wstring BmFavDir() {
    PWSTR la = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);
    std::wstring r = (la ? std::wstring(la) : std::wstring(L".")) + L"\\Docsy\\Favicons";
    if (la) CoTaskMemFree(la);
    return r;
}
static std::wstring BmFavPath(const std::wstring& host) {
    unsigned char h[32];
    std::string k = WxU(host);
    crxk::Sha256((const unsigned char*)k.data(), k.size(), h);
    wchar_t hex[17];
    for (int i = 0; i < 8; i++) swprintf_s(hex + i * 2, 3, L"%02x", h[i]);
    hex[16] = 0;
    return BmFavDir() + L"\\" + hex + L".png";
}
static void BmFaviconArrived(std::wstring* host) {   // WM_APP+9, the UI thread: the thread's
    if (host)                                        // host is home, busy or not
        for (size_t i = 0; i < g_wxFavBusy.size(); i++)
            if (g_wxFavBusy[i] == *host) { g_wxFavBusy.erase(g_wxFavBusy.begin() + i); break; }
    bool any = false;
    for (Bm& b : g_bm) {
        if (b.ico || !b.icoTried || b.url.empty()) continue;
        std::wstring hp = BmHostOf(b.url);
        if (host && hp != *host) continue;
        std::wstring p = BmFavPath(hp);
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        b.ico = WxLoadIcon(p);
        any = true;
    }
    delete host;
    if (any) BmRepaint();
}
static void BmEnsureIco(Bm& b) {
    if (b.ico || b.icoTried || b.url.empty() || g.quiet) return;
    std::wstring host = BmHostOf(b.url);
    if (host.empty() || std::find(g_wxFavBusy.begin(), g_wxFavBusy.end(), host) != g_wxFavBusy.end()) return;
    std::wstring p = BmFavPath(host);
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {   // a past session fetched it:
        b.ico = WxLoadIcon(p);                    // no thread, no network - the disk knows
        b.icoTried = true;
        return;
    }
    if (GetFileAttributesW((p + L".none").c_str()) != INVALID_FILE_ATTRIBUTES) {
        b.icoTried = true;                        // the miss is already on record
        return;
    }
    b.icoTried = true;                            // the road runs once, whatever it answers
    g_wxFavBusy.push_back(host);
    WxMkdirs(BmFavDir());
    HWND main = g.main;
    std::wstring* done = new std::wstring(host);
    std::thread([host, p, main, done]() {
        std::vector<unsigned char> d;
        std::wstring err;
        if (WxHttpGet(L"https://" + host + L"/favicon.ico", L"Docsy/1.0", d, err) && !d.empty())
            WxWriteFile(p, d.data(), d.size());
        else
            WxWriteFile(p + L".none", (const unsigned char*)"x", 1);
        if (!PostMessageW(main, WM_APP + 9, 0, (LPARAM)done)) delete done;   // the window is gone:
    }).detach();                                       // the next launch's load reads the cache
}

// ---- the bar's rows ------------------------------------------------------
// Rows are computed fresh for paint and hit alike - a handful of items, a
// handful of rects, no state to drift. A folder rides the bar at the seat
// its first appearance in the list gives it, whether that is a seat line
// or an item wearing the name; the chevron says what it is, a click opens
// its menu, and whatever the bar could not seat answers to the » at the
// right end.

struct BmRow { int kind; int idx; RECT r; };      // kind: 0 item, 1 folder, 2 the overflow's »
static std::vector<BmRow> g_bmRows;
static std::vector<std::wstring> g_bmRowFolders;
static int WebBarH() { return (g_webOn && g_webBarOn && !g_webFail) ? (int)(30 * g.dpi) : 0; }

static void BmBarRows(int w) {
    g_bmRows.clear();
    g_bmRowFolders.clear();
    if (!g_webOn || !g_webBarOn || !g_wstrip) return;
    int y0 = (int)(44 * g.dpi), y1 = y0 + (int)(30 * g.dpi);
    if (y1 <= y0) return;
    int x = (int)(8 * g.dpi), lim = w - (int)(8 * g.dpi);
    int moreW = (int)(26 * g.dpi);
    HDC dc = GetDC(g_wstrip);
    HGDIOBJ of = SelectObject(dc, g_webEditFont ? g_webEditFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    auto textW = [&](const std::wstring& s, int cap) {
        SIZE sz{};
        int n = (int)s.size();
        if (n > cap) n = cap;
        GetTextExtentPoint32W(dc, s.c_str(), n, &sz);
        return min((int)sz.cx, cap);          // sz.cx is a LONG: std::min needs one type to deduce
    };
    auto FolderAt = [&](const std::wstring& f) {
        for (size_t k = 0; k < g_bmRowFolders.size(); k++) if (g_bmRowFolders[k] == f) return (int)k;
        g_bmRowFolders.push_back(f);
        return (int)g_bmRowFolders.size() - 1;
    };
    auto Seated = [&](int fi) {
        for (const BmRow& r : g_bmRows) if (r.kind == 1 && r.idx == fi) return true;
        return false;
    };
    auto Full = [&]() { return !g_bmRows.empty() && x > lim - moreW - (int)(40 * g.dpi); };
    for (int i = 0; i < (int)g_bm.size(); i++) {
        const Bm& b = g_bm[i];
        if (b.url.empty()) {
            if (!b.folder.empty()) FolderAt(b.folder);   // a seat line registers the folder -
            continue;                                    // its row comes at its first mention
        }
        if (!b.folder.empty()) {                         // an item wearing a folder seats that
            int fi = FolderAt(b.folder);                 // folder the moment it is first reached
            if (Seated(fi)) continue;                    // (already on the bar: its items are the
            int tw = (int)(7 * g.dpi) + (int)(16 * g.dpi) + (int)(5 * g.dpi)   // folder menu's)
                   + textW(g_bmRowFolders[fi], 128) + (int)(4 * g.dpi) + (int)(10 * g.dpi) + (int)(14 * g.dpi);
            if (Full()) {
                g_bmRows.push_back({ 2, i, { lim - moreW, y0 + (int)(3 * g.dpi), lim, y1 - (int)(3 * g.dpi) } });
                break;
            }
            g_bmRows.push_back({ 1, fi, { x, y0 + (int)(3 * g.dpi), x + tw, y1 - (int)(3 * g.dpi) } });
            x += tw;
            continue;
        }
        int tw = (int)(7 * g.dpi) + (int)(16 * g.dpi) + (int)(5 * g.dpi)
               + textW(b.title, (int)(150 * g.dpi)) + (int)(14 * g.dpi);
        if (Full()) {                                    // the bar is full: everything left
            g_bmRows.push_back({ 2, i, { lim - moreW, y0 + (int)(3 * g.dpi), lim, y1 - (int)(3 * g.dpi) } });
            break;                                       // answers to the » at the right end
        }
        g_bmRows.push_back({ 0, i, { x, y0 + (int)(3 * g.dpi), x + tw, y1 - (int)(3 * g.dpi) } });
        x += tw;
    }
    SelectObject(dc, of);
    ReleaseDC(g_wstrip, dc);
}
static int BmHit(POINT p) {
    for (size_t k = 0; k < g_bmRows.size(); k++) {
        const RECT& r = g_bmRows[k].r;
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return (int)k;
    }
    return -1;
}

static void BmTileDisc(Gdiplus::Graphics& gx, HDC dc, const RECT& rc, const std::wstring& label,
                       const std::wstring& seed) {
    unsigned hh = 0;                              // the disc's hue comes from the host: every
    for (wchar_t c : seed) hh = hh * 31 + (unsigned)c;   // site wears its own color, no two alike
    float hue = (hh % 360) / 360.0f;
    float h6 = hue * 6.0f;
    int seg = (int)h6 % 6;
    float f = h6 - (float)seg, v = 150.0f;        // HSV, S=1: q and t ride the value, not 255
    float q = v * (1.0f - f), t = v * f;
    BYTE r0 = 0, g0 = 0, b0 = 0;
    switch (seg % 6) {
    case 0: r0 = (BYTE)v; g0 = (BYTE)t; b0 = (BYTE)q; break;
    case 1: r0 = (BYTE)q; g0 = (BYTE)v; b0 = (BYTE)t; break;
    case 2: r0 = (BYTE)t; g0 = (BYTE)v; b0 = (BYTE)q; break;
    case 3: r0 = (BYTE)q; g0 = (BYTE)t; b0 = (BYTE)v; break;
    case 4: r0 = (BYTE)t; g0 = (BYTE)q; b0 = (BYTE)v; break;
    default: r0 = (BYTE)v; g0 = (BYTE)q; b0 = (BYTE)t; break;
    }
    Gdiplus::Color c(255, r0, g0, b0);
    int is = rc.bottom - rc.top;
    Gdiplus::SolidBrush br(c);
    gx.FillEllipse(&br, (Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top, (Gdiplus::REAL)is, (Gdiplus::REAL)is);
    wchar_t ch[2] = { label.empty() ? L'?' : (wchar_t)towupper(label[0]), 0 };
    SelectObject(dc, g_webEditFont ? g_webEditFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, wvb::Ink);
    DrawTextW(dc, ch, 1, (RECT*)&rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
static void BmBarDraw(HDC dc, int w, int h) {
    if (!g_webOn || !g_webBarOn) return;
    int y0 = (int)(44 * g.dpi);
    if (y0 >= h) return;
    BmBarRows(w);
    RECT band{ 0, y0, w, h };
    HBRUSH bg = CreateSolidBrush(wvb::StripBg);   // the toolbar's fade ends at the band's top:
    FillRect(dc, &band, bg); DeleteObject(bg);    // the bar paints over it - one flat sheet
    RECT edge{ 0, h - 1, w, h };
    HBRUSH eb = CreateSolidBrush(wvb::Edge);      // the hairline the toolbar drew was covered:
    FillRect(dc, &edge, eb); DeleteObject(eb);    // it is the card's boundary, it comes back
    SetBkMode(dc, TRANSPARENT);
    Gdiplus::Graphics gx(dc);
    gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    int is = (int)(16 * g.dpi);
    for (size_t k = 0; k < g_bmRows.size(); k++) {
        BmRow& r = g_bmRows[k];
        RECT rc = r.r;
        bool hot = (int)k == g_bmHot, pr = (int)k == g_bmPress;
        if (hot || pr) {                          // the toolbar's own tile story, 6px corners
            Gdiplus::GraphicsPath p;
            WebRound(p, (float)rc.left, (float)rc.top + 1, (float)rc.right, (float)rc.bottom - 1,
                     6.0f * (float)g.dpi);
            Gdiplus::SolidBrush tb(Gdiplus::Color(255, GetRValue(pr ? wvb::Pressed : wvb::Hover),
                                                  GetGValue(pr ? wvb::Pressed : wvb::Hover),
                                                  GetBValue(pr ? wvb::Pressed : wvb::Hover)));
            gx.FillPath(&tb, &p);
        }
        if (r.kind == 2) {                        // » : everything the bar could not seat
            SelectObject(dc, WebGlyphFont(13));
            SetTextColor(dc, wvb::InkDim);
            DrawTextW(dc, L"\uE712", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            continue;
        }
        RECT ir{ rc.left + (int)(7 * g.dpi), rc.top + (rc.bottom - rc.top - is) / 2,
                 rc.left + (int)(7 * g.dpi) + is, rc.top + (rc.bottom - rc.top + is) / 2 };
        SelectObject(dc, g_webEditFont ? g_webEditFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        if (r.kind == 1) {                        // a folder: the library glyph, the name, the
            SelectObject(dc, WebGlyphFont(11));   // open-down chevron
            SetTextColor(dc, wvb::Ink);
            DrawTextW(dc, L"\uE8A9", -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT tr{ ir.right + (int)(5 * g.dpi), rc.top, rc.right - (int)(2 * g.dpi), rc.bottom };
            const std::wstring& label = g_bmRowFolders[r.idx];
            SetTextColor(dc, wvb::Ink);
            DrawTextW(dc, label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SIZE sz{};
            GetTextExtentPoint32W(dc, label.c_str(), (int)min((int)label.size(), 128), &sz);
            RECT cr{ tr.left + sz.cx + (int)(2 * g.dpi), rc.top, tr.right, rc.bottom };
            SelectObject(dc, WebGlyphFont(9));
            SetTextColor(dc, wvb::InkDim);
            DrawTextW(dc, L"\uE70D", -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            continue;
        }
        Bm& b = g_bm[r.idx];
        BmEnsureIco(b);
        if (b.ico) {
            gx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            gx.DrawImage(b.ico, ir.left, ir.top, is, is);
        } else {
            std::wstring host = BmHostOf(b.url);
            BmTileDisc(gx, dc, ir, b.title, host.empty() ? b.title : host);
        }
        RECT tr{ ir.right + (int)(5 * g.dpi), rc.top, rc.right - (int)(2 * g.dpi), rc.bottom };
        SetTextColor(dc, wvb::Ink);
        DrawTextW(dc, b.title.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// ---- the bar's clicks and menus -----------------------------------------

static void BmOpenRow(const Bm& b, bool newTab) {
    if (b.url.empty()) return;
    if (newTab) WebNewTab(b.url, true);
    else WebGo(b.url);                            // the omnibox's own road: the URL is absolute,
}                                                 // so it navigates without a search detour

static void BmItemMenu(const Bm& b, POINT screen) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, 1, L"Open in new tab");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, L"Edit\u2026");
    AppendMenuW(m, MF_STRING, 3, L"Remove");
    SetForegroundWindow(g.main);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, g.main, nullptr);
    DestroyMenu(m);
    if (c == 1) BmOpenRow(b, true);
    else if (c == 2) BmBubbleOpen(b.id);
    else if (c == 3) {
        for (size_t i = 0; i < g_bm.size(); i++) if (g_bm[i].id == b.id) { BmRemoveAt((int)i); break; }
    }
}

static void BmFolderMenu(const std::wstring& name, POINT screen) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    UINT base = 100;
    int n = 0;
    for (size_t i = 0; i < g_bm.size(); i++) {
        const Bm& b = g_bm[i];
        if (b.folder == name && !b.url.empty()) {
            std::wstring label;
            for (wchar_t ch : b.title) { label += ch; if (ch == L'&') label += ch; }
            AppendMenuW(m, MF_STRING, base + (UINT)i, label.c_str());
            n++;
        }
    }
    if (!n) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(empty)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, L"Remove folder");
    SetForegroundWindow(g.main);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, g.main, nullptr);
    DestroyMenu(m);
    if (c == 2) {                                 // the folder goes, its items come back to the
        for (Bm& b : g_bm) if (b.folder == name) b.folder.clear();   // bar - nothing is lost
        for (size_t i = g_bm.size(); i > 0; i--)  // and the folder's own seat line goes with it
            if (g_bm[i - 1].url.empty() && g_bm[i - 1].folder == name)
                g_bm.erase(g_bm.begin() + (i - 1));
        BmSave();
        BmRepaint();
    } else if (c >= 100 && (size_t)(c - 100) < g_bm.size()) BmOpenRow(g_bm[c - 100], false);
}

static void BmRowMenu(int k) {
    if (k < 0 || k >= (int)g_bmRows.size() || g_bmRows[k].kind == 2) return;
    BmRow& r = g_bmRows[k];
    POINT s{ r.r.left, r.r.bottom };
    ClientToScreen(g_wstrip, &s);
    if (r.kind == 1) BmFolderMenu(g_bmRowFolders[r.idx], s);
    else BmItemMenu(g_bm[r.idx], s);
}

static void BmMoreMenu(int firstIdx) {            // the »'s list: everything past the bar's edge,
    HMENU m = CreatePopupMenu();                  // one flat menu, in the list's own order
    if (!m) return;
    UINT base = 500;
    int n = 0;
    for (int i = firstIdx; i < (int)g_bm.size(); i++) {
        const Bm& b = g_bm[i];
        if (b.url.empty()) continue;              // folders keep their own seats; their items speak
        std::wstring label;
        for (wchar_t ch : b.title) { label += ch; if (ch == L'&') label += ch; }
        if (!b.folder.empty()) label = std::wstring(b.folder) + L":  " + label;
        AppendMenuW(m, MF_STRING, base + (UINT)i, label.c_str());
        n++;
    }
    if (!n) { DestroyMenu(m); return; }
    SetForegroundWindow(g.main);
    RECT mr{ 0, 0, 0, 0 };
    if (g_bmRows.size()) {
        mr = g_bmRows[g_bmRows.size() - 1].r;
        ClientToScreen(g_wstrip, (POINT*)&mr);    // the menu opens at the »'s left foot
    }
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, mr.left, mr.bottom, 0, g.main, nullptr);
    DestroyMenu(m);
    if (c >= 500 && (size_t)(c - 500) < g_bm.size()) BmOpenRow(g_bm[c - 500], false);
}

static void BmClick(int k, bool newTab) {
    if (k < 0 || k >= (int)g_bmRows.size()) return;
    BmRow& r = g_bmRows[k];
    if (r.kind == 2) { BmMoreMenu(r.idx); return; }
    if (r.kind == 1) {
        POINT s{ r.r.left, r.r.bottom };
        ClientToScreen(g_wstrip, &s);
        BmFolderMenu(g_bmRowFolders[r.idx], s);
        return;
    }
    BmOpenRow(g_bm[r.idx], newTab);
}

// ---- the save bubble -------------------------------------------------------
// The star's own prompt, in the window and of the chrome's language: a
// dark card under the star with the page's name editable, the folder one
// menu away (an existing one, or New folder... which turns the row into
// a field), Done and Remove where a thumb expects them. Enter saves,
// Esc leaves the page as it was, a click anywhere outside saves - the
// Chrome bubble's manners at a fraction of its weight.

static std::vector<std::wstring> BmAllFolders() {
    std::vector<std::wstring> v;
    for (const Bm& b : g_bm)
        if (!b.folder.empty() && std::find(v.begin(), v.end(), b.folder) == v.end())
            v.push_back(b.folder);
    return v;
}

static void BmPopDestroy() {                      // the window and its controls go, state clears
    HWND h = g_bmPop.h;
    g_bmPop.h = nullptr;
    if (g_bmPop.edit) { DestroyWindow(g_bmPop.edit); g_bmPop.edit = nullptr; }
    if (g_bmPop.fedit) { DestroyWindow(g_bmPop.fedit); g_bmPop.fedit = nullptr; }
    g_bmPop.newFolder = false; g_bmPop.target = 0; g_bmPop.hot = 0;
    g_bmPop.url.clear(); g_bmPop.title.clear(); g_bmPop.folder.clear();
    if (h) DestroyWindow(h);
}
static void BmPopCommit() {                       // Enter, Done, or the click that went elsewhere:
    if (!g_bmPop.h) return;                       // the bubble's answer lands in the list
    std::wstring name, folder = g_bmPop.folder, url = g_bmPop.url;
    unsigned long long id = g_bmPop.target;
    if (g_bmPop.edit) {
        wchar_t t[256] = L"";
        GetWindowTextW(g_bmPop.edit, t, 256);
        name = t;
    }
    if (g_bmPop.fedit) {                          // a folder was being named when the answer came:
        wchar_t t[128] = L"";                     // what the field holds IS the choice
        GetWindowTextW(g_bmPop.fedit, t, 128);
        if (t[0]) folder = t;
    }
    while (!name.empty() && name[0] == L' ') name.erase(0, 1);
    while (!name.empty() && name[name.size() - 1] == L' ') name.pop_back();
    if (name.empty()) name = BmHostOf(url);
    if (name.empty()) name = L"Bookmark";
    BmPopDestroy();
    BmUpsert(id, url, name, folder);
}
static WNDPROC g_bmEditPrev = nullptr, g_bmFEditPrev = nullptr;
static LRESULT CALLBACK BmEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN) {
        if (wp == VK_RETURN) {                        // Enter answers where the caret lives: the
            if (h == g_bmPop.fedit) {                 // folder field names the folder and returns
                wchar_t t[128] = L"";
                GetWindowTextW(h, t, 128);
                if (t[0]) g_bmPop.folder = t;
                DestroyWindow(h);
                g_bmPop.fedit = nullptr; g_bmPop.newFolder = false;
                if (g_bmPop.h) {
                    InvalidateRect(g_bmPop.h, nullptr, FALSE);
                    if (g_bmPop.edit) SetFocus(g_bmPop.edit);
                }
                return 0;
            }
            BmPopCommit();                            // the name field commits the whole bubble
            return 0;
        }
        if (wp == VK_ESCAPE) {                        // Esc: the naming, or the whole bubble, unsaved
            if (h == g_bmPop.fedit) {
                DestroyWindow(h);
                g_bmPop.fedit = nullptr; g_bmPop.newFolder = false;
                if (g_bmPop.h) {
                    InvalidateRect(g_bmPop.h, nullptr, FALSE);
                    if (g_bmPop.edit) SetFocus(g_bmPop.edit);
                }
            } else BmPopDestroy();
            return 0;
        }
    }
    return CallWindowProcW(h == g_bmPop.fedit ? g_bmFEditPrev : g_bmEditPrev, h, m, wp, lp);
}
static void BmPopPaint(HDC dc, int w, int h) {
    SetBkMode(dc, TRANSPARENT);
    Gdiplus::Graphics gx(dc);
    gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;                      // the card: the chrome's own shape language
    WebRound(p, 0.5f, 0.5f, (float)w - 1, (float)h - 1, 10.0f * (float)g.dpi);
    Gdiplus::SolidBrush face(Gdiplus::Color(255, GetRValue(wvb::StripBg), GetGValue(wvb::StripBg),
                                            GetBValue(wvb::StripBg)));
    gx.FillPath(&face, &p);
    Gdiplus::Pen ring(Gdiplus::Color(255, GetRValue(wvb::Edge), GetGValue(wvb::Edge), GetBValue(wvb::Edge)), 1.0f);
    gx.DrawPath(&ring, &p);
    static HFONT fl = nullptr, fb = nullptr;
    if (!fl) fl = CreateFontW(-(int)std::lround(11 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!fb) fb = CreateFontW(-(int)std::lround(13 * g.dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    int m = (int)(14 * g.dpi);
    SelectObject(dc, fl);
    SetTextColor(dc, wvb::InkDim);
    RECT lr{ m, (int)(10 * g.dpi), w - m, (int)(28 * g.dpi) };
    DrawTextW(dc, L"Name", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT fr = g_bmPop.folderR;
    RECT dr = g_bmPop.doneR, rr = g_bmPop.remR;
    // the folder row: a tile like the omnibox, the chevron says a menu lives here
    bool fhot = g_bmPop.hot == 1;
    Gdiplus::GraphicsPath fp;
    WebRound(fp, (float)fr.left, (float)fr.top, (float)fr.right, (float)fr.bottom, 6.0f * (float)g.dpi);
    COLORREF fc = fhot ? wvb::Hover : wvb::Omnibox;
    Gdiplus::SolidBrush fb2(Gdiplus::Color(255, GetRValue(fc), GetGValue(fc), GetBValue(fc)));
    gx.FillPath(&fb2, &fp);
    SelectObject(dc, fb);
    SetTextColor(dc, wvb::Ink);
    RECT flr{ fr.left + (int)(10 * g.dpi), fr.top, fr.right - (int)(24 * g.dpi), fr.bottom };
    const wchar_t* fname = g_bmPop.newFolder ? L"" : (g_bmPop.folder.empty() ? L"Bookmarks bar"
                                                                          : g_bmPop.folder.c_str());
    DrawTextW(dc, fname, -1, &flr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!g_bmPop.newFolder) {
        SelectObject(dc, WebGlyphFont(9));
        SetTextColor(dc, wvb::InkDim);
        RECT cr{ fr.right - (int)(20 * g.dpi), fr.top, fr.right - (int)(4 * g.dpi), fr.bottom };
        DrawTextW(dc, L"\uE70D", -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // the buttons: Done in the accent, Remove where an edit has something to lose
    auto Btn = [&](const RECT& b, const wchar_t* label, bool accent, bool hot) {
        Gdiplus::GraphicsPath bp;
        WebRound(bp, (float)b.left, (float)b.top, (float)b.right, (float)b.bottom, 6.0f * (float)g.dpi);
        COLORREF c = accent ? wvb::Blue : (hot ? wvb::Pressed : wvb::Omnibox);
        Gdiplus::SolidBrush bb(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
        gx.FillPath(&bb, &bp);
        SelectObject(dc, fb);
        SetTextColor(dc, accent ? RGB(0xff, 0xff, 0xff) : wvb::Ink);
        RECT tr = b;
        DrawTextW(dc, label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    Btn(dr, L"Done", true, g_bmPop.hot == 2);
    if (g_bmPop.target) Btn(rr, L"Remove", false, g_bmPop.hot == 3);
}
static LRESULT CALLBACK BmPopProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) BmPopPaint(dc, r.right, r.bottom);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hot = PtInRect(&g_bmPop.folderR, p) ? 1
                : PtInRect(&g_bmPop.doneR, p) ? 2
                : (g_bmPop.target && PtInRect(&g_bmPop.remR, p)) ? 3 : 0;
        if (hot != g_bmPop.hot) {
            g_bmPop.hot = hot;
            if (hot) TrackLeave(h);                  // the leave that clears the tile has to be asked for
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_bmPop.hot) { g_bmPop.hot = 0; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_SETFOCUS:
        if (g_bmPop.edit) SetFocus(g_bmPop.edit);    // the caret belongs to the name field from the start
        return 0;
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&g_bmPop.doneR, p)) { BmPopCommit(); return 0; }
        if (g_bmPop.target && PtInRect(&g_bmPop.remR, p)) {
            unsigned long long id = g_bmPop.target;
            BmPopDestroy();
            for (size_t i = 0; i < g_bm.size(); i++) if (g_bm[i].id == id) { BmRemoveAt((int)i); break; }
            return 0;
        }
        if (PtInRect(&g_bmPop.folderR, p) && !g_bmPop.newFolder) {   // the folder menu: the bar
            HMENU mm = CreatePopupMenu();                            // itself, every folder with
            if (mm) {                                                // its mark, and the new-folder road
                std::vector<std::wstring> fl = BmAllFolders();
                AppendMenuW(mm, MF_STRING | (g_bmPop.folder.empty() ? MF_CHECKED : 0), 1, L"Bookmarks bar");
                for (size_t i = 0; i < fl.size(); i++)
                    AppendMenuW(mm, MF_STRING | (g_bmPop.folder == fl[i] ? MF_CHECKED : 0),
                                10 + (UINT)i, fl[i].c_str());
                AppendMenuW(mm, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(mm, MF_STRING, 2, L"New folder\u2026");
                RECT fr = g_bmPop.folderR;       // the anchor is client geometry: read it before
                POINT a{ fr.left, fr.bottom };   // anything that could close the bubble under it
                ClientToScreen(h, &a);
                SetForegroundWindow(g.main);
                g_bmPop.menuUp = true;           // the menu's activation shift is not a click-away
                int c = TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, a.x, a.y, 0, g.main, nullptr);
                g_bmPop.menuUp = false;
                DestroyMenu(mm);
                if (c == 1) { g_bmPop.folder.clear(); InvalidateRect(h, nullptr, FALSE); }
                else if (c == 2) {
                    g_bmPop.newFolder = true;
                    g_bmPop.fedit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                                    g_bmPop.folderR.left + (int)(8 * g.dpi),
                                                    g_bmPop.folderR.top + (int)(5 * g.dpi),
                                                    g_bmPop.folderR.right - g_bmPop.folderR.left - (int)(16 * g.dpi),
                                                    g_bmPop.folderR.bottom - g_bmPop.folderR.top - (int)(10 * g.dpi),
                                                    h, nullptr, g.hinst, nullptr);
                    if (g_bmPop.fedit) {
                        SendMessageW(g_bmPop.fedit, WM_SETFONT, (WPARAM)g_webEditFont, TRUE);
                        g_bmFEditPrev = (WNDPROC)SetWindowLongPtrW(g_bmPop.fedit, GWLP_WNDPROC,
                                                                   (LONG_PTR)BmEditProc);
                        SetFocus(g_bmPop.fedit);
                        InvalidateRect(h, nullptr, FALSE);
                    } else g_bmPop.newFolder = false;
                } else if (c >= 10 && (size_t)(c - 10) < fl.size()) {
                    g_bmPop.folder = fl[(size_t)(c - 10)];
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLOREDIT:                          // both fields wear the omnibox's colors
        return (LRESULT)WebEditColors((HDC)wp, (HWND)lp);
    case WM_ACTIVATE:                              // a click anywhere outside saves, Chrome's way -
        if (LOWORD(wp) == WA_INACTIVE && g_bmPop.h == h && !g_bmPop.newFolder &&   // but never while
            !g_bmPop.menuUp) BmPopCommit();        // the bubble's own menu or a folder name is up
        return 0;
    case WM_CLOSE: BmPopDestroy(); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void BmBubbleOpen(unsigned long long id) {
    if (!g_webOn || !g_wstrip) return;
    BmPopDestroy();                                // a fresh bubble, whatever stood before
    g_bmPop.target = id;
    if (id) {
        for (const Bm& b : g_bm) if (b.id == id) {
            g_bmPop.url = b.url; g_bmPop.title = b.title; g_bmPop.folder = b.folder;
            break;
        }
        if (g_bmPop.url.empty()) return;           // the item vanished under its own menu
    } else {
        WebTab* t = WebCur();
        if (!t || t->url.empty() || g_webFail) return;
        g_bmPop.url = t->url; g_bmPop.title = t->title;
    }
    int W = (int)(320 * g.dpi), H = (int)(176 * g.dpi);
    POINT a{ g_wbR[WB_STAR].left, g_wbR[WB_STAR].bottom }, b{ g_wbR[WB_STAR].right, g_wbR[WB_STAR].top };
    ClientToScreen(g_wstrip, &a); ClientToScreen(g_wstrip, &b);
    MONITORINFO mi{ sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(g_wstrip, MONITOR_DEFAULTTONEAREST), &mi);
    int x = std::clamp((int)b.x - W, (int)mi.rcWork.left, max((int)mi.rcWork.left, (int)mi.rcWork.right - W));
    int y = a.y + (int)(4 * g.dpi);
    if (y + H > mi.rcWork.bottom) y = max((int)mi.rcWork.top, (int)a.y - (int)(4 * g.dpi) - H);
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW, L"DocsyBmPop", L"", WS_POPUP, x, y, W, H,
                              g.main, nullptr, g.hinst, nullptr);
    if (!hw) return;
    int m = (int)(14 * g.dpi), bh = (int)(30 * g.dpi);
    g_bmPop.folderR = { m, (int)(72 * g.dpi), W - m, (int)(72 * g.dpi) + bh };
    g_bmPop.doneR = { W - m - (int)(92 * g.dpi), H - m - bh, W - m, H - m };
    g_bmPop.remR = { m, H - m - bh, m + (int)(92 * g.dpi), H - m };
    g_bmPop.h = hw;
    g_bmPop.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   m, (int)(32 * g.dpi), W - 2 * m, (int)(28 * g.dpi),
                                   hw, nullptr, g.hinst, nullptr);
    if (g_bmPop.edit) {
        SendMessageW(g_bmPop.edit, WM_SETFONT, (WPARAM)g_webEditFont, TRUE);
        g_bmEditPrev = (WNDPROC)SetWindowLongPtrW(g_bmPop.edit, GWLP_WNDPROC, (LONG_PTR)BmEditProc);
        SetWindowTextW(g_bmPop.edit, (g_bmPop.title.empty() ? BmHostOf(g_bmPop.url) : g_bmPop.title).c_str());
    }
    ShowWindow(hw, SW_SHOW);
    if (g_bmPop.edit) SetFocus(g_bmPop.edit);      // the caret lands once the window can take it
}

// ---- the star ------------------------------------------------------------

static void BmStarClick() {                        // saved page: the same click takes it back.
    WebTab* t = WebCur();                          // unsaved: the bubble opens, the name rides in
    if (!t || t->url.empty() || g_webFail) return; // prefilled, the folder one menu away
    int i = BmFindUrl(t->url);
    if (i >= 0) BmRemoveAt(i);
    else BmBubbleOpen(0);
}
