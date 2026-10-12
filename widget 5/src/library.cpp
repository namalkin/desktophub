#include "library.h"

#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <unordered_map>

#include "util.h"

namespace library {

namespace {

// Всё, что умеет проиграть Media Foundation на обычной Windows 10/11.
const wchar_t* kExtensions[] = {L".mp3", L".m4a", L".m4b", L".aac",  L".adts", L".wav",
                                L".wma", L".flac", L".mp4", L".ac3", L".aif",  L".aiff"};

constexpr int kMaxTracks = 20000;  // защита от «выбрал диск C целиком»
constexpr int kTagBatch = 24;      // через столько файлов публикуем прочитанное
// Копирование пачки файлов — это десятки уведомлений подряд, а недокопированный файл ещё
// не прочитать. Поэтому список пересобираем, когда папка помолчала столько времени.
constexpr ULONGLONG kSettleMs = 1500;

CRITICAL_SECTION g_lock;
bool g_lockReady = false;

std::vector<Track> g_tracks;
NameMode g_mode = NameMode::Tags;
volatile LONG g_version = 0;
volatile LONG g_busy = 0;
volatile LONG g_cancel = 0;
HANDLE g_thread = nullptr;

// Наблюдение за папкой живёт в потоке окна: Scan и FolderChanged зовутся только оттуда.
HANDLE g_watch = INVALID_HANDLE_VALUE;
bool g_changePending = false;
ULONGLONG g_lastChange = 0;

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
void Bump() { InterlockedIncrement(&g_version); }

// «Ameno - Era» -> («Ameno», «Era») или наоборот, смотря что выбрано в настройках.
// Разделителем считаем только « - » с пробелами: дефис внутри названия («Ne-Yo»,
// «поп-музыка») разрывать нельзя.
void SplitStem(const std::wstring& stem, NameMode mode, std::wstring& title, std::wstring& artist) {
    title = stem;
    artist.clear();
    if (mode == NameMode::Tags) return;

    const size_t dash = stem.find(L" - ");
    if (dash == std::wstring::npos) return;
    const std::wstring left = TrimSpaces(stem.substr(0, dash));
    const std::wstring right = TrimSpaces(stem.substr(dash + 3));
    if (left.empty() || right.empty()) return;

    if (mode == NameMode::ArtistFirst) {
        artist = left;
        title = right;
    } else {
        title = left;
        artist = right;
    }
}

// Подписи по имени файла — то, что виджет показывает, пока теги ещё не прочитаны.
void LabelFromName(Track& track, NameMode mode) {
    SplitStem(FileStem(track.path), mode, track.title, track.artist);
}

// Теги читаем у оболочки, а не разбираем ID3 сами: так работает ровно то, что показывает
// проводник, — и для mp3, и для m4a, и для flac.
void ReadTags(Track& track, NameMode mode) {
    track.tagged = true;

    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreFromParsingName(track.path.c_str(), nullptr, GPS_DEFAULT,
                                                 IID_IPropertyStore, reinterpret_cast<void**>(&store)))) {
        return;
    }

    auto readString = [&](const PROPERTYKEY& key) -> std::wstring {
        PROPVARIANT value;
        PropVariantInit(&value);
        std::wstring out;
        if (SUCCEEDED(store->GetValue(key, &value))) {
            wchar_t buf[512] = {0};
            // Исполнителей в теге может быть несколько — PropVariantToString склеит их
            // через «; », это как раз то, что нужно показать.
            if (SUCCEEDED(PropVariantToString(value, buf, 512))) out = TrimSpaces(buf);
        }
        PropVariantClear(&value);
        return out;
    };

    const std::wstring tagTitle = readString(PKEY_Title);
    const std::wstring tagArtist = readString(PKEY_Music_Artist);
    track.album = readString(PKEY_Music_AlbumTitle);

    {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_Media_Duration, &value))) {
            ULONGLONG hns = 0;
            if (SUCCEEDED(PropVariantToUInt64(value, &hns))) track.durationMs = hns / 10000ULL;
        }
        PropVariantClear(&value);
    }
    store->Release();

    // Имя файла разобрано заранее; теги перебивают его только там, где они есть.
    if (mode == NameMode::Tags) {
        if (!tagTitle.empty()) track.title = tagTitle;
        if (!tagArtist.empty()) track.artist = tagArtist;
    } else if (track.artist.empty() && !tagArtist.empty()) {
        // Имя файла оказалось без « - » — исполнителя всё равно подскажет тег.
        track.artist = tagArtist;
    }
}

DWORD WINAPI TagThread(LPVOID) {
    // Свой поток — свой COM: без этого оболочка не отдаст свойства.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    NameMode mode;
    size_t total;
    {
        Guard guard;
        mode = g_mode;
        total = g_tracks.size();
    }

    // Читаем пачками. Под замком — только снятие копии и запись результата: сам диск
    // опрашивается без него, иначе окна ждали бы каждый файл.
    for (size_t i = 0; i < total && !Cancelled(); i += kTagBatch) {
        std::vector<std::pair<size_t, Track>> batch;
        {
            Guard guard;
            if (g_tracks.size() != total) break;  // список пересобрали под нами
            for (size_t k = i; k < total && k < i + kTagBatch; k++) {
                if (!g_tracks[k].tagged) batch.emplace_back(k, g_tracks[k]);
            }
        }
        if (batch.empty()) continue;

        for (auto& item : batch) {
            if (Cancelled()) break;
            ReadTags(item.second, mode);
        }

        {
            Guard guard;
            if (g_tracks.size() != total) break;
            for (auto& item : batch) {
                Track& slot = g_tracks[item.first];
                if (EqualsNoCase(slot.path, item.second.path)) slot = item.second;
            }
        }
        Bump();
    }

    if (SUCCEEDED(comHr)) CoUninitialize();
    InterlockedExchange(&g_busy, 0);
    Bump();
    return 0;
}

void StopThread() {
    InterlockedExchange(&g_cancel, 1);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    InterlockedExchange(&g_busy, 0);
    InterlockedExchange(&g_cancel, 0);
}

void StartThread() {
    InterlockedExchange(&g_cancel, 0);
    InterlockedExchange(&g_busy, 1);
    g_thread = CreateThread(nullptr, 0, &TagThread, nullptr, 0, nullptr);
    if (!g_thread) {
        InterlockedExchange(&g_busy, 0);
        return;
    }
    // Чтение тегов — дело не срочное: пусть не отбирает время у музыки.
    SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
}

void Walk(const std::wstring& folder, bool recursive, std::vector<Track>& out, int depth) {
    if (out.size() >= kMaxTracks || depth > 12) return;

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(LongPath(folder + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subfolders;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // ссылки не ходим

        const std::wstring full = folder + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recursive) subfolders.push_back(full);
            continue;
        }
        if (!IsAudioFile(full)) continue;

        Track track;
        track.path = full;
        track.writeTime = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                          fd.ftLastWriteTime.dwLowDateTime;
        track.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(std::move(track));
        if (out.size() >= kMaxTracks) break;
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    // Сначала файлы самой папки, потом вложенные — так порядок совпадает с проводником.
    std::sort(subfolders.begin(), subfolders.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (const std::wstring& sub : subfolders) Walk(sub, recursive, out, depth + 1);
}

void StopWatch() {
    if (g_watch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(g_watch);
    g_watch = INVALID_HANDLE_VALUE;
    g_changePending = false;
}

void StartWatch(const std::wstring& folder, bool recursive) {
    StopWatch();
    if (folder.empty() || !IsDirectory(folder)) return;
    // LAST_WRITE — чтобы подхватить и перезаписанный файл, и правку тегов в проводнике.
    g_watch = FindFirstChangeNotificationW(
        LongPath(folder).c_str(), recursive ? TRUE : FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
}

}  // namespace

bool IsAudioFile(const std::wstring& path) {
    const std::wstring ext = FileExtLower(path);
    if (ext.empty()) return false;
    for (const wchar_t* known : kExtensions) {
        if (ext == known) return true;
    }
    return false;
}

const wchar_t* FileDialogFilter() {
    return L"Музыка\0*.mp3;*.m4a;*.m4b;*.aac;*.adts;*.wav;*.wma;*.flac;*.mp4;*.ac3;*.aif;*.aiff\0"
           L"Все файлы\0*.*\0\0";
}

void Scan(const std::wstring& folder, bool recursive, NameMode mode) {
    EnsureLock();
    StopThread();

    std::vector<Track> found;
    if (!folder.empty() && IsDirectory(folder)) Walk(folder, recursive, found, 0);

    // Порядок — по имени файла внутри папки: так же, как в проводнике, и он не зависит
    // от того, успели ли прочитаться теги.
    std::sort(found.begin(), found.end(), [](const Track& a, const Track& b) {
        return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
    });

    {
        Guard guard;
        // Фоновый поток уже остановлен, так что старый список можно читать спокойно.
        std::unordered_map<std::wstring, const Track*> known;
        if (mode == g_mode) {
            for (const Track& old : g_tracks) {
                if (old.tagged) known.emplace(ToLower(old.path), &old);
            }
        }
        for (Track& track : found) {
            const auto it = known.find(ToLower(track.path));
            if (it != known.end() && it->second->writeTime == track.writeTime && it->second->size == track.size) {
                track = *it->second;
            } else {
                LabelFromName(track, mode);
            }
        }
        g_tracks = std::move(found);
        g_mode = mode;
    }
    Bump();
    StartWatch(folder, recursive);
    if (Count() > 0) StartThread();
}

bool FolderChanged() {
    if (g_watch == INVALID_HANDLE_VALUE) return false;
    // Сигнал не копится: за одно срабатывание может прийти сколько угодно изменений.
    while (WaitForSingleObject(g_watch, 0) == WAIT_OBJECT_0) {
        g_changePending = true;
        g_lastChange = GetTickCount64();
        if (!FindNextChangeNotification(g_watch)) {
            // Папку удалили или переименовали: следить больше не за чем, но список обновим.
            FindCloseChangeNotification(g_watch);
            g_watch = INVALID_HANDLE_VALUE;
            break;
        }
    }
    if (!g_changePending || GetTickCount64() - g_lastChange < kSettleMs) return false;
    g_changePending = false;
    return true;
}

void Relabel(NameMode mode) {
    EnsureLock();
    StopThread();
    {
        Guard guard;
        g_mode = mode;
        for (Track& track : g_tracks) {
            LabelFromName(track, mode);
            track.tagged = false;  // подписи пересоберутся из тегов заново
        }
    }
    Bump();
    if (Count() > 0) StartThread();
}

void Cancel() { InterlockedExchange(&g_cancel, 1); }

void Shutdown() {
    StopWatch();
    if (!g_lockReady) return;
    StopThread();
}

bool Busy() { return InterlockedCompareExchange(&g_busy, 0, 0) != 0; }

int Version() { return static_cast<int>(InterlockedCompareExchange(&g_version, 0, 0)); }

int Count() {
    EnsureLock();
    Guard guard;
    return static_cast<int>(g_tracks.size());
}

std::vector<Track> Snapshot() {
    EnsureLock();
    Guard guard;
    return g_tracks;
}

bool At(int index, Track& out) {
    EnsureLock();
    Guard guard;
    if (index < 0 || index >= static_cast<int>(g_tracks.size())) return false;
    out = g_tracks[static_cast<size_t>(index)];
    return true;
}

int IndexOf(const std::wstring& path) {
    EnsureLock();
    Guard guard;
    for (size_t i = 0; i < g_tracks.size(); i++) {
        if (EqualsNoCase(g_tracks[i].path, path)) return static_cast<int>(i);
    }
    return -1;
}

Track Probe(const std::wstring& path, NameMode mode) {
    Track track;
    track.path = path;
    LabelFromName(track, mode);
    ReadTags(track, mode);
    return track;
}

}  // namespace library
