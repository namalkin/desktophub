#include "widget.h"

#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "csv.h"
#include "dialog.h"
#include "hub_link.h"
#include "store.h"
#include "sys.h"
#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClassName[] = L"NewYearWidgetMain";
constexpr wchar_t kOwnerClass[] = L"NewYearWidgetOwner";  // скрытое окно: трей, меню, диалоги
constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 250;  // тик чаще секунды, чтобы счётчик не «плавал»

// Пока виджет тащат, мы ведём мышь сами: он — дочернее окно рабочего стола, активным не
// бывает, а захват мыши (SetCapture) у неактивного окна работает только пока курсор над ним.
constexpr UINT kMouseTimerId = 2;
constexpr UINT kMouseTimerMs = 15;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr int kHotkeyId = 1;
constexpr int kIconResourceId = 1;

// Пункты меню.
enum : UINT {
    kCmdAdd = 100,
    kCmdEditDebt,
    kCmdGoal,
    kCmdToggleTop,
    kCmdExportCsv,
    kCmdImportCsv,
    kCmdOpenFolder,
    kCmdOpenBackups,
    kCmdReset,
    kCmdAutostart,
    kCmdHotkey,
    kCmdHideDebt,
    kCmdToggleVisible,
    kCmdExit,
    kCmdThemeDark,
    kCmdThemeLight,
    kCmdGoalAdd,
    kCmdGoalDelete,
    kCmdAccentBase = 200,     // + индекс акцента
    kCmdOpacityBase = 300,    // + индекс в kOpacitySteps
    kCmdGoalSelectBase = 400  // + индекс события
};

const int kOpacitySteps[] = {100, 90, 80, 70, 60};

// Логическая геометрия (96 DPI).
constexpr int kCardW = 340;
constexpr int kShadow = 18;
constexpr int kPad = 22;
constexpr int kRowH = 30;
constexpr int kMaxRows = 6;
constexpr int kCardRadius = 18;

// Панель быстрых кнопок слева от карточки, отделённая зазором.
constexpr int kRailW = 36;
constexpr int kRailBtn = 34;
constexpr int kRailPad = 5;
constexpr int kRailGap = 10;
constexpr int kRailLabelW = 150;  // прозрачное место слева под подписи кнопок

enum RailButton {
    kRailMenu = 0,
    kRailAdd,
    kRailAddEvent,
    kRailTheme,
    kRailPin,
    kRailHide,
    kRailClose,
    kRailCount,
};

const wchar_t* RailLabel(int button, bool dark, bool onTop) {
    switch (button) {
        case kRailMenu: return L"Все настройки";
        case kRailAdd: return L"Добавить операцию";
        case kRailAddEvent: return L"Добавить событие";
        case kRailTheme: return dark ? L"Светлая тема" : L"Тёмная тема";
        case kRailPin: return onTop ? L"Не поверх окон" : L"Поверх всех окон";
        case kRailHide: return L"Свернуть в трей";
        case kRailClose: return L"Закрыть виджет";
        default: return L"";
    }
}

struct Countdown {
    long long days = 0;
    int hours = 0, minutes = 0, seconds = 0;
    bool reached = false;
};

Countdown ComputeCountdown(const AppData& data) {
    SYSTEMTIME now;
    GetLocalTime(&now);

    SYSTEMTIME target = {};
    bool custom = false;
    if (!data.ActiveGoal().date.empty() && ParseIsoDate(data.ActiveGoal().date, target)) {
        custom = true;
    } else {
        target.wYear = static_cast<WORD>(now.wYear + 1);  // ближайшее 1 января
        target.wMonth = 1;
        target.wDay = 1;
    }

    FILETIME ftNow, ftTarget;
    Countdown c;
    if (!SystemTimeToFileTime(&now, &ftNow) || !SystemTimeToFileTime(&target, &ftTarget)) return c;

    ULARGE_INTEGER a, b;
    a.LowPart = ftNow.dwLowDateTime;
    a.HighPart = ftNow.dwHighDateTime;
    b.LowPart = ftTarget.dwLowDateTime;
    b.HighPart = ftTarget.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) {
        c.reached = custom;  // для Нового года такого не бывает: цель уезжает на следующий год
        return c;
    }

    long long total = static_cast<long long>((b.QuadPart - a.QuadPart) / 10000000ULL);  // секунды
    c.days = total / 86400;
    total %= 86400;
    c.hours = static_cast<int>(total / 3600);
    total %= 3600;
    c.minutes = static_cast<int>(total / 60);
    c.seconds = static_cast<int>(total % 60);
    return c;
}


// Русские окончания: 1 день / 2 дня / 5 дней.
const wchar_t* PluralRu(long long n, const wchar_t* one, const wchar_t* few, const wchar_t* many) {
    const long long mod100 = n % 100;
    const long long mod10 = n % 10;
    if (mod100 >= 11 && mod100 <= 14) return many;
    if (mod10 == 1) return one;
    if (mod10 >= 2 && mod10 <= 4) return few;
    return many;
}

struct Fonts {
    std::unique_ptr<FontFamily> family;
    std::unique_ptr<Font> label, digits, digitLabel, money, moneyNote, rowAmount, rowText, rowSmall;

    void Rebuild(int dpi) {
        family.reset(new FontFamily(L"Segoe UI"));
        auto make = [&](float size, INT style) {
            return std::unique_ptr<Font>(new Font(family.get(), ScF(size, dpi), style, UnitPixel));
        };
        label = make(11.0f, FontStyleBold);
        digits = make(36.0f, FontStyleBold);
        digitLabel = make(10.0f, FontStyleRegular);
        money = make(30.0f, FontStyleBold);
        moneyNote = make(11.0f, FontStyleRegular);
        rowAmount = make(13.0f, FontStyleBold);
        rowText = make(13.0f, FontStyleRegular);
        rowSmall = make(11.0f, FontStyleRegular);
    }
};

// Иконки рисуем примитивами: так они одинаковы на любой машине и чётки в любом DPI.
void DrawRailIcon(Graphics& g, int button, const RectF& box, ARGB color, int dpi) {
    const float cx = box.X + box.Width / 2.0f;
    const float cy = box.Y + box.Height / 2.0f;
    const float u = ScF(1.0f, dpi);  // единица масштаба
    Color penColor(color);
    Pen pen(penColor, ScF(1.6f, dpi));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    SolidBrush brush(penColor);

    switch (button) {
        case kRailMenu:
            for (int i = -1; i <= 1; i++) {
                g.DrawLine(&pen, cx - 6 * u, cy + i * 4.5f * u, cx + 6 * u, cy + i * 4.5f * u);
            }
            break;
        case kRailAdd:
            g.DrawLine(&pen, cx - 6 * u, cy, cx + 6 * u, cy);
            g.DrawLine(&pen, cx, cy - 6 * u, cx, cy + 6 * u);
            break;
        case kRailAddEvent: {
            // Календарь с плюсом: отличает добавление события от добавления операции.
            RectF body(cx - 6.5f * u, cy - 5.5f * u, 13 * u, 12.5f * u);
            gfx::StrokeRoundRect(g, body, 2.0f * u, color, ScF(1.4f, dpi));
            g.DrawLine(&pen, body.X + 1.5f * u, cy - 1.5f * u, body.GetRight() - 1.5f * u, cy - 1.5f * u);
            g.DrawLine(&pen, cx - 2.5f * u, cy + 3.5f * u, cx + 2.5f * u, cy + 3.5f * u);
            g.DrawLine(&pen, cx, cy + 1.0f * u, cx, cy + 6.0f * u);
            break;
        }
        case kRailTheme: {
            // Круг, наполовину залитый: узнаваемый значок смены темы.
            const float r = 6.0f * u;
            g.FillPie(&brush, cx - r, cy - r, r * 2, r * 2, 90.0f, 180.0f);
            g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);
            break;
        }
        case kRailPin: {
            const float head = 3.2f * u;
            g.FillEllipse(&brush, cx - head, cy - 6 * u, head * 2, head * 2);
            g.DrawLine(&pen, cx, cy - 6 * u + head * 2, cx, cy + 6 * u);
            g.DrawLine(&pen, cx - 5 * u, cy + 1 * u, cx + 5 * u, cy + 1 * u);
            break;
        }
        case kRailHide:
            g.DrawLine(&pen, cx - 5 * u, cy - 4 * u, cx, cy + 1 * u);
            g.DrawLine(&pen, cx + 5 * u, cy - 4 * u, cx, cy + 1 * u);
            g.DrawLine(&pen, cx - 6 * u, cy + 5 * u, cx + 6 * u, cy + 5 * u);
            break;
        case kRailClose:
            g.DrawLine(&pen, cx - 5 * u, cy - 5 * u, cx + 5 * u, cy + 5 * u);
            g.DrawLine(&pen, cx + 5 * u, cy - 5 * u, cx - 5 * u, cy + 5 * u);
            break;
        default:
            break;
    }
}

enum class Zone { None, Rail, Row, RowDelete, Amount, Title };

struct Hit {
    Zone zone = Zone::None;
    int index = -1;  // индекс в data.tx для Row / RowDelete

    bool operator==(const Hit& o) const { return zone == o.zone && index == o.index; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

struct Layout {
    int dpi = 96;
    int shadow = kShadow;
    int cardW = kCardW, cardH = 0;
    int winW = 0, winH = 0;
    RECT card{};
    RECT rail{};
    std::vector<RECT> railBtns;
    int labelRight = 0;  // правая граница зоны подписей = левый край панели
    RECT amountArea{};
    RECT titleArea{};
    int yTitle = 0, yDigits = 0, yDigitLabels = 0, ySep = 0, yDebtLabel = 0, yAmount = 0, yProgress = 0, yListTop = 0;
    int visibleRows = 0;
    std::vector<RECT> rowRects;
    std::vector<RECT> rowDelRects;
    std::vector<int> rowIndex;  // индекс в data.tx
};

class Widget {
public:
    bool Create(HINSTANCE inst);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void BuildLayout();
    void Render();
    void EnsureSurface(int w, int h);
    void ReleaseSurface();
    Hit HitTest(POINT pt) const;

    void OnAdd();
    void OnEditTransaction(int index);
    void OnDeleteTransaction(int index);
    void OnEditInitialDebt();
    bool AskGoal(const wchar_t* dialogTitle, const wchar_t* okLabel, Goal& goal);
    void OnEditGoal();
    void OnAddGoal();
    void OnDeleteGoal();
    void OnResetAll();
    void OnExportCsv();
    void OnImportCsv();

    void ApplyTopmost();  // приклеить к рабочему столу или поднять поверх всех окон
    void KeepAttached();  // проводник перезапускался — вернуться на рабочий стол
    void Reparent(HWND parent);  // переезд между рабочим столом и верхним уровнем
    void BeginMove();
    void MoveTo(int screenX, int screenY);  // экранные координаты -> координаты родителя
    void MouseTick();       // очередной шаг переноса окна
    void EndMouseAction();  // кнопку отпустили: сохранить место
    // Владелец меню и диалогов: само окно виджета им быть не может — оно дочернее окно
    // рабочего стола, и владельцем стал бы сам стол. Скрытое окно подгоняется под виджет,
    // чтобы системные окна вставали по центру именно его.
    HWND OwnerWindow();
    bool RunDialog(DialogSpec& spec);
    void ApplyHotkey();
    void ApplyTheme();
    void ToggleVisible();
    void UpdateTrayTip();
    void PlaceWindow(bool firstTime);
    void SavePositionFromWindow();
    void ShowMenu(POINT screenPt);
    void ShowTrayMenu(POINT screenPt);
    void RunCommand(UINT cmd);
    void OnRailClick(int button, POINT screenPt);
    LRESULT OnHub(WPARAM command);
    void SetTrayShown(bool shown);

    static LRESULT CALLBACK OwnerProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;  // скрытое окно верхнего уровня: трей, горячая клавиша, меню, диалоги
    HINSTANCE inst_ = nullptr;
    HICON icon_ = nullptr;
    AppData data_;
    Layout layout_;
    Fonts fonts_;
    int dpi_ = 96;
    bool visible_ = true;
    bool trayShown_ = false;

    // Виджет — дочернее окно рабочего стола, а такие окна система мышью не двигает:
    // перенос делаем сами, от нажатия до отпускания.
    bool moving_ = false;
    POINT dragCursor_{};
    POINT dragOrigin_{};

    // Слоёная поверхность: 32bpp DIB, поверх него рисует GDI+.
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int surfaceW_ = 0, surfaceH_ = 0;

    Hit hover_;
    bool mouseTracking_ = false;
    int scroll_ = 0;
    int lastSecond_ = -1;
};

Widget* g_widget = nullptr;

// Повторный запуск .exe шлёт это сообщение уже работающему виджету, чтобы тот
// показался, если был скрыт в трее.
UINT ShowMessageId() {
    static const UINT id = RegisterWindowMessageW(L"NewYearWidget_Show");
    return id;
}

// ---------- Раскладка ----------

void Widget::BuildLayout() {
    Layout& L = layout_;
    L.dpi = dpi_;
    L.shadow = Sc(kShadow, dpi_);
    L.cardW = Sc(kCardW, dpi_);

    const int pad = Sc(kPad, dpi_);
    int y = Sc(20, dpi_);
    L.yTitle = y;
    y += Sc(14 + 16, dpi_);
    L.yDigits = y;
    y += Sc(46, dpi_);
    L.yDigitLabels = y;
    y += Sc(13 + 20, dpi_);
    L.ySep = y;
    y += Sc(20, dpi_);
    L.yDebtLabel = y;
    y += Sc(14 + 10, dpi_);
    L.yAmount = y;
    y += Sc(38 + 12, dpi_);
    L.yProgress = y;
    y += Sc(5 + 18, dpi_);
    L.yListTop = y;

    const int total = static_cast<int>(data_.ActiveGoal().tx.size());
    L.visibleRows = total < kMaxRows ? total : kMaxRows;
    const int maxScroll = total > L.visibleRows ? total - L.visibleRows : 0;
    if (scroll_ > maxScroll) scroll_ = maxScroll;
    if (scroll_ < 0) scroll_ = 0;

    const int listH = L.visibleRows > 0 ? L.visibleRows * Sc(kRowH, dpi_) : Sc(34, dpi_);
    // Операции добавляются кнопкой «+» на панели слева и по Ctrl+Alt+N,
    // поэтому отдельной широкой кнопки внизу карточки нет.
    y += listH + Sc(6, dpi_) + pad;
    L.cardH = y;

    // Слева: [подписи][панель кнопок][зазор][тень][карточка][тень]
    const int labelW = Sc(kRailLabelW, dpi_);
    const int railW = Sc(kRailW, dpi_);
    const int railGap = Sc(kRailGap, dpi_);
    const int cardLeft = labelW + railW + railGap + L.shadow;

    L.winW = cardLeft + L.cardW + L.shadow;
    L.winH = L.cardH + L.shadow * 2;
    L.card = {cardLeft, L.shadow, cardLeft + L.cardW, L.shadow + L.cardH};
    L.labelRight = labelW;

    const int btn = Sc(kRailBtn, dpi_);
    const int railPad = Sc(kRailPad, dpi_);
    const int railH = btn * kRailCount + railPad * 2;
    const int railTop = L.card.top + (L.cardH - railH) / 2;
    L.rail = {labelW, railTop, labelW + railW, railTop + railH};

    L.railBtns.clear();
    for (int i = 0; i < kRailCount; i++) {
        const int top = railTop + railPad + i * btn;
        L.railBtns.push_back({labelW + (railW - btn) / 2, top, labelW + (railW - btn) / 2 + btn, top + btn});
    }

    L.amountArea = {L.card.left + pad, L.card.top + L.yAmount, L.card.right - pad,
                    L.card.top + L.yAmount + Sc(38, dpi_)};

    L.titleArea = {L.card.left + pad, L.card.top + L.yTitle - Sc(6, dpi_), L.card.right - pad,
                   L.card.top + L.yTitle + Sc(26, dpi_)};

    // Транзакции показываем от новых к старым.
    L.rowRects.clear();
    L.rowDelRects.clear();
    L.rowIndex.clear();
    const int rowH = Sc(kRowH, dpi_);
    for (int i = 0; i < L.visibleRows; i++) {
        const int dataIdx = total - 1 - (i + scroll_);
        if (dataIdx < 0) break;
        RECT r = {L.card.left + pad, L.card.top + L.yListTop + i * rowH, L.card.right - pad,
                  L.card.top + L.yListTop + (i + 1) * rowH};
        L.rowRects.push_back(r);
        L.rowIndex.push_back(dataIdx);
        const int del = Sc(20, dpi_);
        RECT d = {r.right - del, r.top + (rowH - del) / 2, r.right, r.top + (rowH - del) / 2 + del};
        L.rowDelRects.push_back(d);
    }
}

// ---------- Поверхность ----------

void Widget::ReleaseSurface() {
    if (memDc_) {
        if (oldBitmap_) SelectObject(memDc_, oldBitmap_);
        DeleteDC(memDc_);
        memDc_ = nullptr;
        oldBitmap_ = nullptr;
    }
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    bits_ = nullptr;
    surfaceW_ = surfaceH_ = 0;
}

void Widget::EnsureSurface(int w, int h) {
    if (memDc_ && surfaceW_ == w && surfaceH_ == h) return;
    ReleaseSurface();

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    dib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    memDc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dib_ || !memDc_) {
        ReleaseSurface();
        return;
    }
    oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, dib_));
    surfaceW_ = w;
    surfaceH_ = h;
}

// ---------- Отрисовка ----------

void Widget::Render() {
    BuildLayout();
    const Layout& L = layout_;

    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    if (wr.right - wr.left != L.winW || wr.bottom - wr.top != L.winH) {
        SetWindowPos(hwnd_, nullptr, 0, 0, L.winW, L.winH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    EnsureSurface(L.winW, L.winH);
    if (!bits_) return;

    Bitmap surface(L.winW, L.winH, L.winW * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits_));
    Graphics g(&surface);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0x00000000));

    const float cardX = static_cast<float>(L.card.left);
    const float cardY = static_cast<float>(L.card.top);
    const float cardW = static_cast<float>(L.cardW);
    const float cardH = static_cast<float>(L.cardH);
    const float radius = ScF(kCardRadius, dpi_);
    const float pad = static_cast<float>(Sc(kPad, dpi_));

    // Мягкая тень: несколько расширяющихся контуров с малой альфой.
    for (int i = L.shadow; i > 0; i -= 2) {
        const BYTE alpha = static_cast<BYTE>(3 + (L.shadow - i) / 3);
        RectF r(cardX - i, cardY - i + i * 0.35f, cardW + i * 2, cardH + i * 2);
        gfx::FillRoundRect(g, r, radius + i, (static_cast<ARGB>(alpha) << 24));
    }

    RectF cardRect(cardX, cardY, cardW, cardH);
    gfx::FillRoundRect(g, cardRect, radius, P().cardBg);
    gfx::StrokeRoundRect(g, cardRect, radius, P().cardBorder, ScF(1.0f, dpi_));

    const float centerX = cardX + cardW / 2.0f;

    // --- Заголовок и кнопка закрытия ---
    const Countdown cd = ComputeCountdown(data_);
    lastSecond_ = cd.seconds;

    {
        const bool many = data_.goals.size() > 1;
        ARGB titleColor = cd.reached ? P().accent : P().textMuted;
        if (many && hover_.zone == Zone::Title) titleColor = P().textPrimary;
        gfx::DrawTracked(g, data_.ActiveGoal().title.c_str(), *fonts_.label, titleColor, centerX,
                         cardY + L.yTitle, ScF(3.0f, dpi_), gfx::Align::Center);

        // Точки-индикатор: сколько событий и какое показано.
        if (many) {
            const float dot = ScF(4.0f, dpi_);
            const float step = dot * 2.5f;
            const int count = static_cast<int>(data_.goals.size());
            float x = centerX - (count - 1) * step / 2.0f;
            const float y = cardY + L.yTitle + ScF(18.0f, dpi_);
            for (int i = 0; i < count; i++) {
                const bool active = (i == data_.activeGoal);
                SolidBrush brush(Color(active ? P().accent : P().textFaint));
                g.FillEllipse(&brush, x - dot / 2.0f, y, dot, dot);
                x += step;
            }
        }
    }

    // --- Панель быстрых кнопок слева ---
    {
        RectF railRect(static_cast<float>(L.rail.left), static_cast<float>(L.rail.top),
                       static_cast<float>(L.rail.right - L.rail.left),
                       static_cast<float>(L.rail.bottom - L.rail.top));
        const float railRadius = railRect.Width / 2.0f;

        for (int i = Sc(10, dpi_); i > 0; i -= 2) {
            const BYTE alpha = static_cast<BYTE>(3 + i / 3);
            RectF r(railRect.X - i, railRect.Y - i + i * 0.35f, railRect.Width + i * 2,
                    railRect.Height + i * 2);
            gfx::FillRoundRect(g, r, railRadius + i, (static_cast<ARGB>(alpha) << 24));
        }
        gfx::FillRoundRect(g, railRect, railRadius, P().cardBg);
        gfx::StrokeRoundRect(g, railRect, railRadius, P().cardBorder, ScF(1.0f, dpi_));

        for (int i = 0; i < kRailCount; i++) {
            const RECT& b = L.railBtns[static_cast<size_t>(i)];
            RectF bf(static_cast<float>(b.left), static_cast<float>(b.top),
                     static_cast<float>(b.right - b.left), static_cast<float>(b.bottom - b.top));
            const bool hot = (hover_.zone == Zone::Rail && hover_.index == i);
            // Включённый переключатель заливается акцентом — состояние видно без подписи.
            const bool active = (i == kRailPin && data_.alwaysOnTop);

            if (active) {
                gfx::FillRoundRect(g, bf, bf.Width / 2.0f, P().accent);
            } else if (hot) {
                gfx::FillRoundRect(g, bf, bf.Width / 2.0f, P().buttonBgHover);
            }

            ARGB iconColor = P().textMuted;
            if (active) iconColor = P().onAccent;
            else if (hot) iconColor = (i == kRailClose) ? P().debt : P().textPrimary;
            DrawRailIcon(g, i, bf, iconColor, dpi_);

            if (hot) {
                // Подпись слева от панели, в отведённой прозрачной зоне.
                const wchar_t* label = RailLabel(i, P().dark, data_.alwaysOnTop);
                const float textW = gfx::TextWidth(g, label, *fonts_.rowText);
                const float padX = ScF(10.0f, dpi_);
                const float h = ScF(26.0f, dpi_);
                RectF tip(static_cast<float>(L.labelRight) - ScF(8.0f, dpi_) - textW - padX * 2,
                          bf.Y + (bf.Height - h) / 2.0f, textW + padX * 2, h);
                gfx::FillRoundRect(g, tip, h / 2.0f, P().cardBg);
                gfx::StrokeRoundRect(g, tip, h / 2.0f, P().cardBorder, ScF(1.0f, dpi_));
                gfx::DrawText(g, label, *fonts_.rowText, P().textPrimary, tip.X + padX,
                              tip.Y + (h - ScF(16.0f, dpi_)) / 2.0f);
            }
        }
    }

    // --- Счётчик ---
    struct Cell {
        std::wstring value;
        const wchar_t* label;
    };
    wchar_t buf[16];
    Cell cells[4];
    swprintf(buf, 16, L"%lld", cd.days);
    cells[0] = {buf, PluralRu(cd.days, L"день", L"дня", L"дней")};
    swprintf(buf, 16, L"%02d", cd.hours);
    cells[1] = {buf, PluralRu(cd.hours, L"час", L"часа", L"часов")};
    swprintf(buf, 16, L"%02d", cd.minutes);
    cells[2] = {buf, PluralRu(cd.minutes, L"минута", L"минуты", L"минут")};
    swprintf(buf, 16, L"%02d", cd.seconds);
    cells[3] = {buf, PluralRu(cd.seconds, L"секунда", L"секунды", L"секунд")};

    {
        // Ширину колонок берём по содержимому: число дней бывает и однозначным,
        // и трёхзначным, а двоеточия должны стоять с одинаковыми отступами.
        const float gap = ScF(7.0f, dpi_);
        const float sepW = gfx::TextWidth(g, L":", *fonts_.digits);
        float valueW[4];
        float total = 0.0f;
        for (int i = 0; i < 4; i++) {
            valueW[i] = gfx::DigitsWidth(g, cells[i].value.c_str(), *fonts_.digits);
            const float labelW = gfx::TextWidth(g, cells[i].label, *fonts_.digitLabel);
            if (labelW > valueW[i]) valueW[i] = labelW;  // подпись бывает шире числа
            total += valueW[i];
        }
        total += 3.0f * (sepW + gap * 2.0f);

        float x = centerX - total / 2.0f;
        for (int i = 0; i < 4; i++) {
            const float cx = x + valueW[i] / 2.0f;
            gfx::DrawDigits(g, cells[i].value.c_str(), *fonts_.digits,
                            cd.reached ? P().textFaint : P().textPrimary, cx, cardY + L.yDigits);
            if (!cd.reached) {
                gfx::DrawText(g, cells[i].label, *fonts_.digitLabel, P().textFaint, cx,
                              cardY + L.yDigitLabels, gfx::Align::Center);
            }
            x += valueW[i];
            if (i < 3) {
                gfx::DrawText(g, L":", *fonts_.digits, P().accent, x + gap + sepW / 2.0f,
                              cardY + L.yDigits, gfx::Align::Center);
                x += sepW + gap * 2.0f;
            }
        }
        if (cd.reached) {
            gfx::DrawText(g, L"событие наступило", *fonts_.digitLabel, P().accent, centerX,
                          cardY + L.yDigitLabels, gfx::Align::Center);
        }
    }

    // --- Разделитель ---
    {
        Color sepColor(P().separator);
        Pen sep(sepColor, ScF(1.0f, dpi_));
        g.DrawLine(&sep, cardX + pad, cardY + L.ySep, cardX + cardW - pad, cardY + L.ySep);
    }

    // --- Долг ---
    const long long balance = data_.Balance();
    const bool cleared = balance <= 0;
    gfx::DrawTracked(g, L"ДОЛГ", *fonts_.label, P().textMuted, cardX + pad, cardY + L.yDebtLabel,
                     ScF(3.0f, dpi_));

    // Сумму можно закрыть звёздочками: пока курсор не на ней, чужой глаз ничего не увидит.
    const bool debtHidden = data_.hideDebt && hover_.zone != Zone::Amount;

    if (hover_.zone == Zone::Amount) {
        gfx::DrawText(g, L"изменить", *fonts_.rowSmall, P().textFaint, cardX + cardW - pad,
                      cardY + L.yDebtLabel + ScF(1.0f, dpi_), gfx::Align::Right);
    } else if (data_.hideDebt) {
        gfx::DrawText(g, L"наведите, чтобы увидеть", *fonts_.rowSmall, P().textFaint, cardX + cardW - pad,
                      cardY + L.yDebtLabel + ScF(1.0f, dpi_), gfx::Align::Right);
    }

    if (debtHidden) {
        // Звёздочку рисуем лучами, а не буквой: шрифтовой символ «*» висит у верхней
        // кромки строки и рядом с крупным «₽» смотрелся бы обрезанным.
        const float radius = ScF(7.0f, dpi_);
        const float step = radius * 2.6f;
        const float cy = cardY + L.yAmount + ScF(19.0f, dpi_);
        Color color(cleared ? P().paid : P().textPrimary);
        Pen pen(color, ScF(2.6f, dpi_));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        for (int i = 0; i < 5; i++) {
            const float cx = cardX + pad + radius + i * step;
            for (int ray = 0; ray < 3; ray++) {
                const float angle = 3.14159265f / 3.0f * ray + 3.14159265f / 2.0f;
                const float dx = cosf(angle) * radius;
                const float dy = sinf(angle) * radius;
                g.DrawLine(&pen, cx - dx, cy - dy, cx + dx, cy + dy);
            }
        }
        gfx::DrawText(g, L"₽", *fonts_.money, cleared ? P().paid : P().textPrimary,
                      cardX + pad + 5 * step + ScF(4.0f, dpi_), cardY + L.yAmount);
    } else {
        gfx::DrawText(g, FormatMoney(cleared ? 0 : balance).c_str(), *fonts_.money,
                      cleared ? P().paid : P().textPrimary, cardX + pad, cardY + L.yAmount);
    }

    // --- Прогресс погашения ---
    {
        long long obligations = data_.ActiveGoal().initialDebtCents;
        for (const Transaction& t : data_.ActiveGoal().tx) {
            if (t.cents > 0) obligations += t.cents;
        }
        const long long paid = data_.PaidCents();
        double frac = 0.0;
        if (obligations > 0) frac = static_cast<double>(paid) / static_cast<double>(obligations);
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;

        const float barW = cardW - pad * 2;
        const float barH = ScF(5.0f, dpi_);
        RectF track(cardX + pad, cardY + L.yProgress, barW, barH);
        gfx::FillRoundRect(g, track, barH / 2.0f, P().separator);
        if (frac > 0.001) {
            RectF fill(track.X, track.Y, barW * static_cast<float>(frac), barH);
            if (fill.Width < barH) fill.Width = barH;
            gfx::FillRoundRect(g, fill, barH / 2.0f, cleared ? P().paid : P().accent);
        }
        wchar_t note[64];
        if (debtHidden) {
            swprintf(note, 64, L"сумма скрыта");
        } else {
            swprintf(note, 64, cleared ? L"долг закрыт" : L"погашено %d%%",
                     static_cast<int>(frac * 100.0 + 0.5));
        }
        gfx::DrawText(g, note, *fonts_.moneyNote, P().textFaint, cardX + cardW - pad,
                      cardY + L.yAmount + ScF(18.0f, dpi_), gfx::Align::Right);
    }

    // --- Список операций ---
    if (L.rowRects.empty()) {
        gfx::DrawText(g, L"Операций пока нет", *fonts_.rowText, P().textFaint, centerX,
                      cardY + L.yListTop + ScF(8.0f, dpi_), gfx::Align::Center);
    } else {
        for (size_t i = 0; i < L.rowRects.size(); i++) {
            const RECT& r = L.rowRects[i];
            const Transaction& t = data_.ActiveGoal().tx[static_cast<size_t>(L.rowIndex[i])];
            const bool rowHover = (hover_.zone == Zone::Row || hover_.zone == Zone::RowDelete) &&
                                  hover_.index == L.rowIndex[i];

            RectF rf(static_cast<float>(r.left), static_cast<float>(r.top),
                     static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
            if (rowHover) {
                RectF bg(rf.X - ScF(6.0f, dpi_), rf.Y + ScF(1.0f, dpi_), rf.Width + ScF(12.0f, dpi_),
                         rf.Height - ScF(2.0f, dpi_));
                gfx::FillRoundRect(g, bg, ScF(7.0f, dpi_), P().hover);
            }

            const float textY = rf.Y + (rf.Height - ScF(16.0f, dpi_)) / 2.0f;
            const std::wstring amount = FormatAmount(t.cents, true);
            const ARGB amountColor = t.cents < 0 ? P().paid : P().debt;
            gfx::DrawText(g, amount.c_str(), *fonts_.rowAmount, amountColor, rf.X, textY);

            const float amountW = gfx::TextWidth(g, amount.c_str(), *fonts_.rowAmount);
            const float descX = rf.X + amountW + ScF(12.0f, dpi_);
            const float descRight = rowHover ? static_cast<float>(L.rowDelRects[i].left) - ScF(8.0f, dpi_)
                                             : rf.GetRight() - ScF(58.0f, dpi_);

            // Обрезаем описание по доступной ширине.
            std::wstring desc = t.desc.empty() ? L"—" : t.desc;
            if (descX + gfx::TextWidth(g, desc.c_str(), *fonts_.rowText) > descRight) {
                while (desc.size() > 1 &&
                       descX + gfx::TextWidth(g, (desc + L"…").c_str(), *fonts_.rowText) > descRight) {
                    desc.pop_back();
                }
                desc += L"…";
            }
            gfx::DrawText(g, desc.c_str(), *fonts_.rowText, P().textMuted, descX, textY);

            if (rowHover) {
                const RECT& d = L.rowDelRects[i];
                RectF df(static_cast<float>(d.left), static_cast<float>(d.top),
                         static_cast<float>(d.right - d.left), static_cast<float>(d.bottom - d.top));
                if (hover_.zone == Zone::RowDelete) {
                    gfx::FillRoundRect(g, df, df.Width / 2.0f, (P().debt & 0x00FFFFFF) | 0x33000000);
                }
                gfx::DrawText(g, L"×", *fonts_.rowSmall,
                              hover_.zone == Zone::RowDelete ? P().debt : P().textFaint,
                              df.X + df.Width / 2.0f, df.Y + ScF(3.0f, dpi_), gfx::Align::Center);
            } else if (!t.date.empty()) {
                // "2026-09-05" -> "05.09"
                std::wstring shortDate = t.date;
                if (shortDate.size() >= 10) {
                    shortDate = shortDate.substr(8, 2) + L"." + shortDate.substr(5, 2);
                }
                gfx::DrawText(g, shortDate.c_str(), *fonts_.rowSmall, P().textFaint, rf.GetRight(),
                              textY + ScF(2.0f, dpi_), gfx::Align::Right);
            }
        }

        // Индикатор прокрутки, если операций больше, чем помещается.
        const int total = static_cast<int>(data_.ActiveGoal().tx.size());
        if (total > L.visibleRows) {
            const float trackX = cardX + cardW - pad + ScF(9.0f, dpi_);
            const float trackTop = static_cast<float>(L.rowRects.front().top);
            const float trackBottom = static_cast<float>(L.rowRects.back().bottom);
            const float trackH = trackBottom - trackTop;
            const float thumbH = trackH * static_cast<float>(L.visibleRows) / static_cast<float>(total);
            const float offset = (trackH - thumbH) * static_cast<float>(scroll_) /
                                 static_cast<float>(total - L.visibleRows);
            gfx::FillRoundRect(g, RectF(trackX, trackTop, ScF(3.0f, dpi_), trackH), ScF(1.5f, dpi_),
                               P().separator);
            gfx::FillRoundRect(g, RectF(trackX, trackTop + offset, ScF(3.0f, dpi_), thumbH), ScF(1.5f, dpi_),
                               P().textFaint);
        }
    }

    // --- Перенос на экран ---
    POINT src = {0, 0};
    SIZE size = {L.winW, L.winH};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, static_cast<BYTE>(255 * data_.opacity / 100), AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, screen, nullptr, &size, memDc_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// ---------- Попадания мышью ----------

Hit Widget::HitTest(POINT pt) const {
    const Layout& L = layout_;
    for (size_t i = 0; i < L.railBtns.size(); i++) {
        if (PtInRect(&L.railBtns[i], pt)) return {Zone::Rail, static_cast<int>(i)};
    }
    if (PtInRect(&L.amountArea, pt)) return {Zone::Amount, -1};
    // Переключать нечего, пока событие одно — тогда и подсвечивать заголовок не нужно.
    if (data_.goals.size() > 1 && PtInRect(&L.titleArea, pt)) return {Zone::Title, -1};
    for (size_t i = 0; i < L.rowRects.size(); i++) {
        if (PtInRect(&L.rowDelRects[i], pt)) return {Zone::RowDelete, L.rowIndex[i]};
        if (PtInRect(&L.rowRects[i], pt)) return {Zone::Row, L.rowIndex[i]};
    }
    return {Zone::None, -1};
}

// ---------- Действия ----------

void Widget::OnAdd() {
    if (!visible_) ToggleVisible();  // по горячей клавише виджет мог быть скрыт

    DialogSpec spec;
    spec.title = L"Новая операция";
    spec.okLabel = L"Добавить";
    spec.fields.push_back({L"Сумма", L"-2000 — выплата долга, +5000 — долг вырос", L"",
                           DialogField::Kind::Amount});
    spec.fields.push_back({L"Описание", L"", L"", DialogField::Kind::Text});
    if (!RunDialog(spec)) return;

    long long cents = 0;
    if (!ParseAmount(spec.fields[0].value, cents)) return;

    Transaction t;
    t.cents = cents;
    t.desc = spec.fields[1].value;
    t.date = Utf8ToWide(TodayIso());
    data_.ActiveGoal().tx.push_back(std::move(t));
    scroll_ = 0;  // показываем новую операцию сверху
    SaveData(data_);
    UpdateTrayTip();
    Render();
}

void Widget::OnEditTransaction(int index) {
    if (index < 0 || index >= static_cast<int>(data_.ActiveGoal().tx.size())) return;

    DialogSpec spec;
    spec.title = L"Изменить операцию";
    spec.okLabel = L"Сохранить";
    spec.fields.push_back({L"Сумма", L"-2000 — выплата долга, +5000 — долг вырос",
                           FormatAmount(data_.ActiveGoal().tx[static_cast<size_t>(index)].cents, true),
                           DialogField::Kind::Amount});
    spec.fields.push_back({L"Описание", L"", data_.ActiveGoal().tx[static_cast<size_t>(index)].desc,
                           DialogField::Kind::Text});
    if (!RunDialog(spec)) return;

    long long cents = 0;
    if (!ParseAmount(spec.fields[0].value, cents)) return;
    data_.ActiveGoal().tx[static_cast<size_t>(index)].cents = cents;
    data_.ActiveGoal().tx[static_cast<size_t>(index)].desc = spec.fields[1].value;
    SaveData(data_);
    UpdateTrayTip();
    Render();
}

void Widget::OnDeleteTransaction(int index) {
    if (index < 0 || index >= static_cast<int>(data_.ActiveGoal().tx.size())) return;
    data_.ActiveGoal().tx.erase(data_.ActiveGoal().tx.begin() + index);
    hover_ = {};
    SaveData(data_);
    UpdateTrayTip();
    Render();
}

void Widget::OnEditInitialDebt() {
    DialogSpec spec;
    spec.title = L"Начальный долг";
    spec.okLabel = L"Сохранить";
    spec.fields.push_back({L"Сумма", L"Сумма долга на старте",
                           FormatAmount(data_.ActiveGoal().initialDebtCents, false),
                           DialogField::Kind::AmountPositive});
    if (!RunDialog(spec)) return;

    long long cents = 0;
    if (!ParseAmount(spec.fields[0].value, cents)) return;
    data_.ActiveGoal().initialDebtCents = cents < 0 ? -cents : cents;
    SaveData(data_);
    UpdateTrayTip();
    Render();
}

bool Widget::AskGoal(const wchar_t* dialogTitle, const wchar_t* okLabel, Goal& goal) {
    DialogSpec spec;
    spec.title = dialogTitle;
    spec.okLabel = okLabel;
    spec.fields.push_back({L"Название", L"Показывается заголовком карточки", goal.title,
                           DialogField::Kind::Text});
    // У каждого события свой долг, поэтому сумма спрашивается прямо здесь.
    spec.fields.push_back({L"Начальный долг", L"Сколько должны на старте по этому событию",
                           FormatAmount(goal.initialDebtCents, false),
                           DialogField::Kind::AmountPositive});
    spec.fields.push_back({L"Дата", L"", goal.date, DialogField::Kind::Calendar});
    if (!RunDialog(spec)) return false;

    goal.title = spec.fields[0].value.empty() ? L"NEW YEAR" : spec.fields[0].value;
    long long cents = 0;
    if (ParseAmount(spec.fields[1].value, cents)) goal.initialDebtCents = cents < 0 ? -cents : cents;
    goal.date = spec.fields[2].value;
    return true;
}

void Widget::OnEditGoal() {
    Goal goal = data_.ActiveGoal();
    if (!AskGoal(L"Событие", L"Сохранить", goal)) return;
    data_.goals[static_cast<size_t>(data_.activeGoal)] = goal;
    SaveData(data_);
    Render();
}

void Widget::OnAddGoal() {
    Goal goal;
    goal.title.clear();      // пусть пользователь введёт своё название
    goal.initialDebtCents = 0;  // и свою сумму: чужой долг сюда тянуть незачем
    if (!AskGoal(L"Новое событие", L"Добавить", goal)) return;
    data_.goals.push_back(goal);
    data_.SelectGoal(static_cast<int>(data_.goals.size()) - 1);
    scroll_ = 0;
    SaveData(data_);
    UpdateTrayTip();
    Render();
}

void Widget::OnDeleteGoal() {
    if (data_.goals.size() <= 1) {
        MessageBoxW(OwnerWindow(), L"Это единственное событие — удалить его нельзя.\nМожно изменить название и дату.",
                    L"События", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring message = L"Удалить событие «" + data_.ActiveGoal().title + L"»?";
    if (MessageBoxW(OwnerWindow(), message.c_str(), L"События", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    data_.goals.erase(data_.goals.begin() + data_.activeGoal);
    data_.SelectGoal(data_.activeGoal);  // приводит индекс в границы
    scroll_ = 0;
    SaveData(data_);
    UpdateTrayTip();
    Render();
}

void Widget::OnResetAll() {
    if (MessageBoxW(OwnerWindow(), L"Удалить все операции? Начальный долг останется прежним.", L"Очистить список",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    data_.ActiveGoal().tx.clear();
    scroll_ = 0;
    hover_ = {};
    SaveData(data_);
    UpdateTrayTip();
    Render();
}

void Widget::OnExportCsv() {
    std::wstring path;
    if (!ExportCsv(hwnd_, data_, path)) return;
    const std::wstring message = L"Операции сохранены:\n" + path;
    MessageBoxW(OwnerWindow(), message.c_str(), L"Экспорт в CSV", MB_OK | MB_ICONINFORMATION);
}

void Widget::OnImportCsv() {
    int added = 0;
    if (!ImportCsv(hwnd_, data_, added)) {
        if (added == 0) {
            MessageBoxW(OwnerWindow(), L"Не удалось прочитать ни одной операции.\nОжидается: дата;сумма;описание",
                        L"Импорт из CSV", MB_OK | MB_ICONWARNING);
        }
        return;
    }
    scroll_ = 0;
    SaveData(data_);
    UpdateTrayTip();
    Render();

    wchar_t message[128];
    swprintf(message, 128, L"Добавлено операций: %d", added);
    MessageBoxW(OwnerWindow(), message, L"Импорт из CSV", MB_OK | MB_ICONINFORMATION);
}

// ---------- Режимы ----------

// Виджет живёт в одном из двух состояний:
//   «на рабочем столе» — дочернее окно хозяина рабочего стола. Тогда он и есть часть рабочего стола:
//      никогда не оказывается над чужими окнами (даже когда по столу кликнули мышью или
//      это второй монитор), но виден вместе со столом, в том числе по Win+D;
//   «поверх всех окон» — обычное окно верхнего уровня с HWND_TOPMOST.
void Widget::ApplyTopmost() {
    if (!hwnd_) return;
    const bool onTop = data_.alwaysOnTop;
    HWND desktop = onTop ? nullptr : DesktopHostWindow();
    const HWND parent = GetParent(hwnd_);

    if (desktop) {
        if (parent != desktop) {
            Reparent(desktop);
            PlaceWindow(false);  // координаты теперь считаются от клиентской области стола
            Render();
        }
        // Внутри рабочего стола держимся поверх значков, но ниже любого обычного окна.
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    if (parent) {
        Reparent(nullptr);
        PlaceWindow(false);
        Render();
    }
    SetWindowPos(hwnd_, onTop ? HWND_TOPMOST : HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Переезд между рабочим столом и верхним уровнем. Тонкость: у окна, сменившего родителя,
// слоёная поверхность перестаёт показываться, пока WS_EX_LAYERED не выставлен заново —
// иначе виджет выглядит как исчезнувший, хотя окно на месте и «видимо».
void Widget::Reparent(HWND parent) {
    const bool toDesktop = parent != nullptr;
    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    // Именно WS_CHILD, а не WS_POPUP с родителем: гибрид система перерисовывает рывками,
    // и полупрозрачная карточка заметно мерцает поверх обоев.
    style = toDesktop ? ((style & ~WS_POPUP) | WS_CHILD) : ((style & ~WS_CHILD) | WS_POPUP);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
    SetParent(hwnd_, parent);

    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (visible_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

// Проводник мог перезапуститься или перестроить рабочий стол (Snap Assist, Win+Tab):
// хозяин значков сменился, и виджет нужно перевесить на новый — иначе он под обоями.
void Widget::KeepAttached() {
    if (!hwnd_ || moving_) return;
    if (data_.alwaysOnTop) return;
    HWND desktop = DesktopHostWindow();
    if (desktop && GetParent(hwnd_) != desktop) ApplyTopmost();
}

void Widget::BeginMove() {
    moving_ = true;
    GetCursorPos(&dragCursor_);
    RECT r{};
    GetWindowRect(hwnd_, &r);
    dragOrigin_ = {r.left, r.top};
    SetCapture(hwnd_);
    SetTimer(hwnd_, kMouseTimerId, kMouseTimerMs, nullptr);
}

// Позиция хранится в экранных координатах, а SetWindowPos у дочернего окна ждёт координаты
// родителя — переводим здесь, в одном месте.
void Widget::MoveTo(int screenX, int screenY) {
    POINT origin = {0, 0};
    if (HWND parent = GetParent(hwnd_)) ClientToScreen(parent, &origin);
    SetWindowPos(hwnd_, nullptr, screenX - origin.x, screenY - origin.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Курсор и кнопку читаем напрямую: сообщения о движении приходят только пока курсор над
// виджетом, а при переносе он легко оказывается за его краем.
void Widget::MouseTick() {
    if (!moving_ || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        EndMouseAction();
        return;
    }
    POINT cursor;
    GetCursorPos(&cursor);
    MoveTo(dragOrigin_.x + (cursor.x - dragCursor_.x), dragOrigin_.y + (cursor.y - dragCursor_.y));
}

void Widget::EndMouseAction() {
    KillTimer(hwnd_, kMouseTimerId);
    if (!moving_) return;
    moving_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    SavePositionFromWindow();
    Render();
}

HWND Widget::OwnerWindow() {
    if (owner_ && hwnd_) {
        RECT r{};
        GetWindowRect(hwnd_, &r);
        SetWindowPos(owner_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return owner_ ? owner_ : hwnd_;
}

// Пока идёт ввод, виджет не принимает мышь: владелец диалога — скрытое окно, и обычной
// модальности через EnableWindow(owner) тут не хватает.
bool Widget::RunDialog(DialogSpec& spec) {
    EnableWindow(hwnd_, FALSE);
    const bool ok = ShowDialog(OwnerWindow(), hwnd_, spec);
    EnableWindow(hwnd_, TRUE);
    return ok;
}

void Widget::ApplyHotkey() {
    UnregisterHotKey(owner_, kHotkeyId);
    if (!data_.hotkeyEnabled) return;
    // Комбинацию мог занять кто-то другой — тогда просто живём без неё.
    RegisterHotKey(owner_, kHotkeyId, MOD_CONTROL | MOD_ALT, 'N');
}

void Widget::ApplyTheme() { SetTheme(data_.darkTheme, data_.accent); }

void Widget::ToggleVisible() {
    visible_ = !visible_;
    ShowWindow(hwnd_, visible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible_) {
        ApplyTopmost();
        Render();
    }
}

void Widget::UpdateTrayTip() {
    // Подсказка трея видна всем, кто смотрит на экран, — при скрытой сумме молчим и здесь.
    if (data_.hideDebt) {
        TrayUpdateTip(owner_, (data_.ActiveGoal().title + L": сумма скрыта").c_str());
        return;
    }
    const long long balance = data_.Balance();
    const std::wstring tip = data_.ActiveGoal().title + L": " + FormatMoney(balance <= 0 ? 0 : balance);
    TrayUpdateTip(owner_, tip.c_str());
}

void Widget::PlaceWindow(bool firstTime) {
    BuildLayout();
    int x = data_.winX;
    int y = data_.winY;

    bool needDefault = (x == INT32_MIN || y == INT32_MIN);
    if (!needDefault) {
        // Позиция могла остаться от отключённого монитора — проверяем.
        const POINT probe = {x + layout_.winW / 2, y + layout_.winH / 2};
        if (!MonitorFromPoint(probe, MONITOR_DEFAULTTONULL)) needDefault = true;
    }
    if (needDefault) {
        MONITORINFO mi = {sizeof(mi)};
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (GetMonitorInfoW(mon, &mi)) {
            x = mi.rcWork.right - layout_.winW - Sc(24, dpi_);
            y = mi.rcWork.top + Sc(24, dpi_);
        } else {
            x = y = 100;
        }
        data_.winX = x;
        data_.winY = y;
    }

    // Ширина окна могла вырасти (панель кнопок) — не даём карточке уехать за край экрана.
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromPoint(POINT{x + layout_.winW / 2, y + layout_.winH / 2},
                                    MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(mon, &mi)) {
        if (x + layout_.winW > mi.rcWork.right) x = mi.rcWork.right - layout_.winW;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + layout_.winH > mi.rcWork.bottom) y = mi.rcWork.bottom - layout_.winH;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
        data_.winX = x;
        data_.winY = y;
    }

    POINT origin = {0, 0};
    if (HWND parent = GetParent(hwnd_)) ClientToScreen(parent, &origin);
    SetWindowPos(hwnd_, nullptr, x - origin.x, y - origin.y, layout_.winW, layout_.winH,
                 SWP_NOZORDER | SWP_NOACTIVATE | (firstTime ? 0 : SWP_NOSIZE));
}

void Widget::SavePositionFromWindow() {
    RECT r{};
    GetWindowRect(hwnd_, &r);
    if (r.left != data_.winX || r.top != data_.winY) {
        data_.winX = r.left;
        data_.winY = r.top;
        SaveData(data_);
    }
}

// ---------- Меню ----------

void Widget::ShowMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, kCmdAdd, L"Добавить операцию\tCtrl+Alt+N");
    AppendMenuW(menu, MF_STRING, kCmdEditDebt, L"Изменить начальный долг");

    HMENU goalMenu = CreatePopupMenu();
    for (size_t i = 0; i < data_.goals.size(); i++) {
        std::wstring label = data_.goals[i].title;
        if (!data_.goals[i].date.empty()) label += L"   " + data_.goals[i].date;
        AppendMenuW(goalMenu, MF_STRING | (static_cast<int>(i) == data_.activeGoal ? MF_CHECKED : 0),
                    kCmdGoalSelectBase + static_cast<UINT>(i), label.c_str());
    }
    AppendMenuW(goalMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(goalMenu, MF_STRING, kCmdGoalAdd, L"Добавить событие…");
    AppendMenuW(goalMenu, MF_STRING, kCmdGoal, L"Изменить текущее…");
    AppendMenuW(goalMenu, MF_STRING | (data_.goals.size() > 1 ? 0 : MF_GRAYED), kCmdGoalDelete,
                L"Удалить текущее");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(goalMenu), L"События");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU look = CreatePopupMenu();
    AppendMenuW(look, MF_STRING | (data_.darkTheme ? MF_CHECKED : 0), kCmdThemeDark, L"Тёмная тема");
    AppendMenuW(look, MF_STRING | (data_.darkTheme ? 0 : MF_CHECKED), kCmdThemeLight, L"Светлая тема");
    AppendMenuW(look, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < AccentCount(); i++) {
        AppendMenuW(look, MF_STRING | (data_.accent == i ? MF_CHECKED : 0), kCmdAccentBase + i,
                    AccentName(i));
    }
    AppendMenuW(look, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < static_cast<int>(sizeof(kOpacitySteps) / sizeof(kOpacitySteps[0])); i++) {
        wchar_t text[32];
        swprintf(text, 32, L"Непрозрачность %d%%", kOpacitySteps[i]);
        AppendMenuW(look, MF_STRING | (data_.opacity == kOpacitySteps[i] ? MF_CHECKED : 0),
                    kCmdOpacityBase + i, text);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(look), L"Оформление");

    AppendMenuW(menu, MF_STRING | (data_.alwaysOnTop ? MF_CHECKED : 0), kCmdToggleTop, L"Поверх всех окон");
    AppendMenuW(menu, MF_STRING | (data_.hideDebt ? MF_CHECKED : 0), kCmdHideDebt,
                L"Скрывать сумму долга");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU dataMenu = CreatePopupMenu();
    AppendMenuW(dataMenu, MF_STRING, kCmdExportCsv, L"Экспорт в CSV…");
    AppendMenuW(dataMenu, MF_STRING, kCmdImportCsv, L"Импорт из CSV…");
    AppendMenuW(dataMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(dataMenu, MF_STRING, kCmdOpenFolder, L"Папка с данными");
    AppendMenuW(dataMenu, MF_STRING, kCmdOpenBackups, L"Резервные копии");
    AppendMenuW(dataMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(dataMenu, MF_STRING, kCmdReset, L"Очистить список операций");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(dataMenu), L"Данные");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), kCmdAutostart,
                L"Запускать вместе с Windows");
    AppendMenuW(menu, MF_STRING | (data_.hotkeyEnabled ? MF_CHECKED : 0), kCmdHotkey,
                L"Горячая клавиша Ctrl+Alt+N");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdToggleVisible, L"Скрыть в трей");
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Закрыть виджет");

    // Без этого меню не закрывается по клику мимо окна.
    SetForegroundWindow(OwnerWindow());
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                      screenPt.x, screenPt.y, 0, OwnerWindow(), nullptr));
    PostMessageW(OwnerWindow(), WM_NULL, 0, 0);
    DestroyMenu(menu);
    RunCommand(cmd);
}

void Widget::ShowTrayMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdToggleVisible, visible_ ? L"Скрыть виджет" : L"Показать виджет");
    AppendMenuW(menu, MF_STRING, kCmdAdd, L"Добавить операцию");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Закрыть виджет");

    SetForegroundWindow(OwnerWindow());
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                      screenPt.x, screenPt.y, 0, OwnerWindow(), nullptr));
    PostMessageW(OwnerWindow(), WM_NULL, 0, 0);
    DestroyMenu(menu);
    RunCommand(cmd);
}

void Widget::OnRailClick(int button, POINT screenPt) {
    switch (button) {
        case kRailMenu: ShowMenu(screenPt); break;
        case kRailAdd: OnAdd(); break;
        case kRailAddEvent: OnAddGoal(); break;
        case kRailTheme: RunCommand(data_.darkTheme ? kCmdThemeLight : kCmdThemeDark); break;
        case kRailPin: RunCommand(kCmdToggleTop); Render(); break;
        case kRailHide: ToggleVisible(); break;
        case kRailClose: DestroyWindow(hwnd_); break;
        default: break;
    }
}

void Widget::RunCommand(UINT cmd) {
    if (cmd == 0) return;

    // Диапазоны проверяем от старших к младшим: kCmdGoalSelectBase > kCmdOpacityBase > kCmdAccentBase.
    if (cmd >= kCmdGoalSelectBase) {
        const int index = static_cast<int>(cmd - kCmdGoalSelectBase);
        if (index < static_cast<int>(data_.goals.size())) {
            data_.SelectGoal(index);
            scroll_ = 0;  // у каждого события свой список операций
            hover_ = {};
            SaveData(data_);
            UpdateTrayTip();
            Render();
        }
        return;
    }
    if (cmd >= kCmdOpacityBase) {
        const int index = static_cast<int>(cmd - kCmdOpacityBase);
        if (index < static_cast<int>(sizeof(kOpacitySteps) / sizeof(kOpacitySteps[0]))) {
            data_.opacity = kOpacitySteps[index];
            SaveData(data_);
            Render();
        }
        return;
    }
    if (cmd >= kCmdAccentBase) {
        const int index = static_cast<int>(cmd - kCmdAccentBase);
        if (index < AccentCount()) {
            data_.accent = index;
            ApplyTheme();
            SaveData(data_);
            Render();
        }
        return;
    }

    switch (cmd) {
        case kCmdAdd: OnAdd(); break;
        case kCmdEditDebt: OnEditInitialDebt(); break;
        case kCmdGoal: OnEditGoal(); break;
        case kCmdGoalAdd: OnAddGoal(); break;
        case kCmdGoalDelete: OnDeleteGoal(); break;
        case kCmdToggleTop:
            data_.alwaysOnTop = !data_.alwaysOnTop;
            ApplyTopmost();
            SaveData(data_);
            break;
        case kCmdThemeDark:
        case kCmdThemeLight:
            data_.darkTheme = (cmd == kCmdThemeDark);
            ApplyTheme();
            SaveData(data_);
            Render();
            break;
        case kCmdExportCsv: OnExportCsv(); break;
        case kCmdImportCsv: OnImportCsv(); break;
        case kCmdOpenFolder: OpenFolder(DataDir()); break;
        case kCmdOpenBackups: OpenFolder(BackupDir()); break;
        case kCmdReset: OnResetAll(); break;
        case kCmdAutostart:
            if (!SetAutostart(!IsAutostartEnabled())) {
                MessageBoxW(OwnerWindow(), L"Не удалось изменить автозапуск.", L"Автозапуск", MB_OK | MB_ICONWARNING);
            }
            break;
        case kCmdHotkey:
            data_.hotkeyEnabled = !data_.hotkeyEnabled;
            ApplyHotkey();
            SaveData(data_);
            break;
        case kCmdHideDebt:
            data_.hideDebt = !data_.hideDebt;
            SaveData(data_);
            UpdateTrayTip();
            Render();
            break;
        case kCmdToggleVisible: ToggleVisible(); break;
        case kCmdExit: DestroyWindow(hwnd_); break;
        default: break;
    }
}

// ---------- Центр виджетов ----------

void Widget::SetTrayShown(bool shown) {
    if (shown == trayShown_) return;
    if (shown) {
        trayShown_ = TrayAdd(owner_, kTrayMessage, icon_, L"New Year Widget");
        UpdateTrayTip();
    } else {
        TrayRemove(owner_);
        trayShown_ = false;
    }
}

LRESULT Widget::OnHub(WPARAM command) {
    switch (command) {
        case hub::kShow: if (!visible_) ToggleVisible(); break;
        case hub::kHide: if (visible_) ToggleVisible(); break;
        case hub::kToggle: ToggleVisible(); break;
        case hub::kSettings: {
            // Отдельного окна настроек у виджета нет — всё настраивается его меню.
            if (!visible_) ToggleVisible();
            POINT pt;
            GetCursorPos(&pt);
            ShowMenu(pt);
            break;
        }
        case hub::kExit: DestroyWindow(hwnd_); break;
        case hub::kTrayOn: SetTrayShown(true); break;
        case hub::kTrayOff: SetTrayShown(false); break;
        case hub::kQuery: return hub::kAlive | (visible_ ? hub::kVisible : 0);
        default: break;
    }
    return hub::kAlive;
}

// ---------- Оконная процедура ----------

LRESULT Widget::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == ShowMessageId()) {
        if (!visible_) ToggleVisible();
        return 0;
    }
    if (msg == hub::MessageId()) return OnHub(wp);

    switch (msg) {
        case WM_TIMER: {
            if (wp == kMouseTimerId) {
                MouseTick();
                return 0;
            }
            if (wp != kTimerId) break;
            if (!visible_) return 0;  // скрытый виджет перерисовывать незачем
            KeepAttached();
            const Countdown cd = ComputeCountdown(data_);
            if (cd.seconds != lastSecond_) Render();
            return 0;
        }

        case kTrayMessage: {
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                ShowTrayMenu(pt);
            } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
                ToggleVisible();
            }
            return 0;
        }

        case WM_HOTKEY:
            if (wp == kHotkeyId) OnAdd();
            return 0;

        // Дочерних контролов у окна нет, так что WM_COMMAND — это всегда команда меню.
        case WM_COMMAND:
            RunCommand(LOWORD(wp));
            return 0;

        case WM_MOUSEMOVE: {
            if (!mouseTracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                mouseTracking_ = true;
                Render();  // показываем кнопку закрытия
            }
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Hit hit = HitTest(pt);
            if (hit != hover_) {
                hover_ = hit;
                Render();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            hover_ = {};
            Render();
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Hit hit = HitTest(pt);
            switch (hit.zone) {
                case Zone::Rail: {
                    POINT screen = pt;
                    ClientToScreen(hwnd, &screen);
                    OnRailClick(hit.index, screen);
                    return 0;
                }
                case Zone::RowDelete:
                    OnDeleteTransaction(hit.index);
                    return 0;
                case Zone::Amount:
                    OnEditInitialDebt();
                    return 0;
                case Zone::Title:
                    data_.SelectGoal(data_.activeGoal + 1);  // клик по заголовку — следующее событие
                    scroll_ = 0;
                    hover_ = {};
                    SaveData(data_);
                    UpdateTrayTip();
                    Render();
                    return 0;
                default:
                    break;
            }
            // Клик по пустому месту карточки — перетаскивание окна.
            BeginMove();
            return 0;
        }

        case WM_LBUTTONUP:
            if (moving_) EndMouseAction();
            return 0;

        // Захват мыши у неактивного окна система забирает сама, и перенос от этого
        // прерываться не должен — его ведёт таймер, пока нажата кнопка.
        case WM_CAPTURECHANGED:
            return 0;

        case WM_LBUTTONDBLCLK: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Hit hit = HitTest(pt);
            if (hit.zone == Zone::Row) OnEditTransaction(hit.index);
            return 0;
        }

        case WM_RBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            ShowMenu(pt);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int total = static_cast<int>(data_.ActiveGoal().tx.size());
            const int maxScroll = total > layout_.visibleRows ? total - layout_.visibleRows : 0;
            if (maxScroll == 0) return 0;
            const int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
            const int next = scroll_ + delta;
            scroll_ = next < 0 ? 0 : (next > maxScroll ? maxScroll : next);
            hover_ = {};
            Render();
            return 0;
        }


        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            fonts_.Rebuild(dpi_);
            auto* suggested = reinterpret_cast<RECT*>(lp);
            data_.winX = suggested->left;
            data_.winY = suggested->top;
            BuildLayout();
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, layout_.winW, layout_.winH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Render();
            return 0;
        }

        case WM_DISPLAYCHANGE:
            PlaceWindow(false);
            Render();
            return 0;

        case WM_ENDSESSION:
            SavePositionFromWindow();
            SaveData(data_);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            KillTimer(hwnd, kMouseTimerId);
            UnregisterHotKey(owner_, kHotkeyId);
            TrayRemove(owner_);
            SavePositionFromWindow();
            SaveData(data_);
            ReleaseSurface();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Widget::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_widget && g_widget->hwnd_ == hwnd) return g_widget->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Скрытое окно верхнего уровня. На нём висят значок в трее, горячая клавиша и меню:
// само окно виджета для этого не годится — оно дочернее окно рабочего стола.
LRESULT CALLBACK Widget::OwnerProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_widget || g_widget->owner_ != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == kTrayMessage || msg == WM_HOTKEY || msg == WM_COMMAND || msg == ShowMessageId() ||
        msg == hub::MessageId()) {
        return g_widget->WndProc(g_widget->hwnd_, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Widget::Create(HINSTANCE inst) {
    inst_ = inst;
    g_widget = this;

    LoadData(data_);  // файла нет — остаются значения по умолчанию
    ApplyTheme();

    icon_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON, 0, 0,
                                          LR_DEFAULTSIZE | LR_SHARED));

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &Widget::WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon_;
    wc.hIconSm = icon_;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    WNDCLASSEXW ownerClass = {sizeof(ownerClass)};
    ownerClass.lpfnWndProc = &Widget::OwnerProcThunk;
    ownerClass.hInstance = inst;
    ownerClass.hIcon = icon_;
    ownerClass.hIconSm = icon_;
    ownerClass.lpszClassName = kOwnerClass;
    if (!RegisterClassExW(&ownerClass)) return false;

    owner_ = CreateWindowExW(WS_EX_TOOLWINDOW, kOwnerClass, L"New Year Widget", WS_POPUP, 0, 0, 0, 0, nullptr,
                             nullptr, inst, nullptr);
    if (!owner_) return false;

    // WS_EX_NOACTIVATE: виджет принимает клики, но не забирает фокус — рабочий стол и чужие
    // окна не «мигают» активацией, когда по нему просто ткнули мышью.
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
                            L"New Year Widget", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    dpi_ = WindowDpi(hwnd_);
    fonts_.Rebuild(dpi_);

    PlaceWindow(true);
    ApplyTopmost();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Render();

    // При запущенном центре значок виджета живёт в его меню, а не отдельно в трее.
    SetTrayShown(!hub::HubRunning());
    ApplyHotkey();
    SetTimer(hwnd_, kTimerId, kTimerMs, nullptr);
    return true;
}

Widget g_instance;

}  // namespace

bool CreateWidgetWindow(HINSTANCE inst) { return g_instance.Create(inst); }
