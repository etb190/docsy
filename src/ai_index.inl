// ---------------- docsy AI: the retrieval + memory layer ----------------
// Session keys, quote location, the document chunker, the persisted index,
// the hybrid retriever, the chat-history store, the summary cache and the
// notes recall logic - DocumentChunker.cs, VectorIndex.cs, HybridRetriever.cs,
// QuoteLocator.cs, WebChat.cs, ChatHistoryStore.cs, SummaryCache.cs and
// NotesRecallLogic.cs condensed into docsy's containers. No SQLite: the index
// rides a JSON sidecar + a float32 vector file per document, the history and
// the cache ride JSON files, all under %LocalAppData%\docsy\AI.
#pragma once

// ---- session keys (doc_/wt_/ed_ - WebChat/EditorChat/DocumentIndexer) ----
static std::string AiSha256Hex16(const std::wstring& s) {
    // the doc identity: SHA-256 of the lowercased full path, first 16 hex
    std::wstring low = s;
    for (auto& c : low) c = (wchar_t)towlower(c);
    std::string u = WxU(low);
    unsigned char h[32];
    crxk::Sha256((const unsigned char*)u.data(), u.size(), h);   // crxk::Sha256 lives in crx_unpack.inl
    static const char* hexd = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; i++) { out += hexd[h[i] >> 4]; out += hexd[h[i] & 15]; }
    return out;
}
static std::string AiDocSessionKey(const std::wstring& path) {
    return "doc_" + AiSha256Hex16(path);
}
static std::string AiWebSessionKey(const std::wstring& tabId) {
    return "wt_" + WxU(tabId);
}
static std::string AiEditorSessionKey(const std::wstring& tabId) {
    return "ed_" + WxU(tabId);
}

// ---- AiSource (the citation the model's answer carries) ----
enum AiQuoteLoc { AQL_EXACT, AQL_APPROX, AQL_UNLOCATED };
struct AiSource {
    std::string id;              // SOURCE_n, the model's spelling kept
    int page = 0;                // 1-based
    int pageIndex = -1;          // 0-based
    std::string quote, reason;
    bool verified = false;
    int location = AQL_UNLOCATED;
    // resolved geometry: the page's rects in page DIPs (for the highlight)
    std::vector<FRect> rects;
};
struct AiChatMsg {
    int role = 0;                // 0 user, 1 assistant
    std::string content;
    long long ts = 0;
    std::vector<AiSource> sources;
};

// ---- word normalization (QuoteLocator: lowercase letters+digits) ----
static void AiWordize(const std::string& text, std::vector<std::string>& words) {
    words.clear();
    std::string cur;
    for (char c : text) {
        if (isspace((unsigned char)c)) {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else if (isalnum((unsigned char)c)) {
            cur += (char)tolower((unsigned char)c);
        }
    }
    if (!cur.empty()) words.push_back(cur);
}
// LocateInChunk: Pass A the exact consecutive run, Pass B the in-order
// subsequence trusted at max(3, ceil(0.8 * needle words)).
enum AiQuoteHit { AQH_NONE = 0, AQH_EXACT, AQH_APPROX };
static AiQuoteHit AiLocateWords(const std::vector<std::string>& chunk,
                                const std::vector<std::string>& needle,
                                size_t& startOut, size_t& endOut) {
    if (needle.empty() || chunk.empty()) return AQH_NONE;
    // Pass A
    for (size_t i = 0; i + needle.size() <= chunk.size(); i++) {
        bool eq = true;
        for (size_t j = 0; j < needle.size(); j++)
            if (chunk[i + j] != needle[j]) { eq = false; break; }
        if (eq) { startOut = i; endOut = i + needle.size(); return AQH_EXACT; }
    }
    // Pass B: needle >= 3 words
    if (needle.size() < 3) return AQH_NONE;
    size_t need = max<size_t>(3, (size_t)ceil(0.8 * (double)needle.size()));
    size_t first = std::string::npos, matched = 0;
    size_t j = 0;
    for (size_t i = 0; i < chunk.size() && j < needle.size(); i++) {
        if (chunk[i] == needle[j]) {
            if (first == std::string::npos) first = i;
            matched++; j++;
        }
    }
    if (j == needle.size() && matched >= need && first != std::string::npos) {
        startOut = first;
        endOut = first + 1;      // the window's end lands below
        return AQH_APPROX;
    }
    return AQH_NONE;
}
static AiQuoteHit AiLocateInText(const std::string& chunkText, const std::string& quote,
                                 size_t& startOut, size_t& endOut) {
    std::vector<std::string> cw, nw;
    AiWordize(chunkText, cw);
    AiWordize(quote, nw);
    return AiLocateWords(cw, nw, startOut, endOut);
}
// PassageQuoteValidator: >= 8 normalized letters, else unverified
static bool AiQuotePlausible(const std::string& quote, const std::string& chunkText) {
    std::string qj, cj;
    {
        std::vector<std::string> w;
        AiWordize(quote, w);
        for (auto& x : w) qj += x;
        AiWordize(chunkText, w);
        for (auto& x : w) cj += x;
    }
    if (qj.size() < 8) return false;
    return cj.find(qj) != std::string::npos;
}

// ---- web page segmentation (WebChat.SplitPageSegments) ----
static const char* AI_WEB_TRUNC_MARKER =
    "\n\n[The page's text was cut here to fit the context window: the beginning and the end are shown, the middle left out.]\n\n";
static std::string AiClampPageText(const std::string& text, bool& truncated) {
    truncated = false;
    const size_t maxChars = 160000, headChars = 140000, tailChars = 20000;
    if (text.size() <= maxChars) return text;
    truncated = true;
    return text.substr(0, headChars) + AI_WEB_TRUNC_MARKER + text.substr(text.size() - tailChars);
}
static size_t AiSentenceCut(const std::string& t, size_t start, size_t preferred, size_t limit) {
    size_t hard = min(preferred, t.size() - 1);
    if (hard <= start) return min(start + 1, t.size());
    for (size_t i = hard; i > start; i--) {
        char c = t[i - 1];
        if (c == '.' || c == '!' || c == '?' || c == '.') return i;
    }
    return hard;
}
static std::vector<std::string> AiSplitPageSegments(const std::string& pageText) {
    const size_t target = 1200, splitMax = 2400, minSeg = 80;
    const size_t maxSegs = 160;
    std::vector<std::string> segments, pieces;
    std::string marker = std::string(AI_WEB_TRUNC_MARKER);
    while (!marker.empty() && (marker.front() == '\n' || marker.front() == ' ')) marker.erase(0, 1);
    while (!marker.empty() && (marker.back() == '\n' || marker.back() == ' ')) marker.pop_back();
    // paragraphs: blank-line separated
    size_t p = 0;
    while (p < pageText.size()) {
        size_t e = pageText.find("\n\n", p);
        std::string para = pageText.substr(p, e == std::string::npos ? std::string::npos : e - p);
        while (!para.empty() && (para.front() == '\r' || para.front() == '\n')) para.erase(0, 1);
        while (!para.empty() && (para.back() == '\r' || para.back() == '\n' || para.back() == ' ')) para.pop_back();
        p = e == std::string::npos ? pageText.size() : e + 2;
        if (para.empty() || para == marker) continue;
        if (para.size() <= splitMax) { pieces.push_back(para); continue; }
        size_t start = 0;
        while (start < para.size()) {
            size_t remain = para.size() - start;
            if (remain <= splitMax) { pieces.push_back(para.substr(start)); break; }
            size_t cut = AiSentenceCut(para, start, start + target, start + splitMax);
            std::string piece = para.substr(start, cut - start);
            while (!piece.empty() && isspace((unsigned char)piece.back())) piece.pop_back();
            if (!piece.empty()) pieces.push_back(piece);
            start = cut;
            while (start < para.size() && isspace((unsigned char)para[start])) start++;
        }
    }
    for (auto& piece : pieces) {
        if (!segments.empty() && segments.back().size() < minSeg)
            segments.back() += "\n\n" + piece;
        else segments.push_back(piece);
    }
    if (segments.size() > 1 && segments.back().size() < minSeg) {
        segments[segments.size() - 2] += "\n\n" + segments.back();
        segments.pop_back();
    }
    while (segments.size() > maxSegs) {
        std::vector<std::string> merged;
        for (size_t i = 0; i < segments.size(); i += 2) {
            if (i + 1 < segments.size()) merged.push_back(segments[i] + "\n\n" + segments[i + 1]);
            else merged.push_back(segments[i]);
        }
        segments = std::move(merged);
    }
    return segments;
}
static std::string AiNormalizeForMatch(const std::string& text) {
    std::string out;
    bool ws = false;
    for (char c : text) {
        if (isspace((unsigned char)c)) { ws = true; continue; }
        if (ws && !out.empty()) out += ' ';
        ws = false;
        out += c;
    }
    return out;
}
// SegmentHead: the first <= 120 chars of the normalized segment, word-bounded
static std::string AiSegmentHead(const std::string& seg) {
    std::string n = AiNormalizeForMatch(seg);
    if (n.size() <= 120) return n;
    n.resize(120);
    size_t sp = n.find_last_of(' ');
    if (sp != std::string::npos && sp > 40) n.resize(sp);
    return n;
}

// ---- the chunk model ----
struct AiPageSpan {
    int page = 0;                          // 0-based
    int wordStart = 0, wordEnd = 0;        // chunk-relative word range on that page
    float l = 0, t = 0, r = 0, b = 0;      // union box of those words (page DIPs)
    float pw = 0, ph = 0;
};
struct AiChunk {
    std::string id;
    std::string text;
    std::string heading;                   // section heading if any
    int index = 0;
    std::vector<AiPageSpan> pages;
};

// ---- the chunker (DocumentChunker's streaming algorithm) ----
struct AiWordIn {
    std::wstring text;
    float l = 0, t = 0, r = 0, b = 0;      // page DIPs, top-left origin (docsy cells)
    bool paraStart = false, headingStart = false;
    int page = 0;
};
struct AiChunker {
    std::vector<AiWordIn> pending;
    size_t pendingChars = 0;
    std::vector<AiChunk> done;
    size_t charOffset = 0;
    std::wstring pendingHeading;           // the heading waiting for its anchor word
    const size_t Target = 500, Max = 800, MinWords = 20, Overlap = 40;

    void Flush(size_t breakAt) {           // [0, breakAt) becomes a chunk
        AiChunk c;
        std::string text;
        std::map<int, AiPageSpan> spans;
        for (size_t i = 0; i < breakAt; i++) {
            if (i) text += " ";
            text += WxU(pending[i].text);
            auto& sp = spans[pending[i].page];
            if (sp.wordEnd == 0 && sp.wordStart == 0) {
                sp.wordStart = (int)i;
                sp.l = pending[i].l; sp.t = pending[i].t;
            }
            sp.wordEnd = (int)(i + 1);
            sp.page = pending[i].page;
            sp.r = max(sp.r, pending[i].r);
            sp.b = max(sp.b, pending[i].b);
        }
        c.text = text;
        c.id = "chunk_" + std::to_string(done.size()) + "_" +
               std::to_string(charOffset);
        c.heading = WxU(pendingHeading);
        c.index = (int)done.size();
        charOffset += text.size() + 1;
        for (auto& kv : spans) c.pages.push_back(kv.second);
        done.push_back(std::move(c));
        // overlap: the head's tail rides again at the front, flags cleared
        std::vector<AiWordIn> rest(pending.begin() + breakAt, pending.end());
        size_t ov = min(Overlap, breakAt / 4);
        std::vector<AiWordIn> overlap(pending.begin() + (breakAt > ov ? breakAt - ov : 0),
                                      pending.begin() + breakAt);
        for (auto& w : overlap) { w.paraStart = false; w.headingStart = false; }
        pending.clear();
        pending = overlap;
        pending.insert(pending.end(), rest.begin(), rest.end());
        pendingChars = 0;
        for (auto& w : pending) pendingChars += WxU(w.text).size() + 1;
        pendingHeading.clear();
    }
    int FindBreak() {
        // strongest first: heading start > paragraph start > sentence end > clause end
        for (size_t i = MinWords; i < pending.size(); i++)
            if (pending[i].headingStart) return (int)i;
        int para = -1, sent = -1, clause = -1;
        for (size_t i = MinWords; i < pending.size(); i++) {
            if (pending[i].paraStart && para < 0) para = (int)i;
            const std::wstring& prev = pending[i - 1].text;
            if (!prev.empty()) {
                wchar_t lc = prev.back();
                if (lc == L'.' || lc == L'!' || lc == L'?') { if (sent < 0) sent = (int)i; }
                else if (lc == L',' || lc == L':' || lc == L';') { if (clause < 0) clause = (int)i; }
            }
        }
        if (sent >= 0) return sent;
        if (clause >= 0) return clause;
        if (para >= 0) return para;
        return -1;
    }
    void TryBreak() {
        if (pendingChars < (long long)Target) return;
        int b = FindBreak();
        if (b < 0) {
            if (pendingChars < (long long)Max) return;
            // hard overflow: cut at the MinWords boundary
            b = (int)min(MinWords, pending.size());
            if (b <= 0) return;
        }
        Flush(b);
    }
    void Append(const AiWordIn& w) {
        pending.push_back(w);
        pendingChars += WxU(w.text).size() + 1;
        TryBreak();
    }
    void Finish() {
        if (pending.empty()) return;
        size_t words = pending.size();
        if (words < MinWords && !done.empty()) {
            // trailing remainder merges into the previous chunk when it fits
            AiChunk& last = done.back();
            std::string tail;
            for (auto& w : pending) { if (!tail.empty()) tail += " "; tail += WxU(w.text); }
            if (last.text.size() + tail.size() <= Max) {
                last.text += " " + tail;
                for (auto& w : pending) {
                    bool merged = false;
                    for (auto& sp : last.pages)
                        if (sp.page == w.page) {
                            sp.r = max(sp.r, w.r); sp.b = max(sp.b, w.b);
                            merged = true;
                        }
                    if (!merged)
                        last.pages.push_back({ w.page, 0, 0, w.l, w.t, w.r, w.b, 0, 0 });
                }
                pending.clear();
                pendingChars = 0;
                return;
            }
        }
        Flush(pending.size());
    }
};
// Page words from docsy's OCR cells: words in reading order with paragraph /
// heading flags derived from the line boxes (PageWordStreamBuilder's geometry,
// reduced to what docsy's TxtPage already carries).
static void AiPageWords(const TxtPage& pt, int page, std::vector<AiWordIn>& out) {
    out.clear();
    if (pt.cells.empty()) return;
    // words: cells grouped by their word index, boxes unioned
    std::map<int, std::wstring> wt;
    std::map<int, std::array<float, 4>> wbox;
    std::vector<float> hts;
    for (auto& c : pt.cells) {
        auto& bx = wbox[c.word];
        if (bx[2] == 0 && bx[3] == 0) bx = { c.l, c.t, c.r, c.b };
        else { bx[0] = min(bx[0], c.l); bx[1] = min(bx[1], c.t);
               bx[2] = max(bx[2], c.r); bx[3] = max(bx[3], c.b); }
        wt[c.word] += c.text;
    }
    for (auto& kv : wbox) hts.push_back(kv.second[3] - kv.second[1]);
    float medH = 10.f;
    if (!hts.empty()) { std::sort(hts.begin(), hts.end()); medH = hts[hts.size() / 2]; }
    // lines: cells grouped by their line index, ordered by top (reading order)
    std::map<int, std::vector<int>> byLine;
    for (auto& c : pt.cells) byLine[c.line].push_back(c.word);
    std::vector<std::pair<int, std::vector<int>>> lines(byLine.begin(), byLine.end());
    std::sort(lines.begin(), lines.end(), [&](auto& a, auto& b) {
        return wbox[a.second.front()][1] < wbox[b.second.front()][1];
    });
    // median line height for the paragraph-gap rule
    std::vector<float> lh;
    for (auto& L : lines) {
        float top = 1e9f, bottom = -1e9f;
        for (int w : L.second) { top = min(top, wbox[w][1]); bottom = max(bottom, wbox[w][3]); }
        if (bottom > top) lh.push_back(bottom - top);
    }
    float medLine = lh.empty() ? medH : lh[lh.size() / 2];
    int headingUsed = 0;
    float prevBottom = -1;
    for (auto& L : lines) {
        float top = 1e9f, bottom = -1e9f;
        std::wstring joined;
        for (int w : L.second) {
            top = min(top, wbox[w][1]); bottom = max(bottom, wbox[w][3]);
            joined += wt[w];
        }
        bool paraStart = prevBottom >= 0 && medLine > 0 && (top - prevBottom) > 1.6f * medLine;
        prevBottom = bottom;
        bool heading = false;
        if (headingUsed < 1 && L.second.size() <= 8 && joined.size() <= 60 &&
            medH > 0 && (bottom - top) >= 1.2f * medH) {
            heading = true;
            headingUsed = 1;
        }
        int idx = 0;
        for (int w : L.second) {
            AiWordIn wi;
            wi.text = wt[w];
            auto& bx = wbox[w];
            wi.l = bx[0]; wi.t = bx[1]; wi.r = bx[2]; wi.b = bx[3];
            wi.page = page;
            wi.paraStart = paraStart && idx == 0;
            wi.headingStart = heading && idx == 0;
            out.push_back(std::move(wi));
            idx++;
        }
    }
}

// ---- the document index (per document, persisted) ----
struct AiDocIndex {
    std::string docId;
    std::wstring path;
    int pageCount = 0;
    std::vector<AiChunk> chunks;
    std::vector<std::vector<float>> vectors;    // parallel to chunks (may be empty)
    int dim = 0;
    std::string embModel;                       // the vectors' identity
    std::string prefixKey;
    std::string contentHash;
};
static std::string AiFileHash(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    unsigned char buf[65536], sha[32];
    unsigned long long total = 0;
    // the file's own bytes hash (a doc-sized SHA over chunks)
    DWORD rd = 0;
    std::vector<unsigned char> all;
    while (ReadFile(h, buf, sizeof buf, &rd, nullptr) && rd) all.insert(all.end(), buf, buf + rd);
    CloseHandle(h);
    crxk::Sha256(all.data(), all.size(), sha);
    static const char* hexd = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; i++) { out[i * 2] = hexd[sha[i] >> 4]; out[i * 2 + 1] = hexd[sha[i] & 15]; }
    return out;
}
static bool AiIndexSave(const AiDocIndex& ix) {
    std::string j = "{\"docId\":" + WxJsonStr(WxU(ix.docId)) +
        ",\"path\":" + WxJsonStr(ix.path) +
        ",\"pageCount\":" + std::to_string(ix.pageCount) +
        ",\"dim\":" + std::to_string(ix.dim) +
        ",\"embModel\":" + WxJsonStr(WxU(ix.embModel)) +
        ",\"prefixKey\":" + WxJsonStr(WxU(ix.prefixKey)) +
        ",\"contentHash\":" + WxJsonStr(WxU(ix.contentHash)) + ",\"chunks\":[";
    for (size_t i = 0; i < ix.chunks.size(); i++) {
        auto& c = ix.chunks[i];
        if (i) j += ",";
        j += "{\"id\":" + WxJsonStr(WxU(c.id)) +
             ",\"index\":" + std::to_string(c.index) +
             ",\"heading\":" + WxJsonStr(WxU(c.heading)) +
             ",\"text\":" + WxJsonStr(WxU(c.text)) + ",\"pages\":[";
        for (size_t k = 0; k < c.pages.size(); k++) {
            auto& s = c.pages[k];
            if (k) j += ",";
            char b[220];
            snprintf(b, sizeof b, "[%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f]",
                     s.page, s.wordStart, s.wordEnd, s.l, s.t, s.r, s.b, s.pw, s.ph);
            j += b;
        }
        j += "]}";
    }
    j += "]}";
    if (!WxWriteFile(AiIndexPath(ix.docId), (const unsigned char*)j.data(), j.size()))
        return false;
    // the vectors ride their own float32 file (dim + count header)
    if (!ix.vectors.empty() && ix.dim > 0) {
        std::vector<unsigned char> bin;
        unsigned hdr[2] = { (unsigned)ix.dim, (unsigned)ix.vectors.size() };
        bin.insert(bin.end(), (unsigned char*)hdr, (unsigned char*)hdr + 8);
        for (auto& v : ix.vectors)
            bin.insert(bin.end(), (const unsigned char*)v.data(),
                       (const unsigned char*)v.data() + v.size() * sizeof(float));
        WxWriteFile(AiVecPath(ix.docId), bin.data(), bin.size());
    }
    return true;
}
// The build's landing save, off the UI thread. AiIndexSave builds the whole
// chunk deck as one JSON string - for a real book that is tens of megabytes of
// escaping and appending, seconds of a frozen message loop at the exact moment
// the reader starts asking the panel questions (the "send, then the whole app
// dies five seconds later" report was this save freezing the UI thread at the
// build's end). The copy is one memcpy sweep; the string build and the write
// ride the thread. A same-path write that collides with the semantic pass's
// own save just fails its CreateFile - no corruption, the later save wins.
static void AiIndexSaveAsync(const AiDocIndex& ix) {
    std::shared_ptr<AiDocIndex> copy = std::make_shared<AiDocIndex>(ix);
    // the guard is the rule for every AI worker: an uncaught exception on a
    // detached thread is std::terminate, and std::terminate is the whole app
    // dying with nothing on screen. A failed save is silent by nature.
    std::thread([copy]() {
        try { AiIndexSave(*copy); } catch (...) {}
    }).detach();
}
static bool AiIndexLoad(const std::string& docId, AiDocIndex& ix) {
    std::string s;
    if (!WxReadFile(AiIndexPath(docId), s)) return false;
    crxk::J j;
    if (!crxk::JParse(s, j) || j.t != 'o') return false;
    ix.docId = docId;
    const crxk::J* p;
    if ((p = j.get("path")) && p->t == 's') ix.path = WxW(p->s);
    if ((p = j.get("pageCount")) && p->t == 'd') ix.pageCount = (int)p->d;
    if ((p = j.get("dim")) && p->t == 'd') ix.dim = (int)p->d;
    if ((p = j.get("embModel")) && p->t == 's') ix.embModel = p->s;
    if ((p = j.get("prefixKey")) && p->t == 's') ix.prefixKey = p->s;
    if ((p = j.get("contentHash")) && p->t == 's') ix.contentHash = p->s;
    const crxk::J* cs = j.get("chunks");
    if (cs && cs->t == 'a')
        for (auto& c : cs->a) {
            if (c.t != 'o') continue;
            AiChunk ch;
            if ((p = c.get("id")) && p->t == 's') ch.id = p->s;
            if ((p = c.get("index")) && p->t == 'd') ch.index = (int)p->d;
            if ((p = c.get("heading")) && p->t == 's') ch.heading = p->s;
            if ((p = c.get("text")) && p->t == 's') ch.text = p->s;
            const crxk::J* ps = c.get("pages");
            if (ps && ps->t == 'a')
                for (auto& e : ps->a) {
                    if (e.t != 'a' || e.a.size() < 9) continue;
                    AiPageSpan sp;
                    sp.page = (int)e.a[0].d; sp.wordStart = (int)e.a[1].d;
                    sp.wordEnd = (int)e.a[2].d;
                    sp.l = (float)e.a[3].d; sp.t = (float)e.a[4].d;
                    sp.r = (float)e.a[5].d; sp.b = (float)e.a[6].d;
                    sp.pw = (float)e.a[7].d; sp.ph = (float)e.a[8].d;
                    ch.pages.push_back(sp);
                }
            ix.chunks.push_back(std::move(ch));
        }
    // vectors
    std::string binS;
    std::vector<unsigned char> bin;
    if (ix.dim > 0 && WxReadFile(AiVecPath(docId), binS)) {
        bin.assign(binS.begin(), binS.end());
    }
    if (ix.dim > 0 && bin.size() > 8) {
        unsigned hdr[2];
        memcpy(hdr, bin.data(), 8);
        size_t need = (size_t)hdr[0] * hdr[1] * sizeof(float) + 8;
        if ((int)hdr[0] == ix.dim && bin.size() >= need && hdr[1] == ix.chunks.size()) {
            ix.vectors.resize(hdr[1]);
            for (size_t i = 0; i < hdr[1]; i++) {
                ix.vectors[i].resize(hdr[0]);
                memcpy(ix.vectors[i].data(), bin.data() + 8 + i * hdr[0] * sizeof(float),
                       hdr[0] * sizeof(float));
            }
        }
    }
    return !ix.chunks.empty();
}

// ---- the hybrid retriever ----
static bool AiIsStopword(const std::string& w) {
    static const char* sw[] = { "a","an","the","and","or","but","if","then","than","so","because",
        "as","of","to","in","on","at","by","for","with","from","about","into","over","after",
        "before","between","out","off","up","down","again","further","once","here","there",
        "all","any","both","each","few","more","most","other","some","such","no","nor","not",
        "only","own","same","too","very","can","cannot","could","should","shall","will","would",
        "may","might","must","do","does","did","doing","done","have","has","had","having","be",
        "is","are","was","were","been","being","am","it","its","this","that","these","those",
        "i","me","my","we","our","you","your","he","him","his","she","her","they","them",
        "their","what","which","who","whom","whose","when","where","why","how" };
    for (const char* s : sw) if (w == s) return true;
    return false;
}
static void AiQueryTerms(const std::string& q, std::vector<std::string>& terms) {
    terms.clear();
    std::string cur;
    auto push = [&]() {
        if (cur.size() >= 2 && !AiIsStopword(cur) && terms.size() < 24) {
            bool dup = false;
            for (auto& t : terms) if (t == cur) { dup = true; break; }
            if (!dup) terms.push_back(cur);
        }
        cur.clear();
    };
    for (char c : q) {
        if (isalnum((unsigned char)c)) cur += (char)tolower((unsigned char)c);
        else push();
    }
    push();
}
static double AiCosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); i++) { dot += (double)a[i] * b[i]; na += (double)a[i] * a[i]; nb += (double)b[i] * b[i]; }
    if (na <= 0 || nb <= 0) return 0;
    return dot / (sqrt(na) * sqrt(nb));
}
struct AiRetrieved {
    const AiChunk* chunk = nullptr;
    int chunkIdx = -1;
    double score = 0;
    int method = 0;   // 0 lexical, 1 hybrid, 2 reranked
};
// BM25 over the in-memory chunks (FTS5's rank, approximated)
static void AiLexicalScores(const AiDocIndex& ix, const std::string& query,
                            std::vector<std::pair<int, double>>& out) {
    std::vector<std::string> terms;
    AiQueryTerms(query, terms);
    out.clear();
    if (terms.empty() || ix.chunks.empty()) return;
    // document frequency per term
    size_t N = ix.chunks.size();
    std::vector<std::vector<std::string>> toks(N);
    std::vector<double> len(N);
    double avgLen = 0;
    std::map<std::string, int> df;
    for (size_t i = 0; i < N; i++) {
        AiWordize(ix.chunks[i].text, toks[i]);
        len[i] = (double)toks[i].size();
        avgLen += len[i];
        std::set<std::string> seen;
        for (auto& t : toks[i]) if (seen.insert(t).second) df[t]++;
    }
    avgLen /= (double)N;
    if (avgLen <= 0) avgLen = 1;
    for (size_t i = 0; i < N; i++) {
        std::map<std::string, int> tf;
        for (auto& t : toks[i]) tf[t]++;
        double s = 0;
        for (auto& q : terms) {
            auto dit = df.find(q);
            if (dit == df.end()) continue;
            int n = tf[q];
            if (!n) continue;
            double idf = log(1.0 + (double)(N - dit->second + 0.5) / (double)(dit->second + 0.5));
            double k1 = 1.2, b = 0.75;
            s += idf * (n * (k1 + 1.0)) / (n + k1 * (1.0 - b + b * len[i] / avgLen));
        }
        if (s > 0) out.push_back({ (int)i, s });
    }
    std::sort(out.begin(), out.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
}
static double AiJaccard(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::set<std::string> sa(a.begin(), a.end()), sb(b.begin(), b.end());
    if (sa.empty() || sb.empty()) return 0;
    int inter = 0;
    for (auto& x : sa) if (sb.count(x)) inter++;
    return (double)inter / (double)(sa.size() + sb.size() - inter);
}
// The full channel: semantic gate, pools, fusion, rerank, dedupe, budget.
static std::vector<AiRetrieved> AiRetrieve(const AiDocIndex& ix, const std::string& query,
                                           const std::vector<float>* queryVec, int topK,
                                           int evidenceBudget) {
    std::vector<AiRetrieved> results;
    const int pool = 80;
    // lexical pool
    std::vector<std::pair<int, double>> lex;
    AiLexicalScores(ix, query, lex);
    if (lex.size() > (size_t)pool) lex.resize(pool);
    double lexMin = 0, lexMax = 0;
    if (!lex.empty()) {
        lexMin = lex.back().second; lexMax = lex.front().second;
        if (lexMax <= lexMin) lexMax = lexMin + 1;
    }
    // semantic pool
    std::vector<std::pair<int, double>> sem;
    if (queryVec && !queryVec->empty() && ix.dim > 0 &&
        queryVec->size() == (size_t)ix.dim && ix.vectors.size() == ix.chunks.size()) {
        for (size_t i = 0; i < ix.vectors.size(); i++) {
            double c = AiCosine(*queryVec, ix.vectors[i]);
            if (c > 0.1) sem.push_back({ (int)i, c });
        }
        std::sort(sem.begin(), sem.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (sem.size() > (size_t)pool) sem.resize(pool);
    }
    double semMin = 0, semMax = 0;
    if (!sem.empty()) {
        semMin = sem.back().second; semMax = sem.front().second;
        if (semMax <= semMin) semMax = semMin + 1;
    }
    // fusion 0.4 / 0.6 on min-max normalized pools
    std::map<int, double> fused;
    std::set<int> semIds;
    for (auto& s : sem) {
        semIds.insert(s.first);
        double n = (s.second - semMin) / (semMax - semMin);
        fused[s.first] += 0.6 * n;
    }
    for (auto& l : lex) {
        double n = (l.second - lexMin) / (lexMax - lexMin);
        fused[l.first] += (semIds.empty() ? 1.0 : 0.4) * n;
    }
    std::vector<std::pair<int, double>> rank(fused.begin(), fused.end());
    std::sort(rank.begin(), rank.end(), [](auto& a, auto& b) { return a.second > b.second; });
    // single hit forced to 1
    if (rank.size() == 1) rank[0].second = 1.0;
    // rerank: 0.7 score + 0.3 term density (+0.1 heading, +0.02 early)
    std::vector<std::string> qterms;
    AiQueryTerms(query, qterms);
    for (auto& r : rank) {
        const AiChunk& c = ix.chunks[r.first];
        std::string low = c.text;
        for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
        int hits = 0;
        for (auto& t : qterms) if (low.find(t) != std::string::npos) hits++;
        double dens = qterms.empty() ? 0 : (double)hits / (double)qterms.size();
        double rr = r.second * 0.7 + dens * 0.3;
        std::string headLow = c.heading;
        for (auto& ch : headLow) ch = (char)tolower((unsigned char)ch);
        for (auto& t : qterms)
            if (!headLow.empty() && headLow.find(t) != std::string::npos) { rr += 0.1; break; }
        if (c.index < 5) rr += 0.02;
        r.second = min(1.0, rr);
    }
    // dedupe overlapping hits (Jaccard > 0.6). `words` is indexed by RANK
    // POSITION - the old dedupe indexed it with d.chunkIdx, the chunk's index
    // in ix.chunks, and on any real book the top-scoring chunk's index sits
    // far beyond rank.size(): an out-of-bounds vector read that turned the
    // first real retrieval into the "send, then the app dies five seconds
    // later" crash (the tiny CI sample never scattered its indices, so it
    // never fired there). The kept entries now carry their own rank position.
    std::vector<std::vector<std::string>> words;
    words.resize(rank.size());
    for (size_t i = 0; i < rank.size(); i++) AiWordize(ix.chunks[rank[i].first].text, words[i]);
    std::vector<AiRetrieved> dedup;
    std::vector<size_t> dedupPos;        // dedup[k] landed at rank position dedupPos[k]
    for (size_t i = 0; i < rank.size(); i++) {
        bool dup = false;
        for (size_t k = 0; k < dedup.size(); k++)
            if (AiJaccard(words[i], words[dedupPos[k]]) > 0.6) { dup = true; break; }
        if (!dup) {
            dedup.push_back({ &ix.chunks[rank[i].first], rank[i].first, rank[i].second, 2 });
            dedupPos.push_back(i);
        }
    }
    // relative floor + topK + evidence budget
    double best = dedup.empty() ? 0 : dedup[0].score;
    int total = 0;
    for (auto& r : dedup) {
        if (results.size() >= (size_t)topK) break;
        if (r.score < 0.15 || r.score < best * 0.12) break;
        if (total > 0 && total + (int)r.chunk->text.size() > evidenceBudget) break;
        total += (int)r.chunk->text.size();
        results.push_back(r);
    }
    return results;
}

// ---- the chat history store (ChatHistoryStore, JSON) ----
struct AiConv {
    std::string id, kind, title, sourceRef;
    long long createdAt = 0, updatedAt = 0;
    std::vector<AiChatMsg> msgs;
};
static std::vector<AiConv> g_aiHist;
static bool g_aiHistLoaded = false;
static long long AiNowMs() {
    return (long long)GetTickCount64();   // relative stamps; ordering is all we need
}
static void AiHistLoad() {
    if (g_aiHistLoaded) return;
    g_aiHistLoaded = true;
    std::string s;
    if (!WxReadFile(AiRootDir() + L"\\chat_history.json", s)) return;
    crxk::J j;
    if (!crxk::JParse(s, j) || j.t != 'o') return;
    const crxk::J* cs = j.get("conversations");
    if (!cs || cs->t != 'a') return;
    for (auto& c : cs->a) {
        if (c.t != 'o') continue;
        AiConv cv;
        const crxk::J* p;
        if ((p = c.get("id")) && p->t == 's') cv.id = p->s;
        if ((p = c.get("kind")) && p->t == 's') cv.kind = p->s;
        if ((p = c.get("title")) && p->t == 's') cv.title = p->s;
        if ((p = c.get("sourceRef")) && p->t == 's') cv.sourceRef = p->s;
        if ((p = c.get("createdAt")) && p->t == 'd') cv.createdAt = (long long)p->d;
        if ((p = c.get("updatedAt")) && p->t == 'd') cv.updatedAt = (long long)p->d;
        const crxk::J* ms = c.get("messages");
        if (ms && ms->t == 'a')
            for (auto& m : ms->a) {
                if (m.t != 'o') continue;
                AiChatMsg msg;
                if ((p = m.get("role")) && p->t == 'd') msg.role = (int)p->d;
                if ((p = m.get("content")) && p->t == 's') msg.content = p->s;
                if ((p = m.get("ts")) && p->t == 'd') msg.ts = (long long)p->d;
                const crxk::J* ss = m.get("sources");
                if (ss && ss->t == 'a')
                    for (auto& e : ss->a) {
                        if (e.t != 'o') continue;
                        AiSource src;
                        if ((p = e.get("id")) && p->t == 's') src.id = p->s;
                        if ((p = e.get("page")) && p->t == 'd') src.page = (int)p->d;
                        if ((p = e.get("pageIndex")) && p->t == 'd') src.pageIndex = (int)p->d;
                        if ((p = e.get("quote")) && p->t == 's') src.quote = p->s;
                        if ((p = e.get("reason")) && p->t == 's') src.reason = p->s;
                        if ((p = e.get("verified")) && p->t == 'b') src.verified = p->b;
                        if ((p = e.get("location")) && p->t == 'd') src.location = (int)p->d;
                        msg.sources.push_back(std::move(src));
                    }
                cv.msgs.push_back(std::move(msg));
            }
        g_aiHist.push_back(std::move(cv));
    }
}
static void AiHistSave() {
    std::string j = "{\"conversations\":[";
    for (size_t i = 0; i < g_aiHist.size(); i++) {
        auto& c = g_aiHist[i];
        if (i) j += ",";
        j += "{\"id\":" + WxJsonStr(WxU(c.id)) + ",\"kind\":" + WxJsonStr(WxU(c.kind)) +
             ",\"title\":" + WxJsonStr(WxU(c.title)) +
             ",\"sourceRef\":" + WxJsonStr(WxU(c.sourceRef)) +
             ",\"createdAt\":" + std::to_string(c.createdAt) +
             ",\"updatedAt\":" + std::to_string(c.updatedAt) + ",\"messages\":[";
        for (size_t k = 0; k < c.msgs.size(); k++) {
            auto& m = c.msgs[k];
            if (k) j += ",";
            j += "{\"role\":" + std::to_string(m.role) +
                 ",\"content\":" + WxJsonStr(WxU(m.content)) +
                 ",\"ts\":" + std::to_string(m.ts) + ",\"sources\":[";
            for (size_t s = 0; s < m.sources.size(); s++) {
                auto& src = m.sources[s];
                if (s) j += ",";
                j += "{\"id\":" + WxJsonStr(WxU(src.id)) +
                     ",\"page\":" + std::to_string(src.page) +
                     ",\"pageIndex\":" + std::to_string(src.pageIndex) +
                     ",\"quote\":" + WxJsonStr(WxU(src.quote)) +
                     ",\"reason\":" + WxJsonStr(WxU(src.reason)) +
                     ",\"verified\":" + (src.verified ? "true" : "false") +
                     ",\"location\":" + std::to_string(src.location) + "}";
            }
            j += "]}";
        }
        j += "]}";
    }
    j += "]}";
    WxWriteFile(AiRootDir() + L"\\chat_history.json", (const unsigned char*)j.data(), j.size());
}
static AiConv* AiHistLatest(const char* kind, const std::string& sourceRef) {
    AiHistLoad();
    AiConv* best = nullptr;
    for (auto& c : g_aiHist)
        if (c.kind == kind && c.sourceRef == sourceRef)
            if (!best || c.updatedAt > best->updatedAt) best = &c;
    return best;
}
static AiConv* AiHistById(const std::string& id) {
    AiHistLoad();
    for (auto& c : g_aiHist) if (c.id == id) return &c;
    return nullptr;
}
static std::string AiHistEnsure(const char* kind, const std::string& sourceRef,
                                const std::string& title) {
    AiHistLoad();
    AiConv* ex = AiHistLatest(kind, sourceRef);
    if (ex) return ex->id;
    AiConv c;
    static unsigned long long seq = 0;
    c.id = "c" + std::to_string(++seq) + "_" + std::to_string(GetTickCount64());
    c.kind = kind; c.sourceRef = sourceRef; c.title = title;
    c.createdAt = c.updatedAt = AiNowMs();
    g_aiHist.push_back(std::move(c));
    return g_aiHist.back().id;
}
static void AiHistUpsert(const std::string& id, const char* kind,
                         const std::string& sourceRef, const std::string& title,
                         const std::vector<AiChatMsg>& msgs) {
    AiHistLoad();
    if (msgs.empty()) return;                       // an empty snapshot never wipes a row
    AiConv* c = AiHistById(id);
    if (!c) {
        AiConv nw;
        nw.id = id; nw.kind = kind; nw.sourceRef = sourceRef; nw.title = title;
        nw.createdAt = nw.updatedAt = AiNowMs();
        g_aiHist.push_back(std::move(nw));
        c = &g_aiHist.back();
    }
    c->title = title;
    c->updatedAt = AiNowMs();
    c->msgs = msgs;
    AiHistSave();
}
static void AiHistDelete(const std::string& id) {
    AiHistLoad();
    for (size_t i = 0; i < g_aiHist.size(); i++)
        if (g_aiHist[i].id == id) { g_aiHist.erase(g_aiHist.begin() + i); break; }
    AiHistSave();
}

// ---- the summary cache (SummaryCache, JSON) ----
static std::string AiCacheKey(const std::string& docId, int first, int last,
                              const std::string& model, const std::string& variant) {
    return docId + "|" + std::to_string(first) + "|" + std::to_string(last) + "|" +
           model + "|" + variant;
}
static bool AiCacheGet(const std::string& docId, int first, int last,
                       const std::string& model, const std::string& variant,
                       const std::string& hash, std::string& text) {
    std::string s;
    if (!WxReadFile(AiRootDir() + L"\\summary_cache.json", s)) return false;
    crxk::J j;
    if (!crxk::JParse(s, j) || j.t != 'o') return false;
    const crxk::J* e = j.get(AiCacheKey(docId, first, last, model, variant).c_str());
    if (!e || e->t != 'o') return false;
    const crxk::J* h = e->get("hash"), * c = e->get("text");
    if (!h || h->t != 's' || !c || c->t != 's') return false;
    if (h->s != hash) return false;
    text = c->s;
    return !text.empty();
}
static void AiCachePut(const std::string& docId, int first, int last,
                       const std::string& model, const std::string& variant,
                       const std::string& hash, const std::string& text) {
    std::string s;
    crxk::J j;
    if (WxReadFile(AiRootDir() + L"\\summary_cache.json", s) &&
        crxk::JParse(s, j) && j.t == 'o') {
        // keep at most 40 entries: drop the stalest
        while (j.o.size() >= 40) {
            size_t oldest = 0; long long ot = LLONG_MAX;
            for (size_t i = 0; i < j.o.size(); i++) {
                const crxk::J* t = j.o[i].second.get("ts");
                long long v = t && t->t == 'd' ? (long long)t->d : 0;
                if (v < ot) { ot = v; oldest = i; }
            }
            j.o.erase(j.o.begin() + oldest);
        }
    } else j = crxk::J();
    crxk::J e; e.t = 'o';
    crxk::J hv; hv.t = 's'; hv.s = hash; e.o.push_back({ "hash", hv });
    crxk::J tv; tv.t = 's'; tv.s = text; e.o.push_back({ "text", tv });
    crxk::J ts; ts.t = 'd'; ts.d = (double)AiNowMs(); e.o.push_back({ "ts", ts });
    j.t = 'o';
    j.o.push_back({ AiCacheKey(docId, first, last, model, variant), e });
    std::string out;
    WxJsonEmit(j, out);
    WxWriteFile(AiRootDir() + L"\\summary_cache.json", (const unsigned char*)out.data(), out.size());
}
static void AiCacheDeleteRange(const std::string& docId, int first, int last) {
    std::string s;
    crxk::J j;
    if (!WxReadFile(AiRootDir() + L"\\summary_cache.json", s) ||
        !crxk::JParse(s, j) || j.t != 'o') return;
    std::string prefix = docId + "|" + std::to_string(first) + "|" + std::to_string(last) + "|";
    std::vector<std::string> del;
    for (auto& kv : j.o)
        if (kv.first.rfind(prefix, 0) == 0) del.push_back(kv.first);
    for (auto& k : del)
        for (size_t i = 0; i < j.o.size(); i++)
            if (j.o[i].first == k) { j.o.erase(j.o.begin() + i); break; }
    std::string out;
    WxJsonEmit(j, out);
    WxWriteFile(AiRootDir() + L"\\summary_cache.json", (const unsigned char*)out.data(), out.size());
}

// Reset's full meaning: the button wipes EVERY cached summary - every range,
// every model, every document - not just the stretch currently on screen. The
// old range-scoped delete left the neighbors alive: the user hit Reset, hit
// Start (or stepped a range chip) and the cache still answered "fromCache"
// with the exact text they just wiped. The cache is a 40-entry replay room,
// not an archive - wiping the file wholesale is the honest reset.
static void AiCacheDeleteAll() {
    WxWriteFile(AiRootDir() + L"\\summary_cache.json",
                (const unsigned char*)"{}", 2);
}

// ---- the notes recall logic (NotesRecallLogic) ----
struct AiNoteCard { int first = 0, last = 0; std::string content; };
static std::vector<std::pair<int, int>> AiNoteBlocks(int first, int last) {
    // blocks of 50; a trailing remainder < 15 joins the previous block
    std::vector<std::pair<int, int>> out;
    if (last < first) std::swap(first, last);
    const int size = 50, minTail = 15;
    for (int a = first; a <= last; a += size) {
        int b = min(a + size - 1, last);
        if (b - a + 1 < minTail && !out.empty()) out.back().second = b;
        else out.push_back({ a, b });
    }
    return out;
}
static std::vector<AiNoteCard> AiParseNoteCards(const std::string& reply, int first, int last) {
    std::vector<AiNoteCard> out;
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : reply) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur += c;
        }
        lines.push_back(cur);
    }
    AiNoteCard cur;
    bool open = false;
    std::regex head("^#{1,6}\\s*Pages?\\s*\\.?\\s*[:\\-]?\\s*(\\d+)\\s*(?:[\xe2\x80\x93\xe2\x80\x94-]|to)\\s*(\\d+)\\s*:?$",
                    std::regex_constants::icase);
    for (auto& ln : lines) {
        std::smatch m;
        if (std::regex_match(ln, m, head)) {
            if (open && !cur.content.empty()) {
                while (!cur.content.empty() && (cur.content.back() == '\n' || cur.content.back() == '\r'))
                    cur.content.pop_back();
                out.push_back(cur);
            }
            cur = AiNoteCard();
            cur.first = min(atoi(m[1].str().c_str()), atoi(m[2].str().c_str()));
            cur.last = max(atoi(m[1].str().c_str()), atoi(m[2].str().c_str()));
            open = true;
            continue;
        }
        if (open) cur.content += ln + "\n";
    }
    if (open && !cur.content.empty()) {
        while (!cur.content.empty() && (cur.content.back() == '\n' || cur.content.back() == '\r'))
            cur.content.pop_back();
        out.push_back(cur);
    }
    if (out.empty() && !reply.empty()) {
        // the model ignored the heading contract: one card over the whole span
        AiNoteCard c;
        c.first = first; c.last = last;
        c.content = reply;
        while (!c.content.empty() && (c.content.back() == '\n' || c.content.back() == '\r'))
            c.content.pop_back();
        out.push_back(c);
    }
    return out;
}
