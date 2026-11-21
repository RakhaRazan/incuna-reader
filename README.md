# Incuna Reader

**Incuna Reader** is a fast, portable Windows desktop comic, manga, and PDF reader built with a native C++17 backend, embedded WebView2 host, and a React + Tailwind CSS frontend.

---

## Architecture

Incuna Reader runs as a single portable Windows executable (`IncunaReader.exe`) with zero console window and zero external installer requirements:

- **Native Host & HTTP API (`backend-cpp/src/main.cpp`, `api/`):**
  - **Win32 + WebView2 (`webview`):** Native window host with custom application icon, Windows Jump List integration (`win_jumplist.cpp`), and native file/folder picker dialogs (`win_dialogs.cpp`).
  - **Embedded HTTP Server (`cpp-httplib`):** Binds to `127.0.0.1` on an ephemeral port, serving the static frontend from `runtime/dist/` and exposing REST endpoints for library management, cover delivery, and page streaming.
- **Database & Full-Text Search (`backend-cpp/src/db/`):**
  - **SQLite3 + FTS5:** Stores comics, folders, reading progress, bookmarks, smart views, custom shelves, and `covers.pack` byte offsets in `data/library.db` (WAL mode).
- **Format & Streaming Engine (`backend-cpp/src/formats/`):**
  - **Archives (`archive_reader.cpp`):** Reads `.cbz`, `.zip`, `.cbr`, `.rar` (RAR5), `.cb7`, `.7z`, `.cbt`, `.tar`, and `.epub` via `libarchive`, `zlib`, and `liblzma`, plus standalone image folders.
  - **PDF (`pdf_reader.cpp`):** Renders `.pdf` pages on-demand using delay-loaded `runtime/pdfium.dll`.
  - **Reader Session (`reader_session.cpp`):** 5-page sliding window cache with `libwebp` / `stb` image transcoding (`image_utils.cpp`) for smooth page turns.
- **Library Scanner & Watcher (`backend-cpp/src/watcher/`):**
  - Recursive folder scanner with `ReadDirectoryChangesW` live filesystem watching and automatic removable-drive remapping.
- **Frontend UI (`frontend/src/`):**
  - React 18 + Vite + Tailwind CSS + `i18next` (`en`, `id`, `ja`), featuring virtualized library grid scrolling, smart views, shelves, batch selection, and a multi-mode reader (`single`, `dual`, `vertical`, LTR/RTL, zoom, night mode, bookmarks).

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
    ├── settings.json       # User preferences
    ├── offline_cache/      # Pinned offline comic cache
    └── stream_cache/       # Temporary page streaming cache
```

---

## 2-Mode Cover System

Incuna Reader includes a configurable 2-mode cover thumbnail engine (`backend-cpp/src/covers/cover_manager.cpp`) designed to avoid SATA HDD thrashing while keeping library scrolling instant:

1. **Mode 1 — Persistent Cover Pack (`persistent_pack`, default):**
   - Stores resized cover thumbnails (`200x280` max) in a single concatenated `data/covers.pack` file indexed by `(cover_pack_offset, cover_pack_size)` in SQLite for **O(1)** disk reads.
   - Automatically builds missing covers in the background using a strict **2-thread** worker pool to prevent HDD seek contention, with on-demand fallback and a 64 MB RAM LRU cache.
2. **Mode 2 — RAM-Only / Zero Disk (`ram_only`):**
   - Uses **0 MB** of persistent cover disk storage (`covers.pack` can be purged cleanly from Settings).
   - Serves covers from a **64 MB bounded RAM LRU cache**, falling back to Windows Shell thumbnail cache (`IShellItemImageFactory` with `SIIGBF_INCACHEONLY`) and a **2-thread** extraction pool with automatic scroll cancellation for off-screen items.

---

## Keyboard Shortcuts

### Library View
| Shortcut | Action |
| :--- | :--- |
| `/` or `Ctrl+F` | Focus library search bar |
| `Esc` | Close open modal or context menu |

### Reader View
| Shortcut | Action |
| :--- | :--- |
| `ArrowRight` / `ArrowLeft` | Next / Previous page (automatically flips in RTL manga mode) |
| `PageDown` / `PageUp` | Next / Previous page |
| `Space` / `Shift+Space` | Next / Previous page (when not in vertical scroll mode) |
| `F` | Toggle fullscreen |
| `M` | Toggle reading direction (`LTR` / `RTL`) |
| `D` | Cycle layout mode (`single` / `dual` / `vertical`) |
| `B` | Open / close bookmarks drawer |
| `+` (`=`) / `-` / `0` | Zoom in / Zoom out / Reset zoom |
| `Enter` | Open next volume (when prompt is active) |
| `Esc` | Close bookmarks drawer, or exit reader |

---

## Building from Source

### Frontend
```powershell
cd frontend
npm install
npm run build
```

### Backend (Windows x64 MSVC + CMake)
```powershell
cmake -S backend-cpp -B backend-cpp/build -A x64
cmake --build backend-cpp/build --config Release
```
