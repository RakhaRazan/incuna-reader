#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace koma {

/**
 * Opens the native Windows Explorer folder browser using Win32 COM
 * IFileOpenDialog (FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM).
 * Returns the selected filesystem path, or an empty string if cancelled.
 */
std::wstring PickFolderDialog(HWND owner);

/**
 * Opens the native Windows Explorer multi-file picker using Win32 COM
 * IFileOpenDialog (FOS_ALLOWMULTISELECT) filtered to comic, archive,
 * PDF, EPUB, and image extensions.
 * Returns the selected filesystem paths, or an empty vector if cancelled.
 */
std::vector<std::wstring> PickComicFilesDialog(HWND owner);

} // namespace koma
