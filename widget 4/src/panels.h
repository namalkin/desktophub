// Содержимое блоков: каждый тип рисует себя сам и попутно складывает в ctx.elems
// прямоугольники того, что можно нажать. Одна отрисовка = одна раскладка, поэтому
// попадания мыши всегда совпадают с картинкой.
#pragma once

#include <vector>

#include "scan.h"
#include "store.h"
#include "ui.h"

// Из какого списка строка: одного индекса мало, действия у списков разные.
enum class ListKind { None, Folder, BigFile, OldFile, Orphan, System, Junk, Slice, SlicePart, Drive };

// Строка внутри раскрытой доли адресуется одним числом: доля и место в ней.
// Долей единицы, частей в каждой — единицы, поэтому хватает и такой упаковки.
constexpr int kPartsPerSlice = 16;
inline int PackPart(int slice, int part) { return slice * kPartsPerSlice + part; }
inline int SliceOfPart(int packed) { return packed / kPartsPerSlice; }
inline int PartOfPart(int packed) { return packed % kPartsPerSlice; }

// Что делает нажатие. Сами действия выполняет виджет — панели только сообщают о них.
enum class Act {
    None,
    StartScan,
    StopScan,
    CleanNow,      // пропылесосить отмеченные группы мусора
    OpenReport,
    PickDrive,     // a — индекс диска в ctx.drives
    ToggleJunk,    // a — id группы мусора
    JunkAll,       // a = 1 — отметить всё безопасное, 0 — снять отметки
    OpenRow,       // a — индекс строки, b — ListKind: открыть папку или файл
    RowMenu,       // то же, но меню строки
    ExpandSlice,   // a — доля диаграммы: показать или скрыть, из чего она состоит
    Elevate,       // перезапустить виджет с правами администратора
};

struct Elem {
    Act act = Act::None;
    int a = 0;
    int b = 0;
    RECT rect{};
};

struct Hover {
    Act act = Act::None;
    int a = 0;
    int b = 0;

    bool Is(Act other, int oa, int ob = 0) const { return act == other && a == oa && b == ob; }
    bool operator==(const Hover& o) const { return act == o.act && a == o.a && b == o.b; }
    bool operator!=(const Hover& o) const { return !(*this == o); }
};

struct PanelFonts {
    Gdiplus::FontFamily* family = nullptr;
    const Gdiplus::Font* big = nullptr;    // крупная цифра
    const Gdiplus::Font* row = nullptr;    // основной текст строки
    const Gdiplus::Font* small = nullptr;  // подписи
    const Gdiplus::Font* tiny = nullptr;   // пути, пояснения
    const Gdiplus::Font* num = nullptr;    // размеры справа
};

struct PanelCtx {
    Gdiplus::Graphics* g = nullptr;
    const AppData* data = nullptr;
    const Panel* panel = nullptr;
    const scan::Result* result = nullptr;
    const scan::Progress* progress = nullptr;
    const std::vector<scan::Drive>* drives = nullptr;
    bool elevated = false;
    int dpi = 96;

    // Содержимое начинается с body.top (в нём уже учтена прокрутка), видно — в пределах viewport.
    RECT body{};
    RECT viewport{};

    PanelFonts fonts;
    Hover hover;
    std::vector<Elem>* elems = nullptr;

    int contentH = 0;         // out: полная высота содержимого
    bool scrollable = false;  // out: блок листается колесом
};

void PaintPanel(PanelCtx& ctx);

// Цвет доли диаграммы. Один и тот же тон всегда одного цвета — легенда и кольцо
// должны совпадать без лишних договорённостей.
Gdiplus::ARGB SliceColor(int tone);
