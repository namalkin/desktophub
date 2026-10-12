#include <windows.h>
#include <objidl.h>

#include <algorithm>
using std::max;
using std::min;

#include <gdiplus.h>

#include "widget.h"

// -mwindows: точка входа WinMain, консольное окно не создаётся.
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    // Второй экземпляр виджета не нужен.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\NewYearWidget_SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Ищем скрытое окно-владельца: сам виджет — дочернее окно рабочего стола, и
        // FindWindow, который видит только окна верхнего уровня, его не находит.
        if (HWND existing = FindWindowW(L"NewYearWidgetOwner", nullptr)) {
            // Виджет может быть свёрнут в трей — просим его показаться.
            PostMessageW(existing, RegisterWindowMessageW(L"NewYearWidget_Show"), 0, 0);
            SetForegroundWindow(existing);
        }
        CloseHandle(single);
        return 0;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Не удалось инициализировать GDI+.", L"New Year Widget", MB_ICONERROR);
        return 1;
    }

    int exitCode = 0;
    if (CreateWidgetWindow(inst)) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = static_cast<int>(msg.wParam);
    } else {
        MessageBoxW(nullptr, L"Не удалось создать окно виджета.", L"New Year Widget", MB_ICONERROR);
        exitCode = 1;
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    if (single) CloseHandle(single);
    return exitCode;
}
