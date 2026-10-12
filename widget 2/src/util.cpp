#include "util.h"

#include <shlobj.h>
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <cwctype>

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring DataDir() {
    PWSTR path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
        dir.assign(path);
        CoTaskMemFree(path);
    } else {
        wchar_t buf[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) > 0) dir.assign(buf);
    }
    if (dir.empty()) return L".";
    dir += L"\\ShortcutDock";
    return dir;
}

std::wstring DataFile() { return DataDir() + L"\\data.json"; }
std::wstring BackupDir() { return DataDir() + L"\\backups"; }
std::wstring StorageDir() { return DataDir() + L"\\shortcuts"; }

std::wstring DesktopDir() {
    PWSTR path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path))) {
        dir.assign(path);
        CoTaskMemFree(path);
    }
    if (dir.empty()) {
        wchar_t buf[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH) > 0) dir = std::wstring(buf) + L"\\Desktop";
    }
    return dir;
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring FileStem(const std::wstring& path) {
    std::wstring name = FileName(path);
    const size_t dot = name.find_last_of(L'.');
    // Точка первым символом — это не расширение, а скрытый файл вроде ".gitignore".
    if (dot != std::wstring::npos && dot > 0) name.erase(dot);
    return name;
}

std::wstring FileExtLower(const std::wstring& path) {
    const std::wstring name = FileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return std::wstring();
    std::wstring ext = name.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

std::wstring ParentDir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    if (slash == 2 && path.size() > 2 && path[1] == L':') return path.substr(0, 3);  // корень диска
    return path.substr(0, slash);
}

bool PathExists(const std::wstring& path) {
    if (path.empty()) return false;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsShellPath(const std::wstring& path) {
    if (path.size() > 2 && path[0] == L':' && path[1] == L':') return true;
    return path.size() > 6 && _wcsnicmp(path.c_str(), L"shell:", 6) == 0;
}

bool IsUrl(const std::wstring& path) {
    auto starts = [&](const wchar_t* prefix) {
        const size_t n = wcslen(prefix);
        return path.size() > n && _wcsnicmp(path.c_str(), prefix, n) == 0;
    };
    return starts(L"http://") || starts(L"https://") || starts(L"ftp://") || starts(L"mailto:");
}

std::wstring TrimSpaces(const std::wstring& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && iswspace(s[begin])) begin++;
    while (end > begin && iswspace(s[end - 1])) end--;
    return s.substr(begin, end - begin);
}

std::string TodayIso() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return std::string(buf);
}
