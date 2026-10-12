#include "util.h"

#include <windows.h>
#include <shlobj.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr wchar_t kNbsp = L' ';       // неразрывный пробел (разделитель тысяч)
constexpr wchar_t kNarrowNbsp = L' '; // узкий неразрывный пробел (может прийти из буфера обмена)
constexpr wchar_t kMinusSign = L'−';  // типографский минус
}  // namespace

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
    dir += L"\\NewYearWidget";
    return dir;
}

std::wstring DataFile() { return DataDir() + L"\\data.json"; }

std::wstring FormatAmount(long long cents, bool withSign) {
    const bool negative = cents < 0;
    // -(LLONG_MIN) переполняет, поэтому модуль берём через unsigned.
    unsigned long long abs = negative ? static_cast<unsigned long long>(-(cents + 1)) + 1ULL
                                      : static_cast<unsigned long long>(cents);
    const unsigned long long whole = abs / 100;
    const unsigned frac = static_cast<unsigned>(abs % 100);

    wchar_t digits[32];
    std::swprintf(digits, 32, L"%llu", whole);

    std::wstring grouped;
    const size_t len = wcslen(digits);
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) grouped += kNbsp;
        grouped += digits[i];
    }

    std::wstring out;
    if (withSign) out += negative ? L'-' : L'+';
    else if (negative) out += L'-';
    out += grouped;
    if (frac != 0) {
        wchar_t tail[8];
        std::swprintf(tail, 8, L",%02u", frac);
        out += tail;
    }
    return out;
}

std::wstring FormatMoney(long long cents) {
    return FormatAmount(cents, false) + kNbsp + L"₽";  // ₽
}

bool ParseAmount(const std::wstring& text, long long& cents) {
    std::wstring clean;
    for (wchar_t c : text) {
        if (c == L' ' || c == kNbsp || c == kNarrowNbsp || c == L'\'' || c == L'\t') continue;
        clean += (c == L',') ? L'.' : c;
    }
    if (clean.empty()) return false;

    size_t i = 0;
    int sign = 1;
    if (clean[0] == L'+' || clean[0] == L'-' || clean[0] == kMinusSign) {
        sign = (clean[0] == L'+') ? 1 : -1;
        i = 1;
    }
    if (i >= clean.size()) return false;

    long long whole = 0;
    long long frac = 0;
    int fracDigits = 0;
    bool seenDigit = false;
    bool inFrac = false;
    for (; i < clean.size(); i++) {
        const wchar_t c = clean[i];
        if (c == L'.') {
            if (inFrac) return false;
            inFrac = true;
            continue;
        }
        if (c < L'0' || c > L'9') return false;
        seenDigit = true;
        if (inFrac) {
            if (fracDigits < 2) {
                frac = frac * 10 + (c - L'0');
                fracDigits++;
            }
        } else {
            if (whole > 90000000000LL) return false;  // защита от переполнения
            whole = whole * 10 + (c - L'0');
        }
    }
    if (!seenDigit) return false;
    while (fracDigits < 2) {
        frac *= 10;
        fracDigits++;
    }

    cents = sign * (whole * 100 + frac);
    return true;
}

bool ParseIsoDate(const std::wstring& text, SYSTEMTIME& out) {
    if (text.size() != 10 || text[4] != L'-' || text[7] != L'-') return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (text[i] < L'0' || text[i] > L'9') return false;
    }
    const int year = (text[0] - L'0') * 1000 + (text[1] - L'0') * 100 + (text[2] - L'0') * 10 + (text[3] - L'0');
    const int month = (text[5] - L'0') * 10 + (text[6] - L'0');
    const int day = (text[8] - L'0') * 10 + (text[9] - L'0');
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1) return false;

    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit = kDays[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) limit = 29;
    if (day > limit) return false;

    out = SYSTEMTIME{};
    out.wYear = static_cast<WORD>(year);
    out.wMonth = static_cast<WORD>(month);
    out.wDay = static_cast<WORD>(day);
    return true;
}

std::string TodayIso() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return std::string(buf);
}
