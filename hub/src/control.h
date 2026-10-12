// Работа с запущенными виджетами: найти, спросить состояние, запустить, закрыть.
#pragma once

#include <windows.h>

#include <string>

#include "catalog.h"
#include "hub_link.h"

namespace control {

struct Status {
    HWND window = nullptr;  // скрытое окно виджета, которому центр шлёт команды
    DWORD pid = 0;
    bool running = false;
    bool linked = false;   // отвечает на команды центра; старые сборки молчат
    bool visible = false;  // блоки на экране
    std::wstring processPath;
};

Status Probe(const catalog::Entry& entry);

// Где лежит .exe: у запущенного процесса, запомненный путь, запись автозапуска, папка
// установки, рядом с центром, сборка в репозитории. Пусто — виджет не найден.
std::wstring ResolvePath(const catalog::Entry& entry, const std::wstring& remembered, const Status& status);

bool Launch(const std::wstring& exePath, std::wstring& error);

// Команда без ожидания ответа. Для меню и окна настроек виджету разрешается выйти
// на передний план — иначе Windows покажет их под окном центра.
void Post(const Status& status, WPARAM command);

// Просит виджет закрыться и ждёт, пока процесс завершится (с сохранением данных).
bool StopAndWait(const catalog::Entry& entry, DWORD timeoutMs);

}  // namespace control
