#include "download.h"

#include <shlobj.h>
#include <wininet.h>

#include <map>

#include "control.h"
#include "util.h"

namespace download {

namespace {

struct Job {
    HWND notify = nullptr;
    int index = 0;
    catalog::Entry entry;
    std::wstring url;
    std::wstring dest;
    bool self = false;  // обновляется сам центр
    bool busy = false;
    int percent = -1;
    std::wstring result;
};

// MinGW с win32-потоками не даёт std::thread и std::mutex — обходимся WinAPI.
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
std::map<int, Job> g_jobs;

struct Guard {
    Guard() { EnterCriticalSection(&g_lock); }
    ~Guard() { LeaveCriticalSection(&g_lock); }
};

void EnsureLock() {
    if (g_lockReady) return;
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
}

std::wstring KiB(unsigned long long bytes) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%llu КБ", (bytes + 1023) / 1024);
    return buf;
}

// Возвращает пустую строку при успехе, иначе — понятное пользователю сообщение.
std::wstring Fetch(Job& job, const std::wstring& tmp, unsigned long long& total) {
    HINTERNET net = InternetOpenW(L"WidgetHub/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) return L"Нет доступа к интернету.";

    // GitHub отвечает редиректом на хранилище файлов — WinINet идёт по нему сам.
    const DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI |
                        INTERNET_FLAG_NO_COOKIES;
    HINTERNET url = InternetOpenUrlW(net, job.url.c_str(), nullptr, 0, flags, 0);
    if (!url) {
        const DWORD err = GetLastError();
        InternetCloseHandle(net);
        wchar_t buf[96];
        swprintf(buf, 96, L"Не удалось соединиться с GitHub (код %lu).", err);
        return buf;
    }

    DWORD status = 0;
    DWORD len = sizeof(status);
    HttpQueryInfoW(url, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr);
    if (status != 200) {
        InternetCloseHandle(url);
        InternetCloseHandle(net);
        if (status == 404) return L"В последнем релизе нет файла " + job.entry.exe + L" — или репозиторий указан неверно.";
        wchar_t buf[64];
        swprintf(buf, 64, L"GitHub ответил кодом %lu.", status);
        return buf;
    }

    DWORD length = 0;
    len = sizeof(length);
    if (!HttpQueryInfoW(url, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &length, &len, nullptr)) {
        length = 0;
    }

    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(url);
        InternetCloseHandle(net);
        return L"Не удалось создать файл в " + ParentDir(tmp);
    }

    std::wstring error;
    char buf[64 * 1024];
    total = 0;
    int lastPercent = -2;
    bool sawHeader = false;
    for (;;) {
        DWORD got = 0;
        if (!InternetReadFile(url, buf, sizeof(buf), &got)) {
            error = L"Загрузка оборвалась.";
            break;
        }
        if (got == 0) break;
        if (!sawHeader) {
            // Вместо программы мог прийти HTML (страница входа, ошибка) — такое не ставим.
            if (got < 2 || buf[0] != 'M' || buf[1] != 'Z') {
                error = L"Скачался не .exe — проверьте репозиторий.";
                break;
            }
            sawHeader = true;
        }
        DWORD written = 0;
        if (!WriteFile(file, buf, got, &written, nullptr) || written != got) {
            error = L"Не хватило места на диске.";
            break;
        }
        total += got;
        const int percent = length > 0 ? static_cast<int>(total * 100 / length) : -1;
        if (percent != lastPercent) {
            lastPercent = percent;
            {
                Guard g;
                g_jobs[job.index].percent = percent;
            }
            PostMessageW(job.notify, kMsgProgress, static_cast<WPARAM>(job.index), percent);
        }
    }
    CloseHandle(file);
    InternetCloseHandle(url);
    InternetCloseHandle(net);

    if (error.empty() && total < 16 * 1024) error = L"Файл подозрительно маленький — загрузка не удалась.";
    return error;
}

DWORD WINAPI Worker(LPVOID param) {
    Job job;
    {
        Guard g;
        job = g_jobs[static_cast<int>(reinterpret_cast<INT_PTR>(param))];
    }

    SHCreateDirectoryExW(nullptr, ParentDir(job.dest).c_str(), nullptr);
    const std::wstring tmp = job.dest + L".download";
    unsigned long long total = 0;
    std::wstring error = Fetch(job, tmp, total);

    if (job.self) {
        if (error.empty()) {
            const std::wstring old = job.dest + L".old";
            DeleteFileW(old.c_str());
            if (!MoveFileExW(job.dest.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                error = L"Не удалось отодвинуть текущий WidgetHub.exe.";
            } else if (!MoveFileExW(tmp.c_str(), job.dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                MoveFileExW(old.c_str(), job.dest.c_str(), MOVEFILE_REPLACE_EXISTING);  // вернуть как было
                error = L"Не удалось поставить новый WidgetHub.exe.";
            }
        }
        if (!error.empty()) DeleteFileW(tmp.c_str());
        {
            Guard g;
            Job& slot = g_jobs[job.index];
            slot.busy = false;
            slot.result = error.empty() ? L"Центр обновлён, перезапускаю…" : error;
        }
        PostMessageW(job.notify, kMsgDone, static_cast<WPARAM>(job.index), error.empty() ? 1 : 0);
        return 0;
    }

    bool wasRunning = false;
    if (error.empty()) {
        // Работающий .exe не перезаписать: просим виджет закрыться — он сохранит данные.
        wasRunning = FindWindowW(job.entry.windowClass.c_str(), nullptr) != nullptr;
        if (wasRunning && !control::StopAndWait(job.entry, 6000)) {
            error = L"Виджет не закрылся — закройте его вручную и повторите.";
        }
    }
    if (error.empty()) {
        bool moved = false;
        for (int attempt = 0; attempt < 10 && !moved; attempt++) {
            // Файл ещё может держать антивирус или только что закрывшийся процесс.
            moved = MoveFileExW(tmp.c_str(), job.dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!moved) Sleep(300);
        }
        if (!moved) error = L"Не удалось заменить " + job.dest;
    }
    if (!error.empty()) DeleteFileW(tmp.c_str());

    std::wstring result;
    if (error.empty()) {
        result = (wasRunning ? L"Обновлено, " : L"Скачано, ") + KiB(total);
        if (wasRunning) {
            std::wstring launchError;
            if (!control::Launch(job.dest, launchError)) result += L". " + launchError;
        }
    } else {
        result = error;
    }

    {
        Guard g;
        Job& slot = g_jobs[job.index];
        slot.busy = false;
        slot.result = result;
    }
    PostMessageW(job.notify, kMsgDone, static_cast<WPARAM>(job.index), error.empty() ? 1 : 0);
    return 0;
}

}  // namespace

std::wstring ReleaseUrl(const std::wstring& repo, const std::wstring& exe) {
    return L"https://github.com/" + repo + L"/releases/latest/download/" + exe;
}

bool Start(HWND notify, int index, const catalog::Entry& entry, const std::wstring& url, const std::wstring& dest) {
    EnsureLock();
    {
        Guard g;
        Job& job = g_jobs[index];
        if (job.busy) return false;
        job = Job{};
        job.notify = notify;
        job.index = index;
        job.entry = entry;
        job.url = url;
        job.dest = dest;
        job.self = index == kSelfIndex;
        job.busy = true;
    }
    HANDLE thread = CreateThread(nullptr, 0, &Worker, reinterpret_cast<LPVOID>(static_cast<INT_PTR>(index)), 0, nullptr);
    if (!thread) {
        Guard g;
        g_jobs[index].busy = false;
        g_jobs[index].result = L"Не удалось начать загрузку.";
        return false;
    }
    CloseHandle(thread);
    return true;
}

bool StartSelf(HWND notify, const std::wstring& url) {
    catalog::Entry self;
    self.exe = L"WidgetHub.exe";
    return Start(notify, kSelfIndex, self, url, ExePath());  // Start сам помечает задание как self
}

bool Busy(int index) {
    if (!g_lockReady) return false;
    Guard g;
    auto it = g_jobs.find(index);
    return it != g_jobs.end() && it->second.busy;
}

int Percent(int index) {
    if (!g_lockReady) return -1;
    Guard g;
    auto it = g_jobs.find(index);
    return it == g_jobs.end() ? -1 : it->second.percent;
}

std::wstring TakeResult(int index) {
    if (!g_lockReady) return std::wstring();
    Guard g;
    auto it = g_jobs.find(index);
    if (it == g_jobs.end()) return std::wstring();
    std::wstring r = it->second.result;
    it->second.result.clear();
    return r;
}

}  // namespace download
