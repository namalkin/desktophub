// Общие для блоков, диалога и настроек: палитра, DPI и обёртки над GDI+.
#pragma once

#include <windows.h>
#include <objidl.h>

#include <algorithm>
using std::max;
using std::min;

#include <gdiplus.h>

#include <string>

// Палитра выбирается в рантайме (тема + акцент), поэтому не constexpr.
struct Palette {
    Gdiplus::ARGB cardBg;
    Gdiplus::ARGB cardBgTop;  // верх градиента карточки
    Gdiplus::ARGB cardBorder;
    Gdiplus::ARGB cardGloss;  // блик по верхней кромке
    Gdiplus::ARGB groupBg;    // подложка под строкой, ячейкой таблицы
    Gdiplus::ARGB textPrimary;
    Gdiplus::ARGB textMuted;
    Gdiplus::ARGB textFaint;
    Gdiplus::ARGB separator;
    Gdiplus::ARGB accent;
    Gdiplus::ARGB onAccent;  // текст поверх акцентной заливки
    Gdiplus::ARGB danger;    // удаление, «невыполнено»
    Gdiplus::ARGB ok;        // «выполнено»
    Gdiplus::ARGB track;     // дорожка диаграмм: кольца, столбики, полосы
    Gdiplus::ARGB hover;
    Gdiplus::ARGB buttonBg;
    Gdiplus::ARGB buttonBgHover;
    Gdiplus::ARGB dialogBg;
    Gdiplus::ARGB fieldBg;
    Gdiplus::ARGB fieldBorder;
    COLORREF editText;  // для WM_CTLCOLOREDIT — GDI, не GDI+
    COLORREF editBack;
    bool dark;
};

const Palette& P();

// opacityPercent (40..100) — насколько плотный фон будет у блока. Сквозь прозрачную карточку
// видны обои, поэтому чем она прозрачнее, тем ближе второстепенный текст к основному:
// иначе приглушённые подписи первыми пропадают на пёстром фоне.
void SetTheme(bool dark, int accentIndex, int opacityPercent = 100);

int AccentCount();
const wchar_t* AccentName(int index);

// Шрифт интерфейса — общий для всех блоков. Список короткий и только из тех семейств,
// что есть в самой Windows: виджет ничего не устанавливает.
int UiFontCount();
const wchar_t* UiFontLabel(int index);   // подпись в настройках
const wchar_t* UiFontFamily(int index);  // имя семейства для GDI+
// Кегли у семейств разные: Verdana крупнее Segoe UI при том же размере в пикселях.
// Коэффициент выравнивает их так, чтобы блок не «прыгал» при смене шрифта.
float UiFontScale(int index);
// Выбранного семейства может не быть в системе — тогда вернётся Segoe UI.
Gdiplus::FontFamily* MakeUiFontFamily(int index);

// DPI текущего окна (96 = 100%), с fallback для старых сборок Windows.
int WindowDpi(HWND hwnd);

// Логические единицы (при 96 DPI) -> физические пиксели.
inline int Sc(int value, int dpi) { return MulDiv(value, dpi, 96); }
inline float ScF(float value, int dpi) { return value * static_cast<float>(dpi) / 96.0f; }

// Тот же цвет с другой прозрачностью.
Gdiplus::ARGB WithAlpha(Gdiplus::ARGB color, BYTE alpha);

// Прозрачность блока: гасим только фон, текст и диаграммы остаются чёткими.
Gdiplus::ARGB Fade(Gdiplus::ARGB color, int percent);

// Смешивание цветов, t = 0 -> a, t = 1 -> b. Альфа берётся у a.
Gdiplus::ARGB Mix(Gdiplus::ARGB a, Gdiplus::ARGB b, float t);

namespace gfx {

Gdiplus::GraphicsPath* RoundRectPath(const Gdiplus::RectF& r, float radius);
void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, Gdiplus::ARGB color);
void StrokeRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, Gdiplus::ARGB color,
                     float width = 1.0f);
// Пунктирная рамка — «сюда можно нажать и добавить».
void DashRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, Gdiplus::ARGB color, float width);

// Вертикальный градиент: карточка выглядит объёмной, а не плоской заливкой.
void FillRoundRectGradient(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, Gdiplus::ARGB top,
                           Gdiplus::ARGB bottom);

// Блик по верхней кромке карточки — «стеклянный» край.
void TopHighlight(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, Gdiplus::ARGB color, float width);

// Кольцевая диаграмма: дорожка на весь круг и дуга прогресса от 12 часов по часовой.
void Ring(Gdiplus::Graphics& g, float cx, float cy, float radius, float thickness, float fraction,
          Gdiplus::ARGB track, Gdiplus::ARGB color);

enum class Align { Left, Center, Right };

float TextWidth(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font);

// x трактуется по align; y — верх строки.
void DrawText(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font, Gdiplus::ARGB color, float x,
              float y, Align align = Align::Left);

// Обрезает текст по ширине и дописывает многоточие.
std::wstring Ellipsize(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::Font& font, float maxWidth);

// Заголовки вроде "ЦЕЛИ НЕДЕЛИ" — с разрядкой между буквами.
float TrackedWidth(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font, float tracking);
void DrawTracked(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font, Gdiplus::ARGB color, float x,
                 float y, float tracking, Align align = Align::Left);

// Мягкая тень под скруглённым прямоугольником: несколько контуров с малой альфой.
// percent — та же прозрачность, что и у фона: у полупрозрачного блока тень тоже light.
void DropShadow(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, int spread, int percent = 100);

// Полоска-градиент: слева насыщенный цвет, справа сходит на нет.
void FadeLine(Gdiplus::Graphics& g, const Gdiplus::RectF& r, Gdiplus::ARGB color);

}  // namespace gfx
