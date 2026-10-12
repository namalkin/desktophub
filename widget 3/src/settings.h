// Окно настроек: не модальное, все переключатели применяются сразу — видно, как
// меняются блоки на рабочем столе.
#pragma once

#include <windows.h>

#include "store.h"

namespace settings {

// Окно настроек ничего не знает про виджет и дёргает его через этот интерфейс.
class Host {
public:
    virtual ~Host() = default;

    virtual AppData& Data() = 0;
    virtual bool HasPanel(int panel) = 0;
    virtual void Save() = 0;
    virtual void ApplyPanelLook(int panel) = 0;   // тема, акцент, непрозрачность — перерисовка блока
    virtual void ApplyStyleToAll(int panel) = 0;  // оформление этого блока — всем остальным
    virtual void ApplyTopmost() = 0;
    virtual void ApplyHotkey() = 0;
    virtual void RefreshAll() = 0;
    virtual void AddPanel(PanelKind kind) = 0;
    virtual void RemovePanels(PanelKind kind) = 0;
    virtual void ArrangePanels() = 0;
};

// Два разных окна по смыслу, одно по коду:
//   Panel  — оформление и содержимое одного блока (шестерёнка в его заголовке);
//   Global — блоки на экране, шрифт, общие переключатели и данные (значок в трее).
enum class Mode { Panel, Global };

// panel — блок, настройки которого правим: окно оформлено в его теме.
void Show(HINSTANCE inst, Host* host, int panel, POINT nearScreenPt, Mode mode = Mode::Panel);
void Close();
bool IsOpen();

}  // namespace settings
