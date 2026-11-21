#include "win_dialogs.hpp"

#include <objbase.h>
#include <shobjidl.h>

namespace koma {

namespace {

struct ComScope {
    HRESULT hr = E_FAIL;
    bool need_uninit = false;

    ComScope() {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (hr == S_OK || hr == S_FALSE) {
            need_uninit = true;
        }
    }

    ~ComScope() {
        if (need_uninit) {
            CoUninitialize();
        }
    }

    bool ok() const {
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
};

template <typename T>
struct ComPtr {
    T* ptr = nullptr;
    ~ComPtr() { reset(); }
    void reset() {
        if (ptr) {
            ptr->Release();
            ptr = nullptr;
        }
    }
    T** put() {
        reset();
        return &ptr;
    }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

struct CoTaskMemStr {
    PWSTR ptr = nullptr;
    ~CoTaskMemStr() {
        if (ptr) {
            CoTaskMemFree(ptr);
            ptr = nullptr;
        }
    }
};

} // namespace

std::wstring PickFolderDialog(HWND owner) {
    ComScope com;
    if (!com.ok()) return {};

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()));
    if (FAILED(hr) || !dialog) return {};

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    } else {
        dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }

    dialog->SetTitle(L"Import Comic / Manga Folder");

    hr = dialog->Show(owner);
    if (FAILED(hr)) return {};

    ComPtr<IShellItem> item;
    hr = dialog->GetResult(item.put());
    if (FAILED(hr) || !item) return {};

    CoTaskMemStr path;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path.ptr);
    if (FAILED(hr) || !path.ptr) return {};

    return std::wstring(path.ptr);
}

std::vector<std::wstring> PickComicFilesDialog(HWND owner) {
    std::vector<std::wstring> results;

    ComScope com;
    if (!com.ok()) return results;

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()));
    if (FAILED(hr) || !dialog) return results;

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    } else {
        dialog->SetOptions(FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    }

    static const COMDLG_FILTERSPEC kFilters[] = {
        {
            L"Comic & Manga Files",
            L"*.cbz;*.zip;*.cbr;*.rar;*.cb7;*.7z;*.cbt;*.pdf;*.epub;*.jpg;*.jpeg;*.png;*.webp"
        },
        { L"All Files (*.*)", L"*.*" }
    };
    dialog->SetFileTypes(static_cast<UINT>(sizeof(kFilters) / sizeof(kFilters[0])), kFilters);
    dialog->SetFileTypeIndex(1);
    dialog->SetTitle(L"Open Comic / Manga Files");

    hr = dialog->Show(owner);
    if (FAILED(hr)) return results;

    ComPtr<IShellItemArray> items;
    hr = dialog->GetResults(items.put());
    if (FAILED(hr) || !items) return results;

    DWORD count = 0;
    hr = items->GetCount(&count);
    if (FAILED(hr) || count == 0) return results;

    results.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(items->GetItemAt(i, item.put())) && item) {
            CoTaskMemStr path;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path.ptr)) && path.ptr) {
                results.emplace_back(path.ptr);
            }
        }
    }

    return results;
}

} // namespace koma
