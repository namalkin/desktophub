#include "cover.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <map>
#include <memory>
#include <vector>

#include "util.h"

using namespace Gdiplus;

namespace cover {

namespace {

// GDI+ рисует прямо из нашего буфера, поэтому буфер живёт столько же, сколько Bitmap.
struct Entry {
    std::vector<BYTE> pixels;
    std::unique_ptr<Bitmap> bitmap;
};

std::map<std::wstring, Entry> g_cache;
constexpr size_t kMaxEntries = 64;  // обложки тяжёлые, всю папку в памяти не держим

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
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return false;

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

// SIIGBF_THUMBNAILONLY — именно обложка из тега, без подстановки значка «файл mp3».
// Значок нам не нужен: если обложки нет, блок рисует свою ноту, и это выглядит лучше.
bool LoadThumbnail(const std::wstring& path, int px, std::vector<BYTE>& pixels, int& w, int& h) {
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
        if (SUCCEEDED(factory->GetImage(size, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbm)) && hbm) {
            ok = ReadBitmapBits(hbm, pixels, w, h);
            DeleteObject(hbm);
        }
        factory->Release();
    }
    item->Release();
    return ok;
}

}  // namespace

Bitmap* Get(const std::wstring& path, int px) {
    if (path.empty() || px <= 0) return nullptr;

    // Размер округляем до 32 пикселей: при растягивании блока мышью иначе пересоздавался
    // бы кэш на каждый пиксель ширины.
    const int bucket = ((px + 31) / 32) * 32;
    const std::wstring key = std::to_wstring(bucket) + L"|" + path;
    auto found = g_cache.find(key);
    if (found != g_cache.end()) return found->second.bitmap.get();

    if (g_cache.size() >= kMaxEntries) g_cache.clear();

    Entry entry;
    int w = 0, h = 0;
    if (!LoadThumbnail(path, bucket, entry.pixels, w, h)) {
        g_cache.emplace(key, Entry{});  // не нашли — не пытаемся снова на каждой перерисовке
        return nullptr;
    }
    // Обложка непрозрачна, но эскиз оболочки иногда приходит с нулевой альфой.
    if (!HasAlpha(entry.pixels)) {
        for (size_t i = 3; i < entry.pixels.size(); i += 4) entry.pixels[i] = 255;
    }
    entry.bitmap.reset(new Bitmap(w, h, w * 4, PixelFormat32bppPARGB, entry.pixels.data()));

    Bitmap* result = entry.bitmap.get();
    g_cache.emplace(key, std::move(entry));
    return result;
}

void Clear() { g_cache.clear(); }

}  // namespace cover
