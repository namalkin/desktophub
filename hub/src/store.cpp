#include "store.h"

#include <shlobj.h>
#include <windows.h>

#include "json.h"
#include "util.h"

namespace {

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (1 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);

    // BOM, если файл правили вручную в блокноте.
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

// Пишем во временный файл и подменяем — так настройки не теряются при сбое записи.
bool WriteWholeFileAtomic(const std::wstring& path, const std::string& content) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = TRUE;
    if (!content.empty()) {
        ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    }
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != content.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::wstring ReadString(const mj::Value& root, const char* key) {
    if (const mj::Value* v = root.Find(key)) {
        if (v->type == mj::Type::String) return Utf8ToWide(v->str);
    }
    return std::wstring();
}

bool ReadBool(const mj::Value& root, const char* key, bool fallback) {
    if (const mj::Value* v = root.Find(key)) return v->BoolOr(fallback);
    return fallback;
}

int ReadInt(const mj::Value& root, const char* key, int fallback) {
    if (const mj::Value* v = root.Find(key)) {
        if (v->type == mj::Type::Number) return static_cast<int>(v->number);
    }
    return fallback;
}

}  // namespace

std::wstring HubData::PathOf(const std::string& id) const {
    for (const WidgetPath& p : paths) {
        if (p.id == id) return p.path;
    }
    return std::wstring();
}

void HubData::SetPath(const std::string& id, const std::wstring& path) {
    for (WidgetPath& p : paths) {
        if (p.id == id) {
            p.path = path;
            return;
        }
    }
    paths.push_back(WidgetPath{id, path});
}

std::wstring NormalizeRepo(const std::wstring& input) {
    std::wstring s = TrimSpaces(input);
    for (const wchar_t* prefix : {L"https://", L"http://"}) {
        const std::wstring p = prefix;
        if (s.compare(0, p.size(), p) == 0) s.erase(0, p.size());
    }
    const std::wstring host = L"github.com/";
    if (s.compare(0, host.size(), host) == 0) s.erase(0, host.size());
    if (s.size() > 4 && s.compare(s.size() - 4, 4, L".git") == 0) s.resize(s.size() - 4);

    // Оставляем ровно «owner/name»: хвост вроде /releases/latest отбрасываем.
    const size_t first = s.find(L'/');
    if (first == std::wstring::npos || first == 0) return std::wstring();
    size_t second = s.find(L'/', first + 1);
    if (second == std::wstring::npos) second = s.size();
    if (second == first + 1) return std::wstring();
    std::wstring repo = s.substr(0, second);
    for (wchar_t ch : repo) {
        const bool ok = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') ||
                        ch == L'-' || ch == L'_' || ch == L'.' || ch == L'/';
        if (!ok) return std::wstring();
    }
    return repo;
}

bool LoadData(HubData& data) {
    std::string raw;
    if (!ReadWholeFile(DataFile(), raw)) return false;

    mj::Value root;
    if (!mj::Parse(raw, root) || root.type != mj::Type::Object) return false;

    if (const mj::Value* t = root.Find("theme")) {
        if (t->type == mj::Type::String) data.darkTheme = (t->str != "light");
    }
    data.accent = Clamp(ReadInt(root, "accent", data.accent), 0, 15);  // лишнее отсечёт SetTheme
    data.repo = NormalizeRepo(ReadString(root, "repo"));
    data.closeToTray = ReadBool(root, "closeToTray", data.closeToTray);
    if (const mj::Value* sel = root.Find("selected")) {
        if (sel->type == mj::Type::String) data.selected = sel->str;
    }

    if (const mj::Value* win = root.Find("window")) {
        if (win->type == mj::Type::Object) {
            data.winX = ReadInt(*win, "x", data.winX);
            data.winY = ReadInt(*win, "y", data.winY);
            data.winW = Clamp(ReadInt(*win, "w", data.winW), 640, 8000);
            data.winH = Clamp(ReadInt(*win, "h", data.winH), 440, 8000);
            data.maximized = ReadBool(*win, "maximized", data.maximized);
        }
    }

    if (const mj::Value* list = root.Find("widgets")) {
        if (list->type == mj::Type::Object) {
            for (const auto& kv : list->obj) {
                if (kv.second.type != mj::Type::Object) continue;
                const std::wstring path = ReadString(kv.second, "path");
                if (!path.empty()) data.SetPath(kv.first, path);
            }
        }
    }
    return true;
}

bool SaveData(const HubData& data) {
    mj::Value root = mj::Value::Object();
    root.Set("theme", mj::Value::Str(data.darkTheme ? "dark" : "light"));
    root.Set("accent", mj::Value::Num(data.accent));
    root.Set("repo", mj::Value::Str(WideToUtf8(data.repo)));
    root.Set("closeToTray", mj::Value::Bool(data.closeToTray));
    root.Set("selected", mj::Value::Str(data.selected));

    mj::Value win = mj::Value::Object();
    win.Set("x", mj::Value::Num(data.winX));
    win.Set("y", mj::Value::Num(data.winY));
    win.Set("w", mj::Value::Num(data.winW));
    win.Set("h", mj::Value::Num(data.winH));
    win.Set("maximized", mj::Value::Bool(data.maximized));
    root.Set("window", std::move(win));

    mj::Value list = mj::Value::Object();
    for (const WidgetPath& p : data.paths) {
        mj::Value entry = mj::Value::Object();
        entry.Set("path", mj::Value::Str(WideToUtf8(p.path)));
        list.Set(p.id, std::move(entry));
    }
    root.Set("widgets", std::move(list));

    const std::wstring dir = DataDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);  // no-op, если уже есть
    return WriteWholeFileAtomic(DataFile(), mj::Serialize(root));
}
