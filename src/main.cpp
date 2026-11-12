// Docsy - minimal, fast, native Win32 PDF reader.
// Modularized architecture: components separated into clean modules.

#include "common.h"
#include "pdf_render.inl"
#include "tabs.inl"
#include "annotations.inl"
#include "page_view.inl"
#include "tab_strip.inl"
#include "footer.inl"
#include "crx_unpack.inl"
#include "extensions.inl"
#include "browser.inl"
#include "find_bar.inl"
#include "sidebar.inl"
#include "doc_info.inl"
#include "save_dialog.inl"
#include "main_window.inl"
#include "ext_selftest.inl"

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
    // --extcheck <id|store-url> [out=path]: the restart road in the same harness -
    // a fresh process over the profile a previous one left behind, running the
    // GUI's own WxInit, and polling until every folder rides a live registration
    // the way the toolbar's icons need. CI chains it after --exttest, so "the
    // icons vanish on reopen" is a verdict a build carries, not a user's word.
    if (argc >= 2 && (!lstrcmpW(argv[1], L"--exttest") || !lstrcmpW(argv[1], L"--extdiag") || !lstrcmpW(argv[1], L"--extcheck"))) {
        g_xt.check = !lstrcmpW(argv[1], L"--extcheck");
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
                t1.url = L"https://www.google.com/"; t1.title = L"Google";
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

    WxLoadDisk();
    ShowWindow(g.main, show);
    UpdateWindow(g.main);
    if (argc > 1) TabOpenInto(false, argv[1]);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
