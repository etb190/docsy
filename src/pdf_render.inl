// ---- film-grain margins (port of KillerPDF's procedural GrainTexture) ----
// One seamless 256x256 tile of gray specks: Gaussian around 128 (sigma 150),
// softened with wrapping neighbours so the tile repeats without a seam, then
// contrast-boosted to keep dark and bright specks distinct. KillerPDF paints
// this tile at alpha 70/255 and then scales the whole overlay down by its
// theme resource GrainOpacity - 0.24 on the dark themes - so the effective
// blend is (70/255) * 0.24 ~= 0.066. Baking that product in reproduces the
// rendered frost exactly; painting at the raw alpha 70 reads as harsh TV
// static. The seed is fixed so every build shows the identical pattern, and
// the tile stays fixed to the window while pages scroll, matching KillerPDF's
// viewport-fixed overlay.
//
// Two KillerPDF details this port now keeps exactly:
//  * the lean specks. 40% of the specks lean toward the theme's secondary
//    color - the complement of the accent hue (LeanFor/Generate in
//    Controls/GrainTexture.cs). The green #22bb56 accent sits at hue 132, the
//    complement 312 lands on pink, and the family rule leans greens to purple
//    (285) instead: FromHsv(285, 0.55, 0.85) = RGB(187, 98, 217). A leaning
//    speck keeps its own lightness (k = 0.35 + 1.1*shade/255) and mixes 80% of
//    the way over, so the tint reads as a cool grey, never a color wash.
//  * the surfaces. The dark theme paints the title bar, the sidebar and the
//    footer the SAME BackgroundBrush #1c1c1c under one shared tile
//    (GrainBrushShared), which is why KillerPDF's frame reads as one
//    continuous container; only the document card is lighter (PaneBrush
//    #3a3a3a). docsy now bakes the same neutrals: slot 0 is the document
//    canvas, slot 1 the shared dark chrome (sidebar + footer + title bar all
//    return it), so the two frame the page area on two sides seamlessly.
static HBRUSH g_grainBrush[3] = { nullptr, nullptr, nullptr };
static HBITMAP g_grainBmp[3] = { nullptr, nullptr, nullptr };

static HBRUSH GrainBrush(int which) {
    if (which > 2) which = 2;
    if (g_grainBrush[which]) return g_grainBrush[which];
    const int S = 256;
    const double Spread = 150.0, Soften = 0.10, Contrast = 1.35;
    const double LeanShare = 0.40, LeanMix = 0.80;
    const double Alpha = 70.0 / 255.0 * 0.24;   // tile alpha 70 times the dark-theme GrainOpacity
    const BYTE back[3][3] = {
        { 58, 58, 58 },   // document canvas  - KillerPDF's BgCanvas/PaneBrush #3a3a3a
        { 28, 28, 28 },   // dark chrome      - BackgroundBrush/TitleBarBrush/FooterBackgroundBrush #1c1c1c
        { 20, 20, 20 },   // floating surface - MenuBackgroundBrush #141414 (the Find bar)
    };                                    // stored B,G,R; all neutral greys, exactly as the theme states them
    const double leanC[3] = { 217.0, 98.0, 187.0 };   // the purple lean, B,G,R
    auto cl = [](double v) { return v < 0 ? 0.0 : v > 255 ? 255.0 : v; };
    unsigned long long st = 1337;  // splitmix64: deterministic stream, same fixed-seed intent
    auto rnd = [&st]() {
        st += 0x9E3779B97F4A7C15ULL;
        unsigned long long z = st;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return ((z ^ (z >> 31)) >> 11) * (1.0 / 9007199254740992.0);  // [0,1)
    };
    std::vector<double> raw((size_t)S * S);
    std::vector<BYTE> lean((size_t)S * S);
    for (size_t i = 0; i < raw.size(); i++) {
        double u1 = 1.0 - rnd(), u2 = rnd();
        raw[i] = cl(128.0 + Spread * std::sqrt(-2.0 * std::log(u1)) * std::cos(2 * 3.14159265358979323846 * u2));
        lean[i] = rnd() < LeanShare;
    }
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), S, S, 1, 32, BI_RGB };
    BYTE* px = nullptr;
    g_grainBmp[which] = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    if (!px || !g_grainBmp[which]) return (HBRUSH)GetStockObject(GRAY_BRUSH);
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            size_t i = (size_t)y * S + x;
            double around = (raw[((y + S - 1) % S) * S + x] + raw[((y + 1) % S) * S + x]
                           + raw[y * S + (x + S - 1) % S] + raw[y * S + (x + 1) % S]) / 4.0;
            double soft = raw[i] * (1.0 - Soften) + around * Soften;
            double shade = cl(128.0 + (soft - 128.0) * Contrast);
            double speck[3] = { shade, shade, shade };
            if (lean[i]) {
                double k = 0.35 + 1.1 * shade / 255.0;      // keeps the speck's own lightness
                for (int c = 0; c < 3; c++)
                    speck[c] = shade * (1.0 - LeanMix) + cl(leanC[c] * k) * LeanMix;
            }
            BYTE* p = px + i * 4;
            for (int c = 0; c < 3; c++) p[c] = (BYTE)std::lround(back[which][c] * (1.0 - Alpha) + speck[c] * Alpha);
            p[3] = 255;
        }
    }
    g_grainBrush[which] = CreatePatternBrush(g_grainBmp[which]);
    if (!g_grainBrush[which]) return (HBRUSH)GetStockObject(GRAY_BRUSH);
    return g_grainBrush[which];
}

static void ClearCache(DocState& D) {
    for (auto& kv : D.cache) DeleteObject(kv.second.b);
    D.cache.clear();
    D.cacheBytes = 0;
}

static void CacheDrop(DocState& D, int i) {   // one entry out, byte weight with it
    auto it = D.cache.find(i);
    if (it == D.cache.end()) return;
    D.cacheBytes -= (long long)it->second.w * it->second.h * 4;
    DeleteObject(it->second.b);
    D.cache.erase(it);
}

static HFONT UiFont() {
    if (!g_uifont) {
        g_uifont = CreateFontW(-MulDiv(9, (int)(g.dpi * 96), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    return g_uifont;
}

// The BMP the engine encodes is a plain 32bpp file: parse its headers straight
// into a DIB section instead of paying a GDI+ decode AND a GetHBITMAP copy on
// every page - the pixel copy is the only work left. Anything unexpected
// (palette, RLE, odd masks, absurd sizes, a short read) answers nullptr and
// the caller falls back to the GDI+ path, so this can stay naive about
// everything it does not recognize.
static HBITMAP BmpStreamToDib(IStream* is) {
    BITMAPFILEHEADER fh{};
    ULONG got = 0;
    if (!is || FAILED(is->Read(&fh, sizeof fh, &got)) || got != sizeof fh || fh.bfType != 0x4D42)
        return nullptr;
    BITMAPINFOHEADER ih{};
    if (FAILED(is->Read(&ih, sizeof ih, &got)) || got != sizeof ih || ih.biSize < sizeof ih)
        return nullptr;
    if (ih.biSize > sizeof ih) {                        // V4/V5 header: the shape is the
        LARGE_INTEGER skip{};                           // same 40-byte base plus extra fields
        skip.QuadPart = (LONGLONG)(ih.biSize - sizeof ih);
        if (FAILED(is->Seek(skip, STREAM_SEEK_CUR, nullptr))) return nullptr;
    }
    if (ih.biPlanes != 1 || ih.biBitCount != 32) return nullptr;
    if (ih.biCompression != BI_RGB && ih.biCompression != BI_BITFIELDS) return nullptr;
    if (ih.biCompression == BI_BITFIELDS) {             // only the standard BGRx masks ride -
        DWORD m[3]{};                                   // they sit right after the base header
        if (FAILED(is->Read(m, sizeof m, &got)) || got != sizeof m) return nullptr;   // in both the
        if (m[0] != 0x00FF0000u || m[1] != 0x0000FF00u || m[2] != 0x000000FFu) return nullptr;   // 40-byte and V4/V5 shapes
    }
    if (ih.biWidth <= 0 || ih.biWidth > 30000 || ih.biHeight == 0 ||
        ih.biHeight > 30000 || ih.biHeight < -30000) return nullptr;
    int w = (int)ih.biWidth, hh = (int)labs((long)ih.biHeight);
    long long stride = (long long)w * 4, bytes = stride * (long long)hh;
    if (bytes <= 0 || bytes > (512LL << 20)) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;                        // top-down DIB: rows in scan order
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC tdc = CreateCompatibleDC(nullptr);
    HBITMAP hb = tdc ? CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (tdc) DeleteDC(tdc);
    if (!hb || !bits) { if (hb) DeleteObject(hb); return nullptr; }
    std::vector<BYTE> buf((size_t)bytes);
    if (FAILED(is->Read(buf.data(), (ULONG)bytes, &got)) || (long long)got != bytes) {
        DeleteObject(hb);
        return nullptr;
    }
    BYTE* dst = (BYTE*)bits;
    if (ih.biHeight < 0) memcpy(dst, buf.data(), (size_t)bytes);      // already top-down
    else for (int y = 0; y < hh; y++)                                 // bottom-up: flip
        memcpy(dst + (size_t)y * stride, buf.data() + (size_t)(hh - 1 - y) * stride, (size_t)stride);
    return hb;
}

// Every rasterizer of a PdfDocument - the worker's RenderCore and the UI
// thread's own renders (the OCR pass in RenderSoft, the save writer, shot
// mode) - queues behind this one gate: Windows.Data.Pdf does not survive two
// concurrent RenderToStreamAsync calls on one document, and before the gate a
// pre-warm OCR 250ms after a page settled raced the worker's render of the
// same page and took the process down. The wait is bounded by ONE page render
// (the lock is taken per call, never across the queue), and no path holds it
// together with the job-queue mutex, so the worker and the drain cannot cycle.
static std::mutex g_renderM;

static HBITMAP RenderCore(winrt::Windows::Data::Pdf::PdfDocument doc, int page, int w, int h,
                          int rot, int flip) {
    try {
        std::lock_guard<std::mutex> lk(g_renderM);
        // the Transform's quarter-turns and flips bake into the render: the page
        // rasterizes at its UNROTATED aspect, then GDI+ rotates/flips into the
        // caller's (already swapped) bitmap, so every downstream path - layout,
        // hit tests, annotation painting - reads ordinary w x h pixels
        int rw = (rot % 2 == 0) ? w : h, rh = (rot % 2 == 0) ? h : w;
        auto pageObj = doc.GetPage((uint32_t)page);
        PdfPageRenderOptions o;
        o.DestinationWidth((uint32_t)max(1, rw));
        o.DestinationHeight((uint32_t)max(1, rh));
        o.BackgroundColor(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
        o.BitmapEncoderId(winrt::Windows::Graphics::Imaging::BitmapEncoder::BmpEncoderId()); // raw BMP: no PNG cost
        InMemoryRandomAccessStream st;
        pageObj.RenderToStreamAsync(st, o).get();
        st.Seek(0);
        winrt::com_ptr<IStream> is;
        winrt::check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(st), winrt::guid_of<IStream>(), is.put_void()));
        if (rot == 0 && flip == 0) {
            HBITMAP fast = BmpStreamToDib(is.get());   // one copy, no GDI+ decode:
            if (fast) return fast;                     // misses fall through to GDI+
        }
        Gdiplus::Bitmap bmp(is.get());
        if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;
        HBITMAP hb = nullptr;
        if (rot == 0 && flip == 0) {
            bmp.GetHBITMAP(Gdiplus::Color(255, 255, 255), &hb);
            return hb;
        }
        Gdiplus::Bitmap* out = bmp.Clone(0, 0, bmp.GetWidth(), bmp.GetHeight(), PixelFormat32bppARGB);
        if (!out || out->GetLastStatus() != Gdiplus::Ok) { delete out; return nullptr; }
        // the compose order matches TransformApply's coordinate map exactly:
        // flips first, then the quarter-turns, one named call each - no enum
        // arithmetic to miscount
        if (flip & 1) out->RotateFlip(Gdiplus::RotateNoneFlipX);
        if (flip & 2) out->RotateFlip(Gdiplus::RotateNoneFlipY);
        if ((rot & 3) == 1) out->RotateFlip(Gdiplus::Rotate90FlipNone);
        else if ((rot & 3) == 2) out->RotateFlip(Gdiplus::Rotate180FlipNone);
        else if ((rot & 3) == 3) out->RotateFlip(Gdiplus::Rotate270FlipNone);
        out->GetHBITMAP(Gdiplus::Color(255, 255, 255), &hb);
        delete out;
        return hb;
    } catch (...) { return nullptr; }
}

static HBITMAP Render(DocState& D, int i, int w, int h) {
    int rot = D.prot.count(i) ? D.prot[i] : 0;
    int flip = D.pflip.count(i) ? D.pflip[i] : 0;
    return RenderCore(D.doc, i, w, h, rot, flip);
}

// ---- the async render pipeline ----
// Every page and thumbnail renders on one background worker: the UI thread
// asks (GetBmp / SidePump), gets a placeholder the same frame, and results
// land through a posted message whose drain inserts them into the same caches
// the sync path used. Shot mode (g_sync) keeps the old synchronous behavior -
// the CI previews composite one frame directly, so they must have every pixel
// now. Jobs carry the document's stable id and an agile PdfDocument ref, never
// a DocState*: the tabs vector reallocates, and the agile ref keeps the engine
// object alive even when the tab closes mid-render (the drain then finds no
// owner and frees the pixels).
static bool g_sync = false;                 // shot mode: render inside the asking call
static std::mutex g_rqM;
static std::condition_variable g_rqCv;
static bool g_rqQuit = false;
static HANDLE g_rqThread = nullptr;
static unsigned long long g_rqSeq = 0;      // bump on every ask; the worker answers the newest first
enum { RJ_PAGE, RJ_THUMB };
struct RJob {
    int docId, page, kind;
    int w, h, rot, flip;                    // rot: quarter-turns; flip: 1=X 2=Y (RenderCore's own bits)
    winrt::agile_ref<winrt::Windows::Data::Pdf::PdfDocument> doc;
    unsigned long long seq;
};
struct RRes {
    int docId, page, kind;
    int w, h, rot, flip;                    // pages: rot = the packed Ent key (quarter | flip<<4)
    HBITMAP b;                              // may be null: a failed thumb caches its null as a placeholder
    unsigned long long seq;
};
static std::vector<RJob> g_rq;              // pending, consumed by the worker
static std::vector<RRes> g_rqDone;          // finished, drained on the UI thread
static bool RqSame(const RJob& j, int docId, int page, int kind, int w, int h, int rot, int flip) {
    return j.docId == docId && j.page == page && j.kind == kind &&
           j.w == w && j.h == h && j.rot == rot && j.flip == flip;
}
static void RqEnqueue(DocState& D, int i, int kind, int w, int h, int rot, int flip) {
    if (!D.id || !D.doc) return;
    std::lock_guard<std::mutex> lk(g_rqM);
    for (size_t k = 0; k < g_rq.size(); k++) {          // supersede a stale-size job for
        if (g_rq[k].docId == D.id && g_rq[k].page == i && g_rq[k].kind == kind) {   // the same slot
            if (RqSame(g_rq[k], D.id, i, kind, w, h, rot, flip)) {
                g_rq[k].seq = ++g_rqSeq;                // already asked: just raise its turn
                return;
            }
            g_rq[k] = g_rq.back(); g_rq.pop_back(); k--;    // old size is wasted work now
        }
    }
    for (auto& r : g_rqDone)                            // a matching result is waiting to
        if (r.docId == D.id && r.page == i && r.kind == kind &&
            r.w == w && r.h == h && r.rot == rot && r.flip == flip) return;   // drain: no second render
    g_rq.push_back(RJob{ D.id, i, kind, w, h, rot, flip, winrt::make_agile(D.doc), ++g_rqSeq });
    g_rqCv.notify_one();
}
static void RqPurgeThumbs(int docId, int first, int last) {
    std::lock_guard<std::mutex> lk(g_rqM);
    for (size_t k = 0; k < g_rq.size(); k++) {          // thumb jobs for another document - the
        if (g_rq[k].kind != RJ_THUMB) continue;         // strip switched - or for pages the scroll
        if (g_rq[k].docId == docId && g_rq[k].page >= first - 1 && g_rq[k].page <= last + 1) continue;
        g_rq[k] = g_rq.back(); g_rq.pop_back(); k--;    // left behind: neither will ever be looked at,
    }                                                   // so the worker must not spend renders on them
}
static DWORD WINAPI RqWorker(LPVOID) {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    for (;;) {
        RJob j;
        {
            std::unique_lock<std::mutex> lk(g_rqM);
            g_rqCv.wait(lk, [] { return g_rqQuit || !g_rq.empty(); });
            if (g_rqQuit) break;
            size_t best = 0; bool have = false;         // PAGES before thumbs: every open and tab
            unsigned long long bs = 0;                  // switch asks the strip's fifteen thumbs a
            for (size_t k = 0; k < g_rq.size(); k++)    // tick after the pages, and pure recency then
                if (g_rq[k].kind == RJ_PAGE && (!have || g_rq[k].seq > bs)) { best = k; bs = g_rq[k].seq; have = true; }
            if (!have)                                  // held the eye's page hostage behind the whole
                for (size_t k = 0; k < g_rq.size(); k++)// card shelf - seconds of white pages on every
                    if (!have || g_rq[k].seq > bs) { best = k; bs = g_rq[k].seq; have = true; }
            j = g_rq[best];
            g_rq[best] = g_rq.back(); g_rq.pop_back();
        }
        HBITMAP b = nullptr;
        try { b = RenderCore(j.doc.get(), j.page, j.w, j.h, j.rot, j.flip); } catch (...) {}
        {
            std::lock_guard<std::mutex> lk(g_rqM);
            int protp = j.rot, flipp = j.flip;
            g_rqDone.push_back(RRes{ j.docId, j.page, j.kind, j.w, j.h,
                                     j.kind == RJ_PAGE ? (protp | (flipp << 4)) : 0, flipp, b, j.seq });
        }
        if (g.view) PostMessageW(g.view, WM_APP + 2, 0, 0);   // the drain paints the result
    }
    return 0;
}
static void RqEnsureThread() {
    if (!g_rqThread) g_rqThread = CreateThread(nullptr, 0, RqWorker, nullptr, 0, nullptr);
}
static void RqShutdown() {
    HANDLE t;
    { std::lock_guard<std::mutex> lk(g_rqM); g_rqQuit = true; t = g_rqThread; g_rqThread = nullptr; }
    g_rqCv.notify_all();
    if (t) { WaitForSingleObject(t, 3000); CloseHandle(t); }
}

// The paint ask. An exact hit answers itself. A miss enqueues the worker render
// and answers nothing (the frame paints a placeholder, the drain lands pixels).
// And a wrong-SIZE entry - a zoom the worker has not caught up with - keeps
// speaking for the page: it comes back with its true shape in bw/bh and Draw
// stretches it into the new rect, so a zoom gesture never blanks the pages it
// is resizing; the fresh render rides the queue the whole time. A transform
// mismatch still drops and goes white: sideways pixels stretched into an
// upright rect read broken, a placeholder reads pending. `exact` keeps the old
// contract for callers that sample page pixels at the CURRENT scale (the text
// box's tone sampler) - they must not be handed another zoom's raster.
static HBITMAP GetBmpEx(DocState& D, int i, int w, int h, int& bw, int& bh, bool exact) {
    bw = w; bh = h;
    auto it = D.cache.find(i);
    int rot = (D.prot.count(i) ? D.prot[i] : 0) | (D.pflip.count(i) ? D.pflip[i] << 4 : 0);
    if (it != D.cache.end()) {
        if (it->second.w == w && it->second.h == h && it->second.rot == rot) { it->second.t = ++D.tick; return it->second.b; }
        if (!exact && !g_sync && it->second.rot == rot) {   // stale size, same transform:
            it->second.t = ++D.tick;                        // the eye's page stays warm while
            RqEnqueue(D, i, RJ_PAGE, w, h, rot & 3, rot >> 4);   // the right-size render flies
            RqEnsureThread();
            bw = it->second.w; bh = it->second.h;
            return it->second.b;
        }
        CacheDrop(D, i);
    }
    if (!g_sync) {                                     // live: the worker owns the render -
        RqEnqueue(D, i, RJ_PAGE, w, h, rot & 3, rot >> 4);   // this frame paints the placeholder,
        RqEnsureThread();                              // the drain lands the pixels
        return nullptr;
    }
    HBITMAP b = Render(D, i, w, h);
    if (!b) return nullptr;
    D.cache[i] = Ent{ b, w, h, rot, ++D.tick };
    D.cacheBytes += (long long)w * h * 4;
    return b;
}
static HBITMAP GetBmp(DocState& D, int i, int w, int h) {
    int bw, bh;
    return GetBmpEx(D, i, w, h, bw, bh, true);
}

// The same render as Render(), decoded as a SoftwareBitmap - the input format the
// OS OCR engine consumes for the text-selection pass. Rotated/flipped pages bake
// their transform the same way, so a transformed page's OCR cells land in its
// new coordinate space and selection keeps working there.
static winrt::Windows::Graphics::Imaging::SoftwareBitmap RenderSoft(DocState& D, int i, int w, int h) {
    try {
        std::lock_guard<std::mutex> lk(g_renderM);   // the same gate: this render runs on the
        int rot = D.prot.count(i) ? D.prot[i] : 0;   // UI thread beside the worker's own
        int flip = D.pflip.count(i) ? D.pflip[i] : 0;
        int rw = (rot % 2 == 0) ? w : h, rh = (rot % 2 == 0) ? h : w;
        auto page = D.doc.GetPage((uint32_t)i);
        PdfPageRenderOptions o;
        o.DestinationWidth((uint32_t)max(1, rw));
        o.DestinationHeight((uint32_t)max(1, rh));
        o.BackgroundColor(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
        o.BitmapEncoderId(winrt::Windows::Graphics::Imaging::BitmapEncoder::BmpEncoderId());
        InMemoryRandomAccessStream st;
        page.RenderToStreamAsync(st, o).get();
        st.Seek(0);
        winrt::com_ptr<IStream> is;
        winrt::check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(st), winrt::guid_of<IStream>(), is.put_void()));
        Gdiplus::Bitmap bmp(is.get());
        if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;
        if (rot == 0 && flip == 0) {
            auto dec = winrt::Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(st).get();
            return dec.GetSoftwareBitmapAsync().get();
        }
        Gdiplus::Bitmap* out = bmp.Clone(0, 0, bmp.GetWidth(), bmp.GetHeight(), PixelFormat32bppARGB);
        if (!out || out->GetLastStatus() != Gdiplus::Ok) { delete out; return nullptr; }
        if (flip & 1) out->RotateFlip(Gdiplus::RotateNoneFlipX);
        if (flip & 2) out->RotateFlip(Gdiplus::RotateNoneFlipY);
        if ((rot & 3) == 1) out->RotateFlip(Gdiplus::Rotate90FlipNone);
        else if ((rot & 3) == 2) out->RotateFlip(Gdiplus::Rotate180FlipNone);
        else if ((rot & 3) == 3) out->RotateFlip(Gdiplus::Rotate270FlipNone);
        // re-encode the rotated raster so the OCR path reads the same pixels
        CLSID bmpCls{}; UINT cnt = 0, bytes = 0;
        Gdiplus::GetImageEncodersSize(&cnt, &bytes);
        std::vector<BYTE> buf(bytes);
        auto* enc = (Gdiplus::ImageCodecInfo*)buf.data();
        Gdiplus::GetImageEncoders(cnt, bytes, enc);
        for (UINT e = 0; e < cnt; e++) if (!lstrcmpW(enc[e].MimeType, L"image/bmp")) bmpCls = enc[e].Clsid;
        InMemoryRandomAccessStream st2;
        winrt::com_ptr<IStream> os;
        winrt::check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(st2), winrt::guid_of<IStream>(), os.put_void()));
        out->Save(os.get(), &bmpCls, nullptr);
        delete out;
        LARGE_INTEGER z0{}; z0.QuadPart = 0;
        os->Seek(z0, STREAM_SEEK_SET, nullptr);
        auto dec2 = winrt::Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(st2).get();
        return dec2.GetSoftwareBitmapAsync().get();
    } catch (...) { return nullptr; }
}

static const long long PAGE_CACHE_BUDGET = 160LL << 20;   // per document: hundreds of
                                          // light pages survive; giant zoomed pages cap it
static const long long TAB_CACHE_BUDGET = 640LL << 20;    // across a pane's whole tab strip
static void Evict(DocState& D, int lo, int hi) {
    while ((D.cacheBytes > PAGE_CACHE_BUDGET && D.cache.size() > 24) || D.cache.size() > 4096) {
        int k = -1; unsigned long long best = ~0ULL;
        for (auto& kv : D.cache)
            if ((kv.first < lo - 1 || kv.first > hi + 1) && kv.second.t < best) { best = kv.second.t; k = kv.first; }
        if (k < 0) break;
        D.cacheBytes -= (long long)D.cache[k].w * D.cache[k].h * 4;
        DeleteObject(D.cache[k].b); D.cache.erase(k);
    }
}

static int PageIn(const std::vector<int>& top, int n, int y) {
    if (n <= 0 || top.empty()) return 0;
    if (n > (int)top.size() - 1) n = (int)top.size() - 1;   // a stale layout (the focused document
    if (n <= 0) return 0;                                   // switched under it) must not walk off
    auto it = std::upper_bound(top.begin(), top.begin() + n, y);   // the array it still describes
    return std::clamp((int)(it - top.begin()) - 1, 0, n - 1);
}

// ---- view modes (KillerPDF's F5-F8) over a row-based layout ----
// Continuous keeps the classic single column; Single lays out ONE page (the
// current one) and flips pages at the wheel's edges; TwoPage pairs pages into
// spreads ((0,1), (2,3) ... - KillerPDF's default pairing, book mode is its
// opt-in setting); Grid flows 3 columns (KillerPDF's default _gridColumns).
// top[] becomes the ROW tops, px[] each page's x inside its row, rh[] the row
// heights - the single column is just the 1-page-per-row case.
static int PerRow(const DocState& D) { return D.vm == 2 ? 2 : D.vm == 3 ? 3 : 1; }
static int RowCount(const DocState& D) {
    if (!D.n) return 0;
    if (D.vm == 1) return 1;                          // single: one row holding the current page
    return (D.n + PerRow(D) - 1) / PerRow(D);
}
static int RowPage(const DocState& D, int r, int k) { return D.vm == 1 ? D.cur : r * PerRow(D) + k; }
static int RowAt(const DocState& D, int y) { return PageIn(D.top, RowCount(D), y); }
static int PageTop(const DocState& D, int i) {       // page's row top (pixel)
    if (!D.n || D.top.empty()) return 0;
    int r = std::clamp(i / PerRow(D), 0, (int)D.top.size() - 2);
    return D.top[r];
}
static int PageAt(const DocState& D, int y) {        // primary page at a viewport y
    if (!D.n) return 0;
    int r = RowAt(D, y);
    return D.vm == 1 ? D.cur : min(D.n - 1, r * PerRow(D));
}

// ---------------- text selection core (verbatim-portable geometry) ----------------
// Everything in this block is pure std:: - the same functions are compiled by
// scripts/text_check.cpp on Linux against synthetic pages.

// Group cells into visual lines: a cell joins a line when its vertical band overlaps
// the line's band by at least half the smaller height, and bands grow as members join.
// Then KillerPDF's #185 column-aware reading order: each band is split at gutter-sized
// gaps into segments; narrow segments cluster into columns by X-interval overlap and
// whole columns emit left-to-right (top-to-bottom inside each), while wide segments -
// titles and footers spanning the text width - flush the open column first. Windows.Data.Pdf
// exposes no writing directions, so the original's vertical-text ordering and RTL mirroring
// have nothing to key on and are not ported; lines read left-to-right.
static void TtLines(TxtPage& pt) {
    if (pt.cells.empty()) return;
    struct Band { std::vector<int> idx; float top, bottom; };
    std::vector<Band> bands;
    for (int i = 0; i < (int)pt.cells.size(); i++) {
        const TxtCell& c = pt.cells[i];
        int found = -1;
        for (int k = 0; k < (int)bands.size() && found < 0; k++) {
            float ov = min(bands[k].bottom, c.b) - max(bands[k].top, c.t);
            float mh = min(bands[k].bottom - bands[k].top, c.b - c.t);
            if (mh > 0 && ov >= mh * 0.5f) found = k;
        }
        if (found < 0) bands.push_back({ {i}, c.t, c.b });
        else {
            bands[found].idx.push_back(i);
            bands[found].top = min(bands[found].top, c.t);
            bands[found].bottom = max(bands[found].bottom, c.b);
        }
    }
    std::sort(bands.begin(), bands.end(), [](const Band& a, const Band& b) { return a.top < b.top; });

    float textL = FLT_MAX, textR = -FLT_MAX;
    for (const TxtCell& c : pt.cells) { textL = min(textL, c.l); textR = max(textR, c.r); }
    float wideW = (textR - textL) * 0.62f;   // spans most of the text width = not a column line

    std::vector<Band> ordered;
    struct Seg { std::vector<int> idx; float top, bottom, l, r; };
    std::vector<Seg> pending;
    auto flush = [&]() {
        if (pending.empty()) return;
        struct Col { float l, r; std::vector<int> idx; };
        std::vector<Col> cols;
        std::vector<int> byLeft(pending.size());
        for (int i = 0; i < (int)pending.size(); i++) byLeft[i] = i;
        std::sort(byLeft.begin(), byLeft.end(), [&](int a, int b) { return pending[a].l < pending[b].l; });
        for (int i : byLeft) {
            int hit = -1;
            for (int c = 0; c < (int)cols.size() && hit < 0; c++) {
                float ov = min(cols[c].r, pending[i].r) - max(cols[c].l, pending[i].l);
                float mw = min(cols[c].r - cols[c].l, pending[i].r - pending[i].l);
                if (mw > 0 && ov >= mw * 0.5f) hit = c;
            }
            if (hit < 0) cols.push_back({ pending[i].l, pending[i].r, {i} });
            else {
                cols[hit].idx.push_back(i);
                cols[hit].l = min(cols[hit].l, pending[i].l);
                cols[hit].r = max(cols[hit].r, pending[i].r);
            }
        }
        std::sort(cols.begin(), cols.end(), [](const Col& a, const Col& b) { return a.l < b.l; });
        for (Col& col : cols) {
            std::sort(col.idx.begin(), col.idx.end(), [&](int a, int b) { return pending[a].top < pending[b].top; });
            for (int i : col.idx) ordered.push_back({ pending[i].idx, pending[i].top, pending[i].bottom });
        }
        pending.clear();
    };
    for (const Band& band : bands) {
        std::vector<int> sorted = band.idx;
        std::sort(sorted.begin(), sorted.end(), [&](int a, int b) { return pt.cells[a].l < pt.cells[b].l; });
        // split threshold: well past a word space (~0.25em) but below a column gutter
        float tw = 0; int tn = 0;
        for (int i : sorted) { tw += pt.cells[i].r - pt.cells[i].l; tn += max(1, (int)pt.cells[i].text.size()); }
        float gapT = max(10.0f, (tn > 0 ? tw / tn : 5.0f) * 3);
        std::vector<Seg> segs;
        segs.push_back({ {sorted[0]}, 0, 0, 0, 0 });
        for (size_t k = 1; k < sorted.size(); k++) {
            if (pt.cells[sorted[k]].l - pt.cells[sorted[k - 1]].r > gapT) segs.push_back({ {}, 0, 0, 0, 0 });
            segs.back().idx.push_back(sorted[k]);
        }
        for (Seg& seg : segs) {
            float sT = FLT_MAX, sB = -FLT_MAX, sL = FLT_MAX, sR = -FLT_MAX;
            for (int i : seg.idx) {
                const TxtCell& c = pt.cells[i];
                sT = min(sT, c.t); sB = max(sB, c.b); sL = min(sL, c.l); sR = max(sR, c.r);
            }
            seg.top = sT; seg.bottom = sB; seg.l = sL; seg.r = sR;
            if (sR - sL >= wideW) { flush(); ordered.push_back({ seg.idx, sT, sB }); }
            else pending.push_back(seg);
        }
    }
    flush();

    // flatten in reading order; cells inside a line read left-to-right, word ordinals
    // restart where a space sits on either side of a join or the gap is a real word
    // space (a narrow gap with no spaces is one word split across range boxes)
    std::vector<TxtCell> flat; flat.reserve(pt.cells.size());
    int lineNo = 0, wordNo = 0;
    for (const Band& band : ordered) {
        std::vector<int> mem = band.idx;
        std::sort(mem.begin(), mem.end(), [&](int a, int b) { return pt.cells[a].l < pt.cells[b].l; });
        float bandH = max(1.0f, band.bottom - band.top);
        TxtLine ln; ln.start = (int)flat.size();
        float lt = FLT_MAX, lb = -FLT_MAX, ll = FLT_MAX, lr = -FLT_MAX;
        for (int i : mem) {
            TxtCell f = pt.cells[i];
            if (ln.count == 0) f.word = wordNo++;                    // a new line starts a word
            else {
                const TxtCell& p = flat.back();
                bool sp = (!p.text.empty() && p.text.back() == L' ') || (!f.text.empty() && f.text.front() == L' ');
                if (!sp && f.l - p.r <= bandH * 0.3f) f.word = p.word;
                else f.word = wordNo++;
            }
            f.line = lineNo;
            lt = min(lt, f.t); lb = max(lb, f.b); ll = min(ll, f.l); lr = max(lr, f.r);
            flat.push_back(std::move(f));
            ln.count++;
        }
        ln.top = lt; ln.bottom = lb; ln.left = ll; ln.right = lr;
        pt.lines.push_back(ln);
        lineNo++;
    }
    pt.cells = std::move(flat);
}

// KillerPDF's cells are the engine's LETTERS; ours start as OCR word boxes. Slice
// every word box into one cell per character - a uniform advance across the box,
// which is what hit-testing needs since the caret only compares mid-points - so
// drags move character by character instead of snapping whole words, exactly at
// KillerPDF's granularity. Every character keeps the word's ordinal, so the space
// between words, the double-click word range and the copied text are unchanged.
static void TtSplitChars(TxtPage& pt) {
    if (pt.cells.empty() || pt.lines.empty()) return;
    std::vector<TxtCell> out; out.reserve(pt.cells.size() * 4);
    std::vector<TxtLine> lines; lines.reserve(pt.lines.size());
    int lineStart = 0;
    for (const TxtLine& ln : pt.lines) {
        for (int i = ln.start; i < ln.end(); i++) {
            const TxtCell& w = pt.cells[i];
            int n = max(1, (int)w.text.size());
            float wl = (w.r - w.l) / n;
            for (int j = 0; j < n; j++) {
                TxtCell c;
                c.text.assign(1, w.text[j]);
                c.l = w.l + wl * j; c.r = w.l + wl * (j + 1);
                c.t = w.t; c.b = w.b;
                c.word = w.word; c.line = (int)lines.size();
                out.push_back(std::move(c));
            }
        }
        TxtLine nl = ln;                       // the band is the word band: contiguous
        nl.start = lineStart;                  // character tiles span the same extent
        nl.count = (int)out.size() - lineStart;
        lineStart = (int)out.size();
        lines.push_back(nl);
    }
    pt.cells = std::move(out);
    pt.lines = std::move(lines);
}

// True when the point sits ON text: inside a line's vertical band and within its
// horizontal extent (small slop). This is the gate that decides flowing selection
// vs doing nothing - a press on text selects, a press anywhere else is dead.
// The band carries KillerPDF's own 12% line-height pad (SelectionBounds grows
// every quad by exactly that), because the OCR word boxes hug the glyphs: a
// press between two lines' boxes or on a tall letter's headroom is ON that text
// for the user, and the highlight must arm there - not hint "no text".
static bool TtOverText(const TxtPage& pt, float x, float y) {
    const float slop = 2.0f;   // DIPs
    for (const TxtLine& ln : pt.lines) {
        float pad = (ln.bottom - ln.top) * 0.12f;
        if (y <= ln.bottom + pad + slop && y >= ln.top - pad - slop &&
            x >= ln.left - slop && x <= ln.right + slop)
            return true;
    }
    return false;
}

// Caret (0..cells) nearest a point, browser-style clamping: above the first line
// selects from the page start, below the last to the page end, between lines snaps
// to the closer line, beyond a line's ends clamps to its ends. (KillerPDF's
// CaretFromPoint at cell granularity; y grows downward in DIP space.)
static int TtCaret(const TxtPage& pt, float x, float y) {
    if (pt.lines.empty()) return 0;
    const TxtLine* target = nullptr;
    float bestV = FLT_MAX, bestH = FLT_MAX;
    for (const TxtLine& ln : pt.lines) {
        float v = y < ln.top ? ln.top - y : y > ln.bottom ? y - ln.bottom : 0;
        float h = x < ln.left ? ln.left - x : x > ln.right ? x - ln.right : 0;
        if (v < bestV || (v == bestV && h < bestH)) { bestV = v; bestH = h; target = &ln; }
    }
    if (!target) return 0;
    const TxtLine& first = pt.lines.front();
    const TxtLine& last = pt.lines.back();
    if (y < first.top && target == &first && x < first.left) return 0;
    if (y > last.bottom && target == &last && x > last.right) return (int)pt.cells.size();
    if (x <= target->left) return target->start;
    if (x >= target->right) return target->end();
    for (int i = target->start; i < target->end(); i++) {
        const TxtCell& c = pt.cells[i];
        if (x < (c.l + c.r) * 0.5f) return i;
    }
    return target->end();
}

// Caret range of the word (cell) under a point, or false when the point is outside
// selectable text - the double-click word selection.
static bool TtWordRange(const TxtPage& pt, float x, float y, int& s, int& e) {
    if (!TtOverText(pt, x, y)) return false;
    int best = -1; float bestD = FLT_MAX;
    for (int i = 0; i < (int)pt.cells.size(); i++) {
        const TxtCell& c = pt.cells[i];
        const TxtLine& ln = pt.lines[c.line];
        if (y < ln.top - 2 || y > ln.bottom + 2 || x < ln.left - 2 || x > ln.right + 2) continue;
        float d = x < c.l ? c.l - x : x > c.r ? x - c.r : 0;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return false;
    int word = pt.cells[best].word;
    s = best; e = best + 1;
    while (s > 0 && pt.cells[s - 1].word == word) s--;
    while (e < (int)pt.cells.size() && pt.cells[e].word == word) e++;
    return true;
}

// Per-line selection rects covering carets [start, end): one rect per line, first
// selected cell to last, padded 12% of the line height - KillerPDF's SelectionBounds.
static void TtRects(const TxtPage& pt, int start, int end, std::vector<FRect>& out) {
    int i = max(0, start);
    end = min(end, (int)pt.cells.size());
    while (i < end) {
        const TxtLine& ln = pt.lines[pt.cells[i].line];
        int segEnd = min(end, ln.end());
        float left = FLT_MAX, right = -FLT_MAX;
        for (int k = i; k < segEnd; k++) { left = min(left, pt.cells[k].l); right = max(right, pt.cells[k].r); }
        float pad = (ln.bottom - ln.top) * 0.12f;
        out.push_back({ left, ln.top - pad, right - left, (ln.bottom + pad) - (ln.top - pad) });
        i = segEnd;
    }
}

// Text for the caret range [start, end): newlines between lines, single spaces
// between words; a cell's own text (which may contain spaces) is appended verbatim.
// Also reports how many distinct words the range touches.
static std::wstring TtText(const TxtPage& pt, int start, int end, int* words) {
    if (words) *words = 0;
    std::wstring out;
    int lastWord = -1, lastLine = -1;
    for (int i = max(0, start); i < min(end, (int)pt.cells.size()); i++) {
        const TxtCell& c = pt.cells[i];
        if (lastLine >= 0 && c.line != lastLine) out += L'\n';
        else if (lastWord >= 0 && c.word != lastWord) out += L' ';
        if (c.word != lastWord && words) (*words)++;
        out += c.text;
        lastWord = c.word; lastLine = c.line;
    }
    return out;
}

// ---------------- Find Text core (pure std:: - scripts/find_check.cpp) ----------------

// KillerPDF's PdfSearchText.Fold, reduced to what our OCR cells need: 1:1
// lower-case folding (a length-changing fold would break the char->cell map).
static std::wstring FdLower(const std::wstring& s) {
    std::wstring t(s);
    for (wchar_t& c : t) c = (wchar_t)towlower(c);
    return t;
}

// SearchService over the cell stream: the page flattens to a string (cells of
// one word join with no separator, a word gap or a line break one space), every
// occurrence is folded to lower case unless Match Case, Whole Word rejects
// letter/digit/underscore neighbours (KillerPDF's \p{L}\p{Nd}_ lookarounds), and
// each match lands as ONE RECT PER TOUCHED WORD - the union of that word's
// matched cell boxes, FindExactMatchesOnPage's word split. Occurrences scan
// forward without overlap, like the engine's. Returns the match count; the
// per-word rects come back through out.
static int FdMatch(const TxtPage& pt, const std::wstring& query, bool matchCase,
                   bool wholeWord, std::vector<FRect>& out) {
    out.clear();
    if (pt.cells.empty() || query.empty()) return 0;
    std::wstring text; text.reserve(pt.cells.size() * 3);
    std::vector<int> cellOf; cellOf.reserve(pt.cells.size() * 3);
    int lastWord = -1, lastLine = -1;
    for (int i = 0; i < (int)pt.cells.size(); i++) {
        const TxtCell& c = pt.cells[i];
        if (lastLine >= 0 && c.line != lastLine) { text += L' '; cellOf.push_back(-1); }
        else if (lastWord >= 0 && c.word != lastWord) { text += L' '; cellOf.push_back(-1); }
        for (wchar_t ch : c.text) { text += ch; cellOf.push_back(i); }
        lastWord = c.word; lastLine = c.line;
    }
    std::wstring hay = matchCase ? text : FdLower(text);
    std::wstring needle = matchCase ? query : FdLower(query);
    int n = (int)needle.length(), N = (int)hay.length();
    if (n == 0 || n > N) return 0;
    auto wordy = [](wchar_t c) { return iswalnum(c) || c == L'_'; };
    int matches = 0;
    for (int p = 0; p + n <= N; p++) {
        if (hay.compare(p, n, needle) != 0) continue;
        if (wholeWord && ((p > 0 && wordy(hay[p - 1])) || (p + n < N && wordy(hay[p + n])))) continue;
        int s = p, e = p + n;
        while (s < e && cellOf[s] < 0) s++;          // a query never starts or ends on a
        while (e > s && cellOf[e - 1] < 0) e--;      // separator; trim the join spaces
        if (s >= e) continue;
        int i = s;
        while (i < e) {                              // one rect per touched word
            if (cellOf[i] < 0) { i++; continue; }    // the join space between words
            int w = pt.cells[cellOf[i]].word;
            float l = FLT_MAX, r = -FLT_MAX, t = FLT_MAX, b = -FLT_MAX;
            while (i < e && pt.cells[cellOf[i]].word == w) {
                const TxtCell& c = pt.cells[cellOf[i]];
                l = min(l, c.l); r = max(r, c.r); t = min(t, c.t); b = max(b, c.b);
                i++;
            }
            out.push_back({ l, t, r - l, b - t });
        }
        matches++;
        p += n - 1;
    }
    return matches;
}

// ---------------- text selection app glue ----------------

// Per-page text geometry, built lazily on first touch (cursor, press or a selection
// slice) and LRU-capped at 48 pages, so a 2,000-page book never pre-extracts.
static TxtPage* TtFor(DocState& D, int i) {
    if (!D.n || i < 0 || i >= D.n) return nullptr;
    auto it = D.txt.find(i);
    if (it != D.txt.end()) { it->second.tick = ++D.tick; return &it->second; }
    TxtPage pt; pt.w = D.pw[i]; pt.h = D.ph[i];
    try {
        // Windows.Data.Pdf exposes no text layer, so the selectable cells come from the
        // built-in OCR pass (Windows.Media.Ocr) over a fresh 2x render: one box per
        // recognized word, scaled back to page DIPs, feeding KillerPDF's exact cell
        // pipeline below. Built lazily on first touch and LRU-capped, so a 2,000-page
        // book never OCRs what you never touch; no OCR language pack means selection
        // is quietly off and the page keeps its pan gesture.
        static winrt::Windows::Media::Ocr::OcrEngine s_ocr = nullptr;
        static bool s_tried = false;
        if (!s_tried) {
            s_tried = true;
            s_ocr = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
        }
        if (s_ocr) {
            int ow = std::clamp((int)std::lround(D.pw[i] * 2), 800, 1600);
            int oh = max(1, (int)std::lround(ow * D.ph[i] / max(1.f, D.pw[i])));
            auto soft = RenderSoft(D, i, ow, oh);
            if (soft) {
                auto res = s_ocr.RecognizeAsync(soft).get();
                float kx = pt.w / ow, ky = pt.h / oh;
                for (winrt::Windows::Media::Ocr::OcrLine line : res.Lines())
                    for (winrt::Windows::Media::Ocr::OcrWord wd : line.Words()) {
                        auto r = wd.BoundingRect();
                        if (r.Width <= 0 || r.Height <= 0 || wd.Text().empty()) continue;
                        TxtCell c;
                        c.text = std::wstring(wd.Text());
                        c.l = r.X * kx; c.t = r.Y * ky;
                        c.r = c.l + r.Width * kx; c.b = c.t + r.Height * ky;
                        pt.cells.push_back(std::move(c));
                    }
            }
        }
    } catch (...) { /* no OCR pack / render failure: selection is just not offered here */ }
    TtLines(pt);
    TtSplitChars(pt);   // word boxes -> KillerPDF's letter-granular cells
    pt.tick = ++D.tick;
    D.txt[i] = std::move(pt);
    while (D.txt.size() > 256) {            // LRU: drop the stalest page's geometry (a
        auto oldest = D.txt.end();          // 2,000-cell page is ~100KB, so 256 pages ride
        for (auto x = D.txt.begin(); x != D.txt.end(); ++x)   // comfortably under 30MB)
            if (x->first != i && (oldest == D.txt.end() || x->second.tick < oldest->second.tick)) oldest = x;
        if (oldest == D.txt.end()) break;
        D.txt.erase(oldest);
    }
    return &D.txt[i];
}

// Peek at already-built text geometry without triggering the OCR build - the
// cursor only advertises text that is ready, and the build itself happens off
// the click path (the pre-warm timer, or the first press as a last resort).
static TxtPage* TtPeek(DocState& D, int i) {
    auto it = D.txt.find(i);
    return it != D.txt.end() ? &it->second : nullptr;
}

static void SelClear(DocState& D) {
    bool had = D.selRange || D.selArm || D.selDrag || !D.selText.empty();
    D.selArm = D.selDrag = D.selRange = false;
    D.selAPage = D.selFPage = -1;
    D.selACaret = D.selFCaret = 0;
    D.selText.clear();
    g.selCommit = 0;                       // the flowing highlight's tool dies with the
    if (had && g.view) InvalidateRect(g.view, nullptr, FALSE);   // selection it rode
}

// The anchor->focus pair in reading order.
static void SelOrdered(const DocState& D, int& sp, int& sc, int& ep, int& ec) {
    bool aFirst = D.selAPage < D.selFPage || (D.selAPage == D.selFPage && D.selACaret <= D.selFCaret);
    if (aFirst) { sp = D.selAPage; sc = D.selACaret; ep = D.selFPage; ec = D.selFCaret; }
    else { sp = D.selFPage; sc = D.selFCaret; ep = D.selAPage; ec = D.selACaret; }
}

// The caret slice of the selection that falls on one page, or (0,0) when none.
static void SelSliceForPage(DocState& D, int page, int& start, int& end) {
    start = end = 0;
    int sp, sc, ep, ec;
    SelOrdered(D, sp, sc, ep, ec);
    if (page < sp || page > ep) return;
    TxtPage* pt = TtFor(D, page);
    int n = pt ? (int)pt->cells.size() : 0;
    start = page == sp ? sc : 0;
    end = page == ep ? ec : n;
    start = std::clamp(start, 0, n);
    end = std::clamp(end, 0, n);
    if (start > end) std::swap(start, end);
}

// Cross-page text: one page's slice per line group, pages joined with '\n'
// (KillerPDF's BuildSelectedText).
static std::wstring SelBuildText(DocState& D, int* words) {
    if (words) *words = 0;
    int sp, sc, ep, ec;
    SelOrdered(D, sp, sc, ep, ec);
    std::wstring out;
    for (int p = sp; p <= ep; p++) {
        TxtPage* pt = TtFor(D, p);
        if (!pt || pt->cells.empty()) continue;
        int s, e;
        SelSliceForPage(D, p, s, e);
        if (s >= e) continue;
        int w = 0;
        std::wstring t = TtText(*pt, s, e, &w);
        if (t.empty()) continue;
        if (!out.empty()) out += L'\n';
        out += t;
        if (words) *words += w;
    }
    return out;
}

static void CopyClipboard(const std::wstring& s) {
    if (!OpenClipboard(g.main)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        void* m = GlobalLock(hg);
        if (m) {
            memcpy(m, s.c_str(), bytes);
            GlobalUnlock(hg);
            if (!SetClipboardData(CF_UNICODETEXT, hg)) GlobalFree(hg);
        } else GlobalFree(hg);
    }
    CloseClipboard();
}

// Ctrl+A: flowing select-all on the current page and copy (KillerPDF's SelectAllText).
static void SelAll(DocState& D) {
    if (!D.n) return;
    int pg = PageAt(D, D.sy + g.ch / 3);
    TxtPage* pt = TtFor(D, pg);
    if (!pt || pt->cells.empty()) return;
    SelClear(D);
    D.selAPage = D.selFPage = pg;
    D.selACaret = 0;
    D.selFCaret = (int)pt->cells.size();
    D.selRange = true;
    D.selText = TtText(*pt, 0, (int)pt->cells.size(), nullptr);
    if (!D.selText.empty()) CopyClipboard(D.selText);
    InvalidateRect(g.view, nullptr, FALSE);
}

// The view->page transform, mirroring Draw()'s page placement exactly: which page a
// view point falls on and where it sits inside that page, in DIPs. The point is
// clamped into the page, so dragging past an edge parks the caret at the start or
// end of the nearest line instead of losing the selection (KillerPDF clamps the
// drag point into the page canvas the same way). Multi-column rows resolve the
// page by x (a point in the gap clamps to the nearest page, so a drag across a
// spread flows through both pages like KillerPDF's Continuous does).
// Pane-aware core: the caller names the scroll state the point lives in (pane
// B passes its own pair). Everything else is the classic mapping.
static bool ViewToPageAt(DocState& D, POINT v, int w, int h, int sx, int sy, int& page, float& dx, float& dy) {
    if (!D.n) return false;
    int gut = D.docH > h ? SBar() : 0;
    int offx = D.docW < w - gut ? (w - gut - D.docW) / 2 : -sx;
    int r = RowAt(D, v.y + sy);
    if (r < 0 || r >= RowCount(D)) return false;
    int cnt = D.vm == 1 ? 1 : min(PerRow(D), D.n - r * PerRow(D));
    int best = -1; float bestD = FLT_MAX;
    for (int k = 0; k < cnt; k++) {
        int i = RowPage(D, r, k);
        int x0 = offx + D.px[i], x1 = x0 + PW(D, i);
        float d = v.x < x0 ? (float)(x0 - v.x) : v.x > x1 ? (float)(v.x - x1) : 0;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return false;
    float k = (float)(D.zoom * g.dpi);
    if (k <= 0) return false;
    page = best;
    int py = D.top[r] - sy + (D.rh[r] - PH(D, best)) / 2;
    dx = (v.x - (offx + D.px[best])) / k;
    dy = (v.y - py) / k;
    dx = std::clamp(dx, 0.0f, (float)PW(D, best) / k);
    dy = std::clamp(dy, 0.0f, (float)PH(D, best) / k);
    return true;
}

static bool ViewToPage(DocState& D, POINT v, int w, int h, int& page, float& dx, float& dy) {
    return ViewToPageAt(D, v, w, h, D.sx, D.sy, page, dx, dy);
}

static void SideSync();
static void SideLayout();
static void LayoutMain();
static void StatusRepaint();

static void UpdateTitle() {   // the sidebar highlight follows the focused pane's
    DocState& D = DVF();                             // page under its upper third. The
    int c = D.n ? PageAt(D, FocusSy() + g.ch / 3) : -1;   // window text stays "Docsy" - the
    if (c != D.cur) {                             // open file's name shows nowhere.
        D.cur = c;
        if (g.view) SetTimer(g.view, 4, 160, nullptr);   // pre-warm the page's text once
        SideSync();                           // the scroll settles (never mid-gesture)
        StatusRepaint();                      // "Page N of M" moved
    }
}

static void UpdateScroll(DocState& D, bool b) {   // one pane's scroll clamps into ITS
    g.busy = true;                                // document's layout at ITS viewport: the
    D.sy = std::clamp(D.sy, 0, max(0, D.docH - ViewH(D, b)));   // reserved bar rows are part
    D.sx = std::clamp(D.sx, 0, max(0, D.docW - ViewW(D, b)));   // of the viewport now, so the
    g.busy = false;                               // content stops AT the bar, never under it
    D.scrollTick = GetTickCount64();
}

static void Layout(DocState& D, bool b) {   // one pane's document at the pane's own
    D.gap = Gap();                          // metrics: two panes hold two documents, so
    int per = PerRow(D);                    // two layouts - each reflows at its own card
    int rows = RowCount(D);                 // width and clamps at its own scroll range
    D.top.assign((size_t)rows + 1, 0);
    D.px.assign((size_t)max(D.n, 1), 0);
    D.rh.assign((size_t)max(rows, 1), 0);
    int y = D.gap + BandInset(b); D.maxw = 0;   // the tab band is a layout row: KillerPDF's card
                                                // starts below its strip, so the pages do too -
                                                // the covered strip scrolls under the opaque band,
                                                // which is the same clip the ScrollViewer row draws
    for (int r = 0; r < rows; r++) {                 // pass 1: row heights and the widest row
        int i0 = D.vm == 1 ? D.cur : r * per;
        int cnt = D.vm == 1 ? 1 : min(per, D.n - r * per);
        int wsum = 0, hmax = 0;
        for (int k = 0; k < cnt; k++) { wsum += PW(D, i0 + k); hmax = max(hmax, PH(D, i0 + k)); }
        D.maxw = max(D.maxw, wsum + (cnt - 1) * D.gap);
        D.top[r] = y; D.rh[r] = hmax;
        y += hmax + D.gap;
    }
    D.top[rows] = y;
    D.docH = rows ? y : 0;
    D.docW = rows ? D.maxw + 2 * D.gap : 0;
    for (int r = 0; r < rows; r++) {                 // pass 2: each row centred on the widest one
        int i0 = D.vm == 1 ? D.cur : r * per;
        int cnt = D.vm == 1 ? 1 : min(per, D.n - r * per);
        int wsum = 0;
        for (int k = 0; k < cnt; k++) wsum += PW(D, i0 + k);
        int x = D.gap + (D.maxw - (wsum + (cnt - 1) * D.gap)) / 2;
        for (int k = 0; k < cnt; k++) { D.px[i0 + k] = x; x += PW(D, i0 + k) + D.gap; }
    }
    UpdateScroll(D, b);
    InvalidateRect(g.view, nullptr, FALSE);
    StatusRepaint();
    UpdateTitle();
}

static void SetZoomAt(double z, int ax, int ay, bool b) {
    DocState& D = DV(b);                          // the anchor names its pane: pane B zooms
    CommitTextEdit();                             // a zoom lands the open text box
    int& sxr = D.sx; int& syr = D.sy;             // around its own viewport point, in its
    z = std::clamp(z, 0.1, 4.0);                  // own document's layout
    double ry = D.docH ? (double)(syr + ay) / D.docH : 0, rx = D.docW ? (double)(sxr + ax) / D.docW : 0;
    D.zoom = z; D.fitMode = 2;              // an explicit zoom leaves the fit modes - and keeps
                                            // the rasters: stale-size entries still paint
                                            // stretched while the worker catches up (Evict
                                            // keeps their weight inside the byte budget)
    Layout(D, b);
    syr = (int)(ry * D.docH) - ay;
    if (D.docW > CardW(b)) sxr = (int)(rx * D.docW) - ax;
    UpdateScroll(D, b);
    InvalidateRect(g.view, nullptr, FALSE);
    UpdateTitle();
    StatusRepaint();
}

static void SetZoom(double z, int ax, int ay) { SetZoomAt(z, ax, ay, FocusB()); }

// KillerPDF's FitToWidth / FitToPage, mode-aware: the fit target is the widest
// ROW (a spread in TwoPage, 3 columns in Grid), and Page additionally height-
// limits the current row. The zoom% the status line and the combo show derives
// from the resulting scale.
static void ApplyFit(bool pageFit, bool b) {   // one pane's fit, at that pane's own
    DocState& D = DV(b);                       // card width - two panes fit independently
    if (!D.n || g.cw <= 0) return;
    int W = CardW(b) - SBar();   // the card's inner width minus the reserved overlay-bar column
    int per = PerRow(D);
    int c = PageAt(D, D.sy + g.ch / 3);
    int r = D.vm == 1 ? 0 : c / per;
    double z;
    if (D.vm == 2) {                         // the current spread's two pages share the row
        int a = c - c % 2, bb = min(D.n - 1, a + 1);
        z = (W - 3.0 * Gap()) / (max(1.f, D.pw[a] + D.pw[bb]) * g.dpi);
    } else if (D.vm == 3) {
        float mw = 0; for (float w : D.pw) mw = max(mw, w);
        z = (W - 4.0 * Gap()) / (per * mw * g.dpi);
    } else {
        int i = D.vm == 1 ? max(0, D.cur) : 0;
        float mw = D.vm == 1 ? D.pw[i] : 0;
        if (D.vm == 0) for (float w : D.pw) mw = max(mw, w);
        z = (W - 2.0 * Gap()) / (max(1.f, mw) * g.dpi);
    }
    if (pageFit) {                           // the current row must also fit vertically
        int i0 = D.vm == 1 ? max(0, D.cur) : r * per;
        int cnt = D.vm == 1 ? 1 : min(per, D.n - i0);
        float rh = 0; for (int k = 0; k < cnt; k++) rh = max(rh, D.ph[i0 + k]);
        z = min(z, (g.ch - BandInset(b) - 2.0 * Gap()) / (max(1.f, rh) * g.dpi));
    }
    SetZoomAt(z, 0, 0, b);
    D.fitMode = pageFit ? 1 : 0;
    StatusRepaint();
}

// One pane's scroll: clamped into the shared layout, invalidated over THAT
// pane's pixels only, and the sidebar/status follow just the focused pane (a
// background pane may scroll without dragging the page ring along).
static void ScrollPane(bool b, int x, int y) {
    DocState& D = DV(b);                        // this pane's own document and viewport: the
    CommitTextEdit();                           // the box rides nothing: a scroll lands it
    x = std::clamp(x, 0, max(0, D.docW - ViewW(D, b)));   // clamps stop the content AT the
    y = std::clamp(y, 0, max(0, D.docH - ViewH(D, b)));   // reserved bars, KillerPDF's ScrollViewer
    if (x == D.sx && y == D.sy) return;
    D.sx = x; D.sy = y;
    D.scrollTick = GetTickCount64();
    if (g.view) {
        RECT r{ 0, 0, g.split ? g.splitX : g.cw, g.ch };   // pane A ends at the divider line,
        if (b) r = { g.splitX, 0, g.cw, g.ch };            // pane B starts at it - the gutter
        InvalidateRect(g.view, &r, FALSE);                 // straddles the line, 4px a side
    }
    if (b == FocusB()) UpdateTitle();
}

static void ScrollTo(int x, int y) { ScrollPane(FocusB(), x, y); }

// Single mode's page flip (KillerPDF's F6 view): the document IS one page, so
// navigation replaces it and parks at the top. The sidebar's current-page ring
// and the footer's "Page N of M" follow.
static void SetPage(int i) {
    DocState& D = DVF();                       // the flip acts on the focused pane's document
    if (!D.n) return;
    i = std::clamp(i, 0, D.n - 1);
    D.cur = i;
    if (D.fitMode != 2) ApplyFit(D.fitMode == 1, FocusB()); else Layout(D, FocusB());
    D.sx = 0; D.sy = 0;                        // the flip parks the focused pane at the top
    UpdateScroll(D, FocusB());
    if (g.view) SetTimer(g.view, 4, 160, nullptr);   // pre-warm the new page's text
    SideSync();
    StatusRepaint();
}

// Click/wheel focus (KillerPDF's FocusPaneFromPointer): the pane under the
// pointer becomes the one every window-level surface serves - the sidebar's
// ring retargets to its document, the footer's page/zoom reads it, the find
// bar anchors over its card. One switch, one repaint; no per-surface plumbing.
static void FocusPane(bool b) {
    if (!g.split || b == FocusB()) return;
    g.splitA = !b;
    for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);   // the sidebar's thumbs
    sd.th.clear(); sd.thBytes = 0; sd.scroll = 0; sd.cur = -1; sd.hold = 0;              // belong to the focused
                                                    // pane's document now - and cw/ch stay:
                                                    // zeroing them early-returned the SideLayout
                                                    // below and dead-pumped the strip
    SideLayout();
    UpdateTitle();
    SideSync();
    StatusRepaint();
    RailRepaint();                        // the Pages tab's light rides the focused pane too
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // the toolbar's selected tool is
                                                        // the focused pane's tool
    InvalidateRect(g.view, nullptr, FALSE);
}

static bool Open(DocState& D, const std::wstring& p) {   // opens into ONE pane: Ctrl+O and
    try {                                                // the toolbar act on the focused pane,
        wchar_t full[MAX_PATH * 4];                      // a drop loads the pane it landed on
        GetFullPathNameW(p.c_str(), (DWORD)_countof(full), full, nullptr);
        // the file is read into memory and the engine chews on the copy:
        // LoadFromFileAsync pins the on-disk file for the PdfDocument's whole
        // life, so every later in-place save died on a sharing violation and
        // the disk never changed - the reader's "save does nothing". A memory
        // copy holds no handle; the path stays free for Save's overwrite.
        HANDLE fh = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) throw std::runtime_error("open");
        LARGE_INTEGER fsz{};
        if (!GetFileSizeEx(fh, &fsz) || fsz.QuadPart <= 0) { CloseHandle(fh); throw std::runtime_error("size"); }
        std::vector<BYTE> bytes((size_t)fsz.QuadPart);
        DWORD got = 0, rd = 0;
        while (got < bytes.size() &&
               ReadFile(fh, bytes.data() + got, (DWORD)(bytes.size() - got), &rd, nullptr) && rd) got += rd;
        CloseHandle(fh);                                 // the disk file is nobody's hostage now
        if (got != bytes.size()) throw std::runtime_error("read");
        InMemoryRandomAccessStream st;
        DataWriter dw(st);
        dw.WriteBytes(winrt::array_view<const BYTE>(bytes.data(), bytes.data() + bytes.size()));
        dw.StoreAsync().get();
        dw.FlushAsync().get();
        dw.DetachStream();                               // the writer lets go; the doc keeps the stream
        st.Seek(0);
        winrt::Windows::Data::Pdf::PdfDocument d{ nullptr };   // the projection has no default ctor
        {   // the load queues behind the render gate too: the worker may be inside a page render
            // of ANOTHER document right now, and the engine's loader rides the same machinery
            std::lock_guard<std::mutex> lk(g_renderM);
            d = PdfDocument::LoadFromStreamAsync(st).get();
        }
        uint32_t n = d.PageCount();
        if (!n) throw std::runtime_error("empty");
        std::vector<float> w(n, 612.f), h(n, 792.f);
        for (uint32_t i = 0; i < n; i++) {
            try { auto s = d.GetPage(i).Size(); w[i] = s.Width; h[i] = s.Height; } catch (...) {}
        }
        std::wstring nm = full;
        size_t sl = nm.find_last_of(L"\\/");
        if (sl != std::wstring::npos) nm = nm.substr(sl + 1);
        ClearCache(D);
        D.doc = d; D.n = (int)n; D.pw = w; D.ph = h; D.name = nm; D.path = full;
        D.id = ++g.nextDocId;               // the render queue's stable handle for this document
        D.sx = D.sy = 0; D.zoom = 1.0; D.fitMode = 2; D.cur = -1;   // every document opens
                                                      // at 100%; -1 so LayoutMain's UpdateTitle
                                                      // sees the change and syncs the sidebar highlight
        D.txt.clear();               // the new document's text geometry starts fresh
        D.annos.clear();             // the tools' marks die with their document - KillerPDF's
                                     // annotations live in the PDF; docsy's live with it
        D.prot.clear(); D.pflip.clear();
        D.tool = TOOL_SELECT;        // the pane's tool re-lands on Select, like KillerPDF's
                                     // open path restores the toolbar
        g.annSelPage = -1; g.annSelIdx = -1;   // and the Select tool's ring has nothing to hold
        SelClear(D);
        SnapClean(D);                          // a fresh open IS the saved checkpoint
        bool mine = fd.pane == (&D == &g.dB);        // the find dies only when ITS document
        if (mine && fd.open) {                       // dies; the other pane's scan keeps going
            if (g.view) { KillTimer(g.view, 7); KillTimer(g.view, 8); }
            if (g_fedit) ShowWindow(g_fedit, SW_HIDE);
            fd = Find{};                             // the Find bar's results die with the old document
        }
        if (&D == &DVF()) {                          // the sidebar shows the FOCUSED pane's pages:
            for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);   // a drop or open
            sd.th.clear(); sd.thBytes = 0; sd.scroll = 0; sd.cur = -1; sd.hold = 0; sd.tw = 0;   // into the pane
            sd.cw = 0; sd.ch = 0;                    // you read rebuilds the ring around it
            g.sideOn = true;
            if (g.side) ShowWindow(g.side, SW_SHOWNOACTIVATE);
        }
        LayoutMain();
        if (g.view) InvalidateRect(g.view, nullptr, FALSE);   // the new document is the card now:
                                                              // WS_CLIPCHILDREN keeps the main
                                                              // window's repaint out of the view,
                                                              // and the band's arrival reflowed the
                                                              // pages - the card must not wait for a
                                                              // WM_SIZE or the render drain to show
        InvalidateRect(g.main, nullptr, FALSE);
        if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);   // the Find Text button undims (Opacity 0.4 -> 1)
        RailRepaint();                                      // Pages/Info undim, the Pages light lands
        return true;
    } catch (...) {
        if (!g.quiet) MessageBoxW(g.main, L"Could not open this file (invalid, encrypted or unsupported PDF).", L"Docsy", MB_ICONWARNING);
        return false;
    }
}

static void OpenDlg() {                          // the dialog opens into the FOCUSED pane
    wchar_t f[MAX_PATH * 2] = L"";
    OPENFILENAMEW o{}; o.lStructSize = sizeof o; o.hwndOwner = g.main;
    o.lpstrFilter = L"PDF files\0*.pdf\0All files\0*.*\0";
    o.lpstrFile = f; o.nMaxFile = (DWORD)_countof(f);
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) TabOpenInto(FocusB(), f);   // OpenInNewTab: the dialog lands in
}

// The drop queue: every dropped file waits here and opens one per posted
// message, so a folder drop of tens of PDFs loads in sequence while the window
// keeps painting between loads - no single giant freeze.
static std::vector<std::wstring> g_dropQ;
static std::vector<bool> g_dropPane;

static void HandleDrop(HDROP d, HWND h) {        // the pane under the drop takes the files:
    UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);   // the view maps the point to its
    POINT p; DragQueryPoint(d, &p);              // pane, a drop on the sidebar or the frame
    bool b = FocusB();                           // goes to the pane the reader is working in
    if (h == g.view) b = PaneB(p.x);
    for (UINT i = 0; i < n; i++) {
        wchar_t f[MAX_PATH * 2];
        if (!DragQueryFileW(d, i, f, (UINT)_countof(f))) continue;
        g_dropQ.push_back(f);
        g_dropPane.push_back(b);
    }
    DragFinish(d);
    if (!g_dropQ.empty()) PostMessageW(g.main, WM_APP + 3, 0, 0);   // one open per message
}

// ---------------- file cluster (KillerPDF's FileOperations.cs) ----------------
// Close / Open / Save, reduced to docsy's single-document model: a pane IS
// KillerPDF's tab.

// New_Click -> NewDocument: a blank one-page PDF opens temp-backed with the
// suggested name "Untitled.pdf" - the first Save asks where to keep it, which
// is SaveDoc's no-real-home -> Save As branch. The writer is the same minimal
// PDF the Flatten path emits; a blank page is its MediaBox and nothing else.
static void NewBlank() {
    static int seq = 0;
    wchar_t tmp[MAX_PATH * 2];
    GetTempPathW((DWORD)_countof(tmp), tmp);
    wchar_t file[MAX_PATH * 2];
    swprintf(file, MAX_PATH * 2, L"%sUntitled%d.pdf", tmp, ++seq);
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { SetStatusHold(L"New document failed: the file could not be written"); return; }
    std::string head = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    DWORD wr = 0;
    WriteFile(f, head.data(), (DWORD)head.size(), &wr, nullptr);
    long long pos = (long long)head.size();
    auto emit = [&](const std::string& s) { WriteFile(f, s.data(), (DWORD)s.size(), &wr, nullptr); pos += (long long)s.size(); };
    long long catOff = pos;
    emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    long long pagesOff = pos;
    emit("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    long long pageOff = pos;
    emit("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n");
    long long xref = pos;
    std::string xr = "xref\n0 4\n0000000000 65535 f \n";
    auto off10 = [](long long v) { char b[16]; snprintf(b, 16, "%010lld", v); return std::string(b); };
    xr += off10(catOff) + " 00000 n \n" + off10(pagesOff) + " 00000 n \n" + off10(pageOff) + " 00000 n \n";
    xr += "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    emit(xr);
    CloseHandle(f);
    // NewDocument opens the blank in its OWN tab (BeginTabLoad): a busy front
    // tab keeps its state, an empty one is reused, so Ctrl+N never eats work
    if (TabOpenInto(FocusB(), file)) SetStatusHold(L"New blank document");
}

// CloseFile_Click -> CloseTab: the focused pane's front tab closes through the
// tab model's CloseTabCore - the neighbor takes the front, the last document
// lands on KillerPDF's empty state, and a dirty tab asks first.
static void CloseDoc() {
    CloseTabAt(FocusB(), Act(FocusB()));
}

// The shared write-out: every page renders at ~150dpi with the annotations
// composited - the same paint the screen shows - and lands as JPEG pages in a
// minimal PDF. SaveDoc's writer is the only emitter. The bytes land in a
// sibling temp first and REPLACE the target only once the whole PDF is out -
// a half-written save must never eat the file it was saving. SaveAs adopts
// the same rule: the copy is completed off to the side, then moved into place.
static bool WriteAnnotatedPdf(DocState& D, const std::wstring& file, std::wstring& err) {
    std::wstring tmp = file + L".docsy-tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"the file could not be written"; return false; }
    auto pageJpeg = [&](int i, std::vector<BYTE>& out) -> bool {
        int w = std::clamp((int)std::lround(D.pw[i] * 2.0f), 64, 2400);
        int h = max(1, (int)std::lround((float)w * D.ph[i] / max(1.f, D.pw[i])));
        HBITMAP b = Render(D, i, w, h);    // the writer renders synchronously: save needs
        if (!b) return false;              // every page's pixels now, not on the worker's
                                           // queue (and a save-sized 2x render never
                                           // pollutes the view cache the way GetBmp did)
        // the burn-in lands on a private 32bpp DIB section, and the JPEG encoder
        // reads the section's OWN memory (Bitmap over scan0). The old path handed
        // GDI+ an HBITMAP that was selected into a DC - GetDIBits, which GDI+
        // reads pixels through, is undefined for a selected bitmap and came back
        // all zeros: the saved PDF was white pages, and the encoder's walk over
        // the bad buffer took the app down. A DIB section has no DC coupling and
        // no HBITMAP wrap at all - the buffer is ours, the pixels are real.
        void* bits = nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;                    // top-down, like every consumer here
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) { if (dib) DeleteObject(dib); return false; }
        HDC ddc = CreateCompatibleDC(nullptr);
        HGDIOBJ od = SelectObject(ddc, dib);
        HDC sdc = CreateCompatibleDC(nullptr);         // the fresh render is a read-only
        HGDIOBJ osel = SelectObject(sdc, b);           // source: a plain blit, never drawn into
        BitBlt(ddc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
        SelectObject(sdc, osel); DeleteDC(sdc);
        DeleteObject(b);                   // the fresh render was this call's own
        float k = (float)w / max(1.f, D.pw[i]);
        PaintAnnots(ddc, D, i, 0, 0, k);               // annotations burn in
        CLSID jpg{}; UINT cnt = 0, bytes = 0;
        Gdiplus::GetImageEncodersSize(&cnt, &bytes);
        std::vector<BYTE> buf(bytes);
        auto* enc = (Gdiplus::ImageCodecInfo*)buf.data();
        Gdiplus::GetImageEncoders(cnt, bytes, enc);
        for (UINT e = 0; e < cnt; e++) if (!lstrcmpW(enc[e].MimeType, L"image/jpeg")) jpg = enc[e].Clsid;
        IStream* os = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &os) != S_OK) { SelectObject(ddc, od); DeleteDC(ddc); DeleteObject(dib); return false; }
        bool ok = false;
        {
            Gdiplus::Bitmap bm(w, h, w * 4, PixelFormat32bppRGB, (BYTE*)bits);   // wraps OUR
            ok = bm.Save(os, &jpg, nullptr) == Gdiplus::Ok;   // buffer - it stays alive
        }                                                     // until after this Save
        if (ok) {
            LARGE_INTEGER z1{}; z1.QuadPart = 0;
            os->Seek(z1, STREAM_SEEK_SET, nullptr);
            STATSTG st2{};
            os->Stat(&st2, STATFLAG_NONAME);
            ULARGE_INTEGER sz = st2.cbSize;
            out.resize((size_t)sz.QuadPart);
            ULONG rd = 0;
            ok = os->Read(out.data(), (ULONG)out.size(), &rd) == S_OK && rd == out.size();
        }
        os->Release();
        SelectObject(ddc, od); DeleteDC(ddc); DeleteObject(dib);   // the bitmap view is
        return ok && !out.empty();                                 // gone before the buffer
    };
    std::string head = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    DWORD wr = 0;
    WriteFile(f, head.data(), (DWORD)head.size(), &wr, nullptr);
    long long pos = (long long)head.size();
    auto emit = [&](const std::string& s) { WriteFile(f, s.data(), (DWORD)s.size(), &wr, nullptr); pos += (long long)s.size(); };
    long long catOff = pos;
    emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    long long pagesOff = pos;
    std::string kids;
    // object numbering: catalog 1, pages 2, then THREE objects per page -
    // page 3+3i, contents 4+3i, image 5+3i. The old 2-per-page stride made
    // page i's number (3+2i) collide with page i-1's image (5+2(i-1)): two
    // "N 0 obj" bodies, an xref that pointed the Kids at image XObjects, and
    // a document whose pages rendered white everywhere but page one.
    for (int i = 0; i < D.n; i++) kids += std::to_string(3 + i * 3) + " 0 R ";
    emit("2 0 obj\n<< /Type /Pages /Kids [ " + kids + "] /Count " + std::to_string(D.n) + " >>\nendobj\n");
    std::vector<long long> pageOff(D.n), contOff(D.n), imgOff(D.n);
    bool failed = false;
    for (int i = 0; i < D.n && !failed; i++) {
        std::vector<BYTE> jpg;
        if (!pageJpeg(i, jpg)) { failed = true; break; }
        // Windows.Data.Pdf's Size() is DIPs (96/inch); the page box wants PDF
        // points (72/inch) - x0.75 lands the saved page at its true print size
        // (the old x2 wrote DIPs as points and bloated every page by a third)
        float wpt = D.pw[i] * 0.75f, hpt = D.ph[i] * 0.75f;
        char hb2[256];
        pageOff[i] = pos;
        snprintf(hb2, 256, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Contents %d 0 R /Resources << /XObject << /Im %d 0 R >> >> >>\nendobj\n",
                 3 + i * 3, wpt, hpt, 4 + i * 3, 5 + i * 3);
        emit(hb2);
        contOff[i] = pos;
        std::string cs = "q\n" + std::to_string(wpt).substr(0, 8) + " 0 0 " + std::to_string(hpt).substr(0, 8) + " 0 0 cm\n/Im Do\nQ\n";
        emit(std::to_string(4 + i * 3) + " 0 obj\n<< /Length " + std::to_string(cs.size()) + " >>\nstream\n" + cs + "endstream\nendobj\n");
        imgOff[i] = pos;
        std::string ih = std::to_string(5 + i * 3) + " 0 obj\n<< /Type /XObject /Subtype /Image /Width " +
            std::to_string(std::clamp((int)std::lround(D.pw[i] * 2.0f), 64, 2400)) +
            " /Height " + std::to_string(max(1, (int)std::lround((float)std::clamp((int)std::lround(D.pw[i] * 2.0f), 64, 2400) * D.ph[i] / max(1.f, D.pw[i])))) +
            " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length " + std::to_string(jpg.size()) + " >>\nstream\n";
        emit(ih);
        WriteFile(f, jpg.data(), (DWORD)jpg.size(), &wr, nullptr);
        pos += (long long)jpg.size();
        emit("\nendstream\nendobj\n");
    }
    if (failed) {
        CloseHandle(f);
        DeleteFileW(tmp.c_str());   // the temp dies alone; the real file never knew
        err = L"a page could not be rendered";
        return false;
    }
    long long xref = pos;
    std::string xr = "xref\n0 " + std::to_string(3 + D.n * 3) + "\n0000000000 65535 f \n";
    auto off10 = [](long long v) { char b[16]; snprintf(b, 16, "%010lld", v); return std::string(b); };
    xr += off10(catOff) + " 00000 n \n" + off10(pagesOff) + " 00000 n \n";
    for (int i = 0; i < D.n; i++)
        xr += off10(pageOff[i]) + " 00000 n \n" + off10(contOff[i]) + " 00000 n \n" + off10(imgOff[i]) + " 00000 n \n";
    xr += "trailer\n<< /Size " + std::to_string(3 + D.n * 3) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    emit(xr);
    CloseHandle(f);
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());   // the temp dies alone; the real file never knew
        err = L"the file could not be replaced";
        return false;
    }
    return true;
}

// Save_Click: no document -> "Open a file first"; a real path -> KillerPDF's
// save prompt, but only over CHANGES - a document matching its checkpoint has
// nothing to replace, so Ctrl+S stays quiet ("No changes to save"). The
// prompt is the themed KillerDialog box, "Replace {name}?" answered with
// Overwrite (saves in place), Save As (the file dialog, then the new path is
// adopted) or Cancel; Save As (Ctrl+Shift+S or the chevron) always asks and
// always writes - export semantics, no dirty gate.
static void SaveDoc(bool saveAs) {
    DocState& D = DVF();
    if (!D.n) { SetStatusHold(L"Open a file first"); return; }
    if (g.tedit) CommitTextEdit();
    std::wstring file = D.path;
    if (saveAs || file.empty()) {
        wchar_t buf[MAX_PATH * 2] = L"";
        wcsncpy(buf, D.name.c_str(), MAX_PATH * 2 - 1);
        OPENFILENAMEW o{}; o.lStructSize = sizeof o; o.hwndOwner = g.main;
        o.lpstrFilter = L"PDF files (*.pdf)\0*.pdf\0All files\0*.*\0";
        o.lpstrFile = buf; o.nMaxFile = (DWORD)_countof(buf);
        o.lpstrTitle = L"Save PDF As"; o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        o.lpstrDefExt = L"pdf";
        if (!GetSaveFileNameW(&o)) { SetStatusHold(L"Save canceled (no file written)"); return; }
        file = buf;
    } else {
        if (!DocDirty(D)) { SetStatusHold(L"No changes to save"); return; }
        int r = AskPrompt3(L" - Save", (std::wstring(L"Replace ") + D.name + L"?").c_str(),
                           L"Cancel", L"Save As", L"Overwrite");
        if (r == 1) { SaveDoc(true); return; }          // Save As asks for the new path
        if (r != 2) { SetStatusHold(L"Save canceled"); return; }   // Cancel or dismissed
    }
    std::wstring err;
    if (!WriteAnnotatedPdf(D, file, err)) { SetStatusHold(std::wstring(L"Save failed: ") + err); return; }
    SnapClean(D);                                // this state is now the checkpoint
    if (file != D.path) {                        // Save As adopts its file (KillerPDF's
        D.path = file;                           // SaveAsWithName lands the doc on it)
        size_t sl = file.find_last_of(L"\\/");
        D.name = sl == std::wstring::npos ? file : file.substr(sl + 1);
        UpdateTitle();
    }
    SetStatusHold(std::wstring(L"Saved to ") + file);
}

// ---------------- sidebar ----------------

static int SW() {                                                     // sidebar window width
    int w = g.sideW ? g.sideW : (int)(180 * g.dpi);                   // KillerPDF's pages-panel default
    int lo = (int)(120 * g.dpi);                                      // nMinColumn
    int hi = (int)(234 * g.dpi);                                      // nMaxPages (KillerPDF's pages cap)
    RECT r;
    if (g.main && GetClientRect(g.main, &r) && r.right > 0)
        hi = max(lo, min(hi, (int)r.right - (int)(160 * g.dpi)));
    return std::clamp(w, lo, hi);
}
static int STH(int i) {                 // sidebar thumb height: the FOCUSED pane's pages,
    DocState& D = DVF();                // the sidebar is a window-level surface like KillerPDF's
    if (i < 0 || i >= D.n) return 1;    // a stale index (the document switched mid-flight) reads nothing
    return max(1, (int)std::lround(sd.tw * D.ph[i] / max(1.f, D.pw[i])));
}
static int SidePageAt(int y) { return PageIn(sd.top, DVF().n, y); }

static void SideScrollTo(int y) {
    y = std::clamp(y, 0, max(0, sd.docH - sd.ch));
    if (y == sd.scroll) return;
    sd.scroll = y;
    if (g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE)) {
        InvalidateRect(g.side, nullptr, FALSE);
        SetTimer(g.side, 2, 25, nullptr);   // pump newly visible thumbs
    }
}

static void SideLayout() {
    DocState& D = DVF();                    // the sidebar lays out the FOCUSED pane's pages
    if (!D.n || sd.cw <= 0) { sd.docH = 0; return; }
    if (!g.resizing) {                             // splitter drags keep the old pitch and thumbs;
        int tw = (int)(96 * g.dpi);                // fixed-size thumbs, centred: smaller than
        if (sd.tw != tw) {                         // KillerPDF's ~134px stretch-to-width card
            sd.tw = tw;
            for (auto& kv : sd.th) if (kv.second.b) DeleteObject(kv.second.b);
            sd.th.clear(); sd.thBytes = 0;
        }
        // KillerPDF's page item: a full-width ListBoxItem band (Padding 8,6) around a
        // StackPanel (Margin 8,2) holding the thumb and the label line. Consecutive
        // bands touch, the way list items stack - a selected band reads as one card.
        int padT = (int)(8 * g.dpi);                    // 6 item pad + 2 stack margin above the thumb
        int below = (int)(28 * g.dpi);                  // 2 gap + ~20 label line + 6 pad below it
        sd.top.assign((size_t)D.n + 1, 0);
        int y = (int)(8 * g.dpi);                       // the first band's top
        for (int i = 0; i < D.n; i++) { sd.top[i] = y + padT; y = sd.top[i] + STH(i) + below; }
        sd.top[D.n] = y;
        sd.docH = y + (int)(8 * g.dpi);
    }
    sd.scroll = std::clamp(sd.scroll, 0, max(0, sd.docH - sd.ch));
    InvalidateRect(g.side, nullptr, FALSE);
    SetTimer(g.side, 2, 25, nullptr);
}

// The rail's thumb LRU: evicts farthest-off-screen first, oldest first,
// against a byte budget and a hard entry ceiling - so a 2,000-page document
// keeps a couple thousand light thumbs resident while a giant-dpi strip gives
// memory back.
static const long long THUMB_CACHE_BUDGET = 96LL << 20;
static void SideThEvict(int first, int last) {
    for (;;) {
        if (sd.thBytes <= THUMB_CACHE_BUDGET && sd.th.size() <= 4096) return;
        int k = -1; unsigned long long best = ~0ULL;
        for (auto& kv : sd.th)
            if ((kv.first < first - 2 || kv.first > last + 2) && kv.second.t < best) { best = kv.second.t; k = kv.first; }
        if (k < 0) return;                       // everything left is on screen
        if (sd.th[k].b) { sd.thBytes -= (long long)sd.th[k].w * sd.th[k].h * 4; DeleteObject(sd.th[k].b); }
        sd.th.erase(k);
    }
}

// Asks for every visible thumbnail (the worker renders them in recency order,
// results land through the drain). Returns true while visible thumbs are still
// missing, so a 25 ms timer can re-pump the strip as the scroll reveals more.
// Shot mode keeps the old one-thumb-per-call contract - the startup loop
// expects to fill the rail synchronously before its one composite.
static bool SidePump() {
    DocState& D = DVF();                    // thumbs render from the FOCUSED pane's document
    if (!D.n || sd.ch <= 0 || sd.docH <= 0) return false;
    if (!(g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE))) return false;   // nothing to pump while hidden (shot mode shows it first)
    int first = SidePageAt(sd.scroll), last = first;
    while (last + 1 < D.n && sd.top[last + 1] < sd.scroll + sd.ch) last++;
    if (g_sync) {
        int pick = -1, bestd = INT_MAX, center = (first + last) / 2;
        for (int i = max(0, first - 1); i <= min(D.n - 1, last + 1); i++) {
            if (sd.th.find(i) != sd.th.end()) continue;
            int d = abs(i - center);
            if (d < bestd) { bestd = d; pick = i; }
        }
        if (pick < 0) return false;
        HBITMAP b = Render(D, pick, sd.tw, STH(pick));
        sd.th[pick] = Ent{ b, sd.tw, STH(pick), 0, ++sd.tick };   // b may be null: stays a placeholder, no retry spin
        if (b) sd.thBytes += (long long)sd.tw * STH(pick) * 4;
        SideThEvict(first, last);
        InvalidateRect(g.side, nullptr, FALSE);
        return true;
    }
    int missing = 0;
    RqPurgeThumbs(D.id, first, last);       // the strip's stale asks die here: another document's
    for (int i = max(0, first - 1); i <= min(D.n - 1, last + 1); i++) {   // thumbs, pages the scroll left
        if (sd.th.find(i) != sd.th.end()) continue;
        missing++;
        RqEnqueue(D, i, RJ_THUMB, sd.tw, STH(i), 0, 0);
    }
    return missing > 0;
}

// Keeps the sidebar highlight in step with the view and follows the current
// page. The follow is edge-reversed on purpose: a selection that runs off the
// bottom of the rail is scrolled flush to the TOP edge (the pages after it
// stay in sight below), and one that runs off the top lands flush at the
// bottom edge - instead of the usual ensure-visible, which anchored it the
// other way around and parked the selected card at the bottom of the rail.
static void SideSync() {
    DocState& D = DVF();                    // the ring follows the focused pane's page
    if (!g.side || !D.n) { sd.cur = -1; return; }
    int c = D.cur;
    if (c == sd.cur) return;
    sd.cur = c;
    if (c >= 0 && (size_t)c + 1 < sd.top.size() && sd.docH > 0 && GetTickCount64() >= sd.hold) {
        int y0 = sd.top[c] - (int)(8 * g.dpi);                        // the item band's edges
        int y1 = sd.top[c] + STH(c) + (int)(28 * g.dpi);
        if (y1 > sd.scroll + sd.ch) SideScrollTo(y0 - (int)(4 * g.dpi));          // off the bottom: card at the top
        else if (y0 < sd.scroll) SideScrollTo(y1 - sd.ch + (int)(4 * g.dpi));     // off the top: card at the bottom
    }
    if (g.side && (GetWindowLongW(g.side, GWL_STYLE) & WS_VISIBLE)) InvalidateRect(g.side, nullptr, FALSE);
}

