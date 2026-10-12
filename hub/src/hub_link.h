// Связь с «Центром виджетов». Один и тот же файл лежит в каждом виджете и в самом центре —
// меняя его, меняйте во всех копиях.
//
// Центр находит виджет по классу его скрытого окна верхнего уровня (того, что держит трей)
// и шлёт ему зарегистрированное сообщение MessageId() с командой в wParam. Команды с
// интерфейсом (меню, окно настроек) приходят через PostMessage — иначе центр ждал бы,
// пока пользователь закроет меню виджета.
#pragma once

#include <windows.h>

namespace hub {

constexpr wchar_t kHubClass[] = L"DesktopWidgetsHub";  // главное окно центра

enum Command : WPARAM {
    kShow = 1,      // показать блоки
    kHide = 2,      // спрятать блоки
    kToggle = 3,    // показать / спрятать
    kSettings = 4,  // открыть настройки виджета у курсора
    kExit = 5,      // закрыться, сохранив данные
    kTrayOn = 6,    // вернуть свой значок в трей (центр закрывается)
    kTrayOff = 7,   // убрать значок: трей держит центр
    kQuery = 8,     // SendMessage: ответ kAlive | (kVisible, если блоки на экране)
};

constexpr LRESULT kAlive = 0x100;  // старый виджет без протокола ответит 0
constexpr LRESULT kVisible = 0x1;

inline UINT MessageId() {
    static const UINT id = RegisterWindowMessageW(L"DesktopWidgets_Hub");
    return id;
}

// Запущенный центр сам показывает всё в своём трее, поэтому виджет свой значок не ставит.
inline bool HubRunning() { return FindWindowW(kHubClass, nullptr) != nullptr; }

}  // namespace hub
