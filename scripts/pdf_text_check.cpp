// ---------------------------------------------------------------------------
// pdf_text_check.cpp - the Linux harness for docsy's PDF text layer.
//
// Compiles the pure-std:: engine half of src/pdf_text.inl standalone
// (DOCSY_PDFTEXT_TEST) and asserts it against the synthetic documents in
// testdata/ (scripts/make_test_pdfs.py builds them): the classic-table and
// cross-reference-stream xrefs, object streams, FlateDecode, the simple-font
// encodings with Differences, the Type0/Identity-H fonts with their ToUnicode
// CMaps - Arabic, Persian, French accents, Cyrillic - form XObjects,
// ActualText marked content, TJ positioning, and the /Rotate mapping the
// selection boxes ride.
//
//   python3 scripts/make_test_pdfs.py
//   g++ -std=c++20 -O1 -I src -o /tmp/pdf_text_check scripts/pdf_text_check.cpp
//   /tmp/pdf_text_check scripts/testdata
//
// The harness carries its own DEFLATE core (the puff.c-shaped decoder
// crx_unpack.inl ships as crxk::Inflate, copied so the test builds on Linux
// without dragging the CRX code in); docsy.exe itself uses that one.
// ---------------------------------------------------------------------------
#define DOCSY_PDFTEXT_TEST
#include "pdf_text.inl"

#include <cstdio>
#include <string>
#include <vector>

namespace crxk {

// ---- raw DEFLATE (RFC 1951): canonical-Huffman decoder in the puff.c shape --
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
bool Inflate(const unsigned char* src, size_t n, std::vector<unsigned char>& out, size_t limit) {
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
        if (last) break;
    } while (true);
    return true;
}

} // namespace crxk

// ------------------------------------------------------------------ tests --
static int g_failed = 0, g_total = 0;

static void Expect(bool ok, const char* what) {
    g_total++;
    if (!ok) {
        g_failed++;
        printf("  FAIL: %s\n", what);
    }
}
static void ExpectText(bool ok, const char* what, const std::wstring& got, const std::wstring& want) {
    g_total++;
    if (!ok) {
        g_failed++;
        printf("  FAIL: %s\n    got:  \"", what);
        for (wchar_t c : got) printf("%04X ", (unsigned)c);
        printf("\"\n    want: \"");
        for (wchar_t c : want) printf("%04X ", (unsigned)c);
        printf("\"\n");
    }
}
static std::wstring PageTextOf(pdftext::Session& s, int page) {
    std::wstring out;
    const pdftext::PageText& pg = s.Page(page);
    for (const pdftext::Letter& L : pg.letters) out += L.text;
    return out;
}

static void TestFile(const std::string& dir, const std::string& name,
                     const std::wstring& want, const char* what) {
    printf("- %s\n", name.c_str());
    FILE* f = fopen((dir + "/" + name).c_str(), "rb");
    if (!f) { Expect(false, "open the test file"); return; }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + got);
    fclose(f);
    pdftext::Session s;
    Expect(s.Open(std::move(bytes)), "the file parses (xref, catalog, page tree)");
    Expect((int)s.pages.size() == 1, "one page found");
    if (s.pages.empty()) return;
    ExpectText(PageTextOf(s, 0) == want, what, PageTextOf(s, 0), want);
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "scripts/testdata";

    printf("engine tests:\n");
    TestFile(dir, "simple_winansi.pdf", L"Résumë Hàllo",
             "WinAnsiEncoding maps the accented Latin codes");
    TestFile(dir, "differences.pdf", L"résumç",
             "Differences glyph names (eacute, ccedilla) resolve through the AGL table");
    TestFile(dir, "arabic_identity.pdf", L"\u0633\u0644\u0627\u0645\u06A9\u0628",
             "Identity-H + ToUnicode bfchar produces the Arabic and Persian letters");
    TestFile(dir, "latin_cyrillic.pdf", L"ÉÀÇéèçПривет",
             "bfrange UTF-16BE destinations produce French accents and Cyrillic");
    TestFile(dir, "flate_content.pdf", L"Hello Flate",
             "FlateDecode content streams decode");
    TestFile(dir, "objstm_xrefstream.pdf", L"Inside ObjStm",
             "cross-reference streams and object streams resolve");
    TestFile(dir, "form_xobject.pdf", L"BeforeIn formAfter",
             "form XObjects run with their own resources and matrix");
    TestFile(dir, "actualtext.pdf", L"PlainABCTail",
             "BDC ActualText replaces the marked span's letters");
    TestFile(dir, "timestest.pdf", L"TheQuickBrown",
             "TJ positioning and two fonts on one page");

    printf("- rotated.pdf\n");
    {
        FILE* f = fopen((dir + "/rotated.pdf").c_str(), "rb");
        Expect(f != nullptr, "open rotated.pdf");
        if (f) {
            std::vector<uint8_t> bytes;
            uint8_t buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + got);
            fclose(f);
            pdftext::Session s;
            Expect(s.Open(std::move(bytes)), "rotated.pdf parses");
            if (!s.pages.empty()) {
                const pdftext::PageText& pg = s.Page(0);
                Expect(pg.rotation == 90, "the page's /Rotate 90 carries through");
                Expect(pg.letters.size() == 1, "one letter on the rotated page");
                if (pg.letters.size() == 1) {
                    // the letter's box: baseline 700, advance ~0.556*24=13.3pt wide
                    double l = pg.letters[0].l, b = pg.letters[0].b, r = pg.letters[0].r, t = pg.letters[0].t;
                    Expect(l > 95 && l < 105 && b > 695 && b < 705, "PDF-space box sits at (100, 700)");
                    Expect(r > l && t > b, "the box is a box");
                    // rendered space: 90 degrees clockwise on a 612x792 page
                    double rx1, ry1, rx2, ry2;
                    pdftext::RotateToRender(90, 612, 792, l, b, rx1, ry1);
                    pdftext::RotateToRender(90, 612, 792, r, t, rx2, ry2);
                    double rl = std::min(rx1, rx2), rr = std::max(rx1, rx2);
                    double rt = std::min(ry1, ry2), rb = std::max(ry1, ry2);
                    // (100, 695..719) with /Rotate 90 renders at x ~ 792-719=695..792-695=697? no:
                    // x' = 792 - Y where Y = 792 - y -> x' = y (695..719), y' = x (100..113)
                    Expect(rl > 690 && rr < 725 && rt > 95 && rb < 120,
                           "the box lands at the rotated position (top-left origin)");
                }
            }
        }
    }

    printf("- arabic search shape\n");
    {
        // KillerPDF searches the logical order: the line an RTL reader sees is
        // the reversed cell stream, so FdMatch's haystack (built visually) must
        // be flipped per line. The engine's letters arrive in content order,
        // which is logical order for an RTL run - assert that here.
        FILE* f = fopen((dir + "/arabic_identity.pdf").c_str(), "rb");
        if (f) {
            std::vector<uint8_t> bytes;
            uint8_t buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + got);
            fclose(f);
            pdftext::Session s;
            if (s.Open(std::move(bytes)) && !s.pages.empty()) {
                const pdftext::PageText& pg = s.Page(0);
                Expect(pg.letters.size() == 6, "six letters extracted");
                if (pg.letters.size() == 6) {
                    std::wstring logical = PageTextOf(s, 0);
                    Expect(logical.find(L"\u0633\u0644\u0627\u0645") == 0,
                           "the letters stream in logical (content) order");
                }
            }
        }
    }

    printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
