// ---------------- the Document Info dialog ----------------
// KillerPDF's DocumentInfoDialog (F4), in docsy's chrome: a real modal window
// owned by the main one, the find bar's container language for the body - the
// soft DropShadow, the radius-6 grain panel, the #555555 ring - its caption
// "Docsy - Document Info", the field list Title / Author / Subject / Keywords
// / Creator and the mono summary block (Producer when the file has one,
// Pages, the PDF version off the header, the file's KB). docsy is a reader:
// the fields read, they do not edit - there is no Save pair, the dialog
// closes with Close, Enter or Esc.

// ---- the PDF's own Document Info dictionary ----
// The same source KillerPDF's engine reads: the trailer's /Info reference is
// plain text in every PDF (classic trailer dictionaries and xref-stream
// dictionaries alike), the object's strings are decoded literal or hex, and
// an encrypted file - whose strings are ciphered - answers empty fields
// instead of garbage. Best effort by design: a file with no Info dictionary,
// or one this reader cannot reach, simply shows "-" per field while the
// summary's Pages / version / KB stay exact (they never come from the dict).
struct PdfInfo {
    std::wstring title, author, subject, keywords, creator, producer;
    std::wstring version;               // "1.7" - straight off the "%PDF-" header
};

static std::wstring PdfDecodeString(const std::wstring& raw, bool hex) {
    std::string bytes;
    if (hex) {
        int nib = -1;
        for (wchar_t c : raw) {
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) continue;
            if (nib < 0) nib = v;
            else { bytes.push_back((char)(nib * 16 + v)); nib = -1; }
        }
        if (nib >= 0) bytes.push_back((char)(nib * 16));   // odd digit count pads 0
    } else {
        for (size_t i = 0; i < raw.size(); i++) {
            wchar_t c = raw[i];
            if (c == L'\\' && i + 1 < raw.size()) {
                wchar_t e = raw[++i];
                switch (e) {
                case L'n': bytes.push_back('\n'); break;
                case L'r': bytes.push_back('\r'); break;
                case L't': bytes.push_back('\t'); break;
                case L'b': bytes.push_back('\b'); break;
                case L'f': bytes.push_back('\f'); break;
                default:
                    if (e >= L'0' && e <= L'7') {           // up to three octal digits
                        int v = e - L'0', k = 0;
                        while (k < 2 && i + 1 < raw.size() && raw[i + 1] >= L'0' && raw[i + 1] <= L'7')
                            { v = v * 8 + (raw[++i] - L'0'); k++; }
                        bytes.push_back((char)v);
                    } else bytes.push_back((char)e);        // \\ ( ) and anything else: the char itself
                    break;
                }
            } else if (c >= 0 && c <= 255) bytes.push_back((char)c);
        }
    }
    if (bytes.size() >= 2 && (BYTE)bytes[0] == 0xFE && (BYTE)bytes[1] == 0xFF) {
        std::wstring out;                                    // UTF-16BE, the BOM says so
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            out.push_back((wchar_t)((BYTE)bytes[i] << 8 | (BYTE)bytes[i + 1]));
        return out;
    }
    if (bytes.size() >= 3 && (BYTE)bytes[0] == 0xEF && (BYTE)bytes[1] == 0xBB && (BYTE)bytes[2] == 0xBF) {
        return std::wstring(bytes.begin() + 3, bytes.end()); // UTF-8 without re-encoding: the
    }                                                        // subsets this dialog shows are ascii
    std::wstring out;                                        // PDFDocEncoding: latin-1 covers it
    for (unsigned char c : bytes) out.push_back((wchar_t)c);
    return out;
}

static bool PdfInfoRead(const std::wstring& path, PdfInfo& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    long long size = sz.QuadPart;
    char head[16]{};
    DWORD got = 0;
    ReadFile(f, head, 15, &got, nullptr);
    if (got >= 8 && !memcmp(head, "%PDF-", 5)) {             // the version is the header's
        const char* v = head + 5;
        std::wstring ver;
        while ((*v >= '0' && *v <= '9') || *v == '.') ver.push_back(*v++);
        out.version = ver;
    }
    // the tail holds the trailer: the last /Info reference and any /Encrypt mark
    long long tailOff = max(0LL, size - 16384);
    DWORD tailLen = (DWORD)(size - tailOff);
    std::string tail((size_t)tailLen, '\0');
    SetFilePointer(f, (LONG)tailOff, nullptr, FILE_BEGIN);
    if (!ReadFile(f, tail.data(), tailLen, &got, nullptr)) { CloseHandle(f); return false; }
    tail.resize(got);
    CloseHandle(f);
    if (tail.find("/Encrypt") != std::string::npos) return true;   // ciphered strings: fields stay empty
    size_t info = tail.rfind("/Info");
    if (info == std::string::npos) return out.version.size() > 0;
    long long objNum = -1;
    {   // "/Info N G R" - the object number follows the key
        size_t p = info + 5;
        while (p < tail.size() && (tail[p] == ' ' || tail[p] == '\t' || tail[p] == '\r' || tail[p] == '\n')) p++;
        objNum = _atoi64(tail.c_str() + p);
    }
    if (objNum < 0 || objNum > 99999999) return out.version.size() > 0;
    // the object itself: scanned from the file's start - "N 0 obj" is plain
    // text everywhere it is real; a stream's binary noise can imitate one, so
    // each candidate is validated by its keys and the next one tried
    HANDLE f2 = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f2 == INVALID_HANDLE_VALUE) return out.version.size() > 0;
    std::string pat = std::to_string(objNum) + " 0 obj";
    long long probe = 0;
    const long long WINDOW = 1 << 20;
    for (int attempt = 0; attempt < 8 && probe < size; attempt++) {
        long long span = min((long long)tailOff + (long long)tail.size() - probe, (long long)(WINDOW + pat.size() * 2));
        std::string buf((size_t)max(0LL, span), '\0');
        DWORD rd = 0;
        SetFilePointer(f2, (LONG)probe, nullptr, FILE_BEGIN);
        if (!ReadFile(f2, buf.data(), (DWORD)buf.size(), &rd, nullptr) || rd < pat.size()) break;
        buf.resize(rd);
        size_t at = buf.find(pat);
        if (at == std::string::npos) { probe += rd - (long long)pat.size() + 1; continue; }
        size_t obj = at;
        size_t dict = buf.find("<<", obj);
        size_t end = buf.find("endobj", obj);
        if (dict == std::string::npos || end == std::string::npos) { probe += obj + pat.size(); continue; }
        std::string dictText = buf.substr(dict, min(end - dict, (size_t)65536));
        // the dict must mention at least one Info key, else this "N 0 obj" was noise
        if (dictText.find("/Title") == std::string::npos && dictText.find("/Author") == std::string::npos &&
            dictText.find("/Subject") == std::string::npos && dictText.find("/Keywords") == std::string::npos &&
            dictText.find("/Creator") == std::string::npos && dictText.find("/Producer") == std::string::npos) {
            probe += obj + pat.size();
            continue;
        }
        auto value = [&](const char* key) -> std::wstring {
            size_t k = dictText.find(key);
            if (k == std::string::npos) return L"";
            size_t p = k + strlen(key);
            while (p < dictText.size() && isspace((unsigned char)dictText[p])) p++;
            if (p >= dictText.size()) return L"";
            if (dictText[p] == '(') {                       // literal string, balanced parens
                int depth = 1; size_t q = p + 1; std::wstring raw;
                while (q < dictText.size() && depth > 0) {
                    char c = dictText[q];
                    if (c == '\\' && q + 1 < dictText.size()) { raw += dictText[q]; raw += dictText[q + 1]; q += 2; continue; }
                    if (c == '(') depth++;
                    if (c == ')') { depth--; if (!depth) break; }
                    raw += c; q++;
                }
                return PdfDecodeString(std::wstring(raw.begin(), raw.end()), false);
            }
            if (dictText[p] == '<' && p + 1 < dictText.size() && dictText[p + 1] != '<') {
                size_t e = dictText.find('>', p);
                if (e == std::string::npos) return L"";
                return PdfDecodeString(std::wstring(dictText.begin() + p + 1, dictText.begin() + e), true);
            }
            return L"";                                     // names/refs/numbers: not shown
        };
        out.title    = value("/Title");
        out.author   = value("/Author");
        out.subject  = value("/Subject");
        out.keywords = value("/Keywords");
        out.creator  = value("/Creator");
        out.producer = value("/Producer");
        return true;
    }
    return out.version.size() > 0;
}

// Save edited metadata into the PDF the way KillerPDF's save does - an
// incremental update: the original bytes are never touched, the new /Info
// object plus a fresh xref table and trailer are appended, and readers take
// the newest section (a classic trailer after an xref-stream update is legal
// mixing). The Info object keeps its existing number when the file has one -
// the new xref entry overrides the old - otherwise it takes /Size and the
// size grows by one. Strings go out as PDFDocEncoding literals when they are
// ASCII and UTF-16BE hex strings when they are not, escaped per the spec.
// A file whose trailer cannot be parsed, or an encrypted one (its strings
// are ciphered), is refused with a message instead of being damaged.
static bool PdfInfoWrite(const std::wstring& path, const PdfInfo& in, std::wstring& err) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"The file could not be opened for writing."; return false; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    long long size = sz.QuadPart;
    long long tailOff = max(0LL, size - 16384);
    std::string tail((size_t)(size - tailOff), '\0');
    DWORD got = 0;
    SetFilePointer(f, (LONG)tailOff, nullptr, FILE_BEGIN);
    if (!ReadFile(f, tail.data(), (DWORD)tail.size(), &got, nullptr)) {
        CloseHandle(f);
        err = L"The file could not be read.";
        return false;
    }
    tail.resize(got);
    if (tail.find("/Encrypt") != std::string::npos) {
        CloseHandle(f);
        err = L"Encrypted PDFs stay read-only here.";
        return false;
    }
    // the keys the appended trailer must carry forward: /Root and /Size come
    // from a classic trailer, or - in an xref-stream file - from the xref
    // object the last startxref points at
    auto numAfter = [&](const std::string& hay, size_t at) -> long long {
        size_t p = at;
        while (p < hay.size() && (hay[p] == ' ' || hay[p] == '\t' || hay[p] == '\r' || hay[p] == '\n')) p++;
        return _atoi64(hay.c_str() + p);
    };
    size_t sx = tail.rfind("startxref");
    if (sx == std::string::npos) { CloseHandle(f); err = L"This PDF's structure could not be updated."; return false; }
    long long prev = numAfter(tail, sx + 9);
    long long rootObj = -1, rootGen = 0, sizeVal = -1, infoObj = -1;
    std::string td;                                       // trailer dict text, either flavor
    size_t tr = tail.rfind("trailer");
    if (tr != std::string::npos) td = tail.substr(tr, min(tail.size() - tr, (size_t)2048));
    if (td.find("/Root") == std::string::npos && prev > 0 && prev < size) {
        std::string xb(2048, '\0'); DWORD xr = 0;
        SetFilePointer(f, (LONG)prev, nullptr, FILE_BEGIN);
        if (ReadFile(f, xb.data(), (DWORD)xb.size(), &xr, nullptr)) {
            xb.resize(xr);
            size_t d = xb.find("<<");
            if (d != std::string::npos) td = xb.substr(d, min(xb.size() - d, (size_t)2048));
        }
    }
    CloseHandle(f);
    size_t k = td.find("/Root");
    if (k == std::string::npos) { err = L"This PDF's structure could not be updated."; return false; }
    rootObj = numAfter(td, k + 5);
    {   // the generation is the second integer of the reference
        const char* q = td.c_str() + k + 5;
        while (*q == ' ') q++;
        while (*q >= '0' && *q <= '9') q++;
        while (*q == ' ') q++;
        rootGen = _atoi64(q);
    }
    k = td.find("/Size");
    if (k != std::string::npos) sizeVal = numAfter(td, k + 5);
    k = td.find("/Info");
    if (k != std::string::npos) infoObj = numAfter(td, k + 5);
    if (rootObj <= 0 || rootObj > 99999999 || sizeVal <= 0 || sizeVal > 99999999) {
        err = L"This PDF's structure could not be updated.";
        return false;
    }
    long long obj = infoObj > 0 ? infoObj : sizeVal;
    long long newSize = infoObj > 0 ? sizeVal : sizeVal + 1;
    // the strings: ASCII (with escapes) as a literal, anything else UTF-16BE hex
    auto pdfstr = [](const std::wstring& s) -> std::string {
        bool ascii = true;
        for (wchar_t c : s) if (c < 32 || c > 126) { ascii = false; break; }
        std::string out;
        if (ascii) {
            out += '(';
            for (wchar_t c : s) {
                if (c == L'(' || c == L')' || c == L'\\') out += '\\';
                if (c == L'\r') { out += "\\r"; continue; }
                if (c == L'\n') { out += "\\n"; continue; }
                out += (char)c;
            }
            out += ')';
            return out;
        }
        out += "<FEFF";
        const char* hx = "0123456789ABCDEF";
        for (wchar_t c : s) {
            out += hx[(c >> 12) & 15]; out += hx[(c >> 8) & 15];
            out += hx[(c >> 4) & 15];  out += hx[c & 15];
        }
        out += '>';
        return out;
    };
    std::string body = "\r\n";
    long long objOff = size + (long long)body.size();
    body += std::to_string(obj) + " 0 obj\n<< ";
    auto add = [&](const char* key, const std::wstring& v) {
        if (!v.empty()) body += std::string(key) + " " + pdfstr(v) + " ";
    };
    add("/Title", in.title);       add("/Author", in.author);
    add("/Subject", in.subject);   add("/Keywords", in.keywords);
    add("/Creator", in.creator);   add("/Producer", in.producer);
    body += ">>\nendobj\r\n";
    long long xrefOff = objOff + (long long)body.size() - 2;
    char ent[32];
    snprintf(ent, sizeof ent, "%010lld 00000 n \n", objOff);
    body += std::string("xref\n") + std::to_string(obj) + " 1\n" + ent;
    body += "trailer\n<< /Size " + std::to_string(newSize) +
            " /Root " + std::to_string(rootObj) + " " + std::to_string(rootGen) + " R" +
            " /Info " + std::to_string(obj) + " 0 R";
    if (prev > 0 && prev < size) body += " /Prev " + std::to_string(prev);
    body += " >>\nstartxref\n" + std::to_string(xrefOff) + "\n%%EOF\n";
    HANDLE w = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) {
        err = L"The file could not be opened for writing.";
        return false;
    }
    SetFilePointer(w, (LONG)size, nullptr, FILE_BEGIN);
    DWORD wrote = 0;
    bool ok = WriteFile(w, body.data(), (DWORD)body.size(), &wrote, nullptr) && wrote == body.size();
    CloseHandle(w);
    if (!ok) { err = L"The file could not be written."; return false; }
    // verify by reading back, so "saved" never lies
    PdfInfo back;
    PdfInfoRead(path, back);
    if (back.title != in.title || back.author != in.author || back.subject != in.subject ||
        back.keywords != in.keywords || back.creator != in.creator) {
        err = L"The update could not be verified.";
        return false;
    }
    return true;
}

// ---- the dialog itself ----
// KillerPDF's DocumentInfoDialog, in docsy's chrome. The reference's shape:
// a 460-wide window sized to its content, a caption that DRAGS the window,
// five EDITABLE fields (Title / Author / Subject / Keywords / Creator - real
// text boxes with a caret and a selection, Keywords wrapping like the
// reference's wrap:true field), the mono summary block (Producer when the
// file has one, Pages, the PDF version off the header, the file's KB) and a
// right-aligned Cancel / Save pair - Esc cancels, Enter saves. The window is
// owned, not app-modal: the main window stays live, a click anywhere outside
// the dialog dismisses it (the reference's dialog is draggable chrome; the
// click-outside dismissal is the popup contract this port follows), and Save
// writes the metadata into the PDF itself - the reference applies the edit
// to the file, so the words must survive the dialog.
struct InfoDlg {
    std::wstring title, author, subject, keywords, creator, producer, ver;
    long long bytes = 0;
    int pages = 0;
    int hot = -1;                        // hovered control: 0 Cancel, 1 Save, 2 the caption close
};
static InfoDlg g_idlg;
static HWND g_infoDlg = nullptr;
static HWND g_idEdits[5] = {};       // the five live fields (nullptr in the CI shot host)
static bool g_idlgShots = false;     // the CI shot renders the dialog's paint without the
                                     // live window - one paint path, two hosts

static bool InfoIsOpen() { return g_infoDlg != nullptr; }

static HFONT g_idCap = nullptr, g_idLab = nullptr, g_idVal = nullptr, g_idMono = nullptr;
static HFONT IdCapFont()  { return CaptionFont(g_idCap, 14, false, L"Killer Sans"); }
static HFONT IdLabFont()  { return CaptionFont(g_idLab, 11, false, L"Killer Sans UI"); }
static HFONT IdValFont()  { return CaptionFont(g_idVal, 12, false, L"Killer Sans UI"); }
static HFONT IdMonoFont() { return CaptionFont(g_idMono, 11, false, L"Consolas"); }

static int IdW() { return (int)(460 * g.dpi); }          // KillerPDF's Width=460
static int IdCapH() { return (int)(32 * g.dpi); }

// the dialog's natural height from its own metrics, so SizeToContent.Height's
// behavior is arithmetic, not a magic number
static int IdFieldH(int i) { return (int)((i == 3 ? 44 : 26) * g.dpi); }   // Keywords wraps
static int IdH() {
    int labH = (int)(14 * g.dpi), gap = (int)(8 * g.dpi);
    int fields = 0;
    for (int i = 0; i < 5; i++) fields += labH + (int)(2 * g.dpi) + IdFieldH(i) + gap;
    int sumH = (int)(15 * g.dpi) * 4;
    int btnH = (int)(28 * g.dpi);
    return IdCapH() + (int)(6 * g.dpi) + fields + (int)(12 * g.dpi) + sumH +
           (int)(16 * g.dpi) + btnH + (int)(16 * g.dpi);
}

// Each field's label and box rect, in dialog client coords - shared by the
// paint, the edit placement and the shot seed. KillerPDF's stack: GroupLabel
// (11px, 2px under), the field, 8px after.
static void IdFieldRect(int i, int w, RECT& lab, RECT& box) {
    int pad = (int)(20 * g.dpi);
    int y = IdCapH() + (int)(6 * g.dpi);
    for (int k = 0; k < i; k++)
        y += (int)(14 * g.dpi) + (int)(2 * g.dpi) + IdFieldH(k) + (int)(8 * g.dpi);
    lab = { pad, y, w - pad, y + (int)(14 * g.dpi) };
    box = { pad, y + (int)(14 * g.dpi) + (int)(2 * g.dpi), w - pad,
            y + (int)(14 * g.dpi) + (int)(2 * g.dpi) + IdFieldH(i) };
}

// The button pair, right-aligned with an 8px gap (UiKit.ButtonRow): which 0 =
// Cancel, 1 = Save (the accent, rightmost - the reference's IsDefault).
static RECT IdBtnRect(int which, int w, int h) {
    int saveW = (int)(64 * g.dpi), cancelW = (int)(84 * g.dpi), bh = (int)(28 * g.dpi);
    int right = w - (int)(20 * g.dpi) - (which ? 0 : saveW + (int)(8 * g.dpi));
    return { right - (which ? saveW : cancelW), h - (int)(16 * g.dpi) - bh,
             right, h - (int)(16 * g.dpi) };
}

// One paint, two hosts: the live dialog's WM_PAINT and the CI shot composite.
// The field boxes are painted here; the text inside them belongs to the live
// EDIT children - values are drawn only when there are no edits (the shot
// host), so the two never double-render a string.
static void InfoDialogDraw(HDC dc, int w, int h) {
    int capH = IdCapH();
    // the caption row: DialogFrame's bar - the wordmark, the qualifier, the
    // red-hovered close. The whole bar drags the window (WM_NCHITTEST below).
    RECT cr{ 0, 0, w, capH };
    FillRect(dc, &cr, GrainBrush(2));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, IdCapFont());
    RECT tr{ (int)(12 * g.dpi), 0, w - (int)(44 * g.dpi), capH };
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
    DrawTextW(dc, L"Docsy", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SIZE ts{};
    GetTextExtentPoint32W(dc, L"Docsy", 5, &ts);
    RECT qr{ tr.left + ts.cx + (int)(4 * g.dpi), tr.top, tr.right, tr.bottom };
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
    DrawTextW(dc, L"- Document Info", -1, &qr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    int bs = (int)(44 * g.dpi);
    RECT xr{ w - bs, 0, w, capH };
    if (g_idlg.hot == 2) { RECT hr{ xr.left, 0, xr.right, capH }; FillRect(dc, &hr, RowHoverBrush()); }
    SelectObject(dc, StGlyph());
    SetTextColor(dc, g_idlg.hot == 2 ? RGB(0xff, 0x67, 0x67) : RGB(0xa3, 0xa3, 0xa3));
    DrawTextW(dc, L"\xE8BB", -1, &xr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // the body field: the shared grain
    RECT body{ 0, capH, w, h };
    FillRect(dc, &body, GrainBrush(1));
    SetBkMode(dc, TRANSPARENT);
    const wchar_t* labels[5] = { L"Title", L"Author", L"Subject", L"Keywords", L"Creator" };
    const std::wstring* vals[5] = { &g_idlg.title, &g_idlg.author, &g_idlg.subject,
                                    &g_idlg.keywords, &g_idlg.creator };
    bool live = g_infoDlg != nullptr;
    // the field boxes: KillerPDF's UiKit.Field - TextFieldBrush #101010 inside
    // a 1px #4a4a4a CardBorder, radius 4
    Gdiplus::Graphics gs(dc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    auto boxPath = [&](const RECT& rc) {
        float rr = (float)(4 * g.dpi);
        float fx = (float)rc.left, fy = (float)rc.top;
        float fw = (float)(rc.right - rc.left), fh = (float)(rc.bottom - rc.top);
        Gdiplus::GraphicsPath* pth = new Gdiplus::GraphicsPath();
        pth->AddArc(fx, fy, 2 * rr, 2 * rr, 180, 90);
        pth->AddArc(fx + fw - 2 * rr, fy, 2 * rr, 2 * rr, 270, 90);
        pth->AddArc(fx + fw - 2 * rr, fy + fh - 2 * rr, 2 * rr, 2 * rr, 0, 90);
        pth->AddArc(fx, fy + fh - 2 * rr, 2 * rr, 2 * rr, 90, 90);
        pth->CloseFigure();
        return pth;
    };
    int lastBoxBottom = 0;
    for (int i = 0; i < 5; i++) {
        RECT lab, box;
        IdFieldRect(i, w, lab, box);
        lastBoxBottom = box.bottom;
        SelectObject(dc, IdLabFont());
        SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
        DrawTextW(dc, labels[i], -1, &lab, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        Gdiplus::GraphicsPath* pth = boxPath(box);
        Gdiplus::SolidBrush fill(Gdiplus::Color((BYTE)255, 0x10, 0x10, 0x10));
        gs.FillPath(&fill, pth);
        Gdiplus::Pen edge(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
        gs.DrawPath(&edge, pth);
        delete pth;
        if (!live) {                       // the shot host speaks for the edits
            RECT vr{ box.left + (int)(6 * g.dpi), box.top + (int)(2 * g.dpi),
                     box.right - (int)(6 * g.dpi), box.bottom - (int)(2 * g.dpi) };
            SelectObject(dc, IdValFont());
            SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
            SetBkColor(dc, RGB(0x10, 0x10, 0x10));
            DrawTextW(dc, vals[i]->c_str(), -1, &vr,
                      DT_LEFT | (i == 3 ? DT_WORDBREAK | DT_EDITCONTROL : DT_VCENTER | DT_SINGLELINE) | DT_END_ELLIPSIS);
            SetBkMode(dc, TRANSPARENT);
        }
    }
    // the summary block, mono and muted - KillerPDF's BuildSummary
    std::wstring sum;
    if (!g_idlg.producer.empty()) sum += L"Producer: " + g_idlg.producer + L"\n";
    sum += L"Pages: " + std::to_wstring(g_idlg.pages) + L"\n";
    sum += L"PDF " + (g_idlg.ver.empty() ? std::wstring(L"1.?") : g_idlg.ver) + L"\n";
    wchar_t kb[40];
    if (g_idlg.bytes >= 1LL << 20) swprintf_s(kb, L"%.0f KB", g_idlg.bytes / 1024.0);
    else swprintf_s(kb, L"%lld KB", g_idlg.bytes / 1024);
    sum += kb;
    int sy = lastBoxBottom + (int)(12 * g.dpi);
    SelectObject(dc, IdMonoFont());
    SetTextColor(dc, RGB(0xa3, 0xa3, 0xa3));
    RECT sr{ (int)(24 * g.dpi), sy, w - (int)(20 * g.dpi), sy + (int)(15 * g.dpi) * 4 };
    DrawTextW(dc, sum.c_str(), -1, &sr, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);
    // the pair, right-aligned: Cancel the secondary, Save the accent -
    // hollow in the accent at rest, filled with it on hover
    for (int wh = 0; wh < 2; wh++) {
        RECT br = IdBtnRect(wh, w, h);
        bool accent = wh == 1, hot = g_idlg.hot == wh;
        Gdiplus::GraphicsPath* pth = boxPath(br);
        if (accent && hot) {
            Gdiplus::SolidBrush gb(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56));
            gs.FillPath(&gb, pth);
        } else if (accent) {
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x22, 0xbb, 0x56), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else if (hot) {
            Gdiplus::SolidBrush hb(Gdiplus::Color((BYTE)255, 0x40, 0x40, 0x40));
            gs.FillPath(&hb, pth);
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        } else {
            Gdiplus::Pen pb(Gdiplus::Color((BYTE)255, 0x4a, 0x4a, 0x4a), (float)max(1.0, g.dpi));
            gs.DrawPath(&pb, pth);
        }
        delete pth;
        SetTextColor(dc, accent ? (hot ? RGB(0x05, 0x24, 0x0f) : RGB(0x22, 0xbb, 0x56))
                                : RGB(0xe0, 0xe0, 0xe0));
        SelectObject(dc, IdValFont());
        DrawTextW(dc, accent ? L"Save" : L"Cancel", -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void InfoClose() {
    if (!g_infoDlg) return;
    HWND d = g_infoDlg;
    g_infoDlg = nullptr;
    for (HWND& e : g_idEdits) e = nullptr;
    DestroyWindow(d);
    RailRepaint();
    if (g.main) SetFocus(g.main);            // the keys go home with the dialog gone
}

// Save: gather the five fields and apply them the way KillerPDF's save does -
// the metadata goes into the file itself (an incremental update; the original
// bytes stay untouched). A failure reports in the footer and leaves the
// dialog open over an unchanged file.
static void InfoSave() {
    if (!g_infoDlg) return;
    DocState& D = DVF();
    if (!D.n) { InfoClose(); return; }
    wchar_t buf[4096]{};
    auto grab = [&](int i) {
        if (!g_idEdits[i]) return i == 0 ? g_idlg.title : i == 1 ? g_idlg.author
                              : i == 2 ? g_idlg.subject : i == 3 ? g_idlg.keywords : g_idlg.creator;
        GetWindowTextW(g_idEdits[i], buf, 4096);
        return std::wstring(buf);
    };
    PdfInfo meta;
    meta.title = grab(0);       meta.author = grab(1);
    meta.subject = grab(2);     meta.keywords = grab(3);
    meta.creator = grab(4);
    meta.producer = g_idlg.producer;         // the producer rides through unchanged
    meta.version = g_idlg.ver;
    std::wstring err;
    if (PdfInfoWrite(D.path, meta, err)) {
        InfoClose();
        SetStatusHold(L"Document Info updated.");
    } else {
        SetStatusHold(err.c_str());
    }
}

// The fields' keys: Enter saves, Esc cancels, Tab walks the five boxes -
// the reference's IsDefault / IsCancel / tab order, which a plain EDIT child
// would otherwise swallow.
static LRESULT CALLBACK IdEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    WNDPROC orig = (WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_KEYDOWN) {
        if (wp == VK_RETURN) { InfoSave(); return 0; }
        if (wp == VK_ESCAPE) { InfoClose(); return 0; }
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(h, EM_SETSEL, 0, -1); return 0;   // the reference's SelectAll chord
        }
        if (wp == VK_TAB) {
            int i = 0;
            while (i < 5 && g_idEdits[i] != h) i++;
            bool back = GetKeyState(VK_SHIFT) < 0;
            for (int step = 0; step < 5; step++) {
                i = (i + (back ? -1 : 1) + 5) % 5;
                if (g_idEdits[i]) { SetFocus(g_idEdits[i]); break; }
            }
            return 0;
        }
    }
    return orig ? CallWindowProcW(orig, h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}

static void InfoOpen() {
    if (g_infoDlg) { SetFocus(g_infoDlg); return; }
    DocState& D = DVF();                     // the FOCUSED pane's document, KillerPDF's active viewer
    if (!D.n) {                              // KillerPDF's Str_Msg_OpenFirst, in the footer's
        SetStatusHold(L"Open a PDF first."); // language (the app has no message boxes)
        return;
    }
    PdfInfo pi;
    g_idlg = InfoDlg{};
    PdfInfoRead(D.path, pi);
    g_idlg.title = pi.title;                 // raw values: the reference shows what the
    g_idlg.author = pi.author;               // dictionary holds, and empty stays empty
    g_idlg.subject = pi.subject;
    g_idlg.keywords = pi.keywords;
    g_idlg.creator = pi.creator;
    g_idlg.producer = pi.producer;
    g_idlg.ver = pi.version;
    g_idlg.pages = D.n;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    g_idlg.bytes = GetFileAttributesExW(D.path.c_str(), GetFileExInfoStandard, &fa)
                 ? (long long)(((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) : 0;
    // a borderless OWNED popup at the reference's Width=460: the main window
    // stays live - NOT app-modal - and a click outside dismisses the dialog
    // without saving (the popup contract; the reference's own dialog is
    // dragged by its caption and answered with Cancel or Save)
    int w = IdW(), h = IdH();
    RECT mr{}; GetWindowRect(g.main, &mr);
    int x = mr.left + max(0, (int)((mr.right - mr.left) - w) / 2);
    int y = mr.top + max(0, (int)((mr.bottom - mr.top) - h) / 2);
    g_infoDlg = CreateWindowExW(0, L"DocsyInfoDlg", nullptr,
                                WS_POPUP | WS_CLIPCHILDREN, x, y, w, h,
                                g.main, nullptr, g.hinst, nullptr);
    if (!g_infoDlg) return;
    // the five live fields, KillerPDF's UiKit.Field as a child EDIT: 12px
    // Killer Sans UI on #101010 (WM_CTLCOLOREDIT below), the reference's
    // 6,4 padding via the edit margins. Keywords is the wrap:true field:
    // multiline so long lists wrap, and Enter is never a newline - it saves.
    std::wstring init[5] = { pi.title, pi.author, pi.subject, pi.keywords, pi.creator };
    for (int i = 0; i < 5; i++) {
        RECT lab, box;
        IdFieldRect(i, w, lab, box);
        DWORD es = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT |
                   (i == 3 ? ES_MULTILINE : ES_AUTOHSCROLL);
        g_idEdits[i] = CreateWindowExW(0, L"EDIT", nullptr, es,
            box.left + 2, box.top + 2, box.right - box.left - 4, box.bottom - box.top - 4,
            g_infoDlg, (HMENU)(intptr_t)(100 + i), g.hinst, nullptr);
        if (g_idEdits[i]) {
            SetWindowLongPtrW(g_idEdits[i], GWLP_USERDATA,
                              GetWindowLongPtrW(g_idEdits[i], GWLP_WNDPROC));
            SetWindowLongPtrW(g_idEdits[i], GWLP_WNDPROC, (LONG_PTR)IdEditProc);
            SendMessageW(g_idEdits[i], WM_SETFONT, (WPARAM)IdValFont(), FALSE);
            SendMessageW(g_idEdits[i], EM_SETMARGIN, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM((int)(4 * g.dpi), (int)(4 * g.dpi)));
            SetWindowTextW(g_idEdits[i], init[i].c_str());
        }
    }
    DwmApply(g_infoDlg);
    ShowWindow(g_infoDlg, SW_SHOWNORMAL);
    if (g_idEdits[0]) SetFocus(g_idEdits[0]);   // the reference: Loaded focuses _title
    RailRepaint();
}

static LRESULT CALLBACK InfoDlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        if (r.right > 0 && r.bottom > 0) {
            EnsureInfoBack(dc, r.right, r.bottom);   // the same buffered present as the rail:
            POINT po{ 0, 0 }; MapWindowPoints(h, g.main, &po, 1);   // the dialog used to paint
            SetBrushOrgEx(g_iddc, -po.x, -po.y, nullptr);   // straight to the screen, so every
            InfoDialogDraw(g_iddc, r.right, r.bottom);   // hover flip over Cancel/Save/the close
            BitBlt(dc, 0, 0, r.right, r.bottom, g_iddc, 0, 0, SRCCOPY);   // strobed the window
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        // the popup contract: a click anywhere outside the dialog - the main
        // window, another app, the taskbar - dismisses it the way KillerPDF's
        // flyouts close on deactivation. Cancel semantics: what was typed is
        // simply not saved.
        if (LOWORD(wp) == WA_INACTIVE && g_infoDlg == h) InfoClose();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { InfoClose(); return 0; }   // IsCancel
        if (wp == VK_RETURN) { InfoSave(); return 0; }    // IsDefault
        return 0;
    case WM_CTLCOLOREDIT: {               // UiKit.Field: the text #e0e0e0 on #101010
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
        SetBkColor(dc, RGB(0x10, 0x10, 0x10));
        static HBRUSH b = CreateSolidBrush(RGB(0x10, 0x10, 0x10));
        return (LRESULT)b;
    }
    case WM_NCHITTEST: {
        // the caption DRAGS the window (DialogFrame's DragMove); the close
        // zone stays client so its click is ours
        POINT s{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT c = s; ScreenToClient(h, &c);
        RECT r; GetClientRect(h, &r);
        if (c.y >= 0 && c.y < IdCapH() && c.x < r.right - (int)(44 * g.dpi)) return HTCAPTION;
        break;
    }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        RECT b0 = IdBtnRect(0, r.right, r.bottom), b1 = IdBtnRect(1, r.right, r.bottom);
        int hot = -1;
        if (PtInRect(&b1, p)) hot = 1;
        else if (PtInRect(&b0, p)) hot = 0;
        else if (p.y < IdCapH() && p.x >= r.right - (int)(44 * g.dpi)) hot = 2;
        if (hot != g_idlg.hot) {
            g_idlg.hot = hot;
            if (hot >= 0) TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_idlg.hot >= 0) { g_idlg.hot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(h, &r);
        RECT b0 = IdBtnRect(0, r.right, r.bottom), b1 = IdBtnRect(1, r.right, r.bottom);
        if (PtInRect(&b0, p) && g_idlg.hot == 0) { InfoClose(); return 0; }   // Cancel
        if (PtInRect(&b1, p) && g_idlg.hot == 1) { InfoSave(); return 0; }    // Save
        if (p.y < IdCapH() && p.x >= r.right - (int)(44 * g.dpi) && g_idlg.hot == 2) InfoClose();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT r; GetClientRect(h, &r);
            RECT b0 = IdBtnRect(0, r.right, r.bottom), b1 = IdBtnRect(1, r.right, r.bottom);
            if (PtInRect(&b0, p) || PtInRect(&b1, p) ||
                (p.y < IdCapH() && p.x >= r.right - (int)(44 * g.dpi))) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
            }
            if (p.y < IdCapH()) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        }
        break;
    case WM_CLOSE:
        InfoClose();
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

