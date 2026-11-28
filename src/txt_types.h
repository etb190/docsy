// ---------------------------------------------------------------------------
// txt_types.h - the flowing text selection's value types, shared by two
// compilers: common.h (the app, after the Win32 headers) and
// scripts/text_check.cpp (the Linux harness, which compiles the pure
// selection core out of pdf_render.inl against these same structs - no
// Windows headers may appear here).
// ---------------------------------------------------------------------------
#ifndef DOCSY_TXT_TYPES_H
#define DOCSY_TXT_TYPES_H

#include <vector>
#include <string>

// ---- flowing text selection (port of KillerPDF's TextRunService + #127) ----
// A page's selectable units - the PDF text layer's own letters, read by the
// KillerPDF-engine port in pdf_text.inl (all languages the file carries:
// ToUnicode CMaps, standard encodings, Identity-H composites), with the OS
// OCR's per-word boxes as the fallback for scanned pages - are grouped into
// visual lines by vertical band overlap, read in column-aware order, and a
// drag tracks the run of units between two carets (page, index) in that
// reading order. Geometry lives in page-DIP space with the top-left origin
// the render uses, so painting is the same scale the page bitmap already
// uses. Empty page areas are dead to the mouse - the wheel and the keys own
// scrolling, so a grab on the page always means selection, never a scroll.
struct TxtCell {                      // one selectable unit: a character sliced out
    std::wstring text;                // of the text layer's letters (pdf_text.inl) or,
                                      // on scanned pages, of an OCR word box
                                      // (TtSplitChars cuts word boxes into
                                      // KillerPDF's letter-granular cells)
    float l = 0, r = 0, t = 0, b = 0;
    int word = 0, line = 0;           // the word ordinal (renumbered in reading
                                      // order by TtLines) and the line's index
};
struct TxtLine {                      // a contiguous slice of the flattened cells
    int start = 0, count = 0;
    float top = 0, bottom = 0, left = 0, right = 0;
    bool rtl = false;                 // the line reads right-to-left (KillerPDF's
                                      // RunLine.RightToLeft): its words and letters
                                      // stream right-to-left in logical order, so
                                      // carets clamp mirrored
    int end() const { return start + count; }
};
struct TxtPage {
    float w = 0, h = 0;               // page size in DIPs
    unsigned long long tick = 0;      // LRU stamp
    bool parked = false;              // a pump's placeholder, not a build: the AI
                                      // index and the summary extraction park empty
                                      // entries here to mark "OCR in flight" - TtFor
                                      // must rebuild a parked page on touch instead
                                      // of serving the empty shell as the page's
                                      // final truth (selection went dead mid-session
                                      // when a pump crossed the page first)
    std::vector<TxtCell> cells;
    std::vector<TxtLine> lines;
};
struct FRect { float x, y, w, h; };   // selection quad in page DIPs

#endif // DOCSY_TXT_TYPES_H
