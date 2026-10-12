#include "widget.h"

#include <shlobj.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "date.h"
#include "dialog.h"
#include "hub_link.h"
#include "panels.h"
#include "settings.h"
#include "store.h"
#include "sys.h"
#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kAppClass[] = L"PlannerGoApp";      // скрытое окно: трей, горячая клавиша, таймер
constexpr wchar_t kBlockClass[] = L"PlannerGoPanel";  // блок планера на рабочем столе
constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 500;

// Пока блок тащат или растягивают, мы ведём мышь сами: блок — дочернее окно рабочего
// стола, активным оно не бывает, а захват мыши (SetCapture) у неактивного окна работает
// только пока курсор над ним. Поэтому опрашиваем курсор и кнопку по таймеру.
constexpr UINT kMouseTimerId = 2;
constexpr UINT kMouseTimerMs = 15;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr int kHotkeyId = 1;
constexpr int kIconResourceId = 1;

// Логическая геометрия блока (96 DPI).
constexpr int kShadow = 16;
constexpr int kPad = 12;
constexpr int kCardRadius = 14;
constexpr int kHeaderH = 30;
constexpr int kHeaderBtn = 22;
constexpr int kGrip = 9;  // ширина зоны, за которую блок тянут мышью
constexpr int kMinW = 200;
constexpr int kMinH = 120;
constexpr int kMaxW = 1400;
constexpr int kMaxH = 1600;
constexpr int kWideHeaderW = 190;  // уже этого — в заголовке остаётся одна кнопка «⋮»
constexpr int kDragThreshold = 5;

// Пункты меню.
enum : UINT {
    kCmdRenameBlock = 100,
    kCmdToggleCollapse,
    kCmdToggleLock,
    kCmdDeleteBlock,
    kCmdSettings,        // оформление и содержимое одного блока
    kCmdGlobalSettings,  // блоки на экране, шрифт, поведение, данные
    kCmdCollapseAll,
    kCmdExpandAll,
    kCmdToggleVisible,
    kCmdToday,
    kCmdArrange,
    kCmdExit,
    kCmdRowEdit,
    kCmdRowDelete,
    kCmdRowTomorrow,
    kCmdRowClear,
    kCmdAddBlockBase = 600,  // + индекс типа блока
};

bool CanAdd(PanelKind kind) {
    return kind == PanelKind::DayTasks || kind == PanelKind::WeekGoals || kind == PanelKind::Habits;
}

// Число в «пилюле» рядом с названием блока: задачи, цели, привычки.
int HeaderCount(const AppData& data, const Panel& panel, const Date& selected) {
    switch (panel.kind) {
        case PanelKind::DayTasks: {
            const Day* day = data.FindDay(selected);
            return day ? static_cast<int>(day->tasks.size()) : 0;
        }
        case PanelKind::WeekGoals: {
            const Week* week = data.FindWeek(MondayOf(selected));
            return week ? static_cast<int>(week->goals.size()) : 0;
        }
        case PanelKind::Habits: return static_cast<int>(data.habits.size());
        default: return -1;
    }
}

RectF ToRectF(const RECT& r) {
    return RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                 static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
}

int ClampInt(int value, int lo, int hi) { return value < lo ? lo : (value > hi ? hi : value); }

struct Fonts {
    std::unique_ptr<FontFamily> family;
    std::unique_ptr<Font> title, count, row, small, tiny, num;
    int dpi = 0;
    int fontIndex = -1;  // какой шрифт из настроек уже построен

    void Rebuild(int newDpi, int newFontIndex) {
        dpi = newDpi;
        fontIndex = newFontIndex;
        family.reset(MakeUiFontFamily(fontIndex));
        const float k = UiFontScale(fontIndex);
        auto make = [&](float size, INT style) {
            return std::unique_ptr<Font>(new Font(family.get(), ScF(size * k, dpi), style, UnitPixel));
        };
        title = make(12.0f, FontStyleBold);
        count = make(10.0f, FontStyleRegular);
        row = make(12.5f, FontStyleRegular);
        small = make(11.0f, FontStyleRegular);
        tiny = make(9.5f, FontStyleRegular);
        num = make(12.5f, FontStyleBold);
    }
};

enum class Zone { None, Header, HeaderAdd, HeaderSettings, HeaderMenu, ResizeW, ResizeH, ResizeWH, Content };

struct Hit {
    Zone zone = Zone::None;
    Hover hover;
    int elem = -1;

    bool operator==(const Hit& o) const { return zone == o.zone && hover == o.hover; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

struct BlockLayout {
    int dpi = 96;
    int shadow = 0;
    int cardW = 0, cardH = 0;
    int winW = 0, winH = 0;
    RECT card{};
    RECT header{};
    RECT addBtn{};
    RECT gearBtn{};
    RECT menuBtn{};
    RECT viewport{};
    RECT resizeRight{};
    RECT resizeBottom{};
    RECT resizeCorner{};
    bool compactHeader = false;
};

class App;

// ---------- Блок ----------

class Block {
public:
    bool Create(HINSTANCE inst, int panelIndex);
    void Close();

    HWND Hwnd() const { return hwnd_; }
    int PanelIndex() const { return panelIndex_; }
    void SetPanelIndex(int index) { panelIndex_ = index; }

    void Refresh();  // пересчитать раскладку, размер окна и перерисовать
    void Render();
    void ApplyTopmost();   // приклеить к рабочему столу или поднять поверх всех окон
    void KeepAttached();   // проводник перезапускался — вернуться на рабочий стол
    void Reparent(HWND parent);  // переезд между рабочим столом и верхним уровнем
    void Show(bool visible);
    void SavePosition();
    void PlaceWindow();

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    const Panel& Data() const;
    Panel& MutableData();

    void BuildLayout();
    void ClampScroll();
    void ApplyResize(POINT pt);
    void EnsureSurface(int w, int h);
    void ReleaseSurface();
    Hit HitTest(POINT pt) const;
    void DrawHeader(Graphics& g);

    HWND hwnd_ = nullptr;
    int panelIndex_ = 0;
    int dpi_ = 96;
    BlockLayout layout_;
    Fonts fonts_;

    // Слоёная поверхность: 32bpp DIB, поверх него рисует GDI+.
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int surfaceW_ = 0, surfaceH_ = 0;

    // Содержимое блока: что нарисовано, то и нажимается.
    std::vector<Elem> elems_;
    int contentH_ = 0;
    bool scrollable_ = false;
    int scroll_ = 0;

    Hit hover_;
    bool mouseTracking_ = false;

    // Нажатие на заголовок: пока не сдвинули — это клик «свернуть», после — перенос окна.
    bool headerPending_ = false;
    POINT pressPt_{};
    int pressElem_ = -1;

    // Блок — дочернее окно рабочего стола, а такие окна система мышью не двигает:
    // перенос делаем сами, от нажатия до отпускания.
    bool moving_ = false;
    POINT dragCursor_{};  // курсор в момент захвата, экранные координаты
    POINT dragOrigin_{};  // левый верхний угол окна в тот же момент

    bool resizing_ = false;
    bool resizeW_ = false;
    bool resizeH_ = false;

    void BeginMove();
    void MoveTo(int screenX, int screenY);  // экранные координаты -> координаты родителя
    void MouseTick();    // очередной шаг переноса или растягивания
    void EndMouseAction();  // кнопку отпустили: магнит, сохранение, перерисовка
};

// ---------- Приложение ----------

class App : public settings::Host {
public:
    bool Start(HINSTANCE inst);

    // settings::Host — окно настроек меняет данные и просит применить.
    AppData& Data() override { return data_; }
    bool HasPanel(int panel) override {
        return panel >= 0 && panel < static_cast<int>(data_.panels.size());
    }
    void Save() override { SaveData(data_); }
    void ApplyPanelLook(int panel) override {
        if (Block* block = BlockForPanel(panel)) block->Refresh();
    }
    void ApplyStyleToAll(int panel) override;
    void ApplyTopmost() override;
    void ApplyHotkey() override;
    void RefreshAll() override;
    void AddPanel(PanelKind kind) override;
    void RemovePanels(PanelKind kind) override;
    void ArrangePanels() override;

    HINSTANCE Instance() const { return inst_; }
    HWND MainWindow() const { return hwnd_; }
    bool Visible() const { return visible_; }

    Date Selected() const { return selected_; }
    Date TodayDate() const { return today_; }
    void SetSelected(const Date& date);
    void ShiftSelected(int days);

    Block* BlockForPanel(int index);
    HWND AnchorFor(int panel);  // окно блока: по нему центрируются диалоги

    // Действие внутри блока: нажали чек-бокс, стрелку, «⋮», строку дневника.
    void Activate(int panel, const Elem& elem);

    void SnapRect(const Block* self, RECT& rect);
    void SetAllCollapsed(bool collapsed);
    void ToggleVisible();
    void RenameBlock(int panel);
    void DeletePanel(int panel);

    void ShowSettings(int panel, POINT screenPt);
    void ShowGlobalSettings(POINT screenPt);
    void ShowBlockMenu(POINT screenPt, int panel);
    void ShowRowMenu(POINT screenPt, int panel, Act act, int a);
    void ShowTrayMenu(POINT screenPt);
    void RunCommand(UINT cmd);

    void UpdateTrayTip();
    LRESULT OnHub(WPARAM command);
    void SetTrayShown(bool shown);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void CreateBlocks();
    void ReindexBlocks();
    void EnsureBlocks();
    void TickClocks();
    void CheckDateRollover();

    // Модальный ввод строки: используется всеми списками и дневником.
    bool AskText(int panel, const wchar_t* title, const wchar_t* label, const wchar_t* hint,
                 std::wstring& value, bool multiline, const wchar_t* okLabel);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;  // скрытое окно приложения
    HICON icon_ = nullptr;
    AppData data_;
    std::vector<std::unique_ptr<Block>> blocks_;
    bool visible_ = true;
    bool trayShown_ = false;

    Date today_;
    Date selected_;  // общий для всех блоков выбранный день
    int lastMinute_ = -1;
    int lastSecond_ = -1;

    int ctxPanel_ = -1;
    Act ctxAct_ = Act::None;
    int ctxA_ = -1;
};

App g_app;

// Повторный запуск .exe шлёт это сообщение уже работающему виджету, чтобы тот
// показался, если был скрыт в трее.
UINT ShowMessageId() {
    static const UINT id = RegisterWindowMessageW(L"PlannerGo_Show");
    return id;
}

// ---------- Блок: раскладка ----------

const Panel& Block::Data() const { return g_app.Data().panels[static_cast<size_t>(panelIndex_)]; }
Panel& Block::MutableData() { return g_app.Data().panels[static_cast<size_t>(panelIndex_)]; }

void Block::BuildLayout() {
    BlockLayout& L = layout_;
    const Panel& panel = Data();

    L.dpi = dpi_;
    L.shadow = Sc(kShadow, dpi_);
    const int pad = Sc(kPad, dpi_);
    const int headerH = Sc(kHeaderH, dpi_);

    int width = panel.width;
    int height = panel.height;
    if (width <= 0 || height <= 0) {
        int dw = 0, dh = 0;
        PanelDefaultSize(panel.kind, dw, dh);
        if (width <= 0) width = dw;
        if (height <= 0) height = dh;
    }
    L.cardW = Sc(ClampInt(width, kMinW, kMaxW), dpi_);

    const int cardLeft = L.shadow;
    const int cardTop = L.shadow;
    L.header = {cardLeft + pad, cardTop + Sc(5, dpi_), cardLeft + L.cardW - pad,
                cardTop + Sc(5, dpi_) + headerH};

    // В узком блоке для двух кнопок места нет — оставляем «⋮», остальное в меню.
    const int hb = Sc(kHeaderBtn, dpi_);
    const int hbTop = L.header.top + (headerH - hb) / 2;
    const int step = hb + Sc(2, dpi_);
    L.compactHeader = (L.header.right - L.header.left) < Sc(kWideHeaderW, dpi_);
    L.menuBtn = {L.header.right - hb, hbTop, L.header.right, hbTop + hb};
    if (L.compactHeader) {
        L.gearBtn = {0, 0, 0, 0};
        L.addBtn = {0, 0, 0, 0};
    } else {
        L.gearBtn = {L.menuBtn.left - step, hbTop, L.menuBtn.left - step + hb, hbTop + hb};
        if (CanAdd(panel.kind)) {
            L.addBtn = {L.gearBtn.left - step, hbTop, L.gearBtn.left - step + hb, hbTop + hb};
        } else {
            L.addBtn = {0, 0, 0, 0};
        }
    }

    if (panel.collapsed) {
        L.viewport = {L.header.left, L.header.bottom, L.header.right, L.header.bottom};
        L.cardH = L.header.bottom + Sc(5, dpi_) - cardTop;
    } else {
        L.cardH = Sc(ClampInt(height, kMinH, kMaxH), dpi_);
        L.viewport = {cardLeft + pad, L.header.bottom + Sc(4, dpi_), cardLeft + L.cardW - pad,
                      cardTop + L.cardH - pad};
        if (L.viewport.bottom < L.viewport.top) L.viewport.bottom = L.viewport.top;
    }

    L.card = {cardLeft, cardTop, cardLeft + L.cardW, cardTop + L.cardH};
    L.winW = L.cardW + L.shadow * 2;
    L.winH = L.cardH + L.shadow * 2;

    ClampScroll();

    // Уголок и края для изменения размера мышью.
    const int grip = Sc(kGrip, dpi_);
    L.resizeCorner = {L.card.right - grip * 2, L.card.bottom - grip * 2, L.card.right, L.card.bottom};
    L.resizeRight = {L.card.right - grip, L.header.top, L.card.right, L.card.bottom - grip * 2};
    L.resizeBottom = {L.card.left + grip, L.card.bottom - grip, L.card.right - grip * 2, L.card.bottom};
}

void Block::ClampScroll() {
    if (!scrollable_) {
        scroll_ = 0;
        return;
    }
    const int viewH = layout_.viewport.bottom - layout_.viewport.top;
    const int maxScroll = contentH_ > viewH ? contentH_ - viewH : 0;
    if (scroll_ > maxScroll) scroll_ = maxScroll;
    if (scroll_ < 0) scroll_ = 0;
}

// ---------- Блок: поверхность ----------

void Block::ReleaseSurface() {
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

void Block::EnsureSurface(int w, int h) {
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

// ---------- Блок: отрисовка ----------

void Block::DrawHeader(Graphics& g) {
    const BlockLayout& L = layout_;
    const Panel& panel = Data();
    const RectF headerF = ToRectF(L.header);
    const float u = ScF(1.0f, dpi_);
    const bool headerHot = hover_.zone == Zone::Header;

    {
        // Треугольник — акцентного цвета: по нему блок и узнаётся издалека.
        const float cx = headerF.X + ScF(9.0f, dpi_);
        const float cy = headerF.Y + headerF.Height / 2.0f;
        const float s = ScF(4.0f, dpi_);
        SolidBrush brush(Color(headerHot ? P().accent : WithAlpha(P().accent, 0xCC)));
        PointF tri[3];
        if (panel.collapsed) {
            tri[0] = PointF(cx - s * 0.6f, cy - s);
            tri[1] = PointF(cx + s * 0.8f, cy);
            tri[2] = PointF(cx - s * 0.6f, cy + s);
        } else {
            tri[0] = PointF(cx - s, cy - s * 0.6f);
            tri[1] = PointF(cx + s, cy - s * 0.6f);
            tri[2] = PointF(cx, cy + s * 0.8f);
        }
        g.FillPolygon(&brush, tri, 3);
    }

    const float titleX = headerF.X + ScF(20.0f, dpi_);
    const float titleY = headerF.Y + (headerF.Height - ScF(15.0f, dpi_)) / 2.0f;
    const float titleRight = static_cast<float>(L.compactHeader ? L.menuBtn.left
                                                : (L.addBtn.right > 0 ? L.addBtn.left : L.gearBtn.left));
    const float titleMax = titleRight - titleX - ScF(30.0f, dpi_);
    const std::wstring title = gfx::Ellipsize(g, panel.Title(), *fonts_.title, titleMax);
    gfx::DrawText(g, title.c_str(), *fonts_.title, P().textPrimary, titleX, titleY);

    const int count = HeaderCount(g_app.Data(), panel, g_app.Selected());
    if (count >= 0 && !title.empty()) {
        // Счётчик в маленькой «пилюле», чтобы не сливался с названием.
        wchar_t countText[16];
        swprintf(countText, 16, L"%d", count);
        const float textW = gfx::TextWidth(g, countText, *fonts_.count);
        const float h = ScF(16.0f, dpi_);
        const float w = max(textW + ScF(10.0f, dpi_), h);
        RectF pill(titleX + gfx::TextWidth(g, title.c_str(), *fonts_.title) + ScF(8.0f, dpi_),
                   headerF.Y + (headerF.Height - h) / 2.0f, w, h);
        if (pill.GetRight() < titleRight) {
            gfx::FillRoundRect(g, pill, h / 2.0f, WithAlpha(P().accent, 0x2E));
            gfx::DrawText(g, countText, *fonts_.count, P().accent, pill.X + pill.Width / 2.0f,
                          pill.Y + (h - ScF(13.0f, dpi_)) / 2.0f, gfx::Align::Center);
        }
    }

    {
        // Кнопки заголовка видны всегда — иначе о них никто не догадается.
        enum class Glyph { Plus, Gear, Dots };
        auto headerButton = [&](const RECT& r, Glyph glyph, bool hot) {
            const RectF rf = ToRectF(r);
            if (hot) gfx::FillRoundRect(g, rf, rf.Width / 2.0f, WithAlpha(P().accent, 0x33));
            const float cx = rf.X + rf.Width / 2.0f;
            const float cy = rf.Y + rf.Height / 2.0f;
            Color c(hot ? P().accent : P().textFaint);
            Pen pen(c, ScF(1.5f, dpi_));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            switch (glyph) {
                case Glyph::Plus:
                    g.DrawLine(&pen, cx - 4.5f * u, cy, cx + 4.5f * u, cy);
                    g.DrawLine(&pen, cx, cy - 4.5f * u, cx, cy + 4.5f * u);
                    break;
                case Glyph::Gear: {
                    // Шестерёнка: кольцо и восемь зубцов по кругу.
                    const float rIn = 2.6f * u;
                    const float rTooth = 5.6f * u;
                    g.DrawEllipse(&pen, cx - rIn, cy - rIn, rIn * 2, rIn * 2);
                    Pen teeth(c, ScF(1.4f, dpi_));
                    teeth.SetStartCap(LineCapRound);
                    teeth.SetEndCap(LineCapRound);
                    for (int i = 0; i < 8; i++) {
                        const float a = 3.14159265f / 4.0f * i;
                        const float dx = cosf(a), dy = sinf(a);
                        g.DrawLine(&teeth, cx + dx * (rIn + 1.2f * u), cy + dy * (rIn + 1.2f * u),
                                   cx + dx * rTooth, cy + dy * rTooth);
                    }
                    break;
                }
                case Glyph::Dots: {
                    SolidBrush dots(c);
                    for (int i = -1; i <= 1; i++) {
                        g.FillEllipse(&dots, cx - 1.2f * u, cy + i * 4.0f * u - 1.2f * u, 2.4f * u, 2.4f * u);
                    }
                    break;
                }
            }
        };
        if (!L.compactHeader) {
            if (L.addBtn.right > 0) headerButton(L.addBtn, Glyph::Plus, hover_.zone == Zone::HeaderAdd);
            headerButton(L.gearBtn, Glyph::Gear, hover_.zone == Zone::HeaderSettings);
        }
        headerButton(L.menuBtn, Glyph::Dots, hover_.zone == Zone::HeaderMenu);
    }

    if (!panel.collapsed) {
        // Акцентная линия под заголовком: слева насыщенная, справа тает.
        RectF line(headerF.X, static_cast<float>(L.header.bottom), headerF.Width, 1.5f * u);
        gfx::FadeLine(g, line, WithAlpha(P().accent, 0xB0));
    }
}

void Block::Render() {
    const Panel& panel = Data();
    // Оформление у каждого блока своё, поэтому палитру ставим перед каждой отрисовкой.
    SetTheme(panel.darkTheme, panel.accent, panel.opacity);
    // Шрифт общий: сменили его в настройках — блок перестраивает свои шрифты сам.
    if (fonts_.fontIndex != g_app.Data().fontIndex || fonts_.dpi != dpi_) {
        fonts_.Rebuild(dpi_, g_app.Data().fontIndex);
    }

    BuildLayout();
    const BlockLayout& L = layout_;

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

    const RectF cardRect = ToRectF(L.card);
    const float radius = ScF(kCardRadius, dpi_);
    const float u = ScF(1.0f, dpi_);

    // Прозрачность блока — это прозрачность фона: текст и диаграммы всегда чёткие,
    // поэтому гасим сами цвета карточки, а не всё окно целиком.
    //
    // У прозрачности есть пол. Совсем прозрачная карточка светлой темы показывает обои
    // насквозь, и тёмный текст на тёмных обоях перестаёт читаться, поэтому ползунок
    // 40..100 % раскладывается в альфу от «самого прозрачного из читаемых» до полной.
    // Светлой теме нужен фон плотнее: тёмный текст теряется быстрее светлого.
    const int floorOp = P().dark ? 52 : 70;
    const int op = floorOp + (100 - floorOp) * (panel.opacity - 40) / 60;
    const ARGB bgTop = Fade(Mix(P().cardBgTop, P().accent, 0.10f), op);
    const ARGB bgBottom = Fade(Mix(P().cardBg, P().accent, 0.04f), op);
    const ARGB border = Fade(Mix(P().cardBorder, P().accent, 0.5f), op);

    gfx::DropShadow(g, cardRect, radius, L.shadow, op);
    gfx::FillRoundRectGradient(g, cardRect, radius, bgTop, bgBottom);
    gfx::StrokeRoundRect(g, cardRect, radius, border, u);
    gfx::TopHighlight(g, cardRect, radius, Fade(P().cardGloss, op), u);

    DrawHeader(g);

    {
        // Уголок «потяни меня»: три чёрточки в правом нижнем углу карточки.
        const bool hotGrip = hover_.zone == Zone::ResizeWH || hover_.zone == Zone::ResizeW ||
                             hover_.zone == Zone::ResizeH;
        Color c(hotGrip ? P().accent : P().textFaint);
        Pen pen(c, ScF(1.4f, dpi_));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        const float x = static_cast<float>(L.card.right) - ScF(6.0f, dpi_);
        const float y = static_cast<float>(L.card.bottom) - ScF(6.0f, dpi_);
        for (int i = 0; i < 3; i++) {
            const float d = (3.0f + i * 3.5f) * u;
            g.DrawLine(&pen, x - d, y, x, y - d);
        }
    }

    elems_.clear();
    if (!panel.collapsed) {
        g.SetClip(Rect(L.viewport.left, L.viewport.top, L.viewport.right - L.viewport.left,
                       L.viewport.bottom - L.viewport.top));

        PanelCtx ctx;
        ctx.g = &g;
        ctx.data = &g_app.Data();
        ctx.panel = &panel;
        ctx.selected = g_app.Selected();
        ctx.today = g_app.TodayDate();
        GetLocalTime(&ctx.now);
        ctx.dpi = dpi_;
        ctx.viewport = L.viewport;
        ctx.body = {L.viewport.left, L.viewport.top - scroll_, L.viewport.right, L.viewport.bottom};
        ctx.fonts.family = fonts_.family.get();
        ctx.fonts.row = fonts_.row.get();
        ctx.fonts.small = fonts_.small.get();
        ctx.fonts.tiny = fonts_.tiny.get();
        ctx.fonts.num = fonts_.num.get();
        ctx.hover = hover_.zone == Zone::Content ? hover_.hover : Hover{};
        ctx.elems = &elems_;
        PaintPanel(ctx);

        contentH_ = ctx.contentH;
        scrollable_ = ctx.scrollable;
        g.ResetClip();

        // Полоса прокрутки — только когда содержимое не помещается.
        const int viewH = L.viewport.bottom - L.viewport.top;
        if (scrollable_ && contentH_ > viewH) {
            const float trackX = static_cast<float>(L.viewport.right) - 3 * u;
            const float trackH = static_cast<float>(viewH);
            const float thumbH = trackH * static_cast<float>(viewH) / static_cast<float>(contentH_);
            const float offset = (trackH - thumbH) * static_cast<float>(scroll_) /
                                 static_cast<float>(contentH_ - viewH);
            gfx::FillRoundRect(g, RectF(trackX, static_cast<float>(L.viewport.top), 3 * u, trackH), 1.5f * u,
                               P().separator);
            gfx::FillRoundRect(g, RectF(trackX, static_cast<float>(L.viewport.top) + offset, 3 * u, thumbH),
                               1.5f * u, P().textFaint);
        }
    } else {
        contentH_ = 0;
        scrollable_ = false;
    }

    // --- Перенос на экран ---
    POINT src = {0, 0};
    SIZE size = {L.winW, L.winH};
    // Прозрачность уже учтена в цветах фона, поэтому окно выводим как есть.
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, screen, nullptr, &size, memDc_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void Block::Refresh() {
    BuildLayout();
    Render();
}

// ---------- Блок: попадания ----------

Hit Block::HitTest(POINT pt) const {
    const BlockLayout& L = layout_;
    // Края и уголок — размер блока тянут мышью, поэтому они важнее содержимого.
    // У закреплённого блока их нет: он не двигается и не меняет размер.
    if (!Data().locked) {
        if (PtInRect(&L.resizeCorner, pt)) return {Zone::ResizeWH, Hover{}, -1};
        if (PtInRect(&L.resizeRight, pt)) return {Zone::ResizeW, Hover{}, -1};
        if (!Data().collapsed && PtInRect(&L.resizeBottom, pt)) return {Zone::ResizeH, Hover{}, -1};
    }
    if (L.addBtn.right > 0 && PtInRect(&L.addBtn, pt)) return {Zone::HeaderAdd, Hover{}, -1};
    if (L.gearBtn.right > 0 && PtInRect(&L.gearBtn, pt)) return {Zone::HeaderSettings, Hover{}, -1};
    if (PtInRect(&L.menuBtn, pt)) return {Zone::HeaderMenu, Hover{}, -1};
    if (PtInRect(&L.header, pt)) return {Zone::Header, Hover{}, -1};
    if (PtInRect(&L.viewport, pt)) {
        // С конца: то, что нарисовано поверх (кнопка «⋮»), и нажимается первым.
        for (size_t k = elems_.size(); k > 0; k--) {
            const Elem& elem = elems_[k - 1];
            if (PtInRect(&elem.rect, pt)) {
                return {Zone::Content, Hover{elem.act, elem.a, elem.b}, static_cast<int>(k - 1)};
            }
        }
    }
    return {Zone::None, Hover{}, -1};
}

void Block::ApplyResize(POINT pt) {
    Panel& panel = MutableData();
    bool changed = false;

    if (resizeW_) {
        const int px = static_cast<int>(pt.x) - static_cast<int>(layout_.card.left);
        const int value = ClampInt(MulDiv(px, 96, dpi_), kMinW, kMaxW);
        if (value != panel.width) {
            panel.width = value;
            changed = true;
        }
    }
    if (resizeH_ && !panel.collapsed) {
        const int px = static_cast<int>(pt.y) - static_cast<int>(layout_.card.top);
        const int value = ClampInt(MulDiv(px, 96, dpi_), kMinH, kMaxH);
        if (value != panel.height) {
            panel.height = value;
            changed = true;
        }
    }
    if (changed) Refresh();
}

// ---------- Блок: окно ----------

// Блок живёт в одном из двух состояний:
//   «на рабочем столе» — дочернее окно хозяина рабочего стола. Тогда он и есть часть рабочего стола:
//      никогда не оказывается над чужими окнами (даже когда по столу кликнули мышью или
//      это второй монитор), но виден вместе со столом, в том числе по Win+D;
//   «поверх всех окон» — обычное окно верхнего уровня с HWND_TOPMOST.
void Block::ApplyTopmost() {
    if (!hwnd_) return;
    const bool onTop = g_app.Data().alwaysOnTop;
    HWND desktop = onTop ? nullptr : DesktopHostWindow();
    const HWND parent = GetParent(hwnd_);

    if (desktop) {
        if (parent != desktop) {
            Reparent(desktop);
            PlaceWindow();  // координаты теперь считаются от клиентской области стола
            Render();
        }
        // Внутри рабочего стола держимся поверх значков, но ниже любого обычного окна.
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    if (parent) {
        Reparent(nullptr);
        PlaceWindow();
        Render();
    }
    SetWindowPos(hwnd_, onTop ? HWND_TOPMOST : HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Переезд между рабочим столом и верхним уровнем. Тонкость: у окна, сменившего родителя,
// слоёная поверхность перестаёт показываться, пока WS_EX_LAYERED не выставлен заново —
// иначе блок выглядит как исчезнувший, хотя окно на месте и «видимо».
void Block::Reparent(HWND parent) {
    const bool toDesktop = parent != nullptr;
    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    // Именно WS_CHILD, а не WS_POPUP с родителем: гибрид система перерисовывает рывками,
    // и полупрозрачный блок заметно мерцает поверх обоев.
    style = toDesktop ? ((style & ~WS_POPUP) | WS_CHILD) : ((style & ~WS_CHILD) | WS_POPUP);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
    SetParent(hwnd_, parent);

    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (g_app.Visible()) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

// Проводник мог перезапуститься (старый стол уничтожен вместе с нашими окнами — их
// пересоздаёт App::EnsureBlocks) или перестроить рабочий стол после Snap Assist и Win+Tab:
// хозяин значков сменился, и блок нужно перевесить на новый — иначе он под обоями.
void Block::KeepAttached() {
    if (!hwnd_ || moving_ || resizing_) return;
    if (g_app.Data().alwaysOnTop) return;
    HWND desktop = DesktopHostWindow();
    if (desktop && GetParent(hwnd_) != desktop) ApplyTopmost();
}

void Block::BeginMove() {
    if (Data().locked) return;
    moving_ = true;
    GetCursorPos(&dragCursor_);
    RECT r{};
    GetWindowRect(hwnd_, &r);
    dragOrigin_ = {r.left, r.top};
    SetCapture(hwnd_);
    SetTimer(hwnd_, kMouseTimerId, kMouseTimerMs, nullptr);
}

// Шаг переноса или растягивания. Курсор и кнопку читаем напрямую: сообщения о движении
// приходят только пока курсор над блоком, а при переносе он легко оказывается за его краем.
void Block::MouseTick() {
    if (!moving_ && !resizing_ && !headerPending_) {
        EndMouseAction();
        return;
    }
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        // Кнопку отпустили за пределами блока: если это был просто клик по заголовку,
        // сворачиванием займётся WM_LBUTTONUP, сюда мы попадём уже после него.
        EndMouseAction();
        return;
    }

    if (headerPending_) {
        // Нажали на заголовок: пока не сдвинули дальше порога — это клик «свернуть».
        POINT cursor;
        GetCursorPos(&cursor);
        const int threshold = Sc(kDragThreshold, dpi_);
        const int dx = cursor.x - dragCursor_.x;
        const int dy = cursor.y - dragCursor_.y;
        if (dx * dx + dy * dy < threshold * threshold) return;
        headerPending_ = false;
        BeginMove();
        return;
    }

    POINT cursor;
    GetCursorPos(&cursor);
    if (moving_) {
        MoveTo(dragOrigin_.x + (cursor.x - dragCursor_.x), dragOrigin_.y + (cursor.y - dragCursor_.y));
        return;
    }
    POINT local = cursor;
    ScreenToClient(hwnd_, &local);
    ApplyResize(local);
}

void Block::EndMouseAction() {
    KillTimer(hwnd_, kMouseTimerId);
    const bool wasMoving = moving_;
    const bool wasResizing = resizing_;
    moving_ = false;
    resizing_ = false;
    headerPending_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();

    if (wasMoving) {
        // Магнит срабатывает не во время движения, а в момент, когда кнопку отпустили:
        // блок тащится совершенно свободно, а встаёт ровно.
        if (g_app.Data().snapToGrid && !Data().locked) {
            RECT current{};
            GetWindowRect(hwnd_, &current);
            RECT snapped = current;
            g_app.SnapRect(this, snapped);
            if (snapped.left != current.left || snapped.top != current.top) {
                MoveTo(snapped.left, snapped.top);
            }
        }
        SavePosition();
    }
    if (wasResizing) g_app.Save();
    if (!wasMoving && !wasResizing) return;

    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    hover_ = HitTest(pt);
    Render();
}

// Позиция всегда хранится и считается в экранных координатах, а SetWindowPos у дочернего
// окна ждёт координаты родителя — переводим здесь, в одном месте.
void Block::MoveTo(int screenX, int screenY) {
    POINT origin = {0, 0};
    if (HWND parent = GetParent(hwnd_)) ClientToScreen(parent, &origin);
    SetWindowPos(hwnd_, nullptr, screenX - origin.x, screenY - origin.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Block::Show(bool visible) {
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible) {
        ApplyTopmost();
        Render();
    }
}

void Block::SavePosition() {
    RECT r{};
    GetWindowRect(hwnd_, &r);
    Panel& panel = MutableData();
    if (panel.x != r.left || panel.y != r.top) {
        panel.x = r.left;
        panel.y = r.top;
        g_app.Save();
    }
}

void Block::PlaceWindow() {
    BuildLayout();
    Panel& panel = MutableData();
    int x = panel.x;
    int y = panel.y;

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
            // Первый блок — в правом верхнем углу, каждый следующий чуть ниже и левее.
            x = mi.rcWork.right - layout_.winW - Sc(24, dpi_) - panelIndex_ * Sc(18, dpi_);
            y = mi.rcWork.top + Sc(24, dpi_) + panelIndex_ * Sc(30, dpi_);
        } else {
            x = 100 + panelIndex_ * 24;
            y = 100 + panelIndex_ * 24;
        }
    }

    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromPoint(POINT{x + layout_.winW / 2, y + layout_.winH / 2},
                                    MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(mon, &mi)) {
        if (x + layout_.winW > mi.rcWork.right) x = mi.rcWork.right - layout_.winW;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + layout_.winH > mi.rcWork.bottom) y = mi.rcWork.bottom - layout_.winH;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    panel.x = x;
    panel.y = y;

    POINT origin = {0, 0};
    if (HWND parent = GetParent(hwnd_)) ClientToScreen(parent, &origin);
    SetWindowPos(hwnd_, nullptr, x - origin.x, y - origin.y, layout_.winW, layout_.winH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT Block::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                const wchar_t* cursor = IDC_ARROW;
                switch (hover_.zone) {
                    case Zone::ResizeW: cursor = IDC_SIZEWE; break;
                    case Zone::ResizeH: cursor = IDC_SIZENS; break;
                    case Zone::ResizeWH: cursor = IDC_SIZENWSE; break;
                    case Zone::None: break;
                    case Zone::Content: cursor = hover_.hover.act == Act::None ? IDC_ARROW : IDC_HAND; break;
                    default: cursor = IDC_HAND; break;
                }
                SetCursor(LoadCursorW(nullptr, cursor));
                return TRUE;
            }
            break;

        case WM_MOUSEMOVE: {
            if (!mouseTracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                mouseTracking_ = true;
            }
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

            if (resizing_) {
                ApplyResize(pt);
                return 0;
            }

            // Перенос, растягивание и порог сдвига ведёт таймер — здесь только подсветка.
            if (moving_ || resizing_ || headerPending_) return 0;

            const Hit hit = HitTest(pt);
            if (hit != hover_) {
                hover_ = hit;
                Render();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            if (hover_.zone != Zone::None) {
                hover_ = Hit{};
                Render();
            }
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            pressPt_ = pt;
            const Hit hit = HitTest(pt);
            hover_ = hit;
            pressElem_ = -1;
            switch (hit.zone) {
                case Zone::ResizeW:
                case Zone::ResizeH:
                case Zone::ResizeWH:
                    resizing_ = true;
                    resizeW_ = (hit.zone != Zone::ResizeH);
                    resizeH_ = (hit.zone != Zone::ResizeW);
                    SetCapture(hwnd);
                    SetTimer(hwnd, kMouseTimerId, kMouseTimerMs, nullptr);
                    return 0;
                case Zone::HeaderAdd: {
                    // «+» в заголовке добавляет то, что уместно для типа блока.
                    Elem add;
                    switch (Data().kind) {
                        case PanelKind::DayTasks: add.act = Act::AddTask; break;
                        case PanelKind::WeekGoals: add.act = Act::AddGoal; break;
                        case PanelKind::Habits: add.act = Act::AddHabit; break;
                        default: return 0;
                    }
                    g_app.Activate(panelIndex_, add);
                    return 0;
                }
                case Zone::HeaderSettings: {
                    POINT screen = pt;
                    ClientToScreen(hwnd, &screen);
                    g_app.ShowSettings(panelIndex_, screen);
                    return 0;
                }
                case Zone::HeaderMenu: {
                    POINT screen = pt;
                    ClientToScreen(hwnd, &screen);
                    g_app.ShowBlockMenu(screen, panelIndex_);
                    return 0;
                }
                case Zone::Header:
                    // Клик — свернуть/развернуть, но если потянуть — перенос блока.
                    // Порог сдвига тоже сторожит таймер: курсор уходит за край блока
                    // быстрее, чем успевает прийти сообщение о движении.
                    headerPending_ = true;
                    GetCursorPos(&dragCursor_);
                    SetCapture(hwnd);
                    SetTimer(hwnd, kMouseTimerId, kMouseTimerMs, nullptr);
                    return 0;
                case Zone::Content:
                    // Срабатывает по отпусканию: с нажатой кнопкой ещё можно передумать.
                    pressElem_ = hit.elem;
                    SetCapture(hwnd);
                    return 0;
                default:
                    break;
            }
            // Пустое место карточки — тащим блок, если он не закреплён.
            BeginMove();
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (moving_ || resizing_) {
                EndMouseAction();
                return 0;
            }
            KillTimer(hwnd, kMouseTimerId);  // сторож порога сдвига больше не нужен
            const bool wasHeader = headerPending_;
            const int elem = pressElem_;

            headerPending_ = false;
            pressElem_ = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (wasHeader) {
                MutableData().collapsed = !Data().collapsed;
                scroll_ = 0;
                hover_ = Hit{};
                g_app.Save();
                Refresh();
                return 0;
            }
            if (elem >= 0 && elem < static_cast<int>(elems_.size())) {
                // Отпустили там же, где нажали? Иначе это была неудавшаяся попытка потянуть.
                const Elem copy = elems_[static_cast<size_t>(elem)];
                if (PtInRect(&copy.rect, pt)) g_app.Activate(panelIndex_, copy);
            }
            return 0;
        }

        // Клик второй раз подряд приходит сюда, а не как WM_LBUTTONDOWN: гасим его,
        // иначе привычный двойной клик срабатывал бы дважды.
        case WM_LBUTTONDBLCLK:
            return 0;

        case WM_TIMER:
            if (wp == kMouseTimerId) {
                MouseTick();
                return 0;
            }
            break;

        // Захват мыши у неактивного окна система забирает сама, и перенос от этого
        // прерываться не должен — его ведёт таймер, пока нажата кнопка.
        case WM_CAPTURECHANGED:
            if (!moving_ && !resizing_) {
                headerPending_ = false;
                pressElem_ = -1;
            }
            return 0;

        case WM_RBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Hit hit = HitTest(pt);
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            // На строке списка правая кнопка открывает меню самой строки.
            if (hit.zone == Zone::Content) {
                Act menu = Act::None;
                switch (hit.hover.act) {
                    case Act::ToggleTask:
                    case Act::EditTask:
                    case Act::TaskMenu: menu = Act::TaskMenu; break;
                    case Act::ToggleGoal:
                    case Act::EditGoal:
                    case Act::GoalMenu: menu = Act::GoalMenu; break;
                    case Act::ToggleHabit:
                    case Act::HabitMenu: menu = Act::HabitMenu; break;
                    case Act::EditNote:
                    case Act::NoteMenu: menu = Act::NoteMenu; break;
                    default: break;
                }
                if (menu != Act::None) {
                    g_app.ShowRowMenu(screen, panelIndex_, menu, hit.hover.a);
                    return 0;
                }
            }
            g_app.ShowBlockMenu(screen, panelIndex_);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int viewH = layout_.viewport.bottom - layout_.viewport.top;
            if (!scrollable_ || contentH_ <= viewH) return 0;
            scroll_ += (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1) * Sc(40, dpi_);
            ClampScroll();
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            Render();
            hover_ = HitTest(pt);
            Render();
            return 0;
        }

        // Alt+F4 по блоку не должен «терять» его насовсем: блок — это часть планера,
        // а не документ. Прячем все блоки в трей, оттуда они возвращаются.
        case WM_CLOSE:
            g_app.ToggleVisible();
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            fonts_.Rebuild(dpi_, g_app.Data().fontIndex);
            auto* suggested = reinterpret_cast<RECT*>(lp);
            Panel& panel = MutableData();
            panel.x = suggested->left;
            panel.y = suggested->top;
            BuildLayout();
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, layout_.winW, layout_.winH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Render();
            return 0;
        }

        case WM_DISPLAYCHANGE:
            PlaceWindow();
            Render();
            return 0;

        case WM_DESTROY:
            ReleaseSurface();
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Block::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* block = reinterpret_cast<Block*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (block) return block->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Block::Create(HINSTANCE inst, int panelIndex) {
    panelIndex_ = panelIndex;

    // WS_EX_NOACTIVATE: блок принимает клики, но не забирает фокус — рабочий стол и чужие
    // окна не «мигают» активацией, когда по блоку просто ткнули мышью.
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBlockClass, L"Planner GO",
                            WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    dpi_ = WindowDpi(hwnd_);
    fonts_.Rebuild(dpi_, g_app.Data().fontIndex);

    PlaceWindow();
    ApplyTopmost();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Render();
    return true;
}

void Block::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

// ---------- Приложение ----------

Block* App::BlockForPanel(int index) {
    for (auto& block : blocks_) {
        if (block->PanelIndex() == index) return block.get();
    }
    return nullptr;
}

HWND App::AnchorFor(int panel) {
    if (Block* block = BlockForPanel(panel)) {
        if (block->Hwnd()) return block->Hwnd();
    }
    for (auto& block : blocks_) {
        if (block->Hwnd()) return block->Hwnd();
    }
    return hwnd_;
}

void App::RefreshAll() {
    for (auto& block : blocks_) block->Refresh();
    UpdateTrayTip();
}

void App::ReindexBlocks() {
    for (size_t i = 0; i < blocks_.size(); i++) blocks_[i]->SetPanelIndex(static_cast<int>(i));
}

void App::SetSelected(const Date& date) {
    if (!ValidDate(date) || date == selected_) return;
    selected_ = date;
    RefreshAll();
}

void App::ShiftSelected(int days) { SetSelected(AddDays(selected_, days)); }

bool App::AskText(int panel, const wchar_t* title, const wchar_t* label, const wchar_t* hint,
                  std::wstring& value, bool multiline, const wchar_t* okLabel) {
    DialogSpec spec;
    spec.title = title;
    spec.okLabel = okLabel;
    DialogField field;
    field.label = label;
    field.hint = hint ? hint : L"";
    field.value = value;
    field.required = !multiline;  // заметку и мысли можно очистить, оставив поле пустым
    field.multiline = multiline;
    spec.fields.push_back(field);

    // Блок — дочернее окно рабочего стола, владельцем диалога он быть не может, поэтому
    // модальность делаем руками: на время ввода блоки не принимают мышь.
    for (auto& block : blocks_) {
        if (block->Hwnd()) EnableWindow(block->Hwnd(), FALSE);
    }
    const bool ok = ShowDialog(hwnd_, AnchorFor(panel), spec);
    for (auto& block : blocks_) {
        if (block->Hwnd()) EnableWindow(block->Hwnd(), TRUE);
    }

    if (!ok) return false;
    value = spec.fields[0].value;
    return true;
}

void App::Activate(int panelIndex, const Elem& elem) {
    if (!HasPanel(panelIndex)) return;
    Panel& panel = data_.panels[static_cast<size_t>(panelIndex)];

    switch (elem.act) {
        case Act::Shift:
            ShiftSelected(elem.a);
            return;
        case Act::JumpToday:
            SetSelected(today_);
            return;
        case Act::PickDate: {
            const Date date{elem.a / 10000, (elem.a / 100) % 100, elem.a % 100};
            SetSelected(date);
            return;
        }
        case Act::ShiftMonth:
            panel.nav = ClampInt(panel.nav + elem.a, -600, 600);
            Save();
            ApplyPanelLook(panelIndex);
            return;
        case Act::MonthToday:
            panel.nav = 0;
            Save();
            ApplyPanelLook(panelIndex);
            return;

        // --- Задачи дня ---
        case Act::AddTask: {
            std::wstring text;
            if (!AskText(panelIndex, L"Новая задача", L"Что нужно сделать", FormatLong(selected_).c_str(),
                         text, false, L"Добавить")) {
                return;
            }
            Task task;
            task.text = text;
            data_.DayFor(selected_).tasks.push_back(std::move(task));
            break;
        }
        case Act::ToggleTask: {
            Day& day = data_.DayFor(selected_);
            if (elem.a < 0 || elem.a >= static_cast<int>(day.tasks.size())) return;
            Task& task = day.tasks[static_cast<size_t>(elem.a)];
            task.done = !task.done;
            break;
        }
        case Act::EditTask: {
            Day& day = data_.DayFor(selected_);
            if (elem.a < 0 || elem.a >= static_cast<int>(day.tasks.size())) return;
            std::wstring text = day.tasks[static_cast<size_t>(elem.a)].text;
            if (!AskText(panelIndex, L"Задача", L"Название", nullptr, text, false, L"Сохранить")) return;
            day.tasks[static_cast<size_t>(elem.a)].text = text;
            break;
        }
        case Act::TaskMenu:
        case Act::GoalMenu:
        case Act::HabitMenu:
        case Act::NoteMenu: {
            POINT pt;
            GetCursorPos(&pt);
            ShowRowMenu(pt, panelIndex, elem.act, elem.a);
            return;
        }

        // --- Цели недели ---
        case Act::AddGoal: {
            std::wstring text;
            if (!AskText(panelIndex, L"Новая цель недели", L"Цель", FormatWeek(MondayOf(selected_)).c_str(),
                         text, false, L"Добавить")) {
                return;
            }
            Goal goal;
            goal.text = text;
            data_.WeekFor(MondayOf(selected_)).goals.push_back(std::move(goal));
            break;
        }
        case Act::ToggleGoal: {
            Week& week = data_.WeekFor(MondayOf(selected_));
            if (elem.a < 0 || elem.a >= static_cast<int>(week.goals.size())) return;
            Goal& goal = week.goals[static_cast<size_t>(elem.a)];
            goal.done = !goal.done;
            break;
        }
        case Act::EditGoal: {
            Week& week = data_.WeekFor(MondayOf(selected_));
            if (elem.a < 0 || elem.a >= static_cast<int>(week.goals.size())) return;
            std::wstring text = week.goals[static_cast<size_t>(elem.a)].text;
            if (!AskText(panelIndex, L"Цель недели", L"Цель", nullptr, text, false, L"Сохранить")) return;
            week.goals[static_cast<size_t>(elem.a)].text = text;
            break;
        }

        // --- Привычки ---
        case Act::AddHabit: {
            std::wstring name;
            if (!AskText(panelIndex, L"Новая привычка", L"Название",
                         L"Например: зарядка 10 минут", name, false, L"Добавить")) {
                return;
            }
            Habit habit;
            habit.id = data_.nextHabitId++;
            habit.name = name;
            data_.habits.push_back(std::move(habit));
            break;
        }
        case Act::ToggleHabit: {
            const Date date = AddDays(MondayOf(selected_), elem.b);
            data_.DayFor(date).ToggleHabit(elem.a);
            break;
        }

        // --- Заметки ---
        case Act::EditNote: {
            if (elem.a < 0 || elem.a > 2) return;
            Day& day = data_.DayFor(selected_);
            std::wstring text = day.notes[elem.a];
            if (!AskText(panelIndex, L"Заметка дня", L"Текст", FormatLong(selected_).c_str(), text, true,
                         L"Сохранить")) {
                return;
            }
            day.notes[elem.a] = text;
            break;
        }

        // --- Дневник ---
        case Act::SleepMinus:
        case Act::SleepPlus: {
            Journal& journal = data_.DayFor(selected_).journal;
            journal.sleep = ClampInt(journal.sleep + (elem.act == Act::SleepPlus ? 30 : -30), 0, 24 * 60);
            break;
        }
        case Act::SetEnergy: {
            Journal& journal = data_.DayFor(selected_).journal;
            journal.energy = (journal.energy == elem.a) ? 0 : elem.a;  // повторный клик снимает отметку
            break;
        }
        case Act::SetMood: {
            Journal& journal = data_.DayFor(selected_).journal;
            journal.mood = (journal.mood == elem.a) ? 0 : elem.a;
            break;
        }
        case Act::EditLesson:
        case Act::EditGratitude:
        case Act::EditThoughts: {
            Journal& journal = data_.DayFor(selected_).journal;
            std::wstring* target = &journal.thoughts;
            const wchar_t* title = L"Мысли";
            if (elem.act == Act::EditLesson) {
                target = &journal.lesson;
                title = L"Урок дня";
            } else if (elem.act == Act::EditGratitude) {
                target = &journal.gratitude;
                title = L"Благодарность";
            }
            std::wstring text = *target;
            if (!AskText(panelIndex, title, L"Запись", FormatLong(selected_).c_str(), text, true,
                         L"Сохранить")) {
                return;
            }
            *target = text;
            break;
        }

        default:
            return;
    }

    // Любое изменение данных меняет и диаграммы соседних блоков — перерисовываем все.
    Save();
    RefreshAll();
}

void App::ApplyStyleToAll(int panel) {
    if (!HasPanel(panel)) return;
    const Panel& source = data_.panels[static_cast<size_t>(panel)];
    for (Panel& target : data_.panels) {
        target.darkTheme = source.darkTheme;
        target.accent = source.accent;
        target.opacity = source.opacity;
    }
    RefreshAll();
}

void App::ApplyTopmost() {
    for (auto& block : blocks_) block->ApplyTopmost();
}

void App::ApplyHotkey() {
    UnregisterHotKey(hwnd_, kHotkeyId);
    if (!data_.hotkeyEnabled) return;
    // Комбинацию мог занять кто-то другой — тогда просто живём без неё.
    RegisterHotKey(hwnd_, kHotkeyId, MOD_CONTROL | MOD_ALT, 'P');
}

void App::AddPanel(PanelKind kind) {
    Panel panel;
    panel.kind = kind;
    panel.darkTheme = data_.darkTheme;
    panel.accent = data_.accent;
    panel.opacity = data_.opacity;
    data_.panels.push_back(panel);

    auto block = std::unique_ptr<Block>(new Block());
    if (!block->Create(inst_, static_cast<int>(data_.panels.size()) - 1)) {
        data_.panels.pop_back();
        return;
    }
    blocks_.push_back(std::move(block));
    Save();
    UpdateTrayTip();
}

void App::RemovePanels(PanelKind kind) {
    for (int i = static_cast<int>(data_.panels.size()) - 1; i >= 0; i--) {
        if (data_.panels[static_cast<size_t>(i)].kind == kind) DeletePanel(i);
    }
}

void App::DeletePanel(int panel) {
    if (!HasPanel(panel)) return;
    data_.panels.erase(data_.panels.begin() + panel);
    for (size_t i = 0; i < blocks_.size(); i++) {
        if (blocks_[i]->PanelIndex() != panel) continue;
        blocks_[i]->Close();
        blocks_.erase(blocks_.begin() + static_cast<long>(i));
        break;
    }
    ReindexBlocks();
    Save();
    RefreshAll();
}

void App::RenameBlock(int panel) {
    if (!HasPanel(panel)) return;
    Panel& target = data_.panels[static_cast<size_t>(panel)];
    std::wstring title = target.Title();
    if (!AskText(panel, L"Название блока", L"Заголовок", L"Пустое поле вернёт название по умолчанию", title,
                 false, L"Сохранить")) {
        return;
    }
    target.title = (title == PanelKindName(target.kind)) ? std::wstring() : title;
    Save();
    ApplyPanelLook(panel);
}

// Расставляет блоки сеткой по рабочей области — спасение, когда окна расползлись.
void App::ArrangePanels() {
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromWindow(AnchorFor(0), MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi)) return;

    const int gap = 8;
    int x = mi.rcWork.left + gap;
    int y = mi.rcWork.top + gap;
    int rowHeight = 0;
    for (auto& block : blocks_) {
        if (!block->Hwnd()) continue;
        RECT r{};
        GetWindowRect(block->Hwnd(), &r);
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        if (x + w > mi.rcWork.right - gap && rowHeight > 0) {
            x = mi.rcWork.left + gap;
            y += rowHeight;
            rowHeight = 0;
        }
        SetWindowPos(block->Hwnd(), nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        block->SavePosition();
        x += w;
        if (h > rowHeight) rowHeight = h;
    }
    Save();
}

void App::SnapRect(const Block* self, RECT& rect) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int threshold = 24;  // притягиваем щедро: во время движения это уже не мешает
    const int gap = 12;        // ровный промежуток между соседними блоками

    const int x = rect.left;
    const int y = rect.top;
    int bestX = x, bestY = y;
    int bestDx = threshold + 1, bestDy = threshold + 1;

    auto tryX = [&](int candidate) {
        const int distance = candidate > x ? candidate - x : x - candidate;
        if (distance < bestDx) {
            bestDx = distance;
            bestX = candidate;
        }
    };
    auto tryY = [&](int candidate) {
        const int distance = candidate > y ? candidate - y : y - candidate;
        if (distance < bestDy) {
            bestDy = distance;
            bestY = candidate;
        }
    };

    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromPoint(POINT{x + width / 2, y + height / 2}, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(mon, &mi)) {
        tryX(static_cast<int>(mi.rcWork.left));
        tryX(static_cast<int>(mi.rcWork.right) - width);
        tryY(static_cast<int>(mi.rcWork.top));
        tryY(static_cast<int>(mi.rcWork.bottom) - height);
    }

    for (auto& block : blocks_) {
        if (block.get() == self || !block->Hwnd()) continue;
        RECT other{};
        GetWindowRect(block->Hwnd(), &other);
        const int left = static_cast<int>(other.left);
        const int right = static_cast<int>(other.right);
        const int top = static_cast<int>(other.top);
        const int bottom = static_cast<int>(other.bottom);

        tryX(left);                // левые края вровень
        tryX(right - width);       // правые края вровень
        tryX(right + gap);         // рядом справа, через ровный зазор
        tryX(left - width - gap);  // рядом слева
        tryY(top);
        tryY(bottom - height);
        tryY(bottom + gap);
        tryY(top - height - gap);
    }

    rect.left = (bestDx <= threshold) ? bestX : x;
    rect.top = (bestDy <= threshold) ? bestY : y;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
}

void App::SetAllCollapsed(bool collapsed) {
    for (Panel& panel : data_.panels) panel.collapsed = collapsed;
    Save();
    RefreshAll();
}

void App::ToggleVisible() {
    visible_ = !visible_;
    for (auto& block : blocks_) block->Show(visible_);
}

// ---------- Меню ----------

void App::ShowSettings(int panel, POINT screenPt) {
    if (!HasPanel(panel)) return;
    // Окно настроек ставим рядом с курсором, чтобы не закрывало сам блок.
    POINT where = {screenPt.x + 20, screenPt.y};
    settings::Show(inst_, this, panel, where, settings::Mode::Panel);
}

// Общие настройки: блоки на экране, шрифт, поведение, данные. Про конкретный блок здесь
// ничего нет — он настраивается шестерёнкой в своём заголовке.
void App::ShowGlobalSettings(POINT screenPt) {
    POINT where = {screenPt.x + 20, screenPt.y};
    settings::Show(inst_, this, HasPanel(ctxPanel_) ? ctxPanel_ : 0, where, settings::Mode::Global);
}

namespace {

// Подменю «Добавить блок»: все девять типов с галочками у уже открытых.
HMENU BuildAddMenu(const AppData& data) {
    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < static_cast<int>(PanelKind::Count); i++) {
        const PanelKind kind = static_cast<PanelKind>(i);
        const int count = data.PanelCount(kind);
        std::wstring label = PanelKindName(kind);
        if (count > 0) label += L"  ✓";
        AppendMenuW(menu, MF_STRING, kCmdAddBlockBase + static_cast<UINT>(i), label.c_str());
    }
    return menu;
}

}  // namespace

void App::ShowBlockMenu(POINT screenPt, int panel) {
    if (!HasPanel(panel)) return;
    ctxPanel_ = panel;
    ctxAct_ = Act::None;
    ctxA_ = -1;
    const Panel& target = data_.panels[static_cast<size_t>(panel)];

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdRenameBlock, L"Переименовать блок…");
    AppendMenuW(menu, MF_STRING, kCmdToggleCollapse, target.collapsed ? L"Развернуть" : L"Свернуть");
    AppendMenuW(menu, MF_STRING | (target.locked ? MF_CHECKED : 0), kCmdToggleLock,
                L"Закрепить блок (не двигать)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildAddMenu(data_)), L"Добавить блок");
    AppendMenuW(menu, MF_STRING | (data_.panels.size() > 1 ? 0 : MF_GRAYED), kCmdDeleteBlock,
                L"Убрать этот блок");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdToday, L"Перейти к сегодня");
    AppendMenuW(menu, MF_STRING, kCmdArrange, L"Расставить блоки по экрану");
    AppendMenuW(menu, MF_STRING, kCmdCollapseAll, L"Свернуть все блоки");
    AppendMenuW(menu, MF_STRING, kCmdExpandAll, L"Развернуть все блоки");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"Настройки блока…");
    AppendMenuW(menu, MF_STRING, kCmdGlobalSettings, L"Настройки планера…");
    AppendMenuW(menu, MF_STRING, kCmdToggleVisible, L"Скрыть все блоки");
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Закрыть виджет");

    // Без этого меню не закрывается по клику мимо окна.
    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                      screenPt.x, screenPt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    RunCommand(cmd);
}

void App::ShowRowMenu(POINT screenPt, int panel, Act act, int a) {
    if (!HasPanel(panel)) return;
    ctxPanel_ = panel;
    ctxAct_ = act;
    ctxA_ = a;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    switch (act) {
        case Act::TaskMenu:
            AppendMenuW(menu, MF_STRING, kCmdRowEdit, L"Изменить…");
            AppendMenuW(menu, MF_STRING, kCmdRowTomorrow, L"Перенести на завтра");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCmdRowDelete, L"Удалить задачу");
            break;
        case Act::GoalMenu:
            AppendMenuW(menu, MF_STRING, kCmdRowEdit, L"Изменить…");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCmdRowDelete, L"Удалить цель");
            break;
        case Act::HabitMenu:
            AppendMenuW(menu, MF_STRING, kCmdRowEdit, L"Переименовать…");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCmdRowDelete, L"Удалить привычку");
            break;
        case Act::NoteMenu:
            AppendMenuW(menu, MF_STRING, kCmdRowEdit, L"Изменить…");
            AppendMenuW(menu, MF_STRING, kCmdRowClear, L"Очистить");
            break;
        default:
            DestroyMenu(menu);
            return;
    }

    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                      screenPt.x, screenPt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    RunCommand(cmd);
}

void App::ShowTrayMenu(POINT screenPt) {
    ctxPanel_ = 0;
    ctxAct_ = Act::None;
    ctxA_ = -1;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdToggleVisible, visible_ ? L"Скрыть все блоки" : L"Показать все блоки");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildAddMenu(data_)), L"Добавить блок");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdToday, L"Перейти к сегодня");
    AppendMenuW(menu, MF_STRING, kCmdArrange, L"Расставить блоки по экрану");
    AppendMenuW(menu, MF_STRING, kCmdCollapseAll, L"Свернуть все блоки");
    AppendMenuW(menu, MF_STRING, kCmdExpandAll, L"Развернуть все блоки");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdGlobalSettings, L"Настройки планера…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Закрыть виджет");

    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                      screenPt.x, screenPt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    RunCommand(cmd);
}

void App::RunCommand(UINT cmd) {
    if (cmd == 0) return;

    if (cmd >= kCmdAddBlockBase) {
        const int index = static_cast<int>(cmd - kCmdAddBlockBase);
        if (index >= 0 && index < static_cast<int>(PanelKind::Count)) AddPanel(static_cast<PanelKind>(index));
        return;
    }

    switch (cmd) {
        case kCmdRenameBlock: RenameBlock(ctxPanel_); break;
        case kCmdToggleCollapse:
            if (HasPanel(ctxPanel_)) {
                Panel& panel = data_.panels[static_cast<size_t>(ctxPanel_)];
                panel.collapsed = !panel.collapsed;
                Save();
                ApplyPanelLook(ctxPanel_);
            }
            break;
        case kCmdToggleLock:
            if (HasPanel(ctxPanel_)) {
                Panel& panel = data_.panels[static_cast<size_t>(ctxPanel_)];
                panel.locked = !panel.locked;
                Save();
                ApplyPanelLook(ctxPanel_);
            }
            break;
        case kCmdDeleteBlock:
            if (data_.panels.size() > 1) DeletePanel(ctxPanel_);
            break;
        case kCmdSettings: {
            POINT pt;
            GetCursorPos(&pt);
            ShowSettings(HasPanel(ctxPanel_) ? ctxPanel_ : 0, pt);
            break;
        }
        case kCmdGlobalSettings: {
            POINT pt;
            GetCursorPos(&pt);
            ShowGlobalSettings(pt);
            break;
        }
        case kCmdCollapseAll: SetAllCollapsed(true); break;
        case kCmdExpandAll: SetAllCollapsed(false); break;
        case kCmdToday: SetSelected(today_); break;
        case kCmdArrange: ArrangePanels(); break;
        case kCmdToggleVisible: ToggleVisible(); break;
        case kCmdExit: DestroyWindow(hwnd_); break;

        // --- Меню строки списка ---
        case kCmdRowEdit: {
            Elem elem;
            elem.a = ctxA_;
            switch (ctxAct_) {
                case Act::TaskMenu: elem.act = Act::EditTask; break;
                case Act::GoalMenu: elem.act = Act::EditGoal; break;
                case Act::NoteMenu: elem.act = Act::EditNote; break;
                case Act::HabitMenu: {
                    for (Habit& habit : data_.habits) {
                        if (habit.id != ctxA_) continue;
                        std::wstring name = habit.name;
                        if (AskText(ctxPanel_, L"Привычка", L"Название", nullptr, name, false,
                                    L"Сохранить")) {
                            habit.name = name;
                            Save();
                            RefreshAll();
                        }
                        break;
                    }
                    return;
                }
                default: return;
            }
            Activate(ctxPanel_, elem);
            break;
        }
        case kCmdRowDelete: {
            switch (ctxAct_) {
                case Act::TaskMenu: {
                    Day& day = data_.DayFor(selected_);
                    if (ctxA_ >= 0 && ctxA_ < static_cast<int>(day.tasks.size())) {
                        day.tasks.erase(day.tasks.begin() + ctxA_);
                    }
                    break;
                }
                case Act::GoalMenu: {
                    Week& week = data_.WeekFor(MondayOf(selected_));
                    if (ctxA_ >= 0 && ctxA_ < static_cast<int>(week.goals.size())) {
                        week.goals.erase(week.goals.begin() + ctxA_);
                    }
                    break;
                }
                case Act::HabitMenu: {
                    for (size_t i = 0; i < data_.habits.size(); i++) {
                        if (data_.habits[i].id != ctxA_) continue;
                        data_.habits.erase(data_.habits.begin() + static_cast<long>(i));
                        break;
                    }
                    // Отметки удалённой привычки больше ни на что не влияют — убираем их.
                    for (Day& day : data_.days) {
                        for (size_t k = day.habits.size(); k > 0; k--) {
                            if (day.habits[k - 1] == ctxA_) day.habits.erase(day.habits.begin() + (k - 1));
                        }
                    }
                    break;
                }
                default: return;
            }
            Save();
            RefreshAll();
            break;
        }
        case kCmdRowTomorrow: {
            if (ctxAct_ != Act::TaskMenu) break;
            Day& day = data_.DayFor(selected_);
            if (ctxA_ < 0 || ctxA_ >= static_cast<int>(day.tasks.size())) break;
            Task moved = day.tasks[static_cast<size_t>(ctxA_)];
            day.tasks.erase(day.tasks.begin() + ctxA_);
            moved.done = false;
            data_.DayFor(AddDays(selected_, 1)).tasks.push_back(std::move(moved));
            Save();
            RefreshAll();
            break;
        }
        case kCmdRowClear: {
            if (ctxAct_ != Act::NoteMenu || ctxA_ < 0 || ctxA_ > 2) break;
            data_.DayFor(selected_).notes[ctxA_].clear();
            Save();
            RefreshAll();
            break;
        }
        default: break;
    }
}

// ---------- Приложение: окно ----------

void App::UpdateTrayTip() {
    const DayStats stats = StatsForDay(data_, today_);
    wchar_t tip[96];
    if (stats.total > 0) {
        swprintf(tip, 96, L"Планер GO — сегодня %d из %d", stats.done, stats.total);
    } else {
        swprintf(tip, 96, L"Планер GO — на сегодня задач нет");
    }
    TrayUpdateTip(hwnd_, tip);
}

void App::TickClocks() {
    SYSTEMTIME now;
    GetLocalTime(&now);
    const bool minuteChanged = now.wMinute != lastMinute_;
    const bool secondChanged = now.wSecond != lastSecond_;
    if (!minuteChanged && !(data_.showSeconds && secondChanged)) return;
    lastMinute_ = now.wMinute;
    lastSecond_ = now.wSecond;

    for (auto& block : blocks_) {
        if (!HasPanel(block->PanelIndex())) continue;
        if (data_.panels[static_cast<size_t>(block->PanelIndex())].kind == PanelKind::Clock) block->Render();
    }
}

void App::CheckDateRollover() {
    const Date now = Today();
    if (now == today_) return;
    // Наступил новый день: если смотрели «сегодня», уезжаем вместе с ним.
    const bool followed = (selected_ == today_);
    today_ = now;
    if (followed) selected_ = now;
    RefreshAll();
}

// ---------- Центр виджетов ----------

void App::SetTrayShown(bool shown) {
    if (shown == trayShown_) return;
    if (shown) {
        trayShown_ = TrayAdd(hwnd_, kTrayMessage, icon_, L"Планер GO");
        UpdateTrayTip();
    } else {
        TrayRemove(hwnd_);
        trayShown_ = false;
    }
}

LRESULT App::OnHub(WPARAM command) {
    switch (command) {
        case hub::kShow: if (!visible_) ToggleVisible(); break;
        case hub::kHide: if (visible_) ToggleVisible(); break;
        case hub::kToggle: ToggleVisible(); break;
        case hub::kSettings: {
            POINT pt;
            GetCursorPos(&pt);
            ShowGlobalSettings(pt);
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

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == ShowMessageId()) {
        if (!visible_) ToggleVisible();
        return 0;
    }
    if (msg == hub::MessageId()) return OnHub(wp);

    switch (msg) {
        case WM_TIMER:
            if (wp != kTimerId) break;
            EnsureBlocks();
            CheckDateRollover();
            if (!visible_) return 0;
            for (auto& block : blocks_) block->KeepAttached();
            TickClocks();
            return 0;

        case kTrayMessage:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                ShowTrayMenu(pt);
            } else if (LOWORD(lp) == WM_LBUTTONUP) {
                // Клик по значку — общие настройки: там же выбираются блоки на экране.
                // Спрятать и вернуть блоки можно из меню значка и по Ctrl+Alt+P.
                POINT pt;
                GetCursorPos(&pt);
                ShowGlobalSettings(pt);
            }
            return 0;

        case WM_HOTKEY:
            if (wp == kHotkeyId) ToggleVisible();
            return 0;

        case WM_COMMAND:
            RunCommand(LOWORD(wp));
            return 0;

        case WM_ENDSESSION:
            Save();
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            UnregisterHotKey(hwnd, kHotkeyId);
            settings::Close();
            TrayRemove(hwnd);
            for (auto& block : blocks_) block->Close();
            blocks_.clear();
            Save();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK App::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app.hwnd_ == hwnd || msg == WM_NCCREATE) return g_app.WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::CreateBlocks() {
    for (size_t i = 0; i < data_.panels.size(); i++) {
        auto block = std::unique_ptr<Block>(new Block());
        if (block->Create(inst_, static_cast<int>(i))) blocks_.push_back(std::move(block));
    }
}

// Страховка от потерянных блоков: если окно кто-то закрыл, а блок остался
// в данных, окно создаётся заново — иначе его было бы не достать.
void App::EnsureBlocks() {
    bool restored = false;
    for (size_t i = 0; i < blocks_.size(); i++) {
        if (blocks_[i]->Hwnd()) continue;
        blocks_[i].reset(new Block());
        if (blocks_[i]->Create(inst_, static_cast<int>(i))) restored = true;
    }
    for (size_t i = blocks_.size(); i < data_.panels.size(); i++) {
        auto block = std::unique_ptr<Block>(new Block());
        if (block->Create(inst_, static_cast<int>(i))) {
            blocks_.push_back(std::move(block));
            restored = true;
        }
    }
    if (restored) {
        ReindexBlocks();
        for (auto& block : blocks_) block->Show(visible_);
    }
}

bool App::Start(HINSTANCE inst) {
    inst_ = inst;

    today_ = Today();
    selected_ = today_;

    LoadData(data_);  // файла нет — остаются значения по умолчанию
    if (data_.panels.empty()) FillDefaults(data_);
    SetTheme(data_.darkTheme, data_.accent);

    icon_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON, 0, 0,
                                          LR_DEFAULTSIZE | LR_SHARED));

    WNDCLASSEXW appClass = {sizeof(appClass)};
    appClass.lpfnWndProc = &App::WndProcThunk;
    appClass.hInstance = inst;
    appClass.hIcon = icon_;
    appClass.hIconSm = icon_;
    appClass.lpszClassName = kAppClass;
    if (!RegisterClassExW(&appClass)) return false;

    WNDCLASSEXW blockClass = {sizeof(blockClass)};
    blockClass.style = CS_DBLCLKS;
    blockClass.lpfnWndProc = &Block::WndProcThunk;
    blockClass.hInstance = inst;
    blockClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    blockClass.hIcon = icon_;
    blockClass.hIconSm = icon_;
    blockClass.lpszClassName = kBlockClass;
    if (!RegisterClassExW(&blockClass)) return false;

    // Окно приложения не показывается: оно нужно ради трея, горячей клавиши и таймера.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAppClass, L"Planner GO", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            inst, nullptr);
    if (!hwnd_) return false;

    CreateBlocks();
    if (blocks_.empty()) return false;

    // При запущенном центре значок виджета живёт в его меню, а не отдельно в трее.
    SetTrayShown(!hub::HubRunning());
    ApplyHotkey();
    SetTimer(hwnd_, kTimerId, kTimerMs, nullptr);
    return true;
}

}  // namespace

bool CreateWidgetWindow(HINSTANCE inst) { return g_app.Start(inst); }
