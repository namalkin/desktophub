// Каталог виджетов, вшитый в .exe ресурсом catalog.pak (его собирает tools/pack.py):
// catalog.json, README каждого виджета, картинки из README и иконки.
#pragma once

#include "ui.h"

#include <string>
#include <vector>

namespace catalog {

struct Entry {
    std::string id;
    std::wstring folder;       // «widget 5» — папка в репозитории, ключ для README и картинок
    std::wstring name;         // «Плеер» — так виджет назван в списке
    std::wstring title;        // «TuneBox» — имя программы
    std::wstring summary;      // одна строка под названием
    std::wstring exe;          // «TuneBox.exe» — и имя файла в релизе GitHub
    std::wstring windowClass;  // скрытое окно виджета, которому центр шлёт команды
    std::wstring runValue;     // имя значения в HKCU\...\Run
};

bool Load();  // false — ресурс повреждён или его нет
const std::vector<Entry>& Entries();

// Репозиторий по умолчанию («owner/name») — откуда качать, пока в настройках не указан другой.
const std::wstring& DefaultRepo();
int IndexOf(const std::string& id);

std::string Readme(const Entry& entry);  // UTF-8; пусто, если README не упаковали

// Картинка по ссылке из README («docs/screenshot.png»). Кэшируется, владеет каталог.
Gdiplus::Bitmap* Image(const Entry& entry, const std::wstring& ref);

// Иконка виджета — самый крупный кадр из его app.ico. Может вернуть nullptr.
Gdiplus::Bitmap* Icon(const Entry& entry);

}  // namespace catalog
