#include "update.h"

#include <bcrypt.h>
#include <wininet.h>

#include <algorithm>
#include <cwctype>
#include <map>

#include "json.h"
#include "util.h"

#ifndef HUB_VERSION
#define HUB_VERSION "0.0.0"
#endif

namespace update {

namespace {

struct Shared {
    CRITICAL_SECTION lock;
    bool ready = false;
    bool busy = false;
    bool ok = false;
    std::string body;  // ответ API
    std::wstring error;
};
Shared g;

struct Guard {
    Guard() { EnterCriticalSection(&g.lock); }
    ~Guard() { LeaveCriticalSection(&g.lock); }
};

struct CheckArgs {
    HWND notify;
    std::wstring repo;
};

DWORD WINAPI Worker(LPVOID param) {
    CheckArgs* args = static_cast<CheckArgs*>(param);
    std::string body;
    std::wstring error;

    HINTERNET net = InternetOpenW(L"WidgetHub/" HUB_VERSION, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) {
        error = L"Нет доступа к интернету.";
    } else {
        const std::wstring url = L"https://api.github.com/repos/" + args->repo + L"/releases/latest";
        // API GitHub без заголовка Accept отвечает тем же JSON, но так надёжнее.
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
        HINTERNET req = InternetOpenUrlW(net, url.c_str(), headers, static_cast<DWORD>(-1),
                                         INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI |
                                             INTERNET_FLAG_NO_COOKIES,
                                         0);
        if (!req) {
            error = L"Не удалось связаться с GitHub.";
        } else {
            DWORD status = 0, len = sizeof(status);
            HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr);
            char buf[16 * 1024];
            DWORD got = 0;
            while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0 && body.size() < (4u << 20)) {
                body.append(buf, got);
            }
            if (status == 404) error = L"В репозитории ещё нет релизов.";
            else if (status == 403) error = L"GitHub временно ограничил запросы — проверим позже.";
            else if (status != 200) error = L"GitHub ответил кодом " + std::to_wstring(status) + L".";
            InternetCloseHandle(req);
        }
        InternetCloseHandle(net);
    }

    {
        Guard lock;
        g.busy = false;
        g.ok = error.empty();
        g.body = body;
        g.error = error;
    }
    PostMessageW(args->notify, kMsgChecked, 0, error.empty() ? 1 : 0);
    delete args;
    return 0;
}

std::vector<int> Numbers(const std::wstring& v) {
    std::vector<int> out;
    int cur = -1;
    for (wchar_t ch : v) {
        if (iswdigit(ch)) {
            cur = (cur < 0 ? 0 : cur * 10) + (ch - L'0');
        } else if (cur >= 0) {
            out.push_back(cur);
            cur = -1;
            if (ch != L'.') break;  // «1.2.0-beta» — хвост после цифр не сравниваем
        }
    }
    if (cur >= 0) out.push_back(cur);
    return out;
}

std::string Sha256File(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();

    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        unsigned char buf[64 * 1024];
        DWORD got = 0;
        bool ok = true;
        while (ReadFile(file, buf, sizeof(buf), &got, nullptr) && got > 0) {
            if (BCryptHashData(hash, buf, got, 0) != 0) {
                ok = false;
                break;
            }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char kHex[] = "0123456789abcdef";
            for (unsigned char b : digest) {
                hex += kHex[b >> 4];
                hex += kHex[b & 15];
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return hex;
}

struct CachedHash {
    FILETIME written{};
    long long size = 0;
    std::string sha;
};
std::map<std::wstring, CachedHash> g_hashes;

}  // namespace

const Asset* Release::Find(const std::wstring& name) const {
    for (const Asset& a : assets) {
        if (EqualsNoCase(a.name, name)) return &a;
    }
    return nullptr;
}

std::wstring CurrentVersion() { return Utf8ToWide(HUB_VERSION); }

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    const std::vector<int> x = Numbers(a), y = Numbers(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        const int p = i < x.size() ? x[i] : 0;
        const int q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

void Check(HWND notify, const std::wstring& repo) {
    if (!g.ready) {
        InitializeCriticalSection(&g.lock);
        g.ready = true;
    }
    {
        Guard lock;
        if (g.busy || repo.empty()) return;
        g.busy = true;
    }
    HANDLE thread = CreateThread(nullptr, 0, &Worker, new CheckArgs{notify, repo}, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        Guard lock;
        g.busy = false;
    }
}

bool Checking() {
    if (!g.ready) return false;
    Guard lock;
    return g.busy;
}

bool TakeResult(Release& out, std::wstring& error) {
    std::string body;
    {
        Guard lock;
        error = g.error;
        if (!g.ok) return false;
        body = g.body;
    }
    mj::Value root;
    if (!mj::Parse(body, root) || root.type != mj::Type::Object) {
        error = L"GitHub прислал непонятный ответ.";
        return false;
    }
    out = Release{};
    if (const mj::Value* v = root.Find("tag_name")) out.tag = Utf8ToWide(v->str);
    if (const mj::Value* v = root.Find("html_url")) out.url = Utf8ToWide(v->str);
    if (const mj::Value* list = root.Find("assets")) {
        for (const mj::Value& a : list->arr) {
            Asset asset;
            if (const mj::Value* v = a.Find("name")) asset.name = Utf8ToWide(v->str);
            if (const mj::Value* v = a.Find("size")) asset.size = static_cast<long long>(v->NumOr(0));
            if (const mj::Value* v = a.Find("digest")) {
                // «sha256:9f86d0…» — алгоритм перед двоеточием.
                if (v->str.compare(0, 7, "sha256:") == 0) asset.sha256 = v->str.substr(7);
            }
            if (!asset.name.empty()) out.assets.push_back(asset);
        }
    }
    if (out.tag.empty()) {
        error = L"В ответе GitHub нет номера релиза.";
        return false;
    }
    return true;
}

bool SameAsAsset(const std::wstring& path, const Asset& asset) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return true;  // нечего сравнивать
    const long long size = (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (asset.sha256.empty()) return size == asset.size;
    if (size != asset.size) return false;

    CachedHash& c = g_hashes[path];
    if (c.sha.empty() || CompareFileTime(&c.written, &info.ftLastWriteTime) != 0 || c.size != size) {
        c.written = info.ftLastWriteTime;
        c.size = size;
        c.sha = Sha256File(path);
    }
    return c.sha.empty() || c.sha == asset.sha256;
}

bool InRepoCheckout() {
    const std::wstring hubDir = ParentDir(ExeDir());  // ...\hub
    return FileExists(hubDir + L"\\catalog.json") && FileExists(hubDir + L"\\src\\hub.cpp");
}

}  // namespace update
