#pragma once

#include <windows.h>  // SYSTEMTIME

#include <string>

// Кодировки
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// Пути
std::wstring DataDir();   // %APPDATA%\NewYearWidget
std::wstring DataFile();  // %APPDATA%\NewYearWidget\data.json

// Деньги хранятся в копейках, чтобы не терять точность на double.
// "37 000" (NBSP как разделитель тысяч), знак — по флагу.
std::wstring FormatAmount(long long cents, bool withSign);
std::wstring FormatMoney(long long cents);  // то же + " ₽"

// Принимает "-2000", "+5 000", "1000,50", "1000.5". Пустая строка -> false.
bool ParseAmount(const std::wstring& text, long long& cents);

std::string TodayIso();  // "2026-09-05" по локальному времени

// "ГГГГ-ММ-ДД" -> SYSTEMTIME (полночь). Проверяет и число дней в месяце.
bool ParseIsoDate(const std::wstring& text, SYSTEMTIME& out);
