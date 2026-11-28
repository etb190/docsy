// ---------------------------------------------------------------------------
// pdf_fpu.inl - the PDFium fallback raster for pages the Windows engine
// cannot decode.
//
// docsy renders with Windows.Data.Pdf, and that engine cannot decode every
// image codec PDFs ride. JPXDecode (JPEG2000) and JBIG2Decode pages - scans
// of books above all, exactly the files docsy is aimed at - come back as
// garbage: vertical streaks and black boxes over the words, while KillerPDF's
// own engine shows the same page fine. pdf_text.inl flags every page whose
// XObject tree draws such an image (TtPageFpu); this file renders those pages
// with PDFium, the renderer Edge and Chrome ship, instead.
//
// Optional by design: pdfium.dll is looked up beside the exe (the CI job
// downloads it next to the build), and a machine without it keeps today's
// behavior untouched - the Windows render, garbage included. Every entry
// point binds through GetProcAddress: no import table, no header to ship.
//
// Threading: every raster call lands on the one render worker, or on the UI
// thread in shot mode - the serialization g_renderM gives Windows.Data.Pdf.
// The fallback keeps its own mutex so a sync UI render (save writer, shot
// mode, OCR prewarm) can never race the worker's fallback on the same
// cached FPDF_DOCUMENT.
// ---------------------------------------------------------------------------
#ifndef DOCSY_PDF_FPU_INL
#define DOCSY_PDF_FPU_INL

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>

// ---- the API surface docsy needs, bound at first use ----
typedef struct _fpdf_document* FPDF_DOCUMENT;
typedef struct _fpdf_page* FPDF_PAGE;
typedef struct _fpdf_bitmap* FPDF_BITMAP;
typedef void (*FpuFnInit)(void);
typedef FPDF_DOCUMENT (*FpuFnLoadDoc64)(void*, size_t, const char*);
typedef FPDF_DOCUMENT (*FpuFnLoadDoc)(const void*, size_t, const char*);
typedef FPDF_PAGE (*FpuFnLoadPage)(FPDF_DOCUMENT, int);
typedef void (*FpuFnClosePage)(FPDF_PAGE);
typedef void (*FpuFnCloseDoc)(FPDF_DOCUMENT);
typedef FPDF_BITMAP (*FpuFnBmpCreate)(int, int, int);
typedef void (*FpuFnBmpDestroy)(FPDF_BITMAP);
typedef int (*FpuFnBmpStride)(FPDF_BITMAP);
typedef void* (*FpuFnBmpBuffer)(FPDF_BITMAP);
typedef int (*FpuFnBmpFill)(FPDF_BITMAP, int, int, int, int, unsigned long);
typedef void (*FpuFnRender)(FPDF_BITMAP, FPDF_PAGE, int, int, int, int, int, int);

static HMODULE g_fpuMod = nullptr;          // pdfium.dll beside the exe
static bool g_fpuTried = false;             // the lookup ran once
static FpuFnInit f_FpuInit = nullptr;
static FpuFnLoadDoc64 f_LoadDoc64 = nullptr;
static FpuFnLoadDoc f_LoadDoc = nullptr;
static FpuFnLoadPage f_LoadPage = nullptr;
static FpuFnClosePage f_ClosePage = nullptr;
static FpuFnCloseDoc f_CloseDoc = nullptr;
static FpuFnBmpCreate f_BmpCreate = nullptr;
static FpuFnBmpDestroy f_BmpDestroy = nullptr;
static FpuFnBmpStride f_BmpStride = nullptr;
static FpuFnBmpBuffer f_BmpBuffer = nullptr;
static FpuFnBmpFill f_BmpFill = nullptr;
static FpuFnRender f_RenderPage = nullptr;

static bool FpuBind() {
    if (g_fpuTried) return g_fpuMod != nullptr;
    g_fpuTried = true;
    wchar_t dir[MAX_PATH + 16] = L"";
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t* sl = wcsrchr(dir, L'\\');
    if (sl) *(sl + 1) = 0; else dir[0] = 0;
    std::wstring dll = std::wstring(dir) + L"pdfium.dll";
    g_fpuMod = LoadLibraryW(dll.c_str());
    if (!g_fpuMod) return false;
    struct Bind { const char* name; void** out; } binds[] = {
        { "FPDF_InitLibrary", (void**)&f_FpuInit },
        { "FPDF_LoadMemDocument64", (void**)&f_LoadDoc64 },
        { "FPDF_LoadMemDocument", (void**)&f_LoadDoc },
        { "FPDF_LoadPage", (void**)&f_LoadPage },
        { "FPDF_ClosePage", (void**)&f_ClosePage },
        { "FPDF_CloseDocument", (void**)&f_CloseDoc },
        { "FPDFBitmap_Create", (void**)&f_BmpCreate },
        { "FPDFBitmap_Destroy", (void**)&f_BmpDestroy },
        { "FPDFBitmap_GetStride", (void**)&f_BmpStride },
        { "FPDFBitmap_GetBuffer", (void**)&f_BmpBuffer },
        { "FPDFBitmap_FillRect", (void**)&f_BmpFill },
        { "FPDF_RenderPageBitmap", (void**)&f_RenderPage },
    };
    for (auto& b : binds) {
        *b.out = (void*)GetProcAddress(g_fpuMod, b.name);
        if (!*b.out) { g_fpuMod = nullptr; return false; }
    }
    f_FpuInit();
    return true;
}

// The one-document cache: FPDF_LoadMemDocument does not copy its buffer, so
// the file's bytes stay alive beside the handle, and a re-opened (changed)
// file replaces both. `bad` marks a document PDFium refused to open - pages
// stop retrying until the file's stamp changes.
static std::mutex g_fpuM;
static FPDF_DOCUMENT g_fpuDoc = nullptr;
static std::vector<uint8_t> g_fpuBytes;
static std::wstring g_fpuPath;
static long long g_fpuSize = 0;
static unsigned long long g_fpuStamp = 0;
static bool g_fpuBad = false;

static void FpuDropDoc() {
    if (g_fpuDoc && f_CloseDoc) f_CloseDoc(g_fpuDoc);
    g_fpuDoc = nullptr;
    g_fpuBytes.clear();
    g_fpuBytes.shrink_to_fit();
    g_fpuPath.clear();
    g_fpuSize = 0;
    g_fpuStamp = 0;
    g_fpuBad = false;
}

// File stamp: size + last-write time in one wash. A re-saved file (the
// editor's own write-out) invalidates the cached document.
static unsigned long long FpuStamp(const WIN32_FILE_ATTRIBUTE_DATA& fa) {
    unsigned long long t = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) |
                           fa.ftLastWriteTime.dwLowDateTime;
    return t * 1315423911ULL + (unsigned long long)fa.nFileSizeLow +
           ((unsigned long long)fa.nFileSizeHigh << 32);
}

static bool FpuEnsureDoc(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
    long long sz = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    unsigned long long st = FpuStamp(fa);
    // same file as before: the cached document answers (or a cached refusal -
    // pages stop re-opening a file PDFium rejected until the stamp changes)
    if (g_fpuPath == path && g_fpuSize == sz && g_fpuStamp == st)
        return g_fpuDoc != nullptr;
    FpuDropDoc();
    if (sz <= 0) return false;
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> bytes((size_t)sz);
    long long got = 0;
    DWORD rd = 0;
    while (got < sz &&
           ReadFile(fh, bytes.data() + got, (DWORD)min((long long)0x4000000, sz - got), &rd, nullptr) && rd)
        got += rd;
    CloseHandle(fh);
    if (got != sz) return false;
    FPDF_DOCUMENT d = nullptr;
    if (f_LoadDoc64) d = f_LoadDoc64(bytes.data(), bytes.size(), nullptr);
    else if (f_LoadDoc) d = f_LoadDoc(bytes.data(), bytes.size(), nullptr);
    if (!d) {
        // the refusal caches per file stamp: pages stop re-opening it until
        // the file changes
        g_fpuBad = true;
        g_fpuPath = path;
        g_fpuSize = sz;
        g_fpuStamp = st;
        return false;
    }
    g_fpuDoc = d;
    g_fpuBytes = std::move(bytes);   // FPDF_LoadMemDocument does not copy
    g_fpuPath = path;
    g_fpuSize = sz;
    g_fpuStamp = st;
    return true;
}

// The unrotated raster: PDFium maps the page onto the whole bitmap exactly the
// way Windows.Data.Pdf maps it onto DestinationWidth/Height (the layout's own
// pixel size), white background, annotations on. BGRA bytes straight into a
// top-down DIB section - PDFium's in-memory BGRA row order is the DIB's.
static HBITMAP FpuRenderFlat(const std::wstring& path, int page, int w, int h, bool* used) {
    *used = false;
    if (w <= 0 || h <= 0 || page < 0) return nullptr;
    std::lock_guard<std::mutex> lk(g_fpuM);   // the bind, the doc cache and the
    if (!FpuBind()) return nullptr;           // render share one thread-shape:
    if (!FpuEnsureDoc(path)) return nullptr;  // the first caller binds, the rest answer
    FPDF_PAGE pg = f_LoadPage(g_fpuDoc, page);
    if (!pg) return nullptr;
    HBITMAP out = nullptr;
    // alpha=1: BGRA, so the fourth byte is a real alpha the GDI+ rot/flip
    // path can trust - alpha=0's BGRx leaves it undefined, and GetHBITMAP
    // would composite every pixel onto white (a rotated page would come back
    // blank). The white fill pins every pixel opaque before the render.
    FPDF_BITMAP bm = f_BmpCreate(w, h, 1);
    if (bm) {
        f_BmpFill(bm, 0, 0, w, h, 0xFFFFFFFF);
        f_RenderPage(bm, pg, 0, 0, w, h, 0, 1 /*FPDF_ANNOT*/);
        int stride = f_BmpStride(bm);
        void* buf = f_BmpBuffer(bm);
        if (buf && stride >= w * 4) {
            BITMAPINFO bi{};
            bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
            BYTE* bits = nullptr;
            HDC tdc = GetDC(nullptr);
            out = tdc ? CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0) : nullptr;
            if (tdc) ReleaseDC(nullptr, tdc);
            if (out && bits) {
                size_t row = (size_t)w * 4;
                if ((size_t)stride == row) memcpy(bits, buf, row * (size_t)h);
                else for (int y = 0; y < h; y++)
                        memcpy(bits + (size_t)y * row, (uint8_t*)buf + (size_t)y * stride, row);
            } else if (out) {
                DeleteObject(out);
                out = nullptr;
            }
        }
        f_BmpDestroy(bm);
    }
    f_ClosePage(pg);
    if (out) *used = true;
    return out;
}

// The fallback's page entry: page `page` of `path` onto w x h pixels, the
// rot/flip transform baked the way RenderCore bakes it (raster at the
// UNROTATED aspect, then GDI+ rotates/flips into the caller's bitmap, so every
// downstream path reads ordinary w x h pixels). *used tells the caller the
// fallback truly produced this raster; anything else means the caller falls
// back to the engine render.
static HBITMAP FpuRenderPage(const std::wstring& path, int page, int w, int h,
                             int rot, int flip, bool* used) {
    *used = false;
    int rw = (rot % 2 == 0) ? w : h, rh = (rot % 2 == 0) ? h : w;
    HBITMAP flat = FpuRenderFlat(path, page, rw, rh, used);
    if (!flat || !*used) return flat;
    if (rot == 0 && flip == 0) return flat;
    // the HBITMAP-wrapped Bitmap is a read-only view: Clone to writable
    // memory first, transform the clone, then read the result out
    Gdiplus::Bitmap view(flat, nullptr);
    if (view.GetLastStatus() != Gdiplus::Ok) { DeleteObject(flat); *used = false; return nullptr; }
    Gdiplus::Bitmap* out = view.Clone(0, 0, view.GetWidth(), view.GetHeight(), PixelFormat32bppARGB);
    DeleteObject(flat);
    if (!out || out->GetLastStatus() != Gdiplus::Ok) { delete out; *used = false; return nullptr; }
    if (flip & 1) out->RotateFlip(Gdiplus::RotateNoneFlipX);
    if (flip & 2) out->RotateFlip(Gdiplus::RotateNoneFlipY);
    if ((rot & 3) == 1) out->RotateFlip(Gdiplus::Rotate90FlipNone);
    else if ((rot & 3) == 2) out->RotateFlip(Gdiplus::Rotate180FlipNone);
    else if ((rot & 3) == 3) out->RotateFlip(Gdiplus::Rotate270FlipNone);
    HBITMAP hb = nullptr;
    out->GetHBITMAP(Gdiplus::Color(255, 255, 255), &hb);
    delete out;
    if (!hb) *used = false;
    return hb;
}

// The OCR flight's raster: the same pixels as FpuRenderPage, re-encoded as a
// BMP stream and decoded into a SoftwareBitmap - the input format the OS OCR
// engine consumes (RenderSoftDoc's own tail, over the fallback's raster).
static winrt::Windows::Graphics::Imaging::SoftwareBitmap FpuRenderSoftPage(
        const std::wstring& path, int page, int w, int h, int rot, int flip, bool* used) {
    *used = false;
    HBITMAP fb = FpuRenderPage(path, page, w, h, rot, flip, used);
    if (!fb || !*used) return nullptr;
    try {
        Gdiplus::Bitmap view(fb, nullptr);
        if (view.GetLastStatus() != Gdiplus::Ok) { DeleteObject(fb); *used = false; return nullptr; }
        Gdiplus::Bitmap* out = view.Clone(0, 0, view.GetWidth(), view.GetHeight(), PixelFormat32bppARGB);
        DeleteObject(fb);
        if (!out || out->GetLastStatus() != Gdiplus::Ok) { delete out; *used = false; return nullptr; }
        CLSID bmpCls{};
        UINT cnt = 0, bytes = 0;
        Gdiplus::GetImageEncodersSize(&cnt, &bytes);
        std::vector<BYTE> buf(bytes);
        auto* enc = (Gdiplus::ImageCodecInfo*)buf.data();
        Gdiplus::GetImageEncoders(cnt, bytes, enc);
        for (UINT e = 0; e < cnt; e++) if (!lstrcmpW(enc[e].MimeType, L"image/bmp")) bmpCls = enc[e].Clsid;
        winrt::Windows::Storage::Streams::InMemoryRandomAccessStream st;
        winrt::com_ptr<IStream> os;
        winrt::check_hresult(CreateStreamOverRandomAccessStream(winrt::get_unknown(st), winrt::guid_of<IStream>(), os.put_void()));
        out->Save(os.get(), &bmpCls, nullptr);
        delete out;
        LARGE_INTEGER z0{};
        z0.QuadPart = 0;
        os->Seek(z0, STREAM_SEEK_SET, nullptr);
        auto dec = winrt::Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(st).get();
        return dec.GetSoftwareBitmapAsync().get();
    } catch (...) {
        *used = false;
        return nullptr;
    }
}

#endif // DOCSY_PDF_FPU_INL
