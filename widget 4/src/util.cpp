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
    dir += L"\\DiskVac";
    return dir;
}

std::wstring DataFile() { return DataDir() + L"\\data.json"; }
std::wstring ReportFile() { return DataDir() + L"\\отчёт.txt"; }

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

// ---------- Размеры ----------

namespace {

// Гигабайты у нас двоичные — те же, что показывает проводник: 326 ГБ диска там и здесь
// должны совпадать, иначе виджету не поверят.
struct SizeParts {
    double value;
    const wchar_t* unit;
    int decimals;
};

SizeParts SplitSize(unsigned long long bytes) {
    const wchar_t* units[] = {L"Б", L"КБ", L"МБ", L"ГБ", L"ТБ", L"ПБ"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        unit++;
    }
    // Один знак после запятой начиная с мегабайт и только пока число меньше сотни:
    // «7,3 ГБ» читается, «734,8 МБ» — уже нет.
    int decimals = 0;
    if (unit >= 2 && value < 100.0) decimals = 1;
    return {value, units[unit], decimals};
}

}  // namespace

std::wstring FormatSize(unsigned long long bytes) {
    const SizeParts parts = SplitSize(bytes);
    wchar_t buf[48];
    swprintf(buf, 48, parts.decimals ? L"%.1f %ls" : L"%.0f %ls", parts.value, parts.unit);
    // Запятая как десятичный разделитель — так пишут в русской Windows.
    for (wchar_t* p = buf; *p; ++p) {
        if (*p == L'.') *p = L',';
    }
    return buf;
}

std::wstring SizeNumber(unsigned long long bytes) {
    const SizeParts parts = SplitSize(bytes);
    wchar_t buf[32];
    swprintf(buf, 32, parts.decimals ? L"%.1f" : L"%.0f", parts.value);
    for (wchar_t* p = buf; *p; ++p) {
        if (*p == L'.') *p = L',';
    }
    return buf;
}

std::wstring SizeUnit(unsigned long long bytes) { return SplitSize(bytes).unit; }

// ---------- Пути ----------

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

std::wstring FileExtLower(const std::wstring& path) {
    const std::wstring name = FileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return std::wstring();
    std::wstring ext = name.substr(dot);
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    return ext;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(LongPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(LongPath(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring LongPath(const std::wstring& path) {
    // Обход диска легко забирается глубже 260 символов (кэши браузеров, node_modules),
    // и без префикса такие файлы просто «исчезают» из подсчёта.
    if (path.size() < 248) return path;
    if (path.compare(0, 4, L"\\\\?\\") == 0) return path;
    if (path.size() > 1 && path[0] == L'\\' && path[1] == L'\\') return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring ExpandVars(const std::wstring& text) {
    if (text.find(L'%') == std::wstring::npos) return text;
    wchar_t buf[1024] = {0};
    const DWORD n = ExpandEnvironmentStringsW(text.c_str(), buf, 1024);
    if (n == 0 || n > 1024) return text;
    std::wstring out(buf);
    // Нераскрытая переменная осталась бы куском пути вида «%OneDrive%\...» — такой
    // путь бесполезен, лучше честно вернуть пустую строку.
    if (out.find(L'%') != std::wstring::npos) return std::wstring();
    return out;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool StartsWithPath(const std::wstring& path, const std::wstring& prefix) {
    if (prefix.empty() || path.size() < prefix.size()) return false;
    if (_wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0) return false;
    if (path.size() == prefix.size()) return true;
    if (prefix.back() == L'\\') return true;  // префикс уже заканчивается разделителем
    return path[prefix.size()] == L'\\';
}

// ---------- Время ----------

unsigned long long FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

int DaysSince(const FILETIME& ft) {
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const unsigned long long now = FileTimeToU64(nowFt);
    const unsigned long long then = FileTimeToU64(ft);
    if (then == 0 || then >= now) return 0;
    // 100-наносекундные интервалы в сутках.
    return static_cast<int>((now - then) / 864000000000ULL);
}

std::wstring AgeText(int days) {
    if (days < 7) return L"на этой неделе";
    if (days < 31) {
        const int weeks = days / 7;
        wchar_t buf[48];
        swprintf(buf, 48, L"%d %ls назад", weeks, PluralRu(weeks, L"неделю", L"недели", L"недель"));
        return buf;
    }
    if (days < 365) {
        const int months = days / 30;
        wchar_t buf[48];
        swprintf(buf, 48, L"%d %ls назад", months, PluralRu(months, L"месяц", L"месяца", L"месяцев"));
        return buf;
    }
    const int years = days / 365;
    wchar_t buf[48];
    swprintf(buf, 48, L"%d %ls назад", years, PluralRu(years, L"год", L"года", L"лет"));
    return buf;
}
