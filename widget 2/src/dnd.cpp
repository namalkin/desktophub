#include "dnd.h"

#include <objidl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <map>

#include "util.h"

namespace {

// Формат «просто ссылка» — им браузеры отдают перетаскиваемый адрес страницы.
UINT UrlFormatW() {
    static const UINT id = RegisterClipboardFormatW(L"UniformResourceLocatorW");
    return id;
}
UINT UrlFormatA() {
    static const UINT id = RegisterClipboardFormatW(L"UniformResourceLocator");
    return id;
}

bool ReadGlobal(IDataObject* data, UINT format, STGMEDIUM& medium) {
    FORMATETC fmt = {static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SUCCEEDED(data->GetData(&fmt, &medium));
}

// Корзина и прочие виртуальные папки приходят не файлами, а списком PIDL.
// Их «путь» — парсинг-имя вида ::{645FF040-…}: по нему оболочка потом найдёт объект.
void ExtractShellItems(IDataObject* data, std::vector<std::wstring>& paths) {
    static const UINT format = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    STGMEDIUM medium = {};
    if (!ReadGlobal(data, format, medium)) return;

    if (auto* cida = static_cast<CIDA*>(GlobalLock(medium.hGlobal))) {
        const auto* base = reinterpret_cast<const BYTE*>(cida);
        auto pidlAt = [&](UINT index) {
            return reinterpret_cast<LPCITEMIDLIST>(base + cida->aoffset[index]);
        };
        LPCITEMIDLIST folder = pidlAt(0);
        for (UINT i = 0; i < cida->cidl; i++) {
            PIDLIST_ABSOLUTE full = ILCombine(folder, pidlAt(i + 1));
            if (!full) continue;
            PWSTR name = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(full, SIGDN_DESKTOPABSOLUTEPARSING, &name))) {
                paths.emplace_back(name);
                CoTaskMemFree(name);
            }
            ILFree(full);
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
}

std::vector<std::wstring> ExtractPaths(IDataObject* data) {
    std::vector<std::wstring> paths;
    if (!data) return paths;

    STGMEDIUM medium = {};
    if (ReadGlobal(data, CF_HDROP, medium)) {
        if (auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; i++) {
                wchar_t buf[MAX_PATH * 2] = {0};
                if (DragQueryFileW(drop, i, buf, MAX_PATH * 2) > 0) paths.emplace_back(buf);
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
        return paths;
    }

    ExtractShellItems(data, paths);
    if (!paths.empty()) return paths;

    // Ссылка из браузера: сначала широкий формат, потом однобайтовый, потом обычный текст.
    auto takeText = [&](UINT format, bool wide) {
        STGMEDIUM text = {};
        if (!ReadGlobal(data, format, text)) return;
        if (void* raw = GlobalLock(text.hGlobal)) {
            std::wstring value = wide ? std::wstring(static_cast<const wchar_t*>(raw))
                                      : Utf8ToWide(std::string(static_cast<const char*>(raw)));
            value = TrimSpaces(value);
            if (IsUrl(value)) paths.push_back(value);
            GlobalUnlock(text.hGlobal);
        }
        ReleaseStgMedium(&text);
    };
    takeText(UrlFormatW(), true);
    if (paths.empty()) takeText(UrlFormatA(), false);
    if (paths.empty()) takeText(CF_UNICODETEXT, true);
    return paths;
}

class Target : public IDropTarget {
public:
    Target(HWND hwnd, DropSink* sink) : hwnd_(hwnd), sink_(sink) {}
    // У IDropTarget деструктор невиртуальный, а удаляем мы себя сами в Release().
    virtual ~Target() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keyState, POINTL pt,
                                        DWORD* effect) override {
        paths_ = ExtractPaths(data);
        return Update(keyState, pt, effect);
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL pt, DWORD* effect) override {
        return Update(keyState, pt, effect);
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        paths_.clear();
        sink_->OnDragLeave();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keyState, POINTL pt,
                                   DWORD* effect) override {
        if (paths_.empty()) paths_ = ExtractPaths(data);
        const DWORD allowed = effect ? *effect : DROPEFFECT_COPY;
        const bool copyMode = (keyState & MK_CONTROL) != 0;
        if (!paths_.empty()) sink_->OnDrop(paths_, ToClient(pt), copyMode);
        sink_->OnDragLeave();
        paths_.clear();
        if (effect) *effect = PickEffect(allowed);
        return S_OK;
    }

private:
    POINT ToClient(POINTL pt) const {
        POINT client = {pt.x, pt.y};
        ScreenToClient(hwnd_, &client);
        return client;
    }

    // Источник сообщает во входном *effect, что ему вообще позволено: корзину, например,
    // разрешено только «связать». Ответить чем-то другим — значит отменить перетаскивание.
    static DWORD PickEffect(DWORD allowed) {
        if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
        if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
        // DROPEFFECT_MOVE не берём: файл ярлыка виджет уже перенёс сам,
        // и проводник стал бы удалять то, чего на месте нет.
        return DROPEFFECT_NONE;
    }

    HRESULT Update(DWORD keyState, POINTL pt, DWORD* effect) {
        const DWORD allowed = effect ? *effect : DROPEFFECT_COPY;
        const bool copyMode = (keyState & MK_CONTROL) != 0;
        const bool accept = !paths_.empty() && sink_->OnDragOver(ToClient(pt), copyMode);
        if (effect) *effect = accept ? PickEffect(allowed) : DROPEFFECT_NONE;
        return S_OK;
    }

    HWND hwnd_;
    DropSink* sink_;
    std::vector<std::wstring> paths_;
    ULONG refs_ = 1;
};

// Блоков на рабочем столе может быть сколько угодно, и каждый принимает перетаскивание сам.
std::map<HWND, Target*> g_targets;

}  // namespace

bool RegisterDrops(HWND hwnd, DropSink* sink) {
    if (g_targets.count(hwnd)) return false;
    Target* target = new Target(hwnd, sink);
    if (FAILED(RegisterDragDrop(hwnd, target))) {
        target->Release();
        return false;
    }
    g_targets.emplace(hwnd, target);
    return true;
}

void UnregisterDrops(HWND hwnd) {
    auto found = g_targets.find(hwnd);
    if (found == g_targets.end()) return;
    RevokeDragDrop(hwnd);
    found->second->Release();
    g_targets.erase(found);
}
