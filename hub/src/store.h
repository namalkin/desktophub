// Настройки центра: %APPDATA%\WidgetHub\data.json.
#pragma once

#include <string>
#include <vector>

struct WidgetPath {
    std::string id;     // id из catalog.json
    std::wstring path;  // где лежит .exe — скачанный, указанный вручную или найденный у процесса
};

struct HubData {
    bool darkTheme = true;
    int accent = 2;  // синий
    std::wstring repo;  // «owner/name» на GitHub: оттуда качаются виджеты
    bool closeToTray = true;
    std::string selected;  // id выбранного виджета; пусто — страница настроек центра

    // Окно центра. x == y == INT_MIN — ещё ни разу не открывалось.
    int winX = -32768, winY = -32768;
    int winW = 980, winH = 680;
    bool maximized = false;

    std::vector<WidgetPath> paths;

    std::wstring PathOf(const std::string& id) const;
    void SetPath(const std::string& id, const std::wstring& path);
};

bool LoadData(HubData& data);
bool SaveData(const HubData& data);

// «https://github.com/owner/name/releases» -> «owner/name». Неразборчивое — пусто.
std::wstring NormalizeRepo(const std::wstring& input);
