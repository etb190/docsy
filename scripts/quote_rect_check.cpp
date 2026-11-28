// ---------------------------------------------------------------------------
// quote_rect_check.cpp - the Linux harness for docsy's word-accurate citation
// highlight (AiQuoteRects, the pure core in src/ai_ui2.inl) and the quote
// locator it rides on (AiLocateWords + the Arabic skeleton, src/ai_index.inl).
//
// Compiles the matcher straight out of the real sources (extracted verbatim
// by sed - the real functions, not a copy) beside the two blocks it stands
// on: the selection core's TtRects (src/pdf_render.inl) and the index's
// tokenizer AiWordize (src/ai_index.inl). Asserts what the user's eyes see
// when a sidechat source chip is clicked: the highlight covers exactly the
// quoted words - one slim rect per quoted line, RTL included - not the
// chunk's page-span blob, a quote the model reshaped a little still finds
// its passage, the model's PLAIN Arabic copy lands on the page's DECORATED
// words (harakat, hamza carriers, alef maqsura), a needle of common words
// that scatters across the page invents nothing (the span guard is what
// killed the second blob), and a passage that walks over a page break
// lights both pages. The geometric floor under it all gets its own section:
// AiSpanLineRects - the cited chunk's span box intersected with the page's
// live cells - paints one slim rect per cited line with no text matching at
// all, so a quote reshaped past every matcher still lights the passage it
// came from, and the union-box blob stays dead on any page words can carry.
//
//   sed -n '/text selection core (verbatim-portable geometry)/,/text selection app glue/p' src/pdf_render.inl > /tmp/qsel.inc
//   sed -n '/word normalization (QuoteLocator/,/^\/\/ ---- the chunk model ----/p' src/ai_index.inl > /tmp/qtok.inc
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

    printf("AiLocateWords + the skeleton (the model's plain quote vs the page's decorated words):\n");

    // 10. the page's words ride decorated (harakat), the model's quote rides
    //     plain: the skeleton run carries the quote onto the words
    {
        PageBuilder p5;
        // "المَادَة" (al-mādah with fatha) then "الخامسة" plain
        const wchar_t* l0[] = {
            L"\x0627\x0644\x0645\x064E\x0627\x062F\x064E\x0629",
            L"\x0627\x0644\x062E\x0627\x0645\x0633\x0629" };
        p5.Line(l0, 2, 500, 100, 114, 0, 5, 6, true);
        ok = AiQuoteRects(p5.pt,
                          "\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9 "
                          "\xD8\xA7\xD9\x84\xD8\xAE\xD8\xA7\xD9\x85\xD8\xB3\xD8\xA9",
                          rects);
        Expect(ok && rects.size() == 1, "a plain quote lands on a diacritic-carrying word");
        if (rects.size() == 1) {
            // the whole line's two words: from the left word's left rim to 500
            Expect(Near(R0(rects[0]), 500 - 8 * 5 - 6 - 7 * 5, 0.5f),
                   "the rect starts at the decorated first word");
            Expect(Near(R1(rects[0]), 500.f, 0.5f), "and ends at the line's right rim");
        }
    }

    // 11. alef spelled with hamza on the page, plain in the quote; ya spelled
    //     alef maqsura on the page, plain ya in the quote
    {
        PageBuilder p6;
        // "أحكام" (hamza alef) "على" (alef maqsura) "الموظف"
        const wchar_t* l0[] = {
            L"\x0623\x062D\x0643\x0627\x0645",
            L"\x0639\x0644\x0649",
            L"\x0627\x0644\x0645\x0648\x0638\x0641" };
        p6.Line(l0, 3, 500, 100, 114, 0, 5, 6, true);
        // the model's copy: "احكام علي الموظف" (plain alef, plain ya)
        ok = AiQuoteRects(p6.pt,
                          "\xD8\xA7\xD8\xAD\xD9\x83\xD8\xA7\xD9\x85 "
                          "\xD8\xB9\xD9\x84\xD9\x8A "
                          "\xD8\xA7\xD9\x84\xD9\x85\xD9\x88\xD8\xB8\xD9\x81",
                          rects);
        Expect(ok && rects.size() == 1,
               "hamza-carrier and maqsura spellings fold to one skeleton");
    }

    // 12. a reshaped quote (the model dropped/reworded a word mid-passage):
    //     the tight subsequence finds the window, the glue word rides inside
    {
        PageBuilder p7;
        const wchar_t* l0[] = { L"the", L"court", L"shall", L"appoint", L"a",
                                L"guardian", L"for", L"the", L"minor" };
        p7.Line(l0, 9, 72, 100, 112, 0);
        ok = AiQuoteRects(p7.pt, "court shall appoint guardian", rects);
        Expect(ok && rects.size() == 1, "a reshaped quote still finds its passage");
        if (rects.size() == 1) {
            Expect(Near(R0(rects[0]), 72 + 3 * 5 + 6, 0.5f), "starts at 'court'");
            Expect(Near(R1(rects[0]), 72 + (3 + 5 + 5 + 7 + 1 + 8) * 5 + 5 * 6, 0.5f),
                   "ends at 'guardian' (the glue word 'a' rides inside)");
        }
    }

    // 13. the blob killer: a needle of common words that appears in ORDER but
    //     scattered across the line must invent nothing - the unguarded walk
    //     lit everything between the farthest two matches (the second blob:
    //     of at 0, the at 9, and at 17, in at 29 - every in-order chain needs
    //     a gap the guard refuses, and no chain reaches four words)
    {
        PageBuilder p8;
        const wchar_t* l0[] = { L"of", L"heirs", L"estate", L"property", L"share",
                                L"estate", L"court", L"guardian", L"heirs", L"the",
                                L"minor", L"his", L"share", L"estate", L"under",
                                L"court", L"heirs", L"and", L"guardian", L"heirs",
                                L"minor", L"share", L"estate", L"under", L"heirs",
                                L"court", L"heirs", L"heirs", L"heirs", L"in" };
        p8.Line(l0, 30, 72, 100, 112, 0);
        ok = AiQuoteRects(p8.pt, "of the and in", rects);
        Expect(!ok && rects.empty(),
               "a scattered common-word needle invents nothing (gap/span guards)");
    }

    // 14. the passage that walks over a page break: no single page carries
    //     it, the joined two-page stream does - each page lights its own half
    {
        PageBuilder pA, pB;
        const wchar_t* a0[] = { L"the", L"royal", L"decree", L"names", L"the",
                                L"guardian", L"of" };
        pA.Line(a0, 7, 72, 100, 112, 0);
        const wchar_t* b0[] = { L"the", L"minor", L"and", L"his", L"estate" };
        pB.Line(b0, 5, 72, 100, 112, 0);
        AiPageToks t1, t2;
        AiPageTokenize(pA.pt, t1);
        AiPageTokenize(pB.pt, t2);
        size_t n1 = t1.toks.size();
        Expect(n1 == 7, "page one tokenizes into its own words");
        std::vector<std::string> toks = t1.toks;
        toks.insert(toks.end(), t2.toks.begin(), t2.toks.end());
        std::vector<std::string> needle;
        AiWordize("guardian of the minor", needle);
        size_t ws = 0, we = 0;
        Expect(AiLocateWords(toks, needle, ws, we) == AQH_EXACT,
               "the joined stream carries the break-crossing quote verbatim");
        Expect(ws < n1 && we > n1, "the window truly walks over the break");
        if (ws < n1 && we > n1) {
            std::vector<FRect> ra, rb;
            // the window crosses the seam: ws < n1, so page two's half starts
            // at its own first token (0 - never ws - n1, which underflows)
            TtRects(pA.pt, t1.a[ws], t1.b[n1 - 1] + 1, ra);
            TtRects(pB.pt, t2.a[0], t2.b[we - 1 - n1] + 1, rb);
            Expect(ra.size() == 1 && rb.size() == 1,
                   "each page cuts its own half into one slim rect");
            if (ra.size() == 1 && rb.size() == 1) {
                Expect(Near(R0(ra[0]), 72 + (3 + 5 + 6 + 5 + 3) * 5 + 5 * 6, 0.5f),
                       "page one's rect starts at 'guardian'");
                Expect(Near(R1(ra[0]), 72 + (3 + 5 + 6 + 5 + 3 + 8 + 2) * 5 + 6 * 6, 0.5f),
                       "and runs to the line's end at 'of'");
                Expect(Near(R0(rb[0]), 72.f, 0.5f), "page two's rect starts at 'the'");
                Expect(Near(R1(rb[0]), 72 + (3 + 5) * 5 + 1 * 6, 0.5f), "and ends at 'minor'");
            }
        }
    }

    // 15. a needle longer than the stream cannot fit, and a one-word page
    //     stream with a long quote invents nothing
    {
        PageBuilder p9;
        const wchar_t* l0[] = { L"alpha" };
        p9.Line(l0, 1, 72, 100, 112, 0);
        ok = AiQuoteRects(p9.pt, "alpha beta gamma delta epsilon", rects);
        Expect(!ok && rects.empty(), "a needle longer than the page matches nothing");
    }

    // 16. the chunk locator's own upgrade: a decorated chunk text against the
    //     model's plain quote locates - and the window's END is the last
    //     matched word (the old pass B wrote first+1)
    {
        std::string chunk =
            "\xD8\xA7\xD9\x84\xD9\x85\xD9\x8E\xD8\xA7\xD8\xAF\xD9\x8E\xD8\xA9 "   // المَادَة
            "\xD8\xA7\xD9\x84\xD8\xA3\xD8\xAE\xD9\x8A\xD8\xB1\xD8\xA9 "          // الأخيرة
            "\xD8\xAA\xD9\x86\xD8\xB5\xD9\x91 "                                    // تنصّ
            "\xD8\xB9\xD9\x84\xD9\x89 "                                            // على
            "\xD8\xA7\xD9\x84\xD9\x88\xD8\xB5\xD9\x8A\xD9\x91\xD8\xA9";            // الوصيّة
        size_t ws = 0, we = 0;
        AiQuoteHit hit = AiLocateInText(chunk,
            "\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9 "                    // المادة
            "\xD8\xA7\xD9\x84\xD8\xA7\xD8\xAE\xD9\x8A\xD8\xB1\xD8\xA9 "            // الاخيرة
            "\xD8\xAA\xD9\x86\xD8\xB5 "                                            // تنص
            "\xD8\xB9\xD9\x84\xD9\x8A "                                            // علي
            "\xD8\xA7\xD9\x84\xD9\x88\xD8\xB5\xD9\x8A\xD8\xA9", ws, we);           // الوصية
        Expect(hit == AQH_EXACT, "a plain quote locates inside a decorated chunk");
        Expect(we - ws == 5, "the window spans all five words (the real end)");
    }

    // 17. AiSegmentHead cuts on a UTF-8 lead byte, never mid-codepoint
    {
        std::string seg;
        for (int i = 0; i < 60; i++)
            seg += "\xD8\xA7\xD9\x84\xD9\x85\xD8\xA7\xD8\xAF\xD8\xA9 ";   // "المادة "
        std::string head = AiSegmentHead(seg);
        bool clean = true;
        for (size_t i = 0; i < head.size(); ) {
            int len = 0;
            AiUtf8Cp(head, i, len);
            if (len <= 0 || i + (size_t)len > head.size()) { clean = false; break; }
            i += (size_t)len;
        }
        Expect(clean && head.size() <= 120 && !head.empty(),
               "the segment head never snaps a letter in half");
    }

    // ---- AiSpanLineRects: the geometric floor (the span box's own words) ----
    printf("AiSpanLineRects (the citation highlight's geometric floor):\n");

    // 18. a span over two whole lines: one slim rect per line, each clipped to
    //     the span's x-range - the words the chunk carries, never the union box
    {
        PageBuilder p10;
        const wchar_t* l0[] = { L"the", L"cited", L"chunk", L"opens" };
        const wchar_t* l1[] = { L"here", L"with", L"its", L"own", L"words" };
        const wchar_t* l2[] = { L"and", L"strays", L"far", L"below", L"it" };
        p10.Line(l0, 4, 72, 100, 112, 0);
        p10.Line(l1, 5, 72, 130, 142, 1);
        p10.Line(l2, 5, 72, 160, 172, 2);
        // the span's union box over lines 0-1 whole (x 72..206, y 98..144) -
        // what the box would paint is one 46-tall slab; the floor paints each
        // line's own extent instead
        FRect span{ 72.f, 98.f, 134.f, 46.f };
        std::vector<FRect> sr;
        AiSpanLineRects(p10.pt, span, sr);
        Expect(sr.size() == 2, "a two-line span paints one rect per cited line");
        if (sr.size() == 2) {
            Expect(Near(sr[0].y, 100 - 1.44f, 0.05f), "the first rect sits on line 0");
            Expect(Near(sr[1].y, 130 - 1.44f, 0.05f), "the second rect sits on line 1");
            Expect(Near(sr[0].w, 108.f, 0.5f),
                   "line 0's rect is the line's own words, not the span's width");
            Expect(Near(sr[1].w, 119.f, 0.5f), "line 1's rect is its own words too");
            Expect(sr[0].h < 20.f, "each rect is slim - never the union slab");
        }
        // the third line rides outside the span: nothing lights there
        bool third = false;
        for (auto& r : sr) if (r.y > 150.f && r.y < 180.f) third = true;
        Expect(!third, "the line below the span stays dark");
    }

    // 19. a span riding two mid-line words: the rect covers exactly those
    //     words, not the line's start, not the words past the span's edge
    {
        PageBuilder p11;
        const wchar_t* l0[] = { L"aaa", L"bbb", L"ccc", L"ddd", L"eee" };
        p11.Line(l0, 5, 72, 100, 112, 0);   // words at 72,93,114,135,156 (+15 wide, 6 gap)
        FRect span{ 92.f, 99.f, 40.f, 14.f };   // x 92-132: 'bbb' (93-108), 'ccc' (114-129)
        std::vector<FRect> sr;
        AiSpanLineRects(p11.pt, span, sr);
        Expect(sr.size() == 1, "a two-word span paints one rect");
        if (sr.size() == 1) {
            Expect(Near(R0(sr[0]), 93.f, 0.5f),
                   "the rect starts at the span's first word ('bbb')");
            Expect(Near(R1(sr[0]), 129.f, 0.5f), "and ends at the span's last word ('ccc')");
        }
    }

    // 20. the neighbor column stays out: a two-column page whose span covers
    //     only the right column lights only the right column's words
    {
        PageBuilder p12;
        const wchar_t* la[] = { L"left1", L"left2" };
        const wchar_t* lb[] = { L"right1", L"right2" };
        p12.Line(la, 2, 72, 100, 112, 0);          // left column line 0
        p12.Line(lb, 2, 400, 100, 112, 1);         // right column, same band
        FRect span{ 400.f, 98.f, 60.f, 16.f };     // the right column only
        std::vector<FRect> sr;
        AiSpanLineRects(p12.pt, span, sr);
        Expect(sr.size() == 1, "a one-column span paints one rect");
        if (sr.size() == 1)
            Expect(Near(R0(sr[0]), 400.f, 0.5f), "the left column's words stay dark");
    }

    // 21. the degenerate and the missing: a zero-area span and a span that
    //     misses every line invent nothing
    {
        PageBuilder p13;
        const wchar_t* l0[] = { L"alpha", L"beta" };
        p13.Line(l0, 2, 72, 100, 112, 0);
        FRect zero{ 0.f, 0.f, 0.f, 0.f };
        std::vector<FRect> sr;
        AiSpanLineRects(p13.pt, zero, sr);
        Expect(sr.empty(), "a zero-area span paints nothing");
        FRect miss{ 72.f, 500.f, 100.f, 10.f };
        AiSpanLineRects(p13.pt, miss, sr);
        Expect(sr.empty(), "a span below every line paints nothing");
        FRect wide{ 0.f, 0.f, 1000.f, 400.f };     // the whole page: every line
        AiSpanLineRects(p13.pt, wide, sr);
        Expect(sr.size() == 1, "a page-wide span paints the page's lines whole");
    }

    // 22. two cited lines of different lengths under one wide span: each
    //     line's rect keeps its own extent - a shorter line must not inherit
    //     the longer one's right edge (the per-line range scan is per line)
    {
        PageBuilder p14;
        const wchar_t* l0[] = { L"aa", L"bb", L"cc", L"dd", L"ee", L"ff", L"gg", L"hh", L"ii", L"jj" };
        const wchar_t* l1[] = { L"aa", L"bb", L"cc" };
        p14.Line(l0, 10, 72, 100, 112, 0);   // words every 16pt: ends at 226
        p14.Line(l1, 3, 72, 130, 142, 1);    // ends at 114
        FRect span{ 72.f, 98.f, 154.f, 46.f };   // covers both lines whole
        std::vector<FRect> sr;
        AiSpanLineRects(p14.pt, span, sr);
        Expect(sr.size() == 2, "the wide span still paints one rect per line");
        if (sr.size() == 2) {
            Expect(Near(sr[0].w, 154.f, 0.5f), "the long line keeps its full extent");
            Expect(Near(sr[1].w, 42.f, 0.5f),
                   "the short line ends at its own last word, not the long line's");
        }
    }

    printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
