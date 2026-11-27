// ---------------------------------------------------------------------------
// text_check.cpp - the Linux harness for docsy's flowing text selection.
//
// Compiles the pure selection core straight out of src/pdf_render.inl (the
// block between the "text selection core" and "text selection app glue"
// markers, extracted verbatim by sed - the real functions, not a copy) against
// the shared value types in src/txt_types.h, and asserts the geometry the
// user's hands actually feel: the RTL caret clamp that decides whether an
// Arabic drag tracks the letters or pins to the line's first cell, the
// direction-aware word ordering that decides what a copy reads like, the OCR
// word-box slicing, and the untouched LTR behavior.
//
//   sed -n '/text selection core (verbatim-portable geometry)/,/text selection app glue/p' src/pdf_render.inl > /tmp/sel_core.inc
//   g++ -std=c++20 -O1 -I src -I /tmp -o /tmp/text_check scripts/text_check.cpp
//   /tmp/text_check
// ---------------------------------------------------------------------------
#include "txt_types.h"

#include <cstdio>
#include <cwctype>
#include <cfloat>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>

// The core is written against windows.h's min/max macros; on Linux the same
// names ride in as templates (every call passes two same-type arguments).
template <class T> static T min(T a, T b) { return a < b ? a : b; }
template <class T> static T max(T a, T b) { return a < b ? b : a; }

#include "sel_core.inc"

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

static TxtCell C(std::wstring t, float l, float r, int word) {
    TxtCell c;
    c.text = std::move(t);
    c.l = l; c.r = r; c.t = 100; c.b = 112;
    c.word = word;
    return c;
}
static TxtLine LN(int start, int count, float l, float r, bool rtl) {
    TxtLine ln;
    ln.start = start; ln.count = count;
    ln.top = 100; ln.bottom = 112; ln.left = l; ln.right = r;
    ln.rtl = rtl;
    return ln;
}

int main() {
    printf("RTL caret (the branch that decides whether an Arabic drag tracks):\n");
    {
        // one RTL line, five cells streaming right-to-left in logical order
        // (what TtLines' flatten produces): word 0 = cells 0-2 (right),
        // word 1 = cells 3-4 (left)
        TxtPage pt; pt.w = 612; pt.h = 792;
        pt.cells = { C(L"\u0627", 107, 110, 0), C(L"\u0644", 104, 107, 0),
                     C(L"\u0645", 101, 104, 0), C(L"\u0646", 98, 101, 1),
                     C(L"\u0639", 95, 98, 1) };
        pt.lines.push_back(LN(0, 5, 95, 110, true));

        Expect(TtCaret(pt, 120, 106) == 0, "past the right edge clamps to the line START");
        Expect(TtCaret(pt, 109.5, 106) == 0, "the rightmost cell's right half: caret at its start");
        Expect(TtCaret(pt, 105.6, 106) == 1, "past the first cell's midpoint the caret advances one");
        Expect(TtCaret(pt, 100, 106) == 3, "a mid-line point lands between the cells around it");
        Expect(TtCaret(pt, 96, 106) == 5, "near the left edge: caret at the line END");
        Expect(TtCaret(pt, 50, 106) == 5, "past the left edge clamps to the line END");
        // above the first line and left of it selects from the page start, below
        // the last and right of it to the page end (KillerPDF's shortcuts want the
        // point beyond the line's extent too - a point inside the line targets it)
        Expect(TtCaret(pt, 50, 90) == 0, "above the first line and left of it: page start");
        Expect(TtCaret(pt, 150, 130) == 5, "below the last line and right of it: page end");

        int words = 0;
        std::wstring t = TtText(pt, 0, 5, &words);
        ExpectText(t == L"\u0627\u0644\u0645 \u0646\u0639", "the copy reads in logical order, one space",
                   t, L"\u0627\u0644\u0645 \u0646\u0639");
        Expect(words == 2, "two distinct words");
        Expect(t.find(L"  ") == std::wstring::npos, "no doubled space anywhere");

        int s = 0, e = 0;
        Expect(TtWordRange(pt, 108.5, 106, s, e) && s == 0 && e == 3,
               "double-click on the first letter takes its whole word");
        Expect(TtWordRange(pt, 99, 106, s, e) && s == 3 && e == 5,
               "double-click on the left word takes that word");

        std::vector<FRect> rs;
        TtRects(pt, 1, 3, rs);
        Expect(rs.size() == 1 && rs[0].x == 101 && rs[0].w == 6,
               "a highlight quad spans min..max regardless of direction");
    }

    printf("TtLines flatten (words and letters sort by the line's direction):\n");
    {
        // a real visual-RTL file: letters advance +x, content order = draw
        // order, so each word's LOGICAL first letter is its RIGHTMOST glyph.
        // Word ids come from the engine's GroupWords.
        // left word  "سلام": drawn left-to-right as م ا ل س, logical س ل ا م
        // right word "علي":  drawn left-to-right as ي ل ع,  logical ع ل ي
        TxtPage pt; pt.w = 612; pt.h = 792;
        pt.cells = { C(L"\u0645", 100, 103, 1), C(L"\u0627", 103, 106, 1),
                     C(L"\u0644", 106, 109, 1), C(L"\u0633", 109, 112, 1),
                     C(L"\u064A", 116, 119, 2), C(L"\u0644", 119, 122, 2),
                     C(L"\u0639", 122, 125, 2) };
        TtLines(pt);
        Expect(pt.lines.size() == 1, "one visual line");
        Expect(!pt.lines.empty() && pt.lines[0].rtl, "the line reads right-to-left");
        std::wstring t = TtText(pt, 0, (int)pt.cells.size(), nullptr);
        ExpectText(t == L"\u0639\u0644\u064A \u0633\u0644\u0627\u0645",
                   "the copy reads logically: right word first, letters right-to-left",
                   t, L"\u0639\u0644\u064A \u0633\u0644\u0627\u0645");
        // a search for the logical word finds it in the flattened stream
        std::vector<FRect> rs;
        Expect(FdMatch(pt, L"\u0639\u0644\u064A", false, false, rs) == 1,
               "the logical Arabic word is findable");
        Expect(rs.size() == 1, "one word box highlights the match");
    }

    printf("digits and parentheses in an RTL line (the single-space shape):\n");
    {
        // content (draw) order left to right: '(' then the date digits '1442'
        // stored visually, ')' then an Arabic word. Every piece is its own
        // GroupWords word - exactly the shapes real Moroccan legal files carry.
        TxtPage pt; pt.w = 612; pt.h = 792;
        pt.cells = { C(L"(", 20, 23, 3),
                     C(L"1", 23, 26, 1), C(L"4", 26, 29, 1),
                     C(L"4", 29, 32, 1), C(L"2", 32, 35, 1),
                     C(L")", 35, 38, 2),
                     C(L"\u064A", 38, 41, 4), C(L"\u0644", 41, 44, 4),
                     C(L"\u0639", 44, 47, 4) };
        TtLines(pt);
        Expect(!pt.lines.empty() && pt.lines[0].rtl, "Arabic letters make the line RTL");
        std::wstring t = TtText(pt, 0, (int)pt.cells.size(), nullptr);
        ExpectText(t == L"\u0639\u0644\u064A ) 2441 (",
                   "digits copy as KillerPDF's, parens are separate words, one space each",
                   t, L"\u0639\u0644\u064A ) 2441 (");
        Expect(t.find(L"  ") == std::wstring::npos, "no doubled space around the parens");
    }

    printf("LTR behavior (regression):\n");
    {
        TxtPage pt; pt.w = 612; pt.h = 792;
        pt.cells = { C(L"H", 10, 15, 1), C(L"e", 15, 20, 1), C(L"l", 20, 25, 1),
                     C(L"l", 25, 30, 1), C(L"o", 30, 35, 1),
                     C(L"W", 40, 45, 2), C(L"o", 45, 50, 2), C(L"r", 50, 55, 2),
                     C(L"l", 55, 60, 2), C(L"d", 60, 65, 2) };
        TtLines(pt);
        Expect(pt.lines.size() == 1 && !pt.lines[0].rtl, "an English line stays LTR");
        std::wstring t = TtText(pt, 0, (int)pt.cells.size(), nullptr);
        ExpectText(t == L"Hello World", "the copy is unchanged", t, L"Hello World");
        Expect(TtCaret(pt, 17, 106) == 1, "a click between two letters splits there");
        Expect(TtCaret(pt, 8, 106) == 0, "past the left edge: line start");
        Expect(TtCaret(pt, 70, 106) == 10, "past the right edge: line end");
    }

    printf("OCR word boxes slice right-to-left on an RTL line:\n");
    {
        // one OCR box, whole-word text in logical order - TtSplitChars tiles
        // the letters evenly, the logical first letter on the RIGHTMOST tile
        TxtPage pt; pt.w = 612; pt.h = 792;
        pt.cells = { C(L"\u0643\u0644\u0645\u0629", 50, 70, 0) };
        pt.lines.push_back(LN(0, 1, 50, 70, true));
        TtSplitChars(pt);
        Expect(pt.cells.size() == 4, "four letter cells");
        Expect(!pt.cells.empty() && pt.cells[0].text == L"\u0643" && pt.cells[0].l == 65.0f,
               "the logical first letter lands on the rightmost tile");
        std::wstring t = TtText(pt, 0, 4, nullptr);
        ExpectText(t == L"\u0643\u0644\u0645\u0629", "the word still reads logically",
                   t, L"\u0643\u0644\u0645\u0629");
        Expect(TtCaret(pt, 65, 106) == 1,
               "a click in the first letter's left half puts the caret after it");
        // and an OCR English box tiles left-to-right as before
        TxtPage en; en.w = 612; en.h = 792;
        en.cells = { C(L"cat", 50, 65, 0) };
        en.lines.push_back(LN(0, 1, 50, 65, false));
        TtSplitChars(en);
        Expect(en.cells.size() == 3 && en.cells[0].text == L"c" && en.cells[0].l == 50,
               "LTR tiles stay left-to-right");
    }

    printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
