#include "utils/win_system.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <unordered_set>

#include "db/database.hpp"
#include "utils/string_utils.hpp"

namespace koma {

namespace {

std::wstring get_env_wide(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring buf(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, buf.data(), needed);
    if (written == 0) return {};
    if (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    buf.resize(written);
    return buf;
}

std::wstring read_hkcu_reg_string(const wchar_t* subkey, const wchar_t* value_name) {
    DWORD size_bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, subkey, value_name, RRF_RT_REG_SZ, nullptr, nullptr, &size_bytes) != ERROR_SUCCESS ||
        size_bytes <= sizeof(wchar_t)) {
        return {};
    }
    std::wstring buf(size_bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, subkey, value_name, RRF_RT_REG_SZ, nullptr, buf.data(), &size_bytes) != ERROR_SUCCESS) {
        return {};
    }
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

LONG set_hkcu_sz(const std::wstring& subkey, const wchar_t* value_name, const std::wstring& data) {
    HKEY hkey = nullptr;
    LONG rc = RegCreateKeyExW(
        HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hkey, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    const DWORD bytes = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(
        hkey, value_name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(data.c_str()), bytes);
    RegCloseKey(hkey);
    return rc;
}

LONG set_hkcu_none(const std::wstring& subkey, const wchar_t* value_name) {
    HKEY hkey = nullptr;
    LONG rc = RegCreateKeyExW(
        HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hkey, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    rc = RegSetValueExW(hkey, value_name, 0, REG_NONE, nullptr, 0);
    RegCloseKey(hkey);
    return rc;
}

struct CloudSourceCandidate {
    std::string provider;
    std::string name;
    std::wstring path;
};

} // namespace

bool WinFullscreenController::set_fullscreen(HWND hwnd, bool enable) {
    if (!hwnd) return false;
    if (enable && !active_) {
        saved_style_ = GetWindowLongW(hwnd, GWL_STYLE);
        saved_ex_style_ = GetWindowLongW(hwnd, GWL_EXSTYLE);
        saved_placement_.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hwnd, &saved_placement_);

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongW(hwnd, GWL_STYLE, saved_style_ & ~WS_OVERLAPPEDWINDOW);
            SetWindowLongW(
                hwnd, GWL_EXSTYLE,
                saved_ex_style_ & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
            SetWindowPos(
                hwnd,
                HWND_TOP,
                mi.rcMonitor.left,
                mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            active_ = true;
        }
    } else if (!enable && active_) {
        SetWindowLongW(hwnd, GWL_STYLE, saved_style_);
        SetWindowLongW(hwnd, GWL_EXSTYLE, saved_ex_style_);
        SetWindowPlacement(hwnd, &saved_placement_);
        SetWindowPos(
            hwnd,
            nullptr,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        active_ = false;
    }
    return active_;
}

bool set_thread_keep_awake(bool active) {
    const EXECUTION_STATE flags = active
        ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
        : ES_CONTINUOUS;
    return SetThreadExecutionState(flags) != 0;
}

bool register_file_type_associations(const std::wstring& exe_path,
                                     bool include_pdf,
                                     std::vector<std::string>& out_registered_exts,
                                     std::string& out_error) {
    out_registered_exts.clear();
    out_error.clear();

    const std::wstring prog_id = L"IncunaReader.Comic";
    const std::wstring prog_desc = L"Incuna Reader Comic Archive";
    const std::wstring icon_val = L"\"" + exe_path + L"\",0";
    const std::wstring cmd_val = L"\"" + exe_path + L"\" \"%1\"";

    LONG rc = set_hkcu_sz(L"Software\\Classes\\" + prog_id, nullptr, prog_desc);
    if (rc == ERROR_SUCCESS) {
        rc = set_hkcu_sz(L"Software\\Classes\\" + prog_id + L"\\DefaultIcon", nullptr, icon_val);
    }
    if (rc == ERROR_SUCCESS) {
        rc = set_hkcu_sz(L"Software\\Classes\\" + prog_id + L"\\shell\\open\\command", nullptr, cmd_val);
    }
    if (rc != ERROR_SUCCESS) {
        out_error = "Failed to write HKCU ProgID registry keys";
        return false;
    }

    std::vector<std::wstring> exts = {L".cbz", L".cbr", L".cb7", L".cbt", L".epub"};
    if (include_pdf) {
        exts.push_back(L".pdf");
    }

    for (const auto& ext : exts) {
        const std::wstring ext_key = L"Software\\Classes\\" + ext;
        if (set_hkcu_sz(ext_key, nullptr, prog_id) != ERROR_SUCCESS) {
            out_error = "Failed to register extension in HKCU";
            return false;
        }
        set_hkcu_none(ext_key + L"\\OpenWithProgids", prog_id.c_str());
        out_registered_exts.push_back(wide_to_utf8(ext));
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

nlohmann::json discover_local_cloud_sources(Database& db) {
    std::vector<CloudSourceCandidate> candidates;

    const std::wstring onedrive = get_env_wide(L"OneDrive");
    if (!onedrive.empty()) candidates.push_back({"onedrive", "OneDrive", onedrive});

    const std::wstring onedrive_biz = get_env_wide(L"OneDriveCommercial");
    if (!onedrive_biz.empty()) candidates.push_back({"onedrive_commercial", "OneDrive (Work/School)", onedrive_biz});

    const std::wstring onedrive_pers = get_env_wide(L"OneDriveConsumer");
    if (!onedrive_pers.empty()) candidates.push_back({"onedrive", "OneDrive (Personal)", onedrive_pers});

    const std::wstring reg_od_pers = read_hkcu_reg_string(
        L"Software\\Microsoft\\OneDrive\\Accounts\\Personal", L"UserFolder");
    if (!reg_od_pers.empty()) candidates.push_back({"onedrive", "OneDrive (Personal)", reg_od_pers});

    const std::wstring reg_od_biz = read_hkcu_reg_string(
        L"Software\\Microsoft\\OneDrive\\Accounts\\Business1", L"UserFolder");
    if (!reg_od_biz.empty()) candidates.push_back({"onedrive_commercial", "OneDrive (Work/School)", reg_od_biz});

    const std::wstring userprofile = get_env_wide(L"USERPROFILE");
    if (!userprofile.empty()) {
        const std::filesystem::path up(userprofile);
        candidates.push_back({"onedrive", "OneDrive", (up / L"OneDrive").wstring()});
        candidates.push_back({"google_drive", "Google Drive", (up / L"Google Drive").wstring()});
        candidates.push_back({"google_drive", "Google Drive", (up / L"My Drive").wstring()});
        candidates.push_back({"dropbox", "Dropbox", (up / L"Dropbox").wstring()});
        candidates.push_back({"dropbox", "Dropbox (Personal)", (up / L"Dropbox (Personal)").wstring()});
        candidates.push_back({"dropbox", "Dropbox (Business)", (up / L"Dropbox (Business)").wstring()});
    }

    candidates.push_back({"google_drive", "Google Drive (G:)", L"G:\\My Drive"});

    std::unordered_set<std::wstring> watched_lower;
    for (const auto& wf : db.get_watched_folders()) {
        std::error_code ec;
        std::filesystem::path p(utf8_to_wide(wf.path));
        auto canon = std::filesystem::weakly_canonical(p, ec);
        watched_lower.insert(to_lower_wide((ec ? p : canon).wstring()));
    }

    std::unordered_set<std::wstring> seen_paths;
    nlohmann::json arr = nlohmann::json::array();
    std::error_code ec;
    for (const auto& c : candidates) {
        if (c.path.empty()) continue;
        std::filesystem::path p(c.path);
        if (!std::filesystem::is_directory(p, ec) || ec) {
            ec.clear();
            continue;
        }
        auto canon = std::filesystem::weakly_canonical(p, ec);
        const std::filesystem::path& final_p = ec ? p : canon;
        const std::wstring key = to_lower_wide(final_p.wstring());
        if (!seen_paths.insert(key).second) continue;

        arr.push_back({
            {"provider", c.provider},
            {"name", c.name},
            {"path", wide_to_utf8(final_p.wstring())},
            {"already_added", watched_lower.count(key) > 0}
        });
    }
    return arr;
}

} // namespace koma
