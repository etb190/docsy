// ---------------- docsy AI: panel messages, workers, summary window ----------------
// The second half: the panel's postMessage switch, the index/semantic/notes
// pumps, the Page-Summary navigator window (Features/Summary/SummaryWindow +
// WebSummaryWindow in one frame), the citation jump, and the Axo notes save.
#pragma once

static void AiPanelMessage(const wchar_t* json);
static void SumPanelMessage(const wchar_t* json, int seat);
static void SumPostSeat(int seat, const std::string& j);
static void SumPost(const std::string& j);
static void SumHideSeat(int seat, bool fromOutside);   // the popup contract: a click
                                                       // outside its borders folds
                                                       // it, alive
static void SumOpenSeat(int seat);
static void SumEnsureSeat(int seat);
static void SumSavePlacementSeat(int seat);
static void SumPrewarm();        // the idle seats: overlays build hidden
static void SumSavePlacementSeat(int seat);   // one settings write per gesture
static void SumSeatBoundsRead(int seat);      // the overlay card's seat, read from
                                              // settings and clamped - defined below,
                                              // the controller's completion calls it

#include <mmsystem.h>            // PlaySound - the fold's chime lives below

// ---- the completion chime, native ----
// The page's Web Audio chime only answers while the webview renders: a folded
// popup (put_IsVisible FALSE, the page visibility "hidden") gives no guarantee
// its AudioContext breathes, and the user's rule is the chime rides whenever a
// run or a buffer lands - window open or not. So the fold's own ear: the same
// two-tone D5 -> A5 the page plays, synthesized once into a 16-bit mono WAV
// image and handed to PlaySound. No asset, no dependency beyond winmm.
static void SumPingNative() {
    static unsigned char* wav = nullptr;    // process-lifetime: SND_ASYNC keeps
                                            // playing against it after we return
    if (!wav) {
        const DWORD rate = 22050;
        const double dur = 0.55, d5 = 587.33, a5 = 880.0, peak = 0.4;
        const DWORD samples = (DWORD)(rate * dur);
        const DWORD dataLen = samples * 2;
        const DWORD fileLen = 36 + dataLen;
        unsigned char* w = new unsigned char[44 + dataLen];
        if (!w) return;
        auto put32 = [&](DWORD v, int at) {
            w[at] = v & 255; w[at + 1] = (v >> 8) & 255;
            w[at + 2] = (v >> 16) & 255; w[at + 3] = (v >> 24) & 255;
        };
        auto put16 = [&](DWORD v, int at) {
            w[at] = v & 255; w[at + 1] = (v >> 8) & 255;
        };
        memcpy(w, "RIFF", 4); put32(fileLen, 4); memcpy(w + 8, "WAVE", 12);
        memcpy(w + 12, "fmt ", 4); put32(16, 16); put16(1, 20); put16(1, 22);
        put32(rate, 24); put32(rate * 2, 28); put16(2, 32); put16(16, 34);
        memcpy(w + 36, "data", 4); put32(dataLen, 40);
        short* s = (short*)(w + 44);
        for (DWORD i = 0; i < samples; i++) {
            double t = (double)i / rate;
            // the page's envelope: gain 0.12 at t=0, exponentialRamp to 0.001
            // at t=0.55 - same shape, scaled to a volume an ear across the
            // room still catches
            double g = peak * std::pow(0.001 / 0.12, t / dur);
            double f = t < 0.12 ? d5 : a5;
            s[i] = (short)(std::sin(6.283185307179586 * f * t) * g * 32767.0);
        }
        wav = w;
    }
    PlaySoundW((LPCWSTR)wav, nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

static DWORD g_sumHideTick[SUM_SEATS] = {};  // when an outside click folded a seat: the
                                             // toolbar's own click must not reopen
static HHOOK g_sumMouseHook = nullptr;       // the click-outside watcher, one hook
                                             // serving both seats while any is open
static std::string g_sumInitDoc[SUM_SEATS];  // what each page's last init was built
                                             // for - a reopen only re-inits when the
                                             // document moved under the hood

// ---- the page text builder: [p. N] anchored, running paragraphs ----
static std::wstring AiPageTextOf(const TxtPage& pt) {
    if (pt.cells.empty()) return L"";
    std::map<int, std::wstring> wt;
    std::map<int, std::array<float, 4>> wbox;
    for (auto& c : pt.cells) {
        auto& bx = wbox[c.word];
        if (bx[2] == 0 && bx[3] == 0) bx = { c.l, c.t, c.r, c.b };
        else { bx[0] = min(bx[0], c.l); bx[1] = min(bx[1], c.t);
               bx[2] = max(bx[2], c.r); bx[3] = max(bx[3], c.b); }
        wt[c.word] += c.text;
    }
    std::map<int, std::vector<int>> byLine;
    for (auto& c : pt.cells) byLine[c.line].push_back(c.word);
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

// ---- the OCR flight: one page's text layer built OFF the UI thread ----
// TtFor builds synchronously - fine for a cursor or a find, and brutal for a
// sixty-page summary: Windows' OCR over a fresh 2x software render costs
// 100-300ms a page ON THE CALLING THREAD, and the extraction pump calling it
// froze the message loop for the whole range. The app hitched, and the
// low-level mouse hook riding that loop stalled the machine's pointer with
// it. The flight keeps BOTH halves off this thread now: the render rides the
// render worker's queue (the same gate and the same thread every display
// raster uses - the old flight still rendered its 2x input right here, a
// 50-250ms hitch per page under the render gate, and the machine stuttered
// its way through every reading phase), and the recognition runs on its own
// async completion. The pump parks, the message loop breathes, and the
// arrivals re-kick whoever was waiting. One page in flight at a time - both
// pumps park on the same flight.
struct AiOcrFlight {
    bool busy = false;
    std::wstring path;
    int docId = 0;                       // the render queue's stable handle: the
                                         // drain's OCR results match against it
    int page = -1;
    int ow = 0, oh = 0;                  // the raster the flight asked for - the
                                         // payload's boxes are native to it
    DWORD born = 0;
};
static AiOcrFlight g_ocrFlight;
static void AiOcrArrive(AiOcrPayload* pl);   // lands on the UI thread (WM_APP + 72);
                                             // defined below, beside the pumps it re-kicks

static bool AiOcrPrewarm(DocState& D, int pageIdx) {
    if (g_ocrFlight.busy) return true;      // a page is already flying: the caller parks
    auto eng = OcrEngineGet();
    if (!eng || !D.n || pageIdx < 0 || pageIdx >= D.n) return false;
    if (!D.id || !D.doc) return false;      // no render seat to ride (never for a
                                            // live document; the guard keeps the
                                            // flight from parking on a dead ask)
    int ow = std::clamp((int)std::lround(D.pw[pageIdx] * 2), 800, 1600);
    int oh = max(1, (int)std::lround(ow * D.ph[pageIdx] / max(1.f, D.pw[pageIdx])));
    int rot = D.prot.count(pageIdx) ? D.prot[pageIdx] : 0;
    int flip = D.pflip.count(pageIdx) ? D.pflip[pageIdx] : 0;
    g_ocrFlight.busy = true;
    g_ocrFlight.path = D.path;
    g_ocrFlight.docId = D.id;
    g_ocrFlight.page = pageIdx;
    g_ocrFlight.ow = ow; g_ocrFlight.oh = oh;
    g_ocrFlight.born = GetTickCount();
    RqEnqueue(D, pageIdx, RJ_OCR, ow, oh, rot, flip);   // the render rides the
                        // worker's queue like every other raster; the drain
                        // lands it in AiOcrFlightArrive, the recognition
                        // still runs away from this thread, and this thread
                        // gives the machine its frame budget back
    return true;   // parked; the drain's arrival re-kicks the waiting pump
}

static void AiOcrFlightArrive(int docId, int page,
                              winrt::Windows::Graphics::Imaging::SoftwareBitmap soft);
                             // the drain's landing - the recognition starts there;
                             // defined below, beside AiOcrArrive and the pumps it re-kicks

// ---- the range extraction pump (one OCR page per tick) ----
struct AiRangeJob {
    bool active = false;
    std::wstring path;
    int first = 0, last = 0, cur = 0;
    std::wstring text;
    int what = 0;                        // 0 summary, 1 notes, 2 beaker, 3 buffer
    unsigned long long gen = 0;
    int words = 1000;
    std::string lang = "English", genre = "nonfiction_classic";
    bool bypass = false;
};
static AiRangeJob g_range;

// The drain landed the flight's raster: the recognition starts HERE - one
// async call, never a wait - and the boxes ride home on WM_APP + 72 exactly
// as they always have. A render that failed parks its page empty through the
// same arrival (ok=false): re-firing would just re-queue the same failing
// raster forever. A result nobody waits for (the document died mid-flight,
// the seat moved on) frees the flight and re-kicks the waiting pumps itself.
static void AiOcrFlightArrive(int docId, int page,
                              winrt::Windows::Graphics::Imaging::SoftwareBitmap soft) {
    DocState* D = DocById(docId);
    bool ours = g_ocrFlight.busy && g_ocrFlight.docId == docId &&
                g_ocrFlight.page == page && D && !D->path.empty();
    if (!ours) {
        g_ocrFlight.busy = false;
        g_ocrFlight.page = -1;
        if (g_range.active) SetTimer(g.main, 4714, 1, nullptr);
        if (g_aiIndexing) SetTimer(g.main, 4713, 1, nullptr);
        return;
    }
    AiOcrPayload* pl = new AiOcrPayload();
    pl->path = D->path;
    pl->page = page;
    pl->ow = g_ocrFlight.ow; pl->oh = g_ocrFlight.oh;
    if (!soft) {   // the render failed: the page reads empty, the pump moves on
        pl->ok = false;
        if (g.main) PostMessageW(g.main, WM_APP + 72, 0, (LPARAM)pl);
        else delete pl;
        return;
    }
    pl->ok = true;
    try {
        auto op = OcrEngineGet().RecognizeAsync(soft);
        op.Completed([pl](auto&& async, auto&&) {
            try {
                auto res = async.GetResults();
                for (winrt::Windows::Media::Ocr::OcrLine line : res.Lines())
                    for (winrt::Windows::Media::Ocr::OcrWord wd : line.Words()) {
                        auto r = wd.BoundingRect();
                        if (r.Width <= 0 || r.Height <= 0 || wd.Text().empty()) continue;
                        TxtCell c;
                        c.text = std::wstring(wd.Text());
                        c.l = r.X; c.t = r.Y; c.r = c.l + r.Width; c.b = c.t + r.Height;
                        pl->cells.push_back(std::move(c));
                    }
                pl->ok = true;
            } catch (...) { pl->ok = false; }
            if (g.main) PostMessageW(g.main, WM_APP + 72, 0, (LPARAM)pl);
            else { delete pl; g_ocrFlight.busy = false; }   // no frame left to land on
        });
    } catch (...) {   // the engine pack vanished between the ask and the start:
        pl->ok = false;                      // the page parks empty, the pump moves on
        if (g.main) PostMessageW(g.main, WM_APP + 72, 0, (LPARAM)pl);
        else delete pl;
    }
}
static void AiOcrArrive(AiOcrPayload* pl) {
    std::wstring path = std::move(pl->path);
    int page = pl->page, ow = pl->ow, oh = pl->oh;
    bool ok = pl->ok;
    std::vector<TxtCell> cells = std::move(pl->cells);
    delete pl;
    g_ocrFlight.busy = false;
    g_ocrFlight.page = -1;
    DocState& D = DVF();
    if (!path.empty() && path == D.path && page >= 0 && page < D.n && !D.txt.count(page)) {
        TxtPage pt; pt.w = D.pw[page]; pt.h = D.ph[page];
        if (ok) {
            float kx = pt.w / max(1, ow), ky = pt.h / max(1, oh);
            for (auto& c : cells) {
                TxtCell s;
                s.text = std::move(c.text);
                s.l = c.l * kx; s.t = c.t * ky; s.r = c.r * kx; s.b = c.b * ky;
                pt.cells.push_back(std::move(s));
            }
            TtLines(pt);
            TtSplitChars(pt);   // word boxes -> KillerPDF's letter-granular cells
        }
        pt.tick = ++D.tick;
        D.txt[page] = std::move(pt);
        TtTrim(D, page);
    }
    // a failed flight parks an empty page (the pump reads L"" and moves on);
    // a flight whose document died just frees its seat - the living pump for
    // the current document re-fires its own prewarm on its next tick
    if (g_range.active) SetTimer(g.main, 4714, 1, nullptr);   // the parked pump resumes now
    if (g_aiIndexing) SetTimer(g.main, 4713, 1, nullptr);
}
// the buffer's deferred seat: the page's prefetch request waits a beat after
// the popup unfolds (the first paint lands unmolested), then rides the pump
static struct {
    bool on = false;
    int first = 0, pages = 0, words = 1000;
    std::string lang = "English", genre = "nonfiction_classic";
} g_sumPf;

static void AiRangeStart(const std::wstring& path, int first, int last, int what,
                         unsigned long long gen) {
    g_range.active = true;
    g_range.path = path;
    g_range.first = first; g_range.last = last; g_range.cur = first;
    g_range.text.clear();
    g_range.what = what;
    g_range.gen = gen;
    SetTimer(g.main, 4714, 30, nullptr);   // the tick itself re-cadences: its
                                           // idle tail scales with what the
                                           // pages actually cost (see AiRangeTick)
}
static void AiRangeTick() {
    if (!g_range.active) { KillTimer(g.main, 4714); return; }
    if ((g_range.what == 0 || g_range.what == 3) && !g_sumCtr[SUM_DOC]) return;
    if (g_range.what == 1 && !g_sideNotes) return;
    if (g_range.what == 2 && !g_sumCtr[SUM_DOC]) return;   // the beaker's range test rides the summary overlay
    DocState& D = DVF();
    if (D.path.empty() || D.path != g_range.path) {
        g_range.active = false;
        KillTimer(g.main, 4714);
        return;
    }
    // a flight that never landed (a hung engine, a lost message): 30s frees
    // the seat so the pump re-fires instead of waiting forever
    if (g_ocrFlight.busy && GetTickCount() - g_ocrFlight.born > 30000)
        g_ocrFlight.busy = false;

    DWORD startTick = GetTickCount();
    bool parked = false;
    while (g_range.cur <= g_range.last) {
        int p = g_range.cur; // 1-based page number
        auto it = D.txt.find(p - 1);              // 0-based page index
        if (it == D.txt.end()) {
            // The page's text layer is not built: fire the OCR flight (its
            // render is the only this-thread step) and PARK - the old pump
            // called TtFor here and froze the message loop 100-300ms per page,
            // the machine-wide lag the reading phase was famous for. The
            // arrival re-kicks this pump in 1ms.
            if (AiOcrPrewarm(D, p - 1)) { parked = true; break; }
            // no engine / render failed: park an empty page, read L"", move on
            TxtPage empty; empty.w = D.pw[p - 1]; empty.h = D.ph[p - 1];
            empty.tick = ++D.tick;
            it = D.txt.emplace(p - 1, std::move(empty)).first;
            TtTrim(D, p - 1);
        }
        std::wstring t = AiPageTextOf(it->second);
        if (!t.empty()) g_range.text += L"[p. " + std::to_wstring(p) + L"]\n" + t + L"\n\n";
        g_range.cur++;

        // Surface extraction progress to UI
        if (g_range.what == 0) {
            SumPost("{\"cmd\":\"progress\",\"gen\":" + std::to_string(g_range.gen) +
                    ",\"text\":\"Reading pages " + std::to_string(g_range.first) + "-" +
                    std::to_string(min(g_range.cur - 1, g_range.last)) + " of " +
                    // the ellipsis rides as explicit UTF-8 bytes: a narrow \u escape
                    // folds to the ANSI codepage byte 0x85, and WxW's CP_UTF8 read
                    // turns that into the lone '?' the status bar used to show
                    std::to_string(g_range.last) + "\xe2\x80\xa6\"}");
        } else if (g_range.what == 2) {
            SumPost("{\"cmd\":\"test-progress\",\"gen\":" + std::to_string(g_range.gen) +
                    ",\"text\":\"Reading pages " + std::to_string(g_range.first) + "-" +
                    std::to_string(min(g_range.cur - 1, g_range.last)) + " of " +
                    std::to_string(g_range.last) + "\xe2\x80\xa6\"}");
        } else if (g_range.what == 1 && (p & 3) == 0) {
            AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
            e->flag = true;
            wchar_t b[160];
            _snwprintf_s(b, _TRUNCATE, L"Extracting page %d of %d\u2026", p - g_range.first + 1,
                         g_range.last - g_range.first + 1);
            e->a = WxU(b);
            AiPost(e);
        }
        // the buffer fills in silence: its status line already says
        // "Buffer: generating", and a progress post would stomp it

        // The yield rule: one page minimum; extras only while the tick is
        // still fresh. Cached pages cost microseconds, so a small budget here
        // means the loop breaks between pages - never mid-page.
        if (GetTickCount() - startTick >= 12) break;
    }

    // The idle tail scales with what the ticks actually cost. The old pump
    // (15ms tick, 20ms budget) was a continuous saturate - the message loop
    // never drained, and the low-level mouse hook riding it stalled the whole
    // machine's pointer for as long as the extraction ran. Cached pages keep
    // the run brisk; Start (what=0) leans a little tighter - someone is
    // waiting; the background jobs (1/2/3) yield generously. A parked tick
    // (OCR in flight) re-checks gently - the arrival re-kicks in 1ms anyway,
    // this is only the backstop for a flight that died silently.
    DWORD spent = GetTickCount() - startTick;
    DWORD idle = g_range.what == 0 ? max<DWORD>(30, min<DWORD>(120, spent))
                                   : max<DWORD>(50, min<DWORD>(280, spent * 2));

    if (g_range.cur > g_range.last) {
        g_range.active = false;
        KillTimer(g.main, 4714);
        if (g_range.what == 3) {
            // the buffer's quiet fill: cache first, then a background model
            // thread - nothing here needs the event pump
            DocState& D = DVF();
            if (D.path.empty() || D.path != g_range.path) return;   // a different
                                            // document owns the seat now - the
                                            // page's own re-init reset its buffer
                                            // chip; nothing to tell anyone
            if (g_range.text.empty()) {   // no readable text in the stretch: the
                                          // page must hear the fill ended, or
                                          // "Buffer: generating" hangs forever
                AiEv* pe = AiEvMake(AIEV_SUM_PREFETCH);
                pe->key = std::to_string(g_range.first);
                pe->a = "";
                AiPost(pe);
                return;
            }
            int first = g_range.first, last = g_range.last, words = g_range.words;
            std::string lang = g_range.lang, genre = g_range.genre;
            std::string docId = AiDocSessionKey(D.path);
            std::string model = WxU(g_aiCfg.model);
            std::string variant = lang + ":" + std::to_string(words) + ":" + genre;
            std::wstring rangeText = g_range.text;
            unsigned char sha[32];
            crxk::Sha256((const unsigned char*)rangeText.data(),
                         rangeText.size() * sizeof(wchar_t), sha);
            static const char* hx = "0123456789abcdef";
            std::string hash(64, '0');
            for (int i = 0; i < 32; i++) { hash[i * 2] = hx[sha[i] >> 4]; hash[i * 2 + 1] = hx[sha[i] & 15]; }
            std::string cached;
            if (AiCacheGet(docId, first, last, model, variant, hash, cached)) {
                // a lookup, not work: it lands the same breath the request
                // left - no chime, the done chime for the digest on screen
                // just played (the buffer's chime is for a fill that took
                // real time, and an instant replay of it is noise)
                SumPost("{\"cmd\":\"prefetch-ready\",\"start\":" + std::to_string(first) +
                        ",\"text\":" + WxJsonStr(cached) + ",\"ping\":false}");
            } else {
                // The fill never refused to run again: the old !g_sumBusy gate
                // dropped the whole job whenever a summary was streaming, and
                // the page kept saying "Buffer: generating" over a buffer that
                // would never come. The model call is independent; two HTTP
                // calls in flight are fine.
                std::thread([docId, rangeText, first, last, words, lang, genre, hash]() {
                    // the worker's guard: an uncaught exception here is
                    // std::terminate - the buffer's "generating" chip would
                    // hang and the app would die with it
                    try {
                        AiLoadSettings();
                        AiCfg cfg = g_aiCfg;
                        std::string sys = PromptBodyOf(genre.c_str());
                        if (sys.empty()) sys = PromptBodyOf("nonfiction_classic");
                        {
                            std::string ph = std::to_string(words);
                            size_t pos;
                            while ((pos = sys.find("{words}")) != std::string::npos) sys.replace(pos, 7, ph);
                            while ((pos = sys.find("{language}")) != std::string::npos) sys.replace(pos, 10, lang);
                        }
                        int budget = max(cfg.maxTokens, max(10000, 3000 + 4 * words));
                        if (!AiIsLocalHost(WxU(cfg.baseUrl)) && budget > 16384) budget = 16384;
                        std::string user = WxU(rangeText);
                        std::string content;
                        AiError err;
                        std::atomic<bool> cancel{ false };
                        std::string answer;
                        if (AiChatBuffered(cfg, sys, {}, user, budget, nullptr, true, content, err, &cancel) && !content.empty()) {
                            answer = AiExtractStreamingAnswer(content);
                            if (answer.empty()) answer = AiParseStructured(AiStripThinkSpans(content)).answer;
                            if (answer.empty()) answer = content;
                            std::string mdl = WxU(cfg.model);
                            std::string var = lang + ":" + std::to_string(words) + ":" + genre;
                            AiCachePut(docId, first, last, mdl, var, hash, answer);
                        }
                        // the answer rides the pump, never the thread: the old
                        // SumPost here called PostWebMessageAsJson off the UI
                        // thread and the delivery died in the apartment wall - the
                        // cache filled, the page never heard a word of it, and
                        // "Buffer: generating" hung forever. Empty = failed; the
                        // handler turns that into prefetch-fail on the UI thread.
                        AiEv* pe = AiEvMake(AIEV_SUM_PREFETCH);
                        pe->key = std::to_string(first);
                        pe->a = answer;
                        AiPost(pe);
                    } catch (...) {
                        AiEv* pe = AiEvMake(AIEV_SUM_PREFETCH);
                        pe->key = std::to_string(first);
                        pe->a = "";
                        AiPost(pe);
                    }
                }).detach();
            }
            return;
        }
        AiEv* e = AiEvMake(AIEV_EXTRACT_DONE);
        e->a = WxU(g_range.text);
        e->gen = g_range.gen;
        e->key = std::to_string(g_range.what);
        AiPost(e);
        return;
    }
    SetTimer(g.main, 4714, parked ? 90 : idle, nullptr);
}

// ---- the document index pump (OCR one page per tick) ----
static void AiSemanticStart();   // the embedding pass, defined below the pump
static void AiIndexKickoff(const std::wstring& path) {
    if (g_aiIndexing || path.empty()) return;
    AiLoadSettings();
    std::string docId = AiDocSessionKey(path);
    AiDocIndex ix;
    bool stale = false;
    if (AiIndexLoad(docId, ix) && !ix.chunks.empty()) {
        // the file's own bytes decide whether the cache still speaks for it:
        // a re-exported or re-downloaded PDF at the same path re-chunks from
        // zero instead of answering from pages it no longer has. A legacy
        // cache without a recorded hash keeps riding (we cannot know).
        if (!ix.contentHash.empty()) stale = (AiFileHash(path) != ix.contentHash);
    }
    if (!stale && !ix.chunks.empty()) {
        ix.path = path;
        {   // the index swap rides the seat lock: a reply thread's snapshot
            // (or an in-flight merge) must never see a half-assigned index
            std::lock_guard<std::mutex> lk(g_aiIdxMtx);
            g_aiIndex = std::move(ix);
        }
        g_aiIndexReady = true;
        // the cache is the whole point (2026-11): a cached deck re-arms
        // keyword search with zero network, and the embedding pass stays
        // MANUAL - the beaker starts it. The line tells the reader which
        // seat they sit on: fully cached, or text-only with vectors missing.
        g_aiSemStatus =
            (g_aiIndex.semComplete && !g_aiIndex.vectors.empty())
                ? L"Semantic search ready — loaded from cache (no re-embedding)."
                : L"Cached text index loaded — press the beaker to build semantic search.";
        AiPanelSend(WxW(AiStatusJson("status")));
        return;
    }
    g_aiIndexing = true;
    g_aiIndexReady = false;
    g_aiSemWant = false;   // a stale pre-book (press during a build that then
                           // aborted or a doc switch) must not fire THIS build
    g_aiIdxPage = 0;
    g_aiIdxKey = docId;
    g_aiIdxPath = path;
    if (g_aiIdxChunk) { delete g_aiIdxChunk; g_aiIdxChunk = nullptr; }
    g_aiIdxChunk = new AiChunker();
    {
        std::lock_guard<std::mutex> lk(g_aiIdxMtx);
        g_aiIndex = AiDocIndex();
    }
    g_aiIndex.docId = docId;
    g_aiIndex.path = path;
    g_aiIndex.pageCount = DVF().n;
    g_aiIndex.contentHash = AiFileHash(path);
    AiPanelSend(WxW(AiStatusJson("status")));
    // 150ms cadence - see AiRangeStart's note: one dense page's text layer can
    // cost the UI thread 100-200ms, and the 30ms tick turned the build into a
    // continuous saturate. The index is background work; the UI leads.
    SetTimer(g.main, 4713, 150, nullptr);
}
static void AiIndexTick() {
    if (!g_aiIndexing) { KillTimer(g.main, 4713); return; }
    if (!g_aiOn) return;   // the panel's absence pauses the build: the "app runs
                           // at 5 fps with the AI panel closed" report was this
                           // pump grinding pages nobody was asking about
    DocState& D = DVF();
    if (D.path.empty() || D.path != g_aiIdxPath || D.n <= 0) {
        g_aiIndexing = false;
        g_aiSemWant = false;   // the build died: a pre-booked pass must not ride
                               // a later, unrelated landing
        if (g_aiIdxChunk) { delete g_aiIdxChunk; g_aiIdxChunk = nullptr; }
        KillTimer(g.main, 4713);
        AiPanelSend(WxW(AiStatusJson("status")));
        return;
    }
    int p = g_aiIdxPage;
    if (p >= D.n) {
        g_aiIdxChunk->Finish();
        {
            // the build's landing move rides the seat lock - the reply thread's
            // snapshot or a merge may be mid-read of the old chunks
            std::lock_guard<std::mutex> lk(g_aiIdxMtx);
            g_aiIndex.chunks = std::move(g_aiIdxChunk->done);
        }
        delete g_aiIdxChunk; g_aiIdxChunk = nullptr;
        g_aiIndexing = false;
        KillTimer(g.main, 4713);
        g_aiIndexReady = !g_aiIndex.chunks.empty();
        AiIndexSaveAsync(g_aiIndex);   // the deck rides a thread: the JSON build
                                       // for a real book is seconds of UI-thread
                                       // freeze at the exact moment the reader
                                       // starts asking the panel questions
        // the embedding pass is MANUAL now (2026-11): the reader clicks the
        // beaker when they want it - a build that fired the embedding door on
        // every open cost money and minutes nobody asked for. A press DURING
        // the build pre-booked the pass (g_aiSemWant), so that intent still
        // lands without a second click.
        if (g_aiSemWant) {
            g_aiSemWant = false;
            AiSemanticStart();
        } else {
            g_aiSemStatus = L"Document index ready — press the beaker to build semantic search.";
            AiPanelSend(WxW(AiStatusJson("status")));
        }
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
    // the same disease the range pump had: TtFor built the page synchronously
    // and froze the message loop 100-200ms a page while the index built. The
    // async flight parks this pump instead; the arrival re-kicks it in 1ms.
    auto it = D.txt.find(p);
    if (it == D.txt.end()) {
        if (AiOcrPrewarm(D, p)) return;   // parked; the flight's arrival continues
        TxtPage empty; empty.w = D.pw[p]; empty.h = D.ph[p];   // no engine / render failed
        empty.tick = ++D.tick;
        D.txt.emplace(p, std::move(empty));
        TtTrim(D, p);
        it = D.txt.find(p);
    }
    if (it != D.txt.end() && !it->second.cells.empty()) {
        std::vector<AiWordIn> words;
        AiPageWords(it->second, p, words);
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
    AiDocIndex ix;
    {
        // the pass's working copy rides the seat lock - the copy must not
        // straddle a kickoff's reset or a build's landing move
        std::lock_guard<std::mutex> lk(g_aiIdxMtx);
        ix = g_aiIndex;
    }
    AiCfg cfg = g_aiCfg;
    // the NIM doors name their side with input_type - Google's task prefixes
    // would ride INSIDE the text as noise on that dialect, so the nemotron
    // seat speaks raw passage text. The prefix key follows: a seat that
    // switched dialects must rebuild, never mix two vocabularies.
    bool nim = AiEmbIsNimModel(cfg.embeddingModel);
    std::wstring docPrefix = nim ? std::wstring() : cfg.embDocPrefix;
    std::wstring prefixKey = nim ? (std::wstring(L"\x01") + L"nim")
                                 : (cfg.embDocPrefix + L"\x01" + cfg.embQueryPrefix);
    std::string model = WxU(cfg.embeddingModel.empty() ? L"nvidia/nemotron-3.5-embed-8b"
                                                       : cfg.embeddingModel);
    std::thread([ix, cfg, docPrefix, prefixKey, model]() mutable {
        // the guard is the rule for every AI worker: an uncaught exception on a
        // detached thread is std::terminate - the whole app dying with the
        // beaker line frozen. The catch folds the pass and speaks.
        try {
            // the working index rides a shared seat: the lanes below fly batches
            // concurrently and write disjoint vector ranges into one index
            auto pix = std::make_shared<AiDocIndex>(std::move(ix));
            // lane-shared counters: batches interleave, so single-thread ints
            // would race - the counters go atomic, the refusal rides a mutex
            std::atomic<int> skipped{0};             // chunks the door refused at width 1
            std::atomic<bool> anyLanded{false};      // the door proved alive at least once
            std::atomic<size_t> doneEmb{0};          // chunks whose batch fully landed
            std::atomic<int> dimA{0};                // the first real vector's width
            std::mutex refusalM;
            std::wstring refusal;                    // the last refusal's own words
            bool upToDate = (pix->embModel == model && pix->prefixKey == WxU(prefixKey) &&
                             pix->semComplete &&
                             !pix->vectors.empty() &&
                             pix->vectors.size() == pix->chunks.size());
            bool saved = true;   // the completion save's verdict, reported on
                                 // the ready line (a silent disk failure here
                                 // is a re-embed nobody ordered)
            if (!upToDate) {
                size_t total = pix->chunks.size();
                // the warm-up call: one short text at a 10-second cap, twice at
                // most. A door that hangs (a black-holing middlebox, a region
                // gate, a dead key) used to be discovered BY the lanes - each
                // in-flight call burning its full 120-second receive timeout
                // while the line sat silent and every beaker press read
                // "stopped". The probe names the wall in under a minute,
                // before the lanes exist.
                {
                    // the probe rides once per door per session: a rebuild that
                    // follows a fresh wake (a second document, a re-run after a
                    // few flagged chunks) starts instantly instead of paying
                    // the warm-up latency again
                    std::string probeKey = WxU(cfg.baseUrl) + "|" + model;
                    if (g_aiSemProbeKey != probeKey) {
                        AiEv* wk = AiEvMake(AIEV_SEM_STATUS);
                        wk->a = WxU(L"Waking the embedding door…");
                        AiPost(wk);
                        std::vector<std::string> probe;
                        probe.push_back(WxU(docPrefix) + "warm-up");
                        std::vector<std::vector<float>> pv;
                        AiError pe;
                        bool ok = false;
                        for (int a = 0; a < 2 && !ok; a++) {
                            if (g_aiSemCancel.load()) break;
                            ok = AiEmbedBatch(cfg, probe, false, pv, pe, &g_aiSemCancel, 10 * 1000);
                            if (!ok && a == 0) Sleep(400);
                        }
                        if (g_aiSemCancel.load()) {
                            g_aiSemActive = false;
                            AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                            ev->a = WxU(L"Semantic search stopped — press again to rebuild.");
                            AiPost(ev);
                            return;
                        }
                        if (!ok) {
                            g_aiSemActive = false;
                            AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                            ev->a = WxU(L"Semantic search can't start: the embedding door didn't answer "
                                        L"two 10-second warm-up calls — ") + WxU(pe.Friendly()) +
                                    WxU(L" — keyword search stays available. Press the beaker to try again.");
                            AiPost(ev);
                            return;
                        }
                        g_aiSemProbeKey = probeKey;   // woke once: the session believes
                    }
                    anyLanded.store(true);   // the door proved alive (fresh probe or cached):
                                             // width-1 refusals may stay home
                }
                // resume, not wipe: a pass that ended early (stall, the old
                // ten-minute wall, a stop press, a dead network) left its landed
                // vectors on the seat - keep them and fly only the gaps. The
                // deck itself must match (same model, same prefix, same count):
                // a changed doc re-chunks and the count moves, so stale vectors
                // cannot ride a new deck.
                bool resume = (pix->embModel == model && pix->prefixKey == WxU(prefixKey) &&
                               pix->dim > 0 && pix->vectors.size() == total &&
                               pix->semRefused.size() == total);
                if (resume) {
                    size_t have = 0;
                    int ref = 0;
                    for (size_t i = 0; i < total; i++) {
                        if (!pix->vectors[i].empty() || pix->semRefused[i]) have++;
                        if (pix->semRefused[i]) ref++;
                    }
                    doneEmb.store(have);
                    skipped.store(ref);
                    pix->semComplete = false;
                    AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                    ev->a = WxU(L"Resuming semantic search… " +
                                std::to_wstring((unsigned long long)have) + L"/" +
                                std::to_wstring((unsigned long long)total) +
                                L" already in place — flying the rest.");
                    AiPost(ev);
                } else {
                    pix->vectors.assign(total, {});
                    pix->semRefused.assign(total, 0);
                    pix->dim = 0;
                    // the identity rides at pass START, not only at success: a
                    // failed fresh pass must still resume on the next press
                    pix->embModel = model;
                    pix->prefixKey = WxU(prefixKey);
                    pix->semComplete = false;
                }
                // One batch, any size: a door that refuses N inputs on its own
                // limit (400 / 413 / 422) gets the batch HALVED - those are
                // deterministic refusals, so no same-width retry rides first
                // (the old flow let a 4xx fall into the no-vectors reading and
                // burn the pass at full width). A reply that arrives WHOLE and
                // parsed yet holds no vectors is a reader mismatch, not a
                // width problem - it retries the SAME width once and then
                // speaks its verdict. Auth, model and credit failures cure
                // themselves at no width and end the pass on first sight. At
                // width 1 a refusing chunk stays home with keyword-only
                // coverage - one poison chunk must not drown a whole pass -
                // but only once the door has proven alive: before anything has
                // landed, refusals down the width ladder are a wall (region,
                // credits, key) and the pass speaks instead of grinding
                // hundreds of one-chunk calls.
                // the pass's whole clock: a door that starts answering then
                // goes silent mid-book used to walk the halving ladder all
                // night (each hung call burning its receive timeout). Ten
                // quiet minutes end the pass with a verdict instead.
                unsigned long long t0 = GetTickCount64();
                std::atomic<unsigned long long> lastLand{ t0 };   // the watchdog's breath,
                                                                  // refreshed by every landing
                auto slice = [&](auto&& self, size_t b0, size_t b1, int depth, int retries,
                                 AiError& eOut) -> bool {
                    if (g_aiSemCancel.load()) return false;
                    if (GetTickCount64() - lastLand.load() > 180000) {
                        // three minutes with NOTHING landing: the door went silent
                        // mid-book (hung calls still break on their receive timeout
                        // and re-enter here, so the watchdog gets its breath). The
                        // old wall counted ten minutes of TOTAL pass clock and
                        // murdered a healthy big book mid-air at 10,000 of 14,000.
                        eOut.cat = AI_ERR_TIMEOUT;
                        eOut.detail = L"the embedding door went silent — no batch landed for three minutes";
                        return false;
                    }
                    if (GetTickCount64() - t0 > 1800000) {
                        // 30 minutes total on a huge book: the pass ends with its
                        // landed work kept - the next press resumes where this one
                        // stopped
                        eOut.cat = AI_ERR_TIMEOUT;
                        eOut.detail = L"this book outgrew one 30-minute pass — press the beaker to resume where it stopped";
                        return false;
                    }
                    std::vector<std::string> inputs;
                    for (size_t i = b0; i < b1; i++) {
                        // the clamp, not a raw resize: a cut inside a multibyte
                        // sequence ships invalid UTF-8 and the door's 400 rides
                        // every halved retry - one curly quote killed the pass
                        std::string inp = WxU(docPrefix) + pix->chunks[i].text;
                        inputs.push_back(AiClipUtf8(inp, 1200));
                    }
                    std::vector<std::vector<float>> vecs;
                    AiError e;
                    if (AiEmbedBatch(cfg, inputs, false, vecs, e, &g_aiSemCancel) &&
                        vecs.size() == b1 - b0) {
                        // the dim rides the first real vector - blank chunks
                        // come home with empty vectors and must not pin the
                        // seat's dim to 0 (scanned before the move below)
                        if (dimA.load() == 0)
                            for (auto& v : vecs) if (!v.empty()) { dimA.store((int)v.size()); break; }
                        for (size_t i = b0; i < b1; i++) pix->vectors[i] = std::move(vecs[i - b0]);
                        anyLanded.store(true);
                        doneEmb.fetch_add(b1 - b0);
                        lastLand.store(GetTickCount64());
                        AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                        wchar_t b[128];
                        _snwprintf_s(b, _TRUNCATE, L"Building semantic search… %zu/%zu",
                                     doneEmb.load(), total);
                        ev->a = WxU(b);
                        AiPost(ev);
                        return true;
                    }
                    eOut = e;
                    if (g_aiSemCancel.load()) return false;
                    if (e.cat == AI_ERR_MODEL_NOT_FOUND || e.cat == AI_ERR_NOT_SIGNED_IN ||
                        e.cat == AI_ERR_USAGE_LIMIT)
                        return false;                    // no width cures these
                    bool refused = (e.contentFlagged ||
                                    e.status == 400 || e.status == 413 || e.status == 422);
                    if (refused) {
                        // a width refusal or the content screen - NVIDIA's safety
                        // screening rejects the WHOLE batch when ONE chunk trips
                        // it, so the cure is the same: halve until the flagged
                        // chunk stands alone at width 1 and stays home keyword-
                        // only while its siblings land. The old bail at width 4
                        // made the ladder die a few steps short of the cure -
                        // one flagged paragraph killed the whole pass with the
                        // "content safety screening" verdict, after grinding the
                        // generic ladder's hundreds of calls first.
                        if (b1 - b0 <= 1 && anyLanded.load()) {
                            pix->vectors[b0].clear();
                            pix->semRefused[b0] = 1;
                            skipped.fetch_add(1);
                            lastLand.store(GetTickCount64());
                            std::wstring why = e.Friendly();
                            if (why.size() > 240) why.resize(240);
                            {
                                std::lock_guard<std::mutex> lk(refusalM);
                                refusal = (e.status > 0 && why.find(L"HTTP ") == std::wstring::npos)
                                            ? (L"HTTP " + std::to_wstring(e.status) + L" — " + why)
                                            : why;
                            }
                            return true;
                        }
                        if (b1 - b0 <= 1 || !anyLanded.load())
                            return false;                // the wall speaks, the pass ends
                        size_t mid = b0 + (b1 - b0) / 2;
                        return self(self, b0, mid, depth + 1, 0, eOut) &&
                               self(self, mid, b1, depth + 1, 0, eOut);
                    }
                    if (e.shapeMismatch) {               // the reader's mismatch - and the
                        // content screen never rides here: a flagged 200-body rides
                        // contentFlagged above, so a real reader bug still retries
                        // its width once without the ladder eating the clock
                        if (retries < 1)
                            return self(self, b0, b1, depth, retries + 1, eOut);
                        return false;                    // speaks at full width
                    }
                    if (e.cat == AI_ERR_BUSY) {          // 429 / 503: a rate or concurrency
                        // signal - halving the width never cures it (the ladder's
                        // 511 one-line calls make a rate limit WORSE, and they
                        // grind silently for the better part of an hour with the
                        // seat marked active, so every beaker press read
                        // "stopped"). Back off at the SAME width - four patient
                        // breaths, a burst budget recovers without the pass dying
                        // at "a certain point" - then speak the verdict.
                        if (retries < 4) {
                            if (g_aiSemCancel.load()) return false;
                            if (depth == 0) {
                                AiEv* hb = AiEvMake(AIEV_SEM_STATUS);
                                hb->a = WxU(L"Building semantic search… the embedding door is "
                                            L"busy — backing off before the next attempt.");
                                AiPost(hb);
                            }
                            Sleep(retries == 0 ? 1200 : (retries == 1 ? 3000 : 6000));
                            return self(self, b0, b1, depth, retries + 1, eOut);
                        }
                        return false;
                    }
                    if (b1 - b0 <= 1 || depth >= 8) return false;
                    if (retries < 2) {                   // a hiccup (timeout / busy /
                        if (g_aiSemCancel.load()) return false;   // burst) - cancel
                        Sleep(1200);                     // bites between breaths, then
                        return self(self, b0, b1, depth, retries + 1, eOut);   // ride again
                    }
                    size_t mid = b0 + (b1 - b0) / 2;
                    return self(self, b0, mid, depth + 1, 0, eOut) &&
                           self(self, mid, b1, depth + 1, 0, eOut);
                };
                // Batch size rides the door: NIM's own table takes 512+ texts
                // a call; 256 real 1200-byte chunks ride one wide request and
                // the halving ladder above still cures a width refusal.
                // Google's OpenAI door caps a batch at 100 texts / 20000 tokens
                // and keeps 64; a local engine still takes the widest batch.
                bool nimEmb = AiEmbIsNimModel(cfg.embeddingModel);
                bool local = AiIsLocalHost(WxU(cfg.baseUrl));
                size_t batchN = local ? 256 : (nimEmb ? 256 : 64);
                // lanes: the batches used to walk strictly one after another,
                // so a 3,000-chunk book paid the door's per-batch latency once
                // per batch on the clock - the wall time WAS the batch count.
                // The lanes interleave the batches round-robin (lane L flies
                // batches L, L+lanes, ...), each on its own request over the
                // session's pooled connections: the door's compute overlaps
                // instead of queueing behind one call, and a lane stuck in the
                // halving ladder no longer parks the whole pass. Writes stay
                // disjoint by construction, so no lock guards the vectors.
                size_t nb = (total + batchN - 1) / batchN;
                size_t nl = nb < (size_t)4 ? nb : (size_t)4;
                unsigned lanes = local ? 1u : (unsigned)(nl ? nl : 1);
                std::atomic<bool> abortPass{false};
                std::mutex failM;
                AiError failErr;
                bool failed = false;
                // the partial save: the handback below keeps the landed work in
                // the LIVE seat, but a pass that ends early (the stop press, the
                // watchdog, a lane failure) used to leave NOTHING on disk -
                // "resume where it stopped" lived only inside the session, and a
                // book that outgrew the 30-minute wall re-embedded from zero
                // every restart. The landed rows and refusal marks ride to disk;
                // the loader restores them (all-zero rows come back as empty)
                // and the resume branch flies only the gaps.
                auto savePartial = [&]() {
                    pix->dim = dimA.load();
                    pix->semComplete = false;
                    AiIndexSave(*pix);
                };
                auto lane = [&](unsigned laneId) {
                    for (size_t k = laneId; k < nb; k += lanes) {
                        if (abortPass.load() || g_aiSemCancel.load()) return;
                        size_t b0 = k * batchN;
                        size_t b1 = min(b0 + batchN, total);
                        bool spanDone = true;            // resume: this span already flew
                        for (size_t i = b0; i < b1; i++)
                            if (pix->vectors[i].empty() && !pix->semRefused[i]) { spanDone = false; break; }
                        if (spanDone) continue;
                        AiError e;
                        if (!slice(slice, b0, b1, 0, 0, e)) {
                            if (g_aiSemCancel.load()) return;
                            {
                                std::lock_guard<std::mutex> lk(failM);
                                if (!failed) { failed = true; failErr = e; }
                            }
                            abortPass.store(true);   // the other lanes drain, first verdict wins
                            return;
                        }
                    }
                };
                if (lanes <= 1) {
                    lane(0);
                } else {
                    std::vector<std::thread> ts;
                    for (unsigned L = 0; L < lanes; L++) ts.emplace_back(lane, L);
                    for (auto& t : ts) t.join();
                }
                if (g_aiSemCancel.load()) {
                    // the drain just settled: the line used to keep whatever the
                    // toggle last posted - the pre-booked rebuild (second press)
                    // rides this status home to the pump, which starts it here.
                    // A stopped pass also hands its landed work home: the next
                    // press resumes instead of rebuilding from zero.
                    savePartial();
                    pix->dim = dimA.load();
                    AiEv* hb = AiEvMake(AIEV_SEM_INDEX);
                    hb->heavy = pix;
                    AiPost(hb);
                    g_aiSemActive = false;
                    AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                    ev->a = WxU(L"Semantic search stopped — press again to rebuild.");
                    AiPost(ev);
                    return;
                }
                if (failed) {
                    // the landed work rides home even on a failed pass: the live
                    // seat keeps every vector that landed plus the refusal marks,
                    // so the next press RESUMES the book instead of rebuilding it
                    // from zero - ten minutes of embeddings used to die with this
                    // thread's local copy. The disk save makes the resume true
                    // across restarts, not just across presses.
                    savePartial();
                    pix->dim = dimA.load();
                    AiEv* hb = AiEvMake(AIEV_SEM_INDEX);
                    hb->heavy = pix;
                    AiPost(hb);
                    g_aiSemActive = false;
                    AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                    if (failErr.cat == AI_ERR_MODEL_NOT_FOUND) {
                        ev->a = WxU(L"Embedding model " + failErr.model +
                                    L" not found — check the Embedding Model field in AI settings."
                                    L" Using keyword search only.");
                    } else {
                        // the verdict now names itself: the old line buried
                        // every 401 / 429 / 400 under one phrase, and a user
                        // staring at "semantic search unavailable" had no
                        // way to tell WHICH failure held the pass down
                        std::wstring why = failErr.Friendly();
                        // the cap grew from 220: the door's own words now
                        // ride at the detail's tail and the user's status
                        // line IS the diagnosis - never cut them off
                        if (why.size() > 480) why.resize(480);
                        if (failErr.status > 0 && why.find(L"HTTP ") == std::wstring::npos)
                            why += L" (HTTP " + std::to_wstring(failErr.status) + L")";
                        ev->a = WxU(L"Semantic search failed: " + why +
                                    L" — keyword search stays available.");
                    }
                    AiPost(ev);
                    return;
                }
                if (skipped.load() > 0 && skipped.load() >= (int)total) {
                    // every single chunk the door refused: not a ready seat -
                    // speak the wall and stop, the vectors stay unsaved
                    g_aiSemActive = false;
                    AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                    ev->a = WxU(L"Semantic search failed: the embedding door refused every chunk" +
                                (refusal.empty() ? std::wstring() : (L" — " + refusal)) +
                                L" — keyword search stays available.");
                    AiPost(ev);
                    return;
                }
                pix->embModel = model;
                pix->prefixKey = WxU(prefixKey);
                pix->dim = dimA.load();
                pix->semComplete = true;
                saved = AiIndexSave(*pix);
            }
            // the vectors ride home: the pass used to end here with its work in a
            // local copy - the file was saved, the memory dropped, and the live
            // seat's dim stayed 0, so every answer in this session retrieved by
            // keyword alone and the freshly built nemotron embeddings never spoke
            // until the app restarted and the index loaded from disk. The handback
            // event merges them into the live seat on the UI thread.
            g_aiSemActive = false;
            g_aiSemReadyFlag = true;
            AiEv* ev = AiEvMake(AIEV_SEM_INDEX);
            ev->heavy = pix;                     // the lanes' index flies home whole
            AiPost(ev);
            AiEv* st = AiEvMake(AIEV_SEM_STATUS);
            if (skipped.load() > 0)
                st->a = WxU(L"Semantic search ready — " + std::to_wstring(skipped.load()) +
                            L" chunk(s) the embedding door refused stay keyword-only" +
                            (refusal.empty() ? std::wstring()          // lanes joined above
                                             : (L" (last refusal: " + refusal + L")")) +
                            L".");
            else
                st->a = WxU(L"Semantic search ready.");
            if (!saved)
                st->a += WxU(L" — but the save to disk FAILED, so the next session re-embeds this book.");
            AiPost(st);
        } catch (...) {
            g_aiSemActive = false;
            AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
            ev->a = WxU(L"Semantic search stopped after an internal error — "
                        L"keyword search stays available.");
            AiPost(ev);
        }
    }).detach();
}
static void AiSemanticToggle() {
    // every press speaks: the old toggle was silent in all four states - a
    // press during the build, a press while the pass flew, a press with the
    // index not ready, a press with nothing open - and the beaker button read
    // dead no matter which one held it. The destructive asks ride the app's
    // OWN dialog (AskPrompt3): the grain body, the green accent, smack middle
    // of the frame - not a sheet inside the panel. The toggle only ever runs
    // on the UI thread's command path, so the modal pump is safe here.
    if (g_aiSemActive) {
        int r = AskPrompt3(L" - Semantic search",
                           L"The embedding pass is still running. Stop it now? "
                           L"You can resume later from where it stopped.",
                           L"Keep going", L"", L"Stop");
        if (r != 2) return;   // keep going, Esc, the X, a click away: it rides
        // the confirmed stop; a press during the drain pre-books the rebuild.
        // The old toggle re-posted "stopped" on EVERY press while the drain
        // settled - with a stalled door that drain held the seat for minutes,
        // so no press ever rebuilt anything and the line never changed.
        if (!g_aiSemCancel.exchange(true)) {
            AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
            ev->a = WxU(L"Semantic search stopped — press again to rebuild.");
            AiPost(ev);
        } else {
            g_aiSemRestart = true;
            AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
            ev->a = WxU(L"Stopping — the pass rebuilds the moment the door call lets go.");
            AiPost(ev);
        }
        return;
    }
    if (g_aiIndexing) {
        // the press PRE-BOOKS the pass: the chunk build's landing starts it
        // (AiIndexTick) - the intent lands without a second click, but nothing
        // starts on its own
        g_aiSemWant = true;
        AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
        wchar_t b[200];
        // no "ready" in this line: the status pump greps the word to arm the
        // green button, and a still-building seat must never read armed
        _snwprintf_s(b, _TRUNCATE, L"Still indexing the document (page %d of %d) — "
                     L"embedding starts the moment the index is done.",
                     g_aiIdxPage, g_aiIndex.pageCount);
        ev->a = WxU(b);
        AiPost(ev);
        return;
    }
    if (!g_aiIndexReady || g_aiIndex.chunks.empty()) {
        AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
        if (DVF().path.empty()) ev->a = WxU(L"Open a PDF first — semantic search needs a document to embed.");
        else ev->a = WxU(L"The document index is unavailable — semantic search is off for this file.");
        AiPost(ev);
        return;
    }
    // already built? the rebuild is destructive - the saved vectors die, the
    // door flies every page again - so it only happens over a confirmation
    bool built = false;
    {
        std::lock_guard<std::mutex> lk(g_aiIdxMtx);
        built = g_aiIndex.semComplete && !g_aiIndex.vectors.empty();
    }
    if (built) {
        int r = AskPrompt3(L" - Semantic search",
                           L"This document is already embedded. Rebuilding discards "
                           L"the saved index and embeds every page again.",
                           L"Cancel", L"", L"Rebuild");
        if (r != 2) return;   // cancel, Esc, the X, a click away: nothing dies
        // the confirmed wipe: the vectors die in memory and the pass starts
        // from zero (the disk copy dies with the completion save - a crash
        // mid-rebuild leaves the old cache on disk, the honest middle)
        {
            std::lock_guard<std::mutex> lk(g_aiIdxMtx);
            g_aiIndex.vectors.assign(g_aiIndex.chunks.size(), {});
            g_aiIndex.semRefused.assign(g_aiIndex.chunks.size(), 0);
            g_aiIndex.dim = 0;
            g_aiIndex.semComplete = false;
        }
        g_aiSemReadyFlag = false;
    }
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
    unsigned long long gen = ++g_sumGen[SUM_DOC];
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
        // the worker's guard: an uncaught exception on a detached thread is
        // std::terminate - the notes would die with the app
        try {
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
        } catch (...) {
            g_aiNotesBusy = false;
            AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
            e->a = WxU(L"Notes stopped after an internal error.");
            e->flag = false;
            AiPost(e);
        }
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

// ---- the sidebar's NOTES panel (the reference's own seat: MainWindow's
// NotesPanel under the Pages | Notes segmented tabs, not a tab on the chat) ----
// A small WebView2 on g.side below the band, hosting notes_page.deflate
// (resource 404) - the cards UI the chat panel used to carry. The generation
// plumbing above is unchanged; only the address it reports to moved.
static void NotesPanelMessage(const wchar_t* json);   // the page's messages, below
static wrl::ComPtr<ICoreWebView2Controller> g_notesCtr;
static wrl::ComPtr<ICoreWebView2> g_notesWeb;
static bool g_notesBuilding = false, g_notesReady = false;

static std::wstring NotesPageHtml() {
    HRSRC hs = FindResourceW(g.hinst, MAKEINTRESOURCEW(404), (LPCWSTR)RT_RCDATA);
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
static void NotesPost(const std::string& j) {
    if (g_notesWeb) g_notesWeb->PostWebMessageAsJson(WxW(j).c_str());
}
static void NotesSendState() {
    // the seat's own truth: whose pages it reads, which cards it holds. The
    // cards belong to the document that generated them - a different book in
    // the seat clears the board, the reference's NotesPanel behaves the same.
    DocState& D = DVF();
    if (D.path != g_aiNotesDocPath) {
        g_aiNotesDocPath = D.path;
        g_aiNotesCards.clear();
    }
    std::string j = "{\"cmd\":\"notes-status\",\"text\":";
    j += D.path.empty() ? WxJsonStr(L"Open a PDF to generate notes for its pages.")
                        : "null";
    j += ",\"busy\":" + std::string(g_aiNotesBusy ? "true" : "false") + "}";
    NotesPost(j);
    std::string cards = "[";
    for (size_t i = 0; i < g_aiNotesCards.size(); i++) {
        if (i) cards += ",";
        cards += "{\"first\":" + std::to_string(g_aiNotesCards[i].first) +
                 ",\"last\":" + std::to_string(g_aiNotesCards[i].last) +
                 ",\"content\":" + WxJsonStr(WxU(g_aiNotesCards[i].content)) + "}";
    }
    cards += "]";
    NotesPost("{\"cmd\":\"notes-cards\",\"cards\":" + cards + "}");
}
static void NotesSideLayout() {
    if (!g_notesCtr) return;
    if (!g.side) return;
    int band = SideBandH();
    bool show = g_sideNotes && band > 0 && IsWindowVisible(g.side) &&
                sd.cw > (int)(60 * g.dpi) && sd.ch > band + (int)(40 * g.dpi);
    if (!show) {
        g_notesCtr->put_Bounds({ 0, 0, 0, 0 });
        g_notesCtr->put_IsVisible(FALSE);
        return;
    }
    RECT b{ 0, band, sd.cw, max(band + 1, sd.ch) };
    g_notesCtr->put_Bounds(b);
    g_notesCtr->put_IsVisible(TRUE);
}
static void NotesEnsure() {
    if (g_notesCtr || g_notesBuilding) return;
    if (!g.side) return;
    WebEnsureEnv();
    if (!g_wvEnv) return;
    g_notesBuilding = true;
    g_wvEnv->CreateCoreWebView2Controller(
        g.side,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                g_notesBuilding = false;
                if (FAILED(hr) || !c) return S_OK;
                g_notesCtr = c;
                c->get_CoreWebView2(&g_notesWeb);
                wrl::ComPtr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2)
                    c2->put_DefaultBackgroundColor({ 255, 0x14, 0x14, 0x14 });   // opaque: a
                                              // windowed CHILD surface has no DWM redirection
                                              // of its own, so transparent page pixels expose
                                              // the frame's stale paint; the page lays its plate
                EventRegistrationToken tk;
                g_notesWeb->add_WebMessageReceived(
                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                            LPWSTR msg = nullptr;
                            if (SUCCEEDED(a->get_WebMessageAsJson(&msg)) && msg) {
                                NotesPanelMessage(msg);
                                CoTaskMemFree(msg);
                            }
                            return S_OK;
                        }).Get(), &tk);
                std::wstring html = NotesPageHtml();
                if (!html.empty()) g_notesWeb->NavigateToString(html.c_str());
                NotesSideLayout();
                return S_OK;
            }).Get());
}
static void NotesSideShow(bool on) {
    if (g_sideNotes == on) {                 // the same chip again: a no-op repaint
        InvalidateRect(g.side, nullptr, FALSE);
        return;
    }
    g_sideNotes = on;
    if (on) {
        NotesEnsure();
        if (g_notesReady) NotesSendState();   // a reopen re-seats the cards it holds
    }
    InvalidateRect(g.side, nullptr, FALSE);
    NotesSideLayout();
    AiSetSetting("app.notes", on ? "1" : "0"); // the session reopens the notes face
}
// ---- the notes panel's messages (the chat panel's handlers, moved seat) ----
static void NotesRefresh() {
    // a new document in the focused pane re-reads the seat: the doc-change
    // guard inside NotesSendState clears the board the old book filled
    if (g_sideNotes && g_notesReady) NotesSendState();
}
static void NotesPanelMessage(const wchar_t* json) {
    std::string u8 = WxU(json);
    crxk::J m;
    if (!crxk::JParse(u8, m) || m.t != 'o') return;
    const crxk::J* typ = m.get("type");
    if (!typ || typ->t != 's') return;
    std::string t = typ->s;
    if (t == "ready") {
        g_notesReady = true;
        NotesSendState();
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
                                              " \xe2\x80\x93 " + std::to_string(card.last) +
                                              "**\n\n" + card.content);
                if (!AiAxoAppend(D.path, html, card.first, card.last)) { ok = false; break; }
            }
        }
        AiEv* e = AiEvMake(AIEV_NOTES_STATUS);
        e->a = ok ? WxU(L"Saved all to Axo ✓") :
                    WxU(L"Document is not in the database Books or Articles folder. Axo note not saved.");
        e->flag = false;
        AiPost(e);
    }
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
static void SumSendInitSeat(int seat);
static void SumOpenSeat(int seat);

static void SumPostSeat(int seat, const std::string& j) {
    if (seat >= 0 && seat < SUM_SEATS && g_sumWeb[seat])
        g_sumWeb[seat]->PostWebMessageAsJson(WxW(j).c_str());
}
static void SumPost(const std::string& j) { SumPostSeat(SUM_DOC, j); }   // the doc seat's
                                                                        // own events ride
                                                                        // here by name
// the web seat's page key: the digest settings ride the address the tab wears,
// so a reopen finds the digest without re-reading the page
static std::string SumWebDocKey() {
    std::wstring curUrl = (g_webOn && g_wtCur >= 0 && g_wtCur < (int)g_wt.size())
                          ? g_wt[g_wtCur].url : L"";
    return "web_" + AiSha256Hex16(curUrl);
}
static void SumSendInitSeat(int seat) {
    AiLoadSettings();
    int words = max(100, atoi(AiGetSetting("summary.words", "1000").c_str()));
    std::string lang = AiGetSetting("summary.lang", "English");
    int font = max(10, min(24, atoi(AiGetSetting("summary.font", "13").c_str())));
    std::string j;
    if (seat == SUM_DOC) {
        std::string sRange = AiGetSetting("summary.range", "20");
        int range = max(1, atoi(sRange.c_str()));
        g_sumRange = range;
        bool ping = AiGetSetting("summary.ping.enabled", "1") != "0";
        bool buffer = AiGetSetting("summary.buffer.enabled", "1") != "0";
        bool recap = AiGetSetting("summary.recap.enabled", "0") == "1";
        bool discord = AiGetSetting("summary.discord.enabled", "0") == "1";
        j = "{\"cmd\":\"init\",\"mode\":\"doc\"";
        DocState& D = DVF();
        std::string docId = AiDocSessionKey(D.path);
        std::string sStart = AiGetSetting("summary.start." + docId);
        if (!sStart.empty()) {
            int st = atoi(sStart.c_str());
            if (st >= 1 && st <= D.n) g_sumStart = st;
        }
        j += ",\"pageCount\":" + std::to_string(D.n);
        j += ",\"start\":" + std::to_string(g_sumStart);
        j += ",\"rangePages\":" + std::to_string(g_sumRange);
        j += ",\"words\":" + std::to_string(words);
        j += ",\"lang\":" + WxJsonStr(lang);
        j += ",\"genre\":" + WxJsonStr(AiGetSetting("summary.default_genre", "nonfiction_classic"));
        j += ",\"font\":" + std::to_string(font);
        j += ",\"ping\":" + std::string(ping ? "true" : "false");
        j += ",\"buffer\":" + std::string(buffer ? "true" : "false");
        j += ",\"recap\":" + std::string(recap ? "true" : "false");
        j += ",\"discord\":" + std::string(discord ? "true" : "false");
        std::string savedDigest = AiGetSetting("summary.digest." + docId);
        if (!savedDigest.empty()) {
            j += ",\"digest\":" + WxJsonStr(savedDigest);
            std::string savedFirst = AiGetSetting("summary.digest." + docId + ".first");
            std::string savedLast = AiGetSetting("summary.digest." + docId + ".last");
            std::string savedModel = AiGetSetting("summary.digest." + docId + ".model");
            std::string savedScroll = AiGetSetting("summary.digest." + docId + ".scroll");
            if (!savedFirst.empty()) j += ",\"digestFirst\":" + savedFirst;
            if (!savedLast.empty()) j += ",\"digestLast\":" + savedLast;
            if (!savedModel.empty()) j += ",\"digestModel\":" + WxJsonStr(savedModel);
            if (!savedScroll.empty()) j += ",\"digestScroll\":" + savedScroll;
        }
        g_sumInitDoc[SUM_DOC] = AiDocSessionKey(DVF().path);
    } else {
        // the web seat: the live page's own text. Its digest rides the page's
        // address, so coming back to a page the user already summarized picks
        // the digest straight off the disk - the web never had that before.
        bool ping = AiGetSetting("summary.ping.enabled", "1") != "0";
        j = "{\"cmd\":\"init\",\"mode\":\"web\"";
        j += ",\"words\":" + std::to_string(words);
        j += ",\"lang\":" + WxJsonStr(lang);
        j += ",\"prompts\":[{\"id\":\"web_nonfiction_classic\",\"title\":\"Nonfiction\"}]";
        j += ",\"font\":" + std::to_string(font);
        j += ",\"ping\":" + std::string(ping ? "true" : "false");
        g_sumWebDocId = SumWebDocKey();
        std::string savedDigest = AiGetSetting("summary.digest." + g_sumWebDocId);
        if (!savedDigest.empty()) {
            j += ",\"digest\":" + WxJsonStr(savedDigest);
            std::string savedModel = AiGetSetting("summary.digest." + g_sumWebDocId + ".model");
            if (!savedModel.empty()) j += ",\"digestModel\":" + WxJsonStr(savedModel);
        }
        g_sumInitDoc[SUM_WEB] = g_sumWebDocId;
    }
    j += ",\"model\":" + WxJsonStr(g_aiCfg.model);
    j += "}";
    SumPostSeat(seat, j);
}
static void SumEnsureSeat(int seat) {
    if (seat < 0 || seat >= SUM_SEATS || g_sumCtr[seat] || g_sumBuilding[seat]) return;
    WebEnsureEnv();
    if (!g_wvEnv) {
        // the engine warms up asynchronously: a silent bail here left the seat
        // permanently empty, because the env's own completion retries the tabs
        // and the editor but never this seat - the popup's first open then
        // showed nothing but grain. A quiet knock retries until the env
        // answers, and stands down the moment the runtime is declared absent.
        if (g.main && !g_webFail) SetTimer(g.main, 4722, 250, nullptr);
        return;
    }
    if (g.main) KillTimer(g.main, 4722);
    g_sumBuilding[seat] = true;
    g_sumPre[seat].clear();  // the frame's children before this build: the new surface
    if (g.main)              // window is whichever handle the completion adds
        EnumChildWindows(g.main, [](HWND h, LPARAM lp) -> BOOL {
            ((std::vector<HWND>*)lp)->push_back(h); return TRUE; }, (LPARAM)&g_sumPre[seat]);
    g_wvEnv->CreateCoreWebView2Controller(
        g.main,   // the popup is an OVERLAY of the frame, not a window: the
                  // controller rides the frame and moves with it for free
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [seat](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                g_sumBuilding[seat] = false;
                if (FAILED(hr) || !c || seat < 0 || seat >= SUM_SEATS) return S_OK;
                g_sumCtr[seat] = c;
                if (g.main) {   // the surface window: the frame's newest child -
                    std::vector<HWND> post;   // whatever this build added
                    EnumChildWindows(g.main, [](HWND h, LPARAM lp) -> BOOL {
                        ((std::vector<HWND>*)lp)->push_back(h); return TRUE; }, (LPARAM)&post);
                    for (HWND h : post) {
                        bool known = false;
                        for (HWND p2 : g_sumPre[seat]) if (p2 == h) { known = true; break; }
                        if (!known) { g_sumHwnd[seat] = h; break; }
                    }
                    g_sumPre[seat].clear();   // the diff is done; the slot rests
                }
                // born while folded (the prewarm seat): the runtime hands the
                // controller over not-rendered, and it stays that way until
                // the unfold says otherwise - the one who shows it says it
                // may draw.
                c->put_IsVisible(g_sumShown[seat] ? TRUE : FALSE);
                c->get_CoreWebView2(&g_sumWeb[seat]);
                wrl::ComPtr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2)
                    c2->put_DefaultBackgroundColor({ 255, 0x14, 0x14, 0x14 });   // opaque: a
                                              // windowed CHILD surface has no DWM redirection
                                              // of its own - the top-level window era could
                                              // let the grain show through, the overlay era
                                              // cannot; the page carries its own plate
                EventRegistrationToken tk;
                g_sumWeb[seat]->add_WebMessageReceived(
                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [seat](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                            LPWSTR msg = nullptr;
                            if (SUCCEEDED(a->get_WebMessageAsJson(&msg)) && msg) {
                                SumPanelMessage(msg, seat);
                                CoTaskMemFree(msg);
                            }
                            return S_OK;
                        }).Get(), &tk);
                std::wstring html = AiSumHtml();
                if (!html.empty()) g_sumWeb[seat]->NavigateToString(html.c_str());
                SumSeatBoundsRead(seat);   // the saved card lands, clamped inside
                c->put_Bounds(g_sumRect[seat]);
                if (g_sumHwnd[seat] && g_sumShown[seat])   // the open raced the build:
                    SetWindowPos(g_sumHwnd[seat], HWND_TOP, 0, 0, 0, 0,   // raised here too
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                return S_OK;
            }).Get());
}

// ---- the overlay's own geometry ----
// The popup's card lives in the frame's CLIENT coords: no window class, no
// NCCALCSIZE, no ride-along - the frame moves and the card moves with it. Its
// size and offset ride the same settings keys the window era saved, so an
// upgrade keeps the seat the user had. The saved numbers are read once per
// open (every read is a settings.json parse); a resize only clamps.
static void SumSeatBoundsRead(int seat) {
    if (seat < 0 || seat >= SUM_SEATS || !g.main) return;
    RECT cr; GetClientRect(g.main, &cr);
    double dpi = g.dpi > 0 ? g.dpi : 1.0;
    const char* pre = seat == SUM_WEB ? "summary.win.web" : "summary.win";
    std::string sW = AiGetSetting(std::string(pre) + ".w");
    std::string sH = AiGetSetting(std::string(pre) + ".h");
    int w = sW.empty() ? (int)(560 * dpi) : max((int)(320 * dpi), (int)(atof(sW.c_str()) * dpi));
    int h = sH.empty() ? (int)(630 * dpi) : max((int)(220 * dpi), (int)(atof(sH.c_str()) * dpi));
    w = min(w, max(1, (int)cr.right));
    h = min(h, max(1, (int)cr.bottom));
    std::string sRX = AiGetSetting(std::string(pre) + ".relx");
    std::string sRY = AiGetSetting(std::string(pre) + ".rely");
    int x, y;
    if (!sRX.empty() && !sRY.empty()) {
        x = (int)(atof(sRX.c_str()) * dpi);
        y = (int)(atof(sRY.c_str()) * dpi);
    } else {
        // first open: the card floats at the frame's right, a step below the
        // bands; the web seat stands a card-width to its left so the two can
        // face each other
        x = cr.right - w - (int)(12 * dpi);
        y = (int)(64 * dpi);
        if (seat == SUM_WEB && x - w - (int)(12 * dpi) >= 0) x -= w + (int)(12 * dpi);
    }
    x = max(0, min(x, max(0, (int)cr.right - w)));
    y = max(0, min(y, max(0, (int)cr.bottom - h)));
    g_sumRect[seat] = { x, y, x + w, y + h };
}
static void SumClampSeat(int seat) {
    if (seat < 0 || seat >= SUM_SEATS || !g.main) return;
    RECT cr; GetClientRect(g.main, &cr);
    RECT r = g_sumRect[seat];
    int cw = (int)cr.right, ch = (int)cr.bottom;
    int w = (int)(r.right - r.left); if (w > cw) w = cw; if (w < 1) w = 1;
    int hgt = (int)(r.bottom - r.top); if (hgt > ch) hgt = ch; if (hgt < 1) hgt = 1;
    int x = max(0, min((int)r.left, cw - w));
    int y = max(0, min((int)r.top, ch - hgt));
    g_sumRect[seat] = { x, y, x + w, y + hgt };
}
static void SumApplyBounds(int seat) {
    if (seat < 0 || seat >= SUM_SEATS || !g_sumCtr[seat] || !g.main) return;
    g_sumCtr[seat]->put_Bounds(g_sumRect[seat]);
}
// the overlay's z-order: the frame's later-built webviews (a tab's page, the
// editor's scroller) are full-cover siblings stacked ABOVE a surface born
// earlier - a top-level window never met them, an overlay does. The unfold
// walks the card back to the top of the sibling order; no activation stolen,
// the MoveFocus below owns the keys.
static void SumRaiseSeat(int seat) {
    if (seat < 0 || seat >= SUM_SEATS || !g_sumHwnd[seat] || !g.main) return;
    SetWindowPos(g_sumHwnd[seat], HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
// ---- the popup contract's helpers ----
// The navigator is a popup of the frame, not a free window: a MOUSE CLICK
// anywhere inside the frame but outside its borders folds it. Folding is a
// hide, never a teardown - the webview keeps breathing behind the fold, so a
// generation in flight keeps streaming into it, the digest keeps its scroll,
// and the next open is a ShowWindow away instead of a controller rebuild.
// Deactivation alone folds nothing: task-switching away and back must find
// the navigator standing (a WH_MOUSE_LL watcher below sees the actual clicks,
// so alt-tab, the taskbar, other apps and even the popup's own select
// dropdowns never trip the contract).
static void SumUnhookMouse() {
    // the hook watches for BOTH seats: it leaves only when no seat is unfolded
    for (int s = 0; s < SUM_SEATS; s++)
        if (g_sumShown[s]) return;
    if (g_sumMouseHook) { UnhookWindowsHookEx(g_sumMouseHook); g_sumMouseHook = nullptr; }
}
static void SumHideSeat(int seat, bool fromOutside) {
    if (seat < 0 || seat >= SUM_SEATS || !g_sumShown[seat]) return;
    if (g_sumWeb[seat])   // the digest's reader seat rides out before the fold
        g_sumWeb[seat]->ExecuteScript(L"window.__sumSaveScroll&&window.__sumSaveScroll();", nullptr);
    g_sumShown[seat] = false;
    if (g_sumCtr[seat]) g_sumCtr[seat]->put_IsVisible(FALSE);   // folded = not rendered, not
                                                    // dead: the page keeps its
                                                    // DOM, a generation keeps
                                                    // streaming into it, and the
                                                    // compositor stops burning
                                                    // frames nobody can see
    SumSavePlacementSeat(seat);
    SumUnhookMouse();
    if (fromOutside) g_sumHideTick[seat] = GetTickCount();
}
// The frame's folds drive the seat's render. A minimize takes the whole frame
// down with the overlays on it - the controller keeps claiming visible while
// the surface is gone, so the handback stays explicit: FALSE on the way down,
// TRUE on the way back, with the card's bounds re-clamped into whatever the
// frame's client grew or shrank to. Both calls no-op when nothing moved.
static void SumSyncVisible() {
    for (int s = 0; s < SUM_SEATS; s++) {
        if (!g_sumCtr[s]) continue;
        BOOL want = (g_sumShown[s] && g.main && !IsIconic(g.main)) ? TRUE : FALSE;
        g_sumCtr[s]->put_IsVisible(want);
        if (want) {
            SumClampSeat(s);      // a resize keeps the card inside the frame
            SumApplyBounds(s);
            SumRaiseSeat(s);      // and back above whatever the frame built since
        }
    }
}
static LRESULT CALLBACK SumMouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        if (wp == WM_MOUSEMOVE) {
            // a drag or resize in flight: one tick per move, applied on the pump
            for (int s = 0; s < SUM_SEATS; s++)
                if (g_sumDragMode[s] && g.main) {
                    POINT pt = ((MSLLHOOKSTRUCT*)lp)->pt;
                    PostMessageW(g.main, WM_APP + 62, 0, MAKELPARAM(pt.x, pt.y));
                    break;
                }
        } else if (wp == WM_LBUTTONUP) {
            for (int s = 0; s < SUM_SEATS; s++)
                if (g_sumDragMode[s] && g.main) { PostMessageW(g.main, WM_APP + 63, 0, 0); break; }
        } else if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN ||
                   wp == WM_XBUTTONDOWN || wp == WM_NCLBUTTONDOWN || wp == WM_NCRBUTTONDOWN) {
            // a click inside the frame but outside every unfolded card folds
            // them; the verdict runs on the pump, never the hook
            POINT pt = ((MSLLHOOKSTRUCT*)lp)->pt;
            HWND w = WindowFromPoint(pt);
            HWND root = w ? GetAncestor(w, GA_ROOT) : nullptr;
            if (root && (root == g.main || IsChild(g.main, root)))
                PostMessageW(g.main, WM_APP + 61, 0, MAKELPARAM(pt.x, pt.y));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static void SumSavePlacementSeat(int seat) {
    if (seat < 0 || seat >= SUM_SEATS || !g.main) return;
    RECT r = g_sumRect[seat];
    if (r.right <= r.left) return;   // never seated yet - nothing to save
    double dpi = g.dpi > 0 ? g.dpi : 1.0;
    const char* pre = seat == SUM_WEB ? "summary.win.web" : "summary.win";
    AiSetSetting(std::string(pre) + ".w", std::to_string((int)((r.right - r.left) / dpi)));
    AiSetSetting(std::string(pre) + ".h", std::to_string((int)((r.bottom - r.top) / dpi)));
    AiSetSetting(std::string(pre) + ".relx", std::to_string((int)(r.left / dpi)));
    AiSetSetting(std::string(pre) + ".rely", std::to_string((int)(r.top / dpi)));
}
// the frame moved: an overlay rides its parent for free - the client coords
// never change, so there is nothing to chase. The resize clamp is WM_SIZE's
// SumSyncVisible call.
static void SumOwnerMoved() {
    SumSyncVisible();
}
// the pump's verdict on the hook's outside click: every unfolded card whose
// rect misses the point folds; a click on a card leaves all of them standing
static void SumOutsideClick(long sx, long sy) {
    if (!g.main) return;
    POINT o{ 0, 0 };
    ClientToScreen(g.main, &o);
    RECT cr; GetClientRect(g.main, &cr);
    POINT pt{ (int)sx - o.x, (int)sy - o.y };
    if (pt.x < cr.left || pt.x >= cr.right || pt.y < cr.top || pt.y >= cr.bottom) return;
    for (int s = 0; s < SUM_SEATS; s++)
        if (g_sumShown[s] && !PtInRect(&g_sumRect[s], pt))
            SumHideSeat(s, true);
}
// one tick of a drag or a grip resize: the card follows the cursor, never
// leaving the frame's client, never under its mins
static void SumDragTick(long sx, long sy) {
    if (!g.main) return;
    POINT o{ 0, 0 };
    ClientToScreen(g.main, &o);
    RECT cr; GetClientRect(g.main, &cr);
    double dpi = g.dpi > 0 ? g.dpi : 1.0;
    for (int s = 0; s < SUM_SEATS; s++) {
        if (!g_sumDragMode[s] || !g_sumShown[s]) continue;
        RECT r = g_sumRect[s];
        int px = (int)sx - o.x, py = (int)sy - o.y;
        int w = (int)(r.right - r.left), hgt = (int)(r.bottom - r.top);
        if (g_sumDragMode[s] == 1) {
            r.left = px - g_sumDragOff[s].x;
            r.top = py - g_sumDragOff[s].y;
            // the card may tuck most of the way out, but its edge stays
            // grabbable: 60dp of it always remains inside
            int cwR = (int)cr.right, cbB = (int)cr.bottom;
            r.left = max(cwR - (int)(60 * dpi) - w,
                         min((int)r.left, cwR - (int)(60 * dpi)));
            r.top = max(cbB - (int)(40 * dpi) - hgt,
                        min((int)r.top, cbB - (int)(40 * dpi)));
            r.right = r.left + w; r.bottom = r.top + hgt;
        } else {
            w = max((int)(320 * dpi), min(px + (int)g_sumDragOff[s].x - (int)r.left,
                                       (int)cr.right - (int)r.left));
            hgt = max((int)(220 * dpi), min(py + (int)g_sumDragOff[s].y - (int)r.top,
                                        (int)cr.bottom - (int)r.top));
            r.right = (int)r.left + w; r.bottom = (int)r.top + hgt;
        }
        g_sumRect[s] = r;
        SumApplyBounds(s);
    }
}
// the gesture's button-up: the placement rides out once, not per pixel
static void SumDragEnd() {
    for (int s = 0; s < SUM_SEATS; s++)
        if (g_sumDragMode[s]) {
            g_sumDragMode[s] = 0;
            SumSavePlacementSeat(s);
        }
}
// the seats' knock on the engine's door, served on the pump: one timer on the
// frame answers for both overlays, and stands down when nobody is waiting
static void SumEnsureRetry() {
    bool pending = false;
    for (int s = 0; s < SUM_SEATS; s++) {
        SumEnsureSeat(s);
        pending = pending || (!g_webFail && !g_sumCtr[s] && !g_sumBuilding[s]);
    }
    if (!pending && g.main) KillTimer(g.main, 4722);
}
// the buffer's deferred kick (the window era's 4723, re-homed to the frame):
// the open's paint has landed; if the pump is free, the background reader
// starts turning pages for the next stretch
static void SumPrefetchKick() {
    g_sumPf.on = false;
    if (!g_range.active) {
        DocState& D = DVF();
        if (!D.path.empty() && g_sumPf.first >= 1 && g_sumPf.first <= D.n) {
            g_range.words = g_sumPf.words;   // the cache key and the
            g_range.lang = g_sumPf.lang;     // prompt ride the job, set
            g_range.genre = g_sumPf.genre;   // at fire time, not request
            int last = min(g_sumPf.first + g_sumPf.pages - 1, D.n);
            AiRangeStart(D.path, g_sumPf.first, last, 3, 0);
        }
    }
}
// the toolbar's toggle: each surface opens its own seat and folds it again
static void SumToggle(int seat) {
    if (seat < 0 || seat >= SUM_SEATS) return;
    if (g_sumHideTick[seat] && GetTickCount() - g_sumHideTick[seat] < 400) {
        g_sumHideTick[seat] = 0;   // this button click IS the outside click that just
        return;                    // folded it - a toggle-off, not an invitation back
    }
    if (g_sumShown[seat]) {
        SumHideSeat(seat, false);   // the toolbar button is a toggle: open again rolls it up
        return;
    }
    SumOpenSeat(seat);
}
// what a seat's page's current init was built for - a reopen only re-sends
// init when the document or surface moved under the hood, because re-initing
// a living page wipes its digest view, its busy state and its scroll, exactly
// the "close it and everything resets" the fold was built to stop
static bool SumInitStale(int seat) {
    std::string key = (seat == SUM_DOC) ? AiDocSessionKey(DVF().path) : SumWebDocKey();
    return key != g_sumInitDoc[seat];
}
// the idle seats: two and a half seconds after launch the frame builds its
// overlays hidden - controller, page and all - so the first click on either
// toolbar unfolds a living popup instead of a construction site
static void SumPrewarm() {
    for (int s = 0; s < SUM_SEATS; s++)
        SumEnsureSeat(s);
}
static void SumOpenSeat(int seat) {
    if (seat < 0 || seat >= SUM_SEATS || !g.main) return;
    g_sumShown[seat] = true;
    SumSeatBoundsRead(seat);        // the saved card lands (clamped) before the render
    SumEnsureSeat(seat);            // no-ops while the webview lives; re-arms it after
                                    // a rebuild
    if (g_sumCtr[seat]) {
        g_sumCtr[seat]->put_IsVisible(TRUE);   // the fold took the render away; the
                                               // unfold hands it back (the webview
                                               // was born folded and never
                                               // auto-follows anything)
        SumApplyBounds(seat);
        SumRaiseSeat(seat);   // above the frame's later webviews - a tab's page or
                              // the editor would bury a card born before theirs
        // the page hears keys - Esc, the word plate's Shift - without a click
        // first: a window era's SetForegroundWindow, in overlay terms
        g_sumCtr[seat]->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    if (!g_sumMouseHook) g_sumMouseHook = SetWindowsHookExW(WH_MOUSE_LL, SumMouseHookProc, g.hinst, 0);
    if (g_sumReady[seat] && SumInitStale(seat)) SumSendInitSeat(seat);
}

// ---- the summary's generation thread ----
// `run` is the run token this thread was born under: a redirected Start bumps
// g_sumRun, and every busy-flag write below is gated on still owning it - a
// dying run must never un-busy the run that replaced it. `cancel` is this
// run's OWN stop flag (one per run): the redirect sets the old flag, not the
// new one, so the two runs never race the same boolean.
static void AiSumRun(unsigned long long gen, unsigned long long run,
                     std::shared_ptr<std::atomic<bool>> cancel, const std::string& docId,
                     std::wstring rangeText, int first, int last,
                     int words, std::string lang, std::string genre, bool bypass,
                     int seat = SUM_DOC) {
    AiLoadSettings();
    AiCfg cfg = g_aiCfg;
    std::wstring bodyW = std::regex_replace(rangeText,
        std::wregex(L"\\[{1,2}p\\.\\s*\\d+\\]{1,2}"), L"");
    if (AiLetters(bodyW) < 50) {
        AiEv* e = AiEvMake(AIEV_SUM_NOTEXT);
        e->gen = gen;
        AiPost(e);                       // the panel learns on the pump's thread
        if (run == g_sumRun[seat]) g_sumBusy[seat] = false;
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
        if (run == g_sumRun[seat]) g_sumBusy[seat] = false;
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
    // WxJsonStr returns a quoted literal - it can only ride a VALUE position;
    // embedded raw into "Connecting to ..." it minted a text field with naked
    // inner quotes, the panel's JSON.parse dropped it, and the connect line
    // never showed. Quote the whole sentence instead. And the post rides the
    // pump: this thread is not the UI thread, and a webview call from here
    // dies in the apartment wall - the connect line was the least of what
    // that wall ate.
    {
        AiEv* pe = AiEvMake(AIEV_SUM_PROGRESS);
        pe->gen = gen;
        pe->a = "Connecting to " + model + "\xe2\x80\xa6";
        AiPost(pe);
    }
    AiError err;
    std::string full;
    bool ok = AiChatStream(cfg, sys, {}, user, budget, nullptr, true,
                           [&](const std::string& piece) {
                               if (!piece.empty()) {
                                   AiEv* e = AiEvMake(AIEV_SUM_DELTA);
                                   e->gen = gen;
                                   e->a = piece;
                                   AiPost(e);
                               }
                           }, full, err, cancel.get());
    if (cancel->load()) {
        AiEv* e = AiEvMake(AIEV_SUM_PROGRESS);
        e->gen = gen; e->a = WxU(L"Stopped");
        AiPost(e);
        if (run == g_sumRun[seat]) g_sumBusy[seat] = false;
        return;
    }
    if (!ok || full.empty()) {
        // one quiet buffered retry at twice the budget
        std::string content;
        AiError e2;
        if (AiChatBuffered(cfg, sys, {}, user, budget * 2, nullptr, true, content, e2,
                           cancel.get()) && !content.empty())
            full = content;
        else {
            AiEv* e = AiEvMake(AIEV_SUM_ERROR);
            e->gen = gen;
            std::wstring msg = !e2.detail.empty() ? e2.Friendly() : err.Friendly();
            if (msg.empty()) msg = !err.detail.empty() ? err.detail : L"Failed to generate summary. Please check your AI API key and connection.";
            if (err.cat == AI_ERR_NOT_SIGNED_IN || e2.cat == AI_ERR_NOT_SIGNED_IN)
                msg = L"Authentication failed (API key invalid or expired). Please check your AI API key in Settings.";
            e->a = WxU(msg);
            AiPost(e);
            if (run == g_sumRun[seat]) g_sumBusy[seat] = false;
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
    if (run == g_sumRun[seat]) g_sumBusy[seat] = false;
}

// ---- the summarizer panel's messages ----
static void SumPanelMessage(const wchar_t* json, int seat) {
    if (seat < 0 || seat >= SUM_SEATS) return;
    std::string u8 = WxU(json);
    crxk::J m;
    if (!crxk::JParse(u8, m) || m.t != 'o') return;
    const crxk::J* typ = m.get("type");
    if (!typ || typ->t != 's') return;
    std::string t = typ->s;
    if (t == "ready") {
        g_sumReady[seat] = true;
        SumSendInitSeat(seat);
    } else if (t == "start") {
        // A second Start is a REDIRECT, not a drop. The old gate swallowed the
        // click while a run was busy: the page kept saying "Reading pages..."
        // and every back / forth / range switch went nowhere - the user's
        // "nothing, same shit". The extraction pump is a cheap cursor (it just
        // turns to the new range) and a streaming model call dies on its own
        // per-run cancel flag; the old run's leftovers are all gen-gated away
        // by the page. The new run owns the busy flag from here.
        const crxk::J* g = m.get("gen"), * w = m.get("words"), * st = m.get("start"),
            * rg = m.get("range"), * lg = m.get("lang"), * gr = m.get("genre"), * bp = m.get("bypass");
        unsigned long long gen;
        if (seat == SUM_WEB) {
            // the web seat mints its own gens from a base no document run can
            // reach: every completion finds its window by the gen alone
            gen = (g_sumGen[seat] < kSumWebGenBase) ? kSumWebGenBase : g_sumGen[seat] + 1;
        } else {
            gen = g ? (unsigned long long)g->d : g_sumGen[seat] + 1;
        }
        g_sumGen[seat] = gen;                // the accepted gen: stale completions
                                             // recognize themselves against it
        ++g_sumRun[seat];                    // the new run owns the busy flag
        if (g_sumBusy[seat]) {
            if (g_sumCancel[seat]) *g_sumCancel[seat] = true;   // the old stream folds quietly
            if (seat == SUM_DOC && !(g_range.active && (g_range.what == 0 || g_range.what == 2)))
                g_range.active = false;   // a model-phase run: the pump is idle;
                                          // an extraction in flight just gets
                                          // overwritten by the AiRangeStart below
        }
        g_sumCancel[seat] = std::make_shared<std::atomic<bool>>(false);
        int words = w && w->t == 'd' ? (int)w->d : 1000;
        std::string lang = lg && lg->t == 's' ? lg->s : "English";
        std::string genre = gr && gr->t == 's' ? gr->s : "nonfiction_classic";
        bool bypass = bp && bp->b;
        g_sumBusy[seat] = true;
        if (seat == SUM_DOC) {
            if (st && st->t == 'd') g_sumStart = max(1, (int)st->d);
            if (rg && rg->t == 'd') g_sumRange = max(1, (int)rg->d);
            DocState& D = DVF();
            if (D.path.empty()) {
                AiEv* e = AiEvMake(AIEV_SUM_ERROR);
                e->gen = gen;
                e->a = WxU(L"Open a PDF first, then summarize its pages.");
                AiPost(e);
                g_sumBusy[seat] = false;
                return;
            }
            int last = min(g_sumStart + g_sumRange - 1, D.n);
            AiRangeStart(D.path, g_sumStart, last, 0, gen);
            g_range.words = words;
            g_range.lang = lang;
            g_range.genre = genre;
            g_range.bypass = bypass;
        } else {
            // the live page: one extraction, then the thread. The digest key
            // rides the tab's address, so a reopen of the same page lands the
            // saved digest without a rerun.
            g_sumWebDocId = SumWebDocKey();
            g_sumWebPending = gen;
            AiWebExtract(gen);
        }
    } else if (t == "cancel") {
        // the buffer fast-path displayed its ready summary while a generation
        // was still breathing behind it: the old run folds quietly (its posts
        // are gen-gated away) and the busy flag dies with it - no reset, the
        // cache and the digest stay
        if (g_sumCancel[seat]) *g_sumCancel[seat] = true;
        if (seat == SUM_DOC) {
            g_range.active = false;
            KillTimer(g.main, 4714);
        }
        g_sumBusy[seat] = false;
    } else if (t == "reset") {
        if (g_sumCancel[seat]) *g_sumCancel[seat] = true;
        if (seat == SUM_DOC) {
            g_range.active = false;              // an extraction in flight dies with
            KillTimer(g.main, 4714);             // the reset - the old code left the
                                                 // pump grinding a wiped range
        }
        g_sumBusy[seat] = false;
        if (seat == SUM_DOC) {
            DocState& D = DVF();
            if (!D.path.empty()) {
                std::string docId = AiDocSessionKey(D.path);
                AiSetSetting("summary.digest." + docId, "");
                // Reset's whole point: Start must build anew, not replay the
                // cache the last run left. The digest setting alone never
                // touched summary_cache.json, and the old range-scoped delete
                // only killed the stretch on screen - the buffers and every
                // neighboring range stayed, so the very next Start (or a
                // back/forth step onto a buffered stretch) answered "fromCache"
                // with the same text the user just wiped. Reset clicked = the
                // whole cache dies: every range, every model, every document.
                AiCacheDeleteAll();
                AiSetSetting("summary.digest." + docId + ".first", "");
                AiSetSetting("summary.digest." + docId + ".last", "");
                AiSetSetting("summary.digest." + docId + ".model", "");
                AiSetSetting("summary.digest." + docId + ".scroll", "");
            }
        }
    } else if (t == "save-scroll") {   // the digest's reader seat, per subject
        const crxk::J* tp = m.get("top");
        if (tp && tp->t == 'd') {
            if (seat == SUM_DOC) {
                DocState& D = DVF();
                if (!D.path.empty()) {
                    std::string docId = AiDocSessionKey(D.path);
                    int top = max(0, (int)tp->d);
                    AiSetSetting("summary.digest." + docId + ".scroll", std::to_string(top));
                }
            } else if (!g_sumWebDocId.empty()) {
                int top = max(0, (int)tp->d);
                AiSetSetting("summary.digest." + g_sumWebDocId + ".scroll", std::to_string(top));
            }
        }
    } else if (t == "close") {
        // the chrome's own close chip (DialogChrome's red mark) or Esc: the
        // fold is a hide - the webview, its digest and a running generation
        // all keep breathing behind it
        SumHideSeat(seat, false);
    } else if (t == "drag") {
        // the HTML title bar's drag handle: the popup is an overlay of the
        // frame - the low-level hook carries the gesture, the pump applies it
        if (seat >= 0 && seat < SUM_SEATS && g_sumShown[seat] && g.main) {
            g_sumDragMode[seat] = 1;
            POINT pt; GetCursorPos(&pt);
            POINT o{ 0, 0 };
            ClientToScreen(g.main, &o);
            g_sumDragOff[seat] = { pt.x - o.x - (int)g_sumRect[seat].left,
                                   pt.y - o.y - (int)g_sumRect[seat].top };
        }
    } else if (t == "resize-start") {
        // the corner grip: the same road as the drag, anchored at the card's
        // bottom-right corner
        if (seat >= 0 && seat < SUM_SEATS && g_sumShown[seat] && g.main) {
            g_sumDragMode[seat] = 2;
            POINT pt; GetCursorPos(&pt);
            POINT o{ 0, 0 };
            ClientToScreen(g.main, &o);
            g_sumDragOff[seat] = { (int)g_sumRect[seat].right - (pt.x - o.x),
                                   (int)g_sumRect[seat].bottom - (pt.y - o.y) };
        }
    } else if (t == "word-action") {
        // the word plate's Define and Explain (Avalanche's popup actions): a
        // small buffered pass on the seat's own thread, the answer riding back
        // to the same window's plate. Temperature-0 fact lookups, not streams.
        const crxk::J* ac = m.get("action"), * wd = m.get("word"), * cx = m.get("context");
        std::string action = ac && ac->t == 's' ? ac->s : "";
        std::string word = wd && wd->t == 's' ? wd->s : "";
        std::string context = cx && cx->t == 's' ? cx->s : "";
        if (word.empty() || (action != "define" && action != "explain")) return;
        std::string lang = AiGetSetting("summary.lang", "English");
        std::thread([seat, action, word, context, lang]() {
            // the worker's guard: an uncaught exception on a detached thread
            // is std::terminate - the plate would take the app down with it
            try {
                AiLoadSettings();
                AiCfg cfg = g_aiCfg;
                std::string sys = action == "define"
                    ? "You are an authoritative, concise dictionary assistant."
                    : "You are a scholarly reading companion.";
                std::string user;
                if (action == "define") {
                    std::string tpl = PromptBodyOf("word_define");
                    if (tpl.empty())
                        tpl = "The reader is studying a text in {language} and highlighted the term: \"{term}\".\n\n"
                              "Define \"{term}\" concisely in English (1-3 sentences):\n"
                              "1. Part of speech and core literal definition.\n"
                              "2. If the term is non-English, provide its exact English translation first.\n"
                              "3. Any cultural, technical, or contextual nuance in how the term is used.\n"
                              "Do NOT include conversational meta-talk or introductory pleasantries.";
                    size_t p;
                    while ((p = tpl.find("{term}")) != std::string::npos) tpl.replace(p, 6, word);
                    while ((p = tpl.find("{language}")) != std::string::npos) tpl.replace(p, 10, lang);
                    user = tpl;
                } else {
                    std::string tpl = PromptBodyOf("word_explain");
                    if (tpl.empty())
                        tpl = "The reader is studying a text and highlighted the following passage/term:\n"
                              "\"{term}\"\n\nThe sentence it appears in:\n{context}\n\n"
                              "Explain it as a scholarly reading companion would: what it means HERE, "
                              "grounded in the sentence's own context; unpack any jargon; quote the "
                              "words that carry the meaning. 2-5 sentences, no meta-talk.";
                    size_t p;
                    while ((p = tpl.find("{term}")) != std::string::npos) tpl.replace(p, 6, word);
                    while ((p = tpl.find("{context}")) != std::string::npos) tpl.replace(p, 9, context);
                    while ((p = tpl.find("{language}")) != std::string::npos) tpl.replace(p, 10, lang);
                    user = tpl;
                }
                std::string content;
                AiError err;
                bool ok = AiChatBuffered(cfg, sys, {}, user, 4096, nullptr, true, content, err, nullptr);
                if (!ok || content.empty()) {
                    AiError e2;
                    if (AiChatBuffered(cfg, sys, {}, user, 8192, nullptr, true, content, e2, nullptr)
                        && !content.empty()) ok = true;
                    else ok = false;
                }
                AiEv* ev = AiEvMake(AIEV_WORD_OUT);
                ev->key = std::to_string(seat);
                ev->b = action;
                ev->a = ok ? content : WxU(err.Friendly().empty()
                     ? std::wstring(L"The lookup failed - check the AI settings.") : err.Friendly());
                ev->flag = ok;
                AiPost(ev);
            } catch (...) {
                AiEv* ev = AiEvMake(AIEV_WORD_OUT);
                ev->key = std::to_string(seat);
                ev->b = action;
                ev->flag = false;
                ev->a = "The word lookup stopped after an internal error.";
                AiPost(ev);
            }
        }).detach();
    } else if (t == "word-search") {
        // the word plate's Search (Avalanche's system-browser road, made the
        // app's own): one dedicated browser tab carries every lookup, and the
        // browser itself takes the stage to show it
        const crxk::J* wd = m.get("word");
        std::string word = wd && wd->t == 's' ? wd->s : "";
        if (!word.empty()) WebWordLookup(WxW(word));
    } else if (t == "page") {
        const crxk::J* p = m.get("page");
        if (p && p->t == 'd' && seat == SUM_DOC) {
            int pg = max(1, min((int)p->d, DVF().n));
            SetPage(pg - 1);
            DocState& D = DVF();
            if (!D.path.empty()) {
                std::string docId = AiDocSessionKey(D.path);
                AiSetSetting("summary.start." + docId, std::to_string(pg));
            }
        }
    } else if (t == "range-changed") {
        const crxk::J* p = m.get("pages");
        if (p && p->t == 'd') {
            g_sumRange = max(1, (int)p->d);
            AiSetSetting("summary.range", std::to_string(g_sumRange));
        }
    } else if (t == "set-setting") {
        const crxk::J* k = m.get("key"), * v = m.get("val");
        if (k && k->t == 's' && v && v->t == 's') {
            AiSetSetting(k->s, v->s);
            if (k->s == "summary.range") {
                int r = atoi(v->s.c_str());
                if (r > 0) g_sumRange = r;
            }
        }
    } else if (t == "save-digest") {
        if (seat == SUM_DOC) {
            DocState& D = DVF();
            if (!D.path.empty()) {
                std::string docId = AiDocSessionKey(D.path);
                const crxk::J* txt = m.get("text"), * f = m.get("first"), * l = m.get("last"), * mdl = m.get("model");
                if (txt && txt->t == 's') AiSetSetting("summary.digest." + docId, txt->s);
                if (f && f->t == 'd') AiSetSetting("summary.digest." + docId + ".first", std::to_string((int)f->d));
                if (l && l->t == 'd') AiSetSetting("summary.digest." + docId + ".last", std::to_string((int)l->d));
                if (mdl && mdl->t == 's') AiSetSetting("summary.digest." + docId + ".model", mdl->s);
            }
        } else if (!g_sumWebDocId.empty()) {
            // the web digest saves itself under the page's address: a reopen
            // of the same page finds it without a rerun
            const crxk::J* txt = m.get("text"), * mdl = m.get("model");
            if (txt && txt->t == 's') AiSetSetting("summary.digest." + g_sumWebDocId, txt->s);
            if (mdl && mdl->t == 's') AiSetSetting("summary.digest." + g_sumWebDocId + ".model", mdl->s);
        }
    } else if (t == "test-range") {
        // the title bar's beaker (Avalanche's TestRangeRequested -> OpenAiTestForRange):
        // the tester_standard probe reads the current stretch and reports the first
        // and last sentences it saw - proof the extraction carries real text
        if (seat != SUM_DOC || g_sumBusy[seat]) return;
        DocState& D = DVF();
        if (D.path.empty()) return;
        const crxk::J* f = m.get("first"), * l = m.get("last");
        int first = f && f->t == 'd' ? max(1, (int)f->d) : g_sumStart;
        int last = l && l->t == 'd' ? min((int)l->d, D.n) : min(g_sumStart + g_sumRange - 1, D.n);
        if (first > last) return;
        g_sumBusy[seat] = true;
        AiRangeStart(D.path, first, last, 2, ++g_sumGen[SUM_DOC]);
    } else if (t == "prefetch") {
        const crxk::J* st = m.get("start"), * rg = m.get("range"),
            * w = m.get("words"), * lg = m.get("lang"), * gr = m.get("genre");
        if (seat == SUM_DOC && st && st->t == 'd') {
            int first = (int)st->d;
            int pages = rg && rg->t == 'd' ? (int)rg->d : g_sumRange;
            int words = w && w->t == 'd' ? (int)w->d : 1000;
            std::string lang = lg && lg->t == 's' ? lg->s : "English";
            std::string genre = gr && gr->t == 's' ? gr->s : "nonfiction_classic";
            DocState& D = DVF();
            if (!D.path.empty() && first <= D.n) {
                // This used to extract the whole range right here, on the UI
                // thread - sixty pages of OCR inside one message handler, the
                // machine frozen for as long as the range took, the popup's
                // own open feeling like a system crash. The fill now waits a
                // beat (the open's paint lands unmolested) and then rides the
                // same idle pump the Start button uses. Start owns the pump
                // first: if it is busy the JS asks again on the next done.
                if (g_range.active) return;
                g_sumPf.on = true;
                g_sumPf.first = first;
                g_sumPf.pages = pages;
                g_sumPf.words = words;
                g_sumPf.lang = lang;
                g_sumPf.genre = genre;
                if (g.main) SetTimer(g.main, 4737, 1200, nullptr);
            }
        }
    }
}
// ---- the session: the view, the document, the open panels ride settings.json ----
// Every mode switch saves where the reader is (app.* keys), and a launch with
// no file on the command line puts it all back: same view, same document, the
// AI panel / notes face / thumbnail rails that were open, and the text
// editor's scroller at its saved position (editor.inl owns the edscroll keys).
// The definitions live here, late in the TU, because restore drives every
// surface - the toggles, the rail layout - that only exist by this point.
static void SessionSaveAll() {
    const wchar_t* v = g_editOn ? L"editor" : (g_webOn ? L"web" : L"pdf");
    AiSetSetting("app.view", WxU(v));
    if (!DVF().path.empty()) AiSetSetting("app.doc", WxU(DVF().path));
    if (!g_edPath.empty()) {
        AiSetSetting("app.edoc", WxU(g_edPath));
        EditorSave();                          // the typing rides out with the window
        if (g_edWeb) g_edWeb->PostWebMessageAsJson(L"{\"cmd\":\"scroll-get\"}");
    }
    AiSetSetting("app.aion", g_aiOn ? "1" : "0");
    AiSetSetting("app.notes", g_sideNotes ? "1" : "0");
    AiSetSetting("app.sideon", g.sideOn ? "1" : "0");
    AiSetSetting("app.webside", g_webSide ? "1" : "0");
}
static void SessionRestore() {
    std::string view = AiGetSetting("app.view", "pdf");
    std::string doc = AiGetSetting("app.doc", "");
    if (!doc.empty() && WxFileExists(WxW(doc).c_str()))
        TabOpenInto(false, WxW(doc));          // the document rides beneath every view
    if (view == "web") {
        std::string rail = AiGetSetting("app.webside", "0");
        g_webSide = rail == "1";
        WebToggle(true);
    } else if (view == "editor") {
        EditorToggle(true);
        std::string edoc = AiGetSetting("app.edoc", "");
        if (!edoc.empty() && WxFileExists(WxW(edoc).c_str())) EditorOpenFile(WxW(edoc));
        EditorRefocus();                       // the caret, not the first click
    } else {
        std::string notes = AiGetSetting("app.notes", "0");
        if (notes == "1") NotesSideShow(true);
        std::string side = AiGetSetting("app.sideon", "1");
        if (side == "0" && g.sideOn) {
            g.sideOn = false;
            ShowWindow(g.side, SW_HIDE);
            RailRepaint();
            LayoutMain();
        }
    }
    if (AiGetSetting("app.aion", "0") == "1") AiChatToggle();
}
