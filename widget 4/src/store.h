// Данные виджета: блоки на экране, настройки поиска и короткая память о прошлых уборках.
// Сами результаты сканирования здесь не хранятся — они живут в scan:: и устаревают
// быстрее, чем файл успевает записаться.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scan.h"

// Каждый блок — отдельное окно на рабочем столе. Тип задаёт, что внутри.
enum class PanelKind {
    Vacuum,     // пылесос: запуск, ход работы, итог уборки
    Disk,       // кольцо «занято / свободно» по выбранному диску
    Breakdown,  // куда ушло место: доли и легенда
    Folders,    // папки-тяжеловесы со столбиками
    BigFiles,   // самые большие файлы
    Junk,       // мусор по группам, с галочками
    Old,        // давно не открывали
    Orphans,    // хвосты удалённых программ
    System,     // крупные системные файлы и чем они управляются
    Count
};

const wchar_t* PanelKindName(PanelKind kind);
const wchar_t* PanelKindHint(PanelKind kind);
void PanelDefaultSize(PanelKind kind, int& width, int& height);

struct Panel {
    PanelKind kind = PanelKind::Vacuum;
    std::wstring title;  // пусто — берётся имя типа
    bool collapsed = false;
    int x = INT32_MIN;  // INT32_MIN => место ещё не выбрано, блок встанет сам
    int y = INT32_MIN;
    int width = 0;  // 0 => размер по умолчанию для типа
    int height = 0;
    bool locked = false;

    // Оформление у каждого блока своё: рядом могут стоять тёмный и светлый.
    bool darkTheme = true;
    int accent = 0;
    int opacity = 100;  // 40..100

    bool showChart = true;  // кольца и столбики
    bool showHints = true;  // мелкие пояснения под строками
    int expanded = -1;      // какая доля диаграммы раскрыта («из чего она состоит»)

    std::wstring Title() const { return title.empty() ? PanelKindName(kind) : title; }
};

// Что виджет помнит о прошлой уборке — чтобы после перезапуска блоки не были пустыми.
struct Memory {
    bool valid = false;
    wchar_t drive = L'C';
    unsigned long long used = 0;
    unsigned long long total = 0;
    unsigned long long junk = 0;
    std::wstring when;  // «12 сентября, 18:40»
};

struct AppData {
    std::vector<Panel> panels;

    // Оформление по умолчанию: с ним рождается новый блок.
    bool darkTheme = true;
    int accent = 0;
    int opacity = 100;
    int fontIndex = 0;

    // Общее для всего виджета.
    bool alwaysOnTop = false;
    bool hotkeyEnabled = true;  // Ctrl+Alt+D — показать/скрыть все блоки
    bool snapToGrid = false;
    bool scanOnStart = false;   // сканировать сразу после запуска
    bool confirmDelete = true;  // спрашивать перед удалением

    wchar_t drive = L'C';  // какой диск разбираем
    scan::Options options;

    std::vector<int> checkedJunk;  // отмеченные группы мусора (id из junk::Id)
    unsigned long long freedTotal = 0;  // сколько виджет высосал за всё время
    Memory memory;

    bool JunkChecked(int id) const;
    void ToggleJunk(int id);
    int PanelCount(PanelKind kind) const;
};

// Ошибки чтения не фатальны: битый/отсутствующий файл => значения по умолчанию.
bool LoadData(AppData& data);
bool SaveData(AppData& data);

// Набор блоков для первого запуска: пылесос, диск, куда ушло место, мусор.
void FillDefaults(AppData& data);

// Текстовый отчёт о последнем сканировании — его можно открыть и прочитать глазами.
bool WriteReport(const scan::Result& result);
