#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace koma {

struct JumpListEntry {
    int64_t comic_id = 0;
    std::string title;
    std::string path;
    int current_page = 1; // 1-based page number
    int page_count = 0;
};

/**
 * Manages the Windows Taskbar Jump List ("Continue Reading" category)
 * using Win32 ICustomDestinationList and IShellLinkW.
 */
class WinJumpList {
public:
    static constexpr const wchar_t* kAppUserModelId = L"IncunaReader.Portable.App";

    // Associates the current process with kAppUserModelId so the taskbar button
    // and Jump List stay bound even for a portable, uninstalled executable.
    static void initialize_process_app_id();

    // Formats the Jump List display title, e.g. "One Piece Vol. 105 — Page 42 / 198".
    static std::wstring format_display_title(const JumpListEntry& entry);

    // Rebuilds the "Continue Reading" Jump List category.
    // Safe to call from any thread.
    static bool update_continue_reading(const std::filesystem::path& exe_path,
                                        const std::vector<JumpListEntry>& entries,
                                        std::string* error_out = nullptr);
};

} // namespace koma
