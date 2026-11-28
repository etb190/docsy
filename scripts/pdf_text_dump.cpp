// ---------------------------------------------------------------------------
// pdf_text_dump.cpp - dump docsy's per-page text for a real PDF, BOTH halves:
// the engine's raw letters (pdf_text.inl) and the app's final text (the
// selection core's TtLines + TtText, the same pipeline TtNativeBuild feeds).
//   g++ -std=c++20 -O1 -I src -I /tmp -o /tmp/pdf_text_dump scripts/pdf_text_dump.cpp
//   sed -n '/text selection core (verbatim-portable geometry)/,/text selection app glue/p' src/pdf_render.inl > /tmp/sel_core.inc
//   /tmp/pdf_text_dump file.pdf [first] [last]
// ---------------------------------------------------------------------------
#define DOCSY_PDFTEXT_TEST
#include "pdf_text.inl"

#include <cstdio>
#include <cstdlib>
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

#include "txt_types.h"
#include <cwctype>
#include <cfloat>
#include <algorithm>
#include <unordered_map>

template <class T> static T min(T a, T b) { return a < b ? a : b; }
template <class T> static T max(T a, T b) { return a < b ? b : a; }

#include "sel_core.inc"

static void PrintWide(const std::wstring& out) {
    for (wchar_t c : out) {
        unsigned cp = (unsigned)c;
        if (cp >= 0xFFFD) { printf("<%04X>", cp); continue; }
        if (cp < 0x80) putchar((char)cp);
        else if (cp < 0x800) { putchar(0xC0 | (cp >> 6)); putchar(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { putchar(0xE0 | (cp >> 12)); putchar(0x80 | ((cp >> 6) & 0x3F)); putchar(0x80 | (cp & 0x3F)); }
        else { putchar(0xF0 | (cp >> 18)); putchar(0x80 | ((cp >> 12) & 0x3F)); putchar(0x80 | ((cp >> 6) & 0x3F)); putchar(0x80 | (cp & 0x3F)); }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: pdf_text_dump file.pdf [first last]\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + got);
    fclose(f);
    pdftext::Session s;
    if (!s.Open(std::move(bytes))) { printf("OPEN FAILED\n"); return 1; }
    int first = argc > 2 ? atoi(argv[2]) : 0;
    int last = argc > 3 ? atoi(argv[3]) : (int)s.pages.size() - 1;
    printf("pages=%d\n", (int)s.pages.size());
    for (int p = first; p <= last && p < (int)s.pages.size(); p++) {
        const pdftext::PageText& pg = s.Page(p);
        printf("\n=== page %d letters=%zu fffd=%d ===\n", p + 1, pg.letters.size(), pg.fffd);
        printf("--- raw (engine order) ---\n");
        for (const pdftext::Letter& L : pg.letters) PrintWide(L.text);
        printf("\n--- final (TtLines + TtText, what the app reads) ---\n");
        // TtNativeBuild's cell conversion, verbatim: whitespace letters out,
        // rotation into render space, boxes kept.
        TxtPage pt; pt.w = (float)pg.boxW; pt.h = (float)pg.boxH;
        int rot = ((pg.rotation % 360) + 360) % 360;
        if (rot % 90) rot = (int)(((rot + 45) / 90) * 90) % 360;
        for (const pdftext::Letter& L : pg.letters) {
            if (L.word == 0) continue;
            double rx1, ry1, rx2, ry2;
            pdftext::RotateToRender(rot, pg.boxW, pg.boxH, L.l, L.b, rx1, ry1);
            pdftext::RotateToRender(rot, pg.boxW, pg.boxH, L.r, L.t, rx2, ry2);
            TxtCell c;
            c.text = L.text;
            c.l = (float)(std::min(rx1, rx2)); c.r = (float)(std::max(rx1, rx2));
            c.t = (float)(std::min(ry1, ry2)); c.b = (float)(std::max(ry1, ry2));
            c.word = L.word;
            pt.cells.push_back(std::move(c));
        }
        TtLines(pt);
        std::wstring text = pt.cells.empty() ? std::wstring() : TtText(pt, 0, (int)pt.cells.size(), nullptr);
        PrintWide(text);
        printf("\n");
    }
    return 0;
}
