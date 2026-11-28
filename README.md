# Incuna Reader

**Incuna Reader** is a fast, portable Windows desktop comic, manga, and PDF reader built with a native **C++17** backend, embedded **WebView2** host, **SQLite3 (WAL + FTS5)**, **PDFium** / **libarchive**, and a **React + Tailwind CSS** frontend.

---

## Key Features

- **Multi-Format Engine:** Reads `.cbz`, `.zip`, `.cbr`, `.rar` (RAR5), `.cb7`, `.7z`, `.cbt`, `.tar`, `.epub`, `.pdf`, standalone images, and image folders with 64-bit Win32 streaming and a 5-page sliding RAM lookahead cache.
- **Virtualized 60-120 FPS Library:** Smoothly handles 30,000+ comics with FTS5 full-text search, SmartViews, Custom Shelves, Folder/Series browsing with root-relative breadcrumbs, Continue Reading history, Random Unread Comic, and Multi-Select batch actions.
- **Metadata Extractor & Filters:** Fast `ComicInfo.xml`, PDF metadata (`FPDF_GetMetaText`), and `[Author] Title (Tag)` filename parsing with click-to-filter author and tag pills.
- **2-Mode Cover Thumbnail Engine:** Zero-thrash SATA HDD-friendly cover delivery (`covers.pack` or 0 MB `ram_only` mode) with viewport priority queuing and scroll cancellation.
- **Pro Reader Tools:**
  - **Real-Time WebGL2 Manga Enhancer:** Unsharp mask ink sharpening and black-level/white-paper normalization.
  - **Loupe Magnifier:** Hold `Right-Click` or `L` (scroll wheel `2.5x`-`3.5x`) for an instant hardware-accelerated magnifying glass.
  - **Auto-Scroll:** Hands-free vertical Webtoon scrolling with adjustable speed (`1x`-`5x`).
  - **Layout & Display:** Single, Dual (with smart wide-spread detection), and Vertical Webtoon modes, LTR/RTL direction, auto-border cropping, Night Mode, and Win32 true borderless fullscreen + Keep Screen Awake.
- **Cloud & Offline Support:** Direct Google Drive & OneDrive connector, local cloud-sync folder discovery, and per-comic/batch offline pinning.
- **Windows Integration:** Zero-UAC per-user HKCU file associations and Windows Taskbar Jump List ("Continue Reading").
- **Localization:** English (`en`), Indonesian (`id`), and Japanese (`ja`).

---

## Architecture

Incuna Reader runs as a single portable Windows executable (`IncunaReader.exe`) with zero console window and zero external installer requirements:

- **Native Host & System Utilities (`backend-cpp/src/main.cpp`, `utils/`):**
  - **Win32 + WebView2 (`webview`):** Native window host with custom application icon, Windows Jump List (`win_jumplist.cpp`), native Explorer dialogs (`win_dialogs.cpp`), borderless fullscreen & keep-awake (`utils/win_system.cpp`).
  - **Embedded HTTP Server (`cpp-httplib`):** Binds to `127.0.0.1` on an ephemeral port, serving `runtime/dist/` and REST endpoints (`api/library_routes.cpp`, `api/reader_routes.cpp`).
- **Database (`backend-cpp/src/db/`):**
  - **SQLite3 + FTS5 (WAL mode):** Stores comics, folders, reading progress, bookmarks, smart views, shelves, and `covers.pack` byte offsets in `data/library.db`.
- **Format, Metadata & Streaming Engine (`backend-cpp/src/formats/`):**
  - **Archives (`archive_reader.cpp`):** ZIP64 O(1) Central Directory fast path + `libarchive` / `zlib` / `liblzma` streaming.
  - **PDF (`pdf_reader.cpp`):** Delay-loaded `runtime/pdfium.dll` guarded by a global PDFium mutex (`pdfium_runtime.hpp`).
  - **Metadata (`metadata_extractor.cpp`):** Extracts `ComicInfo.xml`, PDF info tags, and filename metadata.
  - **Reader Session (`reader_session.cpp`):** 5-page sliding RAM cache with `libwebp` / `stb` image transcoding (`image_utils.cpp`).
- **Scanner, Watcher & Cloud (`backend-cpp/src/watcher/`, `cloud/`):**
  - Recursive folder scanner with `ReadDirectoryChangesW` live watching, drive-letter remapping, and OAuth2 Google Drive / OneDrive sync.
- **Frontend UI (`frontend/src/`):**
  - React 18 + Vite + Tailwind CSS + `i18next`, with centralized API client (`api/client.js`), custom hooks (`useLibraryState.js`, `useKeyboardShortcuts.js`, `useMangaEnhancer.js`, `useLoupeMagnifier.js`), and modular components.

---

## Portable Directory Layout

```text
IncunaReader/
├── IncunaReader.exe        # Native Win32 + WebView2 executable
├── runtime/
│   ├── dist/               # Built frontend static assets
│   ├── pdfium.dll          # Delay-loaded PDFium engine
│   └── webview2_udf/       # Portable WebView2 user data folder
└── data/
    ├── library.db          # SQLite3 + FTS5 library database
    ├── covers.pack         # Concatenated cover thumbnails (Mode 1)
    ├── settings.json       # User preferences & custom shortcuts
    ├── cloud_tokens.json   # Saved OAuth2 cloud tokens
    ├── offline_cache/      # Pinned offline comic cache
    └── stream_cache/       # Temporary page streaming cache
```

---

## 2-Mode Cover System

Incuna Reader includes a configurable 2-mode cover thumbnail engine (`backend-cpp/src/covers/cover_manager.cpp`) designed to avoid SATA HDD thrashing while keeping library scrolling instant:

1. **Mode 1 - Persistent Cover Pack (`persistent_pack`, default):**
   - Stores resized cover thumbnails (`200x280` max) in a single concatenated `data/covers.pack` file indexed by `(cover_pack_offset, cover_pack_size)` in SQLite for **O(1)** disk reads.
   - Builds missing covers in the background using a sequential disk reader + parallel CPU decode workers, while visible viewport cards cut to the front of the priority queue.
2. **Mode 2 - RAM-Only / Zero Disk (`ram_only`):**
   - Uses **0 MB** of persistent cover disk storage (`covers.pack` can be purged cleanly from Settings).
   - Serves covers from a **64 MB bounded RAM LRU cache**, falling back to Windows Shell thumbnail cache (`IShellItemImageFactory` with `SIIGBF_INCACHEONLY`) and a **2-thread** extraction pool with automatic scroll cancellation for off-screen items.

---

## Keyboard Shortcuts

All shortcuts can be customized in **Settings → Keyboard shortcuts**.

### Library View
| Shortcut | Action |
| :--- | :--- |
| `/` or `Ctrl+F` | Focus library search bar |
| `Esc` | Close open modal, context menu, or clear selection |

### Reader View
| Shortcut | Action |
| :--- | :--- |
| `ArrowRight` / `ArrowLeft` | Next / Previous page (automatically flips in RTL manga mode) |
| `PageDown` / `PageUp` | Next / Previous page |
| `Space` / `Shift+Space` | Next / Previous page (when not in vertical scroll mode) |
| `Home` / `End` | First / Last page |
| `Shift+Arrow` / `Ctrl+Arrow` | Next / Previous volume |
| `Hold Right-Click` or `L` | Loupe Magnifier (scroll wheel adjusts `2.5x`–`3.5x`) |
| `F` or `F11` | Toggle borderless fullscreen |
| `M` | Toggle reading direction (`LTR` / `RTL`) |
| `D` | Cycle layout mode (`single` / `dual` / `vertical`) |
| `W` | Cycle fit mode (`fitPage` / `fitWidth` / `fitHeight` / `original`) |
| `N` | Toggle Night Mode |
| `H` | Show / hide reader toolbars |
| `B` | Open / close bookmarks drawer |
| `+` (`=`) / `-` / `0` | Zoom in / Zoom out / Reset zoom |
| `Enter` | Open next volume (when end-of-volume prompt is active) |
| `Esc` | Exit fullscreen, close bookmarks drawer, or exit reader |

---

## Building from Source (Windows x64 MSVC)

### Automated Portable Master Build
```powershell
packaging\build_portable.bat
```
This builds `frontend/dist`, compiles `backend-cpp` in `Release` x64 mode via CMake + MSVC, and deploys `IncunaReader.exe` and `runtime/` to the repository root.

### Manual Steps
```powershell
# 1. Frontend
cd frontend
npm install
npm run build
cd ..

# 2. Backend (Windows x64 MSVC + CMake)
cmake -S backend-cpp -B backend-cpp/build -G "Visual Studio 17 2022" -A x64
cmake --build backend-cpp/build --config Release --target IncunaReader --parallel
```

---

## Acknowledgments & Open-Source Credits

Incuna Reader is licensed under the [MIT License](LICENSE) (Copyright 2026 RakhaRazan) and is powered by open-source software:

- **Native Engines & Libraries:** [PDFium](https://pdfium.googlesource.com/pdfium/), [libarchive](https://www.libarchive.org/) (with zlib & liblzma), [SQLite](https://www.sqlite.org/) (WAL + FTS5), [stb](https://github.com/nothings/stb), [libwebp](https://chromium.googlesource.com/webm/libwebp), [cpp-httplib](https://github.com/yhirose/cpp-httplib), [nlohmann/json](https://github.com/nlohmann/json), and [Microsoft Edge WebView2](https://developer.microsoft.com/en-us/microsoft-edge/webview2/) (via `webview/webview`).
- **Frontend:** [React](https://react.dev/), [Vite](https://vitejs.dev/), [Tailwind CSS](https://tailwindcss.com/), and [Lucide Icons](https://lucide.dev/).
- **Design & Schema Inspiration:** Inspired by **ComicInfo.xml** metadata standard.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for full license attributions.

