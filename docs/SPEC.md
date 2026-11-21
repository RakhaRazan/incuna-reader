# KomaReader — Technical Specification (`SPEC.md`)

**KomaReader** is a 100% portable, ultra-lightweight Windows comic, manga, and graphic novel reader & library manager inspired by *Cover*. Designed to handle libraries of 30,000+ files and individual archives or PDFs exceeding 20 GB without loading entire files into RAM, KomaReader pairs a native 64-bit C++20 Win32/WebView2 backend with a lightweight, multi-language frontend.

---

## 1. Portability & Zero-Install Architecture

KomaReader is strictly portable. Extracting the release archive to any directory (local SSD/HDD, external USB drive, or network share) and launching `KomaReader.exe` requires zero installation, zero `%LOCALAPPDATA%` writes, and zero mandatory Windows Registry modifications.

### 1.1 Executable-Relative Path Resolution
- At startup, the backend resolves the canonical executable directory using Win32 `GetModuleFileNameW(nullptr, ...)` and strips the binary filename.
- All runtime, configuration, database, and cache paths are resolved relative to this base directory:
  - `./runtime/` — Embedded WebView2 Evergreen/Fixed-Version runtime binaries (if bundled) and temporary extraction/IPC staging.
  - `./data/library.db` — Primary SQLite3 library database (`WAL` mode + `FTS5`).
  - `./data/settings.json` — User preferences, reader defaults, keybindings, language, and cover mode configuration.
  - `./data/offline_cache/` — Local mirror for cloud/network-pinned comics ("Pin Offline").
  - `./data/covers.pack` — Optional concatenated cover blob file (created only when Cover Mode 2 is active).
- WebView2 User Data Folder (UDF) is explicitly directed to `./runtime/webview2_udf/` via `CreateCoreWebView2EnvironmentWithOptions` so no browser state leaks into `%LOCALAPPDATA%`.

---

## 2. 64-Bit Large-File Engine (1 MB to 20+ GB, Zero Full-File RAM Load)

KomaReader never reads a whole comic archive or PDF into memory. All I/O uses 64-bit file offsets (`int64_t` / `LARGE_INTEGER`) and Win32 `CreateFileW` + `SetFilePointerEx` / `ReadFile` to support files well beyond the 4 GB 32-bit boundary (20 GB+ tested target).

### 2.1 PDF Engine (`PDFium`)
- **Custom 64-Bit Stream:** Uses `FPDF_LoadCustomDocument` backed by a `FPDF_FILEACCESS` implementation wrapping Win32 `HANDLE` with `SetFilePointerEx` and `ReadFile`.
- **Performance Target:** Opening a 20 GB PDF completes in **<100 ms** using **<40 MB RAM**, as PDFium only seeks to the cross-reference table (`xref`) and renders the active page(s).
- **Rendering:** Pages are rendered on-demand at the target viewport DPI/zoom level into RGBA/BGRA buffers and encoded/streamed to the frontend.

### 2.2 Archive & Image Engine (`libarchive` + ZIP64 O(1) Lookup)
- **Supported Formats:**
  - Archives: `CBZ` / `ZIP`, `CBR` / `RAR` (including `RAR5`), `7Z` / `CB7`, `CBT` (`TAR`), `EPUB` (image extraction only, sorted by spine/reading order).
  - Standalone Images: `JPEG`, `PNG`, `GIF`, `BMP`, `WEBP` (and folders of images treated as virtual comics).
- **64-Bit `libarchive` Callbacks:** Configured with custom `archive_read_open2` callbacks using `la_int64_t` (`seek`, `skip`, `read`) backed by Win32 `SetFilePointerEx`.
- **ZIP64 O(1) Central Directory Lookup:** For `CBZ`/`ZIP`/`EPUB` files, a dedicated fast-path parser reads the End of Central Directory (EOCD / ZIP64 EOCD) from the tail of the file, builds an in-memory sorted page index (`filename -> local_header_offset, compressed_size, uncompressed_size, method`), and seeks directly to any requested page in $O(1)$ without scanning preceding entries.
- **Solid Archive Handling (`RAR5`/`7z`):** Maintains an active stream cursor for sequential reading and resets/seeks only on backward jumps or random-access page skips.

### 2.3 Sliding-Window RAM Page Cache
- **5-Page Window:** Keeps `[current - 2, current - 1, current, current + 1, current + 2]` decoded/compressed page buffers in a bounded RAM cache.
- **Background Pre-Decoding:** Adjacent pages (`+1`, `+2`, `-1`) are pre-fetched on a dedicated low-priority worker thread so page turns feel instantaneous even on high-latency SATA 3 HDDs or `\\NAS` SMB shares.

---

## 3. 2-Mode Cover System

KomaReader provides two distinct cover caching strategies configurable in Settings, balancing zero disk footprint against instant scrolling across massive libraries.

### 3.1 Mode 1: `ram_only` (Default — 0 MB Disk Footprint)
- **Zero Disk Usage:** No thumbnails or cover packs are written to `./data/`.
- **Bounded 64 MB RAM LRU Cache:** Visible covers in the virtualized library grid are stored in an in-memory LRU cache capped at 64 MB.
- **Windows Shell Cache Check:** Before opening an archive, KomaReader queries `IShellItemImageFactory` (with `SIIGBF_INCACHEONLY`) to reuse existing Windows Explorer thumbnails if already cached by the OS.
- **2-Thread Anti-Thrashing HDD Queue:**
  - Cover extraction is limited to **2 worker threads** to prevent random-seek thrashing on mechanical HDDs.
  - **Scroll Cancellation:** When the user scrolls rapidly, off-screen/stale cover requests are immediately cancelled via atomic generation tokens before any file I/O begins.

### 3.2 Mode 2: `persistent_pack` (Optional Toggle in Settings)
- **Single-File Concatenated Pack (`./data/covers.pack`):** Extracted WebP/JPEG cover thumbnails are appended sequentially to a single binary pack file instead of creating 30,000+ loose `.jpg` files on NTFS.
- **SQLite Byte-Offset Index:** The `books` (or `covers`) table stores `(pack_offset INTEGER, pack_length INTEGER)` for O(1) `SetFilePointerEx` reads, enabling **0 ms** cover loading while scrolling through 30,000+ comics.
- **Clean Revert:** Switching back to Mode 1 (`ram_only`) automatically deletes `./data/covers.pack` and clears the pack offset columns in SQLite.

---

## 4. Database & Library Architecture (`SQLite3`)

- **Engine Configuration:** `PRAGMA journal_mode = WAL;`, `PRAGMA synchronous = NORMAL;`, `PRAGMA foreign_keys = ON;`, plus `FTS5` virtual tables for instant full-text search across 30,000+ titles, series, authors, tags, and paths.
- **Core Entities:**
  - `sources` — Watched root folders (Local paths, UNC `\\NAS\...` shares, OneDrive, Google Drive, Dropbox).
  - `shelves` — User-defined collections/shelves (many-to-many or ordered grouping).
  - `books` — File path, normalized hash/metadata, format, file size (64-bit), page count, current page, read status (`unread`, `reading`, `read`), favorite flag, `is_pinned_offline`, cover pack offset/length, timestamps.
  - `bookmarks` — Named bookmarks per book (`book_id`, `page_number`, `label`, `created_at`).
  - `smartviews` — Saved dynamic queries/filters (e.g., *Unread Manga*, *Favorites*, *Recently Added*, *Pinned Offline*).
  - `books_fts` — `FTS5` index synchronized via triggers for sub-10 ms search.

---

## 5. Folder Sources, Cloud/NAS & Explorer Auto-Sync

- **Multi-Source Library:** Supports local drives, UNC network paths (`\\NAS\Comics`), and local sync folders for OneDrive, Google Drive, and Dropbox.
- **Pin Offline:** Any book residing on a `\\NAS` or cloud-synced placeholder can be pinned offline, copying it into `./data/offline_cache/` and transparently serving reads from the local cache when the remote source is unavailable.
- **Win32 `ReadDirectoryChangesW` Watcher:** Monitors active local/watched directories asynchronously using overlapped I/O (`FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE`), debouncing Explorer renames, moves, additions, and deletions to keep `library.db` in sync automatically.

---

## 6. Windows Integration & CLI

- **Standalone CLI File Opening:** Launching `KomaReader.exe "C:\Path\To\Comic.cbz"` (or via Windows "Open with") opens the file directly in the reader—even if it is not yet indexed in a library folder—and supports single-instance IPC forwarding if KomaReader is already running.
- **Windows Taskbar Jump Lists:** Uses `ICustomDestinationList` / `IObjectArray` to populate Windows Taskbar Jump Lists with *Continue Reading* / *Recent Comics* and quick-launch actions.

---

## 7. Reader Modes & UI Features

### 7.1 Reader Viewing Modes
- **Page Layouts:** Single Page, Dual Page (spread view with optional cover-page offset).
- **Reading Directions:**
  - Right-to-Left (`RTL` — Manga)
  - Left-to-Right (`LTR` — Western Comics)
  - Vertical Continuous Scroll (`Webtoon` mode)
- **Scaling & Fitting:** Fit Width, Fit Height, Fit Page (Contain), Original Size (1:1), and smooth zoom/pan.
- **Visual Enhancements:**
  - Configurable reader background color (Black, Dark Gray, Sepia, White, Custom).
  - **Auto-Crop Borders:** Automatic detection and trimming of uniform white/black scanner margins.
  - **Night Mode:** Color inversion / warm-tint / brightness-contrast filter for comfortable low-light reading.
- **Input & Ergonomics:** Full touch gesture support (swipe to turn page, pinch-to-zoom, tap zones) and customizable keyboard shortcuts + mouse wheel navigation.

### 7.2 Multi-Language UI (`i18next`)
- Frontend localization powered by `i18next` with modular JSON locale bundles (English, Indonesian, Japanese, Spanish, etc.) and live language switching without restarting.

---

## 8. Repository Structure

```text
KomaReader/
├── docs/                     # Architecture & technical specifications (SPEC.md)
├── backend-cpp/              # Native C++20 Win32 + WebView2 + SQLite3 + PDFium + libarchive backend
│   └── src/
│       ├── db/               # SQLite3 WAL/FTS5 schema, migrations, queries
│       ├── formats/          # 64-bit PDFium, libarchive, ZIP64 O(1), and image decoders
│       ├── covers/           # 2-mode cover manager (RAM LRU + Shell cache / covers.pack)
│       └── watcher/          # Win32 ReadDirectoryChangesW & offline sync manager
├── frontend/                 # WebView2 UI (virtualized library grid, reader canvas, i18next)
├── packaging/                # Portable ZIP/release bundling scripts & manifests
└── .gitignore                # Excludes build/, build_deps/, node_modules/, dist/, data/, runtime/, binaries
```
