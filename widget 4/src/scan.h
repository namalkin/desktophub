// Сканирование диска в фоновом потоке: куда ушло место, что похоже на мусор,
// что давно не открывали и от каких программ остались хвосты.
//
// Весь тяжёлый обход живёт в отдельном потоке; окна забирают у него только копии
// результата (Snapshot) и текущий прогресс (CurrentProgress) — рисовать по живым
// данным нельзя, их в этот момент дописывают.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace scan {

// ---------- Диски ----------

struct Drive {
    wchar_t letter = L'C';
    std::wstring label;  // метка тома, «Локальный диск», если пусто
    std::wstring fs;     // NTFS, exFAT…
    unsigned long long total = 0;
    unsigned long long freeSpace = 0;
    bool system = false;  // тот, с которого загружается Windows

    std::wstring Root() const { return std::wstring(1, letter) + L":\\"; }
    unsigned long long Used() const { return total > freeSpace ? total - freeSpace : 0; }
    int UsedPercent() const {
        return total > 0 ? static_cast<int>(Used() * 100ULL / total) : 0;
    }
};

// Только несъёмные диски — сканировать флешку по таймеру никто не просил.
std::vector<Drive> Drives();
bool DriveOf(wchar_t letter, Drive& out);

// ---------- Результат ----------

struct FolderItem {
    std::wstring path;
    std::wstring name;  // «Program Files», «Users\Ivan\Downloads»
    unsigned long long size = 0;
    int files = 0;
};

// Доля в кольцевой диаграмме «куда ушло место».
struct Slice {
    std::wstring name;
    std::wstring hint;
    unsigned long long size = 0;
    int tone = 0;  // индекс цвета в палитре диаграмм
    // Из чего доля состоит: «Программы» — это Program Files, ProgramData и прочие.
    // Без этого списка круг отвечает «куда», но не отвечает «что именно».
    std::vector<FolderItem> parts;
};

struct FileItem {
    std::wstring path;
    unsigned long long size = 0;
    int days = 0;  // сколько дней назад файл меняли
};

// Группа мусора: набор папок и правил, которые чистятся вместе.
struct JunkItem {
    int id = 0;
    std::wstring name;
    std::wstring hint;
    unsigned long long size = 0;
    int files = 0;
    bool userData = false;  // файлы пользователя: удаляем в корзину и не отмечаем сами
    bool needsAdmin = false;
};

// Папка, для которой не нашлось установленной программы.
struct OrphanItem {
    std::wstring path;
    std::wstring name;
    std::wstring reason;  // «нет в списке установленных, не менялась 2 года»
    unsigned long long size = 0;
    int days = 0;
};

// Крупные системные файлы: их нельзя удалять руками, но именно они чаще всего
// и объясняют «куда делись десятки гигабайт».
struct SysItem {
    std::wstring name;
    std::wstring hint;   // чем это управляется
    std::wstring path;   // пусто, если у элемента нет одного файла
    unsigned long long size = 0;
};

struct Result {
    bool valid = false;
    wchar_t drive = L'C';
    unsigned long long total = 0;
    unsigned long long freeSpace = 0;
    unsigned long long used = 0;

    unsigned long long scanned = 0;    // сумма размеров всех прочитанных файлов
    unsigned long long hidden = 0;     // занято, но обойти не дали (защищённые папки, снимки)
    int deniedDirs = 0;
    int fileCount = 0;
    int dirCount = 0;

    std::vector<Slice> slices;
    std::vector<FolderItem> folders;
    std::vector<FileItem> bigFiles;
    std::vector<FileItem> oldFiles;
    std::vector<JunkItem> junk;
    std::vector<OrphanItem> orphans;
    std::vector<SysItem> system;

    SYSTEMTIME finished{};
    bool partial = false;  // сканирование прервали — цифры неполные

    unsigned long long JunkTotal() const;
    unsigned long long JunkTotal(const std::vector<int>& ids) const;
    const JunkItem* FindJunk(int id) const;
};

// ---------- Ход работы ----------

enum class Stage { Idle, Space, Junk, Orphans, Walk, Cleaning, Done };

struct Progress {
    bool busy = false;
    Stage stage = Stage::Idle;
    int percent = 0;  // 0..100, оценка по прочитанным байтам
    unsigned long long bytes = 0;
    int files = 0;
    std::wstring current;  // папка, которую читаем прямо сейчас
    std::wstring note;     // что делаем: «Ищу мусор», «Обхожу диск»
};

const wchar_t* StageName(Stage stage);

// Настройки поиска — их правит пользователь в настройках виджета.
struct Options {
    int bigFileMinMb = 100;   // файл считается большим начиная с этого размера
    int oldFileDays = 365;    // «старый» — не менялся столько дней
    int oldFileMinMb = 20;    // мелочь в список старых не берём
    int orphanDays = 180;     // хвост программы: папка не менялась столько дней
    bool skipUserFiles = false;  // не заглядывать в документы и рабочий стол
};

// notify получит kScanProgressMessage по ходу работы и kScanDoneMessage в конце.
void Init(HWND notify, UINT progressMessage, UINT doneMessage);

bool Start(wchar_t drive, const Options& options);
// Пересчитать только мусор, не обходя диск заново: нужно сразу после уборки, чтобы
// цифры в блоке «Мусор» не остались от того, что уже удалено.
bool RefreshJunk();
void Cancel();       // просит поток остановиться; он завершится сам
bool Busy();
Progress CurrentProgress();

bool HasResult();
Result Snapshot();  // копия последнего завершённого (или прерванного) результата

// ---------- Очистка ----------

struct CleanRequest {
    std::vector<int> junkIds;               // группы мусора целиком
    std::vector<std::wstring> recyclePaths;  // отдельные файлы и папки — в корзину
    bool emptyRecycleBin = false;
};

struct CleanReport {
    bool done = false;
    unsigned long long freed = 0;
    int files = 0;
    int failed = 0;  // не отдала система: файл занят или нет прав
};

bool StartClean(const CleanRequest& request);
CleanReport LastClean();

// Останавливает поток и освобождает ресурсы — вызывается при закрытии виджета.
void Shutdown();

}  // namespace scan
