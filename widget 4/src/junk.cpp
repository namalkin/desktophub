#include "junk.h"

#include <windows.h>

#include "util.h"

namespace junk {

namespace {

// Шаблон правила до раскрытия путей: пути записаны с переменными окружения и «*».
struct Template {
    int id;
    const wchar_t* name;
    const wchar_t* hint;
    bool userData;
    bool needsAdmin;
    bool toRecycleBin;
    bool deleteRoot;
    bool special;
    int minAgeDays;
    const wchar_t* roots;     // пути через «|»
    const wchar_t* patterns;  // маски через «|», пусто — любые файлы
};

// Порядок тот же, в каком группы показываются в блоке «Мусор»: сверху то,
// что обычно весит больше всего.
const Template kTemplates[] = {
    {TempUser, L"Временные файлы", L"%TEMP% и локальный Temp — их программы создают заново", false, false,
     false, false, false, 1, L"%TEMP%|%LOCALAPPDATA%\\Temp", L""},

    {TempWindows, L"Временные файлы Windows", L"C:\\Windows\\Temp — общая свалка установщиков", false, true,
     false, false, false, 1, L"%SystemRoot%\\Temp", L""},

    {RecycleBin, L"Корзина", L"Пока корзина не очищена, место не освобождается", false, false, false, false,
     true, 0, L"", L""},

    {WindowsUpdate, L"Кэш обновлений Windows", L"Скачанные пакеты обновлений: нужны только на время установки",
     false, true, false, false, false, 0,
     L"%SystemRoot%\\SoftwareDistribution\\Download|%SystemRoot%\\SoftwareDistribution\\DataStore\\Logs", L""},

    {DeliveryOptimization, L"Кэш доставки обновлений", L"Раздача обновлений соседним компьютерам", false, true,
     false, false, false, 0,
     L"%SystemRoot%\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization"
     L"\\Cache",
     L""},

    {Browsers, L"Кэш браузеров", L"Картинки и скрипты сайтов — закачаются снова при заходе", false, false,
     false, false, false, 0,
     L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Cache|"
     L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Code Cache|"
     L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\GPUCache|"
     L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Service Worker\\CacheStorage|"
     L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\Cache|"
     L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\Code Cache|"
     L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\GPUCache|"
     L"%LOCALAPPDATA%\\Yandex\\YandexBrowser\\User Data\\*\\Cache|"
     L"%LOCALAPPDATA%\\Yandex\\YandexBrowser\\User Data\\*\\GPUCache|"
     L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\*\\Cache|"
     L"%LOCALAPPDATA%\\Opera Software\\*\\Cache|"
     L"%LOCALAPPDATA%\\Mozilla\\Firefox\\Profiles\\*\\cache2|"
     L"%APPDATA%\\Mozilla\\Firefox\\Profiles\\*\\cache2|"
     L"%LOCALAPPDATA%\\Vivaldi\\User Data\\*\\Cache",
     L""},

    {AppCaches, L"Кэш программ", L"Discord, Teams, Spotify, Slack и им подобные", false, false, false, false,
     false, 0,
     L"%APPDATA%\\discord\\Cache|%APPDATA%\\discord\\Code Cache|%APPDATA%\\discord\\GPUCache|"
     L"%APPDATA%\\Microsoft\\Teams\\Cache|%APPDATA%\\Microsoft\\Teams\\Code Cache|"
     L"%APPDATA%\\Microsoft\\Teams\\GPUCache|"
     L"%LOCALAPPDATA%\\Packages\\MSTeams_8wekyb3d8bbwe\\LocalCache\\Microsoft\\MSTeams\\Logs|"
     L"%LOCALAPPDATA%\\Spotify\\Storage|%LOCALAPPDATA%\\Spotify\\Data|"
     L"%APPDATA%\\Slack\\Cache|%APPDATA%\\Slack\\Code Cache|"
     L"%APPDATA%\\Telegram Desktop\\tdata\\user_data\\cache|"
     L"%LOCALAPPDATA%\\JetBrains\\*\\caches|"
     L"%APPDATA%\\Code\\Cache|%APPDATA%\\Code\\CachedData|%APPDATA%\\Code\\GPUCache",
     L""},

    {Thumbnails, L"Эскизы и значки", L"База предпросмотра проводника — строится заново на лету", false, false,
     false, false, false, 0, L"%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer",
     L"thumbcache_*.db|iconcache_*.db"},

    {ShaderCache, L"Кэш шейдеров", L"Видеодрайверы и игры пересоберут его при первом запуске", false, false,
     false, false, false, 0,
     L"%LOCALAPPDATA%\\D3DSCache|%LOCALAPPDATA%\\NVIDIA\\DXCache|%LOCALAPPDATA%\\NVIDIA\\GLCache|"
     L"%APPDATA%\\NVIDIA\\ComputeCache|%LOCALAPPDATA%\\AMD\\DxCache|%LOCALAPPDATA%\\AMD\\GLCache|"
     L"%ProgramFiles(x86)%\\Steam\\steamapps\\shadercache|"
     L"%LOCALAPPDATA%\\Microsoft\\Windows\\INetCache\\IE",
     L""},

    {CrashDumps, L"Отчёты о сбоях", L"Дампы упавших программ и синих экранов", false, false, false, false,
     false, 0, L"%LOCALAPPDATA%\\CrashDumps|%SystemRoot%\\Minidump|%SystemRoot%\\LiveKernelReports", L""},

    {ErrorReports, L"Очереди отчётов об ошибках", L"Папки WER: ждут отправки в Microsoft и копятся годами",
     false, false, false, false, false, 0,
     L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER|%ProgramData%\\Microsoft\\Windows\\WER", L""},

    {WindowsLogs, L"Журналы Windows", L"Логи установки и обслуживания системы", false, true, false, false,
     false, 7, L"%SystemRoot%\\Logs|%SystemRoot%\\Panther|%SystemRoot%\\System32\\LogFiles", L""},

    // Дальше — то, что виджет сам не отмечает: там лежат файлы пользователя.
    {WindowsOld, L"Папка Windows.old", L"Прошлая версия Windows вместе со старыми файлами — удаляется целиком",
     true, true, false, true, false, 0, L"%SystemDrive%\\Windows.old|%SystemDrive%\\$Windows.~BT", L""},

    {OldInstallers, L"Старые установщики в Загрузках",
     L"Дистрибутивы и архивы старше полугода — уедут в корзину", true, false, true, false, false, 180,
     L"%USERPROFILE%\\Downloads|%USERPROFILE%\\Загрузки",
     L"*.exe|*.msi|*.iso|*.zip|*.7z|*.rar|*.cab"},
};

std::vector<std::wstring> Split(const std::wstring& text, wchar_t sep) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t next = text.find(sep, start);
        const std::wstring part = text.substr(start, next == std::wstring::npos ? std::wstring::npos
                                                                                : next - start);
        if (!part.empty()) parts.push_back(part);
        if (next == std::wstring::npos) break;
        start = next + 1;
    }
    return parts;
}

}  // namespace

bool WildcardMatch(const std::wstring& name, const std::wstring& pattern) {
    // Классический разбор с запоминанием последней «*»: рекурсия тут не нужна.
    size_t n = 0, p = 0;
    size_t starP = std::wstring::npos, starN = 0;
    while (n < name.size()) {
        const bool same = p < pattern.size() &&
                          (pattern[p] == L'?' || towlower(pattern[p]) == towlower(name[n]));
        if (same) {
            n++;
            p++;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            starP = p++;
            starN = n;
        } else if (starP != std::wstring::npos) {
            p = starP + 1;
            n = ++starN;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') p++;
    return p == pattern.size();
}

std::vector<std::wstring> ExpandWildcards(const std::wstring& pattern) {
    std::vector<std::wstring> result;
    const size_t star = pattern.find(L'*');
    if (star == std::wstring::npos) {
        if (IsDirectory(pattern)) result.push_back(pattern);
        return result;
    }

    // Разбираем только тот сегмент пути, в котором стоит первая звёздочка,
    // и повторяем разбор для каждой найденной папки.
    const size_t slashBefore = pattern.rfind(L'\\', star);
    const size_t slashAfter = pattern.find(L'\\', star);
    if (slashBefore == std::wstring::npos) return result;

    const std::wstring base = pattern.substr(0, slashBefore);
    const std::wstring mask = pattern.substr(slashBefore + 1, slashAfter == std::wstring::npos
                                                                  ? std::wstring::npos
                                                                  : slashAfter - slashBefore - 1);
    const std::wstring tail = slashAfter == std::wstring::npos ? std::wstring() : pattern.substr(slashAfter);
    if (!IsDirectory(base)) return result;

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(LongPath(base + L"\\*").c_str(), FindExInfoBasic, &fd,
                                   FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return result;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
            continue;
        }
        if (!WildcardMatch(fd.cFileName, mask)) continue;
        const std::wstring next = base + L"\\" + fd.cFileName + tail;
        for (std::wstring& found : ExpandWildcards(next)) result.push_back(std::move(found));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return result;
}

std::vector<Rule> Rules(wchar_t drive) {
    std::vector<Rule> rules;
    const wchar_t upper = static_cast<wchar_t>(towupper(drive));

    for (const Template& tpl : kTemplates) {
        Rule rule;
        rule.id = tpl.id;
        rule.name = tpl.name;
        rule.hint = tpl.hint;
        rule.userData = tpl.userData;
        rule.needsAdmin = tpl.needsAdmin;
        rule.toRecycleBin = tpl.toRecycleBin;
        rule.deleteRoot = tpl.deleteRoot;
        rule.special = tpl.special;
        rule.minAgeDays = tpl.minAgeDays;
        rule.patterns = Split(tpl.patterns, L'|');

        if (tpl.special) {
            // Корзина одна на все диски — показываем её только для системного.
            wchar_t windows[MAX_PATH] = {0};
            const bool systemDrive = GetWindowsDirectoryW(windows, MAX_PATH) > 0 &&
                                     towupper(windows[0]) == upper;
            if (!systemDrive) continue;
            rules.push_back(std::move(rule));
            continue;
        }

        for (const std::wstring& raw : Split(tpl.roots, L'|')) {
            const std::wstring expanded = ExpandVars(raw);
            if (expanded.size() < 3 || expanded[1] != L':') continue;
            if (towupper(expanded[0]) != upper) continue;  // папка на другом диске
            for (std::wstring& path : ExpandWildcards(expanded)) rule.roots.push_back(std::move(path));
        }
        if (rule.roots.empty()) continue;  // такой программы на машине нет
        rules.push_back(std::move(rule));
    }
    return rules;
}

}  // namespace junk
