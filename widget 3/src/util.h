#pragma once

#include <windows.h>

#include <string>

// Кодировки
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// Пути
std::wstring DataDir();    // %APPDATA%\PlannerGo
std::wstring DataFile();   // %APPDATA%\PlannerGo\data.json
std::wstring BackupDir();  // %APPDATA%\PlannerGo\backups

std::wstring TrimSpaces(const std::wstring& s);

// Русские окончания: 1 задача / 2 задачи / 5 задач.
const wchar_t* PluralRu(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many);
