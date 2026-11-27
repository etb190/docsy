#!/usr/bin/env python3
"""Rebuild the embedded web pages: src/*.html -> src/*.deflate.

The host loads each page from RCDATA as raw deflate (crxk::Inflate, no zlib
header): 401 ai_page, 402 sum_page, 403 editor_page, 404 notes_page. The HTML
sources are the live artifacts - run this after editing any of them, or the
exe keeps serving the previous page (build 247 shipped a stale sum_page with
no resize grip and a transparent body because the html edits were never
recompressed). The build workflow runs this before cl, so a forgotten
recompression can no longer reach a release.
"""
import zlib, os

PAGES = {
    "ai_page": ["ai_page_head.html", "ai_page_body.html"],
    "sum_page": ["sum_page.html"],
    "editor_page": ["editor_page.html"],
    "notes_page": ["notes_page.html"],
}

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
for name, parts in PAGES.items():
    raw = b"".join(open(os.path.join(root, p), "rb").read() for p in parts)
    blob = zlib.compress(raw, 9)[2:-4]          # strip the zlib wrapper: raw deflate
    assert zlib.decompress(blob, -15) == raw, name   # the host's inflate round-trip
    path = os.path.join(root, name + ".deflate")
    changed = os.path.getsize(path) != len(blob) or open(path, "rb").read() != blob
    with open(path, "wb") as f:
        f.write(blob)
    print(f"{name}: html {len(raw)} -> deflate {len(blob)} bytes" + (" (rewritten)" if changed else " (unchanged)"))
