#include "control.h"

#include "sys.h"
#include "util.h"

namespace control {

namespace {

std::wstring ProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::wstring();
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD size = MAX_PATH * 2;
    std::wstring path;
    if (QueryFullProcessImageNameW(process, 0, buf, &size)) path.assign(buf, size);
    CloseHandle(process);
    return path;
}

}  // namespace

Status Probe(const catalog::Entry& entry) {
    Status s;
    s.window = FindWindowW(entry.windowClass.c_str(), nullptr);
    if (!s.window) return s;
    s.running = true;
    GetWindowThreadProcessId(s.window, &s.pid);
    s.processPath = ProcessPath(s.pid);

    // Ответ ждём недолго: зависший виджет не должен подвешивать центр.
    DWORD_PTR answer = 0;
    if (SendMessageTimeoutW(s.window, hub::MessageId(), hub::kQuery, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300,
                            &answer)) {
        s.linked = (answer & hub::kAlive) != 0;
        s.visible = (answer & hub::kVisible) != 0;
    }
    return s;
}

std::wstring ResolvePath(const catalog::Entry& entry, const std::wstring& remembered, const Status& status) {
    if (!status.processPath.empty()) return status.processPath;
    if (FileExists(remembered)) return remembered;

    const std::wstring fromRun = RunEntry(entry.runValue);
    if (FileExists(fromRun) && EqualsNoCase(FileName(fromRun), entry.exe)) return fromRun;

    const std::wstring here = ExeDir();
    const std::wstring candidates[] = {
        InstallDir() + L"\\" + entry.exe,
        here + L"\\" + entry.exe,
        // Центр запущен прямо из репозитория (hub\dist) — берём свежую сборку виджета.
        NormalizePath(here + L"\\..\\..\\" + entry.folder + L"\\dist\\" + entry.exe),
        NormalizePath(here + L"\\..\\" + entry.folder + L"\\dist\\" + entry.exe),
    };
    for (const std::wstring& path : candidates) {
        if (FileExists(path)) return path;
    }
    return std::wstring();
}

bool Launch(const std::wstring& exePath, std::wstring& error) {
    if (!FileExists(exePath)) {
        error = L"Файл не найден: " + exePath;
        return false;
    }
    const std::wstring dir = ParentDir(exePath);
    std::wstring cmd = L"\"" + exePath + L"\"";

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exePath.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        wchar_t buf[64];
        swprintf(buf, 64, L"Не удалось запустить (код %lu)", GetLastError());
        error = buf;
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void Post(const Status& status, WPARAM command) {
    if (!status.window) return;
    if (command == hub::kSettings || command == hub::kShow || command == hub::kToggle) {
        AllowSetForegroundWindow(status.pid);
    }
    PostMessageW(status.window, hub::MessageId(), command, 0);
}

bool StopAndWait(const catalog::Entry& entry, DWORD timeoutMs) {
    HWND window = FindWindowW(entry.windowClass.c_str(), nullptr);
    if (!window) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(window, hub::MessageId(), hub::kExit, 0);
    bool done = false;
    if (process) {
        done = WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0;
        CloseHandle(process);
    } else {
        // Процесс не открыть — смотрим хотя бы на окно.
        const ULONGLONG until = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < until && IsWindow(window)) Sleep(50);
        done = !IsWindow(window);
    }
    return done;
}

}  // namespace control
