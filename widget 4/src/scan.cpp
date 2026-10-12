#include "scan.h"

#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "junk.h"
#include "sys.h"
#include "util.h"

namespace scan {

namespace {

// Сколько строк держим в списках: больше одного экрана всё равно не листают,
// а память и время на сортировку экономятся заметно.
constexpr size_t kKeepBigFiles = 80;
constexpr size_t kKeepOldFiles = 60;
constexpr size_t kKeepFolders = 40;
constexpr size_t kKeepOrphans = 40;
constexpr int kMaxOrphanCandidates = 600;

// ---------- Общее состояние ----------
//
// К нему ходят два потока: рабочий пишет, окна читают. Всё — только под замком,
// наружу отдаются копии.
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

HWND g_notify = nullptr;
UINT g_progressMsg = 0;
UINT g_doneMsg = 0;

HANDLE g_thread = nullptr;
volatile LONG g_cancel = 0;
volatile LONG g_busy = 0;

Progress g_progress;
Result g_result;
CleanReport g_clean;

struct Job {
    bool cleaning = false;
    bool junkOnly = false;  // пересчёт мусора поверх уже собранного результата
    wchar_t drive = L'C';
    Options options;
    CleanRequest request;
};

Job g_job;

void EnsureLock() {
    if (g_lockReady) return;
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
}

struct Guard {
    Guard() { EnterCriticalSection(&g_lock); }
    ~Guard() { LeaveCriticalSection(&g_lock); }
};

bool Cancelled() { return InterlockedCompareExchange(&g_cancel, 0, 0) != 0; }

void Notify(UINT message) {
    if (g_notify && message) PostMessageW(g_notify, message, 0, 0);
}

// Прогресс шлём не чаще, чем раз в ~120 мс: иначе очередь сообщений забивается,
// и блоки перерисовываются вместо того, чтобы считать.
DWORD g_lastTick = 0;

void PublishProgress(Stage stage, const std::wstring& current, unsigned long long bytes, int files,
                     unsigned long long total, const wchar_t* note, bool force) {
    const DWORD now = GetTickCount();
    if (!force && now - g_lastTick < 120) return;
    g_lastTick = now;
    {
        Guard guard;
        g_progress.busy = true;
        g_progress.stage = stage;
        g_progress.current = current;
        g_progress.bytes = bytes;
        g_progress.files = files;
        g_progress.note = note;
        if (total > 0) {
            const unsigned long long percent = bytes * 100ULL / total;
            g_progress.percent = static_cast<int>(percent > 99 ? 99 : percent);
        }
    }
    Notify(g_progressMsg);
}

}  // namespace

const wchar_t* StageName(Stage stage) {
    switch (stage) {
        case Stage::Space: return L"Смотрю диск";
        case Stage::Junk: return L"Ищу мусор";
        case Stage::Orphans: return L"Ищу хвосты программ";
        case Stage::Walk: return L"Обхожу папки";
        case Stage::Cleaning: return L"Пылесошу";
        case Stage::Done: return L"Готово";
        default: return L"Жду";
    }
}

unsigned long long Result::JunkTotal() const {
    unsigned long long sum = 0;
    for (const JunkItem& item : junk) {
        if (!item.userData) sum += item.size;  // «безопасный» мусор, который и предлагаем
    }
    return sum;
}

unsigned long long Result::JunkTotal(const std::vector<int>& ids) const {
    unsigned long long sum = 0;
    for (int id : ids) {
        if (const JunkItem* item = FindJunk(id)) sum += item->size;
    }
    return sum;
}

const JunkItem* Result::FindJunk(int id) const {
    for (const JunkItem& item : junk) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

// ---------- Диски ----------

std::vector<Drive> Drives() {
    std::vector<Drive> drives;

    wchar_t windows[MAX_PATH] = {0};
    const wchar_t systemLetter = GetWindowsDirectoryW(windows, MAX_PATH) > 0
                                     ? static_cast<wchar_t>(towupper(windows[0]))
                                     : L'C';

    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        const std::wstring root = std::wstring(1, letter) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;

        Drive drive;
        drive.letter = letter;
        drive.system = (letter == systemLetter);

        wchar_t label[MAX_PATH] = {0};
        wchar_t fs[64] = {0};
        if (GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fs, 64)) {
            drive.label = label;
            drive.fs = fs;
        }
        if (drive.label.empty()) drive.label = L"Локальный диск";

        ULARGE_INTEGER freeForUser = {}, total = {}, freeTotal = {};
        if (GetDiskFreeSpaceExW(root.c_str(), &freeForUser, &total, &freeTotal)) {
            drive.total = total.QuadPart;
            drive.freeSpace = freeForUser.QuadPart;
        }
        if (drive.total == 0) continue;  // том есть, но не смонтирован
        drives.push_back(std::move(drive));
    }
    return drives;
}

bool DriveOf(wchar_t letter, Drive& out) {
    for (const Drive& drive : Drives()) {
        if (towupper(drive.letter) == towupper(letter)) {
            out = drive;
            return true;
        }
    }
    return false;
}

namespace {

// ---------- Обход ----------

bool IsDotDir(const wchar_t* name) {
    return name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

unsigned long long SizeOf(const WIN32_FIND_DATAW& fd) {
    ULARGE_INTEGER size;
    size.LowPart = fd.nFileSizeLow;
    size.HighPart = fd.nFileSizeHigh;
    return size.QuadPart;
}

// Узел дерева папок: держим только первые два уровня, дальше размеры складываются
// в ближайшего записанного предка. Дерево на весь диск не влезло бы в память.
struct Agg {
    std::wstring path;
    std::wstring name;  // путь без «C:\»
    unsigned long long size = 0;
    int files = 0;
    int parent = -1;
    int depth = 0;
};

struct Pending {
    std::wstring path;
    int depth = 0;
    int agg = -1;
};

// Папки, которые к «старым файлам пользователя» отношения не имеют: там всё старое
// по определению, и удалять оттуда нельзя.
struct SystemPaths {
    std::vector<std::wstring> roots;
    bool IsSystem(const std::wstring& path) const {
        for (const std::wstring& root : roots) {
            if (StartsWithPath(path, root)) return true;
        }
        // Данные программ внутри профиля — тоже не документы пользователя.
        return path.find(L"\\AppData\\") != std::wstring::npos;
    }
};

SystemPaths BuildSystemPaths(wchar_t drive) {
    SystemPaths paths;
    const std::wstring root = std::wstring(1, drive) + L":\\";
    const wchar_t* names[] = {L"Windows",   L"Program Files", L"Program Files (x86)",
                              L"ProgramData", L"$Recycle.Bin", L"$WinREAgent",
                              L"Recovery",  L"System Volume Information", L"PerfLogs",
                              L"Windows.old", L"$Windows.~BT", L"MSOCache"};
    for (const wchar_t* name : names) paths.roots.push_back(root + name);
    return paths;
}

// Личные папки пользователя — их можно не показывать в списках, если попросили.
std::vector<std::wstring> PersonalFolders() {
    std::vector<std::wstring> folders;
    const KNOWNFOLDERID ids[] = {FOLDERID_Documents, FOLDERID_Desktop, FOLDERID_Pictures,
                                 FOLDERID_Videos, FOLDERID_Music};
    for (const KNOWNFOLDERID& id : ids) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
            folders.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return folders;
}

bool UnderAny(const std::wstring& path, const std::vector<std::wstring>& roots) {
    for (const std::wstring& root : roots) {
        if (StartsWithPath(path, root)) return true;
    }
    return false;
}

bool BySizeDesc(const FileItem& a, const FileItem& b) { return a.size > b.size; }

void TrimFiles(std::vector<FileItem>& list, size_t keep, unsigned long long& threshold) {
    if (list.size() < keep * 2) return;
    std::sort(list.begin(), list.end(), BySizeDesc);
    list.resize(keep);
    threshold = list.back().size;
}

// ---------- Мусор ----------

// Считает содержимое одной папки правила: сколько байт и файлов под него подходит.
void MeasureFolder(const std::wstring& root, const junk::Rule& rule, unsigned long long& size, int& files,
                   int& denied) {
    std::vector<std::wstring> stack{root};
    while (!stack.empty() && !Cancelled()) {
        const std::wstring dir = stack.back();
        stack.pop_back();

        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW(LongPath(dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_ACCESS_DENIED) denied++;
            continue;
        }
        do {
            if (IsDotDir(fd.cFileName)) continue;
            const std::wstring path = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                stack.push_back(path);
                continue;
            }
            if (!rule.patterns.empty()) {
                bool matched = false;
                for (const std::wstring& pattern : rule.patterns) {
                    if (junk::WildcardMatch(fd.cFileName, pattern)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) continue;
            }
            if (rule.minAgeDays > 0 && DaysSince(fd.ftLastWriteTime) < rule.minAgeDays) continue;
            size += SizeOf(fd);
            files++;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
}

void MeasureJunk(wchar_t drive, Result& result) {
    for (const junk::Rule& rule : junk::Rules(drive)) {
        JunkItem item;
        item.id = rule.id;
        item.name = rule.name;
        item.hint = rule.hint;
        item.userData = rule.userData;
        item.needsAdmin = rule.needsAdmin;

        if (rule.special) {
            item.size = RecycleBinSize();
            SHQUERYRBINFO info = {};
            info.cbSize = sizeof(info);
            if (SHQueryRecycleBinW(nullptr, &info) == S_OK && info.i64NumItems > 0) {
                item.files = static_cast<int>(info.i64NumItems);
            }
        } else {
            int denied = 0;
            for (const std::wstring& root : rule.roots) {
                if (Cancelled()) break;
                PublishProgress(Stage::Junk, root, 0, 0, 0, L"Ищу мусор", false);
                MeasureFolder(root, rule, item.size, item.files, denied);
            }
            if (denied > 0) item.needsAdmin = true;  // часть папок не отдала система
        }
        if (item.size == 0 && item.files == 0) continue;  // пустую группу показывать незачем
        result.junk.push_back(std::move(item));
    }

    // От большого к маленькому: первым делом видно, где лежат гигабайты, а не в каком
    // порядке эти группы записаны у нас в правилах.
    std::sort(result.junk.begin(), result.junk.end(),
              [](const JunkItem& a, const JunkItem& b) { return a.size > b.size; });
}

// ---------- Хвосты программ ----------

struct Installed {
    std::wstring name;      // нормализованное DisplayName
    std::wstring location;  // InstallLocation как есть
};

std::wstring Normalize(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        if (iswalnum(ch)) out.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return out;
}

std::wstring RegString(HKEY key, const wchar_t* name) {
    wchar_t buf[1024] = {0};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) != ERROR_SUCCESS) {
        return std::wstring();
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) return std::wstring();
    return std::wstring(buf);
}

void ReadUninstallKey(HKEY root, REGSAM view, std::vector<Installed>& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                      KEY_READ | view, &key) != ERROR_SUCCESS) {
        return;
    }
    for (DWORD index = 0;; index++) {
        wchar_t name[512] = {0};
        DWORD nameLen = 512;
        if (RegEnumKeyExW(key, index, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        HKEY entry = nullptr;
        if (RegOpenKeyExW(key, name, 0, KEY_READ | view, &entry) != ERROR_SUCCESS) continue;

        Installed installed;
        installed.name = Normalize(RegString(entry, L"DisplayName"));
        installed.location = ExpandVars(RegString(entry, L"InstallLocation"));
        RegCloseKey(entry);

        if (installed.name.empty() && installed.location.empty()) continue;
        out.push_back(std::move(installed));
    }
    RegCloseKey(key);
}

std::vector<Installed> InstalledPrograms() {
    std::vector<Installed> list;
    ReadUninstallKey(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, list);
    ReadUninstallKey(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, list);
    ReadUninstallKey(HKEY_CURRENT_USER, 0, list);
    return list;
}

// Папки, которые никогда не бывают «хвостом»: их держит сама Windows или железо.
bool ProtectedFolderName(const std::wstring& name) {
    static const wchar_t* kNames[] = {
        L"Temp",           L"CrashDumps",   L"Programs",     L"VirtualStore", L"History",
        L"Application Data", L"IconCache",  L"Comms",        L"Publishers",   L"ElevatedDiagnostics",
        L"ConnectedDevicesPlatform",        L"D3DSCache",    L"PlaceholderTileLogoFolder",
        L"SystemAppData",  L"Package Cache", L"OneDrive",    L"Desktop",      L"User Data",
        L"Downloads",      L"Documents",    L"Pictures",     L"Music",        L"Videos",
        L"ssh",            L"WinSxS",       L"Installer",    L"assembly",     L"USOShared",
        L"Common Files",   L"Packages",     L"WindowsApps"};
    for (const wchar_t* known : kNames) {
        if (EqualsNoCase(name, known)) return true;
    }
    // Целые семейства папок узнаются по началу имени: «Windows Defender»,
    // «Microsoft Office 15», «NVIDIA Corporation», «InstallShield Installation Information».
    // Их в списке «Uninstall» может не быть вовсе, а предлагать их удалить — вредный совет.
    static const wchar_t* kPrefixes[] = {L"Windows", L"Microsoft", L"Intel", L"AMD",
                                         L"NVIDIA",  L"Realtek",   L"InstallShield"};
    for (const wchar_t* prefix : kPrefixes) {
        const size_t length = wcslen(prefix);
        if (name.size() >= length && _wcsnicmp(name.c_str(), prefix, length) == 0) return true;
    }
    return false;
}

// Размер папки и дата самого свежего файла в ней. Ходим вглубь не дальше kDepth:
// хвост программы узнаётся и так, а полный обход каждой папки-кандидата стоил бы минут.
void MeasureOrphan(const std::wstring& root, unsigned long long& size, int& newestDays) {
    constexpr int kDepth = 4;
    struct Node {
        std::wstring path;
        int depth;
    };
    std::vector<Node> stack{{root, 0}};
    unsigned long long newest = 0;

    while (!stack.empty() && !Cancelled()) {
        const Node node = stack.back();
        stack.pop_back();

        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW(LongPath(node.path + L"\\*").c_str(), FindExInfoBasic, &fd,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (IsDotDir(fd.cFileName)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (node.depth + 1 <= kDepth) {
                    stack.push_back({node.path + L"\\" + fd.cFileName, node.depth + 1});
                }
                continue;
            }
            size += SizeOf(fd);
            const unsigned long long written = FileTimeToU64(fd.ftLastWriteTime);
            if (written > newest) newest = written;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    if (newest == 0) {
        newestDays = 9999;
        return;
    }
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(newest & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(newest >> 32);
    newestDays = DaysSince(ft);
}

void FindOrphans(wchar_t drive, const Options& options, Result& result) {
    const std::vector<Installed> installed = InstalledPrograms();

    // Где вообще заводятся хвосты: папки программ и их данные в профиле.
    std::vector<std::wstring> roots;
    const wchar_t* vars[] = {L"%ProgramFiles%", L"%ProgramFiles(x86)%", L"%ProgramData%",
                             L"%LOCALAPPDATA%", L"%APPDATA%", L"%LOCALAPPDATA%\\Programs"};
    for (const wchar_t* var : vars) {
        const std::wstring path = ExpandVars(var);
        if (path.size() < 3 || towupper(path[0]) != towupper(drive)) continue;
        if (IsDirectory(path)) roots.push_back(path);
    }

    struct Candidate {
        std::wstring path;
        std::wstring name;
    };
    std::vector<Candidate> candidates;

    for (const std::wstring& root : roots) {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW(LongPath(root + L"\\*").c_str(), FindExInfoBasic, &fd,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (IsDotDir(fd.cFileName)) continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            const std::wstring name = fd.cFileName;
            if (ProtectedFolderName(name)) continue;

            const std::wstring path = root + L"\\" + name;
            const std::wstring key = Normalize(name);
            if (key.size() < 4) continue;  // «QT», «vc» — по таким именам не угадать

            bool known = false;
            for (const Installed& program : installed) {
                if (!program.name.empty()) {
                    // Имя папки внутри названия программы («Zed» в «Zed Editor»),
                    // название внутри имени папки — или общее начало: папка
                    // «Microsoft Office 15» и программа «Microsoft Office профессиональный»
                    // одинаковы только первыми буквами.
                    const size_t prefix = std::min(key.size(), program.name.size());
                    if (program.name.find(key) != std::wstring::npos ||
                        (key.size() >= 6 && program.name.size() >= 4 &&
                         key.find(program.name) != std::wstring::npos) ||
                        (prefix >= 10 && key.compare(0, 10, program.name, 0, 10) == 0)) {
                        known = true;
                        break;
                    }
                }
                if (!program.location.empty() &&
                    (StartsWithPath(program.location, path) || StartsWithPath(path, program.location))) {
                    known = true;
                    break;
                }
            }
            if (known) continue;
            candidates.push_back({path, name});
        } while (FindNextFileW(find, &fd) && static_cast<int>(candidates.size()) < kMaxOrphanCandidates);
        FindClose(find);
        if (Cancelled()) return;
    }

    for (const Candidate& candidate : candidates) {
        if (Cancelled()) return;
        PublishProgress(Stage::Orphans, candidate.path, 0, 0, 0, L"Ищу хвосты программ", false);

        unsigned long long size = 0;
        int days = 0;
        MeasureOrphan(candidate.path, size, days);
        if (days < options.orphanDays) continue;   // папкой пользуются
        if (size < 8ULL * 1024 * 1024) continue;   // мелочь не стоит внимания

        OrphanItem item;
        item.path = candidate.path;
        item.name = candidate.name;
        item.size = size;
        item.days = days;
        item.reason = L"нет в списке установленных, не менялась " + AgeText(days);
        result.orphans.push_back(std::move(item));
    }

    std::sort(result.orphans.begin(), result.orphans.end(),
              [](const OrphanItem& a, const OrphanItem& b) { return a.size > b.size; });
    if (result.orphans.size() > kKeepOrphans) result.orphans.resize(kKeepOrphans);
}

// ---------- Крупные системные файлы ----------

unsigned long long FileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER size;
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    return size.QuadPart;
}

void CollectSystem(wchar_t drive, const std::vector<Agg>& aggs, Result& result) {
    const std::wstring root = std::wstring(1, drive) + L":\\";

    struct Known {
        const wchar_t* file;
        const wchar_t* name;
        const wchar_t* hint;
    };
    const Known kFiles[] = {
        {L"hiberfil.sys", L"Файл гибернации",
         L"Выключается командой powercfg /h off — освобождает столько же, сколько оперативной памяти"},
        {L"pagefile.sys", L"Файл подкачки",
         L"Система → Дополнительные параметры → Быстродействие → Виртуальная память"},
        {L"swapfile.sys", L"Подкачка приложений", L"Управляется вместе с файлом подкачки"},
    };
    for (const Known& known : kFiles) {
        const unsigned long long size = FileSizeOf(root + known.file);
        if (size == 0) continue;
        SysItem item;
        item.name = known.name;
        item.hint = known.hint;
        item.path = root + known.file;
        item.size = size;
        result.system.push_back(std::move(item));
    }

    // Папки, размер которых уже посчитан обходом: второй раз их не читаем.
    struct KnownDir {
        const wchar_t* path;
        const wchar_t* name;
        const wchar_t* hint;
    };
    const KnownDir kDirs[] = {
        {L"Windows\\WinSxS", L"Хранилище компонентов",
         L"Очистка диска → Очистить системные файлы → Очистка обновлений Windows"},
        {L"Windows\\Installer", L"Кэш установщиков",
         L"Нужен для удаления и обновления программ — вручную не трогать"},
        {L"Windows\\SoftwareDistribution", L"Кэш Центра обновления",
         L"Очищается из блока «Мусор» или сам после установки обновлений"},
        {L"$Recycle.Bin", L"Корзина", L"Место освобождается только после очистки корзины"},
        {L"Windows.old", L"Прошлая версия Windows",
         L"Удаляется через «Очистку диска» или из блока «Мусор»"},
        {L"System Volume Information", L"Точки восстановления и снимки",
         L"Защита системы → Настроить → Максимальное использование"},
    };
    for (const KnownDir& known : kDirs) {
        const std::wstring path = root + known.path;
        unsigned long long size = 0;
        for (const Agg& agg : aggs) {
            if (EqualsNoCase(agg.path, path)) {
                size = agg.size;
                break;
            }
        }
        if (size == 0 && !IsDirectory(path)) continue;
        SysItem item;
        item.name = known.name;
        item.hint = known.hint;
        item.path = path;
        item.size = size;
        result.system.push_back(std::move(item));
    }

    std::sort(result.system.begin(), result.system.end(),
              [](const SysItem& a, const SysItem& b) { return a.size > b.size; });
}

// ---------- Доли диаграммы ----------

void BuildSlices(wchar_t drive, const std::vector<Agg>& aggs, const std::vector<FolderItem>& rootFiles,
                 Result& result) {
    const std::wstring root = std::wstring(1, drive) + L":\\";
    constexpr size_t kPartsPerSlice = 7;

    // Из чего состоит доля. Если в неё попала одна папка первого уровня (например,
    // «Пользователи» — это всегда C:\Users), показываем то, что внутри неё: иначе
    // раскрытие сообщало бы ровно то же, что и сама строка.
    auto partsFor = [&](const std::vector<const Agg*>& tops) {
        std::vector<FolderItem> parts;
        std::vector<const Agg*> source = tops;
        if (tops.size() == 1) {
            source.clear();
            for (const Agg& agg : aggs) {
                if (agg.depth == 2 && StartsWithPath(agg.path, tops[0]->path)) source.push_back(&agg);
            }
        }
        for (const Agg* agg : source) {
            FolderItem item;
            item.path = agg->path;
            item.name = agg->name;
            item.size = agg->size;
            item.files = agg->files;
            parts.push_back(std::move(item));
        }
        std::sort(parts.begin(), parts.end(),
                  [](const FolderItem& a, const FolderItem& b) { return a.size > b.size; });
        if (parts.size() > kPartsPerSlice) parts.resize(kPartsPerSlice);
        return parts;
    };

    // Категории заданы заранее: пять понятных кусков читаются лучше, чем два десятка
    // папок первого уровня, из которых половина называется «$WinREAgent».
    struct Bucket {
        const wchar_t* name;
        const wchar_t* hint;
        int tone;
        unsigned long long size;
        std::vector<const Agg*> tops;  // папки первого уровня, попавшие в эту долю
    };
    Bucket buckets[] = {
        {L"Windows", L"Сама система: компоненты, драйверы, обновления", 0, 0, {}},
        {L"Программы", L"Program Files, Program Files (x86) и ProgramData", 1, 0, {}},
        {L"Пользователи", L"Профили: документы, загрузки, данные программ", 2, 0, {}},
        {L"Корзина", L"Удалённое, но ещё занимающее место", 3, 0, {}},
        {L"Другие папки", L"Всё остальное в корне диска: игры, архивы, проекты", 4, 0, {}},
        {L"Системные файлы", L"Гибернация и подкачка — файлы в корне диска", 5, 0, {}},
    };

    for (const Agg& agg : aggs) {
        if (agg.depth != 1) continue;
        const std::wstring name = agg.path.substr(root.size());
        Bucket* bucket = &buckets[4];
        if (EqualsNoCase(name, L"Windows")) bucket = &buckets[0];
        else if (EqualsNoCase(name, L"Program Files") || EqualsNoCase(name, L"Program Files (x86)") ||
                 EqualsNoCase(name, L"ProgramData")) {
            bucket = &buckets[1];
        } else if (EqualsNoCase(name, L"Users") || EqualsNoCase(name, L"Пользователи")) {
            bucket = &buckets[2];
        } else if (EqualsNoCase(name, L"$Recycle.Bin")) {
            bucket = &buckets[3];
        }
        bucket->size += agg.size;
        bucket->tops.push_back(&agg);
    }
    for (const FolderItem& file : rootFiles) buckets[5].size += file.size;

    for (const Bucket& bucket : buckets) {
        if (bucket.size == 0) continue;
        Slice slice;
        slice.name = bucket.name;
        slice.hint = bucket.hint;
        slice.size = bucket.size;
        slice.tone = bucket.tone;
        if (bucket.tone == 5) {
            // У системных файлов «состав» — это они сами: hiberfil.sys, pagefile.sys.
            slice.parts = rootFiles;
            std::sort(slice.parts.begin(), slice.parts.end(),
                      [](const FolderItem& a, const FolderItem& b) { return a.size > b.size; });
            if (slice.parts.size() > kPartsPerSlice) slice.parts.resize(kPartsPerSlice);
        } else {
            slice.parts = partsFor(bucket.tops);
        }
        result.slices.push_back(std::move(slice));
    }

    // То, что занято по данным Windows, но не попалось при обходе: защищённые папки,
    // снимки восстановления, чужие профили. Без этой доли круг не сходится, а
    // пользователь как раз и спрашивает «куда делось».
    if (result.used > result.scanned) {
        const unsigned long long hidden = result.used - result.scanned;
        // Мелкое расхождение — это просто округления файловой системы.
        if (hidden > 64ULL * 1024 * 1024) {
            result.hidden = hidden;
            Slice slice;
            slice.name = L"Не удалось заглянуть";
            slice.hint = L"Защищённые папки, точки восстановления, снимки и чужие профили";
            slice.size = hidden;
            slice.tone = 6;
            result.slices.push_back(std::move(slice));
        }
    }

    std::sort(result.slices.begin(), result.slices.end(),
              [](const Slice& a, const Slice& b) { return a.size > b.size; });
}

// ---------- Полный обход ----------

void WalkDrive(wchar_t drive, const Options& options, Result& result) {
    const std::wstring root = std::wstring(1, drive) + L":\\";
    const SystemPaths systemPaths = BuildSystemPaths(drive);
    const std::vector<std::wstring> personal = options.skipUserFiles ? PersonalFolders()
                                                                    : std::vector<std::wstring>();

    const unsigned long long bigMin = static_cast<unsigned long long>(options.bigFileMinMb) * 1024 * 1024;
    const unsigned long long oldMin = static_cast<unsigned long long>(options.oldFileMinMb) * 1024 * 1024;
    unsigned long long bigThreshold = bigMin;
    unsigned long long oldThreshold = oldMin;

    std::vector<Agg> aggs;
    aggs.reserve(512);
    std::vector<Pending> stack;
    stack.push_back({root, 0, -1});

    std::vector<FolderItem> rootFiles;  // hiberfil.sys и прочее прямо в корне диска
    int sinceTick = 0;

    while (!stack.empty()) {
        if (Cancelled()) {
            result.partial = true;
            break;
        }
        const Pending node = stack.back();
        stack.pop_back();

        if (++sinceTick >= 24) {
            sinceTick = 0;
            PublishProgress(Stage::Walk, node.path, result.scanned, result.fileCount, result.used,
                            L"Обхожу папки", false);
        }

        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW(LongPath(node.path + L"*").c_str(), FindExInfoBasic, &fd,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) result.deniedDirs++;
            continue;
        }

        do {
            if (IsDotDir(fd.cFileName)) continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Символические ссылки и точки соединения ведут туда, где мы уже были
                // (или будем): по ним диск считался бы дважды и мог зациклиться.
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

                const std::wstring path = node.path + fd.cFileName + L"\\";
                result.dirCount++;
                if (node.depth < 2) {
                    Agg agg;
                    agg.path = path.substr(0, path.size() - 1);  // без завершающего «\»
                    agg.name = agg.path.size() > root.size() ? agg.path.substr(root.size()) : agg.path;
                    agg.parent = node.agg;
                    agg.depth = node.depth + 1;
                    aggs.push_back(std::move(agg));
                    stack.push_back({path, node.depth + 1, static_cast<int>(aggs.size()) - 1});
                } else {
                    stack.push_back({path, node.depth + 1, node.agg});
                }
                continue;
            }

            const unsigned long long size = SizeOf(fd);
            const std::wstring path = node.path + fd.cFileName;
            result.scanned += size;
            result.fileCount++;

            if (node.agg < 0) {
                FolderItem file;
                file.path = path;
                file.name = fd.cFileName;
                file.size = size;
                rootFiles.push_back(std::move(file));
            } else {
                // Размер поднимается до корня по цепочке — она короткая, не больше двух шагов.
                for (int index = node.agg; index >= 0; index = aggs[static_cast<size_t>(index)].parent) {
                    aggs[static_cast<size_t>(index)].size += size;
                    aggs[static_cast<size_t>(index)].files++;
                }
            }

            // Гибернация и подкачка живут в корне диска, всегда огромны и удалению не
            // подлежат: их место объясняет блок «Системные файлы», а в списке больших
            // файлов они только занимали бы первые строки.
            const bool rootSystemFile = node.agg < 0 && (EqualsNoCase(fd.cFileName, L"hiberfil.sys") ||
                                                         EqualsNoCase(fd.cFileName, L"pagefile.sys") ||
                                                         EqualsNoCase(fd.cFileName, L"swapfile.sys"));
            const bool listable =
                !rootSystemFile && (!options.skipUserFiles || !UnderAny(path, personal));
            if (listable && size >= bigThreshold) {
                FileItem item;
                item.path = path;
                item.size = size;
                item.days = DaysSince(fd.ftLastWriteTime);
                result.bigFiles.push_back(std::move(item));
                TrimFiles(result.bigFiles, kKeepBigFiles, bigThreshold);
            }
            if (listable && size >= oldThreshold && !systemPaths.IsSystem(path)) {
                const int days = DaysSince(fd.ftLastWriteTime);
                if (days >= options.oldFileDays) {
                    FileItem item;
                    item.path = path;
                    item.size = size;
                    item.days = days;
                    result.oldFiles.push_back(std::move(item));
                    TrimFiles(result.oldFiles, kKeepOldFiles, oldThreshold);
                }
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    std::sort(result.bigFiles.begin(), result.bigFiles.end(), BySizeDesc);
    if (result.bigFiles.size() > kKeepBigFiles) result.bigFiles.resize(kKeepBigFiles);
    std::sort(result.oldFiles.begin(), result.oldFiles.end(), BySizeDesc);
    if (result.oldFiles.size() > kKeepOldFiles) result.oldFiles.resize(kKeepOldFiles);

    // Папки-тяжеловесы: оба уровня вперемешку, но родителя и его ребёнка рядом не держим —
    // «Users» и «Users\Ivan» в одном списке выглядят как ошибка.
    std::vector<Agg> sorted = aggs;
    std::sort(sorted.begin(), sorted.end(), [](const Agg& a, const Agg& b) { return a.size > b.size; });
    for (const Agg& agg : sorted) {
        if (result.folders.size() >= kKeepFolders) break;
        if (agg.size < 16ULL * 1024 * 1024) break;
        bool covered = false;
        for (const FolderItem& taken : result.folders) {
            const bool inside = StartsWithPath(agg.path, taken.path);
            const bool outside = StartsWithPath(taken.path, agg.path);
            if (!inside && !outside) continue;
            // «Users» и «Users\Ivan» одного размера — это одна и та же строка дважды.
            // А вот «Users\Ivan\Downloads» заметно меньше профиля и стоит показа.
            if (inside && agg.size * 10 < taken.size * 9) continue;
            covered = true;
            break;
        }
        if (covered) continue;
        FolderItem item;
        item.path = agg.path;
        item.name = agg.name;
        item.size = agg.size;
        item.files = agg.files;
        result.folders.push_back(std::move(item));
    }

    CollectSystem(drive, aggs, result);
    BuildSlices(drive, aggs, rootFiles, result);
}

// ---------- Очистка ----------

// Удаляет содержимое папки по правилу. Занятые файлы система не отдаёт — это нормально,
// их просто считаем в «не удалось».
void CleanFolder(const std::wstring& root, const junk::Rule& rule, CleanReport& report,
                 std::vector<std::wstring>& recycle) {
    std::vector<std::wstring> dirs{root};
    std::vector<std::wstring> visited;

    for (size_t i = 0; i < dirs.size(); i++) {
        if (Cancelled()) return;
        const std::wstring dir = dirs[i];
        visited.push_back(dir);

        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW(LongPath(dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (IsDotDir(fd.cFileName)) continue;
            const std::wstring path = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                dirs.push_back(path);
                continue;
            }
            if (!rule.patterns.empty()) {
                bool matched = false;
                for (const std::wstring& pattern : rule.patterns) {
                    if (junk::WildcardMatch(fd.cFileName, pattern)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) continue;
            }
            if (rule.minAgeDays > 0 && DaysSince(fd.ftLastWriteTime) < rule.minAgeDays) continue;

            const unsigned long long size = SizeOf(fd);
            if (rule.toRecycleBin) {
                recycle.push_back(path);
                report.freed += size;
                report.files++;
                continue;
            }
            if (DeletePermanently(path)) {
                report.freed += size;
                report.files++;
            } else {
                report.failed++;
            }
            if ((report.files & 0x3F) == 0) {
                PublishProgress(Stage::Cleaning, path, report.freed, report.files, 0, L"Пылесошу", false);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    if (rule.toRecycleBin) return;  // папки оставляем: их содержимое уедет в корзину целиком

    // Пустые папки убираем с конца списка — сначала самые глубокие.
    for (size_t i = visited.size(); i > 0; i--) {
        const std::wstring& dir = visited[i - 1];
        if (!rule.deleteRoot && EqualsNoCase(dir, root)) continue;  // сама папка правилу нужна
        RemoveDirectoryW(LongPath(dir).c_str());
    }
}

// Размер того, что уедет в корзину: у папки его иначе не узнать, а после переноса
// спрашивать уже поздно.
unsigned long long PathSize(const std::wstring& path) {
    if (!IsDirectory(path)) return FileSizeOf(path);
    unsigned long long size = 0;
    int days = 0;
    MeasureOrphan(path, size, days);
    return size;
}

void RunClean(wchar_t drive, const CleanRequest& request, CleanReport& report) {
    std::vector<std::wstring> recycle = request.recyclePaths;

    if (!request.junkIds.empty()) {
        // Правила перечитываем заново: с момента сканирования папки могли измениться.
        const std::vector<junk::Rule> rules = junk::Rules(drive);
        for (int id : request.junkIds) {
            if (Cancelled()) break;
            for (const junk::Rule& rule : rules) {
                if (rule.id != id) continue;
                if (rule.special) {
                    const unsigned long long before = RecycleBinSize();
                    if (EmptyRecycleBin(nullptr)) report.freed += before;
                    else report.failed++;
                    continue;
                }
                for (const std::wstring& root : rule.roots) {
                    PublishProgress(Stage::Cleaning, root, report.freed, report.files, 0, L"Пылесошу", true);
                    CleanFolder(root, rule, report, recycle);
                }
            }
        }
    }

    if (!recycle.empty()) {
        PublishProgress(Stage::Cleaning, L"Корзина", report.freed, report.files, 0, L"Отправляю в корзину",
                        true);
        // Размеры считаем до переноса: после него файла по этому пути уже нет.
        for (const std::wstring& path : request.recyclePaths) {
            const unsigned long long size = PathSize(path);
            if (size > 0) {
                report.freed += size;
                report.files++;
            }
        }
        if (!ToRecycleBin(nullptr, recycle)) report.failed++;
    }

    if (request.emptyRecycleBin) {
        const unsigned long long before = RecycleBinSize();
        if (EmptyRecycleBin(nullptr)) report.freed += before;
    }
    report.done = true;
}

// ---------- Поток ----------

DWORD WINAPI WorkerThread(LPVOID) {
    Job job;
    {
        Guard guard;
        job = g_job;
    }

    if (job.cleaning) {
        CleanReport report;
        PublishProgress(Stage::Cleaning, L"", 0, 0, 0, L"Пылесошу", true);
        RunClean(job.drive, job.request, report);
        {
            Guard guard;
            g_clean = report;
            g_progress = Progress{};
            g_progress.stage = Stage::Done;
        }
    } else if (job.junkOnly) {
        // Берём прошлый результат целиком и меняем в нём только список мусора:
        // обходить диск заново после уборки было бы честно, но слишком долго.
        Result result;
        {
            Guard guard;
            result = g_result;
        }
        result.junk.clear();
        PublishProgress(Stage::Junk, L"", 0, 0, 0, L"Пересчитываю мусор", true);
        MeasureJunk(job.drive, result);

        Drive drive;
        if (DriveOf(job.drive, drive)) {
            result.total = drive.total;
            result.freeSpace = drive.freeSpace;
            result.used = drive.Used();
        }
        {
            Guard guard;
            g_result = result;
            g_progress = Progress{};
            g_progress.stage = Stage::Done;
        }
    } else {
        Result result;
        result.drive = job.drive;

        Drive drive;
        if (DriveOf(job.drive, drive)) {
            result.total = drive.total;
            result.freeSpace = drive.freeSpace;
            result.used = drive.Used();
        }
        PublishProgress(Stage::Space, drive.Root(), 0, 0, result.used, L"Смотрю диск", true);

        MeasureJunk(job.drive, result);
        if (!Cancelled()) FindOrphans(job.drive, job.options, result);
        if (!Cancelled()) WalkDrive(job.drive, job.options, result);

        GetLocalTime(&result.finished);
        result.valid = true;
        if (Cancelled()) result.partial = true;

        {
            Guard guard;
            g_result = result;
            g_progress = Progress{};
            g_progress.stage = Stage::Done;
            g_progress.percent = 100;
        }
    }

    InterlockedExchange(&g_busy, 0);
    Notify(g_doneMsg);
    return 0;
}

bool StartJob(const Job& job) {
    EnsureLock();
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return false;  // уже работаем

    if (g_thread) {
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    InterlockedExchange(&g_cancel, 0);
    {
        Guard guard;
        g_job = job;
        g_progress = Progress{};
        g_progress.busy = true;
        g_progress.stage = job.cleaning ? Stage::Cleaning : (job.junkOnly ? Stage::Junk : Stage::Space);
        g_progress.note = job.cleaning ? L"Пылесошу" : (job.junkOnly ? L"Пересчитываю мусор" : L"Смотрю диск");
        if (job.cleaning) g_clean = CleanReport{};
    }

    g_thread = CreateThread(nullptr, 0, &WorkerThread, nullptr, 0, nullptr);
    if (!g_thread) {
        InterlockedExchange(&g_busy, 0);
        return false;
    }
    // Обход диска не должен мешать работать: отдаём ему только свободное время.
    SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
    Notify(g_progressMsg);
    return true;
}

}  // namespace

// ---------- Внешний интерфейс ----------

void Init(HWND notify, UINT progressMessage, UINT doneMessage) {
    EnsureLock();
    g_notify = notify;
    g_progressMsg = progressMessage;
    g_doneMsg = doneMessage;
}

bool Start(wchar_t drive, const Options& options) {
    Job job;
    job.cleaning = false;
    job.drive = static_cast<wchar_t>(towupper(drive));
    job.options = options;
    return StartJob(job);
}

bool RefreshJunk() {
    Job job;
    job.junkOnly = true;
    {
        EnsureLock();
        Guard guard;
        if (!g_result.valid) return false;  // пересчитывать нечего
        job.drive = g_result.drive;
    }
    return StartJob(job);
}

bool StartClean(const CleanRequest& request) {
    Job job;
    job.cleaning = true;
    {
        EnsureLock();
        Guard guard;
        job.drive = g_result.valid ? g_result.drive : L'C';
    }
    job.request = request;
    return StartJob(job);
}

void Cancel() { InterlockedExchange(&g_cancel, 1); }

bool Busy() { return InterlockedCompareExchange(&g_busy, 0, 0) != 0; }

Progress CurrentProgress() {
    EnsureLock();
    Guard guard;
    Progress copy = g_progress;
    copy.busy = InterlockedCompareExchange(&g_busy, 0, 0) != 0;
    return copy;
}

bool HasResult() {
    EnsureLock();
    Guard guard;
    return g_result.valid;
}

Result Snapshot() {
    EnsureLock();
    Guard guard;
    return g_result;
}

CleanReport LastClean() {
    EnsureLock();
    Guard guard;
    return g_clean;
}

void Shutdown() {
    if (!g_lockReady) return;
    Cancel();
    if (g_thread) {
        // Поток проверяет флаг отмены в каждом цикле, так что ждать долго не придётся.
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    g_notify = nullptr;
}

}  // namespace scan
