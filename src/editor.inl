// Docsy - Text Editor module (powered by embedded Quill.js v2 engine)
#pragma once

// Forward declarations
static void WebToggle(bool on);
static void LayoutMain();
static void UpdateTitle();
static void WebEnsureEnv();

// ---- Text Editor State ----
static wrl::ComPtr<ICoreWebView2Controller> g_edCtr;
static wrl::ComPtr<ICoreWebView2> g_edWeb;
static bool g_edBuilding = false;
static bool g_edReady = false;
static std::wstring g_edPath;
static std::wstring g_edTitle = L"Untitled";
static std::wstring g_edPendingLoad;

// Format state reported by editor
static bool g_edBold = false, g_edItalic = false, g_edUnderline = false, g_edStrike = false;
static int g_edHeader = 0;       // 0, 1, 2
static int g_edList = 0;         // 0: none, 1: bullet, 2: number
static bool g_edQuote = false;
static int g_edAlign = 0;        // 0: left, 1: center, 2: justify
static std::wstring g_edFont = L"Segoe UI";
static int g_edSize = 12;
static bool g_edInvert = false;

// Toolbar buttons (Top Bar: navigation & document IO)
enum {
    EB_WEB, EB_PDF,
    EB_SEP1,
    EB_OPEN, EB_SAVE,
    EB_COUNT
};

static int g_edHot = -1, g_edDown = -1;

// Post JSON command to Quill editor
static void EditorPostCmd(const wchar_t* cmd) {
    if (!g_edWeb) return;
    std::wstring msg = L"{\"cmd\":\"";
    msg += cmd;
    msg += L"\"}";
    g_edWeb->PostWebMessageAsJson(msg.c_str());
}

static void EditorPostHeader(int lvl) {
    if (!g_edWeb) return;
    std::wstring msg = L"{\"cmd\":\"header\",\"level\":";
    msg += std::to_wstring(lvl);
    msg += L"}";
    g_edWeb->PostWebMessageAsJson(msg.c_str());
}

static void EditorPostSizeStep(int dir) {
    if (!g_edWeb) return;
    std::wstring msg = L"{\"cmd\":\"sizeStep\",\"dir\":";
    msg += std::to_wstring(dir);
    msg += L"}";
    g_edWeb->PostWebMessageAsJson(msg.c_str());
}

static void EditorPostFont(const std::wstring& font) {
    if (!g_edWeb) return;
    g_edFont = font;
    std::wstring msg = L"{\"cmd\":\"font\",\"name\":\"" + font + L"\"}";
    g_edWeb->PostWebMessageAsJson(msg.c_str());
    if (g_etool) InvalidateRect(g_etool, nullptr, FALSE);
}

static void EditorToggleInvert() {
    g_edInvert = !g_edInvert;
    if (g_edWeb) {
        std::wstring msg = L"{\"cmd\":\"invert\",\"on\":";
        msg += g_edInvert ? L"true}" : L"false}";
        g_edWeb->PostWebMessageAsJson(msg.c_str());
    }
    if (g_etool) InvalidateRect(g_etool, nullptr, FALSE);
}

static void EditorInsertImagePrompt() {
    wchar_t fn[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ sizeof ofn };
    ofn.hwndOwner = g.main;
    ofn.lpstrFilter = L"Image Files (*.png;*.jpg;*.jpeg;*.gif;*.webp)\0*.png;*.jpg;*.jpeg;*.gif;*.webp\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fn;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        std::string s;
        if (WxReadFile(fn, s) && !s.empty()) {
            std::string ext = "png";
            std::wstring wfn = fn;
            size_t dot = wfn.rfind(L'.');
            if (dot != std::wstring::npos) ext = WxU(wfn.substr(dot + 1));
            static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string b64;
            int val = 0, valb = -6;
            for (unsigned char c : s) {
                val = (val << 8) + c;
                valb += 8;
                while (valb >= 0) {
                    b64.push_back(b64c[(val >> valb) & 0x3F]);
                    valb -= 6;
                }
            }
            if (valb > -6) b64.push_back(b64c[((val << 8) >> (valb + 8)) & 0x3F]);
            while (b64.size() % 4) b64.push_back('=');
            std::wstring dataUrl = L"data:image/" + WxW(ext) + L";base64," + WxW(b64);
            std::wstring msg = L"{\"cmd\":\"image\",\"src\":\"" + dataUrl + L"\"}";
            if (g_edWeb) g_edWeb->PostWebMessageAsJson(msg.c_str());
        }
    }
}

static void EditorSave() {
    if (g_edPath.empty()) {
        wchar_t fn[MAX_PATH] = L"document.html";
        OPENFILENAMEW ofn{ sizeof ofn };
        ofn.hwndOwner = g.main;
        ofn.lpstrFilter = L"HTML Document (*.html)\0*.html\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = fn;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"html";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn)) return;
        g_edPath = fn;
        std::wstring wpath = fn;
        size_t slash = wpath.find_last_of(L"\\/");
        g_edTitle = (slash != std::wstring::npos) ? wpath.substr(slash + 1) : wpath;
    }
    if (g_edWeb) g_edWeb->PostWebMessageAsJson(L"{\"cmd\":\"dump\"}");
}

static void EditorLoadContent(const std::wstring& content) {
    std::wstring escaped;
    for (wchar_t c : content) {
        if (c == L'\\') escaped += L"\\\\";
        else if (c == L'"') escaped += L"\\\"";
        else if (c == L'\r') escaped += L"\\r";
        else if (c == L'\n') escaped += L"\\n";
        else if (c == L'\t') escaped += L"\\t";
        else escaped += c;
    }
    std::wstring msg = L"{\"cmd\":\"load\",\"html\":\"" + escaped + L"\"}";
    if (g_edWeb && g_edReady) {
        g_edWeb->PostWebMessageAsJson(msg.c_str());
    } else {
        g_edPendingLoad = escaped;
    }
}

static void EditorOpenFile(const std::wstring& path) {
    std::string s;
    if (WxReadFile(path, s)) {
        g_edPath = path;
        size_t slash = path.find_last_of(L"\\/");
        g_edTitle = (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
        std::wstring whtml = WxW(s);
        EditorLoadContent(whtml);
        UpdateTitle();
    }
}

static void EditorOpen() {
    wchar_t fn[MAX_PATH] = L"";
    OPENFILENAMEW ofn{ sizeof ofn };
    ofn.hwndOwner = g.main;
    ofn.lpstrFilter = L"HTML / Text Documents (*.html;*.htm;*.txt)\0*.html;*.htm;*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fn;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        EditorOpenFile(fn);
    }
}

static void EditorFontMenu(HWND h, const RECT& anchor) {
    static const wchar_t* const fonts[] = {
        L"Segoe UI", L"Arial", L"Times New Roman", L"Georgia",
        L"Courier New", L"Consolas", L"Calibri", L"Verdana",
        L"Trebuchet MS", L"Aptos"
    };
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < (int)_countof(fonts); i++) {
        UINT flags = MF_STRING;
        if (g_edFont == fonts[i]) flags |= MF_CHECKED;
        AppendMenuW(m, flags, (UINT_PTR)(100 + i), fonts[i]);
    }
    POINT pt{ anchor.left, anchor.bottom };
    ClientToScreen(h, &pt);
    int pick = TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, h, nullptr);
    DestroyMenu(m);
    if (pick >= 100 && pick < 100 + (int)_countof(fonts)) {
        EditorPostFont(fonts[pick - 100]);
    }
}

// Receive messages from Quill

static HBITMAP g_edThumbBmp = nullptr;
static int g_edThumbW = 0, g_edThumbH = 0;

static std::vector<unsigned char> Base64Decode(const std::string& in) {
    std::vector<unsigned char> out;
    static const int T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((unsigned char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static void EditorSetThumb(const std::string& dataUrl) {
    size_t comma = dataUrl.find(',');
    if (comma == std::string::npos) return;
    std::string b64 = dataUrl.substr(comma + 1);
    auto bin = Base64Decode(b64);
    if (bin.empty()) return;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bin.size());
    if (!hMem) return;
    void* pMem = GlobalLock(hMem);
    if (!pMem) { GlobalFree(hMem); return; }
    memcpy(pMem, bin.data(), bin.size());
    GlobalUnlock(hMem);

    IStream* pStream = nullptr;
    if (SUCCEEDED(CreateStreamOnHGlobal(hMem, TRUE, &pStream)) && pStream) {
        Gdiplus::Bitmap bmp(pStream);
        if (bmp.GetLastStatus() == Gdiplus::Ok) {
            HBITMAP hbm = nullptr;
            bmp.GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &hbm);
            if (hbm) {
                if (g_edThumbBmp) DeleteObject(g_edThumbBmp);
                g_edThumbBmp = hbm;
                g_edThumbW = (int)bmp.GetWidth();
                g_edThumbH = (int)bmp.GetHeight();
            }
        }
        pStream->Release();
    }
    if (g.side) InvalidateRect(g.side, nullptr, FALSE);
}

static void EditorSideDraw(HDC dc, int w, int h) {
    RECT r{ 0, 0, w, h };
    FillRect(dc, &r, GrainBrush(1)); // the shared dark chrome

    int cardW = min((int)(w - 32 * g.dpi), (int)(130 * g.dpi));
    int cardH = (int)(cardW * 1.294);
    int padT = (int)(16 * g.dpi);
    int x = (w - cardW) / 2;
    int y = padT;

    RECT cardR{ x, y, x + cardW, y + cardH };

    // Selection background band
    int bandTop = max(0, y - (int)(6 * g.dpi));
    int bandBot = min(h, y + cardH + (int)(26 * g.dpi));
    RECT bandR{ 0, bandTop, w, bandBot };
    {
        Gdiplus::Graphics gs(dc);
        Gdiplus::LinearGradientBrush gb(
            Gdiplus::Rect(bandR.left, bandR.top, bandR.right - bandR.left, bandR.bottom - bandR.top),
            Gdiplus::Color(255, 0x38, 0x75, 0x52),
            Gdiplus::Color(255, 0x10, 0x4e, 0x2b), 90.0f);
        gs.FillRectangle(&gb, (int)bandR.left, (int)bandR.top,
                         (int)(bandR.right - bandR.left), (int)(bandR.bottom - bandR.top));
    }

    // Card white background
    HBRUSH wbr = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(dc, &cardR, wbr);

    // Blit thumbnail if available
    if (g_edThumbBmp && g_edThumbW > 0 && g_edThumbH > 0) {
        HDC mdc = CreateCompatibleDC(dc);
        HGDIOBJ obm = SelectObject(mdc, g_edThumbBmp);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, x, y, cardW, cardH, mdc, 0, 0, g_edThumbW, g_edThumbH, SRCCOPY);
        SelectObject(mdc, obm);
        DeleteDC(mdc);
    }

    // Border around card
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x55, 0x55, 0x55));
    HGDIOBJ open = SelectObject(dc, pen);
    HGDIOBJ onb = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x, y, x + cardW, y + cardH);
    SelectObject(dc, onb);
    SelectObject(dc, open);
    DeleteObject(pen);

    // Label "Page 1"
    RECT lr{ 0, y + cardH + (int)(4 * g.dpi), w, y + cardH + (int)(24 * g.dpi) };
    SelectObject(dc, CaptionFont(g_tbcap, 13, false, L"Segoe UI"));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xff, 0xff, 0xff));
    DrawTextW(dc, L"Page 1", -1, &lr, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

static void EditorHandleMessage(const wchar_t* json) {
    if (!json) return;
    std::string u8 = WxU(json);
    crxk::J m;
    if (!crxk::JParse(u8, m) || m.t != 'o') return;
    const crxk::J* typ = m.get("type");
    if (!typ) return;
    std::string t = typ->str();
    if (t == "ready") {
        g_edReady = true;
        if (!g_edPendingLoad.empty()) {
            std::wstring msg = L"{\"cmd\":\"load\",\"html\":\"" + g_edPendingLoad + L"\"}";
            g_edPendingLoad.clear();
            if (g_edWeb) g_edWeb->PostWebMessageAsJson(msg.c_str());
        }
    } else if (t == "state") {
        g_edBold = m.get("b") ? m.get("b")->b : false;
        g_edItalic = m.get("i") ? m.get("i")->b : false;
        g_edUnderline = m.get("u") ? m.get("u")->b : false;
        g_edStrike = m.get("s") ? m.get("s")->b : false;
        g_edHeader = m.get("h") ? (int)m.get("h")->d : 0;
        g_edQuote = m.get("quote") ? m.get("quote")->b : false;
        std::string lst = m.get("list") ? m.get("list")->str() : "";
        g_edList = (lst == "bullet") ? 1 : (lst == "ordered" ? 2 : 0);
        if (m.get("font")) {
            std::wstring f = WxW(m.get("font")->str());
            if (!f.empty()) g_edFont = f;
        }
        if (m.get("size")) {
            std::string sz = m.get("size")->str();
            int n = atoi(sz.c_str());
            if (n > 0) g_edSize = n;
        }
        if (g_etool) InvalidateRect(g_etool, nullptr, FALSE);
    } else if (t == "save") {
        std::string html = m.get("html") ? m.get("html")->str() : "";
        std::string title = m.get("title") ? m.get("title")->str() : "";
        if (!title.empty()) g_edTitle = WxW(title);
        if (!g_edPath.empty() && !html.empty()) {
            std::string fullDoc = "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>"
                                  + WxU(g_edTitle) + "</title>\n</head>\n<body>\n"
                                  + html + "\n</body>\n</html>";
            WxWriteFile(g_edPath, (const unsigned char*)fullDoc.data(), fullDoc.size());
        }
        UpdateTitle();
    } else if (t == "title") {
        std::string title = m.get("title") ? m.get("title")->str() : "";
        if (!title.empty()) { g_edTitle = WxW(title); UpdateTitle(); }
    } else if (t == "thumbs") {
        if (auto th = m.get("thumbs")) {
            if (th->isArr() && !th->a.empty() && th->a[0].isStr()) {
                EditorSetThumb(th->a[0].str());
            }
        }
    }
}

static void EditorRefocus() {
    if (g_edCtr) {
        g_edCtr->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

// Load embedded HTML template from RCDATA 301 (deflated or raw)
static std::wstring EditorGetHtml() {
    HRSRC hs = FindResourceW(g.hinst, MAKEINTRESOURCEW(301), (LPCWSTR)RT_RCDATA);
    if (!hs) return L"";
    HGLOBAL hg = LoadResource(g.hinst, hs);
    if (!hg) return L"";
    const unsigned char* data = (const unsigned char*)LockResource(hg);
    DWORD len = SizeofResource(g.hinst, hs);
    if (!data || !len) return L"";

    // Decompress deflated payload using crxk::Inflate (limit 2MB)
    std::vector<unsigned char> decompressed;
    if (crxk::Inflate(data, len, decompressed, 2 * 1024 * 1024) && !decompressed.empty()) {
        std::string s((const char*)decompressed.data(), decompressed.size());
        return WxW(s);
    }
    std::string s((const char*)data, len);
    return WxW(s);
}

static void EditorEnsureInit() {
    if (g_edCtr || g_edBuilding) return;
    WebEnsureEnv();
    if (!g_wvEnv) return;
    g_edBuilding = true;
    g_wvEnv->CreateCoreWebView2Controller(g.view,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                g_edBuilding = false;
                if (FAILED(hr) || !c) return S_OK;
                g_edCtr = c;
                c->get_CoreWebView2(&g_edWeb);
                wrl::ComPtr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2)
                    c2->put_DefaultBackgroundColor({ 255, 0x3d, 0x40, 0x46 });
                EventRegistrationToken tkm;
                g_edWeb->add_WebMessageReceived(
                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                            LPWSTR msg = nullptr;
                            if (SUCCEEDED(a->get_WebMessageAsJson(&msg)) && msg) {
                                EditorHandleMessage(msg);
                                CoTaskMemFree(msg);
                            }
                            return S_OK;
                        }).Get(), &tkm);
                std::wstring html = EditorGetHtml();
                if (!html.empty()) {
                    g_edWeb->NavigateToString(html.c_str());
                }
                if (g_editOn) {
                    g_edCtr->put_IsVisible(TRUE);
                    RECT r; GetClientRect(g.view, &r);
                    g_edCtr->put_Bounds(r);
                    EditorRefocus();
                } else {
                    g_edCtr->put_IsVisible(FALSE);
                }
                return S_OK;
            }).Get());
}

static void EditorToggle(bool on) {
    if (on == g_editOn) return;
    g_editOn = on;
    if (on) {
        if (g_webOn) WebToggle(false);
        if (g.side && !g.full) ShowWindow(g.side, SW_SHOWNOACTIVATE);
        if (g.rail && !g.full) ShowWindow(g.rail, SW_SHOWNOACTIVATE);
        if (g.status) ShowWindow(g.status, SW_HIDE);
        if (g.bar) ShowWindow(g.bar, SW_HIDE);
        if (g_wstrip) ShowWindow(g_wstrip, SW_HIDE);
        if (g_wtool) ShowWindow(g_wtool, SW_HIDE);
        if (g_etool) ShowWindow(g_etool, SW_SHOWNOACTIVATE);
        EditorEnsureInit();
        if (g_edCtr) {
            g_edCtr->put_IsVisible(TRUE);
            RECT r; GetClientRect(g.view, &r);
            g_edCtr->put_Bounds(r);
            EditorRefocus();
        }
    } else {
        if (g_edCtr) g_edCtr->put_IsVisible(FALSE);
        if (g_etool) ShowWindow(g_etool, SW_HIDE);
        if (!g_webOn) {
            if (g.bar) ShowWindow(g.bar, SW_SHOWNOACTIVATE);
            if (g.status && !g.full) ShowWindow(g.status, SW_SHOWNOACTIVATE);
            if (g.side && g.sideOn && DVF().n && !g.full) ShowWindow(g.side, SW_SHOWNOACTIVATE);
            if (g.rail && !g.full) ShowWindow(g.rail, SW_SHOWNOACTIVATE);
        }
    }
    LayoutMain();
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    UpdateTitle();
}

static int EditorBtnWidth(int i) {
    if (i == EB_SEP1) return (int)(10 * g.dpi);
    return (int)(28 * g.dpi);
}

// Layout calculation for ribbon buttons - left-aligned
static RECT EditorBtnRect(int id, int w, int h) {
    int btnH = (int)(28 * g.dpi);
    int yMid = (h - btnH) / 2;
    int gap = (int)(4 * g.dpi);

    int curX = (int)(8 * g.dpi);

    for (int i = 0; i < EB_COUNT; i++) {
        int bw = EditorBtnWidth(i);
        RECT r{ curX, yMid, curX + bw, yMid + btnH };
        if (i == id) return r;
        curX += bw + gap;
    }
    return RECT{ 0, 0, 0, 0 };
}

static void EditorToolDraw(HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    RECT bar{ 0, 0, w, h };
    FillRect(dc, &bar, GrainBrush(1));

    // Bottom fade
    {
        Gdiplus::Graphics gfade(dc);
        Gdiplus::Color c0(0, 0, 0, 0);
        Gdiplus::Color c1(0x22, 0, 0, 0);
        float fh = (float)h;
        Gdiplus::RectF fadeR(0.f, fh * 0.45f, (float)w, fh * 0.55f);
        Gdiplus::LinearGradientBrush fadeBr(fadeR, c0, c1, Gdiplus::LinearGradientModeVertical);
        gfade.FillRectangle(&fadeBr, fadeR);
    }

    SetBkMode(dc, TRANSPARENT);
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    for (int i = 0; i < EB_COUNT; i++) {
        RECT r = EditorBtnRect(i, w, h);
        if (r.right <= r.left) continue;

        if (i == EB_SEP1) {
            int cx = (r.left + r.right) / 2;
            int sy0 = (h - (int)(18 * g.dpi)) / 2;
            HPEN sp = CreatePen(PS_SOLID, 1, RGB(0x45, 0x48, 0x50));
            HGDIOBJ osp = SelectObject(dc, sp);
            MoveToEx(dc, cx, sy0, nullptr);
            LineTo(dc, cx, sy0 + (int)(18 * g.dpi));
            SelectObject(dc, osp);
            DeleteObject(sp);
            continue;
        }

        bool hot = (g_edHot == i);
        bool pr = (g_edDown == i && hot);

        if (hot || pr) {
            Gdiplus::GraphicsPath p;
            float rr = 3.0f * (float)g.dpi;
            float fx = (float)r.left, fy = (float)r.top;
            float fw = (float)(r.right - r.left), fh = (float)(r.bottom - r.top);
            p.AddArc(fx, fy, 2 * rr, 2 * rr, 180, 90);
            p.AddArc(fx + fw - 2 * rr, fy, 2 * rr, 2 * rr, 270, 90);
            p.AddArc(fx + fw - 2 * rr, fy + fh - 2 * rr, 2 * rr, 2 * rr, 0, 90);
            p.AddArc(fx, fy + fh - 2 * rr, 2 * rr, 2 * rr, 90, 90);
            p.CloseFigure();

            Gdiplus::Color col;
            if (pr) col = Gdiplus::Color(255, 0x26, 0x29, 0x30);
            else col = Gdiplus::Color(255, 0x3c, 0x41, 0x4c);

            Gdiplus::SolidBrush fbr(col);
            gs.FillPath(&fbr, &p);
        }

        COLORREF ink;
        if (hot || pr) ink = RGB(0xf2, 0xf4, 0xf8);
        else ink = RGB(0xc4, 0xc8, 0xd0);

        HFONT btnF = CaptionFont(g_tbglyph, 14, false, L"Segoe MDL2 Assets");
        const wchar_t* text = L"";

        switch (i) {
        case EB_WEB: text = L"\uE774"; break;
        case EB_PDF: text = L"\uE8F1"; break;
        case EB_OPEN: text = L"\uE8DA"; break;
        case EB_SAVE: text = L"\uE74E"; break;
        }

        if (btnF && text[0]) {
            HGDIOBJ obf = SelectObject(dc, btnF);
            SetTextColor(dc, ink);
            DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, obf);
        }
    }
}

static void EditorClick(int id, HWND h, const RECT& r) {
    switch (id) {
    case EB_PDF:
        EditorToggle(false);
        break;
    case EB_WEB:
        EditorToggle(false);
        WebToggle(true);
        break;
    case EB_OPEN:
        EditorOpen();
        break;
    case EB_SAVE:
        EditorSave();
        break;
    }
    EditorRefocus();
}

static HDC g_etooldc = nullptr;
static HBITMAP g_etoolbm = nullptr;
static int g_etoolw = 0, g_etoolh = 0;

static void EnsureEditorToolBack(HDC ref, int w, int h) {
    if (g_etooldc && g_etoolbm && g_etoolw == w && g_etoolh == h) return;
    if (g_etoolbm) DeleteObject(g_etoolbm);
    if (!g_etooldc) g_etooldc = CreateCompatibleDC(ref);
    g_etoolbm = CreateCompatibleBitmap(ref, max(1, w), max(1, h));
    SelectObject(g_etooldc, g_etoolbm);
    g_etoolw = w; g_etoolh = h;
}

static LRESULT CALLBACK EditorToolProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureEditorToolBack(dc, r.right, r.bottom);
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
            SetBrushOrgEx(g_etooldc, -po.x, -po.y, nullptr);
            EditorToolDraw(g_etooldc, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, g_etooldc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT r; GetClientRect(h, &r);
        POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
        SetBrushOrgEx((HDC)wp, -po.x, -po.y, nullptr);
        EditorToolDraw((HDC)wp, r.right, r.bottom);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT cl; GetClientRect(h, &cl);
        int hot = -1;
        for (int i = 0; i < EB_COUNT; i++) {
            if (i == EB_SEP1) continue;
            RECT r = EditorBtnRect(i, cl.right, cl.bottom);
            if (PtInRect(&r, p)) { hot = i; break; }
        }
        if (hot != g_edHot) {
            g_edHot = hot;
            if (hot >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_edHot >= 0) {
            g_edHot = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT cl; GetClientRect(h, &cl);
        for (int i = 0; i < EB_COUNT; i++) {
            if (i == EB_SEP1) continue;
            RECT r = EditorBtnRect(i, cl.right, cl.bottom);
            if (PtInRect(&r, p)) {
                SetCapture(h);
                g_edDown = i; g_edHot = i;
                InvalidateRect(h, nullptr, FALSE);
                break;
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_edDown >= 0) {
            int down = g_edDown;
            g_edDown = -1;
            if (GetCapture() == h) ReleaseCapture();
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT cl; GetClientRect(h, &cl);
            RECT r = EditorBtnRect(down, cl.right, cl.bottom);
            bool inside = PtInRect(&r, p) != 0;
            g_edHot = -1;
            InvalidateRect(h, nullptr, FALSE);
            if (inside) {
                EditorClick(down, h, r);
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_edDown >= 0) { g_edDown = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT cl; GetClientRect(h, &cl);
            for (int i = 0; i < EB_COUNT; i++) {
                if (i == EB_SEP1) continue;
                RECT r = EditorBtnRect(i, cl.right, cl.bottom);
                if (PtInRect(&r, p)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
        }
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}
