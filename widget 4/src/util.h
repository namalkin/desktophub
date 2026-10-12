#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// Кодировки
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// Пути
std::wstring DataDir();   // %APPDATA%\DiskVac
std::wstring DataFile();  // %APPDATA%\DiskVac\data.json
std::wstring ReportFile();  // последний отчёт сканирования (человекочитаемый .txt)

std::wstring TrimSpaces(const std::wstring& s);

// Русские окончания: 1 файл / 2 файла / 5 файлов.
const wchar_t* PluralRu(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many);

// ---------- Размеры ----------

// «12,4 ГБ», «860 МБ», «0 Б» — короткая запись для карточек.
std::wstring FormatSize(unsigned long long bytes);
// Только число («12,4») и только единица («ГБ») — крупная цифра в блоке.
std::wstring SizeNumber(unsigned long long bytes);
std::wstring SizeUnit(unsigned long long bytes);

// ---------- Пути и имена ----------

std::wstring FileName(const std::wstring& path);   // «отчёт.pdf»
std::wstring ParentDir(const std::wstring& path);  // «C:\Users\Ivan»
std::wstring FileExtLower(const std::wstring& path);  // «.pdf», с точкой
bool PathExists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);

// Путь длиннее MAX_PATH: API берут его только с префиксом \\?\.
std::wstring LongPath(const std::wstring& path);

// Развернуть %TEMP%, %LOCALAPPDATA% и прочие переменные окружения.
std::wstring ExpandVars(const std::wstring& text);

// Сравнение путей без учёта регистра; StartsWithPath дополнительно требует,
// чтобы совпадение заканчивалось на границе папки (C:\Prog не подойдёт к C:\Program Files).
bool EqualsNoCase(const std::wstring& a, const std::wstring& b);
bool StartsWithPath(const std::wstring& path, const std::wstring& prefix);

// ---------- Время ----------

unsigned long long FileTimeToU64(const FILETIME& ft);
// Сколько дней прошло с этого момента (отрицательных не бывает — будущее считаем нулём).
int DaysSince(const FILETIME& ft);
// «3 года назад», «полгода назад», «на этой неделе».
std::wstring AgeText(int days);
