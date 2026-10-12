// Воспроизведение через Media Foundation — тот же движок, что у проигрывателя Windows,
// поэтому играет всё, для чего в системе есть кодек: mp3, m4a, wma, flac, wav.
//
// Ни одного дополнительного потока: события сессии разбираются из таймера виджета
// (audio::Pump), а не в callback'е. Так весь плеер живёт на одном потоке и не нужны
// ни блокировки, ни реализация IMFAsyncCallback.
#pragma once

#include <string>

namespace audio {

bool Startup();
void Shutdown();

enum class State {
    Idle,     // ничего не открыто
    Opening,  // топология строится, старта ещё не было
    Playing,
    Paused,
    Error,
};

// Открыть файл и сразу начать играть с позиции startMs.
// paused = true — открыть и встать на паузу (восстановление после перезапуска).
bool Open(const std::wstring& path, unsigned long long startMs = 0, bool paused = false);
void Close();

void Play();
void Pause();
void TogglePlay();

State Current();
bool Playing();
const std::wstring& OpenPath();
// Человекочитаемая причина, почему файл не заиграл.
const std::wstring& LastError();

unsigned long long PositionMs();
unsigned long long DurationMs();
void Seek(unsigned long long ms);

void SetVolume(int percent);  // 0..100

// Разбор событий сессии. Возвращает true, если что-то изменилось и блок стоит перерисовать.
bool Pump();

// Трек доиграл до конца — виджету пора включать следующий. Флаг одноразовый.
bool TakeEnded();

}  // namespace audio
