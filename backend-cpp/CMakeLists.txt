cmake_minimum_required(VERSION 3.20)
project(IncunaReader VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Incuna Reader targets Windows x64 MSVC.")
endif()

include(FetchContent)

# Keep downloaded dependencies inside build/_deps (already gitignored via build/)
set(FETCHCONTENT_QUIET FALSE)

# 1. cpp-httplib (header-only HTTP server)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
    GIT_SHALLOW    TRUE
)

# 2. nlohmann/json
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# 3. webview/webview (WebView2 native host)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG        0.12.0
    GIT_SHALLOW    TRUE
)

# 4. SQLite3 amalgamation (compiled with FTS5)
FetchContent_Declare(
    sqlite3_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# 5. Prebuilt Windows x64 PDFium binaries (bblanchon/pdfium-binaries)
FetchContent_Declare(
    pdfium_prebuilt
    URL https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8165/pdfium-win-x64.tgz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# 6. Single-header stb (stb_image.h, stb_image_write.h, stb_image_resize2.h)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# 7. libwebp (static WebP decoder + encoder for fast reader delivery & .webp comics)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libwebp
    GIT_REPOSITORY https://github.com/webmproject/libwebp.git
    GIT_TAG        v1.4.0
    GIT_SHALLOW    TRUE
)

# 8. zlib (static, for ZIP64 O(1) deflate + libarchive)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE
)

# 9. xz / liblzma (static, for 7z/CB7 LZMA/LZMA2 support in libarchive)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    xz
    GIT_REPOSITORY https://github.com/tukaani-project/xz.git
    GIT_TAG        v5.6.3
    GIT_SHALLOW    TRUE
)

# Populate base dependencies first so zlib/liblzma targets exist for libarchive
FetchContent_MakeAvailable(
    httplib
    nlohmann_json
    webview
    sqlite3_amalgamation
    pdfium_prebuilt
    stb
    libwebp
    zlib
    xz
)

# 10. libarchive (static archive_static with 64-bit custom stream, ZIP/RAR5/7Z/TAR)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_UNZIP OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(ENABLE_CNG OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_NETTLE OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ICONV OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT OFF CACHE BOOL "" FORCE)
set(ENABLE_BZip2 OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZO OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB ON CACHE BOOL "" FORCE)
set(ENABLE_LZMA ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Point libarchive's find_package(ZLIB) and find_package(LibLZMA) at FetchContent targets
set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)
set(ZLIB_LIBRARIES zlibstatic CACHE STRING "" FORCE)

set(LIBLZMA_FOUND TRUE CACHE BOOL "" FORCE)
set(LIBLZMA_INCLUDE_DIR "${xz_SOURCE_DIR}/src/liblzma/api" CACHE PATH "" FORCE)
set(LIBLZMA_INCLUDE_DIRS "${xz_SOURCE_DIR}/src/liblzma/api" CACHE PATH "" FORCE)
set(LIBLZMA_LIBRARY liblzma CACHE STRING "" FORCE)
set(LIBLZMA_LIBRARIES liblzma CACHE STRING "" FORCE)
set(LIBLZMA_HAS_AUTO_DECODER TRUE CACHE BOOL "" FORCE)
set(LIBLZMA_HAS_EASY_ENCODER TRUE CACHE BOOL "" FORCE)
set(LIBLZMA_HAS_LZMA_PRESET TRUE CACHE BOOL "" FORCE)

FetchContent_Declare(
    libarchive
    URL https://github.com/libarchive/libarchive/releases/download/v3.8.11/libarchive-3.8.11.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(libarchive)

# Imported prebuilt PDFium target
add_library(pdfium SHARED IMPORTED GLOBAL)
set_target_properties(pdfium PROPERTIES
    IMPORTED_IMPLIB "${pdfium_prebuilt_SOURCE_DIR}/lib/pdfium.dll.lib"
    IMPORTED_LOCATION "${pdfium_prebuilt_SOURCE_DIR}/bin/pdfium.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${pdfium_prebuilt_SOURCE_DIR}/include"
)

# Build SQLite3 static library with FTS5 enabled
add_library(sqlite3 STATIC
    "${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c"
)
target_include_directories(sqlite3 PUBLIC
    "${sqlite3_amalgamation_SOURCE_DIR}"
)
target_compile_definitions(sqlite3 PUBLIC
    SQLITE_ENABLE_FTS5=1
    SQLITE_ENABLE_JSON1=1
    SQLITE_THREADSAFE=1
    SQLITE_USE_URI=1
    SQLITE_DQS=0
)

# Shared comic formats library (used by IncunaReader.exe and automated verification tests)
add_library(koma_formats STATIC
    src/utils/string_utils.hpp
    src/utils/natural_sort.hpp
    src/utils/natural_sort.cpp
    src/formats/comic_reader.hpp
    src/formats/metadata_extractor.hpp
    src/formats/metadata_extractor.cpp
    src/formats/archive_io.hpp
    src/formats/pdfium_runtime.hpp
    src/formats/image_utils.hpp
    src/formats/image_utils.cpp
    src/formats/pdf_reader.cpp
    src/formats/archive_reader.cpp
    src/formats/reader_session.hpp
    src/formats/reader_session.cpp
)

target_include_directories(koma_formats PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${stb_SOURCE_DIR}"
    "${zlib_SOURCE_DIR}"
    "${zlib_BINARY_DIR}"
    "${libarchive_SOURCE_DIR}/libarchive"
)

target_compile_definitions(koma_formats PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    LIBARCHIVE_STATIC
    LZMA_API_STATIC
)

if(MSVC)
    target_compile_options(koma_formats PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(koma_formats PUBLIC
    pdfium
    archive_static
    zlibstatic
    liblzma
    webp
)

# Core library (Database, 2-mode CoverManager, LibraryScanner & ReadDirectoryChangesW watcher)
add_library(koma_core STATIC
    src/portable_paths.hpp
    src/utils/win_system.hpp
    src/utils/win_system.cpp
    src/win_dialogs.hpp
    src/win_dialogs.cpp
    src/win_jumplist.hpp
    src/win_jumplist.cpp
    src/db/database.hpp
    src/db/database.cpp
    src/covers/cover_manager.hpp
    src/covers/cover_manager.cpp
    src/watcher/library_scanner.hpp
    src/watcher/library_scanner.cpp
    src/cloud/cloud_client.hpp
    src/cloud/cloud_client.cpp
)
target_include_directories(koma_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_definitions(koma_core PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)
if(MSVC)
    target_compile_options(koma_core PRIVATE /W4 /utf-8 /EHsc)
endif()
target_link_libraries(koma_core PUBLIC
    koma_formats
    sqlite3
    nlohmann_json::nlohmann_json
    shell32
    ole32
    gdi32
    user32
    winhttp
    bcrypt
    crypt32
    ws2_32
)

# Incuna Reader native WIN32 GUI executable (zero console window)
add_executable(IncunaReader WIN32
    src/main.cpp
    src/utils/json_helpers.hpp
    src/api/api_helpers.hpp
    src/api/library_routes.hpp
    src/api/library_routes.cpp
    src/api/reader_routes.hpp
    src/api/reader_routes.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/app.rc"
)

target_include_directories(IncunaReader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(IncunaReader PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(IncunaReader PRIVATE
        /W4
        /utf-8
        /EHsc
    )
    # Use wWinMain entry point explicitly for Unicode WIN32 GUI subsystem
    # Delay-load pdfium.dll so portable builds can place it in ./runtime/pdfium.dll
    target_link_options(IncunaReader PRIVATE
        /SUBSYSTEM:WINDOWS
        /ENTRY:wWinMainCRTStartup
        /DELAYLOAD:pdfium.dll
    )
endif()

target_link_libraries(IncunaReader PRIVATE
    koma_core
    zlibstatic
    liblzma
    httplib::httplib
    webview::core_static
    ws2_32
    delayimp
)

# Copy pdfium.dll next to IncunaReader.exe
add_custom_command(TARGET IncunaReader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${pdfium_prebuilt_SOURCE_DIR}/bin/pdfium.dll"
        "$<TARGET_FILE_DIR:IncunaReader>/pdfium.dll"
)

# Automated C++ verification test for CBZ, PDF, EPUB, standalone image & 5-page cache
add_executable(koma_formats_test
    tests/formats_test.cpp
)
target_link_libraries(koma_formats_test PRIVATE
    koma_formats
)
if(MSVC)
    target_compile_options(koma_formats_test PRIVATE /W4 /utf-8 /EHsc)
endif()
add_custom_command(TARGET koma_formats_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${pdfium_prebuilt_SOURCE_DIR}/bin/pdfium.dll"
        "$<TARGET_FILE_DIR:koma_formats_test>/pdfium.dll"
)

# Automated C++ verification test for library scanner, FTS5, bookmarks, watcher & 2-mode covers
add_executable(koma_library_test
    tests/library_test.cpp
)
target_link_libraries(koma_library_test PRIVATE
    koma_core
)
if(MSVC)
    target_compile_options(koma_library_test PRIVATE /W4 /utf-8 /EHsc)
endif()
add_custom_command(TARGET koma_library_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${pdfium_prebuilt_SOURCE_DIR}/bin/pdfium.dll"
        "$<TARGET_FILE_DIR:koma_library_test>/pdfium.dll"
)
