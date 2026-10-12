// Список треков выбранной папки.
//
// Обход дерева быстрый и делается сразу, а теги (название, исполнитель, длительность)
// читает фоновый поток: на папке в несколько сотен файлов это занимает секунды, и всё
// это время виджет должен оставаться живым. Список отдаётся наружу только копиями.
#pragma once

#include <string>
#include <vector>

namespace library {

struct Track {
    std::wstring path;
    std::wstring title;   // из тегов, иначе из имени файла
    std::wstring artist;  // пусто, если неизвестен
    std::wstring album;
    unsigned long long durationMs = 0;
    unsigned long long writeTime = 0;  // время изменения и размер файла на момент обхода:
    unsigned long long size = 0;       // по ним видно, что теги можно не перечитывать
    bool tagged = false;  // теги уже прочитаны
};

// Что делать с именем файла. Теги бывают пустыми или неверными, а имя файла у
// скачанной музыки почти всегда осмысленное — поэтому способ выбирает пользователь.
enum class NameMode {
    Tags = 0,         // сначала теги, имя файла — на подхвате
    ArtistFirst = 1,  // «Исполнитель - Название.mp3»
    TitleFirst = 2,   // «Название - Исполнитель.mp3»
};

// Пересобрать список. Обход папки синхронный, чтение тегов — в фоне. Теги файлов, которые
// не менялись с прошлого обхода, переносятся из старого списка. Заодно папка ставится под
// наблюдение — см. FolderChanged.
void Scan(const std::wstring& folder, bool recursive, NameMode mode);
// В папке что-то добавили, удалили, переименовали или перезаписали, и всё успокоилось.
// Опрашивается таймером окна; true — пора вызвать Scan заново.
bool FolderChanged();
// Перечитать подписи из уже собранных путей: сменился способ разбора имени файла.
void Relabel(NameMode mode);

void Cancel();  // остановить фоновое чтение
void Shutdown();

bool Busy();     // теги ещё читаются
int Version();   // растёт при каждом изменении списка — сигнал перерисовать блок
int Count();

std::vector<Track> Snapshot();
bool At(int index, Track& out);
int IndexOf(const std::wstring& path);

// Файл, открытый мимо папки: теги читаем сразу, тут ждать нечего.
Track Probe(const std::wstring& path, NameMode mode);

bool IsAudioFile(const std::wstring& path);
// Фильтр для диалога открытия файла (строки разделены \0).
const wchar_t* FileDialogFilter();

}  // namespace library
