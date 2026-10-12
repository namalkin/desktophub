#include "widget.h"

#include <windowsx.h>

#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "audio.h"
#include "cover.h"
#include "dialog.h"
#include "hub_link.h"
#include "library.h"
#include "settings.h"
#include "store.h"
#include "sys.h"
#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClassName[] = L"TuneBoxMain";
constexpr wchar_t kOwnerClass[] = L"TuneBoxOwner";  // скрытое окно: трей, меню, диалоги, клавиши

constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 200;  // тик для полосы прогресса и событий сессии

// Пока блок тащат, мы ведём мышь сами: он — дочернее окно рабочего стола, активным не
// бывает, а захват мыши (SetCapture) у неактивного окна работает только пока курсор над ним.
constexpr UINT kMouseTimerId = 2;
constexpr UINT kMouseTimerMs = 15;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr int kIconResourceId = 1;

// Горячие клавиши. Мультимедийные идут отдельными номерами, чтобы их можно было
// выключить, не трогая остальные.
enum : int {
    kHotPlay = 1,
    kHotNext,
    kHotPrev,
    kHotVolUp,
    kHotVolDown,
    kHotMediaPlay,
    kHotMediaNext,
    kHotMediaPrev,
};

// Пункты меню.
enum : UINT {
    kCmdPlayPause = 100,
    kCmdNext,
    kCmdPrev,
    kCmdShuffle,
    kCmdRepeat,
    kCmdPickFolder,
    kCmdOpenFile,
    kCmdFind,
    kCmdClearFilter,
    kCmdRescan,
    kCmdRevealTrack,
    kCmdOpenMusicFolder,
    kCmdSettings,
    kCmdToggleTop,
    kCmdToggleLock,
    kCmdThemeDark,
    kCmdThemeLight,
    kCmdToggleCover,
    kCmdToggleList,
    kCmdToggleVisible,
    kCmdOpenData,
    kCmdExit,
};

// Логическая геометрия (96 DPI).
constexpr int kShadow = 18;
constexpr int kPad = 18;
constexpr int kCardRadius = 18;
constexpr int kCoverSize = 78;
constexpr int kRowH = 28;
constexpr int kMinCardW = 300;
constexpr int kMaxCardW = 620;
constexpr int kMaxListRows = 14;

// Панель быстрых кнопок слева от карточки, отделённая зазором.
constexpr int kRailW = 36;
constexpr int kRailBtn = 34;
constexpr int kRailPad = 5;
constexpr int kRailGap = 10;
constexpr int kRailLabelW = 150;  // прозрачное место слева под подписи кнопок

enum RailButton {
    kRailMenu = 0,
    kRailSettings,
    kRailFolder,
    kRailFind,
    kRailTheme,
    kRailPin,
    kRailHide,
    kRailClose,
    kRailCount,
};

const wchar_t* RailLabel(int button, bool dark, bool onTop) {
    switch (button) {
        case kRailMenu: return L"Меню";
        case kRailSettings: return L"Настройки";
        case kRailFolder: return L"Выбрать папку";
        case kRailFind: return L"Найти трек";
        case kRailTheme: return dark ? L"Светлая тема" : L"Тёмная тема";
        case kRailPin: return onTop ? L"Не поверх окон" : L"Поверх всех окон";
        case kRailHide: return L"Свернуть в трей";
        case kRailClose: return L"Закрыть плеер";
        default: return L"";
    }
}

// ---------- Значки ----------

enum class Glyph {
    Menu,
    Gear,
    Folder,
    Search,
    Theme,
    Pin,
    Hide,
    Close,
    Play,
    Pause,
    Prev,
    Next,
    Shuffle,
    Repeat,
    RepeatOne,
    Speaker,
    Mute,
    Note,
};

// Рисуем примитивами: значки одинаковы на любой машине и чётки в любом DPI.
void DrawGlyph(Graphics& g, Glyph glyph, const RectF& box, ARGB color, int dpi, float scale = 1.0f) {
    const float cx = box.X + box.Width / 2.0f;
    const float cy = box.Y + box.Height / 2.0f;
    const float u = ScF(1.0f, dpi) * scale;
    Color penColor(color);
    Pen pen(penColor, ScF(1.6f, dpi) * scale);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    SolidBrush brush(penColor);

    auto triangle = [&](float x, float halfH, float w) {
        PointF pts[3] = {PointF(x, cy - halfH), PointF(x, cy + halfH), PointF(x + w, cy)};
        g.FillPolygon(&brush, pts, 3);
    };

    switch (glyph) {
        case Glyph::Menu:
            for (int i = -1; i <= 1; i++) {
                g.DrawLine(&pen, cx - 6 * u, cy + i * 4.5f * u, cx + 6 * u, cy + i * 4.5f * u);
            }
            break;

        case Glyph::Gear: {
            const float r = 4.2f * u;
            g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);
            for (int i = 0; i < 6; i++) {
                const float a = static_cast<float>(i) * 3.14159265f / 3.0f;
                const float sx = cx + cosf(a) * (r + 1.4f * u);
                const float sy = cy + sinf(a) * (r + 1.4f * u);
                const float ex = cx + cosf(a) * (r + 3.4f * u);
                const float ey = cy + sinf(a) * (r + 3.4f * u);
                g.DrawLine(&pen, sx, sy, ex, ey);
            }
            break;
        }

        case Glyph::Folder: {
            RectF body(cx - 6.5f * u, cy - 4.0f * u, 13 * u, 9.5f * u);
            gfx::StrokeRoundRect(g, body, 1.8f * u, color, ScF(1.5f, dpi) * scale);
            g.DrawLine(&pen, body.X + 0.5f * u, cy - 4.0f * u, body.X + 4.0f * u, cy - 6.5f * u);
            g.DrawLine(&pen, body.X + 4.0f * u, cy - 6.5f * u, body.X + 7.0f * u, cy - 6.5f * u);
            break;
        }

        case Glyph::Search: {
            const float r = 4.2f * u;
            g.DrawEllipse(&pen, cx - r - 1.2f * u, cy - r - 1.2f * u, r * 2, r * 2);
            g.DrawLine(&pen, cx + 2.2f * u, cy + 2.2f * u, cx + 6.0f * u, cy + 6.0f * u);
            break;
        }

        case Glyph::Theme: {
            const float r = 6.0f * u;
            g.FillPie(&brush, cx - r, cy - r, r * 2, r * 2, 90.0f, 180.0f);
            g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);
            break;
        }

        case Glyph::Pin: {
            const float head = 3.2f * u;
            g.FillEllipse(&brush, cx - head, cy - 6 * u, head * 2, head * 2);
            g.DrawLine(&pen, cx, cy - 6 * u + head * 2, cx, cy + 6 * u);
            g.DrawLine(&pen, cx - 5 * u, cy + 1 * u, cx + 5 * u, cy + 1 * u);
            break;
        }

        case Glyph::Hide:
            g.DrawLine(&pen, cx - 5 * u, cy - 4 * u, cx, cy + 1 * u);
            g.DrawLine(&pen, cx + 5 * u, cy - 4 * u, cx, cy + 1 * u);
            g.DrawLine(&pen, cx - 6 * u, cy + 5 * u, cx + 6 * u, cy + 5 * u);
            break;

        case Glyph::Close:
            g.DrawLine(&pen, cx - 5 * u, cy - 5 * u, cx + 5 * u, cy + 5 * u);
            g.DrawLine(&pen, cx + 5 * u, cy - 5 * u, cx - 5 * u, cy + 5 * u);
            break;

        case Glyph::Play:
            triangle(cx - 3.6f * u, 6.2f * u, 10.0f * u);
            break;

        case Glyph::Pause: {
            const float w = 3.0f * u;
            const float h = 12.0f * u;
            g.FillRectangle(&brush, cx - 5.0f * u, cy - h / 2, w, h);
            g.FillRectangle(&brush, cx + 2.0f * u, cy - h / 2, w, h);
            break;
        }

        case Glyph::Prev: {
            PointF a[3] = {PointF(cx + 1.0f * u, cy - 5.5f * u), PointF(cx + 1.0f * u, cy + 5.5f * u),
                           PointF(cx - 7.0f * u, cy)};
            g.FillPolygon(&brush, a, 3);
            PointF b[3] = {PointF(cx + 7.0f * u, cy - 5.5f * u), PointF(cx + 7.0f * u, cy + 5.5f * u),
                           PointF(cx - 0.5f * u, cy)};
            g.FillPolygon(&brush, b, 3);
            break;
        }

        case Glyph::Next: {
            PointF a[3] = {PointF(cx - 1.0f * u, cy - 5.5f * u), PointF(cx - 1.0f * u, cy + 5.5f * u),
                           PointF(cx + 7.0f * u, cy)};
            g.FillPolygon(&brush, a, 3);
            PointF b[3] = {PointF(cx - 7.0f * u, cy - 5.5f * u), PointF(cx - 7.0f * u, cy + 5.5f * u),
                           PointF(cx + 0.5f * u, cy)};
            g.FillPolygon(&brush, b, 3);
            break;
        }

        case Glyph::Shuffle: {
            // Две перекрещенные стрелки: слева входят рядом, справа расходятся.
            g.DrawLine(&pen, cx - 7 * u, cy - 3.5f * u, cx - 3 * u, cy - 3.5f * u);
            g.DrawLine(&pen, cx - 3 * u, cy - 3.5f * u, cx + 4 * u, cy + 3.5f * u);
            g.DrawLine(&pen, cx - 7 * u, cy + 3.5f * u, cx - 3 * u, cy + 3.5f * u);
            g.DrawLine(&pen, cx - 3 * u, cy + 3.5f * u, cx + 4 * u, cy - 3.5f * u);
            g.DrawLine(&pen, cx + 4 * u, cy - 3.5f * u, cx + 6.5f * u, cy - 3.5f * u);
            g.DrawLine(&pen, cx + 4 * u, cy + 3.5f * u, cx + 6.5f * u, cy + 3.5f * u);
            g.DrawLine(&pen, cx + 4.0f * u, cy - 6.0f * u, cx + 6.8f * u, cy - 3.5f * u);
            g.DrawLine(&pen, cx + 4.0f * u, cy - 1.0f * u, cx + 6.8f * u, cy - 3.5f * u);
            g.DrawLine(&pen, cx + 4.0f * u, cy + 1.0f * u, cx + 6.8f * u, cy + 3.5f * u);
            g.DrawLine(&pen, cx + 4.0f * u, cy + 6.0f * u, cx + 6.8f * u, cy + 3.5f * u);
            break;
        }

        case Glyph::Repeat:
        case Glyph::RepeatOne: {
            // Петля с разрывом в правом верхнем боку и стрелкой в разрыве — ход по часовой.
            // Дуга идёт от 40° до 320°, поэтому стрелка стоит на её конце, по касательной
            // (вправо-вниз): координаты посчитаны от точки 320° на эллипсе.
            const float rx = 6.8f * u;
            const float ry = 6.0f * u;
            RectF loop(cx - rx, cy - ry, rx * 2, ry * 2);
            g.DrawArc(&pen, loop, 40.0f, 280.0f);
            PointF arrow[3] = {PointF(cx + 6.6f * u, cy - 2.2f * u), PointF(cx + 1.6f * u, cy - 3.0f * u),
                               PointF(cx + 6.4f * u, cy - 7.2f * u)};
            g.FillPolygon(&brush, arrow, 3);
            if (glyph == Glyph::RepeatOne) {
                // Единица внутри петли: обычная палочка с засечкой.
                g.FillRectangle(&brush, cx - 0.4f * u, cy - 3.2f * u, 1.6f * u, 6.4f * u);
                g.DrawLine(&pen, cx - 2.4f * u, cy - 1.6f * u, cx - 0.2f * u, cy - 3.2f * u);
            }
            break;
        }

        case Glyph::Speaker:
        case Glyph::Mute: {
            PointF body[6] = {PointF(cx - 6 * u, cy - 2.4f * u), PointF(cx - 3.2f * u, cy - 2.4f * u),
                              PointF(cx - 0.4f * u, cy - 5.6f * u), PointF(cx - 0.4f * u, cy + 5.6f * u),
                              PointF(cx - 3.2f * u, cy + 2.4f * u), PointF(cx - 6 * u, cy + 2.4f * u)};
            g.FillPolygon(&brush, body, 6);
            if (glyph == Glyph::Mute) {
                g.DrawLine(&pen, cx + 1.6f * u, cy - 3.2f * u, cx + 6.4f * u, cy + 3.2f * u);
                g.DrawLine(&pen, cx + 6.4f * u, cy - 3.2f * u, cx + 1.6f * u, cy + 3.2f * u);
            } else {
                Pen arc(penColor, ScF(1.4f, dpi) * scale);
                arc.SetStartCap(LineCapRound);
                arc.SetEndCap(LineCapRound);
                g.DrawArc(&arc, cx - 1.0f * u, cy - 3.4f * u, 5.0f * u, 6.8f * u, -60.0f, 120.0f);
                g.DrawArc(&arc, cx - 2.0f * u, cy - 6.0f * u, 9.0f * u, 12.0f * u, -55.0f, 110.0f);
            }
            break;
        }

        case Glyph::Note: {
            const float r = 3.4f * u;
            g.FillEllipse(&brush, cx - 6.5f * u, cy + 1.0f * u, r * 2, r * 2 * 0.85f);
            g.FillEllipse(&brush, cx + 1.5f * u, cy - 0.6f * u, r * 2, r * 2 * 0.85f);
            Pen stem(penColor, ScF(1.8f, dpi) * scale);
            g.DrawLine(&stem, cx - 6.5f * u + r * 2, cy + 1.6f * u, cx - 6.5f * u + r * 2, cy - 6.4f * u);
            g.DrawLine(&stem, cx + 1.5f * u + r * 2, cy, cx + 1.5f * u + r * 2, cy - 8.0f * u);
            g.DrawLine(&stem, cx - 6.5f * u + r * 2, cy - 6.4f * u, cx + 1.5f * u + r * 2, cy - 8.0f * u);
            break;
        }
    }
}

// ---------- Шрифты ----------

struct Fonts {
    int dpi = 0;
    int fontIndex = -1;
    std::unique_ptr<FontFamily> family;
    std::unique_ptr<Font> label, title, artist, note, time, row, rowSmall;

    void Rebuild(int newDpi, int newFontIndex) {
        dpi = newDpi;
        fontIndex = newFontIndex;
        family.reset(MakeUiFontFamily(newFontIndex));
        const float k = UiFontScale(newFontIndex);
        auto make = [&](float size, INT style) {
            return std::unique_ptr<Font>(new Font(family.get(), ScF(size * k, dpi), style, UnitPixel));
        };
        label = make(10.0f, FontStyleBold);
        title = make(15.5f, FontStyleBold);
        artist = make(12.0f, FontStyleRegular);
        note = make(10.5f, FontStyleRegular);
        time = make(10.5f, FontStyleRegular);
        row = make(12.5f, FontStyleRegular);
        rowSmall = make(10.5f, FontStyleRegular);
    }
};

// ---------- Раскладка ----------

enum class Zone {
    None,
    Rail,
    Folder,
    Cover,
    Title,
    Progress,
    Shuffle,
    Prev,
    Play,
    Next,
    Repeat,
    Volume,
    Search,
    FilterClear,
    Row,
    Scrollbar,
    ResizeW,
    ResizeH,
    ResizeWH,
};

struct Hit {
    Zone zone = Zone::None;
    int index = -1;  // номер кнопки панели или индекс трека в библиотеке

    bool operator==(const Hit& o) const { return zone == o.zone && index == o.index; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

struct Layout {
    int dpi = 96;
    int shadow = kShadow;
    int cardW = 0, cardH = 0;
    int winW = 0, winH = 0;
    RECT card{};
    RECT rail{};
    std::vector<RECT> railBtns;
    int labelRight = 0;  // правая граница зоны подписей = левый край панели

    RECT header{};
    RECT folderChip{};
    RECT cover{};
    RECT titleArea{};
    RECT progress{};      // сама полоска
    RECT progressHit{};   // зона попадания — выше полоски, чтобы легко хваталась
    RECT times{};
    RECT btnShuffle{}, btnPrev{}, btnPlay{}, btnNext{}, btnRepeat{};
    RECT volIcon{}, volTrack{}, volHit{};
    RECT listHeader{}, searchBtn{}, filterClear{};
    RECT scrollbar{};
    std::vector<RECT> rowRects;
    std::vector<int> rowIndex;  // индексы в библиотеке
    int visibleRows = 0;
    bool hasList = false;

    RECT resizeRight{}, resizeBottom{}, resizeCorner{};
};

RectF ToRectF(const RECT& r) {
    return RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
                 static_cast<float>(r.bottom - r.top));
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

class Widget : public settings::Host {
public:
    bool Create(HINSTANCE inst);

    // settings::Host
    AppData& Data() override { return data_; }
    void Save() override { SaveData(data_); }
    void ApplyLook() override;
    void ApplyTopmost() override;
    void ApplyHotkeys() override;
    void ApplyVolume() override { audio::SetVolume(data_.volume); }
    void ApplyNameMode() override;
    void ChooseFolder(HWND owner) override;
    void Rescan() override;
    std::wstring StatusLine() override;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK OwnerProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void BuildLayout();
    void Render();
    void DrawNowPlaying(Graphics& g);
    void DrawTransport(Graphics& g);
    void DrawList(Graphics& g);
    void EnsureSurface(int w, int h);
    void ReleaseSurface();
    Hit HitTest(POINT pt) const;

    // ---------- Плеер ----------
    void SyncLibrary(bool force);   // список изменился — пересобрать порядок и вид
    void RebuildView();             // фильтр
    void RebuildOrder(bool keepCurrent);
    void PlayTrack(int libIndex, unsigned long long startMs = 0, bool paused = false);
    void Advance(int delta, bool automatic);
    void OnTrackEnded();
    void TogglePlay();
    void OnOpenFile();
    void OnFind();
    void ScrollToCurrent();
    const library::Track* CurrentTrack() const;
    unsigned long long ShownPositionMs() const;

    // ---------- Режимы ----------
    void ApplyTheme();
    void KeepAttached();
    void Reparent(HWND parent);
    void BeginMove();
    void MoveTo(int screenX, int screenY);
    void MouseTick();
    void EndMouseAction();
    void ApplyResize(POINT pt);
    void ApplyScrub(POINT pt);
    void ApplyVolumeDrag(POINT pt);
    void ApplyScrollDrag(POINT pt);
    void RenderDrag();  // перерисовка во время перетаскивания — с ограничением частоты
    void ClampScroll();
    HWND OwnerWindow();
    bool RunDialog(DialogSpec& spec);
    void ToggleVisible();
    void UpdateTrayTip();
    void PlaceWindow(bool firstTime);
    void SavePositionFromWindow();
    void RememberPlayback(bool toDisk);
    void ShowMenu(POINT screenPt);
    void ShowTrayMenu(POINT screenPt);
    void ShowSettings(POINT screenPt);
    void RunCommand(UINT cmd);
    void OnRailClick(int button, POINT screenPt);
    LRESULT OnHub(WPARAM command);
    void SetTrayShown(bool shown);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HICON icon_ = nullptr;
    AppData data_;
    Layout layout_;
    Fonts fonts_;
    int dpi_ = 96;
    bool visible_ = true;
    bool trayShown_ = false;

    std::vector<library::Track> tracks_;
    int libraryVersion_ = -1;
    std::vector<int> view_;   // индексы треков, прошедших фильтр
    std::vector<int> order_;  // порядок воспроизведения
    int orderPos_ = -1;
    int current_ = -1;  // индекс текущего трека в библиотеке
    std::wstring filter_;
    std::wstring outsideNote_;  // открыт файл вне папки — показываем его подписи
    library::Track outside_;
    bool outsidePlaying_ = false;
    int failures_ = 0;  // подряд не открывшиеся файлы: не гоняем список по кругу
    int scroll_ = 0;
    std::mt19937 rng_{static_cast<unsigned>(GetTickCount64())};

    bool moving_ = false;
    bool resizing_ = false, resizeW_ = false, resizeH_ = false;
    bool scrubbing_ = false;
    bool volumeDrag_ = false;
    bool scrollDrag_ = false;
    int scrollGrab_ = 0;
    unsigned long long scrubMs_ = 0;
    ULONGLONG lastDragDraw_ = 0;
    POINT dragCursor_{};
    POINT dragOrigin_{};

    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int surfaceW_ = 0, surfaceH_ = 0;

    Hit hover_;
    bool mouseTracking_ = false;
    std::wstring lastTip_;
};

Widget* g_widget = nullptr;

// Повторный запуск .exe шлёт это сообщение уже работающему плееру, чтобы тот показался.
UINT ShowMessageId() {
    static const UINT id = RegisterWindowMessageW(L"TuneBox_Show");
    return id;
}

// ---------- Раскладка ----------

void Widget::BuildLayout() {
    Layout& L = layout_;
    L.dpi = dpi_;
    L.shadow = Sc(kShadow, dpi_);
    L.cardW = Sc(Clamp(data_.cardW, kMinCardW, kMaxCardW), dpi_);

    const int pad = Sc(kPad, dpi_);
    const int inner = L.cardW - pad * 2;

    const int trackCount = static_cast<int>(view_.size());
    L.hasList = data_.showList && data_.listRows > 0;
    L.visibleRows = 0;
    if (L.hasList) {
        L.visibleRows = data_.listRows;
        if (trackCount > 0 && trackCount < L.visibleRows) L.visibleRows = trackCount;
        if (L.visibleRows < 1) L.visibleRows = 1;
    }

    // Считаем смещения от верха карточки, потом переводим в координаты окна.
    int y = Sc(16, dpi_);
    const int yHeader = y;
    y += Sc(18, dpi_) + Sc(14, dpi_);

    const int cover = Sc(kCoverSize, dpi_);
    const int yMeta = y;
    const int metaH = data_.showCover ? cover : Sc(56, dpi_);
    y += metaH + Sc(16, dpi_);

    const int yProgress = y;
    y += Sc(6, dpi_) + Sc(7, dpi_);
    const int yTimes = y;
    y += Sc(14, dpi_) + Sc(8, dpi_);

    const int yTransport = y;
    const int transportH = Sc(44, dpi_);
    y += transportH + Sc(12, dpi_);

    const int yVolume = y;
    y += Sc(20, dpi_);

    int yListHeader = 0;
    int yListTop = 0;
    if (L.hasList) {
        y += Sc(12, dpi_);
        yListHeader = y;
        y += Sc(20, dpi_);
        yListTop = y;
        y += L.visibleRows * Sc(kRowH, dpi_);
        y += Sc(2, dpi_);
    }
    y += pad;
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
        L.railBtns.push_back(
            {labelW + (railW - btn) / 2, top, labelW + (railW - btn) / 2 + btn, top + btn});
    }

    const int left = L.card.left + pad;
    const int right = L.card.right - pad;
    const int top = L.card.top;

    L.header = {left, top + yHeader - Sc(4, dpi_), right, top + yHeader + Sc(20, dpi_)};
    // Правую половину заголовка занимает кнопка выбора папки.
    L.folderChip = {left + inner / 2, L.header.top, right, L.header.bottom};

    if (data_.showCover) {
        L.cover = {left, top + yMeta, left + cover, top + yMeta + cover};
        L.titleArea = {left + cover + Sc(14, dpi_), top + yMeta, right, top + yMeta + metaH};
    } else {
        L.cover = {0, 0, 0, 0};
        L.titleArea = {left, top + yMeta, right, top + yMeta + metaH};
    }

    L.progress = {left, top + yProgress, right, top + yProgress + Sc(6, dpi_)};
    L.progressHit = {left - Sc(4, dpi_), top + yProgress - Sc(7, dpi_), right + Sc(4, dpi_),
                     top + yProgress + Sc(13, dpi_)};
    L.times = {left, top + yTimes, right, top + yTimes + Sc(14, dpi_)};

    // Транспорт: перемешать | назад | играть | вперёд | повтор — по центру карточки.
    {
        const int small = Sc(30, dpi_);
        const int mid = Sc(36, dpi_);
        const int big = Sc(44, dpi_);
        const int gap = Sc(10, dpi_);
        const int totalW = small * 2 + mid * 2 + big + gap * 4;
        int x = L.card.left + (L.cardW - totalW) / 2;
        const int cy = top + yTransport + transportH / 2;
        auto place = [&](int size) {
            RECT r = {x, cy - size / 2, x + size, cy + size / 2};
            x += size + gap;
            return r;
        };
        L.btnShuffle = place(small);
        L.btnPrev = place(mid);
        L.btnPlay = place(big);
        L.btnNext = place(mid);
        L.btnRepeat = place(small);
    }

    {
        const int icon = Sc(20, dpi_);
        const int cy = top + yVolume + Sc(9, dpi_);
        L.volIcon = {left, cy - icon / 2, left + icon, cy + icon / 2};
        const int labelW2 = Sc(34, dpi_);
        L.volTrack = {left + icon + Sc(8, dpi_), cy - Sc(3, dpi_), right - labelW2, cy + Sc(3, dpi_)};
        L.volHit = {L.volTrack.left - Sc(6, dpi_), cy - Sc(10, dpi_), L.volTrack.right + Sc(6, dpi_),
                    cy + Sc(10, dpi_)};
    }

    L.rowRects.clear();
    L.rowIndex.clear();
    L.listHeader = {0, 0, 0, 0};
    L.searchBtn = {0, 0, 0, 0};
    L.filterClear = {0, 0, 0, 0};
    L.scrollbar = {0, 0, 0, 0};

    if (L.hasList) {
        L.listHeader = {left, top + yListHeader, right, top + yListHeader + Sc(20, dpi_)};
        const int icon = Sc(20, dpi_);
        L.searchBtn = {right - icon, L.listHeader.top, right, L.listHeader.top + icon};
        if (!filter_.empty()) {
            L.filterClear = {L.searchBtn.left - Sc(24, dpi_), L.listHeader.top,
                             L.searchBtn.left - Sc(4, dpi_), L.listHeader.top + icon};
        }

        ClampScroll();
        const int rowH = Sc(kRowH, dpi_);
        const bool needBar = trackCount > L.visibleRows;
        const int rowRight = needBar ? right - Sc(10, dpi_) : right;
        for (int i = 0; i < L.visibleRows; i++) {
            const int viewIdx = i + scroll_;
            if (viewIdx >= trackCount) break;
            const int rowTop = top + yListTop + i * rowH;
            L.rowRects.push_back({left, rowTop, rowRight, rowTop + rowH});
            L.rowIndex.push_back(view_[static_cast<size_t>(viewIdx)]);
        }
        if (needBar && !L.rowRects.empty()) {
            L.scrollbar = {right - Sc(9, dpi_), L.rowRects.front().top, right, L.rowRects.back().bottom};
        }
    }

    // Ручки изменения размера по правому краю, низу и уголку.
    const int grip = Sc(8, dpi_);
    L.resizeCorner = {L.card.right - grip * 2, L.card.bottom - grip * 2, L.card.right, L.card.bottom};
    L.resizeRight = {L.card.right - grip, L.card.top + Sc(40, dpi_), L.card.right,
                     L.card.bottom - grip * 2};
    L.resizeBottom = {L.card.left + grip, L.card.bottom - grip, L.card.right - grip * 2, L.card.bottom};
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

// ---------- Текущий трек ----------

const library::Track* Widget::CurrentTrack() const {
    if (outsidePlaying_) return &outside_;
    if (current_ < 0 || current_ >= static_cast<int>(tracks_.size())) return nullptr;
    return &tracks_[static_cast<size_t>(current_)];
}

unsigned long long Widget::ShownPositionMs() const {
    return scrubbing_ ? scrubMs_ : audio::PositionMs();
}

// ---------- Отрисовка ----------

void Widget::DrawNowPlaying(Graphics& g) {
    const Layout& L = layout_;
    const library::Track* track = CurrentTrack();
    const float u = ScF(1.0f, dpi_);

    // Обложка.
    if (data_.showCover) {
        const RectF box = ToRectF(L.cover);
        const float radius = ScF(12.0f, dpi_);
        Bitmap* art = track ? cover::Get(track->path, L.cover.right - L.cover.left) : nullptr;

        if (art && art->GetWidth() > 0) {
            GraphicsPath* clip = gfx::RoundRectPath(box, radius);
            g.SetClip(clip);
            // Обложки бывают не квадратные — вписываем по короткой стороне, без искажений.
            const float aw = static_cast<float>(art->GetWidth());
            const float ah = static_cast<float>(art->GetHeight());
            const float scale = max(box.Width / aw, box.Height / ah);
            const float dw = aw * scale;
            const float dh = ah * scale;
            g.DrawImage(art, RectF(box.X + (box.Width - dw) / 2, box.Y + (box.Height - dh) / 2, dw, dh));
            g.ResetClip();
            delete clip;
        } else {
            gfx::FillRoundRect(g, box, radius, Mix(P().groupBg, P().accent, 0.22f));
            DrawGlyph(g, Glyph::Note, box, WithAlpha(P().accent, 0x99), dpi_, 1.9f);
        }
        gfx::StrokeRoundRect(g, box, radius, P().cardBorder, u);

        // Наведение на обложку — она же кнопка «играть/пауза».
        if (hover_.zone == Zone::Cover && track) {
            gfx::FillRoundRect(g, box, radius, 0x66000000);
            DrawGlyph(g, audio::Playing() ? Glyph::Pause : Glyph::Play, box, 0xFFFFFFFF, dpi_, 1.6f);
        }
    }

    // Название, исполнитель, примечание.
    const RectF area = ToRectF(L.titleArea);
    std::wstring title = L"Нечего играть";
    std::wstring artist;
    std::wstring note;

    if (track) {
        title = track->title.empty() ? FileStem(track->path) : track->title;
        artist = track->artist;
        if (!audio::LastError().empty()) {
            note = audio::LastError();
        } else if (outsidePlaying_) {
            note = L"файл вне папки";
        } else if (!track->album.empty()) {
            note = track->album;
        } else if (current_ >= 0) {
            wchar_t buf[64];
            swprintf(buf, 64, L"%d из %d", current_ + 1, static_cast<int>(tracks_.size()));
            note = buf;
        }
    } else if (data_.folder.empty()) {
        title = L"Папка не выбрана";
        note = L"нажмите «Выбрать папку» слева";
    } else if (tracks_.empty()) {
        title = L"В папке нет музыки";
        note = FolderName(data_.folder);
    } else {
        title = L"Выберите трек";
        note = StatusLine();
    }

    // Строк бывает от одной до трёх — блок текста выравниваем по центру обложки,
    // иначе одинокое название «висит» у её верхнего края.
    const bool hasArtist = !artist.empty();
    const float lineTitle = ScF(23.0f, dpi_);
    const float lineArtist = ScF(19.0f, dpi_);
    const float lineNote = ScF(16.0f, dpi_);
    const float blockH = lineTitle + (hasArtist ? lineArtist : 0.0f) + (note.empty() ? 0.0f : lineNote);
    float y = area.Y + (area.Height - blockH) / 2.0f;
    if (y < area.Y) y = area.Y;

    const ARGB titleColor = hover_.zone == Zone::Title ? P().accent : P().textPrimary;
    gfx::DrawText(g, gfx::Ellipsize(g, title, *fonts_.title, area.Width).c_str(), *fonts_.title, titleColor,
                  area.X, y);
    y += lineTitle;
    if (hasArtist) {
        gfx::DrawText(g, gfx::Ellipsize(g, artist, *fonts_.artist, area.Width).c_str(), *fonts_.artist,
                      P().textMuted, area.X, y);
        y += lineArtist;
    }
    if (!note.empty()) {
        const ARGB color = audio::LastError().empty() ? P().textFaint : P().danger;
        gfx::DrawText(g, gfx::Ellipsize(g, note, *fonts_.note, area.Width).c_str(), *fonts_.note, color,
                      area.X, y);
    }
}

void Widget::DrawTransport(Graphics& g) {
    const Layout& L = layout_;
    const float u = ScF(1.0f, dpi_);
    const unsigned long long duration = audio::DurationMs();
    const unsigned long long position = ShownPositionMs();

    // --- Полоса прогресса ---
    {
        const RectF track = ToRectF(L.progress);
        const bool hot = hover_.zone == Zone::Progress || scrubbing_;
        gfx::FillRoundRect(g, track, track.Height / 2.0f, P().track);

        float frac = 0.0f;
        if (duration > 0) frac = static_cast<float>(position) / static_cast<float>(duration);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (frac > 0.0f) {
            RectF fill(track.X, track.Y, track.Width * frac, track.Height);
            if (fill.Width < track.Height) fill.Width = track.Height;
            gfx::FillRoundRect(g, fill, track.Height / 2.0f, P().accent);
        }
        if (hot && duration > 0) {
            const float knob = ScF(13.0f, dpi_);
            const float knobX = track.X + track.Width * frac - knob / 2.0f;
            RectF knobRect(knobX, track.Y + track.Height / 2.0f - knob / 2.0f, knob, knob);
            gfx::FillRoundRect(g, knobRect, knob / 2.0f, P().dark ? 0xFFF2F4F8 : 0xFFFFFFFF);
            gfx::StrokeRoundRect(g, knobRect, knob / 2.0f, P().accent, 2.0f * u);
        }
    }

    // --- Время ---
    {
        const RectF times = ToRectF(L.times);
        gfx::DrawText(g, FormatTime(position).c_str(), *fonts_.time, P().textMuted, times.X, times.Y);
        if (duration > 0) {
            gfx::DrawText(g, FormatTime(duration).c_str(), *fonts_.time, P().textFaint, times.GetRight(),
                          times.Y, gfx::Align::Right);
        }
        // В середине — режимы, которые иначе пришлось бы угадывать по подсветке кнопок.
        std::wstring modes;
        if (data_.shuffle) modes = L"перемешано";
        if (data_.repeat == RepeatMode::One) modes += modes.empty() ? L"по кругу: трек" : L" · по кругу: трек";
        else if (data_.repeat == RepeatMode::All) modes += modes.empty() ? L"по кругу" : L" · по кругу";
        if (!modes.empty()) {
            gfx::DrawText(g, modes.c_str(), *fonts_.time, P().textFaint, times.X + times.Width / 2.0f,
                          times.Y, gfx::Align::Center);
        }
    }

    // --- Кнопки ---
    auto button = [&](const RECT& rect, Glyph glyph, bool active, Zone zone, float scale) {
        const RectF box = ToRectF(rect);
        const bool hot = hover_.zone == zone;
        if (hot) gfx::FillRoundRect(g, box, box.Width / 2.0f, P().buttonBgHover);
        ARGB color = active ? P().accent : P().textMuted;
        if (hot) color = active ? P().accent : P().textPrimary;
        DrawGlyph(g, glyph, box, color, dpi_, scale);
        if (active) {
            // Точка под кнопкой: видно, что режим включён, даже мельком.
            const float dot = ScF(3.0f, dpi_);
            gfx::FillRoundRect(g, RectF(box.X + box.Width / 2.0f - dot / 2, box.GetBottom() - dot, dot, dot),
                               dot / 2.0f, P().accent);
        }
    };

    button(L.btnShuffle, Glyph::Shuffle, data_.shuffle, Zone::Shuffle, 0.95f);
    button(L.btnPrev, Glyph::Prev, false, Zone::Prev, 1.0f);
    button(L.btnNext, Glyph::Next, false, Zone::Next, 1.0f);
    button(L.btnRepeat, data_.repeat == RepeatMode::One ? Glyph::RepeatOne : Glyph::Repeat,
           data_.repeat != RepeatMode::Off, Zone::Repeat, 0.95f);

    // Главная кнопка — залитый акцентом круг.
    {
        const RectF box = ToRectF(L.btnPlay);
        const bool hot = hover_.zone == Zone::Play;
        gfx::FillRoundRect(g, RectF(box.X, box.Y + 2 * u, box.Width, box.Height), box.Width / 2.0f,
                           0x28000000);
        ARGB bg = P().accent;
        if (hot) bg = Mix(bg, P().dark ? 0xFFFFFFFF : 0xFF000000, 0.18f);
        gfx::FillRoundRect(g, box, box.Width / 2.0f, bg);
        DrawGlyph(g, audio::Playing() ? Glyph::Pause : Glyph::Play, box, P().onAccent, dpi_, 1.15f);
    }

    // --- Громкость ---
    {
        const RectF icon = ToRectF(L.volIcon);
        DrawGlyph(g, data_.volume == 0 ? Glyph::Mute : Glyph::Speaker, icon,
                  hover_.zone == Zone::Volume ? P().textPrimary : P().textFaint, dpi_, 0.85f);

        const RectF track = ToRectF(L.volTrack);
        gfx::FillRoundRect(g, track, track.Height / 2.0f, P().track);
        const float frac = static_cast<float>(data_.volume) / 100.0f;
        if (frac > 0.0f) {
            RectF fill(track.X, track.Y, track.Width * frac, track.Height);
            if (fill.Width < track.Height) fill.Width = track.Height;
            gfx::FillRoundRect(g, fill, track.Height / 2.0f, Mix(P().accent, P().textMuted, 0.25f));
        }
        if (hover_.zone == Zone::Volume || volumeDrag_) {
            const float knob = ScF(11.0f, dpi_);
            const float knobX = track.X + track.Width * frac - knob / 2.0f;
            RectF knobRect(knobX, track.Y + track.Height / 2.0f - knob / 2.0f, knob, knob);
            gfx::FillRoundRect(g, knobRect, knob / 2.0f, P().dark ? 0xFFF2F4F8 : 0xFFFFFFFF);
            gfx::StrokeRoundRect(g, knobRect, knob / 2.0f, P().accent, ScF(1.8f, dpi_));
        }
        wchar_t buf[16];
        swprintf(buf, 16, L"%d%%", data_.volume);
        gfx::DrawText(g, buf, *fonts_.rowSmall, P().textFaint, static_cast<float>(layout_.card.right - Sc(kPad, dpi_)),
                      track.Y - ScF(5.0f, dpi_), gfx::Align::Right);
    }
}

void Widget::DrawList(Graphics& g) {
    const Layout& L = layout_;
    if (!L.hasList) return;
    const float u = ScF(1.0f, dpi_);

    // --- Заголовок списка ---
    {
        const RectF header = ToRectF(L.listHeader);
        std::wstring caption = L"СПИСОК";
        if (!filter_.empty()) caption = L"НАЙДЕНО";
        gfx::DrawTracked(g, caption.c_str(), *fonts_.label, P().textFaint, header.X,
                         header.Y + ScF(4.0f, dpi_), ScF(2.0f, dpi_));

        const float capW = gfx::TrackedWidth(g, caption.c_str(), *fonts_.label, ScF(2.0f, dpi_));
        std::wstring count;
        if (!filter_.empty()) {
            wchar_t buf[128];
            swprintf(buf, 128, L"«%ls» — %d", filter_.c_str(), static_cast<int>(view_.size()));
            count = buf;
        } else {
            wchar_t buf[64];
            const int n = static_cast<int>(tracks_.size());
            swprintf(buf, 64, L"%d %ls", n, PluralRu(n, L"трек", L"трека", L"треков"));
            count = buf;
        }
        const float countX = header.X + capW + ScF(10.0f, dpi_);
        const float countMax = static_cast<float>(L.filterClear.right ? L.filterClear.left : L.searchBtn.left) -
                               countX - ScF(8.0f, dpi_);
        gfx::DrawText(g, gfx::Ellipsize(g, count, *fonts_.rowSmall, countMax).c_str(), *fonts_.rowSmall,
                      P().textFaint, countX, header.Y + ScF(3.0f, dpi_));

        DrawGlyph(g, Glyph::Search, ToRectF(L.searchBtn),
                  hover_.zone == Zone::Search ? P().accent : P().textFaint, dpi_, 0.85f);
        if (L.filterClear.right) {
            DrawGlyph(g, Glyph::Close, ToRectF(L.filterClear),
                      hover_.zone == Zone::FilterClear ? P().danger : P().textFaint, dpi_, 0.7f);
        }
    }

    // --- Строки ---
    if (L.rowRects.empty()) {
        const RECT& area = L.listHeader;
        const float y = static_cast<float>(area.bottom) + ScF(10.0f, dpi_);
        const wchar_t* text = filter_.empty() ? L"Треков нет" : L"Ничего не найдено";
        gfx::DrawText(g, text, *fonts_.row, P().textFaint,
                      static_cast<float>(L.card.left + L.cardW / 2), y, gfx::Align::Center);
        return;
    }

    for (size_t i = 0; i < L.rowRects.size(); i++) {
        const RECT& r = L.rowRects[i];
        const int index = L.rowIndex[i];
        if (index < 0 || index >= static_cast<int>(tracks_.size())) continue;
        const library::Track& track = tracks_[static_cast<size_t>(index)];
        const bool isCurrent = (index == current_ && !outsidePlaying_);
        const bool hot = hover_.zone == Zone::Row && hover_.index == index;
        const RectF row = ToRectF(r);

        if (isCurrent) {
            gfx::FillRoundRect(g, RectF(row.X - ScF(6.0f, dpi_), row.Y + u, row.Width + ScF(12.0f, dpi_),
                                        row.Height - 2 * u),
                               ScF(8.0f, dpi_), WithAlpha(P().accent, P().dark ? 0x30 : 0x24));
        } else if (hot) {
            gfx::FillRoundRect(g, RectF(row.X - ScF(6.0f, dpi_), row.Y + u, row.Width + ScF(12.0f, dpi_),
                                        row.Height - 2 * u),
                               ScF(8.0f, dpi_), P().hover);
        }

        // Слева — номер, у текущего трека вместо номера значок воспроизведения.
        const float markW = ScF(22.0f, dpi_);
        const float textY = row.Y + (row.Height - ScF(16.0f, dpi_)) / 2.0f;
        if (isCurrent || hot) {
            RectF mark(row.X, row.Y, markW, row.Height);
            DrawGlyph(g, (isCurrent && audio::Playing()) ? Glyph::Pause : Glyph::Play, mark,
                      isCurrent ? P().accent : P().textMuted, dpi_, 0.55f);
        } else {
            wchar_t buf[16];
            swprintf(buf, 16, L"%d", index + 1);
            gfx::DrawText(g, buf, *fonts_.rowSmall, P().textFaint, row.X + markW - ScF(6.0f, dpi_),
                          textY + ScF(1.0f, dpi_), gfx::Align::Right);
        }

        // Справа — длительность, если её уже прочитали.
        float rightEdge = row.GetRight();
        if (track.durationMs > 0) {
            const std::wstring time = FormatTime(track.durationMs);
            gfx::DrawText(g, time.c_str(), *fonts_.rowSmall, P().textFaint, rightEdge, textY + ScF(1.0f, dpi_),
                          gfx::Align::Right);
            rightEdge -= gfx::TextWidth(g, time.c_str(), *fonts_.rowSmall) + ScF(10.0f, dpi_);
        }

        const float textX = row.X + markW;
        std::wstring line = track.title.empty() ? FileStem(track.path) : track.title;
        const ARGB nameColor = isCurrent ? P().accent : (hot ? P().textPrimary : P().textMuted);

        if (!track.artist.empty()) {
            // Исполнитель приписывается к названию тем же серым — строка не рвётся на две.
            const std::wstring name = gfx::Ellipsize(g, line, *fonts_.row, (rightEdge - textX) * 0.62f);
            gfx::DrawText(g, name.c_str(), *fonts_.row, nameColor, textX, textY);
            const float used = gfx::TextWidth(g, name.c_str(), *fonts_.row);
            const float artistX = textX + used + ScF(8.0f, dpi_);
            if (rightEdge - artistX > ScF(24.0f, dpi_)) {
                gfx::DrawText(g, gfx::Ellipsize(g, track.artist, *fonts_.rowSmall, rightEdge - artistX).c_str(),
                              *fonts_.rowSmall, P().textFaint, artistX, textY + ScF(2.0f, dpi_));
            }
        } else {
            gfx::DrawText(g, gfx::Ellipsize(g, line, *fonts_.row, rightEdge - textX).c_str(), *fonts_.row,
                          nameColor, textX, textY);
        }
    }

    // --- Полоса прокрутки: её можно тащить, без этого по 400 трекам не полазишь ---
    if (L.scrollbar.right) {
        const int total = static_cast<int>(view_.size());
        const RectF bar = ToRectF(L.scrollbar);
        const float trackX = bar.X + bar.Width / 2.0f - ScF(1.5f, dpi_);
        const float thumbH = max(bar.Height * static_cast<float>(L.visibleRows) / static_cast<float>(total),
                                 ScF(24.0f, dpi_));
        const int maxScroll = total - L.visibleRows;
        const float offset = maxScroll > 0 ? (bar.Height - thumbH) * static_cast<float>(scroll_) /
                                                 static_cast<float>(maxScroll)
                                           : 0.0f;
        const bool hot = hover_.zone == Zone::Scrollbar || scrollDrag_;
        gfx::FillRoundRect(g, RectF(trackX, bar.Y, ScF(3.0f, dpi_), bar.Height), ScF(1.5f, dpi_),
                           P().separator);
        gfx::FillRoundRect(g, RectF(trackX - (hot ? ScF(1.0f, dpi_) : 0.0f), bar.Y + offset,
                                    ScF(hot ? 5.0f : 3.0f, dpi_), thumbH),
                           ScF(2.5f, dpi_), hot ? P().accent : P().textFaint);
    }
}

void Widget::Render() {
    if (!hwnd_) return;
    SetTheme(data_.darkTheme, data_.accent, data_.opacity);
    if (fonts_.fontIndex != data_.fontIndex || fonts_.dpi != dpi_) fonts_.Rebuild(dpi_, data_.fontIndex);

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
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.Clear(Color(0x00000000));

    const RectF cardRect = ToRectF(L.card);
    const float radius = ScF(kCardRadius, dpi_);
    const float u = ScF(1.0f, dpi_);

    // У прозрачности есть пол: совсем прозрачная карточка светлой темы показывает обои
    // насквозь, и тёмный текст на тёмных обоях перестаёт читаться.
    const int floorOp = P().dark ? 52 : 70;
    const int op = floorOp + (100 - floorOp) * (Clamp(data_.opacity, 40, 100) - 40) / 60;
    const ARGB bgTop = Fade(Mix(P().cardBgTop, P().accent, 0.10f), op);
    const ARGB bgBottom = Fade(Mix(P().cardBg, P().accent, 0.04f), op);
    const ARGB border = Fade(Mix(P().cardBorder, P().accent, 0.5f), op);

    gfx::DropShadow(g, cardRect, radius, L.shadow, op);
    gfx::FillRoundRectGradient(g, cardRect, radius, bgTop, bgBottom);
    gfx::StrokeRoundRect(g, cardRect, radius, border, u);
    gfx::TopHighlight(g, cardRect, radius, Fade(P().cardGloss, op), u);

    // --- Заголовок карточки ---
    {
        const RectF header = ToRectF(L.header);
        gfx::DrawTracked(g, L"ПЛЕЕР", *fonts_.label, P().textMuted, header.X, header.Y + ScF(5.0f, dpi_),
                         ScF(3.0f, dpi_));

        const bool hot = hover_.zone == Zone::Folder;
        std::wstring folder = data_.folder.empty() ? L"выбрать папку" : FolderName(data_.folder);
        if (library::Busy()) folder += L" …";
        // Слева от текста — значок папки, справа и слева — поля скруглённой плашки.
        const float glyphW = ScF(22.0f, dpi_);
        const float sidePad = ScF(8.0f, dpi_);
        const RectF chip = ToRectF(L.folderChip);
        const float wanted = gfx::TextWidth(g, folder.c_str(), *fonts_.note) + glyphW + sidePad * 2;
        const float textW = min(wanted, chip.Width);
        RectF chipBox(chip.GetRight() - textW, chip.Y + ScF(1.0f, dpi_), textW, chip.Height - ScF(2.0f, dpi_));
        if (hot) gfx::FillRoundRect(g, chipBox, chipBox.Height / 2.0f, P().buttonBg);
        DrawGlyph(g, Glyph::Folder, RectF(chipBox.X + sidePad / 2, chipBox.Y, glyphW, chipBox.Height),
                  hot ? P().accent : P().textFaint, dpi_, 0.7f);
        gfx::DrawText(g,
                      gfx::Ellipsize(g, folder, *fonts_.note, chipBox.Width - glyphW - sidePad * 1.5f).c_str(),
                      *fonts_.note, hot ? P().textPrimary : P().textMuted,
                      chipBox.X + sidePad / 2 + glyphW, chipBox.Y + (chipBox.Height - ScF(14.0f, dpi_)) / 2.0f);

        RectF line(static_cast<float>(L.card.left + Sc(kPad, dpi_)), static_cast<float>(L.header.bottom),
                   static_cast<float>(L.cardW - Sc(kPad, dpi_) * 2), 1.5f * u);
        gfx::FadeLine(g, line, WithAlpha(P().accent, 0xA0));
    }

    DrawNowPlaying(g);
    DrawTransport(g);
    DrawList(g);

    // --- Уголок «потяни меня» ---
    if (!data_.locked) {
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

    // --- Панель кнопок слева ---
    //
    // Панель нарисована всегда, пусть и бледно. Прятать её до наведения нельзя: у слоёного
    // окна попадание мыши определяется по альфе, и сквозь прозрачную панель курсор просто
    // не был бы замечен — навести на неё стало бы невозможно.
    {
        const bool railHot = hover_.zone == Zone::Rail;
        const RectF railRect = ToRectF(L.rail);
        const float railRadius = railRect.Width / 2.0f;
        if (railHot) {
            gfx::DropShadow(g, railRect, railRadius, Sc(10, dpi_), op);
            gfx::FillRoundRectGradient(g, railRect, railRadius, bgTop, bgBottom);
            gfx::StrokeRoundRect(g, railRect, railRadius, border, u);
        } else {
            gfx::FillRoundRect(g, railRect, railRadius, Fade(bgBottom, 46));
            gfx::StrokeRoundRect(g, railRect, railRadius, Fade(border, 55), u);
        }
        for (int i = 0; i < kRailCount; i++) {
            const RectF box = ToRectF(L.railBtns[static_cast<size_t>(i)]);
            const bool hot = railHot && hover_.index == i;
            bool active = false;
            if (i == kRailPin) active = data_.alwaysOnTop;

            if (hot) gfx::FillRoundRect(g, box, box.Width / 2.0f, P().buttonBgHover);

            Glyph glyph = Glyph::Menu;
            switch (i) {
                case kRailMenu: glyph = Glyph::Menu; break;
                case kRailSettings: glyph = Glyph::Gear; break;
                case kRailFolder: glyph = Glyph::Folder; break;
                case kRailFind: glyph = Glyph::Search; break;
                case kRailTheme: glyph = Glyph::Theme; break;
                case kRailPin: glyph = Glyph::Pin; break;
                case kRailHide: glyph = Glyph::Hide; break;
                case kRailClose: glyph = Glyph::Close; break;
                default: break;
            }
            ARGB color = railHot ? P().textMuted : WithAlpha(P().textFaint, 0x9A);
            if (active) color = railHot ? P().accent : WithAlpha(P().accent, 0xB0);
            if (hot) color = (i == kRailClose) ? P().danger : P().textPrimary;
            DrawGlyph(g, glyph, box, color, dpi_);

            if (hot) {
                // Подпись слева от панели, в прозрачной зоне окна.
                const wchar_t* label = RailLabel(i, data_.darkTheme, data_.alwaysOnTop);
                const float textW = gfx::TextWidth(g, label, *fonts_.note);
                const float h = ScF(24.0f, dpi_);
                RectF pill(railRect.X - textW - ScF(22.0f, dpi_), box.Y + (box.Height - h) / 2.0f,
                           textW + ScF(16.0f, dpi_), h);
                gfx::DropShadow(g, pill, h / 2.0f, Sc(8, dpi_), op);
                gfx::FillRoundRect(g, pill, h / 2.0f, Fade(P().cardBgTop, max(op, 88)));
                gfx::StrokeRoundRect(g, pill, h / 2.0f, border, u);
                gfx::DrawText(g, label, *fonts_.note, P().textPrimary, pill.X + ScF(8.0f, dpi_),
                              pill.Y + (h - ScF(14.0f, dpi_)) / 2.0f);
            }
        }
    }

    // --- Перенос на экран ---
    POINT src = {0, 0};
    SIZE size = {L.winW, L.winH};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
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
    // Вся полоса панели считается «наведением», иначе кнопки пропадали бы между собой.
    if (PtInRect(&L.rail, pt)) return {Zone::Rail, -1};
    if (!PtInRect(&L.card, pt)) return {Zone::None, -1};

    if (!data_.locked) {
        if (PtInRect(&L.resizeCorner, pt)) return {Zone::ResizeWH, -1};
        if (PtInRect(&L.resizeRight, pt)) return {Zone::ResizeW, -1};
        if (L.hasList && PtInRect(&L.resizeBottom, pt)) return {Zone::ResizeH, -1};
    }

    if (PtInRect(&L.folderChip, pt)) return {Zone::Folder, -1};
    if (data_.showCover && PtInRect(&L.cover, pt)) return {Zone::Cover, -1};
    if (PtInRect(&L.titleArea, pt)) return {Zone::Title, -1};
    if (PtInRect(&L.progressHit, pt)) return {Zone::Progress, -1};

    if (PtInRect(&L.btnShuffle, pt)) return {Zone::Shuffle, -1};
    if (PtInRect(&L.btnPrev, pt)) return {Zone::Prev, -1};
    if (PtInRect(&L.btnPlay, pt)) return {Zone::Play, -1};
    if (PtInRect(&L.btnNext, pt)) return {Zone::Next, -1};
    if (PtInRect(&L.btnRepeat, pt)) return {Zone::Repeat, -1};
    if (PtInRect(&L.volHit, pt) || PtInRect(&L.volIcon, pt)) return {Zone::Volume, -1};

    if (L.hasList) {
        if (PtInRect(&L.searchBtn, pt)) return {Zone::Search, -1};
        if (L.filterClear.right && PtInRect(&L.filterClear, pt)) return {Zone::FilterClear, -1};
        if (L.scrollbar.right) {
            RECT wide = {L.scrollbar.left - Sc(4, dpi_), L.scrollbar.top, L.scrollbar.right,
                         L.scrollbar.bottom};
            if (PtInRect(&wide, pt)) return {Zone::Scrollbar, -1};
        }
        for (size_t i = 0; i < L.rowRects.size(); i++) {
            if (PtInRect(&L.rowRects[i], pt)) return {Zone::Row, L.rowIndex[i]};
        }
    }
    return {Zone::None, -1};
}

// ---------- Список и порядок ----------

void Widget::RebuildView() {
    view_.clear();
    view_.reserve(tracks_.size());
    for (size_t i = 0; i < tracks_.size(); i++) {
        if (filter_.empty()) {
            view_.push_back(static_cast<int>(i));
            continue;
        }
        const library::Track& t = tracks_[i];
        if (ContainsNoCase(t.title, filter_) || ContainsNoCase(t.artist, filter_) ||
            ContainsNoCase(t.album, filter_) || ContainsNoCase(FileName(t.path), filter_)) {
            view_.push_back(static_cast<int>(i));
        }
    }
    ClampScroll();
}

void Widget::RebuildOrder(bool keepCurrent) {
    order_.clear();
    order_.reserve(tracks_.size());
    for (size_t i = 0; i < tracks_.size(); i++) order_.push_back(static_cast<int>(i));

    if (data_.shuffle && order_.size() > 1) {
        for (size_t i = order_.size() - 1; i > 0; i--) {
            std::uniform_int_distribution<size_t> pick(0, i);
            std::swap(order_[i], order_[pick(rng_)]);
        }
    }

    orderPos_ = -1;
    if (keepCurrent && current_ >= 0) {
        for (size_t i = 0; i < order_.size(); i++) {
            if (order_[i] == current_) {
                orderPos_ = static_cast<int>(i);
                break;
            }
        }
    }
}

void Widget::ClampScroll() {
    const int total = static_cast<int>(view_.size());
    const int rows = layout_.visibleRows > 0 ? layout_.visibleRows : data_.listRows;
    const int maxScroll = total > rows ? total - rows : 0;
    scroll_ = Clamp(scroll_, 0, maxScroll);
}

void Widget::SyncLibrary(bool force) {
    const int version = library::Version();
    if (!force && version == libraryVersion_) return;
    libraryVersion_ = version;

    const std::wstring currentPath = (current_ >= 0 && current_ < static_cast<int>(tracks_.size()))
                                         ? tracks_[static_cast<size_t>(current_)].path
                                         : std::wstring();
    tracks_ = library::Snapshot();

    // Индекс мог сдвинуться: список пересобрали. Держимся за путь, а не за номер.
    current_ = -1;
    if (!currentPath.empty()) {
        for (size_t i = 0; i < tracks_.size(); i++) {
            if (EqualsNoCase(tracks_[i].path, currentPath)) {
                current_ = static_cast<int>(i);
                break;
            }
        }
    }
    if (current_ < 0 && !audio::OpenPath().empty() && !outsidePlaying_) {
        for (size_t i = 0; i < tracks_.size(); i++) {
            if (EqualsNoCase(tracks_[i].path, audio::OpenPath())) {
                current_ = static_cast<int>(i);
                break;
            }
        }
    }

    RebuildView();
    if (order_.size() != tracks_.size()) RebuildOrder(true);
    else if (current_ >= 0) {
        bool found = false;
        for (size_t i = 0; i < order_.size(); i++) {
            if (order_[i] == current_) {
                orderPos_ = static_cast<int>(i);
                found = true;
                break;
            }
        }
        if (!found) RebuildOrder(true);
    }
}

void Widget::ScrollToCurrent() {
    if (current_ < 0 || layout_.visibleRows <= 0) return;
    for (size_t i = 0; i < view_.size(); i++) {
        if (view_[i] != current_) continue;
        const int row = static_cast<int>(i);
        if (row < scroll_ || row >= scroll_ + layout_.visibleRows) {
            scroll_ = row - layout_.visibleRows / 2;
        }
        ClampScroll();
        return;
    }
}

// ---------- Воспроизведение ----------

void Widget::PlayTrack(int libIndex, unsigned long long startMs, bool paused) {
    if (libIndex < 0 || libIndex >= static_cast<int>(tracks_.size())) return;

    outsidePlaying_ = false;
    current_ = libIndex;
    for (size_t i = 0; i < order_.size(); i++) {
        if (order_[i] == libIndex) {
            orderPos_ = static_cast<int>(i);
            break;
        }
    }

    const std::wstring& path = tracks_[static_cast<size_t>(libIndex)].path;
    const bool ok = audio::Open(path, startMs, paused);
    failures_ = ok ? 0 : failures_ + 1;
    data_.lastTrack = path;
    data_.lastPositionMs = startMs;
    SaveData(data_);
    UpdateTrayTip();
    ScrollToCurrent();
    if (settings::IsOpen()) settings::Refresh();

    // Битый файл в середине папки не должен останавливать музыку — идём дальше,
    // но не больше нескольких раз подряд, иначе укатимся по всему списку.
    if (!ok && failures_ < 5) Advance(1, true);
}

void Widget::Advance(int delta, bool automatic) {
    if (tracks_.empty()) return;
    if (order_.size() != tracks_.size()) RebuildOrder(true);
    if (order_.empty()) return;

    if (automatic && data_.repeat == RepeatMode::One && current_ >= 0) {
        PlayTrack(current_, 0, false);
        return;
    }

    int next = orderPos_ + delta;
    if (next >= static_cast<int>(order_.size())) {
        if (automatic && data_.repeat == RepeatMode::Off) {
            audio::Pause();
            audio::Seek(0);
            return;
        }
        next = 0;
    }
    if (next < 0) next = static_cast<int>(order_.size()) - 1;
    PlayTrack(order_[static_cast<size_t>(next)], 0, false);
}

void Widget::OnTrackEnded() {
    if (outsidePlaying_) {
        // Открытый мимо папки файл доиграл — возвращаемся к списку.
        outsidePlaying_ = false;
        if (!tracks_.empty()) Advance(1, true);
        return;
    }
    failures_ = 0;
    Advance(1, true);
}

void Widget::TogglePlay() {
    if (audio::Current() == audio::State::Idle || audio::Current() == audio::State::Error) {
        // Играть ещё нечего: берём первый трек порядка (или найденный фильтром).
        if (!view_.empty()) {
            PlayTrack(view_.front(), 0, false);
        } else if (!order_.empty()) {
            PlayTrack(order_.front(), 0, false);
        } else if (data_.folder.empty()) {
            ChooseFolder(OwnerWindow());
        }
        return;
    }
    audio::TogglePlay();
    UpdateTrayTip();
}

void Widget::OnOpenFile() {
    std::wstring path;
    const std::wstring startAt = data_.folder.empty() ? DefaultMusicFolder() : data_.folder;
    if (!PickFile(OwnerWindow(), library::FileDialogFilter(), startAt, path)) return;

    const int inLibrary = library::IndexOf(path);
    if (inLibrary >= 0) {
        SyncLibrary(true);
        PlayTrack(inLibrary, 0, false);
        Render();
        return;
    }

    // Файл вне папки: играем его, не перетряхивая список.
    outside_ = library::Probe(path, data_.NameModeValue());
    outsidePlaying_ = true;
    current_ = -1;
    audio::Open(path, 0, false);
    data_.lastTrack = path;
    data_.lastPositionMs = 0;
    SaveData(data_);
    UpdateTrayTip();
    Render();
}

void Widget::OnFind() {
    DialogSpec spec;
    spec.title = L"Найти трек";
    spec.okLabel = L"Найти";
    DialogField field;
    field.label = L"Название или исполнитель";
    field.hint = L"Пустая строка — показать все треки";
    field.value = filter_;
    spec.fields.push_back(field);
    if (!RunDialog(spec)) return;

    filter_ = TrimSpaces(spec.fields[0].value);
    scroll_ = 0;
    RebuildView();
    // Нашли ровно один трек — очевидно, его и хотели услышать.
    if (view_.size() == 1) PlayTrack(view_.front(), 0, false);
    Render();
}

// ---------- Настройки и список ----------

void Widget::ApplyLook() {
    ApplyTheme();
    Render();
}

void Widget::ApplyTheme() { SetTheme(data_.darkTheme, data_.accent, data_.opacity); }

void Widget::ApplyNameMode() {
    library::Relabel(data_.NameModeValue());
    libraryVersion_ = -1;
}

void Widget::ChooseFolder(HWND owner) {
    std::wstring folder;
    const std::wstring startAt = data_.folder.empty() ? DefaultMusicFolder() : data_.folder;
    if (!PickFolder(owner ? owner : OwnerWindow(), startAt, folder)) return;
    if (EqualsNoCase(folder, data_.folder)) return;

    data_.folder = folder;
    filter_.clear();
    scroll_ = 0;
    current_ = -1;
    orderPos_ = -1;
    SaveData(data_);
    Rescan();
}

void Widget::Rescan() {
    library::Scan(data_.folder, data_.recursive, data_.NameModeValue());
    libraryVersion_ = -1;
    SyncLibrary(true);
    RebuildOrder(true);
    Render();
    if (settings::IsOpen()) settings::Refresh();
}

std::wstring Widget::StatusLine() {
    if (data_.folder.empty()) return L"папка не выбрана";
    const int n = static_cast<int>(tracks_.size());
    wchar_t buf[160];
    if (n == 0) {
        swprintf(buf, 160, L"в папке «%ls» музыки нет", FolderName(data_.folder).c_str());
    } else if (library::Busy()) {
        swprintf(buf, 160, L"%d %ls · читаю теги…", n, PluralRu(n, L"трек", L"трека", L"треков"));
    } else {
        swprintf(buf, 160, L"%d %ls в «%ls»", n, PluralRu(n, L"трек", L"трека", L"треков"),
                 FolderName(data_.folder).c_str());
    }
    return buf;
}

// ---------- Режимы окна ----------

// Плеер живёт в одном из двух состояний:
//   «на рабочем столе» — дочернее окно хозяина рабочего стола. Тогда он и есть часть стола:
//      никогда не оказывается над чужими окнами, но виден вместе со столом, в том числе по Win+D;
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
// иначе блок выглядит как исчезнувший, хотя окно на месте и «видимо».
void Widget::Reparent(HWND parent) {
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
    if (visible_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

// Проводник мог перезапуститься: старый рабочий стол уничтожен, и блок нужно вернуть новому.
void Widget::KeepAttached() {
    if (!hwnd_ || moving_ || resizing_) return;
    if (data_.alwaysOnTop) return;
    HWND desktop = DesktopHostWindow();
    if (desktop && GetParent(hwnd_) != desktop) ApplyTopmost();
}

void Widget::BeginMove() {
    if (data_.locked) return;
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

void Widget::ApplyResize(POINT pt) {
    bool changed = false;
    if (resizeW_) {
        const int px = pt.x - layout_.card.left;
        const int wanted = Clamp(MulDiv(px, 96, dpi_), kMinCardW, kMaxCardW);
        if (wanted != data_.cardW) {
            data_.cardW = wanted;
            changed = true;
        }
    }
    if (resizeH_ && layout_.hasList && !layout_.rowRects.empty()) {
        const int rowH = Sc(kRowH, dpi_);
        const int top = layout_.rowRects.front().top;
        int rows = (pt.y - top + rowH / 2) / rowH;
        rows = Clamp(rows, 1, kMaxListRows);
        if (rows != data_.listRows) {
            data_.listRows = rows;
            ClampScroll();
            changed = true;
        }
    }
    if (changed) RenderDrag();
}

// Мышь опрашивается каждые 15 мс, а полный кадр с тенью, обложкой и списком столько раз
// в секунду не нарисовать: ползунок начинал мерцать и «не догонять» курсор. Поэтому во
// время перетаскивания кадры ограничены 25 в секунду, а последний рисуется при отпускании.
void Widget::RenderDrag() {
    const ULONGLONG now = GetTickCount64();
    if (now - lastDragDraw_ < 40) return;
    lastDragDraw_ = now;
    Render();
}

void Widget::ApplyScrub(POINT pt) {
    const unsigned long long duration = audio::DurationMs();
    if (duration == 0) return;
    const RECT& track = layout_.progress;
    const int width = track.right - track.left;
    if (width <= 0) return;
    float t = static_cast<float>(pt.x - track.left) / static_cast<float>(width);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const unsigned long long ms = static_cast<unsigned long long>(t * static_cast<float>(duration));
    if (ms == scrubMs_) return;
    scrubMs_ = ms;
    RenderDrag();
}

void Widget::ApplyVolumeDrag(POINT pt) {
    const RECT& track = layout_.volTrack;
    const int width = track.right - track.left;
    if (width <= 0) return;
    float t = static_cast<float>(pt.x - track.left) / static_cast<float>(width);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const int value = static_cast<int>(t * 100.0f + 0.5f);
    if (value == data_.volume) return;
    data_.volume = value;
    audio::SetVolume(value);
    RenderDrag();
}

void Widget::ApplyScrollDrag(POINT pt) {
    const int total = static_cast<int>(view_.size());
    const int rows = layout_.visibleRows;
    if (total <= rows || layout_.scrollbar.right == 0) return;

    const RECT& bar = layout_.scrollbar;
    const int barH = bar.bottom - bar.top;
    const float thumbH = max(static_cast<float>(barH) * rows / static_cast<float>(total),
                             ScF(24.0f, dpi_));
    const float span = static_cast<float>(barH) - thumbH;
    if (span <= 0.0f) return;

    const float y = static_cast<float>(pt.y - bar.top - scrollGrab_);
    float t = y / span;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const int next = static_cast<int>(t * static_cast<float>(total - rows) + 0.5f);
    if (next == scroll_) return;
    scroll_ = next;
    RenderDrag();
}

// Курсор и кнопку читаем напрямую: сообщения о движении приходят только пока курсор над
// блоком, а при переносе он легко оказывается за его краем.
void Widget::MouseTick() {
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        EndMouseAction();
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
    if (resizing_) ApplyResize(local);
    else if (scrubbing_) ApplyScrub(local);
    else if (volumeDrag_) ApplyVolumeDrag(local);
    else if (scrollDrag_) ApplyScrollDrag(local);
    else EndMouseAction();
}

void Widget::EndMouseAction() {
    KillTimer(hwnd_, kMouseTimerId);
    const bool wasMoving = moving_;
    const bool wasResizing = resizing_;
    const bool wasScrubbing = scrubbing_;
    const bool wasVolume = volumeDrag_;
    const bool wasScroll = scrollDrag_;
    moving_ = resizing_ = scrubbing_ = volumeDrag_ = scrollDrag_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();

    if (wasScrubbing) audio::Seek(scrubMs_);
    if (wasMoving) SavePositionFromWindow();
    if (wasResizing || wasVolume) SaveData(data_);
    if (!wasMoving && !wasResizing && !wasScrubbing && !wasVolume && !wasScroll) return;

    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    BuildLayout();
    hover_ = HitTest(pt);
    Render();
}

// Владелец меню и диалогов: само окно блока им быть не может — оно дочернее окно
// рабочего стола, и владельцем стал бы сам стол. Скрытое окно подгоняется под блок,
// чтобы системные окна вставали по центру именно его.
HWND Widget::OwnerWindow() {
    if (owner_ && hwnd_) {
        RECT r{};
        GetWindowRect(hwnd_, &r);
        SetWindowPos(owner_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return owner_ ? owner_ : hwnd_;
}

// Пока идёт ввод, блок не принимает мышь: владелец диалога — скрытое окно, и обычной
// модальности через EnableWindow(owner) тут не хватает.
bool Widget::RunDialog(DialogSpec& spec) {
    EnableWindow(hwnd_, FALSE);
    const bool ok = ShowDialog(OwnerWindow(), hwnd_, spec);
    EnableWindow(hwnd_, TRUE);
    return ok;
}

void Widget::ApplyHotkeys() {
    for (int id = kHotPlay; id <= kHotMediaPrev; id++) UnregisterHotKey(owner_, id);
    // Комбинацию мог занять кто-то другой — тогда просто живём без неё.
    if (data_.hotkeysEnabled) {
        RegisterHotKey(owner_, kHotPlay, MOD_CONTROL | MOD_ALT, 'P');
        RegisterHotKey(owner_, kHotNext, MOD_CONTROL | MOD_ALT, VK_RIGHT);
        RegisterHotKey(owner_, kHotPrev, MOD_CONTROL | MOD_ALT, VK_LEFT);
        RegisterHotKey(owner_, kHotVolUp, MOD_CONTROL | MOD_ALT, VK_UP);
        RegisterHotKey(owner_, kHotVolDown, MOD_CONTROL | MOD_ALT, VK_DOWN);
    }
    if (data_.mediaKeys) {
        RegisterHotKey(owner_, kHotMediaPlay, 0, VK_MEDIA_PLAY_PAUSE);
        RegisterHotKey(owner_, kHotMediaNext, 0, VK_MEDIA_NEXT_TRACK);
        RegisterHotKey(owner_, kHotMediaPrev, 0, VK_MEDIA_PREV_TRACK);
    }
}

void Widget::ToggleVisible() {
    visible_ = !visible_;
    ShowWindow(hwnd_, visible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible_) {
        ApplyTopmost();
        Render();
    }
}

void Widget::UpdateTrayTip() {
    std::wstring tip = L"Плеер";
    if (const library::Track* track = CurrentTrack()) {
        tip = audio::Playing() ? L"▶ " : L"❚❚ ";
        tip += track->title.empty() ? FileStem(track->path) : track->title;
        if (!track->artist.empty()) tip += L" — " + track->artist;
    } else {
        tip += L": " + StatusLine();
    }
    // Подсказка трея ограничена 128 символами, длинное название придётся обрезать.
    if (tip.size() > 120) tip = tip.substr(0, 119) + L"…";
    if (tip == lastTip_) return;
    lastTip_ = tip;
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
            y = mi.rcWork.bottom - layout_.winH - Sc(24, dpi_);
        } else {
            x = y = 100;
        }
        data_.winX = x;
        data_.winY = y;
    }

    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon =
        MonitorFromPoint(POINT{x + layout_.winW / 2, y + layout_.winH / 2}, MONITOR_DEFAULTTONEAREST);
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

void Widget::RememberPlayback(bool toDisk) {
    if (const library::Track* track = CurrentTrack()) {
        data_.lastTrack = track->path;
        data_.lastPositionMs = audio::PositionMs();
    }
    if (toDisk) SaveData(data_);
}

// ---------- Меню ----------

void Widget::ShowMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const bool havePlayback = audio::Current() != audio::State::Idle;
    AppendMenuW(menu, MF_STRING, kCmdPlayPause,
                audio::Playing() ? L"Пауза\tCtrl+Alt+P" : L"Играть\tCtrl+Alt+P");
    AppendMenuW(menu, MF_STRING | (tracks_.empty() ? MF_GRAYED : 0), kCmdNext, L"Следующий\tCtrl+Alt+→");
    AppendMenuW(menu, MF_STRING | (tracks_.empty() ? MF_GRAYED : 0), kCmdPrev, L"Предыдущий\tCtrl+Alt+←");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (data_.shuffle ? MF_CHECKED : 0), kCmdShuffle, L"Перемешать");
    {
        const wchar_t* label = L"По кругу: выключено";
        if (data_.repeat == RepeatMode::All) label = L"По кругу: весь список";
        if (data_.repeat == RepeatMode::One) label = L"По кругу: один трек";
        AppendMenuW(menu, MF_STRING, kCmdRepeat, label);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, kCmdPickFolder, L"Выбрать папку…");
    AppendMenuW(menu, MF_STRING, kCmdOpenFile, L"Открыть трек…");
    AppendMenuW(menu, MF_STRING, kCmdFind, L"Найти трек…");
    if (!filter_.empty()) AppendMenuW(menu, MF_STRING, kCmdClearFilter, L"Снять фильтр");
    AppendMenuW(menu, MF_STRING, kCmdRescan, L"Обновить список");
    AppendMenuW(menu, MF_STRING | (havePlayback ? 0 : MF_GRAYED), kCmdRevealTrack,
                L"Показать трек в проводнике");
    AppendMenuW(menu, MF_STRING | (data_.folder.empty() ? MF_GRAYED : 0), kCmdOpenMusicFolder,
                L"Открыть папку с музыкой");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU look = CreatePopupMenu();
    AppendMenuW(look, MF_STRING | (data_.darkTheme ? MF_CHECKED : 0), kCmdThemeDark, L"Тёмная тема");
    AppendMenuW(look, MF_STRING | (!data_.darkTheme ? MF_CHECKED : 0), kCmdThemeLight, L"Светлая тема");
    AppendMenuW(look, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(look, MF_STRING | (data_.showCover ? MF_CHECKED : 0), kCmdToggleCover, L"Обложка");
    AppendMenuW(look, MF_STRING | (data_.showList ? MF_CHECKED : 0), kCmdToggleList, L"Список треков");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(look), L"Оформление");

    AppendMenuW(menu, MF_STRING, kCmdSettings, L"Все настройки…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (data_.alwaysOnTop ? MF_CHECKED : 0), kCmdToggleTop, L"Поверх всех окон");
    AppendMenuW(menu, MF_STRING | (data_.locked ? MF_CHECKED : 0), kCmdToggleLock, L"Закрепить блок");
    AppendMenuW(menu, MF_STRING, kCmdOpenData, L"Папка с настройками");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdToggleVisible, L"Свернуть в трей");
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Закрыть плеер");

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
    AppendMenuW(menu, MF_STRING, kCmdPlayPause, audio::Playing() ? L"Пауза" : L"Играть");
    AppendMenuW(menu, MF_STRING, kCmdNext, L"Следующий");
    AppendMenuW(menu, MF_STRING, kCmdPrev, L"Предыдущий");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdToggleVisible, visible_ ? L"Скрыть блок" : L"Показать блок");
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"Настройки…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Закрыть плеер");

    SetForegroundWindow(OwnerWindow());
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                      screenPt.x, screenPt.y, 0, OwnerWindow(), nullptr));
    PostMessageW(OwnerWindow(), WM_NULL, 0, 0);
    DestroyMenu(menu);
    RunCommand(cmd);
}

void Widget::ShowSettings(POINT screenPt) {
    settings::Show(inst_, this, screenPt);
}

void Widget::OnRailClick(int button, POINT screenPt) {
    switch (button) {
        case kRailMenu: ShowMenu(screenPt); break;
        case kRailSettings: ShowSettings(screenPt); break;
        case kRailFolder: ChooseFolder(OwnerWindow()); break;
        case kRailFind: OnFind(); break;
        case kRailTheme: RunCommand(data_.darkTheme ? kCmdThemeLight : kCmdThemeDark); break;
        case kRailPin: RunCommand(kCmdToggleTop); break;
        case kRailHide: ToggleVisible(); break;
        case kRailClose: DestroyWindow(hwnd_); break;
        default: break;
    }
}

void Widget::RunCommand(UINT cmd) {
    if (cmd == 0) return;
    switch (cmd) {
        case kCmdPlayPause:
            TogglePlay();
            Render();
            break;
        case kCmdNext:
            Advance(1, false);
            Render();
            break;
        case kCmdPrev:
            Advance(-1, false);
            Render();
            break;
        case kCmdShuffle:
            data_.shuffle = !data_.shuffle;
            RebuildOrder(true);
            SaveData(data_);
            Render();
            break;
        case kCmdRepeat: {
            const int next = (static_cast<int>(data_.repeat) + 1) % 3;
            data_.repeat = static_cast<RepeatMode>(next);
            SaveData(data_);
            Render();
            break;
        }
        case kCmdPickFolder: ChooseFolder(OwnerWindow()); break;
        case kCmdOpenFile: OnOpenFile(); break;
        case kCmdFind: OnFind(); break;
        case kCmdClearFilter:
            filter_.clear();
            scroll_ = 0;
            RebuildView();
            ScrollToCurrent();
            Render();
            break;
        case kCmdRescan: Rescan(); break;
        case kCmdRevealTrack:
            if (const library::Track* track = CurrentTrack()) RevealInExplorer(track->path);
            break;
        case kCmdOpenMusicFolder: OpenFolder(data_.folder); break;
        case kCmdSettings: {
            RECT r{};
            GetWindowRect(hwnd_, &r);
            POINT pt = {r.left + Sc(40, dpi_), r.top + Sc(40, dpi_)};
            ShowSettings(pt);
            break;
        }
        case kCmdToggleTop:
            data_.alwaysOnTop = !data_.alwaysOnTop;
            ApplyTopmost();
            SaveData(data_);
            Render();
            break;
        case kCmdToggleLock:
            data_.locked = !data_.locked;
            SaveData(data_);
            Render();
            break;
        case kCmdThemeDark:
        case kCmdThemeLight:
            data_.darkTheme = (cmd == kCmdThemeDark);
            ApplyTheme();
            SaveData(data_);
            Render();
            if (settings::IsOpen()) settings::Refresh();
            break;
        case kCmdToggleCover:
            data_.showCover = !data_.showCover;
            SaveData(data_);
            Render();
            break;
        case kCmdToggleList:
            data_.showList = !data_.showList;
            SaveData(data_);
            Render();
            break;
        case kCmdOpenData: OpenFolder(DataDir()); break;
        case kCmdToggleVisible: ToggleVisible(); break;
        case kCmdExit: DestroyWindow(hwnd_); break;
        default: break;
    }
}

// ---------- Центр виджетов ----------

void Widget::SetTrayShown(bool shown) {
    if (shown == trayShown_) return;
    if (shown) {
        trayShown_ = TrayAdd(owner_, kTrayMessage, icon_, L"Плеер");
        lastTip_.clear();
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
            if (!visible_) ToggleVisible();
            POINT pt;
            GetCursorPos(&pt);
            ShowSettings(pt);
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
            KeepAttached();

            bool dirty = audio::Pump();
            if (audio::TakeEnded()) {
                OnTrackEnded();
                dirty = true;
            }
            // Файлы в папке поменяли в проводнике. Играющий трек держится за путь, а
            // прочитанные теги переносятся, так что пересборка списка музыку не прерывает.
            if (library::FolderChanged()) library::Scan(data_.folder, data_.recursive, data_.NameModeValue());
            if (library::Version() != libraryVersion_) {
                SyncLibrary(false);
                dirty = true;
                if (settings::IsOpen()) settings::Refresh();
            }
            if (audio::Playing()) {
                dirty = true;  // полоса прогресса и время идут вперёд
                RememberPlayback(false);
            }
            if (dirty) UpdateTrayTip();
            // Пока мышь что-то тащит, кадры рисует её таймер. Иначе два таймера рисовали
            // бы наперегонки разные положения ползунка — и он заметно мерцал.
            const bool dragging = moving_ || resizing_ || scrubbing_ || volumeDrag_ || scrollDrag_;
            if (dirty && visible_ && !dragging) Render();
            return 0;
        }

        case kTrayMessage: {
            const UINT event = LOWORD(lp);
            if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                ShowTrayMenu(pt);
            } else if (event == WM_LBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                ShowSettings(pt);
            } else if (event == WM_LBUTTONDBLCLK) {
                ToggleVisible();
            }
            return 0;
        }

        case WM_HOTKEY:
            switch (static_cast<int>(wp)) {
                case kHotPlay:
                case kHotMediaPlay:
                    TogglePlay();
                    Render();
                    break;
                case kHotNext:
                case kHotMediaNext:
                    Advance(1, false);
                    Render();
                    break;
                case kHotPrev:
                case kHotMediaPrev:
                    Advance(-1, false);
                    Render();
                    break;
                case kHotVolUp:
                case kHotVolDown: {
                    const int delta = (static_cast<int>(wp) == kHotVolUp) ? 5 : -5;
                    data_.volume = Clamp(data_.volume + delta, 0, 100);
                    audio::SetVolume(data_.volume);
                    SaveData(data_);
                    Render();
                    break;
                }
                default: break;
            }
            return 0;

        // Дочерних контролов у окна нет, так что WM_COMMAND — это всегда команда меню.
        case WM_COMMAND:
            RunCommand(LOWORD(wp));
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                const wchar_t* cursor = IDC_ARROW;
                switch (hover_.zone) {
                    case Zone::ResizeW: cursor = IDC_SIZEWE; break;
                    case Zone::ResizeH: cursor = IDC_SIZENS; break;
                    case Zone::ResizeWH: cursor = IDC_SIZENWSE; break;
                    case Zone::None: break;
                    case Zone::Title: break;
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
            // Перенос, растягивание и ползунки ведёт таймер — здесь только подсветка.
            if (moving_ || resizing_ || scrubbing_ || volumeDrag_ || scrollDrag_) return 0;

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
            if (hover_.zone != Zone::None) {
                hover_ = {};
                Render();
            }
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Hit hit = HitTest(pt);
            hover_ = hit;

            switch (hit.zone) {
                case Zone::Rail:
                    if (hit.index >= 0) {
                        POINT screen = pt;
                        ClientToScreen(hwnd, &screen);
                        OnRailClick(hit.index, screen);
                    }
                    return 0;

                case Zone::ResizeW:
                case Zone::ResizeH:
                case Zone::ResizeWH:
                    resizing_ = true;
                    resizeW_ = (hit.zone != Zone::ResizeH);
                    resizeH_ = (hit.zone != Zone::ResizeW);
                    SetCapture(hwnd);
                    SetTimer(hwnd, kMouseTimerId, kMouseTimerMs, nullptr);
                    return 0;

                case Zone::Progress:
                    if (audio::DurationMs() > 0) {
                        scrubbing_ = true;
                        scrubMs_ = audio::PositionMs();
                        SetCapture(hwnd);
                        SetTimer(hwnd, kMouseTimerId, kMouseTimerMs, nullptr);
                        ApplyScrub(pt);
                    }
                    return 0;

                case Zone::Volume:
                    volumeDrag_ = true;
                    SetCapture(hwnd);
                    SetTimer(hwnd, kMouseTimerId, kMouseTimerMs, nullptr);
                    ApplyVolumeDrag(pt);
                    return 0;

                case Zone::Scrollbar: {
                    // Запоминаем, за какое место ползунка взялись: иначе он прыгает под курсор.
                    const int total = static_cast<int>(view_.size());
                    const RECT& bar = layout_.scrollbar;
                    const int barH = bar.bottom - bar.top;
                    const int rows = layout_.visibleRows;
                    const float thumbH = max(static_cast<float>(barH) * rows / static_cast<float>(total),
                                             ScF(24.0f, dpi_));
                    const int maxScroll = total > rows ? total - rows : 0;
                    const float offset = maxScroll > 0 ? (barH - thumbH) * static_cast<float>(scroll_) /
                                                             static_cast<float>(maxScroll)
                                                       : 0.0f;
                    const int local = pt.y - bar.top;
                    scrollGrab_ = (local >= offset && local <= offset + thumbH)
                                      ? local - static_cast<int>(offset)
                                      : static_cast<int>(thumbH / 2);
                    scrollDrag_ = true;
                    SetCapture(hwnd);
                    SetTimer(hwnd, kMouseTimerId, kMouseTimerMs, nullptr);
                    ApplyScrollDrag(pt);
                    return 0;
                }

                case Zone::Folder: ChooseFolder(OwnerWindow()); return 0;
                case Zone::Cover:
                case Zone::Play:
                    TogglePlay();
                    Render();
                    return 0;
                case Zone::Prev: RunCommand(kCmdPrev); return 0;
                case Zone::Next: RunCommand(kCmdNext); return 0;
                case Zone::Shuffle: RunCommand(kCmdShuffle); return 0;
                case Zone::Repeat: RunCommand(kCmdRepeat); return 0;
                case Zone::Search: OnFind(); return 0;
                case Zone::FilterClear: RunCommand(kCmdClearFilter); return 0;
                case Zone::Title:
                    ScrollToCurrent();
                    Render();
                    return 0;
                case Zone::Row:
                    if (hit.index == current_ && !outsidePlaying_) TogglePlay();
                    else PlayTrack(hit.index, 0, false);
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
            if (moving_ || resizing_ || scrubbing_ || volumeDrag_ || scrollDrag_) EndMouseAction();
            return 0;

        // Захват мыши у неактивного окна система забирает сама, и перенос от этого
        // прерываться не должен — его ведёт таймер, пока нажата кнопка.
        case WM_CAPTURECHANGED:
            return 0;

        case WM_LBUTTONDBLCLK: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Hit hit = HitTest(pt);
            // Двойной клик по строке — показать файл в проводнике: одиночный уже играет.
            if (hit.zone == Zone::Row && hit.index >= 0 &&
                hit.index < static_cast<int>(tracks_.size())) {
                RevealInExplorer(tracks_[static_cast<size_t>(hit.index)].path);
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            ShowMenu(pt);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            const int steps = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
            const bool overList = layout_.hasList && !layout_.rowRects.empty() &&
                                  pt.y >= layout_.rowRects.front().top &&
                                  pt.y <= layout_.rowRects.back().bottom;
            if (overList) {
                scroll_ += steps * 2;
                ClampScroll();
            } else {
                data_.volume = Clamp(data_.volume - steps * 4, 0, 100);
                audio::SetVolume(data_.volume);
                SaveData(data_);
            }
            Render();
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            fonts_.Rebuild(dpi_, data_.fontIndex);
            cover::Clear();
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
            RememberPlayback(false);
            SavePositionFromWindow();
            SaveData(data_);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            KillTimer(hwnd, kMouseTimerId);
            for (int id = kHotPlay; id <= kHotMediaPrev; id++) UnregisterHotKey(owner_, id);
            settings::Close();
            RememberPlayback(false);
            SavePositionFromWindow();
            SaveData(data_);
            library::Shutdown();
            audio::Shutdown();
            TrayRemove(owner_);
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

// Скрытое окно верхнего уровня. На нём висят значок в трее, горячие клавиши и меню:
// само окно блока для этого не годится — оно дочернее окно рабочего стола.
LRESULT CALLBACK Widget::OwnerProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_widget || g_widget->owner_ != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == kTrayMessage || msg == WM_HOTKEY || msg == WM_COMMAND || msg == ShowMessageId() ||
        msg == hub::MessageId() || msg == WM_ENDSESSION) {
        return g_widget->WndProc(g_widget->hwnd_, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Widget::Create(HINSTANCE inst) {
    inst_ = inst;
    g_widget = this;

    LoadData(data_);  // файла нет — остаются значения по умолчанию
    if (data_.folder.empty()) data_.folder = DefaultMusicFolder();
    data_.ClampAll();
    ApplyTheme();

    icon_ = static_cast<HICON>(
        LoadImageW(inst, MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));

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

    owner_ = CreateWindowExW(WS_EX_TOOLWINDOW, kOwnerClass, L"Плеер", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                             inst, nullptr);
    if (!owner_) return false;

    // WS_EX_NOACTIVATE: блок принимает клики, но не забирает фокус — рабочий стол и чужие
    // окна не «мигают» активацией, когда по нему просто ткнули мышью.
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"Плеер",
                            WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    dpi_ = WindowDpi(hwnd_);
    fonts_.Rebuild(dpi_, data_.fontIndex);

    audio::Startup();
    audio::SetVolume(data_.volume);

    library::Scan(data_.folder, data_.recursive, data_.NameModeValue());
    SyncLibrary(true);
    RebuildOrder(false);

    PlaceWindow(true);
    ApplyTopmost();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Render();

    // При запущенном центре значок плеера живёт в его меню, а не отдельно в трее.
    SetTrayShown(!hub::HubRunning());
    ApplyHotkeys();
    SetTimer(hwnd_, kTimerId, kTimerMs, nullptr);

    // Продолжаем с того места, где закончили: трек открывается на паузе.
    if (data_.resumeOnStart && !data_.lastTrack.empty() && PathExists(data_.lastTrack)) {
        const int index = library::IndexOf(data_.lastTrack);
        if (index >= 0) {
            PlayTrack(index, data_.lastPositionMs, !data_.playOnStart);
        } else {
            outside_ = library::Probe(data_.lastTrack, data_.NameModeValue());
            outsidePlaying_ = true;
            audio::Open(data_.lastTrack, data_.lastPositionMs, !data_.playOnStart);
        }
        UpdateTrayTip();
        Render();
    }
    return true;
}

Widget g_instance;

}  // namespace

bool CreateWidgetWindow(HINSTANCE inst) { return g_instance.Create(inst); }
