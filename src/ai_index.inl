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
    // the cited chunk's own words per page (0-based page -> the chunk's word
    // slice on that page): the highlight's primary needle - the WHOLE passage
    // the answer drew from, where the quote field carries only the chunk's
    // 120-byte head. Rides the answer blob and the persisted transcript, so
    // reloaded conversations' chips light their full passages too.
    std::vector<std::pair<int, std::string>> passages;
};
struct AiChatMsg {
    int role = 0;                // 0 user, 1 assistant
    std::string content;
    long long ts = 0;
    int durMs = 0;               // the answer's generation time (the footer's label)
    std::vector<AiSource> sources;
};

// ---- word normalization (QuoteLocator: lowercase letters+digits) ----
// A byte >= 0x80 is a UTF-8 multibyte sequence's interior - Arabic, accented
// Latin, CJK. The old isalnum test read those bytes as punctuation and every
// non-ASCII word vanished from the tokens: an Arabic document's chunks
// tokenized down to their digits and article numbers, and an Arabic question
// produced no terms at all - the sidechat's lexical pool went empty (or
// matched only "numbers and codes") while the summarizer, which rides the raw
// text, read the same document fine. Non-ASCII now rides as its own bytes (no
// tolower on them: folding a UTF-8 lead byte corrupts it); both sides of a
// match run through this same rule, so byte-exact words still meet. ASCII
// behavior is untouched: only whitespace splits, other ASCII punctuation is
// absorbed into the word, exactly as the byte-drop era matched Latin text.
static unsigned AiUtf8Cp(const std::string& s, size_t i, int& len) {
    unsigned char u = (unsigned char)s[i];
    if (u < 0x80) { len = 1; return u; }
    if ((u & 0xE0) == 0xC0 && i + 1 < s.size()) {
        len = 2; return ((u & 31u) << 6) | ((unsigned char)s[i + 1] & 63u);
    }
    if ((u & 0xF0) == 0xE0 && i + 2 < s.size()) {
        len = 3; return ((u & 15u) << 12) | (((unsigned char)s[i + 1] & 63u) << 6) |
                        ((unsigned char)s[i + 2] & 63u);
    }
    if ((u & 0xF8) == 0xF0 && i + 3 < s.size()) {
        len = 4; return ((u & 7u) << 18) | (((unsigned char)s[i + 1] & 63u) << 12) |
                        (((unsigned char)s[i + 2] & 63u) << 6) | ((unsigned char)s[i + 3] & 63u);
    }
    len = 1; return u;   // a broken sequence rides as one byte, like the old code
}
// The non-ASCII codepoints that still SPLIT a word: the punctuation Arabic
// writes inside a sentence - comma \xD8\x8C U+060C, semicolon \xD8\x9B U+061B, question mark
// \xD8\x9F U+061F, full stop \xD9\x94 U+06D4 - the invisible direction marks U+200E/U+200F/U+061C,
// the quote and dash marks that wrap titles, and the multibyte spaces (NBSP
// and friends). Without the split, "الوثيقة؟" glued the question mark onto the
// word and an exact match for the word itself missed. Everything else
// non-ASCII is a letter and rides whole.
static bool AiCpSplits(unsigned cp) {
    if (cp == 0x60C || cp == 0x61B || cp == 0x61F || cp == 0x6D4 || cp == 0x61C) return true;
    if (cp == 0x200E || cp == 0x200F || cp == 0x200B || cp == 0xFEFF) return true;
    if (cp == 0xAB || cp == 0xBB || cp == 0x201C || cp == 0x201D ||
        cp == 0x2018 || cp == 0x2019 || cp == 0x2026 || cp == 0x2013 || cp == 0x2014) return true;
    if (cp == 0xA0 || cp == 0x202F || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200A)) return true;
    return false;
}
static void AiWordize(const std::string& text, std::vector<std::string>& words) {
    words.clear();
    std::string cur;
    auto flush = [&]() { if (!cur.empty()) { words.push_back(cur); cur.clear(); } };
    for (size_t i = 0; i < text.size(); ) {
        unsigned char u = (unsigned char)text[i];
        int len = 0;
        unsigned cp = AiUtf8Cp(text, i, len);
        if (u < 0x80) {
            if (isspace(u)) flush();
            else if (isalnum(u)) cur += (char)tolower(u);
            i++;
            continue;
        }
        if (AiCpSplits(cp)) flush();
        else cur.append(text, i, (size_t)len);
        i += (size_t)len;
    }
    flush();
}
// The quote match's Arabic skeleton. Models quote Arabic without the marks
// the text layer carries - a legal PDF writes fatha, kasra, shadda, tatweel
// inside its words and writes alef as hamza/madda carriers and final ya as
// alef maqsura, while the model's copy rides plain - so byte-exact word
// equality missed every decorated word and the highlight fell to the chunk's
// page-span box. One rule now rides BOTH sides of every quote match: strip
// the marks that live inside a word (harakat through Quranic annotation,
// tatweel, the direction chameleons) and fold the letters Arabic spells
// several ways onto one (alef family onto alef, alef maqsura and Farsi yeh
// onto yeh). ASCII and the other scripts ride untouched.
static void AiArSkeleton(const std::string& w, std::string& out) {
    out.clear(); out.reserve(w.size());
    for (size_t i = 0; i < w.size(); ) {
        unsigned char u = (unsigned char)w[i];
        int len = 0;
        unsigned cp = AiUtf8Cp(w, i, len);
        if (u < 0x80) { out.append(w, i, 1); i++; continue; }
        bool strip = (cp >= 0x610 && cp <= 0x61A) ||   // Quranic annotation signs
                     cp == 0x640 ||                    // tatweel
                     (cp >= 0x64B && cp <= 0x65F) ||   // harakat through small ya
                     cp == 0x670 ||                    // superscript alef
                     (cp >= 0x6D6 && cp <= 0x6ED) ||   // Quranic marginal marks
                     cp == 0x6DD || cp == 0xFEFF;
        if (strip) { i += (size_t)len; continue; }
        if (cp == 0x622 || cp == 0x623 || cp == 0x625 || cp == 0x671)
            cp = 0x627;                                // madda/hamza alefs -> alef
        else if (cp == 0x649 || cp == 0x6CC)
            cp = 0x64A;                                // alef maqsura / Farsi yeh -> ya
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 63));
        } else {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 63));
            out += (char)(0x80 | (cp & 63));
        }
        i += (size_t)len;
    }
}
// LocateInChunk: Pass A the quote's consecutive run - byte-equal first, then
// the Arabic skeleton (the model's plain copy against the page's decorated
// words); Pass B the tight in-order subsequence: the needle's own words fill
// the window, a reshaped needle word is skipped only within a small budget,
// the stream words between matches stay within a few, and the whole window
// stays quote-sized - a needle of common words that scatters across a page
// must NOT be trusted to light the page (the span guard is what kept the old
// pass B's wide walk from painting a bigger blob than the chunk box it
// replaced). The window's end is the LAST matched word - the old pass B wrote
// first+1, stranding every geometric reader of the window on the first word's
// page. The same matcher rides both sides of the citation: the chunk locator
// here and the highlight's page matcher in ai_ui2.inl.
enum AiQuoteHit { AQH_NONE = 0, AQH_EXACT, AQH_APPROX };
static AiQuoteHit AiLocateWords(const std::vector<std::string>& chunk,
                                const std::vector<std::string>& needle,
                                size_t& startOut, size_t& endOut) {
    startOut = endOut = 0;
    if (needle.empty() || chunk.empty() || needle.size() > chunk.size())
        return AQH_NONE;
    // the skeletons once per stream (byte equality stays the fast path)
    std::vector<std::string> skC(chunk.size()), skN(needle.size());
    for (size_t i = 0; i < chunk.size(); i++) AiArSkeleton(chunk[i], skC[i]);
    for (size_t i = 0; i < needle.size(); i++) AiArSkeleton(needle[i], skN[i]);
    // Pass A: the exact consecutive run - byte-equal first, skeleton-equal
    // next; the first home wins
    for (int wantSk = 0; wantSk < 2; wantSk++) {
        for (size_t i = 0; i + needle.size() <= chunk.size(); i++) {
            bool eq = true;
            for (size_t j = 0; j < needle.size(); j++) {
                const std::string& a = wantSk ? skC[i + j] : chunk[i + j];
                const std::string& b = wantSk ? skN[j] : needle[j];
                if (a != b) { eq = false; break; }
            }
            if (eq) { startOut = i; endOut = i + needle.size(); return AQH_EXACT; }
        }
    }
    // Pass B: the tight in-order subsequence (the needle carries >= 3 words)
    if (needle.size() < 3) return AQH_NONE;
    const size_t maxSkip = needle.size() / 5;        // the model reshaped a few words
    const size_t maxGap = 3;                         // stream words a wrap may interleave
    const size_t minMatch = max<size_t>(3, (size_t)ceil(0.75 * (double)needle.size()));
    const size_t maxSpan = needle.size() * 2 + 4;    // the window stays quote-sized
    bool bestSet = false;
    size_t bFirst = 0, bLast = 0, bMatched = 0, bSpan = 0;
    for (size_t s = 0; s + minMatch <= chunk.size(); s++) {
        size_t k = 0, matched = 0, skipped = 0;
        size_t first = (size_t)-1, last = (size_t)-1;
        for (size_t i = s; i < chunk.size() && k < needle.size(); i++) {
            // the walk dies where a quote does not reach: before the first
            // match the window cannot grow past its own length; after it the
            // gap between matched words stays a wrap's width
            if (first == (size_t)-1) { if (i - s > maxSpan) break; }
            else if (i - last > maxGap + 1) break;
            size_t kk = k, use = 0;
            while (kk < needle.size() &&
                   !(chunk[i] == needle[kk] || skC[i] == skN[kk])) {
                kk++; use++;
                if (use > maxSkip) break;
            }
            if (kk < needle.size() &&
                (chunk[i] == needle[kk] || skC[i] == skN[kk])) {
                if (first == (size_t)-1) first = i;
                matched++; skipped += use; k = kk + 1; last = i;
            }
        }
        if (k == needle.size() && matched >= minMatch) {
            size_t span = last - first + 1;
            if (span <= maxSpan &&
                (!bestSet || matched > bMatched ||
                 (matched == bMatched &&
                  (span < bSpan || (span == bSpan && first < bFirst))))) {
                bestSet = true; bFirst = first; bLast = last;
                bMatched = matched; bSpan = span;
            }
        }
    }
    if (bestSet) { startOut = bFirst; endOut = bLast + 1; return AQH_APPROX; }
    return AQH_NONE;
}
static AiQuoteHit AiLocateInText(const std::string& chunkText, const std::string& quote,
                                 size_t& startOut, size_t& endOut) {
    std::vector<std::string> cw, nw;
    AiWordize(chunkText, cw);
    AiWordize(quote, nw);
    return AiLocateWords(cw, nw, startOut, endOut);
}
// The cited chunk's own words on one page. The chunk's text is its words
// joined with single spaces (the chunker's Flush) and a page word never
// carries a space, so the split below walks the chunker's own word sequence
// one for one. Each word rides the wordizer on its own - re-wordizing the
// joined text would drift the indices wherever a word's punctuation vanishes
// (AiWordize sheds ASCII punctuation and the Arabic split marks: a "-" word
// yields nothing, "12:30" yields "1230") - and empty tokens drop out of the
// needle while the chunker's indices stay exact. The join rebuilds a
// quote-shaped needle the page matcher (AiLocateWords over the page's live
// cells) reads like any other quote - the WHOLE passage the answer drew from,
// not the 120-byte head the quote field carries.
static std::string AiSliceWords(const std::string& chunkText, long long wordStart, long long wordEnd) {
    if (wordEnd <= 0 || chunkText.empty()) return "";
    std::string out;
    long long idx = 0;
    size_t i = 0, n = chunkText.size();
    while (i < n && idx < wordEnd) {
        size_t j = chunkText.find(' ', i);
        if (j == std::string::npos) j = n;
        if (idx >= wordStart && j > i) {
            std::vector<std::string> w;
            AiWordize(chunkText.substr(i, j - i), w);
            for (auto& t : w) { if (!out.empty()) out += ' '; out += t; }
        }
        idx++;
        i = j + 1;
    }
    return out;
}
// PassageQuoteValidator: >= 8 normalized letters, else unverified. The join
// rides the Arabic skeleton - a decorated chunk against the model's plain
// quote still reads as the same letters (the byte join failed every harakat
// and the quote landed unverified, which sent the highlight to the span box).
static bool AiQuotePlausible(const std::string& quote, const std::string& chunkText) {
    std::string qj, cj, sk;
    {
        std::vector<std::string> w;
        AiWordize(quote, w);
        for (auto& x : w) { AiArSkeleton(x, sk); qj += sk; }
        AiWordize(chunkText, w);
        for (auto& x : w) { AiArSkeleton(x, sk); cj += sk; }
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
// SegmentHead: the first <= 120 bytes of the normalized segment, word-bounded.
// The cut walks back to a UTF-8 lead byte first - a 120-byte resize mid-
// codepoint snapped an Arabic letter in half and the fragment rode into the
// quote as a token no page word could ever equal (the backfilled citation's
// highlight fell to the span box on a byte that belongs to no word).
static std::string AiSegmentHead(const std::string& seg) {
    std::string n = AiNormalizeForMatch(seg);
    if (n.size() <= 120) return n;
    size_t cut = 120;
    while (cut > 0 && ((unsigned char)n[cut] & 0xC0) == 0x80) cut--;   // not a lead byte
    n.resize(cut);
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
    // 1200-char targets: the deck used to split at 500 CHARS, so a 100-page
    // book came out as ~14,000 slivers and the embed pass flew ~55 wide
    // batches where 23 carry the same book - and retrieval wants whole
    // paragraphs anyway. Matches the web splitter's target above.
    const size_t Target = 1200, Max = 2400, MinWords = 20, Overlap = 50;

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
// Page words from docsy's text cells (the engine text layer's letters, OCR's
// word boxes on scanned pages): words in reading order with paragraph /
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
// The extraction pipeline's build stamp. Bumped whenever a change alters what
// a page yields: an index chunked by an older build captured that build's
// extraction (parked shells, OCR fragments over native pages) and its disk
// cache would otherwise ride forever - the file's bytes never changed, so the
// hash check passed, and the sidechat answered from junk while the
// summarizer read the same pages fine. v1 is the native-first pump/arrival
// era (the commit that stopped the pumps parking empty pages and the OCR
// landing overwriting the engine text layer).
static const int AI_EXTRACT_VERSION = 1;
struct AiDocIndex {
    std::string docId;
    std::wstring path;
    int pageCount = 0;
    int extractVersion = 0;                     // AI_EXTRACT_VERSION at build time;
                                                // 0 = a legacy cache (pre-stamp) -
                                                // always stale, it cannot speak for
                                                // what the current extraction reads
    int emptyPages = 0;                         // pages that contributed zero words at
                                                // build time: parked shells, blanks,
                                                // flights that failed. A cache whose
                                                // document was mostly unread to it
                                                // must not pose as the document
    std::vector<AiChunk> chunks;
    std::vector<std::vector<float>> vectors;    // parallel to chunks (may be empty)
    int dim = 0;
    std::string embModel;                       // the vectors' identity
    std::string prefixKey;
    std::string contentHash;
    bool semComplete = false;                   // every chunk landed or stays home refused -
                                                // a partial seat must rebuild (resume), the
                                                // old "vectors exist" check called a half-fed
                                                // seat done
    std::vector<unsigned char> semRefused;      // parallel to chunks: 1 = width-1 refusal,
                                                // stays keyword-only, a resume skips it
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
    std::string refused;
    for (size_t i = 0; i < ix.semRefused.size(); i++) {
        if (i) refused += ",";
        refused += ix.semRefused[i] ? "1" : "0";
    }
    std::string j = "{\"docId\":" + WxJsonStr(WxU(ix.docId)) +
        ",\"path\":" + WxJsonStr(ix.path) +
        ",\"pageCount\":" + std::to_string(ix.pageCount) +
        ",\"extractVersion\":" + std::to_string(ix.extractVersion) +
        ",\"emptyPages\":" + std::to_string(ix.emptyPages) +
        ",\"dim\":" + std::to_string(ix.dim) +
        ",\"embModel\":" + WxJsonStr(WxU(ix.embModel)) +
        ",\"prefixKey\":" + WxJsonStr(WxU(ix.prefixKey)) +
        ",\"contentHash\":" + WxJsonStr(WxU(ix.contentHash)) +
        ",\"semComplete\":" + (ix.semComplete ? "1" : "0") +
        ",\"semRefused\":[" + refused + "],\"chunks\":[";
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
    // the vectors ride their own float32 file (dim + count header); its
    // failure must SPEAK - a silent miss here is a re-embed nobody ordered:
    // the chunks reload fine and the up-to-date check sees no vectors
    bool binOk = true;
    if (!ix.vectors.empty() && ix.dim > 0) {
        std::vector<unsigned char> bin;
        unsigned hdr[2] = { (unsigned)ix.dim, (unsigned)ix.vectors.size() };
        bin.insert(bin.end(), (unsigned char*)hdr, (unsigned char*)hdr + 8);
        for (auto& v : ix.vectors) {
            if ((int)v.size() == ix.dim)
                bin.insert(bin.end(), (const unsigned char*)v.data(),
                           (const unsigned char*)v.data() + v.size() * sizeof(float));
            else
                // refused / not-flown slot rides dim zeros: the file must stay
                // full-size or the loader's need-check rejects the whole index
                bin.insert(bin.end(), (size_t)ix.dim * sizeof(float), 0);
        }
        binOk = WxWriteFile(AiVecPath(ix.docId), bin.data(), bin.size());
    }
    return binOk;
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
    // the index rides a 1GB read cap: a big book's chunk deck crosses the
    // default 64MB guard, and a cache that exists but can never load back is
    // the re-embed-every-session report wearing a disguise
    if (!WxReadFile(AiIndexPath(docId), s, 1024ll << 20)) return false;
    crxk::J j;
    if (!crxk::JParse(s, j) || j.t != 'o') return false;
    ix.docId = docId;
    const crxk::J* p;
    if ((p = j.get("path")) && p->t == 's') ix.path = WxW(p->s);
    if ((p = j.get("pageCount")) && p->t == 'd') ix.pageCount = (int)p->d;
    if ((p = j.get("extractVersion")) && p->t == 'd') ix.extractVersion = (int)p->d;
    if ((p = j.get("emptyPages")) && p->t == 'd') ix.emptyPages = (int)p->d;
    if ((p = j.get("dim")) && p->t == 'd') ix.dim = (int)p->d;
    if ((p = j.get("embModel")) && p->t == 's') ix.embModel = p->s;
    if ((p = j.get("prefixKey")) && p->t == 's') ix.prefixKey = p->s;
    if ((p = j.get("contentHash")) && p->t == 's') ix.contentHash = p->s;
    if ((p = j.get("semComplete")) && p->t == 'd' && p->d != 0) ix.semComplete = true;
    const crxk::J* srf = j.get("semRefused");
    if (srf && srf->t == 'a') {
        ix.semRefused.assign(srf->a.size(), 0);
        for (size_t i = 0; i < srf->a.size(); i++)
            if (srf->a[i].t == 'd' && srf->a[i].d != 0) ix.semRefused[i] = 1;
    }
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
    // vectors: their own float32 file (dim + count header). The read rides the
    // 1GB cap too - the bin is dim x chunks x 4 bytes and a real book at 4096
    // dims crosses 64MB near 4,000 chunks, where the default guard made the
    // cache write-any-read-never: loaded chunks, empty vectors, full re-embed.
    std::string binS;
    std::vector<unsigned char> bin;
    if (ix.dim > 0 && WxReadFile(AiVecPath(docId), binS, 1024ll << 20)) {
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
                // an all-zero row is the save's own "never landed" mark, not an
                // embedding: restore it as empty so the resume check sees the
                // gap it must fly (a real embedder never answers all zeros)
                bool zero = true;
                for (int f = 0; f < hdr[0]; f++)
                    if (ix.vectors[i][f] != 0.0f) { zero = false; break; }
                if (zero) ix.vectors[i].clear();
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
    for (size_t i = 0; i < q.size(); ) {
        unsigned char u = (unsigned char)q[i];
        int len = 0;
        unsigned cp = AiUtf8Cp(q, i, len);
        if (u < 0x80) {
            if (isalnum(u)) cur += (char)tolower(u);
            else push();
            i++;
            continue;
        }
        if (AiCpSplits(cp)) push();       // the same split rule AiWordize runs:
        else cur.append(q, i, (size_t)len);  // a question mark must not ride into
        i += (size_t)len;                    // the last word it lands on
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
                if ((p = m.get("dur")) && p->t == 'd') msg.durMs = (int)p->d;
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
                        // the cited span's box, so a reloaded answer's chips
                        // still light their chunk's words on the page
                        const crxk::J* rr = e.get("rects");
                        if (rr && rr->t == 'a')
                            for (auto& rq : rr->a) {
                                if (rq.t != 'a' || rq.a.size() < 4) continue;
                                src.rects.push_back({ (float)rq.a[0].d, (float)rq.a[1].d,
                                                      (float)rq.a[2].d, (float)rq.a[3].d });
                            }
                        // the chunk's own words per page: a reloaded answer's
                        // chips light their full passages, not the quote head
                        const crxk::J* pp = e.get("passages");
                        if (pp && pp->t == 'a')
                            for (auto& pq : pp->a) {
                                if (pq.t != 'a' || pq.a.size() < 2) continue;
                                if (pq.a[0].t != 'd' || pq.a[1].t != 's') continue;
                                src.passages.push_back({ (int)pq.a[0].d, pq.a[1].s });
                            }
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
                 ",\"ts\":" + std::to_string(m.ts) +
                 ",\"dur\":" + std::to_string(m.durMs) + ",\"sources\":[";
            for (size_t s = 0; s < m.sources.size(); s++) {
                auto& src = m.sources[s];
                if (s) j += ",";
                j += "{\"id\":" + WxJsonStr(WxU(src.id)) +
                     ",\"page\":" + std::to_string(src.page) +
                     ",\"pageIndex\":" + std::to_string(src.pageIndex) +
                     ",\"quote\":" + WxJsonStr(WxU(src.quote)) +
                     ",\"reason\":" + WxJsonStr(WxU(src.reason)) +
                     ",\"verified\":" + (src.verified ? "true" : "false") +
                     ",\"location\":" + std::to_string(src.location) + ",\"rects\":[";
                for (size_t q = 0; q < src.rects.size(); q++) {
                    if (q) j += ",";
                    j += "[" + std::to_string(src.rects[q].x) + "," +
                         std::to_string(src.rects[q].y) + "," +
                         std::to_string(src.rects[q].w) + "," +
                         std::to_string(src.rects[q].h) + "]";
                }
                j += "]}";
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
