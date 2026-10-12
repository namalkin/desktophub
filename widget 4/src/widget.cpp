#include "widget.h"

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "dialog.h"
#include "hub_link.h"
#include "junk.h"
#include "panels.h"
#include "scan.h"
#include "settings.h"
#include "store.h"
#include "sys.h"
#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kAppClass[] = L"DiskVacApp";      // скрытое окно: трей, горячая клавиша, таймер
constexpr wchar_t kBlockClass[] = L"DiskVacPanel";  // блок на рабочем столе
constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 500;

// Пока блок тащат или растягивают, мы ведём мышь сами: блок — дочернее окно рабочего
// стола, активным оно не бывает, а захват мыши у неактивного окна работает только пока
// курсор над ним. Поэтому опрашиваем курсор и кнопку по таймеру.
constexpr UINT kMouseTimerId = 2;
constexpr UINT kMouseTimerMs = 15;
constexpr UINT kStartScanTimerId = 3;  // отложенный запуск при старте Windows

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kScanProgressMessage = WM_APP + 2;
constexpr UINT kScanDoneMessage = WM_APP + 3;
constexpr int kHotkeyId = 1;
constexpr int kIconResourceId = 1;

// Логическая геометрия блока (96 DPI).
constexpr int kShadow = 16;
constexpr int kPad = 12;
constexpr int kCardRadius = 14;
constexpr int kHeaderH = 30;
constexpr int kHeaderBtn = 22;
constexpr int kGrip = 9;
constexpr int kMinW = 220;
constexpr int kMinH = 120;
constexpr int kMaxW = 1400;
constexpr int kMaxH = 1600;
constexpr int kWideHeaderW = 190;
constexpr int kDragThreshold = 5;

// Пункты меню.
enum : UINT {
    kCmdRenameBlock = 100,
    kCmdToggleCollapse,
    kCmdToggleLock,
    kCmdDeleteBlock,
    kCmdSettings,
    kCmdGlobalSettings,
    kCmdCollapseAll,
    kCmdExpandAll,
    kCmdToggleVisible,
    kCmdArrange,
    kCmdScan,
    kCmdStopScan,
    kCmdClean,
    kCmdReport,
    kCmdExit,
    kCmdRowOpen,
    kCmdRowReveal,
    kCmdRowProps,
    kCmdRowDelete,
    kCmdAddBlockBase = 600,
};

RectF ToRectF(const RECT& r) {
    return RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                 static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
}

int ClampInt(int value, int lo, int hi) { return value < lo ? lo : (value > hi ? hi : value); }

// Число в «пилюле» рядом с названием блока — сколько строк внутри.
int HeaderCount(const scan::Result& result, const Panel& panel) {
    if (!result.valid) return -1;
    switch (panel.kind) {
        case PanelKind::Folders: return static_cast<int>(result.folders.size());
        case PanelKind::BigFiles: return static_cast<int>(result.bigFiles.size());
        case PanelKind::Junk: return static_cast<int>(result.junk.size());
        case PanelKind::Old: return static_cast<int>(result.oldFiles.size());
        case PanelKind::Orphans: return static_cast<int>(result.orphans.size());
        case PanelKind::System: return static_cast<int>(result.system.size());
        default: return -1;
    }
}

struct Fonts {
    std::unique_ptr<FontFamily> family;
    std::unique_ptr<Font> title, count, big, row, small, tiny, num;
    int dpi = 0;
    int fontIndex = -1;

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
        big = make(26.0f, FontStyleBold);
        row = make(12.5f, FontStyleRegular);
        small = make(11.0f, FontStyleRegular);
        tiny = make(9.5f, FontStyleRegular);
        num = make(12.0f, FontStyleBold);
    }
};

enum class Zone { None, Header, HeaderSettings, HeaderMenu, ResizeW, ResizeH, ResizeWH, Content };

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

    void Refresh();
    // attempt — защита от повторов: содержимое рисуется второй раз, только если после
    // первого прохода выяснилось, что полоса прокрутки появилась (или исчезла).
    void Render(int attempt = 0);
    void ApplyTopmost();
    void KeepAttached();
    void Reparent(HWND parent);
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

    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int surfaceW_ = 0, surfaceH_ = 0;

    std::vector<Elem> elems_;
    int contentH_ = 0;
    bool scrollable_ = false;
    int scroll_ = 0;

    Hit hover_;
    bool mouseTracking_ = false;

    bool headerPending_ = false;
    POINT pressPt_{};
    int pressElem_ = -1;

    bool moving_ = false;
    POINT dragCursor_{};
    POINT dragOrigin_{};

    bool resizing_ = false;
    bool resizeW_ = false;
    bool resizeH_ = false;

    void BeginMove();
    void MoveTo(int screenX, int screenY);
    void MouseTick();
    void EndMouseAction();
};

// ---------- Приложение ----------

class App : public settings::Host {
public:
    bool Start(HINSTANCE inst);

    // settings::Host
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
    void PickDrive(wchar_t letter) override;
    void StartScan() override;
    void OpenReport() override;

    HINSTANCE Instance() const { return inst_; }
    bool Visible() const { return visible_; }

    const scan::Result& Result() const { return result_; }
    const scan::Progress& Progress() const { return progress_; }
    const std::vector<scan::Drive>& Drives() const { return drives_; }
    bool Elevated() const { return elevated_; }

    Block* BlockForPanel(int index);
    HWND AnchorFor(int panel);

    void Activate(int panel, const Elem& elem);

    void SnapRect(const Block* self, RECT& rect);
    void SetAllCollapsed(bool collapsed);
    void ToggleVisible();
    void RenameBlock(int panel);
    void DeletePanel(int panel);

    void ShowSettings(int panel, POINT screenPt);
    void ShowGlobalSettings(POINT screenPt);
    void ShowBlockMenu(POINT screenPt, int panel);
    void ShowRowMenu(POINT screenPt, int panel, int index, ListKind kind);
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
    void PullScanState();   // забрать у сканера свежие прогресс и результат
    void OnScanFinished();

    void StopScan();
    void CleanNow();
    void DeleteRow(int index, ListKind kind);
    std::wstring RowPath(int index, ListKind kind) const;

    bool AskText(int panel, const wchar_t* title, const wchar_t* label, const wchar_t* hint,
                 std::wstring& value, const wchar_t* okLabel);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    AppData data_;
    std::vector<std::unique_ptr<Block>> blocks_;
    bool visible_ = true;
    bool trayShown_ = false;
    bool elevated_ = false;

    scan::Result result_;
    scan::Progress progress_;
    std::vector<scan::Drive> drives_;
    bool cleaning_ = false;  // последняя запущенная работа — уборка, а не сканирование

    int ctxPanel_ = -1;
    int ctxRow_ = -1;
    ListKind ctxKind_ = ListKind::None;
};

App g_app;

// Повторный запуск .exe шлёт это сообщение уже работающему виджету, чтобы тот
// показался, если был скрыт в трее.
UINT ShowMessageId() {
    static const UINT id = RegisterWindowMessageW(L"DiskVac_Show");
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

    const int hb = Sc(kHeaderBtn, dpi_);
    const int hbTop = L.header.top + (headerH - hb) / 2;
    const int step = hb + Sc(2, dpi_);
    L.compactHeader = (L.header.right - L.header.left) < Sc(kWideHeaderW, dpi_);
    L.menuBtn = {L.header.right - hb, hbTop, L.header.right, hbTop + hb};
    if (L.compactHeader) {
        L.gearBtn = {0, 0, 0, 0};
    } else {
        L.gearBtn = {L.menuBtn.left - step, hbTop, L.menuBtn.left - step + hb, hbTop + hb};
    }

    if (panel.collapsed) {
        L.viewport = {L.header.left, L.header.bottom, L.header.right, L.header.bottom};
        L.cardH = L.header.bottom + Sc(5, dpi_) - cardTop;
    } else {
        L.cardH = Sc(ClampInt(height, kMinH, kMaxH), dpi_);
        L.viewport = {cardLeft + pad, L.header.bottom + Sc(6, dpi_), cardLeft + L.cardW - pad,
                      cardTop + L.cardH - pad};
        if (L.viewport.bottom < L.viewport.top) L.viewport.bottom = L.viewport.top;
    }

    L.card = {cardLeft, cardTop, cardLeft + L.cardW, cardTop + L.cardH};
    L.winW = L.cardW + L.shadow * 2;
    L.winH = L.cardH + L.shadow * 2;

    ClampScroll();

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
    const float titleRight = static_cast<float>(L.compactHeader ? L.menuBtn.left : L.gearBtn.left);
    const float titleMax = titleRight - titleX - ScF(30.0f, dpi_);
    const std::wstring title = gfx::Ellipsize(g, panel.Title(), *fonts_.title, titleMax);
    gfx::DrawText(g, title.c_str(), *fonts_.title, P().textPrimary, titleX, titleY);

    const int count = HeaderCount(g_app.Result(), panel);
    if (count >= 0 && !title.empty()) {
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
        enum class Glyph { Gear, Dots };
        auto headerButton = [&](const RECT& r, Glyph glyph, bool hot) {
            const RectF rf = ToRectF(r);
            if (hot) gfx::FillRoundRect(g, rf, rf.Width / 2.0f, WithAlpha(P().accent, 0x33));
            const float cx = rf.X + rf.Width / 2.0f;
            const float cy = rf.Y + rf.Height / 2.0f;
            Color c(hot ? P().accent : P().textFaint);
            Pen pen(c, ScF(1.5f, dpi_));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            if (glyph == Glyph::Gear) {
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
            } else {
                SolidBrush dots(c);
                for (int i = -1; i <= 1; i++) {
                    g.FillEllipse(&dots, cx - 1.2f * u, cy + i * 4.0f * u - 1.2f * u, 2.4f * u, 2.4f * u);
                }
            }
        };
        if (!L.compactHeader) headerButton(L.gearBtn, Glyph::Gear, hover_.zone == Zone::HeaderSettings);
        headerButton(L.menuBtn, Glyph::Dots, hover_.zone == Zone::HeaderMenu);
    }

    if (!panel.collapsed) {
        RectF line(headerF.X, static_cast<float>(L.header.bottom), headerF.Width, 1.5f * u);
        gfx::FadeLine(g, line, WithAlpha(P().accent, 0xB0));
    }
}

void Block::Render(int attempt) {
    const Panel& panel = Data();
    // Оформление у каждого блока своё, поэтому палитру ставим перед каждой отрисовкой.
    SetTheme(panel.darkTheme, panel.accent, panel.opacity);
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

    // У прозрачности есть пол: совсем прозрачная карточка светлой темы показывает обои
    // насквозь, и тёмный текст на тёмных обоях перестаёт читаться.
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

    // Нужен ли отступ под полосу прокрутки, точно известно только после отрисовки
    // содержимого. Берём ответ прошлого прохода, а если он не подтвердился — рисуем
    // содержимое ещё раз: один лишний проход в момент появления полосы дешевле, чем
    // кадр, в котором размеры перечёркнуты полосой.
    const int viewH = L.viewport.bottom - L.viewport.top;
    const bool wantGutter = scrollable_ && contentH_ > viewH;
    const int gutter = wantGutter ? Sc(14, dpi_) : 0;

    elems_.clear();
    if (!panel.collapsed) {
        g.SetClip(Rect(L.viewport.left, L.viewport.top, L.viewport.right - L.viewport.left,
                       L.viewport.bottom - L.viewport.top));

        PanelCtx ctx;
        ctx.g = &g;
        ctx.data = &g_app.Data();
        ctx.panel = &panel;
        ctx.result = &g_app.Result();
        ctx.progress = &g_app.Progress();
        ctx.drives = &g_app.Drives();
        ctx.elevated = g_app.Elevated();
        ctx.dpi = dpi_;
        ctx.viewport = L.viewport;
        // Полоса прокрутки идёт по правому краю окна просмотра, а размеры в списках
        // прижаты вправо — без отступа полоса ложится прямо на «12,4 ГБ».
        ctx.body = {L.viewport.left, L.viewport.top - scroll_, L.viewport.right - gutter,
                    L.viewport.bottom};
        ctx.fonts.family = fonts_.family.get();
        ctx.fonts.big = fonts_.big.get();
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

        if ((scrollable_ && contentH_ > viewH) != wantGutter && attempt == 0) {
            ClampScroll();
            Render(1);
            return;
        }

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

    POINT src = {0, 0};
    SIZE size = {L.winW, L.winH};
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
    if (!Data().locked) {
        if (PtInRect(&L.resizeCorner, pt)) return {Zone::ResizeWH, Hover{}, -1};
        if (PtInRect(&L.resizeRight, pt)) return {Zone::ResizeW, Hover{}, -1};
        if (!Data().collapsed && PtInRect(&L.resizeBottom, pt)) return {Zone::ResizeH, Hover{}, -1};
    }
    if (L.gearBtn.right > 0 && PtInRect(&L.gearBtn, pt)) return {Zone::HeaderSettings, Hover{}, -1};
    if (PtInRect(&L.menuBtn, pt)) return {Zone::HeaderMenu, Hover{}, -1};
    if (PtInRect(&L.header, pt)) return {Zone::Header, Hover{}, -1};
    if (PtInRect(&L.viewport, pt)) {
        // С конца: то, что нарисовано поверх, и нажимается первым.
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
//   «на рабочем столе» — дочернее окно хозяина значков. Тогда он и есть часть рабочего
//      стола: никогда не оказывается над чужими окнами, но виден вместе со столом;
//   «поверх всех окон» — обычное окно верхнего уровня с HWND_TOPMOST.
void Block::ApplyTopmost() {
    if (!hwnd_) return;
    const bool onTop = g_app.Data().alwaysOnTop;
    HWND desktop = onTop ? nullptr : DesktopHostWindow();
    const HWND parent = GetParent(hwnd_);

    if (desktop) {
        if (parent != desktop) {
            Reparent(desktop);
            PlaceWindow();
            Render();
        }
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

void Block::MouseTick() {
    if (!moving_ && !resizing_ && !headerPending_) {
        EndMouseAction();
        return;
    }
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        EndMouseAction();
        return;
    }

    if (headerPending_) {
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
        const POINT probe = {x + layout_.winW / 2, y + layout_.winH / 2};
        if (!MonitorFromPoint(probe, MONITOR_DEFAULTTONULL)) needDefault = true;
    }
    if (needDefault) {
        MONITORINFO mi = {sizeof(mi)};
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (GetMonitorInfoW(mon, &mi)) {
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
            if (moving_ || headerPending_) return 0;

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
            BeginMove();
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (moving_ || resizing_) {
                EndMouseAction();
                return 0;
            }
            KillTimer(hwnd, kMouseTimerId);
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
                const Elem copy = elems_[static_cast<size_t>(elem)];
                if (PtInRect(&copy.rect, pt)) g_app.Activate(panelIndex_, copy);
            }
            return 0;
        }

        // Клик второй раз подряд приходит сюда, а не как WM_LBUTTONDOWN: гасим его.
        case WM_LBUTTONDBLCLK:
            return 0;

        case WM_TIMER:
            if (wp == kMouseTimerId) {
                MouseTick();
                return 0;
            }
            break;

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
            if (hit.zone == Zone::Content && hit.hover.act == Act::OpenRow) {
                g_app.ShowRowMenu(screen, panelIndex_, hit.hover.a, static_cast<ListKind>(hit.hover.b));
                return 0;
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

        // Alt+F4 по блоку не должен «терять» его насовсем: блок — часть виджета,
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
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBlockClass,
                            L"Диск-пылесос", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, this);
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

bool App::AskText(int panel, const wchar_t* title, const wchar_t* label, const wchar_t* hint,
                  std::wstring& value, const wchar_t* okLabel) {
    DialogSpec spec;
    spec.title = title;
    spec.okLabel = okLabel;
    DialogField field;
    field.label = label;
    field.hint = hint ? hint : L"";
    field.value = value;
    field.required = true;
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

// ---------- Приложение: сканирование ----------

void App::PullScanState() {
    progress_ = scan::CurrentProgress();
    if (scan::HasResult()) result_ = scan::Snapshot();
}

void App::StartScan() {
    if (scan::Busy()) return;
    cleaning_ = false;
    if (!scan::Start(data_.drive, data_.options)) return;
    PullScanState();
    RefreshAll();
    if (settings::IsOpen()) settings::Refresh();
}

void App::StopScan() {
    scan::Cancel();
    PullScanState();
    RefreshAll();
}

void App::OnScanFinished() {
    PullScanState();
    drives_ = scan::Drives();

    if (cleaning_) {
        const scan::CleanReport report = scan::LastClean();
        cleaning_ = false;
        if (report.freed > 0) {
            data_.freedTotal += report.freed;
            Save();
        }
        wchar_t text[320];
        if (report.failed > 0) {
            swprintf(text, 320,
                     L"Вынесено %ls (%d %ls).\n\nЧасть файлов система не отдала — они заняты "
                     L"работающими программами или требуют прав администратора.",
                     FormatSize(report.freed).c_str(), report.files,
                     PluralRu(report.files, L"файл", L"файла", L"файлов"));
        } else {
            swprintf(text, 320, L"Вынесено %ls (%d %ls).", FormatSize(report.freed).c_str(), report.files,
                     PluralRu(report.files, L"файл", L"файла", L"файлов"));
        }
        MessageBoxW(hwnd_, text, L"Диск-пылесос", MB_OK | MB_ICONINFORMATION);
        // Цифры мусора остались от того, что уже удалено, — пересчитываем их отдельно,
        // не обходя диск заново.
        scan::RefreshJunk();
        PullScanState();
        RefreshAll();
        if (settings::IsOpen()) settings::Refresh();
        return;
    }

    if (result_.valid) {
        data_.memory.valid = true;
        data_.memory.drive = result_.drive;
        data_.memory.used = result_.used;
        data_.memory.total = result_.total;
        data_.memory.junk = result_.JunkTotal();
        wchar_t when[64];
        swprintf(when, 64, L"%02d.%02d.%04d, %02d:%02d", result_.finished.wDay, result_.finished.wMonth,
                 result_.finished.wYear, result_.finished.wHour, result_.finished.wMinute);
        data_.memory.when = when;
        Save();
        WriteReport(result_);

        // Впервые собранный список мусора отмечаем сам — кроме групп с файлами
        // пользователя: за них решает он.
        if (data_.checkedJunk.empty()) {
            for (const scan::JunkItem& item : result_.junk) {
                if (!item.userData) data_.checkedJunk.push_back(item.id);
            }
            Save();
        }
    }
    RefreshAll();
    if (settings::IsOpen()) settings::Refresh();
}

void App::CleanNow() {
    if (scan::Busy() || !result_.valid) return;

    scan::CleanRequest request;
    unsigned long long total = 0;
    std::wstring list;
    int groups = 0;
    bool userData = false;
    for (const scan::JunkItem& item : result_.junk) {
        if (!data_.JunkChecked(item.id)) continue;
        request.junkIds.push_back(item.id);
        total += item.size;
        if (groups < 12) list += L"  • " + item.name + L" — " + FormatSize(item.size) + L"\n";
        groups++;
        if (item.userData) userData = true;
    }
    if (request.junkIds.empty()) return;

    if (data_.confirmDelete) {
        std::wstring text = L"Вынести отмеченное — " + FormatSize(total) + L"?\n\n" + list;
        if (groups > 12) text += L"  … и ещё группы\n";
        text +=
            L"\nКэши и временные файлы удаляются сразу: Windows и программы создадут их "
            L"заново.";
        if (userData) {
            text +=
                L"\n\nСреди отмеченного есть ваши файлы (загрузки, Windows.old). "
                L"Проверьте список ещё раз.";
        }
        const int answer = MessageBoxW(hwnd_, text.c_str(), L"Диск-пылесос",
                                       MB_OKCANCEL | (userData ? MB_ICONWARNING : MB_ICONQUESTION));
        if (answer != IDOK) return;
    }

    cleaning_ = true;
    if (!scan::StartClean(request)) {
        cleaning_ = false;
        return;
    }
    PullScanState();
    RefreshAll();
}

std::wstring App::RowPath(int index, ListKind kind) const {
    switch (kind) {
        case ListKind::Folder:
            if (index >= 0 && index < static_cast<int>(result_.folders.size())) {
                return result_.folders[static_cast<size_t>(index)].path;
            }
            break;
        case ListKind::BigFile:
            if (index >= 0 && index < static_cast<int>(result_.bigFiles.size())) {
                return result_.bigFiles[static_cast<size_t>(index)].path;
            }
            break;
        case ListKind::OldFile:
            if (index >= 0 && index < static_cast<int>(result_.oldFiles.size())) {
                return result_.oldFiles[static_cast<size_t>(index)].path;
            }
            break;
        case ListKind::Orphan:
            if (index >= 0 && index < static_cast<int>(result_.orphans.size())) {
                return result_.orphans[static_cast<size_t>(index)].path;
            }
            break;
        case ListKind::System:
            if (index >= 0 && index < static_cast<int>(result_.system.size())) {
                return result_.system[static_cast<size_t>(index)].path;
            }
            break;
        case ListKind::SlicePart: {
            const int slice = SliceOfPart(index);
            const int part = PartOfPart(index);
            if (slice < 0 || slice >= static_cast<int>(result_.slices.size())) break;
            const std::vector<scan::FolderItem>& parts = result_.slices[static_cast<size_t>(slice)].parts;
            if (part < 0 || part >= static_cast<int>(parts.size())) break;
            return parts[static_cast<size_t>(part)].path;
        }
        default:
            break;
    }
    return std::wstring();
}

void App::DeleteRow(int index, ListKind kind) {
    const std::wstring path = RowPath(index, kind);
    if (path.empty()) return;
    if (kind == ListKind::System) {
        MessageBoxW(hwnd_,
                    L"Системные файлы виджет не удаляет: без них Windows работает хуже, "
                    L"а иногда и вовсе не загружается.\n\nВ блоке написано, какой настройкой "
                    L"Windows уменьшает каждый из них.",
                    L"Диск-пылесос", MB_OK | MB_ICONINFORMATION);
        return;
    }

    unsigned long long size = 0;
    switch (kind) {
        case ListKind::BigFile: size = result_.bigFiles[static_cast<size_t>(index)].size; break;
        case ListKind::OldFile: size = result_.oldFiles[static_cast<size_t>(index)].size; break;
        case ListKind::Orphan: size = result_.orphans[static_cast<size_t>(index)].size; break;
        case ListKind::Folder: size = result_.folders[static_cast<size_t>(index)].size; break;
        default: break;
    }

    std::wstring text = L"Отправить в корзину?\n\n" + path + L"\n" + FormatSize(size) +
                        L"\n\nИз корзины файл можно вернуть, но место освободится только "
                        L"после её очистки.";
    if (MessageBoxW(hwnd_, text.c_str(), L"Диск-пылесос", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;

    if (!ToRecycleBin(hwnd_, {path})) {
        MessageBoxW(hwnd_, L"Не получилось: файл занят программой или защищён системой.",
                    L"Диск-пылесос", MB_OK | MB_ICONWARNING);
        return;
    }
    data_.freedTotal += size;
    Save();
    // Строку из списка убираем сразу: файла по этому пути больше нет.
    switch (kind) {
        case ListKind::BigFile: result_.bigFiles.erase(result_.bigFiles.begin() + index); break;
        case ListKind::OldFile: result_.oldFiles.erase(result_.oldFiles.begin() + index); break;
        case ListKind::Orphan: result_.orphans.erase(result_.orphans.begin() + index); break;
        case ListKind::Folder: result_.folders.erase(result_.folders.begin() + index); break;
        default: break;
    }
    RefreshAll();
}

void App::OpenReport() {
    if (!result_.valid) return;
    WriteReport(result_);
    ShellExecuteW(nullptr, L"open", ReportFile().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::PickDrive(wchar_t letter) {
    if (towupper(letter) == towupper(data_.drive)) return;
    data_.drive = static_cast<wchar_t>(towupper(letter));
    Save();
    drives_ = scan::Drives();
    RefreshAll();
    if (settings::IsOpen()) settings::Refresh();
}

void App::Activate(int panelIndex, const Elem& elem) {
    if (!HasPanel(panelIndex)) return;

    switch (elem.act) {
        case Act::StartScan: StartScan(); return;
        case Act::StopScan: StopScan(); return;
        case Act::CleanNow: CleanNow(); return;
        case Act::OpenReport: OpenReport(); return;
        case Act::PickDrive:
            if (elem.a >= 0 && elem.a < static_cast<int>(drives_.size())) {
                PickDrive(drives_[static_cast<size_t>(elem.a)].letter);
            }
            return;
        case Act::ToggleJunk:
            data_.ToggleJunk(elem.a);
            Save();
            RefreshAll();
            return;
        case Act::JunkAll:
            data_.checkedJunk.clear();
            if (elem.a == 1) {
                for (const scan::JunkItem& item : result_.junk) {
                    if (!item.userData) data_.checkedJunk.push_back(item.id);
                }
            }
            Save();
            RefreshAll();
            return;
        case Act::ExpandSlice: {
            Panel& panel = data_.panels[static_cast<size_t>(panelIndex)];
            panel.expanded = (panel.expanded == elem.a) ? -1 : elem.a;
            Save();
            ApplyPanelLook(panelIndex);
            return;
        }
        case Act::OpenRow: {
            const ListKind kind = static_cast<ListKind>(elem.b);
            const std::wstring path = RowPath(elem.a, kind);
            if (path.empty()) return;
            if (IsDirectory(path)) OpenFolder(path);
            else RevealInExplorer(path);
            return;
        }
        case Act::RowMenu: {
            POINT pt;
            GetCursorPos(&pt);
            ShowRowMenu(pt, panelIndex, elem.a, static_cast<ListKind>(elem.b));
            return;
        }
        case Act::Elevate: {
            if (MessageBoxW(hwnd_,
                            L"Перезапустить виджет с правами администратора?\n\nТогда станут видны "
                            L"системные папки, которые сейчас закрыты.",
                            L"Диск-пылесос", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
                return;
            }
            if (RestartElevated()) DestroyWindow(hwnd_);
            return;
        }
        default:
            return;
    }
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
    RegisterHotKey(hwnd_, kHotkeyId, MOD_CONTROL | MOD_ALT, 'D');
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
                 L"Сохранить")) {
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
    const int threshold = 24;
    const int gap = 12;

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
    POINT where = {screenPt.x + 20, screenPt.y};
    settings::Show(inst_, this, panel, where, settings::Mode::Panel);
}

void App::ShowGlobalSettings(POINT screenPt) {
    POINT where = {screenPt.x + 20, screenPt.y};
    settings::Show(inst_, this, HasPanel(ctxPanel_) ? ctxPanel_ : 0, where, settings::Mode::Global);
}

namespace {

// Подменю «Добавить блок»: все типы с галочками у уже открытых.
HMENU BuildAddMenu(const AppData& data) {
    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < static_cast<int>(PanelKind::Count); i++) {
        const PanelKind kind = static_cast<PanelKind>(i);
        std::wstring label = PanelKindName(kind);
        if (data.PanelCount(kind) > 0) label += L"  ✓";
        AppendMenuW(menu, MF_STRING, kCmdAddBlockBase + static_cast<UINT>(i), label.c_str());
    }
    return menu;
}

}  // namespace

void App::ShowBlockMenu(POINT screenPt, int panel) {
    if (!HasPanel(panel)) return;
    ctxPanel_ = panel;
    ctxRow_ = -1;
    ctxKind_ = ListKind::None;
    const Panel& target = data_.panels[static_cast<size_t>(panel)];

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool busy = scan::Busy();
    AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), kCmdScan, L"Просканировать диск");
    if (busy) AppendMenuW(menu, MF_STRING, kCmdStopScan, L"Остановить сканирование");
    AppendMenuW(menu, MF_STRING | ((!busy && result_.valid) ? 0 : MF_GRAYED), kCmdClean,
                L"Пропылесосить отмеченное");
    AppendMenuW(menu, MF_STRING | (result_.valid ? 0 : MF_GRAYED), kCmdReport, L"Открыть отчёт");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdRenameBlock, L"Переименовать блок…");
    AppendMenuW(menu, MF_STRING, kCmdToggleCollapse, target.collapsed ? L"Развернуть" : L"Свернуть");
    AppendMenuW(menu, MF_STRING | (target.locked ? MF_CHECKED : 0), kCmdToggleLock,
                L"Закрепить блок (не двигать)");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildAddMenu(data_)), L"Добавить блок");
    AppendMenuW(menu, MF_STRING | (data_.panels.size() > 1 ? 0 : MF_GRAYED), kCmdDeleteBlock,
                L"Убрать этот блок");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdArrange, L"Расставить блоки по экрану");
    AppendMenuW(menu, MF_STRING, kCmdCollapseAll, L"Свернуть все блоки");
    AppendMenuW(menu, MF_STRING, kCmdExpandAll, L"Развернуть все блоки");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"Настройки блока…");
    AppendMenuW(menu, MF_STRING, kCmdGlobalSettings, L"Настройки пылесоса…");
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

void App::ShowRowMenu(POINT screenPt, int panel, int index, ListKind kind) {
    const std::wstring path = RowPath(index, kind);
    if (path.empty()) {
        ShowBlockMenu(screenPt, panel);
        return;
    }
    ctxPanel_ = panel;
    ctxRow_ = index;
    ctxKind_ = kind;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdRowOpen, IsDirectory(path) ? L"Открыть папку" : L"Открыть файл");
    AppendMenuW(menu, MF_STRING, kCmdRowReveal, L"Показать в проводнике");
    AppendMenuW(menu, MF_STRING, kCmdRowProps, L"Свойства");
    if (kind != ListKind::System) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdRowDelete, L"Отправить в корзину…");
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
    ctxRow_ = -1;
    ctxKind_ = ListKind::None;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool busy = scan::Busy();
    AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), kCmdScan, L"Просканировать диск");
    if (busy) AppendMenuW(menu, MF_STRING, kCmdStopScan, L"Остановить сканирование");
    AppendMenuW(menu, MF_STRING | ((!busy && result_.valid) ? 0 : MF_GRAYED), kCmdClean,
                L"Пропылесосить отмеченное");
    AppendMenuW(menu, MF_STRING | (result_.valid ? 0 : MF_GRAYED), kCmdReport, L"Открыть отчёт");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdToggleVisible, visible_ ? L"Скрыть все блоки" : L"Показать все блоки");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildAddMenu(data_)), L"Добавить блок");
    AppendMenuW(menu, MF_STRING, kCmdArrange, L"Расставить блоки по экрану");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdGlobalSettings, L"Настройки пылесоса…");
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
        case kCmdScan: StartScan(); break;
        case kCmdStopScan: StopScan(); break;
        case kCmdClean: CleanNow(); break;
        case kCmdReport: OpenReport(); break;
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
        case kCmdArrange: ArrangePanels(); break;
        case kCmdToggleVisible: ToggleVisible(); break;
        case kCmdExit: DestroyWindow(hwnd_); break;

        case kCmdRowOpen: {
            const std::wstring path = RowPath(ctxRow_, ctxKind_);
            if (path.empty()) break;
            if (IsDirectory(path)) OpenFolder(path);
            else ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case kCmdRowReveal: RevealInExplorer(RowPath(ctxRow_, ctxKind_)); break;
        case kCmdRowProps: ShowFileProperties(hwnd_, RowPath(ctxRow_, ctxKind_)); break;
        case kCmdRowDelete: DeleteRow(ctxRow_, ctxKind_); break;
        default: break;
    }
}

// ---------- Приложение: окно ----------

void App::UpdateTrayTip() {
    wchar_t tip[128];
    if (scan::Busy()) {
        swprintf(tip, 128, L"Диск-пылесос — %ls, %d%%", progress_.note.c_str(), progress_.percent);
    } else if (result_.valid) {
        swprintf(tip, 128, L"Диск-пылесос — %lc: свободно %ls, мусора %ls", result_.drive,
                 FormatSize(result_.freeSpace).c_str(), FormatSize(result_.JunkTotal()).c_str());
    } else {
        swprintf(tip, 128, L"Диск-пылесос — диск ещё не сканировали");
    }
    TrayUpdateTip(hwnd_, tip);
}

// ---------- Центр виджетов ----------

void App::SetTrayShown(bool shown) {
    if (shown == trayShown_) return;
    if (shown) {
        trayShown_ = TrayAdd(hwnd_, kTrayMessage, icon_, L"Диск-пылесос");
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
            if (wp == kStartScanTimerId) {
                KillTimer(hwnd, kStartScanTimerId);
                StartScan();
                return 0;
            }
            if (wp != kTimerId) break;
            EnsureBlocks();
            if (!visible_) return 0;
            for (auto& block : blocks_) block->KeepAttached();
            if (scan::Busy()) {
                // Пока идёт обход, блок «Пылесос» показывает прогресс и крутит робота.
                PullScanState();
                for (auto& block : blocks_) {
                    if (!HasPanel(block->PanelIndex())) continue;
                    const PanelKind kind = data_.panels[static_cast<size_t>(block->PanelIndex())].kind;
                    if (kind == PanelKind::Vacuum) block->Render();
                }
                UpdateTrayTip();
            }
            return 0;

        case kScanProgressMessage:
            PullScanState();
            return 0;

        case kScanDoneMessage:
            OnScanFinished();
            return 0;

        case kTrayMessage:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                ShowTrayMenu(pt);
            } else if (LOWORD(lp) == WM_LBUTTONUP) {
                // Клик по значку — общие настройки: там же выбираются блоки и диск.
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
            scan::Shutdown();
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
    elevated_ = IsElevated();

    LoadData(data_);  // файла нет — остаются значения по умолчанию
    if (data_.panels.empty()) FillDefaults(data_);
    SetTheme(data_.darkTheme, data_.accent);

    drives_ = scan::Drives();
    // Диск из настроек мог исчезнуть (внешний, отключённый) — берём системный.
    bool known = false;
    for (const scan::Drive& drive : drives_) {
        if (towupper(drive.letter) == towupper(data_.drive)) known = true;
    }
    if (!known && !drives_.empty()) {
        data_.drive = drives_[0].letter;
        for (const scan::Drive& drive : drives_) {
            if (drive.system) data_.drive = drive.letter;
        }
    }

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
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAppClass, L"Диск-пылесос", WS_POPUP, 0, 0, 0, 0, nullptr,
                            nullptr, inst, nullptr);
    if (!hwnd_) return false;

    scan::Init(hwnd_, kScanProgressMessage, kScanDoneMessage);

    CreateBlocks();
    if (blocks_.empty()) return false;

    // При запущенном центре значок виджета живёт в его меню, а не отдельно в трее.
    SetTrayShown(!hub::HubRunning());
    ApplyHotkey();
    SetTimer(hwnd_, kTimerId, kTimerMs, nullptr);
    // Сканирование при запуске откладываем: сразу после старта Windows диск занят
    // без нас, и обход мешал бы загрузке.
    if (data_.scanOnStart) SetTimer(hwnd_, kStartScanTimerId, 30000, nullptr);
    return true;
}

}  // namespace

bool CreateWidgetWindow(HINSTANCE inst) { return g_app.Start(inst); }
