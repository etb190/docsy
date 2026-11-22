// ---------------- docsy AI: panel messages, workers, summary window ----------------
// The second half: the panel's postMessage switch, the index/semantic/notes
// pumps, the Page-Summary navigator window (Features/Summary/SummaryWindow +
// WebSummaryWindow in one frame), the citation jump, and the Axo notes save.
#pragma once

static void AiPanelMessage(const wchar_t* json);
static void SumPanelMessage(const wchar_t* json);
static LRESULT CALLBACK SumWndProc(HWND h, UINT m, WPARAM wp, LPARAM lp);

// ---- the page text builder: [p. N] anchored, running paragraphs ----
static std::wstring AiPageTextFrom(const DocState& D, int page) {
    TxtPage* pt = TtFor(const_cast<DocState&>(D), page);
    if (!pt || pt->cells.empty()) return L"";
    std::map<int, std::wstring> wt;
    std::map<int, std::array<float, 4>> wbox;
    for (auto& c : pt->cells) {
        auto& bx = wbox[c.word];
        if (bx[2] == 0 && bx[3] == 0) bx = { c.l, c.t, c.r, c.b };
        else { bx[0] = min(bx[0], c.l); bx[1] = min(bx[1], c.t);
               bx[2] = max(bx[2], c.r); bx[3] = max(bx[3], c.b); }
        wt[c.word] += c.text;
    }
    std::map<int, std::vector<int>> byLine;
    for (auto& c : pt->cells) byLine[c.line].push_back(c.word);
    std::vector<std::pair<int, std::vector<int>>> lines(byLine.begin(), byLine.end());
    std::sort(lines.begin(), lines.end(), [&](auto& a, auto& b) {
        return wbox[a.second.front()][1] < wbox[b.second.front()][1];
    });
    std::vector<float> lh;
    for (auto& L : lines) {
        float top = 1e9f, bottom = -1e9f;
        for (int w : L.second) { top = min(top, wbox[w][1]); bottom = max(bottom, wbox[w][3]); }
        if (bottom > top) lh.push_back(bottom - top);
    }
    std::sort(lh.begin(), lh.end());
    float medLine = lh.empty() ? 0 : lh[lh.size() / 2];
    std::vector<float> whs;
    for (auto& kv : wbox) whs.push_back(kv.second[3] - kv.second[1]);
    std::sort(whs.begin(), whs.end());
    float medH = whs.empty() ? 0 : whs[whs.size() / 2];
    std::wstring out;
    float prevBottom = -1;
    bool headingOpen = false;
    for (auto& L : lines) {
        float top = 1e9f, bottom = -1e9f;
        std::wstring joined;
        for (int w : L.second) {
            top = min(top, wbox[w][1]); bottom = max(bottom, wbox[w][3]);
            if (!joined.empty()) joined += L" ";
            joined += wt[w];
        }
        bool heading = joined.size() <= 60 && L.second.size() <= 8 && medH > 0 &&
                       (bottom - top) >= 1.2f * medH;
        if (heading && !headingOpen) out += L"\n\n# ";
        else if (prevBottom >= 0 && medLine > 0 && (top - prevBottom) > 1.6f * medLine) out += L"\n\n";
        else if (!out.empty()) out += L"\n";
        out += joined;
        headingOpen = heading;
        prevBottom = bottom;
    }
    return out;
}
static int AiLetters(const std::wstring& s) {
    int n = 0;
    for (wchar_t c : s) if (iswalpha(c)) n++;
    return n;
}

// ---- the range extraction pump (one OCR page per tick) ----
struct AiRangeJob {
    bool active = false;
    std::wstring path;
    int first = 0, last = 0, cur = 0;
    std::wstring text;
    int what = 0;                        // 0 summary, 1 notes
    unsigned long long gen = 0;
    int words = 1000;
    std::string lang = "English", genre = "nonfiction_classic";
    bool bypass = false;
};
static AiRangeJob g_range;

static void AiRangeStart(const std::wstring& path, int first, int last, int what,
                         unsigned long long gen) {
    g_range.active = true;
    g_range.path = path;
    g_range.first = first; g_range.last = last; g_range.cur = first;
    g_range.text.clear();
    g_range.what = what;
    g_range.gen = gen;
    SetTimer(g.main, 4714, 30, nullptr);
}
static void AiRangeTick() {
    if (!g_range.active) { KillTimer(g.main, 4714); return; }
    DocState& D = DVF();
    if (D.path.empty() || D.path != g_range.path) {
        g_range.active = false;
        KillTimer(g.main, 4714);
        return;
    }
    int p = g_range.cur;
    if (p > g_range.last) {
        g_range.active = false;
        KillTimer(g.main, 4714);
        AiEv* e = AiEvMake(AIEV_EXTRACT_DONE);
        e->a = WxU(g_range.text);
        e->gen = g_range.gen;
        e->key = std::to_string(g_range.what);
        AiPost(e);
        return;
    }
    std::wstring t = AiPageTextFrom(D, p);
    if (!t.empty()) g_range.text += L"[p. " + std::to_wstring(p + 1) + L"]\n" + t + L"\n\n";
    g_range.cur++;
    if (g_range.what == 1 && (p & 3) == 0) {
        AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
        e->flag = true;
        wchar_t b[160];
        _snwprintf_s(b, _TRUNCATE, L"Extracting page %d of %d\u2026", p - g_range.first + 1,
                     g_range.last - g_range.first + 1);
        e->a = WxU(b);
        AiPost(e);
    }
}

// ---- the document index pump (OCR one page per tick) ----
static void AiIndexKickoff(const std::wstring& path) {
    if (g_aiIndexing || path.empty()) return;
    AiLoadSettings();
    std::string docId = AiDocSessionKey(path);
    AiDocIndex ix;
    if (AiIndexLoad(docId, ix) && !ix.chunks.empty()) {
        ix.path = path;
        g_aiIndex = std::move(ix);
        g_aiIndexReady = true;
        AiPanelSend(WxW(AiStatusJson("status")));
        return;
    }
    g_aiIndexing = true;
    g_aiIndexReady = false;
    g_aiIdxPage = 0;
    g_aiIdxKey = docId;
    g_aiIdxPath = path;
    if (g_aiIdxChunk) { delete g_aiIdxChunk; g_aiIdxChunk = nullptr; }
    g_aiIdxChunk = new AiChunker();
    g_aiIndex = AiDocIndex();
    g_aiIndex.docId = docId;
    g_aiIndex.path = path;
    g_aiIndex.pageCount = DVF().n;
    g_aiIndex.contentHash = AiFileHash(path);
    AiPanelSend(WxW(AiStatusJson("status")));
    SetTimer(g.main, 4713, 30, nullptr);
}
static void AiIndexTick() {
    if (!g_aiIndexing) { KillTimer(g.main, 4713); return; }
    DocState& D = DVF();
    if (D.path.empty() || D.path != g_aiIdxPath || D.n <= 0) {
        g_aiIndexing = false;
        if (g_aiIdxChunk) { delete g_aiIdxChunk; g_aiIdxChunk = nullptr; }
        KillTimer(g.main, 4713);
        AiPanelSend(WxW(AiStatusJson("status")));
        return;
    }
    int p = g_aiIdxPage;
    if (p >= D.n) {
        g_aiIdxChunk->Finish();
        g_aiIndex.chunks = std::move(g_aiIdxChunk->done);
        delete g_aiIdxChunk; g_aiIdxChunk = nullptr;
        g_aiIndexing = false;
        KillTimer(g.main, 4713);
        g_aiIndexReady = !g_aiIndex.chunks.empty();
        AiIndexSave(g_aiIndex);
        std::string key = g_aiIdxKey;
        std::string queued;
        {
            std::lock_guard<std::mutex> lk(g_aiMtx);
            auto it = g_aiCtx.find(key);
            if (it != g_aiCtx.end() && !it->second.pendingInput.empty()) {
                queued = it->second.pendingInput;
                it->second.pendingInput.clear();
            }
        }
        AiPanelSend(WxW(AiStatusJson("status")));
        if (!queued.empty()) AiStartReplyThread(key, queued);
        return;
    }
    TxtPage* pt = TtFor(D, p);
    if (pt && !pt->cells.empty()) {
        std::vector<AiWordIn> words;
        AiPageWords(*pt, p, words);
        for (auto& w : words) g_aiIdxChunk->Append(w);
    }
    g_aiIdxPage++;
    AiPanelSend(WxW(AiStatusJson("status")));
}

// ---- the semantic research pass (the embedding thread) ----
static void AiSemanticStart() {
    if (!g_aiIndexReady || g_aiSemActive) return;
    AiLoadSettings();
    g_aiSemActive = true;
    g_aiSemCancel = false;
    g_aiSemStatus = L"Ready — building semantic search…";
    AiPanelSend(WxW(AiStatusJson("status")));
    AiDocIndex ix = g_aiIndex;
    AiCfg cfg = g_aiCfg;
    std::wstring prefixKey = cfg.embDocPrefix + L"\x01" + cfg.embQueryPrefix;
    std::string model = WxU(cfg.embeddingModel.empty() ? L"embeddinggemma:latest"
                                                       : cfg.embeddingModel);
    std::thread([ix, cfg, prefixKey, model]() mutable {
        bool upToDate = (ix.embModel == model && ix.prefixKey == WxU(prefixKey) &&
                         !ix.vectors.empty() &&
                         ix.vectors.size() == ix.chunks.size());
        if (!upToDate) {
            size_t total = ix.chunks.size();
            ix.vectors.assign(total, {});
            ix.dim = 0;
            const size_t B = 256;
            for (size_t b0 = 0; b0 < total; b0 += B) {
                if (g_aiSemCancel.load()) { g_aiSemActive = false; return; }
                size_t b1 = min(b0 + B, total);
                std::vector<std::string> inputs;
                for (size_t i = b0; i < b1; i++) {
                    std::string inp = WxU(cfg.embDocPrefix) + ix.chunks[i].text;
                    if (inp.size() > 1200) inp.resize(1200);
                    inputs.push_back(inp);
                }
                std::vector<std::vector<float>> vecs;
                AiError e;
                bool ok = AiEmbedBatch(cfg, inputs, vecs, e, &g_aiSemCancel);
                if (!ok && vecs.empty() && !g_aiSemCancel.load())
                    ok = AiEmbedBatch(cfg, inputs, vecs, e, &g_aiSemCancel);
                if (!ok || vecs.size() != b1 - b0) {
                    g_aiSemActive = false;
                    if (g_aiSemCancel.load()) return;
                    AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                    if (e.cat == AI_ERR_MODEL_NOT_FOUND) {
                        ev->a = WxU(L"Embedding model " + e.model +
                                    L" not found. Run: ollama pull " + e.model +
                                    L" — using keyword search only.");
                    } else {
                        ev->a = WxU(L"Semantic search is unavailable right now — using keyword search only.");
                    }
                    AiPost(ev);
                    return;
                }
                if (ix.dim == 0) ix.dim = (int)vecs[0].size();
                for (size_t i = b0; i < b1; i++) ix.vectors[i] = vecs[i - b0];
                AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                wchar_t b[128];
                _snwprintf_s(b, _TRUNCATE, L"Building semantic search… %zu/%zu", b1, total);
                ev->a = WxU(b);
                AiPost(ev);
            }
            ix.embModel = model;
            ix.prefixKey = WxU(prefixKey);
            AiIndexSave(ix);
        }
        g_aiSemActive = false;
        g_aiSemReadyFlag = true;
        AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
        ev->a = WxU(L"Semantic search ready.");
        AiPost(ev);
    }).detach();
}
static void AiSemanticToggle() {
    if (g_aiSemActive) { g_aiSemCancel = true; return; }
    if (g_aiSemReadyFlag) g_aiSemReadyFlag = false;
    AiSemanticStart();
}

// ---- notes generation ----
static void AiNotesGenerate(const std::wstring& rangeText) {
    if (g_aiNotesBusy) return;
    DocState& D = DVF();
    if (D.path.empty()) {
        AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
        e->a = WxU(L"Open a PDF first, then generate notes for its pages.");
        e->flag = false;
        AiPost(e);
        return;
    }
    int first = 1, last = D.n;
    std::wstring t = rangeText;
    size_t dash = t.find_first_of(L"-\u2013");
    if (!t.empty()) {
        if (dash != std::wstring::npos) {
            first = _wtoi(t.substr(0, dash).c_str());
            last = _wtoi(t.substr(dash + 1).c_str());
        } else {
            first = 1;
            last = _wtoi(t.c_str());
        }
        first = max(1, min(first, D.n));
        last = max(first, min(last, D.n));
    }
    g_aiNotesBusy = true;
    g_aiNotesCancel = false;
    AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
    wchar_t b[160];
    _snwprintf_s(b, _TRUNCATE, L"Generating notes for pages %d to %d\u2026", first, last);
    e->a = WxU(b); e->flag = true;
    AiPost(e);
    unsigned long long gen = ++g_sumGen;
    AiRangeStart(D.path, first, last, 1, gen);
}
static void AiNotesRunModel(std::wstring rangeText, int first, int last) {
    AiLoadSettings();
    AiCfg cfg = g_aiCfg;
    auto blocks = AiNoteBlocks(first, last);
    if (AiLetters(rangeText) < 250) {
        AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
        e->a = WxU(L"the selected pages have no usable text layer (a scanned book without OCR?)");
        e->flag = false;
        AiPost(e);
        g_aiNotesBusy = false;
        return;
    }
    std::string user = "Blocks to cover (one card per block):\n";
    for (auto& b : blocks)
        user += "- Pages " + std::to_string(b.first) + "-" + std::to_string(b.second) + "\n";
    user += "\n=== TEXT (pages " + std::to_string(first) + "-" + std::to_string(last) + ") ===\n";
    user += WxU(rangeText);
    std::string sys = PromptBodyOf("notes_standard");
    int maxTok = max(cfg.maxTokens, 2000 + 1200 * (int)blocks.size());
    if (!AiIsLocalHost(WxU(cfg.baseUrl)) && maxTok > 8192) maxTok = 8192;
    std::thread([user, sys, maxTok, first, last]() {
        AiError err;
        std::string reply;
        bool ok = AiChatBuffered(g_aiCfg, sys, {}, user, maxTok, nullptr, true, reply, err,
                                 &g_aiNotesCancel);
        g_aiNotesBusy = false;
        if (g_aiNotesCancel.load()) {
            AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
            e->a = ""; e->flag = false;
            AiPost(e);
            return;
        }
        if (!ok) {
            AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
            std::wstring f = err.Friendly();
            if (f.size() > 160) f.resize(160);
            e->a = WxU(L"Notes failed: " + f);
            e->flag = false;
            AiPost(e);
            return;
        }
        std::vector<AiNoteCard> cards = AiParseNoteCards(reply, first, last);
        AiEv* e = AiEvMake(AIEV_NOTES_CARDS);
        std::string sb = "[";
        for (size_t i = 0; i < cards.size(); i++) {
            if (i) sb += ",";
            sb += "{\"first\":" + std::to_string(cards[i].first) +
                  ",\"last\":" + std::to_string(cards[i].last) +
                  ",\"content\":" + WxJsonStr(WxU(cards[i].content)) + "}";
        }
        sb += "]";
        e->b = sb;
        AiPost(e);
    }).detach();
}

// ---- the Axo notes save (AxoNotesService) ----
static std::wstring AiAxoRootFor(const std::wstring& path) {
    std::wstring low = path;
    for (auto& c : low) c = (wchar_t)towlower(c);
    const wchar_t* roots[] = { L"c:\\users\\pc\\desktop\\database\\books",
                               L"c:\\users\\pc\\desktop\\database\\articles" };
    for (const wchar_t* r : roots) {
        std::wstring rr = r;
        if (low.rfind(rr, 0) == 0) {
            size_t n = wcslen(r);
            if (low.size() > n && low[n] != L'\\' && low[n] != L'/') continue;
            return std::wstring(r);
        }
    }
    return L"";
}
static std::string AiMdToHtml(const std::string& md) {
    std::string out;
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : md) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur += c; }
        lines.push_back(cur);
    }
    auto escAll = [](std::string s) {
        s = std::regex_replace(s, std::regex("&"), "&amp;");
        s = std::regex_replace(s, std::regex("<"), "&lt;");
        s = std::regex_replace(s, std::regex(">"), "&gt;");
        s = std::regex_replace(s, std::regex("\\*\\*([^*]+)\\*\\*"), "<strong>$1</strong>");
        s = std::regex_replace(s, std::regex("\\*([^*]+)\\*"), "<em>$1</em>");
        return s;
    };
    bool inList = false, para = false;
    auto flushPara = [&]() { if (para) { out += "</p>"; para = false; } };
    auto flushList = [&]() { if (inList) { out += "</ul>"; inList = false; } };
    for (auto& ln : lines) {
        std::string t = ln;
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
        if (t.empty()) { flushPara(); flushList(); continue; }
        if (t.rfind("- ", 0) == 0 || t.rfind("* ", 0) == 0) {
            flushPara();
            if (!inList) { flushList(); out += "<ul>"; inList = true; }
            out += "<li>" + escAll(t.substr(2)) + "</li>";
            continue;
        }
        if (!t.empty() && t[0] == '#') {
            size_t h = t.find_first_not_of("#");
            flushPara(); flushList();
            std::string txt = t.substr(h == std::string::npos ? t.size() : h);
            while (!txt.empty() && txt[0] == ' ') txt.erase(0, 1);
            out += "<p><strong>" + escAll(txt) + "</strong></p>";
            continue;
        }
        flushList();
        if (!para) { out += "<p>"; para = true; }
        else out += "<br />";
        out += escAll(t);
    }
    flushPara(); flushList();
    return out;
}
static bool AiAxoAppend(const std::wstring& docPath, const std::string& html,
                        int first, int last) {
    std::wstring root = AiAxoRootFor(docPath);
    if (root.empty()) return false;
    std::wstring dir = root + L"\\Notes";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring title = docPath;
    size_t sl = title.find_last_of(L"\\/");
    if (sl != std::wstring::npos) title = title.substr(sl + 1);
    size_t dot = title.rfind(L'.');
    if (dot != std::wstring::npos) title.resize(dot);
    std::wstring file = dir + L"\\" + title + L".json";
    std::string s;
    WxReadFile(file, s);
    crxk::J arr;
    if (!s.empty() && crxk::JParse(s, arr) && arr.t == 'a') {} else { arr.t = 'a'; }
    crxk::J item; item.t = 'o';
    crxk::J idv; idv.t = 's'; idv.s = std::to_string((long long)GetTickCount64());
    item.o.push_back({ "id", idv });
    crxk::J cv; cv.t = 's'; cv.s = html;
    item.o.push_back({ "content", cv });
    crxk::J s1; s1.t = 's'; s1.s = std::to_string(first);
    item.o.push_back({ "startPage", s1 });
    crxk::J s2; s2.t = 's'; s2.s = std::to_string(last);
    item.o.push_back({ "endPage", s2 });
    crxk::J tv; tv.t = 's'; tv.s = "general";
    item.o.push_back({ "type", tv });
    SYSTEMTIME st; GetSystemTime(&st);
    wchar_t tsb[64];
    _snwprintf_s(tsb, _TRUNCATE, L"%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    crxk::J tsv; tsv.t = 's'; tsv.s = WxU(tsb);
    item.o.push_back({ "timestamp", tsv });
    arr.a.push_back(item);
    std::string out;
    WxJsonEmit(arr, out);
    std::wstring tmp = file + L".tmp";
    if (!WxWriteFile(tmp, (const unsigned char*)out.data(), out.size())) return false;
    DeleteFileW(file.c_str());
    return MoveFileW(tmp.c_str(), file.c_str()) != 0;
}

// ---- the citation jump: the PDF page + the highlight ----
struct AiHighlight {
    int gen = 0;
    int page = -1;
    std::vector<FRect> rects;
};
static AiHighlight g_aiHi;
static void AiNavigateToSource(int pageIndex, const std::vector<FRect>& rects) {
    DocState& D = DVF();
    if (pageIndex < 0 || pageIndex >= D.n) {
        g.statusHoldText = L"The quoted passage could not be found in this document.";
        g.statusHoldUntil = GetTickCount64() + 4000;
        StatusRepaint();
        return;
    }
    g_aiHi.gen++;
    g_aiHi.page = pageIndex;
    g_aiHi.rects = rects;
    bool b = FocusB();
    if (D.vm == 1 && pageIndex != D.cur) SetPage(pageIndex);
    int row = std::clamp(pageIndex / PerRow(D), 0, max(0, (int)D.rh.size() - 1));
    float cy = 0;
    if (!rects.empty()) cy = rects[0].y + rects[0].h * 0.5f;
    int yDoc = PageTop(D, pageIndex) + (D.rh[row] - PH(D, pageIndex)) / 2 +
               (int)(cy * D.zoom * g.dpi);
    ScrollPane(b, D.sx, yDoc - g.ch / 2);
    InvalidateRect(g.view, nullptr, FALSE);
}
static void AiPaintPage(HDC dc, const DocState& D, int page, int px, int py) {
    if (g_aiHi.page != page || g_aiHi.rects.empty()) return;
    float k = (float)(D.zoom * g.dpi);
    Gdiplus::Graphics gs(dc);
    Gdiplus::SolidBrush br(Gdiplus::Color(88, 100, 180, 255));
    for (auto& r : g_aiHi.rects)
        gs.FillRectangle(&br, Gdiplus::RectF(px + r.x * k, py + r.y * k,
                                             max(r.w * k, 3.0f), max(r.h * k, 3.0f)));
}

// ---- the web citation jump: find + select + scroll on the live page ----
static void AiWebCiteJump(const std::wstring& quote) {
    if (g_webOn && g_wtCur >= 0 && g_wtCur < (int)g_wt.size() && g_wt[g_wtCur].web) {
        std::wstring q;
        for (wchar_t c : quote) {
            if (c == L'\\') q += L"\\\\";
            else if (c == L'"' || c == L'\'') q += L"\\'" ;
            else if (c == L'\n' || c == L'\r') q += L" ";
            else q += c;
        }
        std::wstring js =
            L"(function(){var q=\"" + q + L"\".toLowerCase();function norm(s){return (s||'').replace(/[\\s\\u00A0]+/g,' ');}"
            L"var els=document.querySelectorAll('p,h1,h2,h3,h4,li,td,blockquote');"
            L"var probe=q.length>80?q.substr(0,80):q;var target=null;"
            L"for(var i=0;i<els.length;i++){var t=norm(els[i].textContent).toLowerCase();"
            L"if(t.length>10&&t.indexOf(probe)>=0){target=els[i];break;}}"
            L"if(target){target.scrollIntoView({behavior:'smooth',block:'center'});"
            L"try{var r=document.createRange();r.selectNodeContents(target);var s=window.getSelection();"
            L"s.removeAllRanges();s.addRange(r);}catch(e){}return JSON.stringify({ok:true});}"
            L"return JSON.stringify({ok:false});})()";
        g_wt[g_wtCur].web->ExecuteScript(js.c_str(), nullptr);
    }
}

// ---- the web page text extraction (the browser sidechat's reader) ----
static void AiWebExtract(unsigned long long cbId) {
    if (!(g_webOn && g_wtCur >= 0 && g_wtCur < (int)g_wt.size() && g_wt[g_wtCur].web)) {
        AiEv* e = AiEvMake(AIEV_WEB_TEXT);
        e->a = ""; e->gen = cbId;
        AiPost(e);
        return;
    }
    std::wstring js =
        L"(function(){"
        L"var art=document.querySelector('article')||document.querySelector('[role=main]')||"
        L"document.querySelector('main')||document.body;"
        L"var mode='readability';var t='';"
        L"if(art&&art!==document.body){t=art.innerText||'';}else{t=document.body?document.body.innerText:'';mode='raw';}"
        L"return JSON.stringify({ok:!!t&&t.trim().length>0,mode:mode,"
        L"url:location.href,title:document.title||'',byline:'',site:location.hostname||'',text:(t||'').trim()});"
        L"})()";
    auto* web = g_wt[g_wtCur].web.Get();
    auto cb = Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [cbId](HRESULT hr, LPCWSTR result) -> HRESULT {
            std::string text;
            if (SUCCEEDED(hr) && result) {
                crxk::J j;
                std::string outer = WxU(result);
                // the engine returns the script's string as a JSON string literal:
                // both the double-encoded and the object shape are accepted
                crxk::J inner;
                if (crxk::JParse(outer, inner) && inner.t == 's') {
                    if (crxk::JParse(inner.s, j) && j.t == 'o') {}
                    else j = crxk::J();
                } else if (crxk::JParse(outer, j) && j.t == 'o') {}
                if (j.t == 'o') {
                    const crxk::J* ok = j.get("ok");
                    const crxk::J* tx = j.get("text");
                    const crxk::J* ti = j.get("title");
                    const crxk::J* si = j.get("site");
                    const crxk::J* by = j.get("byline");
                    const crxk::J* mo = j.get("mode");
                    if (ok && ok->b && tx && tx->t == 's') {
                        // rebuild the snapshot shape the VM expects
                        std::string snap = "{\"ok\":true,\"text\":" + WxJsonStr(WxW(tx->s)) +
                            ",\"title\":" + (ti && ti->t == 's' ? WxJsonStr(WxW(ti->s)) : "\"\"") +
                            ",\"site\":" + (si && si->t == 's' ? WxJsonStr(WxW(si->s)) : "\"\"") +
                            ",\"byline\":" + (by && by->t == 's' ? WxJsonStr(WxW(by->s)) : "\"\"") +
                            ",\"mode\":" + (mo && mo->t == 's' ? WxJsonStr(WxW(mo->s)) : "\"readability\"") + "}";
                        text = snap;
                    }
                }
            }
            AiEv* e = AiEvMake(AIEV_WEB_TEXT);
            e->a = text;
            e->gen = cbId;
            AiPost(e);
            return S_OK;
        });
    web->ExecuteScript(js.c_str(), cb.Get());
}

// ---- the summarizer window ----
static void SumSendInit();
static void SumOpen(int mode);

static void SumPost(const std::string& j) {
    if (g_sumWeb) g_sumWeb->PostWebMessageAsJson(WxW(j).c_str());
}
static void SumSendInit() {
    AiLoadSettings();
    std::string j = "{\"cmd\":\"init\",\"mode\":\"" + std::string(g_sumMode ? "web" : "doc") + "\"";
    if (g_sumMode == 0) {
        DocState& D = DVF();
        j += ",\"pageCount\":" + std::to_string(D.n);
        j += ",\"start\":" + std::to_string(g_sumStart);
        j += ",\"rangePages\":" + std::to_string(g_sumRange);
        j += ",\"words\":1000,\"lang\":\"English\",\"genre\":\"nonfiction_classic\"";
    } else {
        j += ",\"words\":1000,\"lang\":\"English\"";
        j += ",\"prompts\":[{\"id\":\"web_nonfiction_classic\",\"title\":\"Nonfiction\"}]";
    }
    j += ",\"model\":" + WxJsonStr(g_aiCfg.model);
    j += "}";
    SumPost(j);
}
static void SumEnsure() {
    if (g_sumCtr || g_sumBuilding) return;
    WebEnsureEnv();
    if (!g_wvEnv) return;
    g_sumBuilding = true;
    g_wvEnv->CreateCoreWebView2Controller(
        g_sumWnd,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                g_sumBuilding = false;
                if (FAILED(hr) || !c) return S_OK;
                g_sumCtr = c;
                c->get_CoreWebView2(&g_sumWeb);
                wrl::ComPtr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2)
                    c2->put_DefaultBackgroundColor({ 255, 0x1c, 0x1c, 0x1c });
                EventRegistrationToken tk;
                g_sumWeb->add_WebMessageReceived(
                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                            LPWSTR msg = nullptr;
                            if (SUCCEEDED(a->get_WebMessageAsJson(&msg)) && msg) {
                                SumPanelMessage(msg);
                                CoTaskMemFree(msg);
                            }
                            return S_OK;
                        }).Get(), &tk);
                std::wstring html = AiSumHtml();
                if (!html.empty()) g_sumWeb->NavigateToString(html.c_str());
                RECT r; GetClientRect(g_sumWnd, &r);
                c->put_Bounds(r);
                return S_OK;
            }).Get());
}
static void SumToggle(int mode) { SumOpen(mode); }
static void SumOpen(int mode) {
    if (g_sumWnd) {
        if (g_sumMode != mode) {
            g_sumMode = mode;
            if (g_sumCtr) { g_sumCtr->Close(); g_sumCtr = nullptr; g_sumWeb = nullptr; g_sumReady = false; }
            if (g_sumMode == 1) g_sumReady = true;   // web init needs no book
        }
        ShowWindow(g_sumWnd, SW_SHOWNOACTIVATE);
        SetForegroundWindow(g_sumWnd);
        if (g_sumReady) SumSendInit();
        return;
    }
    g_sumMode = mode;
    static bool cls = false;
    if (!cls) {
        cls = true;
        WNDCLASSEXW wc{ sizeof wc };
        wc.hInstance = g.hinst;
        wc.lpfnWndProc = SumWndProc;
        wc.lpszClassName = L"docsy_sum";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(0x1c, 0x1c, 0x1c));
        RegisterClassExW(&wc);
    }
    int w = (int)(560 * g.dpi), h = (int)(630 * g.dpi);
    RECT rc{ 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_sumWnd = CreateWindowExW(0, L"docsy_sum", L"Page Summary",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               rc.right - rc.left, rc.bottom - rc.top,
                               g.main, nullptr, g.hinst, nullptr);
    if (g_sumMode == 1) g_sumReady = true;
    SumEnsure();
}

// ---- the summary's generation thread ----
static void AiSumRun(unsigned long long gen, const std::string& docId,
                     std::wstring rangeText, int first, int last,
                     int words, std::string lang, std::string genre, bool bypass) {
    AiLoadSettings();
    AiCfg cfg = g_aiCfg;
    std::wstring bodyW = std::regex_replace(rangeText,
        std::wregex(L"\\[{1,2}p\\.\\s*\\d+\\]{1,2}"), L"");
    if (AiLetters(bodyW) < 250) {
        AiEv* e = AiEvMake(AIEV_SUM_NOTEXT);
        e->gen = gen;
        AiPost(e);                       // the panel learns on the pump's thread
        g_sumBusy = false;
        return;
    }
    std::string hash = [&] {
        unsigned char sha[32];
        crxk::Sha256((const unsigned char*)rangeText.data(),
                     rangeText.size() * sizeof(wchar_t), sha);
        static const char* hx = "0123456789abcdef";
        std::string o(64, '0');
        for (int i = 0; i < 32; i++) { o[i * 2] = hx[sha[i] >> 4]; o[i * 2 + 1] = hx[sha[i] & 15]; }
        return o;
    }();
    std::string model = WxU(cfg.model);
    std::string variant = lang + ":" + std::to_string(words) + ":" + genre;
    std::string cached;
    if (!bypass && AiCacheGet(docId, first, last, model, variant, hash, cached)) {
        AiEv* e = AiEvMake(AIEV_SUM_DONE);
        e->gen = gen;
        e->a = WxU(cached);
        e->b = "fromCache";
        AiPost(e);
        g_sumBusy = false;
        return;
    }
    std::string sys = PromptBodyOf(genre.c_str());
    if (sys.empty()) sys = PromptBodyOf("nonfiction_classic");
    {
        std::string ph = std::to_string(words);
        size_t p;
        while ((p = sys.find("{words}")) != std::string::npos) sys.replace(p, 7, ph);
        while ((p = sys.find("{language}")) != std::string::npos) sys.replace(p, 10, lang);
    }
    int budget = max(cfg.maxTokens, max(10000, 3000 + 4 * words));
    if (!AiIsLocalHost(WxU(cfg.baseUrl)) && budget > 16384) budget = 16384;
    std::string user = WxU(rangeText);
    AiError err;
    std::string full;
    bool ok = AiChatStream(cfg, sys, {}, user, budget, nullptr, true,
                           [&](const std::string&) {
                               AiEv* e = AiEvMake(AIEV_SUM_DELTA);
                               e->gen = gen;
                               e->a = WxU(AiExtractStreamingAnswer(full));
                               AiPost(e);
                           }, full, err, &g_sumCancel);
    if (g_sumCancel.load()) {
        AiEv* e = AiEvMake(AIEV_SUM_PROGRESS);
        e->gen = gen; e->a = WxU(L"Stopped");
        AiPost(e);
        g_sumBusy = false;
        return;
    }
    if (!ok || AiExtractStreamingAnswer(full).empty()) {
        // one quiet buffered retry at twice the budget
        std::string content;
        AiError e2;
        if (AiChatBuffered(cfg, sys, {}, user, budget * 2, nullptr, true, content, e2,
                           &g_sumCancel) && !content.empty())
            full = content;
        else {
            AiEv* e = AiEvMake(AIEV_SUM_ERROR);
            e->gen = gen;
            e->a = WxU(err.Friendly());
            AiPost(e);
            g_sumBusy = false;
            return;
        }
    }
    std::string answer = AiExtractStreamingAnswer(full);
    if (answer.empty()) answer = AiParseStructured(AiStripThinkSpans(full)).answer;
    if (answer.empty()) answer = full;
    std::string low = answer;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    if (low.find("no text provided") == std::string::npos &&
        low.find("provide the page content") == std::string::npos &&
        low.find("cannot create a reading digest") == std::string::npos)
        AiCachePut(docId, first, last, model, variant, hash, answer);
    AiEv* e = AiEvMake(AIEV_SUM_DONE);
    e->gen = gen;
    e->a = WxU(answer);
    e->b = "gen";
    AiPost(e);
    g_sumBusy = false;
}

// ---- the summarizer panel's messages ----
static void SumPanelMessage(const wchar_t* json) {
    std::string u8 = WxU(json);
    crxk::J m;
    if (!crxk::JParse(u8, m) || m.t != 'o') return;
    const crxk::J* typ = m.get("type");
    if (!typ || typ->t != 's') return;
    std::string t = typ->s;
    if (t == "ready") {
        g_sumReady = true;
        SumSendInit();
    } else if (t == "start") {
        if (g_sumBusy) return;
        const crxk::J* g = m.get("gen"), * w = m.get("words"), * st = m.get("start"),
            * rg = m.get("range"), * lg = m.get("lang"), * gr = m.get("genre"), * bp = m.get("bypass");
        unsigned long long gen = g ? (unsigned long long)g->d : ++g_sumGen;
        int words = w ? (int)w->d : 1000;
        std::string lang = lg && lg->t == 's' ? lg->s : "English";
        std::string genre = gr && gr->t == 's' ? gr->s : "nonfiction_classic";
        bool bypass = bp && bp->b;
        g_sumBusy = true;
        g_sumCancel = false;
        if (g_sumMode == 0) {
            if (st && st->t == 'd') g_sumStart = max(1, (int)st->d);
            if (rg && rg->t == 'd') g_sumRange = max(1, (int)rg->d);
            DocState& D = DVF();
            if (D.path.empty()) {
                AiEv* e = AiEvMake(AIEV_SUM_ERROR);
                e->gen = gen;
                e->a = WxU(L"Open a PDF first, then summarize its pages.");
                AiPost(e);
                g_sumBusy = false;
                return;
            }
            int last = min(g_sumStart + g_sumRange - 1, D.n);
            AiRangeStart(D.path, g_sumStart, last, 0, gen);
            g_range.words = words;
            g_range.lang = lang;
            g_range.genre = genre;
            g_range.bypass = bypass;
        } else {
            // the live page: one extraction, then the thread
            g_sumWebPending = gen;
            AiWebExtract(gen);
        }
    } else if (t == "reset") {
        g_sumCancel = true;
        g_sumBusy = false;
    } else if (t == "page") {
        const crxk::J* p = m.get("page");
        if (p && p->t == 'd' && g_sumMode == 0) {
            int pg = max(1, min((int)p->d, DVF().n));
            SetPage(pg - 1);
        }
    } else if (t == "range-changed") {
        const crxk::J* p = m.get("pages");
        if (p && p->t == 'd') g_sumRange = max(1, (int)p->d);
    }
}
static LRESULT CALLBACK SumWndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE:
        if (g_sumCtr) {
            RECT r; GetClientRect(h, &r);
            g_sumCtr->put_Bounds(r);
        }
        return 0;
    case WM_DESTROY:
        g_sumCancel = true;
        g_sumWnd = nullptr;
        g_sumCtr = nullptr; g_sumWeb = nullptr; g_sumReady = false;
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
