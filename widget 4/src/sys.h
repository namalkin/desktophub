// Интеграция с Windows: автозапуск, значок в трее, проводник, корзина.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

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
void RevealInExplorer(const std::wstring& path);   // показать файл в проводнике
void ShowFileProperties(HWND owner, const std::wstring& path);

// Запущены ли мы с правами администратора: без них часть системных папок не прочитать
// и не очистить, и об этом честнее сказать заранее.
bool IsElevated();
// Перезапустить себя с запросом прав (UAC). true — новый процесс стартовал, этот закрывается.
bool RestartElevated();

// ---------- Удаление ----------
//
// Два разных удаления, и это принципиально:
//   ToRecycleBin — для файлов пользователя. Они возвращаются из корзины, но место
//     освобождается только после её очистки (на том же диске это просто перемещение);
//   DeletePermanently — для кэшей и временных файлов. Их не жалко, и место освобождается
//     сразу, как и делает «Очистка диска» Windows.
bool ToRecycleBin(HWND owner, const std::vector<std::wstring>& paths);
bool DeletePermanently(const std::wstring& path);  // файл или пустая папка

// Размер корзины по всем дискам и её очистка.
unsigned long long RecycleBinSize();
bool EmptyRecycleBin(HWND owner);
