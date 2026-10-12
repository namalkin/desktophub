// Интеграция с Windows: автозапуск, значок в трее, проводник.
#pragma once

#include <windows.h>

#include <string>

// HKCU\...\Run. Права администратора не нужны. Тем же ключом пользуются сами виджеты:
// значение «TuneBox» = "путь\TuneBox.exe" — центр читает и пишет его в их формате, так
// что переключатель в центре и переключатель в настройках виджета — одна и та же галочка.
std::wstring RunEntry(const std::wstring& name);  // путь без кавычек или пусто
bool SetRunEntry(const std::wstring& name, const std::wstring& exePath, const std::wstring& args, bool enable);

bool TrayAdd(HWND hwnd, UINT callbackMsg, HICON icon, const wchar_t* tip);
void TrayRemove(HWND hwnd);

void OpenUrl(const std::wstring& url);
void OpenFolder(const std::wstring& path);
void RevealInExplorer(const std::wstring& path);

bool PickExe(HWND owner, const std::wstring& title, const std::wstring& startAt, std::wstring& out);
