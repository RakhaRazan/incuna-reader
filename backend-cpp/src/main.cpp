#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4005)
#endif
#include <webview/webview.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "api/library_routes.hpp"
#include "api/reader_routes.hpp"
#include "cloud/cloud_client.hpp"
#include "covers/cover_manager.hpp"
#include "db/database.hpp"
#include "formats/reader_session.hpp"
#include "portable_paths.hpp"
#include "utils/string_utils.hpp"
#include "utils/win_system.hpp"
#include "watcher/library_scanner.hpp"
#include "win_dialogs.hpp"
#include "win_jumplist.hpp"

#ifndef IDI_ICON1
#define IDI_ICON1 101
#endif

namespace {

std::string generate_session_token_hex() {
    std::array<uint8_t, 32> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        const uint64_t seed = static_cast<uint64_t>(GetTickCount64()) ^
                              (static_cast<uint64_t>(GetCurrentProcessId()) << 17);
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<uint8_t>((seed >> ((i % 8) * 8)) ^ (i * 131 + 17));
        }
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (uint8_t b : bytes) {
        hex.push_back(kHex[(b >> 4) & 0x0F]);
        hex.push_back(kHex[b & 0x0F]);
    }
    return hex;
}

WNDPROC g_prev_wndproc = nullptr;
std::function<void(const std::string&)> g_on_file_drop;

LRESULT CALLBACK IncunaDropWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DROPFILES) {
        HDROP hdrop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0) {
            const UINT len = DragQueryFileW(hdrop, 0, nullptr, 0);
            if (len > 0) {
                std::wstring wpath(len + 1, L'\0');
                if (DragQueryFileW(hdrop, 0, wpath.data(), len + 1) > 0) {
                    wpath.resize(len);
                    const std::string utf8_path = koma::wide_to_utf8(wpath);
                    if (!utf8_path.empty() && g_on_file_drop) {
                        g_on_file_drop(utf8_path);
                    }
                }
            }
        }
        DragFinish(hdrop);
        return 0;
    }
    if (g_prev_wndproc) {
        return CallWindowProcW(g_prev_wndproc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

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

    const std::string session_token = generate_session_token_hex();

    httplib::Server server;
    const unsigned int pool_threads = (std::max)(12u, std::thread::hardware_concurrency() * 2u);
    server.new_task_queue = [pool_threads]() { return new httplib::ThreadPool(pool_threads); };

    server.set_pre_routing_handler([session_token](const httplib::Request& req, httplib::Response& res) {
        if (req.method == "POST" || req.method == "PUT" || req.method == "PATCH" || req.method == "DELETE") {
            std::string provided;
            if (req.has_header("X-Incuna-Token")) {
                provided = req.get_header_value("X-Incuna-Token");
            } else if (req.has_param("token")) {
                provided = req.get_param_value("token");
            }
            if (provided != session_token) {
                res.status = 403;
                res.set_content(
                    R"({"error":"Forbidden: missing or invalid session token"})",
                    "application/json; charset=utf-8");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
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

    auto fullscreen = std::make_shared<koma::WinFullscreenController>();

    koma::api::register_library_routes(server, {db, scanner, cloud_client, cover_manager, reader_session, paths, refresh_jumplist});
    koma::api::register_reader_routes(server, {
        db, db_ok, settings, reader_session, cover_manager, scanner, paths,
        startup, startup_mutex, refresh_jumplist,
        [&]() { return run_on_ui([](HWND owner) { return koma::PickFolderDialog(owner); }); },
        [&]() { return run_on_ui([](HWND owner) { return koma::PickComicFilesDialog(owner); }); },
        [&](bool active) {
            return run_on_ui([active](HWND) { return koma::set_thread_keep_awake(active); });
        },
        [&, fullscreen](bool enable) {
            return run_on_ui([enable, fullscreen](HWND hwnd) {
                return fullscreen->set_fullscreen(hwnd, enable);
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

            g_on_file_drop = [&w](const std::string& utf8_path) {
                const std::string detail_json = nlohmann::json{{"path", utf8_path}}.dump();
                const std::string script =
                    "window.dispatchEvent(new CustomEvent('incuna-file-drop', { detail: " + detail_json + " }));";
                w.dispatch([&w, script]() { w.eval(script); });
            };
            DragAcceptFiles(hwnd, TRUE);
            g_prev_wndproc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(IncunaDropWndProc)));
        }

        main_hwnd.store(hwnd);
        active_webview.store(&w);
        ui_running.store(true);
        w.init("window.__INCUNA_TOKEN__ = \"" + session_token + "\";");
        w.navigate("http://127.0.0.1:" + std::to_string(port) + "/");
        w.run();
    } catch (const std::exception& ex) {
        std::wstring msg = koma::utf8_to_wide(std::string("WebView2 error: ") + ex.what());
        MessageBoxW(nullptr, msg.c_str(), L"Incuna Reader", MB_ICONERROR);
        exit_code = 1;
    }

    ui_running.store(false);
    g_on_file_drop = nullptr;
    active_webview.store(nullptr);
    main_hwnd.store(nullptr);

    // Always restore default OS display/sleep power state on shutdown
    koma::set_thread_keep_awake(false);

    server.stop();
    if (server_thread.joinable()) server_thread.join();
    cloud_client.stop();
    scanner.stop_all_watchers();
    cover_manager.stop();
    reader_session.close();
    db.close();
    return exit_code;
}
