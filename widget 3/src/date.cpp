#include "date.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace {

const wchar_t* kWeekdaysFull[7] = {L"понедельник", L"вторник", L"среда",      L"четверг",
                                   L"пятница",     L"суббота", L"воскресенье"};
const wchar_t* kWeekdaysShort[7] = {L"пн", L"вт", L"ср", L"чт", L"пт", L"сб", L"вс"};

const wchar_t* kMonthsGenitive[12] = {L"января", L"февраля", L"марта",    L"апреля",  L"мая",    L"июня",
                                      L"июля",   L"августа", L"сентября", L"октября", L"ноября", L"декабря"};

const wchar_t* kMonthsNominative[12] = {L"Январь", L"Февраль", L"Март",     L"Апрель",  L"Май",    L"Июнь",
                                        L"Июль",   L"Август",  L"Сентябрь", L"Октябрь", L"Ноябрь", L"Декабрь"};

// Дни считаем через FILETIME: 100-наносекундные интервалы, ровно 864 000 000 000 в сутках.
constexpr long long kTicksPerDay = 24LL * 60LL * 60LL * 10000000LL;

bool ToTicks(const Date& date, long long& out) {
    if (!ValidDate(date)) return false;
    SYSTEMTIME st = {};
    st.wYear = static_cast<WORD>(date.y);
    st.wMonth = static_cast<WORD>(date.m);
    st.wDay = static_cast<WORD>(date.d);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    out = static_cast<long long>(v.QuadPart);
    return true;
}

bool FromTicks(long long ticks, Date& out) {
    if (ticks < 0) return false;
    ULARGE_INTEGER v;
    v.QuadPart = static_cast<ULONGLONG>(ticks);
    FILETIME ft;
    ft.dwLowDateTime = v.LowPart;
    ft.dwHighDateTime = v.HighPart;
    SYSTEMTIME st = {};
    if (!FileTimeToSystemTime(&ft, &st)) return false;
    out.y = st.wYear;
    out.m = st.wMonth;
    out.d = st.wDay;
    return true;
}

}  // namespace

Date Today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Date{st.wYear, st.wMonth, st.wDay};
}

bool ValidDate(const Date& date) {
    if (date.y < 1601 || date.y > 9000) return false;
    if (date.m < 1 || date.m > 12) return false;
    return date.d >= 1 && date.d <= DaysInMonth(date.y, date.m);
}

int DaysInMonth(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

Date AddDays(const Date& date, int days) {
    long long ticks = 0;
    if (!ToTicks(date, ticks)) return date;
    Date out;
    if (!FromTicks(ticks + static_cast<long long>(days) * kTicksPerDay, out)) return date;
    return out;
}

Date AddMonths(const Date& date, int months) {
    int total = date.y * 12 + (date.m - 1) + months;
    if (total < 0) total = 0;
    Date out;
    out.y = total / 12;
    out.m = total % 12 + 1;
    const int last = DaysInMonth(out.y, out.m);
    out.d = date.d < last ? date.d : last;  // 31 января + месяц = 28 (или 29) февраля
    return out;
}

int DiffDays(const Date& from, const Date& to) {
    long long a = 0, b = 0;
    if (!ToTicks(from, a) || !ToTicks(to, b)) return 0;
    return static_cast<int>((b - a) / kTicksPerDay);
}

int CompareDate(const Date& a, const Date& b) {
    if (a.y != b.y) return a.y < b.y ? -1 : 1;
    if (a.m != b.m) return a.m < b.m ? -1 : 1;
    if (a.d != b.d) return a.d < b.d ? -1 : 1;
    return 0;
}

int Weekday(const Date& date) {
    long long ticks = 0;
    if (!ToTicks(date, ticks)) return 0;
    ULARGE_INTEGER v;
    v.QuadPart = static_cast<ULONGLONG>(ticks);
    FILETIME ft;
    ft.dwLowDateTime = v.LowPart;
    ft.dwHighDateTime = v.HighPart;
    SYSTEMTIME st = {};
    if (!FileTimeToSystemTime(&ft, &st)) return 0;
    // wDayOfWeek: 0 — воскресенье; у нас неделя начинается с понедельника.
    return (st.wDayOfWeek + 6) % 7;
}

Date MondayOf(const Date& date) { return AddDays(date, -Weekday(date)); }

std::wstring IsoW(const Date& date) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%04d-%02d-%02d", date.y, date.m, date.d);
    return std::wstring(buf);
}

std::string Iso(const Date& date) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", date.y, date.m, date.d);
    return std::string(buf);
}

bool ParseIso(const std::wstring& text, Date& out) {
    if (text.size() < 10) return false;
    Date d;
    d.y = _wtoi(text.substr(0, 4).c_str());
    d.m = _wtoi(text.substr(5, 2).c_str());
    d.d = _wtoi(text.substr(8, 2).c_str());
    if (!ValidDate(d)) return false;
    out = d;
    return true;
}

bool ParseIso(const std::string& text, Date& out) {
    if (text.size() < 10) return false;
    Date d;
    d.y = std::atoi(text.substr(0, 4).c_str());
    d.m = std::atoi(text.substr(5, 2).c_str());
    d.d = std::atoi(text.substr(8, 2).c_str());
    if (!ValidDate(d)) return false;
    out = d;
    return true;
}

const wchar_t* WeekdayFull(int index) { return kWeekdaysFull[(index % 7 + 7) % 7]; }
const wchar_t* WeekdayShort(int index) { return kWeekdaysShort[(index % 7 + 7) % 7]; }

const wchar_t* MonthGenitive(int month) {
    if (month < 1 || month > 12) month = 1;
    return kMonthsGenitive[month - 1];
}

const wchar_t* MonthNominative(int month) {
    if (month < 1 || month > 12) month = 1;
    return kMonthsNominative[month - 1];
}

std::wstring FormatLong(const Date& date) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%ls, %d %ls", WeekdayFull(Weekday(date)), date.d, MonthGenitive(date.m));
    return std::wstring(buf);
}

std::wstring FormatDayMonth(const Date& date) {
    wchar_t buf[48];
    swprintf(buf, 48, L"%d %ls", date.d, MonthGenitive(date.m));
    return std::wstring(buf);
}

std::wstring FormatWeek(const Date& monday) {
    const Date sunday = AddDays(monday, 6);
    wchar_t buf[64];
    if (monday.m == sunday.m) {
        swprintf(buf, 64, L"%d–%d %ls", monday.d, sunday.d, MonthGenitive(monday.m));
    } else {
        swprintf(buf, 64, L"%d %ls – %d %ls", monday.d, MonthGenitive(monday.m), sunday.d,
                 MonthGenitive(sunday.m));
    }
    return std::wstring(buf);
}

std::wstring FormatMonthYear(const Date& date) {
    wchar_t buf[48];
    swprintf(buf, 48, L"%ls %d", MonthNominative(date.m), date.y);
    return std::wstring(buf);
}
