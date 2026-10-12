#pragma once

#include <windows.h>

// Повторный запуск .exe шлёт это сообщение уже работающему центру, чтобы тот показался.
UINT HubShowMessage();

// startHidden — запуск из автозапуска: центр сразу уходит в трей.
bool CreateHubWindow(HINSTANCE inst, bool startHidden);
