// Приём перетаскивания из проводника: полноценный IDropTarget, а не WM_DROPFILES,
// потому что подсветка группы под курсором нужна ещё до отпускания кнопки.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

class DropSink {
public:
    virtual ~DropSink() = default;

    // pt — клиентские координаты окна. Возвращает true, если бросать сюда можно.
    virtual bool OnDragOver(POINT pt, bool copyMode) = 0;
    virtual void OnDragLeave() = 0;

    // copyMode — зажат Ctrl: ярлык копируется, а не забирается с рабочего стола.
    virtual void OnDrop(const std::vector<std::wstring>& paths, POINT pt, bool copyMode) = 0;
};

bool RegisterDrops(HWND hwnd, DropSink* sink);
void UnregisterDrops(HWND hwnd);
