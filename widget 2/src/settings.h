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
    virtual bool HasGroup(int group) = 0;
    virtual void Save() = 0;
    virtual void ApplyBlockLook(int group) = 0;    // тема, акцент, непрозрачность — перерисовка блока
    virtual void ApplyBlockLayout(int group) = 0;  // размер значков, сетка, подписи — пересчёт блока
    virtual void ApplyStyleToAll(int group) = 0;   // оформление этого блока — всем остальным
    virtual void ApplyTopmost() = 0;
    virtual void ApplyHotkey() = 0;
    virtual void RefreshIcons() = 0;
    virtual void AddGroup() = 0;
};

// Два разных окна по смыслу, одно по коду:
//   Panel  — оформление и сетка одного блока (шестерёнка в его заголовке);
//   Global — общие переключатели, данные и «новый блок» (значок в трее).
enum class Mode { Panel, Global };

// group — блок, настройки которого правим: окно оформлено в его теме.
void Show(HINSTANCE inst, Host* host, int group, POINT nearScreenPt, Mode mode = Mode::Panel);
void Close();
bool IsOpen();

}  // namespace settings
