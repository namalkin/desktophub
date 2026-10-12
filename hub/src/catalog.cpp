#include "catalog.h"

#include <shlwapi.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>

#include "json.h"
#include "util.h"

using namespace Gdiplus;

namespace catalog {

namespace {

constexpr int kPakResourceId = 2;

struct Blob {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

std::map<std::string, Blob> g_files;  // имя в пакете -> байты прямо в образе .exe
std::vector<Entry> g_entries;
std::wstring g_repo;
std::map<std::string, std::unique_ptr<Bitmap>> g_images;  // ключ — путь в пакете
std::map<std::string, std::unique_ptr<Bitmap>> g_icons;   // ключ — id виджета

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool ParsePak(const uint8_t* p, size_t size) {
    if (size < 8 || std::memcmp(p, "WPAK", 4) != 0) return false;
    const uint32_t count = ReadU32(p + 4);
    size_t at = 8;
    for (uint32_t i = 0; i < count; i++) {
        if (at + 4 > size) return false;
        const uint32_t nameLen = ReadU32(p + at);
        at += 4;
        if (at + nameLen + 4 > size) return false;
        std::string name(reinterpret_cast<const char*>(p + at), nameLen);
        at += nameLen;
        const uint32_t dataLen = ReadU32(p + at);
        at += 4;
        if (at + dataLen > size) return false;
        g_files[name] = Blob{p + at, dataLen};
        at += dataLen;
    }
    return true;
}

std::wstring Str(const mj::Value& obj, const char* key) {
    const mj::Value* v = obj.Find(key);
    return v && v->type == mj::Type::String ? Utf8ToWide(v->str) : std::wstring();
}

const Blob* File(const std::string& name) {
    auto it = g_files.find(name);
    return it == g_files.end() ? nullptr : &it->second;
}

// Картинка из памяти. Bitmap, созданный из потока, держит поток до конца жизни, поэтому
// перерисовываем её в собственный битмап — заодно в PARGB, который GDI+ рисует быстрее.
std::unique_ptr<Bitmap> Decode(const uint8_t* data, size_t size) {
    IStream* stream = SHCreateMemStream(data, static_cast<UINT>(size));
    if (!stream) return nullptr;
    std::unique_ptr<Bitmap> result;
    {
        Bitmap decoded(stream);
        if (decoded.GetLastStatus() == Ok && decoded.GetWidth() > 0) {
            const int w = static_cast<int>(decoded.GetWidth());
            const int h = static_cast<int>(decoded.GetHeight());
            result.reset(new Bitmap(w, h, PixelFormat32bppPARGB));
            Graphics g(result.get());
            g.DrawImage(&decoded, 0, 0, w, h);
        }
    }
    stream->Release();
    return result;
}

// В .ico ищем кадр-PNG (256x256 у всех виджетов серии) — у него есть альфа, и GDI+
// читает его напрямую. Если PNG нет, берём ico целиком: GDI+ возьмёт первый кадр.
std::unique_ptr<Bitmap> DecodeIcon(const Blob& ico) {
    const uint8_t* p = ico.data;
    if (ico.size >= 6 && ReadU16(p + 2) == 1) {
        const int count = ReadU16(p + 4);
        const uint8_t* best = nullptr;
        size_t bestSize = 0;
        int bestDim = 0;
        for (int i = 0; i < count; i++) {
            const uint8_t* e = p + 6 + i * 16;
            if (e + 16 > p + ico.size) break;
            const int dim = e[0] == 0 ? 256 : e[0];
            const uint32_t bytes = ReadU32(e + 8);
            const uint32_t offset = ReadU32(e + 12);
            if (offset + bytes > ico.size || bytes < 8) continue;
            const bool png = std::memcmp(p + offset, "\x89PNG", 4) == 0;
            if (png && dim > bestDim) {
                best = p + offset;
                bestSize = bytes;
                bestDim = dim;
            }
        }
        if (best) return Decode(best, bestSize);
    }
    return Decode(ico.data, ico.size);
}

std::string Key(const Entry& entry, const std::wstring& ref) {
    std::wstring r = ref;
    if (r.compare(0, 2, L"./") == 0) r.erase(0, 2);
    return WideToUtf8(entry.folder + L"/" + r);
}

}  // namespace

bool Load() {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(kPakResourceId), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!res) return false;
    HGLOBAL handle = LoadResource(self, res);
    if (!handle) return false;
    const auto* data = static_cast<const uint8_t*>(LockResource(handle));
    const size_t size = SizeofResource(self, res);
    if (!data || !ParsePak(data, size)) return false;

    const Blob* json = File("catalog.json");
    if (!json) return false;
    mj::Value root;
    if (!mj::Parse(std::string(reinterpret_cast<const char*>(json->data), json->size), root)) return false;
    g_repo = Str(root, "repo");
    const mj::Value* list = root.Find("widgets");
    if (!list || list->type != mj::Type::Array) return false;

    for (const mj::Value& w : list->arr) {
        if (w.type != mj::Type::Object) continue;
        Entry e;
        if (const mj::Value* id = w.Find("id")) e.id = id->str;
        e.folder = Str(w, "folder");
        e.name = Str(w, "name");
        e.title = Str(w, "title");
        e.summary = Str(w, "summary");
        e.exe = Str(w, "exe");
        e.windowClass = Str(w, "window");
        e.runValue = Str(w, "run");
        if (e.id.empty() || e.exe.empty() || e.windowClass.empty()) continue;
        if (e.name.empty()) e.name = e.title;
        g_entries.push_back(std::move(e));
    }
    return !g_entries.empty();
}

const std::vector<Entry>& Entries() { return g_entries; }

const std::wstring& DefaultRepo() { return g_repo; }

int IndexOf(const std::string& id) {
    for (size_t i = 0; i < g_entries.size(); i++) {
        if (g_entries[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

std::string Readme(const Entry& entry) {
    const Blob* blob = File(WideToUtf8(entry.folder + L"/README.md"));
    if (!blob) return std::string();
    return std::string(reinterpret_cast<const char*>(blob->data), blob->size);
}

Bitmap* Image(const Entry& entry, const std::wstring& ref) {
    const std::string key = Key(entry, ref);
    auto it = g_images.find(key);
    if (it != g_images.end()) return it->second.get();
    const Blob* blob = File(key);
    std::unique_ptr<Bitmap> bmp = blob ? Decode(blob->data, blob->size) : nullptr;
    Bitmap* raw = bmp.get();
    g_images[key] = std::move(bmp);  // и неудачу запоминаем: не декодируем её на каждом кадре
    return raw;
}

Bitmap* Icon(const Entry& entry) {
    auto it = g_icons.find(entry.id);
    if (it != g_icons.end()) return it->second.get();
    const Blob* blob = File(WideToUtf8(entry.folder + L"/res/app.ico"));
    std::unique_ptr<Bitmap> bmp = blob ? DecodeIcon(*blob) : nullptr;
    Bitmap* raw = bmp.get();
    g_icons[entry.id] = std::move(bmp);
    return raw;
}

}  // namespace catalog
