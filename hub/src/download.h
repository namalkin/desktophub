// Скачивание виджета из релиза GitHub в фоновом потоке.
//
// Поток делает всё до конца: качает во временный файл рядом с целевым, проверяет, что
// это .exe, закрывает работающий виджет (он сохраняет данные), подменяет файл и, если
// виджет работал, запускает его снова. Окну центра приходят только сообщения о ходе.
#pragma once

#include <windows.h>

#include <string>

#include "catalog.h"

namespace download {

constexpr UINT kMsgProgress = WM_APP + 20;  // wParam — индекс виджета, lParam — проценты или -1
constexpr UINT kMsgDone = WM_APP + 21;      // wParam — индекс, lParam — 1 удачно / 0 ошибка

// https://github.com/<repo>/releases/latest/download/<exe>
std::wstring ReleaseUrl(const std::wstring& repo, const std::wstring& exe);

bool Start(HWND notify, int index, const catalog::Entry& entry, const std::wstring& url, const std::wstring& dest);

// Обновление самого центра. Запущенный .exe нельзя перезаписать, но можно переименовать:
// текущий файл уходит в WidgetHub.exe.old, новый встаёт на его место. Запустить новый и
// выйти — дело окна (kMsgDone с wParam == kSelfIndex). .old удаляет следующий запуск.
constexpr int kSelfIndex = 1000;
bool StartSelf(HWND notify, const std::wstring& url);
bool Busy(int index);
int Percent(int index);  // -1 — размер неизвестен

// Итог последней загрузки: «Обновлено, 385 КБ» или текст ошибки. Читается один раз.
std::wstring TakeResult(int index);

}  // namespace download
