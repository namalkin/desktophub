#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Transaction {
    long long cents = 0;    // < 0 — выплата (долг уменьшается), > 0 — долг вырос
    std::wstring desc;
    std::wstring date;      // ISO, "2026-09-05"
};

// Событие для обратного отсчёта. Пустая дата — ближайшее 1 января
// с автопереходом на следующий год. Долг и операции у каждого события свои:
// отпуск и ремонт — это разные деньги.
struct Goal {
    std::wstring title = L"NEW YEAR";
    std::wstring date;  // "YYYY-MM-DD" или пусто

    long long initialDebtCents = 5000000;  // 50 000 ₽ по умолчанию
    std::vector<Transaction> tx;
};

struct AppData {
    std::vector<Goal> goals{Goal{}};  // всегда непусто
    int activeGoal = 0;

    const Goal& ActiveGoal() const;
    Goal& ActiveGoal();
    void SelectGoal(int index);

    // Оформление.
    bool darkTheme = true;
    int accent = 0;
    int opacity = 100;  // 60..100, прозрачность всей карточки

    bool hotkeyEnabled = true;  // Ctrl+Alt+N — быстрое добавление операции
    bool hideDebt = false;      // сумма долга закрыта, показывается только при наведении

    // Состояние окна — тоже здесь, чтобы был ровно один файл настроек.
    int winX = INT32_MIN;  // INT32_MIN => позиция ещё не выбрана
    int winY = INT32_MIN;
    bool alwaysOnTop = false;

    // Считаются по текущему событию.
    long long Balance() const;
    long long PaidCents() const;  // сколько уже погашено (для прогресс-бара)
};

// Ошибки чтения не фатальны: битый/отсутствующий файл => значения по умолчанию.
bool LoadData(AppData& data);
bool SaveData(const AppData& data);

// Папка с резервными копиями: %APPDATA%\NewYearWidget\backups
std::wstring BackupDir();
