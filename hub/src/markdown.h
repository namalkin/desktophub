// README виджета на странице центра: разбор Markdown, раскладка по ширине и отрисовка.
//
// Поддерживается то, что встречается в README серии: заголовки, абзацы, списки,
// таблицы, блоки кода, цитаты, линии, картинки, **жирный**, `код` и [ссылки](url).
// Разделы для разработчика («Сборка», «Как устроен код»…) пропускаются: в центре README
// читает пользователь, а не тот, кто собирает виджет.
#pragma once

#include "ui.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catalog.h"

namespace md {

class Document {
public:
    Document();
    ~Document();

    void SetSource(const catalog::Entry* entry);  // nullptr — пустой документ

    // Раскладка пересчитывается, только если поменялись ширина или DPI.
    void Layout(Gdiplus::Graphics& g, float width, int dpi);
    float Height() const { return height_; }

    // (x, y) — где на экране верхний левый угол документа; clipTop/clipBottom — видимая полоса.
    void Draw(Gdiplus::Graphics& g, float x, float y, float clipTop, float clipBottom) const;

    // Ссылка под точкой в координатах документа. Пусто — не ссылка.
    std::wstring LinkAt(float x, float y) const;

    struct Block;
    struct Run;
    struct Prim;

private:
    void BuildFonts(int dpi);
    float Wrap(Gdiplus::Graphics& g, const std::vector<Run>& runs, float x, float y, float width, int baseFont,
               bool draw);
    float TokenWidth(Gdiplus::Graphics& g, const std::wstring& text, int font);
    Gdiplus::Bitmap* Scaled(Gdiplus::Bitmap* src, int w, int h);

    const catalog::Entry* entry_ = nullptr;
    std::vector<Block> blocks_;
    std::vector<std::wstring> links_;

    std::vector<Prim> prims_;
    float height_ = 0.0f;
    float width_ = -1.0f;
    int dpi_ = 0;

    std::unique_ptr<Gdiplus::FontFamily> family_;
    std::unique_ptr<Gdiplus::FontFamily> mono_;
    std::vector<std::unique_ptr<Gdiplus::Font>> fonts_;
    std::vector<float> lineH_;  // высота строки для каждого шрифта
    std::map<std::pair<int, std::wstring>, float> widths_;
    std::map<Gdiplus::Bitmap*, std::unique_ptr<Gdiplus::Bitmap>> scaled_;
};

}  // namespace md
