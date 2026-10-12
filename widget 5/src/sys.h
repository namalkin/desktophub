// Интеграция с Windows: автозапуск, значок в трее, рабочий стол, диалоги выбора.
#pragma once

#include <windows.h>

#include <string>

std::wstring ExePath();

bool IsAutostartEnabled();
bool SetAutostart(bool enable);  // HKCU\...\Run — права администратора не нужны

// Окно — хозяин рабочего стола. Блок, ставший его дочерним окном, живёт внутри рабочего
// стола: не всплывает над чужими окнами и виден ровно тогда, когда виден сам стол.
HWND DesktopHostWindow();

bool TrayAdd(HWND hwnd, UINT callbackMsg, HICON icon, const wchar_t* tip);
void TrayUpdateTip(HWND hwnd, const wchar_t* tip);
void TrayRemove(HWND hwnd);

void OpenFolder(const std::wstring& path);
void RevealInExplorer(const std::wstring& path);  // показать файл в проводнике

// Диалоги оболочки. Возвращают false, если пользователь отменил.
bool PickFolder(HWND owner, const std::wstring& startAt, std::wstring& out);
bool PickFile(HWND owner, const wchar_t* filter, const std::wstring& startAt, std::wstring& out);
