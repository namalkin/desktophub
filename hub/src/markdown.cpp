#include "markdown.h"

#include <cwctype>

#include "util.h"

using namespace Gdiplus;

namespace md {

namespace {

enum FontId { kBody, kBold, kCode, kH2, kH3, kSmall, kSmallBold, kFontCount };

// Стиль куска текста — сам шрифт зависит от того, где кусок стоит (абзац, таблица, заголовок).
enum class Style : uint8_t { Plain, Strong, Code };

// Роль цвета, а не сам цвет: тема переключается без новой раскладки.
enum Ink : uint8_t { kPrimary, kMuted, kAccent, kCodeBg, kRule, kHeadBg, kQuoteBar };

enum class Kind : uint8_t { Heading, Paragraph, Bullet, Numbered, Code, Table, Image, Rule, Quote };

// Разделы README, адресованные разработчику. Сравнение по началу заголовка, без регистра.
const wchar_t* const kDevSections[] = {
    L"быстрый старт", L"выбор стека", L"структура проекта", L"как устроен код",
    L"сборка",        L"что проверено", L"идеи",
};

std::wstring Lower(std::wstring s) {
    for (wchar_t& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

bool IsDevSection(const std::wstring& title) {
    const std::wstring t = Lower(TrimSpaces(title));
    for (const wchar_t* prefix : kDevSections) {
        const std::wstring p = prefix;
        if (t.compare(0, p.size(), p) == 0) return true;
    }
    return false;
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    const std::wstring p = prefix;
    return s.compare(0, p.size(), p) == 0;
}

int LeadingSpaces(const std::wstring& s) {
    int n = 0;
    for (wchar_t ch : s) {
        if (ch == L' ') n++;
        else if (ch == L'\t') n += 4;
        else break;
    }
    return n;
}

// «- текст», «* текст», «+ текст» -> «текст».
bool BulletText(const std::wstring& trimmed, std::wstring& text) {
    if (trimmed.size() >= 2 && (trimmed[0] == L'-' || trimmed[0] == L'*' || trimmed[0] == L'+') &&
        trimmed[1] == L' ') {
        text = trimmed.substr(2);
        return true;
    }
    return false;
}

// «12. текст» -> «12.», «текст».
bool NumberedText(const std::wstring& trimmed, std::wstring& number, std::wstring& text) {
    size_t i = 0;
    while (i < trimmed.size() && iswdigit(trimmed[i])) i++;
    if (i == 0 || i > 3 || i + 1 >= trimmed.size()) return false;
    if ((trimmed[i] != L'.' && trimmed[i] != L')') || trimmed[i + 1] != L' ') return false;
    number = trimmed.substr(0, i) + L".";
    text = trimmed.substr(i + 2);
    return true;
}

bool IsTableRow(const std::wstring& trimmed) { return !trimmed.empty() && trimmed[0] == L'|'; }

std::vector<std::wstring> SplitRow(const std::wstring& trimmed) {
    std::vector<std::wstring> cells;
    std::wstring cur;
    bool inCode = false;
    for (size_t i = 1; i < trimmed.size(); i++) {  // первый символ — «|»
        const wchar_t ch = trimmed[i];
        if (ch == L'`') inCode = !inCode;
        if (ch == L'\\' && i + 1 < trimmed.size() && trimmed[i + 1] == L'|') {
            cur += L'|';
            i++;
            continue;
        }
        if (ch == L'|' && !inCode) {
            cells.push_back(TrimSpaces(cur));
            cur.clear();
            continue;
        }
        cur += ch;
    }
    if (!TrimSpaces(cur).empty()) cells.push_back(TrimSpaces(cur));
    return cells;
}

bool IsSeparatorRow(const std::vector<std::wstring>& cells) {
    if (cells.empty()) return false;
    for (const std::wstring& c : cells) {
        if (c.empty()) return false;
        for (wchar_t ch : c) {
            if (ch != L'-' && ch != L':' && ch != L' ') return false;
        }
    }
    return true;
}

bool IsFence(const std::wstring& trimmed) { return StartsWith(trimmed, L"```") || StartsWith(trimmed, L"~~~"); }

bool IsRule(const std::wstring& trimmed) {
    if (trimmed.size() < 3) return false;
    const wchar_t c = trimmed[0];
    if (c != L'-' && c != L'*' && c != L'_') return false;
    for (wchar_t ch : trimmed) {
        if (ch != c && ch != L' ') return false;
    }
    return true;
}

// «![подпись](docs/screenshot.png)» -> «docs/screenshot.png».
bool ImageRef(const std::wstring& trimmed, std::wstring& ref) {
    if (!StartsWith(trimmed, L"![")) return false;
    const size_t mid = trimmed.find(L"](");
    if (mid == std::wstring::npos) return false;
    const size_t end = trimmed.find(L')', mid + 2);
    if (end == std::wstring::npos) return false;
    ref = trimmed.substr(mid + 2, end - mid - 2);
    const size_t space = ref.find(L' ');  // ![x](path "title")
    if (space != std::wstring::npos) ref.resize(space);
    return !ref.empty();
}

}  // namespace

struct Document::Run {
    std::wstring text;
    Style style = Style::Plain;
    int link = -1;
    int id = 0;  // соседние куски одного `кода` склеиваются в одну подложку
};

struct Document::Block {
    Kind kind = Kind::Paragraph;
    int level = 0;  // заголовок: 2 или 3; список: вложенность
    std::wstring number;
    std::vector<Run> runs;
    std::vector<std::wstring> code;
    std::vector<std::vector<std::vector<Run>>> rows;  // таблица: строки -> ячейки -> куски
    std::wstring image;
};

struct Document::Prim {
    enum Type : uint8_t { Text, Fill, Stroke, Line, Dot, Image, Link } type = Text;
    uint8_t font = kBody;
    uint8_t color = kPrimary;
    float radius = 0.0f;
    RectF r;
    std::wstring text;
    Bitmap* image = nullptr;
    int link = -1;
};

namespace {

using Run = Document::Run;

// Строка Markdown -> куски текста со стилем. Картинки внутри строки и HTML-теги выбрасываются.
std::vector<Run> ParseInline(const std::wstring& s, std::vector<std::wstring>& links, int& nextId) {
    std::vector<Run> runs;
    std::wstring cur;
    bool strong = false;
    auto flush = [&]() {
        if (cur.empty()) return;
        Run r;
        r.text = cur;
        r.style = strong ? Style::Strong : Style::Plain;
        r.id = nextId++;
        runs.push_back(std::move(r));
        cur.clear();
    };

    for (size_t i = 0; i < s.size(); i++) {
        const wchar_t c = s[i];
        const wchar_t next = i + 1 < s.size() ? s[i + 1] : L'\0';
        if (c == L'\\' && next && iswpunct(next)) {
            cur += next;
            i++;
            continue;
        }
        if (c == L'`') {
            const size_t end = s.find(L'`', i + 1);
            if (end != std::wstring::npos) {
                flush();
                Run r;
                r.text = s.substr(i + 1, end - i - 1);
                r.style = Style::Code;
                r.id = nextId++;
                runs.push_back(std::move(r));
                i = end;
                continue;
            }
        }
        if (c == L'*' && next == L'*') {
            flush();
            strong = !strong;
            i++;
            continue;
        }
        if (c == L'!' && next == L'[') {
            const size_t mid = s.find(L"](", i);
            const size_t end = mid == std::wstring::npos ? mid : s.find(L')', mid);
            if (end != std::wstring::npos) {
                i = end;
                continue;
            }
        }
        if (c == L'[') {
            const size_t mid = s.find(L"](", i);
            const size_t end = mid == std::wstring::npos ? mid : s.find(L')', mid);
            if (end != std::wstring::npos && s.find(L'\n', i) > end) {
                flush();
                std::wstring text;
                for (wchar_t ch : s.substr(i + 1, mid - i - 1)) {
                    if (ch != L'`' && ch != L'*') text += ch;
                }
                links.push_back(s.substr(mid + 2, end - mid - 2));
                Run r;
                r.text = text;
                r.style = strong ? Style::Strong : Style::Plain;
                r.link = static_cast<int>(links.size()) - 1;
                r.id = nextId++;
                runs.push_back(std::move(r));
                i = end;
                continue;
            }
        }
        if (c == L'<') {
            const size_t end = s.find(L'>', i);
            if (end != std::wstring::npos && end - i < 12 && (iswalpha(next) || next == L'/')) {
                if (StartsWith(s.substr(i), L"<br")) cur += L' ';
                i = end;
                continue;
            }
        }
        cur += c;
    }
    flush();
    return runs;
}

}  // namespace

Document::Document() = default;
Document::~Document() = default;

void Document::SetSource(const catalog::Entry* entry) {
    entry_ = entry;
    blocks_.clear();
    links_.clear();
    prims_.clear();
    scaled_.clear();
    width_ = -1.0f;
    height_ = 0.0f;
    if (!entry) return;

    const std::wstring text = Utf8ToWide(catalog::Readme(*entry));
    std::vector<std::wstring> lines;
    {
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos) end = text.size();
            std::wstring line = text.substr(start, end - start);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            lines.push_back(line);
            start = end + 1;
        }
    }

    int nextId = 0;
    bool skipping = false;
    size_t i = 0;
    auto isBlockStart = [&](const std::wstring& trimmed) {
        std::wstring a, b;
        return trimmed.empty() || trimmed[0] == L'#' || IsFence(trimmed) || IsTableRow(trimmed) ||
               StartsWith(trimmed, L"![") || StartsWith(trimmed, L"> ") || IsRule(trimmed) ||
               BulletText(trimmed, a) || NumberedText(trimmed, a, b);
    };

    while (i < lines.size()) {
        const std::wstring& line = lines[i];
        const std::wstring trimmed = TrimSpaces(line);

        // Код пропускаем целиком, даже в пропускаемых разделах: внутри бывает «# комментарий».
        if (IsFence(trimmed)) {
            Block b;
            b.kind = Kind::Code;
            i++;
            while (i < lines.size() && !IsFence(TrimSpaces(lines[i]))) b.code.push_back(lines[i++]);
            i++;
            if (!skipping) blocks_.push_back(std::move(b));
            continue;
        }

        if (!trimmed.empty() && trimmed[0] == L'#') {
            int level = 0;
            while (level < static_cast<int>(trimmed.size()) && trimmed[level] == L'#') level++;
            const std::wstring title = TrimSpaces(trimmed.substr(level));
            i++;
            if (level == 1) continue;  // название виджета и так в шапке страницы
            if (level == 2) skipping = IsDevSection(title);
            if (skipping) continue;
            Block b;
            b.kind = Kind::Heading;
            b.level = level == 2 ? 2 : 3;
            b.runs = ParseInline(title, links_, nextId);
            blocks_.push_back(std::move(b));
            continue;
        }

        if (skipping || trimmed.empty()) {
            i++;
            continue;
        }

        std::wstring ref;
        if (ImageRef(trimmed, ref) && trimmed.back() == L')') {
            Block b;
            b.kind = Kind::Image;
            b.image = ref;
            blocks_.push_back(std::move(b));
            i++;
            continue;
        }

        if (IsRule(trimmed)) {
            Block b;
            b.kind = Kind::Rule;
            blocks_.push_back(std::move(b));
            i++;
            continue;
        }

        if (IsTableRow(trimmed)) {
            Block b;
            b.kind = Kind::Table;
            while (i < lines.size() && IsTableRow(TrimSpaces(lines[i]))) {
                const std::vector<std::wstring> cells = SplitRow(TrimSpaces(lines[i]));
                i++;
                if (IsSeparatorRow(cells)) continue;
                std::vector<std::vector<Run>> row;
                for (const std::wstring& cell : cells) row.push_back(ParseInline(cell, links_, nextId));
                b.rows.push_back(std::move(row));
            }
            if (!b.rows.empty()) blocks_.push_back(std::move(b));
            continue;
        }

        if (StartsWith(trimmed, L"> ") || trimmed == L">") {
            std::wstring joined;
            while (i < lines.size()) {
                const std::wstring t = TrimSpaces(lines[i]);
                if (t.empty() || t[0] != L'>') break;
                joined += (joined.empty() ? L"" : L" ") + TrimSpaces(t.substr(1));
                i++;
            }
            Block b;
            b.kind = Kind::Quote;
            b.runs = ParseInline(joined, links_, nextId);
            blocks_.push_back(std::move(b));
            continue;
        }

        std::wstring itemText, number;
        const bool bullet = BulletText(trimmed, itemText);
        const bool numbered = !bullet && NumberedText(trimmed, number, itemText);
        if (bullet || numbered) {
            const int indent = LeadingSpaces(line);
            i++;
            // Продолжение пункта — строки с отступом, которые не начинают новый блок.
            while (i < lines.size()) {
                const std::wstring t = TrimSpaces(lines[i]);
                if (t.empty() || isBlockStart(t) || LeadingSpaces(lines[i]) <= indent) break;
                itemText += L" " + t;
                i++;
            }
            Block b;
            b.kind = bullet ? Kind::Bullet : Kind::Numbered;
            b.level = indent >= 2 ? 1 : 0;
            b.number = number;
            b.runs = ParseInline(itemText, links_, nextId);
            blocks_.push_back(std::move(b));
            continue;
        }

        // Абзац: строки до пустой или до начала другого блока склеиваются через пробел.
        std::wstring joined = trimmed;
        i++;
        while (i < lines.size()) {
            const std::wstring t = TrimSpaces(lines[i]);
            if (isBlockStart(t)) break;
            joined += L" " + t;
            i++;
        }
        Block b;
        b.kind = Kind::Paragraph;
        b.runs = ParseInline(joined, links_, nextId);
        blocks_.push_back(std::move(b));
    }
}

void Document::BuildFonts(int dpi) {
    family_.reset(new FontFamily(L"Segoe UI"));
    if (!family_->IsAvailable()) family_.reset(FontFamily::GenericSansSerif()->Clone());
    mono_.reset(new FontFamily(L"Consolas"));
    if (!mono_->IsAvailable()) mono_.reset(FontFamily::GenericMonospace()->Clone());

    struct Spec {
        bool mono;
        float size;
        int style;
        float leading;
    };
    const Spec specs[kFontCount] = {
        {false, 14.0f, FontStyleRegular, 1.55f},  // kBody
        {false, 14.0f, FontStyleBold, 1.55f},     // kBold
        {true, 12.5f, FontStyleRegular, 1.55f},   // kCode
        {false, 19.0f, FontStyleBold, 1.30f},     // kH2
        {false, 15.5f, FontStyleBold, 1.35f},     // kH3
        {false, 13.0f, FontStyleRegular, 1.45f},  // kSmall
        {false, 13.0f, FontStyleBold, 1.45f},     // kSmallBold
    };
    fonts_.clear();
    lineH_.clear();
    for (const Spec& s : specs) {
        const float px = ScF(s.size, dpi);
        fonts_.emplace_back(new Font(s.mono ? mono_.get() : family_.get(), px, s.style, UnitPixel));
        lineH_.push_back(px * s.leading);
    }
    widths_.clear();
}

float Document::TokenWidth(Graphics& g, const std::wstring& text, int font) {
    const auto key = std::make_pair(font, text);
    auto it = widths_.find(key);
    if (it != widths_.end()) return it->second;
    // Пробелы на конце слова учитываем: из них и складывается расстояние между словами.
    static StringFormat fmt(StringFormat::GenericTypographic());
    static bool init = false;
    if (!init) {
        fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
        init = true;
    }
    RectF bounds;
    g.MeasureString(text.c_str(), static_cast<INT>(text.size()), fonts_[static_cast<size_t>(font)].get(),
                    PointF(0.0f, 0.0f), &fmt, &bounds);
    widths_[key] = bounds.Width;
    return bounds.Width;
}

namespace {

int FontFor(Style style, int base) {
    if (style == Style::Code) return kCode;
    if (base == kBody) return style == Style::Strong ? kBold : kBody;
    if (base == kSmall) return style == Style::Strong ? kSmallBold : kSmall;
    return base;  // заголовки и шапка таблицы и так жирные
}

}  // namespace

// Раскладывает куски по строкам ширины width. Возвращает высоту. draw == false — только меряет.
float Document::Wrap(Graphics& g, const std::vector<Run>& runs, float x, float y, float width, int base,
                     bool draw) {
    struct Piece {
        std::wstring text;
        float x, w, ink;  // ink — ширина без пробелов на конце: по ней рисуется подложка кода
        int font, link, id;
        Style style;
    };
    std::vector<Piece> line;
    float cx = 0.0f;
    float cy = 0.0f;
    const float baseH = lineH_[static_cast<size_t>(base)];
    const Ink baseColor = kPrimary;

    auto flushLine = [&]() {
        float lh = baseH;
        for (const Piece& p : line) lh = max(lh, lineH_[static_cast<size_t>(p.font)]);
        if (draw) {
            // Сначала подложки `кода`, потом текст поверх.
            for (size_t k = 0; k < line.size(); k++) {
                if (line[k].style != Style::Code) continue;
                const float startX = line[k].x;
                float endX = line[k].x + line[k].ink;
                while (k + 1 < line.size() && line[k + 1].style == Style::Code && line[k + 1].id == line[k].id) {
                    k++;
                    endX = line[k].x + line[k].ink;
                }
                const float ph = lineH_[kCode] * 0.82f;
                Prim p;
                p.type = Prim::Fill;
                p.color = kCodeBg;
                p.radius = ph * 0.22f;
                p.r = RectF(x + startX - 3.0f, y + cy + (lh - ph) / 2.0f, endX - startX + 6.0f, ph);
                prims_.push_back(p);
            }
            for (const Piece& piece : line) {
                const float fh = fonts_[static_cast<size_t>(piece.font)]->GetSize() * 1.33f;
                // У Consolas глифы сидят выше, чем у Segoe UI того же кегля, — опускаем к базовой линии.
                const float nudge = piece.font == kCode ? fh * 0.10f : 0.0f;
                Prim p;
                p.type = Prim::Text;
                p.font = static_cast<uint8_t>(piece.font);
                p.color = piece.link >= 0 ? kAccent : baseColor;
                p.r = RectF(x + piece.x, y + cy + (lh - fh) / 2.0f + nudge, piece.w, fh);
                p.text = piece.text;
                prims_.push_back(p);
                if (piece.link >= 0) {
                    Prim l;
                    l.type = Prim::Link;
                    l.link = piece.link;
                    l.r = RectF(x + piece.x, y + cy, piece.w, lh);
                    prims_.push_back(l);
                }
            }
        }
        line.clear();
        cx = 0.0f;
        cy += lh;
    };

    for (const Run& run : runs) {
        const int font = FontFor(run.style, base);
        // Режем на слова, оставляя за каждым его пробелы.
        size_t pos = 0;
        while (pos < run.text.size()) {
            size_t end = pos;
            while (end < run.text.size() && run.text[end] != L' ') end++;
            size_t ink = end;
            while (end < run.text.size() && run.text[end] == L' ') end++;
            std::wstring token = run.text.substr(pos, end - pos);
            const std::wstring inkText = run.text.substr(pos, ink - pos);
            pos = end;

            float w = TokenWidth(g, token, font);
            float inkW = inkText.size() == token.size() ? w : TokenWidth(g, inkText, font);
            if (cx > 0.0f && cx + inkW > width) flushLine();
            // Слово шире строки (длинный путь в узкой ячейке) — рубим по буквам.
            while (inkW > width && token.size() > 1) {
                size_t fit = token.size() - 1;
                while (fit > 1 && TokenWidth(g, token.substr(0, fit), font) > width - cx) fit--;
                const std::wstring head = token.substr(0, fit);
                const float hw = TokenWidth(g, head, font);
                line.push_back(Piece{head, cx, hw, hw, font, run.link, run.id, run.style});
                flushLine();
                token.erase(0, fit);
                w = TokenWidth(g, token, font);
                inkW = w;
            }
            line.push_back(Piece{token, cx, w, inkW, font, run.link, run.id, run.style});
            cx += w;
        }
    }
    if (!line.empty()) flushLine();
    return cy;
}

Bitmap* Document::Scaled(Bitmap* src, int w, int h) {
    auto it = scaled_.find(src);
    if (it != scaled_.end() && static_cast<int>(it->second->GetWidth()) == w &&
        static_cast<int>(it->second->GetHeight()) == h) {
        return it->second.get();
    }
    std::unique_ptr<Bitmap> out(new Bitmap(w, h, PixelFormat32bppPARGB));
    {
        Graphics g(out.get());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        // Скруглённые углы — как у карточек самих виджетов.
        GraphicsPath* path = gfx::RoundRectPath(RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)),
                                                ScF(8.0f, dpi_));
        TextureBrush brush(src, WrapModeClamp);
        brush.ScaleTransform(static_cast<REAL>(w) / src->GetWidth(), static_cast<REAL>(h) / src->GetHeight());
        g.FillPath(&brush, path);
        delete path;
    }
    Bitmap* raw = out.get();
    scaled_[src] = std::move(out);
    return raw;
}

void Document::Layout(Graphics& g, float width, int dpi) {
    if (width == width_ && dpi == dpi_) return;
    if (dpi != dpi_ || fonts_.empty()) BuildFonts(dpi);
    width_ = width;
    dpi_ = dpi;
    prims_.clear();

    const float W = width;
    float y = 0.0f;
    auto gap = [&](float v) { return ScF(v, dpi); };

    for (size_t bi = 0; bi < blocks_.size(); bi++) {
        const Block& b = blocks_[bi];
        const bool nextIsList = bi + 1 < blocks_.size() &&
                                (blocks_[bi + 1].kind == Kind::Bullet || blocks_[bi + 1].kind == Kind::Numbered);
        switch (b.kind) {
            case Kind::Heading: {
                if (y > 0.0f) y += gap(b.level == 2 ? 26.0f : 14.0f);
                const float h = Wrap(g, b.runs, 0.0f, y, W, b.level == 2 ? kH2 : kH3, true);
                y += h;
                if (b.level == 2) {
                    Prim line;
                    line.type = Prim::Line;
                    line.color = kRule;
                    line.r = RectF(0.0f, y + gap(4.0f), W, 0.0f);
                    prims_.push_back(line);
                    y += gap(14.0f);
                } else {
                    y += gap(6.0f);
                }
                break;
            }
            case Kind::Paragraph:
                y += Wrap(g, b.runs, 0.0f, y, W, kBody, true) + gap(10.0f);
                break;
            case Kind::Bullet:
            case Kind::Numbered: {
                const float indent = gap(22.0f) * static_cast<float>(1 + b.level);
                const float lh = lineH_[kBody];
                Prim marker;
                if (b.kind == Kind::Bullet) {
                    const float d = gap(5.0f);
                    marker.type = Prim::Dot;
                    marker.color = kMuted;
                    marker.r = RectF(indent - gap(13.0f), y + (lh - d) / 2.0f, d, d);
                } else {
                    marker.type = Prim::Text;
                    marker.font = kBody;
                    marker.color = kMuted;
                    const float nw = TokenWidth(g, b.number, kBody);
                    marker.r = RectF(indent - gap(6.0f) - nw, y + (lh - fonts_[kBody]->GetSize() * 1.33f) / 2.0f,
                                     nw, lh);
                    marker.text = b.number;
                }
                prims_.push_back(marker);
                y += Wrap(g, b.runs, indent, y, W - indent, kBody, true) + gap(nextIsList ? 3.0f : 12.0f);
                break;
            }
            case Kind::Quote: {
                const float pad = gap(14.0f);
                const size_t barAt = prims_.size();
                const float h = Wrap(g, b.runs, pad, y, W - pad, kBody, true);
                for (size_t k = barAt; k < prims_.size(); k++) {
                    if (prims_[k].type == Prim::Text && prims_[k].color == kPrimary) prims_[k].color = kMuted;
                }
                Prim bar;
                bar.type = Prim::Fill;
                bar.color = kQuoteBar;
                bar.radius = gap(1.5f);
                bar.r = RectF(0.0f, y, gap(3.0f), h);
                prims_.push_back(bar);
                y += h + gap(12.0f);
                break;
            }
            case Kind::Rule: {
                Prim line;
                line.type = Prim::Line;
                line.color = kRule;
                line.r = RectF(0.0f, y + gap(8.0f), W, 0.0f);
                prims_.push_back(line);
                y += gap(18.0f);
                break;
            }
            case Kind::Code: {
                const float pad = gap(12.0f);
                const float lh = lineH_[kCode];
                const float h = lh * static_cast<float>(b.code.size()) + pad * 2.0f;
                Prim bg;
                bg.type = Prim::Fill;
                bg.color = kCodeBg;
                bg.radius = gap(8.0f);
                bg.r = RectF(0.0f, y, W, h);
                prims_.push_back(bg);
                for (size_t k = 0; k < b.code.size(); k++) {
                    Prim t;
                    t.type = Prim::Text;
                    t.font = kCode;
                    t.color = kPrimary;
                    t.text = gfx::Ellipsize(g, b.code[k], *fonts_[kCode], W - pad * 2.0f);
                    t.r = RectF(pad, y + pad + lh * static_cast<float>(k) + (lh - fonts_[kCode]->GetSize() * 1.33f) / 2.0f,
                                W - pad * 2.0f, lh);
                    prims_.push_back(t);
                }
                y += h + gap(12.0f);
                break;
            }
            case Kind::Image: {
                Bitmap* src = entry_ ? catalog::Image(*entry_, b.image) : nullptr;
                if (!src || src->GetWidth() == 0) break;
                // Скриншот показываем не крупнее, чем он есть: увеличенный он только мылится.
                const float natural = static_cast<float>(src->GetWidth());
                const int w = static_cast<int>(min(W, natural));
                const int h = static_cast<int>(static_cast<float>(w) * src->GetHeight() / src->GetWidth());
                Prim img;
                img.type = Prim::Image;
                img.image = Scaled(src, w, h);
                img.r = RectF(0.0f, y, static_cast<REAL>(w), static_cast<REAL>(h));
                prims_.push_back(img);
                Prim frame;
                frame.type = Prim::Stroke;
                frame.color = kRule;
                frame.radius = gap(8.0f);
                frame.r = img.r;
                prims_.push_back(frame);
                y += static_cast<float>(h) + gap(16.0f);
                break;
            }
            case Kind::Table: {
                size_t cols = 0;
                for (const auto& row : b.rows) cols = max(cols, row.size());
                if (cols == 0) break;
                const float pad = gap(9.0f);

                // Естественная ширина колонки — самая длинная ячейка в одну строку.
                std::vector<float> natural(cols, gap(40.0f));
                for (size_t r = 0; r < b.rows.size(); r++) {
                    const int font = r == 0 ? kSmallBold : kSmall;
                    for (size_t c = 0; c < b.rows[r].size(); c++) {
                        float w = 0.0f;
                        for (const Run& run : b.rows[r][c]) w += TokenWidth(g, run.text, FontFor(run.style, font));
                        natural[c] = max(natural[c], w + pad * 2.0f + 2.0f);
                    }
                }
                std::vector<float> colW(cols);
                float total = 0.0f;
                for (float v : natural) total += v;
                if (total <= W) {
                    colW = natural;
                } else {
                    // Не влезает: каждой колонке — не больше честной доли, остаток — тем, кому мало.
                    const float fair = W / static_cast<float>(cols);
                    float used = 0.0f, deficit = 0.0f;
                    for (size_t c = 0; c < cols; c++) {
                        colW[c] = min(natural[c], fair);
                        used += colW[c];
                        deficit += natural[c] - colW[c];
                    }
                    const float spare = W - used;
                    for (size_t c = 0; c < cols && deficit > 0.0f; c++) {
                        colW[c] += spare * (natural[c] - colW[c]) / deficit;
                    }
                }
                float tableW = 0.0f;
                for (float v : colW) tableW += v;

                const float top = y;
                for (size_t r = 0; r < b.rows.size(); r++) {
                    const int font = r == 0 ? kSmallBold : kSmall;
                    float rowH = 0.0f;
                    for (size_t c = 0; c < b.rows[r].size(); c++) {
                        rowH = max(rowH, Wrap(g, b.rows[r][c], 0.0f, 0.0f, colW[c] - pad * 2.0f, font, false));
                    }
                    rowH += pad * 2.0f - gap(4.0f);
                    if (r == 0) {
                        Prim head;
                        head.type = Prim::Fill;
                        head.color = kHeadBg;
                        head.radius = 0.0f;
                        head.r = RectF(1.0f, y + 1.0f, tableW - 2.0f, rowH - 1.0f);
                        prims_.push_back(head);
                    }
                    float x = 0.0f;
                    for (size_t c = 0; c < cols; c++) {
                        if (c < b.rows[r].size()) {
                            Wrap(g, b.rows[r][c], x + pad, y + pad - gap(2.0f), colW[c] - pad * 2.0f, font, true);
                        }
                        x += colW[c];
                    }
                    y += rowH;
                    if (r + 1 < b.rows.size()) {
                        Prim line;
                        line.type = Prim::Line;
                        line.color = kRule;
                        line.r = RectF(0.0f, y, tableW, 0.0f);
                        prims_.push_back(line);
                    }
                }
                float x = 0.0f;
                for (size_t c = 0; c + 1 < cols; c++) {
                    x += colW[c];
                    Prim line;
                    line.type = Prim::Line;
                    line.color = kRule;
                    line.r = RectF(x, top, 0.0f, y - top);
                    prims_.push_back(line);
                }
                Prim frame;
                frame.type = Prim::Stroke;
                frame.color = kRule;
                frame.radius = gap(6.0f);
                frame.r = RectF(0.0f, top, tableW, y - top);
                prims_.push_back(frame);
                y += gap(14.0f);
                break;
            }
        }
    }
    height_ = y;
}

void Document::Draw(Graphics& g, float x, float y, float clipTop, float clipBottom) const {
    const Palette& pal = P();
    auto color = [&](uint8_t c) -> ARGB {
        switch (c) {
            case kMuted: return pal.textMuted;
            case kAccent: return pal.accent;
            case kCodeBg: return pal.dark ? 0x16FFFFFF : 0x0E000000;
            case kRule: return pal.dark ? 0x1FFFFFFF : 0x1C000000;
            case kHeadBg: return pal.dark ? 0x0DFFFFFF : 0x09000000;
            case kQuoteBar: return WithAlpha(pal.accent, 0xB0);
            default: return pal.textPrimary;
        }
    };

    for (const Prim& p : prims_) {
        const float top = y + p.r.Y;
        const float bottom = top + p.r.Height;
        if (bottom < clipTop - 2.0f || top > clipBottom + 2.0f) continue;
        const RectF r(x + p.r.X, top, p.r.Width, p.r.Height);
        switch (p.type) {
            case Prim::Text:
                gfx::DrawText(g, p.text.c_str(), *fonts_[p.font], color(p.color), r.X, r.Y);
                break;
            case Prim::Fill:
                gfx::FillRoundRect(g, r, p.radius, color(p.color));
                break;
            case Prim::Stroke:
                gfx::StrokeRoundRect(g, r, p.radius, color(p.color), 1.0f);
                break;
            case Prim::Line: {
                Pen pen(Color(color(p.color)), 1.0f);
                g.DrawLine(&pen, r.X, r.Y, r.X + r.Width, r.Y + r.Height);
                break;
            }
            case Prim::Dot: {
                const Color c(color(p.color));
                SolidBrush brush(c);
                g.FillEllipse(&brush, r);
                break;
            }
            case Prim::Image:
                g.DrawImage(p.image, static_cast<INT>(r.X + 0.5f), static_cast<INT>(r.Y + 0.5f),
                            static_cast<INT>(p.r.Width), static_cast<INT>(p.r.Height));
                break;
            case Prim::Link:
                break;
        }
    }
}

std::wstring Document::LinkAt(float x, float y) const {
    for (const Prim& p : prims_) {
        if (p.type == Prim::Link && p.r.Contains(x, y)) {
            if (p.link >= 0 && p.link < static_cast<int>(links_.size())) return links_[static_cast<size_t>(p.link)];
        }
    }
    return std::wstring();
}

}  // namespace md
