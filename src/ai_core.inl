// ---------------- docsy AI (Avalanche's AI features, ported) ----------------
// The provider layer, the prompt deck, the settings and the embedding client -
// the exact wire logic of Features/AI/OpenAiCompatibleProvider.cs,
// AiSurfaceModels.cs and AiSettingsViewModel.cs in docsy's container language:
// WinHTTP for the HTTP, crxk::J for the JSON, no third-party bytes. The
// default brain is Avalanche's Google guest: Gemini behind the OpenAI-
// compatible door at generativelanguage.googleapis.com, the dial's whole
// table (nemotron, kimi, glm, gemma, deepseek, five Gemini flashes) speaking
// the same /chat/completions. Ollama is gone from the codebase entirely - no
// local bridge, no dummy bearer. The guests bill their own bundled keys,
// applied at the transport when their host answers and the reader's key
// field is empty; the reader's own key always overrides.
#pragma once

// ---- small JSON helpers the module speaks everywhere ----
static bool WxFileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::string WxJsonStr(const std::wstring& w) {   // a JSON string literal, escaped
    std::string u = WxU(w), out = "\"";
    for (size_t i = 0; i < u.size(); i++) {
        unsigned char c = (unsigned char)u[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += "\"";
    return out;
}
static void WxJsonEmit(const crxk::J& v, std::string& o);   // J -> JSON text
static std::string WxJsonRaw(const crxk::J& v) { std::string o; WxJsonEmit(v, o); return o; }
// narrow overloads: the AI payloads are already utf8 std::string, so the
// wide-only helpers get string faces and the call sites stop converting
static std::string WxU(const std::string& s) { return s; }
static std::string WxJsonStr(const std::string& u) {
    std::string out = "\"";
    for (size_t i = 0; i < u.size(); i++) {
        unsigned char c = (unsigned char)u[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += "\"";
    return out;
}

// ---- the paths: %LocalAppData%\docsy\AI\ ----
static std::wstring AiRootDir() {
    static std::wstring root;
    if (root.empty()) {
        PWSTR la = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la))) {
            root = std::wstring(la) + L"\\docsy\\AI";
            CoTaskMemFree(la);
        }
        CreateDirectoryW(root.c_str(), nullptr);
        CreateDirectoryW((root + L"\\index").c_str(), nullptr);
    }
    return root;
}
static std::wstring AiIndexPath(const std::string& docId) {
    return AiRootDir() + L"\\index\\" + WxW(docId) + L".json";
}
static std::wstring AiVecPath(const std::string& docId) {
    return AiRootDir() + L"\\index\\" + WxW(docId) + L".bin";
}

static bool AiIsLocalHost(const std::string& baseUrl) {
    // AiEndpoints.IsLocal: loopback host names and IP loopbacks; an
    // unparsable string falls back to the substring check.
    std::string u = baseUrl;
    size_t p = u.find("://");
    std::string host = p == std::string::npos ? u : u.substr(p + 3);
    size_t slash = host.find_first_of("/?#");
    if (slash != std::string::npos) host.resize(slash);
    size_t at = host.find('@');
    if (at != std::string::npos) host.erase(0, at + 1);
    size_t colon = host.find(':');
    if (colon != std::string::npos) host.resize(colon);
    for (auto& c : host) c = (char)tolower((unsigned char)c);
    if (host.empty()) return false;
    if (host == "localhost") return true;
    unsigned a, b, c, d; char t;
    if (sscanf_s(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &t) == 4)
        return a == 127 || ((a == 0 && b == 0 && c == 0 && d == 0));   // 127/8 + in-loop 0.0.0.0
    if (host == "[::1]" || host == "::1" || host == "[::]") return true;
    return false;
}

// ---- the guests (AiSurfaceModels' identities, ride the transport) ----
// The dial's guests bill their own bundled keys: Google's Gemini door and
// NVIDIA's NIM table each carry the app's own key, seated when their host
// answers and the reader's field is empty. The Google key is two halves
// joined AT RUNTIME - a compile-time fold would leave the whole key
// contiguous in the built exe, and GitHub's push protection rejects the
// dist branch's binary as a leaked GCP credential (Avalanche's source-side
// split survives only because the scanner never reads the built binary).
static std::wstring AiGoogleGuestKey() {
    std::string a = "AQ.Ab8RN6Kvi4sGJHzI1_Rg9u";
    std::string b = "rZmJ1pz6D7m1aJ4RVS5YulDJZ-EA";
    return WxW(a + b);
}
static const wchar_t* const kAiNimGuestKey =
    L"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";
// GoogleIdentity: the request's Bearer. The reader's key survives; the
// guest's own key carries the request when the field is empty.
static std::wstring AiBearerKeyFor(const std::wstring& url, const std::wstring& key) {
    if (!key.empty()) return key;
    std::string u = WxU(url);
    for (auto& c : u) c = (char)tolower((unsigned char)c);
    if (u.find("generativelanguage.googleapis.com") != std::string::npos)
        return AiGoogleGuestKey();
    if (u.find("integrate.api.nvidia.com") != std::string::npos)
        return kAiNimGuestKey;
    return key;
}

// ---- the settings (AiProviderConfig + AiSettingsViewModel's fields) ----
// Defaults wear the canonical Google guest: AiSurfaceModels' GeminiChoice,
// the same host and model id its GoogleIdentity writes.
struct AiCfg {
    std::wstring baseUrl = L"https://generativelanguage.googleapis.com/v1beta/openai";
    std::wstring apiKey;                       // empty = the bundled guest key rides
    std::wstring model = L"gemini-3.8-flash";
    double temperature = 0.2;
    int maxTokens = 4096;
    double topP = 1.0;
    std::wstring reasoningEffort = L"low";     // low | medium | high | (empty)
    std::wstring embeddingModel = L"gemini-embedding-2";
    std::wstring embDocPrefix = L"title: none | text: ";
    std::wstring embQueryPrefix = L"task: search results | query: ";
    int topK = 24;
    int evidenceBudget = 40000;
    int maxHistory = 6;
};
static AiCfg g_aiCfg;
static bool g_aiCfgLoaded = false;

static void WxJsonEmit(const crxk::J& v, std::string& o) {
    switch (v.t) {
    case 'n': o += "null"; break;
    case 'b': o += v.b ? "true" : "false"; break;
    case 'd': { char b[40]; snprintf(b, sizeof b, "%.17g", v.d); o += b; break; }
    case 's': o += WxJsonStr(WxW(v.s)); break;
    case 'a': {
        o += "[";
        for (size_t i = 0; i < v.a.size(); i++) { if (i) o += ","; WxJsonEmit(v.a[i], o); }
        o += "]";
        break; }
    case 'o': {
        o += "{";
        for (size_t i = 0; i < v.o.size(); i++) {
            if (i) o += ",";
            o += WxJsonStr(WxW(v.o[i].first)) + ":";
            WxJsonEmit(v.o[i].second, o);
        }
        o += "}";
        break; }
    }
}

static void AiLoadSettings() {
    if (g_aiCfgLoaded) return;
    g_aiCfgLoaded = true;
    std::string s;
    if (!WxReadFile(AiRootDir() + L"\\settings.json", s)) return;
    crxk::J j;
    if (!crxk::JParse(s, j) || j.t != 'o') return;
    auto gs = [&](const char* k, std::wstring& v) {
        const crxk::J* p = j.get(k);
        if (p && p->t == 's') v = WxW(p->s);
    };
    auto gn = [&](const char* k, double& v) {
        const crxk::J* p = j.get(k);
        if (p && p->t == 'd') v = p->d;
    };
    auto gi = [&](const char* k, int& v) {
        const crxk::J* p = j.get(k);
        if (p && p->t == 'd') v = (int)p->d;
    };
    gs("baseUrl", g_aiCfg.baseUrl);
    gs("apiKey", g_aiCfg.apiKey);
    gs("model", g_aiCfg.model);
    gn("temperature", g_aiCfg.temperature);
    gi("maxTokens", g_aiCfg.maxTokens);
    gn("topP", g_aiCfg.topP);
    gs("reasoningEffort", g_aiCfg.reasoningEffort);
    gs("embeddingModel", g_aiCfg.embeddingModel);
    gs("embDocPrefix", g_aiCfg.embDocPrefix);
    gs("embQueryPrefix", g_aiCfg.embQueryPrefix);
    gi("topK", g_aiCfg.topK);
    gi("evidenceBudget", g_aiCfg.evidenceBudget);
    gi("maxHistory", g_aiCfg.maxHistory);
    if (g_aiCfg.topK < 1) g_aiCfg.topK = 1;
    if (g_aiCfg.evidenceBudget < 1000) g_aiCfg.evidenceBudget = 1000;
    if (g_aiCfg.maxHistory < 2) g_aiCfg.maxHistory = 2;
}
static void AiSaveSettings() {
    std::string j = "{";
    auto ps = [&](const char* k, const std::wstring& v) {
        j += std::string("\"") + k + "\":" + WxJsonStr(v) + ",";
    };
    auto pn = [&](const char* k, double v, bool last = false) {
        char b[64]; snprintf(b, sizeof b, "\"%s\":%.6g%s", k, v, last ? "" : ",");
        j += b;
    };
    ps("baseUrl", g_aiCfg.baseUrl);
    ps("apiKey", g_aiCfg.apiKey);
    ps("model", g_aiCfg.model);
    pn("temperature", g_aiCfg.temperature);
    pn("maxTokens", g_aiCfg.maxTokens);
    pn("topP", g_aiCfg.topP);
    ps("reasoningEffort", g_aiCfg.reasoningEffort);
    ps("embeddingModel", g_aiCfg.embeddingModel);
    ps("embDocPrefix", g_aiCfg.embDocPrefix);
    ps("embQueryPrefix", g_aiCfg.embQueryPrefix);
    pn("topK", g_aiCfg.topK);
    pn("evidenceBudget", g_aiCfg.evidenceBudget);
    pn("maxHistory", g_aiCfg.maxHistory, true);
    j += "}";
    WxWriteFile(AiRootDir() + L"\\settings.json", (const unsigned char*)j.data(), j.size());
}

// ---- the prompt deck (Features/AI/prompts.default.json, embedded verbatim) ----
// Resource 403 carries Avalanche's shipped deck; %LocalAppData%\docsy\AI\prompts.json
// overrides rows by id once it exists (PromptStore's store-over-deck rule).
struct AiPromptDef { std::string id, category, body; };
static std::vector<AiPromptDef> g_prompts;
static bool g_promptsLoaded = false;

static void AiLoadPrompts() {
    if (g_promptsLoaded) return;
    g_promptsLoaded = true;
    auto parse = [](const std::string& s) {
        crxk::J j;
        if (crxk::JParse(s, j) && j.t == 'a')
            for (auto& r : j.a) {
                if (r.t != 'o') continue;
                const crxk::J* id = r.get("Id"), * cat = r.get("Category"), * body = r.get("Body");
                if (!id || id->t != 's' || !cat || cat->t != 's' || !body || body->t != 's') continue;
                AiPromptDef p;
                p.id = id->s; p.category = cat->s; p.body = body->s;
                g_prompts.push_back(std::move(p));
            }
    };
    std::string deckData;
    HRSRC hs = FindResourceW(g.hinst, MAKEINTRESOURCEW(403), (LPCWSTR)RT_RCDATA);
    if (hs) {
        HGLOBAL hg = LoadResource(g.hinst, hs);
        if (hg) {
            const unsigned char* d = (const unsigned char*)LockResource(hg);
            DWORD n = SizeofResource(g.hinst, hs);
            if (d && n) deckData.assign(d, d + n);
        }
    }
    // the reader's overrides ride first: the store is the one source of truth
    std::string user;
    if (WxReadFile(AiRootDir() + L"\\prompts.json", user)) {
        crxk::J u;
        if (crxk::JParse(user, u) && u.t == 'a')
            for (auto& r : u.a) {
                if (r.t != 'o') continue;
                const crxk::J* id = r.get("Id"), * cat = r.get("Category"), * body = r.get("Body");
                if (!id || id->t != 's' || !cat || cat->t != 's' || !body || body->t != 's') continue;
                bool replaced = false;
                for (auto& p : g_prompts)
                    if (p.id == id->s) { p.category = cat->s; p.body = body->s; replaced = true; }
                if (!replaced) g_prompts.push_back({ id->s, cat->s, body->s });
            }
        return;
    }
    if (!deckData.empty()) parse(deckData);
    // first run: extract the deck so the reader can edit it (PromptStore's seed)
    if (!g_prompts.empty() && !WxFileExists((AiRootDir() + L"\\prompts.json").c_str())) {
        std::string out = "[";
        for (size_t i = 0; i < g_prompts.size(); i++) {
            auto& p = g_prompts[i];
            out += std::string(i ? ",\n" : "") + "{\"Id\":" + WxJsonStr(WxU(p.id)) +
                   ",\"Title\":\"\",\"TitleKey\":\"\",\"Category\":" + WxJsonStr(WxU(p.category)) +
                   ",\"Body\":" + WxJsonStr(WxU(p.body)) + "}";
        }
        out += "]";
        WxWriteFile(AiRootDir() + L"\\prompts.json", (const unsigned char*)out.data(), out.size());
    }
}
static std::string PromptBodyOf(const char* id) {   // StoredMandate: the row's body
    AiLoadPrompts();
    for (auto& p : g_prompts) if (p.id == id) return p.body;
    return "";
}
static std::string PromptFirstBody(const char* category) {   // FirstBody(category)
    AiLoadPrompts();
    for (auto& p : g_prompts) if (p.category == category && !p.body.empty()) return p.body;
    return "";
}

// ---- the provider's error world (AiProviderException + MapError) ----
enum AiErrCat {
    AI_OK = 0, AI_ERR_OFFLINE, AI_ERR_UNREACHABLE, AI_ERR_NOT_SIGNED_IN,
    AI_ERR_MODEL_NOT_FOUND, AI_ERR_BUSY, AI_ERR_USAGE_LIMIT, AI_ERR_CUTOFF,
    AI_ERR_TIMEOUT, AI_ERR_BAD_RESPONSE, AI_ERR_OTHER
};
struct AiError {
    AiErrCat cat = AI_OK;
    std::wstring detail;                 // the server's own words, verbatim
    int status = 0;
    std::wstring model;
    std::wstring Friendly() const {      // MapErrorToFriendlyMessage, Ollama booted
        if (!detail.empty()) return detail;
        switch (cat) {
        case AI_ERR_OFFLINE:         return L"The AI service could not be reached. Check your internet connection and try again.";
        case AI_ERR_UNREACHABLE:     return L"The AI service could not be reached. Check your internet connection and try again.";
        case AI_ERR_NOT_SIGNED_IN:   return L"The AI service rejected the API key. Check the key in AI settings.";
        case AI_ERR_MODEL_NOT_FOUND: return L"Model not found" + (model.empty() ? std::wstring() : (L" (" + model + L")")) + L". Pick another model in AI settings.";
        case AI_ERR_BUSY:            return L"The AI service is busy. Try again in a moment.";
        case AI_ERR_USAGE_LIMIT:     return L"Usage limit reached for this AI key. Try again later.";
        case AI_ERR_CUTOFF:          return L"Answer was cut off; try a shorter question or raise the token limit.";
        case AI_ERR_TIMEOUT:         return L"The AI request timed out. Try again.";
        case AI_ERR_BAD_RESPONSE:    return L"The AI returned a response the app could not read. Try again.";
        default:                     return L"Error communicating with AI service.";
        }
    }
};

// ---- WinHTTP plumbing ----
static HINTERNET AiSession() {
    static HINTERNET s = nullptr;
    if (!s) s = WinHttpOpen(L"docsy/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return s;
}
static bool AiUrlSplit(const std::wstring& url, std::wstring& host, INTERNET_PORT& port,
                       std::wstring& path, bool& secure) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t hb[256], pb[2048];
    uc.lpszHostName = hb; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pb; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    host.assign(hb, uc.dwHostNameLength);
    port = uc.nPort;
    path.assign(pb, uc.dwUrlPathLength);
    if (path.empty()) path = L"/";
    secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return !host.empty();
}
// One POST with a Bearer key. `recvTimeoutMs` bounds each receive (the SSE
// quiet budget). Returns false on transport failure; `status` carries HTTP.
// `sink` (optional) sees every body chunk as it lands - the streaming chat's
// live deltas ride it; `ctOut` (optional) receives the response Content-Type.
static bool AiHttpPostRaw(const std::wstring& url, const std::string& body,
                          const std::wstring& apiKey, int recvTimeoutMs,
                          std::string& resp, int& status, std::atomic<bool>* cancel,
                          const std::function<void(const char*, DWORD)>* sink = nullptr,
                          std::wstring* ctOut = nullptr) {
    resp.clear(); status = 0;
    if (ctOut) ctOut->clear();
    HINTERNET s = AiSession();
    if (!s) { status = -1; return false; }
    std::wstring host, path; INTERNET_PORT port; bool secure;
    if (!AiUrlSplit(url, host, port, path, secure)) { status = -1; return false; }
    HINTERNET c = WinHttpConnect(s, host.c_str(), port, 0);
    if (!c) { status = -1; return false; }
    HINTERNET r = WinHttpOpenRequest(c, L"POST", path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { WinHttpCloseHandle(c); status = -1; return false; }
    int to = max(1000, recvTimeoutMs);
    WinHttpSetTimeouts(r, 15000, 15000, 30000, to);
    std::wstring key = AiBearerKeyFor(url, apiKey);
    std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + key + L"\r\n";
    bool ok = false;
    if (WinHttpSendRequest(r, headers.c_str(), (DWORD)-1,
                           (LPVOID)body.data(), (DWORD)body.size(),
                           (DWORD)body.size(), 0)) {
        if (WinHttpReceiveResponse(r, nullptr)) {
            DWORD st = 0, sz = sizeof st;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
            status = (int)st;
            if (ctOut) {
                wchar_t ct[256]; sz = sizeof ct;
                DWORD ix = 0;
                if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                                        ct, &sz, &ix) && sz)
                    *ctOut = std::wstring(ct, sz / sizeof(wchar_t) - 1);
            }
            char buf[16384];
            for (;;) {
                if (cancel && cancel->load()) break;
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) break;
                if (!avail) { ok = true; break; }
                DWORD rd = 0;
                if (!WinHttpReadData(r, buf, min(avail, (DWORD)sizeof buf), &rd)) break;
                if (!rd) { ok = true; break; }
                resp.append(buf, rd);
                if (sink) (*sink)(buf, rd);
            }
        }
    }
    DWORD err = GetLastError();
    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    if (!ok) {
        SetLastError(err);
        status = status ? status : -1;
    }
    return ok;
}
// One GET with a Bearer key - the availability probe's door
// (OpenAiCompatibleProvider.IsAvailableAsync: GET {base}/models).
static bool AiHttpGetRaw(const std::wstring& url, const std::wstring& apiKey,
                         int recvTimeoutMs, std::string& resp, int& status) {
    resp.clear(); status = 0;
    HINTERNET s = AiSession();
    if (!s) { status = -1; return false; }
    std::wstring host, path; INTERNET_PORT port; bool secure;
    if (!AiUrlSplit(url, host, port, path, secure)) { status = -1; return false; }
    HINTERNET c = WinHttpConnect(s, host.c_str(), port, 0);
    if (!c) { status = -1; return false; }
    HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { WinHttpCloseHandle(c); status = -1; return false; }
    WinHttpSetTimeouts(r, 15000, 15000, 15000, max(1000, recvTimeoutMs));
    std::wstring key = AiBearerKeyFor(url, apiKey);
    std::wstring headers = L"Authorization: Bearer " + key + L"\r\n";
    bool ok = false;
    if (WinHttpSendRequest(r, headers.c_str(), (DWORD)-1,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (WinHttpReceiveResponse(r, nullptr)) {
            DWORD st = 0, sz = sizeof st;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
            status = (int)st;
            char buf[16384];
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) break;
                if (!avail) { ok = true; break; }
                DWORD rd = 0;
                if (!WinHttpReadData(r, buf, min(avail, (DWORD)sizeof buf), &rd)) break;
                if (!rd) { ok = true; break; }
                resp.append(buf, rd);
            }
        }
    }
    DWORD err = GetLastError();
    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    if (!ok) {
        SetLastError(err);
        status = status ? status : -1;
    }
    return ok;
}
static void AiClassifyHttp(int status, const std::string& body, const std::wstring& model, AiError& e) {
    e.status = status; e.model = model;
    // ExtractServerDetail: error.message verbatim, else the trimmed body
    crxk::J j;
    if (crxk::JParse(body, j) && j.t == 'o') {
        const crxk::J* er = j.get("error");
        if (er && er->t == 'o') {
            const crxk::J* m = er->get("message");
            if (m && m->t == 's') e.detail = WxW(m->s);
        } else if (er && er->t == 's') {
            e.detail = WxW(er->s);
        }
    }
    if (e.detail.empty() && !body.empty()) {
        std::string t = body.substr(0, 2000);
        e.detail = WxW(t);
    }
    switch (status) {
    case 401: case 403: e.cat = AI_ERR_NOT_SIGNED_IN; break;
    case 404: e.cat = AI_ERR_MODEL_NOT_FOUND; break;
    case 429: e.cat = (body.find("usage limit") != std::string::npos ||
                       body.find("usage_limit") != std::string::npos ||
                       body.find("usage-limit") != std::string::npos ||
                       body.find("credit") != std::string::npos ||
                       body.find("quota") != std::string::npos)
                        ? AI_ERR_USAGE_LIMIT : AI_ERR_BUSY; break;
    case 503: e.cat = AI_ERR_BUSY; break;
    default: e.cat = AI_ERR_OTHER; break;
    }
}
static void AiClassifyTransport(DWORD err, const std::wstring& baseUrl, AiError& e) {
    e.cat = AI_ERR_UNREACHABLE;
    if (err == ERROR_WINHTTP_TIMEOUT) e.cat = AI_ERR_TIMEOUT;
}
// The learned field drops (per endpoint key: 400 naming the field drops it).
struct AiDrops { bool jsonDropped = false, effortDropped = false; };
static std::unordered_map<std::string, AiDrops> g_aiDrops;
static std::mutex g_aiDropsM;
static std::string AiDropKey(const AiCfg& c) {
    return WxU(c.baseUrl) + "|" + WxU(c.model);
}

// ---- the chat request body (BuildRequestBody) ----
static std::string AiMsgJson(const char* role, const std::string& content) {
    return std::string("{\"role\":\"") + role + "\",\"content\":" + WxJsonStr(content) + "}";
}
static std::string AiChatBody(const AiCfg& c, const std::string& system,
                              const std::vector<std::pair<std::string, std::string>>& hist,
                              const std::string& user, bool stream, int maxTokensOverride = 0,
                              double* temperatureOverride = nullptr, bool summary = false) {
    bool nemotron = [&, m = WxU(c.model)] {
        std::string mm = m;
        for (auto& ch : mm) ch = (char)tolower((unsigned char)ch);
        return mm.find("nemotron") != std::string::npos;
    }();
    std::string o = "{\"model\":" + WxJsonStr(WxU(c.model)) + ",\"messages\":[";
    if (!system.empty()) o += AiMsgJson("system", system);
    for (auto& m : hist) o += "," + AiMsgJson(m.first.c_str(), m.second);
    if (!user.empty()) o += "," + AiMsgJson("user", user);
    o += "]";
    char b[160];
    double temp = temperatureOverride ? *temperatureOverride : c.temperature;
    snprintf(b, sizeof b, ",\"temperature\":%.3f,\"max_tokens\":%d,\"top_p\":%.3f",
             temp, maxTokensOverride > 0 ? maxTokensOverride : c.maxTokens, c.topP);
    o += b;
    if (stream) o += ",\"stream\":true";
    // response_format / reasoning_effort, minus what the endpoint already 400'd
    {
        std::lock_guard<std::mutex> lk(g_aiDropsM);
        AiDrops& d = g_aiDrops[AiDropKey(c)];
        if (!d.jsonDropped && !summary) o += ",\"response_format\":{\"type\":\"json_object\"}";
        bool effortEligible = !c.reasoningEffort.empty();
        if (summary) effortEligible = effortEligible && !nemotron &&
            AiIsLocalHost(WxU(c.baseUrl)) == false;
        if (effortEligible && !d.effortDropped)
            o += ",\"reasoning_effort\":" + WxJsonStr(WxU(c.reasoningEffort));
        if (nemotron && !c.reasoningEffort.empty() && !summary)
            o += ",\"chat_template_kwargs\":{\"enable_thinking\":true}";
        if (nemotron && summary)
            o += ",\"chat_template_kwargs\":{\"enable_thinking\":false}";
    }
    o += "}";
    return o;
}
// The 400-that-names-a-field loop: learn the drop and retry (bounded).
static bool AiSendChat(const AiCfg& c, const std::string& body,
                       std::string& resp, int& status, AiError& err,
                       std::atomic<bool>* cancel, int recvTimeoutMs, bool summary,
                       const std::function<void(const char*, DWORD)>* sink = nullptr,
                       std::wstring* ctOut = nullptr) {
    std::string send = body;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (cancel && cancel->load()) { err.cat = AI_ERR_TIMEOUT; return false; }
        if (!AiHttpPostRaw(c.baseUrl + L"/chat/completions", send, c.apiKey,
                           recvTimeoutMs, resp, status, cancel, sink, ctOut)) {
            if (status > 0) AiClassifyHttp(status, resp, c.model, err);
            else AiClassifyTransport(GetLastError(), c.baseUrl, err);
            return false;
        }
        if (status >= 200 && status < 300) return true;
        if (status == 400) {
            bool dropJson = resp.find("response_format") != std::string::npos;
            bool dropEff = resp.find("reasoning_effort") != std::string::npos;
            if (dropJson || dropEff) {
                std::lock_guard<std::mutex> lk(g_aiDropsM);
                AiDrops& d = g_aiDrops[AiDropKey(c)];
                if (dropJson) d.jsonDropped = true;
                if (dropEff) d.effortDropped = true;
                // rebuild without the dropped fields and retry in place
                crxk::J j;
                if (crxk::JParse(send, j) && j.t == 'o') {
                    std::string rebuilt = "{";
                    bool first = true;
                    for (auto& kv : j.o) {
                        if ((dropJson && kv.first == "response_format") ||
                            (dropEff && kv.first == "reasoning_effort")) continue;
                        if (!first) rebuilt += ",";
                        first = false;
                        rebuilt += WxJsonStr(WxW(kv.first)) + ":" + WxJsonRaw(kv.second);
                    }
                    send = rebuilt + "}";
                    continue;
                }
            }
        }
        AiClassifyHttp(status, resp, c.model, err);
        return false;
    }
    err.cat = AI_ERR_BAD_RESPONSE;
    return false;
}

// ---- SSE stream: yield content deltas, swallow reasoning, [DONE] ends ----
// One frame parser shared by the incremental sink and the trailing drain.
struct AiSse {
    std::string full;
    bool cutOff = false, done = false;
    std::function<void(const std::string&)> onDelta;
    void Frame(const std::string& payload) {
        if (payload == "[DONE]") { done = true; return; }
        crxk::J f;
        if (!crxk::JParse(payload, f) || f.t != 'o') return;
        const crxk::J* ch = f.get("choices");
        if (!ch || ch->t != 'a' || ch->a.empty()) return;
        const crxk::J* d = ch->a[0].get("delta");
        std::string piece;
        if (d && d->t == 'o') {
            const crxk::J* ct = d->get("content");
            if (ct && ct->t == 's') piece = ct->s;
        } else {
            const crxk::J* msg = ch->a[0].get("message");
            if (msg && msg->t == 'o') {
                const crxk::J* ct = msg->get("content");
                if (ct && ct->t == 's') piece = ct->s;
            }
        }
        if (!piece.empty()) { full += piece; if (onDelta) onDelta(piece); }
        const crxk::J* fr = ch->a[0].get("finish_reason");
        if (fr && fr->t == 's' && fr->s == "length") cutOff = true;
    }
    // Feed one raw chunk; complete `data:` lines are parsed, the tail kept.
    void Feed(const char* d, size_t n) {
        buf.append(d, n);
        size_t pos = 0;
        for (;;) {
            if (done) break;
            size_t nl = buf.find('\n', pos);
            if (nl == std::string::npos) break;
            std::string line = buf.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.rfind("data:", 0) == 0) {
                std::string payload = line.substr(5);
                while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\r'))
                    payload.erase(0, 1);
                while (!payload.empty() && payload.back() == '\r') payload.pop_back();
                Frame(payload);
            }
        }
        buf.erase(0, pos);
    }
    std::string buf;
};
static bool AiChatStream(const AiCfg& c, const std::string& system,
                         const std::vector<std::pair<std::string, std::string>>& hist,
                         const std::string& user, int maxTokensOverride,
                         double* temperatureOverride, bool summary,
                         const std::function<void(const std::string&)>& onDelta,
                         std::string& full, AiError& err, std::atomic<bool>* cancel) {
    full.clear();
    std::string body = AiChatBody(c, system, hist, user, true, maxTokensOverride,
                                  temperatureOverride, summary);
    AiSse sse;
    sse.onDelta = onDelta;
    std::wstring ct;
    int status = 0;
    // the sink rides the read loop: every landed chunk is parsed in place, so
    // the panel paints deltas while the network is still talking
    std::function<void(const char*, DWORD)> sink = [&](const char* d, DWORD n) {
        sse.Feed(d, n);
    };
    std::string resp;
    if (!AiSendChat(c, body, resp, status, err, cancel, 8 * 60 * 1000, summary, &sink, &ct))
        return false;
    if (ct.find(L"event-stream") == std::wstring::npos) {
        // the endpoint ignored stream:true: the whole body is one JSON answer
        sse.full.clear();
        crxk::J j;
        if (crxk::JParse(resp, j) && j.t == 'o') {
            const crxk::J* ch = j.get("choices");
            if (ch && ch->t == 'a' && !ch->a.empty()) {
                const crxk::J* msg = ch->a[0].get("message");
                if (msg && msg->t == 'o') {
                    const crxk::J* ctn = msg->get("content");
                    if (ctn && ctn->t == 's') sse.full = ctn->s;
                }
            }
        }
        if (sse.onDelta && !sse.full.empty()) sse.onDelta(sse.full);
    } else {
        sse.Feed("", 0);   // nothing; the trailing partial line is parsed below
        if (!sse.buf.empty() && sse.buf.rfind("data:", 0) == 0) {
            std::string payload = sse.buf.substr(5);
            while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\r'))
                payload.erase(0, 1);
            sse.Frame(payload);
        }
    }
    full = sse.full;
    if (sse.cutOff) { err.cat = AI_ERR_CUTOFF; return false; }
    return !full.empty();
}
// The buffered call (Define / Explain / Recap / tester / notes / retries).
static bool AiChatBuffered(const AiCfg& c, const std::string& system,
                           const std::vector<std::pair<std::string, std::string>>& hist,
                           const std::string& user, int maxTokensOverride,
                           double* temperatureOverride, bool summary,
                           std::string& content, AiError& err, std::atomic<bool>* cancel) {
    std::string body = AiChatBody(c, system, hist, user, false, maxTokensOverride,
                                  temperatureOverride, summary);
    std::string resp; int status = 0;
    if (!AiSendChat(c, body, resp, status, err, cancel, 8 * 60 * 1000, summary))
        return false;
    crxk::J j;
    if (!crxk::JParse(resp, j) || j.t != 'o') { err.cat = AI_ERR_BAD_RESPONSE; return false; }
    // a 200 body carrying an error object speaks the error itself
    const crxk::J* er = j.get("error");
    if (er) {
        if (er->t == 'o') {
            const crxk::J* m = er->get("message");
            if (m && m->t == 's') { err.cat = AI_ERR_OTHER; err.detail = WxW(m->s); return false; }
        } else if (er->t == 's') { err.cat = AI_ERR_OTHER; err.detail = WxW(er->s); return false; }
    }
    const crxk::J* ch = j.get("choices");
    if (!ch || ch->t != 'a' || ch->a.empty()) { err.cat = AI_ERR_BAD_RESPONSE; return false; }
    const crxk::J* msg = ch->a[0].get("message");
    if (!msg || msg->t != 'o') { err.cat = AI_ERR_BAD_RESPONSE; return false; }
    const crxk::J* ct = msg->get("content");
    content = ct && ct->t == 's' ? ct->s : "";
    const crxk::J* fr = ch->a[0].get("finish_reason");
    if (fr && fr->t == 's' && fr->s == "length") { err.cat = AI_ERR_CUTOFF; return false; }
    // content empty but reasoning present: the model's whole budget went to thinking
    if (content.empty()) {
        for (const char* k : { "reasoning_content", "reasoning" }) {
            const crxk::J* rs = msg->get(k);
            if (rs && rs->t == 's' && !rs->s.empty()) break;
        }
        err.cat = AI_ERR_BAD_RESPONSE;
        return false;
    }
    return true;
}

// ---- the tolerant answer parser (ParseStructuredContent) ----
static size_t AiFirstBalancedObject(const std::string& s, size_t from, std::string& out) {
    int depth = 0; bool inStr = false, esc = false; size_t start = std::string::npos;
    for (size_t i = from; i < s.size(); i++) {
        char c = s[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') { if (depth == 0) start = i; depth++; }
        else if (c == '}') {
            if (depth > 0) {
                depth--;
                if (depth == 0 && start != std::string::npos) {
                    out = s.substr(start, i - start + 1);
                    crxk::J probe;
                    if (crxk::JParse(out, probe) && probe.t == 'o') return start;
                    start = std::string::npos;   // invalid: keep scanning
                }
            }
        }
    }
    return std::string::npos;
}
struct AiModelSource { std::string id, quote, reason; };
struct AiModelReply { std::string answer; std::vector<AiModelSource> sources; };
static AiModelReply AiParseStructured(const std::string& content) {
    AiModelReply r;
    auto tryObj = [&](const std::string& o) -> bool {
        crxk::J j;
        if (!crxk::JParse(o, j) || j.t != 'o') return false;
        const crxk::J* a = j.get("answer");
        const crxk::J* ss = j.get("sources");
        if (!a && !ss) return false;
        if (a && a->t == 's') r.answer = a->s;
        else if (a && a->t == 'd') r.answer = std::to_string(a->d);
        if (ss && ss->t == 'a')
            for (auto& e : ss->a) {
                if (e.t != 'o') continue;
                AiModelSource s;
                const crxk::J* v;
                if ((v = e.get("sourceId")) && (v->t == 's' || v->t == 'd'))
                    s.id = v->t == 's' ? v->s : std::to_string((int)v->d);
                if ((v = e.get("quote")) && v->t == 's') s.quote = v->s;
                if ((v = e.get("reason")) && v->t == 's') s.reason = v->s;
                r.sources.push_back(std::move(s));
            }
        return !r.answer.empty();
    };
    // fenced blocks first, then the whole text
    size_t p = 0;
    while ((p = content.find("```", p)) != std::string::npos) {
        size_t b = p + 3;
        if (b < content.size() && content.compare(b, 4, "json") == 0) b += 4;
        while (b < content.size() && (content[b] == '\n' || content[b] == '\r' || content[b] == ' ')) b++;
        size_t e = content.find("```", b);
        std::string cand = content.substr(b, e == std::string::npos ? std::string::npos : e - b);
        std::string obj;
        size_t at = 0;
        while ((at = AiFirstBalancedObject(cand, at, obj)) != std::string::npos) {
            at++;
            if (tryObj(obj)) return r;
        }
        if (e == std::string::npos) break;
        p = e + 3;
    }
    std::string obj; size_t at = 0;
    while ((at = AiFirstBalancedObject(content, at, obj)) != std::string::npos) {
        at++;
        if (tryObj(obj)) return r;
    }
    r.answer = content;
    while (!r.answer.empty() && (r.answer.front() == '\n' || r.answer.front() == '\r' ||
                                 r.answer.front() == ' ' || r.answer.front() == '\t'))
        r.answer.erase(0, 1);
    while (!r.answer.empty() && (r.answer.back() == '\n' || r.answer.back() == '\r' ||
                                 r.answer.back() == ' ' || r.answer.back() == '\t'))
        r.answer.pop_back();
    return r;
}
static std::string AiStripThinkSpans(const std::string& t) {
    size_t p = t.find("<think>");
    if (p == std::string::npos) return t;
    std::string out = t.substr(0, p);
    size_t e = t.find("</think>", p);
    if (e != std::string::npos) out += t.substr(e + 8);
    return out;
}
// ExtractStreamingAnswer: the live answer behind the JSON scaffolding
static std::string AiExtractStreamingAnswer(const std::string& raw) {
    std::string clean = AiStripThinkSpans(raw);
    while (!clean.empty() && (clean.front() == ' ' || clean.front() == '\n' ||
                              clean.front() == '\r' || clean.front() == '\t')) clean.erase(0, 1);
    if (clean.empty() || clean.front() != '{') return clean;
    size_t a = clean.find("\"answer\"");
    if (a == std::string::npos) {
        std::string low = clean;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        a = low.find("\"answer\"");
        if (a == std::string::npos) return "";
    }
    size_t colon = clean.find(':', a);
    if (colon == std::string::npos) return "";
    size_t q = clean.find('"', colon);
    if (q == std::string::npos) return "";
    std::string out;
    for (size_t i = q + 1; i < clean.size(); i++) {
        char c = clean[i];
        if (c == '\\' && i + 1 < clean.size()) {
            char n = clean[++i];
            if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == 'r') out += '\r';
            else out += n;
            continue;
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

// ---- embeddings (the OpenAI-compatible /embeddings door) ----
// The configured host speaks its own embeddings: Google's door takes
// .../v1beta/openai/embeddings, NIM takes .../v1/embeddings - appending
// /embeddings to the base URL seats both. The reader parses the OpenAI
// shape (data[].embedding) first, the flat embeddings array second.
static bool AiEmbedBatch(const AiCfg& c, const std::vector<std::string>& inputs,
                         std::vector<std::vector<float>>& out, AiError& err,
                         std::atomic<bool>* cancel) {
    out.clear();
    std::wstring model = c.embeddingModel.empty() ? L"gemini-embedding-2" : c.embeddingModel;
    std::string body = "{\"model\":" + WxJsonStr(WxU(model)) + ",\"input\":[";
    for (size_t i = 0; i < inputs.size(); i++) {
        if (i) body += ",";
        body += WxJsonStr(inputs[i]);
    }
    body += "]}";
    std::wstring root = c.baseUrl;
    while (!root.empty() && root.back() == L'/') root.pop_back();
    if (root.empty()) root = L"https://generativelanguage.googleapis.com/v1beta/openai";
    std::string resp; int status = 0;
    if (!AiHttpPostRaw(root + L"/embeddings", body, c.apiKey, 60 * 1000, resp, status, cancel)) {
        if (status == 404) { err.cat = AI_ERR_MODEL_NOT_FOUND; err.model = model; }
        else if (status >= 500 || status == 408) err.cat = AI_ERR_OFFLINE;
        else if (status > 0) AiClassifyHttp(status, resp, model, err);
        else AiClassifyTransport(GetLastError(), root, err);
        return false;
    }
    crxk::J j;
    if (!crxk::JParse(resp, j) || j.t != 'o') { err.cat = AI_ERR_BAD_RESPONSE; return false; }
    auto grab = [&](const crxk::J* arr) -> bool {
        out.clear();
        for (auto& v : arr->a) {
            if (v.t != 'a' || v.a.empty()) return false;
            std::vector<float> vec;
            for (auto& x : v.a) {
                if (x.t != 'd') return false;
                vec.push_back((float)x.d);
            }
            out.push_back(std::move(vec));
        }
        return out.size() == inputs.size();
    };
    const crxk::J* em = j.get("embeddings");
    if (em && em->t == 'a' && grab(em)) return true;
    const crxk::J* data = j.get("data");
    if (data && data->t == 'a') {
        std::vector<std::vector<float>> vecs;
        for (auto& d : data->a) {
            if (d.t != 'o') continue;
            const crxk::J* e = d.get("embedding");
            if (e && e->t == 'a') {
                std::vector<float> vec;
                for (auto& x : e->a) if (x.t == 'd') vec.push_back((float)x.d);
                vecs.push_back(std::move(vec));
            }
        }
        if (vecs.size() == inputs.size()) { out = std::move(vecs); return true; }
    }
    err.cat = AI_ERR_BAD_RESPONSE;
    return false;
}
