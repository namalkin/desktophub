#include "dialog.h"

#include <windowsx.h>

#include <vector>

#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClassName[] = L"NewYearWidgetDialog";
constexpr int kFirstEditId = 2000;

// Логические размеры (96 DPI).
constexpr int kW = 340;
constexpr int kPad = 22;
constexpr int kFieldH = 38;
constexpr int kBtnH = 38;
constexpr int kRadius = 12;

// Геометрия календаря (логические единицы).
constexpr int kCalHeaderH = 36;
constexpr int kCalWeekH = 24;
constexpr int kCalRowH = 32;
constexpr int kCalRows = 6;
constexpr int kCalClearH = 38;
constexpr int kCalH = kCalHeaderH + kCalWeekH + kCalRowH * kCalRows + kCalClearH;

// Особые значения для «горячего» элемента календаря.
constexpr int kHotNone = -1;
constexpr int kHotPrev = -2;
constexpr int kHotNext = -3;
constexpr int kHotClear = -4;

const wchar_t* kMonthNames[] = {L"Январь", L"Февраль", L"Март",     L"Апрель",  L"Май",    L"Июнь",
                                L"Июль",   L"Август",  L"Сентябрь", L"Октябрь", L"Ноябрь", L"Декабрь"};
const wchar_t* kWeekDays[] = {L"Пн", L"Вт", L"Ср", L"Чт", L"Пт", L"Сб", L"Вс"};

int DaysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return kDays[month - 1];
}

// Номер столбца для 1-го числа: неделя начинается с понедельника.
int FirstColumn(int year, int month) {
    SYSTEMTIME st = {};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = 1;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft) || !FileTimeToSystemTime(&ft, &st)) return 0;
    return (st.wDayOfWeek + 6) % 7;  // wDayOfWeek: 0 — воскресенье
}

struct FieldUi {
    HWND edit = nullptr;
    int yLabel = 0;
    int yField = 0;
    int yHint = 0;
    bool hasHint = false;
    bool isCalendar = false;
};

struct DialogState {
    DialogSpec* spec = nullptr;
    bool accepted = false;
    int dpi = 96;
    HFONT font = nullptr;
    HBRUSH fieldBrush = nullptr;
    std::vector<FieldUi> ui;
    RECT okBtn{};
    RECT cancelBtn{};
    int hotBtn = 0;   // 0 — ничего, 1 — OK, 2 — отмена
    int errorAt = -1; // индекс поля с ошибкой
    int yButtons = 0;
    int height = 0;

    // Календарь (в диалоге он не больше одного).
    int calField = -1;
    int calYear = 0, calMonth = 0;      // показанный месяц
    bool calHasDate = false;            // false — «без даты»
    int selYear = 0, selMonth = 0, selDay = 0;
    RECT calPrev{}, calNext{}, calClear{};
    RECT calCells[42]{};
    int calDays[42]{};                  // 0 — пустая ячейка
    int calHot = kHotNone;
};

DialogState* StateOf(HWND hwnd) {
    return reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void ComputeLayout(DialogState& st) {
    const int dpi = st.dpi;
    st.ui.assign(st.spec->fields.size(), FieldUi{});

    int y = Sc(58, dpi);
    for (size_t i = 0; i < st.spec->fields.size(); i++) {
        FieldUi& f = st.ui[i];
        f.hasHint = !st.spec->fields[i].hint.empty();
        f.isCalendar = st.spec->fields[i].kind == DialogField::Kind::Calendar;
        f.yLabel = y;
        y += Sc(20, dpi);
        f.yField = y;
        y += f.isCalendar ? Sc(kCalH, dpi) : Sc(kFieldH, dpi);
        if (f.isCalendar) {
            st.calField = static_cast<int>(i);
            y += Sc(10, dpi);
            continue;
        }
        if (f.hasHint) {
            y += Sc(8, dpi);
            f.yHint = y;
            y += Sc(24, dpi);
        } else {
            y += Sc(14, dpi);
        }
    }
    y += Sc(8, dpi);
    st.yButtons = y;
    st.height = y + Sc(kBtnH + kPad, dpi);
}

// Пересчитывает прямоугольники календаря под текущий показанный месяц.
void LayoutCalendar(DialogState& st, int width) {
    if (st.calField < 0) return;
    const int dpi = st.dpi;
    const int pad = Sc(kPad, dpi);
    const int top = st.ui[static_cast<size_t>(st.calField)].yField;
    const int inner = width - pad * 2;

    const int arrow = Sc(28, dpi);
    const int headerTop = top + (Sc(kCalHeaderH, dpi) - arrow) / 2;
    st.calPrev = {pad, headerTop, pad + arrow, headerTop + arrow};
    st.calNext = {pad + inner - arrow, headerTop, pad + inner, headerTop + arrow};

    const int gridTop = top + Sc(kCalHeaderH + kCalWeekH, dpi);
    const int cellW = inner / 7;
    const int cellH = Sc(kCalRowH, dpi);
    const int gridLeft = pad + (inner - cellW * 7) / 2;

    const int first = FirstColumn(st.calYear, st.calMonth);
    const int days = DaysInMonth(st.calYear, st.calMonth);
    for (int i = 0; i < 42; i++) {
        const int day = i - first + 1;
        st.calDays[i] = (day >= 1 && day <= days) ? day : 0;
        const int row = i / 7, col = i % 7;
        st.calCells[i] = {gridLeft + col * cellW, gridTop + row * cellH, gridLeft + (col + 1) * cellW,
                          gridTop + (row + 1) * cellH};
    }

    const int clearTop = gridTop + cellH * kCalRows + Sc(4, dpi);
    st.calClear = {pad, clearTop, pad + inner, clearTop + Sc(kCalClearH, dpi) - Sc(8, dpi)};
}

const wchar_t* ErrorFor(DialogField::Kind kind) {
    switch (kind) {
        case DialogField::Kind::Amount: return L"Введите сумму, например -2000 или +5000";
        case DialogField::Kind::AmountPositive: return L"Введите сумму, например 50000";
        case DialogField::Kind::Date: return L"Формат даты: ГГГГ-ММ-ДД, например 2027-01-01";
        default: return L"Заполните поле";
    }
}

void PaintCalendar(Graphics& g, DialogState& st, int width, const Font& fontLabel, const Font& fontDay,
                   const Font& fontMonth) {
    const int dpi = st.dpi;
    const int pad = Sc(kPad, dpi);
    const int inner = width - pad * 2;
    const int top = st.ui[static_cast<size_t>(st.calField)].yField;

    // Заголовок: ‹ Сентябрь 2026 ›
    auto arrowButton = [&](const RECT& r, bool left, bool hot) {
        RectF rf(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
                 static_cast<float>(r.bottom - r.top));
        if (hot) gfx::FillRoundRect(g, rf, rf.Width / 2.0f, P().buttonBgHover);
        const float cx = rf.X + rf.Width / 2.0f;
        const float cy = rf.Y + rf.Height / 2.0f;
        const float u = ScF(1.0f, dpi);
        Color c(P().textPrimary);
        Pen pen(c, ScF(1.6f, dpi));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        const float dir = left ? -1.0f : 1.0f;
        g.DrawLine(&pen, cx - dir * 2 * u, cy - 4 * u, cx + dir * 2 * u, cy);
        g.DrawLine(&pen, cx + dir * 2 * u, cy, cx - dir * 2 * u, cy + 4 * u);
    };
    arrowButton(st.calPrev, true, st.calHot == kHotPrev);
    arrowButton(st.calNext, false, st.calHot == kHotNext);

    wchar_t header[64];
    // %ls, а не %s: в широком printf по стандарту C именно %ls означает wchar_t*.
    swprintf(header, 64, L"%ls %d", kMonthNames[st.calMonth - 1], st.calYear);
    gfx::DrawText(g, header, fontMonth, P().textPrimary, static_cast<float>(pad + inner / 2),
                  static_cast<float>(top + Sc(9, dpi)), gfx::Align::Center);

    // Дни недели
    for (int i = 0; i < 7; i++) {
        const RECT& cell = st.calCells[i];
        const float cx = (cell.left + cell.right) / 2.0f;
        gfx::DrawText(g, kWeekDays[i], fontLabel, i >= 5 ? P().textFaint : P().textMuted, cx,
                      static_cast<float>(top + Sc(kCalHeaderH + 4, dpi)), gfx::Align::Center);
    }

    SYSTEMTIME now;
    GetLocalTime(&now);

    for (int i = 0; i < 42; i++) {
        if (st.calDays[i] == 0) continue;
        const RECT& cell = st.calCells[i];
        const int day = st.calDays[i];
        const bool selected = st.calHasDate && st.selYear == st.calYear && st.selMonth == st.calMonth &&
                              st.selDay == day;
        const bool today = now.wYear == st.calYear && now.wMonth == st.calMonth && now.wDay == day;
        const bool hot = st.calHot == i;

        const float size = static_cast<float>(min(cell.right - cell.left, cell.bottom - cell.top)) -
                           ScF(4.0f, dpi);
        RectF box((cell.left + cell.right) / 2.0f - size / 2.0f, (cell.top + cell.bottom) / 2.0f - size / 2.0f,
                  size, size);

        if (selected) {
            gfx::FillRoundRect(g, box, size / 2.0f, P().accent);
        } else if (hot) {
            gfx::FillRoundRect(g, box, size / 2.0f, P().buttonBgHover);
        } else if (today) {
            gfx::StrokeRoundRect(g, box, size / 2.0f, P().accent, ScF(1.0f, dpi));
        }

        wchar_t text[8];
        swprintf(text, 8, L"%d", day);
        ARGB color = P().textPrimary;
        if (selected) color = P().onAccent;
        else if (i % 7 >= 5) color = P().textMuted;  // выходные приглушены
        gfx::DrawText(g, text, fontDay, color, box.X + box.Width / 2.0f,
                      box.Y + (box.Height - ScF(15.0f, dpi)) / 2.0f, gfx::Align::Center);
    }

    // «Без даты» — режим ближайшего Нового года.
    RectF clear(static_cast<float>(st.calClear.left), static_cast<float>(st.calClear.top),
                static_cast<float>(st.calClear.right - st.calClear.left),
                static_cast<float>(st.calClear.bottom - st.calClear.top));
    const bool clearActive = !st.calHasDate;
    gfx::FillRoundRect(g, clear, ScF(8.0f, dpi),
                       clearActive ? P().accent
                                   : (st.calHot == kHotClear ? P().buttonBgHover : P().buttonBg));
    gfx::DrawText(g, L"Без даты — до Нового года", fontLabel,
                  clearActive ? P().onAccent : P().textMuted, clear.X + clear.Width / 2.0f,
                  clear.Y + (clear.Height - ScF(14.0f, dpi)) / 2.0f, gfx::Align::Center);
}

void PaintDialog(HWND hwnd, DialogState& st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;

    // Двойная буферизация — иначе поля мигают при перерисовке.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(P().dialogBg));

        const int dpi = st.dpi;
        const float pad = static_cast<float>(Sc(kPad, dpi));
        FontFamily family(L"Segoe UI");
        Font fontTitle(&family, ScF(15.0f, dpi), FontStyleBold, UnitPixel);
        Font fontLabel(&family, ScF(11.0f, dpi), FontStyleRegular, UnitPixel);
        Font fontBtn(&family, ScF(13.0f, dpi), FontStyleBold, UnitPixel);

        gfx::DrawText(g, st.spec->title.c_str(), fontTitle, P().textPrimary, pad,
                      static_cast<float>(Sc(22, dpi)));

        Font fontDay(&family, ScF(14.0f, dpi), FontStyleRegular, UnitPixel);
        Font fontMonth(&family, ScF(14.0f, dpi), FontStyleBold, UnitPixel);

        for (size_t i = 0; i < st.spec->fields.size(); i++) {
            const DialogField& field = st.spec->fields[i];
            const FieldUi& f = st.ui[i];
            const bool bad = (st.errorAt == static_cast<int>(i));

            gfx::DrawText(g, field.label.c_str(), fontLabel, P().textMuted, pad,
                          static_cast<float>(f.yLabel));

            if (f.isCalendar) {
                PaintCalendar(g, st, w, fontLabel, fontDay, fontMonth);
                continue;
            }

            RectF box(pad, static_cast<float>(f.yField), w - pad * 2, static_cast<float>(Sc(kFieldH, dpi)));
            gfx::FillRoundRect(g, box, ScF(8.0f, dpi), P().fieldBg);
            gfx::StrokeRoundRect(g, box, ScF(8.0f, dpi), bad ? P().debt : P().fieldBorder, ScF(1.0f, dpi));

            if (bad) {
                gfx::DrawText(g, ErrorFor(field.kind), fontLabel, P().debt, pad,
                              static_cast<float>(f.hasHint ? f.yHint : f.yField + Sc(kFieldH + 4, dpi)));
            } else if (f.hasHint) {
                gfx::DrawText(g, field.hint.c_str(), fontLabel, P().textFaint, pad,
                              static_cast<float>(f.yHint));
            }
        }

        auto drawButton = [&](const RECT& r, const wchar_t* text, bool primary, bool hot) {
            RectF rf(static_cast<float>(r.left), static_cast<float>(r.top),
                     static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
            ARGB bg;
            if (primary) {
                bg = P().accent;
                if (hot) bg = (bg & 0x00FFFFFF) | 0xD0000000;  // при наведении чуть светлее
            } else {
                bg = hot ? P().buttonBgHover : P().buttonBg;
            }
            gfx::FillRoundRect(g, rf, ScF(8.0f, dpi), bg);
            gfx::DrawText(g, text, fontBtn, primary ? P().onAccent : P().textPrimary,
                          rf.X + rf.Width / 2.0f, rf.Y + (rf.Height - ScF(16.0f, dpi)) / 2.0f,
                          gfx::Align::Center);
        };
        drawButton(st.cancelBtn, L"Отмена", false, st.hotBtn == 2);
        drawButton(st.okBtn, st.spec->okLabel.c_str(), true, st.hotBtn == 1);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

bool ValidateField(const DialogField& field, const std::wstring& text) {
    long long cents = 0;
    switch (field.kind) {
        case DialogField::Kind::Amount:
            return ParseAmount(text, cents) && cents != 0;
        case DialogField::Kind::AmountPositive:
            return ParseAmount(text, cents) && cents != 0;
        case DialogField::Kind::Date: {
            if (text.empty()) return true;  // пусто = ближайшее 1 января
            SYSTEMTIME st;
            return ParseIsoDate(text, st);
        }
        case DialogField::Kind::Text:
        default:
            return true;
    }
}

void Accept(HWND hwnd, DialogState& st) {
    std::vector<std::wstring> values(st.spec->fields.size());
    for (size_t i = 0; i < st.spec->fields.size(); i++) {
        if (st.ui[i].isCalendar) {
            if (st.calHasDate) {
                wchar_t iso[16];
                swprintf(iso, 16, L"%04d-%02d-%02d", st.selYear, st.selMonth, st.selDay);
                values[i].assign(iso);
            }
            continue;  // календарь не может дать неверное значение
        }
        wchar_t buf[512] = {0};
        GetWindowTextW(st.ui[i].edit, buf, 512);
        values[i].assign(buf);

        if (!ValidateField(st.spec->fields[i], values[i])) {
            st.errorAt = static_cast<int>(i);
            InvalidateRect(hwnd, nullptr, FALSE);
            SetFocus(st.ui[i].edit);
            SendMessageW(st.ui[i].edit, EM_SETSEL, 0, -1);
            return;
        }
    }
    for (size_t i = 0; i < st.spec->fields.size(); i++) {
        st.spec->fields[i].value = values[i];
    }
    st.accepted = true;
    DestroyWindow(hwnd);
}

int HitButton(DialogState& st, POINT pt) {
    if (PtInRect(&st.okBtn, pt)) return 1;
    if (PtInRect(&st.cancelBtn, pt)) return 2;
    return 0;
}

int HitCalendar(DialogState& st, POINT pt) {
    if (st.calField < 0) return kHotNone;
    if (PtInRect(&st.calPrev, pt)) return kHotPrev;
    if (PtInRect(&st.calNext, pt)) return kHotNext;
    if (PtInRect(&st.calClear, pt)) return kHotClear;
    for (int i = 0; i < 42; i++) {
        if (st.calDays[i] != 0 && PtInRect(&st.calCells[i], pt)) return i;
    }
    return kHotNone;
}

// Возвращает true, если календарь обработал клик и нужна перерисовка.
bool ClickCalendar(DialogState& st, POINT pt, int width) {
    const int hit = HitCalendar(st, pt);
    switch (hit) {
        case kHotNone:
            return false;
        case kHotPrev:
            if (--st.calMonth < 1) { st.calMonth = 12; st.calYear--; }
            break;
        case kHotNext:
            if (++st.calMonth > 12) { st.calMonth = 1; st.calYear++; }
            break;
        case kHotClear:
            st.calHasDate = false;
            break;
        default:
            st.calHasDate = true;
            st.selYear = st.calYear;
            st.selMonth = st.calMonth;
            st.selDay = st.calDays[hit];
            break;
    }
    LayoutCalendar(st, width);
    return true;
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DialogState* st = StateOf(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;  // не return: DefWindowProc должен доделать создание, включая заголовок окна
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (st) PaintDialog(hwnd, *st);
            return 0;
        case WM_CTLCOLOREDIT: {
            if (!st) break;
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, P().editText);
            SetBkColor(dc, P().editBack);
            return reinterpret_cast<LRESULT>(st->fieldBrush);
        }
        case WM_MOUSEMOVE: {
            if (!st) break;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int hot = HitButton(*st, pt);
            const int calHot = HitCalendar(*st, pt);
            if (hot != st->hotBtn || calHot != st->calHot) {
                st->hotBtn = hot;
                st->calHot = calHot;
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (st && (st->hotBtn || st->calHot != kHotNone)) {
                st->hotBtn = 0;
                st->calHot = kHotNone;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (!st) break;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (ClickCalendar(*st, pt, rc.right)) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int hit = HitButton(*st, pt);
            if (hit == 1) Accept(hwnd, *st);
            else if (hit == 2) DestroyWindow(hwnd);
            return 0;
        }
        case WM_COMMAND:
            if (!st) break;
            if (LOWORD(wp) == IDOK) { Accept(hwnd, *st); return 0; }
            if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);  // выходим из локального модального цикла
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureClass(HINSTANCE inst) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

bool ShowDialog(HWND owner, HWND anchor, DialogSpec& spec) {
    if (spec.fields.empty()) return false;
    if (!anchor) anchor = owner;

    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!inst) inst = GetModuleHandleW(nullptr);
    EnsureClass(inst);

    DialogState st;
    st.spec = &spec;
    st.dpi = WindowDpi(anchor);
    ComputeLayout(st);

    const int w = Sc(kW, st.dpi);
    const int h = st.height;

    // Центрируем по виджету, но держим в пределах рабочей области монитора.
    RECT ownerRect{};
    GetWindowRect(anchor, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - w) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - h) / 2;
    HMONITOR mon = MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) {
        x = max(static_cast<int>(mi.rcWork.left), min(x, static_cast<int>(mi.rcWork.right) - w));
        y = max(static_cast<int>(mi.rcWork.top), min(y, static_cast<int>(mi.rcWork.bottom) - h));
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kClassName, spec.title.c_str(),
                                WS_POPUP | WS_CLIPCHILDREN, x, y, w, h, owner, nullptr, inst, &st);
    if (!hwnd) return false;

    // Скруглённые углы без DWM — работает одинаково на Windows 10 и 11.
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, Sc(kRadius * 2, st.dpi), Sc(kRadius * 2, st.dpi));
    SetWindowRgn(hwnd, rgn, TRUE);

    st.fieldBrush = CreateSolidBrush(P().editBack);
    st.font = CreateFontW(-Sc(15, st.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                          L"Segoe UI");

    const int pad = Sc(kPad, st.dpi);
    const int inset = Sc(10, st.dpi);
    const int fieldH = Sc(kFieldH, st.dpi);
    const int editH = Sc(20, st.dpi);

    for (size_t i = 0; i < spec.fields.size(); i++) {
        if (st.ui[i].isCalendar) continue;  // календарь рисуется сам, поля ввода ему не нужно
        const int editY = st.ui[i].yField + (fieldH - editH) / 2;
        st.ui[i].edit = CreateWindowExW(0, L"EDIT", spec.fields[i].value.c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, pad + inset,
                                        editY, w - pad * 2 - inset * 2, editH, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kFirstEditId + i)),
                                        inst, nullptr);
        SendMessageW(st.ui[i].edit, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    }

    const int btnW = Sc(120, st.dpi);
    const int btnH = Sc(kBtnH, st.dpi);
    st.cancelBtn = {pad, st.yButtons, pad + btnW, st.yButtons + btnH};
    st.okBtn = {w - pad - btnW, st.yButtons, w - pad, st.yButtons + btnH};

    // Начальное состояние календаря: выбранная дата либо текущий месяц.
    if (st.calField >= 0) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        SYSTEMTIME parsed;
        if (ParseIsoDate(spec.fields[static_cast<size_t>(st.calField)].value, parsed)) {
            st.calHasDate = true;
            st.selYear = parsed.wYear;
            st.selMonth = parsed.wMonth;
            st.selDay = parsed.wDay;
            st.calYear = parsed.wYear;
            st.calMonth = parsed.wMonth;
        } else {
            st.calHasDate = false;
            st.calYear = now.wYear;
            st.calMonth = now.wMonth;
        }
        LayoutCalendar(st, w);
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    for (size_t i = 0; i < st.ui.size(); i++) {
        if (st.ui[i].edit) {
            SetFocus(st.ui[i].edit);
            SendMessageW(st.ui[i].edit, EM_SETSEL, 0, -1);
            break;
        }
    }

    EnableWindow(owner, FALSE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsWindow(hwnd)) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);

    if (st.font) DeleteObject(st.font);
    if (st.fieldBrush) DeleteObject(st.fieldBrush);
    return st.accepted;
}
