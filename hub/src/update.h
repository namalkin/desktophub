// Проверка обновлений: последний релиз GitHub против версии центра и файлов виджетов.
//
// Центр сравнивается по номеру версии (тег релиза «v1.2.0» против вшитой при сборке),
// виджеты — по содержимому: GitHub отдаёт SHA-256 каждого файла релиза, и если он не
// совпадает с хешем локального .exe, у виджета есть обновление. Так видно и виджет,
// поставленный мимо центра. Если хеша в ответе нет, сравнивается размер.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace update {

constexpr UINT kMsgChecked = WM_APP + 30;  // lParam — 1 удачно / 0 ошибка

struct Asset {
    std::wstring name;
    long long size = 0;
    std::string sha256;  // hex в нижнем регистре; пусто — GitHub его не прислал
};

struct Release {
    std::wstring tag;  // «v1.2.0»
    std::wstring url;  // страница релиза
    std::vector<Asset> assets;

    const Asset* Find(const std::wstring& name) const;
};

std::wstring CurrentVersion();  // версия этого центра, «1.1.0»

// Сравнение версий «v1.2.0» / «1.10» по числам: <0, 0, >0.
int CompareVersions(const std::wstring& a, const std::wstring& b);

void Check(HWND notify, const std::wstring& repo);  // в фоне; итог — kMsgChecked
bool Checking();
bool TakeResult(Release& out, std::wstring& error);  // после kMsgChecked

// Совпадает ли локальный файл с файлом релиза. Хеш кэшируется по времени изменения.
bool SameAsAsset(const std::wstring& path, const Asset& asset);

// Центр запущен из hub\dist репозитория: сам себя он тогда не заменяет — это сборка разработчика.
bool InRepoCheckout();

}  // namespace update
