#pragma once

#include <windows.h>

// Создаёт скрытое окно приложения и окна блоков. false — что-то не получилось,
// показывать виджет не из чего.
bool CreateWidgetWindow(HINSTANCE inst);
