#include "store.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "json.h"
#include "util.h"

namespace {

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (4 << 20)) {
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

int ClampInt(int value, int lo, int hi) { return value < lo ? lo : (value > hi ? hi : value); }

std::wstring StringField(const mj::Value& obj, const char* key) {
    if (const mj::Value* v = obj.Find(key)) {
        if (v->type == mj::Type::String) return Utf8ToWide(v->str);
    }
    return std::wstring();
}

int IntField(const mj::Value& obj, const char* key, int fallback) {
    if (const mj::Value* v = obj.Find(key)) {
        if (v->type == mj::Type::Number) return static_cast<int>(v->number);
    }
    return fallback;
}

unsigned long long BigField(const mj::Value& obj, const char* key) {
    if (const mj::Value* v = obj.Find(key)) {
        if (v->type == mj::Type::Number && v->number > 0) {
            return static_cast<unsigned long long>(v->number);
        }
    }
    return 0;
}

bool BoolField(const mj::Value& obj, const char* key, bool fallback) {
    if (const mj::Value* v = obj.Find(key)) return v->BoolOr(fallback);
    return fallback;
}

}  // namespace

// ---------- Типы блоков ----------

const wchar_t* PanelKindName(PanelKind kind) {
    switch (kind) {
        case PanelKind::Vacuum: return L"Пылесос";
        case PanelKind::Disk: return L"Диск";
        case PanelKind::Breakdown: return L"Куда ушло место";
        case PanelKind::Folders: return L"Папки-тяжеловесы";
        case PanelKind::BigFiles: return L"Большие файлы";
        case PanelKind::Junk: return L"Мусор";
        case PanelKind::Old: return L"Давно не открывали";
        case PanelKind::Orphans: return L"Хвосты программ";
        case PanelKind::System: return L"Системные файлы";
        default: return L"Блок";
    }
}

const wchar_t* PanelKindHint(PanelKind kind) {
    switch (kind) {
        case PanelKind::Vacuum: return L"Кнопка «Просканировать», ход работы и итог уборки";
        case PanelKind::Disk: return L"Кольцо «занято / свободно» и выбор диска";
        case PanelKind::Breakdown: return L"Доли: Windows, программы, профили, скрытое";
        case PanelKind::Folders: return L"Самые тяжёлые папки со столбиками";
        case PanelKind::BigFiles: return L"Крупные файлы: открыть папку или удалить";
        case PanelKind::Junk: return L"Временные файлы, кэши, корзина — с галочками";
        case PanelKind::Old: return L"Большие файлы, которых давно не касались";
        case PanelKind::Orphans: return L"Папки, для которых не нашлось программы";
        case PanelKind::System: return L"Гибернация, подкачка, точки восстановления";
        default: return L"";
    }
}

void PanelDefaultSize(PanelKind kind, int& width, int& height) {
    switch (kind) {
        case PanelKind::Vacuum: width = 300; height = 210; break;
        case PanelKind::Disk: width = 260; height = 250; break;
        case PanelKind::Breakdown: width = 320; height = 310; break;
        case PanelKind::Folders: width = 340; height = 300; break;
        case PanelKind::BigFiles: width = 360; height = 300; break;
        case PanelKind::Junk: width = 350; height = 340; break;
        case PanelKind::Old: width = 340; height = 280; break;
        case PanelKind::Orphans: width = 340; height = 280; break;
        case PanelKind::System: width = 340; height = 290; break;
        default: width = 320; height = 260; break;
    }
}

// ---------- Данные ----------

bool AppData::JunkChecked(int id) const {
    return std::find(checkedJunk.begin(), checkedJunk.end(), id) != checkedJunk.end();
}

void AppData::ToggleJunk(int id) {
    auto it = std::find(checkedJunk.begin(), checkedJunk.end(), id);
    if (it == checkedJunk.end()) checkedJunk.push_back(id);
    else checkedJunk.erase(it);
}

int AppData::PanelCount(PanelKind kind) const {
    int count = 0;
    for (const Panel& panel : panels) {
        if (panel.kind == kind) count++;
    }
    return count;
}

void FillDefaults(AppData& data) {
    const PanelKind starters[] = {PanelKind::Vacuum, PanelKind::Disk, PanelKind::Breakdown, PanelKind::Junk};
    for (PanelKind kind : starters) {
        Panel panel;
        panel.kind = kind;
        panel.darkTheme = data.darkTheme;
        panel.accent = data.accent;
        panel.opacity = data.opacity;
        data.panels.push_back(panel);
    }
}

// ---------- Чтение ----------

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
            data.accent = IntField(*look, "accent", data.accent);
            data.opacity = ClampInt(IntField(*look, "opacity", data.opacity), 40, 100);
            data.fontIndex = IntField(*look, "font", data.fontIndex);
        }
    }

    if (const mj::Value* list = root.Find("panels")) {
        if (list->type == mj::Type::Array) {
            std::vector<Panel> panels;
            for (const mj::Value& item : list->arr) {
                if (item.type != mj::Type::Object) continue;
                const int kind = IntField(item, "kind", -1);
                if (kind < 0 || kind >= static_cast<int>(PanelKind::Count)) continue;

                Panel panel;
                panel.kind = static_cast<PanelKind>(kind);
                panel.title = StringField(item, "title");
                panel.collapsed = BoolField(item, "collapsed", false);
                panel.locked = BoolField(item, "locked", false);
                panel.x = IntField(item, "x", INT32_MIN);
                panel.y = IntField(item, "y", INT32_MIN);
                panel.width = ClampInt(IntField(item, "width", 0), 0, 2400);
                panel.height = ClampInt(IntField(item, "height", 0), 0, 2400);

                panel.darkTheme = data.darkTheme;
                panel.accent = data.accent;
                panel.opacity = data.opacity;
                if (const mj::Value* t = item.Find("theme")) {
                    if (t->type == mj::Type::String) panel.darkTheme = (t->str != "light");
                }
                panel.accent = IntField(item, "accent", panel.accent);
                panel.opacity = ClampInt(IntField(item, "opacity", panel.opacity), 40, 100);
                panel.showChart = BoolField(item, "chart", true);
                panel.showHints = BoolField(item, "hints", true);
                panel.expanded = ClampInt(IntField(item, "expanded", -1), -1, 15);
                panels.push_back(std::move(panel));
            }
            data.panels = std::move(panels);
        }
    }

    data.alwaysOnTop = BoolField(root, "onTop", data.alwaysOnTop);
    data.hotkeyEnabled = BoolField(root, "hotkey", data.hotkeyEnabled);
    data.snapToGrid = BoolField(root, "snapToGrid", data.snapToGrid);
    data.scanOnStart = BoolField(root, "scanOnStart", data.scanOnStart);
    data.confirmDelete = BoolField(root, "confirmDelete", data.confirmDelete);
    data.freedTotal = BigField(root, "freedTotal");

    const std::wstring drive = StringField(root, "drive");
    if (!drive.empty() && iswalpha(drive[0])) data.drive = static_cast<wchar_t>(towupper(drive[0]));

    if (const mj::Value* opts = root.Find("search")) {
        if (opts->type == mj::Type::Object) {
            data.options.bigFileMinMb = ClampInt(IntField(*opts, "bigMb", data.options.bigFileMinMb), 10, 4096);
            data.options.oldFileDays = ClampInt(IntField(*opts, "oldDays", data.options.oldFileDays), 90, 3650);
            data.options.oldFileMinMb = ClampInt(IntField(*opts, "oldMb", data.options.oldFileMinMb), 1, 2048);
            data.options.orphanDays = ClampInt(IntField(*opts, "orphanDays", data.options.orphanDays), 30,
                                               3650);
            data.options.skipUserFiles = BoolField(*opts, "skipPersonal", data.options.skipUserFiles);
        }
    }

    if (const mj::Value* checked = root.Find("junk")) {
        if (checked->type == mj::Type::Array) {
            for (const mj::Value& item : checked->arr) {
                if (item.type == mj::Type::Number) data.checkedJunk.push_back(static_cast<int>(item.number));
            }
        }
    }

    if (const mj::Value* memory = root.Find("memory")) {
        if (memory->type == mj::Type::Object) {
            data.memory.valid = BoolField(*memory, "valid", false);
            data.memory.used = BigField(*memory, "used");
            data.memory.total = BigField(*memory, "total");
            data.memory.junk = BigField(*memory, "junk");
            data.memory.when = StringField(*memory, "when");
            const std::wstring letter = StringField(*memory, "drive");
            if (!letter.empty()) data.memory.drive = static_cast<wchar_t>(towupper(letter[0]));
        }
    }
    return true;
}

// ---------- Запись ----------

bool SaveData(AppData& data) {
    mj::Value root = mj::Value::Object();

    mj::Value panels = mj::Value::Array();
    for (const Panel& panel : data.panels) {
        mj::Value item = mj::Value::Object();
        item.Set("kind", mj::Value::Num(static_cast<int>(panel.kind)));
        item.Set("title", mj::Value::Str(WideToUtf8(panel.title)));
        item.Set("collapsed", mj::Value::Bool(panel.collapsed));
        item.Set("locked", mj::Value::Bool(panel.locked));
        item.Set("x", mj::Value::Num(panel.x));
        item.Set("y", mj::Value::Num(panel.y));
        item.Set("width", mj::Value::Num(panel.width));
        item.Set("height", mj::Value::Num(panel.height));
        item.Set("theme", mj::Value::Str(panel.darkTheme ? "dark" : "light"));
        item.Set("accent", mj::Value::Num(panel.accent));
        item.Set("opacity", mj::Value::Num(panel.opacity));
        item.Set("chart", mj::Value::Bool(panel.showChart));
        item.Set("hints", mj::Value::Bool(panel.showHints));
        item.Set("expanded", mj::Value::Num(panel.expanded));
        panels.arr.push_back(std::move(item));
    }
    root.Set("panels", std::move(panels));

    mj::Value look = mj::Value::Object();
    look.Set("theme", mj::Value::Str(data.darkTheme ? "dark" : "light"));
    look.Set("accent", mj::Value::Num(data.accent));
    look.Set("opacity", mj::Value::Num(data.opacity));
    look.Set("font", mj::Value::Num(data.fontIndex));
    root.Set("appearance", std::move(look));

    root.Set("onTop", mj::Value::Bool(data.alwaysOnTop));
    root.Set("hotkey", mj::Value::Bool(data.hotkeyEnabled));
    root.Set("snapToGrid", mj::Value::Bool(data.snapToGrid));
    root.Set("scanOnStart", mj::Value::Bool(data.scanOnStart));
    root.Set("confirmDelete", mj::Value::Bool(data.confirmDelete));
    root.Set("freedTotal", mj::Value::Num(static_cast<double>(data.freedTotal)));
    root.Set("drive", mj::Value::Str(WideToUtf8(std::wstring(1, data.drive))));

    mj::Value options = mj::Value::Object();
    options.Set("bigMb", mj::Value::Num(data.options.bigFileMinMb));
    options.Set("oldDays", mj::Value::Num(data.options.oldFileDays));
    options.Set("oldMb", mj::Value::Num(data.options.oldFileMinMb));
    options.Set("orphanDays", mj::Value::Num(data.options.orphanDays));
    options.Set("skipPersonal", mj::Value::Bool(data.options.skipUserFiles));
    root.Set("search", std::move(options));

    mj::Value checked = mj::Value::Array();
    for (int id : data.checkedJunk) checked.arr.push_back(mj::Value::Num(id));
    root.Set("junk", std::move(checked));

    mj::Value memory = mj::Value::Object();
    memory.Set("valid", mj::Value::Bool(data.memory.valid));
    memory.Set("drive", mj::Value::Str(WideToUtf8(std::wstring(1, data.memory.drive))));
    memory.Set("used", mj::Value::Num(static_cast<double>(data.memory.used)));
    memory.Set("total", mj::Value::Num(static_cast<double>(data.memory.total)));
    memory.Set("junk", mj::Value::Num(static_cast<double>(data.memory.junk)));
    memory.Set("when", mj::Value::Str(WideToUtf8(data.memory.when)));
    root.Set("memory", std::move(memory));

    SHCreateDirectoryExW(nullptr, DataDir().c_str(), nullptr);
    return WriteWholeFileAtomic(DataFile(), mj::Serialize(root));
}

// ---------- Отчёт ----------

namespace {

void AppendLine(std::string& text, const std::wstring& line) {
    text += WideToUtf8(line);
    text += "\r\n";
}

std::wstring Pad(const std::wstring& text, size_t width) {
    std::wstring out = text;
    while (out.size() < width) out.push_back(L' ');
    return out;
}

}  // namespace

bool WriteReport(const scan::Result& result) {
    if (!result.valid) return false;

    std::string text = "\xEF\xBB\xBF";  // BOM: иначе блокнот покажет кракозябры
    wchar_t line[512];

    swprintf(line, 512, L"Диск-пылесос — отчёт о диске %lc:", result.drive);
    AppendLine(text, line);
    swprintf(line, 512, L"%02d.%02d.%04d %02d:%02d", result.finished.wDay, result.finished.wMonth,
             result.finished.wYear, result.finished.wHour, result.finished.wMinute);
    AppendLine(text, line);
    AppendLine(text, L"");

    swprintf(line, 512, L"Всего на диске: %ls", FormatSize(result.total).c_str());
    AppendLine(text, line);
    swprintf(line, 512, L"Занято:         %ls", FormatSize(result.used).c_str());
    AppendLine(text, line);
    swprintf(line, 512, L"Свободно:       %ls", FormatSize(result.freeSpace).c_str());
    AppendLine(text, line);
    swprintf(line, 512, L"Обошли:         %ls в %d файлах", FormatSize(result.scanned).c_str(),
             result.fileCount);
    AppendLine(text, line);
    if (result.hidden > 0) {
        swprintf(line, 512, L"Не заглянули:   %ls (%d защищённых папок)", FormatSize(result.hidden).c_str(),
                 result.deniedDirs);
        AppendLine(text, line);
    }

    AppendLine(text, L"");
    AppendLine(text, L"--- Куда ушло место ---");
    for (const scan::Slice& slice : result.slices) {
        const int percent = result.used > 0 ? static_cast<int>(slice.size * 100ULL / result.used) : 0;
        swprintf(line, 512, L"%ls %8ls  %2d%%", Pad(slice.name, 24).c_str(), FormatSize(slice.size).c_str(),
                 percent);
        AppendLine(text, line);
    }

    AppendLine(text, L"");
    AppendLine(text, L"--- Папки-тяжеловесы ---");
    for (const scan::FolderItem& folder : result.folders) {
        swprintf(line, 512, L"%8ls  %ls", FormatSize(folder.size).c_str(), folder.name.c_str());
        AppendLine(text, line);
    }

    AppendLine(text, L"");
    AppendLine(text, L"--- Мусор ---");
    for (const scan::JunkItem& item : result.junk) {
        swprintf(line, 512, L"%8ls  %ls%ls", FormatSize(item.size).c_str(), item.name.c_str(),
                 item.userData ? L"  (файлы пользователя — только вручную)" : L"");
        AppendLine(text, line);
    }

    AppendLine(text, L"");
    AppendLine(text, L"--- Большие файлы ---");
    for (const scan::FileItem& file : result.bigFiles) {
        swprintf(line, 512, L"%8ls  %ls", FormatSize(file.size).c_str(), file.path.c_str());
        AppendLine(text, line);
    }

    AppendLine(text, L"");
    AppendLine(text, L"--- Давно не открывали ---");
    for (const scan::FileItem& file : result.oldFiles) {
        swprintf(line, 512, L"%8ls  %ls  (%ls)", FormatSize(file.size).c_str(), file.path.c_str(),
                 AgeText(file.days).c_str());
        AppendLine(text, line);
    }

    AppendLine(text, L"");
    AppendLine(text, L"--- Хвосты программ ---");
    for (const scan::OrphanItem& item : result.orphans) {
        swprintf(line, 512, L"%8ls  %ls  (%ls)", FormatSize(item.size).c_str(), item.path.c_str(),
                 item.reason.c_str());
        AppendLine(text, line);
    }

    AppendLine(text, L"");
    AppendLine(text, L"--- Системные файлы ---");
    for (const scan::SysItem& item : result.system) {
        swprintf(line, 512, L"%8ls  %ls — %ls", FormatSize(item.size).c_str(), item.name.c_str(),
                 item.hint.c_str());
        AppendLine(text, line);
    }

    SHCreateDirectoryExW(nullptr, DataDir().c_str(), nullptr);
    return WriteWholeFileAtomic(ReportFile(), text);
}
