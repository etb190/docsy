// ---------------- docsy AI: the panel, the chat VM and the workers ----------------
// The 380px overlay (Avalanche's AiChatOverlay) hosting ai_page.html, the
// per-context chat state machine (AiChatViewModel's context handling, send
// flow, streaming, citation resolution, history persistence) and the index /
// semantic / notes / summary workers. The network rides std::threads; the UI
// learns through a hidden message window's WM_APP pump, so the WebView2 and
// the GDI stay on the one thread they were born on.
#pragma once

// forward decls the earlier modules already call
static void AiChatToggle();
static void AiOverlayLayout();
static void AiPaintPage(HDC dc, const DocState& D, int page, int px, int py);
static void AiModeChanged();          // browser/editor/reader flips rebind the context
static void AiPanelMessage(const wchar_t* json);   // defined in ai_ui2.inl
static void AiHandleEvent(AiEv* e);                // defined in ai_ui3.inl
static void AiIndexKickoff(const std::wstring& path);   // defined in ai_ui2.inl

// ---- the panel ----
static wrl::ComPtr<ICoreWebView2Controller> g_aiCtr;
static wrl::ComPtr<ICoreWebView2> g_aiWeb;
static bool g_aiBuilding = false, g_aiReady = false;
static bool g_aiOn = false;

// ---- the per-context state (AiChatViewModel's world, one map) ----
struct AiCtxState {
    std::string key;                     // doc_ / wt_ / ed_ session key
    int mode = 0;                        // 0 doc, 1 web, 2 editor
    std::wstring title;                  // ContextTitle
    std::wstring pathOrRef;              // book path / page URL / editor title
    std::vector<AiChatMsg> msgs;         // the live transcript (no error/loading rows)
    std::string convId;
    std::string pendingInput;            // queued behind the index build
    bool loadingBubble = false;          // the thinking box is on screen
    std::string streamingRaw;            // the reply's raw accumulation
    std::atomic<bool> cancel{ false };
    bool inflight = false;
    unsigned long long gen = 0;
    // web sidechat
    std::string pageTitle, pageUrl;
};
static std::map<std::string, AiCtxState> g_aiCtx;
static std::string g_aiCurKey;
static std::mutex g_aiMtx;               // guards the ctx map's structural edits

// ---- the document index lives here (one active book) ----
static AiDocIndex g_aiIndex;
static bool g_aiIndexReady = false;
static bool g_aiIndexing = false;
static int g_aiIdxPage = 0;
static std::string g_aiIdxKey;
static std::wstring g_aiIdxPath;
static AiChunker* g_aiIdxChunk = nullptr;
static std::wstring g_aiSemStatus;       // the current doc's semantic line
static bool g_aiSemActive = false, g_aiSemReadyFlag = false;
static std::atomic<bool> g_aiSemCancel{ false };
static std::thread g_aiSemThread;
static std::set<std::string> g_aiSemRequested;

// the reply thread
static std::thread g_aiReplyThread;
static unsigned long long g_aiReplyGen = 0;

// the notes / range extraction pump
static std::thread g_aiNotesThread;
static std::atomic<bool> g_aiNotesCancel{ false };
static bool g_aiNotesBusy = false;
static std::vector<AiNoteCard> g_aiNotesCards;
static std::wstring g_aiNotesDocPath;

// the summarizer window
static HWND g_sumWnd = nullptr;
static wrl::ComPtr<ICoreWebView2Controller> g_sumCtr;
static wrl::ComPtr<ICoreWebView2> g_sumWeb;
static bool g_sumBuilding = false, g_sumReady = false;
static int g_sumMode = 0;                // 0 doc, 1 web
static std::wstring g_sumDocPath;
static int g_sumPageCount = 0, g_sumStart = 1, g_sumRange = 20;
static unsigned long long g_sumGen = 0;
static unsigned long long g_sumWebPending = 0;   // the web summary waiting on its extraction
static std::thread g_sumThread;
static std::atomic<bool> g_sumCancel{ false };
static bool g_sumBusy = false;
static std::wstring g_sumOwnerPanePath;  // the pane whose pages the window follows

// editor text (the aitext round trip)
static std::string g_edAiText;
static bool g_edAiTextPending = false;
static std::string g_edAiPendingInput;

// web page text
static std::string g_webAiText;
static bool g_webAiTextPending = false;
static std::string g_webAiPendingInput;

// ---- small pushers (UI thread -> panel) ----
static void AiPanelPost(const std::wstring& json) {
    if (g_aiWeb) g_aiWeb->PostWebMessageAsJson(json.c_str());
}
static void AiPanelSend(const std::string& utf8json) {
    AiPanelPost(WxW(utf8json));
}
static std::string AiStatusJson(const char* cmd) {
    // the status row block every paint carries
    std::string j = std::string("{\"cmd\":\"") + cmd + "\",\"indexing\":{\"on\":";
    j += g_aiIndexing ? "true" : "false";
    j += ",\"status\":" + WxJsonStr(g_aiIndexing ? std::wstring(L"Preparing document…")
                                       : (g_aiIndexReady ? std::wstring(L"Ready") : std::wstring()));
    j += ",\"progress\":" + std::to_string(g_aiIndexing && g_aiIndex.pageCount > 0
              ? (double)g_aiIdxPage / (double)g_aiIndex.pageCount : (g_aiIndexReady ? 1.0 : 0.0));
    j += "},\"semantic\":" + WxJsonStr(g_aiSemStatus);
    j += ",\"semActive\":" + std::string(g_aiSemActive ? "true" : "false");
    j += ",\"semReady\":" + std::string(g_aiSemReadyFlag ? "true" : "false");
    j += "}";
    return j;
}
static std::string AiSettingsJson() {
    std::string j = "{";
    j += "\"baseUrl\":" + WxJsonStr(g_aiCfg.baseUrl);
    j += ",\"apiKey\":" + WxJsonStr(g_aiCfg.apiKey);
    j += ",\"model\":" + WxJsonStr(g_aiCfg.model);
    j += ",\"temperature\":" + std::to_string(g_aiCfg.temperature);
    j += ",\"maxTokens\":" + std::to_string(g_aiCfg.maxTokens);
    j += ",\"topP\":" + std::to_string(g_aiCfg.topP);
    j += ",\"reasoningEffort\":" + WxJsonStr(g_aiCfg.reasoningEffort);
    j += ",\"embeddingModel\":" + WxJsonStr(g_aiCfg.embeddingModel);
    j += ",\"topK\":" + std::to_string(g_aiCfg.topK);
    j += ",\"evidenceBudget\":" + std::to_string(g_aiCfg.evidenceBudget);
    j += ",\"maxHistory\":" + std::to_string(g_aiCfg.maxHistory);
    j += "}";
    return j;
}

// ---- the prompt blocks (the exact dialects the reference speaks) ----
static std::string AiSidechatSystem(const std::vector<AiRetrieved>& ev) {
    std::string s = PromptFirstBody("sidechat");
    s += "\n\nEVIDENCE FORMAT:\n[SOURCE_1] Page 147 (section: ...)\n<text>\n\nRETRIEVED EVIDENCE:\n\n";
    for (size_t i = 0; i < ev.size(); i++) {
        char b[64];
        int pg = ev[i].chunk->pages.empty() ? 0 : ev[i].chunk->pages.front().page + 1;
        snprintf(b, sizeof b, "[SOURCE_%zu] Page %d", i + 1, pg);
        s += b;
        if (!ev[i].chunk->heading.empty())
            s += " (section: " + ev[i].chunk->heading + ")";
        s += "\n";
        s += ev[i].chunk->text;
        s += "\n\n";
    }
    s += "INSTRUCTIONS:\n"
         "1. Answer based ONLY on the provided sources above.\n"
         "2. If sources don't contain the answer, say: 'The document does not contain information about this.'\n"
         "3. Return JSON with 'answer' and 'sources' fields.\n"
         "4. Each source must include: 'sourceId' (use the SOURCE_n ID above), 'quote' (exact text from source), 'reason' (why it supports the answer).\n"
         "5. Use the EXACT sourceId from the evidence (e.g., 'SOURCE_1', 'SOURCE_2').\n"
         "6. Do NOT invent page numbers or source IDs.\n"
         "7. Cite multiple sources when appropriate.\n"
         "8. Distinguish the document's claims from your explanation.\n"
         "9. CRITICAL CITATION MANDATE - IMMEDIATE INLINE PLACEMENT: every factual claim, date, name, statistic, or finding carries its [SOURCE_n] marker IMMEDIATELY after that clause or sentence; NEVER cluster citations at the end of a sentence, paragraph, or answer.\n"
         "   If citing multiple sources for one statement, write separate markers like [SOURCE_1] [SOURCE_2]; NEVER write combined brackets like [SOURCE_1, SOURCE_2] or [ , ].\n"
         "Output ONLY a JSON object:\n"
         "{\"answer\": \"The fee doubles after the first year. [SOURCE_1]\", \"sources\": [{\"sourceId\": \"SOURCE_1\", \"quote\": \"<exact text from that source>\", \"reason\": \"<why it supports the answer>\"}]}";
    return s;
}
static std::string AiWebSystem(const std::string& pageText, const std::string& title,
                               const std::string& url, bool article, bool truncated) {
    std::string s = PromptFirstBody("websidechat");
    s += "\n\nPAGE:\nTITLE: " + (title.empty() ? "(untitled)" : title) + "\n";
    s += "URL: " + (url.empty() ? "(unknown)" : url) + "\n";
    if (!article) s += "NOTE: the page resisted article extraction, so the text below is everything visible on it (navigations and chrome included).\n";
    if (truncated) s += "NOTE: the page is long; only its beginning and end are shown.\n";
    s += "\nPAGE EVIDENCE:\n\n";
    std::vector<std::string> segs = AiSplitPageSegments(pageText);
    for (size_t i = 0; i < segs.size(); i++) {
        s += "[SOURCE_" + std::to_string(i + 1) + "]\n" + segs[i] + "\n\n";
    }
    s += "Return ONLY a JSON object in this exact shape:\n";
    s += "{\"answer\": \"<your full answer with inline [SOURCE_n] markers>\", \"sources\": [{\"sourceId\": \"SOURCE_1\", \"quote\": \"<exact text copied from that segment>\", \"reason\": \"<why it supports the answer>\"}]}\n";
    s += "The 'quote' must be a short exact excerpt (up to ~300 characters) copied verbatim from the cited segment - it is used to find the passage on the live page.\n";
    s += "CRITICAL CITATION MANDATE - IMMEDIATE INLINE PLACEMENT: every factual claim, date, name, statistic, or finding carries its [SOURCE_n] marker IMMEDIATELY after that clause or sentence; NEVER cluster citations at the end of a sentence, paragraph, or answer.\n";
    s += "If citing multiple sources for one statement, write separate markers like [SOURCE_1] [SOURCE_2]. NEVER write combined brackets like [SOURCE_1, SOURCE_2] or [ , ].";
    return s;
}
static std::string AiEditorSystem(const std::string& title, const std::string& text) {
    std::string s = PromptFirstBody("editorsidechat");
    s += "\n\nDOCUMENT TITLE: " + (title.empty() ? "Untitled Document" : title) + "\n";
    s += "DOCUMENT CONTENT:\n";
    s += text.empty() ? "(The document is currently empty.)" : text;
    return s;
}

// ---- source id parsing + resolution (ResolveSources) ----
static int AiParseSourceId(const std::string& s) {
    // SOURCE_3 / source 3 / [3] / bare digits
    std::string low;
    for (char c : s) low += (char)tolower((unsigned char)c);
    size_t p = low.find("source");
    if (p != std::string::npos) {
        size_t d = low.find_first_of("0123456789", p);
        if (d != std::string::npos) return atoi(low.c_str() + d);
    }
    size_t d0 = low.find_first_of("0123456789");
    if (d0 != std::string::npos) return atoi(low.c_str() + d0);
    return -1;
}
static std::vector<AiSource> AiResolveDocSources(const std::vector<AiModelSource>& in,
                                                 const std::vector<AiRetrieved>& ev) {
    std::vector<AiSource> out;
    if (ev.empty()) return out;
    // 0-based shift: every id in range and at least one == 0
    int offset = 0;
    std::vector<int> ids;
    bool allInRange = !in.empty(), sawZero = false;
    for (auto& s : in) {
        int n = AiParseSourceId(s.id);
        ids.push_back(n);
        if (n < 0 || n >= (int)ev.size()) allInRange = false;
        if (n == 0) sawZero = true;
    }
    if (allInRange && sawZero) offset = 1;
    std::set<int> seen;
    for (size_t i = 0; i < in.size(); i++) {
        int n = ids[i] + offset;
        if (n < 1 || n > (int)ev.size()) continue;
        if (seen.count(n)) continue;
        seen.insert(n);
        const AiChunk& ch = *ev[n - 1].chunk;
        AiSource src;
        src.id = in[i].id.empty() ? ("SOURCE_" + std::to_string(n - offset)) : in[i].id;
        src.quote = in[i].quote;
        src.reason = in[i].reason;
        size_t ws = 0, we = 0;
        AiQuoteHit hit = AiLocateInText(ch.text, src.quote, ws, we);
        src.location = hit == AQH_EXACT ? AQL_EXACT : hit == AQH_APPROX ? AQL_APPROX : AQL_UNLOCATED;
        src.verified = hit != AQH_NONE || AiQuotePlausible(src.quote, ch.text);
        // the page whose span carries the located words (chunk-relative)
        int pageIdx = ch.pages.empty() ? -1 : ch.pages.front().page;
        if (hit != AQH_NONE) {
            for (auto& sp : ch.pages) {
                if (ws >= (size_t)sp.wordStart && we <= (size_t)sp.wordEnd) {
                    pageIdx = sp.page;
                    src.rects.push_back({ sp.l, sp.t, sp.r - sp.l, sp.b - sp.t });
                    break;
                }
            }
        }
        src.pageIndex = pageIdx;
        src.page = pageIdx + 1;
        out.push_back(std::move(src));
    }
    return out;
}
static std::vector<AiSource> AiResolveWebSources(const std::vector<AiModelSource>& in,
                                                 const std::string& answer,
                                                 const std::vector<std::string>& segs,
                                                 const std::string& pageText) {
    std::vector<AiSource> out;
    if (segs.empty()) return out;
    std::string pageNorm = AiNormalizeForMatch(pageText);
    int offset = 0;
    std::vector<int> ids;
    bool allInRange = !in.empty(), sawZero = false;
    for (auto& s : in) {
        int n = AiParseSourceId(s.id);
        ids.push_back(n);
        if (n < 0 || n >= (int)segs.size()) allInRange = false;
        if (n == 0) sawZero = true;
    }
    if (allInRange && sawZero) offset = 1;
    std::set<int> seen;
    auto keep = [&](int n, const std::string& id, const std::string& quote, const std::string& reason) {
        if (n < 1 || n > (int)segs.size()) return;
        if (seen.count(n)) return;
        seen.insert(n);
        AiSource src;
        src.id = id.empty() ? ("SOURCE_" + std::to_string(n - offset)) : id;
        src.reason = reason;
        src.pageIndex = -1;
        src.page = 0;
        std::string q = AiNormalizeForMatch(quote);
        if (q.size() >= 3 && !pageNorm.empty() && pageNorm.find(q) != std::string::npos) {
            src.quote = quote;
            src.verified = true;
            src.location = AQL_EXACT;
        } else {
            src.quote = AiSegmentHead(segs[n - 1]);
            src.verified = true;
            src.location = AQL_APPROX;
        }
        out.push_back(std::move(src));
    };
    for (size_t i = 0; i < in.size(); i++)
        keep(ids[i] + offset, in[i].id, in[i].quote, in[i].reason);
    // marker backfill: inline SOURCE_n the sources list never carried
    std::regex rx("SOURCE[\\s_\\-]*(\\d{1,3})", std::regex_constants::icase);
    auto begin = std::sregex_iterator(answer.begin(), answer.end(), rx);
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        int n = atoi(it->str(1).c_str()) + offset;
        keep(n, "SOURCE_" + it->str(1), "", "");
    }
    return out;
}

// ---- the reply thread's body (GenerateReplyAsync) ----
static void AiPushHistoryWindow(std::vector<std::pair<std::string, std::string>>& hist,
                                const std::vector<AiChatMsg>& msgs, size_t maxN) {
    // roles != system, no loading/errored rows; assistant content stripped of markers
    std::vector<std::pair<std::string, std::string>> all;
    for (auto& m : msgs) {
        std::string c = m.content;
        // StripCitationMarkers: drop SOURCE_n tokens and empty brackets
        c = std::regex_replace(c, std::regex(R"([\[(]\s*SOURCE\s*_\s*\d+\s*[\])])",
                                              std::regex_constants::icase), "");
        c = std::regex_replace(c, std::regex("\\[\u005cs*(,\u005cs*)*\\]"), "");
        all.push_back({ m.role == 0 ? "user" : "assistant", c });
    }
    // the current input was already appended by the caller
    size_t n = min(maxN, all.size() > 0 ? all.size() - 1 : 0);
    for (size_t i = all.size() - 1 - n; i < all.size() - 1; i++) hist.push_back(all[i]);
}
static bool AiLooksLikeFollowUp(const std::string& input) {
    static const char* openers[] = { "it","its","that","this","these","those","they","them",
        "their","he","she","his","her","so","and","but","because","then","also","more",
        "else","explain","continue" };
    std::string first;
    for (char c : input) {
        if (isspace((unsigned char)c)) break;
        if (isalpha((unsigned char)c)) first += (char)tolower((unsigned char)c);
    }
    for (const char* o : openers) if (first == o) return true;
    std::vector<std::string> terms;
    AiQueryTerms(input, terms);
    return terms.size() < 3;
}
static std::string AiBuildRetrievalQuery(const std::string& input,
                                         const std::vector<AiChatMsg>& msgs) {
    // the last User message before this input
    std::string prev;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->role == 0 && it->content != input) { prev = it->content; break; }
    if (prev.empty() || prev == input || !AiLooksLikeFollowUp(input)) return input;
    if (prev.size() > 200) prev.resize(200);
    std::string combined = prev + " " + input;
    while (combined.size() > 400 && prev.size() > 32) {
        prev.resize(prev.size() - 32);
        combined = prev + " " + input;
    }
    return combined;
}
static std::string AiFormatDuration(DWORD ms) {
    DWORD s = (ms + 999) / 1000;
    wchar_t b[64];
    if (s < 60) _snwprintf_s(b, _TRUNCATE, L"%us", s);
    else if (s % 60 == 0) _snwprintf_s(b, _TRUNCATE, L"%dm", s / 60);
    else _snwprintf_s(b, _TRUNCATE, L"%dm %us", s / 60, s % 60);
    return WxU(b);
}
static void AiStartReplyThread(const std::string& key, const std::string& input);

static void AiRunReply(const std::string& key, const std::string& input) {
    AiCtxState* st = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_aiMtx);
        auto it = g_aiCtx.find(key);
        if (it == g_aiCtx.end()) return;
        st = &it->second;
        st->inflight = true;
        st->streamingRaw.clear();
        st->cancel = false;
        st->gen++;
        g_aiReplyGen = st->gen;
    }
    unsigned long long gen = g_aiReplyGen;
    DWORD t0 = GetTickCount();
    AiCfg cfg = g_aiCfg;
    AiError err;
    std::string raw, system;
    std::vector<std::pair<std::string, std::string>> hist;
    std::vector<AiRetrieved> ev;
    std::vector<std::string> webSegs;
    std::string webPageText, webTitle, webUrl;
    std::vector<AiModelSource> msources;
    bool ok = false, delivered = false;
    int mode = 0;
    std::vector<AiChatMsg> msgsSnapshot;
    {
        std::lock_guard<std::mutex> lk(g_aiMtx);
        msgsSnapshot = st->msgs;
        mode = st->mode;
        webTitle = WxU(st->pageTitle);
        webUrl = WxU(st->pageUrl);
    }
    do {
        if (mode == 2) {   // editor: the document IS the evidence
            std::string docText, title;
            {
                std::lock_guard<std::mutex> lk(g_aiMtx);
                docText = g_edAiText;
                title = WxU(st->title);
            }
            system = AiEditorSystem(title, docText);
            break;
        }
        if (mode == 1) {   // web: the page snapshot from the browser
            std::string snap;
            bool readable = false;
            {
                std::lock_guard<std::mutex> lk(g_aiMtx);
                snap = g_webAiText;
            }
            // the snapshot's own truth: an unreadable page answers with the
            // reference's "no page" line, not an HTTP error
            crxk::J sj;
            if (crxk::JParse(snap, sj) && sj.t == 'o') {
                const crxk::J* okj = sj.get("ok");
                const crxk::J* tx = sj.get("text");
                readable = okj && okj->b && tx && tx->t == 's' && !tx->s.empty();
            }
            if (!readable) {
                AiEv* e = AiEvMake(AIEV_ERROR);
                e->key = key; e->a = WxU("I can't read a page here. Open a web page in the browser tab and ask again.");
                AiPost(e);
                ok = true; delivered = true;
                break;
            }
            std::string title, url, mode2;
            bool article = true;
            crxk::J sj2;
            if (crxk::JParse(snap, sj2) && sj2.t == 'o') {
                const crxk::J* p;
                if ((p = sj2.get("title")) && p->t == 's') title = p->s;
                if ((p = sj2.get("url"))) {
                    if (p->t == 's') url = p->s;
                }
                if ((p = sj2.get("mode")) && p->t == 's') { mode2 = p->s; article = (mode2 == "readability"); }
            }
            (void)mode2;
            const crxk::J* tx = sj2.get("text");
            bool tr = false;
            webPageText = AiClampPageText(tx && tx->t == 's' ? tx->s : std::string(), tr);
            // the page evidence rides EVERY turn (v1.19.95: endpoints are stateless)
            system = AiWebSystem(webPageText, title, url, article, tr);
            break;
        }
        // document: retrieval
        ev.clear();
        {
            // the query embedding rides the same thread (network is free here)
            std::vector<float> qvec;
            std::string q = AiBuildRetrievalQuery(input, msgsSnapshot);
            std::vector<std::string> one = { cfg.embQueryPrefix + q.substr(0, 1200) };
            std::vector<std::vector<float>> vecs;
            AiError e2;
            if (g_aiIndex.dim > 0 && g_aiIndex.vectors.size() == g_aiIndex.chunks.size() &&
                AiEmbedBatch(cfg, one, vecs, e2, nullptr) && vecs.size() == 1)
                qvec = vecs[0];
            ev = AiRetrieve(g_aiIndex, q, qvec.empty() ? nullptr : &qvec,
                            cfg.topK, cfg.evidenceBudget);
        }
        if (ev.empty()) {
            AiEv* e = AiEvMake(AIEV_ERROR);
            e->key = key; e->a = WxU("I couldn't find any relevant passages in the document for your question.");
            AiPost(e);
            ok = true; delivered = true;   // delivered as its own message; no error surface
            raw = "";
        } else {
            system = AiSidechatSystem(ev);
        }
    } while (false);
    if (!ok && !system.empty()) {
        AiPushHistoryWindow(hist, msgsSnapshot, (size_t)max(2, cfg.maxHistory));
        std::string full;
        ok = AiChatStream(cfg, system, hist, input, 0, nullptr, false,
                          [&](const std::string& piece) {
                              raw += piece;
                              AiEv* e = AiEvMake(AIEV_DELTA);
                              e->key = key; e->gen = gen;
                              e->a = WxU(AiExtractStreamingAnswer(raw));
                              AiPost(e);
                          }, full, err, &st->cancel);
        raw = full;
        if (ok) msources = AiParseStructured(AiStripThinkSpans(raw)).sources;
    }
    DWORD elapsed = GetTickCount() - t0;
    // finalize
    bool cancelled = st->cancel.load();
    if (cancelled) {
        AiEv* e = AiEvMake(AIEV_CANCEL);
        e->key = key; e->gen = gen;
        AiPost(e);
    } else if (ok && !raw.empty()) {
        std::string clean = AiStripThinkSpans(raw);
        AiModelReply parsed = AiParseStructured(clean);
        std::vector<AiSource> sources;
        if (mode == 1) {
            webSegs = AiSplitPageSegments(webPageText);
            sources = AiResolveWebSources(parsed.sources, parsed.answer, webSegs, webPageText);
        } else if (mode == 0 && !ev.empty()) {
            sources = AiResolveDocSources(parsed.sources, ev);
        }
        AiEv* e = AiEvMake(AIEV_DONE);
        e->key = key; e->gen = gen;
        e->a = WxU(parsed.answer.empty() ? clean : parsed.answer);
        // sources blob
        std::string sb = "[";
        for (size_t i = 0; i < sources.size(); i++) {
            if (i) sb += ",";
            sb += "{\"id\":" + WxJsonStr(WxU(sources[i].id)) +
                  ",\"page\":" + std::to_string(sources[i].page) +
                  ",\"pageIndex\":" + std::to_string(sources[i].pageIndex) +
                  ",\"quote\":" + WxJsonStr(WxU(sources[i].quote)) +
                  ",\"reason\":" + WxJsonStr(WxU(sources[i].reason)) +
                  ",\"verified\":" + (sources[i].verified ? "true" : "false") +
                  ",\"location\":" + std::to_string(sources[i].location) + "}";
        }
        sb += "]";
        e->b = sb;
        e->d = (double)elapsed;
        AiPost(e);
    } else if (!ok) {
        if (!delivered) {
            AiEv* e = AiEvMake(AIEV_ERROR);
            e->key = key; e->gen = gen;
            e->a = WxU(err.Friendly());
            AiPost(e);
        }
    } else {
        if (!delivered) {          // empty reply, no error: a bad response
            AiEv* e = AiEvMake(AIEV_ERROR);
            e->key = key; e->gen = gen;
            e->a = WxU(AiError{ AI_ERR_BAD_RESPONSE }.Friendly());
            AiPost(e);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_aiMtx);
        st->inflight = false;
        st->loadingBubble = false;
    }
}
static void AiStartReplyThread(const std::string& key, const std::string& input) {
    // detached: a background conversation's late finish must never hold the UI
    // thread hostage (the reply's state lives in the context map, the events
    // find their own way through the pump)
    std::thread([key, input]() { AiRunReply(key, input); }).detach();
}

// ---- the AI panel HTML (resources 401) ----
static std::wstring AiPanelHtml() {
    HRSRC hs = FindResourceW(g.hinst, MAKEINTRESOURCEW(401), (LPCWSTR)RT_RCDATA);
    if (!hs) return L"";
    HGLOBAL hg = LoadResource(g.hinst, hs);
    if (!hg) return L"";
    const unsigned char* data = (const unsigned char*)LockResource(hg);
    DWORD len = SizeofResource(g.hinst, hs);
    if (!data || !len) return L"";
    std::vector<unsigned char> out;
    if (crxk::Inflate(data, len, out, 4 * 1024 * 1024) && !out.empty())
        return WxW(std::string((const char*)out.data(), out.size()));
    return WxW(std::string((const char*)data, len));
}
static std::wstring AiSumHtml() {
    HRSRC hs = FindResourceW(g.hinst, MAKEINTRESOURCEW(402), (LPCWSTR)RT_RCDATA);
    if (!hs) return L"";
    HGLOBAL hg = LoadResource(g.hinst, hs);
    if (!hg) return L"";
    const unsigned char* data = (const unsigned char*)LockResource(hg);
    DWORD len = SizeofResource(g.hinst, hs);
    if (!data || !len) return L"";
    std::vector<unsigned char> out;
    if (crxk::Inflate(data, len, out, 4 * 1024 * 1024) && !out.empty())
        return WxW(std::string((const char*)out.data(), out.size()));
    return WxW(std::string((const char*)data, len));
}

// ---- context binding (OpenAiChat's priority: editor, web, book) ----
static void AiSendSnapshot(AiCtxState& st) {
    // the transcript lands as one msgs frame
    std::string j = "{\"cmd\":\"msgs\",\"msgs\":[";
    for (size_t i = 0; i < st.msgs.size(); i++) {
        auto& m = st.msgs[i];
        if (i) j += ",";
        j += "{\"role\":" + std::string(m.role == 0 ? "\"user\"" : "\"bot\"") +
             ",\"content\":" + WxJsonStr(WxU(m.content)) + "}";
    }
    j += "]}";
    AiPanelSend(j);
}
static void AiSendCtxFrame(AiCtxState& st) {
    const char* mode = st.mode == 2 ? "editor" : st.mode == 1 ? "web" : "doc";
    std::string j = std::string("{\"cmd\":\"ctx\",\"mode\":\"") + mode + "\",\"title\":" +
                    WxJsonStr(st.title) + "}";
    AiPanelSend(j);
    AiSendSnapshot(st);
    if (st.loadingBubble) AiPanelSend("{\"cmd\":\"loading\"}");
    AiPanelSend(WxW(AiStatusJson("status")));
}
static void AiBindContext() {
    std::string key;
    int mode = 0;
    std::wstring title, ref;
    if (g_editOn) {
        mode = 2;
        std::wstring id = g_edPath.empty() ? L"untitled" : g_edPath;
        key = AiEditorSessionKey(id);
        title = g_edTitle.empty() ? L"Document" : g_edTitle;
        ref = WxU(title.empty() ? L"Document" : g_edTitle);
    } else if (g_webOn) {
        mode = 1;
        WebTab* t = (g_wtCur >= 0 && g_wtCur < (int)g_wt.size()) ? &g_wt[g_wtCur] : nullptr;
        if (!t) return;
        wchar_t tb[32];
        _snwprintf_s(tb, _TRUNCATE, L"%llu", t->id);
        key = AiWebSessionKey(tb);
        title = t->title.empty() ? WebHostOf(t->url) : t->title;
        ref = WxU(t->url);
        std::lock_guard<std::mutex> lk(g_aiMtx);
        auto& st = g_aiCtx[key];
        st.pageTitle = t->title; st.pageUrl = t->url;
    } else {
        DocState& D = DVF();
        if (D.path.empty()) {
            // no book: the panel still opens (the VM answers "open a PDF first")
            key = "";
        } else {
            mode = 0;
            key = AiDocSessionKey(D.path);
            title = D.name;
            ref = WxU(D.path);
        }
    }
    // same key: the title may have drifted (a tab's load finished) but the
    // conversation must NOT reset - only the title line repaints
    if (key == g_aiCurKey && !key.empty()) {
        std::lock_guard<std::mutex> lk(g_aiMtx);
        auto it = g_aiCtx.find(key);
        if (it != g_aiCtx.end()) {
            it->second.title = title;
            AiPanelSend("{\"cmd\":\"title\",\"title\":" + WxJsonStr(title) + "}");
        }
        return;
    }
    g_aiCurKey = key;
    if (key.empty()) {
        AiPanelSend("{\"cmd\":\"ctx\",\"mode\":\"doc\",\"title\":\"\"}");
        AiPanelSend(WxW(AiStatusJson("status")));
        return;
    }
    AiCtxState* st = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_aiMtx);
        st = &g_aiCtx[key];
        if (st->key.empty()) {
            st->key = key;
            st->mode = mode;
            st->title = title;
            st->pathOrRef = WxW(ref);
            // restore the newest conversation for this source (memory wins)
            AiConv* cv = AiHistLatest(mode == 2 ? "editor" : mode == 1 ? "web" : "doc",
                                      WxU(st->pathOrRef));
            if (cv) { st->convId = cv->id; st->msgs = cv->msgs; }
        } else {
            st->mode = mode;
            st->title = title;
            if (mode == 0) st->pathOrRef = WxW(ref);
        }
        AiSendCtxFrame(*st);
    }
    // the document's index: load or start the build
    if (mode == 0 && !g_aiIndexReady && !g_aiIndexing)
        AiIndexKickoff(DVF().path);
}
static void AiIndexKickoff(const std::wstring& path);   // defined with the pump

// ---- the panel window host ----
static void AiPanelEnsure() {
    if (g_aiCtr || g_aiBuilding) return;
    WebEnsureEnv();
    if (!g_wvEnv) return;
    g_aiBuilding = true;
    g_wvEnv->CreateCoreWebView2Controller(
        g.view,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                g_aiBuilding = false;
                if (FAILED(hr) || !c) return S_OK;
                g_aiCtr = c;
                c->get_CoreWebView2(&g_aiWeb);
                wrl::ComPtr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2)
                    c2->put_DefaultBackgroundColor({ 255, 0x1c, 0x1c, 0x1c });
                EventRegistrationToken tk;
                g_aiWeb->add_WebMessageReceived(
                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                            LPWSTR msg = nullptr;
                            if (SUCCEEDED(a->get_WebMessageAsJson(&msg)) && msg) {
                                AiPanelMessage(msg);
                                CoTaskMemFree(msg);
                            }
                            return S_OK;
                        }).Get(), &tk);
                std::wstring html = AiPanelHtml();
                if (!html.empty()) g_aiWeb->NavigateToString(html.c_str());
                RECT r; GetClientRect(g.view, &r);
                AiOverlayLayout();
                c->put_IsVisible(g_aiOn ? TRUE : FALSE);
                return S_OK;
            }).Get());
}
static int AiOverlayW(int vw) {
    int w = (int)(380 * g.dpi);
    return min(w, max(0, vw - (int)(80 * g.dpi)));
}
static void AiOverlayLayout() {
    if (!g_aiCtr) return;
    RECT r; GetClientRect(g.view, &r);
    int w = g_aiOn ? AiOverlayW(r.right) : 0;
    RECT b{ max(0, r.right - w), 0, r.right, r.bottom };
    g_aiCtr->put_Bounds(b);
    g_aiCtr->put_IsVisible(g_aiOn && r.right > (int)(100 * g.dpi) ? TRUE : FALSE);
}
static void AiChatToggle() {
    g_aiOn = !g_aiOn;
    AiLoadSettings();
    // the workers' event pump: a message-only window, made once
    if (!g_aiMsg) {
        static bool cls = false;
        if (!cls) {
            cls = true;
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.lpszClassName = L"docsy_ai_pump";
            wc.hInstance = g.hinst;
            RegisterClassW(&wc);
        }
        g_aiMsg = CreateWindowExW(0, L"docsy_ai_pump", L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, g.hinst, nullptr);
    }
    if (g_aiOn && !g_aiCtr) AiPanelEnsure();
    if (g_aiOn) AiBindContext();
    AiOverlayLayout();
    if (g.view) InvalidateRect(g.view, nullptr, FALSE);
    if (g_editOn && g_edCtr) {   // the editor's keys yield while the panel shows
        // nothing special: the panel overlays the editor's right edge
    }
}
