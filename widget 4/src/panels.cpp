#include "panels.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "junk.h"
#include "util.h"

using namespace Gdiplus;

namespace {

// ---------- Мелкие помощники ----------

RectF ToRectF(const RECT& r) {
    return RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                 static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
}

void Add(PanelCtx& ctx, Act act, int a, int b, const RECT& rect) {
    if (!ctx.elems) return;
    Elem elem;
    elem.act = act;
    elem.a = a;
    elem.b = b;
    elem.rect = rect;
    ctx.elems->push_back(elem);
}

bool Hot(const PanelCtx& ctx, Act act, int a, int b = 0) { return ctx.hover.Is(act, a, b); }

// Строка списка: подсветка под курсором и общая геометрия.
RECT RowRect(const PanelCtx& ctx, int top, int height) {
    return {ctx.body.left, top, ctx.body.right, top + height};
}

void RowHighlight(Graphics& g, const PanelCtx& ctx, const RECT& row, bool hot) {
    if (!hot) return;
    RectF box = ToRectF(row);
    box.Inflate(ScF(4.0f, ctx.dpi), -ScF(1.0f, ctx.dpi));
    gfx::FillRoundRect(g, box, ScF(8.0f, ctx.dpi), P().hover);
}

// Горизонтальный столбик «сколько это от самого большого».
void Bar(Graphics& g, const PanelCtx& ctx, float x, float y, float width, float fraction, ARGB color) {
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    const float h = ScF(4.0f, ctx.dpi);
    gfx::FillRoundRect(g, RectF(x, y, width, h), h / 2.0f, P().track);
    float filled = width * fraction;
    if (filled < h) filled = h;  // совсем тонкую полоску не видно
    gfx::FillRoundRect(g, RectF(x, y, filled, h), h / 2.0f, color);
}

void Checkbox(Graphics& g, const PanelCtx& ctx, const RECT& box, bool checked, bool hot) {
    const RectF rf = ToRectF(box);
    const float u = ScF(1.0f, ctx.dpi);
    const float radius = ScF(5.0f, ctx.dpi);
    if (checked) {
        gfx::FillRoundRect(g, rf, radius, P().accent);
        Color c(P().onAccent);
        Pen pen(c, ScF(1.8f, ctx.dpi));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawLine(&pen, rf.X + rf.Width * 0.24f, rf.Y + rf.Height * 0.52f, rf.X + rf.Width * 0.43f,
                   rf.Y + rf.Height * 0.72f);
        g.DrawLine(&pen, rf.X + rf.Width * 0.43f, rf.Y + rf.Height * 0.72f, rf.X + rf.Width * 0.76f,
                   rf.Y + rf.Height * 0.30f);
    } else {
        gfx::StrokeRoundRect(g, rf, radius, hot ? P().accent : P().fieldBorder, 1.4f * u);
    }
}

// Кнопка во всю ширину блока: главное действие панели.
void Button(Graphics& g, const PanelCtx& ctx, const RECT& box, const wchar_t* text, bool primary, bool hot,
            bool disabled) {
    const RectF rf = ToRectF(box);
    const float radius = ScF(9.0f, ctx.dpi);
    ARGB bg;
    ARGB fg;
    if (disabled) {
        bg = P().buttonBg;
        fg = P().textFaint;
    } else if (primary) {
        bg = hot ? Mix(P().accent, P().textPrimary, 0.12f) : P().accent;
        fg = P().onAccent;
    } else {
        bg = hot ? P().buttonBgHover : P().buttonBg;
        fg = hot ? P().textPrimary : P().textMuted;
    }
    gfx::FillRoundRect(g, rf, radius, bg);
    gfx::DrawText(g, text, *ctx.fonts.row, fg, rf.X + rf.Width / 2.0f,
                  rf.Y + (rf.Height - ScF(16.0f, ctx.dpi)) / 2.0f, gfx::Align::Center);
}

// Заглушка «данных ещё нет»: у всех списков она одинаковая.
void Empty(Graphics& g, PanelCtx& ctx, int& y, const wchar_t* text) {
    const float x = static_cast<float>(ctx.body.left);
    const float width = static_cast<float>(ctx.body.right - ctx.body.left);
    const std::wstring line = gfx::Ellipsize(g, text, *ctx.fonts.small, width);
    gfx::DrawText(g, line.c_str(), *ctx.fonts.small, P().textFaint, x + width / 2.0f,
                  static_cast<float>(y) + ScF(14.0f, ctx.dpi), gfx::Align::Center);
    y += Sc(48, ctx.dpi);
}

// Путь целиком в блок не влезает, а нужен он ради «где это лежит»: показываем хвост.
std::wstring ShortPath(Graphics& g, const std::wstring& path, const Font& font, float maxWidth) {
    if (gfx::TextWidth(g, path.c_str(), font) <= maxWidth) return path;
    std::wstring cut = path;
    while (!cut.empty() && gfx::TextWidth(g, (L"…" + cut).c_str(), font) > maxWidth) cut.erase(0, 1);
    return L"…" + cut;
}

// Размер строкой «12,4 ГБ» и он же, но крупной цифрой с мелкой единицей.
void DrawSizeRight(Graphics& g, const PanelCtx& ctx, unsigned long long bytes, float right, float y,
                   ARGB color) {
    gfx::DrawText(g, FormatSize(bytes).c_str(), *ctx.fonts.num, color, right, y, gfx::Align::Right);
}

int PercentOf(unsigned long long part, unsigned long long total) {
    if (total == 0) return 0;
    const unsigned long long percent = part * 100ULL / total;
    return percent > 100 ? 100 : static_cast<int>(percent);
}

}  // namespace

ARGB SliceColor(int tone) {
    // Семь тонов: шесть содержательных и серый для «не удалось заглянуть».
    static const ARGB dark[] = {0xFF3ED6B0, 0xFF6E8BFF, 0xFFA97BFF, 0xFFFF9B4D,
                                0xFFF2C14E, 0xFF5FD2F0, 0xFF7E869A};
    static const ARGB light[] = {0xFF0E9C86, 0xFF3A5CE0, 0xFF7B44E0, 0xFFD9701A,
                                 0xFFC08A00, 0xFF1687A8, 0xFF6B7383};
    const int count = 7;
    if (tone < 0 || tone >= count) tone = count - 1;
    return P().dark ? dark[tone] : light[tone];
}

namespace {

// ---------- Робот ----------

// Маленький пылесос рядом с заголовком блока: пока идёт уборка, вокруг него бежит дуга.
void Robot(Graphics& g, const PanelCtx& ctx, float cx, float cy, float radius, bool busy, ARGB color) {
    const float u = ScF(1.0f, ctx.dpi);
    Color body(color);
    Pen pen(body, 1.8f * u);
    g.DrawEllipse(&pen, cx - radius, cy - radius, radius * 2, radius * 2);

    // Глаза-датчики и щётка снизу — минимум деталей, чтобы читалось и в 16 пикселей.
    SolidBrush dots(body);
    const float eye = radius * 0.16f;
    g.FillEllipse(&dots, cx - radius * 0.38f - eye, cy - radius * 0.18f - eye, eye * 2, eye * 2);
    g.FillEllipse(&dots, cx + radius * 0.38f - eye, cy - radius * 0.18f - eye, eye * 2, eye * 2);
    Pen brush(body, 1.4f * u);
    g.DrawArc(&brush, cx - radius * 0.5f, cy - radius * 0.1f, radius, radius * 0.8f, 20.0f, 140.0f);

    if (!busy) return;
    // Дуга крутится по часам: положение берём из системного времени, таймер и так тикает.
    const float angle = static_cast<float>((GetTickCount() / 4) % 360);
    Pen spin(Color(WithAlpha(color, 0xCC)), 2.2f * u);
    spin.SetStartCap(LineCapRound);
    spin.SetEndCap(LineCapRound);
    const float outer = radius + 4.0f * u;
    g.DrawArc(&spin, cx - outer, cy - outer, outer * 2, outer * 2, angle, 90.0f);
}

// ---------- Пылесос ----------

void PaintVacuum(PanelCtx& ctx, int& y) {
    Graphics& g = *ctx.g;
    const AppData& data = *ctx.data;
    const scan::Result& result = *ctx.result;
    const scan::Progress& progress = *ctx.progress;
    const int dpi = ctx.dpi;
    const float left = static_cast<float>(ctx.body.left);
    const float right = static_cast<float>(ctx.body.right);
    const float width = right - left;

    const bool busy = progress.busy;

    // --- Шапка: робот и что он сейчас делает ---
    {
        const float rowTop = static_cast<float>(y);
        const float radius = ScF(13.0f, dpi);
        Robot(g, ctx, left + radius + ScF(3.0f, dpi), rowTop + radius, radius, busy, P().accent);

        const float textX = left + radius * 2 + ScF(14.0f, dpi);
        std::wstring title;
        if (busy) title = progress.note.empty() ? L"Работаю" : progress.note;
        else if (result.valid) title = L"Уборка закончена";
        else title = L"Готов пылесосить";
        gfx::DrawText(g, title.c_str(), *ctx.fonts.row, P().textPrimary, textX, rowTop + ScF(1.0f, dpi));

        std::wstring second;
        if (busy) {
            second = progress.current.empty() ? L"" : progress.current;
        } else if (result.valid) {
            wchar_t buf[128];
            swprintf(buf, 128, L"Диск %lc: — %d %ls, %ls", result.drive, result.fileCount,
                     PluralRu(result.fileCount, L"файл", L"файла", L"файлов"),
                     FormatSize(result.scanned).c_str());
            second = buf;
        } else if (data.memory.valid) {
            second = L"Прошлый заход: " + data.memory.when;
        } else {
            second = L"Посмотрю, куда ушло место, и вынесу мусор";
        }
        gfx::DrawText(g, ShortPath(g, second, *ctx.fonts.tiny, width - (textX - left)).c_str(),
                      *ctx.fonts.tiny, P().textFaint, textX, rowTop + ScF(19.0f, dpi));
        y += Sc(38, dpi);
    }

    // --- Полоса хода работы ---
    if (busy) {
        const float barY = static_cast<float>(y) + ScF(4.0f, dpi);
        const float h = ScF(8.0f, dpi);
        gfx::FillRoundRect(g, RectF(left, barY, width, h), h / 2.0f, P().track);
        const float fraction = static_cast<float>(progress.percent) / 100.0f;
        float filled = width * fraction;
        if (filled < h) filled = h;
        gfx::FillRoundRect(g, RectF(left, barY, filled, h), h / 2.0f, P().accent);

        wchar_t buf[96];
        swprintf(buf, 96, L"%d%%  ·  %ls", progress.percent, FormatSize(progress.bytes).c_str());
        gfx::DrawText(g, buf, *ctx.fonts.tiny, P().textMuted, left, barY + h + ScF(5.0f, dpi));
        gfx::DrawText(g, scan::StageName(progress.stage), *ctx.fonts.tiny, P().textFaint, right,
                      barY + h + ScF(5.0f, dpi), gfx::Align::Right);
        y += Sc(36, dpi);
    }

    // --- Итог: сколько мусора нашлось ---
    if (!busy && result.valid) {
        const unsigned long long junk = result.JunkTotal();
        const float numberY = static_cast<float>(y) + ScF(2.0f, dpi);
        const std::wstring number = SizeNumber(junk);
        gfx::DrawText(g, number.c_str(), *ctx.fonts.big, junk > 0 ? P().accent : P().textMuted, left,
                      numberY);
        const float numberW = gfx::TextWidth(g, number.c_str(), *ctx.fonts.big);
        gfx::DrawText(g, SizeUnit(junk).c_str(), *ctx.fonts.small, P().textMuted,
                      left + numberW + ScF(6.0f, dpi), numberY + ScF(12.0f, dpi));
        gfx::DrawText(g, junk > 0 ? L"мусора можно вынести" : L"мусора не нашлось", *ctx.fonts.tiny,
                      P().textFaint, left, numberY + ScF(30.0f, dpi));

        wchar_t used[96];
        swprintf(used, 96, L"занято %ls из %ls", FormatSize(result.used).c_str(),
                 FormatSize(result.total).c_str());
        gfx::DrawText(g, used, *ctx.fonts.small, P().textMuted, right, numberY + ScF(4.0f, dpi),
                      gfx::Align::Right);
        if (result.hidden > 0) {
            wchar_t hidden[128];
            swprintf(hidden, 128, L"скрыто от обхода: %ls", FormatSize(result.hidden).c_str());
            gfx::DrawText(g, hidden, *ctx.fonts.tiny, P().textFaint, right, numberY + ScF(24.0f, dpi),
                          gfx::Align::Right);
        }
        y += Sc(50, dpi);
    }

    // --- Кнопки ---
    {
        const int btnH = Sc(34, dpi);
        if (busy) {
            const RECT stop = {ctx.body.left, y, ctx.body.right, y + btnH};
            Button(g, ctx, stop, L"Остановить", false, Hot(ctx, Act::StopScan, 0), false);
            Add(ctx, Act::StopScan, 0, 0, stop);
            y += btnH + Sc(8, dpi);
        } else {
            wchar_t label[64];
            swprintf(label, 64, result.valid ? L"Просканировать заново (%lc:)" : L"Просканировать диск %lc:",
                     data.drive);
            const RECT scan = {ctx.body.left, y, ctx.body.right, y + btnH};
            Button(g, ctx, scan, label, !result.valid, Hot(ctx, Act::StartScan, 0), false);
            Add(ctx, Act::StartScan, 0, 0, scan);
            y += btnH + Sc(8, dpi);

            if (result.valid) {
                const unsigned long long chosen = result.JunkTotal(data.checkedJunk);
                wchar_t clean[96];
                if (chosen > 0) swprintf(clean, 96, L"Пропылесосить — %ls", FormatSize(chosen).c_str());
                else swprintf(clean, 96, L"Отметьте мусор в блоке «Мусор»");
                const RECT go = {ctx.body.left, y, ctx.body.right, y + btnH};
                Button(g, ctx, go, clean, chosen > 0, Hot(ctx, Act::CleanNow, 0), chosen == 0);
                if (chosen > 0) Add(ctx, Act::CleanNow, 0, 0, go);
                y += btnH + Sc(8, dpi);
            }
        }
    }

    // --- Подвал: сколько вынесено за всё время и ссылка на отчёт ---
    {
        if (data.freedTotal > 0) {
            wchar_t total[96];
            swprintf(total, 96, L"Всего вынесено: %ls", FormatSize(data.freedTotal).c_str());
            gfx::DrawText(g, total, *ctx.fonts.tiny, P().ok, left, static_cast<float>(y));
        }
        if (result.valid) {
            const bool hot = Hot(ctx, Act::OpenReport, 0);
            gfx::DrawText(g, L"Отчёт", *ctx.fonts.tiny, hot ? P().accent : P().textFaint, right,
                          static_cast<float>(y), gfx::Align::Right);
            const float w = gfx::TextWidth(g, L"Отчёт", *ctx.fonts.tiny);
            const RECT rect = {ctx.body.right - static_cast<int>(w) - Sc(4, dpi), y - Sc(2, dpi),
                               ctx.body.right, y + Sc(16, dpi)};
            Add(ctx, Act::OpenReport, 0, 0, rect);
        }
        y += Sc(20, dpi);
    }

    if (!ctx.elevated && result.valid) {
        const bool hot = Hot(ctx, Act::Elevate, 0);
        gfx::DrawText(g, L"Часть системных папок закрыта — перезапустить с правами", *ctx.fonts.tiny,
                      hot ? P().accent : P().textFaint, left, static_cast<float>(y));
        const RECT rect = RowRect(ctx, y - Sc(2, dpi), Sc(18, dpi));
        Add(ctx, Act::Elevate, 0, 0, rect);
        y += Sc(20, dpi);
    }
}

// ---------- Диск ----------

void PaintDisk(PanelCtx& ctx, int& y) {
    Graphics& g = *ctx.g;
    const AppData& data = *ctx.data;
    const int dpi = ctx.dpi;
    const float left = static_cast<float>(ctx.body.left);
    const float right = static_cast<float>(ctx.body.right);
    const float width = right - left;

    const std::vector<scan::Drive>& drives = *ctx.drives;
    const scan::Drive* current = nullptr;
    for (const scan::Drive& drive : drives) {
        if (towupper(drive.letter) == towupper(data.drive)) current = &drive;
    }
    if (!current && !drives.empty()) current = &drives[0];

    if (!current) {
        Empty(g, ctx, y, L"Не вижу ни одного жёсткого диска");
        return;
    }

    // --- Кольцо ---
    if (ctx.panel->showChart) {
        const float radius = ScF(52.0f, dpi);
        const float thickness = ScF(12.0f, dpi);
        // Перо рисует дугу по центру контура, то есть кольцо выпирает за радиус на
        // полтолщины: без этого запаса верхушку срезает верхний край блока.
        const float outer = radius + thickness / 2.0f + ScF(2.0f, dpi);
        const float cx = left + width / 2.0f;
        const float cy = static_cast<float>(y) + outer;
        const float fraction = current->total > 0
                                   ? static_cast<float>(static_cast<double>(current->Used()) /
                                                        static_cast<double>(current->total))
                                   : 0.0f;
        // Красным кольцо становится только когда места действительно мало.
        const ARGB ringColor = fraction > 0.92f ? P().danger : (fraction > 0.8f ? SliceColor(3) : P().accent);
        gfx::Ring(g, cx, cy, radius, thickness, fraction, P().track, ringColor);

        const std::wstring number = SizeNumber(current->freeSpace);
        const float numberW = gfx::TextWidth(g, number.c_str(), *ctx.fonts.big);
        const float unitW = gfx::TextWidth(g, SizeUnit(current->freeSpace).c_str(), *ctx.fonts.small);
        const float totalW = numberW + unitW + ScF(4.0f, dpi);
        gfx::DrawText(g, number.c_str(), *ctx.fonts.big, P().textPrimary, cx - totalW / 2.0f,
                      cy - ScF(20.0f, dpi));
        gfx::DrawText(g, SizeUnit(current->freeSpace).c_str(), *ctx.fonts.small, P().textMuted,
                      cx - totalW / 2.0f + numberW + ScF(4.0f, dpi), cy - ScF(8.0f, dpi));
        gfx::DrawText(g, L"свободно", *ctx.fonts.tiny, P().textFaint, cx, cy + ScF(8.0f, dpi),
                      gfx::Align::Center);
        y += static_cast<int>(outer * 2.0f) + Sc(12, dpi);
    }

    // --- Строки дисков ---
    for (size_t i = 0; i < drives.size(); i++) {
        const scan::Drive& drive = drives[i];
        const bool chosen = towupper(drive.letter) == towupper(data.drive);
        const int rowH = Sc(40, dpi);
        const RECT row = RowRect(ctx, y, rowH);
        const bool hot = Hot(ctx, Act::PickDrive, static_cast<int>(i));
        RowHighlight(g, ctx, row, hot);
        Add(ctx, Act::PickDrive, static_cast<int>(i), 0, row);

        wchar_t letter[8];
        swprintf(letter, 8, L"%lc:", drive.letter);
        gfx::DrawText(g, letter, *ctx.fonts.num, chosen ? P().accent : P().textPrimary, left,
                      static_cast<float>(y) + ScF(3.0f, dpi));

        const float nameX = left + ScF(28.0f, dpi);
        const std::wstring label = gfx::Ellipsize(g, drive.label, *ctx.fonts.small, width * 0.45f);
        gfx::DrawText(g, label.c_str(), *ctx.fonts.small, chosen ? P().textPrimary : P().textMuted, nameX,
                      static_cast<float>(y) + ScF(4.0f, dpi));

        wchar_t sizes[96];
        swprintf(sizes, 96, L"%ls из %ls", FormatSize(drive.freeSpace).c_str(),
                 FormatSize(drive.total).c_str());
        gfx::DrawText(g, sizes, *ctx.fonts.tiny, P().textFaint, right, static_cast<float>(y) + ScF(5.0f, dpi),
                      gfx::Align::Right);

        const float fraction = drive.total > 0
                                   ? static_cast<float>(static_cast<double>(drive.Used()) /
                                                        static_cast<double>(drive.total))
                                   : 0.0f;
        Bar(g, ctx, left, static_cast<float>(y) + ScF(24.0f, dpi), width, fraction,
            chosen ? P().accent : P().textFaint);
        y += rowH;
    }
    ctx.scrollable = true;
}

// ---------- Куда ушло место ----------

void PaintBreakdown(PanelCtx& ctx, int& y) {
    Graphics& g = *ctx.g;
    const scan::Result& result = *ctx.result;
    const int dpi = ctx.dpi;
    const float left = static_cast<float>(ctx.body.left);
    const float right = static_cast<float>(ctx.body.right);
    const float width = right - left;

    if (!result.valid || result.slices.empty()) {
        Empty(g, ctx, y, L"Нажмите «Просканировать» в блоке «Пылесос»");
        return;
    }

    // --- Кольцо из долей ---
    if (ctx.panel->showChart) {
        const float radius = ScF(54.0f, dpi);
        const float thickness = ScF(16.0f, dpi);
        // Дуга рисуется по центру контура и выпирает за радиус на полтолщины, а доля под
        // курсором становится ещё толще — закладываем весь этот запас, иначе кольцо
        // срезает верхним краем блока.
        const float outer = radius + thickness / 2.0f + ScF(4.0f, dpi);
        const float cx = left + width / 2.0f;
        const float cy = static_cast<float>(y) + outer;
        const RectF box(cx - radius, cy - radius, radius * 2, radius * 2);

        Pen track(Color(P().track), thickness);
        g.DrawEllipse(&track, box);

        float start = -90.0f;  // от 12 часов по часовой
        for (size_t i = 0; i < result.slices.size(); i++) {
            const scan::Slice& slice = result.slices[i];
            float sweep = result.used > 0 ? 360.0f * static_cast<float>(static_cast<double>(slice.size) /
                                                                       static_cast<double>(result.used))
                                          : 0.0f;
            if (sweep <= 0.2f) continue;
            const bool hot = Hot(ctx, Act::OpenRow, static_cast<int>(i), static_cast<int>(ListKind::Slice));
            // Между долями оставляем зазор — иначе соседние цвета сливаются в кашу.
            const float gap = sweep > 6.0f ? 1.6f : 0.0f;
            Pen pen(Color(SliceColor(slice.tone)), hot ? thickness + ScF(3.0f, dpi) : thickness);
            g.DrawArc(&pen, box, start + gap / 2.0f, sweep - gap);
            start += sweep;
        }

        const std::wstring number = SizeNumber(result.used);
        const float numberW = gfx::TextWidth(g, number.c_str(), *ctx.fonts.big);
        const float unitW = gfx::TextWidth(g, SizeUnit(result.used).c_str(), *ctx.fonts.small);
        const float totalW = numberW + unitW + ScF(4.0f, dpi);
        gfx::DrawText(g, number.c_str(), *ctx.fonts.big, P().textPrimary, cx - totalW / 2.0f,
                      cy - ScF(20.0f, dpi));
        gfx::DrawText(g, SizeUnit(result.used).c_str(), *ctx.fonts.small, P().textMuted,
                      cx - totalW / 2.0f + numberW + ScF(4.0f, dpi), cy - ScF(8.0f, dpi));
        gfx::DrawText(g, L"занято", *ctx.fonts.tiny, P().textFaint, cx, cy + ScF(8.0f, dpi),
                      gfx::Align::Center);
        y += static_cast<int>(outer * 2.0f) + Sc(10, dpi);
    }

    // --- Легенда ---
    for (size_t i = 0; i < result.slices.size(); i++) {
        const scan::Slice& slice = result.slices[i];
        const bool hasHint = ctx.panel->showHints && !slice.hint.empty();
        const bool open = ctx.panel->expanded == static_cast<int>(i) && !slice.parts.empty();
        const int rowH = Sc(hasHint ? 38 : 26, dpi);
        const RECT row = RowRect(ctx, y, rowH);
        const bool hot = Hot(ctx, Act::ExpandSlice, static_cast<int>(i));
        RowHighlight(g, ctx, row, hot);
        Add(ctx, Act::ExpandSlice, static_cast<int>(i), static_cast<int>(ListKind::Slice), row);

        const float dot = ScF(8.0f, dpi);
        gfx::FillRoundRect(g, RectF(left, static_cast<float>(y) + ScF(5.0f, dpi), dot, dot), dot / 2.0f,
                           SliceColor(slice.tone));

        const float textX = left + dot + ScF(9.0f, dpi);
        wchar_t percent[16];
        swprintf(percent, 16, L"%d%%", PercentOf(slice.size, result.used));
        const float percentW = gfx::TextWidth(g, percent, *ctx.fonts.tiny);
        const float sizeW = gfx::TextWidth(g, FormatSize(slice.size).c_str(), *ctx.fonts.num);

        const std::wstring name =
            gfx::Ellipsize(g, slice.name, *ctx.fonts.row,
                           right - textX - sizeW - percentW - ScF(18.0f, dpi));
        gfx::DrawText(g, name.c_str(), *ctx.fonts.row, P().textPrimary, textX,
                      static_cast<float>(y) + ScF(1.0f, dpi));
        DrawSizeRight(g, ctx, slice.size, right, static_cast<float>(y) + ScF(1.0f, dpi), P().textPrimary);
        gfx::DrawText(g, percent, *ctx.fonts.tiny, P().textFaint, right - sizeW - ScF(8.0f, dpi),
                      static_cast<float>(y) + ScF(3.0f, dpi), gfx::Align::Right);

        if (hasHint) {
            const std::wstring hint = gfx::Ellipsize(g, slice.hint, *ctx.fonts.tiny, right - textX);
            gfx::DrawText(g, hint.c_str(), *ctx.fonts.tiny, P().textFaint, textX,
                          static_cast<float>(y) + ScF(19.0f, dpi));
        }

        // Треугольник у названия: по этой строке можно нажать и увидеть, из чего доля.
        if (!slice.parts.empty()) {
            const float nameW = gfx::TextWidth(g, name.c_str(), *ctx.fonts.row);
            const float cx = textX + nameW + ScF(9.0f, dpi);
            const float cy = static_cast<float>(y) + ScF(9.0f, dpi);
            const float s = ScF(3.2f, dpi);
            SolidBrush brush(Color(hot || open ? P().accent : P().textFaint));
            PointF tri[3];
            if (open) {
                tri[0] = PointF(cx - s, cy - s * 0.6f);
                tri[1] = PointF(cx + s, cy - s * 0.6f);
                tri[2] = PointF(cx, cy + s * 0.8f);
            } else {
                tri[0] = PointF(cx - s * 0.6f, cy - s);
                tri[1] = PointF(cx + s * 0.8f, cy);
                tri[2] = PointF(cx - s * 0.6f, cy + s);
            }
            g.FillPolygon(&brush, tri, 3);
        }
        y += rowH;

        if (!open) continue;

        // Состав доли: те же строки, но со сдвигом и помельче. По клику открывается папка.
        for (size_t p = 0; p < slice.parts.size(); p++) {
            const scan::FolderItem& part = slice.parts[p];
            const int partH = Sc(24, dpi);
            const int packed = PackPart(static_cast<int>(i), static_cast<int>(p));
            const RECT partRow = {ctx.body.left + Sc(14, dpi), y, ctx.body.right, y + partH};
            const bool partHot = Hot(ctx, Act::OpenRow, packed, static_cast<int>(ListKind::SlicePart));
            RowHighlight(g, ctx, partRow, partHot);
            Add(ctx, Act::OpenRow, packed, static_cast<int>(ListKind::SlicePart), partRow);

            const float partX = static_cast<float>(partRow.left);
            const float partSizeW = gfx::TextWidth(g, FormatSize(part.size).c_str(), *ctx.fonts.small);
            const std::wstring partName =
                gfx::Ellipsize(g, part.name, *ctx.fonts.small, right - partX - partSizeW - ScF(12.0f, dpi));
            gfx::DrawText(g, partName.c_str(), *ctx.fonts.small,
                          partHot ? P().textPrimary : P().textMuted, partX,
                          static_cast<float>(y) + ScF(3.0f, dpi));
            gfx::DrawText(g, FormatSize(part.size).c_str(), *ctx.fonts.small, P().textMuted, right,
                          static_cast<float>(y) + ScF(3.0f, dpi), gfx::Align::Right);
            y += partH;
        }
        y += Sc(4, dpi);
    }
    ctx.scrollable = true;
}

// ---------- Списки файлов и папок ----------

// Одна строка «что-то большое»: название, путь, размер и столбик.
void FileRow(PanelCtx& ctx, int& y, int index, ListKind kind, const std::wstring& title,
             const std::wstring& subtitle, unsigned long long size, unsigned long long maxSize,
             ARGB barColor) {
    Graphics& g = *ctx.g;
    const int dpi = ctx.dpi;
    const float left = static_cast<float>(ctx.body.left);
    const float right = static_cast<float>(ctx.body.right);

    const bool hasSub = ctx.panel->showHints && !subtitle.empty();
    const int rowH = Sc(hasSub ? 46 : 32, dpi);
    const RECT row = RowRect(ctx, y, rowH);
    const bool hot = Hot(ctx, Act::OpenRow, index, static_cast<int>(kind));
    RowHighlight(g, ctx, row, hot);
    Add(ctx, Act::OpenRow, index, static_cast<int>(kind), row);

    const float sizeW = gfx::TextWidth(g, FormatSize(size).c_str(), *ctx.fonts.num);
    const std::wstring name = gfx::Ellipsize(g, title, *ctx.fonts.row, right - left - sizeW - ScF(12.0f, dpi));
    gfx::DrawText(g, name.c_str(), *ctx.fonts.row, P().textPrimary, left,
                  static_cast<float>(y) + ScF(1.0f, dpi));
    DrawSizeRight(g, ctx, size, right, static_cast<float>(y) + ScF(1.0f, dpi), P().accent);

    if (hasSub) {
        const std::wstring sub = ShortPath(g, subtitle, *ctx.fonts.tiny, right - left);
        gfx::DrawText(g, sub.c_str(), *ctx.fonts.tiny, P().textFaint, left,
                      static_cast<float>(y) + ScF(18.0f, dpi));
    }
    if (ctx.panel->showChart && maxSize > 0) {
        const float fraction = static_cast<float>(static_cast<double>(size) / static_cast<double>(maxSize));
        Bar(g, ctx, left, static_cast<float>(y) + ScF(hasSub ? 34.0f : 21.0f, dpi), right - left, fraction,
            barColor);
    }
    y += rowH;
}

void PaintFolders(PanelCtx& ctx, int& y) {
    const scan::Result& result = *ctx.result;
    if (!result.valid || result.folders.empty()) {
        Empty(*ctx.g, ctx, y, result.valid ? L"Крупных папок не нашлось"
                                           : L"Нажмите «Просканировать» в блоке «Пылесос»");
        return;
    }
    const unsigned long long top = result.folders[0].size;
    for (size_t i = 0; i < result.folders.size(); i++) {
        const scan::FolderItem& folder = result.folders[i];
        wchar_t files[64];
        swprintf(files, 64, L"%d %ls", folder.files, PluralRu(folder.files, L"файл", L"файла", L"файлов"));
        FileRow(ctx, y, static_cast<int>(i), ListKind::Folder, folder.name, files, folder.size, top,
                SliceColor(static_cast<int>(i) % 6));
    }
    ctx.scrollable = true;
}

void PaintBigFiles(PanelCtx& ctx, int& y) {
    const scan::Result& result = *ctx.result;
    if (!result.valid || result.bigFiles.empty()) {
        Empty(*ctx.g, ctx, y,
              result.valid ? L"Файлов такого размера нет" : L"Нажмите «Просканировать» в блоке «Пылесос»");
        return;
    }
    const unsigned long long top = result.bigFiles[0].size;
    for (size_t i = 0; i < result.bigFiles.size(); i++) {
        const scan::FileItem& file = result.bigFiles[i];
        FileRow(ctx, y, static_cast<int>(i), ListKind::BigFile, FileName(file.path), ParentDir(file.path),
                file.size, top, P().accent);
    }
    ctx.scrollable = true;
}

void PaintOldFiles(PanelCtx& ctx, int& y) {
    const scan::Result& result = *ctx.result;
    if (!result.valid || result.oldFiles.empty()) {
        Empty(*ctx.g, ctx, y,
              result.valid ? L"Старых больших файлов не нашлось" : L"Сначала просканируйте диск");
        return;
    }
    const unsigned long long top = result.oldFiles[0].size;
    for (size_t i = 0; i < result.oldFiles.size(); i++) {
        const scan::FileItem& file = result.oldFiles[i];
        const std::wstring sub = AgeText(file.days) + L"  ·  " + ParentDir(file.path);
        FileRow(ctx, y, static_cast<int>(i), ListKind::OldFile, FileName(file.path), sub, file.size, top,
                SliceColor(4));
    }
    ctx.scrollable = true;
}

void PaintOrphans(PanelCtx& ctx, int& y) {
    Graphics& g = *ctx.g;
    const scan::Result& result = *ctx.result;
    if (!result.valid) {
        Empty(g, ctx, y, L"Сначала просканируйте диск");
        return;
    }
    if (result.orphans.empty()) {
        Empty(g, ctx, y, L"Хвостов не нашлось — папки принадлежат установленным программам");
        return;
    }
    // Предупреждение сверху: удалять такие папки всё-таки на свой страх.
    gfx::DrawText(g, L"Проверьте глазами: совпадение имён — только догадка", *ctx.fonts.tiny, P().textFaint,
                  static_cast<float>(ctx.body.left), static_cast<float>(y));
    y += Sc(18, ctx.dpi);

    const unsigned long long top = result.orphans[0].size;
    for (size_t i = 0; i < result.orphans.size(); i++) {
        const scan::OrphanItem& item = result.orphans[i];
        FileRow(ctx, y, static_cast<int>(i), ListKind::Orphan, item.name, item.path, item.size, top,
                SliceColor(2));
    }
    ctx.scrollable = true;
}

void PaintSystem(PanelCtx& ctx, int& y) {
    Graphics& g = *ctx.g;
    const scan::Result& result = *ctx.result;
    if (!result.valid || result.system.empty()) {
        Empty(g, ctx, y, L"Сначала просканируйте диск");
        return;
    }
    const int dpi = ctx.dpi;
    const float left = static_cast<float>(ctx.body.left);
    const float right = static_cast<float>(ctx.body.right);

    gfx::DrawText(g, L"Это место занимает Windows. Удалять вручную нельзя — вот чем управляется:",
                  *ctx.fonts.tiny, P().textFaint, left, static_cast<float>(y));
    y += Sc(30, dpi);

    for (size_t i = 0; i < result.system.size(); i++) {
        const scan::SysItem& item = result.system[i];
        const bool hasHint = ctx.panel->showHints && !item.hint.empty();
        const int rowH = Sc(hasHint ? 44 : 28, dpi);
        const RECT row = RowRect(ctx, y, rowH);
        const bool hot = Hot(ctx, Act::OpenRow, static_cast<int>(i), static_cast<int>(ListKind::System));
        RowHighlight(g, ctx, row, hot);
        Add(ctx, Act::OpenRow, static_cast<int>(i), static_cast<int>(ListKind::System), row);

        const float sizeW = gfx::TextWidth(g, FormatSize(item.size).c_str(), *ctx.fonts.num);
        const std::wstring name =
            gfx::Ellipsize(g, item.name, *ctx.fonts.row, right - left - sizeW - ScF(12.0f, dpi));
        gfx::DrawText(g, name.c_str(), *ctx.fonts.row, P().textPrimary, left,
                      static_cast<float>(y) + ScF(1.0f, dpi));
        DrawSizeRight(g, ctx, item.size, right, static_cast<float>(y) + ScF(1.0f, dpi), P().textMuted);
        if (hasHint) {
            const std::wstring hint = gfx::Ellipsize(g, item.hint, *ctx.fonts.tiny, right - left);
            gfx::DrawText(g, hint.c_str(), *ctx.fonts.tiny, P().textFaint, left,
                          static_cast<float>(y) + ScF(18.0f, dpi));
        }
        y += rowH;
    }
    ctx.scrollable = true;
}

// ---------- Мусор ----------

void PaintJunk(PanelCtx& ctx, int& y) {
    Graphics& g = *ctx.g;
    const AppData& data = *ctx.data;
    const scan::Result& result = *ctx.result;
    const int dpi = ctx.dpi;
    const float left = static_cast<float>(ctx.body.left);
    const float right = static_cast<float>(ctx.body.right);

    if (!result.valid || result.junk.empty()) {
        Empty(g, ctx, y,
              result.valid ? L"Мусора не нашлось — диск чистый" : L"Нажмите «Просканировать» в блоке «Пылесос»");
        return;
    }

    // Верхняя строка: сколько отмечено и быстрые «всё / ничего».
    {
        const unsigned long long chosen = result.JunkTotal(data.checkedJunk);
        wchar_t summary[96];
        swprintf(summary, 96, L"Отмечено: %ls", FormatSize(chosen).c_str());
        gfx::DrawText(g, summary, *ctx.fonts.small, chosen > 0 ? P().accent : P().textMuted, left,
                      static_cast<float>(y));

        const bool hotAll = Hot(ctx, Act::JunkAll, 1);
        const bool hotNone = Hot(ctx, Act::JunkAll, 0);
        const float noneW = gfx::TextWidth(g, L"снять", *ctx.fonts.tiny);
        gfx::DrawText(g, L"снять", *ctx.fonts.tiny, hotNone ? P().accent : P().textFaint, right,
                      static_cast<float>(y) + ScF(2.0f, dpi), gfx::Align::Right);
        gfx::DrawText(g, L"всё", *ctx.fonts.tiny, hotAll ? P().accent : P().textFaint,
                      right - noneW - ScF(12.0f, dpi), static_cast<float>(y) + ScF(2.0f, dpi),
                      gfx::Align::Right);
        const int rowTop = y - Sc(3, dpi);
        const int rowBottom = y + Sc(17, dpi);
        const float allW = gfx::TextWidth(g, L"всё", *ctx.fonts.tiny);
        Add(ctx, Act::JunkAll, 0, 0,
            RECT{ctx.body.right - static_cast<int>(noneW) - Sc(4, dpi), rowTop, ctx.body.right, rowBottom});
        Add(ctx, Act::JunkAll, 1, 0,
            RECT{ctx.body.right - static_cast<int>(noneW + allW) - Sc(18, dpi), rowTop,
                 ctx.body.right - static_cast<int>(noneW) - Sc(10, dpi), rowBottom});
        y += Sc(24, dpi);
    }

    for (size_t i = 0; i < result.junk.size(); i++) {
        const scan::JunkItem& item = result.junk[i];
        const bool checked = data.JunkChecked(item.id);
        const bool hasHint = ctx.panel->showHints && !item.hint.empty();
        const int rowH = Sc(hasHint ? 44 : 30, dpi);
        const RECT row = RowRect(ctx, y, rowH);
        const bool hot = Hot(ctx, Act::ToggleJunk, item.id);
        RowHighlight(g, ctx, row, hot);
        Add(ctx, Act::ToggleJunk, item.id, static_cast<int>(ListKind::Junk), row);

        const int boxSize = Sc(17, dpi);
        const RECT box = {ctx.body.left, y + Sc(2, dpi), ctx.body.left + boxSize, y + Sc(2, dpi) + boxSize};
        Checkbox(g, ctx, box, checked, hot);

        const float textX = left + boxSize + ScF(10.0f, dpi);
        const float sizeW = gfx::TextWidth(g, FormatSize(item.size).c_str(), *ctx.fonts.num);
        const std::wstring name =
            gfx::Ellipsize(g, item.name, *ctx.fonts.row, right - textX - sizeW - ScF(12.0f, dpi));
        gfx::DrawText(g, name.c_str(), *ctx.fonts.row, checked ? P().textPrimary : P().textMuted, textX,
                      static_cast<float>(y) + ScF(2.0f, dpi));
        DrawSizeRight(g, ctx, item.size, right, static_cast<float>(y) + ScF(2.0f, dpi),
                      checked ? P().accent : P().textMuted);

        if (hasHint) {
            std::wstring hint = item.hint;
            if (item.userData) hint = L"файлы пользователя — " + hint;
            else if (item.needsAdmin) hint = L"нужны права администратора — " + hint;
            const std::wstring line = gfx::Ellipsize(g, hint, *ctx.fonts.tiny, right - textX);
            gfx::DrawText(g, line.c_str(), *ctx.fonts.tiny,
                          item.userData ? SliceColor(3) : P().textFaint, textX,
                          static_cast<float>(y) + ScF(22.0f, dpi));
        }
        y += rowH;
    }

    // Кнопка уборки — в самом конце списка, после всех галочек.
    {
        y += Sc(6, dpi);
        const unsigned long long chosen = result.JunkTotal(data.checkedJunk);
        const bool busy = ctx.progress->busy;
        wchar_t label[96];
        if (busy) swprintf(label, 96, L"Работаю…");
        else if (chosen > 0) swprintf(label, 96, L"Пропылесосить — %ls", FormatSize(chosen).c_str());
        else swprintf(label, 96, L"Отметьте, что выносить");
        const int btnH = Sc(34, dpi);
        const RECT button = {ctx.body.left, y, ctx.body.right, y + btnH};
        Button(g, ctx, button, label, chosen > 0 && !busy, Hot(ctx, Act::CleanNow, 0), chosen == 0 || busy);
        if (chosen > 0 && !busy) Add(ctx, Act::CleanNow, 0, 0, button);
        y += btnH + Sc(4, dpi);
    }
    ctx.scrollable = true;
}

}  // namespace

void PaintPanel(PanelCtx& ctx) {
    int y = ctx.body.top;
    switch (ctx.panel->kind) {
        case PanelKind::Vacuum: PaintVacuum(ctx, y); break;
        case PanelKind::Disk: PaintDisk(ctx, y); break;
        case PanelKind::Breakdown: PaintBreakdown(ctx, y); break;
        case PanelKind::Folders: PaintFolders(ctx, y); break;
        case PanelKind::BigFiles: PaintBigFiles(ctx, y); break;
        case PanelKind::Junk: PaintJunk(ctx, y); break;
        case PanelKind::Old: PaintOldFiles(ctx, y); break;
        case PanelKind::Orphans: PaintOrphans(ctx, y); break;
        case PanelKind::System: PaintSystem(ctx, y); break;
        default: break;
    }
    ctx.contentH = y - ctx.body.top;
}
