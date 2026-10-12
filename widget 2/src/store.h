#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Ярлык — это ссылка: сам файл (.lnk, .exe, папка, документ) остаётся там, где лежал.
struct Shortcut {
    std::wstring path;  // полный путь или URL
    std::wstring name;  // подпись под значком
    std::wstring args;  // аргументы командной строки, обычно пусто

    // Не хранится в файле: пересчитывается при загрузке и после изменений.
    bool broken = false;
};

// Блок на рабочем столе: своё окно, своё место, своя сетка и своё оформление.
struct Group {
    std::wstring title = L"Ярлыки";
    bool collapsed = false;
    int columns = 5;      // 1..12 значков в ряду — ширина блока, тянется мышью
    int maxRows = 0;      // видимых рядов; 0 — показывать все, иначе лишние прокручиваются
    int x = INT32_MIN;    // INT32_MIN => место ещё не выбрано, блок встанет сам
    int y = INT32_MIN;
    bool locked = false;  // закреплён: не двигается и не меняет размер мышью

    // Оформление у каждого блока своё: рядом могут стоять тёмный и светлый.
    bool darkTheme = true;
    int accent = 0;
    int opacity = 100;  // 40..100
    int iconSize = 48;  // 24..96 логических пикселей
    bool showLabels = true;

    std::vector<Shortcut> items;
};

struct AppData {
    std::vector<Group> groups{Group{}};  // всегда непусто

    // Оформление по умолчанию: с ним рождается новый блок.
    bool darkTheme = true;
    int accent = 0;
    int opacity = 100;
    int iconSize = 48;
    bool showLabels = true;

    // Это уже общее для всего виджета.
    bool alwaysOnTop = false;
    bool hotkeyEnabled = true;    // Ctrl+Alt+D — показать/скрыть все блоки
    bool moveIntoStorage = true;  // забирать брошенный .lnk с рабочего стола (Ctrl — копировать)
    bool snapToGrid = false;      // магнит: блок прилипает к краям экрана и к соседям

    int TotalItems() const;
};

// Ошибки чтения не фатальны: битый/отсутствующий файл => значения по умолчанию.
bool LoadData(AppData& data);
bool SaveData(const AppData& data);

// Проверяет, на месте ли цели ярлыков, и проставляет Shortcut::broken.
void RefreshBrokenFlags(AppData& data);
