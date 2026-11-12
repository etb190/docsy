// ============================================================
//  The browser. One toolbar button toggles between the PDF reader and a web
//  mode dressed like Safari (dark appearance): a flat unified toolbar on top
//  (reader button, back/forward chevrons, a centered address field that shows
//  the site while idle - padlock hugging the name - and opens to the full URL
//  with a blue focus ring, reload/stop inside its right edge, a progress line
//  along its foot while a page loads, the menu at the far right), and under
//  it the READER'S OWN tab strip, carried over unchanged for the web tabs:
//  the grainy field, #555 dividers, the 1px green rule over the open tab, the
//  muted close x that turns red, the + at the right. The WebView rides in the
//  same rounded card the reader's pages sit in. The engine is Windows' own
//  WebView2 - Evergreen Chromium, shared with Edge - so the exe carries no
//  browser bytes at all, only this glue. docsy's page-thumbnail rail keeps
//  showing the OPEN WEB TABS, captured live with CapturePreview. No runtime
//  (rare): the mode still toggles and explains itself, nothing else changes.
// ============================================================

struct WebTab {
    unsigned long long id = 0;
    std::wstring url, title;
    wrl::ComPtr<ICoreWebView2Controller> ctr;
    wrl::ComPtr<ICoreWebView2> web;
    double zoom = 1.0;
    bool building = false;
    bool loading = false, canBack = false, canFwd = false;
    bool capturing = false, capturePending = false;
    unsigned long long captureDue = 0;
    unsigned long long navStart = 0;       // when the live navigation left the dock; 0 at rest.
                                           // The watchdog's clock: a navigation that never lands
                                           // is the launch blackscreen's one remaining shape
    bool navKicked = false, navReloaded = false;   // the self-heal's own stages, once each
    int renderDeaths = 0;                  // render-process failures the tab has lived through
    std::wstring pending;                  // the URL a brand-new tab opens once its
                                           // controller finishes building
    EventRegistrationToken tkNavS{}, tkNavC{}, tkSrc{}, tkTitle{}, tkHist{}, tkDl{}, tkProc{};
    HBITMAP thumb = nullptr; int thumbW = 0, thumbH = 0; double ar = 0.0;
};
static std::vector<WebTab> g_wt;
static unsigned long long g_wtIds = 1;
static wrl::ComPtr<ICoreWebView2Environment> g_wvEnv;
static bool g_wvEnvBuilding = false;
static bool g_webChecked = false, g_webAvail = false;

// the sidebar's web state: the one definition lives up by the palette, where
// the toolbar's proc can see ws.sHot; the comment there tells why.

static HWND g_wedit = nullptr;             // the omnibox edit
static WNDPROC g_weditPrev = nullptr;
static std::wstring g_webVer;              // the runtime's version, for the fail card
static HFONT g_webEditFont = nullptr;
static HBRUSH g_webEditBrush = nullptr;
static RECT g_webOmni{};                   // the pill, in bar coords
static RECT g_webFailBtn{};                // the no-engine card's button, view coords

enum { WB_READER, WB_BACK, WB_FWD, WB_RELOAD, WB_NEWTAB, WB_MENU, WB_SIDE, WB_EXT, WB_COUNT };
static RECT g_wbR[WB_COUNT];

static WebTab* WebTabById(unsigned long long id) {
    for (auto& t : g_wt) if (t.id == id) return &t;
    return nullptr;
}
static WebTab* WebCur() { return g_wtCur >= 0 && g_wtCur < (int)g_wt.size() ? &g_wt[g_wtCur] : nullptr; }
static int WebCurIdx() { return g_wtCur; }

static void WebRepaintChrome() {
    if (g_wstrip) InvalidateRect(g_wstrip, nullptr, FALSE);
    if (g.bar) InvalidateRect(g.bar, nullptr, FALSE);
    if (g.side) InvalidateRect(g.side, nullptr, FALSE);
    StatusRepaint();
}
static void WebStripSpin();                // the loading spinner's timer, below
static void WebSyncOmnibox();              // module order: the omnibox helpers live
static void WebSyncOmniboxForce();         // below their first callers
static LRESULT CALLBACK WebEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp);

// ---------------------------------------------------------------------------
// Extension packages: everything between a Chrome Web Store download and an
// unpacked folder WebView2 can load, in plain C++ (no zlib, no tar, no
// PowerShell). A .crx is a small header in front of an ordinary zip; the
// header carries the developer's public key, and Chrome derives an
// extension's ID from that key - so the key is written back into the
// unpacked manifest.json, which makes the loaded extension keep its real
// store ID (extensions that name themselves in OAuth redirects, externally
// connectable pages or web-accessible URLs depend on it).
// ---------------------------------------------------------------------------
namespace crxk {

static void Sha256(const unsigned char* d, size_t n, unsigned char out[32]) {
    static const unsigned K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    unsigned h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    std::vector<unsigned char> m(d, d + n);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    unsigned long long bits = (unsigned long long)n * 8;
    for (int i = 7; i >= 0; i--) m.push_back((unsigned char)(bits >> (i * 8)));
    auto R = [](unsigned x, int k) { return (x >> k) | (x << (32 - k)); };
    for (size_t o = 0; o < m.size(); o += 64) {
        unsigned w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (unsigned)m[o + i*4] << 24 | (unsigned)m[o + i*4 + 1] << 16 | (unsigned)m[o + i*4 + 2] << 8 | m[o + i*4 + 3];
        for (int i = 16; i < 64; i++) {
            unsigned s0 = R(w[i-15], 7) ^ R(w[i-15], 18) ^ (w[i-15] >> 3);
            unsigned s1 = R(w[i-2], 17) ^ R(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g2 = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            unsigned S1 = R(e, 6) ^ R(e, 11) ^ R(e, 25), ch = (e & f) ^ (~e & g2);
            unsigned t1 = hh + S1 + ch + K[i] + w[i];
            unsigned S0 = R(a, 2) ^ R(a, 13) ^ R(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
            unsigned t2 = S0 + mj;
            hh = g2; g2 = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e; h[5] += f; h[6] += g2; h[7] += hh;
    }
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[i*4 + j] = (unsigned char)(h[i] >> (24 - j * 8));
}

static std::string B64(const unsigned char* d, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i+1] << 8 : 0) | (i + 2 < n ? d[i+2] : 0);
        r += T[v >> 18 & 63]; r += T[v >> 12 & 63];
        r += i + 1 < n ? T[v >> 6 & 63] : '='; r += i + 2 < n ? T[v & 63] : '=';
    }
    return r;
}

// ---- raw DEFLATE (RFC 1951): canonical-Huffman decoder in the puff.c shape ----
struct BitIn { const unsigned char* p; size_t n, pos = 0; unsigned long long buf = 0; int cnt = 0; bool err = false;
    int Bits(int k) {
        while (cnt < k) { if (pos >= n) { err = true; return 0; } buf |= (unsigned long long)p[pos++] << cnt; cnt += 8; }
        int v = (int)(buf & ((1ull << k) - 1)); buf >>= k; cnt -= k; return v; } };
struct Huff { short count[16], symbol[288]; };
static int HuffBuild(Huff& h, const short* len, int n) {
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= h.count[l]; if (left < 0) return left; }
    short offs[16]; offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h.count[l];
    for (int i = 0; i < n; i++) if (len[i]) h.symbol[offs[len[i]]++] = (short)i;
    return left;
}
static int HuffDecode(BitIn& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= b.Bits(1); if (b.err) return -1;
        int c = h.count[l];
        if (code - c < first) return h.symbol[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
}
static bool Inflate(const unsigned char* src, size_t n, std::vector<unsigned char>& out, size_t limit) {
    static const short lb[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short le[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short db[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short de[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    BitIn b{ src, n };
    int last;
    do {
        last = b.Bits(1); int type = b.Bits(2);
        if (b.err) return false;
        if (type == 0) {
            b.buf = 0; b.cnt = 0;
            if (b.pos + 4 > n) return false;
            unsigned len = src[b.pos] | src[b.pos + 1] << 8; b.pos += 4;
            if (b.pos + len > n || out.size() + len > limit) return false;
            out.insert(out.end(), src + b.pos, src + b.pos + len); b.pos += len;
        } else if (type == 1 || type == 2) {
            Huff lit, dist; short len[320];
            if (type == 1) {
                int i = 0;
                for (; i < 144; i++) len[i] = 8;
                for (; i < 256; i++) len[i] = 9;
                for (; i < 280; i++) len[i] = 7;
                for (; i < 288; i++) len[i] = 8;
                HuffBuild(lit, len, 288);
                for (i = 0; i < 30; i++) len[i] = 5;
                HuffBuild(dist, len, 30);
            } else {
                int nlen = b.Bits(5) + 257, ndist = b.Bits(5) + 1, ncode = b.Bits(4) + 4;
                if (b.err || nlen > 286 || ndist > 30) return false;
                static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                int i = 0;
                for (; i < ncode; i++) len[order[i]] = (short)b.Bits(3);
                for (; i < 19; i++) len[order[i]] = 0;
                Huff cl; if (HuffBuild(cl, len, 19) != 0) return false;
                i = 0;
                while (i < nlen + ndist) {
                    int sym = HuffDecode(b, cl); if (sym < 0) return false;
                    if (sym < 16) len[i++] = (short)sym;
                    else {
                        int rep, v = 0;
                        if (sym == 16) { if (i == 0) return false; v = len[i - 1]; rep = 3 + b.Bits(2); }
                        else if (sym == 17) rep = 3 + b.Bits(3); else rep = 11 + b.Bits(7);
                        if (i + rep > nlen + ndist) return false;
                        while (rep--) len[i++] = (short)v;
                    }
                }
                if (len[256] == 0) return false;
                int e1 = HuffBuild(lit, len, nlen); if (e1 < 0 || (e1 > 0 && nlen - lit.count[0] != 1)) return false;
                int e2 = HuffBuild(dist, len + nlen, ndist); if (e2 < 0 || (e2 > 0 && ndist - dist.count[0] != 1)) return false;
            }
            for (;;) {
                int sym = HuffDecode(b, lit); if (sym < 0) return false;
                if (sym < 256) { if (out.size() >= limit) return false; out.push_back((unsigned char)sym); }
                else if (sym == 256) break;
                else {
                    sym -= 257; if (sym >= 29) return false;
                    int l = lb[sym] + b.Bits(le[sym]);
                    int ds = HuffDecode(b, dist); if (ds < 0 || ds >= 30) return false;
                    size_t dd = (size_t)db[ds] + b.Bits(de[ds]);
                    if (b.err || dd > out.size() || out.size() + l > limit) return false;
                    for (int k = 0; k < l; k++) out.push_back(out[out.size() - dd]);
                }
            }
        } else return false;
    } while (!last);
    return !b.err;
}

// ---- zip: central directory, stored + deflated entries ----
struct ZipEnt { std::string name; unsigned method, csize, usize, off; };
static unsigned U16(const unsigned char* p) { return p[0] | p[1] << 8; }
static unsigned U32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static bool ZipList(const unsigned char* d, size_t n, std::vector<ZipEnt>& v) {
    if (n < 22) return false;
    size_t e = n - 22; bool found = false;
    size_t lim = n - 22 < 65557 ? n - 22 : 65557;
    for (size_t k = 0; k <= lim; k++, e--)
        if (U32(d + e) == 0x06054b50) { found = true; break; }
    if (!found) return false;
    unsigned cnt = U16(d + e + 10); size_t cd = U32(d + e + 16);
    for (unsigned i = 0; i < cnt; i++) {
        if (cd + 46 > n || U32(d + cd) != 0x02014b50) return false;
        ZipEnt z; z.method = U16(d + cd + 10); z.csize = U32(d + cd + 20); z.usize = U32(d + cd + 24);
        unsigned nl = U16(d + cd + 28), el = U16(d + cd + 30), cl = U16(d + cd + 32); z.off = U32(d + cd + 42);
        if (cd + 46 + nl > n) return false;
        z.name.assign((const char*)d + cd + 46, nl);
        v.push_back(z); cd += 46 + nl + el + cl;
    }
    return true;
}
static bool ZipRead(const unsigned char* d, size_t n, const ZipEnt& z, std::vector<unsigned char>& out) {
    if ((size_t)z.off + 30 > n || U32(d + z.off) != 0x04034b50) return false;
    size_t s = (size_t)z.off + 30 + U16(d + z.off + 26) + U16(d + z.off + 28);
    if (s + z.csize > n || z.usize > (256u << 20)) return false;
    out.clear();
    if (z.method == 0) { out.assign(d + s, d + s + z.csize); return true; }
    if (z.method == 8) return Inflate(d + s, z.csize, out, z.usize);
    return false;
}

// ---- the .crx header: where the zip starts, and the developer's public key ----
static bool Varint(const unsigned char* d, size_t n, size_t& p, unsigned long long& v) {
    v = 0; for (int s = 0; s < 64 && p < n; s += 7) { unsigned char c = d[p++]; v |= (unsigned long long)(c & 0x7f) << s; if (!(c & 0x80)) return true; }
    return false;
}
struct Field { unsigned long long num; const unsigned char* p; size_t len; };
static bool PbFields(const unsigned char* d, size_t n, std::vector<Field>& f) {
    size_t p = 0;
    while (p < n) {
        unsigned long long tag, v; if (!Varint(d, n, p, tag)) return false;
        int wt = (int)(tag & 7);
        if (wt == 0) { if (!Varint(d, n, p, v)) return false; }
        else if (wt == 2) { if (!Varint(d, n, p, v) || p + v > n) return false; f.push_back({ tag >> 3, d + p, (size_t)v }); p += (size_t)v; }
        else if (wt == 1) p += 8; else if (wt == 5) p += 4; else return false;
    }
    return true;
}
static bool CrxParse(const unsigned char* d, size_t n, size_t& zipOff, std::string& key) {
    key.clear();
    if (n >= 4 && d[0] == 'P' && d[1] == 'K') { zipOff = 0; return true; }     // a bare zip
    if (n < 16 || memcmp(d, "Cr24", 4) != 0) return false;
    unsigned ver = U32(d + 4);
    if (ver == 2) {
        size_t pk = U32(d + 8), sg = U32(d + 12);
        if (16 + pk + sg > n) return false;
        key.assign((const char*)d + 16, pk); zipOff = 16 + pk + sg; return true;
    }
    if (ver != 3) return false;
    size_t hl = U32(d + 8);
    if (12 + hl > n) return false;
    zipOff = 12 + hl;
    std::vector<Field> top; if (!PbFields(d + 12, hl, top)) return true;       // no key: still loadable
    std::vector<std::string> keys; std::string crxId;
    for (auto& f : top) {
        if (f.num == 2 || f.num == 3) {                                        // RSA / ECDSA proofs
            std::vector<Field> in; if (!PbFields(f.p, f.len, in)) continue;
            for (auto& g : in) if (g.num == 1) keys.emplace_back((const char*)g.p, g.len);
        } else if (f.num == 10000) {                                           // signed_header_data
            std::vector<Field> in; if (!PbFields(f.p, f.len, in)) continue;
            for (auto& g : in) if (g.num == 1) crxId.assign((const char*)g.p, g.len);
        }
    }
    for (auto& k : keys) {                                                     // the key the CRX id names
        unsigned char h[32]; Sha256((const unsigned char*)k.data(), k.size(), h);
        if (crxId.size() == 16 && memcmp(h, crxId.data(), 16) == 0) { key = k; return true; }
    }
    if (!keys.empty()) key = keys[0];
    return true;
}

// the manifest gets "key" as its first top-level member (skipped when it has one)
static bool JsonHasTopKey(const std::string& s, const char* name) {
    int depth = 0; size_t n = s.size();
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') {
            size_t j = i + 1; std::string str;
            while (j < n && s[j] != '"') { if (s[j] == '\\' && j + 1 < n) j++; str += s[j]; j++; }
            if (depth == 1) { size_t k = j + 1; while (k < n && isspace((unsigned char)s[k])) k++; if (k < n && s[k] == ':' && str == name) return true; }
            i = j;
        } else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
    }
    return false;
}
static std::string ManifestWithKey(const std::string& m, const std::string& keyDer) {
    if (keyDer.empty() || JsonHasTopKey(m, "key")) return m;
    size_t b = m.find('{'); if (b == std::string::npos) return m;
    std::string r = m;
    r.insert(b + 1, "\"key\":\"" + B64((const unsigned char*)keyDer.data(), keyDer.size()) + "\",");
    return r;
}
// the 32-letter extension id (a-p) Chrome derives from the key
static std::string IdFromKey(const std::string& keyDer) {
    unsigned char h[32]; Sha256((const unsigned char*)keyDer.data(), keyDer.size(), h);
    std::string id; for (int i = 0; i < 16; i++) { id += (char)('a' + (h[i] >> 4)); id += (char)('a' + (h[i] & 15)); }
    return id;
}

// ---- a small JSON reader for manifest.json / messages.json (comments tolerated) ----
struct J {
    char t = 'n';                                   // n null, b bool, d number, s string, a array, o object
    std::string s; double d = 0; bool b = false;
    std::vector<J> a; std::vector<std::pair<std::string, J>> o;
    const J* get(const char* k) const {
        if (t != 'o') return nullptr;
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    std::string str() const { return t == 's' ? s : std::string(); }
};
struct JP {
    const std::string& x; size_t i = 0;
    explicit JP(const std::string& s) : x(s) {}
    void Ws() {
        for (;;) {
            while (i < x.size() && (x[i] == ' ' || x[i] == '\t' || x[i] == '\r' || x[i] == '\n')) i++;
            if (i + 1 < x.size() && x[i] == '/' && x[i + 1] == '/') { while (i < x.size() && x[i] != '\n') i++; }
            else if (i + 1 < x.size() && x[i] == '/' && x[i + 1] == '*') {
                size_t e = x.find("*/", i + 2); i = e == std::string::npos ? x.size() : e + 2;
            } else break;
        }
    }
    static void Utf8(std::string& r, unsigned c) {
        if (c < 0x80) r += (char)c;
        else if (c < 0x800) { r += (char)(0xC0 | c >> 6); r += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { r += (char)(0xE0 | c >> 12); r += (char)(0x80 | (c >> 6 & 63)); r += (char)(0x80 | (c & 63)); }
        else { r += (char)(0xF0 | c >> 18); r += (char)(0x80 | (c >> 12 & 63)); r += (char)(0x80 | (c >> 6 & 63)); r += (char)(0x80 | (c & 63)); }
    }
    bool Hex4(unsigned& v) {
        if (i + 4 > x.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = x[i++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false;
        }
        return true;
    }
    bool Str(std::string& r) {
        if (i >= x.size() || x[i] != '"') return false;
        i++;
        while (i < x.size() && x[i] != '"') {
            char c = x[i++];
            if (c != '\\') { r += c; continue; }
            if (i >= x.size()) return false;
            char e = x[i++];
            switch (e) {
            case 'n': r += '\n'; break; case 't': r += '\t'; break; case 'r': r += '\r'; break;
            case 'b': r += '\b'; break; case 'f': r += '\f'; break;
            case 'u': {
                unsigned u; if (!Hex4(u)) return false;
                if (u >= 0xD800 && u < 0xDC00 && i + 1 < x.size() && x[i] == '\\' && x[i + 1] == 'u') {
                    i += 2; unsigned lo; if (!Hex4(lo)) return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                Utf8(r, u); break; }
            default: r += e; break;
            }
        }
        if (i >= x.size()) return false;
        i++; return true;
    }
    bool Val(J& v, int depth) {
        if (depth > 48) return false;
        Ws(); if (i >= x.size()) return false;
        char c = x[i];
        if (c == '{') {
            v.t = 'o'; i++; Ws();
            if (i < x.size() && x[i] == '}') { i++; return true; }
            for (;;) {
                Ws(); std::string k; if (!Str(k)) return false;
                Ws(); if (i >= x.size() || x[i] != ':') return false; i++;
                J c2; if (!Val(c2, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(c2));
                Ws(); if (i >= x.size()) return false;
                if (x[i] == ',') { i++; Ws(); if (i < x.size() && x[i] == '}') { i++; return true; } continue; }
                if (x[i] == '}') { i++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.t = 'a'; i++; Ws();
            if (i < x.size() && x[i] == ']') { i++; return true; }
            for (;;) {
                J c2; if (!Val(c2, depth + 1)) return false;
                v.a.push_back(std::move(c2));
                Ws(); if (i >= x.size()) return false;
                if (x[i] == ',') { i++; Ws(); if (i < x.size() && x[i] == ']') { i++; return true; } continue; }
                if (x[i] == ']') { i++; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = 's'; return Str(v.s); }
        if (x.compare(i, 4, "true") == 0) { v.t = 'b'; v.b = true; i += 4; return true; }
        if (x.compare(i, 5, "false") == 0) { v.t = 'b'; i += 5; return true; }
        if (x.compare(i, 4, "null") == 0) { i += 4; return true; }
        size_t st = i;
        while (i < x.size() && (isdigit((unsigned char)x[i]) || x[i] == '-' || x[i] == '+' || x[i] == '.' || x[i] == 'e' || x[i] == 'E')) i++;
        if (st == i) return false;
        v.t = 'd'; v.d = atof(x.substr(st, i - st).c_str()); return true;
    }
};
static bool JParse(const std::string& in, J& out) {
    size_t o = in.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    std::string s = in.substr(o);
    JP p(s); out = J();
    return p.Val(out, 0);
}
} // namespace crxk

