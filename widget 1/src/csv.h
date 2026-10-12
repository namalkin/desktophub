#pragma once

#include <windows.h>

#include "store.h"

// Экспорт операций в CSV (разделитель ';', UTF-8 с BOM — Excel открывает без плясок).
// Показывает системный диалог сохранения. false — пользователь отменил или запись не удалась.
bool ExportCsv(HWND owner, const AppData& data, std::wstring& savedPath);

// Импорт: строки дописываются к существующим. added — сколько добавлено.
bool ImportCsv(HWND owner, AppData& data, int& added);
