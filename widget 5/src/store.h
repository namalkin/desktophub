#pragma once

#include <string>
#include <vector>

#include "library.h"

// Что делать, когда трек доиграл.
enum class RepeatMode {
    Off = 0,  // дошли до конца списка — остановились
    All = 1,  // список по кругу
    One = 2,  // один трек по кругу
};

struct AppData {
    // ---------- Музыка ----------
    std::wstring folder;        // папка с треками
    bool recursive = true;      // вместе с подпапками
    int nameMode = 0;           // library::NameMode
    bool shuffle = false;
    RepeatMode repeat = RepeatMode::All;
    int volume = 70;            // 0..100

    // Чем закончили в прошлый раз: трек открывается на паузе с той же секунды.
    std::wstring lastTrack;
    unsigned long long lastPositionMs = 0;
    bool resumeOnStart = true;
    bool playOnStart = false;   // не просто открыть, а сразу играть

    // ---------- Оформление ----------
    bool darkTheme = true;
    int accent = 0;
    int opacity = 100;   // 40..100
    int fontIndex = 0;
    bool showCover = true;
    bool showList = true;
    int listRows = 5;    // сколько строк списка видно, 0 — список свёрнут
    int cardW = 360;     // логическая ширина карточки, меняется мышью

    // ---------- Общее ----------
    bool hotkeysEnabled = true;   // Ctrl+Alt+стрелки и пробел
    bool mediaKeys = true;        // мультимедийные клавиши клавиатуры
    bool alwaysOnTop = false;
    bool locked = false;          // блок не двигается и не меняет размер

    // ---------- Окно ----------
    int winX = INT32_MIN;  // INT32_MIN => позиция ещё не выбрана
    int winY = INT32_MIN;

    library::NameMode NameModeValue() const;
    void ClampAll();
};

// Ошибки чтения не фатальны: битый/отсутствующий файл => значения по умолчанию.
bool LoadData(AppData& data);
bool SaveData(const AppData& data);

// Папка музыки пользователя — с неё плеер начинает при первом запуске.
std::wstring DefaultMusicFolder();
