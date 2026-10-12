#include "audio.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include "util.h"

namespace audio {

namespace {

// На каждый трек — своя сессия.
//
// Так сделано не от лени: презентационные часы сессии идут монотонно и не обнуляются ни
// при смене топологии, ни явной позицией в Start. Оставь сессию жить — и второй трек
// начнёт отсчёт с той секунды, где кончился первый, а полоса поедет по чужому времени.
// Свежая сессия начинает с нуля, и позиция трека совпадает с показаниями часов.
IMFMediaSession* g_session = nullptr;
IMFMediaSource* g_source = nullptr;
IMFPresentationClock* g_clock = nullptr;
IMFSimpleAudioVolume* g_volume = nullptr;

State g_state = State::Idle;
std::wstring g_path;
std::wstring g_error;
unsigned long long g_durationMs = 0;
unsigned long long g_lastPositionMs = 0;
unsigned long long g_pendingStartMs = 0;
bool g_pendingPaused = false;
bool g_ended = false;
int g_volumePercent = 70;
bool g_started = false;    // MFStartup прошёл
bool g_switching = false;  // идёт смена трека: конец старой презентации не считается концом трека
ULONGLONG g_openingSince = 0;  // когда начали открывать трек — страховка от зависшей топологии

const std::wstring kNoError;

template <class T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

void ShutdownSource(IMFMediaSource*& source) {
    if (!source) return;
    source->Shutdown();  // без этого файл остаётся занятым
    source->Release();
    source = nullptr;
}

// Миллисекунды <-> 100-наносекундные интервалы, в которых считает Media Foundation.
constexpr unsigned long long kHnsPerMs = 10000ULL;

// Закрыть сессию и всё, что к ней привязано. Порядок важен: пока сессия не подтвердила
// закрытие, источник трогать нельзя — иначе файл остаётся занятым.
void CloseSession() {
    if (g_session) {
        g_session->Close();
        for (int i = 0; i < 100; i++) {
            IMFMediaEvent* event = nullptr;
            if (g_session->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event) == S_OK && event) {
                MediaEventType type = MEUnknown;
                event->GetType(&type);
                event->Release();
                if (type == MESessionClosed) break;
                continue;
            }
            Sleep(2);
        }
        g_session->Shutdown();
        SafeRelease(g_session);
    }
    SafeRelease(g_clock);
    SafeRelease(g_volume);
    ShutdownSource(g_source);
}

// Свежая сессия под новый трек: только у неё часы начинают с нуля.
bool NewSession() {
    if (!g_started) return false;
    CloseSession();
    if (FAILED(MFCreateMediaSession(nullptr, &g_session))) {
        g_session = nullptr;
        return false;
    }
    return true;
}

// Из файла — источник и его длительность.
bool CreateSource(const std::wstring& path, IMFMediaSource** out, unsigned long long& durationMs) {
    *out = nullptr;
    durationMs = 0;

    IMFSourceResolver* resolver = nullptr;
    if (FAILED(MFCreateSourceResolver(&resolver))) return false;

    MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;
    IUnknown* unknown = nullptr;
    const HRESULT hr = resolver->CreateObjectFromURL(path.c_str(), MF_RESOLUTION_MEDIASOURCE, nullptr,
                                                     &objectType, &unknown);
    resolver->Release();
    if (FAILED(hr) || !unknown) return false;

    IMFMediaSource* source = nullptr;
    const HRESULT qi = unknown->QueryInterface(IID_IMFMediaSource, reinterpret_cast<void**>(&source));
    unknown->Release();
    if (FAILED(qi)) return false;

    IMFPresentationDescriptor* pd = nullptr;
    if (SUCCEEDED(source->CreatePresentationDescriptor(&pd))) {
        UINT64 hns = 0;
        if (SUCCEEDED(pd->GetUINT64(MF_PD_DURATION, &hns))) durationMs = hns / kHnsPerMs;
        pd->Release();
    }

    *out = source;
    return true;
}

// Частичная топология: звуковая дорожка -> системный микшер. Видео в .mp4 отключаем,
// иначе сессия попросит окно для картинки.
bool BuildTopology(IMFMediaSource* source, IMFTopology** out) {
    *out = nullptr;

    IMFPresentationDescriptor* pd = nullptr;
    if (FAILED(source->CreatePresentationDescriptor(&pd))) return false;

    DWORD streams = 0;
    if (FAILED(pd->GetStreamDescriptorCount(&streams)) || streams == 0) {
        pd->Release();
        return false;
    }

    IMFTopology* topology = nullptr;
    if (FAILED(MFCreateTopology(&topology))) {
        pd->Release();
        return false;
    }

    bool haveAudio = false;
    for (DWORD i = 0; i < streams; i++) {
        BOOL selected = FALSE;
        IMFStreamDescriptor* sd = nullptr;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &selected, &sd)) || !sd) continue;

        GUID majorType = GUID_NULL;
        IMFMediaTypeHandler* handler = nullptr;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&handler))) {
            handler->GetMajorType(&majorType);
            handler->Release();
        }

        const bool isAudio = (majorType == MFMediaType_Audio);
        if (!isAudio || haveAudio) {
            pd->DeselectStream(i);
            sd->Release();
            continue;
        }
        if (!selected) pd->SelectStream(i);

        IMFTopologyNode* sourceNode = nullptr;
        IMFTopologyNode* outputNode = nullptr;
        IMFActivate* renderer = nullptr;
        bool ok = SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &sourceNode)) &&
                  SUCCEEDED(sourceNode->SetUnknown(MF_TOPONODE_SOURCE, source)) &&
                  SUCCEEDED(sourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pd)) &&
                  SUCCEEDED(sourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, sd)) &&
                  SUCCEEDED(MFCreateAudioRendererActivate(&renderer)) &&
                  SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &outputNode)) &&
                  SUCCEEDED(outputNode->SetObject(renderer)) &&
                  SUCCEEDED(topology->AddNode(sourceNode)) && SUCCEEDED(topology->AddNode(outputNode)) &&
                  SUCCEEDED(sourceNode->ConnectOutput(0, outputNode, 0));

        SafeRelease(renderer);
        SafeRelease(sourceNode);
        SafeRelease(outputNode);
        sd->Release();
        if (ok) haveAudio = true;
    }

    pd->Release();
    if (!haveAudio) {
        topology->Release();
        return false;
    }
    *out = topology;
    return true;
}

void ApplyVolumeToSession() {
    if (!g_volume) return;
    float value = static_cast<float>(g_volumePercent) / 100.0f;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    g_volume->SetMasterVolume(value);
    g_volume->SetMute(FALSE);
}

// Топология собрана — можно брать часы, регулятор громкости и стартовать.
void OnTopologyReady() {
    SafeRelease(g_clock);
    IMFClock* clock = nullptr;
    if (SUCCEEDED(g_session->GetClock(&clock)) && clock) {
        clock->QueryInterface(IID_IMFPresentationClock, reinterpret_cast<void**>(&g_clock));
        clock->Release();
    }

    SafeRelease(g_volume);
    MFGetService(g_session, MR_POLICY_VOLUME_SERVICE, IID_IMFSimpleAudioVolume,
                 reinterpret_cast<void**>(&g_volume));
    ApplyVolumeToSession();

    // Позицию задаём всегда, даже нулевую: у пустого PROPVARIANT (VT_EMPTY) смысл
    // «продолжить с текущего места», а нам нужно ровно то место, которое мы просили.
    PROPVARIANT start;
    PropVariantInit(&start);
    start.vt = VT_I8;
    start.hVal.QuadPart = static_cast<LONGLONG>(g_pendingStartMs * kHnsPerMs);
    const HRESULT hr = g_session->Start(&GUID_NULL, &start);
    PropVariantClear(&start);

    if (FAILED(hr)) {
        g_state = State::Error;
        g_error = L"Не удалось начать воспроизведение.";
        return;
    }
    g_lastPositionMs = g_pendingStartMs;
    g_switching = false;
    g_openingSince = 0;
    if (g_pendingPaused) {
        g_session->Pause();  // команды выполняются по порядку: стартуем и сразу встаём на паузу
        g_state = State::Paused;
    } else {
        g_state = State::Playing;
    }
}

// Одно событие сессии. changed — состояние изменилось и блок стоит перерисовать.
bool HandleEvent(IMFMediaEvent* event, MediaEventType& type) {
    type = MEUnknown;
    HRESULT status = S_OK;
    event->GetType(&type);
    event->GetStatus(&status);

    if (FAILED(status)) {
        g_state = State::Error;
        g_error = L"Файл не проигрывается: нет кодека или он повреждён.";
        g_switching = false;
        return true;
    }

    switch (type) {
        case MESessionTopologyStatus: {
            UINT32 topoStatus = MF_TOPOSTATUS_INVALID;
            if (SUCCEEDED(event->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &topoStatus)) &&
                topoStatus == MF_TOPOSTATUS_READY) {
                OnTopologyReady();
                return true;
            }
            return false;
        }
        case MESessionStarted:
            if (g_state != State::Paused) g_state = State::Playing;
            return true;
        case MESessionPaused:
            if (g_state == State::Playing) g_state = State::Paused;
            return true;
        case MEEndOfPresentation:
        case MESessionEnded:
            // Конец старой презентации во время смены трека — не повод включать следующий:
            // иначе одно нажатие «дальше» пролетало бы сразу через два трека.
            if (g_switching || g_state == State::Opening) return false;
            g_ended = true;
            g_state = State::Paused;
            g_lastPositionMs = g_durationMs;
            return true;
        case MEError:
            g_state = State::Error;
            g_error = L"Звуковая система вернула ошибку.";
            g_switching = false;
            return true;
        default:
            return false;
    }
}

// Разбор очереди событий. Возвращает true, если состояние изменилось.
bool DrainEvents() {
    if (!g_session) return false;
    bool changed = false;

    for (int guard = 0; guard < 64; guard++) {
        IMFMediaEvent* event = nullptr;
        const HRESULT hr = g_session->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE || FAILED(hr) || !event) {
            if (event) event->Release();
            break;
        }
        MediaEventType type = MEUnknown;
        if (HandleEvent(event, type)) changed = true;
        event->Release();
    }
    return changed;
}

}  // namespace

bool Startup() {
    if (g_started) return true;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) return false;
    g_started = true;
    return true;
}

void Shutdown() {
    if (!g_started) return;
    Close();
    if (!g_started) return;
    MFShutdown();
    g_started = false;
}

void Close() {
    CloseSession();
    g_state = State::Idle;
    g_path.clear();
    g_durationMs = 0;
    g_lastPositionMs = 0;
    g_ended = false;
    g_switching = false;
}

bool Open(const std::wstring& path, unsigned long long startMs, bool paused) {
    if (!g_started && !Startup()) {
        g_error = L"Media Foundation недоступна в этой системе.";
        g_state = State::Error;
        return false;
    }
    if (path.empty()) return false;

    // Источник строим до закрытия старой сессии: если файл битый, музыка не оборвётся.
    IMFMediaSource* source = nullptr;
    unsigned long long durationMs = 0;
    if (!CreateSource(path, &source, durationMs)) {
        g_error = L"Файл не открывается: нет кодека или он повреждён.";
        g_state = State::Error;
        g_path = path;
        return false;
    }

    IMFTopology* topology = nullptr;
    if (!BuildTopology(source, &topology)) {
        ShutdownSource(source);
        g_error = L"В файле нет звуковой дорожки.";
        g_state = State::Error;
        g_path = path;
        return false;
    }

    g_switching = true;
    g_state = State::Opening;
    g_openingSince = GetTickCount64();

    // Новая сессия на каждый трек: см. комментарий у g_session. Заодно уходит и старый
    // источник — файл освобождается.
    if (!NewSession()) {
        topology->Release();
        ShutdownSource(source);
        g_error = L"Не удалось создать сессию воспроизведения.";
        g_state = State::Error;
        g_switching = false;
        return false;
    }

    const HRESULT hr = g_session->SetTopology(MFSESSION_SETTOPOLOGY_IMMEDIATE, topology);
    topology->Release();
    if (FAILED(hr)) {
        ShutdownSource(source);
        g_error = L"Не удалось построить цепочку воспроизведения.";
        g_state = State::Error;
        g_switching = false;
        return false;
    }
    g_source = source;

    g_path = path;
    g_durationMs = durationMs;
    g_lastPositionMs = startMs;
    g_pendingStartMs = startMs;
    g_pendingPaused = paused;
    g_ended = false;
    g_error.clear();

    // Событие готовности приходит через несколько миллисекунд. Ждём его здесь, а не в
    // таймере: иначе между нажатием и звуком была бы заметная задержка.
    for (int i = 0; i < 140 && g_state == State::Opening; i++) {
        if (!DrainEvents()) Sleep(5);
    }
    g_switching = false;
    return g_state != State::Error;
}

void Play() {
    if (!g_session) return;
    if (g_state == State::Playing) return;
    // В Opening стартовать нельзя: топология ещё собирается, и своим Start мы бы сбили
    // позицию, с которой её вот-вот запустит OnTopologyReady.
    if (g_state == State::Idle || g_state == State::Error || g_state == State::Opening) return;

    PROPVARIANT start;
    PropVariantInit(&start);
    // Трек доиграл до конца: «играть» означает начать его заново.
    if (g_ended) {
        start.vt = VT_I8;
        start.hVal.QuadPart = 0;
        g_ended = false;
        g_lastPositionMs = 0;
    }
    g_session->Start(&GUID_NULL, &start);
    PropVariantClear(&start);
    g_state = State::Playing;
}

void Pause() {
    if (!g_session || g_state != State::Playing) return;
    g_session->Pause();
    g_state = State::Paused;
}

void TogglePlay() {
    if (g_state == State::Playing) Pause();
    else Play();
}

State Current() { return g_state; }
bool Playing() { return g_state == State::Playing; }
const std::wstring& OpenPath() { return g_path; }
const std::wstring& LastError() { return g_state == State::Error ? g_error : kNoError; }

unsigned long long PositionMs() {
    if (g_state == State::Idle) return 0;
    // Пока топология меняется, часы сессии принадлежат ещё старому треку — показываем ту
    // позицию, с которой новый собирается начать.
    if (g_state == State::Opening || g_switching) return g_lastPositionMs;
    if (g_clock) {
        MFTIME time = 0;
        if (SUCCEEDED(g_clock->GetTime(&time)) && time >= 0) {
            g_lastPositionMs = static_cast<unsigned long long>(time) / kHnsPerMs;
        }
    }
    if (g_durationMs > 0 && g_lastPositionMs > g_durationMs) g_lastPositionMs = g_durationMs;
    return g_lastPositionMs;
}

unsigned long long DurationMs() { return g_durationMs; }

void Seek(unsigned long long ms) {
    if (!g_session || g_state == State::Idle || g_state == State::Error) return;
    if (g_state == State::Opening || g_switching) return;  // трек ещё открывается
    if (g_durationMs > 0 && ms > g_durationMs) ms = g_durationMs;

    const bool keepPaused = (g_state == State::Paused) && !g_ended;
    PROPVARIANT start;
    PropVariantInit(&start);
    start.vt = VT_I8;
    start.hVal.QuadPart = static_cast<LONGLONG>(ms * kHnsPerMs);
    g_session->Start(&GUID_NULL, &start);
    PropVariantClear(&start);

    g_lastPositionMs = ms;
    g_ended = false;
    if (keepPaused) {
        g_session->Pause();
        g_state = State::Paused;
    } else {
        g_state = State::Playing;
    }
}

void SetVolume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volumePercent = percent;
    ApplyVolumeToSession();
}

bool Pump() {
    const bool changed = DrainEvents();
    // Страховка: если сессия так и не сказала, что топология готова, состояние «открываю»
    // залипло бы навсегда — кнопка «играть» перестала бы работать без всякого объяснения.
    if (g_state == State::Opening && g_openingSince != 0 &&
        GetTickCount64() - g_openingSince > 4000) {
        g_state = State::Error;
        g_error = L"Трек не запустился. Попробуйте другой.";
        g_switching = false;
        g_openingSince = 0;
        return true;
    }
    return changed;
}

bool TakeEnded() {
    if (!g_ended) return false;
    g_ended = false;
    return true;
}

}  // namespace audio
