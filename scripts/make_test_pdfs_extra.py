"""Regression-test generators for the real-world recovery paths: the shapes
actual Arabic government PDFs carry, which strict parsing lost to the OCR
fallback. Every generator builds one file and one failure mode."""
import zlib


class Builder:
    """A classic cross-reference PDF assembled from objects."""
    def __init__(self):
        self.objs = {}
    def add(self, num, body):
        self.objs[num] = body if isinstance(body, bytes) else body.encode("latin-1")
    def build(self, root_num):
        head = b"%PDF-1.7\n%\xE2\xE3\xCF\xD3\n"
        body = bytearray(head)
        offsets = {}
        for num in sorted(self.objs):
            offsets[num] = len(body)
            body += b"%d 0 obj\n" % num + self.objs[num] + b"\nendobj\n"
        xref_at = len(body)
        count = max(self.objs) + 1
        body += b"xref\n0 %d\n" % count
        body += b"0000000000 65535 f \n"
        for num in range(1, count):
            if num in offsets:
                body += b"%010d 00000 n \n" % offsets[num]
            else:
                body += b"0000000000 65535 f \n"
        body += b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (count, root_num, xref_at)
        return bytes(body)


def stream_obj(dict_extra, data):
    return ("<< /Length %d %s >>\nstream\n" % (len(data), dict_extra)).encode("latin-1") + data + b"\nendstream"


def hexstr(b):
    return "<" + b.hex() + ">"


def tounicode_cmap(entries):
    lines = ["/CIDInit /ProcSet findresource begin",
             "12 dict begin", "begincmap", "1 begincodespacerange", "<0000> <FFFF> endcodespacerange"]
    lines.append("%d beginbfchar" % len(entries))
    for code, uni in entries:
        dest = bytes.fromhex("%04X" % ord(uni))
        lines.append("%s %s" % (hexstr(code), hexstr(dest)))
    lines.append("endbfchar")
    lines.append("endcmap")
    lines.append("CMapName currentdict /CMap defineresource pop")
    lines.append("end end")
    return "\n".join(lines).encode("latin-1")


def make_hybrid_xref(b):
    """A hybrid-reference file, the shape the real government PDFs use: the
    last revision is a classic table (CRLF endings!) whose /XRefStm carries the
    objects the update moved into an object stream. The catalog, page tree and
    content all live in the ObjStm, so the text only extracts when the walk
    reads the XRefStm before the /Prev chain and parses CRLF tables by token."""
    content = b"BT /F1 18 Tf 72 720 Td <0001000200030004> Tj ET\n"
    # object stream holding: 1 catalog, 2 pages, 3 page, 4 font, 5 content
    obj_bodies = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           b"/Resources << /Font << /F1 4 0 R >> >> /Contents 8 0 R >>",
        4: b"<< /Type /Font /Subtype /Type0 /BaseFont /Ar /Encoding /Identity-H "
           b"/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Ar "
           b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> >>] "
           b"/ToUnicode 6 0 R >>",
    }
    header = ""
    payload = bytearray()
    ordinal = {}
    for k, num in enumerate(sorted(obj_bodies)):
        ordinal[num] = k
        header += "%d %d " % (num, len(payload))
        payload += obj_bodies[num] + b" "
    stm = stream_obj("/Type /ObjStm /N %d /First %d /Filter /FlateDecode"
                     % (len(obj_bodies), len(header.encode("latin-1"))),
                     zlib.compress(header.encode("latin-1") + bytes(payload)))
    uni = tounicode_cmap([
        (b"\x00\x01", "\u0627"), (b"\x00\x02", "\u0644"),
        (b"\x00\x03", "\u0645"), (b"\x00\x04", "\u0643")])
    # Streams stay top-level (the spec forbids them inside ObjStms): the page's
    # content stream is object 8, placed after the cross-reference stream.
    content_at_placeholder = None
    head = b"%PDF-1.5\r\n%\xE2\xE3\xCF\xD3\r\n"
    body = bytearray(head)

    def emit(num, data):
        at = len(body)
        body.extend(b"%d 0 obj\r\n" % num + data + b"\r\nendobj\r\n")
        return at

    uni_at = emit(6, stream_obj("", uni))
    stm_at = emit(7, stm)
    # the first revision: a classic table with CRLF endings covering 0..8
    xref1_at = len(body)
    body += b"xref\r\n0 9\r\n"
    body += b"0000000000 65535 f\r\n"
    for _ in range(1, 7):
        body += b"0000000000 00000 f\r\n"          # objects 1-4, 6, 7: rev-2 homes
    body += b"0000000000 00000 f\r\n"
    body += b"%010d 00000 n\r\n" % uni_at
    body += b"%010d 00000 n\r\n" % stm_at
    body += (b"trailer\r\n<< /Size 8 /Root 1 0 R >>\r\nstartxref\r\n%d\r\n"
             b"%%%%EOF\r\n" % xref1_at)
    # the update: an empty classic table + trailer; the cross-reference stream
    # itself closes the file. The content stream (object 8) is placed BEFORE it,
    # so its offset is known when the rows are built.
    content_at = len(body)
    body += b"8 0 obj\r\n" + stream_obj("", content) + b"\r\nendobj\r\n"
    xref2_at = len(body)
    body += b"xref\r\n0 0\r\ntrailer\r\n<< /Size 9 /Root 1 0 R /Prev %d /XRefStm " % xref1_at
    patch_at = len(body)
    body += b"0000000000" + b" >>\r\nstartxref\r\n%d\r\n%%%%EOF\r\n" % xref2_at
    # the cross-reference stream: type-2 rows for objects 1-4 in ObjStm 7, and
    # type-1 rows for the top-level streams 6, 7 and 8.
    rows = bytearray()
    rows += bytes([0]) + (0).to_bytes(4, "big") + (0).to_bytes(2, "big")
    for num in range(1, 5):
        rows += bytes([2]) + (7).to_bytes(4, "big") + ordinal[num].to_bytes(2, "big")
    rows += bytes([0]) + (0).to_bytes(4, "big") + (0).to_bytes(2, "big")     # 5: none
    rows += bytes([1]) + uni_at.to_bytes(4, "big") + (0).to_bytes(2, "big")  # 6
    rows += bytes([1]) + stm_at.to_bytes(4, "big") + (0).to_bytes(2, "big")  # 7
    rows += bytes([1]) + content_at.to_bytes(4, "big") + (0).to_bytes(2, "big")  # 8
    xstm = stream_obj("/Type /XRef /Size 9 /W [1 4 2] /Root 1 0 R /Filter /FlateDecode",
                      zlib.compress(bytes(rows)))
    xstm_at = len(body)
    body += b"9 0 obj\r\n" + xstm + b"\r\nendobj\r\n"
    body += b"startxref\r\n%d\r\n%%%%EOF\r\n" % xstm_at
    patch = b"%d" % xstm_at
    body[patch_at:patch_at + 10] = patch + b" " * (10 - len(patch))
    return bytes(body)


def make_arabic_recovery(b):
    """A ToUnicode CMap full of real producer quirks: a metadata dictionary,
    a wrong block count, a malformed range, a two-character destination, and
    mappings outside the declared one-byte code space - all recovered, the
    Arabic text intact (KillerPDF's compatibility recovery)."""
    cmap = (
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n"
        "/CMapType 2 def\n"
        "1 begincodespacerange\n"
        "<0000> <00FF>\n"                          # one-byte space declared
        "endcodespacerange\n"
        "2 beginbfchar\n"                          # wrong count: four entries follow
        "<0001> <0627>\n"                          # alef (2-byte code, outside the space)
        "<0002> <0644>\n"                          # lam
        "<0003> <0645>\n"                          # mim
        "<0004> <0640 0640>\n"                     # a two-character destination
        "endbfchar\n"
        "1 beginbfrange\n"
        "<0009> <0003> <0621>\n"                   # malformed: high < low, dropped
        "endbfrange\n"
        "endcmap\n"
        "CMapName currentdict /CMap defineresource pop\n"
        "end end\n").encode("latin-1")
    content = b"BT /F1 18 Tf 72 720 Td <0001000200030004> Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type0 /BaseFont /Ar /Encoding /Identity-H "
             "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Ar "
             "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> >>] "
             "/ToUnicode 6 0 R >>")
    b.add(5, stream_obj("", content))
    b.add(6, stream_obj("", cmap))
    return b.build(1)


def make_arabic_usecmap(b):
    """A ToUnicode stream that inherits half its mappings from a /UseCMap
    parent stream, the KillerPDF ReadToUnicode chain."""
    parent = (
        "/CIDInit /ProcSet findresource begin\n"
        "begincmap\n"
        "2 beginbfchar\n"
        "<0001> <0627>\n"                          # alef from the parent
        "<0002> <0644>\n"                          # lam from the parent
        "endbfchar\n"
        "endcmap\n"
        "end\n").encode("latin-1")
    child = (
        "2 beginbfchar\n"
        "<0003> <0645>\n"                          # mim, local to the child
        "<0004> <0643>\n"                          # kaf
        "endbfchar\n").encode("latin-1")
    content = b"BT /F1 18 Tf 72 720 Td <0001000200030004> Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type0 /BaseFont /Ar /Encoding /Identity-H "
             "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Ar "
             "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> >>] "
             "/ToUnicode 7 0 R >>")
    b.add(5, stream_obj("", content))
    b.add(6, stream_obj("", parent))
    b.add(7, stream_obj("/UseCMap 6 0 R", child))
    return b.build(1)


def make_arabic_embedded_cmap(b):
    """A composite font whose /Encoding is an embedded CID CMap stream (not
    Identity-H): codes 61-64 select CIDs 100-103, ToUnicode keys by CID."""
    encoding = (
        "/CIDInit /ProcSet findresource begin\n"
        "begincmap\n"
        "1 begincodespacerange\n"
        "<00> <FF>\n"
        "endcodespacerange\n"
        "1 begincidrange\n"
        "<0061> <0064> 100\n"
        "endcidrange\n"
        "endcmap\n"
        "CMapName currentdict /CMap defineresource pop\n"
        "end end\n").encode("latin-1")
    uni_entries = [(b"\x61", "\u0627"), (b"\x62", "\u0644"),
                   (b"\x63", "\u0645"), (b"\x64", "\u0643")]
    content = b"BT /F1 18 Tf 72 720 Td (abcd) Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type0 /BaseFont /Ar /Encoding 6 0 R "
             "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Ar "
             "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> >>] "
             "/ToUnicode 7 0 R >>")
    b.add(5, stream_obj("", content))
    b.add(6, stream_obj("", encoding))
    b.add(7, stream_obj("", tounicode_cmap(uni_entries)))
    return b.build(1)


def sfnt_with_cmap(mapping):
    """A minimal sfnt carrying only a format-4 cmap: unicode -> glyph id."""
    segs = sorted(mapping.items())
    n = len(segs)
    segcount = n + 1                            # plus the final 0xFFFF segment
    search_range = 2
    while search_range * 2 <= segcount * 2:
        search_range *= 2
    entry_selector = search_range.bit_length() - 2 if search_range >= 2 else 0
    range_shift = segcount * 2 - search_range
    endcodes = bytearray()
    startcodes = bytearray()
    deltas = bytearray()
    ranges = bytearray()
    for uni, gid in segs:
        endcodes += uni.to_bytes(2, "big")
        startcodes += uni.to_bytes(2, "big")
        deltas += ((gid - uni) & 0xFFFF).to_bytes(2, "big")
        ranges += (0).to_bytes(2, "big")
    endcodes += b"\xff\xff"
    startcodes += b"\xff\xff"
    deltas += (1).to_bytes(2, "big")            # 0xFFFF + 1 = 0 (no glyph)
    ranges += (0).to_bytes(2, "big")
    sub = bytearray()
    sub += (4).to_bytes(2, "big")               # format 4
    sub += (16 + segcount * 8).to_bytes(2, "big")   # length
    sub += (0).to_bytes(2, "big")               # language
    sub += (segcount * 2).to_bytes(2, "big")    # segCountX2
    sub += search_range.to_bytes(2, "big")
    sub += entry_selector.to_bytes(2, "big")
    sub += range_shift.to_bytes(2, "big")
    sub += endcodes
    sub += (0).to_bytes(2, "big")               # reservedPad
    sub += startcodes
    sub += deltas
    sub += ranges
    font = bytearray()
    font += (0x00010000).to_bytes(4, "big")     # sfnt version
    font += (1).to_bytes(2, "big")              # numTables
    font += (16).to_bytes(2, "big")             # searchRange
    font += (0).to_bytes(2, "big")              # entrySelector
    font += (0).to_bytes(2, "big")              # rangeShift
    font += b"cmap"
    font += (0).to_bytes(4, "big")              # checksum
    font += (28).to_bytes(4, "big")             # offset (12 + 16)
    font += (12 + len(sub)).to_bytes(4, "big")  # table length
    cmap = bytearray()
    cmap += (0).to_bytes(2, "big")              # cmap version
    cmap += (1).to_bytes(2, "big")              # one subtable
    cmap += (3).to_bytes(2, "big")              # platform: Windows
    cmap += (1).to_bytes(2, "big")              # encoding: Unicode BMP
    cmap += (12).to_bytes(4, "big")             # subtable offset
    cmap += bytes(sub)
    font += bytes(cmap)
    return bytes(font)


def make_arabic_embedded_font(b):
    """Identity-H with NO ToUnicode at all: the text survives through the
    embedded TrueType program's own cmap (KillerPDF's ReverseUnicode path)."""
    font_data = sfnt_with_cmap({0x0627: 3, 0x0644: 4, 0x0645: 5, 0x0643: 6})
    content = b"BT /F1 18 Tf 72 720 Td <0003000400050006> Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type0 /BaseFont /Ar /Encoding /Identity-H "
             "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Ar "
             "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
             "/FontDescriptor 6 0 R >>] >>")
    b.add(5, stream_obj("", content))
    b.add(6, "<< /Type /FontDescriptor /FontName /Ar /Flags 4 /FontBBox [0 0 1000 1000] "
             "/ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80 "
             "/FontFile2 7 0 R >>")
    b.add(7, stream_obj("/Length1 %d" % len(font_data), font_data))
    return b.build(1)


def make_broken_code_recovery(b):
    """One invalid code byte in the middle of the string: the recovery decode
    consumes it and the letters after it still arrive (KillerPDF's
    DecodeWithCompatibilityRecovery) instead of costing the rest of the page.
    WinAnsi Helvetica + a small ToUnicode over codes 1, 2, 4; bytes 3 and 5
    have no mapping and ride through as U+FFFD."""
    entries = [(b"\x01", "A"), (b"\x02", "B"), (b"\x04", "C")]
    content = b"BT /F1 18 Tf 72 720 Td (\\001\\002\\003\\004) Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding "
             "/ToUnicode 6 0 R >>")
    b.add(5, stream_obj("", content))
    cmap_lines = ["/CIDInit /ProcSet findresource begin", "begincmap",
                  "1 begincodespacerange", "<00> <FF> endcodespacerange",
                  "3 beginbfchar"]
    for code, uni in entries:
        cmap_lines.append("%s %s" % (hexstr(code), hexstr(bytes.fromhex("%04X" % ord(uni)))))
    cmap_lines += ["endbfchar", "endcmap", "end"]
    b.add(6, stream_obj("", "\n".join(cmap_lines).encode("latin-1")))
    return b.build(1)


EXTRA_MAKERS = [
    ("hybrid_xref.pdf", make_hybrid_xref),
    ("arabic_recovery.pdf", make_arabic_recovery),
    ("arabic_usecmap.pdf", make_arabic_usecmap),
    ("arabic_embedded_cmap.pdf", make_arabic_embedded_cmap),
    ("arabic_embedded_font.pdf", make_arabic_embedded_font),
    ("broken_code_recovery.pdf", make_broken_code_recovery),
]
