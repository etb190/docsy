#!/usr/bin/env python3
"""Generates the synthetic PDFs the text-layer harness asserts against
(scripts/pdf_text_check.cpp). Every file exercises one branch of the
KillerPDF-extraction port in src/pdf_text.inl:

  simple_winansi.pdf     simple font, WinAnsiEncoding, accented Latin
  differences.pdf        StandardEncoding + Differences glyph names (French)
  arabic_identity.pdf    Type0 Identity-H + ToUnicode bfchar (Arabic + Persian)
  latin_cyrillic.pdf     Type0 Identity-H + ToUnicode bfrange UTF-16BE (French accents, Cyrillic)
  flate_content.pdf      content stream FlateDecode
  objstm_xrefstream.pdf  cross-reference stream + object streams
  form_xobject.pdf       text inside a Form XObject (Matrix + own resources)
  actualtext.pdf         BMC/BDC ActualText replacement
  rotated.pdf            /Rotate 90, geometry checked through RotateToRender
  timestest.pdf          TJ positioning, word spacing, two fonts on one line
"""
import os, sys, zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata")

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

def make_simple_winansi(b):
    content = b"BT /F1 24 Tf 72 720 Td (R\\351sum\\353 H\\340llo) Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    b.add(5, stream_obj("", content))
    return b.build(1)

def make_differences(b):
    # StandardEncoding has no eacute at 0x65; Differences renames codes 101-102.
    content = b"BT /F1 24 Tf 72 720 Td (r\\145sum\\146) Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
             "/Encoding << /BaseEncoding /StandardEncoding /Differences [101 /eacute 102 /ccedilla] >> >>")
    b.add(5, stream_obj("", content))
    return b.build(1)

def tounicode_cmap(entries):
    """entries: list of (code_bytes, unicode_str). Identity-H maps carry
    two-byte source codes, and destinations are the raw UTF-16BE bytes of each
    character (KillerPDF's Utf16 reads them that way)."""
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

def make_arabic(b):
    # Codes 01-06 map to Arabic/Persian letters; codes 01-04 spell the Arabic
    # word salam (U+0633 U+0644 U+0627 U+0645), 05-06 continue with U+06A9 U+0628.
    codes = [b"\x00\x01", b"\x00\x02", b"\x00\x03", b"\x00\x04", b"\x00\x05", b"\x00\x06"]
    chars = ["\u0633", "\u0644", "\u0627", "\u0645", "\u06A9", "\u0628"]
    body = "".join(hexstr(codes[i]) for i in range(6)).encode("latin-1")
    content = b"BT /F1 18 Tf 72 720 Td " + body + b" Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type0 /BaseFont /Arial /Encoding /Identity-H "
             "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Arial "
             "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
             "/DW 1000 /W [1 [600 500] 5 6 [400]] >>] "
             "/ToUnicode 6 0 R >>")
    b.add(5, stream_obj("", content))
    b.add(6, stream_obj("", tounicode_cmap(list(zip(codes, chars)))))
    return b.build(1)

def make_latin_cyrillic(b):
    # bfrange with a UTF-16BE destination array: codes 01-0A -> French accents
    # then Cyrillic letters, each its own Unicode scalar.
    chars = list("ÉÀÇéèç") + list("Привет")
    content = (b"BT /F1 18 Tf 72 720 Td " + hexstr(bytes.fromhex("000100020003000400050006")).encode("latin-1") + b" Tj "
               b"0 -28 Td " + hexstr(bytes.fromhex("000700080009000a000b000c")).encode("latin-1") + b" Tj ET\n")
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type0 /BaseFont /T /Encoding /Identity-H "
             "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /T "
             "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> >>] "
             "/ToUnicode 6 0 R >>")
    b.add(5, stream_obj("", content))
    lines = ["/CIDInit /ProcSet findresource begin", "12 dict begin", "begincmap",
             "1 begincodespacerange", "<0000> <FFFF> endcodespacerange",
             "1 beginbfrange", "<0001> <000c> ["]
    for c in chars:
        lines.append(hexstr(bytes.fromhex("%04X" % ord(c))))
    lines.append("] endbfrange")
    lines += ["endcmap", "CMapName currentdict /CMap defineresource pop", "end end"]
    b.add(6, stream_obj("", "\n".join(lines).encode("latin-1")))
    return b.build(1)

def make_flate(b):
    content = b"BT /F1 24 Tf 72 720 Td (Hello Flate) Tj ET\n"
    packed = zlib.compress(content)
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    b.add(5, stream_obj("/Filter /FlateDecode", packed))
    return b.build(1)

def make_objstm():
    """A PDF whose xref is a cross-reference stream and whose dictionaries live
    in an object stream (streams stay in the file, per the spec)."""
    content = b"BT /F1 24 Tf 72 720 Td (Inside ObjStm) Tj ET\n"
    obj_bodies = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           b"/Resources << /Font << /F1 4 0 R >> >> /Contents 6 0 R >>",
        4: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    }
    header = ""
    payload = bytearray()
    body_off = {}
    ordinal = {}
    for k, num in enumerate(sorted(obj_bodies)):
        body_off[num] = len(payload)
        ordinal[num] = k
        header += "%d %d " % (num, len(payload))
        payload += obj_bodies[num] + b" "
    stm = stream_obj("/Type /ObjStm /N %d /First %d /Filter /FlateDecode"
                     % (len(obj_bodies), len(header.encode("latin-1"))),
                     zlib.compress(header.encode("latin-1") + bytes(payload)))
    head = b"%PDF-1.7\n%\xE2\xE3\xCF\xD3\n"
    body = bytearray(head)
    stm_at = len(body)
    body += b"5 0 obj\n" + stm + b"\nendobj\n"
    content_at = len(body)
    body += b"6 0 obj\n" + stream_obj("", content) + b"\nendobj\n"
    rows = bytearray()
    rows += bytes([0]) + (0).to_bytes(4, "big") + (0).to_bytes(2, "big")       # object 0: free
    for num in range(1, 5):
        rows += bytes([2]) + (5).to_bytes(4, "big") + ordinal[num].to_bytes(2, "big")
    rows += bytes([1]) + int(stm_at).to_bytes(4, "big") + (0).to_bytes(2, "big")   # 5: the ObjStm itself
    rows += bytes([1]) + int(content_at).to_bytes(4, "big") + (0).to_bytes(2, "big")  # 6: the page's content stream
    packed_rows = zlib.compress(bytes(rows))
    xref_at = len(body)
    xref = stream_obj("/Type /XRef /Size 7 /W [1 4 2] /Root 1 0 R /Filter /FlateDecode", packed_rows)
    body += b"7 0 obj\n" + xref + b"\nendobj\n"
    body += b"startxref\n%d\n%%%%EOF\n" % xref_at
    return bytes(body)

def make_form_xobject(b):
    content = (b"BT /F1 24 Tf 72 720 Td (Before) Tj ET "
               b"q 1 0 0 1 100 100 cm /Fm0 Do Q "
               b"BT /F1 24 Tf 72 660 Td (After) Tj ET\n")
    form = b"BT /F1 24 Tf 12 12 Td (In form) Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> /XObject << /Fm0 6 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    b.add(5, stream_obj("", content))
    b.add(6, stream_obj("/Type /XObject /Subtype /Form /BBox [0 0 200 60] /Matrix [1 0 0 1 0 0] "
                        "/Resources << /Font << /F1 4 0 R >> >>", form))
    return b.build(1)

def make_actualtext(b):
    # The marked span's letters are replaced by ActualText (KillerPDF's EMC).
    content = (b"BT /F1 24 Tf 72 720 Td (Plain) Tj "
               b"/Span <</ActualText <FEFF004100420043>>> BDC (xxx) Tj EMC "
               b"0 -28 Td (Tail) Tj ET\n")
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    b.add(5, stream_obj("", content))
    return b.build(1)

def make_rotated(b):
    # One letter at (100, 700) on a 612x792 page rotated 90 degrees clockwise.
    content = b"BT /F1 24 Tf 100 700 Td (A) Tj ET\n"
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Rotate 90 "
             "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    b.add(5, stream_obj("", content))
    return b.build(1)

def make_times(b):
    # Two fonts, TJ positioning with kerning adjustments.
    content = (b"BT /F1 24 Tf 72 720 Td (The) Tj ET\n"
               b"BT /F2 18 Tf 72 690 Td [(Quick) 120 (Brown)] TJ ET\n")
    b.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 4 0 R /F2 5 0 R >> >> /Contents 6 0 R >>")
    b.add(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    b.add(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /Encoding /WinAnsiEncoding >>")
    b.add(6, stream_obj("", content))
    return b.build(1)

MAKERS = [
    ("simple_winansi.pdf", make_simple_winansi),
    ("differences.pdf", make_differences),
    ("arabic_identity.pdf", make_arabic),
    ("latin_cyrillic.pdf", make_latin_cyrillic),
    ("flate_content.pdf", make_flate),
    ("form_xobject.pdf", make_form_xobject),
    ("actualtext.pdf", make_actualtext),
    ("rotated.pdf", make_rotated),
    ("timestest.pdf", make_times),
]

def main():
    os.makedirs(OUT, exist_ok=True)
    for name, maker in MAKERS:
        b = Builder()
        data = maker(b)
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(data)
        print("wrote", name, len(data), "bytes")
    data = make_objstm()
    with open(os.path.join(OUT, "objstm_xrefstream.pdf"), "wb") as f:
        f.write(data)
    print("wrote objstm_xrefstream.pdf", len(data), "bytes")

if __name__ == "__main__":
    main()
