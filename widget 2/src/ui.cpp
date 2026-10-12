#include "ui.h"

using namespace Gdiplus;

namespace {

struct AccentPair {
    const wchar_t* name;
    ARGB dark;   // на тёмном фоне
    ARGB light;  // на светлом — темнее, иначе не хватает контраста
};

const AccentPair kAccents[] = {
    {L"Фиолетовый", 0xFFA97BFF, 0xFF7B44E0},
    {L"Синий", 0xFF6E8BFF, 0xFF3A5CE0},
    {L"Зелёный", 0xFF3FD08A, 0xFF12996B},
    {L"Оранжевый", 0xFFFF9B4D, 0xFFD9701A},
};

Palette MakeDark(ARGB accent) {
    Palette p{};
    p.cardBg = 0xF2171A21;
    p.cardBgTop = 0xF2242833;
    p.cardBorder = 0x24FFFFFF;
    p.cardGloss = 0x1AFFFFFF;
    p.groupBg = 0x0AFFFFFF;
    p.textPrimary = 0xFFF2F4F8;
    p.textMuted = 0xFF7E869A;
    p.textFaint = 0xFF5A6273;
    p.separator = 0x14FFFFFF;
    p.accent = accent;
    p.onAccent = 0xFF0E1220;
    p.danger = 0xFFFF6B72;
    p.ok = 0xFF3FD08A;
    p.hover = 0x12FFFFFF;
    p.buttonBg = 0x1AFFFFFF;
    p.buttonBgHover = 0x2EFFFFFF;
    p.dialogBg = 0xFF1A1D24;
    p.fieldBg = 0xFF12151B;
    p.fieldBorder = 0x26FFFFFF;
    p.editText = RGB(0xF2, 0xF4, 0xF8);
    p.editBack = RGB(0x12, 0x15, 0x1B);
    p.dark = true;
    return p;
}

Palette MakeLight(ARGB accent) {
    Palette p{};
    p.cardBg = 0xF7F1F3F8;
    p.cardBgTop = 0xF9FFFFFF;
    p.cardBorder = 0x22000000;
    p.cardGloss = 0x80FFFFFF;
    p.groupBg = 0x08000000;
    p.textPrimary = 0xFF11151C;
    p.textMuted = 0xFF5C6577;
    p.textFaint = 0xFF8A93A6;
    p.separator = 0x14000000;
    p.accent = accent;
    p.onAccent = 0xFFFFFFFF;
    p.danger = 0xFFD92D3C;
    p.ok = 0xFF0E8F63;
    p.hover = 0x0D000000;
    p.buttonBg = 0x0F000000;
    p.buttonBgHover = 0x1A000000;
    p.dialogBg = 0xFFF7F8FB;
    p.fieldBg = 0xFFFFFFFF;
    p.fieldBorder = 0x26000000;
    p.editText = RGB(0x11, 0x15, 0x1C);
    p.editBack = RGB(0xFF, 0xFF, 0xFF);
    p.dark = false;
    return p;
}

Palette g_palette = MakeDark(kAccents[0].dark);

}  // namespace

const Palette& P() { return g_palette; }

void SetTheme(bool dark, int accentIndex) {
    if (accentIndex < 0 || accentIndex >= AccentCount()) accentIndex = 0;
    const AccentPair& a = kAccents[accentIndex];
    g_palette = dark ? MakeDark(a.dark) : MakeLight(a.light);
}

int AccentCount() { return static_cast<int>(sizeof(kAccents) / sizeof(kAccents[0])); }

const wchar_t* AccentName(int index) {
    if (index < 0 || index >= AccentCount()) index = 0;
    return kAccents[index].name;
}

ARGB WithAlpha(ARGB color, BYTE alpha) {
    return (color & 0x00FFFFFF) | (static_cast<ARGB>(alpha) << 24);
}

ARGB Fade(ARGB color, int percent) {
    if (percent >= 100) return color;
    if (percent < 0) percent = 0;
    const unsigned alpha = ((color >> 24) & 0xFF) * static_cast<unsigned>(percent) / 100u;
    return WithAlpha(color, static_cast<BYTE>(alpha));
}

ARGB Mix(ARGB a, ARGB b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return WithAlpha(b, static_cast<BYTE>((a >> 24) & 0xFF));
    auto channel = [&](int shift) {
        const float ca = static_cast<float>((a >> shift) & 0xFF);
        const float cb = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ARGB>(ca + (cb - ca) * t + 0.5f) & 0xFF;
    };
    return (a & 0xFF000000) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

int WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = []() -> GetDpiForWindowFn {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetDpiForWindowFn>(
                            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")))
                      : nullptr;
    }();
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi > 0) return static_cast<int>(dpi);
    }
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    return dpi > 0 ? dpi : 96;
}

namespace gfx {

namespace {

// GenericTypographic не добавляет «воздух» по краям — метрики совпадают с отрисовкой.
const StringFormat& TightFormat() {
    static StringFormat fmt(StringFormat::GenericTypographic());
    static bool init = false;
    if (!init) {
        fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
        fmt.SetTrimming(StringTrimmingNone);
        init = true;
    }
    return fmt;
}

}  // namespace

GraphicsPath* RoundRectPath(const RectF& r, float radius) {
    auto* path = new GraphicsPath();
    const float d = radius * 2.0f;
    if (radius <= 0.0f) {
        path->AddRectangle(r);
        path->CloseFigure();
        return path;
    }
    path->AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path->AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path->AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
    return path;
}

void FillRoundRect(Graphics& g, const RectF& r, float radius, ARGB color) {
    GraphicsPath* path = RoundRectPath(r, radius);
    const Color c(color);  // именованная переменная: SolidBrush b(Color(x)) читается как объявление функции
    SolidBrush brush(c);
    g.FillPath(&brush, path);
    delete path;
}

void StrokeRoundRect(Graphics& g, const RectF& r, float radius, ARGB color, float width) {
    // Обводка рисуется по центру контура — смещаем внутрь на полтолщины.
    RectF inner(r.X + width / 2.0f, r.Y + width / 2.0f, r.Width - width, r.Height - width);
    GraphicsPath* path = RoundRectPath(inner, radius);
    Pen pen(Color(color), width);
    g.DrawPath(&pen, path);
    delete path;
}

void DashRoundRect(Graphics& g, const RectF& r, float radius, ARGB color, float width) {
    RectF inner(r.X + width / 2.0f, r.Y + width / 2.0f, r.Width - width, r.Height - width);
    GraphicsPath* path = RoundRectPath(inner, radius);
    Pen pen(Color(color), width);
    pen.SetDashStyle(DashStyleDash);
    g.DrawPath(&pen, path);
    delete path;
}

void FillRoundRectGradient(Graphics& g, const RectF& r, float radius, ARGB top, ARGB bottom) {
    GraphicsPath* path = RoundRectPath(r, radius);
    // Кисть строим на чуть большем прямоугольнике: иначе GDI+ подмешивает крайние пиксели.
    RectF span(r.X - 1.0f, r.Y - 1.0f, r.Width + 2.0f, r.Height + 2.0f);
    LinearGradientBrush brush(span, Color(top), Color(bottom), LinearGradientModeVertical);
    g.FillPath(&brush, path);
    delete path;
}

void TopHighlight(Graphics& g, const RectF& r, float radius, ARGB color, float width) {
    // Дуга по верхней кромке: слева направо, только верхняя половина контура.
    GraphicsPath path;
    const float d = radius * 2.0f;
    path.AddArc(r.X + width / 2.0f, r.Y + width / 2.0f, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight() - d - width / 2.0f, r.Y + width / 2.0f, d, d, 270.0f, 90.0f);
    Pen pen(Color(color), width);
    g.DrawPath(&pen, &path);
}

float TextWidth(Graphics& g, const wchar_t* text, const Font& font) {
    RectF bounds;
    g.MeasureString(text, -1, &font, PointF(0.0f, 0.0f), &TightFormat(), &bounds);
    return bounds.Width;
}

void DrawText(Graphics& g, const wchar_t* text, const Font& font, ARGB color, float x, float y, Align align) {
    float drawX = x;
    if (align != Align::Left) {
        const float w = TextWidth(g, text, font);
        drawX = (align == Align::Center) ? x - w / 2.0f : x - w;
    }
    const Color c(color);  // именованная переменная: SolidBrush b(Color(x)) читается как объявление функции
    SolidBrush brush(c);
    g.DrawString(text, -1, &font, PointF(drawX, y), &TightFormat(), &brush);
}

std::wstring Ellipsize(Graphics& g, const std::wstring& text, const Font& font, float maxWidth) {
    if (maxWidth <= 0.0f) return std::wstring();
    if (TextWidth(g, text.c_str(), font) <= maxWidth) return text;
    std::wstring cut = text;
    while (!cut.empty() && TextWidth(g, (cut + L"…").c_str(), font) > maxWidth) cut.pop_back();
    return cut + L"…";
}

float TrackedWidth(Graphics& g, const wchar_t* text, const Font& font, float tracking) {
    float total = 0.0f;
    for (const wchar_t* p = text; *p; ++p) {
        const wchar_t ch[2] = {*p, L'\0'};
        total += TextWidth(g, ch, font) + tracking;
    }
    return total > 0.0f ? total - tracking : 0.0f;
}

void DrawTracked(Graphics& g, const wchar_t* text, const Font& font, ARGB color, float x, float y, float tracking,
                 Align align) {
    float drawX = x;
    if (align != Align::Left) {
        const float w = TrackedWidth(g, text, font, tracking);
        drawX = (align == Align::Center) ? x - w / 2.0f : x - w;
    }
    const Color c(color);  // именованная переменная: SolidBrush b(Color(x)) читается как объявление функции
    SolidBrush brush(c);
    for (const wchar_t* p = text; *p; ++p) {
        const wchar_t ch[2] = {*p, L'\0'};
        g.DrawString(ch, 1, &font, PointF(drawX, y), &TightFormat(), &brush);
        drawX += TextWidth(g, ch, font) + tracking;
    }
}

void DropShadow(Graphics& g, const RectF& r, float radius, int spread, int percent) {
    for (int i = spread; i > 0; i -= 2) {
        const BYTE alpha = static_cast<BYTE>(3 + (spread - i) / 3);
        RectF box(r.X - i, r.Y - i + i * 0.35f, r.Width + i * 2, r.Height + i * 2);
        FillRoundRect(g, box, radius + i, Fade(static_cast<ARGB>(alpha) << 24, percent));
    }
}

void FadeLine(Graphics& g, const RectF& r, ARGB color) {
    // Кисть строим шире полоски: у самых краёв GDI+ иначе берёт цвет соседнего пикселя.
    RectF span(r.X - 1.0f, r.Y - 1.0f, r.Width + 2.0f, r.Height + 2.0f);
    LinearGradientBrush brush(span, Color(color), Color(WithAlpha(color, 0)),
                              LinearGradientModeHorizontal);
    g.FillRectangle(&brush, r);
}

}  // namespace gfx
