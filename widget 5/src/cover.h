// Обложка трека. Своего разбора теговых картинок нет и не нужно: оболочка Windows уже
// отдаёт встроенную обложку как эскиз файла — ровно ту же, что видно в проводнике.
#pragma once

#include <windows.h>
#include <objidl.h>

#include <algorithm>
using std::max;
using std::min;

#include <gdiplus.h>

#include <string>

namespace cover {

// nullptr — обложки нет (тогда блок рисует свою заглушку). Результат кэшируется,
// поэтому возвращённый Bitmap принадлежит кэшу и удалять его нельзя.
Gdiplus::Bitmap* Get(const std::wstring& path, int px);

void Clear();

}  // namespace cover
