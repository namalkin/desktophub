#include "hub.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <memory>
#include <string>
#include <vector>

#include "catalog.h"
#include "control.h"
#include "download.h"
#include "hub_link.h"
#include "markdown.h"
#include "store.h"
#include "sys.h"
#include "ui.h"
#include "util.h"

using namespace Gdiplus;

namespace {

constexpr UINT kTimerPoll = 1;
constexpr UINT kTimerPollSoon = 2;  // разовый опрос вскоре после команды виджету
constexpr UINT kPollMs = 1000;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr int kIconResourceId = 1;
constexpr int kEditId = 100;

constexpr wchar_t kHubRunValue[] = L"WidgetHub";

// Значки — эмодзи из Segoe UI Emoji: их форму узнают с первого взгляда, в отличие от
// пиктограмм MDL2 («скрыть» там — чёрточка, «закрыть» — пустой квадрат). GDI+ рисует
// их одним цветом — цветом текста кнопки.
constexpr wchar_t kGlyphPlay[] = L"▶";
constexpr wchar_t kGlyphStop[] = L"✖";
constexpr wchar_t kGlyphSettings[] = L"⚙";
constexpr wchar_t kGlyphShow[] = L"\U0001F441";
constexpr wchar_t kGlyphHide[] = L"\U0001F648";
constexpr wchar_t kGlyphDownload[] = L"⬇";
constexpr wchar_t kGlyphMore[] = L"•••";  // «⋯» в Segoe UI Emoji слишком мелкий
constexpr wchar_t kGlyphFile[] = L"\U0001F4C2";
constexpr wchar_t kGlyphFolder[] = L"\U0001F4C1";

// Что под курсором. Список собирается прямо во время отрисовки.
enum ElemId {
    kNone,
    kListItem,
    kHubItem,
    kRun,
    kStop,
    kWidgetSettings,
    kToggleVisible,
    kDownload,
    kPickFile,
    kMore,
    kAutostart,
    kThemeDark,
    kThemeLight,
    kAccent,
    kHubAutostart,
    kCloseToTray,
    kOpenReleases,
    kOpenInstall,
    kScrollThumb,
};

struct Elem {
    RectF r;
    int id = kNone;
    int arg = 0;
    bool enabled = true;
};

// Команды меню значка в трее: 1000 + индекс виджета * 16 + действие.
constexpr UINT kTrayCmdBase = 1000;
enum TrayAction { kTaShow = 1, kTaHide, kTaSettings, kTaRun, kTaStop, kTaOpenPage };
constexpr UINT kCmdOpenHub = 900;
constexpr UINT kCmdExit = 901;
// Меню «⋯» на странице виджета.
constexpr UINT kCmdUpdate = 910;
constexpr UINT kCmdReveal = 911;
constexpr UINT kCmdPick = 912;
constexpr UINT kCmdReleases = 913;

struct Fonts {
    std::unique_ptr<FontFamily> family;
    std::unique_ptr<FontFamily> glyphs;
    std::unique_ptr<Font> appTitle, title, name, body, small, bold, glyph, glyphSmall, section;

    void Build(int dpi) {
        family.reset(new FontFamily(L"Segoe UI"));
        if (!family->IsAvailable()) family.reset(FontFamily::GenericSansSerif()->Clone());
        glyphs.reset(new FontFamily(L"Segoe UI Emoji"));
        if (!glyphs->IsAvailable()) glyphs.reset(new FontFamily(L"Segoe UI Symbol"));
        auto make = [&](FontFamily* f, float size, int style) {
            return std::unique_ptr<Font>(new Font(f, ScF(size, dpi), style, UnitPixel));
        };
        appTitle = make(family.get(), 15.0f, FontStyleBold);
        title = make(family.get(), 23.0f, FontStyleBold);
        name = make(family.get(), 13.5f, FontStyleBold);
        body = make(family.get(), 13.0f, FontStyleRegular);
        small = make(family.get(), 12.0f, FontStyleRegular);
        bold = make(family.get(), 13.0f, FontStyleBold);
        section = make(family.get(), 11.0f, FontStyleBold);
        glyph = make(glyphs.get(), 14.0f, FontStyleRegular);
        glyphSmall = make(glyphs.get(), 12.0f, FontStyleRegular);
    }
};

class Hub {
public:
    bool Create(HINSTANCE inst, bool startHidden);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // Состояние виджетов
    void Poll();
    void PollSoon() { SetTimer(hwnd_, kTimerPollSoon, 700, nullptr); }
    bool Installed(int i) const { return FileExists(paths_[static_cast<size_t>(i)]); }
    std::wstring StatusText(int i) const;
    ARGB StatusColor(int i) const;

    // Действия
    void Select(int index);
    void OnClick(const Elem& e);
    void RunWidget(int i);
    void StartDownload(int i);
    void PickFile(int i);
    void ToggleAutostart(int i);
    void ShowMoreMenu(int i, POINT screenPt);
    void ShowTrayMenu(POINT screenPt);
    void RunTrayCommand(UINT cmd);
    void ApplyTheme();
    void ShowHub();
    void HideHub();
    void Exit();
    void SavePlacement();

    // Отрисовка
    void Paint();
    void DrawSidebar(Graphics& g, float h);
    void DrawWidgetPage(Graphics& g, float x0, float w, float h);
    void DrawSettingsPage(Graphics& g, float x0, float w, float h);
    float DrawButton(Graphics& g, float x, float y, const wchar_t* glyph, const wchar_t* label, bool primary,
                     bool enabled, int id, int arg = 0);
    void DrawSwitch(Graphics& g, float x, float y, bool on, int id, int arg = 0);
    void DrawGlyphButton(Graphics& g, const RectF& r, const wchar_t* glyph, int id, int arg = 0);
    void AddElem(const RectF& r, int id, int arg = 0, bool enabled = true);
    // Верх эмодзи, чтобы он стоял по центру полосы высотой height. У Segoe UI Emoji строка
    // выше, чем у Segoe UI, и по обычной формуле значок проваливается ниже текста.
    float GlyphTop(float top, float height) const {
        const float size = fonts_.glyph->GetSize();
        return top + (height - size) / 2.0f - size * 0.24f;
    }
    bool IsHover(int id, int arg = 0) const { return hover_.id == id && hover_.arg == arg; }
    const Elem* HitTest(POINT pt) const;
    std::wstring DocLinkAt(POINT pt) const;
    void ClampScroll();
    void LayoutEdit(bool show, const RectF& field);
    void RebuildEditFont();

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HICON iconBig_ = nullptr;
    HICON iconSmall_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    UINT taskbarCreated_ = 0;
    int dpi_ = 96;
    Fonts fonts_;
    HubData data_;

    std::vector<control::Status> status_;
    std::vector<std::wstring> paths_;
    std::vector<std::wstring> notes_;  // итог последней операции: «Скачано…», ошибка
    std::vector<bool> noteIsError_;
    std::vector<bool> autostart_;
    std::vector<HWND> trayHidden_;  // каким окнам виджетов уже сказали спрятать значок
    bool hubAutostart_ = false;

    int selected_ = 0;  // -1 — настройки центра
    md::Document doc_;
    float scroll_ = 0.0f;
    float docTop_ = 0.0f, docBottom_ = 0.0f, docX_ = 0.0f;
    float docW_ = 0.0f;
    RectF thumb_;
    bool thumbDrag_ = false;
    float thumbGrabY_ = 0.0f, thumbGrabScroll_ = 0.0f;

    std::vector<Elem> elems_;
    Elem hover_;
    Elem pressed_;
    bool tracking_ = false;
    bool trayAdded_ = false;
    bool editShown_ = false;
    RECT editRect_{};
};

Hub* g_hub = nullptr;

// ---------- Состояние виджетов ----------

void Hub::Poll() {
    const auto& entries = catalog::Entries();
    bool changed = false;
    for (size_t i = 0; i < entries.size(); i++) {
        const catalog::Entry& e = entries[i];
        const control::Status st = control::Probe(e);
        const control::Status& old = status_[i];
        if (st.running != old.running || st.visible != old.visible || st.linked != old.linked ||
            st.window != old.window) {
            changed = true;
        }
        status_[i] = st;

        // Виджет, запущенный мимо центра (автозапуском, двойным кликом), тоже прячет значок.
        if (st.running && st.linked && trayHidden_[i] != st.window) {
            control::Post(st, hub::kTrayOff);
            trayHidden_[i] = st.window;
        }
        if (!st.running) trayHidden_[i] = nullptr;

        // Путь запущенного процесса запоминаем: когда виджет закроют, центр всё ещё знает, где он.
        if (!st.processPath.empty() && !EqualsNoCase(data_.PathOf(e.id), st.processPath)) {
            data_.SetPath(e.id, st.processPath);
            SaveData(data_);
        }
        const std::wstring path = control::ResolvePath(e, data_.PathOf(e.id), st);
        if (path != paths_[i]) {
            paths_[i] = path;
            changed = true;
        }
        const bool auto_ = !path.empty() && EqualsNoCase(RunEntry(e.runValue), path);
        if (auto_ != autostart_[i]) {
            autostart_[i] = auto_;
            changed = true;
        }
    }
    const bool hubAuto = EqualsNoCase(RunEntry(kHubRunValue), ExePath());
    if (hubAuto != hubAutostart_) {
        hubAutostart_ = hubAuto;
        changed = true;
    }
    if (changed && IsWindowVisible(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring Hub::StatusText(int i) const {
    const size_t k = static_cast<size_t>(i);
    if (download::Busy(i)) {
        const int p = download::Percent(i);
        if (p < 0) return L"Скачивание…";
        wchar_t buf[48];
        swprintf(buf, 48, L"Скачивание… %d%%", p);
        return buf;
    }
    if (status_[k].running) {
        if (!status_[k].linked) return L"Работает · старая сборка";
        return status_[k].visible ? L"Работает" : L"Работает · скрыт";
    }
    return Installed(i) ? L"Не запущен" : L"Не скачан";
}

ARGB Hub::StatusColor(int i) const {
    const size_t k = static_cast<size_t>(i);
    if (download::Busy(i)) return P().accent;
    if (status_[k].running) return status_[k].linked ? P().ok : 0xFFE0A030;
    return Installed(i) ? P().textMuted : P().textFaint;
}

// ---------- Действия ----------

void Hub::Select(int index) {
    const auto& entries = catalog::Entries();
    if (index >= static_cast<int>(entries.size())) index = static_cast<int>(entries.size()) - 1;
    selected_ = index;
    scroll_ = 0.0f;
    doc_.SetSource(index >= 0 ? &entries[static_cast<size_t>(index)] : nullptr);
    data_.selected = index >= 0 ? entries[static_cast<size_t>(index)].id : std::string();
    SaveData(data_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Hub::RunWidget(int i) {
    const size_t k = static_cast<size_t>(i);
    std::wstring error;
    if (control::Launch(paths_[k], error)) {
        notes_[k].clear();
    } else {
        notes_[k] = error;
        noteIsError_[k] = true;
    }
    PollSoon();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Hub::StartDownload(int i) {
    const size_t k = static_cast<size_t>(i);
    const catalog::Entry& e = catalog::Entries()[k];
    if (data_.repo.empty()) {
        notes_[k] = L"Сначала укажите репозиторий GitHub в настройках центра.";
        noteIsError_[k] = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // Обновляем там, где виджет уже лежит; новый кладём в папку центра.
    const std::wstring dest = Installed(i) ? paths_[k] : InstallDir() + L"\\" + e.exe;
    notes_[k].clear();
    if (!download::Start(hwnd_, i, e, download::ReleaseUrl(data_.repo, e.exe), dest)) {
        notes_[k] = download::TakeResult(i);
        noteIsError_[k] = true;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Hub::PickFile(int i) {
    const size_t k = static_cast<size_t>(i);
    const catalog::Entry& e = catalog::Entries()[k];
    std::wstring path;
    if (!PickExe(hwnd_, L"Где лежит " + e.exe, paths_[k], path)) return;
    data_.SetPath(e.id, path);
    SaveData(data_);
    if (!EqualsNoCase(FileName(path), e.exe)) {
        notes_[k] = L"Имя файла не совпадает с " + e.exe + L" — проверьте, тот ли это виджет.";
        noteIsError_[k] = true;
    } else {
        notes_[k].clear();
    }
    Poll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Hub::ToggleAutostart(int i) {
    const size_t k = static_cast<size_t>(i);
    const catalog::Entry& e = catalog::Entries()[k];
    if (!Installed(i)) return;
    if (!SetRunEntry(e.runValue, paths_[k], L"", !autostart_[k])) {
        notes_[k] = L"Не удалось изменить автозапуск.";
        noteIsError_[k] = true;
    }
    Poll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Hub::ShowMoreMenu(int i, POINT pt) {
    const size_t k = static_cast<size_t>(i);
    const bool installed = Installed(i);
    const bool repo = !data_.repo.empty();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (installed && repo ? 0 : MF_GRAYED), kCmdUpdate, L"Обновить из GitHub");
    AppendMenuW(menu, MF_STRING | (installed ? 0 : MF_GRAYED), kCmdReveal, L"Показать файл в проводнике");
    AppendMenuW(menu, MF_STRING, kCmdPick, L"Указать файл вручную…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (repo ? 0 : MF_GRAYED), kCmdReleases, L"Страница релизов на GitHub");
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    switch (cmd) {
        case kCmdUpdate: StartDownload(i); break;
        case kCmdReveal: RevealInExplorer(paths_[k]); break;
        case kCmdPick: PickFile(i); break;
        case kCmdReleases: OpenUrl(L"https://github.com/" + data_.repo + L"/releases"); break;
        default: break;
    }
}

void Hub::ShowTrayMenu(POINT pt) {
    const auto& entries = catalog::Entries();
    HMENU menu = CreatePopupMenu();
    for (size_t k = 0; k < entries.size(); k++) {
        const int i = static_cast<int>(k);
        const UINT base = kTrayCmdBase + static_cast<UINT>(k) * 16;
        const control::Status& st = status_[k];
        if (!Installed(i) && !st.running) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (entries[k].name + L" — не скачан").c_str());
            continue;
        }
        HMENU sub = CreatePopupMenu();
        if (st.running && st.linked) {
            AppendMenuW(sub, MF_STRING, base + (st.visible ? kTaHide : kTaShow),
                        st.visible ? L"Скрыть блоки" : L"Показать блоки");
            AppendMenuW(sub, MF_STRING, base + kTaSettings, L"Настройки…");
            AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(sub, MF_STRING, base + kTaStop, L"Закрыть");
        } else if (st.running) {
            AppendMenuW(sub, MF_STRING, base + kTaOpenPage, L"Старая сборка — открыть в центре");
        } else {
            AppendMenuW(sub, MF_STRING, base + kTaRun, L"Запустить");
        }
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, base + kTaOpenPage, L"Страница в центре");
        AppendMenuW(menu, MF_POPUP | (st.running ? MF_CHECKED : 0), reinterpret_cast<UINT_PTR>(sub),
                    entries[k].name.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kCmdOpenHub, L"Открыть центр виджетов");
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Выход");

    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    RunTrayCommand(cmd);
}

void Hub::RunTrayCommand(UINT cmd) {
    if (cmd == 0) return;
    if (cmd == kCmdOpenHub) {
        ShowHub();
        return;
    }
    if (cmd == kCmdExit) {
        Exit();
        return;
    }
    if (cmd < kTrayCmdBase) return;
    const int i = static_cast<int>((cmd - kTrayCmdBase) / 16);
    const int action = static_cast<int>((cmd - kTrayCmdBase) % 16);
    if (i < 0 || i >= static_cast<int>(catalog::Entries().size())) return;
    const control::Status& st = status_[static_cast<size_t>(i)];
    switch (action) {
        case kTaShow: control::Post(st, hub::kShow); break;
        case kTaHide: control::Post(st, hub::kHide); break;
        case kTaSettings: control::Post(st, hub::kSettings); break;
        case kTaStop: control::Post(st, hub::kExit); break;
        case kTaRun: RunWidget(i); break;
        case kTaOpenPage:
            Select(i);
            ShowHub();
            break;
        default: break;
    }
    PollSoon();
}

void Hub::ApplyTheme() {
    SetTheme(data_.darkTheme, data_.accent);
    // Тёмный заголовок окна: атрибут 20 на Windows 10 2004+ и 11, 19 — на сборках постарше.
    const BOOL dark = data_.darkTheme ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));
    }
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(P().editBack);
    if (edit_) InvalidateRect(edit_, nullptr, TRUE);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Hub::ShowHub() {
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    else if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, data_.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    SetForegroundWindow(hwnd_);
    Poll();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Hub::HideHub() {
    SavePlacement();
    ShowWindow(hwnd_, SW_HIDE);
}

// Центр уходит — значки виджетов возвращаются в трей, иначе ими было бы не управлять.
void Hub::Exit() {
    for (const control::Status& st : status_) {
        if (st.running && st.linked) control::Post(st, hub::kTrayOn);
    }
    DestroyWindow(hwnd_);
}

void Hub::SavePlacement() {
    WINDOWPLACEMENT wp = {sizeof(wp)};
    if (!GetWindowPlacement(hwnd_, &wp)) return;
    const RECT& r = wp.rcNormalPosition;
    data_.winX = r.left;
    data_.winY = r.top;
    data_.winW = MulDiv(r.right - r.left, 96, dpi_);
    data_.winH = MulDiv(r.bottom - r.top, 96, dpi_);
    data_.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (IsZoomed(hwnd_) != FALSE);
    SaveData(data_);
}

void Hub::OnClick(const Elem& e) {
    if (!e.enabled) return;
    const int i = selected_;
    const size_t k = static_cast<size_t>(i < 0 ? 0 : i);
    switch (e.id) {
        case kListItem: Select(e.arg); break;
        case kHubItem: Select(-1); break;
        case kRun: RunWidget(i); break;
        case kStop:
            control::Post(status_[k], hub::kExit);
            PollSoon();
            break;
        case kWidgetSettings: control::Post(status_[k], hub::kSettings); break;
        case kToggleVisible:
            control::Post(status_[k], hub::kToggle);
            PollSoon();
            break;
        case kDownload: StartDownload(i); break;
        case kPickFile: PickFile(i); break;
        case kMore: {
            POINT pt = {static_cast<LONG>(e.r.X), static_cast<LONG>(e.r.GetBottom() + 4)};
            ClientToScreen(hwnd_, &pt);
            ShowMoreMenu(i, pt);
            break;
        }
        case kAutostart: ToggleAutostart(i); break;
        case kThemeDark:
        case kThemeLight:
            data_.darkTheme = e.id == kThemeDark;
            SaveData(data_);
            ApplyTheme();
            break;
        case kAccent:
            data_.accent = e.arg;
            SaveData(data_);
            ApplyTheme();
            break;
        case kHubAutostart:
            SetRunEntry(kHubRunValue, ExePath(), L"--tray", !hubAutostart_);
            Poll();
            break;
        case kCloseToTray:
            data_.closeToTray = !data_.closeToTray;
            SaveData(data_);
            break;
        case kOpenReleases: OpenUrl(L"https://github.com/" + data_.repo + L"/releases"); break;
        case kOpenInstall: OpenFolder(InstallDir()); break;
        default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------- Отрисовка ----------

void Hub::AddElem(const RectF& r, int id, int arg, bool enabled) {
    Elem e;
    e.r = r;
    e.id = id;
    e.arg = arg;
    e.enabled = enabled;
    elems_.push_back(e);
}

const Elem* Hub::HitTest(POINT pt) const {
    // С конца: то, что нарисовано позже, лежит сверху.
    for (size_t k = elems_.size(); k > 0; k--) {
        const Elem& e = elems_[k - 1];
        if (e.r.Contains(static_cast<REAL>(pt.x), static_cast<REAL>(pt.y))) return &e;
    }
    return nullptr;
}

std::wstring Hub::DocLinkAt(POINT pt) const {
    if (selected_ < 0) return std::wstring();
    const float y = static_cast<float>(pt.y);
    if (y < docTop_ || y > docBottom_) return std::wstring();
    return doc_.LinkAt(static_cast<float>(pt.x) - docX_, y - (docTop_ + ScF(20.0f, dpi_) - scroll_));
}

float Hub::DrawButton(Graphics& g, float x, float y, const wchar_t* glyph, const wchar_t* label, bool primary,
                      bool enabled, int id, int arg) {
    const Palette& pal = P();
    const float h = ScF(36.0f, dpi_);
    const float padX = ScF(15.0f, dpi_);
    const float glyphW = glyph ? ScF(16.0f, dpi_) + ScF(8.0f, dpi_) : 0.0f;
    const float textW = gfx::TextWidth(g, label, *fonts_.bold);
    const float w = padX * 2.0f + glyphW + textW;
    const RectF r(x, y, w, h);
    const bool hover = enabled && IsHover(id, arg);

    ARGB bg, fg;
    if (primary) {
        bg = hover ? Mix(pal.accent, 0xFFFFFFFF, pal.dark ? 0.12f : 0.10f) : pal.accent;
        fg = pal.onAccent;
    } else {
        bg = hover ? pal.buttonBgHover : pal.buttonBg;
        fg = pal.textPrimary;
    }
    if (!enabled) {
        bg = pal.buttonBg;
        fg = pal.textFaint;
    }
    gfx::FillRoundRect(g, r, ScF(9.0f, dpi_), bg);
    float tx = x + padX;
    const float ty = y + (h - fonts_.bold->GetSize() * 1.33f) / 2.0f;
    if (glyph) {
        const float gy = GlyphTop(y, h);
        gfx::DrawText(g, glyph, *fonts_.glyph, fg, tx, gy);
        tx += glyphW;
    }
    gfx::DrawText(g, label, *fonts_.bold, fg, tx, ty);
    AddElem(r, id, arg, enabled);
    return w;
}

void Hub::DrawGlyphButton(Graphics& g, const RectF& r, const wchar_t* glyph, int id, int arg) {
    const Palette& pal = P();
    gfx::FillRoundRect(g, r, ScF(9.0f, dpi_), IsHover(id, arg) ? pal.buttonBgHover : pal.buttonBg);
    const Font& font = glyph == kGlyphMore ? *fonts_.small : *fonts_.glyph;
    const float top = glyph == kGlyphMore ? r.Y + (r.Height - font.GetSize() * 1.33f) / 2.0f - ScF(1.0f, dpi_)
                                          : GlyphTop(r.Y, r.Height);
    gfx::DrawText(g, glyph, font, pal.textPrimary, r.X + r.Width / 2.0f, top, gfx::Align::Center);
    AddElem(r, id, arg);
}

void Hub::DrawSwitch(Graphics& g, float x, float y, bool on, int id, int arg) {
    const Palette& pal = P();
    const float w = ScF(40.0f, dpi_), h = ScF(22.0f, dpi_);
    const RectF r(x, y, w, h);
    ARGB track = on ? pal.accent : (pal.dark ? 0x33FFFFFF : 0x26000000);
    if (IsHover(id, arg)) track = Mix(track, pal.dark ? 0xFFFFFFFF : 0xFF000000, 0.08f);
    gfx::FillRoundRect(g, r, h / 2.0f, track);
    const float d = h - ScF(6.0f, dpi_);
    const float kx = on ? x + w - d - ScF(3.0f, dpi_) : x + ScF(3.0f, dpi_);
    const Color knob(on ? pal.onAccent : (pal.dark ? 0xFFE6E8EE : 0xFFFFFFFF));
    SolidBrush brush(knob);
    g.FillEllipse(&brush, kx, y + ScF(3.0f, dpi_), d, d);
    AddElem(RectF(x - 2.0f, y - 4.0f, w + 4.0f, h + 8.0f), id, arg);
}

void Hub::DrawSidebar(Graphics& g, float h) {
    const Palette& pal = P();
    const float w = ScF(264.0f, dpi_);
    const Color sideColor(pal.dark ? 0xFF13161C : 0xFFECEEF3);
    SolidBrush side(sideColor);
    g.FillRectangle(&side, 0.0f, 0.0f, w, h);
    {
        Pen edge(Color(pal.separator), 1.0f);
        g.DrawLine(&edge, w - 0.5f, 0.0f, w - 0.5f, h);
    }

    const auto& entries = catalog::Entries();
    int running = 0;
    for (const control::Status& st : status_) running += st.running ? 1 : 0;

    const float pad = ScF(20.0f, dpi_);
    gfx::DrawText(g, L"Центр виджетов", *fonts_.appTitle, pal.textPrimary, pad, ScF(20.0f, dpi_));
    wchar_t sub[64];
    swprintf(sub, 64, L"%d в каталоге · %d запущено", static_cast<int>(entries.size()), running);
    gfx::DrawText(g, sub, *fonts_.small, pal.textMuted, pad, ScF(44.0f, dpi_));

    float y = ScF(78.0f, dpi_);
    const float itemH = ScF(60.0f, dpi_);
    const float mx = ScF(10.0f, dpi_);
    for (size_t k = 0; k < entries.size(); k++) {
        const int i = static_cast<int>(k);
        const RectF r(mx, y, w - mx * 2.0f, itemH);
        const bool sel = selected_ == i;
        if (sel) {
            gfx::FillRoundRect(g, r, ScF(10.0f, dpi_), WithAlpha(pal.accent, pal.dark ? 0x26 : 0x1F));
            gfx::FillRoundRect(g, RectF(r.X, r.Y + ScF(16.0f, dpi_), ScF(3.0f, dpi_), r.Height - ScF(32.0f, dpi_)),
                               ScF(1.5f, dpi_), pal.accent);
        } else if (IsHover(kListItem, i)) {
            gfx::FillRoundRect(g, r, ScF(10.0f, dpi_), pal.hover);
        }

        const float iconSize = ScF(36.0f, dpi_);
        const float ix = r.X + ScF(12.0f, dpi_);
        const float iy = r.Y + (itemH - iconSize) / 2.0f;
        if (Bitmap* icon = catalog::Icon(entries[k])) {
            g.DrawImage(icon, RectF(ix, iy, iconSize, iconSize));
        }

        const float tx = ix + iconSize + ScF(12.0f, dpi_);
        const float maxW = r.GetRight() - tx - ScF(8.0f, dpi_);
        gfx::DrawText(g, gfx::Ellipsize(g, entries[k].name, *fonts_.name, maxW).c_str(), *fonts_.name,
                      pal.textPrimary, tx, r.Y + ScF(11.0f, dpi_));
        const float dot = ScF(7.0f, dpi_);
        const float sy = r.Y + ScF(35.0f, dpi_);
        {
            const Color dc(StatusColor(i));
            SolidBrush db(dc);
            g.FillEllipse(&db, tx, sy + ScF(4.0f, dpi_), dot, dot);
        }
        gfx::DrawText(g, gfx::Ellipsize(g, StatusText(i), *fonts_.small, maxW - dot - 6.0f).c_str(), *fonts_.small,
                      pal.textMuted, tx + dot + ScF(6.0f, dpi_), sy);
        AddElem(r, kListItem, i);
        y += itemH + ScF(4.0f, dpi_);
    }

    // Настройки центра — внизу, отдельно от списка.
    const RectF sr(mx, h - ScF(58.0f, dpi_), w - mx * 2.0f, ScF(44.0f, dpi_));
    if (selected_ < 0) {
        gfx::FillRoundRect(g, sr, ScF(10.0f, dpi_), WithAlpha(pal.accent, pal.dark ? 0x26 : 0x1F));
    } else if (IsHover(kHubItem)) {
        gfx::FillRoundRect(g, sr, ScF(10.0f, dpi_), pal.hover);
    }
    gfx::DrawText(g, kGlyphSettings, *fonts_.glyph, selected_ < 0 ? pal.accent : pal.textMuted,
                  sr.X + ScF(14.0f, dpi_), GlyphTop(sr.Y, sr.Height));
    gfx::DrawText(g, L"Настройки центра", *fonts_.bold, pal.textPrimary, sr.X + ScF(40.0f, dpi_),
                  sr.Y + (sr.Height - fonts_.bold->GetSize() * 1.33f) / 2.0f);
    AddElem(sr, kHubItem);
}

void Hub::DrawWidgetPage(Graphics& g, float x0, float w, float h) {
    const Palette& pal = P();
    const size_t k = static_cast<size_t>(selected_);
    const int i = selected_;
    const catalog::Entry& e = catalog::Entries()[k];
    const control::Status& st = status_[k];
    const bool installed = Installed(i);
    const bool busy = download::Busy(i);

    const float margin = ScF(36.0f, dpi_);
    const float cw = min(w - margin * 2.0f, ScF(860.0f, dpi_));
    const float cx = x0 + margin;
    float y = ScF(30.0f, dpi_);

    // Шапка: иконка, название, статус, одна строка описания.
    const float iconSize = ScF(64.0f, dpi_);
    if (Bitmap* icon = catalog::Icon(e)) g.DrawImage(icon, RectF(cx, y, iconSize, iconSize));
    const float tx = cx + iconSize + ScF(18.0f, dpi_);
    gfx::DrawText(g, e.name.c_str(), *fonts_.title, pal.textPrimary, tx, y + ScF(2.0f, dpi_));
    {
        // Статус — плашкой справа от названия.
        const float nameW = gfx::TextWidth(g, e.name.c_str(), *fonts_.title);
        const std::wstring status = StatusText(i);
        const float sw = gfx::TextWidth(g, status.c_str(), *fonts_.small);
        const float dot = ScF(7.0f, dpi_);
        const RectF chip(tx + nameW + ScF(14.0f, dpi_), y + ScF(8.0f, dpi_),
                         sw + dot + ScF(24.0f, dpi_), ScF(24.0f, dpi_));
        gfx::FillRoundRect(g, chip, chip.Height / 2.0f, WithAlpha(StatusColor(i), pal.dark ? 0x22 : 0x1C));
        const Color dc(StatusColor(i));
        SolidBrush db(dc);
        g.FillEllipse(&db, chip.X + ScF(10.0f, dpi_), chip.Y + (chip.Height - dot) / 2.0f, dot, dot);
        gfx::DrawText(g, status.c_str(), *fonts_.small, pal.textPrimary, chip.X + ScF(10.0f, dpi_) + dot + ScF(6.0f, dpi_),
                      chip.Y + (chip.Height - fonts_.small->GetSize() * 1.33f) / 2.0f);
    }
    const std::wstring subtitle = e.title + L"  ·  " + e.summary;
    gfx::DrawText(g, gfx::Ellipsize(g, subtitle, *fonts_.body, cx + cw - tx).c_str(), *fonts_.body, pal.textMuted,
                  tx, y + ScF(40.0f, dpi_));
    y += iconSize + ScF(22.0f, dpi_);

    // Кнопки: набор зависит от того, скачан ли виджет и работает ли он.
    float bx = cx;
    const float gap = ScF(8.0f, dpi_);
    if (st.running) {
        const bool linked = st.linked;
        bx += DrawButton(g, bx, y, kGlyphSettings, L"Настройки", true, linked, kWidgetSettings) + gap;
        bx += DrawButton(g, bx, y, st.visible ? kGlyphHide : kGlyphShow, st.visible ? L"Скрыть" : L"Показать", false,
                         linked, kToggleVisible) + gap;
        bx += DrawButton(g, bx, y, kGlyphStop, L"Закрыть", false, linked, kStop) + gap;
    } else if (installed) {
        bx += DrawButton(g, bx, y, kGlyphPlay, L"Запустить", true, !busy, kRun) + gap;
    } else {
        bx += DrawButton(g, bx, y, kGlyphDownload, L"Скачать", true, !busy, kDownload) + gap;
        bx += DrawButton(g, bx, y, kGlyphFile, L"Указать файл", false, true, kPickFile) + gap;
    }
    const float btnH = ScF(36.0f, dpi_);
    if (installed || st.running) DrawGlyphButton(g, RectF(bx, y, btnH, btnH), kGlyphMore, kMore);

    // Справа в том же ряду — автозапуск с Windows.
    if (installed) {
        const float swW = ScF(40.0f, dpi_);
        const float sx = cx + cw - swW;
        const float sy = y + (btnH - ScF(22.0f, dpi_)) / 2.0f;
        const wchar_t* label = L"Запускать с Windows";
        const float lw = gfx::TextWidth(g, label, *fonts_.body);
        if (sx - lw - ScF(12.0f, dpi_) > bx + btnH + gap) {
            gfx::DrawText(g, label, *fonts_.body, pal.textMuted, sx - lw - ScF(10.0f, dpi_),
                          y + (btnH - fonts_.body->GetSize() * 1.33f) / 2.0f);
            DrawSwitch(g, sx, sy, autostart_[k], kAutostart);
        }
    }
    y += btnH + ScF(14.0f, dpi_);

    // Ход загрузки, итог последней операции и где лежит файл.
    if (busy) {
        const int p = download::Percent(i);
        const RectF track(cx, y, min(cw, ScF(360.0f, dpi_)), ScF(4.0f, dpi_));
        gfx::FillRoundRect(g, track, track.Height / 2.0f, pal.track);
        const float frac = p < 0 ? 0.3f : static_cast<float>(p) / 100.0f;
        gfx::FillRoundRect(g, RectF(track.X, track.Y, max(track.Height, track.Width * frac), track.Height),
                           track.Height / 2.0f, pal.accent);
        y += ScF(14.0f, dpi_);
    }
    std::wstring note = notes_[k];
    bool error = noteIsError_[k];
    if (note.empty() && st.running && !st.linked) {
        note = L"Запущена сборка без связи с центром — обновите виджет, чтобы управлять им отсюда.";
        error = true;
    }
    if (note.empty() && !installed && !st.running && data_.repo.empty()) {
        note = L"Скачивание берёт файлы из релизов GitHub — укажите репозиторий в настройках центра.";
        error = false;
    }
    if (!note.empty()) {
        gfx::DrawText(g, gfx::Ellipsize(g, note, *fonts_.body, cw).c_str(), *fonts_.body,
                      error ? pal.danger : pal.textMuted, cx, y);
        y += ScF(22.0f, dpi_);
    }
    if (installed) {
        gfx::DrawText(g, gfx::Ellipsize(g, paths_[k], *fonts_.small, cw).c_str(), *fonts_.small, pal.textFaint, cx, y);
        y += ScF(20.0f, dpi_);
    }
    y += ScF(6.0f, dpi_);

    // README — прокручиваемая часть страницы.
    {
        Pen sep(Color(pal.separator), 1.0f);
        g.DrawLine(&sep, x0, y, x0 + w, y);
    }
    docTop_ = y;
    docBottom_ = h;
    docX_ = cx;
    docW_ = cw;
    doc_.Layout(g, cw, dpi_);
    ClampScroll();

    const GraphicsState state = g.Save();
    g.SetClip(RectF(x0, docTop_ + 1.0f, w, docBottom_ - docTop_ - 1.0f));
    const float docY = docTop_ + ScF(20.0f, dpi_) - scroll_;
    doc_.Draw(g, cx, docY, docTop_, docBottom_);
    if (doc_.Height() <= 0.0f) {
        gfx::DrawText(g, L"Описание для этого виджета не упаковано.", *fonts_.body, pal.textMuted, cx,
                      docTop_ + ScF(20.0f, dpi_));
    }
    g.Restore(state);

    // Полоса прокрутки: тонкая, у правого края окна.
    const float viewH = docBottom_ - docTop_;
    const float contentH = doc_.Height() + ScF(40.0f, dpi_);
    thumb_ = RectF();
    if (contentH > viewH) {
        const float trackX = x0 + w - ScF(10.0f, dpi_);
        const float thumbH = max(ScF(36.0f, dpi_), viewH * viewH / contentH);
        const float maxScroll = contentH - viewH;
        const float ty = docTop_ + (viewH - thumbH) * (scroll_ / maxScroll);
        thumb_ = RectF(trackX, ty + 2.0f, ScF(6.0f, dpi_), thumbH - 4.0f);
        const bool hot = thumbDrag_ || IsHover(kScrollThumb);
        gfx::FillRoundRect(g, thumb_, thumb_.Width / 2.0f,
                           hot ? (pal.dark ? 0x66FFFFFF : 0x55000000) : (pal.dark ? 0x33FFFFFF : 0x2A000000));
        AddElem(RectF(trackX - 4.0f, thumb_.Y, thumb_.Width + 8.0f, thumb_.Height), kScrollThumb);
    }
}

void Hub::DrawSettingsPage(Graphics& g, float x0, float w, float h) {
    const Palette& pal = P();
    const float margin = ScF(36.0f, dpi_);
    const float cw = min(w - margin * 2.0f, ScF(720.0f, dpi_));
    const float cx = x0 + margin;
    float y = ScF(30.0f, dpi_);
    docTop_ = docBottom_ = 0.0f;
    thumb_ = RectF();

    gfx::DrawText(g, L"Настройки центра", *fonts_.title, pal.textPrimary, cx, y);
    y += ScF(40.0f, dpi_);
    gfx::DrawText(g, L"Оформление, откуда скачивать виджеты и как ведёт себя центр.", *fonts_.body, pal.textMuted, cx,
                  y);
    y += ScF(40.0f, dpi_);

    const float rowH = ScF(48.0f, dpi_);
    const float labelX = cx + ScF(18.0f, dpi_);
    const float rightX = cx + cw - ScF(18.0f, dpi_);
    auto section = [&](const wchar_t* title, int rows, float extra) {
        gfx::DrawTracked(g, title, *fonts_.section, pal.textMuted, cx + ScF(4.0f, dpi_), y, ScF(1.2f, dpi_));
        y += ScF(22.0f, dpi_);
        const RectF card(cx, y, cw, rowH * static_cast<float>(rows) + extra);
        gfx::FillRoundRect(g, card, ScF(12.0f, dpi_), pal.dark ? 0x0AFFFFFF : 0xFFFFFFFF);
        gfx::StrokeRoundRect(g, card, ScF(12.0f, dpi_), pal.separator, 1.0f);
        return card;
    };
    auto rowLabel = [&](const wchar_t* text, float rowY) {
        gfx::DrawText(g, text, *fonts_.body, pal.textPrimary, labelX, rowY + (rowH - fonts_.body->GetSize() * 1.33f) / 2.0f);
    };
    auto rowSep = [&](float rowY) {
        Pen sep(Color(pal.separator), 1.0f);
        g.DrawLine(&sep, labelX, rowY, rightX, rowY);
    };

    // --- Оформление ---
    RectF card = section(L"ОФОРМЛЕНИЕ", 2, 0.0f);
    float ry = card.Y;
    rowLabel(L"Тема", ry);
    {
        const float segW = ScF(96.0f, dpi_), segH = ScF(30.0f, dpi_);
        const float sx = rightX - segW * 2.0f;
        const float sy = ry + (rowH - segH) / 2.0f;
        gfx::FillRoundRect(g, RectF(sx, sy, segW * 2.0f, segH), ScF(8.0f, dpi_), pal.buttonBg);
        const bool darkSel = data_.darkTheme;
        const RectF a(sx + 2.0f, sy + 2.0f, segW - 4.0f, segH - 4.0f);
        const RectF b(sx + segW + 2.0f, sy + 2.0f, segW - 4.0f, segH - 4.0f);
        gfx::FillRoundRect(g, darkSel ? a : b, ScF(6.0f, dpi_), pal.accent);
        const float ty = sy + (segH - fonts_.body->GetSize() * 1.33f) / 2.0f;
        gfx::DrawText(g, L"Тёмная", *fonts_.body, darkSel ? pal.onAccent : pal.textPrimary, a.X + a.Width / 2.0f, ty,
                      gfx::Align::Center);
        gfx::DrawText(g, L"Светлая", *fonts_.body, darkSel ? pal.textPrimary : pal.onAccent, b.X + b.Width / 2.0f, ty,
                      gfx::Align::Center);
        AddElem(a, kThemeDark);
        AddElem(b, kThemeLight);
    }
    ry += rowH;
    rowSep(ry);
    rowLabel(L"Акцент", ry);
    {
        const float d = ScF(22.0f, dpi_), step = ScF(32.0f, dpi_);
        const int n = AccentCount();
        float ax = rightX - step * static_cast<float>(n) + (step - d);
        // Цвета акцентов берём из палитры, временно переключая её, — так кружки совпадают с темой.
        for (int a = 0; a < n; a++) {
            SetTheme(data_.darkTheme, a);
            const ARGB color = P().accent;
            const RectF dot(ax, ry + (rowH - d) / 2.0f, d, d);
            const Color c(color);
            SolidBrush brush(c);
            g.FillEllipse(&brush, dot);
            if (a == data_.accent || IsHover(kAccent, a)) {
                Pen ring(Color(a == data_.accent ? pal.textPrimary : pal.textMuted), ScF(2.0f, dpi_));
                g.DrawEllipse(&ring, dot.X - ScF(3.5f, dpi_), dot.Y - ScF(3.5f, dpi_), d + ScF(7.0f, dpi_),
                              d + ScF(7.0f, dpi_));
            }
            AddElem(RectF(dot.X - 4.0f, dot.Y - 4.0f, d + 8.0f, d + 8.0f), kAccent, a);
            ax += step;
        }
        SetTheme(data_.darkTheme, data_.accent);
    }
    y = card.GetBottom() + ScF(26.0f, dpi_);

    // --- Где брать виджеты ---
    const float fieldH = ScF(34.0f, dpi_);
    card = section(L"СКАЧИВАНИЕ", 1, fieldH + ScF(70.0f, dpi_));
    ry = card.Y + ScF(14.0f, dpi_);
    gfx::DrawText(g, L"Репозиторий GitHub", *fonts_.body, pal.textPrimary, labelX, ry);
    ry += ScF(26.0f, dpi_);
    const RectF field(labelX, ry, rightX - labelX, fieldH);
    gfx::FillRoundRect(g, field, ScF(8.0f, dpi_), pal.fieldBg);
    gfx::StrokeRoundRect(g, field, ScF(8.0f, dpi_), GetFocus() == edit_ ? pal.accent : pal.fieldBorder, 1.0f);
    LayoutEdit(true, field);
    ry += fieldH + ScF(10.0f, dpi_);
    if (!data_.repo.empty()) {
        const std::wstring hint = L"Виджеты берутся из последнего релиза: github.com/" + data_.repo + L"/releases";
        gfx::DrawText(g, gfx::Ellipsize(g, hint, *fonts_.small, field.Width - ScF(150.0f, dpi_)).c_str(), *fonts_.small,
                      pal.textMuted, labelX, ry);
        const wchar_t* open = L"Открыть релизы";
        const float ow = gfx::TextWidth(g, open, *fonts_.small);
        const RectF link(rightX - ow, ry - 2.0f, ow, ScF(18.0f, dpi_));
        gfx::DrawText(g, open, *fonts_.small, IsHover(kOpenReleases) ? pal.textPrimary : pal.accent, link.X, ry);
        AddElem(link, kOpenReleases);
    } else {
        gfx::DrawText(g, L"Формат: владелец/репозиторий. В релиз выкладываются .exe виджетов с их обычными именами.",
                      *fonts_.small, pal.textMuted, labelX, ry);
    }
    ry = card.GetBottom() - rowH;
    rowSep(ry);
    rowLabel(L"Папка для скачанных", ry);
    {
        const float bw = ScF(34.0f, dpi_);
        const RectF btn(rightX - bw, ry + (rowH - bw) / 2.0f, bw, bw);
        DrawGlyphButton(g, btn, kGlyphFolder, kOpenInstall);
        const float lx = labelX + ScF(170.0f, dpi_);
        gfx::DrawText(g, gfx::Ellipsize(g, InstallDir(), *fonts_.small, btn.X - lx - ScF(10.0f, dpi_)).c_str(),
                      *fonts_.small, pal.textFaint, lx, ry + (rowH - fonts_.small->GetSize() * 1.33f) / 2.0f);
    }
    y = card.GetBottom() + ScF(26.0f, dpi_);

    // --- Поведение ---
    card = section(L"ПОВЕДЕНИЕ", 2, 0.0f);
    ry = card.Y;
    const float swY = (rowH - ScF(22.0f, dpi_)) / 2.0f;
    rowLabel(L"Запускать центр вместе с Windows", ry);
    DrawSwitch(g, rightX - ScF(40.0f, dpi_), ry + swY, hubAutostart_, kHubAutostart);
    ry += rowH;
    rowSep(ry);
    rowLabel(L"Крестик прячет центр в трей, а не закрывает", ry);
    DrawSwitch(g, rightX - ScF(40.0f, dpi_), ry + swY, data_.closeToTray, kCloseToTray);
    y = card.GetBottom() + ScF(16.0f, dpi_);

    gfx::DrawText(g, L"Пока центр работает, значки виджетов в трее спрятаны: всё управление — здесь и в меню",
                  *fonts_.small, pal.textMuted, cx + ScF(4.0f, dpi_), y);
    gfx::DrawText(g, L"значка центра. При выходе из центра значки возвращаются.", *fonts_.small, pal.textMuted,
                  cx + ScF(4.0f, dpi_), y + ScF(18.0f, dpi_));
}

void Hub::RebuildEditFont() {
    if (editFont_) DeleteObject(editFont_);
    editFont_ = CreateFontW(-Sc(14, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (edit_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
}

// Поле ввода — настоящий EDIT поверх нарисованной рамки: курсор, выделение и вставка
// работают как везде в Windows.
void Hub::LayoutEdit(bool show, const RectF& field) {
    if (!show) {
        if (editShown_) {
            ShowWindow(edit_, SW_HIDE);
            editShown_ = false;
        }
        return;
    }
    const int padX = Sc(10, dpi_);
    const int textH = Sc(20, dpi_);
    RECT r;
    r.left = static_cast<LONG>(field.X) + padX;
    r.right = static_cast<LONG>(field.GetRight()) - padX;
    r.top = static_cast<LONG>(field.Y + (field.Height - static_cast<float>(textH)) / 2.0f);
    r.bottom = r.top + textH;
    if (!EqualRect(&r, &editRect_)) {
        editRect_ = r;
        SetWindowPos(edit_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!editShown_) {
        ShowWindow(edit_, SW_SHOWNA);
        editShown_ = true;
    }
}

void Hub::ClampScroll() {
    const float viewH = docBottom_ - docTop_;
    const float maxScroll = max(0.0f, doc_.Height() + ScF(40.0f, dpi_) - viewH);
    if (scroll_ > maxScroll) scroll_ = maxScroll;
    if (scroll_ < 0.0f) scroll_ = 0.0f;
}

void Hub::Paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        EndPaint(hwnd_, &ps);
        return;
    }

    // Рисуем в память и выкладываем одним BitBlt — без мерцания при прокрутке.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        const Color bgColor(P().dialogBg);
        g.Clear(bgColor);

        elems_.clear();
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        DrawSidebar(g, fh);
        const float x0 = ScF(264.0f, dpi_);
        if (selected_ >= 0 && selected_ < static_cast<int>(catalog::Entries().size())) {
            LayoutEdit(false, RectF());
            DrawWidgetPage(g, x0, fw - x0, fh);
        } else {
            DrawSettingsPage(g, x0, fw - x0, fh);
        }
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

// ---------- Оконная процедура ----------

LRESULT Hub::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == HubShowMessage()) {
        ShowHub();
        return 0;
    }
    if (taskbarCreated_ && msg == taskbarCreated_) {
        // Проводник перезапустился — значок надо поставить заново.
        trayAdded_ = TrayAdd(hwnd_, kTrayMessage, iconSmall_, L"Центр виджетов");
        return 0;
    }

    switch (msg) {
        case WM_PAINT:
            Paint();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = Sc(760, dpi_);
            mmi->ptMinTrackSize.y = Sc(500, dpi_);
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            fonts_.Build(dpi_);
            RebuildEditFont();
            editRect_ = RECT{};
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_TIMER:
            if (wp == kTimerPollSoon) KillTimer(hwnd, kTimerPollSoon);
            Poll();
            return 0;

        case WM_MOUSEMOVE: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!tracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            if (thumbDrag_) {
                const float viewH = docBottom_ - docTop_;
                const float contentH = doc_.Height() + ScF(40.0f, dpi_);
                const float travel = viewH - thumb_.Height - 4.0f;
                if (travel > 0.0f) {
                    scroll_ = thumbGrabScroll_ + (static_cast<float>(pt.y) - thumbGrabY_) * (contentH - viewH) / travel;
                    ClampScroll();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            const Elem* hit = HitTest(pt);
            Elem now = hit ? *hit : Elem();
            if (now.id != hover_.id || now.arg != hover_.arg) {
                hover_ = now;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hover_.id != kNone) {
                hover_ = Elem();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                const Elem* hit = HitTest(pt);
                const bool hand = (hit && hit->enabled && hit->id != kScrollThumb) || !DocLinkAt(pt).empty();
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (GetFocus() == edit_) SetFocus(hwnd);  // клик мимо поля завершает ввод
            const Elem* hit = HitTest(pt);
            pressed_ = hit ? *hit : Elem();
            if (pressed_.id == kScrollThumb) {
                thumbDrag_ = true;
                thumbGrabY_ = static_cast<float>(pt.y);
                thumbGrabScroll_ = scroll_;
            }
            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ReleaseCapture();
            if (thumbDrag_) {
                thumbDrag_ = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const Elem* hit = HitTest(pt);
            if (hit && hit->id == pressed_.id && hit->arg == pressed_.arg && hit->id != kNone) {
                const Elem clicked = *hit;  // OnClick перерисует окно и пересоберёт elems_
                OnClick(clicked);
            } else if (pressed_.id == kNone) {
                const std::wstring link = DocLinkAt(pt);
                if (link.compare(0, 4, L"http") == 0) OpenUrl(link);
            }
            pressed_ = Elem();
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (selected_ < 0) return 0;
            const float lines = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            scroll_ -= lines * ScF(64.0f, dpi_);
            ClampScroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            const int count = static_cast<int>(catalog::Entries().size());
            switch (wp) {
                case VK_UP: Select(selected_ <= 0 ? (selected_ < 0 ? count - 1 : 0) : selected_ - 1); break;
                case VK_DOWN: Select(selected_ < 0 ? 0 : (selected_ + 1 >= count ? -1 : selected_ + 1)); break;
                case VK_PRIOR:
                case VK_NEXT:
                    scroll_ += (wp == VK_NEXT ? 1.0f : -1.0f) * (docBottom_ - docTop_) * 0.85f;
                    ClampScroll();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                case VK_HOME:
                    scroll_ = 0.0f;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                default: break;
            }
            return 0;
        }

        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, P().editText);
            SetBkColor(dc, P().editBack);
            return reinterpret_cast<LRESULT>(editBrush_);
        }

        case WM_COMMAND:
            if (LOWORD(wp) == kEditId) {
                const UINT code = HIWORD(wp);
                // Поле шлёт EN_CHANGE ещё из CreateWindowEx, когда edit_ не присвоен, — берём окно из сообщения.
                HWND field = reinterpret_cast<HWND>(lp);
                if ((code == EN_CHANGE || code == EN_KILLFOCUS) && field) {
                    wchar_t buf[512] = {0};
                    GetWindowTextW(field, buf, 512);
                    const std::wstring repo = NormalizeRepo(buf);
                    if (repo != data_.repo) {
                        data_.repo = repo;
                        SaveData(data_);
                    }
                    // Вставили ссылку целиком — показываем, как её поняли.
                    if (code == EN_KILLFOCUS && !repo.empty() && repo != buf) SetWindowTextW(field, repo.c_str());
                }
                if (code == EN_SETFOCUS || code == EN_KILLFOCUS || code == EN_CHANGE) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            break;

        case kTrayMessage:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                    ShowHub();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: {
                    Poll();
                    POINT pt;
                    GetCursorPos(&pt);
                    ShowTrayMenu(pt);
                    break;
                }
                default: break;
            }
            return 0;

        case download::kMsgProgress:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case download::kMsgDone: {
            const int i = static_cast<int>(wp);
            if (i >= 0 && i < static_cast<int>(notes_.size())) {
                notes_[static_cast<size_t>(i)] = download::TakeResult(i);
                noteIsError_[static_cast<size_t>(i)] = lp == 0;
            }
            Poll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_CLOSE:
            if (data_.closeToTray && trayAdded_) HideHub();
            else Exit();
            return 0;

        case WM_ENDSESSION:
            if (wp) SavePlacement();
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerPoll);
            KillTimer(hwnd, kTimerPollSoon);
            if (IsWindowVisible(hwnd)) SavePlacement();
            else SaveData(data_);
            TrayRemove(hwnd);
            if (editFont_) DeleteObject(editFont_);
            if (editBrush_) DeleteObject(editBrush_);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Hub::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_hub && (g_hub->hwnd_ == hwnd || !g_hub->hwnd_)) {
        if (!g_hub->hwnd_) g_hub->hwnd_ = hwnd;
        return g_hub->WndProc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Hub::Create(HINSTANCE inst, bool startHidden) {
    inst_ = inst;
    g_hub = this;

    if (!catalog::Load()) {
        MessageBoxW(nullptr, L"Каталог виджетов внутри программы повреждён.", L"Центр виджетов", MB_ICONERROR);
        return false;
    }
    const size_t n = catalog::Entries().size();
    status_.assign(n, control::Status());
    paths_.assign(n, std::wstring());
    notes_.assign(n, std::wstring());
    noteIsError_.assign(n, false);
    autostart_.assign(n, false);
    trayHidden_.assign(n, nullptr);

    LoadData(data_);
    if (data_.repo.empty()) data_.repo = NormalizeRepo(catalog::DefaultRepo());
    SetTheme(data_.darkTheme, data_.accent);
    selected_ = data_.selected.empty() ? -1 : catalog::IndexOf(data_.selected);
    if (selected_ < 0 && !data_.selected.empty()) selected_ = 0;
    if (data_.selected.empty() && data_.paths.empty()) selected_ = 0;  // первый запуск — сразу к виджетам

    iconBig_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    iconSmall_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                               LR_SHARED));

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = &Hub::WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = iconBig_;
    wc.hIconSm = iconSmall_;
    wc.lpszClassName = hub::kHubClass;
    if (!RegisterClassExW(&wc)) return false;

    // Окно создаём скрытым, размер и место ставим уже с учётом DPI монитора.
    hwnd_ = CreateWindowExW(0, hub::kHubClass, L"Центр виджетов", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    dpi_ = WindowDpi(hwnd_);
    fonts_.Build(dpi_);

    edit_ = CreateWindowExW(0, L"EDIT", data_.repo.c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), inst, nullptr);
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"владелец/репозиторий"));
    RebuildEditFont();
    ApplyTheme();

    WINDOWPLACEMENT wp = {sizeof(wp)};
    GetWindowPlacement(hwnd_, &wp);
    int pw = Sc(data_.winW, dpi_), ph = Sc(data_.winH, dpi_);
    RECT saved = {data_.winX, data_.winY, data_.winX + pw, data_.winY + ph};
    // Мониторов может быть несколько, и любой из них могут отключить. Сохранённое место
    // берём, только если оно всё ещё на каком-то экране; иначе — центр монитора с курсором.
    if (data_.winX == -32768 || !MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT& work = mi.rcWork;
        pw = min(pw, static_cast<int>(work.right - work.left));
        ph = min(ph, static_cast<int>(work.bottom - work.top));
        saved.left = work.left + (work.right - work.left - pw) / 2;
        saved.top = work.top + (work.bottom - work.top - ph) / 2;
        saved.right = saved.left + pw;
        saved.bottom = saved.top + ph;
    }
    // rcNormalPosition — в координатах рабочей области; GetWindowPlacement отдаёт их же,
    // так что сохранённое и восстановленное совпадают.
    wp.rcNormalPosition = saved;
    wp.showCmd = SW_HIDE;
    SetWindowPlacement(hwnd_, &wp);

    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    trayAdded_ = TrayAdd(hwnd_, kTrayMessage, iconSmall_, L"Центр виджетов");

    Poll();  // заодно просит уже запущенные виджеты спрятать свои значки
    if (selected_ >= 0) doc_.SetSource(&catalog::Entries()[static_cast<size_t>(selected_)]);
    SetTimer(hwnd_, kTimerPoll, kPollMs, nullptr);

    if (!startHidden || !trayAdded_) {
        ShowWindow(hwnd_, data_.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        UpdateWindow(hwnd_);
    }
    return true;
}

Hub g_instance;

}  // namespace

UINT HubShowMessage() {
    static const UINT id = RegisterWindowMessageW(L"WidgetHub_Show");
    return id;
}

bool CreateHubWindow(HINSTANCE inst, bool startHidden) { return g_instance.Create(inst, startHidden); }
