#include "panels.h"

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "util.h"

using namespace Gdiplus;

namespace {

// Логическая геометрия содержимого (96 DPI).
constexpr int kNavH = 30;      // строка с датой и стрелками
constexpr int kRowH = 27;      // строка списка
constexpr int kAddH = 30;      // строка «добавить»
constexpr int kCheck = 17;     // сторона чек-бокса
constexpr int kDots = 18;      // кнопка «⋮» в строке

RectF ToF(const RECT& r) {
    return RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                 static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
}

// Общая мастерская: масштаб, шрифты, мелкие элементы. Панели пользуются только ею.
struct Painter {
    PanelCtx& c;
    Graphics& g;
    int dpi;
    float u;

    explicit Painter(PanelCtx& ctx) : c(ctx), g(*ctx.g), dpi(ctx.dpi), u(ScF(1.0f, ctx.dpi)) {}

    int S(int v) const { return Sc(v, dpi); }
    float F(float v) const { return ScF(v, dpi); }
    int Width() const { return c.body.right - c.body.left; }

    void Add(Act act, int a, int b, const RECT& r) { c.elems->push_back(Elem{act, a, b, r}); }
    bool Hot(Act act, int a = 0, int b = 0) const { return c.hover.Is(act, a, b); }

    const Font& Row() const { return *c.fonts.row; }
    const Font& Small() const { return *c.fonts.small; }
    const Font& Tiny() const { return *c.fonts.tiny; }
    const Font& Num() const { return *c.fonts.num; }

    float TextW(const wchar_t* text, const Font& font) { return gfx::TextWidth(g, text, font); }
    std::wstring Cut(const std::wstring& text, const Font& font, float maxW) {
        return gfx::Ellipsize(g, text, font, maxW);
    }
    void Text(const std::wstring& text, const Font& font, ARGB color, float x, float y,
              gfx::Align align = gfx::Align::Left) {
        gfx::DrawText(g, text.c_str(), font, color, x, y, align);
    }

    // Высота строки текста для выбранного шрифта — чтобы центрировать без «пляски».
    float LineH(const Font& font) const { return font.GetSize(); }

    void RowHover(const RECT& r, bool hot) {
        if (!hot) return;
        RectF box(ToF(r));
        box.X -= F(4.0f);
        box.Width += F(8.0f);
        gfx::FillRoundRect(g, box, F(8.0f), P().hover);
    }

    // Чек-бокс: залитый акцентом квадрат с галочкой либо пустая рамка.
    void Check(const RECT& r, bool on, bool hot) {
        RectF box(ToF(r));
        const float radius = box.Width * 0.28f;
        if (on) {
            gfx::FillRoundRect(g, box, radius, P().accent);
            Color mark(P().onAccent);
            Pen pen(mark, F(2.0f));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            const float x = box.X, y = box.Y, w = box.Width, h = box.Height;
            g.DrawLine(&pen, x + w * 0.24f, y + h * 0.52f, x + w * 0.43f, y + h * 0.71f);
            g.DrawLine(&pen, x + w * 0.43f, y + h * 0.71f, x + w * 0.77f, y + h * 0.30f);
        } else {
            gfx::FillRoundRect(g, box, radius, P().groupBg);
            gfx::StrokeRoundRect(g, box, radius, hot ? P().accent : P().fieldBorder, hot ? 1.6f * u : u);
        }
    }

    // Кнопка «⋮» — появляется под курсором, чтобы строки не рябили.
    void Dots(const RECT& r, bool hot) {
        RectF box(ToF(r));
        if (hot) gfx::FillRoundRect(g, box, box.Width / 2.0f, P().buttonBgHover);
        Color c(hot ? P().accent : P().textFaint);
        SolidBrush dots(c);
        const float cx = box.X + box.Width / 2.0f;
        const float cy = box.Y + box.Height / 2.0f;
        for (int i = -1; i <= 1; i++) {
            g.FillEllipse(&dots, cx - 1.1f * u, cy + i * 3.4f * u - 1.1f * u, 2.2f * u, 2.2f * u);
        }
    }

    // Стрелка «‹» (dir = -1) или «›» (dir = +1).
    void Arrow(const RECT& r, int dir, bool hot) {
        RectF box(ToF(r));
        if (hot) gfx::FillRoundRect(g, box, box.Width / 2.0f, WithAlpha(P().accent, 0x33));
        Color c(hot ? P().accent : P().textMuted);
        Pen pen(c, F(1.6f));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        const float cx = box.X + box.Width / 2.0f;
        const float cy = box.Y + box.Height / 2.0f;
        const float d = F(3.6f);
        // Остриё смотрит в сторону dir: «‹» уводит назад, «›» — вперёд.
        g.DrawLine(&pen, cx - dir * d * 0.6f, cy - d, cx + dir * d * 0.6f, cy);
        g.DrawLine(&pen, cx + dir * d * 0.6f, cy, cx - dir * d * 0.6f, cy + d);
    }

    // Значение в «пилюле»: выполнено / невыполнено / проценты.
    void Pill(const RECT& r, const std::wstring& text, ARGB color) {
        RectF box(ToF(r));
        gfx::FillRoundRect(g, box, box.Height / 2.0f, WithAlpha(color, 0x26));
        const std::wstring cut = Cut(text, Small(), box.Width - F(12.0f));
        Text(cut, Small(), color, box.X + box.Width / 2.0f, box.Y + (box.Height - F(11.0f)) / 2.0f,
             gfx::Align::Center);
    }

    void Bar(const RectF& box, float fraction, ARGB color) {
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        gfx::FillRoundRect(g, box, box.Height / 2.0f, P().track);
        if (fraction <= 0.0f) return;
        RectF fill(box.X, box.Y, box.Width * fraction, box.Height);
        if (fill.Width < box.Height) fill.Width = box.Height;
        gfx::FillRoundRect(g, fill, box.Height / 2.0f, color);
    }

    void Separator(int y) {
        Color line(P().separator);
        Pen pen(line, u);
        g.DrawLine(&pen, static_cast<float>(c.body.left), static_cast<float>(y),
                   static_cast<float>(c.body.right), static_cast<float>(y));
    }

    // Строка «+ Добавить …»: пунктирная рамка, под курсором подсвечивается акцентом.
    void AddRow(const RECT& r, const wchar_t* label, Act act, bool hot) {
        RectF box(ToF(r));
        box.Y += F(3.0f);
        box.Height -= F(6.0f);
        gfx::DashRoundRect(g, box, F(8.0f), hot ? P().accent : P().separator, u);
        Text(label, Small(), hot ? P().accent : P().textFaint, box.X + box.Width / 2.0f,
             box.Y + (box.Height - F(11.0f)) / 2.0f, gfx::Align::Center);
        Add(act, 0, 0, r);
    }

    // Подпись-заголовок раздела внутри блока.
    void Caption(const wchar_t* text, int y) {
        gfx::DrawTracked(g, text, Tiny(), P().textFaint, static_cast<float>(c.body.left),
                         static_cast<float>(y), F(1.4f));
    }

    // Строка с датой и стрелками. step — на сколько дней прыгают стрелки (1 — день, 7 — неделя).
    int NavRow(int y, const std::wstring& label, int step, bool isToday) {
        const int h = S(kNavH);
        const int btn = S(22);
        const int top = y + (h - btn) / 2;

        RECT left = {c.body.left, top, c.body.left + btn, top + btn};
        RECT right = {c.body.right - btn, top, c.body.right, top + btn};
        Arrow(left, -1, Hot(Act::Shift, -step));
        Arrow(right, +1, Hot(Act::Shift, step));
        Add(Act::Shift, -step, 0, left);
        Add(Act::Shift, step, 0, right);

        const float centerX = static_cast<float>(c.body.left + c.body.right) / 2.0f;
        const float maxW = static_cast<float>(c.body.right - c.body.left - btn * 2 - S(8));
        const std::wstring text = Cut(label, Row(), maxW);
        const bool hotLabel = Hot(Act::JumpToday);
        const ARGB color = isToday ? P().textPrimary : (hotLabel ? P().accent : P().textMuted);
        Text(text, Row(), color, centerX, static_cast<float>(y) + (h - F(13.0f)) / 2.0f, gfx::Align::Center);

        if (!isToday) {
            // Ушли от сегодняшнего дня — по клику на дату возвращаемся.
            const float w = TextW(text.c_str(), Row());
            RECT label_rect = {static_cast<LONG>(centerX - w / 2.0f) - S(6), y,
                               static_cast<LONG>(centerX + w / 2.0f) + S(6), y + h};
            Add(Act::JumpToday, 0, 0, label_rect);
        } else {
            // Маленькая точка слева от даты: «это сегодня».
            const float w = TextW(text.c_str(), Row());
            SolidBrush dot(Color(P().accent));
            g.FillEllipse(&dot, centerX - w / 2.0f - F(9.0f), static_cast<float>(y) + h / 2.0f - 2.0f * u,
                          4.0f * u, 4.0f * u);
        }
        return h;
    }

    // Молния трекера энергии.
    void Bolt(const RectF& box, ARGB color) {
        PointF pts[6] = {
            PointF(box.X + box.Width * 0.58f, box.Y),
            PointF(box.X + box.Width * 0.14f, box.Y + box.Height * 0.58f),
            PointF(box.X + box.Width * 0.46f, box.Y + box.Height * 0.58f),
            PointF(box.X + box.Width * 0.36f, box.Y + box.Height),
            PointF(box.X + box.Width * 0.86f, box.Y + box.Height * 0.40f),
            PointF(box.X + box.Width * 0.52f, box.Y + box.Height * 0.40f),
        };
        const Color fill(color);  // именованная переменная: SolidBrush b(Color(x)) читается как объявление функции
        SolidBrush brush(fill);
        g.FillPolygon(&brush, pts, 6);
    }

    // Смайлик настроения: 1 — совсем грустный, 5 — довольный.
    void Face(const RectF& box, int level, bool active, bool hot) {
        const float r = box.Width / 2.0f;
        const float cx = box.X + r;
        const float cy = box.Y + r;
        const ARGB color = active ? P().accent : (hot ? P().textMuted : P().textFaint);
        if (active) {
            gfx::FillRoundRect(g, box, r, WithAlpha(P().accent, 0x26));
        }
        Color c(color);
        Pen pen(c, F(1.4f));
        g.DrawEllipse(&pen, box.X + u, box.Y + u, box.Width - 2 * u, box.Height - 2 * u);

        SolidBrush eyes(c);
        const float eyeR = r * 0.11f;
        g.FillEllipse(&eyes, cx - r * 0.36f - eyeR, cy - r * 0.22f - eyeR, eyeR * 2, eyeR * 2);
        g.FillEllipse(&eyes, cx + r * 0.36f - eyeR, cy - r * 0.22f - eyeR, eyeR * 2, eyeR * 2);

        const float mw = r * 0.9f;
        const float mh = r * 0.7f;
        if (level == 3) {
            g.DrawLine(&pen, cx - mw / 2.0f, cy + r * 0.32f, cx + mw / 2.0f, cy + r * 0.32f);
        } else if (level > 3) {
            const float depth = level == 5 ? 1.0f : 0.6f;
            g.DrawArc(&pen, cx - mw / 2.0f, cy + r * 0.05f, mw, mh * depth, 20.0f, 140.0f);
        } else {
            const float depth = level == 1 ? 1.0f : 0.6f;
            g.DrawArc(&pen, cx - mw / 2.0f, cy + r * 0.35f, mw, mh * depth, 200.0f, 140.0f);
        }
    }
};

// ---------- Задачи дня ----------

void PaintDayTasks(Painter& p) {
    PanelCtx& c = p.c;
    const Day* day = c.data->FindDay(c.selected);
    const DayStats stats = StatsForDay(*c.data, c.selected);
    int y = c.body.top;

    y += p.NavRow(y, FormatLong(c.selected), 1, c.selected == c.today);

    if (c.panel->showChart) {
        // Кольцо прогресса: радиус подбираем под ширину блока, но не больше разумного.
        const float maxR = static_cast<float>(min(p.Width() / 2 - p.S(20), p.S(52)));
        const float radius = max(maxR, static_cast<float>(p.S(24)));
        const float thickness = radius * 0.24f;
        const float cx = static_cast<float>(c.body.left + c.body.right) / 2.0f;
        const float cy = static_cast<float>(y) + radius + p.F(6.0f);
        const float fraction = stats.total > 0 ? static_cast<float>(stats.done) / stats.total : 0.0f;
        gfx::Ring(p.g, cx, cy, radius, thickness, fraction, P().track, P().accent);

        wchar_t percent[8];
        swprintf(percent, 8, L"%d%%", stats.Percent());
        // Цифры и подпись под ними — один блок, центрированный по кольцу: у GDI+ строка
        // рисуется от верхней кромки, поэтому поднимаем её на половину высоты пары.
        Font big(c.fonts.family, radius * 0.62f, FontStyleBold, UnitPixel);
        p.Text(percent, big, P().textPrimary, cx, cy - radius * 0.48f, gfx::Align::Center);
        p.Text(L"выполнено", p.Tiny(), P().textFaint, cx, cy + radius * 0.18f, gfx::Align::Center);

        y = static_cast<int>(cy + radius + p.F(10.0f));
    }

    const int rowH = p.S(kRowH);
    const int check = p.S(kCheck);
    const int dots = p.S(kDots);
    const int count = day ? static_cast<int>(day->tasks.size()) : 0;

    for (int i = 0; i < count; i++) {
        const Task& task = day->tasks[static_cast<size_t>(i)];
        RECT row = {c.body.left, y, c.body.right, y + rowH};
        const bool hotRow = p.Hot(Act::EditTask, i) || p.Hot(Act::ToggleTask, i) || p.Hot(Act::TaskMenu, i);
        p.RowHover(row, hotRow);

        RECT box = {row.left, y + (rowH - check) / 2, row.left + check, y + (rowH - check) / 2 + check};
        p.Check(box, task.done, p.Hot(Act::ToggleTask, i));
        p.Add(Act::ToggleTask, i, 0, box);

        const float textX = static_cast<float>(box.right + p.S(9));
        const float limit = static_cast<float>(row.right) - textX - (hotRow ? dots + p.F(6.0f) : p.F(2.0f));
        const std::wstring text = p.Cut(task.text, p.Row(), limit);
        const ARGB color = task.done ? P().textFaint : P().textPrimary;
        const float textY = static_cast<float>(y) + (rowH - p.F(13.0f)) / 2.0f;
        p.Text(text, p.Row(), color, textX, textY);
        if (task.done) {
            // Зачёркиваем выполненное — видно с одного взгляда, что уже позади.
            Color line(P().textFaint);
            Pen pen(line, p.u);
            const float w = p.TextW(text.c_str(), p.Row());
            p.g.DrawLine(&pen, textX, textY + p.F(7.0f), textX + w, textY + p.F(7.0f));
        }

        RECT textRect = {box.right, row.top, row.right - (hotRow ? dots : 0), row.bottom};
        p.Add(Act::EditTask, i, 0, textRect);

        if (hotRow) {
            RECT menu = {row.right - dots, y + (rowH - dots) / 2, row.right, y + (rowH - dots) / 2 + dots};
            p.Dots(menu, p.Hot(Act::TaskMenu, i));
            p.Add(Act::TaskMenu, i, 0, menu);
        }
        y += rowH;
    }

    if (count == 0) {
        p.Text(L"На этот день задач нет", p.Small(), P().textFaint,
               static_cast<float>(c.body.left + c.body.right) / 2.0f, static_cast<float>(y) + p.F(6.0f),
               gfx::Align::Center);
        y += p.S(26);
    }

    RECT add = {c.body.left, y, c.body.right, y + p.S(kAddH)};
    p.AddRow(add, L"+  Добавить задачу", Act::AddTask, p.Hot(Act::AddTask));
    y += p.S(kAddH);

    if (c.panel->showTotals && stats.total > 0) {
        y += p.S(6);
        p.Separator(y);
        y += p.S(8);
        const int pillH = p.S(22);
        const int half = (p.Width() - p.S(8)) / 2;
        wchar_t doneText[32], leftText[32];
        swprintf(doneText, 32, L"Выполнено %d", stats.done);
        swprintf(leftText, 32, L"Осталось %d", stats.total - stats.done);
        RECT a = {c.body.left, y, c.body.left + half, y + pillH};
        RECT b = {c.body.right - half, y, c.body.right, y + pillH};
        p.Pill(a, doneText, P().ok);
        p.Pill(b, leftText, stats.done == stats.total ? P().textFaint : P().danger);
        y += pillH;
    }

    c.contentH = y - c.body.top + p.S(4);
    c.scrollable = true;
}

// ---------- Цели недели ----------

void PaintWeekGoals(Painter& p) {
    PanelCtx& c = p.c;
    const Date monday = MondayOf(c.selected);
    const Week* week = c.data->FindWeek(monday);
    const bool thisWeek = MondayOf(c.today) == monday;
    int y = c.body.top;

    y += p.NavRow(y, FormatWeek(monday), 7, thisWeek);

    const int rowH = p.S(kRowH + 3);
    const int check = p.S(kCheck);
    const int dots = p.S(kDots);
    const int count = week ? static_cast<int>(week->goals.size()) : 0;
    int done = 0;

    for (int i = 0; i < count; i++) {
        const Goal& goal = week->goals[static_cast<size_t>(i)];
        if (goal.done) done++;
        RECT row = {c.body.left, y, c.body.right, y + rowH};
        const bool hotRow = p.Hot(Act::EditGoal, i) || p.Hot(Act::ToggleGoal, i) || p.Hot(Act::GoalMenu, i);
        p.RowHover(row, hotRow);

        // Номер цели — как в бумажном планере.
        wchar_t number[8];
        swprintf(number, 8, L"%d.", i + 1);
        const float numX = static_cast<float>(row.left) + p.F(2.0f);
        p.Text(number, p.Small(), P().textFaint, numX, static_cast<float>(y) + (rowH - p.F(11.0f)) / 2.0f);

        RECT box = {row.right - check, y + (rowH - check) / 2, row.right, y + (rowH - check) / 2 + check};
        if (hotRow) {
            // Под курсором справа появляется «⋮» — двигаем галочку влево, чтобы не наезжали.
            box.left -= dots + p.S(4);
            box.right -= dots + p.S(4);
        }
        p.Check(box, goal.done, p.Hot(Act::ToggleGoal, i));
        p.Add(Act::ToggleGoal, i, 0, box);

        const float textX = numX + p.F(18.0f);
        const std::wstring text = p.Cut(goal.text, p.Row(), static_cast<float>(box.left) - textX - p.F(8.0f));
        const float textY = static_cast<float>(y) + (rowH - p.F(13.0f)) / 2.0f;
        p.Text(text, p.Row(), goal.done ? P().textFaint : P().textPrimary, textX, textY);
        if (goal.done) {
            Color line(P().textFaint);
            Pen pen(line, p.u);
            const float w = p.TextW(text.c_str(), p.Row());
            p.g.DrawLine(&pen, textX, textY + p.F(7.0f), textX + w, textY + p.F(7.0f));
        }

        RECT textRect = {row.left, row.top, box.left, row.bottom};
        p.Add(Act::EditGoal, i, 0, textRect);

        if (hotRow) {
            RECT menu = {row.right - dots, y + (rowH - dots) / 2, row.right, y + (rowH - dots) / 2 + dots};
            p.Dots(menu, p.Hot(Act::GoalMenu, i));
            p.Add(Act::GoalMenu, i, 0, menu);
        }
        y += rowH;
    }

    if (count == 0) {
        p.Text(L"Цели на эту неделю ещё не заданы", p.Small(), P().textFaint,
               static_cast<float>(c.body.left + c.body.right) / 2.0f, static_cast<float>(y) + p.F(6.0f),
               gfx::Align::Center);
        y += p.S(26);
    }

    RECT add = {c.body.left, y, c.body.right, y + p.S(kAddH)};
    p.AddRow(add, L"+  Добавить цель", Act::AddGoal, p.Hot(Act::AddGoal));
    y += p.S(kAddH);

    if (c.panel->showTotals && count > 0) {
        y += p.S(8);
        RectF bar(static_cast<float>(c.body.left), static_cast<float>(y), static_cast<float>(p.Width()),
                  p.F(6.0f));
        p.Bar(bar, static_cast<float>(done) / static_cast<float>(count), P().accent);
        y += p.S(12);
        wchar_t text[48];
        swprintf(text, 48, L"%d из %d %ls достигнуто", done, count,
                 PluralRu(count, L"цели", L"целей", L"целей"));
        p.Text(text, p.Small(), P().textMuted, static_cast<float>(c.body.left), static_cast<float>(y));
        y += p.S(16);
    }

    c.contentH = y - c.body.top + p.S(4);
    c.scrollable = true;
}

// ---------- Прогресс недели ----------

void PaintWeekProgress(Painter& p) {
    PanelCtx& c = p.c;
    const Date monday = MondayOf(c.selected);
    const bool thisWeek = MondayOf(c.today) == monday;
    int y = c.body.top;

    y += p.NavRow(y, FormatWeek(monday), 7, thisWeek);

    // Легенда: что за столбики.
    {
        const float boxW = p.F(9.0f);
        float x = static_cast<float>(c.body.left);
        const float ly = static_cast<float>(y);
        gfx::FillRoundRect(p.g, RectF(x, ly + p.F(1.0f), boxW, boxW), boxW * 0.3f, P().track);
        p.Text(L"запланировано", p.Tiny(), P().textFaint, x + boxW + p.F(5.0f), ly);
        x += boxW + p.F(5.0f) + p.TextW(L"запланировано", p.Tiny()) + p.F(12.0f);
        gfx::FillRoundRect(p.g, RectF(x, ly + p.F(1.0f), boxW, boxW), boxW * 0.3f, P().accent);
        p.Text(L"выполнено", p.Tiny(), P().textFaint, x + boxW + p.F(5.0f), ly);
        y += p.S(16);
    }

    const int labelH = p.S(18);
    const int chartTop = y + p.S(4);
    const int chartBottom = c.viewport.bottom - labelH - p.S(4);
    const int chartH = max(chartBottom - chartTop, p.S(40));

    DayStats stats[7];
    int maxTotal = 1;
    for (int i = 0; i < 7; i++) {
        stats[i] = StatsForDay(*c.data, AddDays(monday, i));
        if (stats[i].total > maxTotal) maxTotal = stats[i].total;
    }

    // Три линии сетки: по ним видно, что столбики можно сравнивать.
    {
        Color c1(P().separator);
        Pen pen(c1, p.u);
        for (int k = 0; k <= 2; k++) {
            const float gy = static_cast<float>(chartBottom) - chartH * k / 2.0f;
            p.g.DrawLine(&pen, static_cast<float>(c.body.left), gy, static_cast<float>(c.body.right), gy);
        }
    }

    const float slotW = static_cast<float>(p.Width()) / 7.0f;
    const float barW = min(slotW * 0.56f, p.F(26.0f));
    for (int i = 0; i < 7; i++) {
        const Date date = AddDays(monday, i);
        const bool isToday = date == c.today;
        const bool isSelected = date == c.selected;
        const float cx = static_cast<float>(c.body.left) + slotW * (i + 0.5f);

        const float totalH = chartH * static_cast<float>(stats[i].total) / static_cast<float>(maxTotal);
        const float doneH = chartH * static_cast<float>(stats[i].done) / static_cast<float>(maxTotal);

        if (stats[i].total > 0) {
            RectF track(cx - barW / 2.0f, static_cast<float>(chartBottom) - totalH, barW, totalH);
            gfx::FillRoundRect(p.g, track, min(barW / 2.0f, p.F(5.0f)), P().track);
        }
        if (stats[i].done > 0) {
            RectF fill(cx - barW / 2.0f, static_cast<float>(chartBottom) - doneH, barW, doneH);
            gfx::FillRoundRect(p.g, fill, min(barW / 2.0f, p.F(5.0f)), P().accent);
        }
        if (stats[i].total > 0) {
            wchar_t value[16];
            swprintf(value, 16, L"%d/%d", stats[i].done, stats[i].total);
            p.Text(value, p.Tiny(), P().textFaint, cx,
                   static_cast<float>(chartBottom) - max(totalH, doneH) - p.F(13.0f), gfx::Align::Center);
        }

        // Подпись дня: сегодня — акцентом, выбранный день — в «пилюле».
        const float labelY = static_cast<float>(chartBottom) + p.F(5.0f);
        if (isSelected) {
            const float w = p.TextW(WeekdayShort(i), p.Small()) + p.F(12.0f);
            gfx::FillRoundRect(p.g, RectF(cx - w / 2.0f, labelY - p.F(2.0f), w, p.F(16.0f)), p.F(8.0f),
                               WithAlpha(P().accent, 0x26));
        }
        p.Text(WeekdayShort(i), p.Small(), isToday ? P().accent : P().textMuted, cx, labelY,
               gfx::Align::Center);

        RECT hitArea = {static_cast<LONG>(cx - slotW / 2.0f), chartTop, static_cast<LONG>(cx + slotW / 2.0f),
                        chartBottom + labelH};
        p.Add(Act::PickDate, date.y * 10000 + date.m * 100 + date.d, 0, hitArea);
    }

    c.contentH = c.viewport.bottom - c.viewport.top;
    c.scrollable = false;
}

// ---------- Аналитика недели ----------

void PaintWeekStats(Painter& p) {
    PanelCtx& c = p.c;
    const Date monday = MondayOf(c.selected);
    const WeekSummary week = StatsForWeek(*c.data, monday);
    const bool thisWeek = MondayOf(c.today) == monday;
    int y = c.body.top;

    y += p.NavRow(y, FormatWeek(monday), 7, thisWeek);

    int rowsLeft = c.body.left;
    if (c.panel->showChart) {
        const int space = c.viewport.bottom - y;
        const bool side = p.Width() >= p.S(280);  // широкий блок: кольцо слева, цифры справа
        const float radius = side ? static_cast<float>(min(p.Width() / 4, min(space / 2, p.S(46))))
                                  : static_cast<float>(min(p.Width() / 4, p.S(40)));
        const float thickness = radius * 0.24f;
        const float cx = side ? static_cast<float>(c.body.left) + radius + p.F(6.0f)
                              : static_cast<float>(c.body.left + c.body.right) / 2.0f;
        const float cy = side ? static_cast<float>(y) + radius + p.F(4.0f)
                              : static_cast<float>(y) + radius + p.F(4.0f);
        gfx::Ring(p.g, cx, cy, radius, thickness, week.avgPercent / 100.0f, P().track, P().accent);

        wchar_t percent[8];
        swprintf(percent, 8, L"%d%%", week.avgPercent);
        Font big(c.fonts.family, radius * 0.55f, FontStyleBold, UnitPixel);
        p.Text(percent, big, P().textPrimary, cx, cy - radius * 0.44f, gfx::Align::Center);
        p.Text(L"средний", p.Tiny(), P().textFaint, cx, cy + radius * 0.16f, gfx::Align::Center);

        if (side) {
            rowsLeft = static_cast<int>(cx + radius + p.F(14.0f));
        } else {
            y = static_cast<int>(cy + radius + p.F(8.0f));
        }
    }

    struct StatRow {
        const wchar_t* label;
        std::wstring value;
        ARGB color;
    };
    wchar_t goals[24], best[24];
    swprintf(goals, 24, L"%d из %d", week.goalsDone, week.goalsTotal);
    if (week.bestDay >= 0) swprintf(best, 24, L"%ls", WeekdayFull(week.bestDay));
    else swprintf(best, 24, L"—");

    const StatRow rows[] = {
        {L"Всего задач", std::to_wstring(week.total), P().textPrimary},
        {L"Выполнено", std::to_wstring(week.done), P().ok},
        {L"Невыполнено", std::to_wstring(week.undone), week.undone > 0 ? P().danger : P().textFaint},
        {L"Продуктивный день", best, P().textPrimary},
        {L"Цели недели", goals, week.goalsTotal > 0 ? P().accent : P().textFaint},
    };

    // Строк ровно пять, и они важнее свободного места: в низком блоке просто сжимаются.
    const int count = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
    const int space = c.viewport.bottom - y;
    int rowH = p.S(kRowH);
    if (count * rowH > space) rowH = max(space / count, p.S(15));

    for (const StatRow& row : rows) {
        const float ty = static_cast<float>(y) + (rowH - p.F(13.0f)) / 2.0f;
        const float valueW = p.TextW(row.value.c_str(), p.Num());
        const std::wstring label =
            p.Cut(row.label, p.Row(), static_cast<float>(c.body.right - rowsLeft) - valueW - p.F(10.0f));
        p.Text(label, p.Row(), P().textMuted, static_cast<float>(rowsLeft), ty);
        p.Text(row.value, p.Num(), row.color, static_cast<float>(c.body.right), ty, gfx::Align::Right);
        y += rowH;
    }

    c.contentH = c.viewport.bottom - c.viewport.top;
    c.scrollable = false;
}

// ---------- Трекер привычек ----------

void PaintHabits(Painter& p) {
    PanelCtx& c = p.c;
    const Date monday = MondayOf(c.selected);
    const bool thisWeek = MondayOf(c.today) == monday;
    int y = c.body.top;

    y += p.NavRow(y, FormatWeek(monday), 7, thisWeek);

    const int pctW = c.panel->showTotals ? p.S(46) : 0;
    const int nameW = max(p.S(64), (p.Width() - pctW) * 34 / 100);
    const float colW = static_cast<float>(p.Width() - nameW - pctW) / 7.0f;
    const int check = max(min(p.S(kCheck), static_cast<int>(colW) - p.S(4)), p.S(10));

    // Шапка таблицы: дни недели.
    {
        p.Text(L"Привычка", p.Tiny(), P().textFaint, static_cast<float>(c.body.left),
               static_cast<float>(y) + p.F(2.0f));
        for (int i = 0; i < 7; i++) {
            const Date date = AddDays(monday, i);
            const float cx = static_cast<float>(c.body.left + nameW) + colW * (i + 0.5f);
            p.Text(WeekdayShort(i), p.Tiny(), date == c.today ? P().accent : P().textFaint, cx,
                   static_cast<float>(y) + p.F(2.0f), gfx::Align::Center);
        }
        if (pctW > 0) {
            p.Text(L"%", p.Tiny(), P().textFaint, static_cast<float>(c.body.right),
                   static_cast<float>(y) + p.F(2.0f), gfx::Align::Right);
        }
        y += p.S(18);
        p.Separator(y - p.S(4));
    }

    const int rowH = p.S(kRowH);
    const int dots = p.S(kDots);
    for (size_t k = 0; k < c.data->habits.size(); k++) {
        const Habit& habit = c.data->habits[k];
        RECT row = {c.body.left, y, c.body.right, y + rowH};
        const bool hotRow = p.Hot(Act::HabitMenu, habit.id);
        bool hotAny = hotRow;
        for (int i = 0; i < 7 && !hotAny; i++) hotAny = p.Hot(Act::ToggleHabit, habit.id, i);
        p.RowHover(row, hotAny);

        const float textY = static_cast<float>(y) + (rowH - p.F(12.0f)) / 2.0f;
        const std::wstring name =
            p.Cut(habit.name, p.Small(), static_cast<float>(nameW) - p.F(10.0f) - (hotAny ? dots : 0));
        p.Text(name, p.Small(), P().textPrimary, static_cast<float>(c.body.left), textY);

        for (int i = 0; i < 7; i++) {
            const Day* day = c.data->FindDay(AddDays(monday, i));
            const bool on = day && day->HasHabit(habit.id);
            const float cx = static_cast<float>(c.body.left + nameW) + colW * (i + 0.5f);
            RECT box = {static_cast<LONG>(cx - check / 2.0f), y + (rowH - check) / 2,
                        static_cast<LONG>(cx - check / 2.0f) + check, y + (rowH - check) / 2 + check};
            p.Check(box, on, p.Hot(Act::ToggleHabit, habit.id, i));
            RECT hit = {static_cast<LONG>(cx - colW / 2.0f), row.top, static_cast<LONG>(cx + colW / 2.0f),
                        row.bottom};
            p.Add(Act::ToggleHabit, habit.id, i, hit);
        }

        if (pctW > 0) {
            const int percent = HabitPercent(*c.data, monday, habit.id);
            wchar_t text[8];
            swprintf(text, 8, L"%d%%", percent);
            p.Text(text, p.Tiny(), percent >= 70 ? P().accent : P().textMuted,
                   static_cast<float>(c.body.right), textY, gfx::Align::Right);
            RectF bar(static_cast<float>(c.body.right - pctW + p.S(6)), static_cast<float>(row.bottom) - p.F(6.0f),
                      static_cast<float>(pctW - p.S(6)), p.F(3.0f));
            p.Bar(bar, percent / 100.0f, P().accent);
        }

        if (hotAny) {
            RECT menu = {c.body.left + nameW - dots - p.S(4), y + (rowH - dots) / 2,
                         c.body.left + nameW - p.S(4), y + (rowH - dots) / 2 + dots};
            p.Dots(menu, hotRow);
            p.Add(Act::HabitMenu, habit.id, 0, menu);
        }
        y += rowH;
    }

    if (c.data->habits.empty()) {
        p.Text(L"Привычек пока нет", p.Small(), P().textFaint,
               static_cast<float>(c.body.left + c.body.right) / 2.0f, static_cast<float>(y) + p.F(6.0f),
               gfx::Align::Center);
        y += p.S(26);
    }

    RECT add = {c.body.left, y, c.body.right, y + p.S(kAddH)};
    p.AddRow(add, L"+  Добавить привычку", Act::AddHabit, p.Hot(Act::AddHabit));
    y += p.S(kAddH);

    c.contentH = y - c.body.top + p.S(4);
    c.scrollable = true;
}

// ---------- Дневник ----------

void PaintJournal(Painter& p) {
    PanelCtx& c = p.c;
    const Day* day = c.data->FindDay(c.selected);
    Journal journal;
    if (day) journal = day->journal;
    int y = c.body.top;

    y += p.NavRow(y, FormatLong(c.selected), 1, c.selected == c.today);

    const int rowH = p.S(30);

    // --- Сон: минус, значение, плюс ---
    {
        RECT row = {c.body.left, y, c.body.right, y + rowH};
        p.Text(L"Сон", p.Row(), P().textMuted, static_cast<float>(row.left),
               static_cast<float>(y) + (rowH - p.F(13.0f)) / 2.0f);

        const int btn = p.S(20);
        RECT plus = {row.right - btn, y + (rowH - btn) / 2, row.right, y + (rowH - btn) / 2 + btn};
        RECT minus = {row.right - btn * 3 - p.S(40), plus.top, row.right - btn * 2 - p.S(40), plus.bottom};

        auto stepper = [&](const RECT& r, bool isPlus, bool hot) {
            RectF box(ToF(r));
            gfx::FillRoundRect(p.g, box, box.Width / 2.0f, hot ? WithAlpha(P().accent, 0x33) : P().buttonBg);
            Color col(hot ? P().accent : P().textMuted);
            Pen pen(col, p.F(1.5f));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            const float cx = box.X + box.Width / 2.0f, cy = box.Y + box.Height / 2.0f;
            p.g.DrawLine(&pen, cx - p.F(4.0f), cy, cx + p.F(4.0f), cy);
            if (isPlus) p.g.DrawLine(&pen, cx, cy - p.F(4.0f), cx, cy + p.F(4.0f));
        };
        stepper(minus, false, p.Hot(Act::SleepMinus));
        stepper(plus, true, p.Hot(Act::SleepPlus));
        p.Add(Act::SleepMinus, 0, 0, minus);
        p.Add(Act::SleepPlus, 0, 0, plus);

        wchar_t value[24];
        if (journal.sleep <= 0) swprintf(value, 24, L"—");
        else if (journal.sleep % 60 == 0) swprintf(value, 24, L"%d ч", journal.sleep / 60);
        else swprintf(value, 24, L"%d ч %02d", journal.sleep / 60, journal.sleep % 60);
        p.Text(value, p.Num(), journal.sleep > 0 ? P().textPrimary : P().textFaint,
               static_cast<float>(minus.right + plus.left) / 2.0f,
               static_cast<float>(y) + (rowH - p.F(13.0f)) / 2.0f, gfx::Align::Center);
        y += rowH;
    }

    // --- Энергия: три молнии ---
    {
        RECT row = {c.body.left, y, c.body.right, y + rowH};
        p.Text(L"Энергия", p.Row(), P().textMuted, static_cast<float>(row.left),
               static_cast<float>(y) + (rowH - p.F(13.0f)) / 2.0f);
        const int size = p.S(18);
        for (int i = 3; i >= 1; i--) {
            const int right = row.right - (3 - i) * (size + p.S(6));
            RECT box = {right - size, y + (rowH - size) / 2, right, y + (rowH - size) / 2 + size};
            const bool on = journal.energy >= i;
            const bool hot = p.Hot(Act::SetEnergy, i);
            p.Bolt(ToF(box), on ? P().accent : (hot ? P().textMuted : P().track));
            p.Add(Act::SetEnergy, i, 0, box);
        }
        y += rowH;
    }

    // --- Настроение: пять смайлов ---
    {
        RECT row = {c.body.left, y, c.body.right, y + rowH + p.S(4)};
        p.Text(L"Настроение", p.Row(), P().textMuted, static_cast<float>(row.left),
               static_cast<float>(row.top) + (row.bottom - row.top - p.F(13.0f)) / 2.0f);
        const int size = max(min(p.S(22), (p.Width() - p.S(90)) / 5 - p.S(2)), p.S(12));
        for (int i = 5; i >= 1; i--) {
            const int right = row.right - (5 - i) * (size + p.S(4));
            RECT box = {right - size, row.top + (row.bottom - row.top - size) / 2, right,
                        row.top + (row.bottom - row.top - size) / 2 + size};
            p.Face(ToF(box), i, journal.mood == i, p.Hot(Act::SetMood, i));
            p.Add(Act::SetMood, i, 0, box);
        }
        y = row.bottom;
    }

    y += p.S(6);
    p.Separator(y);
    y += p.S(8);

    // --- Три текстовых поля дня ---
    struct TextRow {
        const wchar_t* label;
        const std::wstring* value;
        Act act;
    };
    const TextRow texts[] = {
        {L"Урок дня", &journal.lesson, Act::EditLesson},
        {L"Благодарность", &journal.gratitude, Act::EditGratitude},
        {L"Мысли", &journal.thoughts, Act::EditThoughts},
    };
    for (const TextRow& item : texts) {
        const int h = p.S(38);
        RECT row = {c.body.left, y, c.body.right, y + h};
        const bool hot = p.Hot(item.act);
        p.RowHover(row, hot);
        p.Text(item.label, p.Tiny(), P().textFaint, static_cast<float>(row.left), static_cast<float>(y) + p.F(2.0f));

        const bool empty = item.value->empty();
        const std::wstring text =
            p.Cut(empty ? L"нажмите, чтобы записать" : *item.value, p.Small(),
                  static_cast<float>(p.Width()) - p.F(4.0f));
        p.Text(text, p.Small(), empty ? P().textFaint : P().textPrimary, static_cast<float>(row.left),
               static_cast<float>(y) + p.F(17.0f));
        p.Add(item.act, 0, 0, row);
        y += h;
    }

    c.contentH = y - c.body.top + p.S(4);
    c.scrollable = true;
}

// ---------- Заметки дня ----------

void PaintNotes(Painter& p) {
    PanelCtx& c = p.c;
    const Day* day = c.data->FindDay(c.selected);
    int y = c.body.top;

    y += p.NavRow(y, FormatLong(c.selected), 1, c.selected == c.today);

    const int rowH = p.S(30);
    const int dots = p.S(kDots);
    for (int i = 0; i < 3; i++) {
        const std::wstring value = day ? day->notes[i] : std::wstring();
        RECT row = {c.body.left, y, c.body.right, y + rowH};
        const bool hot = p.Hot(Act::EditNote, i) || p.Hot(Act::NoteMenu, i);
        p.RowHover(row, hot);

        wchar_t number[8];
        swprintf(number, 8, L"%d", i + 1);
        const float ty = static_cast<float>(y) + (rowH - p.F(12.0f)) / 2.0f;
        p.Text(number, p.Tiny(), P().textFaint, static_cast<float>(row.left) + p.F(2.0f), ty);

        const float textX = static_cast<float>(row.left) + p.F(16.0f);
        const bool empty = value.empty();
        const float limit = static_cast<float>(row.right) - textX - (hot && !empty ? dots + p.F(4.0f) : 0.0f);
        p.Text(p.Cut(empty ? L"—" : value, p.Small(), limit), p.Small(),
               empty ? P().textFaint : P().textPrimary, textX, ty);
        p.Add(Act::EditNote, i, 0, row);

        if (hot && !empty) {
            RECT menu = {row.right - dots, y + (rowH - dots) / 2, row.right, y + (rowH - dots) / 2 + dots};
            p.Dots(menu, p.Hot(Act::NoteMenu, i));
            p.Add(Act::NoteMenu, i, 0, menu);
        }
        y += rowH;
    }

    c.contentH = y - c.body.top + p.S(4);
    c.scrollable = true;
}

// ---------- Часы ----------

void PaintClock(Painter& p) {
    PanelCtx& c = p.c;
    const int h = c.viewport.bottom - c.viewport.top;
    const int w = p.Width();

    wchar_t time[16];
    if (c.data->showSeconds) {
        swprintf(time, 16, L"%02d:%02d:%02d", c.now.wHour, c.now.wMinute, c.now.wSecond);
    } else {
        swprintf(time, 16, L"%02d:%02d", c.now.wHour, c.now.wMinute);
    }

    // Размер цифр подбираем под блок: часы должны читаться через комнату.
    const float byWidth = static_cast<float>(w) / (c.data->showSeconds ? 5.0f : 3.45f);
    const float bySpace = static_cast<float>(h) * (c.panel->showChart ? 0.42f : 0.5f);
    float size = min(byWidth, bySpace);
    if (size < p.F(18.0f)) size = p.F(18.0f);

    Font digits(c.fonts.family, size, FontStyleBold, UnitPixel);
    const float cx = static_cast<float>(c.body.left + c.body.right) / 2.0f;

    // Содержимое стоит по центру блока: растянутые часы не должны липнуть к заголовку.
    float total = size * 1.06f + p.F(18.0f);
    if (c.panel->showChart) total += p.F(25.0f);
    if (c.panel->showTotals) total += p.F(14.0f);
    float y = static_cast<float>(c.viewport.top) + max((h - total) / 2.0f, p.F(2.0f));
    p.Text(time, digits, P().textPrimary, cx, y, gfx::Align::Center);
    y += size * 1.06f;

    p.Text(p.Cut(FormatLong(c.today), p.Small(), static_cast<float>(w)), p.Small(), P().textMuted, cx, y,
           gfx::Align::Center);
    y += p.F(18.0f);

    if (c.panel->showChart) {
        // Полоса «сколько дня прошло» — тот же прогресс, только по часам.
        const int minutes = c.now.wHour * 60 + c.now.wMinute;
        const float fraction = static_cast<float>(minutes) / (24.0f * 60.0f);
        RectF bar(static_cast<float>(c.body.left), y, static_cast<float>(w), p.F(6.0f));
        p.Bar(bar, fraction, P().accent);
        y += p.F(11.0f);
        wchar_t caption[48];
        swprintf(caption, 48, L"день прошёл на %d%%", static_cast<int>(fraction * 100.0f));
        p.Text(caption, p.Tiny(), P().textFaint, cx, y, gfx::Align::Center);
        y += p.F(14.0f);
    }

    if (c.panel->showTotals) {
        const DayStats stats = StatsForDay(*c.data, c.today);
        wchar_t caption[64];
        if (stats.total > 0) {
            swprintf(caption, 64, L"задачи сегодня: %d из %d", stats.done, stats.total);
        } else {
            swprintf(caption, 64, L"на сегодня задач нет");
        }
        p.Text(caption, p.Tiny(), P().textMuted, cx, y, gfx::Align::Center);
    }

    c.contentH = h;
    c.scrollable = false;
}

// ---------- Календарь ----------

void PaintCalendar(Painter& p) {
    PanelCtx& c = p.c;
    const Date base = AddMonths(c.today, c.panel->nav);
    int y = c.body.top;

    // Шапка месяца со стрелками.
    {
        const int btn = p.S(22);
        const int top = y + (p.S(kNavH) - btn) / 2;
        RECT left = {c.body.left, top, c.body.left + btn, top + btn};
        RECT right = {c.body.right - btn, top, c.body.right, top + btn};
        p.Arrow(left, -1, p.Hot(Act::ShiftMonth, -1));
        p.Arrow(right, +1, p.Hot(Act::ShiftMonth, 1));
        p.Add(Act::ShiftMonth, -1, 0, left);
        p.Add(Act::ShiftMonth, 1, 0, right);

        const float cx = static_cast<float>(c.body.left + c.body.right) / 2.0f;
        const bool hot = p.Hot(Act::MonthToday);
        const std::wstring text = p.Cut(FormatMonthYear(base), p.Row(),
                                        static_cast<float>(p.Width() - btn * 2 - p.S(8)));
        p.Text(text, p.Row(), c.panel->nav == 0 ? P().textPrimary : (hot ? P().accent : P().textMuted), cx,
               static_cast<float>(y) + (p.S(kNavH) - p.F(13.0f)) / 2.0f, gfx::Align::Center);
        if (c.panel->nav != 0) {
            const float w = p.TextW(text.c_str(), p.Row());
            RECT hit = {static_cast<LONG>(cx - w / 2.0f) - p.S(6), y, static_cast<LONG>(cx + w / 2.0f) + p.S(6),
                        y + p.S(kNavH)};
            p.Add(Act::MonthToday, 0, 0, hit);
        }
        y += p.S(kNavH);
    }

    const float cellW = static_cast<float>(p.Width()) / 7.0f;

    // Строка дней недели.
    for (int i = 0; i < 7; i++) {
        const float cx = static_cast<float>(c.body.left) + cellW * (i + 0.5f);
        p.Text(WeekdayShort(i), p.Tiny(), i >= 5 ? P().textFaint : P().textMuted, cx,
               static_cast<float>(y) + p.F(2.0f), gfx::Align::Center);
    }
    y += p.S(16);

    const Date first = Date{base.y, base.m, 1};
    const Date gridStart = MondayOf(first);
    const int rows = 6;
    const int available = c.viewport.bottom - y - p.S(2);
    const float cellH = max(static_cast<float>(available) / rows, p.F(18.0f));
    const float dotR = p.F(2.2f);

    for (int r = 0; r < rows; r++) {
        for (int col = 0; col < 7; col++) {
            const Date date = AddDays(gridStart, r * 7 + col);
            const bool inMonth = date.m == base.m && date.y == base.y;
            const bool isToday = date == c.today;
            const bool isSelected = date == c.selected;
            const int packed = date.y * 10000 + date.m * 100 + date.d;
            const bool hot = p.Hot(Act::PickDate, packed);

            const float cx = static_cast<float>(c.body.left) + cellW * (col + 0.5f);
            const float cy = static_cast<float>(y) + cellH * (r + 0.5f);
            const float d = min(cellW, cellH) - p.F(4.0f);
            RectF box(cx - d / 2.0f, cy - d / 2.0f, d, d);

            if (isSelected) {
                gfx::FillRoundRect(p.g, box, d / 2.0f, P().accent);
            } else if (hot) {
                gfx::FillRoundRect(p.g, box, d / 2.0f, P().hover);
            }
            if (isToday && !isSelected) {
                gfx::StrokeRoundRect(p.g, box, d / 2.0f, P().accent, 1.4f * p.u);
            }

            ARGB color = P().textPrimary;
            if (isSelected) color = P().onAccent;
            else if (!inMonth) color = P().textFaint;
            else if (col >= 5) color = P().textMuted;

            wchar_t number[8];
            swprintf(number, 8, L"%d", date.d);
            p.Text(number, p.Small(), color, cx, cy - p.F(9.0f), gfx::Align::Center);

            // Точка под числом: в этот день были задачи. Заполненная — все сделаны.
            const DayStats stats = StatsForDay(*c.data, date);
            if (stats.total > 0) {
                const ARGB dot = isSelected ? P().onAccent
                                            : (stats.done == stats.total ? P().ok : WithAlpha(P().accent, 0x99));
                const Color dotColor(dot);
                SolidBrush brush(dotColor);
                p.g.FillEllipse(&brush, cx - dotR, cy + p.F(6.0f), dotR * 2, dotR * 2);
            }

            RECT hit = {static_cast<LONG>(cx - cellW / 2.0f), static_cast<LONG>(cy - cellH / 2.0f),
                        static_cast<LONG>(cx + cellW / 2.0f), static_cast<LONG>(cy + cellH / 2.0f)};
            p.Add(Act::PickDate, packed, 0, hit);
        }
    }

    c.contentH = c.viewport.bottom - c.viewport.top;
    c.scrollable = false;
}

}  // namespace

void PaintPanel(PanelCtx& ctx) {
    Painter painter(ctx);
    switch (ctx.panel->kind) {
        case PanelKind::Clock: PaintClock(painter); break;
        case PanelKind::Calendar: PaintCalendar(painter); break;
        case PanelKind::DayTasks: PaintDayTasks(painter); break;
        case PanelKind::WeekGoals: PaintWeekGoals(painter); break;
        case PanelKind::WeekProgress: PaintWeekProgress(painter); break;
        case PanelKind::WeekStats: PaintWeekStats(painter); break;
        case PanelKind::Habits: PaintHabits(painter); break;
        case PanelKind::Journal: PaintJournal(painter); break;
        case PanelKind::Notes: PaintNotes(painter); break;
        default: break;
    }
}
