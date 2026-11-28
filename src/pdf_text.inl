// ---------------------------------------------------------------------------
// pdf_text.inl - KillerPDF's engine text layer, ported to docsy.
//
// KillerPDF detects text with its own PDF engine: content streams are parsed
// (the full text state machine: BT/ET, Tf, Tj/TJ/'/", Td/TD/Tm/T*, Tc, Tw, Tz,
// TL, Ts, q/Q/cm, BMC/BDC/EMC with ActualText), every glyph code is decoded to
// Unicode through the font's ToUnicode CMap (bfchar / bfrange, one to four byte
// code spaces, UTF-16BE destinations) with the standard encodings and Adobe
// glyph names underneath, and each decoded letter keeps its transformed bounds.
// Nothing in that pipeline is language-specific: Arabic, French, Cyrillic,
// Greek, Hebrew, CJK - whatever the PDF's text layer holds - comes out as the
// real Unicode string, exactly the text search, copy and selection need.
//
// docsy renders with Windows.Data.Pdf, which exposes NO text API at all (only
// RenderToStreamAsync - checked against the shipped API surface), so docsy's
// selection and search used to run entirely on OS OCR and only saw the OCR
// pack's languages. This file ports KillerPDF's extraction logic to plain C++
// over the same bytes docsy already opens: object parser and xref (classic
// table, cross-reference streams, object streams, and the recovery scan for
// damaged files), the stream filters, the content tokenizer and text
// interpreter, the ToUnicode CMap parser, the CID/Identity-H composite fonts,
// the simple-font encodings with Differences, and the Type 3 FontMatrix rule.
// The OCR pass stays exactly where it was, as the fallback for scanned pages -
// KillerPDF's own architecture (engine text layer first, OCR underneath).
//
// Two reductions against the engine are deliberate and marked inline: no
// predefined shift-CID CMap data (90ms-R31, RKSJ, GBK-EUC - those pages score
// above the U+FFFD gate and take the OCR fallback), and no Core-14 AFM widths
// (a width-less simple font draws boxes on a 556/1000 default advance).
//
// The engine half of this file is pure std:: and is compiled standalone on
// Linux by scripts/pdf_text_check.cpp against synthetic documents
// (DOCSY_PDFTEXT_TEST); only the small glue at the end touches Win32.
// ---------------------------------------------------------------------------
#ifndef DOCSY_PDF_TEXT_INL
#define DOCSY_PDF_TEXT_INL

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <tuple>
#include <algorithm>
#include <utility>

#ifdef DOCSY_PDFTEXT_TEST
// The test harness ships its own DEFLATE core under crxk's name.
namespace crxk { bool Inflate(const unsigned char* src, size_t n,
                              std::vector<unsigned char>& out, size_t limit); }
#else
// docsy's inflate lives in crx_unpack.inl, included after this file in
// main.cpp - one translation unit, so a matching static declaration here
// resolves to that definition.
namespace crxk { static bool Inflate(const unsigned char* src, size_t n,
                                     std::vector<unsigned char>& out, size_t limit); }
#endif

#include "pdf_text_tables.inc"

namespace pdftext {

// KillerPDF's limits (PdfContentStreamReader.MaximumSourceBytes and the
// extractors' character, mapping and expansion caps).
constexpr size_t MaximumSourceBytes = 64ull * 1024 * 1024;
constexpr int MaximumLetters = 1000000;
constexpr int MaximumInstructions = 1000000;
constexpr int MaximumOperands = 4096;
constexpr int MaximumMappings = 65536;
constexpr int MaximumFormDepth = 16;
constexpr int MaximumFormExpansions = 64;
constexpr int MaximumTreeDepth = 64;
constexpr int MaximumStateStack = 256;
constexpr double DefaultAscent = 800, DefaultDescent = -200;

struct PdfError {};   // any structural surprise: the page falls back to OCR

// ---------------------------------------------------------------- objects --
struct PdfObj {
    enum T { NIL, BOOL, INT, REAL, STR, NAME, KEYWORD, ARR, DICT, REF, STREAM } t = NIL;
    bool b = false;
    long long i = 0;
    double d = 0;
    std::vector<uint8_t> bytes;        // STR payload
    std::string name;                  // NAME (decoded, no leading '/') or KEYWORD
    std::vector<PdfObj> arr;
    std::vector<std::pair<std::string, PdfObj>> dict;
    // STREAM: the dictionary plus where the raw bytes start, so the cache never
    // holds every stream's body - data is fetched from the file on demand.
    size_t dataOffset = 0, dataLen = 0;
    int Find(const char* key) const {
        for (size_t k = 0; k < dict.size(); k++)
            if (dict[k].first == key) return (int)k;
        return -1;
    }
    const PdfObj* Get(const char* key) const {
        int k = Find(key);
        return k < 0 ? nullptr : &dict[k].second;
    }
};

static bool IsWhite(uint8_t c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
static bool IsDelim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}
static bool IsDigit(uint8_t c) { return c >= '0' && c <= '9'; }
static bool IsHexC(uint8_t c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int HexVal(uint8_t c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

// PDF numbers are plain: sign, digits, optional fraction, optional exponent.
// Parsed by hand so no CRT locale can move the decimal point under us.
static double ParseDouble(const char* s, size_t n) {
    double out = 0; size_t k = 0; bool neg = false;
    if (k < n && (s[k] == '+' || s[k] == '-')) { neg = s[k] == '-'; k++; }
    for (; k < n && IsDigit((uint8_t)s[k]); k++) out = out * 10 + (s[k] - '0');
    if (k < n && s[k] == '.') {
        k++;
        double frac = 0.1;
        for (; k < n && IsDigit((uint8_t)s[k]); k++, frac *= 0.1) out += (s[k] - '0') * frac;
    }
    if (k < n && (s[k] == 'e' || s[k] == 'E')) {
        k++;
        bool eneg = false;
        if (k < n && (s[k] == '+' || s[k] == '-')) { eneg = s[k] == '-'; k++; }
        int e = 0;
        for (; k < n && IsDigit((uint8_t)s[k]); k++) e = e * 10 + (s[k] - '0');
        double mul = 1;
        for (int x = 0; x < e && x < 300; x++) mul *= eneg ? 0.1 : 10.0;
        out *= mul;
    }
    return neg ? -out : out;
}

// ------------------------------------------------------------------ lexer --
struct Lexer {
    const uint8_t* p = nullptr, *end = nullptr;

    void SkipWhite() {
        while (p < end) {
            if (IsWhite(*p)) { p++; continue; }
            if (*p == '%') { while (p < end && *p != 10 && *p != 13) p++; continue; }
            break;
        }
    }
    bool EatDelim(const char* kw) {          // keyword must end at a delimiter
        size_t n = strlen(kw);
        if (p + n > end || memcmp(p, kw, n) != 0) return false;
        if (p + n < end && !IsWhite(p[n]) && !IsDelim(p[n])) return false;
        p += n;
        return true;
    }
    // One PDF token: object syntax (numbers, names, strings, arrays, dicts,
    // references) or a bare keyword. Returns false at end of input; on an
    // unbalanced delimiter it skips the byte so no caller can spin in place.
    bool Token(PdfObj& out) {
        SkipWhite();
        if (p >= end) return false;
        uint8_t c = *p;
        if (IsDigit(c) || c == '+' || c == '-') {
            const uint8_t* s = p;
            p++;
            bool real = false;
            while (p < end && (IsDigit(*p) || *p == '.' || *p == 'e' || *p == 'E' ||
                               ((*p == '+' || *p == '-') && (p[-1] == 'e' || p[-1] == 'E')))) {
                if (*p == '.') real = true;
                p++;
            }
            std::string num((const char*)s, p - s);
            out = PdfObj();
            if (!real) { out.t = PdfObj::INT; out.i = strtoll(num.c_str(), nullptr, 10); }
            else { out.t = PdfObj::REAL; out.d = ParseDouble(num.c_str(), num.size()); }
            return true;
        }
        if (c == '/') {
            p++;
            std::string name;
            while (p < end && !IsWhite(*p) && !IsDelim(*p)) {
                if (*p == '#' && p + 2 < end && IsHexC(p[1]) && IsHexC(p[2])) {
                    name += (char)((HexVal(p[1]) << 4) | HexVal(p[2]));
                    p += 3;
                } else name += (char)*p++;
            }
            out = PdfObj(); out.t = PdfObj::NAME; out.name = std::move(name);
            return true;
        }
        if (c == '(') {
            out = PdfObj(); out.t = PdfObj::STR;
            p++;
            ReadLiteral(out.bytes);
            return true;
        }
        if (c == '<') {
            p++;
            if (p < end && *p == '<') {          // dictionary
                p++;
                out = PdfObj(); out.t = PdfObj::DICT;
                while (true) {
                    SkipWhite();
                    if (p + 1 < end && p[0] == '>' && p[1] == '>') { p += 2; return true; }
                    if (p >= end) return true;   // damaged: take what parsed
                    if (*p != '/') return false; // not a name: caller resyncs
                    PdfObj key, value;
                    if (!Token(key)) return false;
                    if (!ParseValue(value)) value = PdfObj();
                    out.dict.push_back({ std::move(key.name), std::move(value) });
                }
            }
            out = PdfObj(); out.t = PdfObj::STR;
            ReadHex(out.bytes);
            return true;
        }
        if (c == '[') {
            p++;
            out = PdfObj(); out.t = PdfObj::ARR;
            while (true) {
                SkipWhite();
                if (p >= end) return true;
                if (*p == ']') { p++; return true; }
                PdfObj item;
                if (!ParseValue(item)) return false;
                out.arr.push_back(std::move(item));
            }
        }
        if (c == ']' || c == '>' || c == '}' || c == ')') { p++; return false; }
        const uint8_t* s = p;                    // keyword
        while (p < end && !IsWhite(*p) && !IsDelim(*p)) p++;
        out = PdfObj(); out.t = PdfObj::KEYWORD; out.name.assign((const char*)s, p - s);
        return true;
    }
    // A complete value: keyword true/false/null, or a reference "n g R".
    bool ParseValue(PdfObj& out) {
        if (!Token(out)) return false;
        if (out.t == PdfObj::KEYWORD) {
            if (out.name == "true") { out = PdfObj(); out.t = PdfObj::BOOL; out.b = true; }
            else if (out.name == "false") { out = PdfObj(); out.t = PdfObj::BOOL; }
            else if (out.name == "null") out = PdfObj();
            return true;
        }
        if (out.t != PdfObj::INT) return true;
        PdfObj first = out;
        const uint8_t* save = p;
        PdfObj gen, r;
        if (!Token(gen) || gen.t != PdfObj::INT) { p = save; out = first; return true; }
        if (Token(r) && r.t == PdfObj::KEYWORD && r.name == "R") {
            out = PdfObj(); out.t = PdfObj::REF; out.i = first.i;
            return true;
        }
        p = save;                                    // not a reference: the integers stay
        out = first;
        return true;
    }
    void ReadLiteral(std::vector<uint8_t>& out) {
        int depth = 1;                           // the opening '(' is consumed
        while (p < end) {
            uint8_t c = *p++;
            if (c == '\\') {
                if (p >= end) break;
                uint8_t e = *p++;
                switch (e) {
                    case 'n': out.push_back(10); break;
                    case 'r': out.push_back(13); break;
                    case 't': out.push_back(9); break;
                    case 'b': out.push_back(8); break;
                    case 'f': out.push_back(12); break;
                    case '(': out.push_back('('); break;
                    case ')': out.push_back(')'); break;
                    case '\\': out.push_back('\\'); break;
                    default:
                        if (e >= '0' && e <= '7') {          // one to three octal digits
                            int v = e - '0', k = 1;
                            while (k < 3 && p < end && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; }
                            out.push_back((uint8_t)v);
                        } else if (e == 10) { /* line continuation */ }
                        else if (e == 13) { if (p < end && *p == 10) p++; }
                        else out.push_back(e);
                }
                continue;
            }
            if (c == '(') { depth++; out.push_back(c); continue; }
            if (c == ')') { depth--; if (!depth) return; out.push_back(c); continue; }
            out.push_back(c);
        }
    }
    void ReadHex(std::vector<uint8_t>& out) {
        int nib = -1;
        while (p < end) {
            uint8_t c = *p++;
            if (c == '>') break;
            if (IsWhite(c)) continue;
            if (!IsHexC(c)) { nib = -1; continue; }
            int v = HexVal(c);
            if (nib < 0) nib = v;
            else { out.push_back((uint8_t)((nib << 4) | v)); nib = -1; }
        }
        if (nib >= 0) out.push_back((uint8_t)(nib << 4));    // odd length pads 0
    }
};

// ------------------------------------------------------------------- file --
struct PdfFile {
    std::vector<uint8_t> bytes;
    std::map<size_t, PdfObj> cache;             // "n g obj" offset -> object
    std::map<int, size_t> offsets;              // object number -> file offset
    std::map<int, std::pair<int, int>> stmIndex; // objnum -> (objstm num, index)
    std::map<int, std::vector<uint8_t>> objstm; // decoded object streams
    std::map<std::pair<int, int>, PdfObj*> stmObjs;   // (objstm, index) -> parsed:
                                                      // the memo one object stream
                                                      // entry deserves - without it
                                                      // every Resolve of an ObjStm-
                                                      // resident object parses fresh,
                                                      // and every fresh pointer misses
                                                      // the font cache (keyed by dict
                                                      // address), so a page whose /F1
                                                      // fires 300 times re-reads the
                                                      // font dict and re-inflates its
                                                      // ToUnicode 300 times. KillerPDF
                                                      // keeps these reader-level too.
    std::vector<std::unique_ptr<PdfObj>> anon;  // objects parsed out of streams
    std::vector<std::unique_ptr<PdfObj>> trailerStore;
    bool encrypted = false, recovered = false, valid = false;
    const PdfObj* root = nullptr;

    bool Open(std::vector<uint8_t>&& data) {
        bytes = std::move(data);
        if (bytes.size() < 8) return false;
        if (!ReadXref() || offsets.empty()) { recovered = true; ScanObjects(); }
        root = Resolve(root);
        return valid = root != nullptr;
    }

    // ---- xref: classic table, cross-reference stream, /Prev chain, hybrid ----
    bool ReadXref() {
        size_t sx = FindLast("startxref", 4096);
        if (sx == (size_t)-1) return false;
        Lexer lx{ bytes.data() + sx + 9, bytes.data() + bytes.size() };
        lx.SkipWhite();
        PdfObj n;
        if (!lx.Token(n) || n.t != PdfObj::INT) return false;
        long long at = n.i;
        if (at <= 0 || (size_t)at >= bytes.size()) return false;
        std::map<int, size_t> found;
        const PdfObj* trailer = nullptr;
        int hops = 0;
        while (at > 0 && (size_t)at < bytes.size() && hops++ < 64) {
            const PdfObj* t = ReadXrefAt((size_t)at, found);
            if (!t) break;
            if (!trailer) trailer = t;
            // Hybrid files (the common "incremental update moved objects into
            // object streams" shape): this section's /XRefStm supplements the
            // classic table and is read BEFORE the /Prev chain, so its entries
            // win over the older revisions' offsets - the update's locations
            // are the current ones. First-wins recording does the rest.
            const PdfObj* xs = ResolveIn(t, "XRefStm");
            if (xs && xs->t == PdfObj::INT && xs->i > 0 && (size_t)xs->i < bytes.size())
                ReadXrefAt((size_t)xs->i, found);
            const PdfObj* prev = ResolveIn(t, "Prev");
            if (!prev || prev->t != PdfObj::INT || prev->i <= 0) break;
            at = prev->i;
        }
        if (!trailer) return false;
        offsets = std::move(found);
        if (!encrypted) {
            const PdfObj* enc = ResolveIn(trailer, "Encrypt");
            encrypted = enc != nullptr && enc->t != PdfObj::NIL;
        }
        root = ResolveIn(trailer, "Root");
        return root != nullptr;
    }

    size_t FindLast(const char* kw, size_t window) {
        size_t n = strlen(kw), from = bytes.size() > window ? bytes.size() - window : 0;
        for (size_t i = bytes.size(); i-- > from;) {
            if (i + n > bytes.size()) continue;
            if (memcmp(bytes.data() + i, kw, n) == 0 &&
                (i == 0 || IsWhite(bytes[i - 1])))
                return i;
        }
        return (size_t)-1;
    }

    // One xref table section or one cross-reference stream at `at`. Later
    // sections (via /Prev) never override an already-found object, which is
    // what a newer revision's entries winning looks like reversed.
    const PdfObj* ReadXrefAt(size_t at, std::map<int, size_t>& found) {
        if (at >= bytes.size()) return nullptr;
        Lexer lx{ bytes.data() + at, bytes.data() + bytes.size() };
        lx.SkipWhite();
        if (lx.EatDelim("xref")) {               // classic table
            while (true) {
                lx.SkipWhite();
                if (lx.EatDelim("trailer")) break;
                PdfObj first, count;
                if (!lx.Token(first) || first.t != PdfObj::INT) return nullptr;
                if (!lx.Token(count) || count.t != PdfObj::INT) return nullptr;
                // Entries read as tokens, not fixed 20-byte rows: the spec pins
                // the width, but real producers end the header line with \r\n,
                // \n or " \n", and a fixed walk that skips only one whitespace
                // byte shifts every entry and loses the whole table (seen on a
                // real government file). Token parsing survives every EOL.
                for (long long k = 0; k < count.i; k++) {
                    lx.SkipWhite();
                    PdfObj off, gen, type;
                    if (!lx.Token(off) || off.t != PdfObj::INT) return nullptr;
                    if (!lx.Token(gen) || gen.t != PdfObj::INT) return nullptr;
                    if (!lx.Token(type) || type.t != PdfObj::KEYWORD) return nullptr;
                    if (type.name != "n") continue;             // 'f' = free
                    long long num = first.i + k;
                    if (num >= 0 && num < 100000000 && !found.count((int)num) &&
                        off.i > 0 && (size_t)off.i < bytes.size())
                        found[(int)num] = (size_t)off.i;
                }
            }
            lx.SkipWhite();
            PdfObj dict;
            if (!lx.ParseValue(dict) || dict.t != PdfObj::DICT) return nullptr;
            trailerStore.push_back(std::make_unique<PdfObj>(std::move(dict)));
            return trailerStore.back().get();
        }
        // cross-reference stream: the object that lives at `at`
        PdfObj* obj = ParseObjAt(at);
        if (!obj || obj->t != PdfObj::STREAM) return nullptr;
        const PdfObj* w = ResolveIn(obj, "W");
        if (!w || w->t != PdfObj::ARR || w->arr.size() < 3) return nullptr;
        size_t w1 = ColNum(w, 0), w2 = ColNum(w, 1), w3 = ColNum(w, 2);
        if (!w1 || !w3 || w1 > 8 || w2 > 8 || w3 > 8) return nullptr;
        std::vector<uint8_t> data;
        if (!StreamData(obj, data) || data.empty()) return nullptr;
        long long size = 0;
        if (const PdfObj* sz = ResolveIn(obj, "Size")) size = sz->i;
        std::vector<std::pair<long long, long long>> spans;
        const PdfObj* index = ResolveIn(obj, "Index");
        if (index && index->t == PdfObj::ARR) {
            for (size_t k = 0; k + 1 < index->arr.size(); k += 2) {
                const PdfObj* a = Resolve(&index->arr[k]);
                const PdfObj* b = Resolve(&index->arr[k + 1]);
                if (a && a->t == PdfObj::INT && b && b->t == PdfObj::INT)
                    spans.push_back({ a->i, b->i });
            }
        } else spans.push_back({ 0, size });
        size_t row = w1 + w2 + w3, pos = 0;
        for (auto& sp : spans)
            for (long long k = 0; k < sp.second && pos + row <= data.size(); k++, pos += row) {
                const uint8_t* r = data.data() + pos;
                long long type = Col(r, w1), f2 = Col(r + w1, w2), f3 = Col(r + w1 + w2, w3);
                long long num = sp.first + k;
                if (num < 0 || num >= 100000000) continue;
                if (type == 1) {
                    if (!found.count((int)num) && f2 > 0 && (size_t)f2 < bytes.size())
                        found[(int)num] = (size_t)f2;
                } else if (type == 2) {                      // inside an object stream
                    if (!stmIndex.count((int)num)) stmIndex[(int)num] = { (int)f2, (int)f3 };
                }
            }
        return obj;
    }
    static long long Col(const uint8_t* r, size_t width) {
        long long v = 0;
        for (size_t k = 0; k < width; k++) v = (v << 8) | r[k];
        return v;
    }
    size_t ColNum(const PdfObj* w, int k) {
        const PdfObj* c = Resolve(&w->arr[k]);
        return c && c->t == PdfObj::INT && c->i >= 0 ? (size_t)c->i : 0;
    }

    // Recovery: every "n g obj" in the file becomes an object (the scan also
    // catches files whose startxref is broken, the way KillerPDF's
    // compatibility recovery does).
    void ScanObjects() {
        recovered = true;
        const char* s = (const char*)bytes.data();
        size_t n = bytes.size();
        for (size_t i = 0; i + 4 < n; i++) {
            if (memcmp(s + i, " obj", 4) != 0) continue;
            // The header is "N G obj": the digits before " obj" are the
            // GENERATION, the number lives before that. Registering the
            // generation would collapse every object into object 0.
            size_t a = i;
            while (a > 0 && IsDigit((uint8_t)s[a - 1])) a--;         // generation digits
            if (a == i) continue;                                    // "N obj": no generation
            while (a > 0 && IsWhite((uint8_t)s[a - 1])) a--;         // the separator
            size_t numStart = a;
            while (numStart > 0 && IsDigit((uint8_t)s[numStart - 1])) numStart--;   // object number
            if (numStart == a) continue;
            long long num = strtoll(s + numStart, nullptr, 10);
            if (num >= 0 && num < 100000000 && !offsets.count((int)num)) offsets[(int)num] = numStart;
        }
        for (size_t i = 0; i + 15 < n; i++) {    // the catalog: first /Type/Catalog
            if (memcmp(s + i, "/Type/Catalog", 13) == 0 || memcmp(s + i, "/Type /Catalog", 14) == 0) {
                for (size_t k = i + 1; k-- > 0;) {
                    if (i - k > 4096) break;
                    if (k + 4 <= n && memcmp(s + k, " obj", 4) == 0) {
                        size_t aa = k;                       // "N G obj": skip the generation
                        while (aa > 0 && IsDigit((uint8_t)s[aa - 1])) aa--;
                        while (aa > 0 && IsWhite((uint8_t)s[aa - 1])) aa--;
                        size_t nn = aa;                      // then the object number
                        while (nn > 0 && IsDigit((uint8_t)s[nn - 1])) nn--;
                        if (nn < aa) {
                            root = ObjByNum((int)strtoll(s + nn, nullptr, 10));
                            if (root) return;
                        }
                    }
                }
            }
        }
    }

    PdfObj* ParseObjAt(size_t at) {
        auto hit = cache.find(at);
        if (hit != cache.end()) return &hit->second;
        if (at + 8 >= bytes.size()) return nullptr;
        Lexer lx{ bytes.data() + at, bytes.data() + bytes.size() };
        lx.SkipWhite();
        PdfObj num, gen, kw;
        if (!lx.Token(num) || num.t != PdfObj::INT) return nullptr;
        if (!lx.Token(gen) || gen.t != PdfObj::INT) return nullptr;
        if (!lx.Token(kw) || !(kw.t == PdfObj::KEYWORD && kw.name == "obj")) return nullptr;
        PdfObj value;
        if (!lx.ParseValue(value)) return nullptr;
        lx.SkipWhite();
        if (lx.EatDelim("stream") && value.t == PdfObj::DICT) {
            value.t = PdfObj::STREAM;
            if (lx.p < lx.end && *lx.p == 13) lx.p++;
            if (lx.p < lx.end && *lx.p == 10) lx.p++;
            value.dataOffset = (size_t)(lx.p - bytes.data());
            long long length = -1;
            const PdfObj* len = value.Get("Length");
            if (len && len->t == PdfObj::REF) len = Resolve(len);   // resolve before caching
            if (len && len->t == PdfObj::INT) length = len->i;
            if (length < 0) {                            // hunt the real endstream
                const char* s = (const char*)bytes.data();
                for (size_t k = value.dataOffset; k + 9 < bytes.size(); k++)
                    if (memcmp(s + k, "endstream", 9) == 0) { length = (long long)(k - value.dataOffset); break; }
                if (length < 0) return nullptr;
                if (length > 0 && bytes[value.dataOffset + length - 1] == 10) length--;
                if (length > 0 && bytes[value.dataOffset + length - 1] == 13) length--;
            }
            if ((unsigned long long)length > bytes.size() - value.dataOffset)
                length = (long long)(bytes.size() - value.dataOffset);
            value.dataLen = (size_t)(length > 0 ? length : 0);
        }
        auto it = cache.emplace(at, std::move(value));
        return &it.first->second;
    }

    const PdfObj* Resolve(const PdfObj* o, int depth = 0) {
        while (o && o->t == PdfObj::REF && depth++ < 32) o = ObjByNum((int)o->i);
        return o && o->t != PdfObj::REF ? o : nullptr;
    }
    const PdfObj* ResolveIn(const PdfObj* dict, const char* key) {
        const PdfObj* v = dict ? dict->Get(key) : nullptr;
        return v ? Resolve(v) : nullptr;
    }
    PdfObj* ObjByNum(int num) {
        auto stm = stmIndex.find(num);
        if (stm != stmIndex.end()) return ObjFromStm(num, stm->second.first, stm->second.second);
        auto off = offsets.find(num);
        if (off == offsets.end()) return nullptr;
        return ParseObjAt(off->second);
    }
    PdfObj* ObjFromStm(int num, int stmNum, int index) {
        auto off = offsets.find(stmNum);
        if (off == offsets.end()) return nullptr;
        PdfObj* stmObj = ParseObjAt(off->second);
        if (!stmObj || stmObj->t != PdfObj::STREAM) return nullptr;
        auto cached = objstm.find(stmNum);
        if (cached == objstm.end()) {
            std::vector<uint8_t> data;
            if (!StreamData(stmObj, data)) return nullptr;
            cached = objstm.emplace(stmNum, std::move(data)).first;
        }
        const PdfObj* n = ResolveIn(stmObj, "N");
        long long first = 0;
        if (const PdfObj* f = ResolveIn(stmObj, "First")) first = f->i;
        if (!n || n->t != PdfObj::INT || index < 0 || index >= n->i) return nullptr;
        const std::vector<uint8_t>& data = cached->second;
        if (first < 0 || (size_t)first > data.size()) return nullptr;
        Lexer lx{ data.data(), data.data() + first };
        for (int k = 0; k <= index; k++) {
            lx.SkipWhite();
            PdfObj a, b;
            if (!lx.Token(a) || a.t != PdfObj::INT) return nullptr;
            if (!lx.Token(b) || b.t != PdfObj::INT) return nullptr;
            if (k == index) {
                if (a.i != num || b.i < 0 || (size_t)(first + b.i) > data.size()) return nullptr;
                auto sk = std::make_pair(stmNum, index);
                auto seen = stmObjs.find(sk);
                if (seen != stmObjs.end()) return seen->second;   // the memo's word
                Lexer body{ data.data() + first + b.i, data.data() + data.size() };
                PdfObj value;
                if (!body.ParseValue(value)) return nullptr;
                anon.push_back(std::make_unique<PdfObj>(std::move(value)));
                stmObjs[sk] = anon.back().get();
                return anon.back().get();
            }
        }
        return nullptr;
    }

    // ---- stream filters: FlateDecode, LZWDecode, ASCIIHexDecode, ASCII85Decode,
    //      RunLengthDecode - the set text extraction meets. DCT and friends
    //      return false, which degrades that one stream, never the page. ----

    // DecodeParms picks its entry by the filter's seat in the chain: a dict
    // belongs to a single filter, an array walks the chain slot by slot.
    const PdfObj* DecodeParmsFor(const PdfObj* stream, size_t index) {
        const PdfObj* parms = ResolveIn(stream, "DecodeParms");
        if (!parms) parms = ResolveIn(stream, "DP");
        if (!parms) return nullptr;
        if (parms->t == PdfObj::DICT) return index == 0 ? parms : nullptr;
        if (parms->t == PdfObj::ARR && index < parms->arr.size()) {
            const PdfObj* e = Resolve(&parms->arr[index]);
            return e && e->t == PdfObj::DICT ? e : nullptr;
        }
        return nullptr;
    }
    static long long ParmInt(const PdfObj* parms, const char* key, long long def) {
        const PdfObj* v = parms ? parms->Get(key) : nullptr;
        if (!v || v->t == PdfObj::NIL) return def;
        if (v->t == PdfObj::INT) return v->i;
        if (v->t == PdfObj::REAL) return (long long)v->d;
        return def;
    }

    // KillerPDF's ReversePredictor: TIFF predictor 2 and the PNG family 10-15,
    // riding FlateDecode and LZWDecode through DecodeParms. Real producers
    // (linearized PDF 1.5 files above all) wrap their cross-reference streams
    // and object streams in /Predictor 12 - without the reversal the rows
    // decode to noise and the whole file fails to open. Recovery-tolerant the
    // way KillerPDF's compatibility mode is: an incomplete tail row truncates,
    // a wrong per-row filter or a filter byte outside 0-4 stops the walk and
    // keeps what decoded.
    static bool ReversePredictor(std::vector<uint8_t>& data, const PdfObj* parms) {
        long long predictor = ParmInt(parms, "Predictor", 1);
        if (predictor == 1) return true;
        long long colors = ParmInt(parms, "Colors", 1);
        long long bpc = ParmInt(parms, "BitsPerComponent", 8);
        long long columns = ParmInt(parms, "Columns", 1);
        if (colors < 1 || columns < 1 || bpc < 1 || bpc > 16) return false;
        if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) return false;
        long long bytesPerPixel = (colors * bpc + 7) / 8;
        long long rowLength = (columns * colors * bpc + 7) / 8;
        if (rowLength <= 0 || rowLength > (1ll << 28)) return false;
        if (predictor == 2) {                       // TIFF: samples accumulate left
            size_t rows = data.size() / (size_t)rowLength;
            if (rows == 0) return false;
            data.resize(rows * (size_t)rowLength);
            if (bpc == 8) {
                for (size_t row = 0; row < data.size(); row += (size_t)rowLength)
                    for (size_t off = row + (size_t)colors; off < row + (size_t)rowLength; off++)
                        data[off] = (uint8_t)(data[off] + data[off - (size_t)colors]);
            } else if (bpc == 16) {
                size_t stride = (size_t)colors * 2;
                for (size_t row = 0; row < data.size(); row += (size_t)rowLength)
                    for (size_t off = row + stride; off + 1 < row + (size_t)rowLength; off += 2) {
                        size_t prev = off - stride;
                        unsigned v = (((unsigned)data[off] << 8) | data[off + 1]) +
                                     (((unsigned)data[prev] << 8) | data[prev + 1]);
                        data[off] = (uint8_t)(v >> 8);
                        data[off + 1] = (uint8_t)v;
                    }
            } else {                                  // packed samples: bit arithmetic
                long long samplesPerRow = colors * columns;
                int mask = (1 << (int)bpc) - 1;
                auto readBit = [&](size_t rowBase, long long bitAt) -> int {
                    size_t byte = rowBase + (size_t)(bitAt / 8);
                    int shift = 7 - (int)(bitAt % 8);
                    return (data[byte] >> shift) & 1;
                };
                auto writeBit = [&](size_t rowBase, long long bitAt, int bit) {
                    size_t byte = rowBase + (size_t)(bitAt / 8);
                    int shift = 7 - (int)(bitAt % 8);
                    uint8_t m = (uint8_t)(1 << shift);
                    if (bit) data[byte] |= m; else data[byte] &= (uint8_t)~m;
                };
                auto readSample = [&](size_t rowBase, long long sample) {
                    int v = 0;
                    for (long long b = 0; b < bpc; b++)
                        v = (v << 1) | readBit(rowBase, sample * bpc + b);
                    return v;
                };
                auto writeSample = [&](size_t rowBase, long long sample, int value) {
                    for (long long b = 0; b < bpc; b++)
                        writeBit(rowBase, sample * bpc + b, (value >> ((int)bpc - (int)b - 1)) & 1);
                };
                for (size_t row = 0; row < data.size(); row += (size_t)rowLength)
                    for (long long s = colors; s < samplesPerRow; s++) {
                        int v = (readSample(row, s) + readSample(row, s - colors)) & mask;
                        writeSample(row, s, v);
                    }
            }
            return true;
        }
        if (predictor < 10 || predictor > 15) return false;
        size_t encodedRow = (size_t)rowLength + 1;
        size_t rows = data.size() / encodedRow;
        if (rows == 0) return false;
        std::vector<uint8_t> decoded(rows * (size_t)rowLength);
        for (size_t row = 0; row < rows; row++) {
            size_t inAt = row * encodedRow, outAt = row * (size_t)rowLength;
            uint8_t filter = data[inAt];
            if (filter > 4) {                       // a garbage tail row: keep what
                if (row == 0) return false;         // decoded before it - recovery
                decoded.resize(outAt);
                break;
            }
            for (long long col = 0; col < rowLength; col++) {
                uint8_t raw = data[inAt + 1 + (size_t)col];
                uint8_t left = col >= bytesPerPixel ? decoded[outAt + (size_t)col - (size_t)bytesPerPixel] : (uint8_t)0;
                uint8_t above = row > 0 ? decoded[outAt - (size_t)rowLength + (size_t)col] : (uint8_t)0;
                uint8_t upperLeft = row > 0 && col >= bytesPerPixel
                    ? decoded[outAt - (size_t)rowLength + (size_t)col - (size_t)bytesPerPixel] : (uint8_t)0;
                uint8_t v;
                switch (filter) {
                case 0: v = raw; break;
                case 1: v = (uint8_t)(raw + left); break;
                case 2: v = (uint8_t)(raw + above); break;
                case 3: v = (uint8_t)(raw + (uint8_t)(((unsigned)left + above) / 2)); break;
                default: {                          // Paeth
                    int p = (int)left + above - upperLeft;
                    int pl = p - left < 0 ? left - p : p - left;
                    int pa = p - above < 0 ? above - p : p - above;
                    int pu = p - upperLeft < 0 ? upperLeft - p : p - upperLeft;
                    uint8_t best = pl <= pa && pl <= pu ? left : (pa <= pu ? above : upperLeft);
                    v = (uint8_t)(raw + best);
                    break;
                }
                }
                decoded[outAt + (size_t)col] = v;
            }
        }
        data = std::move(decoded);
        return !data.empty();
    }

    // KillerPDF's DecodeLzw: the classic Welch LZW with PDF's early-change
    // width bump (EarlyChange 1 bumps one code early, 0 on time). Legacy
    // producers loved it; the interpreter meets it on old Arabic files.
    static bool Lzw(const std::vector<uint8_t>& in, const PdfObj* parms, std::vector<uint8_t>& out) {
        long long early = ParmInt(parms, "EarlyChange", 1);
        if (early != 0 && early != 1) early = 1;
        static const int MaxCodes = 4096;
        std::vector<std::vector<uint8_t>> dict(MaxCodes);
        for (int v = 0; v < 256; v++) dict[v] = { (uint8_t)v };
        size_t bitAt = 0;
        int width = 9, nextCode = 258;
        std::vector<uint8_t> previous;
        auto readCode = [&](int& code) -> bool {
            if (bitAt + (size_t)width > in.size() * 8) return false;
            code = 0;
            for (int b = 0; b < width; b++) {
                size_t abs = bitAt + (size_t)b;
                code = (code << 1) | ((in[abs / 8] >> (7 - (int)(abs % 8))) & 1);
            }
            bitAt += (size_t)width;
            return true;
        };
        for (;;) {
            int code = 0;
            if (!readCode(code)) break;
            if (code == 256) {                    // clear: the table resets
                for (int v = 0; v < MaxCodes; v++) dict[v].clear();
                for (int v = 0; v < 256; v++) dict[v] = { (uint8_t)v };
                width = 9; nextCode = 258; previous.clear();
                continue;
            }
            if (code == 257) break;               // end of data
            std::vector<uint8_t> current;
            if (code < nextCode && !dict[code].empty()) current = dict[code];
            else if (code == nextCode && !previous.empty()) {
                current = previous; current.push_back(previous[0]);
            } else break;                         // invalid code: the tail dies
            if (out.size() + current.size() > MaximumSourceBytes) return false;
            out.insert(out.end(), current.begin(), current.end());
            if (!previous.empty() && nextCode < MaxCodes) {
                dict[nextCode] = previous; dict[nextCode].push_back(current[0]);
                nextCode++;
                if (width < 12 && nextCode == (1 << width) - (int)early) width++;
            }
            previous = std::move(current);
        }
        return true;
    }

    bool StreamData(const PdfObj* stream, std::vector<uint8_t>& out) {
        if (!stream) return false;
        if (stream->t == PdfObj::DICT) {         // an object-stream body, already decoded
            out.assign(stream->bytes.begin(), stream->bytes.end());
            return true;
        }
        if (stream->t != PdfObj::STREAM) return false;
        if (stream->dataOffset + stream->dataLen > bytes.size()) return false;
        const uint8_t* src = bytes.data() + stream->dataOffset;
        const PdfObj* filter = ResolveIn(stream, "Filter");
        if (!filter || filter->t == PdfObj::NIL) {
            out.assign(src, src + stream->dataLen);
            return true;
        }
        std::vector<const PdfObj*> chain;
        if (filter->t == PdfObj::NAME) chain.push_back(filter);
        else if (filter->t == PdfObj::ARR)
            for (const PdfObj& f : filter->arr) chain.push_back(Resolve(&f));
        else return false;
        out.assign(src, src + stream->dataLen);
        for (size_t ci = 0; ci < chain.size(); ci++) {
            const PdfObj* f = chain[ci];
            if (!f || f->t != PdfObj::NAME) return false;
            const PdfObj* parms = DecodeParmsFor(stream, ci);
            std::vector<uint8_t> next;
            if (f->name == "FlateDecode" || f->name == "Fl") {
                if (!Flate(out, next)) return false;
                if (parms && !ReversePredictor(next, parms)) return false;
            } else if (f->name == "LZWDecode" || f->name == "LZW") {
                if (!Lzw(out, parms, next)) return false;
            } else if (f->name == "ASCIIHexDecode" || f->name == "AHx") {
                if (!AsciiHex(out, next)) return false;
            } else if (f->name == "ASCII85Decode" || f->name == "A85") {
                if (!Ascii85(out, next)) return false;
            } else if (f->name == "RunLengthDecode" || f->name == "RL") {
                if (!RunLength(out, next)) return false;
            } else return false;                 // image codecs: this stream skips
            out = std::move(next);
            if (out.size() > MaximumSourceBytes) return false;
        }
        return true;
    }
    static bool Flate(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        if (in.size() >= 2 && (in[0] & 15) == 8 && (((in[0] << 8) | in[1]) % 31) == 0)
            return crxk::Inflate(in.data() + 2, in.size() - 2, out, MaximumSourceBytes);
        return crxk::Inflate(in.data(), in.size(), out, MaximumSourceBytes);   // raw deflate
    }
    static bool AsciiHex(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        int nib = -1;
        for (uint8_t c : in) {
            if (c == '>') break;
            if (IsWhite(c)) continue;
            if (!IsHexC(c)) return false;
            int v = HexVal(c);
            if (nib < 0) nib = v;
            else { out.push_back((uint8_t)((nib << 4) | v)); nib = -1; }
        }
        if (nib >= 0) out.push_back((uint8_t)(nib << 4));
        return true;
    }
    static bool Ascii85(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        unsigned group[5] = { 84, 84, 84, 84, 84 };
        int k = 0;
        for (uint8_t c : in) {
            if (IsWhite(c)) continue;
            if (c == '~') break;
            if (c == 'z' && k == 0) { out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0); continue; }
            if (c < '!' || c > 'u') return false;
            group[k++] = (unsigned)(c - '!');
            if (k < 5) continue;
            unsigned long long v = 0;
            for (int j = 0; j < 5; j++) v = v * 85 + group[j];
            for (int j = 3; j >= 0; j--) out.push_back((uint8_t)(v >> (8 * j)));
            k = 0;
        }
        if (k == 1) return false;
        if (k > 1) {
            for (int j = k; j < 5; j++) group[j] = 84;
            unsigned long long v = 0;
            for (int j = 0; j < 5; j++) v = v * 85 + group[j];
            for (int j = 0; j < k - 1; j++) out.push_back((uint8_t)(v >> (8 * (3 - j))));
        }
        return true;
    }
    static bool RunLength(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        for (size_t i = 0; i < in.size();) {
            int b = in[i++];
            if (b == 128) return true;
            if (b < 128) {
                for (int k = 0; k <= b; k++, i++) {
                    if (i > in.size()) return false;
                    out.push_back(in[i - 1]);
                }
                continue;
            }
            if (i >= in.size()) return false;
            uint8_t v = in[i++];
            for (int k = 0; k < 257 - b; k++) out.push_back(v);
        }
        return true;
    }
};

// ------------------------------------------------------ Unicode conversions --
static std::wstring Utf16be(const uint8_t* s, size_t n) {
    std::wstring out;
    for (size_t k = 0; k + 1 < n; k += 2) out += (wchar_t)((s[k] << 8) | s[k + 1]);
    return out;
}
static void AppendUtf8(std::wstring& out, const uint8_t* s, size_t n) {
    for (size_t k = 0; k < n;) {
        uint8_t c = s[k];
        if (c < 0x80) { out += (wchar_t)c; k++; continue; }
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        unsigned v = c & (len == 4 ? 0x07u : len == 3 ? 0x0Fu : len == 2 ? 0x1Fu : 0x7Fu);
        int got = 1;
        for (int j = 1; j < len && k + j < n && (s[k + j] & 0xC0) == 0x80; j++) { v = (v << 6) | (s[k + j] & 0x3F); got++; }
        k += got;
        if (got < len || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { out += (wchar_t)0xFFFD; continue; }
        if (v > 0xFFFF) {
            v -= 0x10000;
            out += (wchar_t)(0xD800 + (v >> 10));
            out += (wchar_t)(0xDC00 + (v & 0x3FF));
        } else out += (wchar_t)v;
    }
}
// KillerPDF's PdfUnicodeEncoding.DecodeTextString: a UTF-16BE or UTF-8 BOM
// picks the encoding, anything else reads as PDFDocEncoding.
static std::wstring DecodeTextString(const std::vector<uint8_t>& v) {
    if (v.size() >= 2 && v[0] == 0xFE && v[1] == 0xFF) return Utf16be(v.data() + 2, v.size() - 2);
    if (v.size() >= 3 && v[0] == 0xEF && v[1] == 0xBB && v[2] == 0xBF) {
        std::wstring out;
        AppendUtf8(out, v.data() + 3, v.size() - 3);
        return out;
    }
    std::wstring out;
    for (uint8_t c : v) {
        if (c == 0x7F || c == 0x9F || c == 0xAD) continue;   // undefined slots skipped
        switch (c) {                                          // PDFDocEncoding's deviations
            case 0x18: out += L"\u02D8"; break;
            case 0x19: out += L"\u02C7"; break;
            case 0x1A: out += L"\u02C6"; break;
            case 0x1B: out += L"\u02D9"; break;
            case 0x1C: out += L"\u02DD"; break;
            case 0x1D: out += L"\u02DB"; break;
            case 0x1E: out += L"\u02DA"; break;
            case 0x1F: out += L"\u02DC"; break;
            case 0x80: out += L"\u2022"; break;
            case 0x81: out += L"\u2020"; break;
            case 0x82: out += L"\u2021"; break;
            case 0x83: out += L"\u2026"; break;
            case 0x84: out += L"\u2014"; break;
            case 0x85: out += L"\u2013"; break;
            case 0x86: out += L"\u0192"; break;
            case 0x87: out += L"\u2044"; break;
            case 0x88: out += L"\u2039"; break;
            case 0x89: out += L"\u203A"; break;
            case 0x8A: out += L"\u2212"; break;
            case 0x8B: out += L"\u2030"; break;
            case 0x8C: out += L"\u201E"; break;
            case 0x8D: out += L"\u201C"; break;
            case 0x8E: out += L"\u201D"; break;
            case 0x8F: out += L"\u2018"; break;
            case 0x90: out += L"\u2019"; break;
            case 0x91: out += L"\u201A"; break;
            case 0x92: out += L"\u2122"; break;
            case 0x93: out += L"\uFB01"; break;
            case 0x94: out += L"\uFB02"; break;
            case 0x95: out += L"\u0141"; break;
            case 0x96: out += L"\u0152"; break;
            case 0x97: out += L"\u0160"; break;
            case 0x98: out += L"\u0178"; break;
            case 0x99: out += L"\u017D"; break;
            case 0x9A: out += L"\u0131"; break;
            case 0x9B: out += L"\u0142"; break;
            case 0x9C: out += L"\u0153"; break;
            case 0x9D: out += L"\u0161"; break;
            case 0x9E: out += L"\u017E"; break;
            case 0xA0: out += L"\u20AC"; break;
            default: out += (wchar_t)c; break;
        }
    }
    return out;
}

// ------------------------------------------------------------------- fonts --
struct DecChar { uint32_t code; int len; std::wstring text; };

// The Adobe Glyph List subset from pdf_text_tables.inc, with KillerPDF's
// GlyphText algorithm: suffix after '.', _-joined compounds, uniXXXX and
// uXXXXXX generated names, and single characters as themselves.
static bool GlyphText(const std::string& raw, std::wstring& out) {
    static const std::unordered_map<std::string, std::wstring> table = [] {
        std::unordered_map<std::string, std::wstring> t;
        for (const PdfGlyphName& g : GLYPH_NAMES) t[g.name] = g.text;
        return t;
    }();
    std::string name = raw;
    size_t suffix = name.find('.');
    if (suffix != std::string::npos) name.resize(suffix);
    auto hit = table.find(name);
    if (hit != table.end()) { out = hit->second; return true; }
    if (name.find('_') != std::string::npos) {                 // "a_b" -> both parts
        std::wstring joined;
        size_t start = 0;
        while (start <= name.size()) {
            size_t stop = name.find('_', start);
            std::string part = name.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
            std::wstring piece;
            if (!GlyphText(part, piece)) return false;
            joined += piece;
            if (stop == std::string::npos) break;
            start = stop + 1;
        }
        out = joined;
        return true;
    }
    if (name.size() > 3 && name.compare(0, 3, "uni") == 0 && (name.size() - 3) % 4 == 0) {
        std::wstring value;
        for (size_t k = 3; k < name.size(); k += 4) {
            int scalar = 0;
            for (int j = 0; j < 4; j++) {
                char c = name[k + j];
                if (!IsHexC((uint8_t)c)) return false;
                scalar = scalar * 16 + HexVal((uint8_t)c);
            }
            if (scalar >= 0xD800 && scalar <= 0xDFFF) return false;
            value += (wchar_t)scalar;
        }
        out = value;
        return true;
    }
    if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u') {
        int scalar = 0;
        for (size_t k = 1; k < name.size(); k++) {
            char c = name[k];
            if (!IsHexC((uint8_t)c)) return false;
            scalar = scalar * 16 + HexVal((uint8_t)c);
        }
        if (scalar >= 0xD800 && scalar <= 0xDFFF) return false;
        out = std::wstring(1, (wchar_t)scalar);
        return true;
    }
    if (name.size() == 1) { out = std::wstring(1, (wchar_t)(uint8_t)name[0]); return true; }
    return false;
}

// The standard encodings, by the names PDF fonts carry in /Encoding.
static const char* const* EncodingNames(const std::string& name) {
    static const std::unordered_map<std::string, const char* const*> table = [] {
        std::unordered_map<std::string, const char* const*> t;
        t["WinAnsiEncoding"] = ENC_WINANSIENCODING;
        t["MacRomanEncoding"] = ENC_MACROMANENCODING;
        t["StandardEncoding"] = ENC_STANDARDENCODING;
        t["SymbolEncoding"] = ENC_SYMBOLENCODING;
        t["ZapfDingbatsEncoding"] = ENC_ZAPFDINGBATSENCODING;
        t["PDFDocEncoding"] = ENC_PDFDOCENCODING;
        t["MacExpertEncoding"] = ENC_MACEXPERTENCODING;
        return t;
    }();
    auto hit = table.find(name);
    return hit == table.end() ? nullptr : hit->second;
}

// ---- ToUnicode CMaps (KillerPDF's PdfToUnicodeMap.ParseCore / DecodeCore) --
struct CodeSpace { uint32_t low, high; int len; };

struct UnicodeMap {
    std::map<std::pair<uint32_t, int>, std::wstring> map;
    std::vector<CodeSpace> spaces;

    bool InSpace(uint32_t code, int len) const {
        for (const CodeSpace& s : spaces)
            if (s.len == len && code >= s.low && code <= s.high) return true;
        return false;
    }
    const std::wstring* Lookup(uint32_t code, int len) const {
        auto hit = map.find({ code, len });
        return hit == map.end() ? nullptr : &hit->second;
    }
    // KillerPDF's ParseCore tail: simple fonts hold one-byte codes even when a
    // producer declares two bytes, and an Identity composite's space is 0-FFFF.
    void NormalizeSimple() {
        std::map<std::pair<uint32_t, int>, std::wstring> next;
        for (auto& kv : map)
            if (kv.first.first <= 255) next[{kv.first.first, 1}] = kv.second;
        map = std::move(next);
        spaces.clear();
        spaces.push_back({ 0, 255, 1 });
    }
    void NormalizeIdentity() {
        bool all2 = !map.empty();
        for (auto& kv : map) if (kv.first.second != 2) all2 = false;
        if (map.empty() && !spaces.empty()) {
            all2 = true;
            for (const CodeSpace& s : spaces) if (s.len != 1) all2 = false;
        }
        if (all2) {
            spaces.clear();
            spaces.push_back({ 0, 65535, 2 });
        }
        if (spaces.empty() && !map.empty()) {        // derive spaces from the mappings
            std::map<int, std::pair<uint32_t, uint32_t>> byLen;
            for (auto& kv : map) {
                auto& span = byLen[kv.first.second];
                span.first = std::min<uint32_t>(span.first ? span.first : kv.first.first, kv.first.first);
                span.second = std::max(span.second, kv.first.first);
            }
            for (auto& kv : byLen) spaces.push_back({ kv.second.first, kv.second.second, kv.first });
        }
    }
};

// One instruction: an operator keyword plus the operands before it. Shared by
// the content interpreter and the CMap parsers, like KillerPDF's single reader.
struct InstReader {
    Lexer lx;
    bool Next(std::string& op, std::vector<PdfObj>& operands) {
        operands.clear();
        while (true) {
            PdfObj tok;
            if (!lx.Token(tok)) return false;
            if (tok.t != PdfObj::KEYWORD) {
if ((int)operands.size() >= MaximumOperands) throw PdfError();
                operands.push_back(std::move(tok));
                continue;
            }
            op = std::move(tok.name);
            return true;
        }
    }
};

// KillerPDF's PdfToUnicodeMap.Unicode under compatibility recovery: an empty
// destination maps to U+FFFD and an odd trailing byte reads as U+FFFD instead
// of invalidating the whole mapping - real producers emit both.
static std::wstring Utf16Destination(const std::vector<uint8_t>& b) {
    if (b.size() == 2 && b[0] == 0xFF && b[1] == 0xFD) return std::wstring(1, (wchar_t)0xFFFD);
    if (b.empty()) return std::wstring(1, (wchar_t)0xFFFD);
    std::wstring out;
    for (size_t k = 0; k + 1 < b.size(); k += 2) out += (wchar_t)((b[k] << 8) | b[k + 1]);
    if (b.size() % 2) out += (wchar_t)0xFFFD;        // the odd trailing byte
    return out;
}
static void IncrementBytes(std::vector<uint8_t>& b) {
    for (size_t k = b.size(); k-- > 0;)
        if (++b[k] != 0) return;
throw PdfError(); // range overflow
}

// Parses decoded CMap bytes: codespacerange, bfchar, bfrange, usecmap. Runs
// under KillerPDF's compatibility recovery: metadata dictionaries and other
// PostScript chatter are skipped, malformed entries are dropped (not fatal),
// a declared block count that disagrees with its entries still parses when the
// stride fits, and mappings that fall outside the declared code spaces extend
// the spaces (IncludeMappedCodeSpaces) instead of dying.
static UnicodeMap ParseUnicodeMap(const std::vector<uint8_t>& src, bool simpleFont,
                                  bool identityFont, const UnicodeMap* inherited) {
    UnicodeMap map;
    if (inherited) {
        map.map = inherited->map;
        map.spaces = inherited->spaces;
    }
    InstReader rd{ Lexer{ src.data(), src.data() + src.size() } };
    std::string op;
    std::vector<PdfObj> args;
    std::string block;
    int blockCount = 0;
    while (rd.Next(op, args)) {
            // CMap metadata dictionaries (/CIDSystemInfo << ... >> def and
            // friends) are PostScript, not mappings - KillerPDF strips them
            // from the source before parsing (PdfCMapMetadata).
            if (!args.empty()) {
                auto drop = [](const PdfObj& o) { return o.t == PdfObj::DICT || o.t == PdfObj::NIL; };
                args.erase(std::remove_if(args.begin(), args.end(), drop), args.end());
            }
            if (op == "usecmap") {
                // A named parent needs the engine's predefined CMap data, which
                // this port does not carry; the Identity family is handled by
                // the normalization below, anything else keeps what it has.
                continue;
            }
            if (op == "begincodespacerange" || op == "beginbfchar" || op == "beginbfrange") {
                if (!block.empty() || args.size() != 1 || args[0].t != PdfObj::INT ||
                    args[0].i < 0 || args[0].i > MaximumMappings)
throw PdfError();
                block = op;
                blockCount = (int)args[0].i;
                continue;
            }
            std::string want = block.empty() ? std::string() : "end" + block.substr(5);
            if (!block.empty() && op == want) {
                int stride = block == "beginbfrange" ? 3 : 2;
                // The declared count is advisory under recovery: the entries
                // are trusted when the stride fits them.
                if (args.size() % stride || args.size() / stride > (size_t)MaximumMappings)
throw PdfError();
                for (size_t k = 0; k + stride <= args.size(); k += stride) {
                    PdfObj& lowObj = args[k];
                    if (lowObj.t != PdfObj::STR || lowObj.bytes.empty() || lowObj.bytes.size() > 4)
                        continue;                            // malformed entry: dropped
                    uint32_t low = 0;
                    for (uint8_t c : lowObj.bytes) low = (low << 8) | c;
                    int len = (int)lowObj.bytes.size();
                    if (block == "beginbfchar") {
                        if (args[k + 1].t == PdfObj::STR)
                            map.map[{low, len}] = Utf16Destination(args[k + 1].bytes);
                        continue;
                    }
                    PdfObj& highObj = args[k + 1];
                    if (highObj.t != PdfObj::STR || highObj.bytes.size() != lowObj.bytes.size())
                        continue;                            // malformed entry: dropped
                    uint32_t high = 0;
                    for (uint8_t c : highObj.bytes) high = (high << 8) | c;
                    if (block == "begincodespacerange") {
if (map.spaces.size() >= 256) throw PdfError();
                        if (high >= low) {
                            CodeSpace space{ low, high, len };
                            bool seen = false;
                            for (const CodeSpace& s : map.spaces)
                                if (s.low == space.low && s.high == space.high && s.len == space.len)
                                    { seen = true; break; }
                            if (!seen) map.spaces.push_back(space);
                        }
                        continue;
                    }
                    if (high < low) continue;                // malformed range: dropped
                    unsigned long long count = (unsigned long long)(high - low) + 1;
if (count > (unsigned long long)MaximumMappings) throw PdfError();
                    if (args[k + 2].t == PdfObj::ARR) {
                        const PdfObj& dests = args[k + 2];
                        unsigned long long take = std::min<unsigned long long>(dests.arr.size(), count);
                        for (unsigned long long j = 0; j < take; j++)
                            if (dests.arr[j].t == PdfObj::STR)
                                map.map[{low + (uint32_t)j, len}] = Utf16Destination(dests.arr[j].bytes);
                    } else if (args[k + 2].t == PdfObj::STR) {
                        std::vector<uint8_t> dest = args[k + 2].bytes;
                        for (unsigned long long j = 0; j < count; j++) {
                            map.map[{low + (uint32_t)j, len}] = Utf16Destination(dest);
                            if (j + 1 < count) IncrementBytes(dest);
                        }
                    }
                }
                block.clear();
                continue;
            }
if (!block.empty() && op.rfind("end", 0) == 0) throw PdfError();  // wrong end
            if (op.rfind("begin", 0) == 0 && op != "begincodespacerange" &&
                op != "beginbfchar" && op != "beginbfrange")
                continue;                            // registry chatter the map ignores
    }
if (!block.empty()) throw PdfError(); // an unterminated block
    if (simpleFont) map.NormalizeSimple();
    else if (identityFont) map.NormalizeIdentity();
    {
        // IncludeMappedCodeSpaces, always: mappings outside every declared
        // space join the space table, grouped by their code length - without
        // this, a codespacerange that disagrees with its own bfchar entries
        // (a real producer habit) decodes nothing but U+FFFD.
        std::map<int, std::pair<uint32_t, uint32_t>> byLen;
        for (auto& kv : map.map) {
            bool covered = false;
            for (const CodeSpace& s : map.spaces)
                if (s.len == kv.first.second && kv.first.first >= s.low && kv.first.first <= s.high)
                    { covered = true; break; }
            if (covered) continue;
            auto& span = byLen[kv.first.second];
            span.first = std::min<uint32_t>(span.first ? span.first : kv.first.first, kv.first.first);
            span.second = std::max(span.second, kv.first.first);
        }
        for (auto& kv : byLen) map.spaces.push_back({ kv.second.first, kv.second.second, kv.first });
if (map.spaces.size() > 256) throw PdfError();
    }
    if (map.spaces.empty()) throw PdfError();
    return map;
}

// ---- composite-font encoding CMaps: codespaces plus cidrange/cidchar -------
struct CidMap {
    std::map<uint32_t, uint32_t> map;
    std::vector<CodeSpace> spaces;
    bool vertical = false;
    bool ucs2Base = false;                           // a predefined UCS2-family parent
};
static CidMap ParseCidMap(const std::vector<uint8_t>& src, const CidMap* inherited, int depth) {
if (depth > 32) throw PdfError();
    CidMap out;
    if (inherited) {
        out.map = inherited->map;
        out.spaces = inherited->spaces;
        out.vertical = inherited->vertical;
        out.ucs2Base = inherited->ucs2Base;
    }
    InstReader rd{ Lexer{ src.data(), src.data() + src.size() } };
    std::string op;
    std::vector<PdfObj> args;
    long long expanded = 0;
    while (rd.Next(op, args)) {
        // metadata dictionaries are PostScript chatter, not mappings
        if (!args.empty()) {
            auto drop = [](const PdfObj& o) { return o.t == PdfObj::DICT || o.t == PdfObj::NIL; };
            args.erase(std::remove_if(args.begin(), args.end(), drop), args.end());
        }
        if (op == "usecmap") {
            if (args.size() == 1 && args[0].t == PdfObj::NAME) {
                if (args[0].name.find("UCS2") != std::string::npos) out.ucs2Base = true;
                if (args[0].name.size() >= 2 && args[0].name.compare(args[0].name.size() - 2, 2, "-V") == 0)
                    out.vertical = true;
            }
            continue;
        }
        if (op == "def" && args.size() == 2 && args[0].t == PdfObj::NAME &&
            args[0].name == "WMode" && args[1].t == PdfObj::INT)
            out.vertical = args[1].i == 1;
        if (op == "endcodespacerange") {
if (args.size() % 2 || args.size() > 512) throw PdfError();
            for (size_t k = 0; k + 1 < args.size(); k += 2) {
                if (args[k].t != PdfObj::STR || args[k + 1].t != PdfObj::STR ||
                    args[k].bytes.empty() || args[k].bytes.size() > 4 ||
                    args[k].bytes.size() != args[k + 1].bytes.size())
throw PdfError();
                uint32_t low = 0, high = 0;
                for (uint8_t c : args[k].bytes) low = (low << 8) | c;
                for (uint8_t c : args[k + 1].bytes) high = (high << 8) | c;
if (high < low) throw PdfError();
                out.spaces.push_back({ low, high, (int)args[k].bytes.size() });
            }
            continue;
        }
        if (op != "endcidchar" && op != "endcidrange") continue;
        int stride = op == "endcidchar" ? 2 : 3;
if (args.size() % stride) throw PdfError();
        for (size_t k = 0; k + stride <= args.size(); k += stride) {
            if (args[k].t != PdfObj::STR || args[k].bytes.empty() || args[k].bytes.size() > 4)
throw PdfError();
            uint32_t first = 0;
            for (uint8_t c : args[k].bytes) first = (first << 8) | c;
            uint32_t last = first;
            if (stride == 3) {
                if (args[k + 1].t != PdfObj::STR || args[k + 1].bytes.size() != args[k].bytes.size())
throw PdfError();
                last = 0;
                for (uint8_t c : args[k + 1].bytes) last = (last << 8) | c;
            }
if (args[k + stride - 1].t != PdfObj::INT) throw PdfError();
            uint32_t cid = (uint32_t)args[k + stride - 1].i;
if (last < first) throw PdfError();
if ((unsigned long long)(last - first) + cid > 65535) throw PdfError();
if ((expanded += (long long)(last - first) + 1) > 1000000) throw PdfError();
            for (uint32_t offset = 0; offset <= last - first; offset++) {
if (out.map.size() >= 65536) throw PdfError();
                out.map[first + offset] = cid + offset;
            }
        }
    }
    if (out.spaces.empty() && !out.ucs2Base)
        out.spaces.push_back({ 0, 65535, 2 });       // Identity-shaped default
    return out;
}

// ---- one font resource (KillerPDF's PdfFontResourceReader, extraction cut) --
struct PdfFont {
    bool valid = false;
    bool composite = false, ucs2Direct = false, type3 = false;
    UnicodeMap uni;                                  // ToUnicode, when present
    bool hasUni = false;
    std::vector<CodeSpace> spaces;                   // decode spaces
    std::map<uint32_t, std::wstring> simpleText;     // encoding names per code
    std::map<uint32_t, uint32_t> cidMap;             // code -> CID (CMap stream)
    std::unordered_map<uint32_t, std::wstring> gidUni;   // embedded cmap reverse map
    std::map<uint32_t, double> widths;
    double defaultWidth = 0;
    double ascent = DefaultAscent, descent = DefaultDescent;
    std::string fontName;

    double GetWidth(uint32_t code) const {
        uint32_t key = composite ? CidOf(code) : code;
        auto hit = widths.find(key);
        return hit == widths.end() ? defaultWidth : hit->second;
    }
    uint32_t CidOf(uint32_t code) const {
        if (!composite) return code;
        if (cidMap.empty()) return code;             // Identity-H, or a UCS2 family
        auto hit = cidMap.find(code);
        return hit == cidMap.end() ? 0 : hit->second;
    }
    // KillerPDF's extraction Fallback: the encoding's own text for a simple
    // font; for a composite, the UCS2 families read the code as Unicode and
    // otherwise the embedded font program's cmap names the glyph's Unicode
    // (ReverseUnicode) - the path real-world fonts without a usable ToUnicode
    // still take.
    std::wstring Fallback(uint32_t code) const {
        if (!composite) {
            auto hit = simpleText.find(code);
            return hit == simpleText.end() ? std::wstring(1, (wchar_t)0xFFFD) : hit->second;
        }
        if (ucs2Direct && code <= 0xFFFF && !(code >= 0xD800 && code <= 0xDFFF))
            return std::wstring(1, (wchar_t)code);   // the UCS2 CMaps: code IS Unicode
        if (!gidUni.empty()) {
            uint32_t cid = CidOf(code);
            if (cid <= 0xFFFF) {
                auto hit = gidUni.find(cid);
                if (hit != gidUni.end()) return hit->second;
            }
        }
        return std::wstring(1, (wchar_t)0xFFFD);
    }
    // KillerPDF's PdfToUnicodeMap.DecodeCore under compatibility recovery
    // (DecodeWithCompatibilityRecovery): one to four byte codes walked against
    // the code spaces; a missing mapping falls back, and an incomplete or
    // invalid code consumes one byte and emits the fallback text instead of
    // invalidating the rest of the string - one broken glyph never costs the
    // line (or the page) its remaining letters.
    std::vector<DecChar> Decode(const uint8_t* s, size_t n) const {
        std::vector<DecChar> out;
        size_t pos = 0;
        while (pos < n) {
            uint32_t code = 0;
            bool matched = false;
            for (int len = 1; len <= 4 && pos + (size_t)len <= n; len++) {
                code = (code << 8) | s[pos + len - 1];
                if (!InSpace(code, len)) continue;
                std::wstring text;
                const std::wstring* hit = uni.Lookup(code, len);
                if (!hit)                                 // any other length's mapping too
                    for (int other = 1; other <= 4 && !hit; other++)
                        hit = other == len ? nullptr : uni.Lookup(code, other);
                if (hit) text = *hit;
                else text = Fallback(code);
                out.push_back({ code, len, std::move(text) });
                pos += (size_t)len;
                matched = true;
                break;
            }
            if (!matched) {                               // recovery: one byte, one placeholder
                code = s[pos++];
                out.push_back({ code, 1, Fallback(code) });
            }
        }
        return out;
    }
    bool InSpace(uint32_t code, int len) const {
        for (const CodeSpace& s : spaces)
            if (s.len == len && code >= s.low && code <= s.high) return true;
        return false;
    }
};

static double MetricNum(const PdfObj* o, double fallback) {
    if (!o) return fallback;
    if (o->t == PdfObj::INT) return (double)o->i;
    if (o->t == PdfObj::REAL) return o->d;
    return fallback;
}

static void ReadCidWidths(PdfFile& F, const PdfObj* array, std::map<uint32_t, double>& widths);
static const PdfObj* ResolveInF(PdfFile& F, const PdfObj* dict, const char* key);

// The ToUnicode stream with its /UseCMap chain, the way KillerPDF's
// ReadToUnicode follows one: a stream parent's mappings sit under the child's,
// and a named parent (predefined CMap data the port does not carry) is ignored.
static UnicodeMap ReadUnicodeMapStream(PdfFile& F, const PdfObj* stream,
                                       bool simpleFont, bool identityFont, int depth) {
if (depth > 32 || !stream || stream->t != PdfObj::STREAM) throw PdfError();
    UnicodeMap inherited;
    bool haveParent = false;
    if (const PdfObj* useCmap = stream->Get("UseCMap")) {
        const PdfObj* parent = F.Resolve(useCmap);
        if (parent && parent->t == PdfObj::STREAM && parent != stream) {
            inherited = ReadUnicodeMapStream(F, parent, simpleFont, identityFont, depth + 1);
            haveParent = true;
        }
    }
    std::vector<uint8_t> bytes;
if (!F.StreamData(stream, bytes)) throw PdfError();
    return ParseUnicodeMap(bytes, simpleFont, identityFont, haveParent ? &inherited : nullptr);
}

// The composite-font /Encoding CMap stream with its /UseCMap chain (KillerPDF's
// ReadCidMap): producers embed a small override CMap on top of a base one.
static CidMap ReadCidMapStream(PdfFile& F, const PdfObj* stream, int depth) {
if (depth > 32 || !stream || stream->t != PdfObj::STREAM) throw PdfError();
    CidMap base;
    bool haveBase = false;
    if (const PdfObj* useCmap = stream->Get("UseCMap")) {
        const PdfObj* parent = F.Resolve(useCmap);
        if (parent && parent->t == PdfObj::STREAM && parent != stream) {
            base = ReadCidMapStream(F, parent, depth + 1);
            haveBase = true;
        }
    }
    std::vector<uint8_t> bytes;
if (!F.StreamData(stream, bytes)) throw PdfError();
    return ParseCidMap(bytes, haveBase ? &base : nullptr, depth);
}

// The embedded TrueType program's cmap, read straight out of /FontFile2 - the
// same source KillerPDF's ReverseUnicode names glyph Unicode from when the PDF
// carries no usable ToUnicode (real-world fonts ship like that). The subtable
// scores are TrueTypeFont.ReadCmap's; formats 0, 4, 6 and 12 cover what PDF
// subsets embed in practice. CFF-based /FontFile3 programs carry no sfnt cmap
// and are skipped, exactly like KillerPDF's TrueTypeFont.LoadForExtraction.
static void BuildReverseCmap(PdfFile& F, const PdfObj* descriptor,
                             std::unordered_map<uint32_t, std::wstring>& out) {
    if (!descriptor || !out.empty()) return;
    const PdfObj* file = ResolveInF(F, descriptor, "FontFile2");
    if (!file || file->t != PdfObj::STREAM) return;
    std::vector<uint8_t> data;
    if (!F.StreamData(file, data) || data.size() < 12) return;
    auto U16 = [&](size_t at) -> uint32_t {
        return at + 2 <= data.size() ? (uint32_t)((data[at] << 8) | data[at + 1]) : 0;
    };
    auto U32 = [&](size_t at) -> uint32_t {
        return at + 4 <= data.size()
            ? (uint32_t)(((uint32_t)data[at] << 24) | ((uint32_t)data[at + 1] << 16) |
                         ((uint32_t)data[at + 2] << 8) | data[at + 3]) : 0;
    };
    size_t face = 0;
    if (U32(0) == 0x74746366) {                      // 'ttcf': the first face
        size_t off = U32(12);
        if (off + 12 > data.size()) return;
        face = off;
    }
    uint32_t version = U32(face);
    if (version != 0x00010000 && version != 0x74727565) return;   // sfnt or 'true'
    uint32_t numTables = U16(face + 4);
    if (numTables > 512) return;
    size_t cmapAt = 0, cmapLen = 0;
    for (uint32_t k = 0; k < numTables; k++) {
        size_t rec = face + 12 + (size_t)k * 16;
        if (rec + 16 > data.size()) return;
        if (memcmp(data.data() + rec, "cmap", 4) == 0) {
            cmapAt = U32(rec + 8);
            cmapLen = U32(rec + 12);
            break;
        }
    }
    if (!cmapAt || cmapAt + 4 > data.size() || cmapLen < 4) return;
    uint32_t count = U16(cmapAt + 2);
    if (count > 64 || 4ull + (unsigned long long)count * 8 > cmapLen) return;
    int best = 0;
    size_t bestAt = 0;
    for (uint32_t k = 0; k < count; k++) {
        size_t rec = cmapAt + 4 + (size_t)k * 8;
        uint32_t platform = U16(rec), encoding = U16(rec + 2);
        uint32_t relative = U32(rec + 4);
        if (relative > cmapLen - 2) continue;
        size_t sub = cmapAt + relative;
        if (sub + 2 > data.size()) continue;
        uint32_t format = U16(sub);
        int score = 0;                               // TrueTypeFont.ReadCmap's scores
        if (platform == 3 && encoding == 10 && format == 12) score = 500;
        else if (platform == 3 && encoding == 10 && format == 13) score = 490;
        else if (platform == 3 && encoding == 10 && format == 10) score = 480;
        else if (platform == 3 && encoding == 10 && format == 8) score = 470;
        else if (platform == 0 && format == 12) score = 450;
        else if (platform == 0 && format == 13) score = 440;
        else if (platform == 0 && format == 10) score = 430;
        else if (platform == 0 && format == 8) score = 420;
        else if (platform == 3 && encoding == 1 && format == 4) score = 400;
        else if (platform == 3 && encoding == 1 && format == 12) score = 395;
        else if (platform == 0 && format == 4) score = 350;
        else if (platform == 3 && encoding == 1 && format == 6) score = 340;
        else if (platform == 0 && format == 6) score = 330;
        else if (platform == 0 && format == 0) score = 320;
        else if (platform == 0 && format == 2) score = 310;
        else if (platform == 3 && encoding == 0 && format == 4) score = 200;
        else if (platform == 1 && encoding == 0 && (format == 0 || format == 6)) score = 100;
        if (score > best) { best = score; bestAt = sub; }
    }
    if (!bestAt) return;
    uint32_t format = U16(bestAt);
    std::unordered_map<uint32_t, uint32_t> uniToGid;
    if (format == 0) {                               // byte glyph indices
        for (uint32_t c = 0; c < 256 && bestAt + 6 + c < data.size(); c++) {
            uint32_t gid = data[bestAt + 6 + c];
            if (gid) uniToGid[c] = gid;
        }
    } else if (format == 4) {                        // the BMP segments
        uint32_t segX2 = U16(bestAt + 6);
        uint32_t segCount = segX2 / 2;
        if (!segCount || segCount > 8192) return;
        size_t endAt = bestAt + 14;
        size_t startAt = endAt + (size_t)segCount * 2 + 2;
        size_t deltaAt = startAt + (size_t)segCount * 2;
        size_t rangeAt = deltaAt + (size_t)segCount * 2;
        for (uint32_t s = 0; s < segCount; s++) {
            uint32_t end = U16(endAt + (size_t)s * 2);
            uint32_t start = U16(startAt + (size_t)s * 2);
            if (start > end || start == 0xFFFF) continue;
            int delta = (int)(short)(uint16_t)U16(deltaAt + (size_t)s * 2);
            uint32_t rangeOff = U16(rangeAt + (size_t)s * 2);
            for (uint32_t c = start; c <= end; c++) {
                uint32_t gid = 0;
                if (rangeOff == 0) {
                    gid = (uint32_t)((c + delta) & 0xFFFF);
                } else {
                    size_t at = rangeAt + (size_t)s * 2 + (size_t)rangeOff + (size_t)(c - start) * 2;
                    gid = U16(at);
                    if (gid) gid = (uint32_t)((gid + delta) & 0xFFFF);
                }
                if (gid) uniToGid[c] = gid;
            }
        }
    } else if (format == 6) {                        // a trimmed run
        uint32_t first = U16(bestAt + 6), entries = U16(bestAt + 8);
        for (uint32_t k = 0; k < entries; k++) {
            uint32_t gid = U16(bestAt + 10 + (size_t)k * 2);
            if (gid) uniToGid[first + k] = gid;
        }
    } else if (format == 12 || format == 13) {       // the full-repertoire groups
        uint32_t groups = U32(bestAt + 12);
        if (groups > 200000) return;
        for (uint32_t g = 0; g < groups; g++) {
            size_t at = bestAt + 16 + (size_t)g * 12;
            if (at + 12 > data.size()) return;
            uint32_t start = U32(at), end = U32(at + 4), gid0 = U32(at + 8);
            if (start > end || end - start > 65535) continue;
            for (uint32_t c = start; c <= end; c++) {
                if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) continue;
                uniToGid[c] = gid0 + (c - start);    // format 13: same glyph, one entry is enough
                if (format == 13) break;
            }
        }
    } else return;
    // ReverseUnicode: the lowest scalar wins each glyph (the walk is ascending).
    for (uint32_t scalar = 0; scalar <= 0xFFFF; scalar++) {
        if (scalar >= 0xD800 && scalar <= 0xDFFF) continue;
        auto hit = uniToGid.find(scalar);
        if (hit == uniToGid.end()) continue;
        out.emplace(hit->second, std::wstring(1, (wchar_t)scalar));
    }
}

// Reads one font resource dictionary (already resolved) the way
// PdfFontResourceReader.Read does, minus the outline machinery docsy's
// extraction subset does not need.
static PdfFont ReadFont(PdfFile& F, const PdfObj* font) {
    PdfFont out;
    const PdfObj* subtype = ResolveInF(F, font, "Subtype");
    std::string sub = subtype && subtype->t == PdfObj::NAME ? subtype->name : std::string();
    bool composite = sub == "Type0";
    out.type3 = sub == "Type3";
    const PdfObj* metrics = font;
    if (composite) {
        const PdfObj* descendants = ResolveInF(F, font, "DescendantFonts");
        if (descendants && descendants->t == PdfObj::ARR && !descendants->arr.empty()) {
            const PdfObj* d0 = F.Resolve(&descendants->arr[0]);
            if (d0 && d0->t == PdfObj::DICT) metrics = d0;
        }
        if (metrics == font) {
            // KillerPDF's recovery: an Identity composite whose ToUnicode is
            // present reads with an empty metrics dictionary - the W and DW
            // lookups simply miss and the default width carries the boxes.
            std::string enc0;
            const PdfObj* enc = font->Get("Encoding");
            if (enc && enc->t == PdfObj::NAME) enc0 = enc->name;
            if (enc0 == "Identity-H" || enc0 == "Identity-V") metrics = font;
else throw PdfError();
        }
    }
    const PdfObj* descriptor = ResolveInF(F, metrics, "FontDescriptor");

    const PdfObj* nameObj = ResolveInF(F, font, "BaseFont");
    if (!nameObj || nameObj->t != PdfObj::NAME) nameObj = ResolveInF(F, metrics, "BaseFont");
    if (!nameObj || nameObj->t != PdfObj::NAME) nameObj = ResolveInF(F, descriptor, "FontName");
    if (nameObj && nameObj->t == PdfObj::NAME) out.fontName = nameObj->name;
    std::string metricsName = out.fontName;
    if (metricsName.size() > 7 && metricsName[6] == '+') metricsName = metricsName.substr(7);

    std::string encodingName;
    const PdfObj* encoding = ResolveInF(F, font, "Encoding");
    const PdfObj* encodingDict = nullptr;
    if (encoding && encoding->t == PdfObj::NAME) encodingName = encoding->name;
    else if (encoding && encoding->t == PdfObj::DICT) {
        encodingDict = encoding;
        const PdfObj* base = ResolveInF(F, encoding, "BaseEncoding");
        if (base && base->t == PdfObj::NAME) encodingName = base->name;
    }
    bool vertical = encodingName.size() >= 2 && encodingName.compare(encodingName.size() - 2, 2, "-V") == 0;
    (void)vertical;                                  // vertical writing stays horizontal here

    if (composite) {
        out.composite = true;
        ReadCidWidths(F, ResolveInF(F, metrics, "W"), out.widths);
        const PdfObj* dw = ResolveInF(F, metrics, "DW");
        out.defaultWidth = dw ? MetricNum(dw, 1000) : 1000;
        if (encoding && encoding->t == PdfObj::STREAM) {
            try {
                CidMap map = ReadCidMapStream(F, encoding, 0);
                out.cidMap = std::move(map.map);
                out.spaces = std::move(map.spaces);
                out.ucs2Direct = map.ucs2Base;
            } catch (PdfError&) {
                // KillerPDF's "an unreadable map was ignored" path: the font's
                // own cmap still names the glyphs below, so keep an Identity-
                // shaped space instead of costing the page its remaining text.
                out.spaces.push_back({ 0, 65535, 2 });
            }
        } else if (encodingName == "Identity-H" || encodingName == "Identity-V") {
            // code == CID, two bytes
        } else if (encodingName.find("UCS2") != std::string::npos) {
            out.ucs2Direct = true;                   // predefined UCS2: code IS Unicode
        } else {
throw PdfError(); // shift-CID data stays behind the gate
        }
        BuildReverseCmap(F, descriptor, out.gidUni);
        if (out.spaces.empty()) out.spaces.push_back({ 0, 65535, 2 });
    } else {
        out.defaultWidth = 0;                        // MissingWidth's slot
        // KillerPDF's recovery: the WinAnsi typo producers emit, and an
        // unrecognized encoding name carries no mapping - the font's metrics
        // name picks the standard encoding, exactly like an absent one.
        if (encodingName == "WinAnsiEcoding") encodingName = "WinAnsiEncoding";
        if (!encodingName.empty() && !EncodingNames(encodingName)) encodingName.clear();
        if (encodingName.empty()) {
            encodingName = metricsName == "Symbol" ? "SymbolEncoding"
                         : metricsName == "ZapfDingbats" ? "ZapfDingbatsEncoding"
                         : "StandardEncoding";
        }
        const char* const* names = EncodingNames(encodingName);
        if (!names) names = EncodingNames("StandardEncoding");   // unknown name: absent
        std::string glyphNames[256];
        for (int code = 0; code < 256; code++) glyphNames[code] = names[code];
        if (encodingDict) {                          // Differences rename codes
            const PdfObj* diffs = ResolveInF(F, encodingDict, "Differences");
            if (diffs && diffs->t == PdfObj::ARR) {
                int index = -1;
                for (const PdfObj& item : diffs->arr) {
                    const PdfObj* value = F.Resolve(&item);
                    if (value && value->t == PdfObj::INT && value->i >= 0 && value->i < 256)
                        index = (int)value->i;
                    else if (value && value->t == PdfObj::NAME && index >= 0 && index < 256)
                        glyphNames[index++] = value->name;
                    else index = -1;                 // invalid entry: the rest is ignored
                }
            }
        }
        for (int code = 0; code < 256; code++) {
            std::wstring text;
            if (GlyphText(glyphNames[code], text)) out.simpleText[(uint32_t)code] = text;
            else if (out.type3) out.simpleText[(uint32_t)code] = std::wstring(1, (wchar_t)code);
        }
        const PdfObj* widths = ResolveInF(F, font, "Widths");
        if (widths && widths->t == PdfObj::ARR) {
            const PdfObj* firstObj = ResolveInF(F, font, "FirstChar");
            long long first = firstObj ? (long long)MetricNum(firstObj, 0) : 0;
            if (first >= 0 && first < 256)
                for (size_t k = 0; k < widths->arr.size() && first + (long long)k < 256; k++) {
                    const PdfObj* w = F.Resolve(&widths->arr[k]);
                    if (w && (w->t == PdfObj::INT || w->t == PdfObj::REAL))
                        out.widths[(uint32_t)(first + k)] = MetricNum(w, 0);
                }
        }
        // A width-less simple font still needs selectable boxes: one default
        // advance (the deliberate AFM reduction - boxes stay usable, text exact).
        if (out.widths.empty()) out.defaultWidth = 556;
        if (out.spaces.empty()) out.spaces.push_back({ 0, 255, 1 });
    }

    const PdfObj* uniStream = ResolveInF(F, font, "ToUnicode");
    if (uniStream && uniStream->t == PdfObj::STREAM) {
        try {
            out.uni = ReadUnicodeMapStream(F, uniStream, !composite, composite &&
                                           (encodingName == "Identity-H" || encodingName == "Identity-V"), 0);
            out.hasUni = true;
            if (!out.uni.spaces.empty()) out.spaces = out.uni.spaces;
        } catch (PdfError&) {
            // "An unreadable ToUnicode map was ignored": the embedded font's
            // own cmap (read above) still names the glyphs, so extraction
            // continues through the fallback instead of losing the page.
        }
    }
    if (out.spaces.empty()) out.spaces.push_back({ 0, composite ? 65535u : 255u, composite ? 2 : 1 });

    out.ascent = descriptor ? MetricNum(ResolveInF(F, descriptor, "Ascent"), DefaultAscent) : DefaultAscent;
    out.descent = descriptor ? MetricNum(ResolveInF(F, descriptor, "Descent"), DefaultDescent) : DefaultDescent;
    if (out.ascent <= out.descent) {
        const PdfObj* box = descriptor ? ResolveInF(F, descriptor, "FontBBox") : nullptr;
        if (box && box->t == PdfObj::ARR && box->arr.size() == 4) {
            out.ascent = MetricNum(F.Resolve(&box->arr[3]), DefaultAscent);
            out.descent = MetricNum(F.Resolve(&box->arr[1]), DefaultDescent);
        }
        if (out.ascent <= out.descent) { out.ascent = DefaultAscent; out.descent = DefaultDescent; }
    }
    if (out.type3) {                                 // Type 3 widths live in glyph space
        const PdfObj* matrix = ResolveInF(F, font, "FontMatrix");
        if (matrix && matrix->t == PdfObj::ARR && matrix->arr.size() == 6) {
            double scale = MetricNum(F.Resolve(&matrix->arr[0]), 0.001) * 1000;
            if (scale > 0 && scale != 1) {
                for (auto& kv : out.widths) kv.second *= scale;
                out.defaultWidth *= scale;
            }
        }
    }
    out.valid = true;
    return out;
}
static void ReadCidWidths(PdfFile& F, const PdfObj* array, std::map<uint32_t, double>& widths) {
    if (!array || array->t != PdfObj::ARR) return;
    long long expanded = 0;
    for (size_t k = 0; k < array->arr.size();) {
        const PdfObj* first = F.Resolve(&array->arr[k++]);
if (!first || first->t != PdfObj::INT || first->i < 0) throw PdfError();
if (k >= array->arr.size()) throw PdfError();
        const PdfObj* second = F.Resolve(&array->arr[k++]);
        if (second && second->t == PdfObj::ARR) {
if (first->i + (long long)second->arr.size() > 65536) throw PdfError();
if ((expanded += (long long)second->arr.size()) > 1000000) throw PdfError();
            for (size_t j = 0; j < second->arr.size(); j++) {
                const PdfObj* w = F.Resolve(&second->arr[j]);
                widths[(uint32_t)first->i + (uint32_t)j] = w ? MetricNum(w, 1000) : 1000;
            }
        } else {
if (!second || second->t != PdfObj::INT || second->i < first->i) throw PdfError();
if (k >= array->arr.size()) throw PdfError();
            const PdfObj* w = F.Resolve(&array->arr[k++]);
if ((expanded += second->i - first->i + 1) > 1000000) throw PdfError();
            for (uint32_t cid = (uint32_t)first->i; cid <= (uint32_t)second->i; cid++)
                widths[cid] = w ? MetricNum(w, 1000) : 1000;
        }
    }
}

// ------------------------------------------------- the text interpreter ----
// KillerPDF's PdfTextContentReader.ReadInstructions, with the page-level form
// recursion its PdfPageContentReader.Walk does: full text state machine, marked
// content with ActualText, form XObjects carrying their own resources.
struct Matrix {
    double A = 1, B = 0, C = 0, D = 1, E = 0, F = 0;
    void Point(double x, double y, double& px, double& py) const {
        px = A * x + C * y + E;
        py = B * x + D * y + F;
    }
    Matrix Then(const Matrix& m) const {             // this x m, KillerPDF's order
        Matrix r;
        r.A = m.A * A + m.C * B;
        r.B = m.B * A + m.D * B;
        r.C = m.A * C + m.C * D;
        r.D = m.B * C + m.D * D;
        r.E = m.A * E + m.C * F + m.E;
        r.F = m.B * E + m.D * F + m.F;
        return r;
    }
    Matrix Move(double x, double y) const { return Matrix{ 1, 0, 0, 1, x, y }.Then(*this); }
};

struct Letter {
    std::wstring text;
    double l = 0, r = 0, b = 0, t = 0;               // cropbox-relative points, y-up
    double point = 0;
    std::string fontName;
    // KillerPDF's StartBaseLine / AdvanceEnd (PdfTextPlacement.Origin/AdvanceEnd):
    // where this glyph's baseline starts and ends - the advance width, not the
    // outline. GroupWords measures word gaps along this vector, so the rule is
    // the engine's own and RTL runs written with mirrored advances split right.
    double sbx = 0, sby = 0, ebx = 0, eby = 0;
    // the word this letter belongs to (GroupWords below): 1-based content-order
    // groups, 0 = a whitespace letter that belongs to no word and is not
    // selectable - KillerPDF's GroupWords flushes on them and skips them
    int word = 0;
};

struct Session;

struct Interp {
    // The shared per-document font cache, behind pointers so the interpreter
    // stays compilable ahead of Session's definition.
    PdfFile* file = nullptr;
    std::map<const void*, int>* fontIds = nullptr;
    std::vector<PdfFont>* fonts = nullptr;
    std::vector<Letter> out;
    int expansions = 0;

    struct St {
        Matrix ctm;
        std::string font;
        double size = 0, charSp = 0, wordSp = 0, scale = 1, leading = 0, rise = 0;
    };

    PdfFont& Font(const PdfObj* resources, const std::string& key, int depth);

    void Show(const PdfObj& str, Matrix& text, St& st, const PdfObj* resources, int depth) {
if (st.font.empty()) throw PdfError();
        PdfFont& font = Font(resources, st.font, depth);
if (str.t != PdfObj::STR) throw PdfError();
        std::vector<DecChar> chars = font.Decode(str.bytes.data(), str.bytes.size());
        for (const DecChar& ch : chars) {
if ((int)out.size() >= MaximumLetters) throw PdfError();
            double size = std::max(st.size, 0.000001);
            // KillerPDF's Show: the box is the advance box (no outline reader
            // here), corners pushed through text x CTM and min/maxed.
            Matrix transform = text.Then(st.ctm);
            double left = 0;
            double right = font.GetWidth(ch.code) / 1000 * st.size * st.scale;
            double bottom = font.descent / 1000 * st.size + st.rise;
            double top = font.ascent / 1000 * st.size + st.rise;
            double x0, y0, x1, y1, x2, y2, x3, y3;
            transform.Point(left, bottom, x0, y0);
            transform.Point(right, bottom, x1, y1);
            transform.Point(left, top, x2, y2);
            transform.Point(right, top, x3, y3);
            Letter L;
            L.text = ch.text;
            L.l = std::min(std::min(x0, x1), std::min(x2, x3));
            L.r = std::max(std::max(x0, x1), std::max(x2, x3));
            L.b = std::min(std::min(y0, y1), std::min(y2, y3));
            L.t = std::max(std::max(y0, y1), std::max(y2, y3));
            double sx, sy, ex, ey;
            transform.Point(0, 0, sx, sy);
            transform.Point(0, size, ex, ey);
            L.point = std::sqrt((ex - sx) * (ex - sx) + (ey - sy) * (ey - sy));
            // the baseline endpoints KillerPDF's GroupWords measures along:
            // start at the glyph origin, end one advance width along the line
            double ax, ay;
            transform.Point(right, 0, ax, ay);
            L.sbx = sx; L.sby = sy; L.ebx = ax; L.eby = ay;
            L.fontName = font.fontName;
            out.push_back(std::move(L));
            double spacing = st.charSp;
            if (ch.len == 1 && ch.code == 32) spacing += st.wordSp;
            text = text.Move(right + spacing * st.scale, 0);
        }
    }

    void Run(const std::vector<uint8_t>& src, const PdfObj* resources, Matrix ctm0, int depth) {
if (depth > MaximumFormDepth) throw PdfError();
        St st;
        st.ctm = ctm0;
        Matrix text = Matrix(), line = Matrix();
        bool insideText = false;
        std::vector<St> stack;
        std::vector<std::tuple<size_t, std::vector<uint8_t>, int>> marked;   // start, ActualText, MCID
        int markedId = -1;
        InstReader rd{ Lexer{ src.data(), src.data() + src.size() } };
        std::string op;
        std::vector<PdfObj> args;
        while (rd.Next(op, args)) {
            // Operands are read from the END (the last N belong to the operator):
            // common producers leave stray operands before text operators, and
            // every viewer accepts them - KillerPDF's compat path does too.
            auto Num = [&](int k) -> double {              // k counts from the end, 0 = last
                int idx = (int)args.size() - 1 - k;
if (idx < 0) throw PdfError();
                if (args[idx].t == PdfObj::INT) return (double)args[idx].i;
                if (args[idx].t == PdfObj::REAL) return args[idx].d;
throw PdfError();
            };
            auto RequireText = [&]() { if (!insideText) throw PdfError(); };
            auto MoveLine = [&](double x, double y) {
                RequireText();
                text = line = line.Move(x, y);
            };
            if (op == "q") {
if ((int)stack.size() >= MaximumStateStack) throw PdfError();
                stack.push_back(st);
            } else if (op == "Q") {
if (stack.empty()) throw PdfError();
                st = stack.back(); stack.pop_back();
            } else if (op == "cm") {
if (args.size() < 6) throw PdfError();
                Matrix m{ Num(5), Num(4), Num(3), Num(2), Num(1), Num(0) };
                st.ctm = m.Then(st.ctm);
            } else if (op == "BT") {
if (insideText) throw PdfError();
                insideText = true;
                text = line = Matrix();
            } else if (op == "ET") {
                RequireText();
                insideText = false;
            } else if (op == "Tf") {
if (args.size() < 2 || args[(int)args.size() - 2].t != PdfObj::NAME) throw PdfError();
                st.font = args[(int)args.size() - 2].name;
                st.size = Num(0);
            } else if (op == "Tc") { st.charSp = Num(0);
            } else if (op == "Tw") { st.wordSp = Num(0);
            } else if (op == "Tz") { st.scale = Num(0) / 100;
            } else if (op == "TL") { st.leading = Num(0);
            } else if (op == "Ts") { st.rise = Num(0);
            } else if (op == "Tr") {
                if (args.empty() || args.back().t != PdfObj::INT || args.back().i < 0 || args.back().i > 7)
throw PdfError();
            } else if (op == "Tm") {
                RequireText();
if (args.size() < 6) throw PdfError();
                text = line = Matrix{ Num(5), Num(4), Num(3), Num(2), Num(1), Num(0) };
            } else if (op == "Td") { MoveLine(Num(1), Num(0));
            } else if (op == "TD") { st.leading = -Num(1); MoveLine(Num(1), Num(0));
            } else if (op == "T*") { MoveLine(0, -st.leading);
            } else if (op == "Tj") {
                RequireText();
                bool any = false;
                for (const PdfObj& item : args)
                    if (item.t == PdfObj::STR) { Show(item, text, st, resources, depth); any = true; }
if (!any) throw PdfError();
            } else if (op == "'") {
                MoveLine(0, -st.leading);
                bool any = false;
                for (const PdfObj& item : args)
                    if (item.t == PdfObj::STR) { Show(item, text, st, resources, depth); any = true; }
if (!any) throw PdfError();
            } else if (op == "\"") {
if (args.size() < 3) throw PdfError();
                st.wordSp = Num(2); st.charSp = Num(1);
                MoveLine(0, -st.leading);
                Show(args.back(), text, st, resources, depth);
            } else if (op == "TJ") {
                RequireText();
if (args.empty() || args.back().t != PdfObj::ARR) throw PdfError();
                for (const PdfObj& item : args.back().arr) {
                    if (item.t == PdfObj::STR || item.t == PdfObj::ARR) Show(item, text, st, resources, depth);
                    else if (item.t == PdfObj::INT || item.t == PdfObj::REAL) {
                        double adj = item.t == PdfObj::INT ? (double)item.i : item.d;
                        text = text.Move(-adj / 1000 * st.size * st.scale, 0);
} else throw PdfError();
                }
            } else if (op == "BMC" || op == "BDC") {
if ((int)marked.size() >= MaximumStateStack) throw PdfError();
                std::vector<uint8_t> actual;
                int mcid = -1;
                if (op == "BDC" && !args.empty() && args.back().t == PdfObj::DICT) {
                    const PdfObj* at = args.back().Get("ActualText");
                    if (at && at->t == PdfObj::STR) actual = at->bytes;
                    const PdfObj* id = args.back().Get("MCID");
                    if (id && id->t == PdfObj::INT && id->i >= 0) mcid = (int)id->i;
                }
                marked.push_back({ out.size(), std::move(actual), mcid });
                if (mcid >= 0) markedId = mcid;
            } else if (op == "EMC") {
if (marked.empty()) throw PdfError();
                auto section = marked.back(); marked.pop_back();
                if (std::get<2>(section) >= 0) {
                    markedId = -1;
                    for (auto it = marked.rbegin(); it != marked.rend(); it++)
                        if (std::get<2>(*it) >= 0) { markedId = std::get<2>(*it); break; }
                }
                const std::vector<uint8_t>& replacement = std::get<1>(section);
                bool outerHas = false;
                for (auto& m : marked) if (std::get<1>(m).size()) outerHas = true;
                if (replacement.size() && !outerHas && out.size() > std::get<0>(section)) {
                    std::wstring text2 = DecodeTextString(replacement);
                    double l = out[std::get<0>(section)].l, r = l, b = out[std::get<0>(section)].b, t = out[std::get<0>(section)].t;
                    for (size_t k = std::get<0>(section); k < out.size(); k++) {
                        l = std::min(l, out[k].l); r = std::max(r, out[k].r);
                        b = std::min(b, out[k].b); t = std::max(t, out[k].t);
                    }
                    // the replacement rides the span's baseline like KillerPDF's
                    // "first with { AdvanceEnd = last.AdvanceEnd }": the first
                    // letter's origin and size, the last letter's advance end, so
                    // GroupWords keeps it glued to the neighbours it replaced
                    double fpx = out[std::get<0>(section)].point;
                    double fsx = out[std::get<0>(section)].sbx, fsy = out[std::get<0>(section)].sby;
                    double lex = out.back().ebx, ley = out.back().eby;
                    out.resize(std::get<0>(section));
                    if (!text2.empty()) {
                        Letter L = out.empty() ? Letter() : Letter();
                        L.text = text2;
                        L.l = l; L.r = r; L.b = b; L.t = t;
                        L.point = fpx;
                        L.sbx = fsx; L.sby = fsy; L.ebx = lex; L.eby = ley;
                        out.push_back(std::move(L));
                    }
                }
            } else if (op == "Do") {
if (args.size() != 1 || args[0].t != PdfObj::NAME) throw PdfError();
                const PdfObj* xobjs = ResolveInF(*file, resources, "XObject");
                const PdfObj* entry = xobjs && xobjs->t == PdfObj::DICT ? xobjs->Get(args[0].name.c_str()) : nullptr;
                const PdfObj* xo = entry ? file->Resolve(entry) : nullptr;
if (!xo || xo->t != PdfObj::STREAM) throw PdfError();
                const PdfObj* sub = ResolveInF(*file, xo, "Subtype");
                if (sub && sub->t == PdfObj::NAME && sub->name == "Form") {
if (++expansions > MaximumFormExpansions) throw PdfError();
                    std::vector<uint8_t> bytes;
if (!file->StreamData(xo, bytes)) throw PdfError();
                    const PdfObj* own = ResolveInF(*file, xo, "Resources");
                    const PdfObj* formRes = own ? own : resources;
                    Matrix form = Matrix();
                    const PdfObj* fm = ResolveInF(*file, xo, "Matrix");
                    if (fm && fm->t == PdfObj::ARR && fm->arr.size() == 6)
                        form = Matrix{ MetricNum(file->Resolve(&fm->arr[0]), 0), MetricNum(file->Resolve(&fm->arr[1]), 0),
                                       MetricNum(file->Resolve(&fm->arr[2]), 0), MetricNum(file->Resolve(&fm->arr[3]), 0),
                                       MetricNum(file->Resolve(&fm->arr[4]), 0), MetricNum(file->Resolve(&fm->arr[5]), 0) };
                    Run(bytes, formRes, form.Then(st.ctm), depth + 1);
                }
                // image XObjects carry no text: skipped
            } else if (op == "BI") {
                // inline image: params up to ID, then the data up to a
                // whitespace-delimited EI (the encoded end marker)
                std::string k;
                std::vector<PdfObj> kv;
                while (rd.Next(k, kv))
                    if (k == "ID") break;
                if (rd.lx.p < rd.lx.end && *rd.lx.p == 13) {
                    rd.lx.p++;
                    if (rd.lx.p < rd.lx.end && *rd.lx.p == 10) rd.lx.p++;
                } else if (rd.lx.p < rd.lx.end && *rd.lx.p == 10) rd.lx.p++;
                const uint8_t* s = rd.lx.p;
                while (rd.lx.p + 2 <= rd.lx.end) {
                    if (rd.lx.p[0] == 'E' && rd.lx.p[1] == 'I' &&
                        (rd.lx.p == s || IsWhite(rd.lx.p[-1]))) {
                        rd.lx.p += 2;
                        break;
                    }
                    rd.lx.p++;
                }
            }
            // every other operator paints or strokes - no text in it - and is
            // skipped, which is what KillerPDF's compatibility sections do with
            // the ones it does not interpret.
        }
if (insideText || !stack.empty() || !marked.empty()) throw PdfError();
    }
};

// ------------------------------------------------------------- page walk ----
struct PageText {
    bool failed = false, truncated = false;
    double boxW = 0, boxH = 0;
    int rotation = 0;
    int fffd = 0;
    std::vector<Letter> letters;
};

// A point in the cropbox's own space (y-up, origin at its corner) mapped into
// the space the page renders in: y-down, with the page's /Rotate quarter-turns
// applied. The same mapping Windows.Data.Pdf renders by, so text boxes land on
// the pixels they belong to. Pure - the Linux harness asserts it too.
static void RotateToRender(int rotation, double boxW, double boxH,
                           double x, double y, double& rx, double& ry) {
    int rot = ((rotation % 360) + 360) % 360;
    if (rot % 90) rot = (int)(((rot + 45) / 90) * 90) % 360;
    double X = x, Y = boxH - y;                      // to y-down first
    switch (rot) {
        case 90:  rx = boxH - Y; ry = X; break;      // clockwise quarter-turn
        case 180: rx = boxW - X; ry = boxH - Y; break;
        case 270: rx = Y;        ry = boxW - X; break;
        default:  rx = X;        ry = Y; break;
    }
}

// Resource lookups that may land on an indirect reference.
static const PdfObj* ResolveInF(PdfFile& F, const PdfObj* dict, const char* key) {
    const PdfObj* v = dict ? dict->Get(key) : nullptr;
    return v ? F.Resolve(v) : nullptr;
}

// .NET's char.IsWhiteSpace, the set KillerPDF's GroupWords flushes on: the
// ASCII spaces plus the Unicode separators a PDF's ToUnicode can decode to.
static bool LetterIsWhite(const std::wstring& s) {
    if (s.empty()) return true;
    for (wchar_t c : s) {
        if (c == L' ' || (c >= 0x09 && c <= 0x0D)) continue;
        if (c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
            c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000)
            continue;
        return false;
    }
    return true;
}

// KillerPDF's PdfPageContent.GroupWords, letter for letter: words group letters
// in CONTENT order; a whitespace letter flushes the open word and joins none
// (it is not selectable and never copied - the space between words comes from
// TextForRange's word-ordinal rule, which is why docsy's old space cells
// tripled every gap); a word also breaks when the gap along the baseline
// exceeds 0.22 em (TeX and table cells separate words by position alone), the
// perpendicular offset exceeds 0.35 em (a new line or a superscript), or the
// advance backtracks past half an em (a separate positioning op). Measuring
// along each letter's own baseline vector makes the rule direction-blind, so
// an RTL run written with mirrored advances splits exactly where it should.
static void GroupWords(std::vector<Letter>& letters) {
    int word = 0;
    const Letter* prev = nullptr;
    for (Letter& L : letters) {
        if (LetterIsWhite(L.text)) { prev = nullptr; L.word = 0; continue; }
        if (prev) {
            double dx = prev->ebx - prev->sbx, dy = prev->eby - prev->sby;
            double length = std::sqrt(dx * dx + dy * dy);
            double ux = length > 0.001 ? dx / length : 1.0;
            double uy = length > 0.001 ? dy / length : 0.0;
            double gapX = L.sbx - prev->ebx, gapY = L.sby - prev->eby;
            double gap = gapX * ux + gapY * uy;
            double across = std::fabs(gapY * ux - gapX * uy);
            double em = std::max(1.0, std::min(prev->point, L.point));
            if (across > em * 0.35 || gap > em * 0.22 ||
                gap < -std::max(length, em * 0.5))
                word++;                        // flush: a new word starts here
        } else {
            word++;                            // open the first word after a space
        }
        L.word = word;
        prev = &L;
    }
}

PdfFont& Interp::Font(const PdfObj* resources, const std::string& key, int depth) {
    (void)depth;
    const PdfObj* fontsDict = ResolveInF(*file, resources, "Font");
    const PdfObj* entry = fontsDict && fontsDict->t == PdfObj::DICT ? fontsDict->Get(key.c_str()) : nullptr;
    const PdfObj* fontDict = entry ? file->Resolve(entry) : nullptr;
if (!fontDict || fontDict->t != PdfObj::DICT) throw PdfError();
    auto hit = fontIds->find(fontDict);
    if (hit != fontIds->end()) return (*fonts)[hit->second];
    PdfFont f;
    try {
        f = ReadFont(*file, fontDict);
    } catch (PdfError&) {
        // A font resource that cannot be read must not cost the page the text
        // drawn after it. KillerPDF's reader reaches the same page through its
        // recovery without ever failing here; the extraction keeps flowing
        // with placeholder glyphs on a default-advance box so the line (and
        // everything after it) stays selectable.
        f = PdfFont{};
        f.spaces.push_back({ 0, 255, 1 });
        f.defaultWidth = 556;
    }
    (*fontIds)[fontDict] = (int)fonts->size();
    fonts->push_back(std::move(f));
    return fonts->back();
}

struct Session {
    PdfFile file;
    struct Node {
        const PdfObj* dict = nullptr;
        const PdfObj* resources = nullptr;
        std::vector<double> media, crop;
        long long rotate = 0;
    };
    std::vector<Node> pages;
    std::map<int, PageText> built;
    bool opened = false;
    // The font cache is per document (KillerPDF's reader-level _fontCache),
    // shared by every page build; the mutex keeps the UI thread and the AI
    // pumps' async arrivals from racing the same cache.
    std::map<const void*, int> fontIds;
    std::vector<PdfFont> fonts;
    std::mutex lock;

    bool Open(std::vector<uint8_t>&& bytes) {
        opened = file.Open(std::move(bytes));
        if (!opened) return false;
        try {
            // the catalog's /Pages is the tree's root (KillerPDF's PdfPageTree.Read)
            const PdfObj* pagesRoot = ResolveInF(file, file.root, "Pages");
            if (pagesRoot) Walk(pagesRoot, nullptr, {}, {}, 0, 0);
        } catch (PdfError&) { return false; }
        catch (const std::exception&) { return false; }
        return !pages.empty();
    }

    // The page tree with its inherited /Resources, boxes and /Rotate - the
    // KillerPDF PdfPageTree walk, with the same cycle cap.
    void Walk(const PdfObj* node, const PdfObj* res, std::vector<double> media,
              std::vector<double> crop, long long rotate, int depth) {
if (depth > MaximumTreeDepth || pages.size() > 65536) throw PdfError();
        node = file.Resolve(node);
if (!node || node->t != PdfObj::DICT) throw PdfError();
        const PdfObj* ownRes = ResolveInF(file, node, "Resources");
        if (ownRes) res = ownRes;
        const PdfObj* m = ResolveInF(file, node, "MediaBox");
        if (m && m->t == PdfObj::ARR && m->arr.size() == 4) media = BoxNums(m);
        const PdfObj* c = ResolveInF(file, node, "CropBox");
        if (c && c->t == PdfObj::ARR && c->arr.size() == 4) crop = BoxNums(c);
        const PdfObj* r = ResolveInF(file, node, "Rotate");
        if (r && r->t == PdfObj::INT) rotate = r->i;
        const PdfObj* kids = ResolveInF(file, node, "Kids");
        if (kids && kids->t == PdfObj::ARR) {
            for (const PdfObj& kid : kids->arr)
                Walk(file.Resolve(&kid), res, media, crop, rotate, depth + 1);
            return;
        }
        pages.push_back({ node, res, std::move(media), std::move(crop), rotate });
    }
    std::vector<double> BoxNums(const PdfObj* box) {
        std::vector<double> v;
        for (const PdfObj& item : box->arr) {
            const PdfObj* n = item.t == PdfObj::REF ? file.Resolve(&item) : &item;
if (!n || n->t != PdfObj::INT && n->t != PdfObj::REAL) throw PdfError();
            v.push_back(MetricNum(n, 0));
        }
        return v;
    }

    // KillerPDF's PdfPageInformation.EffectiveBox: crop cut to media, media
    // alone, or the compatibility default.
    static void EffectiveBox(const std::vector<double>& media, const std::vector<double>& crop,
                             double& l, double& b, double& w, double& h) {
        static const double fallbackBox[4] = { 0, 0, 612, 792 };
        const std::vector<double>& box = !crop.empty() ? crop : (!media.empty() ? media : std::vector<double>());
        double x1, y1, x2, y2;
        if (box.size() == 4) { x1 = box[0]; y1 = box[1]; x2 = box[2]; y2 = box[3]; }
        else { x1 = fallbackBox[0]; y1 = fallbackBox[1]; x2 = fallbackBox[2]; y2 = fallbackBox[3]; }
        if (box.size() == 4 && crop.size() == 4 && media.size() == 4) {
            double mx1 = media[0], my1 = media[1], mx2 = media[2], my2 = media[3];
            l = std::max(std::min(x1, x2), std::min(mx1, mx2));
            b = std::max(std::min(y1, y2), std::min(my1, my2));
            double r = std::min(std::max(x1, x2), std::max(mx1, mx2));
            double t = std::min(std::max(y1, y2), std::max(my1, my2));
            if (l <= r && b <= t) { w = r - l; h = t - b; return; }
            l = std::min(x1, x2); b = std::min(y1, y2);   // the two never met: crop alone
            w = std::abs(x2 - x1); h = std::abs(y2 - y1);
            return;
        }
        l = std::min(x1, x2); b = std::min(y1, y2);
        w = std::abs(x2 - x1); h = std::abs(y2 - y1);
    }

    PageText& Page(int index) {
        std::lock_guard<std::mutex> lk(lock);
        auto hit = built.find(index);
        if (hit != built.end()) return hit->second;
        PageText pg;
        try {
if (!opened || index < 0 || index >= (int)pages.size()) throw PdfError();
        } catch (PdfError&) {
            built[index] = std::move(pg);
            return built[index];
        }
        const Node& node = pages[index];
        double l, b, w, h;
        EffectiveBox(node.media, node.crop, l, b, w, h);
        pg.boxW = w;
        pg.boxH = h;
        long long rot = ((node.rotate % 360) + 360) % 360;
        if (rot % 90) rot = ((rot + 45) / 90) * 90 % 360;
        pg.rotation = (int)rot;
        // Contents: one stream or an array of them, joined with newlines,
        // bounded like KillerPDF's ContentBytes. A stream that will not decode
        // degrades to a notice, not to losing the page's other streams.
        const PdfObj* contents = ResolveInF(file, node.dict, "Contents");
        std::vector<uint8_t> all;
        bool truncated = false;
        if (contents) {
            std::vector<const PdfObj*> streams;
            if (contents->t == PdfObj::ARR) {
                for (const PdfObj& item : contents->arr) {
                    const PdfObj* s = item.t == PdfObj::REF ? file.Resolve(&item) : &item;
                    if (s) streams.push_back(s);
                }
            } else streams.push_back(contents);
            for (const PdfObj* s : streams) {
                if (s->t != PdfObj::STREAM) continue;
                std::vector<uint8_t> bytes;
                if (!file.StreamData(s, bytes)) { truncated = true; continue; }
                if (all.size() + bytes.size() + 1 > MaximumSourceBytes) { truncated = true; break; }
                all.insert(all.end(), bytes.begin(), bytes.end());
                all.push_back(10);
            }
        }
        pg.truncated = truncated;
        if (!all.empty()) {
            Interp interp;
            interp.file = &file;
            interp.fontIds = &fontIds;
            interp.fonts = &fonts;
            Matrix initial;                        // translate(-l, -b): cropbox-relative
            initial.E = -l;
            initial.F = -b;
            // Whatever landed before a failure stays - KillerPDF keeps the
            // truncated prefix instead of dropping the page's text.
            try {
                interp.Run(all, node.resources, initial, 0);
            } catch (PdfError&) {
            } catch (const std::exception&) {
            }
            pg.letters = std::move(interp.out);
        }
        GroupWords(pg.letters);
        pg.fffd = 0;
        for (const Letter& L : pg.letters)
            if (L.text.size() == 1 && L.text[0] == (wchar_t)0xFFFD) pg.fffd++;
        built[index] = std::move(pg);
        return built[index];
    }
};

} // namespace pdftext

#ifndef DOCSY_PDFTEXT_TEST
// -------------------------------------------------------------- Win32 glue --
// The session is built once per open document, from the file docsy already
// opened; it lives in DocState::ts and dies with the document.
namespace pdftext { struct PdfTextSession { Session s; bool ready = false; }; }

static pdftext::PdfTextSession* TtPdfSession(DocState& D) {
    if (D.ts) return D.ts;
    pdftext::PdfTextSession* ts = new pdftext::PdfTextSession();
    D.ts = ts;
    try {
        HANDLE fh = CreateFileW(D.path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) throw std::runtime_error("open");
        LARGE_INTEGER fsz{};
        if (!GetFileSizeEx(fh, &fsz) || fsz.QuadPart <= 0 ||
            fsz.QuadPart > (LONGLONG)pdftext::MaximumSourceBytes) {
            CloseHandle(fh);
            throw std::runtime_error("size");
        }
        std::vector<uint8_t> bytes((size_t)fsz.QuadPart);
        DWORD got = 0, rd = 0;
        while (got < bytes.size() &&
               ReadFile(fh, bytes.data() + got, (DWORD)(bytes.size() - got), &rd, nullptr) && rd)
            got += rd;
        CloseHandle(fh);
        if (got != bytes.size()) throw std::runtime_error("read");
        ts->ready = ts->s.Open(std::move(bytes));
    } catch (...) {
        ts->ready = false;
    }
    return ts;
}
static void TtPdfSessionDrop(DocState& D) {
    delete D.ts;
    D.ts = nullptr;
}
// The page's engine text, or null when this page takes the OCR fallback
// (encrypted, unreadable, scanned empty, or the extractor's page tree
// disagreeing with Windows').
static pdftext::PageText* TtPdfPage(DocState& D, int i) {
    pdftext::PdfTextSession* ts = TtPdfSession(D);
    if (!ts->ready || ts->s.file.encrypted) return nullptr;
    if (i < 0 || i >= (int)ts->s.pages.size() || (int)ts->s.pages.size() != D.n) return nullptr;
    pdftext::PageText& pg = ts->s.Page(i);
    return pg.failed ? nullptr : &pg;
}
#endif // DOCSY_PDFTEXT_TEST

#endif // DOCSY_PDF_TEXT_INL
