#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace koma {

inline std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size_needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return {};
    std::string result(static_cast<size_t>(size_needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        result.data(), size_needed, nullptr, nullptr);
    return result;
}

inline std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size_needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (size_needed <= 0) return {};
    std::wstring result(static_cast<size_t>(size_needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        result.data(), size_needed);
    return result;
}

class PortablePaths {
public:
    PortablePaths() {
        exe_path_ = resolve_exe_path();
        exe_dir_ = exe_path_.parent_path();
    }

    const std::filesystem::path& exe_path() const { return exe_path_; }
    const std::filesystem::path& exe_dir() const { return exe_dir_; }

    // All runtime data resolves strictly inside <exe_dir>/data/ and <exe_dir>/runtime/
    std::filesystem::path data_dir() const { return exe_dir_ / L"data"; }
    std::filesystem::path library_db() const { return data_dir() / L"library.db"; }
    std::filesystem::path settings_json() const { return data_dir() / L"settings.json"; }
    std::filesystem::path cloud_tokens_json() const { return data_dir() / L"cloud_tokens.json"; }
    std::filesystem::path offline_cache_dir() const { return data_dir() / L"offline_cache"; }
    std::filesystem::path stream_cache_dir() const { return data_dir() / L"stream_cache"; }
    std::filesystem::path covers_pack() const { return data_dir() / L"covers.pack"; }

    std::filesystem::path runtime_dir() const { return exe_dir_ / L"runtime"; }
    std::filesystem::path webview2_udf_dir() const { return runtime_dir() / L"webview2_udf"; }

    // Resolve frontend/dist for both portable distribution (<exe_dir>/runtime/dist or <exe_dir>/frontend/dist)
    // and development builds (e.g. backend-cpp/build/Release/KomaReader.exe).
    std::filesystem::path frontend_dist_dir() const {
        const std::vector<std::filesystem::path> candidates = {
            exe_dir_ / L"runtime" / L"dist",
            exe_dir_ / L"frontend" / L"dist",
            exe_dir_.parent_path() / L"frontend" / L"dist",
            exe_dir_.parent_path().parent_path() / L"frontend" / L"dist",
            exe_dir_.parent_path().parent_path().parent_path() / L"frontend" / L"dist",
            exe_dir_.parent_path().parent_path().parent_path().parent_path() / L"frontend" / L"dist"
        };
        std::error_code ec;
        for (const auto& candidate : candidates) {
            if (std::filesystem::is_directory(candidate, ec) &&
                std::filesystem::exists(candidate / L"index.html", ec)) {
                return std::filesystem::weakly_canonical(candidate, ec);
            }
        }
        return exe_dir_ / L"frontend" / L"dist";
    }

    void ensure_directories() const {
        std::error_code ec;
        std::filesystem::create_directories(data_dir(), ec);
        std::filesystem::create_directories(offline_cache_dir(), ec);
        std::filesystem::create_directories(stream_cache_dir(), ec);
        std::filesystem::create_directories(runtime_dir(), ec);
        std::filesystem::create_directories(webview2_udf_dir(), ec);
    }

    std::string exe_dir_utf8() const { return wide_to_utf8(exe_dir_.wstring()); }
    std::string library_db_utf8() const { return wide_to_utf8(library_db().wstring()); }
    std::string settings_json_utf8() const { return wide_to_utf8(settings_json().wstring()); }
    std::string frontend_dist_utf8() const { return wide_to_utf8(frontend_dist_dir().wstring()); }

private:
    static std::filesystem::path resolve_exe_path() {
        std::wstring buffer(MAX_PATH, L'\0');
        while (true) {
            DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (len == 0) {
                throw std::runtime_error("GetModuleFileNameW failed.");
            }
            if (len < buffer.size() - 1) {
                buffer.resize(len);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
        return std::filesystem::path(buffer);
    }

    std::filesystem::path exe_path_;
    std::filesystem::path exe_dir_;
};

} // namespace koma
