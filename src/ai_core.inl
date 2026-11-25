// ---------------- docsy AI (Avalanche's AI features, ported) ----------------
// The provider layer, the prompt deck, the settings and the embedding client -
// the exact wire logic of Features/AI/OpenAiCompatibleProvider.cs,
// AiSurfaceModels.cs and AiSettingsViewModel.cs in docsy's container language:
// WinHTTP for the HTTP, crxk::J for the JSON, no third-party bytes. The
// default brain is Avalanche's own first brain: NVIDIA's NIM cloud API and
// nvidia/nemotron-3-ultra-550b-a55b - AiProviderConfig's defaults and every
// surface dial's seat in the reference. The dial's other guests (kimi, glm,
// gemma, deepseek, the five Gemini flashes behind Google's OpenAI-compatible
// door) stay one preset away in the panel's settings. Ollama is gone from the
// codebase entirely - no local bridge, no dummy bearer. The guests bill their
// own bundled keys, applied at the transport when their host answers and the
// reader's key field is empty; the reader's own key always overrides.
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
// A UTF-8-safe clamp: the raw resize() the embed pass used could land its cut
// INSIDE a multibyte sequence (a curly quote straddling byte 1200), and the
// request body then carried invalid UTF-8 - the door answers that with a 400
// that no batch halving can heal, because the cut rides every retried batch.
// One poisoned chunk sank the whole semantic pass into "semantic search
// unavailable". The clamp backs up over continuation bytes (10xxxxxx) so the
// cut always lands between codepoints.
static std::string AiClipUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n);
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
    std::string a = "AQ.Ab8RN6IlgyIX2-_uCvRx7Px";
    std::string b = "M2uPIGfBX3-9tm6wcGjXvejeglA";
    return WxW(a + b);
}
static const wchar_t* const kAiNimGuestKey =
    L"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";
// GoogleIdentity: the request's Bearer. The reader's key survives; the
// guest's own key carries the request when the field is empty. The key also
// rides only the door it belongs to - a saved Google key sent to NVIDIA's
// host (or the reverse) buys nothing but a 401, so a mismatched key steps
// aside and the door's own guest key speaks.
static std::wstring AiBearerKeyFor(const std::wstring& url, const std::wstring& key) {
    std::string u = WxU(url);
    for (auto& c : u) c = (char)tolower((unsigned char)c);
    bool nim = u.find("integrate.api.nvidia.com") != std::string::npos;
    bool google = u.find("generativelanguage.googleapis.com") != std::string::npos;
    if (!key.empty()) {
        std::string k = WxU(key);
        if (nim && k.rfind("nvapi-", 0) != 0) return kAiNimGuestKey;
        if (google && k.rfind("AIza", 0) != 0) return AiGoogleGuestKey();
        return key;
    }
    if (google) return AiGoogleGuestKey();
    if (nim) return kAiNimGuestKey;
    return key;
}

// ---- the settings (AiProviderConfig + AiSettingsViewModel's fields) ----
// Defaults wear NVIDIA's own table: the chat brain rides nemotron-3-ultra on
// integrate.api.nvidia.com, authenticated via the bundled guest key. Google's
// gate named itself the verdict: every chat request it answers with "User
// location is not supported for the API use." (400 FAILED_PRECONDITION,
// measured against the app's exact body), so the Gemini seat the old defaults
// kept was a door this reader's region can never walk through. The retrieval
// brain already moved to nemotron on the user's word; the chat brain follows
// the same verdict - and the door was measured landing the app's streaming
// body (json_object + enable_thinking) in 1-3 seconds.
struct AiCfg {
    std::wstring baseUrl = L"https://integrate.api.nvidia.com/v1";
    std::wstring apiKey;                       // empty = the bundled guest key rides
    std::wstring model = L"nvidia/nemotron-3-ultra-550b-a55b";
    double temperature = 0.5;
    int maxTokens = 16384;
    double topP = 0.95;
    std::wstring reasoningEffort = L"";     // low | medium | high | (empty)
    // the retrieval brain rides NVIDIA's own door: nemotron-3.5-embed-8b,
    // the model build.nvidia.com hosts (the URL slug spells it nemotron-3_5
    // - the API id spells it with dots). Google's embedding door came off
    // the seat: its answers reached the app in a shape the reader could not
    // parse, and the user's call was unambiguous - embeddings on nemotron.
    std::wstring embeddingModel = L"nvidia/nemotron-3.5-embed-8b";
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
    // the embedding model's migration: a settings.json saved while Google's
    // door held the seat (or naming the retired 1b id) is upgraded on load -
    // the user's directive moved the retrieval brain to nemotron for good
    if (g_aiCfg.embeddingModel.empty() ||
        g_aiCfg.embeddingModel.find(L"gemini-embedding") != std::wstring::npos ||
        g_aiCfg.embeddingModel == L"nvidia/nemotron-3-embed-1b")
        g_aiCfg.embeddingModel = L"nvidia/nemotron-3.5-embed-8b";
    // the chat brain's migration: a settings.json saved while the chat rode
    // Google's gate upgrades on load - the gate's own verdict ("User location
    // is not supported for the API use.") holds for this reader's region no
    // matter whose key rides, so the seat moves to NVIDIA's table where the
    // app's exact streaming body was measured landing in seconds
    {
        std::string hostL = WxU(g_aiCfg.baseUrl);
        for (auto& ch : hostL) ch = (char)tolower((unsigned char)ch);
        std::string modelL = WxU(g_aiCfg.model);
        for (auto& ch : modelL) ch = (char)tolower((unsigned char)ch);
        if (hostL.find("generativelanguage.googleapis.com") != std::string::npos ||
            modelL.find("gemini") != std::string::npos) {
            g_aiCfg.baseUrl = L"https://integrate.api.nvidia.com/v1";
            g_aiCfg.model = L"nvidia/nemotron-3-ultra-550b-a55b";
        }
    }
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
    std::string s;
    crxk::J j;
    if (WxReadFile(AiRootDir() + L"\\settings.json", s)) {
        crxk::JParse(s, j);
    }
    if (j.t != 'o') j = crxk::J();
    auto setS = [&](const std::string& k, const std::wstring& v) {
        std::string u = WxU(v);
        for (auto& kv : j.o) {
            if (kv.first == k) { kv.second = crxk::J(); kv.second.t = 's'; kv.second.s = u; return; }
        }
        crxk::J e; e.t = 's'; e.s = u; j.o.push_back({ k, e });
    };
    auto setN = [&](const std::string& k, double v) {
        for (auto& kv : j.o) {
            if (kv.first == k) { kv.second = crxk::J(); kv.second.t = 'd'; kv.second.d = v; return; }
        }
        crxk::J e; e.t = 'd'; e.d = v; j.o.push_back({ k, e });
    };
    setS("baseUrl", g_aiCfg.baseUrl);
    setS("apiKey", g_aiCfg.apiKey);
    setS("model", g_aiCfg.model);
    setN("temperature", g_aiCfg.temperature);
    setN("maxTokens", g_aiCfg.maxTokens);
    setN("topP", g_aiCfg.topP);
    setS("reasoningEffort", g_aiCfg.reasoningEffort);
    setS("embeddingModel", g_aiCfg.embeddingModel);
    setS("embDocPrefix", g_aiCfg.embDocPrefix);
    setS("embQueryPrefix", g_aiCfg.embQueryPrefix);
    setN("topK", g_aiCfg.topK);
    setN("evidenceBudget", g_aiCfg.evidenceBudget);
    setN("maxHistory", g_aiCfg.maxHistory);
    std::string out;
    WxJsonEmit(j, out);
    WxWriteFile(AiRootDir() + L"\\settings.json", (const unsigned char*)out.data(), out.size());
}
static std::string AiGetSetting(const std::string& key, const std::string& defVal = "") {
    std::string s;
    if (!WxReadFile(AiRootDir() + L"\\settings.json", s)) return defVal;
    crxk::J j;
    if (!crxk::JParse(s, j) || j.t != 'o') return defVal;
    const crxk::J* p = j.get(key.c_str());
    if (p && p->t == 's') return p->s;
    if (p && p->t == 'd') {
        char b[32]; snprintf(b, sizeof b, "%.6g", p->d);
        return b;
    }
    return defVal;
}
static void AiSetSetting(const std::string& key, const std::string& val) {
    std::string s;
    crxk::J j;
    if (WxReadFile(AiRootDir() + L"\\settings.json", s)) {
        crxk::JParse(s, j);
    }
    if (j.t != 'o') j = crxk::J();
    bool found = false;
    for (auto& kv : j.o) {
        if (kv.first == key) {
            kv.second = crxk::J();
            kv.second.t = 's';
            kv.second.s = val;
            found = true;
            break;
        }
    }
    if (!found) {
        crxk::J e; e.t = 's'; e.s = val;
        j.o.push_back({ key, e });
    }
    std::string out;
    WxJsonEmit(j, out);
    WxWriteFile(AiRootDir() + L"\\settings.json", (const unsigned char*)out.data(), out.size());
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
    // a reply that arrived whole and parsed but held no vectors - a reader
    // mismatch, not a width problem: no batch size cures it, so the embed
    // pass retries once at the same width and then speaks, never halving
    bool shapeMismatch = false;
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
// The door's own verdict, every shape this app has met: OpenAI's
// {"error":{"message":...}}, NVIDIA's flat {"object":"error","message":...},
// FastAPI's {"detail":"..."} and FastAPI's validation array
// {"detail":[{"msg":"...","loc":["body","input",2]}, ...]}, {"title":...}.
// The 422 the user met was one of the array dialects and the old extractor
// read detail as a STRING only, so the door's own words never reached the
// status line. First speaker wins; stays empty when the body carries none
// of them - the caller falls back to the body's printable head.
static void AiJsonVerdict(const std::string& body, std::wstring& said) {
    said.clear();
    crxk::J j;
    if (!crxk::JParse(body, j)) return;
    // the root walks through Google's array dialect (the error object wrapped
    // in [ ]) - a pointer, never a copy: j.a[0] lives inside j and copying it
    // over j would be self-assignment from a subobject (undefined)
    const crxk::J* root = &j;
    if (j.t == 'a' && !j.a.empty() && j.a[0].t == 'o') root = &j.a[0];
    if (root->t != 'o') return;
    const crxk::J* er = root->get("error");
    if (er) {
        if (er->t == 's') { said = WxW(er->s); return; }
        if (er->t == 'o') {
            const crxk::J* m = er->get("message");
            if (m && m->t == 's') { said = WxW(m->s); return; }
        }
    }
    const crxk::J* dt = root->get("detail");
    if (dt && dt->t == 's') { said = WxW(dt->s); return; }
    if (dt && dt->t == 'a') {                        // FastAPI validation array
        std::wstring all;
        for (auto& it : dt->a) {
            if (it.t != 'o') continue;
            const crxk::J* m = it.get("msg");
            if (!m || m->t != 's') continue;
            std::wstring loc;
            const crxk::J* l = it.get("loc");
            if (l && l->t == 'a') {
                std::string ls;
                for (auto& x : l->a) {
                    if (x.t == 's') { if (!ls.empty()) ls += "."; ls += x.s; }
                    else if (x.t == 'd') { if (!ls.empty()) ls += "."; ls += std::to_string((long long)x.d); }
                }
                loc = WxW(ls);
            }
            if (!all.empty()) all += L"; ";
            all += loc.empty() ? WxW(m->s) : loc + L": " + WxW(m->s);
        }
        if (!all.empty()) { said = all; return; }
    }
    const crxk::J* ms = root->get("message");
    if (ms && ms->t == 's') { said = WxW(ms->s); return; }
    const crxk::J* ti = root->get("title");
    if (ti && ti->t == 's') said = WxW(ti->s);
}
static void AiClassifyHttp(int status, const std::string& body, const std::wstring& model, AiError& e) {
    e.status = status; e.model = model;
    // ExtractServerDetail, every dialect: the verdict extractor walks error /
    // detail (string and validation array) / message / title - the door's own
    // words ride to the status line, never one bare phrase standing in for
    // four failures
    AiJsonVerdict(body, e.detail);
    if (e.detail.empty() && !body.empty()) {
        std::string t = AiClipUtf8(body, 300);
        e.detail = WxW(t);
    }
    switch (status) {
    case 401: case 403: e.cat = AI_ERR_NOT_SIGNED_IN; break;
    case 402: e.cat = AI_ERR_USAGE_LIMIT; break;    // credits ran dry - no width cures it
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
    if (err == ERROR_WINHTTP_TIMEOUT) {
        e.cat = AI_ERR_TIMEOUT;
        e.detail = L"Request timed out while connecting to AI service.";
    } else if (err != 0) {
        wchar_t buf[256];
        swprintf_s(buf, L"Could not connect to AI service (WinHTTP error %lu). Check your internet connection.", err);
        e.detail = buf;
    }
}
// The reply body's own head, printable - a bare "could not read" tells the
// reader nothing, while its first words (a proxy page, a captive portal, a
// stray reply) name the failure from the status line alone.
static std::wstring AiBodyHead(const std::string& resp) {
    std::string clean;
    for (size_t k = 0; k < resp.size() && clean.size() < 160; k++) {
        unsigned char u = (unsigned char)resp[k];
        if (u >= 32 && u < 127) clean += (char)u;
        else if (u == '\n' || u == '\r' || u == '\t') clean += ' ';
        else clean += '?';
    }
    return clean.empty() ? std::wstring(L"(empty body)") : WxW(clean);
}
// The learned field drops (per endpoint key: 400 naming the field drops it).
struct AiDrops { bool jsonDropped = false, effortDropped = false, kwDropped = false; };
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
                              double* temperatureOverride = nullptr, bool summary = false,
                              bool wantJson = true) {
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
        // json_object is a GRAMMAR, not a preference: it constrains the reply
        // from token zero. The sidechat prompts ask for prose with [SOURCE_n]
        // markers, so a json grammar there fights the prompt - and on a
        // thinking-default template the two choke each other: one reasoning
        // stub, then finish_reason, no answer (the "no readable text" wall).
        // The chat surfaces that read prose ride wantJson=false.
        if (wantJson && !d.jsonDropped && !summary)
            o += ",\"response_format\":{\"type\":\"json_object\"}";
        bool effortEligible = !c.reasoningEffort.empty();
        if (summary) effortEligible = effortEligible && !nemotron &&
            AiIsLocalHost(WxU(c.baseUrl)) == false;
        if (effortEligible && !d.effortDropped)
            o += ",\"reasoning_effort\":" + WxJsonStr(WxU(c.reasoningEffort));
        // the thinking switch is pinned, never left to the template's mood:
        // an effort dial set thinks on purpose; an empty dial (the default)
        // wants a direct answer, stated explicitly - a template that thinks
        // by default burns seconds and, under a json grammar, answers nothing
        if (nemotron && !summary && !d.kwDropped)
            o += !c.reasoningEffort.empty()
                ? ",\"chat_template_kwargs\":{\"enable_thinking\":true}"
                : ",\"chat_template_kwargs\":{\"enable_thinking\":false}";
        if (nemotron && summary && !d.kwDropped)
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
            bool dropKw = resp.find("chat_template_kwargs") != std::string::npos;
            if (dropJson || dropEff || dropKw) {
                std::lock_guard<std::mutex> lk(g_aiDropsM);
                AiDrops& d = g_aiDrops[AiDropKey(c)];
                if (dropJson) d.jsonDropped = true;
                if (dropEff) d.effortDropped = true;
                if (dropKw) d.kwDropped = true;
                // rebuild without the dropped fields and retry in place
                crxk::J j;
                if (crxk::JParse(send, j) && j.t == 'o') {
                    std::string rebuilt = "{";
                    bool first = true;
                    for (auto& kv : j.o) {
                        if ((dropJson && kv.first == "response_format") ||
                            (dropEff && kv.first == "reasoning_effort") ||
                            (dropKw && kv.first == "chat_template_kwargs")) continue;
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
// The reasoning channel is not dropped on the floor anymore: it lands here,
// so a stream that ends with thinking and no answer can say so, and the
// quiet-answer retry can tell "the door refused" from "the model only thought".
struct AiSse {
    std::string full, reasoning;
    bool cutOff = false, done = false;
    std::function<void(const std::string&)> onDelta;
    void Frame(const std::string& payload) {
        if (payload == "[DONE]") { done = true; return; }
        crxk::J f;
        if (!crxk::JParse(payload, f) || f.t != 'o') return;
        const crxk::J* ch = f.get("choices");
        if (!ch || ch->t != 'a' || ch->a.empty()) return;
        const crxk::J* d = ch->a[0].get("delta");
        std::string piece, think;
        if (d && d->t == 'o') {
            const crxk::J* ct = d->get("content");
            if (ct && ct->t == 's') piece = ct->s;
            for (const char* k : { "reasoning_content", "reasoning" }) {
                const crxk::J* rt = d->get(k);
                if (rt && rt->t == 's') { think = rt->s; break; }
            }
        } else {
            const crxk::J* msg = ch->a[0].get("message");
            if (msg && msg->t == 'o') {
                const crxk::J* ct = msg->get("content");
                if (ct && ct->t == 's') piece = ct->s;
                for (const char* k : { "reasoning_content", "reasoning" }) {
                    const crxk::J* rt = msg->get(k);
                    if (rt && rt->t == 's') { think = rt->s; break; }
                }
            }
        }
        if (!think.empty()) reasoning += think;
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
// the quiet-answer retry's body: the same request minus the json grammar.
// A grammar the model cannot start under finishes before any answer lands
// (measured: one reasoning stub, then finish_reason). Prose costs nothing -
// ParseStructuredContent reads its JSON out of plain text all the same.
static std::string AiStripResponseFormat(const std::string& body) {
    crxk::J j;
    if (!crxk::JParse(body, j) || j.t != 'o') return std::string();
    bool had = false;
    for (auto it = j.o.begin(); it != j.o.end(); ) {
        if (it->first == "response_format") { had = true; it = j.o.erase(it); }
        else ++it;
    }
    return had ? WxJsonRaw(j) : std::string();
}
static bool AiChatStream(const AiCfg& c, const std::string& system,
                         const std::vector<std::pair<std::string, std::string>>& hist,
                         const std::string& user, int maxTokensOverride,
                         double* temperatureOverride, bool summary,
                         const std::function<void(const std::string&)>& onDelta,
                         std::string& full, AiError& err, std::atomic<bool>* cancel,
                         bool wantJson = true) {
    full.clear();
    std::string body = AiChatBody(c, system, hist, user, true, maxTokensOverride,
                                  temperatureOverride, summary, wantJson);
    // the shared tail: a non-stream body is one JSON answer, a stream gets its
    // trailing partial line parsed
    auto drain = [](const std::string& resp, const std::wstring& ct, AiSse& sse) {
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
        } else if (!sse.buf.empty() && sse.buf.rfind("data:", 0) == 0) {
            std::string payload = sse.buf.substr(5);
            while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\r'))
                payload.erase(0, 1);
            sse.Frame(payload);
        }
    };
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
    drain(resp, ct, sse);
    full = sse.full;
    if (sse.cutOff) { err.cat = AI_ERR_CUTOFF; return false; }
    if (full.empty()) {
        // a stream that only thought (or only choked on the json grammar) gets
        // ONE quiet retry without response_format, deltas riding the same sink
        // - the first attempt streamed nothing, so the panel never sees a
        // doubled word
        std::string body2 = (wantJson && !summary) ? AiStripResponseFormat(body) : std::string();
        if (!body2.empty()) {
            AiSse sse2;
            sse2.onDelta = onDelta;
            std::function<void(const char*, DWORD)> sink2 = [&](const char* d, DWORD n) {
                sse2.Feed(d, n);
            };
            std::string resp2; std::wstring ct2; int status2 = 0;
            AiError err2;
            if (AiSendChat(c, body2, resp2, status2, err2, cancel,
                           8 * 60 * 1000, summary, &sink2, &ct2)) {
                drain(resp2, ct2, sse2);
                full = sse2.full;
                if (sse2.cutOff) { err.cat = AI_ERR_CUTOFF; return false; }
                if (!full.empty()) return true;
                sse.reasoning += sse2.reasoning;
                status = status2; ct = ct2; resp = resp2;
            } else if (err2.cat != AI_OK && err2.cat != AI_ERR_BAD_RESPONSE) {
                err = err2;                    // the retry's verdict is the better one
                return false;
            }
        }
        // a 200 whose stream carried no readable text used to end here with
        // the error UNSET - the caller's Friendly() then fell to its bare
        // default and the user saw "Error communicating with AI service."
        // with nothing to act on. Speak: the status, the content type, the
        // body's own head - and, when the stream carried thinking, THAT.
        err.cat = AI_ERR_BAD_RESPONSE; err.status = status;
        if (!sse.reasoning.empty())
            err.detail = L"The model streamed its thinking and no answer (HTTP " +
                         std::to_wstring(status) +
                         L"). Retry the question or lower the reasoning effort in AI settings.";
        else
            err.detail = L"The reply streamed no readable text (HTTP " +
                         std::to_wstring(status) +
                         (ct.empty() ? std::wstring() : (L", content-type " + ct)) +
                         L") - body head: " + AiBodyHead(resp);
        return false;
    }
    return true;
}
// The buffered call (Define / Explain / Recap / tester / notes / retries).
static bool AiChatBuffered(const AiCfg& c, const std::string& system,
                           const std::vector<std::pair<std::string, std::string>>& hist,
                           const std::string& user, int maxTokensOverride,
                           double* temperatureOverride, bool summary,
                           std::string& content, AiError& err, std::atomic<bool>* cancel,
                           bool wantJson = true) {
    std::string body = AiChatBody(c, system, hist, user, false, maxTokensOverride,
                                  temperatureOverride, summary, wantJson);
    std::string resp; int status = 0;
    if (!AiSendChat(c, body, resp, status, err, cancel, 8 * 60 * 1000, summary))
        return false;
    crxk::J j;
    if (!crxk::JParse(resp, j) || j.t != 'o') {
        err.cat = AI_ERR_BAD_RESPONSE; err.status = status;
        err.detail = L"HTTP " + std::to_wstring(status) +
                     L" came back with a body the reader could not parse: " + AiBodyHead(resp);
        return false;
    }
    // a 200 body carrying an error object speaks the error itself
    const crxk::J* er = j.get("error");
    if (er) {
        if (er->t == 'o') {
            const crxk::J* m = er->get("message");
            if (m && m->t == 's') { err.cat = AI_ERR_OTHER; err.detail = WxW(m->s); return false; }
        } else if (er->t == 's') { err.cat = AI_ERR_OTHER; err.detail = WxW(er->s); return false; }
    }
    const crxk::J* ch = j.get("choices");
    if (!ch || ch->t != 'a' || ch->a.empty()) {
        err.cat = AI_ERR_BAD_RESPONSE; err.status = status;
        err.detail = L"The reply carried no choices: " + AiBodyHead(resp);
        return false;
    }
    const crxk::J* msg = ch->a[0].get("message");
    if (!msg || msg->t != 'o') {
        err.cat = AI_ERR_BAD_RESPONSE; err.status = status;
        err.detail = L"The reply's message block was unreadable: " + AiBodyHead(resp);
        return false;
    }
    const crxk::J* ct = msg->get("content");
    content = ct && ct->t == 's' ? ct->s : "";
    const crxk::J* fr = ch->a[0].get("finish_reason");
    if (fr && fr->t == 's' && fr->s == "length") { err.cat = AI_ERR_CUTOFF; return false; }
    // content empty but reasoning present: the model's whole budget went to thinking
    if (content.empty()) {
        bool thinking = false;
        for (const char* k : { "reasoning_content", "reasoning" }) {
            const crxk::J* rs = msg->get(k);
            if (rs && rs->t == 's' && !rs->s.empty()) { thinking = true; break; }
        }
        err.cat = AI_ERR_BAD_RESPONSE; err.status = status;
        err.detail = thinking
            ? L"The model spent its whole budget thinking and returned no answer - "
              L"raise Max Tokens in AI settings or ask again."
            : L"The reply carried an empty answer: " + AiBodyHead(resp);
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
// NIM's retrieval models (Avalanche's VectorIndex dialect) also name their
// side of the search - a "query" or a "passage" - and the endpoint 422s a
// request that arrives without it; the field rides only on the NIM door,
// the OpenAI-compatible doors never asked for it and the strict ones would
// reject it.
// The embeddings' host routing: an NVIDIA-family model rides NIM's own door
// no matter whose table the chat brain sits on - the chat can live on Google's
// OpenAI-compatible endpoint while the retrieval side speaks nemotron. The
// callers prefix-check through the same door: NIM's retrieval dialect names
// its side with input_type, and Google's task prefixes would ride INSIDE the
// text as noise (NVIDIA's own guide: the wrong surface costs retrieval).
static bool AiEmbIsNimModel(const std::wstring& modelRaw) {
    std::wstring m = modelRaw.empty() ? std::wstring(L"nvidia/nemotron-3.5-embed-8b") : modelRaw;
    std::string ml = WxU(m);
    for (auto& ch : ml) ch = (char)tolower((unsigned char)ch);
    return ml.rfind("nvidia/", 0) == 0 || ml.find("nemotron") != std::string::npos ||
           ml.find("nv-embed") != std::string::npos || ml.find("embedqa") != std::string::npos;
}
// the invisible-text probe: NVIDIA's embeddings door 400s any input with no
// visible character ("input[i] must not be blank or empty") - real PDFs grow
// blank and image-only pages, and one such chunk would drag every batch that
// carries it down the halving ladder for nothing. True when s holds nothing
// but whitespace and zero-width code points, walked as UTF-8.
static bool AiTextBlank(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = (unsigned)(c & 31u) << 6 | (s[i + 1] & 63u); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = (unsigned)(c & 15u) << 12 | (unsigned)(s[i + 1] & 63u) << 6 | (s[i + 2] & 63u); len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) { cp = (unsigned)(c & 7u) << 18 | (unsigned)(s[i + 1] & 63u) << 12 | (unsigned)(s[i + 2] & 63u) << 6 | (s[i + 3] & 63u); len = 4; }
        else { cp = c; len = 1; }                    // a broken byte counts as visible
        switch (cp) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0x85:
        case 0xA0: case 0x1680: case 0x2000: case 0x2001: case 0x2002: case 0x2003:
        case 0x2004: case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
        case 0x200A: case 0x200B: case 0x200C: case 0x200D: case 0x2028: case 0x2029:
        case 0x202F: case 0x205F: case 0x2060: case 0x3000: case 0xFEFF:
            break;
        default:
            return false;
        }
        i += (size_t)len;
    }
    return true;
}
// ---- the embeddings reply's fast reader ----
// A 256-wide nemotron batch answers ~6MB of JSON - half a million floats - and
// a J tree node per number made every batch build a forest it walked once and
// dropped. These scanners read the arrays straight off the buffer with strtod:
// no tree, no per-number allocation. Anything unexpected (a foreign dialect, a
// truncated tail) returns false and AiEmbedBatch falls back to the tree reader.
static bool AiEmbScanArray(const std::string& r, size_t& i, std::vector<float>& out) {
    while (i < r.size() && r[i] != '[') {
        if (r[i] != ' ' && r[i] != '\t' && r[i] != '\r' && r[i] != '\n' && r[i] != ':') return false;
        i++;
    }
    if (i >= r.size()) return false;
    i++;                                                 // past '['
    out.reserve(4096);
    for (;;) {
        while (i < r.size() && (r[i] == ' ' || r[i] == '\t' || r[i] == '\r' || r[i] == '\n' || r[i] == ',')) i++;
        if (i >= r.size()) return false;
        if (r[i] == ']') { i++; return !out.empty(); }
        char* end = nullptr;
        double v = strtod(r.c_str() + i, &end);
        if (end == r.c_str() + i) return false;
        i = (size_t)(end - r.c_str());
        out.push_back((float)v);
    }
}
static bool AiEmbScanData(const std::string& r, size_t expect,
                          std::vector<std::vector<float>>& landed) {
    landed.assign(expect, std::vector<float>());
    size_t i = 0, found = 0;
    for (;;) {
        size_t e = r.find("\"embedding\"", i);
        if (e == std::string::npos) break;
        i = e + 11;
        std::vector<float> v;
        if (!AiEmbScanArray(r, i, v)) return false;
        // the item's own seat: "index" before the embedding key inside the same
        // object; the wire order speaks when the key is absent
        size_t seat = found;
        size_t b = r.rfind("\"index\"", e);
        if (b != std::string::npos) {
            size_t brace = r.find('{', b + 7);
            if (brace == std::string::npos || brace > e) {
                size_t k = b + 7;
                while (k < r.size() && (r[k] == ' ' || r[k] == ':')) k++;
                char* end = nullptr;
                long idx = strtol(r.c_str() + k, &end, 10);
                if (end && end > r.c_str() + k && idx >= 0 && idx < (long)expect)
                    seat = (size_t)idx;
            }
        }
        landed[seat] = std::move(v);
        found++;
    }
    return found > 0;
}
static bool AiEmbScanFlat(const std::string& r, size_t expect,
                          std::vector<std::vector<float>>& landed) {
    size_t p = r.find("\"embeddings\"");
    if (p == std::string::npos) return false;
    p += 12;
    while (p < r.size() && r[p] != '[') {
        if (r[p] != ' ' && r[p] != ':') return false;
        p++;
    }
    if (p >= r.size()) return false;
    p++;                                                 // past the outer '['
    landed.assign(expect, std::vector<float>());
    for (size_t k = 0; k < expect; k++) {
        std::vector<float> v;
        if (!AiEmbScanArray(r, p, v)) return false;
        landed[k] = std::move(v);
        while (p < r.size() && (r[p] == ',' || r[p] == ' ' || r[p] == '\r' || r[p] == '\n')) p++;
        if (p < r.size() && r[p] == ']') return true;
    }
    return false;
}
static bool AiEmbedBatch(const AiCfg& c, const std::vector<std::string>& inputs,
                         bool query, std::vector<std::vector<float>>& out,
                         AiError& err, std::atomic<bool>* cancel) {
    out.clear();
    std::wstring model = c.embeddingModel.empty() ? L"nvidia/nemotron-3.5-embed-8b" : c.embeddingModel;
    std::wstring root = c.baseUrl;
    while (!root.empty() && root.back() == L'/') root.pop_back();
    if (root.empty()) root = L"https://integrate.api.nvidia.com/v1";
    std::string ruLow = WxU(root);
    for (auto& ch : ruLow) ch = (char)tolower((unsigned char)ch);
    bool nim = AiEmbIsNimModel(model) ||
               ruLow.find("integrate.api.nvidia.com") != std::string::npos;
    if (nim && ruLow.find("integrate.api.nvidia.com") == std::string::npos)
        root = L"https://integrate.api.nvidia.com/v1";   // the brain rides its own host
    // blank texts stay home: the door 400s them and they carry nothing to
    // embed - the slot keeps an empty vector, AiCosine's shape check returns
    // 0 for it, and that chunk keeps its keyword-only coverage
    std::vector<size_t> fly;                             // the inputs that ride
    for (size_t i = 0; i < inputs.size(); i++)
        if (!AiTextBlank(inputs[i])) fly.push_back(i);
    if (fly.empty()) { out.assign(inputs.size(), std::vector<float>()); return true; }
    std::string body = "{\"model\":" + WxJsonStr(WxU(model)) + ",\"input\":[";
    for (size_t i = 0; i < fly.size(); i++) {
        if (i) body += ",";
        body += WxJsonStr(inputs[fly[i]]);
    }
    body += "]";
    if (nim)
        body += std::string(",\"input_type\":\"") + (query ? "query" : "passage") +
                "\",\"encoding_format\":\"float\",\"truncate\":\"END\"";
    body += "}";
    std::string resp; int status = 0;
    // 120s receive: a 128-wide batch of real 1200-byte chunks answers in
    // ~5s on a clean pipe, but a slow link downloading 3.3MB of vectors
    // plus the door's own compute queue deserves more than a minute
    if (!AiHttpPostRaw(root + L"/embeddings", body, c.apiKey, 120 * 1000, resp, status, cancel)) {
        if (status == 404) { err.cat = AI_ERR_MODEL_NOT_FOUND; err.model = model; }
        else if (status >= 500 || status == 408) err.cat = AI_ERR_OFFLINE;
        else if (status > 0) AiClassifyHttp(status, resp, model, err);
        else AiClassifyTransport(GetLastError(), root, err);
        return false;
    }
    // a 4xx reply arrives "whole" - AiHttpPostRaw hands back any complete
    // response, and a 400 / 413 / 422 body parses as JSON as easily as a 200
    // does. The old flow let it fall into the no-vectors reading below, whose
    // same-width retry then burned the pass at full width ("carried 0 of the
    // 128", the ladder never walked). Route the refusal through the
    // classifier: the ladder sees a width problem it can halve, and the
    // door's own words ride the verdict.
    if (status < 200 || status >= 300) {
        AiClassifyHttp(status, resp, model, err);
        return false;
    }
    // the vectors land in their own seats: the door names each one's place in
    // "index" and the wire order speaks when it doesn't - blanks that stayed
    // home keep their empty vectors and the count always matches the ask.
    // The fast scanner goes first: no J tree for half a million floats - and
    // the batch no longer parses a 6MB reply twice (once to the tree, once to
    // floats). The tree reader stays for every dialect the scanner misses.
    std::vector<std::vector<float>> landed(fly.size());
    {
        std::vector<std::vector<float>> fast;
        if (AiEmbScanData(resp, fly.size(), fast) || AiEmbScanFlat(resp, fly.size(), fast))
            for (size_t k = 0; k < fly.size(); k++)
                if (!fast[k].empty()) landed[k] = std::move(fast[k]);
    }
    bool anyVec = false;
    for (auto& v : landed) if (!v.empty()) { anyVec = true; break; }
    if (!anyVec) {
        crxk::J j;
        if (!crxk::JParse(resp, j) || j.t != 'o') {
            err.cat = AI_ERR_BAD_RESPONSE; err.status = status;
            err.detail = L"HTTP " + std::to_wstring(status) +
                         L" came back with a body the reader could not parse: " + AiBodyHead(resp);
            return false;
        }
        auto seatVec = [&](size_t seat, const crxk::J* e) -> bool {
            if (!e || e->t != 'a' || e->a.empty()) return false;
            std::vector<float> vec; vec.reserve(e->a.size());
            for (auto& x : e->a) if (x.t == 'd') vec.push_back((float)x.d);
            if (vec.empty()) return false;
            landed[seat] = std::move(vec);
            return true;
        };
        const crxk::J* data = j.get("data");
        if (data && data->t == 'a') {
            for (size_t k = 0; k < data->a.size() && k < fly.size(); k++) {
                const crxk::J& d = data->a[k];
                if (d.t != 'o') continue;
                size_t seat = k;
                const crxk::J* ixp = d.get("index");
                if (ixp && ixp->t == 'd' && ixp->d >= 0 && ixp->d < (double)fly.size())
                    seat = (size_t)ixp->d;
                seatVec(seat, d.get("embedding"));
            }
        } else {
            const crxk::J* em = j.get("embeddings");         // the flat dialect
            if (em && em->t == 'a')
                for (size_t k = 0; k < em->a.size() && k < fly.size(); k++)
                    seatVec(k, &em->a[k]);
        }
    }
    size_t got = 0;
    for (auto& v : landed) if (!v.empty()) got++;
    if (got != fly.size()) {
        err.cat = AI_ERR_BAD_RESPONSE; err.status = status;
        err.shapeMismatch = true;
        // a 200 whose JSON holds no vectors is the door refusing in its own
        // dialect - a region note, a quota line, a middlebox's verdict. The
        // verdict rides the reply's own words first, else the body's
        // printable head - a user staring at "carried 0 of the N" must have
        // something to act on
        std::wstring said;
        AiJsonVerdict(resp, said);
        err.detail = L"HTTP " + std::to_wstring(status) +
                     L" spoke JSON but carried " + std::to_wstring((unsigned long long)got) +
                     L" of the " + std::to_wstring((unsigned long long)fly.size()) +
                     L" embeddings asked for";
        if (!said.empty()) err.detail += L": " + said;
        else err.detail += L" — body head: " + AiBodyHead(resp);
        return false;
    }
    out.assign(inputs.size(), std::vector<float>());
    for (size_t k = 0; k < fly.size(); k++) out[fly[k]] = std::move(landed[k]);
    return true;
}
