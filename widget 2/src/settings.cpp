#include "settings.h"

#include <shlobj.h>
#include <windowsx.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "icons.h"
#include "sys.h"
#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace settings {

namespace {

constexpr wchar_t kClassName[] = L"ShortcutDockSettings";
constexpr int kShadow = 18;
constexpr int kPad = 18;
constexpr int kCardW = 344;
constexpr int kRadius = 16;
constexpr int kHeaderH = 46;

constexpr int kOpacityMin = 40, kOpacityMax = 100;
constexpr int kIconMin = 24, kIconMax = 96, kIconStep = 8;
constexpr int kColumnsMin = 1, kColumnsMax = 12;

enum class Id {
    None,
    Close,
    Theme,
    Accent,
    Opacity,
    IconSize,
    Columns,
    Labels,
    Locked,
    StyleToAll,
    GoGlobal,  // из настроек блока — к общим
    Snap,
    OnTop,
    Autostart,
    Hotkey,
    Storage,
    OpenData,
    OpenBackups,
    OpenStorage,
    RefreshIcons,
    AddGroup,
};

enum class Kind { Section, Theme, Accents, Slider, Toggle, Link, Primary };

struct Item {
    Kind kind = Kind::Section;
    Id id = Id::None;
    std::wstring label;
    std::wstring value;   // подпись значения справа (для ползунка)
    std::wstring hint;    // мелкая строка под подписью (для тумблера)
    bool on = false;      // состояние тумблера
    float fraction = 0.f; // положение ползунка, 0..1
    RECT rect{};          // строка целиком
};

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

class Window {
public:
    bool Open(HINSTANCE inst, Host* host, int group, POINT nearScreenPt, Mode mode);
    void Destroy();
    bool IsOpen() const { return hwnd_ != nullptr; }
    HWND Hwnd() const { return hwnd_; }
    void SetGroup(int group) { groupIndex_ = group; }
    void SetMode(Mode mode) {
        mode_ = mode;
        scroll_ = 0;
    }
    void Refresh() {
        if (hwnd_) Render();
    }

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void BuildItems();
    void Layout();
    void Render();
    void EnsureSurface(int w, int h);
    void ReleaseSurface();
    int HitTest(POINT pt) const;  // индекс в items_, -1 — мимо
    RECT ToggleRect(const Item& item) const;
    RECT TrackRect(const Item& item) const;
    RECT AccentDot(const Item& item, int index) const;
    void ApplySlider(const Item& item, POINT pt);
    void Activate(int index, POINT pt);
    void ClampScroll();

    HWND hwnd_ = nullptr;
    Host* host_ = nullptr;
    int groupIndex_ = 0;
    Mode mode_ = Mode::Panel;
    int dpi_ = 96;

    std::vector<Item> items_;
    int contentH_ = 0;
    int scroll_ = 0;
    int cardW_ = 0, cardH_ = 0, winW_ = 0, winH_ = 0;
    RECT card_{}, header_{}, closeBtn_{}, viewport_{};

    int hot_ = -1;
    int dragItem_ = -1;
    bool mouseTracking_ = false;

    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int surfaceW_ = 0, surfaceH_ = 0;

    std::unique_ptr<FontFamily> family_;
    std::unique_ptr<Font> fontTitle_, fontSection_, fontRow_, fontValue_, fontHint_;
};

Window g_window;

// ---------- Содержимое ----------

void Window::BuildItems() {
    const AppData& data = host_->Data();
    // Блок могли удалить, пока окно открыто: общие настройки от этого не зависят.
    const size_t index = static_cast<size_t>(groupIndex_ < static_cast<int>(data.groups.size()) &&
                                                     groupIndex_ >= 0
                                                 ? groupIndex_
                                                 : 0);
    const Group& block = data.groups[index];
    items_.clear();

    auto section = [&](const wchar_t* text) {
        Item item;
        item.kind = Kind::Section;
        item.label = text;
        items_.push_back(item);
    };
    auto toggle = [&](Id id, const wchar_t* label, const wchar_t* hint, bool on) {
        Item item;
        item.kind = Kind::Toggle;
        item.id = id;
        item.label = label;
        item.hint = hint;
        item.on = on;
        items_.push_back(item);
    };
    auto link = [&](Id id, const wchar_t* label) {
        Item item;
        item.kind = Kind::Link;
        item.id = id;
        item.label = label;
        items_.push_back(item);
    };

    // Настройки блока и настройки всего виджета — два разных окна: в первом только то,
    // что относится к этой полке, во втором — общее поведение и данные.
    if (mode_ == Mode::Panel) {
        section(L"ОФОРМЛЕНИЕ БЛОКА");
        {
            Item theme;
            theme.kind = Kind::Theme;
            theme.id = Id::Theme;
            theme.on = block.darkTheme;
            items_.push_back(theme);

            Item accents;
            accents.kind = Kind::Accents;
            accents.id = Id::Accent;
            accents.label = L"Акцент";
            items_.push_back(accents);

            Item opacity;
            opacity.kind = Kind::Slider;
            opacity.id = Id::Opacity;
            opacity.label = L"Непрозрачность";
            wchar_t buf[16];
            swprintf(buf, 16, L"%d%%", block.opacity);
            opacity.value = buf;
            opacity.fraction = static_cast<float>(block.opacity - kOpacityMin) /
                               static_cast<float>(kOpacityMax - kOpacityMin);
            items_.push_back(opacity);
        }

        section(L"СЕТКА БЛОКА");
        {
            Item size;
            size.kind = Kind::Slider;
            size.id = Id::IconSize;
            size.label = L"Размер значка";
            wchar_t buf[16];
            swprintf(buf, 16, L"%d px", block.iconSize);
            size.value = buf;
            size.fraction =
                static_cast<float>(block.iconSize - kIconMin) / static_cast<float>(kIconMax - kIconMin);
            items_.push_back(size);

            Item columns;
            columns.kind = Kind::Slider;
            columns.id = Id::Columns;
            columns.label = L"Значков в ряду";
            wchar_t cols[16];
            swprintf(cols, 16, L"%d", block.columns);
            columns.value = cols;
            columns.fraction = static_cast<float>(block.columns - kColumnsMin) /
                               static_cast<float>(kColumnsMax - kColumnsMin);
            items_.push_back(columns);

            toggle(Id::Labels, L"Подписи под значками", L"Имя ярлыка под картинкой", block.showLabels);
            toggle(Id::Locked, L"Закрепить блок", L"Не двигается и не меняет размер мышью", block.locked);
            link(Id::StyleToAll, L"Применить оформление ко всем блокам");
            link(Id::GoGlobal, L"Общие настройки виджета…");
        }
        return;
    }

    section(L"ОБЩЕЕ");
    toggle(Id::Snap, L"Магнит", L"Отпущенный блок встаёт вровень с краем и соседями", data.snapToGrid);
    toggle(Id::OnTop, L"Поверх всех окон", L"Блоки не прячутся за приложениями", data.alwaysOnTop);
    toggle(Id::Autostart, L"Запускать вместе с Windows", L"Запись в HKCU\\...\\Run", IsAutostartEnabled());
    toggle(Id::Hotkey, L"Горячая клавиша Ctrl+Alt+D", L"Показать или скрыть все блоки",
           data.hotkeyEnabled);
    toggle(Id::Storage, L"Забирать ярлык с рабочего стола",
           L"Ctrl при перетаскивании — копировать", data.moveIntoStorage);

    section(L"ДАННЫЕ");
    link(Id::RefreshIcons, L"Обновить значки и проверить ярлыки");
    link(Id::OpenData, L"Папка с данными");
    link(Id::OpenBackups, L"Резервные копии");
    link(Id::OpenStorage, L"Хранилище ярлыков");

    Item add;
    add.kind = Kind::Primary;
    add.id = Id::AddGroup;
    add.label = L"+   Новый блок";
    items_.push_back(add);
}

void Window::Layout() {
    const int pad = Sc(kPad, dpi_);
    cardW_ = Sc(kCardW, dpi_);

    int y = 0;
    for (Item& item : items_) {
        int h = 0;
        switch (item.kind) {
            case Kind::Section: h = Sc(30, dpi_); break;
            case Kind::Theme: h = Sc(42, dpi_); break;
            case Kind::Accents: h = Sc(40, dpi_); break;
            case Kind::Slider: h = Sc(56, dpi_); break;
            case Kind::Toggle: h = Sc(46, dpi_); break;
            case Kind::Link: h = Sc(38, dpi_); break;
            case Kind::Primary: h = Sc(56, dpi_); break;
        }
        item.rect = {0, y, cardW_ - pad * 2, y + h};  // координаты внутри содержимого
        y += h;
    }
    contentH_ = y + Sc(4, dpi_);

    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    int workH = 900;
    if (GetMonitorInfoW(mon, &mi)) workH = mi.rcWork.bottom - mi.rcWork.top;

    const int shadow = Sc(kShadow, dpi_);
    const int headerH = Sc(kHeaderH, dpi_);
    const int maxCard = workH - shadow * 2 - Sc(20, dpi_);
    const int wanted = headerH + contentH_ + pad;
    cardH_ = wanted < maxCard ? wanted : maxCard;

    card_ = {shadow, shadow, shadow + cardW_, shadow + cardH_};
    winW_ = cardW_ + shadow * 2;
    winH_ = cardH_ + shadow * 2;

    header_ = {card_.left, card_.top, card_.right, card_.top + headerH};
    const int close = Sc(26, dpi_);
    closeBtn_ = {card_.right - pad - close, header_.top + (headerH - close) / 2, card_.right - pad,
                 header_.top + (headerH - close) / 2 + close};
    viewport_ = {card_.left + pad, header_.bottom, card_.right - pad, card_.bottom - Sc(6, dpi_)};

    ClampScroll();

    // Переносим строки в экранные координаты окна с учётом прокрутки.
    for (Item& item : items_) {
        const int h = item.rect.bottom - item.rect.top;
        const int top = viewport_.top + item.rect.top - scroll_;
        item.rect = {viewport_.left, top, viewport_.right, top + h};
    }
}

void Window::ClampScroll() {
    const int viewH = viewport_.bottom - viewport_.top;
    const int maxScroll = contentH_ > viewH ? contentH_ - viewH : 0;
    scroll_ = Clamp(scroll_, 0, maxScroll);
}

RECT Window::ToggleRect(const Item& item) const {
    const int w = Sc(46, dpi_);
    const int h = Sc(26, dpi_);
    const int top = item.rect.top + (item.rect.bottom - item.rect.top - h) / 2;
    return {item.rect.right - w, top, item.rect.right, top + h};
}

RECT Window::TrackRect(const Item& item) const {
    const int h = Sc(6, dpi_);
    const int top = item.rect.bottom - Sc(18, dpi_);
    return {item.rect.left, top, item.rect.right, top + h};
}

RECT Window::AccentDot(const Item& item, int index) const {
    const int d = Sc(22, dpi_);
    const int gap = Sc(10, dpi_);
    const int total = AccentCount() * d + (AccentCount() - 1) * gap;
    const int left = item.rect.right - total + index * (d + gap);
    const int top = item.rect.top + (item.rect.bottom - item.rect.top - d) / 2;
    return {left, top, left + d, top + d};
}

// ---------- Поверхность ----------

void Window::ReleaseSurface() {
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

void Window::EnsureSurface(int w, int h) {
    if (memDc_ && surfaceW_ == w && surfaceH_ == h) return;
    ReleaseSurface();

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
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

void Window::Render() {
    const AppData& data = host_->Data();
    // Блок могли удалить, пока окно открыто: настройки блока в этом случае закрываются,
    // а общие живут дальше — они ни к какой полке не привязаны.
    if (!host_->HasGroup(groupIndex_)) {
        if (mode_ == Mode::Panel || data.groups.empty()) {
            Destroy();
            return;
        }
        groupIndex_ = 0;
    }
    // Настройки блока живут в его теме — сразу видно, что настраиваешь; общие берут тему
    // по умолчанию, ту, с которой рождается новый блок.
    const Group& block = data.groups[static_cast<size_t>(groupIndex_)];
    if (mode_ == Mode::Panel) SetTheme(block.darkTheme, block.accent);
    else SetTheme(data.darkTheme, data.accent);

    BuildItems();
    Layout();

    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    if (wr.right - wr.left != winW_ || wr.bottom - wr.top != winH_) {
        SetWindowPos(hwnd_, nullptr, 0, 0, winW_, winH_, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    EnsureSurface(winW_, winH_);
    if (!bits_) return;

    Bitmap surface(winW_, winH_, winW_ * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits_));
    Graphics g(&surface);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0x00000000));

    const float u = ScF(1.0f, dpi_);
    const float radius = ScF(kRadius, dpi_);
    RectF cardRect(static_cast<float>(card_.left), static_cast<float>(card_.top),
                   static_cast<float>(cardW_), static_cast<float>(cardH_));

    gfx::DropShadow(g, cardRect, radius, Sc(kShadow, dpi_));
    gfx::FillRoundRectGradient(g, cardRect, radius, P().cardBgTop, P().cardBg);
    gfx::StrokeRoundRect(g, cardRect, radius, P().cardBorder, u);
    gfx::TopHighlight(g, cardRect, radius, P().cardGloss, u);

    // --- Шапка ---
    {
        const float titleX = static_cast<float>(card_.left + Sc(kPad, dpi_));
        const float titleY = static_cast<float>(header_.top) + (header_.bottom - header_.top) / 2.0f -
                             ScF(10.0f, dpi_);
        const float maxW = static_cast<float>(closeBtn_.left) - titleX - ScF(10.0f, dpi_);
        const std::wstring caption =
            (mode_ == Mode::Panel) ? (L"Блок «" + block.title + L"»") : L"Настройки виджета";
        const std::wstring title = gfx::Ellipsize(g, caption, *fontTitle_, maxW);
        gfx::DrawText(g, title.c_str(), *fontTitle_, P().textPrimary, titleX, titleY);
    }
    {
        RectF closeRect(static_cast<float>(closeBtn_.left), static_cast<float>(closeBtn_.top),
                        static_cast<float>(closeBtn_.right - closeBtn_.left),
                        static_cast<float>(closeBtn_.bottom - closeBtn_.top));
        const bool hot = hot_ == -2;
        if (hot) gfx::FillRoundRect(g, closeRect, closeRect.Width / 2.0f, P().buttonBgHover);
        const float cx = closeRect.X + closeRect.Width / 2.0f;
        const float cy = closeRect.Y + closeRect.Height / 2.0f;
        Color c(hot ? P().danger : P().textMuted);
        Pen pen(c, ScF(1.6f, dpi_));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawLine(&pen, cx - 5 * u, cy - 5 * u, cx + 5 * u, cy + 5 * u);
        g.DrawLine(&pen, cx + 5 * u, cy - 5 * u, cx - 5 * u, cy + 5 * u);
    }
    {
        Color sep(P().separator);
        Pen pen(sep, u);
        g.DrawLine(&pen, static_cast<float>(card_.left + Sc(kPad, dpi_)), static_cast<float>(header_.bottom),
                   static_cast<float>(card_.right - Sc(kPad, dpi_)), static_cast<float>(header_.bottom));
    }

    g.SetClip(Rect(viewport_.left - Sc(4, dpi_), viewport_.top,
                   viewport_.right - viewport_.left + Sc(8, dpi_), viewport_.bottom - viewport_.top));

    for (size_t i = 0; i < items_.size(); i++) {
        const Item& item = items_[i];
        const RectF row(static_cast<float>(item.rect.left), static_cast<float>(item.rect.top),
                        static_cast<float>(item.rect.right - item.rect.left),
                        static_cast<float>(item.rect.bottom - item.rect.top));
        if (item.rect.bottom < viewport_.top || item.rect.top > viewport_.bottom) continue;
        const bool hot = (hot_ == static_cast<int>(i));

        switch (item.kind) {
            case Kind::Section:
                gfx::DrawTracked(g, item.label.c_str(), *fontSection_, P().textFaint, row.X,
                                 row.Y + row.Height - ScF(18.0f, dpi_), ScF(2.0f, dpi_));
                break;

            case Kind::Theme: {
                RectF box(row.X, row.Y + ScF(4.0f, dpi_), row.Width, row.Height - ScF(10.0f, dpi_));
                gfx::FillRoundRect(g, box, box.Height / 2.0f, P().buttonBg);
                RectF active(item.on ? box.X : box.X + box.Width / 2.0f, box.Y, box.Width / 2.0f, box.Height);
                gfx::FillRoundRect(g, active, active.Height / 2.0f, P().accent);
                gfx::DrawText(g, L"Тёмная", *fontRow_, item.on ? P().onAccent : P().textMuted,
                              box.X + box.Width / 4.0f, box.Y + (box.Height - ScF(16.0f, dpi_)) / 2.0f,
                              gfx::Align::Center);
                gfx::DrawText(g, L"Светлая", *fontRow_, item.on ? P().textMuted : P().onAccent,
                              box.X + box.Width * 3.0f / 4.0f,
                              box.Y + (box.Height - ScF(16.0f, dpi_)) / 2.0f, gfx::Align::Center);
                break;
            }

            case Kind::Accents: {
                gfx::DrawText(g, item.label.c_str(), *fontRow_, P().textPrimary, row.X,
                              row.Y + (row.Height - ScF(16.0f, dpi_)) / 2.0f);
                for (int a = 0; a < AccentCount(); a++) {
                    const RECT dot = AccentDot(item, a);
                    RectF df(static_cast<float>(dot.left), static_cast<float>(dot.top),
                             static_cast<float>(dot.right - dot.left),
                             static_cast<float>(dot.bottom - dot.top));
                    // Цвет кружка — сам акцент, поэтому временно переключаем палитру.
                    const Group& block = host_->Data().groups[static_cast<size_t>(groupIndex_)];
                    SetTheme(block.darkTheme, a);
                    const ARGB color = P().accent;
                    SetTheme(block.darkTheme, block.accent);

                    if (block.accent == a) {
                        gfx::StrokeRoundRect(g, RectF(df.X - 3 * u, df.Y - 3 * u, df.Width + 6 * u,
                                                      df.Height + 6 * u),
                                             (df.Width + 6 * u) / 2.0f, color, 1.8f * u);
                    }
                    gfx::FillRoundRect(g, df, df.Width / 2.0f, color);
                }
                break;
            }

            case Kind::Slider: {
                gfx::DrawText(g, item.label.c_str(), *fontRow_, P().textPrimary, row.X,
                              row.Y + ScF(6.0f, dpi_));
                gfx::DrawText(g, item.value.c_str(), *fontValue_, P().accent, row.GetRight(),
                              row.Y + ScF(5.0f, dpi_), gfx::Align::Right);

                const RECT track = TrackRect(item);
                RectF trackF(static_cast<float>(track.left), static_cast<float>(track.top),
                             static_cast<float>(track.right - track.left),
                             static_cast<float>(track.bottom - track.top));
                gfx::FillRoundRect(g, trackF, trackF.Height / 2.0f, P().buttonBg);
                RectF fill(trackF.X, trackF.Y, trackF.Width * item.fraction, trackF.Height);
                if (fill.Width < trackF.Height) fill.Width = trackF.Height;
                gfx::FillRoundRect(g, fill, trackF.Height / 2.0f, P().accent);

                const float knob = ScF(16.0f, dpi_);
                const float knobX = trackF.X + trackF.Width * item.fraction - knob / 2.0f;
                const float knobY = trackF.Y + trackF.Height / 2.0f - knob / 2.0f;
                RectF knobRect(knobX, knobY, knob, knob);
                gfx::FillRoundRect(g, RectF(knobX, knobY + u, knob, knob), knob / 2.0f, 0x30000000);
                gfx::FillRoundRect(g, knobRect, knob / 2.0f, P().dark ? 0xFFF2F4F8 : 0xFFFFFFFF);
                gfx::StrokeRoundRect(g, knobRect, knob / 2.0f, P().accent, 2.0f * u);
                break;
            }

            case Kind::Toggle: {
                if (hot) {
                    gfx::FillRoundRect(g, RectF(row.X - ScF(8.0f, dpi_), row.Y + ScF(2.0f, dpi_),
                                                row.Width + ScF(16.0f, dpi_), row.Height - ScF(4.0f, dpi_)),
                                       ScF(10.0f, dpi_), P().hover);
                }
                const bool hasHint = !item.hint.empty();
                gfx::DrawText(g, item.label.c_str(), *fontRow_, P().textPrimary, row.X,
                              row.Y + (hasHint ? ScF(8.0f, dpi_) : (row.Height - ScF(16.0f, dpi_)) / 2.0f));
                if (hasHint) {
                    const std::wstring hint =
                        gfx::Ellipsize(g, item.hint, *fontHint_, row.Width - ScF(60.0f, dpi_));
                    gfx::DrawText(g, hint.c_str(), *fontHint_, P().textFaint, row.X,
                                  row.Y + ScF(25.0f, dpi_));
                }

                const RECT tog = ToggleRect(item);
                RectF togF(static_cast<float>(tog.left), static_cast<float>(tog.top),
                           static_cast<float>(tog.right - tog.left),
                           static_cast<float>(tog.bottom - tog.top));
                gfx::FillRoundRect(g, togF, togF.Height / 2.0f, item.on ? P().accent : P().buttonBg);
                if (!item.on) gfx::StrokeRoundRect(g, togF, togF.Height / 2.0f, P().fieldBorder, u);
                const float knob = togF.Height - 4 * u;
                const float knobX = item.on ? togF.GetRight() - knob - 2 * u : togF.X + 2 * u;
                gfx::FillRoundRect(g, RectF(knobX, togF.Y + 2 * u, knob, knob), knob / 2.0f,
                                   item.on ? P().onAccent : (P().dark ? 0xFFAAB2C0 : 0xFFFFFFFF));
                break;
            }

            case Kind::Link: {
                if (hot) {
                    gfx::FillRoundRect(g, RectF(row.X - ScF(8.0f, dpi_), row.Y + ScF(1.0f, dpi_),
                                                row.Width + ScF(16.0f, dpi_), row.Height - ScF(2.0f, dpi_)),
                                       ScF(10.0f, dpi_), P().hover);
                }
                gfx::DrawText(g, item.label.c_str(), *fontRow_, hot ? P().textPrimary : P().textMuted, row.X,
                              row.Y + (row.Height - ScF(16.0f, dpi_)) / 2.0f);
                const float cx = row.GetRight() - ScF(6.0f, dpi_);
                const float cy = row.Y + row.Height / 2.0f;
                Color arrow(hot ? P().accent : P().textFaint);
                Pen pen(arrow, ScF(1.5f, dpi_));
                pen.SetStartCap(LineCapRound);
                pen.SetEndCap(LineCapRound);
                g.DrawLine(&pen, cx - 4 * u, cy - 4 * u, cx, cy);
                g.DrawLine(&pen, cx, cy, cx - 4 * u, cy + 4 * u);
                break;
            }

            case Kind::Primary: {
                RectF box(row.X, row.Y + ScF(10.0f, dpi_), row.Width, row.Height - ScF(16.0f, dpi_));
                ARGB bg = P().accent;
                if (hot) bg = (bg & 0x00FFFFFF) | 0xD0000000;
                gfx::FillRoundRect(g, box, ScF(10.0f, dpi_), bg);
                gfx::DrawText(g, item.label.c_str(), *fontRow_, P().onAccent, box.X + box.Width / 2.0f,
                              box.Y + (box.Height - ScF(16.0f, dpi_)) / 2.0f, gfx::Align::Center);
                break;
            }
        }
    }

    g.ResetClip();

    // Полоса прокрутки, если настройки не поместились по высоте.
    const int viewH = viewport_.bottom - viewport_.top;
    if (contentH_ > viewH) {
        const float trackX = static_cast<float>(card_.right) - ScF(8.0f, dpi_);
        const float trackH = static_cast<float>(viewH);
        const float thumbH = trackH * static_cast<float>(viewH) / static_cast<float>(contentH_);
        const float offset = (trackH - thumbH) * static_cast<float>(scroll_) /
                             static_cast<float>(contentH_ - viewH);
        gfx::FillRoundRect(g, RectF(trackX, static_cast<float>(viewport_.top), 3 * u, trackH), 1.5f * u,
                           P().separator);
        gfx::FillRoundRect(g, RectF(trackX, static_cast<float>(viewport_.top) + offset, 3 * u, thumbH),
                           1.5f * u, P().textFaint);
    }

    POINT src = {0, 0};
    SIZE size = {winW_, winH_};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, screen, nullptr, &size, memDc_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// ---------- Взаимодействие ----------

int Window::HitTest(POINT pt) const {
    if (PtInRect(&closeBtn_, pt)) return -2;
    if (!PtInRect(&viewport_, pt)) return -1;
    for (size_t i = 0; i < items_.size(); i++) {
        if (items_[i].kind == Kind::Section) continue;
        if (PtInRect(&items_[i].rect, pt)) return static_cast<int>(i);
    }
    return -1;
}

void Window::ApplySlider(const Item& item, POINT pt) {
    const RECT track = TrackRect(item);
    const int width = track.right - track.left;
    if (width <= 0) return;
    float t = static_cast<float>(pt.x - track.left) / static_cast<float>(width);
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;

    AppData& data = host_->Data();
    Group& block = data.groups[static_cast<size_t>(groupIndex_)];
    if (item.id == Id::Opacity) {
        const int value = kOpacityMin + static_cast<int>(t * (kOpacityMax - kOpacityMin) + 0.5f);
        if (value == block.opacity) return;
        block.opacity = value;
        data.opacity = value;  // запоминаем как значение по умолчанию для новых блоков
        host_->ApplyBlockLook(groupIndex_);
    } else if (item.id == Id::IconSize) {
        const int steps = (kIconMax - kIconMin) / kIconStep;
        const int value = kIconMin + static_cast<int>(t * steps + 0.5f) * kIconStep;
        if (value == block.iconSize) return;
        block.iconSize = value;
        data.iconSize = value;
        host_->ApplyBlockLayout(groupIndex_);
    } else if (item.id == Id::Columns) {
        const int value = kColumnsMin + static_cast<int>(t * (kColumnsMax - kColumnsMin) + 0.5f);
        if (value == block.columns) return;
        block.columns = value;
        host_->ApplyBlockLayout(groupIndex_);
    }
    host_->Save();
    Render();
}

void Window::Activate(int index, POINT pt) {
    if (index == -2) {
        Destroy();
        return;
    }
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item item = items_[static_cast<size_t>(index)];
    AppData& data = host_->Data();
    Group& block = data.groups[static_cast<size_t>(groupIndex_)];

    switch (item.id) {
        case Id::Theme: {
            const bool dark = pt.x < (item.rect.left + item.rect.right) / 2;
            if (dark == block.darkTheme) return;
            block.darkTheme = dark;
            data.darkTheme = dark;
            host_->Save();
            host_->ApplyBlockLook(groupIndex_);
            break;
        }
        case Id::Accent:
            for (int a = 0; a < AccentCount(); a++) {
                const RECT dot = AccentDot(item, a);
                RECT wide = {dot.left - Sc(5, dpi_), dot.top - Sc(8, dpi_), dot.right + Sc(5, dpi_),
                             dot.bottom + Sc(8, dpi_)};
                if (PtInRect(&wide, pt)) {
                    block.accent = a;
                    data.accent = a;
                    host_->Save();
                    host_->ApplyBlockLook(groupIndex_);
                    break;
                }
            }
            break;
        case Id::Labels:
            block.showLabels = !block.showLabels;
            data.showLabels = block.showLabels;
            host_->Save();
            host_->ApplyBlockLayout(groupIndex_);
            break;
        case Id::Locked:
            block.locked = !block.locked;
            host_->Save();
            host_->ApplyBlockLayout(groupIndex_);
            break;
        case Id::Snap:
            data.snapToGrid = !data.snapToGrid;
            host_->Save();
            break;
        case Id::StyleToAll:
            host_->ApplyStyleToAll(groupIndex_);
            host_->Save();
            break;
        case Id::GoGlobal:
            SetMode(Mode::Global);
            break;
        case Id::OnTop:
            data.alwaysOnTop = !data.alwaysOnTop;
            host_->Save();
            host_->ApplyTopmost();
            break;
        case Id::Autostart:
            if (!SetAutostart(!IsAutostartEnabled())) {
                MessageBoxW(hwnd_, L"Не удалось изменить автозапуск.", L"Автозапуск", MB_OK | MB_ICONWARNING);
            }
            break;
        case Id::Hotkey:
            data.hotkeyEnabled = !data.hotkeyEnabled;
            host_->Save();
            host_->ApplyHotkey();
            break;
        case Id::Storage:
            data.moveIntoStorage = !data.moveIntoStorage;
            host_->Save();
            if (data.moveIntoStorage) {
                MessageBoxW(hwnd_,
                            L"Теперь брошенный на блок ярлык (.lnk или .url) переедет в папку\n"
                            L"%APPDATA%\\ShortcutDock\\shortcuts и пропадёт с рабочего стола.\n\n"
                            L"Уже добавленные ярлыки остаются на своих местах.",
                            L"Хранилище ярлыков", MB_OK | MB_ICONINFORMATION);
            }
            break;
        case Id::RefreshIcons: host_->RefreshIcons(); break;
        case Id::OpenData: OpenFolder(DataDir()); break;
        case Id::OpenBackups: OpenFolder(BackupDir()); break;
        case Id::OpenStorage:
            SHCreateDirectoryExW(nullptr, StorageDir().c_str(), nullptr);
            OpenFolder(StorageDir());
            break;
        case Id::AddGroup: host_->AddGroup(); break;
        default: break;
    }
    Render();
}

LRESULT Window::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!mouseTracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                mouseTracking_ = true;
            }
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (dragItem_ >= 0 && dragItem_ < static_cast<int>(items_.size())) {
                ApplySlider(items_[static_cast<size_t>(dragItem_)], pt);
                return 0;
            }
            const int hit = HitTest(pt);
            if (hit != hot_) {
                hot_ = hit;
                Render();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            if (hot_ != -1) {
                hot_ = -1;
                Render();
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, hot_ != -1 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int hit = HitTest(pt);
            if (hit >= 0 && items_[static_cast<size_t>(hit)].kind == Kind::Slider) {
                dragItem_ = hit;
                SetCapture(hwnd);
                ApplySlider(items_[static_cast<size_t>(hit)], pt);
                return 0;
            }
            if (hit != -1) return 0;  // остальное срабатывает по отпусканию
            // Пустое место или шапка — перенос окна.
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (dragItem_ >= 0) {
                dragItem_ = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            const int hit = HitTest(pt);
            if (hit != -1) Activate(hit, pt);
            return 0;
        }

        case WM_LBUTTONDBLCLK:
            return 0;

        case WM_CAPTURECHANGED:
            dragItem_ = -1;
            return 0;

        case WM_MOUSEWHEEL: {
            const int viewH = viewport_.bottom - viewport_.top;
            if (contentH_ <= viewH) return 0;
            scroll_ += (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1) * Sc(48, dpi_);
            ClampScroll();
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            Render();
            hot_ = HitTest(pt);
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) Destroy();
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            family_.reset(new FontFamily(L"Segoe UI"));
            fontTitle_.reset(new Font(family_.get(), ScF(16.0f, dpi_), FontStyleBold, UnitPixel));
            fontSection_.reset(new Font(family_.get(), ScF(10.0f, dpi_), FontStyleBold, UnitPixel));
            fontRow_.reset(new Font(family_.get(), ScF(13.0f, dpi_), FontStyleRegular, UnitPixel));
            fontValue_.reset(new Font(family_.get(), ScF(13.0f, dpi_), FontStyleBold, UnitPixel));
            fontHint_.reset(new Font(family_.get(), ScF(10.5f, dpi_), FontStyleRegular, UnitPixel));
            auto* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Render();
            return 0;
        }

        case WM_DESTROY:
            ReleaseSurface();
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Window::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_window.hwnd_ == hwnd || msg == WM_NCCREATE) return g_window.WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Window::Open(HINSTANCE inst, Host* host, int group, POINT nearScreenPt, Mode mode) {
    host_ = host;
    groupIndex_ = group;
    mode_ = mode;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &Window::WndProcThunk;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClassName, L"Настройки", WS_POPUP, 0, 0, 100,
                            100, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    dpi_ = WindowDpi(hwnd_);
    family_.reset(new FontFamily(L"Segoe UI"));
    fontTitle_.reset(new Font(family_.get(), ScF(16.0f, dpi_), FontStyleBold, UnitPixel));
    fontSection_.reset(new Font(family_.get(), ScF(10.0f, dpi_), FontStyleBold, UnitPixel));
    fontRow_.reset(new Font(family_.get(), ScF(13.0f, dpi_), FontStyleRegular, UnitPixel));
    fontValue_.reset(new Font(family_.get(), ScF(13.0f, dpi_), FontStyleBold, UnitPixel));
    fontHint_.reset(new Font(family_.get(), ScF(10.5f, dpi_), FontStyleRegular, UnitPixel));

    scroll_ = 0;
    BuildItems();
    Layout();

    // Ставим рядом с блоком, из которого открыли, но внутри рабочей области.
    int x = nearScreenPt.x;
    int y = nearScreenPt.y;
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromPoint(nearScreenPt, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(mon, &mi)) {
        if (x + winW_ > mi.rcWork.right) x = mi.rcWork.right - winW_;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + winH_ > mi.rcWork.bottom) y = mi.rcWork.bottom - winH_;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, winW_, winH_, SWP_NOACTIVATE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Render();
    SetForegroundWindow(hwnd_);
    return true;
}

void Window::Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
}

}  // namespace

void Show(HINSTANCE inst, Host* host, int group, POINT nearScreenPt, Mode mode) {
    if (g_window.IsOpen()) {
        // Окно одно: попросили настройки другого блока или общие — переключаем его.
        g_window.SetGroup(group);
        g_window.SetMode(mode);
        SetWindowPos(g_window.Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetForegroundWindow(g_window.Hwnd());
        g_window.Refresh();
        return;
    }
    g_window.Open(inst, host, group, nearScreenPt, mode);
}

void Close() { g_window.Destroy(); }

bool IsOpen() { return g_window.IsOpen(); }

}  // namespace settings
