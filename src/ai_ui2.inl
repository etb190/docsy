// ---------------- docsy AI: panel messages, workers, summary window ----------------
// The second half: the panel's postMessage switch, the index/semantic/notes
// pumps, the Page-Summary navigator window (Features/Summary/SummaryWindow +
// WebSummaryWindow in one frame), the citation jump, and the Axo notes save.
#pragma once

static void AiPanelMessage(const wchar_t* json);
static void SumPanelMessage(const wchar_t* json);
static void SumPost(const std::string& j);
static LRESULT CALLBACK SumWndProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static void SumHide(bool fromOutside);   // the popup contract: a click outside
                                         // its borders folds it, alive
static void SumPrewarm();        // the idle seat: window + webview build hidden
static void SumSavePlacement();  // one settings write per gesture, not per pixel

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

static DWORD g_sumHideTick = 0;          // when an outside click folded it: the
                                         // toolbar's own click must not reopen
static RECT g_sumOwnerPrev{};            // the frame's last screen rect: the
                                         // popup rides its moves like a pane
static HHOOK g_sumMouseHook = nullptr;   // the click-outside watcher, only
                                         // while the popup is unfolded
static std::string g_sumInitDoc;         // what the page's last init was built
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
    if ((g_range.what == 0 || g_range.what == 3) && !g_sumWnd) return;
    if (g_range.what == 1 && !g_sideNotes) return;
    if (g_range.what == 2 && !g_sumWnd) return;   // the beaker's range test rides the summary window
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
    if (AiIndexLoad(docId, ix) && !ix.chunks.empty()) {
        ix.path = path;
        {   // the index swap rides the seat lock: a reply thread's snapshot
            // (or an in-flight merge) must never see a half-assigned index
            std::lock_guard<std::mutex> lk(g_aiIdxMtx);
            g_aiIndex = std::move(ix);
        }
        g_aiIndexReady = true;
        AiPanelSend(WxW(AiStatusJson("status")));
        // a cached index re-arms the semantic line the same breath - the
        // up-to-date check makes this a no-op when the vectors already match
        // the model, and a fresh nemotron build when they do not
        AiSemanticStart();
        return;
    }
    g_aiIndexing = true;
    g_aiIndexReady = false;
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
        // Avalanche's DocumentIndexer embeds while it indexes: the lexical
        // chunks are barely down and the semantic pass (nemotron's door) is
        // already riding its thread - the reader never has to find the beaker
        // button for the sidechat's answers to carry real retrieval. The
        // manual toggle stays for a re-run or a cancel.
        AiSemanticStart();
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
    std::wstring prefixKey = cfg.embDocPrefix + L"\x01" + cfg.embQueryPrefix;
    std::string model = WxU(cfg.embeddingModel.empty() ? L"nvidia/nemotron-3-embed-1b"
                                                       : cfg.embeddingModel);
    std::thread([ix, cfg, prefixKey, model]() mutable {
        // the guard is the rule for every AI worker: an uncaught exception on a
        // detached thread is std::terminate - the whole app dying with the
        // beaker line frozen. The catch folds the pass and speaks.
        try {
            bool upToDate = (ix.embModel == model && ix.prefixKey == WxU(prefixKey) &&
                             !ix.vectors.empty() &&
                             ix.vectors.size() == ix.chunks.size());
            if (!upToDate) {
                size_t total = ix.chunks.size();
                ix.vectors.assign(total, {});
                ix.dim = 0;
                // One batch, any size: an endpoint that refuses N inputs (a 400
                // on its own batch limit, a 429 on the burst) gets the batch
                // HALVED and retried - the pass lands whether the door takes
                // 256 inputs, 100 or 16. A single input's failure is the only
                // real failure, and that one speaks the model's own verdict.
                auto slice = [&](auto&& self, size_t b0, size_t b1, int depth,
                                 AiError& eOut) -> bool {
                    if (g_aiSemCancel.load()) return false;
                    std::vector<std::string> inputs;
                    for (size_t i = b0; i < b1; i++) {
                        // the clamp, not a raw resize: a cut inside a multibyte
                        // sequence ships invalid UTF-8 and the door's 400 rides
                        // every halved retry - one curly quote killed the pass
                        std::string inp = WxU(cfg.embDocPrefix) + ix.chunks[i].text;
                        inputs.push_back(AiClipUtf8(inp, 1200));
                    }
                    std::vector<std::vector<float>> vecs;
                    AiError e;
                    if (AiEmbedBatch(cfg, inputs, false, vecs, e, &g_aiSemCancel) &&
                        vecs.size() == b1 - b0) {
                        if (ix.dim == 0 && !vecs.empty()) ix.dim = (int)vecs[0].size();
                        for (size_t i = b0; i < b1; i++) ix.vectors[i] = std::move(vecs[i - b0]);
                        AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                        wchar_t b[128];
                        _snwprintf_s(b, _TRUNCATE, L"Building semantic search… %zu/%zu", b1, total);
                        ev->a = WxU(b);
                        AiPost(ev);
                        return true;
                    }
                    eOut = e;
                    if (g_aiSemCancel.load() || b1 - b0 <= 1 || depth >= 8) return false;
                    size_t mid = b0 + (b1 - b0) / 2;
                    return self(self, b0, mid, depth + 1, eOut) &&
                           self(self, mid, b1, depth + 1, eOut);
                };
                // Google's OpenAI door caps an embeddings batch at 100 texts
                // / 20000 tokens - the 256-wide opening batch 400'd before the
                // pass settled in, and the halving burned two round trips
                // climbing down. Cloud doors open at 64 (64 x 1200 bytes sits
                // under both caps); a local engine still takes the wide batch.
                size_t batchN = AiIsLocalHost(WxU(cfg.baseUrl)) ? 256 : 64;
                for (size_t b0 = 0; b0 < total; ) {
                    if (g_aiSemCancel.load()) { g_aiSemActive = false; return; }
                    size_t b1 = min(b0 + batchN, total);
                    AiError e;
                    if (!slice(slice, b0, b1, 0, e)) {
                        g_aiSemActive = false;
                        if (g_aiSemCancel.load()) return;
                        AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
                        if (e.cat == AI_ERR_MODEL_NOT_FOUND) {
                            ev->a = WxU(L"Embedding model " + e.model +
                                        L" not found — check the Embedding Model field in AI settings."
                                        L" Using keyword search only.");
                        } else {
                            // the verdict now names itself: the old line buried
                            // every 401 / 429 / 400 under one phrase, and a user
                            // staring at "semantic search unavailable" had no
                            // way to tell WHICH failure held the pass down
                            std::wstring why = e.Friendly();
                            if (why.size() > 220) why.resize(220);
                            if (e.status > 0 && why.find(L"HTTP ") == std::wstring::npos)
                                why += L" (HTTP " + std::to_wstring(e.status) + L")";
                            ev->a = WxU(L"Semantic search failed: " + why +
                                        L" — keyword search stays available.");
                        }
                        AiPost(ev);
                        return;
                    }
                    b0 = b1;
                }
                ix.embModel = model;
                ix.prefixKey = WxU(prefixKey);
                AiIndexSave(ix);
            }
            // the vectors ride home: the pass used to end here with its work in a
            // local copy - the file was saved, the memory dropped, and the live
            // seat's dim stayed 0, so every answer in this session retrieved by
            // keyword alone and the freshly built nemotron embeddings never spoke
            // until the app restarted and the index loaded from disk. The handback
            // event merges them into the live seat on the UI thread.
            g_aiSemActive = false;
            g_aiSemReadyFlag = true;
            std::shared_ptr<AiDocIndex> done = std::make_shared<AiDocIndex>(std::move(ix));
            AiEv* ev = AiEvMake(AIEV_SEM_INDEX);
            ev->heavy = std::move(done);
            AiPost(ev);
            AiEv* st = AiEvMake(AIEV_SEM_STATUS);
            st->a = WxU(L"Semantic search ready.");
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
    // dead no matter which one held it
    if (g_aiSemActive) {
        g_aiSemCancel = true;
        AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
        ev->a = WxU(L"Semantic search stopped — press again to rebuild.");
        AiPost(ev);
        return;
    }
    if (g_aiIndexing) {
        AiEv* ev = AiEvMake(AIEV_SEM_STATUS);
        wchar_t b[160];
        // no "ready" in this line: the status pump greps the word to arm the
        // green button, and a still-building seat must never read armed
        _snwprintf_s(b, _TRUNCATE, L"Still indexing the document (page %d of %d) — "
                     L"the semantic pass starts when the index is done.",
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
                    c2->put_DefaultBackgroundColor({ 0, 0, 0, 0 });   // the grain field
                                              // shows through, the pages grid's surface
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
static void SumSendInit();
static void SumOpen(int mode);

static void SumPost(const std::string& j) {
    if (g_sumWeb) g_sumWeb->PostWebMessageAsJson(WxW(j).c_str());
}
static void SumSendInit() {
    AiLoadSettings();
    std::string sRange = AiGetSetting("summary.range", "20");
    int range = max(1, atoi(sRange.c_str()));
    g_sumRange = range;
    int words = max(100, atoi(AiGetSetting("summary.words", "1000").c_str()));
    std::string lang = AiGetSetting("summary.lang", "English");
    std::string genre = AiGetSetting("summary.default_genre", "nonfiction_classic");
    int font = max(10, min(24, atoi(AiGetSetting("summary.font", "13").c_str())));
    bool ping = AiGetSetting("summary.ping.enabled", "1") != "0";
    bool buffer = AiGetSetting("summary.buffer.enabled", "1") != "0";
    bool recap = AiGetSetting("summary.recap.enabled", "0") == "1";
    bool discord = AiGetSetting("summary.discord.enabled", "0") == "1";

    std::string j = "{\"cmd\":\"init\",\"mode\":\"" + std::string(g_sumMode ? "web" : "doc") + "\"";
    if (g_sumMode == 0) {
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
        j += ",\"genre\":" + WxJsonStr(genre);
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
    } else {
        j += ",\"words\":" + std::to_string(words);
        j += ",\"lang\":" + WxJsonStr(lang);
        j += ",\"prompts\":[{\"id\":\"web_nonfiction_classic\",\"title\":\"Nonfiction\"}]";
        j += ",\"font\":" + std::to_string(font);
    }
    j += ",\"model\":" + WxJsonStr(g_aiCfg.model);
    j += "}";
    g_sumInitDoc = (g_sumMode == 0) ? AiDocSessionKey(DVF().path) : std::string("web");
    SumPost(j);
}
static void SumEnsure() {
    if (g_sumCtr || g_sumBuilding) return;
    WebEnsureEnv();
    if (!g_wvEnv) {
        // the engine warms up asynchronously: a silent bail here left the seat
        // permanently empty, because the env's own completion retries the tabs
        // and the editor but never this seat - the popup's first open then
        // showed nothing but grain. A quiet knock retries until the env
        // answers, and stands down the moment the runtime is declared absent.
        if (g_sumWnd && !g_webFail) SetTimer(g_sumWnd, 4722, 250, nullptr);
        return;
    }
    if (g_sumWnd) KillTimer(g_sumWnd, 4722);
    g_sumBuilding = true;
    g_wvEnv->CreateCoreWebView2Controller(
        g_sumWnd,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                g_sumBuilding = false;
                if (FAILED(hr) || !c) return S_OK;
                g_sumCtr = c;
                // born against a hidden window (the prewarm seat): the runtime
                // hands the controller over not-rendered, and it stays that way
                // even after the popup unfolds - the grain showed, the page
                // never did. The one who shows the window says it may draw.
                c->put_IsVisible(IsWindowVisible(g_sumWnd) ? TRUE : FALSE);
                c->get_CoreWebView2(&g_sumWeb);
                wrl::ComPtr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2)
                    c2->put_DefaultBackgroundColor({ 0, 0, 0, 0 });   // the grain field
                                              // shows through, like the notes and
                                              // chat faces - the card sits on the
                                              // app's noise, not on its own paint
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
    if (g_sumMouseHook) { UnhookWindowsHookEx(g_sumMouseHook); g_sumMouseHook = nullptr; }
}
static void SumHide(bool fromOutside) {
    if (!g_sumWnd) return;
    if (g_sumWeb)   // the digest's reader seat rides out before the fold
        g_sumWeb->ExecuteScript(L"window.__sumSaveScroll&&window.__sumSaveScroll();", nullptr);
    if (g_sumCtr) g_sumCtr->put_IsVisible(FALSE);   // folded = not rendered, not
                                                    // dead: the page keeps its
                                                    // DOM, a generation keeps
                                                    // streaming into it, and the
                                                    // compositor stops burning
                                                    // frames nobody can see
    SumSavePlacement();
    ShowWindow(g_sumWnd, SW_HIDE);
    SumUnhookMouse();
    if (fromOutside) g_sumHideTick = GetTickCount();
}
// The frame's folds drive the seat's render. A minimize is a fold SumHide
// never sees: Windows hides an owned popup with its owner, the controller
// keeps claiming visible, and the surface its composition rode on is thrown
// away - the restore then re-shows the popup over a dead surface and the
// webview paints the white card the user reported. The handback must be
// explicit: FALSE on the way down, TRUE on the way back rebuilds the
// composition fresh. Both calls no-op when nothing actually moved.
static void SumSyncVisible() {
    if (!g_sumCtr || !g_sumWnd) return;
    BOOL want = (IsWindowVisible(g_sumWnd) && g.main && !IsIconic(g.main)) ? TRUE : FALSE;
    g_sumCtr->put_IsVisible(want);
}
static LRESULT CALLBACK SumMouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_sumWnd && IsWindowVisible(g_sumWnd)) {
        switch (wp) {
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: {
            POINT pt = ((MSLLHOOKSTRUCT*)lp)->pt;
            HWND w = WindowFromPoint(pt);
            HWND root = w ? GetAncestor(w, GA_ROOT) : nullptr;
            if (root && root != g_sumWnd && (root == g.main || IsChild(g.main, root)))
                PostMessageW(g_sumWnd, WM_APP + 61, 0, 0);   // a click inside the
            break; }   // frame, outside the navigator - the fold itself runs on
        }                  // the pump: a hook must never do file I/O
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static void SumHookMouse() {
    if (!g_sumMouseHook)
        g_sumMouseHook = SetWindowsHookExW(WH_MOUSE_LL, SumMouseHookProc, g.hinst, 0);
}
static void SumSavePlacement() {
    if (!g_sumWnd || IsIconic(g_sumWnd)) return;
    RECT wr; GetWindowRect(g_sumWnd, &wr);
    double dpi = g.dpi > 0 ? g.dpi : 1.0;
    AiSetSetting("summary.win.w", std::to_string((int)((wr.right - wr.left) / dpi)));
    AiSetSetting("summary.win.h", std::to_string((int)((wr.bottom - wr.top) / dpi)));
    if (g.main && !IsIconic(g.main)) {   // the popup's seat is relative to the
        RECT mr; GetWindowRect(g.main, &mr);   // frame - it belongs to the app,
        AiSetSetting("summary.win.relx",       // not to the desktop
                     std::to_string((int)((wr.left - mr.left) / dpi)));
        AiSetSetting("summary.win.rely",
                     std::to_string((int)((wr.top - mr.top) / dpi)));
    }
}
// the frame moved: the popup rides along like any docked pane, and the
// WINDOWPOSCHANGING clamp below keeps it inside the frame while it goes
static void SumOwnerMoved() {
    if (!g.main || IsIconic(g.main)) return;
    RECT mr; GetWindowRect(g.main, &mr);
    if (g_sumOwnerPrev.right > g_sumOwnerPrev.left && g_sumWnd && IsWindowVisible(g_sumWnd)) {
        int dx = mr.left - g_sumOwnerPrev.left, dy = mr.top - g_sumOwnerPrev.top;
        if (dx || dy) {
            RECT wr; GetWindowRect(g_sumWnd, &wr);
            SetWindowPos(g_sumWnd, nullptr, wr.left + dx, wr.top + dy, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    g_sumOwnerPrev = mr;
}
static void SumToggle(int mode) {
    if (g_sumHideTick && GetTickCount() - g_sumHideTick < 400) {
        g_sumHideTick = 0;   // this button click IS the outside click that just
        return;              // folded it - a toggle-off, not an invitation back
    }
    if (g_sumWnd && IsWindowVisible(g_sumWnd) && g_sumMode == mode) {
        SumHide(false);   // the toolbar button is a toggle: open again rolls it up
        return;
    }
    SumOpen(mode);
}
// what the page's current init was built for - a reopen only re-sends init
// when the document or surface moved under the hood, because re-initing a
// living page wipes its digest view, its busy state and its scroll, exactly
// the "close it and everything resets" the fold was built to stop
static bool SumInitStale() {
    std::string key = (g_sumMode == 0) ? AiDocSessionKey(DVF().path) : std::string("web");
    return key != g_sumInitDoc;
}
// the frame moved while the popup was folded: a reopen re-seats it inside the
// frame (the WM_WINDOWPOSCHANGING clamp only rides real moves, and a bare
// ShowWindow carries none)
static void SumClampIntoFrame() {
    if (!g_sumWnd || !g.main || IsIconic(g.main)) return;
    RECT mr; GetWindowRect(g.main, &mr);
    if (mr.right <= mr.left || mr.bottom <= mr.top) return;
    RECT wr; GetWindowRect(g_sumWnd, &wr);
    int w = wr.right - wr.left, hgt = wr.bottom - wr.top;
    int x = max(mr.left, min(wr.left, mr.right - w));
    int y = max(mr.top, min(wr.top, mr.bottom - hgt));
    if (x != wr.left || y != wr.top)
        SetWindowPos(g_sumWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
// the window itself, born hidden: SumPrewarm builds the seat at idle and the
// first SumOpen just unfolds it
static void SumCreateWindow() {
    if (g_sumWnd) return;
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
    double dpi = g.dpi > 0 ? g.dpi : 1.0;
    int defW = (int)(560 * dpi), defH = (int)(630 * dpi);
    std::string sW = AiGetSetting("summary.win.w");
    std::string sH = AiGetSetting("summary.win.h");
    int w = sW.empty() ? defW : max((int)(460 * dpi), (int)(atof(sW.c_str()) * dpi));
    int h = sH.empty() ? defH : max((int)(390 * dpi), (int)(atof(sH.c_str()) * dpi));
    // the popup's seat is the frame's seat: centered over the app on first
    // open, then the saved offset from the frame's corner (a plain absolute
    // restore put it wherever the desktop last saw it - the old windows-own-
    // desktop behavior the popup is here to bury)
    RECT mr{ 0, 0, 0, 0 };
    if (g.main) GetWindowRect(g.main, &mr);
    int x = mr.left + max(0, (int)(((mr.right - mr.left) - w) / 2));
    int y = mr.top + max(0, (int)(((mr.bottom - mr.top) - h) / 3));
    std::string sRX = AiGetSetting("summary.win.relx");
    std::string sRY = AiGetSetting("summary.win.rely");
    if (!sRX.empty() && !sRY.empty()) {
        x = mr.left + (int)(atof(sRX.c_str()) * dpi);
        y = mr.top + (int)(atof(sRY.c_str()) * dpi);
    } else {
        std::string sL = AiGetSetting("summary.win.left");
        std::string sT = AiGetSetting("summary.win.top");
        if (!sL.empty() && !sT.empty()) {   // a pre-popup session's absolute seat
            x = (int)(atof(sL.c_str()) * dpi);   // migrates: the clamp below
            y = (int)(atof(sT.c_str()) * dpi);   // pulls it back inside the app
        }
    }
    if (mr.right > mr.left) {   // never even propose a seat outside the frame
        int maxX = max(mr.left, mr.right - w);
        int maxY = max(mr.top, mr.bottom - h);
        if (x < mr.left) x = mr.left;
        if (x > maxX) x = maxX;
        if (y < mr.top) y = mr.top;
        if (y > maxY) y = maxY;
    }
    // The main frame's own birth - OVERLAPPEDWINDOW minus CAPTION - with the
    // resize bands kept. WS_POPUP + THICKFRAME was tried first and Windows 11
    // punished it: DWM paints a popup's top frame strip in the system's light
    // theme colors and no NCCALCSIZE can claim it away - the white band that
    // rode the card's top edge. The main-family style is claimable; the main
    // frame has worn it captionless its whole life with no such band. No
    // caption boxes, so the HTML bar's drag handle can't maximize it by
    // accident. TOOLWINDOW keeps it out of Alt-Tab - it is a pane that
    // floats, not a window that lives on its own. Born HIDDEN: the prewarm
    // seat is built before anyone looks at it.
    DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_CAPTION) | WS_CLIPCHILDREN;
    RECT rc{ 0, 0, w, h };
    AdjustWindowRect(&rc, style, FALSE);
    g_sumWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"docsy_sum", L"Page Summary",
                               style,
                               x, y,
                               rc.right - rc.left, rc.bottom - rc.top,
                               g.main, nullptr, g.hinst, nullptr);
    if (g_sumWnd) {
        SetWindowPos(g_sumWnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        DwmApply(g_sumWnd);    // the card's rounded corners + tinted border
        g_sumOwnerPrev = RECT{};
        if (g.main) GetWindowRect(g.main, &g_sumOwnerPrev);
    }
}
// the idle seat: two and a half seconds after launch the frame builds its
// navigator hidden - window, controller, page and all - so the first click on
// the toolbar unfolds a living popup instead of a construction site
static void SumPrewarm() {
    SumCreateWindow();
    SumEnsure();
}
static void SumOpen(int mode) {
    SumCreateWindow();
    if (!g_sumWnd) return;
    if (g_sumMode != mode) {
        g_sumMode = mode;
        if (g_sumCtr) { g_sumCtr->Close(); g_sumCtr = nullptr; g_sumWeb = nullptr; g_sumReady = false; }
    }
    ShowWindow(g_sumWnd, SW_SHOWNOACTIVATE);
    if (g_sumCtr) g_sumCtr->put_IsVisible(TRUE);   // the fold took the render
                                                   // away; the unfold hands it
                                                   // back (the webview was born
                                                   // hidden and never auto-follows
                                                   // its parent's visibility)
    SetForegroundWindow(g_sumWnd);
    SumHookMouse();
    SumEnsure();   // no-ops while the webview lives; re-arms it after a mode switch
    SumClampIntoFrame();
    if (g_sumReady && SumInitStale()) SumSendInit();
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
                     int words, std::string lang, std::string genre, bool bypass) {
    AiLoadSettings();
    AiCfg cfg = g_aiCfg;
    std::wstring bodyW = std::regex_replace(rangeText,
        std::wregex(L"\\[{1,2}p\\.\\s*\\d+\\]{1,2}"), L"");
    if (AiLetters(bodyW) < 50) {
        AiEv* e = AiEvMake(AIEV_SUM_NOTEXT);
        e->gen = gen;
        AiPost(e);                       // the panel learns on the pump's thread
        if (run == g_sumRun) g_sumBusy = false;
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
        if (run == g_sumRun) g_sumBusy = false;
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
        if (run == g_sumRun) g_sumBusy = false;
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
            if (run == g_sumRun) g_sumBusy = false;
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
    if (run == g_sumRun) g_sumBusy = false;
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
        // A second Start is a REDIRECT, not a drop. The old gate swallowed the
        // click while a run was busy: the page kept saying "Reading pages..."
        // and every back / forth / range switch went nowhere - the user's
        // "nothing, same shit". The extraction pump is a cheap cursor (it just
        // turns to the new range) and a streaming model call dies on its own
        // per-run cancel flag; the old run's leftovers are all gen-gated away
        // by the page. The new run owns the busy flag from here.
        const crxk::J* g = m.get("gen"), * w = m.get("words"), * st = m.get("start"),
            * rg = m.get("range"), * lg = m.get("lang"), * gr = m.get("genre"), * bp = m.get("bypass");
        unsigned long long gen = g ? (unsigned long long)g->d : ++g_sumGen;
        g_sumGen = gen;                      // the accepted gen: stale completions
                                             // recognize themselves against it
        ++g_sumRun;                          // the new run owns the busy flag
        if (g_sumBusy) {
            if (g_sumCancel) *g_sumCancel = true;   // the old stream folds quietly
            if (!(g_range.active && (g_range.what == 0 || g_range.what == 2)))
                g_range.active = false;   // a model-phase run: the pump is idle;
                                          // an extraction in flight just gets
                                          // overwritten by the AiRangeStart below
        }
        g_sumCancel = std::make_shared<std::atomic<bool>>(false);
        int words = w ? (int)w->d : 1000;
        std::string lang = lg && lg->t == 's' ? lg->s : "English";
        std::string genre = gr && gr->t == 's' ? gr->s : "nonfiction_classic";
        bool bypass = bp && bp->b;
        g_sumBusy = true;
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
    } else if (t == "cancel") {
        // the buffer fast-path displayed its ready summary while a generation
        // was still breathing behind it: the old run folds quietly (its posts
        // are gen-gated away) and the busy flag dies with it - no reset, the
        // cache and the digest stay
        if (g_sumCancel) *g_sumCancel = true;
        g_range.active = false;
        KillTimer(g.main, 4714);
        g_sumBusy = false;
    } else if (t == "reset") {
        if (g_sumCancel) *g_sumCancel = true;
        g_range.active = false;              // an extraction in flight dies with
        KillTimer(g.main, 4714);             // the reset - the old code left the
                                             // pump grinding a wiped range
        g_sumBusy = false;
        if (g_sumMode == 0) {
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
    } else if (t == "save-scroll") {   // the digest's reader seat, per document
        const crxk::J* tp = m.get("top");
        if (g_sumMode == 0 && tp && tp->t == 'd') {
            DocState& D = DVF();
            if (!D.path.empty()) {
                std::string docId = AiDocSessionKey(D.path);
                int top = max(0, (int)tp->d);
                AiSetSetting("summary.digest." + docId + ".scroll", std::to_string(top));
            }
        }
    } else if (t == "close") {
        // the chrome's own close chip (DialogChrome's red mark) or Esc: the
        // fold is a hide - the webview, its digest and a running generation
        // all keep breathing behind it
        SumHide(false);
    } else if (t == "drag") {
        // the HTML title bar's drag handle: hand the move to the OS caption
        // loop - ReleaseCapture is what lets the mouse leave the WebView2
        if (g_sumWnd) {
            ReleaseCapture();
            SendMessageW(g_sumWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
    } else if (t == "page") {
        const crxk::J* p = m.get("page");
        if (p && p->t == 'd' && g_sumMode == 0) {
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
        if (g_sumMode == 0) {
            DocState& D = DVF();
            if (!D.path.empty()) {
                std::string docId = AiDocSessionKey(D.path);
                const crxk::J* txt = m.get("text"), * f = m.get("first"), * l = m.get("last"), * mdl = m.get("model");
                if (txt && txt->t == 's') AiSetSetting("summary.digest." + docId, txt->s);
                if (f && f->t == 'd') AiSetSetting("summary.digest." + docId + ".first", std::to_string((int)f->d));
                if (l && l->t == 'd') AiSetSetting("summary.digest." + docId + ".last", std::to_string((int)l->d));
                if (mdl && mdl->t == 's') AiSetSetting("summary.digest." + docId + ".model", mdl->s);
            }
        }
    } else if (t == "test-range") {
        // the title bar's beaker (Avalanche's TestRangeRequested -> OpenAiTestForRange):
        // the tester_standard probe reads the current stretch and reports the first
        // and last sentences it saw - proof the extraction carries real text
        if (g_sumBusy || g_sumMode != 0) return;
        DocState& D = DVF();
        if (D.path.empty()) return;
        const crxk::J* f = m.get("first"), * l = m.get("last");
        int first = f && f->t == 'd' ? max(1, (int)f->d) : g_sumStart;
        int last = l && l->t == 'd' ? min((int)l->d, D.n) : min(g_sumStart + g_sumRange - 1, D.n);
        if (first > last) return;
        g_sumBusy = true;
        AiRangeStart(D.path, first, last, 2, ++g_sumGen);
    } else if (t == "prefetch") {
        const crxk::J* st = m.get("start"), * rg = m.get("range"),
            * w = m.get("words"), * lg = m.get("lang"), * gr = m.get("genre");
        if (g_sumMode == 0 && st && st->t == 'd') {
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
                if (g_sumWnd) SetTimer(g_sumWnd, 4723, 1200, nullptr);
            }
        }
    }
}
static LRESULT CALLBACK SumWndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_NCCALCSIZE: {   // DialogChrome: the caption row joins the client; the
                            // OS side/bottom borders stay for the resize bands.
                            // Maximized clamps to the work area like the main frame.
        if (!wp || IsIconic(h)) break;
        NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
        RECT orig = p->rgrc[0];
        if (IsZoomed(h)) {
            HMONITOR mon = MonitorFromRect(&orig, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof mi };
            if (!mon || !GetMonitorInfoW(mon, &mi)) break;
            p->rgrc[0].left   = max(orig.left,   mi.rcWork.left);
            p->rgrc[0].top    = max(orig.top,    mi.rcWork.top);
            p->rgrc[0].right  = min(orig.right,  mi.rcWork.right);
            p->rgrc[0].bottom = min(orig.bottom, mi.rcWork.bottom);
            return 0;
        }
        DefWindowProcW(h, m, wp, lp);          // the OS lays out its full frame...
        g_sumFrame = p->rgrc[0].left - orig.left;
        p->rgrc[0].top = orig.top;             // ...and only the caption is dropped
        return 0;
    }
    case WM_NCHITTEST: {   // the top resize grip replaces the stripped OS border;
                           // side/bottom grips come from DefWindowProc, and the
                           // HTML bar drags itself through the "drag" message
        POINT s{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc; GetWindowRect(h, &rc);
        if (!IsZoomed(h) && g_sumFrame > 0 && s.y < rc.top + g_sumFrame) {
            if (s.x <  rc.left + g_sumFrame) return HTTOPLEFT;
            if (s.x >= rc.right - g_sumFrame) return HTTOPRIGHT;
            return HTTOP;
        }
        break;
    }
    case WM_WINDOWPOSCHANGING: {   // the popup is part of the app: no drag, no
                                   // restore, no ride-along may carry it past
                                   // the frame that owns it
        WINDOWPOS* p = (WINDOWPOS*)lp;
        if (p && g.main && !(p->flags & SWP_NOMOVE)) {
            RECT mr;
            GetWindowRect(g.main, &mr);
            int w = p->cx, hgt = p->cy;
            if ((p->flags & SWP_NOSIZE) || w <= 0 || hgt <= 0) {
                RECT wr; GetWindowRect(h, &wr);
                w = wr.right - wr.left; hgt = wr.bottom - wr.top;
            }
            int maxX = max(mr.left, mr.right - w);
            int maxY = max(mr.top, mr.bottom - hgt);
            if (p->x < mr.left) p->x = mr.left;
            if (p->x > maxX) p->x = maxX;
            if (p->y < mr.top) p->y = mr.top;
            if (p->y > maxY) p->y = maxY;
        }
        break;
    }
    case WM_ERASEBKGND: {   // the family noise: one grain tile, phased to the
                            // frame the way every pane's fill phases (the ask
                            // dialog's SetBrushOrgEx recipe)
        HDC dc = (HDC)wp;
        RECT rc; GetClientRect(h, &rc);
        if (g.main) {
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
            SetBrushOrgEx(dc, -po.x, -po.y, nullptr);
        }
        FillRect(dc, &rc, GrainBrush(1));
        return 1;
    }
    case WM_PAINT: {   // the transparent webview lets the grain through - the
                       // window itself lays the tile down on every expose, and
                       // every expose also re-arms the render: the frame's
                       // minimize folded this window without a SumHide (owned
                       // windows hide with their owner) and its restore re-shows
                       // it over a composition the surface threw away - the
                       // white card. The handback below rebuilds it fresh; on a
                       // living controller it is a no-op.
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        if (g.main) {
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);
            SetBrushOrgEx(dc, -po.x, -po.y, nullptr);
        }
        FillRect(dc, &rc, GrainBrush(1));
        EndPaint(h, &ps);
        SumSyncVisible();
        return 0;
    }
    case WM_GETMINMAXINFO: {   // SummaryWindow: MinWidth 460, MinHeight 390
        DefWindowProcW(h, m, wp, lp);
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = (int)(460 * g.dpi);
        mmi->ptMinTrackSize.y = (int)(390 * g.dpi);
        return 0;
    }
    case WM_APP + 61:   // the mouse watcher's verdict, run on the pump: a click
        SumHide(true);  // inside the frame folded the navigator from outside
        return 0;
    case WM_TIMER:
        if (wp == 4722) {   // the seat's knock on the engine's door: the env was
            KillTimer(h, 4722);   // still warming when SumEnsure last looked;
            SumEnsure();          // SumEnsure re-arms the knock itself until the
            return 0;             // runtime answers or is declared absent
        }
        if (wp == 4723) {   // the buffer's deferred kick: the open's paint has
            KillTimer(h, 4723);   // landed; if the pump is free, the background
            g_sumPf.on = false;   // reader starts turning pages for the next
            if (!g_range.active) {   // stretch
                DocState& D = DVF();
                if (!D.path.empty() && g_sumPf.first >= 1 && g_sumPf.first <= D.n) {
                    g_range.words = g_sumPf.words;   // the cache key and the
                    g_range.lang = g_sumPf.lang;     // prompt ride the job, set
                    g_range.genre = g_sumPf.genre;   // at fire time, not request
                    int last = min(g_sumPf.first + g_sumPf.pages - 1, D.n);
                    AiRangeStart(D.path, g_sumPf.first, last, 3, 0);
                }
            }
            return 0;
        }
        break;
    case WM_SIZE: {
        if (g_sumCtr) {
            RECT r; GetClientRect(h, &r);
            g_sumCtr->put_Bounds(r);
        }
        DwmApply(h);       // corner rounding flips when the window maximizes
        return 0;
    }
    case WM_EXITSIZEMOVE:   // one settings write per gesture, not per pixel -
                            // the old WM_MOVE save rewrote settings.json four
                            // times a pixel while the window dragged
        SumSavePlacement();
        return 0;
    case WM_DESTROY: {   // the one true teardown: the frame died (owned popups
                         // go with it) - only here does the navigator really
                         // stop: hook out, placement saved, generation cancelled
        SumSavePlacement();
        SumUnhookMouse();
        g_sumHideTick = 0;
        if (g_sumCancel) *g_sumCancel = true;
        g_range.active = false;
        g_sumWnd = nullptr;
        g_sumCtr = nullptr; g_sumWeb = nullptr; g_sumReady = false;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
