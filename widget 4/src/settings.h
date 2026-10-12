// Окно настроек: не модальное, всё применяется сразу — видно, как меняются блоки
// на рабочем столе.
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
    virtual void PickDrive(wchar_t letter) = 0;  // сменить диск и забыть старый результат
    virtual void StartScan() = 0;
    virtual void OpenReport() = 0;
};

// Два разных окна по смыслу, одно по коду:
//   Panel  — оформление и содержимое одного блока (шестерёнка в его заголовке);
//   Global — блоки на экране, диск, правила поиска, общие переключатели (значок в трее).
enum class Mode { Panel, Global };

void Show(HINSTANCE inst, Host* host, int panel, POINT nearScreenPt, Mode mode = Mode::Panel);
void Close();
bool IsOpen();
void Refresh();  // данные изменились снаружи (например, закончилось сканирование)

}  // namespace settings
