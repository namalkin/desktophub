#include "store.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "json.h"
#include "util.h"

namespace {

constexpr int kBackupsToKeep = 7;

long long ToCents(double units) {
    return static_cast<long long>(std::llround(units * 100.0));
}

double ToUnits(long long cents) { return static_cast<double>(cents) / 100.0; }

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (8 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);

    // BOM, если файл правили вручную в блокноте.
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

// Пишем во временный файл и подменяем — так данные не теряются при сбое записи.
bool WriteWholeFileAtomic(const std::wstring& path, const std::string& content) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = TRUE;
    if (!content.empty()) {
        ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    }
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != content.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// Раз в сутки откладываем копию data.json и держим последние kBackupsToKeep штук.
void RotateBackups() {
    const std::wstring source = DataFile();
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) return;  // ещё нечего копировать

    const std::wstring dir = BackupDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    const std::wstring today = Utf8ToWide(TodayIso());
    const std::wstring target = dir + L"\\data-" + today + L".json";
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) return;  // копия за сегодня уже есть
    CopyFileW(source.c_str(), target.c_str(), TRUE);

    std::vector<std::wstring> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\data-*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) found.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (found.size() <= static_cast<size_t>(kBackupsToKeep)) return;

    std::sort(found.begin(), found.end());  // имена с ISO-датой сортируются хронологически
    const size_t extra = found.size() - static_cast<size_t>(kBackupsToKeep);
    for (size_t i = 0; i < extra; i++) {
        DeleteFileW((dir + L"\\" + found[i]).c_str());
    }
}

}  // namespace

std::wstring BackupDir() { return DataDir() + L"\\backups"; }

const Goal& AppData::ActiveGoal() const {
    static const Goal fallback;  // на случай повреждённых данных
    if (goals.empty()) return fallback;
    const size_t i = static_cast<size_t>(activeGoal);
    return i < goals.size() ? goals[i] : goals.front();
}

Goal& AppData::ActiveGoal() {
    if (goals.empty()) goals.push_back(Goal{});  // список событий всегда непуст
    const size_t i = static_cast<size_t>(activeGoal);
    return i < goals.size() ? goals[i] : goals.front();
}

void AppData::SelectGoal(int index) {
    if (goals.empty()) return;
    const int count = static_cast<int>(goals.size());
    activeGoal = ((index % count) + count) % count;  // корректно и для отрицательных
}

long long AppData::Balance() const {
    const Goal& goal = ActiveGoal();
    long long sum = goal.initialDebtCents;
    for (const auto& t : goal.tx) sum += t.cents;
    return sum;
}

long long AppData::PaidCents() const {
    long long paid = 0;
    for (const auto& t : ActiveGoal().tx) {
        if (t.cents < 0) paid += -t.cents;
    }
    return paid;
}

bool LoadData(AppData& data) {
    std::string raw;
    if (!ReadWholeFile(DataFile(), raw)) return false;

    mj::Value root;
    if (!mj::Parse(raw, root) || root.type != mj::Type::Object) return false;

    auto readTransactions = [](const mj::Value& list, std::vector<Transaction>& out) {
        if (list.type != mj::Type::Array) return;
        out.clear();
        out.reserve(list.arr.size());
        for (const mj::Value& item : list.arr) {
            if (item.type != mj::Type::Object) continue;
            Transaction t;
            if (const mj::Value* a = item.Find("amount")) {
                if (a->type != mj::Type::Number) continue;
                t.cents = ToCents(a->number);
            } else {
                continue;
            }
            if (const mj::Value* d = item.Find("description")) {
                if (d->type == mj::Type::String) t.desc = Utf8ToWide(d->str);
            }
            if (const mj::Value* d = item.Find("date")) {
                if (d->type == mj::Type::String) t.date = Utf8ToWide(d->str);
            }
            out.push_back(std::move(t));
        }
    };

    // Файлы прежних версий: долг и операции лежали в корне и были общими на все события.
    bool legacyDebt = false;
    long long legacyDebtCents = 0;
    std::vector<Transaction> legacyTx;
    if (const mj::Value* v = root.Find("initialDebt")) {
        if (v->type == mj::Type::Number) {
            legacyDebtCents = ToCents(v->number);
            legacyDebt = true;
        }
    }
    if (const mj::Value* list = root.Find("transactions")) {
        readTransactions(*list, legacyTx);
        legacyDebt = legacyDebt || !legacyTx.empty();
    }

    bool goalHasOwnDebt = false;
    auto readGoal = [&](const mj::Value& item, Goal& out) {
        if (item.type != mj::Type::Object) return false;
        if (const mj::Value* t = item.Find("title")) {
            if (t->type == mj::Type::String && !t->str.empty()) out.title = Utf8ToWide(t->str);
        }
        if (const mj::Value* d = item.Find("date")) {
            if (d->type == mj::Type::String) out.date = Utf8ToWide(d->str);
        }
        if (const mj::Value* v = item.Find("initialDebt")) {
            if (v->type == mj::Type::Number) {
                out.initialDebtCents = ToCents(v->number);
                goalHasOwnDebt = true;
            }
        }
        if (const mj::Value* list = item.Find("transactions")) {
            readTransactions(*list, out.tx);
            goalHasOwnDebt = true;
        }
        return true;
    };

    if (const mj::Value* list = root.Find("goals")) {
        if (list->type == mj::Type::Array && !list->arr.empty()) {
            std::vector<Goal> goals;
            for (const mj::Value& item : list->arr) {
                Goal g;
                if (readGoal(item, g)) goals.push_back(std::move(g));
            }
            if (!goals.empty()) data.goals = std::move(goals);
        }
    } else if (const mj::Value* goal = root.Find("goal")) {
        // Формат v2: одно событие. Читаем как список из одного элемента.
        Goal g;
        if (readGoal(*goal, g)) data.goals.assign(1, g);
    }

    // Общий долг старого формата достаётся первому событию — остальные начинают с нуля.
    if (legacyDebt && !goalHasOwnDebt && !data.goals.empty()) {
        data.goals.front().initialDebtCents = legacyDebtCents;
        data.goals.front().tx = std::move(legacyTx);
        for (size_t i = 1; i < data.goals.size(); i++) {
            data.goals[i].initialDebtCents = 0;
            data.goals[i].tx.clear();
        }
    }
    if (const mj::Value* active = root.Find("activeGoal")) {
        if (active->type == mj::Type::Number) data.SelectGoal(static_cast<int>(active->number));
    }
    if (const mj::Value* look = root.Find("appearance")) {
        if (look->type == mj::Type::Object) {
            if (const mj::Value* t = look->Find("theme")) {
                if (t->type == mj::Type::String) data.darkTheme = (t->str != "light");
            }
            if (const mj::Value* a = look->Find("accent")) {
                if (a->type == mj::Type::Number) data.accent = static_cast<int>(a->number);
            }
            if (const mj::Value* o = look->Find("opacity")) {
                if (o->type == mj::Type::Number) {
                    data.opacity = static_cast<int>(o->number);
                    if (data.opacity < 40) data.opacity = 40;
                    if (data.opacity > 100) data.opacity = 100;
                }
            }
        }
    }
    if (const mj::Value* hk = root.Find("hotkey")) data.hotkeyEnabled = hk->BoolOr(true);
    if (const mj::Value* hd = root.Find("hideDebt")) data.hideDebt = hd->BoolOr(false);

    if (const mj::Value* w = root.Find("window")) {
        if (w->type == mj::Type::Object) {
            if (const mj::Value* x = w->Find("x")) {
                if (x->type == mj::Type::Number) data.winX = static_cast<int>(x->number);
            }
            if (const mj::Value* y = w->Find("y")) {
                if (y->type == mj::Type::Number) data.winY = static_cast<int>(y->number);
            }
            if (const mj::Value* t = w->Find("alwaysOnTop")) data.alwaysOnTop = t->BoolOr(false);
        }
    }
    return true;
}

bool SaveData(const AppData& data) {
    mj::Value root = mj::Value::Object();

    mj::Value goals = mj::Value::Array();
    for (const Goal& g : data.goals) {
        mj::Value item = mj::Value::Object();
        item.Set("title", mj::Value::Str(WideToUtf8(g.title)));
        item.Set("date", mj::Value::Str(WideToUtf8(g.date)));
        item.Set("initialDebt", mj::Value::Num(ToUnits(g.initialDebtCents)));

        mj::Value list = mj::Value::Array();
        for (const Transaction& t : g.tx) {
            mj::Value entry = mj::Value::Object();
            entry.Set("amount", mj::Value::Num(ToUnits(t.cents)));
            entry.Set("description", mj::Value::Str(WideToUtf8(t.desc)));
            entry.Set("date", mj::Value::Str(WideToUtf8(t.date)));
            list.arr.push_back(std::move(entry));
        }
        item.Set("transactions", std::move(list));
        goals.arr.push_back(std::move(item));
    }
    root.Set("goals", std::move(goals));
    root.Set("activeGoal", mj::Value::Num(data.activeGoal));

    mj::Value look = mj::Value::Object();
    look.Set("theme", mj::Value::Str(data.darkTheme ? "dark" : "light"));
    look.Set("accent", mj::Value::Num(data.accent));
    look.Set("opacity", mj::Value::Num(data.opacity));
    root.Set("appearance", std::move(look));

    root.Set("hotkey", mj::Value::Bool(data.hotkeyEnabled));
    root.Set("hideDebt", mj::Value::Bool(data.hideDebt));

    mj::Value win = mj::Value::Object();
    win.Set("x", mj::Value::Num(data.winX));
    win.Set("y", mj::Value::Num(data.winY));
    win.Set("alwaysOnTop", mj::Value::Bool(data.alwaysOnTop));
    root.Set("window", std::move(win));

    const std::wstring dir = DataDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);  // no-op, если уже есть
    RotateBackups();
    return WriteWholeFileAtomic(DataFile(), mj::Serialize(root));
}
