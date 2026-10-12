#pragma once

#include <windows.h>

#include <string>

// Кодировки
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// Пути
std::wstring DataDir();     // %APPDATA%\WidgetHub — настройки центра
std::wstring DataFile();    // %APPDATA%\WidgetHub\data.json
std::wstring InstallDir();  // %LOCALAPPDATA%\WidgetHub\widgets — сюда кладутся скачанные виджеты
std::wstring ExePath();
std::wstring ExeDir();

std::wstring TrimSpaces(const std::wstring& s);

std::wstring FileName(const std::wstring& path);
std::wstring ParentDir(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool EqualsNoCase(const std::wstring& a, const std::wstring& b);

// «..\..\widget 5\dist\TuneBox.exe» -> полный путь без «..».
std::wstring NormalizePath(const std::wstring& path);
