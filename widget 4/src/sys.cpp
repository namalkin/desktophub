#include "sys.h"

#include <shellapi.h>
#include <shlobj.h>

#include "util.h"

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DiskVac";
constexpr UINT kTrayId = 1;

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
// сам Progman, но на Windows 11 значки живут в верхнеуровневом WorkerW, который в
// z-порядке выше Progman — дочернее окно Progman там прячется под обоями. Поэтому ищем
// владельца значков, а не окно с заранее известным именем класса.
namespace {

// Значки лежат либо прямо в окне, либо в его WorkerW-ребёнке.
HWND IconHostIn(HWND parent) {
    if (!parent) return nullptr;
    if (FindWindowExW(parent, nullptr, L"SHELLDLL_DefView", nullptr)) return parent;
    HWND worker = nullptr;
    while ((worker = FindWindowExW(parent, worker, L"WorkerW", nullptr)) != nullptr) {
        if (FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr)) return worker;
    }
    return nullptr;
}

// Верхнеуровневый WorkerW со значками — расклад Windows 11.
HWND TopLevelIconHost() {
    HWND worker = nullptr;
    while ((worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
        if (HWND host = IconHostIn(worker)) return host;
    }
    return nullptr;
}

}  // namespace

HWND DesktopHostWindow() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (HWND host = IconHostIn(progman)) return host;
    if (HWND host = TopLevelIconHost()) return host;
    return progman;
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

void RevealInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    if (IsDirectory(path)) {
        OpenFolder(path);
        return;
    }
    const std::wstring params = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

void ShowFileProperties(HWND owner, const std::wstring& path) {
    if (path.empty()) return;
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"properties";
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&info);
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool RestartElevated() {
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";  // тот же .exe, но с запросом прав
    const std::wstring exe = ExePath();
    info.lpFile = exe.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return false;  // в том числе отказ пользователя от UAC
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

// ---------- Удаление ----------

bool ToRecycleBin(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return true;

    // SHFileOperation ждёт список, где строки идут подряд и заканчиваются двойным нулём.
    std::wstring buffer;
    for (const std::wstring& path : paths) {
        if (path.empty()) continue;
        buffer += path;
        buffer.push_back(L'\0');
    }
    if (buffer.empty()) return true;
    buffer.push_back(L'\0');

    SHFILEOPSTRUCTW op = {};
    op.hwnd = owner;
    op.wFunc = FO_DELETE;
    op.pFrom = buffer.c_str();
    // FOF_ALLOWUNDO — это и есть «в корзину»; NOCONFIRMATION потому, что своё
    // подтверждение виджет уже показал, с размерами и списком.
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR;
    const int result = SHFileOperationW(&op);
    return result == 0 && !op.fAnyOperationsAborted;
}

bool DeletePermanently(const std::wstring& path) {
    const std::wstring full = LongPath(path);
    const DWORD attrs = GetFileAttributesW(full.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;  // уже нет — считаем, что удалили

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(full.c_str()) != FALSE;

    // Файлы кэшей часто помечены «только для чтения» или скрытыми.
    if (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
        SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    return DeleteFileW(full.c_str()) != FALSE;
}

unsigned long long RecycleBinSize() {
    SHQUERYRBINFO info = {};
    info.cbSize = sizeof(info);
    // nullptr — корзина по всем дискам сразу.
    if (SHQueryRecycleBinW(nullptr, &info) != S_OK) return 0;
    return info.i64Size > 0 ? static_cast<unsigned long long>(info.i64Size) : 0;
}

bool EmptyRecycleBin(HWND owner) {
    const HRESULT hr = SHEmptyRecycleBinW(owner, nullptr,
                                          SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    // Пустую корзину система возвращает как ошибку — это не наша проблема.
    return SUCCEEDED(hr) || hr == static_cast<HRESULT>(0x8004010FL);
}
