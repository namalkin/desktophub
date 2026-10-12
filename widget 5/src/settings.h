// Окно настроек: не модальное, всё применяется сразу — видно, как меняется блок
// на рабочем столе.
#pragma once

#include <windows.h>

#include <string>

#include "store.h"

namespace settings {

// Окно настроек ничего не знает про плеер и дёргает его через этот интерфейс.
class Host {
public:
    virtual ~Host() = default;

    virtual AppData& Data() = 0;
    virtual void Save() = 0;
    virtual void ApplyLook() = 0;      // тема, акцент, непрозрачность, размеры — перерисовка блока
    virtual void ApplyTopmost() = 0;
    virtual void ApplyHotkeys() = 0;
    virtual void ApplyVolume() = 0;
    virtual void ApplyNameMode() = 0;  // сменился способ разбора имени файла
    virtual void ChooseFolder(HWND owner) = 0;
    virtual void Rescan() = 0;
    virtual std::wstring StatusLine() = 0;  // «410 треков · читаю теги…»
};

void Show(HINSTANCE inst, Host* host, POINT nearScreenPt);
void Close();
bool IsOpen();
void Refresh();  // данные изменились снаружи (дочитались теги, сменился трек)

}  // namespace settings
