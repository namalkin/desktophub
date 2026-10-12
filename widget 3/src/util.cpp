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
    dir += L"\\PlannerGo";
    return dir;
}

std::wstring DataFile() { return DataDir() + L"\\data.json"; }
std::wstring BackupDir() { return DataDir() + L"\\backups"; }

std::wstring TrimSpaces(const std::wstring& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && iswspace(s[begin])) begin++;
    while (end > begin && iswspace(s[end - 1])) end--;
    return s.substr(begin, end - begin);
}

const wchar_t* PluralRu(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many) {
    if (n < 0) n = -n;
    const int mod100 = n % 100;
    const int mod10 = n % 10;
    if (mod100 >= 11 && mod100 <= 14) return many;
    if (mod10 == 1) return one;
    if (mod10 >= 2 && mod10 <= 4) return few;
    return many;
}
