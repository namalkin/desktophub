#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// Кодировки
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// Пути
std::wstring DataDir();   // %APPDATA%\TuneBox
std::wstring DataFile();  // %APPDATA%\TuneBox\data.json

std::wstring TrimSpaces(const std::wstring& s);

// Русские окончания: 1 трек / 2 трека / 5 треков.
const wchar_t* PluralRu(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many);

// ---------- Пути и имена ----------

std::wstring FileName(const std::wstring& path);       // «Ameno - Era.mp3»
std::wstring FileStem(const std::wstring& path);       // «Ameno - Era»
std::wstring ParentDir(const std::wstring& path);      // «D:\vosst»
std::wstring FolderName(const std::wstring& path);     // «yaro» — последний сегмент пути
std::wstring FileExtLower(const std::wstring& path);   // «.mp3», с точкой
bool PathExists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);

// Путь длиннее MAX_PATH: API берут его только с префиксом \\?\.
std::wstring LongPath(const std::wstring& path);

// Сравнение без учёта регистра — и для путей, и для поиска по названию.
bool EqualsNoCase(const std::wstring& a, const std::wstring& b);
bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle);

// Строка в нижнем регистре — для поиска и сортировки.
std::wstring ToLower(const std::wstring& s);

// ---------- Время ----------

// «3:07», «1:02:44» — часы появляются только когда они есть.
std::wstring FormatTime(unsigned long long ms);
