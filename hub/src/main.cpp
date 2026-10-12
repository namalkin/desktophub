#include <objbase.h>
#include <objidl.h>
#include <windows.h>

#include <algorithm>
using std::max;
using std::min;

#include <gdiplus.h>

#include <cwchar>
#include <cwctype>

#include "hub.h"
#include "hub_link.h"
#include "util.h"

// -mwindows: точка входа WinMain, консольное окно не создаётся.
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    // После самообновления новый центр запускает старый с --wait-pid=N: ждём, пока тот
    // выйдет и освободит мьютекс, иначе новый решил бы, что он второй экземпляр.
    if (const wchar_t* wait = wcsstr(GetCommandLineW(), L"--wait-pid=")) {
        const DWORD pid = static_cast<DWORD>(wcstoul(wait + 11, nullptr, 10));
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(old, 15000);
            CloseHandle(old);
        }
    }
    // Прошлая версия после самообновления осталась рядом как WidgetHub.exe.old.
    DeleteFileW((ExePath() + L".old").c_str());

    // Второй центр не нужен: два трея и две очереди команд виджетам только мешали бы.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\WidgetHub_SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(hub::kHubClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, HubShowMessage(), 0, 0);
        }
        CloseHandle(single);
        return 0;
    }

    // Из автозапуска центр стартует свёрнутым в трей.
    const bool startHidden = wcsstr(GetCommandLineW(), L"--tray") != nullptr;

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        MessageBoxW(nullptr, L"Не удалось инициализировать COM.", L"Центр виджетов", MB_ICONERROR);
        return 1;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Не удалось инициализировать GDI+.", L"Центр виджетов", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    int exitCode = 0;
    if (CreateHubWindow(inst, startHidden)) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = static_cast<int>(msg.wParam);
    } else {
        MessageBoxW(nullptr, L"Не удалось создать окно центра.", L"Центр виджетов", MB_ICONERROR);
        exitCode = 1;
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    if (single) CloseHandle(single);
    return exitCode;
}
