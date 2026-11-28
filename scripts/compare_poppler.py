#!/usr/bin/env python3
# Compare docsy's engine extraction vs pdftotext (poppler) per page:
# Arabic letter counts, total letter counts, and which side sees more.
import subprocess, sys, re, unicodedata

AR = re.compile(r'[\u0600-\u06FF\u0750-\u077F\u08A0-\u08FF\uFB50-\uFDFF\uFE70-\uFEFF]')

def poppler_pages(pdf):
    out = subprocess.run(['pdftotext', '-enc', 'UTF-8', pdf, '-'],
                         capture_output=True).stdout.decode('utf-8')
    pages = out.split('\f')
    return pages

def docsy_pages(pdf):
    r = subprocess.run(['/tmp/pdf_text_dump', pdf], capture_output=True)
    if r.returncode != 0:
        print("dump failed:", r.stderr.decode()[:400]); sys.exit(1)
    text = r.stdout.decode('utf-8', errors='replace')
    pages = []
    for m in re.finditer(r'=== page (\d+) letters=(\d+) fffd=(\d+) ===\n--- raw .*?\n--- final .*?\n(.*?)(?=\n=== page |\Z)', text, re.S):
        pages.append({'n': int(m.group(1)), 'letters': int(m.group(2)), 'fffd': int(m.group(3)), 'final': m.group(4)})
    return pages

def counts(s):
    ar = len(AR.findall(s))
    lat = len(re.findall(r'[A-Za-z]', s))
    dg = len(re.findall(r'[0-9]', s))
    return ar, lat, dg

pdf = sys.argv[1]
pp = poppler_pages(pdf)
dp = docsy_pages(pdf)
print(f"{pdf}: poppler={len(pp)} pages, docsy={len(dp)} pages")
worst = []
for i, (p, d) in enumerate(zip(pp, dp)):
    par, plat, pdg = counts(p)
    dar, dlat, ddg = counts(d['final'])
    gap = (par - dar) + (plat - dlat) + max(0, pdg - ddg)
    flag = ""
    if par + plat + pdg > 20 and (dar + dlat + ddg) < (par + plat + pdg) * 0.7:
        flag = "  <<< GAP"
    if flag:
        worst.append((i + 1, (par, plat, pdg), (dar, dlat, ddg), d['fffd']))
    print(f"p{i+1:3d} poppler ar={par:5d} la={plat:4d} dg={pdg:4d} | docsy ar={dar:5d} la={dlat:4d} dg={ddg:4d} fffd={d['fffd']}{flag}")
if worst:
    print("\nPages with a coverage gap:")
    for w in worst[:12]:
        print("  ", w)
