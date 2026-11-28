// Linux syntax-only shim for src/pdf_fpu.inl: stubs the Win32/GDI+/winrt
// surface the fallback touches, so a typo or a bad signature shows up here
// instead of in the Windows CI build. Not a behavior test - a parse test.
//   g++ -std=c++20 -fsyntax-only -I scripts -I src -include scripts/pdf_fpu_stub.h src/pdf_fpu.inl
#pragma once
#include <cstdint>
#include <cstddef>
#include <cwchar>
#include <algorithm>
using std::min;
using std::max;

typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned int UINT;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HBITMAP;
typedef void* HDC;
typedef void* HGDIOBJ;
typedef void* HPALETTE;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef struct _GUID { unsigned long a; } GUID;
typedef GUID CLSID;

#define MAX_PATH 260
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
};
struct BITMAPINFOHEADER {
    DWORD biSize; long biWidth, biHeight; WORD biPlanes, biBitCount;
    DWORD biCompression; DWORD biSizeImage; long biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
};
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; };
#define BI_RGB 0u
#define DIB_RGB_COLORS 0
#define GENERIC_READ 0x80000000u
#define OPEN_EXISTING 3u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define STREAM_SEEK_SET 0u
#define GetFileExInfoStandard 0
inline DWORD GetModuleFileNameW(void*, wchar_t*, DWORD) { return 0; }
inline HMODULE LoadLibraryW(const wchar_t*) { return nullptr; }
inline void* GetProcAddress(HMODULE, const char*) { return nullptr; }
inline BOOL GetFileAttributesExW(const wchar_t*, DWORD, WIN32_FILE_ATTRIBUTE_DATA*) { return 0; }
inline HANDLE CreateFileW(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, void*) { return INVALID_HANDLE_VALUE; }
inline BOOL ReadFile(HANDLE, void*, DWORD, DWORD*, void*) { return 0; }
inline BOOL CloseHandle(HANDLE) { return 0; }
inline BOOL DeleteObject(HGDIOBJ) { return 0; }
inline HDC GetDC(void*) { return nullptr; }
inline int ReleaseDC(void*, HDC) { return 0; }
inline HBITMAP CreateDIBSection(HDC, const BITMAPINFO*, UINT, void**, void*, DWORD) { return nullptr; }
inline int lstrcmpW(const wchar_t*, const wchar_t*) { return 0; }

struct _LARGE_INTEGER { long long QuadPart; };
typedef _LARGE_INTEGER LARGE_INTEGER;
struct IStream { long Seek(const LARGE_INTEGER&, DWORD, void*) { return 0; } };

namespace Gdiplus {
enum Status { Ok = 0, GenericError = 1 };
enum RotateFlipType { RotateNoneFlipX, RotateNoneFlipY, Rotate90FlipNone, Rotate180FlipNone, Rotate270FlipNone };
typedef unsigned int ARGB;
struct Color { Color(ARGB) {} Color(unsigned char, unsigned char, unsigned char) {} };
struct ImageCodecInfo { const wchar_t* MimeType; CLSID Clsid; };
struct Bitmap {
    Bitmap(HBITMAP, HPALETTE) {}
    Bitmap(const void*) {}
    Status GetLastStatus() { return Ok; }
    unsigned GetWidth() { return 0; }
    unsigned GetHeight() { return 0; }
    Bitmap* Clone(unsigned, unsigned, unsigned, unsigned, int) { return nullptr; }
    Status RotateFlip(RotateFlipType) { return Ok; }
    Status Save(IStream*, const CLSID*, const void*) { return Ok; }
    Status GetHBITMAP(const Color&, HBITMAP*) { return Ok; }
};
inline Status GetImageEncodersSize(UINT*, UINT*) { return Ok; }
inline Status GetImageEncoders(UINT, UINT, ImageCodecInfo*) { return Ok; }
}
#define PixelFormat32bppARGB 0x0026200A

namespace winrt {
template <class T> struct com_ptr {
    T* p = nullptr;
    T* operator->() { return p; }
    T* get() { return p; }
    void** put_void() { return (void**)&p; }
};
template <class T> struct agile_ref { T Get() { return T{}; } };
inline void check_hresult(long) {}
template <class T> void* get_unknown(T&) { return nullptr; }
template <class T> const GUID guid_of() { return GUID{}; }
}
inline long CreateStreamOverRandomAccessStream(void*, const GUID&, void**) { return 0; }
namespace winrt {
namespace Windows {
namespace Storage { namespace Streams {
struct InMemoryRandomAccessStream { InMemoryRandomAccessStream() {} };
} }
namespace Graphics { namespace Imaging {
struct SoftwareBitmap { SoftwareBitmap() {} SoftwareBitmap(std::nullptr_t) {} };
struct _SwbFuture {
    struct _Inner { SoftwareBitmap get() { return SoftwareBitmap{}; } };
    _Inner GetSoftwareBitmapAsync() { return {}; }
};
struct _DecFuture { _SwbFuture get() { return _SwbFuture{}; } };
struct BitmapDecoder {
    static _DecFuture CreateAsync(const ::winrt::Windows::Storage::Streams::InMemoryRandomAccessStream&) { return {}; }
};
} }
}
}
