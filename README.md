<p align="center"><img src="logo.png" width="128" alt="Docsy"></p>

# Docsy
Tiny, fast, native Windows PDF reader. One C++ file, no dependencies, uses the PDF engine built into Windows 10/11. Film-grain textured margins, virtualized thumbnail sidebar (F9) and a footer status bar (page counter, paper size, view modes, zoom) that frame the document pane as one container. Flowing text selection with copy in the document's own languages (the KillerPDF-engine text layer: ToUnicode CMaps and standard encodings, so English, Arabic, French and everything else a PDF carries is found, selected and copied; scanned pages fall back to OS OCR), whole-document Find Text in KillerPDF's floating grain bar (Ctrl+F or the title bar's magnifier button, draggable by its grip), a custom borderless frame, portable single exe.

Keys: Ctrl+O open - drag & drop - wheel scroll (single view flips at the page edges) - Ctrl+wheel zoom - PgUp/PgDn/Space - Home/End - +/- zoom - F or Ctrl+0 fit width - Ctrl+1 100% - Ctrl+2 fit page - F5/F6/F7/F8 continuous/single/two-page/grid views - F9 thumbnails - F11 full screen - Ctrl+F find (Enter/Shift+Enter or F3/Shift+F3 step, Alt+C match case, Alt+W whole word, Esc closes) - drag on text to select, double-click for a word, click the status line for the file size.

One instance: opening a PDF (or launching the exe again) while Docsy is already up hands the file to the running window as a new tab instead of a second copy, and every document reopens where it was last read - the page the pane sat on when it or the app closed, the thumbnail rail landing there with it.


## Browser extensions
The built-in browser (toolbar **Web** button) runs Chrome extensions through WebView2. Open the Chrome Web Store from the puzzle button's menu, open an extension's page and press the green **Add to Docsy** button - Docsy downloads the package, unpacks it to `%LOCALAPPDATA%\\Docsy\\Extensions\\<id>`, keeps its real store ID and loads it before the next page opens. Installed extensions with a popup get a toolbar icon (the popup opens in its own window, sized to the page); the puzzle menu opens options pages, turns extensions off/on and removes them. Content scripts and background workers run as in Chrome; popup UIs that depend on Chrome-only APIs (tab management, for instance) may be limited by WebView2.
