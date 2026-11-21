# Third-Party Notices & Open-Source Attributions

Incuna Reader is built on top of the following open-source libraries, engines, tools, and specifications. We gratefully acknowledge their authors and contributors.

## Core Native Engines & Libraries (C++ / Win32)

- **PDFium** (Google / Chromium Authors) - BSD-3-Clause License  
  Used for on-demand PDF page rendering and metadata extraction (`FPDF_GetMetaText`) via `pdfium.dll`.
- **libarchive** (Tim Kientzle & libarchive contributors) - BSD-2-Clause License  
  Used for streaming comic archives (`.cbr`/RAR5, `.cb7`/7z, `.cbt`/TAR, and archive fallbacks), paired with **zlib** (Jean-loup Gailly & Mark Adler, zlib License) and **XZ / liblzma** (Tukaani Project, 0BSD / Public Domain).
- **SQLite** (D. Richard Hipp & SQLite contributors) — Public Domain  
  Used for the local library database (`library.db`) with WAL journaling and FTS5 full-text search.
- **stb** (`stb_image`, `stb_image_write` by Sean Barrett) - MIT / Public Domain  
  Used for image decoding and JPEG encoding, alongside **libwebp** (Google, BSD-3-Clause License) for WebP decoding and encoding.
- **cpp-httplib** (Yuji Hirose) - MIT License  
  Used for the embedded `127.0.0.1` HTTP server bridging the native backend and WebView2 UI.
- **nlohmann/json** (Niels Lohmann) - MIT License  
  Used for JSON serialization and configuration parsing in C++.
- **Microsoft Edge WebView2** & **webview/webview** - Microsoft Edge WebView2 SDK License / MIT License  
  Used for the native Win32 desktop WebView2 host window.

## Frontend Libraries & Tooling

- **React** (Meta Platforms, Inc.) - MIT License
- **Vite** (Evan You & Vite contributors) - MIT License
- **Tailwind CSS** (Tailwind Labs, Inc.) - MIT License
- **Lucide Icons** (Lucide Contributors) - ISC License

## Design & Metadata Inspiration

- **ComicInfo.xml** (ComicRack / Anansi Project) - Community metadata schema used for extracting series, writer, tags, genre, and language metadata from comic archives.
