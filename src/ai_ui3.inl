// ---------------- docsy AI: the panel's message switch + the event pump ----------------
#pragma once

// ---- the connection test (AiSettingsViewModel.TestConnectionAsync) ----
static void AiRunTest(std::string settingsJson) {
    std::thread([settingsJson]() {
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
        // 1. the local Ollama probe (GET /api/version, 5s)
        {
            std::wstring root = c.baseUrl;
            size_t v1 = root.rfind(L"/v1");
            if (v1 != std::wstring::npos) root.resize(v1);
            if (root.empty()) root = L"http://localhost:11434";
            if (AiIsLocalHost(WxU(root))) {
                std::string resp; int status = 0;
                bool ok = AiHttpPostRaw(root + L"/api/version", "", L"", 5000, resp, status, nullptr) &&
                          status >= 200 && status < 300;
                finish(ok ? "\xe2\x9c\x93 Ollama is running" :
                           "\xe2\x9c\x97 Ollama is not reachable on localhost");
            }
        }
        // 2. a real chat completion
        {
            AiError e;
            std::string content;
            bool ok = AiChatBuffered(c, "Reply with just 'OK'", {}, "test", 64, nullptr, false,
                                     content, e, nullptr) && !content.empty();
            finish(ok ? "\xe2\x9c\x93 Chat answered" :
                        ("\xe2\x9c\x97 Chat failed: " + WxU(e.Friendly())));
        }
        // 3. one embedding
        {
            std::vector<std::vector<float>> vecs;
            AiError e;
            bool ok = AiEmbedBatch(c, { "connection test" }, vecs, e, nullptr) &&
                      !vecs.empty() && !vecs[0].empty();
            finish(ok ? "\xe2\x9c\x93 Embeddings ready" :
                        ("\xe2\x9c\x97 Embeddings failed: " + WxU(e.Friendly())));
        }
        AiEv* ev = AiEvMake(AIEV_TEST_OUT);
        ev->a = out;
        AiPost(ev);
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
        AiOverlayLayout();
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);
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
        if (input.empty() || g_aiCurKey.empty()) return;
        AiCtxState* st = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_aiMtx);
            auto it = g_aiCtx.find(g_aiCurKey);
            if (it == g_aiCtx.end()) return;
            st = &it->second;
            if (st->inflight) return;
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
    } else if (t == "notes-gen") {
        const crxk::J* r = m.get("range");
        AiNotesGenerate(r && r->t == 's' ? WxW(r->s) : L"");
    } else if (t == "notes-axo") {
        const crxk::J* f = m.get("first"), * l = m.get("last"), * c = m.get("content");
        if (f && l && c && c->t == 's') {
            DocState& D = DVF();
            bool ok = !D.path.empty() &&
                      AiAxoAppend(D.path, AiMdToHtml(c->s), (int)f->d, (int)l->d);
            AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
            e->a = ok ? WxU(L"Saved to Axo ✓") : WxU(L"Document is not in the database Books or Articles folder. Axo note not saved.");
            e->flag = false;
            AiPost(e);
        }
    } else if (t == "notes-axo-all") {
        DocState& D = DVF();
        bool ok = !D.path.empty() && !g_aiNotesCards.empty();
        if (ok) {
            for (auto& card : g_aiNotesCards) {
                std::string html = AiMdToHtml("**Pages " + std::to_string(card.first) +
                                              " \u2013 " + std::to_string(card.last) +
                                              "**\n\n" + card.content);
                if (!AiAxoAppend(D.path, html, card.first, card.last)) { ok = false; break; }
            }
        }
        AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
        e->a = ok ? WxU(L"Saved all to Axo ✓") :
                    WxU(L"Document is not in the database Books or Articles folder. Axo note not saved.");
        e->flag = false;
        AiPost(e);
    } else if (t == "notes-refresh") {
        // the tab's own state is client-side; nothing to re-sync yet
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
        break;
    case AIEV_NOTES_STATUS:
        AiPanelSend("{\"cmd\":\"notes-status\",\"text\":" + WxJsonStr(e->a) +
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
        AiPanelSend("{\"cmd\":\"notes-cards\",\"cards\":" + e->b + "}");
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
            std::thread([gen, docId, pageText, words, lang, genre]() {
                AiSumRun(gen, docId, pageText, 1, 1, words, lang, genre, false);
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
        } else if (what == 0) {
            // summary: the thread takes the text
            if (g_sumThread.joinable()) g_sumThread.join();
            unsigned long long gen = e->gen;
            std::wstring text = WxW(e->a);
            int first = g_range.first, last = g_range.last;
            int words = g_range.words;
            std::string lang = g_range.lang, genre = g_range.genre;
            bool bypass = g_range.bypass;
            std::string docId = AiDocSessionKey(g_aiIdxPath.empty() ? DVF().path : g_aiIdxPath);
            std::thread([gen, docId, text, first, last, words, lang, genre, bypass]() {
                AiSumRun(gen, docId, text, first, last, words, lang, genre, bypass);
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
        std::string status = fromCache ? "From cache" :
            std::to_string(words) + " words · " + std::to_string(e->a.size()) + " chars";
        SumPost("{\"cmd\":\"done\",\"gen\":" + std::to_string(e->gen) +
                ",\"text\":" + WxJsonStr(e->a) +
                ",\"status\":" + WxJsonStr(status) +
                ",\"model\":" + WxJsonStr(g_aiCfg.model) + "}");
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
    default:
        break;
    }
}

// ---- the mode flip: the reader/editor/browser switches rebind the context ----
static void AiModeChanged() {
    if (!g_aiOn) return;
    AiBindContext();
}
