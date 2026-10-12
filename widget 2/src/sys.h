// Интеграция с Windows: автозапуск, трей, запуск ярлыков, работа с .lnk.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

std::wstring ExePath();

bool IsAutostartEnabled();
bool SetAutostart(bool enable);  // HKCU\...\Run — права администратора не нужны

// Способ крепления блоков к рабочему столу. Оболочка устроена по-разному в разных
// версиях Windows, поэтому режим выбирается при сборке (-DWIDGET_DESKTOP_MODE=N):
//   1 — дочернее окно Progman (так работало до Windows 11);
//   2 — дочернее окно того окна, в котором оболочка реально держит значки;
//   3 — дочернее окно верхнеуровневого WorkerW, при необходимости попросив его создать;
//   4 — без смены родителя: окно верхнего уровня, которое держим прямо над столом.
#ifndef WIDGET_DESKTOP_MODE
#define WIDGET_DESKTOP_MODE 2
#endif

// Окно рабочего стола. Блок, ставший его дочерним окном, живёт внутри рабочего стола:
// не всплывает над чужими окнами и видно его ровно тогда, когда виден сам стол.
HWND DesktopHostWindow();

bool TrayAdd(HWND hwnd, UINT callbackMsg, HICON icon, const wchar_t* tip);
void TrayUpdateTip(HWND hwnd, const wchar_t* tip);
void TrayRemove(HWND hwnd);

void OpenFolder(const std::wstring& path);

// Читает цель и аргументы .lnk. Для ярлыков на виртуальные папки цель пустая — это норма.
bool ResolveShortcut(const std::wstring& lnkPath, std::wstring& target, std::wstring& args);

// Цель ярлыка на месте? Для URL и виртуальных целей всегда true.
bool ShortcutTargetExists(const std::wstring& path);

// Человекочитаемая цель для подсказки: для .lnk — путь назначения, иначе сам путь.
std::wstring ShortcutTargetLabel(const std::wstring& path);

void LaunchShortcut(HWND owner, const std::wstring& path, const std::wstring& args, bool asAdmin);
void RevealInExplorer(const std::wstring& path);
void ShowFileProperties(HWND owner, const std::wstring& path);

// Имя виртуального объекта оболочки («Корзина», «Этот компьютер»).
std::wstring ShellDisplayName(const std::wstring& path);

// Переносит или копирует файл ярлыка в %APPDATA%\ShortcutDock\shortcuts
// (имя при совпадении получает номер).
bool MoveFileIntoStorage(const std::wstring& path, std::wstring& newPath);
bool CopyFileIntoStorage(const std::wstring& path, std::wstring& newPath);
bool IsInStorage(const std::wstring& path);

// Кладёт ярлык на рабочий стол: файл из хранилища переезжает целиком,
// на всё остальное создаётся новый .lnk. Возвращает путь на рабочем столе.
bool PlaceOnDesktop(const std::wstring& path, const std::wstring& name, const std::wstring& args,
                    std::wstring& desktopPath);

// Системный диалог выбора файлов для «Добавить ярлык…».
bool PickFiles(HWND owner, std::vector<std::wstring>& paths);
