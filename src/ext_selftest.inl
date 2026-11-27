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
    bool check = false;                // --extcheck: the restart road, not the install road
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
// --extcheck's verdict, polled while WxInit's roads run. The folders are the
// truth: every one must come back riding a live registration, and every one
// with a popup must carry its enabled switch - exactly the state the toolbar's
// icons are drawn from. A folder whose registration died lands here as FAIL
// with the machine's own words, never a guess.
static void WxCheckPoll() {
    std::wstring root = WxRoot();
    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            dirs.push_back(root + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (dirs.empty()) { WxTestRep(L"check: no extension folders under %s yet", root.c_str()); return; }
    bool all = true;
    for (auto& d : dirs) {
        std::wstring nm = d.substr(d.find_last_of(L'\\') + 1);
        WebExt probe; probe.dir = d; probe.id = nm; probe.storeId = nm;
        WxLoadInfo(probe);                        // the manifest: name, popup, icon
        if (probe.icon) { delete probe.icon; probe.icon = nullptr; }
        const WebExt* e = nullptr;
        for (auto& o : g_wx) if (o.dir == d) { e = &o; break; }
        bool ok = e && e->ext && (e->popup.empty() || e->enabled);
        WxTestRep(L"check: %s '%s' - %s", nm.c_str(), probe.name.c_str(),
                  !e ? L"NOT LISTED" : !e->ext ? L"NO LIVE REGISTRATION" :
                  !ok ? L"OFF" : L"on");
        if (!ok) all = false;
    }
    if (all) { WxTestRep(L"VERDICT: PASS - the restart persists every extension the toolbar draws"); WxTestFinish(0); }
}
static LRESULT CALLBACK WxTestProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER:
        if (wp == 1 && !g_xt.fin) { WxTestRep(L"TIMEOUT after 120s - the engine stopped answering"); WxTestFinish(2); }
        if (wp == 2 && !g_xt.fin && g_xt.check) WxCheckPoll();
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
                L"--disable-features=ExtensionManifestV2Unsupported,ExtensionManifestV2Disabled"
                L" --disable-gpu-compositing");   // the same road the GUI runs, flags and all
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
                                    if (g_xt.check) {          // the restart road itself: no download,
                                        SetTimer(g_xt.w, 2, 250, nullptr);   // and no install - the exact
                                        WxInit();              // WxInit a real launch runs, and the
                                        return;                // poll decides what the toolbar will see
                                    }
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

