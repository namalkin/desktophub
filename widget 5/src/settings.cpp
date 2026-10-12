#include "settings.h"

#include <windowsx.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "library.h"
#include "sys.h"
#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace settings {

namespace {

constexpr wchar_t kClassName[] = L"TuneBoxSettings";
constexpr int kShadow = 18;
constexpr int kPad = 18;
constexpr int kCardW = 380;
constexpr int kRadius = 16;
constexpr int kHeaderH = 46;

enum class Id {
    None,
    Close,
    Theme,
    Accent,
    Opacity,
    Font,        // payload — индекс шрифта
    ShowCover,
    ShowList,
    ListRows,
    CardWidth,
    Locked,
    PickFolder,
    Recursive,
    NameMode,    // payload — library::NameMode
    Shuffle,
    Repeat,      // payload — RepeatMode
    Volume,
    Resume,
    PlayOnStart,
    OnTop,
    Hotkeys,
    MediaKeys,
    Autostart,
    OpenData,
    OpenMusic,
    Rescan,
};

enum class Kind { Section, Theme, Accents, Slider, Toggle, Link, Option, Primary, Note };

struct Item {
    Kind kind = Kind::Section;
    Id id = Id::None;
    int payload = 0;
    std::wstring label;
    std::wstring value;  // подпись значения справа (для ползунка)
    std::wstring hint;   // мелкая строка под подписью
    bool on = false;
    float fraction = 0.f;
    RECT rect{};
};

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Границы ползунков. Шаг нужен, чтобы значение не дрожало по пикселю.
struct Range {
    int lo, hi, step;
};

Range RangeOf(Id id) {
    switch (id) {
        case Id::Opacity: return {40, 100, 1};
        case Id::ListRows: return {0, 14, 1};
        case Id::CardWidth: return {300, 620, 10};
        case Id::Volume: return {0, 100, 1};
        default: return {0, 100, 1};
    }
}

class Window {
public:
    bool Open(HINSTANCE inst, Host* host, POINT nearScreenPt);
    void Destroy();
    bool IsOpen() const { return hwnd_ != nullptr; }
    HWND Hwnd() const { return hwnd_; }
    void Refresh() {
        if (hwnd_) Render();
    }

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void RebuildFonts();
    void BuildItems();
    void Layout();
    void Render();
    void EnsureSurface(int w, int h);
    void ReleaseSurface();
    int HitTest(POINT pt) const;
    RECT ToggleRect(const Item& item) const;
    RECT TrackRect(const Item& item) const;
    RECT AccentDot(const Item& item, int index) const;
    void ApplySlider(const Item& item, POINT pt);
    void Activate(int index, POINT pt);
    void ClampScroll();

    HWND hwnd_ = nullptr;
    Host* host_ = nullptr;
    int dpi_ = 96;
    int fontIndex_ = -1;

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

void Window::RebuildFonts() {
    fontIndex_ = host_ ? host_->Data().fontIndex : 0;
    family_.reset(MakeUiFontFamily(fontIndex_));
    const float k = UiFontScale(fontIndex_);
    auto make = [&](float size, INT style) {
        return std::unique_ptr<Font>(new Font(family_.get(), ScF(size * k, dpi_), style, UnitPixel));
    };
    fontTitle_ = make(16.0f, FontStyleBold);
    fontSection_ = make(10.0f, FontStyleBold);
    fontRow_ = make(13.0f, FontStyleRegular);
    fontValue_ = make(13.0f, FontStyleBold);
    fontHint_ = make(10.5f, FontStyleRegular);
}

void Window::BuildItems() {
    AppData& data = host_->Data();
    items_.clear();

    auto section = [&](const wchar_t* text) {
        Item item;
        item.kind = Kind::Section;
        item.label = text;
        items_.push_back(item);
    };
    auto toggle = [&](Id id, const wchar_t* label, const wchar_t* hint, bool on, int payload = 0) {
        Item item;
        item.kind = Kind::Toggle;
        item.id = id;
        item.payload = payload;
        item.label = label;
        item.hint = hint;
        item.on = on;
        items_.push_back(item);
    };
    auto link = [&](Id id, const wchar_t* label, const std::wstring& hint = std::wstring()) {
        Item item;
        item.kind = Kind::Link;
        item.id = id;
        item.label = label;
        item.hint = hint;
        items_.push_back(item);
    };
    auto option = [&](Id id, int payload, const wchar_t* label, const wchar_t* hint, bool on) {
        Item item;
        item.kind = Kind::Option;
        item.id = id;
        item.payload = payload;
        item.label = label;
        item.hint = hint;
        item.on = on;
        items_.push_back(item);
    };
    auto slider = [&](Id id, const wchar_t* label, const std::wstring& value, int current) {
        const Range range = RangeOf(id);
        Item item;
        item.kind = Kind::Slider;
        item.id = id;
        item.label = label;
        item.value = value;
        item.fraction = static_cast<float>(current - range.lo) / static_cast<float>(range.hi - range.lo);
        items_.push_back(item);
    };

    section(L"ПАПКА С МУЗЫКОЙ");
    link(Id::PickFolder, L"Выбрать папку…",
         data.folder.empty() ? std::wstring(L"папка не выбрана") : data.folder);
    toggle(Id::Recursive, L"Вместе с подпапками", L"Искать треки во всех вложенных папках",
           data.recursive);
    {
        Item note;
        note.kind = Kind::Note;
        note.label = host_->StatusLine();
        items_.push_back(note);
    }
    {
        Item rescan;
        rescan.kind = Kind::Primary;
        rescan.id = Id::Rescan;
        rescan.label = library::Busy() ? L"Читаю теги…" : L"Обновить список";
        items_.push_back(rescan);
    }

    section(L"КАК ЧИТАТЬ ИМЯ ФАЙЛА");
    option(Id::NameMode, 0, L"Сначала теги", L"Название и исполнитель из тегов, имя файла — на подхвате",
           data.nameMode == 0);
    option(Id::NameMode, 1, L"Исполнитель — Название", L"«Era - Ameno.mp3» → Ameno, исполнитель Era",
           data.nameMode == 1);
    option(Id::NameMode, 2, L"Название — Исполнитель", L"«Ameno - Era.mp3» → Ameno, исполнитель Era",
           data.nameMode == 2);

    section(L"ВОСПРОИЗВЕДЕНИЕ");
    toggle(Id::Shuffle, L"Перемешать", L"Треки идут в случайном порядке", data.shuffle);
    option(Id::Repeat, static_cast<int>(RepeatMode::Off), L"По кругу: выключено",
           L"Список доиграл — плеер останавливается", data.repeat == RepeatMode::Off);
    option(Id::Repeat, static_cast<int>(RepeatMode::All), L"По кругу: весь список",
           L"После последнего трека начинается первый", data.repeat == RepeatMode::All);
    option(Id::Repeat, static_cast<int>(RepeatMode::One), L"По кругу: один трек",
           L"Текущий трек повторяется, пока не переключишь", data.repeat == RepeatMode::One);
    {
        wchar_t buf[16];
        swprintf(buf, 16, L"%d%%", data.volume);
        slider(Id::Volume, L"Громкость", buf, data.volume);
    }
    toggle(Id::Resume, L"Помнить, где остановились", L"При запуске открывается тот же трек и та же секунда",
           data.resumeOnStart);
    toggle(Id::PlayOnStart, L"Играть сразу при запуске", L"Иначе трек открывается на паузе",
           data.playOnStart);

    section(L"ОФОРМЛЕНИЕ БЛОКА");
    {
        Item theme;
        theme.kind = Kind::Theme;
        theme.id = Id::Theme;
        theme.on = data.darkTheme;
        items_.push_back(theme);

        Item accents;
        accents.kind = Kind::Accents;
        accents.id = Id::Accent;
        accents.label = L"Акцент";
        items_.push_back(accents);

        wchar_t buf[16];
        swprintf(buf, 16, L"%d%%", data.opacity);
        slider(Id::Opacity, L"Непрозрачность", buf, data.opacity);

        swprintf(buf, 16, L"%d", data.cardW);
        slider(Id::CardWidth, L"Ширина блока", buf, data.cardW);

        if (data.listRows == 0) {
            slider(Id::ListRows, L"Строк в списке", L"список скрыт", data.listRows);
        } else {
            swprintf(buf, 16, L"%d", data.listRows);
            slider(Id::ListRows, L"Строк в списке", buf, data.listRows);
        }
    }
    toggle(Id::ShowCover, L"Обложка", L"Картинка из тегов трека — та же, что видно в проводнике",
           data.showCover);
    toggle(Id::ShowList, L"Список треков", L"Строки под кнопками: клик — играть", data.showList);
    toggle(Id::Locked, L"Закрепить блок", L"Не двигается и не меняет размер мышью", data.locked);

    section(L"ШРИФТ");
    for (int i = 0; i < UiFontCount(); i++) {
        option(Id::Font, i, UiFontLabel(i), L"", data.fontIndex == i);
    }

    section(L"ОБЩЕЕ");
    toggle(Id::OnTop, L"Поверх всех окон", L"Блок не прячется за приложениями", data.alwaysOnTop);
    toggle(Id::Hotkeys, L"Горячие клавиши",
           L"Ctrl+Alt+P — пауза, ← → — треки, ↑ ↓ — громкость", data.hotkeysEnabled);
    toggle(Id::MediaKeys, L"Мультимедийные клавиши",
           L"Кнопки «играть», «дальше», «назад» на клавиатуре", data.mediaKeys);
    toggle(Id::Autostart, L"Запускать вместе с Windows", L"Запись в HKCU\\...\\Run", IsAutostartEnabled());

    section(L"ДАННЫЕ");
    link(Id::OpenData, L"Папка с настройками");
    link(Id::OpenMusic, L"Открыть папку с музыкой");
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
            case Kind::Toggle: h = item.hint.empty() ? Sc(40, dpi_) : Sc(46, dpi_); break;
            case Kind::Link: h = item.hint.empty() ? Sc(38, dpi_) : Sc(48, dpi_); break;
            case Kind::Option: h = item.hint.empty() ? Sc(40, dpi_) : Sc(48, dpi_); break;
            case Kind::Primary: h = Sc(56, dpi_); break;
            case Kind::Note: h = Sc(44, dpi_); break;
        }
        item.rect = {0, y, cardW_ - pad * 2, y + h};
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
    const int d = Sc(20, dpi_);
    const int gap = Sc(8, dpi_);
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
    SetTheme(data.darkTheme, data.accent);
    if (fontIndex_ != data.fontIndex) RebuildFonts();

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

    {
        const float titleX = static_cast<float>(card_.left + Sc(kPad, dpi_));
        const float titleY =
            static_cast<float>(header_.top) + (header_.bottom - header_.top) / 2.0f - ScF(10.0f, dpi_);
        const float maxW = static_cast<float>(closeBtn_.left) - titleX - ScF(10.0f, dpi_);
        gfx::DrawText(g, gfx::Ellipsize(g, L"Настройки плеера", *fontTitle_, maxW).c_str(), *fontTitle_,
                      P().textPrimary, titleX, titleY);
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

    g.SetClip(Rect(viewport_.left - Sc(4, dpi_), viewport_.top, viewport_.right - viewport_.left + Sc(8, dpi_),
                   viewport_.bottom - viewport_.top));

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
                              box.X + box.Width * 3.0f / 4.0f, box.Y + (box.Height - ScF(16.0f, dpi_)) / 2.0f,
                              gfx::Align::Center);
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
                    const int current = data.accent;
                    SetTheme(data.darkTheme, a);
                    const ARGB color = P().accent;
                    SetTheme(data.darkTheme, current);

                    if (current == a) {
                        gfx::StrokeRoundRect(
                            g, RectF(df.X - 3 * u, df.Y - 3 * u, df.Width + 6 * u, df.Height + 6 * u),
                            (df.Width + 6 * u) / 2.0f, color, 1.8f * u);
                    }
                    gfx::FillRoundRect(g, df, df.Width / 2.0f, color);
                }
                break;
            }

            case Kind::Slider: {
                gfx::DrawText(g, item.label.c_str(), *fontRow_, P().textPrimary, row.X, row.Y + ScF(6.0f, dpi_));
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
                    gfx::DrawText(g, hint.c_str(), *fontHint_, P().textFaint, row.X, row.Y + ScF(25.0f, dpi_));
                }

                const RECT tog = ToggleRect(item);
                RectF togF(static_cast<float>(tog.left), static_cast<float>(tog.top),
                           static_cast<float>(tog.right - tog.left), static_cast<float>(tog.bottom - tog.top));
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
                const bool hasHint = !item.hint.empty();
                gfx::DrawText(g, item.label.c_str(), *fontRow_, hot ? P().textPrimary : P().textMuted, row.X,
                              row.Y + (hasHint ? ScF(8.0f, dpi_) : (row.Height - ScF(16.0f, dpi_)) / 2.0f));
                if (hasHint) {
                    // Путь к папке длинный — показываем его хвост, он информативнее начала.
                    std::wstring hint = item.hint;
                    const float maxW = row.Width - ScF(26.0f, dpi_);
                    while (hint.size() > 4 && gfx::TextWidth(g, (L"…" + hint).c_str(), *fontHint_) > maxW) {
                        hint.erase(0, 1);
                    }
                    if (hint != item.hint) hint = L"…" + hint;
                    gfx::DrawText(g, hint.c_str(), *fontHint_, P().textFaint, row.X, row.Y + ScF(26.0f, dpi_));
                }
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

            case Kind::Option: {
                if (hot) {
                    gfx::FillRoundRect(g, RectF(row.X - ScF(8.0f, dpi_), row.Y + ScF(2.0f, dpi_),
                                                row.Width + ScF(16.0f, dpi_), row.Height - ScF(4.0f, dpi_)),
                                       ScF(10.0f, dpi_), P().hover);
                }
                const float mark = ScF(20.0f, dpi_);
                const bool hasHint = !item.hint.empty();
                // Названия шрифтов пишем ими самими — выбирать на глаз, а не по имени.
                if (item.id == Id::Font) {
                    const int index = item.payload;
                    std::unique_ptr<FontFamily> family(MakeUiFontFamily(index));
                    Font preview(family.get(), ScF(14.0f * UiFontScale(index), dpi_),
                                 item.on ? FontStyleBold : FontStyleRegular, UnitPixel);
                    const std::wstring label =
                        gfx::Ellipsize(g, item.label, preview, row.Width - mark - ScF(12.0f, dpi_));
                    gfx::DrawText(g, label.c_str(), preview, item.on ? P().textPrimary : P().textMuted, row.X,
                                  row.Y + (row.Height - ScF(17.0f, dpi_)) / 2.0f);
                } else {
                    const std::wstring label =
                        gfx::Ellipsize(g, item.label, *fontRow_, row.Width - mark - ScF(12.0f, dpi_));
                    gfx::DrawText(g, label.c_str(), *fontRow_, item.on ? P().textPrimary : P().textMuted, row.X,
                                  row.Y + (hasHint ? ScF(8.0f, dpi_) : (row.Height - ScF(16.0f, dpi_)) / 2.0f));
                    if (hasHint) {
                        gfx::DrawText(
                            g, gfx::Ellipsize(g, item.hint, *fontHint_, row.Width - mark - ScF(12.0f, dpi_)).c_str(),
                            *fontHint_, P().textFaint, row.X, row.Y + ScF(26.0f, dpi_));
                    }
                }

                RectF dot(row.GetRight() - mark, row.Y + (row.Height - mark) / 2.0f, mark, mark);
                if (item.on) {
                    gfx::FillRoundRect(g, dot, mark / 2.0f, P().accent);
                    Color check(P().onAccent);
                    Pen pen(check, ScF(1.8f, dpi_));
                    pen.SetStartCap(LineCapRound);
                    pen.SetEndCap(LineCapRound);
                    g.DrawLine(&pen, dot.X + mark * 0.26f, dot.Y + mark * 0.52f, dot.X + mark * 0.44f,
                               dot.Y + mark * 0.70f);
                    g.DrawLine(&pen, dot.X + mark * 0.44f, dot.Y + mark * 0.70f, dot.X + mark * 0.76f,
                               dot.Y + mark * 0.32f);
                } else {
                    gfx::StrokeRoundRect(g, dot, mark / 2.0f, hot ? P().accent : P().fieldBorder, u);
                }
                break;
            }

            case Kind::Note: {
                RectF box(row.X, row.Y + ScF(4.0f, dpi_), row.Width, row.Height - ScF(10.0f, dpi_));
                gfx::FillRoundRect(g, box, ScF(9.0f, dpi_), P().groupBg);
                const std::wstring line = gfx::Ellipsize(g, item.label, *fontHint_, box.Width - ScF(20.0f, dpi_));
                gfx::DrawText(g, line.c_str(), *fontHint_, P().textMuted, box.X + ScF(10.0f, dpi_),
                              box.Y + (box.Height - ScF(14.0f, dpi_)) / 2.0f);
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

    const int viewH = viewport_.bottom - viewport_.top;
    if (contentH_ > viewH) {
        const float trackX = static_cast<float>(card_.right) - ScF(8.0f, dpi_);
        const float trackH = static_cast<float>(viewH);
        const float thumbH = trackH * static_cast<float>(viewH) / static_cast<float>(contentH_);
        const float offset =
            (trackH - thumbH) * static_cast<float>(scroll_) / static_cast<float>(contentH_ - viewH);
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
        if (items_[i].kind == Kind::Section || items_[i].kind == Kind::Note) continue;
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
    const Range range = RangeOf(item.id);
    int value = range.lo + static_cast<int>(t * (range.hi - range.lo) + 0.5f);
    value = (value / range.step) * range.step;
    value = Clamp(value, range.lo, range.hi);

    switch (item.id) {
        case Id::Opacity:
            if (value == data.opacity) return;
            data.opacity = value;
            host_->ApplyLook();
            break;
        case Id::CardWidth:
            if (value == data.cardW) return;
            data.cardW = value;
            host_->ApplyLook();
            break;
        case Id::ListRows:
            if (value == data.listRows) return;
            data.listRows = value;
            host_->ApplyLook();
            break;
        case Id::Volume:
            if (value == data.volume) return;
            data.volume = value;
            host_->ApplyVolume();
            host_->ApplyLook();
            break;
        default:
            return;
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

    switch (item.id) {
        case Id::Theme: {
            const bool dark = pt.x < (item.rect.left + item.rect.right) / 2;
            if (dark == data.darkTheme) return;
            data.darkTheme = dark;
            host_->Save();
            host_->ApplyLook();
            break;
        }
        case Id::Accent:
            for (int a = 0; a < AccentCount(); a++) {
                const RECT dot = AccentDot(item, a);
                RECT wide = {dot.left - Sc(4, dpi_), dot.top - Sc(8, dpi_), dot.right + Sc(4, dpi_),
                             dot.bottom + Sc(8, dpi_)};
                if (PtInRect(&wide, pt)) {
                    data.accent = a;
                    host_->Save();
                    host_->ApplyLook();
                    break;
                }
            }
            break;
        case Id::Font:
            if (data.fontIndex == item.payload) break;
            data.fontIndex = item.payload;
            host_->Save();
            host_->ApplyLook();
            RebuildFonts();
            break;
        case Id::ShowCover:
            data.showCover = !data.showCover;
            host_->Save();
            host_->ApplyLook();
            break;
        case Id::ShowList:
            data.showList = !data.showList;
            host_->Save();
            host_->ApplyLook();
            break;
        case Id::Locked:
            data.locked = !data.locked;
            host_->Save();
            host_->ApplyLook();
            break;
        case Id::PickFolder:
            host_->ChooseFolder(hwnd_);
            break;
        case Id::Recursive:
            data.recursive = !data.recursive;
            host_->Save();
            host_->Rescan();
            break;
        case Id::NameMode:
            if (data.nameMode == item.payload) break;
            data.nameMode = item.payload;
            host_->Save();
            host_->ApplyNameMode();
            break;
        case Id::Shuffle:
            data.shuffle = !data.shuffle;
            host_->Save();
            host_->ApplyLook();
            break;
        case Id::Repeat:
            data.repeat = static_cast<RepeatMode>(item.payload);
            host_->Save();
            host_->ApplyLook();
            break;
        case Id::Resume:
            data.resumeOnStart = !data.resumeOnStart;
            host_->Save();
            break;
        case Id::PlayOnStart:
            data.playOnStart = !data.playOnStart;
            host_->Save();
            break;
        case Id::OnTop:
            data.alwaysOnTop = !data.alwaysOnTop;
            host_->Save();
            host_->ApplyTopmost();
            break;
        case Id::Hotkeys:
            data.hotkeysEnabled = !data.hotkeysEnabled;
            host_->Save();
            host_->ApplyHotkeys();
            break;
        case Id::MediaKeys:
            data.mediaKeys = !data.mediaKeys;
            host_->Save();
            host_->ApplyHotkeys();
            break;
        case Id::Autostart:
            if (!SetAutostart(!IsAutostartEnabled())) {
                MessageBoxW(hwnd_, L"Не удалось изменить автозапуск.", L"Автозапуск", MB_OK | MB_ICONWARNING);
            }
            break;
        case Id::OpenData: OpenFolder(DataDir()); break;
        case Id::OpenMusic: OpenFolder(data.folder); break;
        case Id::Rescan: host_->Rescan(); break;
        default: break;
    }
    if (hwnd_) Render();
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
            RebuildFonts();
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

bool Window::Open(HINSTANCE inst, Host* host, POINT nearScreenPt) {
    host_ = host;

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

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClassName, L"Настройки плеера", WS_POPUP, 0, 0,
                            100, 100, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    dpi_ = WindowDpi(hwnd_);
    RebuildFonts();

    scroll_ = 0;
    BuildItems();
    Layout();

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

void Show(HINSTANCE inst, Host* host, POINT nearScreenPt) {
    if (g_window.IsOpen()) {
        SetWindowPos(g_window.Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetForegroundWindow(g_window.Hwnd());
        g_window.Refresh();
        return;
    }
    g_window.Open(inst, host, nearScreenPt);
}

void Close() { g_window.Destroy(); }

bool IsOpen() { return g_window.IsOpen(); }

void Refresh() { g_window.Refresh(); }

}  // namespace settings
