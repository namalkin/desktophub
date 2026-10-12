#include "util.h"

#include <shlobj.h>
#include <windows.h>

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

namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id, const wchar_t* envFallback) {
    PWSTR path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        dir.assign(path);
        CoTaskMemFree(path);
    } else {
        wchar_t buf[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(envFallback, buf, MAX_PATH) > 0) dir.assign(buf);
    }
    return dir.empty() ? L"." : dir;
}

}  // namespace

std::wstring DataDir() { return KnownFolder(FOLDERID_RoamingAppData, L"APPDATA") + L"\\WidgetHub"; }

std::wstring DataFile() { return DataDir() + L"\\data.json"; }

std::wstring InstallDir() { return KnownFolder(FOLDERID_LocalAppData, L"LOCALAPPDATA") + L"\\WidgetHub\\widgets"; }

std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

std::wstring ExeDir() { return ParentDir(ExePath()); }

std::wstring TrimSpaces(const std::wstring& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && iswspace(s[begin])) begin++;
    while (end > begin && iswspace(s[end - 1])) end--;
    return s.substr(begin, end - begin);
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return path;
    return path.substr(slash + 1);
}

std::wstring ParentDir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    if (slash == 2 && path.size() > 2 && path[1] == L':') return path.substr(0, 3);  // «C:\»
    return path.substr(0, slash);
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring NormalizePath(const std::wstring& path) {
    wchar_t buf[MAX_PATH * 2] = {0};
    const DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 2, buf, nullptr);
    if (n == 0 || n >= MAX_PATH * 2) return path;
    return std::wstring(buf, n);
}
