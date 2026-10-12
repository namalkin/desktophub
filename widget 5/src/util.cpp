#include "util.h"

#include <shlobj.h>
#include <windows.h>

#include <cstdio>
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
    dir += L"\\TuneBox";
    return dir;
}

std::wstring DataFile() { return DataDir() + L"\\data.json"; }

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

// ---------- Пути ----------

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return path;
    return path.substr(slash + 1);
}

std::wstring FileStem(const std::wstring& path) {
    std::wstring name = FileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.erase(dot);
    return name;
}

std::wstring ParentDir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    if (slash == 2 && path.size() > 2 && path[1] == L':') return path.substr(0, 3);  // «C:\»
    return path.substr(0, slash);
}

std::wstring FolderName(const std::wstring& path) {
    std::wstring trimmed = path;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const std::wstring name = FileName(trimmed);
    return name.empty() ? trimmed : name;
}

std::wstring FileExtLower(const std::wstring& path) {
    const std::wstring name = FileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return std::wstring();
    std::wstring ext = name.substr(dot);
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    return ext;
}

bool PathExists(const std::wstring& path) {
    if (path.empty()) return false;
    return GetFileAttributesW(LongPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(LongPath(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring LongPath(const std::wstring& path) {
    // Музыка легко лежит в папках вроде «...\Яндекс.Музыка\Сборники\...» — за 260 символов
    // такой путь уходит без труда, и без префикса файл просто «исчезает».
    if (path.size() < 248) return path;
    if (path.compare(0, 4, L"\\\\?\\") == 0) return path;
    if (path.size() > 1 && path[0] == L'\\' && path[1] == L'\\') return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& ch : out) ch = static_cast<wchar_t>(towlower(ch));
    return out;
}

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

// ---------- Время ----------

std::wstring FormatTime(unsigned long long ms) {
    const unsigned long long total = ms / 1000;
    const unsigned long long hours = total / 3600;
    const unsigned long long minutes = (total % 3600) / 60;
    const unsigned long long seconds = total % 60;
    wchar_t buf[32];
    if (hours > 0) {
        swprintf(buf, 32, L"%llu:%02llu:%02llu", hours, minutes, seconds);
    } else {
        swprintf(buf, 32, L"%llu:%02llu", minutes, seconds);
    }
    return buf;
}
