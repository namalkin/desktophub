// Значки файлов: берём у оболочки Windows и держим в кэше, чтобы не дёргать
// её на каждой перерисовке.
#pragma once

#include <string>

#include "ui.h"

namespace icons {

// Значок для пути нужного размера в пикселях. nullptr — значка нет (URL,
// недоступный файл); виджет в этом случае рисует запасной кружок сам.
Gdiplus::Bitmap* Get(const std::wstring& path, int px);

// Сброс кэша: смена DPI, размера значков или пункт «Обновить значки».
void Clear();

}  // namespace icons
