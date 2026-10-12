// Содержимое блоков: каждый тип рисует себя сам и попутно складывает
// в ctx.elems прямоугольники того, что можно нажать. Одна отрисовка = одна раскладка,
// поэтому попадания мыши всегда совпадают с картинкой.
#pragma once

#include <vector>

#include "date.h"
#include "store.h"
#include "ui.h"

// Что делает нажатие. Сами действия выполняет виджет — панели только сообщают о них.
enum class Act {
    None,
    Shift,      // a — на сколько дней сдвинуть выбранный день (±1, ±7)
    JumpToday,
    AddTask,
    ToggleTask,   // a — индекс задачи
    EditTask,
    TaskMenu,
    AddGoal,
    ToggleGoal,   // a — индекс цели
    EditGoal,
    GoalMenu,
    AddHabit,
    ToggleHabit,  // a — id привычки, b — день недели 0..6
    HabitMenu,    // a — id привычки
    EditNote,     // a — номер заметки 0..2
    NoteMenu,
    SleepMinus,
    SleepPlus,
    SetEnergy,    // a — 1..3
    SetMood,      // a — 1..5
    EditLesson,
    EditGratitude,
    EditThoughts,
    PickDate,     // a — дата, упакованная как y*10000 + m*100 + d
    ShiftMonth,   // a — ±1 месяц (только календарь)
    MonthToday,
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
    const Gdiplus::Font* row = nullptr;    // основной текст строки
    const Gdiplus::Font* small = nullptr;  // подписи, дни недели
    const Gdiplus::Font* tiny = nullptr;   // проценты в таблице
    const Gdiplus::Font* num = nullptr;    // цифры значений
};

struct PanelCtx {
    Gdiplus::Graphics* g = nullptr;
    const AppData* data = nullptr;
    const Panel* panel = nullptr;
    Date selected;  // общий для всех блоков выбранный день
    Date today;
    SYSTEMTIME now{};  // время для блока часов
    int dpi = 96;

    // Содержимое начинается с body.top (в нём уже учтена прокрутка), видно — в пределах viewport.
    RECT body{};
    RECT viewport{};

    PanelFonts fonts;
    Hover hover;
    std::vector<Elem>* elems = nullptr;

    int contentH = 0;    // out: полная высота содержимого
    bool scrollable = false;  // out: блок листается колесом (списки), а не тянется по высоте
};

void PaintPanel(PanelCtx& ctx);
