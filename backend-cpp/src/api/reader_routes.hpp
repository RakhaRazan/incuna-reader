#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace httplib {
class Server;
}

namespace koma {
class CoverManager;
class Database;
class LibraryScanner;
class PortablePaths;
class ReaderSession;
class SettingsManager;
}

namespace koma::api {

struct StartupLaunch {
    std::string file_path;
    int page = 0;
};

StartupLaunch parse_startup_args(PWSTR lpCmdLine);

struct ReaderRouteContext {
    Database& db;
    bool db_ok = false;
    SettingsManager& settings;
    ReaderSession& reader_session;
    CoverManager& cover_manager;
    LibraryScanner& scanner;
    const PortablePaths& paths;
    StartupLaunch& startup;
    std::mutex& startup_mutex;
    std::function<void()> refresh_jumplist;
    std::function<std::wstring()> pick_folder_on_ui;
    std::function<std::vector<std::wstring>()> pick_files_on_ui;
    std::function<bool(bool)> set_keep_awake;
    std::function<bool(bool)> set_fullscreen;
};

void register_reader_routes(httplib::Server& server, ReaderRouteContext ctx);

} // namespace koma::api
