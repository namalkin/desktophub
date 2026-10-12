#include "sys.h"

#include <commdlg.h>
#include <shellapi.h>

#include "util.h"

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT kTrayId = 1;

}  // namespace

std::wstring RunEntry(const std::wstring& name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return L"";

    wchar_t value[MAX_PATH * 2] = {0};
    DWORD size = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS status =
        RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(value), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return L"";

    // "C:\...\TuneBox.exe" --tray -> C:\...\TuneBox.exe
    std::wstring s = value;
    if (!s.empty() && s[0] == L'"') {
        const size_t close = s.find(L'"', 1);
        return close == std::wstring::npos ? s.substr(1) : s.substr(1, close - 1);
    }
    const size_t space = s.find(L" -");
    return space == std::wstring::npos ? s : s.substr(0, space);
}

bool SetRunEntry(const std::wstring& name, const std::wstring& exePath, const std::wstring& args, bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    LSTATUS status;
    if (enable) {
        std::wstring value = L"\"" + exePath + L"\"";
        if (!args.empty()) value += L" " + args;
        status = RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, name.c_str());
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;  // и так выключено
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
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

void TrayRemove(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void OpenUrl(const std::wstring& url) {
    if (url.empty()) return;
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenFolder(const std::wstring& path) {
    if (path.empty()) return;
    CreateDirectoryW(path.c_str(), nullptr);  // папка установки до первого скачивания пуста
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RevealInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    const std::wstring params = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

bool PickExe(HWND owner, const std::wstring& title, const std::wstring& startAt, std::wstring& out) {
    wchar_t buf[MAX_PATH * 2] = {0};
    const std::wstring initial = ParentDir(startAt);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Программа (*.exe)\0*.exe\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf) / sizeof(buf[0]);
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out.assign(buf);
    return !out.empty();
}
