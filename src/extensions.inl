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
    std::wstring engineId;                 // the engine's own id for this folder, recorded in dir\.docsy-engine-id
    std::wstring phId[2];                  // the ids the folder's path hashes to (current + legacy loader rule)
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
static bool g_wxVerifyRan = false;                 // the late load's one verification pass, per session
static UINT_PTR g_wxVerifyTimer = 0;
static UINT_PTR g_wxInitWatch = 0;                 // the listing's own leash: if GetBrowserExtensions'
                                                   // completion never comes back (a wedged engine, a
                                                   // browser process that died mid-call), init finishes
                                                   // from the folders anyway - the faces never hang on it
static void WebEnsureEnv();
static void WxEngineRestart();
static void WxFinishInstall(const std::wstring& id, const std::wstring& dir, HRESULT hr, ICoreWebView2BrowserExtension* ext);
static const wchar_t* WxHrName(HRESULT hr);
static void WebBarLayout();
static void WebNewTab(std::wstring url, bool makeCurrent);
static void WxLoadDisk();

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

// ---- which registration belongs to which folder. The engine answers an added
// folder with its OWN id - for an unpacked load it derives one from the folder's
// path (Chromium's GenerateIdForPath: today the path's own bytes with the drive
// letter forced upper, in older loaders the lowercased ASCII path), and the
// manifest "key" this app writes into store packages does not always pin it.
// ICoreWebView2BrowserExtension carries no folder path, so the restart needs
// notes of its own: the engine id is recorded beside the folder the moment it is
// learned (WxFinishInstall, WxInit's adoption and re-add), and the path-hash ids
// are recomputed from the folder. A folder whose registration is found by any of
// these keeps it - re-adding over a living registration answers
// ERROR_INVALID_STATE, and that answer is what used to strip the icons. ----
static std::wstring WxMarkerPath(const std::wstring& dir) { return dir + L"\\.docsy-engine-id"; }
static void WxSaveEngineId(const std::wstring& dir, const std::wstring& id) {
    if (!WxIsExtId(id)) return;
    std::string s = WxU(id);
    WxWriteFile(WxMarkerPath(dir), (const unsigned char*)s.data(), s.size());
}
static std::wstring WxLoadEngineId(const std::wstring& dir) {
    std::string s;
    if (!WxReadFile(WxMarkerPath(dir), s)) return L"";
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return L"";
    std::wstring id = WxW(s.substr(a, b - a + 1));
    return WxIsExtId(id) ? id : L"";
}
// the user's own on/off switch, beside the folder. The engine's remembered
// enabled state has been the poisoned well this bug drank from: an install that
// died half-landed left 'off' behind, and every restart then read that poison
// back and hid the icon again. docsy keeps the verdict where the folder is.
static std::wstring WxOnPath(const std::wstring& dir) { return dir + L"\\.docsy-on"; }
static void WxSaveOn(const std::wstring& dir, bool on) {
    WxWriteFile(WxOnPath(dir), (const unsigned char*)(on ? "1" : "0"), 1);
}
static bool WxLoadOn(const std::wstring& dir) {
    std::string s;
    if (!WxReadFile(WxOnPath(dir), s) || s.empty()) return true;   // never written: on
    return s[0] != '0';
}
static std::wstring WxPathHashId(const std::wstring& dir, bool native) {
    std::wstring p = dir;
    while (!p.empty() && (p[p.size() - 1] == L'\\' || p[p.size() - 1] == L'/')) p.pop_back();
    std::string bytes;
    if (native) {                          // today's loader: UTF-16LE bytes, drive letter forced upper
        if (p.size() >= 2 && p[0] >= L'a' && p[0] <= L'z' && p[1] == L':')
            p[0] = (wchar_t)(p[0] - L'a' + L'A');
        bytes.assign((const char*)p.data(), p.size() * sizeof(wchar_t));
    } else {                               // the older loader: the lowercased ASCII path
        bool ascii = true;
        for (wchar_t c : p) { if (c > 0x7f) { ascii = false; break; } bytes += (char)tolower((unsigned char)c); }
        if (!ascii) bytes.clear();         // a non-ASCII path hashed as the empty string, faithfully
    }
    unsigned char h[32];
    crxk::Sha256((const unsigned char*)bytes.data(), bytes.size(), h);   // the hash lives in the unpacker
    std::string id;
    for (int i = 0; i < 16; i++) { id += (char)('a' + (h[i] >> 4)); id += (char)('a' + (h[i] & 15)); }
    return WxW(id);
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
    for (int i = 0; i < (int)g_wx.size(); i++)
        if (g_wx[i].id == id || (!g_wx[i].storeId.empty() && g_wx[i].storeId == id) ||
            (!g_wx[i].engineId.empty() && g_wx[i].engineId == id)) return i;
    return -1;
}

// ---- the profile, the existing extensions, loading before the first page ----
static wrl::ComPtr<ICoreWebView2Profile7> WxProfile() {
    for (auto& t : g_wt) {
        if (!t.web) continue;
        wrl::ComPtr<ICoreWebView2_13> w13; wrl::ComPtr<ICoreWebView2Profile> p;
        if (SUCCEEDED(t.web.As(&w13)) && w13 && SUCCEEDED(w13->get_Profile(&p)) && p) {
            wrl::ComPtr<ICoreWebView2Profile7> p7;
            if (SUCCEEDED(p.As(&p7)) && p7) { g_wxProf = p7; return g_wxProf; }
        }
    }
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
// failed install can leave one behind) makes AddBrowserExtension answer ERROR_INVALID_STATE: so
// every registration this folder can ride is removed first - by the store id it is named by, by
// the engine id the last install recorded, by either id the folder's path can hash to and, as the
// weakest net, by the manifest's own name - then the folder is added fresh
static void WxAddFolderFresh(const std::wstring& dir, const std::wstring& id,
                             std::function<void(HRESULT, ICoreWebView2BrowserExtension*)> done) {
    wrl::ComPtr<ICoreWebView2Profile7> p = WxProfile();
    if (!p) { done(E_NOINTERFACE, nullptr); return; }
    std::vector<std::wstring> ids; ids.push_back(id);
    {   std::wstring mk = WxLoadEngineId(dir);
        if (!mk.empty()) ids.push_back(mk);
        std::wstring ph0 = WxPathHashId(dir, true), ph1 = WxPathHashId(dir, false);
        if (!ph1.empty() && ph1 != ph0) ids.push_back(ph1);
        if (!ph0.empty()) ids.push_back(ph0);
    }
    std::wstring fname;                            // the manifest's own name, the last-resort key
    {   WebExt probe; probe.dir = dir;             // (an update that renamed the extension lands here)
        if (WxLoadInfo(probe)) fname = probe.name;
        if (probe.icon) { delete probe.icon; probe.icon = nullptr; }
    }
    HRESULT hr = p->GetBrowserExtensions(
        Microsoft::WRL::Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [dir, id, ids, fname, done](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                auto stale = std::make_shared<std::vector<wrl::ComPtr<ICoreWebView2BrowserExtension>>>();
                UINT n = 0;
                if (SUCCEEDED(hr) && list && SUCCEEDED(list->get_Count(&n)))
                    for (UINT i = 0; i < n; i++) {
                        wrl::ComPtr<ICoreWebView2BrowserExtension> e;
                        if (FAILED(list->GetValueAtIndex(i, &e)) || !e) continue;
                        LPWSTR rid = nullptr;
                        if (SUCCEEDED(e->get_Id(&rid)) && rid) {
                            std::wstring rs = rid;
                            CoTaskMemFree(rid);
                            bool hit = false;
                            for (auto& s : ids) if (rs == s) { hit = true; break; }
                            if (!hit && !fname.empty()) {          // the name answers only when every
                                LPWSTR rn = nullptr;               // id road missed, so an innocent
                                std::wstring rns;                  // namesake is removed only when the
                                if (SUCCEEDED(e->get_Name(&rn)) && rn) { rns = rn; CoTaskMemFree(rn); }
                                if (!rns.empty() && !_wcsicmp(rns.c_str(), fname.c_str())) hit = true;
                            }
                            if (hit) stale->push_back(e);
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
static void CALLBACK WxVerifyTimer(HWND, UINT, UINT_PTR, DWORD);
static void CALLBACK WxInitWatchTimer(HWND, UINT, UINT_PTR, DWORD);
static void WxFinishInit() {
    g_wxReady = true; g_wxBusy = false;
    if (g_wxInitWatch) { KillTimer(nullptr, g_wxInitWatch); g_wxInitWatch = 0; }
    for (auto& t : g_wt)
        if (t.web && !t.pending.empty()) { t.web->Navigate(t.pending.c_str()); t.pending.clear(); }
    WxLoadDisk();
    if (g_webOn) WebBarLayout();
    WebRepaintChrome();
    if (!g_wxVerifyRan && !g_wxVerifyTimer)      // one pass per session, a few seconds out:
        g_wxVerifyTimer = SetTimer(nullptr, 0, 4000, WxVerifyTimer);   // the engine's own load of
                                                   // the profile's persisted extensions can finish
                                                   // after every road above ran - the folders that
                                                   // rode no registration get one more look
    if (!g_wxRetryId.empty()) {                    // the engine that refused this install is
        std::wstring dir = g_wxRetryDir, id = g_wxRetryId;   // gone; the fresh one retries it
        g_wxRetryDir.clear(); g_wxRetryId.clear();
        WxAddFolderFresh(dir, id, [id, dir](HRESULT hr, ICoreWebView2BrowserExtension* ext) {
            WxFinishInstall(id, dir, hr, ext);
        });
    }
}
// the add answered "already here". A live registration the matching pass could
// not see IS a working extension - the engine runs it, only its id was not one
// the folder could predict, and the old answer to that success was to switch
// the entry off and hide its face on every restart. Adopt the live registration:
// by any id the folder can name, then by its name when that name is alone in
// both lists; the marker saves the adopted id, so the next restart matches
// exactly and this road goes quiet for good.
static void WxAdoptLive(const std::wstring& dir, const std::wstring& id) {
    wrl::ComPtr<ICoreWebView2Profile7> p = WxProfile();
    if (!p) return;
    HRESULT hr = p->GetBrowserExtensions(
        Microsoft::WRL::Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [dir, id](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                UINT n = 0;
                if (FAILED(hr) || !list || FAILED(list->get_Count(&n))) {
                    WxLogf(L"adopt: %s - the listing failed 0x%08lX", id.c_str(), (unsigned long)hr);
                    return S_OK;
                }
                std::vector<wrl::ComPtr<ICoreWebView2BrowserExtension>> have;
                std::vector<std::wstring> rids, rnames;
                std::vector<BOOL> rens;
                for (UINT i = 0; i < n; i++) {
                    wrl::ComPtr<ICoreWebView2BrowserExtension> e;
                    if (FAILED(list->GetValueAtIndex(i, &e)) || !e) continue;
                    LPWSTR s = nullptr;
                    std::wstring rid, rn; BOOL en = TRUE;
                    if (SUCCEEDED(e->get_Id(&s)) && s) { rid = s; CoTaskMemFree(s); }
                    if (SUCCEEDED(e->get_Name(&s)) && s) { rn = s; CoTaskMemFree(s); }
                    e->get_IsEnabled(&en);
                    have.push_back(e); rids.push_back(rid); rnames.push_back(rn); rens.push_back(en);
                }
                std::wstring mk = WxLoadEngineId(dir), ph0 = WxPathHashId(dir, true),
                             ph1 = WxPathHashId(dir, false);
                WebExt probe; probe.dir = dir;             // the manifest's name, the last road
                std::wstring fname;
                if (WxLoadInfo(probe)) fname = probe.name;
                if (probe.icon) { delete probe.icon; probe.icon = nullptr; }
                auto owned = [&rids](size_t j) {
                    for (auto& o : g_wx) if (o.ext && o.id == rids[j]) return true;
                    return false;
                };
                int pick = -1; const wchar_t* how = L"";
                for (size_t j = 0; j < have.size(); j++) {
                    if (rids[j].empty() || owned(j)) continue;
                    if (rids[j] == id || (!mk.empty() && rids[j] == mk) ||
                        (!ph0.empty() && rids[j] == ph0) || (!ph1.empty() && rids[j] == ph1)) {
                        pick = (int)j; how = L"id"; break;
                    }
                }
                if (pick < 0 && !fname.empty()) {          // the name, only when it is alone
                    int same = 0;                          // in both lists, so a namesake
                    for (auto& o : g_wx) if (!_wcsicmp(o.name.c_str(), fname.c_str())) same++;
                    int hits = 0, at = -1;
                    for (size_t j = 0; j < have.size(); j++)
                        if (!rnames[j].empty() && !owned(j) &&
                            !_wcsicmp(rnames[j].c_str(), fname.c_str())) { hits++; at = (int)j; }
                    if (same == 1 && hits == 1) { pick = at; how = L"name"; }
                }
                if (pick < 0) {
                    WxLogf(L"adopt: %s found no live registration to claim (%u listed)", id.c_str(), (unsigned)n);
                    WxNotifyTabs();                    // the store page's busy pill resets too
                    return S_OK;
                }
                int k = WxFind(id);
                if (k < 0) { WxNotifyTabs(); return S_OK; }
                g_wx[k].ext = have[pick];
                g_wx[k].id = rids[pick];
                WxSaveEngineId(g_wx[k].dir, rids[pick]);   // learned once, matched forever
                bool want = WxLoadOn(g_wx[k].dir);
                if (want != (rens[pick] != FALSE))         // the user's switch rules, and a
                    g_wx[k].ext->Enable(want ? TRUE : FALSE,   // poisoned 'off' is healed here
                        Microsoft::WRL::Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
                            [](HRESULT) { return S_OK; }).Get());
                g_wx[k].enabled = want;
                WxLogf(L"adopt: %s rides live registration %s (matched by %s), %s",
                       id.c_str(), rids[pick].c_str(), how, want ? L"on" : L"off");
                if (g_webOn) WebBarLayout();
                WebRepaintChrome();
                WxNotifyTabs();
                return S_OK;
            }).Get());
    if (FAILED(hr))                        // a synchronous refusal is a fact worth keeping:
        WxLogf(L"adopt: %s - the listing refused synchronously 0x%08lX", id.c_str(), (unsigned long)hr);
}
static void WxLoadDisk() {
    if (!g_wx.empty()) return;
    std::wstring root = WxRoot();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            WebExt e; e.id = fd.cFileName; e.storeId = e.id; e.dir = root + L"\\" + fd.cFileName;
            e.engineId = WxLoadEngineId(e.dir);
            if (!e.engineId.empty()) e.id = e.engineId;
            e.phId[0] = WxPathHashId(e.dir, true);
            e.phId[1] = WxPathHashId(e.dir, false);
            if (e.phId[1] == e.phId[0]) e.phId[1].clear();
            if (WxLoadInfo(e)) {
                e.enabled = WxLoadOn(e.dir);
                g_wx.push_back(std::move(e));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(g_wx.begin(), g_wx.end(), [](const WebExt& a, const WebExt& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}
static void WxInit() {
    WxLoadDisk();
    if (g_webOn) WebBarLayout();
    WebRepaintChrome();
    if (g_wxReady || g_wxBusy) return;
    wrl::ComPtr<ICoreWebView2Profile7> p = WxProfile();
    if (!p) return;
    g_wxBusy = true;
    g_wxInitWatch = SetTimer(nullptr, 0, 9000, WxInitWatchTimer);   // the listing's leash: a wedged
                                                                    // engine answers nothing, and the
                                                                    // folders' faces must not wait on it
    HRESULT hr0 = p->GetBrowserExtensions(
        Microsoft::WRL::Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                g_wxOk = SUCCEEDED(hr);        // the fresh engine's own verdict - a restart
                std::vector<wrl::ComPtr<ICoreWebView2BrowserExtension>> have;   // resets this
                std::vector<std::wstring> rids, rnames;   // the registrations' own words
                std::vector<BOOL> rens;
                UINT n = 0;
                if (SUCCEEDED(hr) && list && SUCCEEDED(list->get_Count(&n)))
                    for (UINT i = 0; i < n; i++) {
                        wrl::ComPtr<ICoreWebView2BrowserExtension> e;
                        if (SUCCEEDED(list->GetValueAtIndex(i, &e)) && e) {
                            LPWSTR s = nullptr;
                            std::wstring rid, rn; BOOL en = TRUE;
                            if (SUCCEEDED(e->get_Id(&s)) && s) { rid = s; CoTaskMemFree(s); }
                            if (SUCCEEDED(e->get_Name(&s)) && s) { rn = s; CoTaskMemFree(s); }
                            e->get_IsEnabled(&en);
                            have.push_back(e); rids.push_back(rid); rnames.push_back(rn); rens.push_back(en);
                            WxLogf(L"init: registration %s '%s' enabled=%u", rid.c_str(), rn.c_str(), en != FALSE);
                        }
                    }
                // our folders are the source of truth for names, icons and popups.
                if (g_wx.empty()) WxLoadDisk();
                // the restart's matching. The folder name is the store id, the marker
                // remembers the engine id the install landed under, the path hash names
                // what the unpacked loader derives, and the manifest name is the last
                // net - the engine answers with its OWN id, the store id alone never
                // matched, so every folder walked the re-add road and the add over the
                // still-living registration answered ERROR_INVALID_STATE, flipped
                // enabled off and dropped every icon on reopen.
                std::vector<char> claimed(have.size(), 0);
                std::vector<int> toAdd;
                for (int i = 0; i < (int)g_wx.size(); i++) {
                    int pick = -1;
                    const wchar_t* how = L"";
                    for (size_t j = 0; j < have.size(); j++) {
                        if (claimed[j] || rids[j].empty()) continue;
                        const WebExt& e = g_wx[i];
                        if (rids[j] == e.id || rids[j] == e.storeId) { pick = (int)j; how = L"store id"; break; }
                        if (!e.engineId.empty() && rids[j] == e.engineId) { pick = (int)j; how = L"engine marker"; break; }
                        if (!e.phId[0].empty() && rids[j] == e.phId[0]) { pick = (int)j; how = L"path hash"; break; }
                        if (!e.phId[1].empty() && rids[j] == e.phId[1]) { pick = (int)j; how = L"path hash (legacy)"; break; }
                    }
                    if (pick < 0 && !g_wx[i].name.empty()) {   // the name is matched only when one
                        int same = 0;                          // folder and one registration carry it:
                        for (int k = 0; k < (int)g_wx.size(); k++)   // a namesake never steals a claim
                            if (!_wcsicmp(g_wx[k].name.c_str(), g_wx[i].name.c_str())) same++;
                        if (same == 1) {
                            int hits = 0, at = -1;
                            for (size_t j = 0; j < have.size(); j++)
                                if (!claimed[j] && !rnames[j].empty() &&
                                    !_wcsicmp(rnames[j].c_str(), g_wx[i].name.c_str())) { hits++; at = (int)j; }
                            if (hits == 1) { pick = at; how = L"name"; }
                        }
                    }
                    if (pick < 0) { toAdd.push_back(i); continue; }
                    claimed[pick] = 1;
                    bool bridged = g_wx[i].id != rids[pick];
                    g_wx[i].id = rids[pick];               // the engine's id rules the URLs, and
                    g_wx[i].ext = have[pick];              // WxFind rides id, storeId, marker alike
                    bool want = WxLoadOn(g_wx[i].dir);     // the user's switch beside the folder
                    if (want != (rens[pick] != FALSE))     // rules, not the engine's memory - a
                        g_wx[i].ext->Enable(want ? TRUE : FALSE,   // profile that kept a poisoned
                            Microsoft::WRL::Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
                                [](HRESULT) { return S_OK; }).Get());   // 'off' gets healed here
                    g_wx[i].enabled = want;
                    WxSaveEngineId(g_wx[i].dir, rids[pick]);   // the next restart matches by the marker
                    if (bridged)
                        WxLogf(L"init: %s rides registration %s (matched by %s), %s",
                               g_wx[i].storeId.c_str(), rids[pick].c_str(), how, want ? L"on" : L"off");
                }
                for (size_t j = 0; j < have.size(); j++)
                    if (!claimed[j])
                        WxLogf(L"init: registration %s '%s' has no folder - left alone",
                               rids[j].c_str(), rnames[j].c_str());
                WxLogf(L"init: GetBrowserExtensions answered 0x%08lX, %u extension(s) registered",
                       (unsigned long)hr, (unsigned)have.size());
                if (FAILED(hr)) WxLogf(L"init: the listing itself failed - the engine has no working extension subsystem");
                WxLogf(L"init: %u installed, %zu folder(s), %zu to re-add", (unsigned)have.size(), g_wx.size(), toAdd.size());
                if (toAdd.empty()) { WxFinishInit(); return S_OK; }
                auto left = std::make_shared<int>((int)toAdd.size());
                for (int i : toAdd) {
                    std::wstring id = g_wx[i].id, dir = g_wx[i].dir;
                    // fresh, never plain: the profile remembers registrations across runs,
                    // and a plain add over one answers ERROR_INVALID_STATE - the exact
                    // death this path died on every restart
                    WxAddFolderFresh(dir, id, [id, dir, left](HRESULT r, ICoreWebView2BrowserExtension* ext) {
                        WxLogf(L"init: re-add %s -> 0x%08lX", id.c_str(), (unsigned long)r);
                        int k = WxFind(id);
                        if (k < 0) {
                            for (int j = 0; j < (int)g_wx.size(); j++)
                                if (g_wx[j].dir == dir) { k = j; break; }
                        }
                        if (k >= 0) {
                            if (SUCCEEDED(r) && ext) {
                                g_wx[k].ext = ext;
                                LPWSTR rid = nullptr;
                                if (SUCCEEDED(ext->get_Id(&rid)) && rid) {
                                    g_wx[k].id = rid;
                                    WxSaveEngineId(dir, rid);   // learned once, matched forever after
                                    CoTaskMemFree(rid);
                                }
                                g_wx[k].enabled = WxLoadOn(dir);   // the fresh load obeys the user's
                                if (!g_wx[k].enabled && g_wx[k].ext)   // switch, not the engine's mood
                                    g_wx[k].ext->Enable(FALSE,
                                        Microsoft::WRL::Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
                                            [](HRESULT) { return S_OK; }).Get());
                            } else if (r == HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) {
                                // "already here" is a working extension, not a failure: a live
                                // registration the id roads could not see. Adopt it - the old
                                // answer switched the entry off and hid its face on every restart.
                                WxAdoptLive(dir, id);
                            } else {
                                // the engine refused the folder - but the face is the
                                // FOLDER's, not the engine's: the user's own switch beside
                                // it decides, and a refused load wears the switch, not
                                // the engine's verdict. The icons of folders the engine
                                // cannot load are the machine's own fact in webext.log.
                                g_wx[k].enabled = WxLoadOn(dir);
                                WxLogf(L"init: re-add %s refused 0x%08lX %s - the folder keeps its face (%s)",
                                       id.c_str(), (unsigned long)r, WxHrName(r),
                                       g_wx[k].enabled ? L"on" : L"off");
                            }
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

// the late load's last net. WxInit runs while the engine may still be loading the
// profile's own persisted extensions - its enumeration can answer empty or
// half-full, every matching road then runs against a list that is not the whole
// truth, and a folder left riding no registration hides its face even though the
// engine finishes loading it seconds later. The INVALID_STATE adoption catches
// the race only when the add itself collides; nothing revisits a folder that
// simply came up empty. One verification pass, once per session, a few seconds
// after init: enumerate again, claim each orphan's registration by any id the
// folder can name, and walk the fresh-add road once for what is still alone -
// whose INVALID_STATE adoption claims what arrived meanwhile. The folder's own
// switch keeps ruling the face; the engine's refusals land in webext.log.
static void WxVerify() {
    if (g_wxVerifyRan || !g_wxReady || g_wxBusy) return;
    g_wxVerifyRan = true;
    wrl::ComPtr<ICoreWebView2Profile7> p = WxProfile();
    if (!p) return;
    if (g_wx.empty()) {                    // a listing that refused before any folder was read
        std::wstring root = WxRoot();      // left the faces at home entirely - the folders are
        WIN32_FIND_DATAW fd;               // the truth, so the list is rebuilt from them first
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                WebExt e; e.id = fd.cFileName; e.storeId = e.id; e.dir = root + L"\\" + fd.cFileName;
                e.engineId = WxLoadEngineId(e.dir);
                e.phId[0] = WxPathHashId(e.dir, true);
                e.phId[1] = WxPathHashId(e.dir, false);
                if (e.phId[1] == e.phId[0]) e.phId[1].clear();
                if (WxLoadInfo(e)) { e.enabled = WxLoadOn(e.dir); g_wx.push_back(std::move(e)); }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!g_wx.empty())
            WxLogf(L"verify: the list came up empty - %zu folder(s) rebuilt from disk",
                   g_wx.size());
    }
    int orphans = 0;
    for (auto& e : g_wx) if (e.enabled && !e.ext && !e.dir.empty()) orphans++;
    if (!orphans) { WxLogf(L"verify: every folder rides a live registration"); return; }
    HRESULT hr = p->GetBrowserExtensions(
        Microsoft::WRL::Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [orphans](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                std::vector<wrl::ComPtr<ICoreWebView2BrowserExtension>> have;
                std::vector<std::wstring> rids;
                UINT n = 0;
                if (SUCCEEDED(hr) && list && SUCCEEDED(list->get_Count(&n)))
                    for (UINT i = 0; i < n; i++) {
                        wrl::ComPtr<ICoreWebView2BrowserExtension> e;
                        if (SUCCEEDED(list->GetValueAtIndex(i, &e)) && e) {
                            LPWSTR s = nullptr; std::wstring rid;
                            if (SUCCEEDED(e->get_Id(&s)) && s) { rid = s; CoTaskMemFree(s); }
                            have.push_back(e); rids.push_back(rid);
                        }
                    }
                WxLogf(L"verify: %u registration(s) listed now, %d folder(s) rode none at init",
                       (unsigned)have.size(), orphans);
                std::vector<int> redo;
                for (int i = 0; i < (int)g_wx.size(); i++) {
                    if (g_wx[i].ext || !g_wx[i].enabled || g_wx[i].dir.empty()) continue;
                    int pick = -1; const wchar_t* how = L"";
                    for (size_t j = 0; j < have.size(); j++) {
                        if (rids[j].empty()) continue;
                        bool taken = false;                    // a registration another folder
                        for (auto& o : g_wx)                   // already rides is not free
                            if (o.ext && o.id == rids[j]) { taken = true; break; }
                        if (taken) continue;
                        const WebExt& e = g_wx[i];
                        if (rids[j] == e.id || rids[j] == e.storeId) { pick = (int)j; how = L"store id"; break; }
                        if (!e.engineId.empty() && rids[j] == e.engineId) { pick = (int)j; how = L"engine marker"; break; }
                        if (!e.phId[0].empty() && rids[j] == e.phId[0]) { pick = (int)j; how = L"path hash"; break; }
                        if (!e.phId[1].empty() && rids[j] == e.phId[1]) { pick = (int)j; how = L"path hash (legacy)"; break; }
                    }
                    if (pick < 0) { redo.push_back(i); continue; }
                    g_wx[i].ext = have[pick];
                    g_wx[i].id = rids[pick];
                    WxSaveEngineId(g_wx[i].dir, rids[pick]);   // learned once, matched forever
                    WxLogf(L"verify: %s rides registration %s (matched by %s)",
                           g_wx[i].storeId.c_str(), rids[pick].c_str(), how);
                }
                for (int i : redo) {
                    std::wstring id = g_wx[i].id, dir = g_wx[i].dir;
                    WxAddFolderFresh(dir, id, [id, dir](HRESULT r, ICoreWebView2BrowserExtension* ext) {
                        WxLogf(L"verify: re-add %s -> 0x%08lX %s", id.c_str(), (unsigned long)r, WxHrName(r));
                        int k = WxFind(id);
                        if (k >= 0 && SUCCEEDED(r) && ext) {
                            g_wx[k].ext = ext;
                            LPWSTR rid = nullptr;
                            if (SUCCEEDED(ext->get_Id(&rid)) && rid) {
                                g_wx[k].id = rid;
                                WxSaveEngineId(dir, rid);
                                CoTaskMemFree(rid);
                            }
                        } else if (k >= 0 && r == HRESULT_FROM_WIN32(ERROR_INVALID_STATE))
                            WxAdoptLive(dir, id);          // "already here" - the load landed meanwhile
                        if (g_webOn) WebBarLayout();
                        WebRepaintChrome();
                    });
                }
                if (g_webOn) WebBarLayout();
                WebRepaintChrome();
                WxNotifyTabs();
                return S_OK;
            }).Get());
    if (FAILED(hr))
        WxLogf(L"verify: the listing refused synchronously 0x%08lX %s", (unsigned long)hr, WxHrName(hr));
}
static void CALLBACK WxVerifyTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    if (id != g_wxVerifyTimer) return;
    g_wxVerifyTimer = 0;
    WxVerify();
}
// the listing's leash. WxInit hands the engine one GetBrowserExtensions call and
// everything below it - matching, re-adds, the verify pass, WxFinishInit's own
// pending-navigation flush - hangs on that completion coming back. A wedged
// engine answers nothing: g_wxBusy sticks true, no later road ever runs, and the
// session's extensions keep working (the engine loads them from its own
// persistence) while their faces never exist. Nine seconds in, the watchdog
// finishes init anyway - from the folders on disk, which the verify pass then
// rides to their registrations.
static void CALLBACK WxInitWatchTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    if (id != g_wxInitWatch) return;
    g_wxInitWatch = 0;
    if (!g_wxBusy) return;                 // the listing answered in time - the normal road won
    WxLogf(L"init: the listing never came back - finishing from the folders on disk");
    WxFinishInit();                        // busy clears, ready sets, verify is scheduled
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
    {   PWSTR la = nullptr;                // the GPU shader caches go too: a poisoned GPUCache or
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la);   // DawnCache is the classic
        if (la) {                          // every-frame-fails-on-this-machine cause, and caches
            std::wstring base = std::wstring(la) + L"\\Docsy\\WebView2";   // regenerate on the spot
            CoTaskMemFree(la);
            for (const wchar_t* d : { L"\\GPUCache", L"\\DawnCache", L"\\DawnGraphiteCache", L"\\DawnWebGPUCache",
                                      L"\\GrShaderCache", L"\\ShaderCache", L"\\Default\\GPUCache",
                                      L"\\Default\\DawnCache", L"\\Default\\GrShaderCache",
                                      L"\\Default\\DawnGraphiteCache", L"\\Default\\DawnWebGPUCache" })
                WxRmTree(base + d);
            WxLogf(L"env: the gpu shader caches are wiped for the fresh engine");
        }
    }
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
        t.building = false;
        if (t.pending.empty() && !t.url.empty()) t.pending = t.url;   // every live tab re-opens
        t.capturing = false; t.capturePending = false; t.captureDue = 0;
        t.loading = false; t.navStart = 0; t.navKicked = t.navReloaded = false;
    }
    g_wvEnv.Reset();                       // the last host let go: the browser process exits
    g_wvEnvBuilding = false;
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
        if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) {
            // the restart is spent and the folder is still "already here": a live
            // registration the id roads cannot see. The folder is the user's copy -
            // it is never destroyed for being already loaded. The adoption claims
            // the live registration and pins its id beside the folder; if nothing
            // lives, the folder stays and the next restart's WxInit adds it.
            WxLogf(L"install: INVALID_STATE with the restart spent - adopting the live registration");
            WxAdoptLive(dir, id);
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
    WxSaveEngineId(dir, e.id);         // the restart finds this registration through the folder's marker
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
    WxSaveOn(g_wx[i].dir, on);                     // the switch lives beside the folder now: the
    g_wx[i].enabled = on;                          // restart reads it back instead of the engine's
    if (g_webOn) WebBarLayout();                   // memory of what it refused
    WebRepaintChrome();
    g_wx[i].ext->Enable(on ? TRUE : FALSE, Microsoft::WRL::Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
        [id, on](HRESULT hr) -> HRESULT {
            int k = WxFind(id);
            if (k >= 0 && FAILED(hr)) {            // the engine refused: the switch springs back
                WxSaveOn(g_wx[k].dir, !on);
                g_wx[k].enabled = !on;
                if (g_webOn) WebBarLayout();
                WebRepaintChrome();
            }
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

static void WxMenu(POINT cur) {            // the anchor, in strip coords: the right-click's
    if (!g_wstrip) return;                 // cursor, or the keyboard road's own spot
    POINT pt = cur;
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

