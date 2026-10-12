#include "widget.h"

#include <shlobj.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "dialog.h"
#include "hub_link.h"
#include "dnd.h"
#include "dragout.h"
#include "icons.h"
#include "settings.h"
#include "store.h"
#include "sys.h"
#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kAppClass[] = L"ShortcutDockApp";      // скрытое окно: трей, горячая клавиша, таймер
constexpr wchar_t kBlockClass[] = L"ShortcutDockBlock";  // блок-группа на рабочем столе
constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 300;

// Пока блок тащат, растягивают или перетаскивают ярлык, мы ведём мышь сами: блок —
// дочернее окно рабочего стола, активным оно не бывает, а захват мыши (SetCapture)
// у неактивного окна работает только пока курсор над ним.
constexpr UINT kMouseTimerId = 2;
constexpr UINT kMouseTimerMs = 15;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr int kHotkeyId = 1;
constexpr int kIconResourceId = 1;

// Сколько держать перетаскиваемое над свёрнутым блоком, чтобы он раскрылся.
constexpr ULONGLONG kSpringOpenMs = 700;

// Пункты меню.
enum : UINT {
    kCmdAddGroup = 100,
    kCmdRenameGroup,
    kCmdDeleteGroup,
    kCmdToggleCollapse,
    kCmdCollapseAll,
    kCmdExpandAll,
    kCmdAddShortcut,
    kCmdSettings,        // оформление и сетка одного блока
    kCmdGlobalSettings,  // общее поведение, данные, новый блок
    kCmdItemOpen,
    kCmdItemAdmin,
    kCmdItemReveal,
    kCmdItemRename,
    kCmdItemProps,
    kCmdItemRemove,
    kCmdItemToDesktop,
    kCmdToggleLock,
    kCmdToggleVisible,
    kCmdExit,
    kCmdMoveToGroupBase = 600,  // + индекс блока: перенести выбранный ярлык
};

// Логическая геометрия блока (96 DPI).
constexpr int kShadow = 16;
constexpr int kPad = 10;
constexpr int kCardRadius = 14;
constexpr int kHeaderH = 30;
constexpr int kEmptyH = 54;
constexpr int kCellPadX = 28;
constexpr int kMinCardW = 132;     // даже блок в один значок должен показывать название
constexpr int kWideHeaderW = 190;  // уже этого — в заголовке остаётся одна кнопка «⋮»
constexpr int kGrip = 8;           // ширина зоны, за которую блок тянут мышью
constexpr int kMinColumnsUi = 1;
constexpr int kMaxColumnsUi = 12;
constexpr int kMaxRowsUi = 40;
constexpr int kDragThreshold = 5;
constexpr int kHeaderBtn = 22;

// Русские окончания: 1 ярлык / 2 ярлыка / 5 ярлыков.
const wchar_t* PluralRu(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many) {
    const int mod100 = n % 100;
    const int mod10 = n % 10;
    if (mod100 >= 11 && mod100 <= 14) return many;
    if (mod10 == 1) return one;
    if (mod10 >= 2 && mod10 <= 4) return few;
    return many;
}

// Подпись по умолчанию: имя файла без расширения, для папки — её имя, для ссылки — домен.
std::wstring DefaultName(const std::wstring& path) {
    if (IsShellPath(path)) {
        // «Корзина», «Этот компьютер» — имя спрашиваем у самой оболочки.
        const std::wstring name = ShellDisplayName(path);
        return name.empty() ? L"Папка" : name;
    }
    if (IsUrl(path)) {
        std::wstring host = path;
        const size_t scheme = host.find(L"://");
        if (scheme != std::wstring::npos) host.erase(0, scheme + 3);
        const size_t slash = host.find(L'/');
        if (slash != std::wstring::npos) host.erase(slash);
        if (host.size() > 4 && _wcsnicmp(host.c_str(), L"www.", 4) == 0) host.erase(0, 4);
        return host.empty() ? path : host;
    }
    if (IsDirectory(path)) {
        const std::wstring name = FileName(path);
        return name.empty() ? path : name;
    }
    const std::wstring stem = FileStem(path);
    return stem.empty() ? path : stem;
}

RectF ToRectF(const RECT& r) {
    return RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                 static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
}

struct Fonts {
    std::unique_ptr<FontFamily> family;
    std::unique_ptr<Font> title, count, item, tip;

    void Rebuild(int dpi) {
        family.reset(new FontFamily(L"Segoe UI"));
        auto make = [&](float size, INT style) {
            return std::unique_ptr<Font>(new Font(family.get(), ScF(size, dpi), style, UnitPixel));
        };
        title = make(12.0f, FontStyleBold);
        count = make(10.0f, FontStyleRegular);
        item = make(11.0f, FontStyleRegular);
        tip = make(11.0f, FontStyleRegular);
    }
};

enum class Zone { None, Header, HeaderAdd, HeaderSettings, HeaderMenu, Item, ItemMenu, ResizeW, ResizeH, ResizeWH };

struct Hit {
    Zone zone = Zone::None;
    int index = -1;

    bool operator==(const Hit& o) const { return zone == o.zone && index == o.index; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

struct BlockLayout {
    int dpi = 96;
    int shadow = 0;
    int cardW = 0, cardH = 0;
    int winW = 0, winH = 0;
    int cellW = 0, cellH = 0;
    int contentH = 0;
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
    std::vector<RECT> cells;
};

// Кнопка «⋮» в углу ячейки — по ней открывается меню ярлыка (переименовать, убрать и т. д.).
RECT CellMenuRect(const RECT& cell, int dpi) {
    const int d = Sc(18, dpi);
    return {cell.right - d - Sc(4, dpi), cell.top + Sc(3, dpi), cell.right - Sc(4, dpi),
            cell.top + Sc(3, dpi) + d};
}

class App;

// ---------- Блок ----------

class Block : public DropSink {
public:
    bool Create(HINSTANCE inst, int groupIndex);
    void Close();

    HWND Hwnd() const { return hwnd_; }
    int GroupIndex() const { return groupIndex_; }
    void SetGroupIndex(int index) { groupIndex_ = index; }

    void Refresh();  // пересчитать раскладку, размер окна и перерисовать
    void Render();
    void ApplyTopmost();  // приклеить к рабочему столу или поднять поверх всех окон
    void KeepAttached();  // проводник перезапускался — вернуться на рабочий стол
    void Reparent(HWND parent);  // переезд между рабочим столом и верхним уровнем
    void Show(bool visible);
    void SavePosition();
    void PlaceWindow();

    // Куда встанет ярлык, если бросить его в этой точке клиентских координат.
    bool DropSpotAt(POINT pt, int& index) const;
    void AutoScrollFor(POINT pt);

    // DropSink: перетаскивание из проводника.
    bool OnDragOver(POINT pt, bool copyMode) override;
    void OnDragLeave() override;
    void OnDrop(const std::vector<std::wstring>& paths, POINT pt, bool copyMode) override;

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    const Group& Data() const;
    Group& MutableData();

    void BuildLayout();
    int MaxBodyHeight() const;
    void ClampScroll();
    void ApplyResize(POINT pt);
    void EnsureSurface(int w, int h);
    void ReleaseSurface();
    Hit HitTest(POINT pt) const;
    void DrawItem(Graphics& g, const RECT& cell, const Shortcut& item, bool hot, bool faded, bool hotMenu,
                  bool selected);
    void DrawTooltip(Graphics& g);

    HWND hwnd_ = nullptr;
    int groupIndex_ = 0;
    int dpi_ = 96;
    BlockLayout layout_;
    Fonts fonts_;

    // Слоёная поверхность: 32bpp DIB, поверх него рисует GDI+.
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int surfaceW_ = 0, surfaceH_ = 0;

    Hit hover_;
    bool mouseTracking_ = false;
    int scroll_ = 0;

    // Нажатие на заголовок: пока не сдвинули — это клик «свернуть», после — перенос окна.
    bool headerPending_ = false;
    POINT pressPt_{};
    bool itemPending_ = false;

    // После системного перетаскивания приходит «лишний» WM_LBUTTONUP — гасим его,
    // иначе виджет решит, что был обычный клик, и запустит ярлык.
    bool suppressClick_ = false;

    // Блок — дочернее окно рабочего стола, а такие окна система мышью не двигает:
    // перенос делаем сами, от нажатия до отпускания.
    bool moving_ = false;
    POINT dragCursor_{};  // курсор в момент захвата, экранные координаты
    POINT dragOrigin_{};  // левый верхний угол окна в тот же момент

    // Растягивание блока мышью за правый край, нижний край или уголок.
    bool resizing_ = false;
    bool resizeW_ = false;
    bool resizeH_ = false;

    void BeginMove();
    void MoveTo(int screenX, int screenY);  // экранные координаты -> координаты родителя
    void MouseTick();       // очередной шаг переноса, растягивания или перетаскивания ярлыка
    void EndMouseAction();  // кнопку отпустили: магнит, сохранение, перерисовка
};

// ---------- Приложение ----------

class App : public settings::Host {
public:
    bool Start(HINSTANCE inst);

    // settings::Host — окно настроек меняет данные и просит применить.
    AppData& Data() override { return data_; }
    bool HasGroup(int group) override {
        return group >= 0 && group < static_cast<int>(data_.groups.size());
    }
    void Save() override { SaveData(data_); }
    void ApplyBlockLook(int group) override {
        if (Block* block = BlockForGroup(group)) block->Render();
    }
    void ApplyBlockLayout(int group) override {
        if (Block* block = BlockForGroup(group)) block->Refresh();
    }
    void ApplyStyleToAll(int group) override;
    void ApplyTopmost() override;
    void ApplyHotkey() override;
    void RefreshIcons() override;
    void AddGroup() override;

    void ShowSettings(int group, POINT screenPt);
    void ShowGlobalSettings(POINT screenPt);
    HINSTANCE Instance() const { return inst_; }
    HWND MainWindow() const { return hwnd_; }
    bool Visible() const { return visible_; }

    Block* BlockForGroup(int index);
    Block* BlockAtScreen(POINT screenPt);

    HWND OwnerFor(int group);   // владелец системных окон: скрытое окно, подогнанное под блок
    HWND AnchorFor(int group);  // окно блока: по нему центрируются свои диалоги
    bool RunDialog(int group, DialogSpec& spec);  // модальный ввод: блоки на это время замирают
    void RenderAll();
    void RefreshAll();

    // Действия над данными.
    int AddPaths(int group, int index, const std::vector<std::wstring>& paths, bool copyMode = true);
    void RenameGroup(int group);
    void DeleteGroup(int group);
    void AddShortcut(int group);
    void RenameItem(int group, int index);
    // quiet — для пачки: не спрашиваем про файл и не перерисовываем на каждом шаге.
    void RemoveItem(int group, int index, bool quiet = false);
    void MoveItem(int fromGroup, int fromIndex, int toGroup, int toIndex);
    void MoveItems(int fromGroup, std::vector<int> indices, int toGroup, int toIndex);
    void Launch(int group, int index, bool asAdmin);
    void SendToDesktop(int group, int index);

    // Выделение нескольких ярлыков (Ctrl+клик) — живёт внутри одного блока.
    bool HasSelection() const { return !selection_.empty(); }
    bool IsSelected(int group, int index) const;
    void ToggleSelection(int group, int index);
    void ClearSelection();

    // Магнит: блок прилипает к краям экрана, к соседям и к сетке.
    void SnapRect(const Block* self, RECT& rect);
    void SetAllCollapsed(bool collapsed);
    void ToggleVisible();

    // Перетаскивание ярлыка между блоками.
    void BeginDrag(int group, int index);
    bool BeginExternalDrag(HWND from);  // отдать перетаскиваемое системе (рабочий стол, папка)
    void UpdateDragTarget(int group, int index);
    void EndDrag(bool apply);
    bool Dragging() const { return drag_.active; }
    int DragGroup() const { return drag_.fromGroup; }
    int DragIndex() const { return drag_.fromIndex; }
    int DropGroup() const { return drag_.toGroup; }
    int DropIndex() const { return drag_.toIndex; }
    void SpringOpen();

    void ShowBlockMenu(POINT screenPt, int group);
    void ShowItemMenu(POINT screenPt, int group, int index);
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

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;  // скрытое окно приложения
    HICON icon_ = nullptr;
    AppData data_;
    std::vector<std::unique_ptr<Block>> blocks_;
    bool visible_ = true;
    bool trayShown_ = false;

    struct DragState {
        bool active = false;
        int fromGroup = -1, fromIndex = -1;
        int toGroup = -1, toIndex = -1;
        int springGroup = -1;
        ULONGLONG springSince = 0;
    } drag_;

    int ctxGroup_ = -1;
    int ctxIndex_ = -1;

    int selGroup_ = -1;
    std::vector<int> selection_;  // индексы ярлыков в блоке selGroup_
};

App g_app;

// Повторный запуск .exe шлёт это сообщение уже работающему виджету, чтобы тот
// показался, если был скрыт в трее.
UINT ShowMessageId() {
    static const UINT id = RegisterWindowMessageW(L"ShortcutDock_Show");
    return id;
}

// ---------- Блок: раскладка ----------

const Group& Block::Data() const { return g_app.Data().groups[static_cast<size_t>(groupIndex_)]; }
Group& Block::MutableData() { return g_app.Data().groups[static_cast<size_t>(groupIndex_)]; }

int Block::MaxBodyHeight() const {
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    int workH = 900;
    if (GetMonitorInfoW(mon, &mi)) workH = mi.rcWork.bottom - mi.rcWork.top;
    const int room = workH * 75 / 100;
    const int floor = Sc(120, dpi_);
    return room > floor ? room : floor;
}

void Block::BuildLayout() {
    BlockLayout& L = layout_;
    const Group& group = Data();

    L.dpi = dpi_;
    L.shadow = Sc(kShadow, dpi_);
    const int pad = Sc(kPad, dpi_);
    const int headerH = Sc(kHeaderH, dpi_);

    L.cellW = Sc(group.iconSize + kCellPadX, dpi_);
    L.cellH = Sc(group.iconSize + (group.showLabels ? 20 : 4) + 14, dpi_);
    L.cardW = max(pad * 2 + L.cellW * group.columns, Sc(kMinCardW, dpi_));

    const int rows = group.items.empty()
                         ? 0
                         : (static_cast<int>(group.items.size()) + group.columns - 1) / group.columns;
    L.contentH = group.items.empty() ? Sc(kEmptyH, dpi_) : rows * L.cellH + Sc(4, dpi_);

    int viewH = 0;
    if (!group.collapsed) {
        int limit = MaxBodyHeight();
        // Высоту тоже задаёт мышь: лишние ряды прокручиваются колесом.
        if (group.maxRows > 0) limit = min(limit, group.maxRows * L.cellH + Sc(4, dpi_));
        viewH = min(L.contentH, limit);
    }

    const int cardLeft = L.shadow;
    const int cardTop = L.shadow;
    L.header = {cardLeft + pad, cardTop + Sc(5, dpi_), cardLeft + L.cardW - pad,
                cardTop + Sc(5, dpi_) + headerH};

    // В узком блоке для трёх кнопок места нет — оставляем одну, остальное в меню.
    const int hb = Sc(kHeaderBtn, dpi_);
    const int hbTop = L.header.top + (headerH - hb) / 2;
    const int step = hb + Sc(2, dpi_);
    const int headerW = L.header.right - L.header.left;
    L.compactHeader = headerW < Sc(kWideHeaderW, dpi_);
    L.menuBtn = {L.header.right - hb, hbTop, L.header.right, hbTop + hb};
    if (L.compactHeader) {
        L.gearBtn = {0, 0, 0, 0};
        L.addBtn = {0, 0, 0, 0};
    } else {
        L.gearBtn = {L.menuBtn.left - step, hbTop, L.menuBtn.left - step + hb, hbTop + hb};
        L.addBtn = {L.gearBtn.left - step, hbTop, L.gearBtn.left - step + hb, hbTop + hb};
    }

    if (group.collapsed) {
        L.viewport = {L.header.left, L.header.bottom, L.header.right, L.header.bottom};
        L.cardH = L.header.bottom + Sc(5, dpi_) - cardTop;
    } else {
        L.viewport = {cardLeft + pad, L.header.bottom + Sc(2, dpi_), cardLeft + L.cardW - pad,
                      L.header.bottom + Sc(2, dpi_) + viewH};
        L.cardH = L.viewport.bottom + pad - cardTop;
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

    L.cells.clear();
    if (!group.collapsed) {
        const int slack = static_cast<int>(L.viewport.right - L.viewport.left) - L.cellW * group.columns;
        const int gridLeft = static_cast<int>(L.viewport.left) + (slack > 0 ? slack / 2 : 0);
        for (size_t k = 0; k < group.items.size(); k++) {
            const int row = static_cast<int>(k) / group.columns;
            const int col = static_cast<int>(k) % group.columns;
            const int left = gridLeft + col * L.cellW;
            const int top = L.viewport.top - scroll_ + row * L.cellH;
            L.cells.push_back({left, top, left + L.cellW, top + L.cellH});
        }
    }
}

void Block::ClampScroll() {
    const int viewH = layout_.viewport.bottom - layout_.viewport.top;
    const int maxScroll = layout_.contentH > viewH ? layout_.contentH - viewH : 0;
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

void Block::DrawItem(Graphics& g, const RECT& cell, const Shortcut& item, bool hot, bool faded, bool hotMenu,
                     bool selected) {
    const Group& group = Data();
    const RectF box = ToRectF(cell);
    const float u = ScF(1.0f, dpi_);
    if (hot || selected) {
        RectF bg(box.X + ScF(3.0f, dpi_), box.Y + ScF(2.0f, dpi_), box.Width - ScF(6.0f, dpi_),
                 box.Height - ScF(4.0f, dpi_));
        gfx::FillRoundRect(g, bg, ScF(10.0f, dpi_), selected ? WithAlpha(P().accent, 0x26) : P().hover);
        gfx::StrokeRoundRect(g, bg, ScF(10.0f, dpi_), WithAlpha(P().accent, selected ? 0xCC : 0x55),
                             selected ? 1.6f * u : u);
    }

    const int px = Sc(group.iconSize, dpi_);
    const float iconX = box.X + (box.Width - px) / 2.0f;
    const float iconY = box.Y + ScF(6.0f, dpi_);

    ImageAttributes attrs;
    ColorMatrix matrix = {{{1, 0, 0, 0, 0},
                           {0, 1, 0, 0, 0},
                           {0, 0, 1, 0, 0},
                           {0, 0, 0, faded ? 0.35f : 1.0f, 0},
                           {0, 0, 0, 0, 1}}};
    attrs.SetColorMatrix(&matrix);

    if (Bitmap* icon = icons::Get(item.path, px)) {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        const RectF dest(iconX, iconY, static_cast<float>(px), static_cast<float>(px));
        g.DrawImage(icon, dest, 0.0f, 0.0f, static_cast<float>(icon->GetWidth()),
                    static_cast<float>(icon->GetHeight()), UnitPixel, &attrs);
    } else {
        // Ссылка или недоступный файл: кружок с первой буквой подписи.
        RectF circle(iconX, iconY, static_cast<float>(px), static_cast<float>(px));
        const ARGB fill = faded ? ((P().accent & 0x00FFFFFF) | 0x60000000) : P().accent;
        gfx::FillRoundRect(g, circle, px / 2.0f, fill);
        const std::wstring letter = item.name.empty() ? L"?" : item.name.substr(0, 1);
        Font big(fonts_.family.get(), static_cast<float>(px) * 0.45f, FontStyleBold, UnitPixel);
        gfx::DrawText(g, letter.c_str(), big, P().onAccent, circle.X + circle.Width / 2.0f,
                      circle.Y + circle.Height * 0.26f, gfx::Align::Center);
    }

    if (item.broken) {
        // Точка в углу значка: цель ярлыка не найдена.
        const float dot = ScF(7.0f, dpi_);
        SolidBrush brush(Color(P().danger));
        g.FillEllipse(&brush, iconX + px - dot, iconY, dot, dot);
    }

    if (group.showLabels) {
        const float maxW = box.Width - ScF(8.0f, dpi_);
        const std::wstring label = gfx::Ellipsize(g, item.name, *fonts_.item, maxW);
        ARGB color = P().textMuted;
        if (item.broken) color = P().danger;
        else if (faded) color = P().textFaint;
        else if (hot) color = P().textPrimary;
        gfx::DrawText(g, label.c_str(), *fonts_.item, color, box.X + box.Width / 2.0f,
                      iconY + px + ScF(5.0f, dpi_), gfx::Align::Center);
    }

    // Кнопка меню ярлыка — видна только под курсором, чтобы сетка не рябила.
    if (hot) {
        const RECT menu = CellMenuRect(cell, dpi_);
        const RectF mf = ToRectF(menu);
        gfx::FillRoundRect(g, mf, mf.Width / 2.0f, hotMenu ? P().accent : P().buttonBgHover);
        SolidBrush dots(Color(hotMenu ? P().onAccent : P().textPrimary));
        const float cx = mf.X + mf.Width / 2.0f;
        const float cy = mf.Y + mf.Height / 2.0f;
        for (int i = -1; i <= 1; i++) {
            g.FillEllipse(&dots, cx - 1.1f * u, cy + i * 3.4f * u - 1.1f * u, 2.2f * u, 2.2f * u);
        }
    }
}

void Block::DrawTooltip(Graphics& g) {
    if ((hover_.zone != Zone::Item && hover_.zone != Zone::ItemMenu) || g_app.Dragging()) return;
    const Group& group = Data();
    if (hover_.index < 0 || hover_.index >= static_cast<int>(layout_.cells.size())) return;
    const Shortcut& item = group.items[static_cast<size_t>(hover_.index)];

    const RECT& cell = layout_.cells[static_cast<size_t>(hover_.index)];
    const float pad = ScF(9.0f, dpi_);
    const float h = ScF(24.0f, dpi_);
    const float maxW = static_cast<float>(layout_.cardW) - ScF(16.0f, dpi_);
    const std::wstring text = gfx::Ellipsize(g, item.name, *fonts_.tip, maxW - pad * 2);
    const float w = gfx::TextWidth(g, text.c_str(), *fonts_.tip) + pad * 2;

    // Под значком, а если снизу нет места — над ним; по горизонтали держим внутри карточки.
    float x = (cell.left + cell.right) / 2.0f - w / 2.0f;
    float y = static_cast<float>(cell.bottom) - ScF(2.0f, dpi_);
    if (y + h > layout_.card.bottom - ScF(4.0f, dpi_)) y = static_cast<float>(cell.top) - h + ScF(2.0f, dpi_);
    const float leftLimit = static_cast<float>(layout_.card.left) + ScF(6.0f, dpi_);
    const float rightLimit = static_cast<float>(layout_.card.right) - ScF(6.0f, dpi_) - w;
    if (x < leftLimit) x = leftLimit;
    if (x > rightLimit) x = rightLimit;

    RectF tip(x, y, w, h);
    gfx::FillRoundRect(g, tip, h / 2.0f, P().dark ? 0xF0000000 : 0xF0FFFFFF);
    gfx::StrokeRoundRect(g, tip, h / 2.0f, P().cardBorder, ScF(1.0f, dpi_));
    gfx::DrawText(g, text.c_str(), *fonts_.tip, item.broken ? P().danger : P().textPrimary, x + pad,
                  y + (h - ScF(15.0f, dpi_)) / 2.0f);
}

void Block::Render() {
    const Group& group = Data();
    // Оформление у каждого блока своё, поэтому палитру ставим перед каждой отрисовкой.
    SetTheme(group.darkTheme, group.accent);

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
    const bool isDropTarget = g_app.DropGroup() == groupIndex_;

    // Прозрачность блока — это прозрачность фона: значки и подписи всегда чёткие,
    // поэтому гасим сами цвета карточки, а не всё окно целиком.
    const int op = group.opacity;
    const ARGB bgTop = Fade(Mix(P().cardBgTop, P().accent, 0.10f), op);
    const ARGB bgBottom = Fade(Mix(P().cardBg, P().accent, 0.04f), op);
    const ARGB border = isDropTarget ? P().accent : Fade(Mix(P().cardBorder, P().accent, 0.5f), op);

    gfx::DropShadow(g, cardRect, radius, L.shadow, op);
    gfx::FillRoundRectGradient(g, cardRect, radius, bgTop, bgBottom);
    gfx::StrokeRoundRect(g, cardRect, radius, border, isDropTarget ? 1.6f * u : u);
    gfx::TopHighlight(g, cardRect, radius, Fade(P().cardGloss, op), u);

    // --- Заголовок блока ---
    const RectF headerF = ToRectF(L.header);
    const bool headerHot = hover_.zone == Zone::Header;

    {
        // Треугольник — акцентного цвета: по нему блок и узнаётся издалека.
        const float cx = headerF.X + ScF(9.0f, dpi_);
        const float cy = headerF.Y + headerF.Height / 2.0f;
        const float s = ScF(4.0f, dpi_);
        SolidBrush brush(Color(headerHot ? P().accent : WithAlpha(P().accent, 0xCC)));
        PointF tri[3];
        if (group.collapsed) {
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
    // В узком блоке кнопок меньше, значит и место под название считается по другой границе.
    const float titleRight = static_cast<float>(L.compactHeader ? L.menuBtn.left : L.addBtn.left);
    const float titleMax = titleRight - titleX - ScF(L.compactHeader ? 26.0f : 38.0f, dpi_);
    const std::wstring title = gfx::Ellipsize(g, group.title, *fonts_.title, titleMax);
    gfx::DrawText(g, title.c_str(), *fonts_.title, P().textPrimary, titleX, titleY);

    if (!title.empty()) {
        // Счётчик ярлыков — в маленькой «пилюле», чтобы не сливался с названием.
        wchar_t countText[16];
        swprintf(countText, 16, L"%d", static_cast<int>(group.items.size()));
        const float textW = gfx::TextWidth(g, countText, *fonts_.count);
        const float h = ScF(16.0f, dpi_);
        const float w = max(textW + ScF(10.0f, dpi_), h);
        RectF pill(titleX + gfx::TextWidth(g, title.c_str(), *fonts_.title) + ScF(8.0f, dpi_),
                   headerF.Y + (headerF.Height - h) / 2.0f, w, h);
        gfx::FillRoundRect(g, pill, h / 2.0f, WithAlpha(P().accent, 0x2E));
        gfx::DrawText(g, countText, *fonts_.count, P().accent, pill.X + pill.Width / 2.0f,
                      pill.Y + (h - ScF(13.0f, dpi_)) / 2.0f, gfx::Align::Center);

        if (group.locked) {
            // Замочек рядом со счётчиком: блок закреплён и мышью не двигается.
            const float lx = pill.GetRight() + ScF(7.0f, dpi_);
            const float cy = headerF.Y + headerF.Height / 2.0f;
            Color c(P().textFaint);
            Pen pen(c, ScF(1.3f, dpi_));
            RectF body(lx, cy - 1.0f * u, 8 * u, 6.5f * u);
            gfx::FillRoundRect(g, body, 1.5f * u, P().textFaint);
            g.DrawArc(&pen, lx + 1.5f * u, cy - 5.5f * u, 5 * u, 6 * u, 180.0f, 180.0f);
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
            headerButton(L.addBtn, Glyph::Plus, hover_.zone == Zone::HeaderAdd);
            headerButton(L.gearBtn, Glyph::Gear, hover_.zone == Zone::HeaderSettings);
        }
        headerButton(L.menuBtn, Glyph::Dots, hover_.zone == Zone::HeaderMenu);
    }

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

    if (!group.collapsed) {
        // Акцентная линия под заголовком: слева насыщенная, справа тает.
        RectF line(headerF.X, static_cast<float>(L.header.bottom), headerF.Width, 1.5f * u);
        gfx::FadeLine(g, line, WithAlpha(P().accent, 0xB0));
    }

    // --- Содержимое ---
    if (!group.collapsed) {
        g.SetClip(Rect(L.viewport.left, L.viewport.top, L.viewport.right - L.viewport.left,
                       L.viewport.bottom - L.viewport.top));

        if (group.items.empty()) {
            RectF hint(static_cast<float>(L.viewport.left), static_cast<float>(L.viewport.top) + ScF(2.0f, dpi_),
                       static_cast<float>(L.viewport.right - L.viewport.left),
                       static_cast<float>(L.viewport.bottom - L.viewport.top) - ScF(8.0f, dpi_));
            gfx::DashRoundRect(g, hint, ScF(8.0f, dpi_), isDropTarget ? P().accent : P().separator, u);
            gfx::DrawText(g, L"Перетащите сюда ярлыки", *fonts_.item,
                          isDropTarget ? P().accent : P().textFaint, hint.X + hint.Width / 2.0f,
                          hint.Y + (hint.Height - ScF(14.0f, dpi_)) / 2.0f, gfx::Align::Center);
        } else {
            for (size_t k = 0; k < L.cells.size(); k++) {
                const int index = static_cast<int>(k);
                const bool overCell = (hover_.zone == Zone::Item || hover_.zone == Zone::ItemMenu) &&
                                      hover_.index == index;
                const bool selected = g_app.IsSelected(groupIndex_, index);
                // При перетаскивании гаснут и сам значок, и вся выделенная пачка.
                const bool dragged = g_app.Dragging() && g_app.DragGroup() == groupIndex_ &&
                                     (g_app.DragIndex() == index || selected);
                DrawItem(g, L.cells[k], group.items[k], overCell && !g_app.Dragging(), dragged,
                         hover_.zone == Zone::ItemMenu && hover_.index == index, selected);
            }
        }

        // Указатель места вставки.
        if (isDropTarget && g_app.DropIndex() >= 0 && !L.cells.empty()) {
            const int index = min(g_app.DropIndex(), static_cast<int>(L.cells.size()));
            RECT caret;
            if (index < static_cast<int>(L.cells.size())) {
                caret = L.cells[static_cast<size_t>(index)];
                caret.right = caret.left;
            } else {
                caret = L.cells.back();
                caret.left = caret.right;
            }
            RectF bar(static_cast<float>(caret.left) - 1.5f * u, static_cast<float>(caret.top) + 4 * u,
                      3.0f * u, static_cast<float>(caret.bottom - caret.top) - 8 * u);
            gfx::FillRoundRect(g, bar, 1.5f * u, P().accent);
        }

        g.ResetClip();

        // Полоса прокрутки — только когда содержимое не помещается.
        const int viewH = L.viewport.bottom - L.viewport.top;
        if (L.contentH > viewH) {
            const float trackX = static_cast<float>(L.viewport.right) - 3 * u;
            const float trackH = static_cast<float>(viewH);
            const float thumbH = trackH * static_cast<float>(viewH) / static_cast<float>(L.contentH);
            const float offset = (trackH - thumbH) * static_cast<float>(scroll_) /
                                 static_cast<float>(L.contentH - viewH);
            gfx::FillRoundRect(g, RectF(trackX, static_cast<float>(L.viewport.top), 3 * u, trackH), 1.5f * u,
                               P().separator);
            gfx::FillRoundRect(g, RectF(trackX, static_cast<float>(L.viewport.top) + offset, 3 * u, thumbH),
                               1.5f * u, P().textFaint);
        }
    }

    DrawTooltip(g);

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

// ---------- Блок: попадания и перетаскивание ----------

Hit Block::HitTest(POINT pt) const {
    const BlockLayout& L = layout_;
    // Края и уголок — размер блока тянут мышью, поэтому они важнее содержимого.
    // У закреплённого блока их нет: он не двигается и не меняет размер.
    if (!Data().locked) {
        if (PtInRect(&L.resizeCorner, pt)) return {Zone::ResizeWH, -1};
        if (PtInRect(&L.resizeRight, pt)) return {Zone::ResizeW, -1};
        if (!Data().collapsed && PtInRect(&L.resizeBottom, pt)) return {Zone::ResizeH, -1};
    }
    if (PtInRect(&L.addBtn, pt)) return {Zone::HeaderAdd, -1};
    if (PtInRect(&L.gearBtn, pt)) return {Zone::HeaderSettings, -1};
    if (PtInRect(&L.menuBtn, pt)) return {Zone::HeaderMenu, -1};
    if (PtInRect(&L.header, pt)) return {Zone::Header, -1};
    if (PtInRect(&L.viewport, pt)) {
        for (size_t k = 0; k < L.cells.size(); k++) {
            const RECT menu = CellMenuRect(L.cells[k], dpi_);
            if (PtInRect(&menu, pt)) return {Zone::ItemMenu, static_cast<int>(k)};
            if (PtInRect(&L.cells[k], pt)) return {Zone::Item, static_cast<int>(k)};
        }
    }
    return {Zone::None, -1};
}

bool Block::DropSpotAt(POINT pt, int& index) const {
    const BlockLayout& L = layout_;
    const Group& group = Data();
    const bool insideCard = PtInRect(&L.card, pt) != FALSE;
    if (!insideCard) return false;

    if (group.collapsed || group.items.empty() || !PtInRect(&L.viewport, pt)) {
        index = static_cast<int>(group.items.size());  // на заголовок или в пустой блок — в конец
        return true;
    }

    index = static_cast<int>(L.cells.size());
    for (size_t k = 0; k < L.cells.size(); k++) {
        const RECT& cell = L.cells[k];
        if (pt.y >= cell.bottom) continue;                                      // строка ниже
        if (pt.y >= cell.top && pt.x >= (cell.left + cell.right) / 2) continue;  // правее середины
        index = static_cast<int>(k);
        break;
    }
    return true;
}

// Ширина считается в значках, высота — в рядах: блок «щёлкает» по сетке и всегда ровный.
void Block::ApplyResize(POINT pt) {
    Group& group = MutableData();
    bool changed = false;

    if (resizeW_) {
        const int pad = Sc(kPad, dpi_);
        const int inner = static_cast<int>(pt.x) - (static_cast<int>(layout_.card.left) + pad);
        int columns = (inner + layout_.cellW / 2) / layout_.cellW;
        if (columns < kMinColumnsUi) columns = kMinColumnsUi;
        if (columns > kMaxColumnsUi) columns = kMaxColumnsUi;
        if (columns != group.columns) {
            group.columns = columns;
            changed = true;
        }
    }

    if (resizeH_ && !group.collapsed) {
        const int inner = static_cast<int>(pt.y) - static_cast<int>(layout_.viewport.top);
        int rows = (inner + layout_.cellH / 2) / layout_.cellH;
        if (rows < 1) rows = 1;
        if (rows > kMaxRowsUi) rows = kMaxRowsUi;
        const int total = group.items.empty()
                              ? 1
                              : (static_cast<int>(group.items.size()) + group.columns - 1) / group.columns;
        const int value = (rows >= total) ? 0 : rows;  // 0 — показывать все ряды
        if (value != group.maxRows) {
            group.maxRows = value;
            changed = true;
        }
    }

    if (changed) Refresh();
}

void Block::AutoScrollFor(POINT pt) {
    const int viewH = layout_.viewport.bottom - layout_.viewport.top;
    if (layout_.contentH <= viewH) return;
    const int edge = Sc(24, dpi_);
    const int before = scroll_;
    if (pt.y < layout_.viewport.top + edge) scroll_ -= Sc(14, dpi_);
    else if (pt.y > layout_.viewport.bottom - edge) scroll_ += Sc(14, dpi_);
    ClampScroll();
    if (scroll_ != before) Refresh();
}

bool Block::OnDragOver(POINT pt, bool copyMode) {
    (void)copyMode;
    int index = -1;
    if (!DropSpotAt(pt, index)) {
        if (g_app.DropGroup() == groupIndex_) g_app.UpdateDragTarget(-1, -1);
        return false;
    }
    AutoScrollFor(pt);
    g_app.UpdateDragTarget(groupIndex_, index);
    return true;
}

void Block::OnDragLeave() {
    if (g_app.DropGroup() == groupIndex_) g_app.UpdateDragTarget(-1, -1);
}

void Block::OnDrop(const std::vector<std::wstring>& paths, POINT pt, bool copyMode) {
    int index = -1;
    if (!DropSpotAt(pt, index)) index = static_cast<int>(Data().items.size());
    g_app.UpdateDragTarget(-1, -1);
    // Диалогов здесь не показываем: мы внутри цикла перетаскивания проводника.
    // Дубликаты просто не добавляются — это видно и так.
    g_app.AddPaths(groupIndex_, index, paths, copyMode);
    Refresh();
}

// ---------- Блок: окно ----------

// Блок живёт в одном из двух состояний:
//   «на рабочем столе» — дочернее окно Progman. Тогда он и есть часть рабочего стола:
//      никогда не оказывается над чужими окнами (даже когда по столу кликнули мышью или
//      это второй монитор), но виден вместе со столом, в том числе по Win+D;
//   «поверх всех окон» — обычное окно верхнего уровня с HWND_TOPMOST.
void Block::ApplyTopmost() {
    if (!hwnd_) return;
    const bool onTop = g_app.Data().alwaysOnTop;
    HWND desktop = onTop ? nullptr : DesktopHostWindow();
    const HWND parent = GetParent(hwnd_);

    if (desktop) {
#if WIDGET_DESKTOP_MODE == 4
        // Родителя не меняем: блок остаётся окном верхнего уровня, но в z-порядке стоит
        // сразу над рабочим столом, то есть ниже любого чужого окна. Якорем может быть
        // только окно верхнего уровня, а хозяин значков бывает и дочерним WorkerW.
        if (parent) {
            Reparent(nullptr);
            PlaceWindow();
            Render();
        }
        const HWND anchor = GetAncestor(desktop, GA_ROOT);
        SetWindowPos(hwnd_, anchor ? anchor : HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
        if (parent != desktop) {
            Reparent(desktop);
            PlaceWindow();  // координаты теперь считаются от клиентской области стола
            Render();
        }
        // Внутри рабочего стола держимся поверх значков, но ниже любого обычного окна.
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
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

// Проводник мог перезапуститься: старый Progman уничтожен вместе с нашими окнами,
// и блок нужно вернуть новому рабочему столу. Само окно пересоздаёт App::EnsureBlocks.
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

// Шаг переноса, растягивания или перетаскивания ярлыка. Курсор и кнопку читаем напрямую:
// сообщения о движении приходят только пока курсор над блоком, а во всех трёх случаях он
// оказывается за его краем.
void Block::MouseTick() {
    const bool busy = moving_ || resizing_ || headerPending_ || itemPending_ || g_app.Dragging();
    if (!busy) {
        EndMouseAction();
        return;
    }
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        EndMouseAction();
        return;
    }

    POINT cursor;
    GetCursorPos(&cursor);
    const int threshold = Sc(kDragThreshold, dpi_);
    const int dx = cursor.x - dragCursor_.x;
    const int dy = cursor.y - dragCursor_.y;

    if (headerPending_) {
        // Нажали на заголовок: пока не сдвинули дальше порога — это клик «свернуть».
        if (dx * dx + dy * dy < threshold * threshold) return;
        headerPending_ = false;
        BeginMove();
        return;
    }

    if (moving_) {
        MoveTo(dragOrigin_.x + dx, dragOrigin_.y + dy);
        return;
    }

    if (resizing_) {
        POINT local = cursor;
        ScreenToClient(hwnd_, &local);
        ApplyResize(local);
        return;
    }

    // Перетаскивание ярлыка: внутри блока, в соседний блок или наружу, в проводник.
    if (!g_app.Dragging()) {
        if (dx * dx + dy * dy < threshold * threshold) return;
        g_app.BeginDrag(groupIndex_, hover_.index);
    }
    Block* target = g_app.BlockAtScreen(cursor);
    if (!target) {
        // Утащили мимо всех блоков — отдаём ярлык системе: рабочий стол или папка
        // примут его как обычный файл.
        itemPending_ = false;
        suppressClick_ = true;
        KillTimer(hwnd_, kMouseTimerId);  // дальше мышь ведёт цикл перетаскивания OLE
        if (g_app.BeginExternalDrag(hwnd_)) return;
        suppressClick_ = false;
        SetTimer(hwnd_, kMouseTimerId, kMouseTimerMs, nullptr);
    }
    int index = -1;
    if (target) {
        POINT local = cursor;
        ScreenToClient(target->Hwnd(), &local);
        if (!target->DropSpotAt(local, index)) target = nullptr;
        else target->AutoScrollFor(local);
    }
    g_app.UpdateDragTarget(target ? target->GroupIndex() : -1, index);
}

void Block::EndMouseAction() {
    KillTimer(hwnd_, kMouseTimerId);
    const bool wasMoving = moving_;
    const bool wasResizing = resizing_;
    const bool wasDragging = g_app.Dragging();
    moving_ = false;
    resizing_ = false;
    headerPending_ = false;
    itemPending_ = false;

    // Порядок важен: ReleaseCapture шлёт WM_CAPTURECHANGED, а тот отменяет перенос ярлыка.
    // Поэтому сначала применяем перенос и только потом отпускаем мышь.
    if (wasDragging) g_app.EndDrag(true);
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
    if (!wasMoving && !wasResizing && !wasDragging) return;

    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    BuildLayout();
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
    Group& group = MutableData();
    if (group.x != r.left || group.y != r.top) {
        group.x = r.left;
        group.y = r.top;
        g_app.Save();
    }
}

void Block::PlaceWindow() {
    BuildLayout();
    Group& group = MutableData();
    int x = group.x;
    int y = group.y;

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
            x = mi.rcWork.right - layout_.winW - Sc(24, dpi_) - groupIndex_ * Sc(18, dpi_);
            y = mi.rcWork.top + Sc(24, dpi_) + groupIndex_ * Sc(30, dpi_);
        } else {
            x = 100 + groupIndex_ * 24;
            y = 100 + groupIndex_ * 24;
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
    group.x = x;
    group.y = y;

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

            // Перенос, растягивание и перетаскивание ярлыка ведёт таймер — здесь только
            // подсветка того, что под курсором.
            if (resizing_ || moving_ || headerPending_ || itemPending_ || g_app.Dragging()) {
                return 0;
            }

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
                hover_ = {};
                Render();
            }
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            pressPt_ = pt;
            GetCursorPos(&dragCursor_);  // от этой точки таймер отсчитывает порог сдвига
            const Hit hit = HitTest(pt);
            hover_ = hit;
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
                case Zone::HeaderAdd:
                    g_app.AddShortcut(groupIndex_);
                    return 0;
                case Zone::HeaderSettings: {
                    POINT screen = pt;
                    ClientToScreen(hwnd, &screen);
                    g_app.ShowSettings(groupIndex_, screen);
                    return 0;
                }
                case Zone::HeaderMenu: {
                    POINT screen = pt;
                    ClientToScreen(hwnd, &screen);
                    g_app.ShowBlockMenu(screen, groupIndex_);
                    return 0;
                }
                case Zone::ItemMenu: {
                    POINT screen = pt;
                    ClientToScreen(hwnd, &screen);
                    g_app.ShowItemMenu(screen, groupIndex_, hit.index);
                    return 0;
                }
                case Zone::Header:
                    // Клик — свернуть/развернуть, но если потянуть — перенос блока.
                    // Порог сдвига сторожит таймер: курсор уходит за край блока быстрее,
                    // чем успевает прийти сообщение о движении.
                    headerPending_ = true;
                    SetCapture(hwnd);
                    SetTimer(hwnd, kMouseTimerId, kMouseTimerMs, nullptr);
                    return 0;
                case Zone::Item: {
                    // Ctrl добавляет ярлык к выделению; дальше вся пачка тащится вместе.
                    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (ctrl) {
                        g_app.ToggleSelection(groupIndex_, hit.index);
                        Render();
                        return 0;
                    }
                    if (!g_app.IsSelected(groupIndex_, hit.index)) g_app.ClearSelection();
                    // Запуск — по отпусканию: пока кнопка нажата, ярлык ещё может уехать в другой блок.
                    itemPending_ = true;
                    SetCapture(hwnd);
                    SetTimer(hwnd, kMouseTimerId, kMouseTimerMs, nullptr);
                    Render();
                    return 0;
                }
                default:
                    break;
            }
            // Пустое место карточки — снять выделение и, если блок не закреплён, тащить его.
            if (g_app.HasSelection()) g_app.ClearSelection();
            if (Data().locked) {
                Render();
                return 0;
            }
            BeginMove();
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (suppressClick_) {
                suppressClick_ = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                hover_ = HitTest(pt);
                Render();
                return 0;
            }
            // Перенос блока, растягивание и перетаскивание ярлыка заканчиваем в одном месте.
            if (moving_ || resizing_ || g_app.Dragging()) {
                EndMouseAction();
                return 0;
            }

            const bool wasHeader = headerPending_;
            const bool wasItem = itemPending_;
            const int itemIndex = hover_.index;

            headerPending_ = false;
            itemPending_ = false;
            KillTimer(hwnd, kMouseTimerId);  // сторож порога сдвига больше не нужен
            if (GetCapture() == hwnd) ReleaseCapture();

            if (wasHeader) {
                MutableData().collapsed = !Data().collapsed;
                scroll_ = 0;
                hover_ = {};
                g_app.Save();
                Refresh();
                return 0;
            }
            if (wasItem) {
                hover_ = HitTest(pt);
                Render();
                g_app.Launch(groupIndex_, itemIndex, false);
                return 0;
            }
            return 0;
        }

        // Клик второй раз подряд приходит сюда, а не как WM_LBUTTONDOWN: гасим его,
        // иначе привычный двойной клик запускал бы программу дважды.
        case WM_LBUTTONDBLCLK:
            return 0;

        case WM_TIMER:
            if (wp == kMouseTimerId) {
                MouseTick();
                return 0;
            }
            break;

        // Захват мыши у неактивного окна система забирает сама, и начатое действие от
        // этого прерываться не должно — его ведёт таймер, пока нажата кнопка.
        case WM_CAPTURECHANGED:
            if (!moving_ && !resizing_ && !itemPending_ && !g_app.Dragging()) {
                headerPending_ = false;
            }
            return 0;

        case WM_RBUTTONUP: {
            if (g_app.Dragging()) {
                g_app.EndDrag(false);
                return 0;
            }
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Hit hit = HitTest(pt);
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            if (hit.zone == Zone::Item) g_app.ShowItemMenu(screen, groupIndex_, hit.index);
            else g_app.ShowBlockMenu(screen, groupIndex_);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int viewH = layout_.viewport.bottom - layout_.viewport.top;
            if (layout_.contentH <= viewH) return 0;
            scroll_ += (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1) * layout_.cellH / 2;
            ClampScroll();
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            BuildLayout();
            hover_ = HitTest(pt);
            Render();
            return 0;
        }

        // Alt+F4 по блоку не должен «терять» его насовсем: блок — это группа ярлыков,
        // а не документ. Прячем все блоки в трей, оттуда они возвращаются.
        case WM_CLOSE:
            g_app.ToggleVisible();
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            fonts_.Rebuild(dpi_);
            icons::Clear();  // значки кэшированы в пикселях прежнего DPI
            auto* suggested = reinterpret_cast<RECT*>(lp);
            Group& group = MutableData();
            group.x = suggested->left;
            group.y = suggested->top;
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
            UnregisterDrops(hwnd);
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

bool Block::Create(HINSTANCE inst, int groupIndex) {
    groupIndex_ = groupIndex;

    // WS_EX_NOACTIVATE: блок принимает клики, но не забирает фокус — рабочий стол и чужие
    // окна не «мигают» активацией, когда по блоку просто ткнули мышью.
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBlockClass,
                            L"Shortcut Dock", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    dpi_ = WindowDpi(hwnd_);
    fonts_.Rebuild(dpi_);

    PlaceWindow();
    ApplyTopmost();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Render();
    RegisterDrops(hwnd_, this);
    return true;
}

void Block::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

// ---------- Приложение ----------

Block* App::BlockForGroup(int index) {
    for (auto& block : blocks_) {
        if (block->GroupIndex() == index) return block.get();
    }
    return nullptr;
}

// Окно блока — дочернее окно рабочего стола, и владельцем диалога быть не может: им стал бы
// сам рабочий стол, а его на время диалога отключать нельзя. Поэтому владелец — скрытое окно
// приложения, которое мы просто подгоняем под блок, чтобы системные окна вставали по центру
// именно его.
HWND App::OwnerFor(int group) {
    if (Block* block = BlockForGroup(group)) {
        if (block->Hwnd()) {
            RECT r{};
            GetWindowRect(block->Hwnd(), &r);
            SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    return hwnd_;
}

HWND App::AnchorFor(int group) {
    if (Block* block = BlockForGroup(group)) return block->Hwnd();
    if (!blocks_.empty()) return blocks_.front()->Hwnd();
    return hwnd_;
}

// Пока идёт ввод, блоки не принимают мышь: владелец диалога — скрытое окно, и обычной
// модальности через EnableWindow(owner) тут не хватает.
bool App::RunDialog(int group, DialogSpec& spec) {
    for (auto& block : blocks_) {
        if (block->Hwnd()) EnableWindow(block->Hwnd(), FALSE);
    }
    const bool ok = ShowDialog(OwnerFor(group), AnchorFor(group), spec);
    for (auto& block : blocks_) {
        if (block->Hwnd()) EnableWindow(block->Hwnd(), TRUE);
    }
    return ok;
}

Block* App::BlockAtScreen(POINT screenPt) {
    // Под курсором может лежать чужое окно — тогда бросать некуда.
    HWND under = WindowFromPoint(screenPt);
    for (auto& block : blocks_) {
        if (block->Hwnd() == under) return block.get();
    }
    return nullptr;
}

void App::RenderAll() {
    for (auto& block : blocks_) block->Render();
}

void App::RefreshAll() {
    for (auto& block : blocks_) block->Refresh();
}

void App::ReindexBlocks() {
    for (size_t i = 0; i < blocks_.size(); i++) blocks_[i]->SetGroupIndex(static_cast<int>(i));
}

int App::AddPaths(int group, int index, const std::vector<std::wstring>& paths, bool copyMode) {
    if (group < 0 || group >= static_cast<int>(data_.groups.size())) return 0;
    Group& target = data_.groups[static_cast<size_t>(group)];
    if (index < 0 || index > static_cast<int>(target.items.size())) {
        index = static_cast<int>(target.items.size());
    }

    int added = 0;
    for (const std::wstring& raw : paths) {
        std::wstring path = raw;
        const bool virtualItem = IsUrl(path) || IsShellPath(path);
        if (!virtualItem && !PathExists(path)) continue;

        // Ярлык переезжает в блок целиком: файл .lnk уходит с рабочего стола в папку
        // виджета. С зажатым Ctrl он копируется, и оригинал остаётся на месте.
        if (data_.moveIntoStorage && !virtualItem) {
            const std::wstring ext = FileExtLower(path);
            if (ext == L".lnk" || ext == L".url") {
                std::wstring stored;
                const bool ok = copyMode ? CopyFileIntoStorage(path, stored)
                                         : MoveFileIntoStorage(path, stored);
                if (ok) path = stored;
            }
        }

        bool duplicate = false;
        for (const Shortcut& existing : target.items) {
            if (_wcsicmp(existing.path.c_str(), path.c_str()) == 0) duplicate = true;
        }
        if (duplicate) continue;

        Shortcut item;
        item.path = path;
        item.name = DefaultName(path);
        item.broken = !ShortcutTargetExists(path);
        target.items.insert(target.items.begin() + index, std::move(item));
        index++;
        added++;
    }

    if (added > 0) {
        Save();
        UpdateTrayTip();
    }
    return added;
}

void App::AddGroup() {
    SetTheme(data_.darkTheme, data_.accent);  // диалог — в оформлении по умолчанию
    DialogSpec spec;
    spec.title = L"Новый блок";
    spec.okLabel = L"Создать";
    spec.fields.push_back({L"Название", L"Например: Работа, Игры, Документы", L"", true});
    if (!RunDialog(ctxGroup_, spec)) return;

    // Новый блок наследует оформление, которое пользователь выбрал последним.
    Group group;
    group.title = spec.fields[0].value;
    group.darkTheme = data_.darkTheme;
    group.accent = data_.accent;
    group.opacity = data_.opacity;
    group.iconSize = data_.iconSize;
    group.showLabels = data_.showLabels;
    if (!data_.groups.empty()) group.columns = data_.groups.back().columns;
    data_.groups.push_back(std::move(group));

    auto block = std::unique_ptr<Block>(new Block());
    if (!block->Create(inst_, static_cast<int>(data_.groups.size()) - 1)) {
        data_.groups.pop_back();
        return;
    }
    blocks_.push_back(std::move(block));
    Save();
    UpdateTrayTip();
}

void App::RenameGroup(int group) {
    if (group < 0 || group >= static_cast<int>(data_.groups.size())) return;
    Group& target = data_.groups[static_cast<size_t>(group)];
    SetTheme(target.darkTheme, target.accent);  // диалог — в оформлении своего блока

    DialogSpec spec;
    spec.title = L"Название блока";
    spec.fields.push_back({L"Название", L"", target.title, true});
    if (!RunDialog(group, spec)) return;

    target.title = spec.fields[0].value;
    Save();
    if (Block* block = BlockForGroup(group)) block->Refresh();
}

void App::DeleteGroup(int group) {
    if (group < 0 || group >= static_cast<int>(data_.groups.size())) return;
    if (data_.groups.size() <= 1) {
        MessageBoxW(OwnerFor(group), L"Это единственный блок — удалить его нельзя.\nМожно изменить название.",
                    L"Блоки", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Group& target = data_.groups[static_cast<size_t>(group)];
    if (!target.items.empty()) {
        const std::wstring message = L"Удалить блок «" + target.title + L"» вместе с ярлыками (" +
                                     std::to_wstring(target.items.size()) +
                                     L" шт.)?\nСами программы и файлы останутся на месте.";
        if (MessageBoxW(OwnerFor(group), message.c_str(), L"Блоки", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    if (Block* block = BlockForGroup(group)) block->Close();
    for (size_t i = 0; i < blocks_.size(); i++) {
        if (blocks_[i]->GroupIndex() == group) {
            blocks_.erase(blocks_.begin() + static_cast<long>(i));
            break;
        }
    }
    data_.groups.erase(data_.groups.begin() + group);
    ReindexBlocks();
    Save();
    UpdateTrayTip();
    RefreshAll();
}

void App::AddShortcut(int group) {
    if (group < 0 || group >= static_cast<int>(data_.groups.size())) return;
    std::vector<std::wstring> paths;
    if (!PickFiles(OwnerFor(group), paths)) return;
    const int added = AddPaths(group, static_cast<int>(data_.groups[static_cast<size_t>(group)].items.size()),
                               paths);
    if (Block* block = BlockForGroup(group)) block->Refresh();
    if (added == 0) {
        MessageBoxW(OwnerFor(group), L"Эти ярлыки уже есть в блоке.", L"Ярлыки", MB_OK | MB_ICONINFORMATION);
    }
}

void App::RenameItem(int group, int index) {
    if (group < 0 || group >= static_cast<int>(data_.groups.size())) return;
    Group& target = data_.groups[static_cast<size_t>(group)];
    if (index < 0 || index >= static_cast<int>(target.items.size())) return;
    Shortcut& item = target.items[static_cast<size_t>(index)];
    SetTheme(target.darkTheme, target.accent);

    DialogSpec spec;
    spec.title = L"Подпись ярлыка";
    spec.okLabel = L"Сохранить";
    spec.fields.push_back({L"Подпись", ShortcutTargetLabel(item.path), item.name, true});
    spec.fields.push_back({L"Аргументы", L"Передаются программе при запуске, обычно пусто", item.args, false});
    if (!RunDialog(group, spec)) return;

    item.name = spec.fields[0].value;
    item.args = spec.fields[1].value;
    Save();
    if (Block* block = BlockForGroup(group)) block->Refresh();
}

void App::RemoveItem(int group, int index, bool quiet) {
    if (group < 0 || group >= static_cast<int>(data_.groups.size())) return;
    Group& target = data_.groups[static_cast<size_t>(group)];
    if (index < 0 || index >= static_cast<int>(target.items.size())) return;

    // Ярлык, перенесённый в хранилище виджета, больше нигде не лежит — предлагаем удалить файл.
    const std::wstring path = target.items[static_cast<size_t>(index)].path;
    bool deleteFile = false;
    if (IsInStorage(path) && !quiet) {
        const std::wstring message = L"Убрать «" + target.items[static_cast<size_t>(index)].name +
                                     L"» из блока?\n\nФайл ярлыка лежит в хранилище виджета.\n"
                                     L"Да — удалить и файл, Нет — оставить файл в хранилище.";
        const int answer = MessageBoxW(OwnerFor(group), message.c_str(), L"Убрать ярлык",
                                       MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer == IDCANCEL) return;
        deleteFile = (answer == IDYES);
    }

    target.items.erase(target.items.begin() + index);
    if (deleteFile) DeleteFileW(path.c_str());
    Save();
    UpdateTrayTip();
    if (!quiet) {
        if (Block* block = BlockForGroup(group)) block->Refresh();
    }
}

void App::MoveItem(int fromGroup, int fromIndex, int toGroup, int toIndex) {
    if (fromGroup < 0 || fromGroup >= static_cast<int>(data_.groups.size())) return;
    if (toGroup < 0 || toGroup >= static_cast<int>(data_.groups.size())) return;
    Group& source = data_.groups[static_cast<size_t>(fromGroup)];
    if (fromIndex < 0 || fromIndex >= static_cast<int>(source.items.size())) return;

    Shortcut moved = source.items[static_cast<size_t>(fromIndex)];
    source.items.erase(source.items.begin() + fromIndex);

    Group& target = data_.groups[static_cast<size_t>(toGroup)];
    if (fromGroup == toGroup && toIndex > fromIndex) toIndex--;  // индекс сдвинулся после удаления
    if (toIndex < 0) toIndex = 0;
    if (toIndex > static_cast<int>(target.items.size())) toIndex = static_cast<int>(target.items.size());
    target.items.insert(target.items.begin() + toIndex, std::move(moved));

    Save();
    if (Block* block = BlockForGroup(fromGroup)) block->Refresh();
    if (toGroup != fromGroup) {
        if (Block* block = BlockForGroup(toGroup)) block->Refresh();
    }
}

bool App::IsSelected(int group, int index) const {
    if (group != selGroup_) return false;
    for (int i : selection_) {
        if (i == index) return true;
    }
    return false;
}

void App::ToggleSelection(int group, int index) {
    if (group != selGroup_) {
        selGroup_ = group;
        selection_.clear();
    }
    for (size_t i = 0; i < selection_.size(); i++) {
        if (selection_[i] == index) {
            selection_.erase(selection_.begin() + static_cast<long>(i));
            if (selection_.empty()) selGroup_ = -1;
            return;
        }
    }
    selection_.push_back(index);
}

void App::ClearSelection() {
    if (selection_.empty()) return;
    const int group = selGroup_;
    selection_.clear();
    selGroup_ = -1;
    if (Block* block = BlockForGroup(group)) block->Render();
}

void App::MoveItems(int fromGroup, std::vector<int> indices, int toGroup, int toIndex) {
    if (indices.empty()) return;
    if (!HasGroup(fromGroup) || !HasGroup(toGroup)) return;
    Group& source = data_.groups[static_cast<size_t>(fromGroup)];

    std::sort(indices.begin(), indices.end());
    std::vector<Shortcut> moved;
    for (int i : indices) {
        if (i >= 0 && i < static_cast<int>(source.items.size())) moved.push_back(source.items[static_cast<size_t>(i)]);
    }
    if (moved.empty()) return;

    // Удаляем с конца, иначе индексы «поедут» прямо во время удаления.
    for (size_t k = indices.size(); k > 0; k--) {
        const int i = indices[k - 1];
        if (i >= 0 && i < static_cast<int>(source.items.size())) source.items.erase(source.items.begin() + i);
    }

    Group& target = data_.groups[static_cast<size_t>(toGroup)];
    if (fromGroup == toGroup) {
        int before = 0;
        for (int i : indices) {
            if (i < toIndex) before++;
        }
        toIndex -= before;
    }
    if (toIndex < 0) toIndex = 0;
    if (toIndex > static_cast<int>(target.items.size())) toIndex = static_cast<int>(target.items.size());
    target.items.insert(target.items.begin() + toIndex, moved.begin(), moved.end());

    selection_.clear();
    selGroup_ = -1;
    Save();
    if (Block* block = BlockForGroup(fromGroup)) block->Refresh();
    if (toGroup != fromGroup) {
        if (Block* block = BlockForGroup(toGroup)) block->Refresh();
    }
}

// «Вынести на рабочий стол»: ярлык из хранилища переезжает обратно, на остальное
// создаётся новый .lnk. Из блока запись при этом убирается.
void App::SendToDesktop(int group, int index) {
    if (!HasGroup(group)) return;
    Group& target = data_.groups[static_cast<size_t>(group)];
    if (index < 0 || index >= static_cast<int>(target.items.size())) return;
    const Shortcut item = target.items[static_cast<size_t>(index)];

    if (IsUrl(item.path) || IsShellPath(item.path)) {
        MessageBoxW(OwnerFor(group), L"Ссылку и системные папки на рабочий стол так не вынести.",
                    L"Ярлыки", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring desktopPath;
    if (!PlaceOnDesktop(item.path, item.name, item.args, desktopPath)) {
        MessageBoxW(OwnerFor(group), L"Не удалось положить ярлык на рабочий стол.", L"Ярлыки",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    target.items.erase(target.items.begin() + index);
    ClearSelection();
    Save();
    UpdateTrayTip();
    if (Block* block = BlockForGroup(group)) block->Refresh();
}

// Магнит: место блока подправляется в момент отпускания — по краям рабочей области,
// по сторонам соседей и с одинаковым зазором между блоками. Из всех подсказок
// выбирается ближайшая, поэтому окно не «скачет» между вариантами.
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

        tryX(left);                  // левые края вровень
        tryX(right - width);         // правые края вровень
        tryX(right + gap);           // рядом справа, через ровный зазор
        tryX(left - width - gap);    // рядом слева
        tryY(top);
        tryY(bottom - height);
        tryY(bottom + gap);          // ниже соседа, через ровный зазор
        tryY(top - height - gap);
    }

    rect.left = (bestDx <= threshold) ? bestX : x;
    rect.top = (bestDy <= threshold) ? bestY : y;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
}

void App::Launch(int group, int index, bool asAdmin) {
    if (group < 0 || group >= static_cast<int>(data_.groups.size())) return;
    Group& target = data_.groups[static_cast<size_t>(group)];
    if (index < 0 || index >= static_cast<int>(target.items.size())) return;
    const Shortcut& item = target.items[static_cast<size_t>(index)];

    if (item.broken) {
        const std::wstring message = L"Цель ярлыка не найдена:\n" + ShortcutTargetLabel(item.path) +
                                     L"\n\nПопробовать запустить всё равно?";
        if (MessageBoxW(OwnerFor(group), message.c_str(), L"Ярлыки", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    }
    LaunchShortcut(OwnerFor(group), item.path, item.args, asAdmin);
}

void App::SetAllCollapsed(bool collapsed) {
    for (Group& group : data_.groups) group.collapsed = collapsed;
    Save();
    RefreshAll();
}

void App::ToggleVisible() {
    visible_ = !visible_;
    for (auto& block : blocks_) block->Show(visible_);
}

// ---------- Перетаскивание ярлыка между блоками ----------

void App::BeginDrag(int group, int index) {
    drag_ = DragState{};
    drag_.active = true;
    drag_.fromGroup = group;
    drag_.fromIndex = index;
    RenderAll();
}

// Значок вытащили за пределы блоков: дальше работает системное перетаскивание,
// и рабочий стол сам решает, переместить файл или скопировать.
bool App::BeginExternalDrag(HWND from) {
    if (!drag_.active) return false;
    const int fromGroup = drag_.fromGroup;
    if (!HasGroup(fromGroup)) return false;

    std::vector<int> indices;
    if (selection_.size() > 1 && IsSelected(fromGroup, drag_.fromIndex)) indices = selection_;
    else indices.push_back(drag_.fromIndex);
    std::sort(indices.begin(), indices.end());

    const Group& group = data_.groups[static_cast<size_t>(fromGroup)];
    std::vector<std::wstring> paths;
    for (int i : indices) {
        if (i < 0 || i >= static_cast<int>(group.items.size())) continue;
        const std::wstring& path = group.items[static_cast<size_t>(i)].path;
        // Ссылку и виртуальную папку файлом не отдашь — такие остаются в блоке.
        if (IsUrl(path) || IsShellPath(path) || !PathExists(path)) continue;
        paths.push_back(path);
    }
    if (paths.empty()) return false;

    drag_ = DragState{};
    if (GetCapture() == from) ReleaseCapture();
    RenderAll();

    const DWORD effect = dragout::DragFiles(paths);

    // Рабочий стол и проводник часто делают «оптимизированное перемещение»: файл забирают,
    // а MOVE в ответе не возвращают. Поэтому смотрим ещё и на то, остался ли файл на месте.
    ClearSelection();
    bool changed = false;
    for (size_t k = indices.size(); k > 0; k--) {
        const int i = indices[k - 1];
        if (!HasGroup(fromGroup)) break;
        const Group& current = data_.groups[static_cast<size_t>(fromGroup)];
        if (i < 0 || i >= static_cast<int>(current.items.size())) continue;
        const std::wstring path = current.items[static_cast<size_t>(i)].path;
        if (IsUrl(path) || IsShellPath(path)) continue;
        if (effect == DROPEFFECT_MOVE || !PathExists(path)) {
            RemoveItem(fromGroup, i, true);
            changed = true;
        }
    }
    if (changed) {
        if (Block* block = BlockForGroup(fromGroup)) block->Refresh();
    } else {
        RenderAll();
    }
    return true;
}

void App::UpdateDragTarget(int group, int index) {
    if (drag_.toGroup == group && drag_.toIndex == index) return;
    const int previous = drag_.toGroup;
    drag_.toGroup = group;
    drag_.toIndex = index;
    if (group != drag_.springGroup) {
        drag_.springGroup = group;
        drag_.springSince = GetTickCount64();
    }
    if (previous >= 0) {
        if (Block* block = BlockForGroup(previous)) block->Render();
    }
    if (group >= 0) {
        if (Block* block = BlockForGroup(group)) block->Render();
    }
}

void App::EndDrag(bool apply) {
    if (!drag_.active) return;
    const DragState state = drag_;
    drag_ = DragState{};
    if (apply && state.toGroup >= 0) {
        // Если тянули один из выделенных значков — переезжает вся пачка.
        if (selection_.size() > 1 && IsSelected(state.fromGroup, state.fromIndex)) {
            MoveItems(state.fromGroup, selection_, state.toGroup, state.toIndex);
        } else {
            MoveItem(state.fromGroup, state.fromIndex, state.toGroup, state.toIndex);
        }
    }
    RenderAll();
}

// Свёрнутый блок раскрывается сам, если подержать над ним перетаскиваемое.
void App::SpringOpen() {
    if (drag_.springGroup < 0) return;
    if (GetTickCount64() - drag_.springSince < kSpringOpenMs) return;
    if (drag_.springGroup >= static_cast<int>(data_.groups.size())) return;
    Group& group = data_.groups[static_cast<size_t>(drag_.springGroup)];
    if (!group.collapsed) return;
    group.collapsed = false;
    drag_.springSince = GetTickCount64();
    Save();
    if (Block* block = BlockForGroup(drag_.springGroup)) block->Refresh();
}

// ---------- Меню ----------

void App::ShowSettings(int group, POINT screenPt) {
    if (!HasGroup(group)) return;
    // Окно настроек ставим рядом с курсором, чтобы не закрывало сам блок.
    POINT where = {screenPt.x + 20, screenPt.y};
    settings::Show(inst_, this, group, where, settings::Mode::Panel);
}

// Общие настройки: поведение, данные, новый блок. Про конкретный блок здесь ничего нет —
// он настраивается шестерёнкой в своём заголовке.
void App::ShowGlobalSettings(POINT screenPt) {
    POINT where = {screenPt.x + 20, screenPt.y};
    settings::Show(inst_, this, HasGroup(ctxGroup_) ? ctxGroup_ : 0, where, settings::Mode::Global);
}

// «Применить ко всем»: оформление образцового блока копируется в остальные,
// а место, размер сетки и содержимое у каждого остаются своими.
void App::ApplyStyleToAll(int group) {
    if (!HasGroup(group)) return;
    const Group& source = data_.groups[static_cast<size_t>(group)];
    for (Group& target : data_.groups) {
        target.darkTheme = source.darkTheme;
        target.accent = source.accent;
        target.opacity = source.opacity;
        target.iconSize = source.iconSize;
        target.showLabels = source.showLabels;
    }
    RefreshAll();
}

void App::ShowBlockMenu(POINT screenPt, int group) {
    if (group < 0 || group >= static_cast<int>(data_.groups.size())) return;
    ctxGroup_ = group;
    ctxIndex_ = -1;
    const Group& target = data_.groups[static_cast<size_t>(group)];

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdAddShortcut, L"Добавить ярлык…");
    AppendMenuW(menu, MF_STRING, kCmdRenameGroup, L"Переименовать блок…");
    AppendMenuW(menu, MF_STRING, kCmdToggleCollapse, target.collapsed ? L"Развернуть" : L"Свернуть");
    AppendMenuW(menu, MF_STRING | (target.locked ? MF_CHECKED : 0), kCmdToggleLock,
                L"Закрепить блок (не двигать)");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdAddGroup, L"Новый блок…");
    AppendMenuW(menu, MF_STRING | (data_.groups.size() > 1 ? 0 : MF_GRAYED), kCmdDeleteGroup,
                L"Удалить этот блок");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"Настройки блока…");
    AppendMenuW(menu, MF_STRING, kCmdGlobalSettings, L"Настройки виджета…");
    AppendMenuW(menu, MF_STRING, kCmdCollapseAll, L"Свернуть все блоки");
    AppendMenuW(menu, MF_STRING, kCmdExpandAll, L"Развернуть все блоки");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
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

void App::ShowItemMenu(POINT screenPt, int group, int index) {
    if (group < 0 || group >= static_cast<int>(data_.groups.size())) return;
    const Group& target = data_.groups[static_cast<size_t>(group)];
    if (index < 0 || index >= static_cast<int>(target.items.size())) return;
    ctxGroup_ = group;
    ctxIndex_ = index;
    const Shortcut& item = target.items[static_cast<size_t>(index)];

    // Если ярлык входит в выделение, все пункты относятся ко всей пачке.
    const int selCount = IsSelected(group, index) ? static_cast<int>(selection_.size()) : 1;
    const bool many = selCount > 1;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    if (!many) {
        AppendMenuW(menu, MF_STRING, kCmdItemOpen, L"Открыть");
        if (!IsUrl(item.path) && !IsShellPath(item.path)) {
            AppendMenuW(menu, MF_STRING, kCmdItemAdmin, L"Запуск от имени администратора");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCmdItemReveal, L"Показать в проводнике");
            AppendMenuW(menu, MF_STRING, kCmdItemProps, L"Свойства файла");
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdItemRename, L"Переименовать…");
    } else {
        wchar_t header[64];
        swprintf(header, 64, L"Выбрано %d %ls", selCount,
                 PluralRu(selCount, L"ярлык", L"ярлыка", L"ярлыков"));
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, header);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    if (data_.groups.size() > 1) {
        HMENU moveTo = CreatePopupMenu();
        for (size_t i = 0; i < data_.groups.size(); i++) {
            if (static_cast<int>(i) == group) continue;
            AppendMenuW(moveTo, MF_STRING, kCmdMoveToGroupBase + static_cast<UINT>(i),
                        data_.groups[i].title.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moveTo),
                    many ? L"Переместить всё в блок" : L"Переместить в блок");
    }
    AppendMenuW(menu, MF_STRING, kCmdItemToDesktop,
                many ? L"Вынести всё на рабочий стол" : L"Вынести на рабочий стол");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdItemRemove, many ? L"Убрать всё из блока" : L"Убрать из блока");

    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                      screenPt.x, screenPt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    RunCommand(cmd);
}

void App::ShowTrayMenu(POINT screenPt) {
    ctxGroup_ = 0;
    ctxIndex_ = -1;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdToggleVisible, visible_ ? L"Скрыть все блоки" : L"Показать все блоки");
    AppendMenuW(menu, MF_STRING, kCmdAddGroup, L"Новый блок…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdCollapseAll, L"Свернуть все блоки");
    AppendMenuW(menu, MF_STRING, kCmdExpandAll, L"Развернуть все блоки");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdGlobalSettings, L"Настройки виджета…");
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

    // Диапазоны проверяем от старших к младшим.
    if (cmd >= kCmdMoveToGroupBase) {
        const int target = static_cast<int>(cmd - kCmdMoveToGroupBase);
        if (target >= 0 && target < static_cast<int>(data_.groups.size())) {
            const int tail = static_cast<int>(data_.groups[static_cast<size_t>(target)].items.size());
            if (selection_.size() > 1 && IsSelected(ctxGroup_, ctxIndex_)) {
                MoveItems(ctxGroup_, selection_, target, tail);
            } else {
                MoveItem(ctxGroup_, ctxIndex_, target, tail);
            }
        }
        return;
    }

    switch (cmd) {
        case kCmdAddGroup: AddGroup(); break;
        case kCmdRenameGroup: RenameGroup(ctxGroup_); break;
        case kCmdDeleteGroup: DeleteGroup(ctxGroup_); break;
        case kCmdAddShortcut: AddShortcut(ctxGroup_); break;
        case kCmdToggleCollapse:
            if (ctxGroup_ >= 0 && ctxGroup_ < static_cast<int>(data_.groups.size())) {
                Group& group = data_.groups[static_cast<size_t>(ctxGroup_)];
                group.collapsed = !group.collapsed;
                Save();
                if (Block* block = BlockForGroup(ctxGroup_)) block->Refresh();
            }
            break;
        case kCmdCollapseAll: SetAllCollapsed(true); break;
        case kCmdExpandAll: SetAllCollapsed(false); break;
        case kCmdItemOpen: Launch(ctxGroup_, ctxIndex_, false); break;
        case kCmdItemAdmin: Launch(ctxGroup_, ctxIndex_, true); break;
        case kCmdItemReveal:
            if (ctxGroup_ >= 0 && ctxIndex_ >= 0) {
                const Group& group = data_.groups[static_cast<size_t>(ctxGroup_)];
                if (ctxIndex_ < static_cast<int>(group.items.size())) {
                    RevealInExplorer(group.items[static_cast<size_t>(ctxIndex_)].path);
                }
            }
            break;
        case kCmdItemProps:
            if (ctxGroup_ >= 0 && ctxIndex_ >= 0) {
                const Group& group = data_.groups[static_cast<size_t>(ctxGroup_)];
                if (ctxIndex_ < static_cast<int>(group.items.size())) {
                    ShowFileProperties(OwnerFor(ctxGroup_), group.items[static_cast<size_t>(ctxIndex_)].path);
                }
            }
            break;
        case kCmdItemRename: RenameItem(ctxGroup_, ctxIndex_); break;
        case kCmdItemRemove:
            if (selection_.size() > 1 && IsSelected(ctxGroup_, ctxIndex_)) {
                // Удаляем с конца, чтобы индексы оставшихся не сдвигались.
                std::vector<int> indices = selection_;
                std::sort(indices.begin(), indices.end());
                ClearSelection();
                for (size_t k = indices.size(); k > 0; k--) RemoveItem(ctxGroup_, indices[k - 1], true);
                if (Block* block = BlockForGroup(ctxGroup_)) block->Refresh();
            } else {
                RemoveItem(ctxGroup_, ctxIndex_, false);
            }
            break;
        case kCmdItemToDesktop:
            if (selection_.size() > 1 && IsSelected(ctxGroup_, ctxIndex_)) {
                std::vector<int> indices = selection_;
                std::sort(indices.begin(), indices.end());
                ClearSelection();
                for (size_t k = indices.size(); k > 0; k--) SendToDesktop(ctxGroup_, indices[k - 1]);
            } else {
                SendToDesktop(ctxGroup_, ctxIndex_);
            }
            break;
        case kCmdToggleLock:
            if (HasGroup(ctxGroup_)) {
                Group& group = data_.groups[static_cast<size_t>(ctxGroup_)];
                group.locked = !group.locked;
                Save();
                if (Block* block = BlockForGroup(ctxGroup_)) block->Refresh();
            }
            break;
        case kCmdSettings: {
            POINT pt;
            GetCursorPos(&pt);
            ShowSettings(ctxGroup_ >= 0 ? ctxGroup_ : 0, pt);
            break;
        }
        case kCmdGlobalSettings: {
            POINT pt;
            GetCursorPos(&pt);
            ShowGlobalSettings(pt);
            break;
        }
        case kCmdToggleVisible: ToggleVisible(); break;
        case kCmdExit: DestroyWindow(hwnd_); break;
        default: break;
    }
}

// ---------- Приложение: окно ----------

void App::UpdateTrayTip() {
    const int items = data_.TotalItems();
    const int groups = static_cast<int>(data_.groups.size());
    wchar_t tip[96];
    swprintf(tip, 96, L"Ярлыки: %d в %d %ls", items, groups, PluralRu(groups, L"блоке", L"блоках", L"блоках"));
    TrayUpdateTip(hwnd_, tip);
}

void App::ApplyTopmost() {
    for (auto& block : blocks_) block->ApplyTopmost();
}

void App::RefreshIcons() {
    icons::Clear();
    RefreshBrokenFlags(data_);
    RefreshAll();
}

void App::ApplyHotkey() {
    UnregisterHotKey(hwnd_, kHotkeyId);
    if (!data_.hotkeyEnabled) return;
    // Комбинацию мог занять кто-то другой — тогда просто живём без неё.
    RegisterHotKey(hwnd_, kHotkeyId, MOD_CONTROL | MOD_ALT, 'D');
}

// ---------- Центр виджетов ----------

void App::SetTrayShown(bool shown) {
    if (shown == trayShown_) return;
    if (shown) {
        trayShown_ = TrayAdd(hwnd_, kTrayMessage, icon_, L"Shortcut Dock");
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
            if (!visible_) return 0;
            for (auto& block : blocks_) block->KeepAttached();
            SpringOpen();
            return 0;

        case kTrayMessage:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                ShowTrayMenu(pt);
            } else if (LOWORD(lp) == WM_LBUTTONUP) {
                // Клик по значку — общие настройки виджета. Спрятать и вернуть блоки
                // можно из меню значка и по Ctrl+Alt+D.
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
            icons::Clear();
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
    for (size_t i = 0; i < data_.groups.size(); i++) {
        auto block = std::unique_ptr<Block>(new Block());
        if (block->Create(inst_, static_cast<int>(i))) blocks_.push_back(std::move(block));
    }
}

// Страховка от потерянных блоков: если окно кто-то закрыл, а группа осталась
// в данных, окно создаётся заново — иначе блок было бы не достать.
void App::EnsureBlocks() {
    bool restored = false;
    for (size_t i = 0; i < blocks_.size(); i++) {
        if (blocks_[i]->Hwnd()) continue;
        blocks_[i].reset(new Block());
        if (blocks_[i]->Create(inst_, static_cast<int>(i))) restored = true;
    }
    for (size_t i = blocks_.size(); i < data_.groups.size(); i++) {
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

    LoadData(data_);  // файла нет — остаются значения по умолчанию
    RefreshBrokenFlags(data_);
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
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAppClass, L"Shortcut Dock", WS_POPUP, 0, 0, 0, 0, nullptr,
                            nullptr, inst, nullptr);
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
