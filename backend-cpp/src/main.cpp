#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <webview/webview.h>

#include "api/library_routes.hpp"
#include "api/reader_routes.hpp"
#include "cloud/cloud_client.hpp"
#include "covers/cover_manager.hpp"
#include "db/database.hpp"
#include "formats/reader_session.hpp"
#include "portable_paths.hpp"
#include "watcher/library_scanner.hpp"
#include "win_dialogs.hpp"
#include "win_jumplist.hpp"

#ifndef IDI_ICON1
#define IDI_ICON1 101
#endif

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR lpCmdLine, int /*nCmdShow*/) {
    koma::WinJumpList::initialize_process_app_id();

    koma::PortablePaths paths;
    paths.ensure_directories();
    SetDllDirectoryW(paths.runtime_dir().c_str());
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", paths.webview2_udf_dir().c_str());

    std::mutex startup_mutex;
    koma::api::StartupLaunch startup = koma::api::parse_startup_args(lpCmdLine);

    koma::Database db;
    const bool db_ok = db.open(paths.library_db());

    auto refresh_jumplist = [&]() {
        if (!db_ok) return;
        const auto recent = db.get_recent_comics(8);
        std::vector<koma::JumpListEntry> entries;
        entries.reserve(recent.size());
        for (const auto& c : recent) {
            entries.push_back({c.id, c.title, c.path, c.current_page > 0 ? c.current_page : 1, c.page_count});
        }
        koma::WinJumpList::update_continue_reading(paths.exe_path(), entries);
    };
    refresh_jumplist();

    koma::SettingsManager settings(paths.settings_json());
    settings.load_or_create();

    koma::ReaderSession reader_session(paths.stream_cache_dir());
    koma::CoverManager cover_manager(db, settings, paths.covers_pack(), paths.stream_cache_dir());
    cover_manager.start();

    koma::CloudClient cloud_client(db, cover_manager, settings, paths.cloud_tokens_json(), paths.offline_cache_dir());

    koma::LibraryScanner scanner(db, paths.exe_dir(), &cover_manager, &settings);
    scanner.set_on_scan_complete([&]() {
        auto cfg = settings.get();
        if (cfg.value("cover_mode", "persistent_pack") == "persistent_pack") {
            cover_manager.start_pack_build();
        }
    });
    if (db_ok) {
        scanner.auto_remap_drives();
        scanner.start_watching_all();
    }

    httplib::Server server;
    std::atomic<webview::webview*> active_webview{nullptr};
    std::atomic<HWND> main_hwnd{nullptr};
    std::atomic<bool> ui_running{false};

    auto run_on_ui = [&](auto&& fn) {
        using Ret = decltype(fn(static_cast<HWND>(nullptr)));
        webview::webview* wv = active_webview.load();
        HWND owner = main_hwnd.load();
        if (wv && ui_running.load()) {
            auto prom = std::make_shared<std::promise<Ret>>();
            auto fut = prom->get_future();
            wv->dispatch([prom, owner, fn]() mutable { prom->set_value(fn(owner)); });
            return fut.get();
        }
        return fn(owner);
    };

    const auto dist_dir = paths.frontend_dist_dir();
    std::error_code ec;
    if (std::filesystem::is_directory(dist_dir, ec)) {
        server.set_mount_point("/", koma::wide_to_utf8(dist_dir.wstring()));
    }

    struct FullscreenState {
        bool active = false;
        LONG saved_style = 0;
        LONG saved_ex_style = 0;
        WINDOWPLACEMENT saved_placement{sizeof(WINDOWPLACEMENT)};
    };
    auto fs_state = std::make_shared<FullscreenState>();

    koma::api::register_library_routes(server, {db, scanner, cloud_client, paths, refresh_jumplist});
    koma::api::register_reader_routes(server, {
        db, db_ok, settings, reader_session, cover_manager, scanner, paths,
        startup, startup_mutex, refresh_jumplist,
        [&]() { return run_on_ui([](HWND owner) { return koma::PickFolderDialog(owner); }); },
        [&]() { return run_on_ui([](HWND owner) { return koma::PickComicFilesDialog(owner); }); },
        [&](bool active) {
            return run_on_ui([active](HWND) {
                const EXECUTION_STATE flags = active
                    ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
                    : ES_CONTINUOUS;
                return SetThreadExecutionState(flags) != 0;
            });
        },
        [&, fs_state](bool enable) {
            return run_on_ui([enable, fs_state](HWND hwnd) -> bool {
                if (!hwnd) return false;
                if (enable && !fs_state->active) {
                    fs_state->saved_style = GetWindowLongW(hwnd, GWL_STYLE);
                    fs_state->saved_ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
                    fs_state->saved_placement.length = sizeof(WINDOWPLACEMENT);
                    GetWindowPlacement(hwnd, &fs_state->saved_placement);

                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                        SetWindowLongW(hwnd, GWL_STYLE, fs_state->saved_style & ~WS_OVERLAPPEDWINDOW);
                        SetWindowLongW(
                            hwnd, GWL_EXSTYLE,
                            fs_state->saved_ex_style & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
                        SetWindowPos(
                            hwnd,
                            HWND_TOP,
                            mi.rcMonitor.left,
                            mi.rcMonitor.top,
                            mi.rcMonitor.right - mi.rcMonitor.left,
                            mi.rcMonitor.bottom - mi.rcMonitor.top,
                            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                        fs_state->active = true;
                    }
                } else if (!enable && fs_state->active) {
                    SetWindowLongW(hwnd, GWL_STYLE, fs_state->saved_style);
                    SetWindowLongW(hwnd, GWL_EXSTYLE, fs_state->saved_ex_style);
                    SetWindowPlacement(hwnd, &fs_state->saved_placement);
                    SetWindowPos(
                        hwnd,
                        nullptr,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                    fs_state->active = false;
                }
                return fs_state->active;
            });
        }
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        MessageBoxW(nullptr, L"Failed to bind local HTTP server on 127.0.0.1.", L"Incuna Reader", MB_ICONERROR);
        return 1;
    }
    std::thread server_thread([&server]() { server.listen_after_bind(); });

    int exit_code = 0;
    try {
        webview::webview w(/*debug=*/false, nullptr);
        w.set_title("Incuna Reader");
        w.set_size(1366, 860, WEBVIEW_HINT_NONE);

        HWND hwnd = w.window().ok() ? static_cast<HWND>(w.window().value()) : nullptr;
        if (hwnd) {
            auto load_icon = [&](int metric) {
                return static_cast<HICON>(LoadImageW(
                    hInstance, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON,
                    GetSystemMetrics(metric), GetSystemMetrics(metric), LR_DEFAULTCOLOR));
            };
            if (HICON big_icon = load_icon(SM_CXICON))     SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
            if (HICON small_icon = load_icon(SM_CXSMICON)) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
        }

        main_hwnd.store(hwnd);
        active_webview.store(&w);
        ui_running.store(true);
        w.navigate("http://127.0.0.1:" + std::to_string(port) + "/");
        w.run();
    } catch (const std::exception& ex) {
        std::wstring msg = koma::utf8_to_wide(std::string("WebView2 error: ") + ex.what());
        MessageBoxW(nullptr, msg.c_str(), L"Incuna Reader", MB_ICONERROR);
        exit_code = 1;
    }

    ui_running.store(false);
    active_webview.store(nullptr);
    main_hwnd.store(nullptr);

    // Always restore default OS display/sleep power state on shutdown
    SetThreadExecutionState(ES_CONTINUOUS);

    server.stop();
    if (server_thread.joinable()) server_thread.join();
    cloud_client.stop();
    scanner.stop_all_watchers();
    cover_manager.stop();
    reader_session.close();
    db.close();
    return exit_code;
}
