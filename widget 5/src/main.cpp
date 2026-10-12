#include <objbase.h>
#include <objidl.h>
#include <windows.h>

#include <algorithm>
using std::max;
using std::min;

#include <gdiplus.h>

#include "widget.h"

// -mwindows: точка входа WinMain, консольное окно не создаётся.
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    // Второй экземпляр не нужен: два плеера играли бы друг поверх друга.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\TuneBox_SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Ищем скрытое окно-владельца: блок — дочернее окно рабочего стола, и FindWindow,
        // который видит только окна верхнего уровня, его не находит.
        if (HWND existing = FindWindowW(L"TuneBoxOwner", nullptr)) {
            // Плеер может быть свёрнут в трей — просим его показаться.
            PostMessageW(existing, RegisterWindowMessageW(L"TuneBox_Show"), 0, 0);
            SetForegroundWindow(existing);
        }
        CloseHandle(single);
        return 0;
    }

    // COM нужен ради SHGetKnownFolderPath, обложек, тегов и диалогов выбора.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        MessageBoxW(nullptr, L"Не удалось инициализировать COM.", L"Плеер", MB_ICONERROR);
        return 1;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Не удалось инициализировать GDI+.", L"Плеер", MB_ICONERROR);
        CoUninitialize();
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
        MessageBoxW(nullptr, L"Не удалось создать окно плеера.", L"Плеер", MB_ICONERROR);
        exitCode = 1;
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    if (single) CloseHandle(single);
    return exitCode;
}
