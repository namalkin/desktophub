// Календарные даты без времени: арифметика по дням и русские подписи.
#pragma once

#include <string>

struct Date {
    int y = 0;
    int m = 0;  // 1..12
    int d = 0;  // 1..31

    bool operator==(const Date& o) const { return y == o.y && m == o.m && d == o.d; }
    bool operator!=(const Date& o) const { return !(*this == o); }
};

Date Today();
bool ValidDate(const Date& date);

Date AddDays(const Date& date, int days);
Date AddMonths(const Date& date, int months);
int DiffDays(const Date& from, const Date& to);  // to - from
int CompareDate(const Date& a, const Date& b);   // -1 / 0 / 1

int Weekday(const Date& date);   // 0 — понедельник, 6 — воскресенье
Date MondayOf(const Date& date);
int DaysInMonth(int year, int month);

std::wstring IsoW(const Date& date);  // "2026-09-09"
std::string Iso(const Date& date);
bool ParseIso(const std::wstring& text, Date& out);
bool ParseIso(const std::string& text, Date& out);

const wchar_t* WeekdayFull(int index);      // "понедельник"
const wchar_t* WeekdayShort(int index);     // "пн"
const wchar_t* MonthGenitive(int month);    // "октября"
const wchar_t* MonthNominative(int month);  // "Октябрь"

std::wstring FormatLong(const Date& date);        // "понедельник, 13 октября"
std::wstring FormatDayMonth(const Date& date);    // "13 октября"
std::wstring FormatWeek(const Date& monday);      // "13–19 октября"
std::wstring FormatMonthYear(const Date& date);   // "Октябрь 2025"
