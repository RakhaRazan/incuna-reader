#include "win_jumplist.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <objbase.h>
#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "portable_paths.hpp"

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

bool create_shell_link(const std::filesystem::path& exe_path,
                       const JumpListEntry& entry,
                       IShellLinkW** out_link) {
    if (!out_link) return false;
    *out_link = nullptr;

    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(
        CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()));
    if (FAILED(hr) || !link) return false;

    const std::wstring exe_w = exe_path.wstring();
    const std::wstring work_dir = exe_path.parent_path().wstring();
    const std::wstring comic_w = utf8_to_wide(entry.path);
    const int page = std::max(1, entry.current_page);
    const std::wstring args = L"\"" + comic_w + L"\" --page " + std::to_wstring(page);
    const std::wstring title = WinJumpList::format_display_title(entry);

    link->SetPath(exe_w.c_str());
    link->SetWorkingDirectory(work_dir.c_str());
    link->SetArguments(args.c_str());
    link->SetIconLocation(exe_w.c_str(), 0);
    link->SetDescription(title.c_str());

    ComPtr<IPropertyStore> prop_store;
    hr = link->QueryInterface(IID_PPV_ARGS(prop_store.put()));
    if (SUCCEEDED(hr) && prop_store) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        const size_t bytes = (title.size() + 1) * sizeof(wchar_t);
        pv.vt = VT_LPWSTR;
        pv.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (pv.pwszVal) {
            std::memcpy(pv.pwszVal, title.c_str(), bytes);
            prop_store->SetValue(PKEY_Title, pv);
            PropVariantClear(&pv);
            prop_store->Commit();
        }
    }

    *out_link = link.ptr;
    link.ptr = nullptr; // transfer ownership
    return true;
}

std::string format_hr(HRESULT hr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

} // namespace

void WinJumpList::initialize_process_app_id() {
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
}

std::wstring WinJumpList::format_display_title(const JumpListEntry& entry) {
    std::wstring title = utf8_to_wide(entry.title);
    if (title.empty()) {
        std::filesystem::path p(utf8_to_wide(entry.path));
        title = p.stem().wstring();
    }
    const int page = std::max(1, entry.current_page);
    // Use em-dash (U+2014): "One Piece Vol. 105 — Page 42 / 198"
    std::wstring label = title + L" \u2014 Page " + std::to_wstring(page);
    if (entry.page_count > 0) {
        label += L" / " + std::to_wstring(entry.page_count);
    }
    return label;
}

bool WinJumpList::update_continue_reading(const std::filesystem::path& exe_path,
                                          const std::vector<JumpListEntry>& entries,
                                          std::string* error_out) {
    static std::mutex jumplist_mutex;
    std::lock_guard<std::mutex> lock(jumplist_mutex);

    ComScope com;
    if (!com.ok()) {
        if (error_out) *error_out = "CoInitializeEx failed: " + format_hr(com.hr);
        return false;
    }

    ComPtr<ICustomDestinationList> dest_list;
    HRESULT hr = CoCreateInstance(
        CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dest_list.put()));
    if (FAILED(hr) || !dest_list) {
        if (error_out) *error_out = "CoCreateInstance(CLSID_DestinationList) failed: " + format_hr(hr);
        return false;
    }

    dest_list->SetAppID(kAppUserModelId);

    UINT min_slots = 0;
    ComPtr<IObjectArray> removed_items;
    hr = dest_list->BeginList(&min_slots, IID_PPV_ARGS(removed_items.put()));
    if (FAILED(hr)) {
        if (error_out) *error_out = "BeginList failed: " + format_hr(hr);
        return false;
    }

    if (!entries.empty()) {
        ComPtr<IObjectCollection> collection;
        hr = CoCreateInstance(
            CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(collection.put()));
        if (FAILED(hr) || !collection) {
            dest_list->AbortList();
            if (error_out) *error_out = "CoCreateInstance(CLSID_EnumerableObjectCollection) failed: " + format_hr(hr);
            return false;
        }

        const size_t max_items = std::min<size_t>(entries.size(), min_slots > 0 ? static_cast<size_t>(min_slots) : 8);
        size_t added = 0;
        for (size_t i = 0; i < entries.size() && added < max_items; ++i) {
            if (entries[i].path.empty()) continue;
            ComPtr<IShellLinkW> link;
            if (create_shell_link(exe_path, entries[i], link.put()) && link) {
                if (SUCCEEDED(collection->AddObject(link.ptr))) {
                    ++added;
                }
            }
        }

        if (added > 0) {
            ComPtr<IObjectArray> obj_array;
            hr = collection->QueryInterface(IID_PPV_ARGS(obj_array.put()));
            if (SUCCEEDED(hr) && obj_array) {
                hr = dest_list->AppendCategory(L"Continue Reading", obj_array.ptr);
                if (FAILED(hr)) {
                    dest_list->AbortList();
                    if (error_out) *error_out = "AppendCategory failed: " + format_hr(hr);
                    return false;
                }
            }
        }
    }

    hr = dest_list->CommitList();
    if (FAILED(hr)) {
        if (error_out) *error_out = "CommitList failed: " + format_hr(hr);
        return false;
    }

    return true;
}

} // namespace koma
