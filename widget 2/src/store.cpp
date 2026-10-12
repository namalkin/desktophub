#include "store.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "json.h"
#include "sys.h"
#include "util.h"

namespace {

constexpr int kBackupsToKeep = 7;

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (8 << 20)) {
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

// Пишем во временный файл и подменяем — так данные не теряются при сбое записи.
bool WriteWholeFileAtomic(const std::wstring& path, const std::string& content) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

// Раз в сутки откладываем копию data.json и держим последние kBackupsToKeep штук.
void RotateBackups() {
    const std::wstring source = DataFile();
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) return;  // ещё нечего копировать

    const std::wstring dir = BackupDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    const std::wstring today = Utf8ToWide(TodayIso());
    const std::wstring target = dir + L"\\data-" + today + L".json";
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) return;  // копия за сегодня уже есть
    CopyFileW(source.c_str(), target.c_str(), TRUE);

    std::vector<std::wstring> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\data-*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) found.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (found.size() <= static_cast<size_t>(kBackupsToKeep)) return;

    std::sort(found.begin(), found.end());  // имена с ISO-датой сортируются хронологически
    const size_t extra = found.size() - static_cast<size_t>(kBackupsToKeep);
    for (size_t i = 0; i < extra; i++) {
        DeleteFileW((dir + L"\\" + found[i]).c_str());
    }
}

int ClampInt(int value, int lo, int hi) { return value < lo ? lo : (value > hi ? hi : value); }

}  // namespace

int AppData::TotalItems() const {
    int total = 0;
    for (const Group& g : groups) total += static_cast<int>(g.items.size());
    return total;
}

void RefreshBrokenFlags(AppData& data) {
    for (Group& group : data.groups) {
        for (Shortcut& item : group.items) {
            item.broken = !ShortcutTargetExists(item.path);
        }
    }
}

bool LoadData(AppData& data) {
    std::string raw;
    if (!ReadWholeFile(DataFile(), raw)) return false;

    mj::Value root;
    if (!mj::Parse(raw, root) || root.type != mj::Type::Object) return false;

    // Оформление читаем первым: блоки без своих полей наследуют его.
    if (const mj::Value* look = root.Find("appearance")) {
        if (look->type == mj::Type::Object) {
            if (const mj::Value* t = look->Find("theme")) {
                if (t->type == mj::Type::String) data.darkTheme = (t->str != "light");
            }
            if (const mj::Value* a = look->Find("accent")) {
                if (a->type == mj::Type::Number) data.accent = static_cast<int>(a->number);
            }
            if (const mj::Value* o = look->Find("opacity")) {
                if (o->type == mj::Type::Number) data.opacity = ClampInt(static_cast<int>(o->number), 40, 100);
            }
            if (const mj::Value* s = look->Find("iconSize")) {
                if (s->type == mj::Type::Number) data.iconSize = ClampInt(static_cast<int>(s->number), 24, 96);
            }
            if (const mj::Value* l = look->Find("labels")) data.showLabels = l->BoolOr(true);
        }
    }

    if (const mj::Value* list = root.Find("groups")) {
        if (list->type == mj::Type::Array) {
            std::vector<Group> groups;
            for (const mj::Value& item : list->arr) {
                if (item.type != mj::Type::Object) continue;
                Group group;
                if (const mj::Value* t = item.Find("title")) {
                    if (t->type == mj::Type::String && !t->str.empty()) group.title = Utf8ToWide(t->str);
                }
                if (const mj::Value* c = item.Find("collapsed")) group.collapsed = c->BoolOr(false);
                // Оформление блока: если полей нет (файл старой версии) — берём общее.
                group.darkTheme = data.darkTheme;
                group.accent = data.accent;
                group.opacity = data.opacity;
                group.iconSize = data.iconSize;
                group.showLabels = data.showLabels;

                if (const mj::Value* c = item.Find("columns")) {
                    if (c->type == mj::Type::Number) group.columns = ClampInt(static_cast<int>(c->number), 1, 12);
                }
                if (const mj::Value* r = item.Find("maxRows")) {
                    if (r->type == mj::Type::Number) group.maxRows = ClampInt(static_cast<int>(r->number), 0, 40);
                }
                if (const mj::Value* t = item.Find("theme")) {
                    if (t->type == mj::Type::String) group.darkTheme = (t->str != "light");
                }
                if (const mj::Value* a = item.Find("accent")) {
                    if (a->type == mj::Type::Number) group.accent = static_cast<int>(a->number);
                }
                if (const mj::Value* o = item.Find("opacity")) {
                    if (o->type == mj::Type::Number) group.opacity = ClampInt(static_cast<int>(o->number), 40, 100);
                }
                if (const mj::Value* s = item.Find("iconSize")) {
                    if (s->type == mj::Type::Number) group.iconSize = ClampInt(static_cast<int>(s->number), 24, 96);
                }
                if (const mj::Value* l = item.Find("labels")) group.showLabels = l->BoolOr(true);
                if (const mj::Value* k = item.Find("locked")) group.locked = k->BoolOr(false);
                if (const mj::Value* v = item.Find("x")) {
                    if (v->type == mj::Type::Number) group.x = static_cast<int>(v->number);
                }
                if (const mj::Value* v = item.Find("y")) {
                    if (v->type == mj::Type::Number) group.y = static_cast<int>(v->number);
                }
                if (const mj::Value* items = item.Find("items")) {
                    if (items->type == mj::Type::Array) {
                        for (const mj::Value& raw_item : items->arr) {
                            if (raw_item.type != mj::Type::Object) continue;
                            Shortcut shortcut;
                            if (const mj::Value* p = raw_item.Find("path")) {
                                if (p->type != mj::Type::String || p->str.empty()) continue;
                                shortcut.path = Utf8ToWide(p->str);
                            } else {
                                continue;  // ярлык без пути бесполезен
                            }
                            if (const mj::Value* n = raw_item.Find("name")) {
                                if (n->type == mj::Type::String) shortcut.name = Utf8ToWide(n->str);
                            }
                            if (const mj::Value* a = raw_item.Find("args")) {
                                if (a->type == mj::Type::String) shortcut.args = Utf8ToWide(a->str);
                            }
                            if (shortcut.name.empty()) shortcut.name = FileStem(shortcut.path);
                            group.items.push_back(std::move(shortcut));
                        }
                    }
                }
                groups.push_back(std::move(group));
            }
            if (!groups.empty()) data.groups = std::move(groups);
        }
    }

    if (const mj::Value* hk = root.Find("hotkey")) data.hotkeyEnabled = hk->BoolOr(true);
    if (const mj::Value* mv = root.Find("moveIntoStorage")) data.moveIntoStorage = mv->BoolOr(true);
    if (const mj::Value* sn = root.Find("snapToGrid")) data.snapToGrid = sn->BoolOr(false);
    if (const mj::Value* t = root.Find("alwaysOnTop")) data.alwaysOnTop = t->BoolOr(false);
    return true;
}

bool SaveData(const AppData& data) {
    mj::Value root = mj::Value::Object();

    mj::Value groups = mj::Value::Array();
    for (const Group& group : data.groups) {
        mj::Value item = mj::Value::Object();
        item.Set("title", mj::Value::Str(WideToUtf8(group.title)));
        item.Set("collapsed", mj::Value::Bool(group.collapsed));
        item.Set("columns", mj::Value::Num(group.columns));
        item.Set("maxRows", mj::Value::Num(group.maxRows));
        item.Set("x", mj::Value::Num(group.x));
        item.Set("y", mj::Value::Num(group.y));
        item.Set("theme", mj::Value::Str(group.darkTheme ? "dark" : "light"));
        item.Set("accent", mj::Value::Num(group.accent));
        item.Set("opacity", mj::Value::Num(group.opacity));
        item.Set("iconSize", mj::Value::Num(group.iconSize));
        item.Set("labels", mj::Value::Bool(group.showLabels));
        item.Set("locked", mj::Value::Bool(group.locked));
        mj::Value items = mj::Value::Array();
        for (const Shortcut& shortcut : group.items) {
            mj::Value entry = mj::Value::Object();
            entry.Set("path", mj::Value::Str(WideToUtf8(shortcut.path)));
            entry.Set("name", mj::Value::Str(WideToUtf8(shortcut.name)));
            entry.Set("args", mj::Value::Str(WideToUtf8(shortcut.args)));
            items.arr.push_back(std::move(entry));
        }
        item.Set("items", std::move(items));
        groups.arr.push_back(std::move(item));
    }
    root.Set("groups", std::move(groups));

    mj::Value look = mj::Value::Object();
    look.Set("theme", mj::Value::Str(data.darkTheme ? "dark" : "light"));
    look.Set("accent", mj::Value::Num(data.accent));
    look.Set("opacity", mj::Value::Num(data.opacity));
    look.Set("iconSize", mj::Value::Num(data.iconSize));
    look.Set("labels", mj::Value::Bool(data.showLabels));
    root.Set("appearance", std::move(look));

    root.Set("hotkey", mj::Value::Bool(data.hotkeyEnabled));
    root.Set("moveIntoStorage", mj::Value::Bool(data.moveIntoStorage));
    root.Set("snapToGrid", mj::Value::Bool(data.snapToGrid));
    root.Set("alwaysOnTop", mj::Value::Bool(data.alwaysOnTop));

    const std::wstring dir = DataDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);  // no-op, если уже есть
    RotateBackups();
    return WriteWholeFileAtomic(DataFile(), mj::Serialize(root));
}
