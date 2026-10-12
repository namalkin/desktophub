#include "store.h"

#include <shlobj.h>
#include <windows.h>

#include <string>

#include "json.h"
#include "util.h"

namespace {

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

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

library::NameMode AppData::NameModeValue() const {
    switch (nameMode) {
        case 1: return library::NameMode::ArtistFirst;
        case 2: return library::NameMode::TitleFirst;
        default: return library::NameMode::Tags;
    }
}

void AppData::ClampAll() {
    nameMode = Clamp(nameMode, 0, 2);
    volume = Clamp(volume, 0, 100);
    opacity = Clamp(opacity, 40, 100);
    accent = Clamp(accent, 0, 15);
    fontIndex = Clamp(fontIndex, 0, 15);
    listRows = Clamp(listRows, 0, 14);
    cardW = Clamp(cardW, 300, 620);
    const int repeatValue = Clamp(static_cast<int>(repeat), 0, 2);
    repeat = static_cast<RepeatMode>(repeatValue);
}

std::wstring DefaultMusicFolder() {
    PWSTR path = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &path))) {
        folder.assign(path);
        CoTaskMemFree(path);
    }
    if (folder.empty()) {
        wchar_t buf[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH) > 0) {
            folder = std::wstring(buf) + L"\\Music";
        }
    }
    return folder;
}

bool LoadData(AppData& data) {
    std::string raw;
    if (!ReadWholeFile(DataFile(), raw)) return false;

    mj::Value root;
    if (!mj::Parse(raw, root) || root.type != mj::Type::Object) return false;

    if (const mj::Value* music = root.Find("music")) {
        if (music->type == mj::Type::Object) {
            data.folder = ReadString(*music, "folder");
            data.recursive = ReadBool(*music, "recursive", data.recursive);
            data.nameMode = ReadInt(*music, "nameMode", data.nameMode);
            data.shuffle = ReadBool(*music, "shuffle", data.shuffle);
            data.repeat = static_cast<RepeatMode>(ReadInt(*music, "repeat", static_cast<int>(data.repeat)));
            data.volume = ReadInt(*music, "volume", data.volume);
            data.lastTrack = ReadString(*music, "lastTrack");
            if (const mj::Value* pos = music->Find("lastPosition")) {
                if (pos->type == mj::Type::Number && pos->number > 0.0) {
                    data.lastPositionMs = static_cast<unsigned long long>(pos->number);
                }
            }
            data.resumeOnStart = ReadBool(*music, "resumeOnStart", data.resumeOnStart);
            data.playOnStart = ReadBool(*music, "playOnStart", data.playOnStart);
        }
    }

    if (const mj::Value* look = root.Find("appearance")) {
        if (look->type == mj::Type::Object) {
            if (const mj::Value* t = look->Find("theme")) {
                if (t->type == mj::Type::String) data.darkTheme = (t->str != "light");
            }
            data.accent = ReadInt(*look, "accent", data.accent);
            data.opacity = ReadInt(*look, "opacity", data.opacity);
            data.fontIndex = ReadInt(*look, "font", data.fontIndex);
            data.showCover = ReadBool(*look, "cover", data.showCover);
            data.showList = ReadBool(*look, "list", data.showList);
            data.listRows = ReadInt(*look, "listRows", data.listRows);
            data.cardW = ReadInt(*look, "cardWidth", data.cardW);
        }
    }

    if (const mj::Value* common = root.Find("common")) {
        if (common->type == mj::Type::Object) {
            data.hotkeysEnabled = ReadBool(*common, "hotkeys", data.hotkeysEnabled);
            data.mediaKeys = ReadBool(*common, "mediaKeys", data.mediaKeys);
            data.locked = ReadBool(*common, "locked", data.locked);
        }
    }

    if (const mj::Value* win = root.Find("window")) {
        if (win->type == mj::Type::Object) {
            data.winX = ReadInt(*win, "x", data.winX);
            data.winY = ReadInt(*win, "y", data.winY);
            data.alwaysOnTop = ReadBool(*win, "alwaysOnTop", data.alwaysOnTop);
        }
    }

    data.ClampAll();
    return true;
}

bool SaveData(const AppData& data) {
    mj::Value root = mj::Value::Object();

    mj::Value music = mj::Value::Object();
    music.Set("folder", mj::Value::Str(WideToUtf8(data.folder)));
    music.Set("recursive", mj::Value::Bool(data.recursive));
    music.Set("nameMode", mj::Value::Num(data.nameMode));
    music.Set("shuffle", mj::Value::Bool(data.shuffle));
    music.Set("repeat", mj::Value::Num(static_cast<int>(data.repeat)));
    music.Set("volume", mj::Value::Num(data.volume));
    music.Set("lastTrack", mj::Value::Str(WideToUtf8(data.lastTrack)));
    music.Set("lastPosition", mj::Value::Num(static_cast<double>(data.lastPositionMs)));
    music.Set("resumeOnStart", mj::Value::Bool(data.resumeOnStart));
    music.Set("playOnStart", mj::Value::Bool(data.playOnStart));
    root.Set("music", std::move(music));

    mj::Value look = mj::Value::Object();
    look.Set("theme", mj::Value::Str(data.darkTheme ? "dark" : "light"));
    look.Set("accent", mj::Value::Num(data.accent));
    look.Set("opacity", mj::Value::Num(data.opacity));
    look.Set("font", mj::Value::Num(data.fontIndex));
    look.Set("cover", mj::Value::Bool(data.showCover));
    look.Set("list", mj::Value::Bool(data.showList));
    look.Set("listRows", mj::Value::Num(data.listRows));
    look.Set("cardWidth", mj::Value::Num(data.cardW));
    root.Set("appearance", std::move(look));

    mj::Value common = mj::Value::Object();
    common.Set("hotkeys", mj::Value::Bool(data.hotkeysEnabled));
    common.Set("mediaKeys", mj::Value::Bool(data.mediaKeys));
    common.Set("locked", mj::Value::Bool(data.locked));
    root.Set("common", std::move(common));

    mj::Value win = mj::Value::Object();
    win.Set("x", mj::Value::Num(data.winX));
    win.Set("y", mj::Value::Num(data.winY));
    win.Set("alwaysOnTop", mj::Value::Bool(data.alwaysOnTop));
    root.Set("window", std::move(win));

    const std::wstring dir = DataDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);  // no-op, если уже есть
    return WriteWholeFileAtomic(DataFile(), mj::Serialize(root));
}
