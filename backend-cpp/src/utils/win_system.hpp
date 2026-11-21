#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace koma {

class Database;

// Manages Win32 true borderless fullscreen toggling on the main WebView2 HWND.
class WinFullscreenController {
public:
    bool set_fullscreen(HWND hwnd, bool enable);
    bool is_active() const { return active_; }

private:
    bool active_ = false;
    LONG saved_style_ = 0;
    LONG saved_ex_style_ = 0;
    WINDOWPLACEMENT saved_placement_{sizeof(WINDOWPLACEMENT)};
};

// Enables/disables OS display & system idle sleep prevention on the calling thread.
bool set_thread_keep_awake(bool active);

// Registers per-user HKCU file associations (zero UAC required) for comic formats.
bool register_file_type_associations(const std::wstring& exe_path,
                                     bool include_pdf,
                                     std::vector<std::string>& out_registered_exts,
                                     std::string& out_error);

// Discovers local synced cloud folders (OneDrive, Google Drive, Dropbox) via env & HKCU registry.
nlohmann::json discover_local_cloud_sources(Database& db);

} // namespace koma
