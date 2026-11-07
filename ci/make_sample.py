import sys
n = 3
objs = [None] * (3 + 2 * n)
kids = " ".join(f"{4 + 2 * i} 0 R" for i in range(n))
objs[0] = "<< /Type /Catalog /Pages 2 0 R >>"
objs[1] = f"<< /Type /Pages /Kids [{kids}] /Count {n} >>"
objs[2] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
for i in range(n):
    c = (f"0.86 0.92 1 rg 72 420 468 240 re f\n"
         f"0.1 0.3 0.8 RG 3 w 72 420 468 240 re S\n"
         f"0 g BT /F1 40 Tf 72 700 Td (Tiny PDF - page {i + 1}) Tj ET\n"
         f"BT /F1 16 Tf 72 640 Td (Native Windows PDF reader.) Tj ET\n"
         f"BT /F1 16 Tf 72 610 Td (Small, fast, no dependencies.) Tj ET\n"
         f"BT /F1 12 Tf 72 380 Td (Scroll, zoom with Ctrl+wheel, drag to pan.) Tj ET\n")
    objs[3 + 2 * i] = (f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents {5 + 2 * i} 0 R "
                       f"/Resources << /Font << /F1 3 0 R >> >> >>")
    objs[4 + 2 * i] = f"<< /Length {len(c)} >>\nstream\n{c}\nendstream"
out = b"%PDF-1.4\n"
offs = []
for i, o in enumerate(objs):
    offs.append(len(out))
    out += f"{i + 1} 0 obj\n{o}\nendobj\n".encode("latin-1")
x = len(out)
out += f"xref\n0 {len(objs) + 1}\n0000000000 65535 f \n".encode()
for o in offs:
    out += f"{o:010d} 00000 n \n".encode()
out += f"trailer\n<< /Size {len(objs) + 1} /Root 1 0 R >>\nstartxref\n{x}\n%%EOF\n".encode()
open(sys.argv[1], "wb").write(out)
