#pragma once

#include <windows.h>

#include <string>

// Кодировки
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// Пути
std::wstring DataDir();     // %APPDATA%\ShortcutDock
std::wstring DataFile();    // %APPDATA%\ShortcutDock\data.json
std::wstring BackupDir();   // %APPDATA%\ShortcutDock\backups
std::wstring StorageDir();  // %APPDATA%\ShortcutDock\shortcuts — перенесённые сюда .lnk
std::wstring DesktopDir();  // рабочий стол пользователя

// Разбор путей без shlwapi: библиотека лишняя ради трёх функций.
std::wstring FileName(const std::wstring& path);       // "C:\a\b.lnk" -> "b.lnk"
std::wstring FileStem(const std::wstring& path);       // "C:\a\b.lnk" -> "b"
std::wstring FileExtLower(const std::wstring& path);   // "C:\a\B.LNK" -> ".lnk"
std::wstring ParentDir(const std::wstring& path);      // "C:\a\b.lnk" -> "C:\a"

bool PathExists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
bool IsUrl(const std::wstring& path);  // http://, https://, ftp://, mailto:

// Виртуальный объект оболочки: «Корзина», «Этот компьютер» и подобные.
// У них нет файла на диске, путь выглядит как ::{CLSID} или shell:Имя.
bool IsShellPath(const std::wstring& path);

std::wstring TrimSpaces(const std::wstring& s);

std::string TodayIso();  // "2026-09-05" по локальному времени
