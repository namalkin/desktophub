#include "icons.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <map>
#include <memory>
#include <vector>

#include "util.h"

using namespace Gdiplus;

namespace icons {

namespace {

// GDI+ рисует прямо из нашего буфера, поэтому буфер живёт столько же, сколько Bitmap.
struct Entry {
    std::vector<BYTE> pixels;
    std::unique_ptr<Bitmap> bitmap;
};

std::map<std::wstring, Entry> g_cache;

// Оболочка отдаёт 32bpp с premultiplied-альфой — тем же форматом, что и слоёное окно.
std::unique_ptr<Bitmap> MakeBitmap(std::vector<BYTE>& pixels, int w, int h) {
    return std::unique_ptr<Bitmap>(new Bitmap(w, h, w * 4, PixelFormat32bppPARGB, pixels.data()));
}

bool HasAlpha(const std::vector<BYTE>& pixels) {
    for (size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0) return true;
    }
    return false;
}

// Копирует HBITMAP в наш буфер BGRA сверху вниз.
bool ReadBitmapBits(HBITMAP source, std::vector<BYTE>& pixels, int& w, int& h) {
    BITMAP bm = {};
    if (GetObjectW(source, sizeof(bm), &bm) == 0) return false;
    w = bm.bmWidth;
    h = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    HDC screen = GetDC(nullptr);
    const int lines = GetDIBits(screen, source, 0, static_cast<UINT>(h), pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    return lines == h;
}

// HICON: цвет и маска лежат отдельно, альфа-канал есть не у всех значков.
bool ReadIconBits(HICON icon, std::vector<BYTE>& pixels, int& w, int& h) {
    ICONINFO info = {};
    if (!GetIconInfo(icon, &info)) return false;

    bool ok = info.hbmColor && ReadBitmapBits(info.hbmColor, pixels, w, h);
    if (ok && !HasAlpha(pixels)) {
        // Значок без альфы: прозрачность задаёт маска (1 — сквозной пиксель).
        std::vector<BYTE> mask;
        int mw = 0, mh = 0;
        const bool haveMask = info.hbmMask && ReadBitmapBits(info.hbmMask, mask, mw, mh) && mw == w;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                bool transparent = false;
                if (haveMask) {
                    // Маска может быть вдвое выше (XOR+AND в одном bitmap) — берём верхнюю часть.
                    const int my = (mh >= h * 2) ? y : (y < mh ? y : mh - 1);
                    transparent = mask[(static_cast<size_t>(my) * mw + x) * 4] != 0;
                }
                pixels[i + 3] = transparent ? 0 : 255;
                if (transparent) {
                    pixels[i] = pixels[i + 1] = pixels[i + 2] = 0;  // premultiplied
                }
            }
        }
    }

    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return ok;
}

// Основной путь: у оболочки есть готовое изображение любого размера, с альфой.
bool LoadViaShell(const std::wstring& path, int px, std::vector<BYTE>& pixels, int& w, int& h) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item)))) {
        return false;
    }
    IShellItemImageFactory* factory = nullptr;
    bool ok = false;
    if (SUCCEEDED(item->QueryInterface(IID_IShellItemImageFactory, reinterpret_cast<void**>(&factory)))) {
        HBITMAP hbm = nullptr;
        const SIZE size = {px, px};
        if (SUCCEEDED(factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm))) {
            ok = ReadBitmapBits(hbm, pixels, w, h);
            DeleteObject(hbm);
        }
        factory->Release();
    }
    item->Release();
    return ok;
}

// Запасной путь для старых оболочек и нестандартных путей.
bool LoadViaFileInfo(const std::wstring& path, int px, std::vector<BYTE>& pixels, int& w, int& h) {
    SHFILEINFOW info = {};
    const UINT flags = SHGFI_ICON | (px <= 20 ? SHGFI_SMALLICON : SHGFI_LARGEICON);
    if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), flags) || !info.hIcon) return false;
    const bool ok = ReadIconBits(info.hIcon, pixels, w, h);
    DestroyIcon(info.hIcon);
    return ok;
}

}  // namespace

Bitmap* Get(const std::wstring& path, int px) {
    if (path.empty() || px <= 0) return nullptr;
    if (IsUrl(path)) return nullptr;  // для ссылок значок рисует сам виджет

    const std::wstring key = std::to_wstring(px) + L"|" + path;
    auto found = g_cache.find(key);
    if (found != g_cache.end()) return found->second.bitmap.get();

    Entry entry;
    int w = 0, h = 0;
    if (!LoadViaShell(path, px, entry.pixels, w, h) && !LoadViaFileInfo(path, px, entry.pixels, w, h)) {
        g_cache.emplace(key, Entry{});  // не нашли — не пытаемся снова на каждой перерисовке
        return nullptr;
    }
    if (!HasAlpha(entry.pixels)) {
        for (size_t i = 3; i < entry.pixels.size(); i += 4) entry.pixels[i] = 255;
    }
    entry.bitmap = MakeBitmap(entry.pixels, w, h);

    Bitmap* result = entry.bitmap.get();
    g_cache.emplace(key, std::move(entry));
    return result;
}

void Clear() { g_cache.clear(); }

}  // namespace icons
