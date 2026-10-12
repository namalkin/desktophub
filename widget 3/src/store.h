// Данные планера: блоки на экране, дни, недели, привычки — и их чтение/запись в data.json.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "date.h"

// Каждый блок — отдельное окно на рабочем столе. Тип задаёт, что внутри.
enum class PanelKind {
    Clock,         // часы, дата, прогресс дня
    Calendar,      // месяц: выбор дня, точки загруженности
    DayTasks,      // задачи выбранного дня + кольцо прогресса
    WeekGoals,     // цели недели с чек-боксами
    WeekProgress,  // столбики «запланировано / выполнено» по дням недели
    WeekStats,     // аналитика недели цифрами
    Habits,        // трекер привычек: привычки × дни недели
    Journal,       // дневник: сон, энергия, настроение, урок, благодарность, мысли
    Notes,         // три заметки дня
    Count
};

const wchar_t* PanelKindName(PanelKind kind);  // «Задачи дня»
const wchar_t* PanelKindHint(PanelKind kind);  // строка для окна настроек
void PanelDefaultSize(PanelKind kind, int& width, int& height);  // логические пиксели

struct Panel {
    PanelKind kind = PanelKind::DayTasks;
    std::wstring title;  // пусто — берётся имя типа
    bool collapsed = false;
    int x = INT32_MIN;   // INT32_MIN => место ещё не выбрано, блок встанет сам
    int y = INT32_MIN;
    int width = 0;       // 0 => размер по умолчанию для типа
    int height = 0;
    bool locked = false;  // закреплён: не двигается и не меняет размер мышью

    // Оформление у каждого блока своё: рядом могут стоять тёмный и светлый.
    bool darkTheme = true;
    int accent = 0;
    int opacity = 100;  // 40..100

    bool showChart = true;   // кольцо, столбики, точки — то, что рисуется
    bool showTotals = true;  // итоговые строки под содержимым
    int nav = 0;             // календарь: смещение месяца относительно текущего

    std::wstring Title() const { return title.empty() ? PanelKindName(kind) : title; }
};

struct Task {
    std::wstring text;
    bool done = false;
};

struct Goal {
    std::wstring text;
    bool done = false;
};

struct Habit {
    int id = 0;
    std::wstring name;
};

// Дневник дня. Ноль везде — «ещё не заполнено», такой день не сохраняется.
struct Journal {
    int sleep = 0;   // минуты сна, шаг 30
    int energy = 0;  // 0..3
    int mood = 0;    // 0..5
    std::wstring lesson;
    std::wstring gratitude;
    std::wstring thoughts;

    bool Empty() const;
};

struct Day {
    Date date;
    std::vector<Task> tasks;
    std::wstring notes[3];
    Journal journal;
    std::vector<int> habits;  // id отмеченных привычек

    bool Empty() const;
    bool HasHabit(int id) const;
    void ToggleHabit(int id);
};

struct Week {
    Date monday;
    std::vector<Goal> goals;

    bool Empty() const { return goals.empty(); }
};

struct AppData {
    std::vector<Panel> panels;
    std::vector<Day> days;
    std::vector<Week> weeks;
    std::vector<Habit> habits;
    int nextHabitId = 1;

    // Оформление по умолчанию: с ним рождается новый блок.
    bool darkTheme = true;
    int accent = 0;
    int opacity = 100;

    int fontIndex = 0;  // шрифт интерфейса — общий для всех блоков

    // Общее для всего виджета.
    bool alwaysOnTop = false;
    bool hotkeyEnabled = true;  // Ctrl+Alt+P — показать/скрыть все блоки
    bool snapToGrid = false;    // магнит: блок прилипает к краям экрана и к соседям
    bool showSeconds = false;   // секунды в блоке часов

    const Day* FindDay(const Date& date) const;
    Day& DayFor(const Date& date);  // создаёт запись, если её ещё нет
    const Week* FindWeek(const Date& monday) const;
    Week& WeekFor(const Date& monday);

    int PanelCount(PanelKind kind) const;
    void Prune();  // выбрасывает пустые дни и недели — файл не растёт от одних заходов
};

// Сводки. Считаются на лету: хранить проценты незачем, они всегда выводятся из задач.
struct DayStats {
    int total = 0;
    int done = 0;
    int Percent() const { return total > 0 ? done * 100 / total : 0; }
};

struct WeekSummary {
    int total = 0;
    int done = 0;
    int undone = 0;
    int avgPercent = 0;  // средний процент по дням, где есть задачи
    int bestDay = -1;    // индекс дня недели с наибольшим числом выполненных
    int goalsTotal = 0;
    int goalsDone = 0;
    int activeDays = 0;
};

DayStats StatsForDay(const AppData& data, const Date& date);
WeekSummary StatsForWeek(const AppData& data, const Date& monday);
int HabitPercent(const AppData& data, const Date& monday, int habitId);  // 0..100 за неделю

// Ошибки чтения не фатальны: битый/отсутствующий файл => значения по умолчанию.
bool LoadData(AppData& data);
bool SaveData(AppData& data);

// Набор блоков для первого запуска: часы, задачи дня, цели недели.
void FillDefaults(AppData& data);
