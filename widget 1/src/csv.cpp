#include "csv.h"

#include <commdlg.h>

#include <string>
#include <vector>

#include "util.h"

namespace {

constexpr wchar_t kFilter[] = L"CSV (*.csv)\0*.csv\0Все файлы (*.*)\0*.*\0\0";

std::wstring QuoteField(const std::wstring& value) {
    bool needQuotes = false;
    for (wchar_t c : value) {
        if (c == L';' || c == L'"' || c == L'\n' || c == L'\r') {
            needQuotes = true;
            break;
        }
    }
    if (!needQuotes) return value;

    std::wstring out = L"\"";
    for (wchar_t c : value) {
        if (c == L'"') out += L'"';  // удвоение по RFC 4180
        out += c;
    }
    out += L'"';
    return out;
}

// Сумма для Excel в русской локали: десятичная запятая, без разделителей тысяч.
std::wstring AmountForCsv(long long cents) {
    const bool negative = cents < 0;
    unsigned long long abs = negative ? static_cast<unsigned long long>(-(cents + 1)) + 1ULL
                                      : static_cast<unsigned long long>(cents);
    wchar_t buf[48];
    if (abs % 100 == 0) {
        swprintf(buf, 48, L"%s%llu", negative ? L"-" : L"", abs / 100);
    } else {
        swprintf(buf, 48, L"%s%llu,%02u", negative ? L"-" : L"", abs / 100,
                 static_cast<unsigned>(abs % 100));
    }
    return std::wstring(buf);
}

std::vector<std::wstring> SplitLine(const std::wstring& line, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); i++) {
        const wchar_t c = line[i];
        if (inQuotes) {
            if (c == L'"') {
                if (i + 1 < line.size() && line[i + 1] == L'"') {
                    cur += L'"';
                    i++;
                } else {
                    inQuotes = false;
                }
            } else {
                cur += c;
            }
        } else if (c == L'"') {
            inQuotes = true;
        } else if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (16 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

}  // namespace

bool ExportCsv(HWND owner, const AppData& data, std::wstring& savedPath) {
    wchar_t path[MAX_PATH] = {0};
    const std::wstring suggested = L"NewYearWidget-" + Utf8ToWide(TodayIso()) + L".csv";
    wcsncpy(path, suggested.c_str(), MAX_PATH - 1);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Экспорт операций в CSV";
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;

    std::wstring text = L"Дата;Сумма;Описание\r\n";
    for (const Transaction& t : data.ActiveGoal().tx) {
        text += QuoteField(t.date);
        text += L';';
        text += AmountForCsv(t.cents);
        text += L';';
        text += QuoteField(t.desc);
        text += L"\r\n";
    }

    std::string utf8 = "\xEF\xBB\xBF";  // BOM: без него Excel читает как ANSI
    utf8 += WideToUtf8(text);

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != utf8.size()) return false;

    savedPath.assign(path);
    return true;
}

bool ImportCsv(HWND owner, AppData& data, int& added) {
    added = 0;

    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Импорт операций из CSV";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;

    std::string raw;
    if (!ReadFileUtf8(path, raw)) return false;
    const std::wstring text = Utf8ToWide(raw);

    // Разделитель определяем по первой строке: ';' привычнее Excel в RU, ',' — экспортам.
    size_t firstBreak = text.find(L'\n');
    const std::wstring probe = text.substr(0, firstBreak == std::wstring::npos ? text.size() : firstBreak);
    const wchar_t sep = (probe.find(L';') != std::wstring::npos) ? L';' : L',';

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (line.empty()) {
            if (end >= text.size()) break;
            continue;
        }

        const std::vector<std::wstring> cols = SplitLine(line, sep);
        if (cols.size() < 2) {
            if (end >= text.size()) break;
            continue;
        }

        long long cents = 0;
        if (!ParseAmount(cols[1], cents) || cents == 0) {
            if (end >= text.size()) break;
            continue;  // заголовок или мусорная строка — пропускаем молча
        }

        Transaction t;
        t.cents = cents;
        t.date = cols[0];
        if (t.date.empty()) t.date = Utf8ToWide(TodayIso());
        if (cols.size() >= 3) t.desc = cols[2];
        data.ActiveGoal().tx.push_back(std::move(t));
        added++;

        if (end >= text.size()) break;
    }

    return added > 0;
}
