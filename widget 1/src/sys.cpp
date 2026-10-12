#include "sys.h"

#include <shellapi.h>
#include <shlobj.h>

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"NewYearWidget";
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
// (SHELLDLL_DefView): именно оно покрывает все мониторы и рисует обои. Обычно это сам
// Progman, но оболочка умеет перестраивать стол: после Snap Assist, Win+Tab или смены
// обоев значки переезжают в верхнеуровневый WorkerW, а над Progman ложится ещё один
// WorkerW с обоями — дочернее окно Progman оказывается под ним и пропадает. Поэтому ищем
// владельца значков, а не окно с заранее известным именем класса. Сами ничего оболочке не
// посылаем: сообщение «перестрой слой обоев» (0x052C) заставляет её перерисовать рабочий
// стол, и виджет поверх него заметно мерцает.
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

// Верхнеуровневый WorkerW со значками — стол после перестройки и расклад Windows 11.
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
