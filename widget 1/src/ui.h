// Общие для виджета и диалога: палитра, DPI и обёртки над GDI+.
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
    Gdiplus::ARGB cardBorder;
    Gdiplus::ARGB textPrimary;
    Gdiplus::ARGB textMuted;
    Gdiplus::ARGB textFaint;
    Gdiplus::ARGB separator;
    Gdiplus::ARGB accent;
    Gdiplus::ARGB onAccent;  // текст поверх акцентной заливки
    Gdiplus::ARGB debt;      // долг вырос
    Gdiplus::ARGB paid;      // долг погашен
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
void SetTheme(bool dark, int accentIndex);

int AccentCount();
const wchar_t* AccentName(int index);

// DPI текущего окна (96 = 100%), с fallback для старых сборок Windows.
int WindowDpi(HWND hwnd);

// Логические единицы (при 96 DPI) -> физические пиксели.
inline int Sc(int value, int dpi) { return MulDiv(value, dpi, 96); }
inline float ScF(float value, int dpi) { return value * static_cast<float>(dpi) / 96.0f; }

namespace gfx {

Gdiplus::GraphicsPath* RoundRectPath(const Gdiplus::RectF& r, float radius);
void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, Gdiplus::ARGB color);
void StrokeRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, Gdiplus::ARGB color,
                     float width = 1.0f);

enum class Align { Left, Center, Right };

float TextWidth(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font);

// x трактуется по align; y — верх строки.
void DrawText(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font, Gdiplus::ARGB color, float x,
              float y, Align align = Align::Left);

// Заголовки вроде "NEW YEAR" — с разрядкой между буквами.
float TrackedWidth(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font, float tracking);
void DrawTracked(Gdiplus::Graphics& g, const wchar_t* text, const Gdiplus::Font& font, Gdiplus::ARGB color, float x,
                 float y, float tracking, Align align = Align::Left);

// Цифры в ячейках равной ширины: счётчик не «дёргается» при смене секунды.
float DigitsWidth(Gdiplus::Graphics& g, const wchar_t* digits, const Gdiplus::Font& font);
void DrawDigits(Gdiplus::Graphics& g, const wchar_t* digits, const Gdiplus::Font& font, Gdiplus::ARGB color, float cx,
                float y);

}  // namespace gfx
