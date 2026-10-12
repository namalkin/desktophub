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
    // Второй экземпляр планера не нужен: он писал бы в тот же data.json.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\PlannerGo_SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"PlannerGoApp", nullptr)) {
            // Виджет может быть свёрнут в трей — просим его показаться.
            PostMessageW(existing, RegisterWindowMessageW(L"PlannerGo_Show"), 0, 0);
            SetForegroundWindow(existing);
        }
        CloseHandle(single);
        return 0;
    }

    // COM нужен ради SHGetKnownFolderPath и ShellExecute.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        MessageBoxW(nullptr, L"Не удалось инициализировать COM.", L"Планер GO", MB_ICONERROR);
        return 1;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Не удалось инициализировать GDI+.", L"Планер GO", MB_ICONERROR);
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
        MessageBoxW(nullptr, L"Не удалось создать окно виджета.", L"Планер GO", MB_ICONERROR);
        exitCode = 1;
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    if (single) CloseHandle(single);
    return exitCode;
}
