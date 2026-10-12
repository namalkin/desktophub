#include "store.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "json.h"
#include "util.h"

namespace {

constexpr int kBackupsToKeep = 7;

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (8 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);

    // BOM, если файл правили вручную в блокноте.
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

// Пишем во временный файл и подменяем — так данные не теряются при сбое записи.
bool WriteWholeFileAtomic(const std::wstring& path, const std::string& content) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = TRUE;
    if (!content.empty()) {
        ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    }
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != content.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// Раз в сутки откладываем копию data.json и держим последние kBackupsToKeep штук.
// Дневник и задачи восстановить неоткуда, поэтому копии тут даже важнее, чем в других виджетах.
void RotateBackups() {
    const std::wstring source = DataFile();
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) return;  // ещё нечего копировать

    const std::wstring dir = BackupDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    const std::wstring target = dir + L"\\data-" + IsoW(Today()) + L".json";
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) return;  // копия за сегодня уже есть
    CopyFileW(source.c_str(), target.c_str(), TRUE);

    std::vector<std::wstring> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\data-*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) found.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (found.size() <= static_cast<size_t>(kBackupsToKeep)) return;

    std::sort(found.begin(), found.end());  // имена с ISO-датой сортируются хронологически
    const size_t extra = found.size() - static_cast<size_t>(kBackupsToKeep);
    for (size_t i = 0; i < extra; i++) {
        DeleteFileW((dir + L"\\" + found[i]).c_str());
    }
}

int ClampInt(int value, int lo, int hi) { return value < lo ? lo : (value > hi ? hi : value); }

std::wstring StringField(const mj::Value& obj, const char* key) {
    if (const mj::Value* v = obj.Find(key)) {
        if (v->type == mj::Type::String) return Utf8ToWide(v->str);
    }
    return std::wstring();
}

int IntField(const mj::Value& obj, const char* key, int fallback) {
    if (const mj::Value* v = obj.Find(key)) {
        if (v->type == mj::Type::Number) return static_cast<int>(v->number);
    }
    return fallback;
}

bool BoolField(const mj::Value& obj, const char* key, bool fallback) {
    if (const mj::Value* v = obj.Find(key)) return v->BoolOr(fallback);
    return fallback;
}

}  // namespace

// ---------- Типы блоков ----------

const wchar_t* PanelKindName(PanelKind kind) {
    switch (kind) {
        case PanelKind::Clock: return L"Часы";
        case PanelKind::Calendar: return L"Календарь";
        case PanelKind::DayTasks: return L"Задачи дня";
        case PanelKind::WeekGoals: return L"Цели недели";
        case PanelKind::WeekProgress: return L"Прогресс недели";
        case PanelKind::WeekStats: return L"Аналитика недели";
        case PanelKind::Habits: return L"Трекер привычек";
        case PanelKind::Journal: return L"Дневник";
        case PanelKind::Notes: return L"Заметки дня";
        default: return L"Блок";
    }
}

const wchar_t* PanelKindHint(PanelKind kind) {
    switch (kind) {
        case PanelKind::Clock: return L"Время, дата и прогресс дня";
        case PanelKind::Calendar: return L"Месяц целиком, выбор дня для остальных блоков";
        case PanelKind::DayTasks: return L"Задачи одного дня, дни листаются стрелками";
        case PanelKind::WeekGoals: return L"Три-пять целей на неделю с галочками";
        case PanelKind::WeekProgress: return L"Столбики: сколько задач и сколько сделано";
        case PanelKind::WeekStats: return L"Всего, выполнено, невыполнено, средний прогресс";
        case PanelKind::Habits: return L"Привычки по дням недели и процент за неделю";
        case PanelKind::Journal: return L"Сон, энергия, настроение, урок дня, благодарность";
        case PanelKind::Notes: return L"Три короткие заметки к выбранному дню";
        default: return L"";
    }
}

void PanelDefaultSize(PanelKind kind, int& width, int& height) {
    switch (kind) {
        case PanelKind::Clock: width = 240; height = 150; break;
        case PanelKind::Calendar: width = 268; height = 280; break;
        case PanelKind::DayTasks: width = 300; height = 400; break;
        case PanelKind::WeekGoals: width = 288; height = 268; break;
        case PanelKind::WeekProgress: width = 320; height = 230; break;
        case PanelKind::WeekStats: width = 300; height = 240; break;
        case PanelKind::Habits: width = 400; height = 260; break;
        case PanelKind::Journal: width = 340; height = 320; break;
        case PanelKind::Notes: width = 260; height = 170; break;
        default: width = 300; height = 220; break;
    }
}

// ---------- Записи ----------

bool Journal::Empty() const {
    return sleep == 0 && energy == 0 && mood == 0 && lesson.empty() && gratitude.empty() && thoughts.empty();
}

bool Day::Empty() const {
    if (!tasks.empty() || !habits.empty() || !journal.Empty()) return false;
    for (const std::wstring& note : notes) {
        if (!note.empty()) return false;
    }
    return true;
}

bool Day::HasHabit(int id) const {
    return std::find(habits.begin(), habits.end(), id) != habits.end();
}

void Day::ToggleHabit(int id) {
    auto it = std::find(habits.begin(), habits.end(), id);
    if (it == habits.end()) habits.push_back(id);
    else habits.erase(it);
}

const Day* AppData::FindDay(const Date& date) const {
    for (const Day& day : days) {
        if (day.date == date) return &day;
    }
    return nullptr;
}

Day& AppData::DayFor(const Date& date) {
    for (Day& day : days) {
        if (day.date == date) return day;
    }
    Day fresh;
    fresh.date = date;
    days.push_back(std::move(fresh));
    return days.back();
}

const Week* AppData::FindWeek(const Date& monday) const {
    for (const Week& week : weeks) {
        if (week.monday == monday) return &week;
    }
    return nullptr;
}

Week& AppData::WeekFor(const Date& monday) {
    for (Week& week : weeks) {
        if (week.monday == monday) return week;
    }
    Week fresh;
    fresh.monday = monday;
    weeks.push_back(std::move(fresh));
    return weeks.back();
}

int AppData::PanelCount(PanelKind kind) const {
    int count = 0;
    for (const Panel& panel : panels) {
        if (panel.kind == kind) count++;
    }
    return count;
}

void AppData::Prune() {
    days.erase(std::remove_if(days.begin(), days.end(), [](const Day& d) { return d.Empty(); }), days.end());
    weeks.erase(std::remove_if(weeks.begin(), weeks.end(), [](const Week& w) { return w.Empty(); }),
                weeks.end());
}

// ---------- Сводки ----------

DayStats StatsForDay(const AppData& data, const Date& date) {
    DayStats stats;
    if (const Day* day = data.FindDay(date)) {
        stats.total = static_cast<int>(day->tasks.size());
        for (const Task& task : day->tasks) {
            if (task.done) stats.done++;
        }
    }
    return stats;
}

WeekSummary StatsForWeek(const AppData& data, const Date& monday) {
    WeekSummary week;
    int percentSum = 0;
    int bestDone = -1;
    for (int i = 0; i < 7; i++) {
        const DayStats day = StatsForDay(data, AddDays(monday, i));
        week.total += day.total;
        week.done += day.done;
        if (day.total > 0) {
            week.activeDays++;
            percentSum += day.Percent();
        }
        if (day.done > bestDone) {
            bestDone = day.done;
            week.bestDay = day.done > 0 ? i : -1;
        }
    }
    week.undone = week.total - week.done;
    week.avgPercent = week.activeDays > 0 ? percentSum / week.activeDays : 0;

    if (const Week* goals = data.FindWeek(monday)) {
        week.goalsTotal = static_cast<int>(goals->goals.size());
        for (const Goal& goal : goals->goals) {
            if (goal.done) week.goalsDone++;
        }
    }
    return week;
}

int HabitPercent(const AppData& data, const Date& monday, int habitId) {
    int marked = 0;
    for (int i = 0; i < 7; i++) {
        const Day* day = data.FindDay(AddDays(monday, i));
        if (day && day->HasHabit(habitId)) marked++;
    }
    return marked * 100 / 7;
}

// ---------- Первый запуск ----------

void FillDefaults(AppData& data) {
    const PanelKind starters[] = {PanelKind::Clock, PanelKind::DayTasks, PanelKind::WeekGoals};
    for (PanelKind kind : starters) {
        Panel panel;
        panel.kind = kind;
        panel.darkTheme = data.darkTheme;
        panel.accent = data.accent;
        panel.opacity = data.opacity;
        data.panels.push_back(panel);
    }
}

// ---------- Чтение ----------

bool LoadData(AppData& data) {
    std::string raw;
    if (!ReadWholeFile(DataFile(), raw)) return false;

    mj::Value root;
    if (!mj::Parse(raw, root) || root.type != mj::Type::Object) return false;

    // Оформление читаем первым: блоки без своих полей наследуют его.
    if (const mj::Value* look = root.Find("appearance")) {
        if (look->type == mj::Type::Object) {
            if (const mj::Value* t = look->Find("theme")) {
                if (t->type == mj::Type::String) data.darkTheme = (t->str != "light");
            }
            data.accent = IntField(*look, "accent", data.accent);
            data.opacity = ClampInt(IntField(*look, "opacity", data.opacity), 40, 100);
            data.fontIndex = IntField(*look, "font", data.fontIndex);
        }
    }

    if (const mj::Value* list = root.Find("panels")) {
        if (list->type == mj::Type::Array) {
            std::vector<Panel> panels;
            for (const mj::Value& item : list->arr) {
                if (item.type != mj::Type::Object) continue;
                Panel panel;
                const int kind = IntField(item, "kind", -1);
                if (kind < 0 || kind >= static_cast<int>(PanelKind::Count)) continue;
                panel.kind = static_cast<PanelKind>(kind);
                panel.title = StringField(item, "title");
                panel.collapsed = BoolField(item, "collapsed", false);
                panel.locked = BoolField(item, "locked", false);
                panel.x = IntField(item, "x", INT32_MIN);
                panel.y = IntField(item, "y", INT32_MIN);
                panel.width = ClampInt(IntField(item, "width", 0), 0, 2400);
                panel.height = ClampInt(IntField(item, "height", 0), 0, 2400);

                // Оформление блока: если полей нет (файл старой версии) — берём общее.
                panel.darkTheme = data.darkTheme;
                panel.accent = data.accent;
                panel.opacity = data.opacity;
                if (const mj::Value* t = item.Find("theme")) {
                    if (t->type == mj::Type::String) panel.darkTheme = (t->str != "light");
                }
                panel.accent = IntField(item, "accent", panel.accent);
                panel.opacity = ClampInt(IntField(item, "opacity", panel.opacity), 40, 100);
                panel.showChart = BoolField(item, "chart", true);
                panel.showTotals = BoolField(item, "totals", true);
                panel.nav = ClampInt(IntField(item, "nav", 0), -600, 600);
                panels.push_back(std::move(panel));
            }
            data.panels = std::move(panels);
        }
    }

    if (const mj::Value* list = root.Find("habits")) {
        if (list->type == mj::Type::Array) {
            for (const mj::Value& item : list->arr) {
                if (item.type != mj::Type::Object) continue;
                Habit habit;
                habit.id = IntField(item, "id", 0);
                habit.name = StringField(item, "name");
                if (habit.id <= 0 || habit.name.empty()) continue;
                data.habits.push_back(std::move(habit));
            }
        }
    }
    data.nextHabitId = IntField(root, "nextHabitId", 1);
    for (const Habit& habit : data.habits) {
        if (habit.id >= data.nextHabitId) data.nextHabitId = habit.id + 1;
    }

    if (const mj::Value* list = root.Find("days")) {
        if (list->type == mj::Type::Array) {
            for (const mj::Value& item : list->arr) {
                if (item.type != mj::Type::Object) continue;
                Day day;
                if (const mj::Value* d = item.Find("date")) {
                    if (d->type != mj::Type::String || !ParseIso(d->str, day.date)) continue;
                } else {
                    continue;  // день без даты некуда положить
                }
                if (const mj::Value* tasks = item.Find("tasks")) {
                    if (tasks->type == mj::Type::Array) {
                        for (const mj::Value& raw_task : tasks->arr) {
                            if (raw_task.type != mj::Type::Object) continue;
                            Task task;
                            task.text = StringField(raw_task, "text");
                            if (task.text.empty()) continue;
                            task.done = BoolField(raw_task, "done", false);
                            day.tasks.push_back(std::move(task));
                        }
                    }
                }
                if (const mj::Value* notes = item.Find("notes")) {
                    if (notes->type == mj::Type::Array) {
                        for (size_t i = 0; i < notes->arr.size() && i < 3; i++) {
                            if (notes->arr[i].type == mj::Type::String) {
                                day.notes[i] = Utf8ToWide(notes->arr[i].str);
                            }
                        }
                    }
                }
                if (const mj::Value* marks = item.Find("habits")) {
                    if (marks->type == mj::Type::Array) {
                        for (const mj::Value& mark : marks->arr) {
                            if (mark.type == mj::Type::Number) day.habits.push_back(static_cast<int>(mark.number));
                        }
                    }
                }
                if (const mj::Value* journal = item.Find("journal")) {
                    if (journal->type == mj::Type::Object) {
                        day.journal.sleep = ClampInt(IntField(*journal, "sleep", 0), 0, 24 * 60);
                        day.journal.energy = ClampInt(IntField(*journal, "energy", 0), 0, 3);
                        day.journal.mood = ClampInt(IntField(*journal, "mood", 0), 0, 5);
                        day.journal.lesson = StringField(*journal, "lesson");
                        day.journal.gratitude = StringField(*journal, "gratitude");
                        day.journal.thoughts = StringField(*journal, "thoughts");
                    }
                }
                if (!day.Empty()) data.days.push_back(std::move(day));
            }
        }
    }

    if (const mj::Value* list = root.Find("weeks")) {
        if (list->type == mj::Type::Array) {
            for (const mj::Value& item : list->arr) {
                if (item.type != mj::Type::Object) continue;
                Week week;
                if (const mj::Value* d = item.Find("monday")) {
                    if (d->type != mj::Type::String || !ParseIso(d->str, week.monday)) continue;
                } else {
                    continue;
                }
                week.monday = MondayOf(week.monday);
                if (const mj::Value* goals = item.Find("goals")) {
                    if (goals->type == mj::Type::Array) {
                        for (const mj::Value& raw_goal : goals->arr) {
                            if (raw_goal.type != mj::Type::Object) continue;
                            Goal goal;
                            goal.text = StringField(raw_goal, "text");
                            if (goal.text.empty()) continue;
                            goal.done = BoolField(raw_goal, "done", false);
                            week.goals.push_back(std::move(goal));
                        }
                    }
                }
                if (!week.Empty()) data.weeks.push_back(std::move(week));
            }
        }
    }

    data.alwaysOnTop = BoolField(root, "alwaysOnTop", data.alwaysOnTop);
    data.hotkeyEnabled = BoolField(root, "hotkey", data.hotkeyEnabled);
    data.snapToGrid = BoolField(root, "snapToGrid", data.snapToGrid);
    data.showSeconds = BoolField(root, "seconds", data.showSeconds);
    return true;
}

// ---------- Запись ----------

bool SaveData(AppData& data) {
    data.Prune();

    mj::Value root = mj::Value::Object();

    mj::Value panels = mj::Value::Array();
    for (const Panel& panel : data.panels) {
        mj::Value item = mj::Value::Object();
        item.Set("kind", mj::Value::Num(static_cast<int>(panel.kind)));
        item.Set("title", mj::Value::Str(WideToUtf8(panel.title)));
        item.Set("collapsed", mj::Value::Bool(panel.collapsed));
        item.Set("locked", mj::Value::Bool(panel.locked));
        item.Set("x", mj::Value::Num(panel.x));
        item.Set("y", mj::Value::Num(panel.y));
        item.Set("width", mj::Value::Num(panel.width));
        item.Set("height", mj::Value::Num(panel.height));
        item.Set("theme", mj::Value::Str(panel.darkTheme ? "dark" : "light"));
        item.Set("accent", mj::Value::Num(panel.accent));
        item.Set("opacity", mj::Value::Num(panel.opacity));
        item.Set("chart", mj::Value::Bool(panel.showChart));
        item.Set("totals", mj::Value::Bool(panel.showTotals));
        item.Set("nav", mj::Value::Num(panel.nav));
        panels.arr.push_back(std::move(item));
    }
    root.Set("panels", std::move(panels));

    mj::Value habits = mj::Value::Array();
    for (const Habit& habit : data.habits) {
        mj::Value item = mj::Value::Object();
        item.Set("id", mj::Value::Num(habit.id));
        item.Set("name", mj::Value::Str(WideToUtf8(habit.name)));
        habits.arr.push_back(std::move(item));
    }
    root.Set("habits", std::move(habits));
    root.Set("nextHabitId", mj::Value::Num(data.nextHabitId));

    mj::Value days = mj::Value::Array();
    for (const Day& day : data.days) {
        mj::Value item = mj::Value::Object();
        item.Set("date", mj::Value::Str(Iso(day.date)));
        if (!day.tasks.empty()) {
            mj::Value tasks = mj::Value::Array();
            for (const Task& task : day.tasks) {
                mj::Value entry = mj::Value::Object();
                entry.Set("text", mj::Value::Str(WideToUtf8(task.text)));
                entry.Set("done", mj::Value::Bool(task.done));
                tasks.arr.push_back(std::move(entry));
            }
            item.Set("tasks", std::move(tasks));
        }
        if (!day.notes[0].empty() || !day.notes[1].empty() || !day.notes[2].empty()) {
            mj::Value notes = mj::Value::Array();
            for (const std::wstring& note : day.notes) {
                notes.arr.push_back(mj::Value::Str(WideToUtf8(note)));
            }
            item.Set("notes", std::move(notes));
        }
        if (!day.habits.empty()) {
            mj::Value marks = mj::Value::Array();
            for (int id : day.habits) marks.arr.push_back(mj::Value::Num(id));
            item.Set("habits", std::move(marks));
        }
        if (!day.journal.Empty()) {
            mj::Value journal = mj::Value::Object();
            journal.Set("sleep", mj::Value::Num(day.journal.sleep));
            journal.Set("energy", mj::Value::Num(day.journal.energy));
            journal.Set("mood", mj::Value::Num(day.journal.mood));
            journal.Set("lesson", mj::Value::Str(WideToUtf8(day.journal.lesson)));
            journal.Set("gratitude", mj::Value::Str(WideToUtf8(day.journal.gratitude)));
            journal.Set("thoughts", mj::Value::Str(WideToUtf8(day.journal.thoughts)));
            item.Set("journal", std::move(journal));
        }
        days.arr.push_back(std::move(item));
    }
    root.Set("days", std::move(days));

    mj::Value weeks = mj::Value::Array();
    for (const Week& week : data.weeks) {
        mj::Value item = mj::Value::Object();
        item.Set("monday", mj::Value::Str(Iso(week.monday)));
        mj::Value goals = mj::Value::Array();
        for (const Goal& goal : week.goals) {
            mj::Value entry = mj::Value::Object();
            entry.Set("text", mj::Value::Str(WideToUtf8(goal.text)));
            entry.Set("done", mj::Value::Bool(goal.done));
            goals.arr.push_back(std::move(entry));
        }
        item.Set("goals", std::move(goals));
        weeks.arr.push_back(std::move(item));
    }
    root.Set("weeks", std::move(weeks));

    mj::Value look = mj::Value::Object();
    look.Set("theme", mj::Value::Str(data.darkTheme ? "dark" : "light"));
    look.Set("accent", mj::Value::Num(data.accent));
    look.Set("opacity", mj::Value::Num(data.opacity));
    look.Set("font", mj::Value::Num(data.fontIndex));
    root.Set("appearance", std::move(look));

    root.Set("alwaysOnTop", mj::Value::Bool(data.alwaysOnTop));
    root.Set("hotkey", mj::Value::Bool(data.hotkeyEnabled));
    root.Set("snapToGrid", mj::Value::Bool(data.snapToGrid));
    root.Set("seconds", mj::Value::Bool(data.showSeconds));

    const std::wstring dir = DataDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);  // no-op, если уже есть
    RotateBackups();
    return WriteWholeFileAtomic(DataFile(), mj::Serialize(root));
}
