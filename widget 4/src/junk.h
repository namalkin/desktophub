// Правила «что считать мусором». Один список — и для подсчёта, и для очистки:
// показать пользователю одно, а удалить другое было бы нечестно.
//
// В список попадает только то, что Windows и программы создают заново сами.
// Ничего из этого не является документом пользователя, кроме групп с userData —
// такие сами не отмечаются и удаляются в корзину.
#pragma once

#include <string>
#include <vector>

namespace junk {

enum Id {
    TempUser = 1,
    TempWindows,
    RecycleBin,
    WindowsUpdate,
    DeliveryOptimization,
    Browsers,
    AppCaches,
    Thumbnails,
    ShaderCache,
    CrashDumps,
    ErrorReports,
    WindowsLogs,
    WindowsOld,
    OldInstallers,
};

struct Rule {
    int id = 0;
    const wchar_t* name = L"";
    const wchar_t* hint = L"";
    bool userData = false;      // не отмечаем сами, спрашиваем отдельно
    bool needsAdmin = false;    // без прав администратора почистится частично
    bool toRecycleBin = false;  // файлы пользователя — в корзину, кэши — сразу
    bool deleteRoot = false;    // удалять саму папку, а не только содержимое
    bool special = false;       // корзина: считается и чистится через оболочку
    int minAgeDays = 0;         // 0 — всё подряд; иначе только файлы старше
    std::vector<std::wstring> roots;     // существующие папки на нужном диске
    std::vector<std::wstring> patterns;  // маски имён; пусто — любые файлы
};

// Правила для одного диска: пути с других дисков и несуществующие папки отброшены,
// переменные окружения раскрыты, звёздочки в путях развёрнуты в реальные папки.
std::vector<Rule> Rules(wchar_t drive);

// Простое сравнение с маской «*» и «?», без учёта регистра.
bool WildcardMatch(const std::wstring& name, const std::wstring& pattern);

// Раскрывает «C:\...\User Data\*\Cache» в список существующих папок.
std::vector<std::wstring> ExpandWildcards(const std::wstring& pattern);

}  // namespace junk
