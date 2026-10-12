#include "dragout.h"

#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace dragout {

namespace {

// Источник перетаскивания: правила простые — Esc отменяет, отпущенная кнопка бросает.
class Source : public IDropSource {
public:
    virtual ~Source() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
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

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) return DRAGDROP_S_CANCEL;
        if (!(keyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ULONG refs_ = 1;
};

}  // namespace

DWORD DragFiles(const std::vector<std::wstring>& paths) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const std::wstring& path : paths) {
        if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) pidls.push_back(pidl);
    }
    if (pidls.empty()) return 0;

    DWORD effect = 0;
    IShellItemArray* array = nullptr;
    // Готовый объект данных от оболочки: в нём и CF_HDROP, и всё остальное,
    // чего ждут проводник и рабочий стол.
    if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                                    const_cast<PCIDLIST_ABSOLUTE*>(pidls.data()), &array))) {
        IDataObject* data = nullptr;
        if (SUCCEEDED(array->BindToHandler(nullptr, BHID_DataObject, IID_IDataObject,
                                           reinterpret_cast<void**>(&data)))) {
            Source* source = new Source();
            DWORD result = 0;
            const HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK,
                                          &result);
            if (hr == DRAGDROP_S_DROP) effect = result;
            source->Release();
            data->Release();
        }
        array->Release();
    }

    for (PIDLIST_ABSOLUTE pidl : pidls) ILFree(pidl);
    return effect;
}

}  // namespace dragout
