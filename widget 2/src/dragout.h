// Перетаскивание значков из блока наружу — на рабочий стол или в папку.
// Здесь виджет выступает источником OLE-перетаскивания, а не приёмником.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace dragout {

// Запускает системное перетаскивание файлов и ждёт, пока пользователь отпустит кнопку.
// Возвращает DROPEFFECT_* цели (DROPEFFECT_MOVE — файл забрали) или 0, если отменено.
DWORD DragFiles(const std::vector<std::wstring>& paths);

}  // namespace dragout
