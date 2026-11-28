// ---------------------------------------------------------------------------
// quote_rect_check.cpp - the Linux harness for docsy's word-accurate citation
// highlight (AiQuoteRects, the pure core in src/ai_ui2.inl).
//
// Compiles the matcher straight out of the real sources (extracted verbatim
// by sed - the real functions, not a copy) beside the two blocks it stands
// on: the selection core's TtRects (src/pdf_render.inl) and the index's
// tokenizer AiWordize (src/ai_index.inl). Asserts what the user's eyes see
// when a sidechat source chip is clicked: the highlight covers exactly the
// quoted words - one slim rect per quoted line, RTL included - not the
// chunk's page-span blob, and a quote the model reshaped a little still
// finds its passage.
//
//   sed -n '/text selection core (verbatim-portable geometry)/,/text selection app glue/p' src/pdf_render.inl > /tmp/qsel.inc
//   sed -n '/word normalization (QuoteLocator/,/^\/\/ LocateInChunk/p' src/ai_index.inl > /tmp/qtok.inc
//   sed -n "/the citation's word-accurate highlight/,/the Windows half/p" src/ai_ui2.inl > /tmp/qcore.inc
//   g++ -std=c++20 -O1 -I src -I /tmp -o /tmp/quote_rect_check scripts/quote_rect_check.cpp
//   /tmp/quote_rect_check
// ---------------------------------------------------------------------------
#include "txt_types.h"

#include <cstdio>
#include <cctype>
#include <cwctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>

// The cores are written against windows.h's min/max macros; on Linux the same
// names ride in as templates (every call passes two same-type arguments).
template <class T> static T min(T a, T b) { return a < b ? a : b; }
template <class T> static T max(T a, T b) { return a < b ? b : a; }

// WxU's Linux twin (extensions.inl's WideCharToMultiByte): wchar_t is 4 bytes
// here, so the conversion rides UCS-4 -> UTF-8, surrogates never appear.
static std::string WxU(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) {
        unsigned u = (unsigned)c;
        if (u < 0x80) out += (char)u;
        else if (u < 0x800) {
            out += (char)(0xC0 | (u >> 6));
            out += (char)(0x80 | (u & 63));
        } else if (u < 0x10000) {
            out += (char)(0xE0 | (u >> 12));
            out += (char)(0x80 | ((u >> 6) & 63));
            out += (char)(0x80 | (u & 63));
        } else {
            out += (char)(0xF0 | (u >> 18));
            out += (char)(0x80 | ((u >> 12) & 63));
            out += (char)(0x80 | ((u >> 6) & 63));
            out += (char)(0x80 | (u & 63));
        }
    }
    return out;
}

#include "qsel.inc"
#include "qtok.inc"
#include "qcore.inc"

// ------------------------------------------------------------------ tests --
static int g_failed = 0, g_total = 0;

static void Expect(bool ok, const char* what) {
    g_total++;
    if (!ok) { g_failed++; printf("  FAIL: %s\n", what); }
    else printf("  ok: %s\n", what);
}
static bool Near(float a, float b, float eps) { return a > b - eps && a < b + eps; }

// a page builder that mirrors what TtLines hands the matcher: letter-granular
// cells of word ordinals in reading order, one TxtLine per visual line
struct PageBuilder {
    TxtPage pt;
    int word = 0;
    void Line(const wchar_t* const* words, int n, float x0, float yTop,
              float yBot, int lineIdx, float letterW = 5.f, float spaceW = 6.f,
              bool rtl = false) {
        float x = x0;                          // LTR: the pen's left edge; RTL:
        int start = (int)pt.cells.size();      // the right edge of the next cell
        for (int i = 0; i < n; i++) {
            std::wstring t = words[i];
            for (int k = 0; k < (int)t.size(); k++) {
                TxtCell c;
                c.text = std::wstring(1, t[k]);
                if (rtl) { c.l = x - letterW; c.r = x; }   // Arabic writes rightward
                else { c.l = x; c.r = x + letterW; }       // into the page's left rim
                c.t = yTop; c.b = yBot;
                c.word = word; c.line = lineIdx;
                pt.cells.push_back(c);
                if (rtl) x -= letterW; else x += letterW;
            }
            word++;
            if (i + 1 < n) {
                if (rtl) x -= spaceW; else x += spaceW;
            }
        }
        TxtLine ln;
        ln.start = start; ln.count = (int)pt.cells.size() - start;
        ln.top = yTop; ln.bottom = yBot;
        ln.left = rtl ? x : x0; ln.right = rtl ? x0 : x;
        ln.rtl = rtl;
        pt.lines.push_back(ln);
    }
};

static float R0(const FRect& r) { return r.x; }
static float R1(const FRect& r) { return r.x + r.w; }

int main() {
    printf("AiQuoteRects (the citation highlight's word-accurate road):\n");

    // ---- page: three LTR lines (the law's opening, words as letter cells) ----
    PageBuilder b;
    {
        const wchar_t* l0[] = { L"The", L"royal", L"decree", L"establishes" };
        const wchar_t* l1[] = { L"the", L"powers", L"of", L"the", L"minister" };
        const wchar_t* l2[] = { L"of", L"labor", L"within", L"the", L"kingdom" };
        b.Line(l0, 4, 72, 100, 112, 0);
        b.Line(l1, 5, 72, 130, 142, 1);
        b.Line(l2, 5, 72, 160, 172, 2);
    }
    std::vector<FRect> rects;

    // 1. a single-line quote: one slim rect over exactly the quoted words
    bool ok = AiQuoteRects(b.pt, "royal decree establishes", rects);
    Expect(ok && rects.size() == 1, "a one-line quote yields one rect");
    if (rects.size() == 1) {
        // "royal" starts after "The " (3 letters + 1 space) from x=72
        Expect(Near(R0(rects[0]), 72 + 3 * 5 + 6, 0.5f), "the rect starts at the first quoted word");
        Expect(Near(R1(rects[0]), 72 + 3 * 5 + 6 + (5 + 6 + 11) * 5 + 2 * 6, 0.5f),
               "the rect ends at the last quoted word");
        Expect(Near(rects[0].y, 100 - 1.44f, 0.05f) &&
               Near(rects[0].y + rects[0].h, 112 + 1.44f, 0.05f),
               "the rect rides the quoted line's band (12% pad)");
        Expect(rects[0].w < 200.f, "the rect is the passage, not the chunk blob");
    }

    // 2. a quote that wraps: two rects, one per quoted line, each clipped to
    //    the words quoted on that line
    ok = AiQuoteRects(b.pt, "decree establishes the powers", rects);
    Expect(ok && rects.size() == 2, "a two-line quote yields one rect per line");
    if (rects.size() == 2) {
        Expect(Near(rects[0].y, 100 - 1.44f, 0.05f), "the first rect sits on line 0");
        Expect(Near(rects[1].y, 130 - 1.44f, 0.05f), "the second rect sits on line 1");
        Expect(Near(R0(rects[0]), 72 + (3 + 5) * 5 + 2 * 6, 0.5f), "line 0's rect starts at 'decree'");
        Expect(Near(R1(rects[1]), 72 + (3 + 6) * 5 + 1 * 6, 0.5f), "line 1's rect ends at 'powers'");
    }

    // 3. pass B: the quote as an in-order subsequence - the model skipped a
    //    word the page carries and the passage still lights up
    ok = AiQuoteRects(b.pt, "labor within kingdom", rects);
    Expect(ok && rects.size() == 1, "a reshaped quote (page word skipped) still matches");
    if (rects.size() == 1) {
        // from "labor" (after "of ") to the line's end at "kingdom"
        Expect(Near(R0(rects[0]), 72 + 2 * 5 + 6, 0.5f), "the subsequence rect starts at 'labor'");
        Expect(Near(R1(rects[0]), 72 + 2 * 5 + 6 + (5 + 6 + 3 + 7) * 5 + 3 * 6, 0.5f),
               "and runs through 'kingdom'");
    }

    // 4. no match: nothing highlighted, no rect invented
    ok = AiQuoteRects(b.pt, "quantum entanglement protocol", rects);
    Expect(!ok && rects.empty(), "an absent quote matches nothing");

    // 5. the first exact run wins over a later scattered one
    {
        PageBuilder p2;
        const wchar_t* l0[] = { L"alpha", L"beta", L"gamma", L"beta", L"delta" };
        p2.Line(l0, 5, 72, 100, 112, 0);
        ok = AiQuoteRects(p2.pt, "beta gamma beta", rects);
        Expect(ok && rects.size() == 1, "a repeated needle still yields one rect");
        if (rects.size() == 1)
            Expect(Near(R0(rects[0]), 72 + 5 * 5 + 6, 0.5f),
                   "the exact run at its first home is preferred");
    }

    // 6. the RTL page: Arabic lines in reading order, cells walking leftward
    {
        PageBuilder p3;
        // "المادة الخامسة" (article five) then "من القانون" (of the law)
        const wchar_t* l0[] = { L"\x0627\x0644\x0645\x0627\x062F\x0629",
                                L"\x0627\x0644\x062E\x0627\x0645\x0633\x0629" };
        const wchar_t* l1[] = { L"\x0645\x0646", L"\x0627\x0644\x0642\x0627\x0646\x0648\x0646" };
        p3.Line(l0, 2, 500, 100, 114, 0, 5, 6, true);
        p3.Line(l1, 2, 500, 130, 144, 1, 5, 6, true);
        // the quote the model would lift, verbatim off the page
        ok = AiQuoteRects(p3.pt,
                          "\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9 "
                          "\xD8\xA7\xD9\x84\xD8\xAE\xD8\xA7\xD9\x85\xD8\xB3\xD8\xA9",
                          rects);
        Expect(ok && rects.size() == 1, "an Arabic quote lights its own words");
        if (rects.size() == 1) {
            // the line reads right-to-left: the FIRST word sits at the RIGHT
            Expect(Near(R1(rects[0]), 500.f, 0.5f), "the RTL rect ends at the line's right rim");
            Expect(Near(rects[0].y, 100 - 1.68f, 0.05f) &&
                   Near(rects[0].y + rects[0].h, 114 + 1.68f, 0.05f),
                   "the RTL rect rides the line's band");
        }
        // the two-line Arabic quote: "المادة الخامسة من القانون"
        ok = AiQuoteRects(p3.pt,
                          "\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9 "
                          "\xD8\xA7\xD9\x84\xD8\xAE\xD8\xA7\xD9\x85\xD8\xB3\xD8\xA9 "
                          "\xD8\xA7\xD9\x84\xD9\x82\xD8\xA7\xD9\x86\xD9\x88\xD9\x86",
                          rects);
        Expect(ok && rects.size() == 2, "an Arabic quote across two lines yields two rects");
        if (rects.size() == 2)
            Expect(Near(rects[1].y, 130 - 1.68f, 0.05f), "the second rect sits on line 1");
    }

    // 7. a word wearing a glued Arabic comma: the quote names the word, the
    //    highlight covers the word whole (the comma's cells included)
    {
        PageBuilder p4;
        const wchar_t* l0[] = { L"\x0627\x0644\x0645\x0627\x062F\x0629\x060C",
                                L"\x0627\x0644\x062E\x0627\x0645\x0633\x0629" };
        p4.Line(l0, 2, 500, 100, 114, 0, 5, 6, true);
        ok = AiQuoteRects(p4.pt,
                          "\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9", rects);
        Expect(ok && rects.size() == 1, "a quote word before a glued comma still lands");
        if (rects.size() == 1)
            Expect(Near(R0(rects[0]), 500 - 7 * 5, 0.5f),
                   "the rect covers the whole word, comma cells included");
    }

    // 8. OCR-style page: one whole-word cell per word (the scanned road
    //    before TtSplitChars), word ordinals 1:1
    {
        TxtPage pt; pt.w = 612; pt.h = 792;
        const wchar_t* words[] = { L"Signed", L"in", L"Rabat", L"the", L"first" };
        float x = 72;
        for (int i = 0; i < 5; i++) {
            TxtCell c;
            c.text = words[i];
            c.l = x; c.r = x + (float)wcslen(words[i]) * 5;
            c.t = 100; c.b = 112;
            c.word = i; c.line = 0;
            pt.cells.push_back(c);
            x = c.r + 6;
        }
        TxtLine ln; ln.start = 0; ln.count = 5;
        ln.top = 100; ln.bottom = 112; ln.left = 72; ln.right = x;
        pt.lines.push_back(ln);
        ok = AiQuoteRects(pt, "in Rabat", rects);
        Expect(ok && rects.size() == 1, "whole-word OCR cells match the same way");
        if (rects.size() == 1) {
            Expect(Near(R0(rects[0]), 72 + 6 * 5 + 6, 0.5f), "the rect starts at 'in'");
            Expect(Near(R1(rects[0]), 72 + 6 * 5 + 6 + (2 + 5) * 5 + 1 * 6, 0.5f), "and ends at 'Rabat'");
        }
    }

    // 9. guards: empty page, empty quote
    {
        TxtPage empty;
        Expect(!AiQuoteRects(empty, "anything", rects) && rects.empty(),
               "an empty page matches nothing");
        Expect(!AiQuoteRects(b.pt, "", rects) && rects.empty(),
               "an empty quote matches nothing");
    }

    printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
