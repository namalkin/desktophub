#include <windows.h>
#include <objidl.h>
#include <ole2.h>

#include <algorithm>
using std::max;
using std::min;

#include <gdiplus.h>

#include "widget.h"

// -mwindows: точка входа WinMain, консольное окно не создаётся.
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    // Второй экземпляр виджета не нужен.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\ShortcutDock_SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"ShortcutDockApp", nullptr)) {
            // Виджет может быть свёрнут в трей — просим его показаться.
            PostMessageW(existing, RegisterWindowMessageW(L"ShortcutDock_Show"), 0, 0);
            SetForegroundWindow(existing);
        }
        CloseHandle(single);
        return 0;
    }

    // OleInitialize, а не CoInitialize: без него нет ни RegisterDragDrop, ни буфера обмена.
    if (FAILED(OleInitialize(nullptr))) {
        MessageBoxW(nullptr, L"Не удалось инициализировать COM.", L"Shortcut Dock", MB_ICONERROR);
        return 1;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Не удалось инициализировать GDI+.", L"Shortcut Dock", MB_ICONERROR);
        OleUninitialize();
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
        MessageBoxW(nullptr, L"Не удалось создать окно виджета.", L"Shortcut Dock", MB_ICONERROR);
        exitCode = 1;
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    OleUninitialize();
    if (single) CloseHandle(single);
    return exitCode;
}
