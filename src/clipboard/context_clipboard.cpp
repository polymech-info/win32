#include "context_clipboard.hpp"
#include "fs/path_sanitizer.hpp"
#include "process/process_helpers.hpp"
#include "shell/context_shell.hpp"

#if defined(_WIN32)

#include <ole2.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace media::win::context {
namespace {

constexpr size_t kMaxTextStore = 64 * 1024;
constexpr size_t kMaxImageStore = 8 * 1024 * 1024;
constexpr size_t kMaxAudioStore = 4 * 1024 * 1024;

struct ClipGuard {
    ~ClipGuard() { ::CloseClipboard(); }
};

struct LockGuard {
    HANDLE h = nullptr;
    explicit LockGuard(HANDLE handle) : h(handle) {}
    ~LockGuard()
    {
        if (h)
            ::GlobalUnlock(h);
    }
};

std::vector<uint8_t> copy_hglobal(HANDLE h, size_t cap)
{
    std::vector<uint8_t> out;
    if (!h)
        return out;
    const SIZE_T n = ::GlobalSize(h);
    if (n == 0 || n > cap)
        return out;
    const void* p = ::GlobalLock(h);
    if (!p)
        return out;
    LockGuard lock(h);
    out.resize(static_cast<size_t>(n));
    std::memcpy(out.data(), p, out.size());
    return out;
}

std::wstring leaf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return path;
    return path.substr(slash + 1);
}

std::wstring first_line(const std::wstring& text, size_t maxChars)
{
    std::wstring s = text;
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r'
                          || s.front() == L'\n'))
        s.erase(s.begin());
    const size_t nl = s.find_first_of(L"\r\n");
    if (nl != std::wstring::npos)
        s.resize(nl);
    if (s.size() > maxChars)
        s.resize(maxChars);
    return s;
}

void read_hdrop(HANDLE h, std::vector<std::wstring>& paths)
{
    const auto* df = static_cast<const DROPFILES*>(::GlobalLock(h));
    if (!df)
        return;
    LockGuard lock(h);
    if (df->fWide) {
        const wchar_t* p = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const BYTE*>(df) + df->pFiles);
        while (*p) {
            std::wstring path;
            while (*p)
                path.push_back(*p++);
            if (!path.empty())
                paths.push_back(std::move(path));
            ++p;
        }
        return;
    }
    const char* p = reinterpret_cast<const char*>(
        reinterpret_cast<const BYTE*>(df) + df->pFiles);
    while (*p) {
        std::string path;
        while (*p)
            path.push_back(*p++);
        if (!path.empty()) {
            const int n = ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
            std::wstring w(n > 1 ? static_cast<size_t>(n - 1) : 0, L'\0');
            if (!w.empty())
                ::MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, w.data(), n);
            paths.push_back(std::move(w));
        }
        ++p;
    }
}

std::wstring read_unicode_text(HANDLE h)
{
    const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h));
    if (!p)
        return {};
    LockGuard lock(h);
    const SIZE_T bytes = ::GlobalSize(h);
    const size_t chars = bytes / sizeof(wchar_t);
    std::wstring s;
    s.reserve(chars);
    for (size_t i = 0; i < chars && p[i]; ++i)
        s.push_back(p[i]);
    if (s.size() > kMaxTextStore)
        s.resize(kMaxTextStore);
    return s;
}

std::wstring read_ansi_text(HANDLE h)
{
    const char* p = static_cast<const char*>(::GlobalLock(h));
    if (!p)
        return {};
    LockGuard lock(h);
    const SIZE_T bytes = ::GlobalSize(h);
    size_t n = 0;
    while (n < bytes && p[n])
        ++n;
    if (n > kMaxTextStore)
        n = kMaxTextStore;
    const int wlen = ::MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(n), nullptr, 0);
    if (wlen <= 0)
        return {};
    std::wstring s(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(n), s.data(), wlen);
    return s;
}

bool bitmap_to_dib(HBITMAP hb, std::vector<uint8_t>& dib)
{
    if (!hb)
        return false;
    BITMAP bm{};
    if (!::GetObjectW(hb, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return false;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = bm.bmHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    const size_t stride = static_cast<size_t>(((bm.bmWidth * 32 + 31) / 32) * 4);
    const size_t bits = stride * static_cast<size_t>(bm.bmHeight);
    if (sizeof(BITMAPINFOHEADER) + bits > kMaxImageStore)
        return false;
    dib.resize(sizeof(BITMAPINFOHEADER) + bits);
    std::memcpy(dib.data(), &bmi.bmiHeader, sizeof(BITMAPINFOHEADER));
    HDC dc = ::GetDC(nullptr);
    if (!dc)
        return false;
    const int ok = ::GetDIBits(dc, hb, 0, static_cast<UINT>(bm.bmHeight),
                               dib.data() + sizeof(BITMAPINFOHEADER),
                               reinterpret_cast<BITMAPINFO*>(dib.data()), DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    if (ok <= 0) {
        dib.clear();
        return false;
    }
    return true;
}

std::wstring temp_dir()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetTempPathW(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH)
        return {};
    std::filesystem::path dir(std::wstring(buf, n));
    dir /= L"tanit-clipboard";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

bool write_bytes(const std::wstring& path, const void* data, size_t n)
{
    if (path.empty() || !data || n == 0)
        return false;
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return static_cast<bool>(out);
}

bool write_dib_bmp(const std::wstring& path, const std::vector<uint8_t>& dib)
{
    if (dib.size() < sizeof(BITMAPINFOHEADER))
        return false;
    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + bih->biSize);
    if (bih->biBitCount <= 8)
        fh.bfOffBits += (1u << bih->biBitCount) * sizeof(RGBQUAD);
    fh.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + dib.size());
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    out.write(reinterpret_cast<const char*>(dib.data()),
              static_cast<std::streamsize>(dib.size()));
    return static_cast<bool>(out);
}

bool write_utf8_txt(const std::wstring& path, const std::wstring& text)
{
    const std::string utf8 = media::process::narrow_utf8(text);
    if (utf8.empty())
        return false;
    return write_bytes(path, utf8.data(), utf8.size());
}

} // namespace

std::wstring clipboard_format_name(UINT format)
{
    switch (format) {
    case CF_TEXT: return L"CF_TEXT";
    case CF_BITMAP: return L"CF_BITMAP";
    case CF_METAFILEPICT: return L"CF_METAFILEPICT";
    case CF_SYLK: return L"CF_SYLK";
    case CF_DIF: return L"CF_DIF";
    case CF_TIFF: return L"CF_TIFF";
    case CF_OEMTEXT: return L"CF_OEMTEXT";
    case CF_DIB: return L"CF_DIB";
    case CF_PALETTE: return L"CF_PALETTE";
    case CF_PENDATA: return L"CF_PENDATA";
    case CF_RIFF: return L"CF_RIFF";
    case CF_WAVE: return L"CF_WAVE";
    case CF_UNICODETEXT: return L"CF_UNICODETEXT";
    case CF_ENHMETAFILE: return L"CF_ENHMETAFILE";
    case CF_HDROP: return L"CF_HDROP";
    case CF_LOCALE: return L"CF_LOCALE";
    case CF_DIBV5: return L"CF_DIBV5";
    case CF_OWNERDISPLAY: return L"CF_OWNERDISPLAY";
    case CF_DSPTEXT: return L"CF_DSPTEXT";
    case CF_DSPBITMAP: return L"CF_DSPBITMAP";
    case CF_DSPMETAFILEPICT: return L"CF_DSPMETAFILEPICT";
    case CF_DSPENHMETAFILE: return L"CF_DSPENHMETAFILE";
    default:
        break;
    }
    wchar_t name[128]{};
    if (::GetClipboardFormatNameW(format, name, 128) > 0)
        return name;
    return L"CF_" + std::to_wstring(format);
}

std::wstring clipboard_format_display(UINT format)
{
    switch (format) {
    case CF_DIB:
    case CF_DIBV5:
    case CF_BITMAP:
    case CF_DSPBITMAP:
    case CF_TIFF:
        return L"Image";
    case CF_UNICODETEXT:
    case CF_TEXT:
    case CF_OEMTEXT:
    case CF_DSPTEXT:
        return L"Text";
    case CF_HDROP:
        return L"Files";
    case CF_WAVE:
    case CF_RIFF:
        return L"Audio";
    case CF_ENHMETAFILE:
    case CF_METAFILEPICT:
    case CF_DSPENHMETAFILE:
    case CF_DSPMETAFILEPICT:
        return L"Metafile";
    default:
        break;
    }
    const std::wstring name = clipboard_format_name(format);
    if (name == L"PNG" || name == L"image/png" || name == L"image/bmp"
        || name == L"JFIF" || name == L"image/jpeg")
        return L"Image";
    if (name.size() > 3 && name.compare(0, 3, L"CF_") == 0)
        return L"Clipboard";
    return name;
}

const wchar_t* clipboard_kind_name(ClipboardPayloadKind kind)
{
    switch (kind) {
    case ClipboardPayloadKind::Text: return L"text";
    case ClipboardPayloadKind::Files: return L"files";
    case ClipboardPayloadKind::Image: return L"image";
    case ClipboardPayloadKind::Audio: return L"audio";
    case ClipboardPayloadKind::Metafile: return L"metafile";
    case ClipboardPayloadKind::Other: return L"other";
    case ClipboardPayloadKind::Empty: return L"empty";
    }
    return L"other";
}

namespace {

void hydrate_shell_items_from_paths(ClipboardHistoryItem& item)
{
    if (!item.items.empty() || item.paths.empty())
        return;
    item.items.reserve(item.paths.size());
    for (const auto& p : item.paths) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&si)))
            && si) {
            item.items.push_back(shell::item_from_shell_item(si, true, item.items.empty()));
            si->Release();
            continue;
        }
        item.items.push_back(FileItem(p, true, item.items.empty()));
        const DWORD attr = ::GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            item.items.back().kind = ContextItem::Kind::Directory;
    }
}

void fill_file_paths_from_items(ClipboardHistoryItem& item)
{
    if (!item.paths.empty())
        return;
    for (const auto& ci : item.items) {
        if (ci.path && !ci.path->empty())
            item.paths.push_back(*ci.path);
        else if (ci.parsingName && !ci.parsingName->empty())
            item.paths.push_back(*ci.parsingName);
    }
}

bool capture_shell_file_objects(ClipboardHistoryItem& item)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = (com == S_OK);
    IDataObject* data = nullptr;
    if (FAILED(::OleGetClipboard(&data)) || !data) {
        if (uninit)
            ::CoUninitialize();
        return false;
    }
    auto items = shell::items_from_data_object(data);
    data->Release();
    if (uninit)
        ::CoUninitialize();
    if (items.empty())
        return false;
    item.kind = ClipboardPayloadKind::Files;
    item.items = std::move(items);
    fill_file_paths_from_items(item);
    const UINT shellList = ::RegisterClipboardFormatW(L"Shell IDList Array");
    item.primaryFormat = shellList ? shellList : CF_HDROP;
    item.formatName = clipboard_format_name(item.primaryFormat);
    item.label = item.items.front().displayName;
    if (item.label.empty() && !item.paths.empty())
        item.label = leaf(item.paths.front());
    return true;
}

} // namespace

std::optional<ClipboardHistoryItem> capture_clipboard_snapshot()
{
    ClipboardHistoryItem item;
    item.sequence = ::GetClipboardSequenceNumber();
    item.timestampMs = now_ms();

    if (capture_shell_file_objects(item))
        return item;

    if (!::OpenClipboard(nullptr))
        return std::nullopt;
    ClipGuard clip;

    const UINT png = ::RegisterClipboardFormatW(L"PNG");
    const UINT pngMime = ::RegisterClipboardFormatW(L"image/png");

    for (UINT fmt = ::EnumClipboardFormats(0); fmt != 0; fmt = ::EnumClipboardFormats(fmt))
        item.formats.push_back(fmt);

    auto has = [&](UINT fmt) {
        return std::find(item.formats.begin(), item.formats.end(), fmt) != item.formats.end();
    };

    if (has(CF_HDROP)) {
        if (HANDLE h = ::GetClipboardData(CF_HDROP))
            read_hdrop(h, item.paths);
        if (!item.paths.empty()) {
            item.kind = ClipboardPayloadKind::Files;
            item.primaryFormat = CF_HDROP;
            item.formatName = clipboard_format_name(CF_HDROP);
            hydrate_shell_items_from_paths(item);
            item.label = !item.items.empty() && !item.items.front().displayName.empty()
                ? item.items.front().displayName
                : leaf(item.paths.front());
            return item;
        }
    }

    // PNG first — Snipping Tool / browsers ship a compact PNG plus a huge CF_DIB.
    // A 4K DIB is ~33 MB and used to exceed kMaxImageStore, then fall through as
    // an "CF_DIB" Other row.
    if (png && has(png)) {
        if (HANDLE h = ::GetClipboardData(png))
            item.imagePng = copy_hglobal(h, kMaxImageStore);
    } else if (pngMime && has(pngMime)) {
        if (HANDLE h = ::GetClipboardData(pngMime))
            item.imagePng = copy_hglobal(h, kMaxImageStore);
    }
    if (has(CF_DIB)) {
        if (HANDLE h = ::GetClipboardData(CF_DIB))
            item.imageDib = copy_hglobal(h, kMaxImageStore);
    }
    if (item.imageDib.empty() && has(CF_DIBV5)) {
        if (HANDLE h = ::GetClipboardData(CF_DIBV5))
            item.imageDib = copy_hglobal(h, kMaxImageStore);
    }
    if (item.imageDib.empty() && has(CF_BITMAP)) {
        if (HANDLE h = ::GetClipboardData(CF_BITMAP))
            bitmap_to_dib(static_cast<HBITMAP>(h), item.imageDib);
    }
    if (item.imageDib.empty() && has(CF_DSPBITMAP)) {
        if (HANDLE h = ::GetClipboardData(CF_DSPBITMAP))
            bitmap_to_dib(static_cast<HBITMAP>(h), item.imageDib);
    }

    const bool imageFmt = has(CF_DIB) || has(CF_DIBV5) || has(CF_BITMAP) || has(CF_DSPBITMAP)
        || (png && has(png)) || (pngMime && has(pngMime));
    if (!item.imageDib.empty() || !item.imagePng.empty() || imageFmt) {
        item.kind = ClipboardPayloadKind::Image;
        if (!item.imagePng.empty())
            item.primaryFormat = (png && has(png)) ? png : pngMime;
        else if (has(CF_DIB))
            item.primaryFormat = CF_DIB;
        else if (has(CF_DIBV5))
            item.primaryFormat = CF_DIBV5;
        else if (has(CF_BITMAP))
            item.primaryFormat = CF_BITMAP;
        else if (has(CF_DSPBITMAP))
            item.primaryFormat = CF_DSPBITMAP;
        else
            item.primaryFormat = png ? png : pngMime;
        item.formatName = clipboard_format_name(item.primaryFormat);
        item.label = clipboard_format_display(item.primaryFormat);
        if (item.imageDib.size() >= sizeof(BITMAPINFOHEADER)) {
            const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(item.imageDib.data());
            const int w = bih->biWidth;
            const int h = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
            if (w > 0 && h > 0)
                item.label = L"Image " + std::to_wstring(w) + L"x" + std::to_wstring(h);
        } else if (item.imagePng.size() >= 24
            && item.imagePng[0] == 0x89 && item.imagePng[1] == 'P'
            && item.imagePng[2] == 'N' && item.imagePng[3] == 'G') {
            const auto be32 = [](const uint8_t* p) {
                return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
                    | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
            };
            const int w = static_cast<int>(be32(item.imagePng.data() + 16));
            const int h = static_cast<int>(be32(item.imagePng.data() + 20));
            if (w > 0 && h > 0)
                item.label = L"Image " + std::to_wstring(w) + L"x" + std::to_wstring(h);
        } else {
            auto peek_dib = [&](UINT fmt) {
                HANDLE mem = ::GetClipboardData(fmt);
                if (!mem)
                    return;
                const SIZE_T n = ::GlobalSize(mem);
                if (n < sizeof(BITMAPINFOHEADER))
                    return;
                const void* p = ::GlobalLock(mem);
                if (!p)
                    return;
                const auto* bih = static_cast<const BITMAPINFOHEADER*>(p);
                const int w = bih->biWidth;
                const int height = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
                ::GlobalUnlock(mem);
                if (w > 0 && height > 0)
                    item.label = L"Image " + std::to_wstring(w) + L"x" + std::to_wstring(height);
            };
            if (has(CF_DIB))
                peek_dib(CF_DIB);
            else if (has(CF_DIBV5))
                peek_dib(CF_DIBV5);
        }
        return item;
    }

    if (has(CF_UNICODETEXT)) {
        if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT))
            item.text = read_unicode_text(h);
    } else if (has(CF_TEXT) || has(CF_OEMTEXT) || has(CF_DSPTEXT)) {
        const UINT fmt = has(CF_TEXT) ? CF_TEXT : (has(CF_OEMTEXT) ? CF_OEMTEXT : CF_DSPTEXT);
        if (HANDLE h = ::GetClipboardData(fmt))
            item.text = read_ansi_text(h);
        item.primaryFormat = fmt;
    }
    if (!item.text.empty()) {
        item.kind = ClipboardPayloadKind::Text;
        if (!item.primaryFormat)
            item.primaryFormat = CF_UNICODETEXT;
        item.formatName = clipboard_format_name(item.primaryFormat);
        item.label = first_line(item.text, 48);
        if (item.label.empty())
            item.label = L"Text";
        return item;
    }

    if (has(CF_WAVE) || has(CF_RIFF)) {
        const UINT fmt = has(CF_WAVE) ? CF_WAVE : CF_RIFF;
        if (HANDLE h = ::GetClipboardData(fmt))
            item.audioBytes = copy_hglobal(h, kMaxAudioStore);
        if (!item.audioBytes.empty()) {
            item.kind = ClipboardPayloadKind::Audio;
            item.primaryFormat = fmt;
            item.formatName = clipboard_format_name(fmt);
            item.label = L"Audio";
            return item;
        }
    }

    if (has(CF_TIFF)) {
        if (HANDLE h = ::GetClipboardData(CF_TIFF))
            item.rawBytes = copy_hglobal(h, kMaxImageStore);
        if (!item.rawBytes.empty()) {
            item.kind = ClipboardPayloadKind::Image;
            item.primaryFormat = CF_TIFF;
            item.formatName = clipboard_format_name(CF_TIFF);
            item.label = L"TIFF image";
            item.rawExt = L".tif";
            return item;
        }
    }

    auto take_hglobal = [&](UINT fmt, ClipboardPayloadKind kind, const wchar_t* label,
                            const wchar_t* ext, size_t cap) {
        if (!has(fmt))
            return false;
        HANDLE h = ::GetClipboardData(fmt);
        if (!h)
            return false;
        item.rawBytes = copy_hglobal(h, cap);
        if (item.rawBytes.empty())
            return false;
        item.kind = kind;
        item.primaryFormat = fmt;
        item.formatName = clipboard_format_name(fmt);
        item.label = label;
        item.rawExt = ext;
        return true;
    };
    if (take_hglobal(CF_SYLK, ClipboardPayloadKind::Other, L"SYLK", L".slk", kMaxTextStore))
        return item;
    if (take_hglobal(CF_DIF, ClipboardPayloadKind::Other, L"DIF", L".dif", kMaxTextStore))
        return item;

    if (has(CF_ENHMETAFILE) || has(CF_DSPENHMETAFILE) || has(CF_METAFILEPICT)
        || has(CF_DSPMETAFILEPICT)) {
        const UINT emfFmt = has(CF_ENHMETAFILE) ? CF_ENHMETAFILE
            : (has(CF_DSPENHMETAFILE) ? CF_DSPENHMETAFILE : 0);
        if (emfFmt) {
            auto hemf = static_cast<HENHMETAFILE>(::GetClipboardData(emfFmt));
            if (hemf) {
                const UINT n = ::GetEnhMetaFileBits(hemf, 0, nullptr);
                if (n > 0 && n <= kMaxImageStore) {
                    item.rawBytes.resize(n);
                    if (::GetEnhMetaFileBits(hemf, n, item.rawBytes.data()) == n) {
                        item.rawExt = L".emf";
                    } else {
                        item.rawBytes.clear();
                    }
                }
            }
            item.primaryFormat = emfFmt;
        } else {
            item.primaryFormat = has(CF_METAFILEPICT) ? CF_METAFILEPICT : CF_DSPMETAFILEPICT;
        }
        item.kind = ClipboardPayloadKind::Metafile;
        item.formatName = clipboard_format_name(item.primaryFormat);
        item.label = L"Metafile";
        return item;
    }

    if (item.formats.empty())
        return std::nullopt;
    if (item.formats.size() == 1 && item.formats.front() == CF_LOCALE)
        return std::nullopt;

    item.kind = ClipboardPayloadKind::Other;
    item.primaryFormat = item.formats.front();
    item.formatName = clipboard_format_name(item.primaryFormat);
    item.label = clipboard_format_display(item.primaryFormat);
    if (item.primaryFormat != CF_BITMAP && item.primaryFormat != CF_ENHMETAFILE
        && item.primaryFormat != CF_METAFILEPICT && item.primaryFormat != CF_DSPBITMAP
        && item.primaryFormat != CF_DSPENHMETAFILE && item.primaryFormat != CF_DSPMETAFILEPICT
        && item.primaryFormat != CF_PALETTE && item.primaryFormat != CF_OWNERDISPLAY) {
        if (HANDLE h = ::GetClipboardData(item.primaryFormat))
            item.rawBytes = copy_hglobal(h, kMaxAudioStore);
        if (!item.rawBytes.empty())
            item.rawExt = L".bin";
    }
    return item;
}

HBITMAP clipboard_dib_thumb(const std::vector<uint8_t>& dib, int dim)
{
    if (dib.size() < sizeof(BITMAPINFOHEADER) || dim <= 0)
        return nullptr;
    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    const int srcW = bih->biWidth;
    const int srcH = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
    if (srcW <= 0 || srcH <= 0)
        return nullptr;

    BITMAPINFO dst{};
    dst.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dst.bmiHeader.biWidth = dim;
    dst.bmiHeader.biHeight = -dim;
    dst.bmiHeader.biPlanes = 1;
    dst.bmiHeader.biBitCount = 32;
    dst.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = ::CreateDIBSection(nullptr, &dst, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb)
        return nullptr;
    HDC dc = ::CreateCompatibleDC(nullptr);
    if (!dc) {
        ::DeleteObject(hb);
        return nullptr;
    }
    HGDIOBJ old = ::SelectObject(dc, hb);
    RECT rc{ 0, 0, dim, dim };
    HBRUSH br = ::CreateSolidBrush(RGB(32, 32, 36));
    if (br) {
        ::FillRect(dc, &rc, br);
        ::DeleteObject(br);
    }
    const int fitW = srcW > srcH ? dim : (std::max)(1, dim * srcW / srcH);
    const int fitH = srcH > srcW ? dim : (std::max)(1, dim * srcH / srcW);
    const int x = (dim - fitW) / 2;
    const int y = (dim - fitH) / 2;
    std::vector<uint8_t> infoBuf = dib;
    auto* info = reinterpret_cast<BITMAPINFO*>(infoBuf.data());
    ::SetStretchBltMode(dc, HALFTONE);
    ::SetBrushOrgEx(dc, 0, 0, nullptr);
    (void)::StretchDIBits(dc, x, y, fitW, fitH, 0, 0, srcW, srcH,
                          dib.data() + bih->biSize, info, DIB_RGB_COLORS, SRCCOPY);
    ::SelectObject(dc, old);
    ::DeleteDC(dc);
    return hb;
}

bool materialize_clipboard_item(ClipboardHistoryItem& item, std::wstring& path)
{
    path.clear();
    if (!item.materializedPath.empty()) {
        if (::GetFileAttributesW(item.materializedPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            path = item.materializedPath;
            return true;
        }
        item.materializedPath.clear();
    }
    if (item.kind == ClipboardPayloadKind::Files) {
        if (item.paths.empty())
            return false;
        path = item.paths.front();
        return true;
    }
    const std::wstring dir = temp_dir();
    if (dir.empty())
        return false;
    const std::wstring stem = dir + L"\\clip-" + std::to_wstring(item.id ? item.id : item.sequence);
    if (item.kind == ClipboardPayloadKind::Image) {
        if (!item.imagePng.empty()) {
            path = stem + L".png";
            if (!write_bytes(path, item.imagePng.data(), item.imagePng.size()))
                return false;
        } else if (!item.imageDib.empty()) {
            path = stem + L".bmp";
            if (!write_dib_bmp(path, item.imageDib))
                return false;
        } else {
            return false;
        }
        item.materializedPath = path;
        return true;
    }
    if (item.kind == ClipboardPayloadKind::Text && !item.text.empty()) {
        path = stem + L".txt";
        if (!write_utf8_txt(path, item.text))
            return false;
        item.materializedPath = path;
        return true;
    }
    if (item.kind == ClipboardPayloadKind::Audio && !item.audioBytes.empty()) {
        path = stem + L".wav";
        if (!write_bytes(path, item.audioBytes.data(), item.audioBytes.size()))
            return false;
        item.materializedPath = path;
        return true;
    }
    if (!item.rawBytes.empty()) {
        path = stem + (item.rawExt.empty() ? L".bin" : item.rawExt);
        if (!write_bytes(path, item.rawBytes.data(), item.rawBytes.size()))
            return false;
        item.materializedPath = path;
        return true;
    }
    return false;
}

namespace {

HGLOBAL alloc_moveable(const void* data, size_t n)
{
    if (!data || n == 0)
        return nullptr;
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, n);
    if (!h)
        return nullptr;
    void* p = ::GlobalLock(h);
    if (!p) {
        ::GlobalFree(h);
        return nullptr;
    }
    std::memcpy(p, data, n);
    ::GlobalUnlock(h);
    return h;
}

HGLOBAL alloc_hdrop(const std::vector<std::wstring>& paths)
{
    size_t chars = 1;
    for (const auto& p : paths)
        chars += p.size() + 1;
    const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL h = ::GlobalAlloc(GHND, bytes);
    if (!h)
        return nullptr;
    auto* df = static_cast<DROPFILES*>(::GlobalLock(h));
    if (!df) {
        ::GlobalFree(h);
        return nullptr;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    auto* dest = reinterpret_cast<wchar_t*>(
        reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) {
        std::memcpy(dest, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
        dest += p.size() + 1;
    }
    *dest = L'\0';
    ::GlobalUnlock(h);
    return h;
}

std::vector<std::wstring> snapshot_file_paths(const ClipboardHistoryItem& item)
{
    if (!item.paths.empty())
        return item.paths;
    std::vector<std::wstring> paths;
    paths.reserve(item.items.size());
    for (const auto& ci : item.items) {
        if (ci.path && !ci.path->empty())
            paths.push_back(*ci.path);
        else if (ci.parsingName && !ci.parsingName->empty())
            paths.push_back(*ci.parsingName);
    }
    return paths;
}

bool restore_files(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return false;

    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(::SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
            pidls.push_back(pidl);
    }
    bool oleOk = false;
    if (!pidls.empty()) {
        IShellItemArray* arr = nullptr;
        std::vector<PCIDLIST_ABSOLUTE> abs(pidls.begin(), pidls.end());
        if (SUCCEEDED(::SHCreateShellItemArrayFromIDLists(
                static_cast<UINT>(abs.size()), abs.data(), &arr))
            && arr) {
            IDataObject* data = nullptr;
            if (SUCCEEDED(arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))
                && data) {
                oleOk = SUCCEEDED(::OleSetClipboard(data));
                data->Release();
            }
            arr->Release();
        }
    }
    for (auto* pidl : pidls)
        ::CoTaskMemFree(pidl);
    if (oleOk)
        return true;

    HGLOBAL h = alloc_hdrop(paths);
    if (!h)
        return false;
    if (!::OpenClipboard(nullptr)) {
        ::GlobalFree(h);
        return false;
    }
    ClipGuard clip;
    ::EmptyClipboard();
    if (!::SetClipboardData(CF_HDROP, h)) {
        ::GlobalFree(h);
        return false;
    }
    return true;
}

bool restore_open_formats(const ClipboardHistoryItem& item)
{
    if (!::OpenClipboard(nullptr))
        return false;
    ClipGuard clip;
    ::EmptyClipboard();
    bool ok = false;

    auto set_hglobal = [&](UINT fmt, HGLOBAL h) {
        if (!h)
            return;
        if (::SetClipboardData(fmt, h))
            ok = true;
        else
            ::GlobalFree(h);
    };

    if (item.kind == ClipboardPayloadKind::Text && !item.text.empty()) {
        const size_t bytes = (item.text.size() + 1) * sizeof(wchar_t);
        set_hglobal(CF_UNICODETEXT, alloc_moveable(item.text.c_str(), bytes));
    } else if (item.kind == ClipboardPayloadKind::Image) {
        if (!item.imageDib.empty())
            set_hglobal(CF_DIB, alloc_moveable(item.imageDib.data(), item.imageDib.size()));
        if (!item.imagePng.empty()) {
            const UINT png = ::RegisterClipboardFormatW(L"PNG");
            if (png)
                set_hglobal(png, alloc_moveable(item.imagePng.data(), item.imagePng.size()));
        }
    } else if (item.kind == ClipboardPayloadKind::Audio && !item.audioBytes.empty()) {
        set_hglobal(CF_WAVE, alloc_moveable(item.audioBytes.data(), item.audioBytes.size()));
    } else if (item.kind == ClipboardPayloadKind::Metafile && !item.rawBytes.empty()) {
        HENHMETAFILE hemf = ::SetEnhMetaFileBits(
            static_cast<UINT>(item.rawBytes.size()), item.rawBytes.data());
        if (hemf) {
            if (::SetClipboardData(CF_ENHMETAFILE, hemf))
                ok = true;
            else
                ::DeleteEnhMetaFile(hemf);
        }
    } else if (!item.rawBytes.empty() && item.primaryFormat != 0) {
        set_hglobal(item.primaryFormat,
                    alloc_moveable(item.rawBytes.data(), item.rawBytes.size()));
    }
    return ok;
}

} // namespace

bool restore_clipboard_snapshot(const ClipboardHistoryItem& item)
{
    if (item.kind == ClipboardPayloadKind::Files)
        return restore_files(snapshot_file_paths(item));
    return restore_open_formats(item);
}

} // namespace media::win::context

#endif
