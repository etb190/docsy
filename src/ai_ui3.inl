// ---------------- docsy AI: the panel's message switch + the event pump ----------------
#pragma once

// ---- the connection test (AiSettingsViewModel.TestConnectionAsync) ----
static void AiRunTest(std::string settingsJson) {
    std::thread([settingsJson]() {
        // the worker's guard: an uncaught exception on a detached thread is
        // std::terminate - the test would take the whole app with it
        try {
        std::string out;
        auto finish = [&out](const std::string& line) {
            if (!out.empty()) out += "\n";
            out += line;
        };
        // a throwaway config from the panel's current fields
        AiCfg c;
        {
            crxk::J j;
            if (crxk::JParse(settingsJson, j) && j.t == 'o') {
                const crxk::J* p;
                if ((p = j.get("baseUrl")) && p->t == 's') c.baseUrl = WxW(p->s);
                if ((p = j.get("apiKey")) && p->t == 's') c.apiKey = WxW(p->s);
                if ((p = j.get("model")) && p->t == 's') c.model = WxW(p->s);
            }
        }
        // 1. the door probe (GET /models, 8s - IsAvailableAsync's availability check)
        {
            std::wstring root = c.baseUrl;
            while (!root.empty() && root.back() == L'/') root.pop_back();
            std::string resp; int status = 0;
            bool ok = AiHttpGetRaw(root + L"/models", c.apiKey, 8000, resp, status) &&
                      status >= 200 && status < 300;
            finish(ok ? "\xe2\x9c\x93 Provider reachable" :
                       ("\xe2\x9c\x97 Provider unreachable (HTTP " +
                        std::to_string(status) + ")"));
        }
        // 2. a real chat completion
        {
            AiError e;
            std::string content;
            bool ok = AiChatBuffered(c, "Reply with just 'OK'", {}, "test", 64, nullptr, false,
                                     content, e, nullptr, false) && !content.empty();
            finish(ok ? "\xe2\x9c\x93 Chat answered" :
                        ("\xe2\x9c\x97 Chat failed: " + WxU(e.Friendly())));
        }
        // 3. one embedding (a query-shaped probe: the sidechat's retrieval side)
        {
            std::vector<std::vector<float>> vecs;
            AiError e;
            bool ok = AiEmbedBatch(c, { "connection test" }, true, vecs, e, nullptr) &&
                      !vecs.empty() && !vecs[0].empty();
            finish(ok ? "\xe2\x9c\x93 Embeddings ready" :
                        ("\xe2\x9c\x97 Embeddings failed: " + WxU(e.Friendly())));
        }
        AiEv* ev = AiEvMake(AIEV_TEST_OUT);
        ev->a = out;
        AiPost(ev);
        } catch (...) {
            AiEv* ev = AiEvMake(AIEV_TEST_OUT);
            ev->a = "\xe2\x9c\x97 The test stopped after an internal error.";
            AiPost(ev);
        }
    }).detach();
}

// ---- the panel's message switch ----
static void AiPanelMessage(const wchar_t* json) {
    std::string u8 = WxU(json);
    crxk::J m;
    if (!crxk::JParse(u8, m) || m.t != 'o') return;
    const crxk::J* typ = m.get("type");
    if (!typ || typ->t != 's') return;
    std::string t = typ->s;
    if (t == "ready") {
        g_aiReady = true;
        AiBindContext();
    } else if (t == "close") {
        g_aiOn = false;
        LayoutMain();                        // the view takes its width back: the strip's
                                             // grain shows while the dock width is zeroed
    } else if (t == "newchat") {
        std::lock_guard<std::mutex> lk(g_aiMtx);
        auto it = g_aiCtx.find(g_aiCurKey);
        if (it != g_aiCtx.end() && !it->second.inflight) {
            // the swept conversation is archived, not destroyed
            AiCtxState& st = it->second;
            if (!st.msgs.empty()) {
                const char* kind = st.mode == 2 ? "editor" : st.mode == 1 ? "web" : "doc";
                if (st.convId.empty())
                    st.convId = AiHistEnsure(kind, WxU(st.pathOrRef), WxU(st.title));
                AiHistUpsert(st.convId, kind, WxU(st.pathOrRef), WxU(st.title), st.msgs);
            }
            st.convId.clear();
            st.msgs.clear();
            st.pendingInput.clear();
            AiPanelSend("{\"cmd\":\"cleared\"}");
        }
    } else if (t == "send") {
        const crxk::J* tx = m.get("text");
        std::string input = tx && tx->t == 's' ? tx->s : "";
        // the page has already swept the input clean by the time this frame
        // lands: a silent drop here reads as "the message does not send" - the
        // composer emptied, nothing came back, no word of why. Every drop
        // speaks now.
        if (input.empty()) return;
        if (g_aiCurKey.empty()) {
            AiPanelSend("{\"cmd\":\"error\",\"text\":\"Open a PDF first, then ask me about it here.\"}");
            return;
        }
        AiCtxState* st = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_aiMtx);
            auto it = g_aiCtx.find(g_aiCurKey);
            if (it == g_aiCtx.end()) {
                AiPanelSend("{\"cmd\":\"error\",\"text\":\"Open a PDF first, then ask me about it here.\"}");
                return;
            }
            st = &it->second;
            if (st->inflight) {
                AiPanelSend("{\"cmd\":\"error\",\"text\":\"One answer is already on its way - let it finish or press the stop button.\"}");
                return;
            }
        }
        if (st->mode == 0 && !g_aiIndexReady) {
            // queued behind the index build (or refused when the build failed)
            if (g_aiIndexing) {
                std::lock_guard<std::mutex> lk(g_aiMtx);
                st->pendingInput = input;
                st->msgs.push_back({ 0, input, (long long)GetTickCount64(), {} });
                st->loadingBubble = true;
                AiPanelSend("{\"cmd\":\"user\",\"text\":" + WxJsonStr(input) + "}");
                AiPanelSend("{\"cmd\":\"loading\",\"text\":\"Preparing document…\"}");
                return;
            }
            if (st->msgs.empty() && g_aiCtx[g_aiCurKey].pathOrRef.empty()) {
                AiPanelSend("{\"cmd\":\"error\",\"text\":\"Open a PDF first, then ask me about it here.\"}");
                return;
            }
        }
        // the user bubble + the thinking box ride at once
        {
            std::lock_guard<std::mutex> lk(g_aiMtx);
            st->msgs.push_back({ 0, input, (long long)GetTickCount64(), {} });
        }
        AiPanelSend("{\"cmd\":\"user\",\"text\":" + WxJsonStr(input) + "}");
        AiPanelSend("{\"cmd\":\"loading\",\"text\":\"Thinking…\"}");
        st->loadingBubble = true;
        if (st->mode == 2) {
            // the editor's text: ask the page, the reply starts on the answer
            g_edAiPendingInput = input;
            g_edAiTextPending = true;
            if (g_edWeb) g_edWeb->PostWebMessageAsJson(L"{\"cmd\":\"aitext\"}");
            else g_edAiTextPending = false;
        } else if (st->mode == 1) {
            // the live page: one fresh extraction per question
            g_webAiPendingInput = input;
            AiWebExtract(++g_sumGen);
        } else {
            AiStartReplyThread(g_aiCurKey, input);
        }
    } else if (t == "stop") {
        std::lock_guard<std::mutex> lk(g_aiMtx);
        auto it = g_aiCtx.find(g_aiCurKey);
        if (it != g_aiCtx.end()) it->second.cancel = true;
    } else if (t == "semantic") {
        AiSemanticToggle();
    } else if (t == "settings-get") {
        AiPanelSend("{\"cmd\":\"settings\",\"settings\":" + AiSettingsJson() + "}");
    } else if (t == "settings-save") {
        const crxk::J* s = m.get("settings");
        if (s && s->t == 'o') {
            auto gs = [&](const char* k, std::wstring& v) {
                const crxk::J* p = s->get(k);
                if (p && p->t == 's') v = WxW(p->s);
            };
            auto gn = [&](const char* k, double& v) {
                const crxk::J* p = s->get(k);
                if (p && p->t == 'd') v = p->d;
            };
            auto gi = [&](const char* k, int& v) {
                const crxk::J* p = s->get(k);
                if (p && p->t == 'd') v = (int)p->d;
            };
            gs("baseUrl", g_aiCfg.baseUrl);
            gs("apiKey", g_aiCfg.apiKey);
            gs("model", g_aiCfg.model);
            gn("temperature", g_aiCfg.temperature);
            {
                double mt = g_aiCfg.maxTokens;
                gn("maxTokens", mt);
                g_aiCfg.maxTokens = (int)mt;
            }
            gn("topP", g_aiCfg.topP);
            gs("reasoningEffort", g_aiCfg.reasoningEffort);
            gs("embeddingModel", g_aiCfg.embeddingModel);
            gi("topK", g_aiCfg.topK);
            gi("evidenceBudget", g_aiCfg.evidenceBudget);
            gi("maxHistory", g_aiCfg.maxHistory);
            if (g_aiCfg.topK < 1) g_aiCfg.topK = 1;
            if (g_aiCfg.evidenceBudget < 1000) g_aiCfg.evidenceBudget = 1000;
            if (g_aiCfg.maxHistory < 2) g_aiCfg.maxHistory = 2;
            AiSaveSettings();
        }
    } else if (t == "settings-test") {
        const crxk::J* s = m.get("settings");
        if (s && s->t == 'o') {
            std::string sj;
            WxJsonEmit(*s, sj);
            AiRunTest(sj);
        }
    } else if (t == "history-open") {
        AiHistLoad();
        std::string curConv;
        {
            std::lock_guard<std::mutex> lk(g_aiMtx);
            auto it = g_aiCtx.find(g_aiCurKey);
            if (it != g_aiCtx.end()) curConv = it->second.convId;
        }
        std::string j = "{\"cmd\":\"history\",\"entries\":[";
        // newest first
        std::vector<AiConv*> rows;
        for (auto& c : g_aiHist) rows.push_back(&c);
        std::sort(rows.begin(), rows.end(),
                  [](auto* a, auto* b) { return a->updatedAt > b->updatedAt; });
        for (size_t i = 0; i < rows.size(); i++) {
            auto* c = rows[i];
            if (i) j += ",";
            j += "{\"id\":" + WxJsonStr(WxU(c->id)) +
                 ",\"kind\":" + WxJsonStr(WxU(c->kind)) +
                 ",\"title\":" + WxJsonStr(WxU(c->title)) +
                 ",\"sourceRef\":" + WxJsonStr(WxU(c->sourceRef)) +
                 ",\"count\":" + std::to_string(c->msgs.size()) +
                 ",\"updated\":" + WxJsonStr(WxU(L"")) +
                 ",\"current\":" + (c->id == curConv ? "true" : "false") + "}";
        }
        j += "]}";
        AiPanelSend(j);
    } else if (t == "history-back") {
        // the live transcript rides again
        std::lock_guard<std::mutex> lk(g_aiMtx);
        auto it = g_aiCtx.find(g_aiCurKey);
        if (it != g_aiCtx.end()) AiSendSnapshot(it->second);
    } else if (t == "open-conv") {
        const crxk::J* id = m.get("id");
        if (id && id->t == 's') {
            AiConv* cv = AiHistById(id->s);
            if (cv) {
                std::lock_guard<std::mutex> lk(g_aiMtx);
                auto it = g_aiCtx.find(g_aiCurKey);
                if (it != g_aiCtx.end()) {
                    it->second.msgs = cv->msgs;
                    it->second.convId = cv->id;
                    AiSendSnapshot(it->second);
                }
            }
        }
    } else if (t == "del-conv") {
        const crxk::J* id = m.get("id");
        if (id && id->t == 's') {
            AiHistDelete(id->s);
            std::lock_guard<std::mutex> lk(g_aiMtx);
            auto it = g_aiCtx.find(g_aiCurKey);
            if (it != g_aiCtx.end() && it->second.convId == id->s) it->second.convId.clear();
        }
    } else if (t == "reopen") {
        // docsy has no external reopen path: the archive banner explains itself
    } else if (t == "cite") {
        const crxk::J* id = m.get("id");
        if (id && id->t == 's') {
            AiCtxState* st = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_aiMtx);
                auto it = g_aiCtx.find(g_aiCurKey);
                if (it == g_aiCtx.end()) return;
                st = &it->second;
            }
            // find the source by its id across the assistant messages
            int n = AiParseSourceId(id->s);
            AiSource* src = nullptr;
            for (auto it = st->msgs.rbegin(); it != st->msgs.rend() && !src; ++it)
                for (auto& s : it->sources)
                    if (AiParseSourceId(s.id) == n) { src = &s; break; }
            if (!src) return;
            if (st->mode == 1) {
                AiWebCiteJump(WxW(src->quote));
            } else if (st->mode == 0) {
                std::lock_guard<std::mutex> lk(g_aiMtx);
                // the rects were not persisted: re-locate against the live index
                if (!g_aiIndexReady || src->pageIndex < 0) {
                    g.statusHoldText = L"The quoted passage could not be found in this document.";
                    g.statusHoldUntil = GetTickCount64() + 4000;
                    StatusRepaint();
                    return;
                }
                // re-locate the quote against the live index: the verified span's
                // page box when it still lands, else the cited page's own box
                std::vector<FRect> rects;
                bool found = false;
                if (src->location != AQL_UNLOCATED && !src->quote.empty()) {
                    for (auto& ch : g_aiIndex.chunks) {
                        for (auto& sp : ch.pages) {
                            if (sp.page != src->pageIndex) continue;
                            size_t ws, we;
                            if (AiLocateInText(ch.text, src->quote, ws, we) != AQH_NONE &&
                                ws >= (size_t)sp.wordStart && we <= (size_t)sp.wordEnd) {
                                rects.push_back({ sp.l, sp.t, sp.r - sp.l, sp.b - sp.t });
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
                    }
                }
                if (!found)
                    for (auto& ch : g_aiIndex.chunks)
                        for (auto& sp : ch.pages)
                            if (sp.page == src->pageIndex) {
                                rects.push_back({ sp.l, sp.t, sp.r - sp.l, sp.b - sp.t });
                                found = true;
                                break;
                            }
                AiNavigateToSource(src->pageIndex, rects);
            }
        }
    }
}

// ---- the workers' event pump (the UI thread's side of the WM_APP channel) ----
static void AiHandleEvent(AiEv* e) {
    std::unique_ptr<AiEv> hold(e);
    switch (e->kind) {
    case AIEV_DELTA: {
        // the streaming paint: the visible answer behind the scaffolding
        if (e->key == g_aiCurKey && !e->a.empty())
            AiPanelSend("{\"cmd\":\"delta\",\"text\":" + WxJsonStr(e->a) + "}");
        break;
    }
    case AIEV_DONE: {
        AiCtxState* st = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_aiMtx);
            auto it = g_aiCtx.find(e->key);
            if (it == g_aiCtx.end()) break;
            st = &it->second;
            // the transcript row + persistence (the answer is saved the moment it lands)
            AiChatMsg m;
            m.role = 1;
            m.content = e->a;
            m.ts = (long long)GetTickCount64();
            // sources back into the transcript for the citation clicks
            crxk::J sb;
            if (crxk::JParse(e->b, sb) && sb.t == 'a')
                for (auto& s : sb.a) {
                    if (s.t != 'o') continue;
                    AiSource src;
                    const crxk::J* p;
                    if ((p = s.get("id")) && p->t == 's') src.id = p->s;
                    if ((p = s.get("page")) && p->t == 'd') src.page = (int)p->d;
                    if ((p = s.get("pageIndex")) && p->t == 'd') src.pageIndex = (int)p->d;
                    if ((p = s.get("quote")) && p->t == 's') src.quote = p->s;
                    if ((p = s.get("reason")) && p->t == 's') src.reason = p->s;
                    if ((p = s.get("verified")) && p->t == 'b') src.verified = p->b;
                    if ((p = s.get("location")) && p->t == 'd') src.location = (int)p->d;
                    m.sources.push_back(std::move(src));
                }
            st->msgs.push_back(std::move(m));
            const char* kind = st->mode == 2 ? "editor" : st->mode == 1 ? "web" : "doc";
            if (st->convId.empty())
                st->convId = AiHistEnsure(kind, WxU(st->pathOrRef), WxU(st->title));
            AiHistUpsert(st->convId, kind, WxU(st->pathOrRef), WxU(st->title), st->msgs);
            if (st->mode == 1 && !st->msgs.empty()) {
                // the notes tab's save-all rides the last assistant's cards? no:
                // g_aiNotesCards stays the notes feature's own state
            }
        }
        if (e->key == g_aiCurKey) {
            std::string j = "{\"cmd\":\"answer\",\"text\":" + WxJsonStr(e->a) +
                            ",\"sources\":" + e->b +
                            ",\"duration\":\"" + AiFormatDuration((DWORD)e->d) + "\"" +
                            ",\"model\":" + WxJsonStr(g_aiCfg.model) + "}";
            AiPanelSend(j);
        }
        break;
    }
    case AIEV_ERROR: {
        {
            std::lock_guard<std::mutex> lk(g_aiMtx);
            auto it = g_aiCtx.find(e->key);
            if (it != g_aiCtx.end()) it->second.loadingBubble = false;
        }
        if (e->key == g_aiCurKey)
            AiPanelSend("{\"cmd\":\"error\",\"text\":" + WxJsonStr(e->a) + "}");
        break;
    }
    case AIEV_CANCEL: {
        {
            std::lock_guard<std::mutex> lk(g_aiMtx);
            auto it = g_aiCtx.find(e->key);
            if (it != g_aiCtx.end()) it->second.loadingBubble = false;
        }
        if (e->key == g_aiCurKey) AiPanelSend("{\"cmd\":\"cancel\"}");
        break;
    }
    case AIEV_IDX_STATUS:
        AiPanelSend(WxW(AiStatusJson("status")));
        break;
    case AIEV_SEM_STATUS:
        g_aiSemStatus = WxW(e->a);
        if (!g_aiSemActive) g_aiSemReadyFlag = e->a.find("ready") != std::string::npos;
        AiPanelSend(WxW(AiStatusJson("status")));
        // every pass exit posts a status, so the pre-booked rebuild (the
        // second press during a drain) starts HERE on the UI thread - the
        // seat lock and the panel send both belong to this thread
        if (g_aiSemRestart && !g_aiSemActive) {
            g_aiSemRestart = false;
            AiSemanticStart();
        }
        break;
    case AIEV_SEM_INDEX: {
        // the embedding pass's handback: merge its vectors into the LIVE seat.
        // The old pass saved its file and dropped the memory - the live dim
        // stayed 0 and every answer in the session retrieved by keyword alone,
        // so the nemotron embeddings the user just paid for never spoke. A
        // merge only lands when the seat still holds the same document AND
        // the same chunk deck (a doc switch or a re-index between the copy
        // and the handback invalidates the ride: the next pass rebuilds).
        if (e->heavy && e->heavy->dim > 0 &&
            !e->heavy->vectors.empty() &&
            e->heavy->vectors.size() == e->heavy->chunks.size()) {
            std::lock_guard<std::mutex> lk(g_aiIdxMtx);
            if (e->heavy->docId == g_aiIndex.docId &&
                e->heavy->chunks.size() == g_aiIndex.chunks.size()) {
                g_aiIndex.dim = e->heavy->dim;
                g_aiIndex.vectors = std::move(e->heavy->vectors);
                g_aiIndex.embModel = e->heavy->embModel;
                g_aiIndex.prefixKey = e->heavy->prefixKey;
            }
        }
        break;
    }
    case AIEV_NOTES_STATUS:
        // the cards report to the sidebar's notes seat, not the chat panel
        NotesPost("{\"cmd\":\"notes-status\",\"text\":" + WxJsonStr(e->a) +
                  ",\"busy\":" + (e->flag ? "true" : "false") + "}");
        break;
    case AIEV_NOTES_CARDS: {
        crxk::J sb;
        if (crxk::JParse(e->b, sb) && sb.t == 'a') {
            g_aiNotesCards.clear();
            for (auto& s : sb.a) {
                if (s.t != 'o') continue;
                AiNoteCard c;
                const crxk::J* p;
                if ((p = s.get("first")) && p->t == 'd') c.first = (int)p->d;
                if ((p = s.get("last")) && p->t == 'd') c.last = (int)p->d;
                if ((p = s.get("content")) && p->t == 's') c.content = p->s;
                g_aiNotesCards.push_back(std::move(c));
            }
        }
        NotesPost("{\"cmd\":\"notes-cards\",\"cards\":" + e->b + "}");
        break;
    }
    case AIEV_WEB_TEXT: {
        // the browser extraction's answer: store the snapshot, route the waiter
        std::lock_guard<std::mutex> lk(g_aiMtx);
        g_webAiText = e->a;
        if (g_webAiTextPending) {
            g_webAiTextPending = false;
            std::string input = g_webAiPendingInput;
            std::string key = g_aiCurKey;
            if (key.empty() || g_webAiText.empty() ||
                g_webAiText.find("\"ok\":true") == std::string::npos) {
                AiPanelSend("{\"cmd\":\"error\",\"text\":\"I can't read a page here. Open a web page in the browser tab and ask again.\"}");
                break;
            }
            AiStartReplyThread(key, input);
        } else if (g_sumWebPending != 0 && e->gen == g_sumWebPending) {
            // the web summary's extraction: the thread takes the page text
            if (g_webAiText.empty() ||
                g_webAiText.find("\"ok\":true") == std::string::npos) {
                SumPost("{\"cmd\":\"error\",\"gen\":" + std::to_string(e->gen) +
                        ",\"text\":\"I can't read a page here. Open a web page in the browser tab and try again.\"}");
                g_sumBusy = false;
                g_sumWebPending = 0;
                break;
            }
            g_sumWebPending = 0;
            unsigned long long gen = e->gen;
            // the page IS the range: no [p. N] anchors
            crxk::J snap;
            std::wstring pageText;
            if (crxk::JParse(g_webAiText, snap) && snap.t == 'o') {
                const crxk::J* tx = snap.get("text");
                if (tx && tx->t == 's') pageText = WxW(tx->s);
            }
            int words = 1000;
            std::string lang = "English", genre = "nonfiction_classic";
            std::string docId = "web_" + AiSha256Hex16(pageText);
            unsigned long long run = g_sumRun;          // this thread's owner token
            auto cancel = g_sumCancel;                  // this run's own stop flag
            std::thread([gen, run, cancel, docId, pageText, words, lang, genre]() {
                // the worker's guard: an uncaught exception on a detached
                // thread is std::terminate - the run would take the app down
                try {
                    AiSumRun(gen, run, cancel, docId, pageText, 1, 1, words, lang, genre, false);
                } catch (...) {
                    if (run == g_sumRun) g_sumBusy = false;
                    AiEv* se = AiEvMake(AIEV_SUM_ERROR);
                    se->gen = gen;
                    se->a = WxU(L"The run stopped after an internal error.");
                    AiPost(se);
                }
            }).detach();
        }
        break;
    }
    case AIEV_ED_TEXT: {
        std::lock_guard<std::mutex> lk(g_aiMtx);
        g_edAiText = e->a;
        if (g_edAiTextPending) {
            g_edAiTextPending = false;
            AiStartReplyThread(g_aiCurKey, g_edAiPendingInput);
        }
        break;
    }
    case AIEV_EXTRACT_DONE: {
        int what = atoi(e->key.c_str());
        if (what == 1) {
            // notes: the text rides to the model thread
            std::wstring text = WxW(e->a);
            int first = g_range.first, last = g_range.last;
            AiNotesRunModel(text, first, last);
        } else if (what == 2) {
            // the beaker's range test: the tester probe answers what it saw.
            // The run token rides along like any summary thread: a Start
            // pressed mid-test redirects (bumps g_sumRun), and this probe's
            // completion must not un-busy the run that replaced it.
            unsigned long long gen = e->gen;
            unsigned long long run = g_sumRun;
            std::wstring text = WxW(e->a);
            std::thread([gen, run, text]() {
                // the worker's guard: an uncaught exception on a detached
                // thread is std::terminate - the probe would take the app down
                try {
                    if (AiLetters(text) < 50) {
                        // the probe's verdict rides the pump like every other
                        // thread's word: a webview call from this thread died in
                        // the apartment wall and the beaker never reported
                        AiEv* te = AiEvMake(AIEV_SUM_TESTOUT);
                        te->gen = gen;
                        te->a = "The selected pages have no readable text to verify.";
                        te->flag = false;
                        AiPost(te);
                        if (run == g_sumRun) g_sumBusy = false;
                        return;
                    }
                    AiLoadSettings();
                    AiCfg cfg = g_aiCfg;
                    std::string sys = PromptBodyOf("tester_standard");
                    std::string content;
                    AiError err;
                    bool ok = AiChatBuffered(cfg, sys, {}, WxU(text), 2048,
                                             nullptr, true, content, err, nullptr);
                    std::string out = ok ? content : WxU(err.Friendly());
                    AiEv* te = AiEvMake(AIEV_SUM_TESTOUT);
                    te->gen = gen;
                    te->a = out;
                    te->flag = ok;
                    AiPost(te);
                    if (run == g_sumRun) g_sumBusy = false;
                } catch (...) {
                    AiEv* te = AiEvMake(AIEV_SUM_TESTOUT);
                    te->gen = gen;
                    te->a = "The probe stopped after an internal error.";
                    te->flag = false;
                    AiPost(te);
                    if (run == g_sumRun) g_sumBusy = false;
                }
            }).detach();
        } else if (what == 0) {
            // summary: the thread takes the text. The doc's cache key rides
            // the extraction job's own path - DVF() or the index pump's path
            // can point somewhere else by the time the extraction lands, and
            // a digest cached under the wrong document is a digest lost.
            unsigned long long gen = e->gen;
            std::wstring text = WxW(e->a);
            int first = g_range.first, last = g_range.last;
            int words = g_range.words;
            std::string lang = g_range.lang, genre = g_range.genre;
            bool bypass = g_range.bypass;
            std::string docId = AiDocSessionKey(g_range.path);
            unsigned long long run = g_sumRun;          // this thread's owner token
            auto cancel = g_sumCancel;                  // this run's own stop flag
            std::thread([gen, run, cancel, docId, text, first, last, words, lang, genre, bypass]() {
                // the worker's guard: an uncaught exception on a detached
                // thread is std::terminate - the run would take the app down;
                // the page hears a plain error instead
                try {
                    AiSumRun(gen, run, cancel, docId, text, first, last, words, lang, genre, bypass);
                } catch (...) {
                    if (run == g_sumRun) g_sumBusy = false;
                    AiEv* se = AiEvMake(AIEV_SUM_ERROR);
                    se->gen = gen;
                    se->a = WxU(L"The run stopped after an internal error.");
                    AiPost(se);
                }
            }).detach();
        }
        break;
    }
    case AIEV_SUM_DELTA:
        SumPost("{\"cmd\":\"delta\",\"gen\":" + std::to_string(e->gen) +
                ",\"text\":" + WxJsonStr(e->a) + "}");
        break;
    case AIEV_SUM_DONE: {
        int words = 0;
        for (size_t i = 0; i < e->a.size(); i++)
            if (!isspace((unsigned char)e->a[i])) {
                words++;
                while (i < e->a.size() && !isspace((unsigned char)e->a[i])) i++;
            }
        bool fromCache = e->b == "fromCache";
        // the counts ride ALWAYS - a cache hit still fills the page, the user
        // still wants to know how much text is on it; "from cache" shrinks to
        // a suffix and never replaces the numbers again
        std::string status = std::to_string(words) + " words · " +
                             std::to_string(e->a.size()) + " chars" +
                             (fromCache ? " · from cache" : "");
        // the chime's own handoff: a visible popup lets the page play its Web
        // Audio chime (the same rule for cache hits and fresh runs), a folded
        // one gets the native ear - PlaySound answers whether or not any
        // webview renders. The page learns which ear rang through "ping":
        // false mutes its own chime so the room never hears a double.
        bool winVisible = g_sumWnd && IsWindowVisible(g_sumWnd) != FALSE;
        bool currentRun = e->gen == g_sumGen;   // a superseded run's chime
                                                // belongs to no one alive
        if (!winVisible && currentRun &&
            AiGetSetting("summary.ping.enabled", "1") != "0")
            SumPingNative();
        SumPost("{\"cmd\":\"done\",\"gen\":" + std::to_string(e->gen) +
                ",\"text\":" + WxJsonStr(e->a) +
                ",\"status\":" + WxJsonStr(status) +
                ",\"ping\":" + (winVisible ? "true" : "false") +
                ",\"model\":" + WxJsonStr(g_aiCfg.model) + "}");
        // the saved digest rides the CURRENT run's gen: a superseded run's
        // completion (its text already gen-gated away on the page) must not
        // overwrite the digest row with the old range
        if (g_sumMode == 0 && e->gen == g_sumGen) {
            DocState& D = DVF();
            if (!D.path.empty()) {
                std::string docId = AiDocSessionKey(D.path);
                int last = min(g_sumStart + g_sumRange - 1, D.n);
                AiSetSetting("summary.digest." + docId, e->a);
                AiSetSetting("summary.digest." + docId + ".first", std::to_string(g_sumStart));
                AiSetSetting("summary.digest." + docId + ".last", std::to_string(last));
                AiSetSetting("summary.digest." + docId + ".model", WxU(g_aiCfg.model));
                AiSetSetting("summary.start." + docId, std::to_string(g_sumStart));
            }
        }
        break;
    }
    case AIEV_SUM_ERROR:
        SumPost("{\"cmd\":\"error\",\"gen\":" + std::to_string(e->gen) +
                ",\"text\":" + WxJsonStr(e->a) + "}");
        g_sumBusy = false;
        break;
    case AIEV_SUM_NOTEXT:
        SumPost("{\"cmd\":\"notext\",\"gen\":" + std::to_string(e->gen) + "}");
        break;
    case AIEV_SUM_PROGRESS:
        SumPost("{\"cmd\":\"progress\",\"gen\":" + std::to_string(e->gen) +
                ",\"text\":" + WxJsonStr(e->a) + "}");
        break;
    case AIEV_TEST_OUT:
        AiPanelSend("{\"cmd\":\"test\",\"text\":" + WxJsonStr(e->a) + "}");
        break;
    case AIEV_SUM_TESTOUT:   // the beaker's verdict, landed on the UI thread
                             // where the webview call can actually speak
        SumPost("{\"cmd\":\"test-out\",\"gen\":" + std::to_string(e->gen) +
                ",\"text\":" + WxJsonStr(e->a) +
                ",\"ok\":" + (e->flag ? "true" : "false") + "}");
        break;
    case AIEV_SUM_PREFETCH: {   // the buffer's fill: the thread's word comes
                                // home here, on the UI thread, before any
                                // webview hears it. Empty = the fill failed
                                // or found nothing - the page must hear that
                                // too, or its chip hangs on "generating".
        int first = atoi(e->key.c_str());
        bool winVisible = g_sumWnd && IsWindowVisible(g_sumWnd) != FALSE;
        if (!e->a.empty()) {
            if (!winVisible &&
                AiGetSetting("summary.ping.enabled", "1") != "0")
                SumPingNative();   // the fill took real work - the ear rings
                                   // even when the window is folded away
            SumPost("{\"cmd\":\"prefetch-ready\",\"start\":" + std::to_string(first) +
                    ",\"text\":" + WxJsonStr(e->a) +
                    ",\"ping\":" + (winVisible ? "true" : "false") + "}");
        } else {
            SumPost("{\"cmd\":\"prefetch-fail\",\"start\":" + std::to_string(first) + "}");
        }
        break;
    }
    default:
        break;
    }
}

// ---- the mode flip: the reader/editor/browser switches rebind the context ----
static void AiModeChanged() {
    if (!g_aiOn) return;
    AiBindContext();
}
