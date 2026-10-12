#include "sys.h"

#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>

#include "util.h"

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ShortcutDock";
constexpr UINT kTrayId = 1;

std::wstring Expand(const std::wstring& text) {
    if (text.find(L'%') == std::wstring::npos) return text;
    wchar_t buf[1024] = {0};
    const DWORD n = ExpandEnvironmentStringsW(text.c_str(), buf, 1024);
    return (n > 0 && n <= 1024) ? std::wstring(buf) : text;
}

}  // namespace

std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

bool IsAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;

    wchar_t value[MAX_PATH * 2] = {0};
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                            reinterpret_cast<LPBYTE>(value), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return false;

    // Запись могла остаться от .exe, который с тех пор переехал.
    const std::wstring expected = L"\"" + ExePath() + L"\"";
    return expected == value;
}

bool SetAutostart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }

    LSTATUS status;
    if (enable) {
        const std::wstring value = L"\"" + ExePath() + L"\"";
        status = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;  // и так выключено
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// Хозяин рабочего стола — то окно, в клиентской области которого оболочка держит значки
// (SHELLDLL_DefView): именно оно покрывает все мониторы и рисует обои. На Windows 10 это
// сам Progman, но на Windows 11 (особенно 24H2) значки живут в верхнеуровневом WorkerW,
// который в z-порядке выше Progman — дочернее окно Progman там прячется под обоями.
// Поэтому ищем владельца значков, а не окно с заранее известным именем класса.
namespace {

// Значки лежат либо прямо в окне, либо в его WorkerW-ребёнке.
[[maybe_unused]] HWND IconHostIn(HWND parent) {
    if (!parent) return nullptr;
    if (FindWindowExW(parent, nullptr, L"SHELLDLL_DefView", nullptr)) return parent;
    HWND worker = nullptr;
    while ((worker = FindWindowExW(parent, worker, L"WorkerW", nullptr)) != nullptr) {
        if (FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr)) return worker;
    }
    return nullptr;
}

// Верхнеуровневый WorkerW со значками — расклад Windows 11.
[[maybe_unused]] HWND TopLevelIconHost() {
    HWND worker = nullptr;
    while ((worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
        if (HWND host = IconHostIn(worker)) return host;
    }
    return nullptr;
}

}  // namespace

HWND DesktopHostWindow() {
    HWND progman = FindWindowW(L"Progman", nullptr);

#if WIDGET_DESKTOP_MODE == 1
    return progman;
#elif WIDGET_DESKTOP_MODE == 3
    // Только верхнеуровневый WorkerW. Если его ещё нет, просим оболочку его создать:
    // 0x052C — «перестрой слой обоев». В остальных режимах этого не делаем, потому что
    // от него рабочий стол перерисовывается и блоки поверх него заметно мерцают.
    if (HWND host = TopLevelIconHost()) return host;
    if (progman) {
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
        if (HWND host = TopLevelIconHost()) return host;
    }
    return progman;
#else
    if (HWND host = IconHostIn(progman)) return host;
    if (HWND host = TopLevelIconHost()) return host;
    return progman;
#endif
}

bool TrayAdd(HWND hwnd, UINT callbackMsg, HICON icon, const wchar_t* tip) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = callbackMsg;
    nid.hIcon = icon;
    wcsncpy(nid.szTip, tip, sizeof(nid.szTip) / sizeof(nid.szTip[0]) - 1);
    return Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void TrayUpdateTip(HWND hwnd, const wchar_t* tip) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    wcsncpy(nid.szTip, tip, sizeof(nid.szTip) / sizeof(nid.szTip[0]) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayRemove(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void OpenFolder(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool ResolveShortcut(const std::wstring& lnkPath, std::wstring& target, std::wstring& args) {
    target.clear();
    args.clear();

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&link)))) {
        return false;
    }
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READ))) {
            wchar_t buf[MAX_PATH] = {0};
            WIN32_FIND_DATAW fd = {};
            // Без SLGP_RAWPATH Windows пытается «чинить» ярлык, опрашивая диски и сеть,
            // и это подвешивает отрисовку на секунды.
            if (SUCCEEDED(link->GetPath(buf, MAX_PATH, &fd, SLGP_RAWPATH))) target = Expand(buf);
            wchar_t argBuf[1024] = {0};
            if (SUCCEEDED(link->GetArguments(argBuf, 1024))) args.assign(argBuf);
            ok = true;
        }
        file->Release();
    }
    link->Release();
    return ok;
}

std::wstring ShellDisplayName(const std::wstring& path) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item)))) {
        return std::wstring();
    }
    std::wstring name;
    PWSTR display = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display))) {
        name.assign(display);
        CoTaskMemFree(display);
    }
    item->Release();
    return name;
}

bool ShortcutTargetExists(const std::wstring& path) {
    if (path.empty()) return false;
    if (IsUrl(path) || IsShellPath(path)) return true;  // у виртуальных папок цели на диске нет
    if (!PathExists(path)) return false;  // самого файла ярлыка нет

    const std::wstring ext = FileExtLower(path);
    if (ext == L".url") return true;  // интернет-ярлык: проверять нечего
    if (ext != L".lnk") return true;

    std::wstring target, args;
    if (!ResolveShortcut(path, target, args)) return true;  // не смогли прочитать — не пугаем зря
    if (target.empty()) return true;                        // ярлык на виртуальную папку («Этот компьютер»)
    return PathExists(target);
}

std::wstring ShortcutTargetLabel(const std::wstring& path) {
    if (IsShellPath(path)) {
        const std::wstring name = ShellDisplayName(path);
        return name.empty() ? path : name;
    }
    if (FileExtLower(path) == L".lnk") {
        std::wstring target, args;
        if (ResolveShortcut(path, target, args) && !target.empty()) return target;
    }
    return path;
}

void LaunchShortcut(HWND owner, const std::wstring& path, const std::wstring& args, bool asAdmin) {
    // «Корзина» и прочие виртуальные папки открываются проводником по парсинг-имени.
    if (IsShellPath(path)) {
        const std::wstring params = L"\"" + path + L"\"";
        ShellExecuteW(owner, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = asAdmin ? L"runas" : nullptr;
    info.lpFile = path.c_str();
    info.lpParameters = args.empty() ? nullptr : args.c_str();
    info.nShow = SW_SHOWNORMAL;

    // Рабочая папка нужна программам, которые ищут файлы рядом с собой;
    // для .lnk её задаёт сам ярлык, поэтому туда не лезем.
    std::wstring dir;
    if (!IsUrl(path) && FileExtLower(path) != L".lnk") {
        dir = ParentDir(path);
        if (!dir.empty()) info.lpDirectory = dir.c_str();
    }

    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) return;  // пользователь отказался от запроса UAC
        const std::wstring message = L"Не удалось запустить:\n" + path;
        MessageBoxW(owner, message.c_str(), L"Ярлыки", MB_OK | MB_ICONWARNING);
    }
}

void RevealInExplorer(const std::wstring& path) {
    if (IsUrl(path) || IsShellPath(path)) return;
    if (IsDirectory(path)) {
        OpenFolder(path);
        return;
    }
    const std::wstring params = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

void ShowFileProperties(HWND owner, const std::wstring& path) {
    if (IsUrl(path) || IsShellPath(path)) return;
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"properties";
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&info);
}

bool IsInStorage(const std::wstring& path) {
    const std::wstring storage = StorageDir();
    if (path.size() <= storage.size()) return false;
    return _wcsnicmp(path.c_str(), storage.c_str(), storage.size()) == 0 && path[storage.size()] == L'\\';
}

namespace {

// Свободное имя в папке: «Блокнот.lnk», «Блокнот (2).lnk», …
std::wstring FreeNameIn(const std::wstring& dir, const std::wstring& stem, const std::wstring& ext) {
    std::wstring candidate = dir + L"\\" + stem + ext;
    for (int i = 2; PathExists(candidate) && i < 1000; i++) {
        candidate = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
    }
    return candidate;
}

// Создаёт .lnk через оболочку — так же, как это делает «Создать ярлык» в проводнике.
bool WriteShortcutFile(const std::wstring& target, const std::wstring& args, const std::wstring& linkPath) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&link)))) {
        return false;
    }
    bool ok = false;
    link->SetPath(target.c_str());
    if (!args.empty()) link->SetArguments(args.c_str());
    const std::wstring dir = ParentDir(target);
    if (!dir.empty()) link->SetWorkingDirectory(dir.c_str());

    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

}  // namespace

bool MoveFileIntoStorage(const std::wstring& path, std::wstring& newPath) {
    if (IsUrl(path) || IsShellPath(path) || IsDirectory(path) || IsInStorage(path)) return false;

    const std::wstring dir = StorageDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    const std::wstring candidate = FreeNameIn(dir, FileStem(path), FileExtLower(path));
    if (!MoveFileExW(path.c_str(), candidate.c_str(), MOVEFILE_COPY_ALLOWED)) return false;
    newPath = candidate;
    return true;
}

bool CopyFileIntoStorage(const std::wstring& path, std::wstring& newPath) {
    if (IsUrl(path) || IsShellPath(path) || IsDirectory(path) || IsInStorage(path)) return false;

    const std::wstring dir = StorageDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    const std::wstring candidate = FreeNameIn(dir, FileStem(path), FileExtLower(path));
    if (!CopyFileW(path.c_str(), candidate.c_str(), TRUE)) return false;
    newPath = candidate;
    return true;
}

bool PlaceOnDesktop(const std::wstring& path, const std::wstring& name, const std::wstring& args,
                    std::wstring& desktopPath) {
    const std::wstring desktop = DesktopDir();
    if (desktop.empty()) return false;

    const std::wstring title = name.empty() ? FileStem(path) : name;

    // Файл ярлыка, который лежит у нас в хранилище, просто переезжает обратно.
    if (IsInStorage(path)) {
        const std::wstring candidate = FreeNameIn(desktop, title, FileExtLower(path));
        if (!MoveFileExW(path.c_str(), candidate.c_str(), MOVEFILE_COPY_ALLOWED)) return false;
        desktopPath = candidate;
        return true;
    }

    // Всё остальное (программа, папка, чужой .lnk) — новый ярлык на рабочем столе.
    if (IsUrl(path) || IsShellPath(path)) return false;
    const std::wstring candidate = FreeNameIn(desktop, title, L".lnk");
    if (!WriteShortcutFile(path, args, candidate)) return false;
    desktopPath = candidate;
    return true;
}

bool PickFiles(HWND owner, std::vector<std::wstring>& paths) {
    // OFN_ALLOWMULTISELECT возвращает папку и имена через \0, поэтому буфер большой.
    std::vector<wchar_t> buffer(32768, L'\0');

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Ярлыки и программы\0*.lnk;*.url;*.exe;*.bat;*.cmd\0Все файлы\0*.*\0\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = L"Выберите ярлыки или программы";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) return false;

    const wchar_t* p = buffer.data();
    const std::wstring first(p);
    p += first.size() + 1;
    if (*p == L'\0') {
        paths.push_back(first);  // выбран ровно один файл — в буфере полный путь
        return true;
    }
    while (*p) {
        const std::wstring name(p);
        paths.push_back(first + L"\\" + name);
        p += name.size() + 1;
    }
    return !paths.empty();
}
