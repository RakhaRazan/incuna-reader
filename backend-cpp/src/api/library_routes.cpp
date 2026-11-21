#include "api/library_routes.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/api_helpers.hpp"
#include "cloud/cloud_client.hpp"
#include "db/database.hpp"
#include "portable_paths.hpp"
#include "watcher/library_scanner.hpp"

namespace koma::api {

namespace {

void unpin_offline(Database& db, int64_t comic_id, httplib::Response& res) {
    ComicRecord comic;
    if (!db.get_comic(comic_id, comic)) {
        send_error(res, "Comic not found", 404);
        return;
    }
    if (!comic.offline_path.empty()) {
        std::error_code del_ec;
        std::filesystem::remove(utf8_to_wide(comic.offline_path), del_ec);
    }
    db.update_comic_offline_pin(comic_id, false, "");
    send_json(res, {{"id", comic_id}, {"is_offline_pinned", false}, {"offline_path", ""}});
}

std::wstring to_lower_wide(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

std::wstring get_env_wide(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring buf(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name, buf.data(), needed);
    if (written == 0) return {};
    if (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    buf.resize(written);
    return buf;
}

std::wstring read_hkcu_reg_string(const wchar_t* subkey, const wchar_t* value_name) {
    DWORD size_bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, subkey, value_name, RRF_RT_REG_SZ, nullptr, nullptr, &size_bytes) != ERROR_SUCCESS ||
        size_bytes <= sizeof(wchar_t)) {
        return {};
    }
    std::wstring buf(size_bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, subkey, value_name, RRF_RT_REG_SZ, nullptr, buf.data(), &size_bytes) != ERROR_SUCCESS) {
        return {};
    }
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

struct CloudSourceCandidate {
    std::string provider;
    std::string name;
    std::wstring path;
};

nlohmann::json discover_cloud_sources(Database& db) {
    std::vector<CloudSourceCandidate> candidates;

    const std::wstring onedrive = get_env_wide(L"OneDrive");
    if (!onedrive.empty()) candidates.push_back({"onedrive", "OneDrive", onedrive});

    const std::wstring onedrive_biz = get_env_wide(L"OneDriveCommercial");
    if (!onedrive_biz.empty()) candidates.push_back({"onedrive_commercial", "OneDrive (Work/School)", onedrive_biz});

    const std::wstring onedrive_pers = get_env_wide(L"OneDriveConsumer");
    if (!onedrive_pers.empty()) candidates.push_back({"onedrive", "OneDrive (Personal)", onedrive_pers});

    const std::wstring reg_od_pers = read_hkcu_reg_string(
        L"Software\\Microsoft\\OneDrive\\Accounts\\Personal", L"UserFolder");
    if (!reg_od_pers.empty()) candidates.push_back({"onedrive", "OneDrive (Personal)", reg_od_pers});

    const std::wstring reg_od_biz = read_hkcu_reg_string(
        L"Software\\Microsoft\\OneDrive\\Accounts\\Business1", L"UserFolder");
    if (!reg_od_biz.empty()) candidates.push_back({"onedrive_commercial", "OneDrive (Work/School)", reg_od_biz});

    const std::wstring userprofile = get_env_wide(L"USERPROFILE");
    if (!userprofile.empty()) {
        const std::filesystem::path up(userprofile);
        candidates.push_back({"onedrive", "OneDrive", (up / L"OneDrive").wstring()});
        candidates.push_back({"google_drive", "Google Drive", (up / L"Google Drive").wstring()});
        candidates.push_back({"google_drive", "Google Drive", (up / L"My Drive").wstring()});
        candidates.push_back({"dropbox", "Dropbox", (up / L"Dropbox").wstring()});
        candidates.push_back({"dropbox", "Dropbox (Personal)", (up / L"Dropbox (Personal)").wstring()});
        candidates.push_back({"dropbox", "Dropbox (Business)", (up / L"Dropbox (Business)").wstring()});
    }

    candidates.push_back({"google_drive", "Google Drive (G:)", L"G:\\My Drive"});

    std::unordered_set<std::wstring> watched_lower;
    for (const auto& wf : db.get_watched_folders()) {
        std::error_code ec;
        std::filesystem::path p(utf8_to_wide(wf.path));
        auto canon = std::filesystem::weakly_canonical(p, ec);
        watched_lower.insert(to_lower_wide((ec ? p : canon).wstring()));
    }

    std::unordered_set<std::wstring> seen_paths;
    nlohmann::json arr = nlohmann::json::array();
    std::error_code ec;
    for (const auto& c : candidates) {
        if (c.path.empty()) continue;
        std::filesystem::path p(c.path);
        if (!std::filesystem::is_directory(p, ec) || ec) {
            ec.clear();
            continue;
        }
        auto canon = std::filesystem::weakly_canonical(p, ec);
        const std::filesystem::path& final_p = ec ? p : canon;
        const std::wstring key = to_lower_wide(final_p.wstring());
        if (!seen_paths.insert(key).second) continue;

        arr.push_back({
            {"provider", c.provider},
            {"name", c.name},
            {"path", wide_to_utf8(final_p.wstring())},
            {"already_added", watched_lower.count(key) > 0}
        });
    }
    return arr;
}

struct BatchPinManager {
    std::atomic<bool> running{false};
    std::atomic<bool> pinned{true};
    std::atomic<int64_t> total{0};
    std::atomic<int64_t> completed{0};
    std::atomic<int64_t> failed{0};
    std::mutex thread_mutex;
    std::thread worker;

    ~BatchPinManager() {
        std::lock_guard<std::mutex> lock(thread_mutex);
        if (worker.joinable()) {
            worker.join();
        }
    }
};

} // namespace

void register_library_routes(httplib::Server& server, LibraryRouteContext ctx) {
    auto batch_pin = std::make_shared<BatchPinManager>();

    // ---- Library & FTS5 Search ----
    server.Get("/api/library", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        LibraryQuery q;
        q.search        = query_str(req, "search");
        q.smartview     = query_str(req, "smartview");
        q.smartview_id  = query_int64(req, "smartview_id", q.smartview_id);
        q.folder_prefix = query_str(req, "folder_prefix");
        q.author        = query_str(req, "author");
        q.tag           = query_str(req, "tag");
        q.sort          = query_str(req, "sort");
        q.shelf_id       = query_int64(req, "shelf_id", q.shelf_id);
        q.offset         = std::max(0, query_int(req, "offset", 0));
        q.limit          = std::clamp(query_int(req, "limit", 120), 1, 500);
        q.include_hidden = query_bool(req, "include_hidden", false);

        auto page = ctx.db.query_library(q);
        nlohmann::json items = nlohmann::json::array();
        for (const auto& c : page.items) {
            items.push_back(c.to_json());
        }
        send_json(res, {
            {"total", page.total_count},
            {"offset", q.offset},
            {"limit", q.limit},
            {"items", items}
        });
    }));

    // ---- Continue Reading History ----
    server.Get("/api/library/continue-reading", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int limit = std::clamp(query_int(req, "limit", 10), 1, 100);
        const bool include_hidden = query_bool(req, "include_hidden", false);
        auto comics = ctx.db.get_continue_reading(limit, include_hidden);
        nlohmann::json items = nlohmann::json::array();
        for (const auto& c : comics) {
            items.push_back(c.to_json());
        }
        send_json(res, {
            {"total", items.size()},
            {"items", items}
        });
    }));

    // ---- Random Unread Comic (<2ms) ----
    server.Get("/api/library/random", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        RandomComicQuery q;
        q.shelf_id       = query_int64(req, "shelf_id", 0);
        q.smartview      = query_str(req, "smartview");
        q.smartview_id   = query_int64(req, "smartview_id", 0);
        q.author         = query_str(req, "author");
        q.tag            = query_str(req, "tag");
        q.include_hidden = query_bool(req, "include_hidden", false);

        ComicRecord picked;
        if (!ctx.db.get_random_unread_comic(q, picked)) {
            send_error(res, "No unread comics found", 404);
            return;
        }
        send_json(res, {
            {"id", picked.id},
            {"comic", picked.to_json()}
        });
    }));

    // ---- Folder Importer & Watched Folders ----
    server.Post("/api/library/import-folder", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_object(req, body, res)) return;
        if (!body.contains("path") || !body["path"].is_string()) {
            send_error(res, "Missing 'path' string", 400);
            return;
        }
        const std::wstring wpath = utf8_to_wide(body["path"].get<std::string>());
        if (!ctx.scanner.start_folder_import(wpath)) {
            auto st = ctx.scanner.get_scan_status();
            send_json(res, {{"started", false}, {"error", st.last_error}}, 409);
            return;
        }
        send_json(res, {{"started", true}, {"path", body["path"]}}, 202);
    }));

    server.Get("/api/library/scan-status", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        auto st = ctx.scanner.get_scan_status();
        send_json(res, {
            {"scanning", st.scanning},
            {"current_folder", st.current_folder},
            {"queued_folders", st.queued_folders},
            {"files_found", st.files_found},
            {"files_indexed", st.files_indexed},
            {"batches_committed", st.batches_committed},
            {"remapped_paths", st.remapped_paths},
            {"last_error", st.last_error}
        });
    }));

    auto handle_watched_folders = guarded([ctx](const httplib::Request&, httplib::Response& res) {
        auto folders = ctx.db.get_watched_folders();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& f : folders) {
            const bool is_online = LibraryScanner::is_folder_online(utf8_to_wide(f.path));
            arr.push_back({
                {"id", f.id},
                {"path", f.path},
                {"name", f.name},
                {"is_network", f.is_network},
                {"is_online", is_online},
                {"auto_sync", f.auto_sync},
                {"added_at", f.added_at},
                {"last_scanned_at", f.last_scanned_at}
            });
        }
        send_json(res, arr);
    });
    server.Get("/api/library/watched-folders", handle_watched_folders);
    server.Get("/api/watched-folders", handle_watched_folders);

    // ---- Storage: Cloud Discovery, Batch Offline Pinning & Cache Stats ----
    server.Get("/api/storage/cloud-sources", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        send_json(res, {{"sources", discover_cloud_sources(ctx.db)}});
    }));

    server.Get("/api/storage/offline-stats", guarded([ctx, batch_pin](const httplib::Request&, httplib::Response& res) {
        const std::filesystem::path cache_dir = ctx.paths.offline_cache_dir();
        int64_t total_bytes = 0;
        int64_t file_count = 0;
        std::error_code ec;
        if (std::filesystem::is_directory(cache_dir, ec)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(cache_dir, ec)) {
                if (ec) { ec.clear(); continue; }
                if (entry.is_regular_file(ec) && !ec) {
                    const auto sz = entry.file_size(ec);
                    if (!ec) {
                        total_bytes += static_cast<int64_t>(sz);
                        ++file_count;
                    }
                }
            }
        }
        send_json(res, {
            {"cache_dir", wide_to_utf8(cache_dir.wstring())},
            {"pinned_count", ctx.db.count_offline_pinned_comics()},
            {"file_count", file_count},
            {"total_bytes", total_bytes},
            {"batch_running", batch_pin->running.load()},
            {"batch_total", batch_pin->total.load()},
            {"batch_completed", batch_pin->completed.load()},
            {"batch_failed", batch_pin->failed.load()},
            {"batch_pinned", batch_pin->pinned.load()}
        });
    }));

    server.Post("/api/storage/clear-unpinned-cache", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        std::unordered_set<std::wstring> pinned_lower;
        std::error_code ec;
        for (const auto& p_utf8 : ctx.db.get_pinned_offline_paths()) {
            std::filesystem::path p(utf8_to_wide(p_utf8));
            auto canon = std::filesystem::weakly_canonical(p, ec);
            pinned_lower.insert(to_lower_wide((ec ? p : canon).wstring()));
            ec.clear();
        }

        int64_t removed_files = 0;
        int64_t freed_bytes = 0;
        const std::filesystem::path cache_dir = ctx.paths.offline_cache_dir();
        if (std::filesystem::is_directory(cache_dir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(cache_dir, ec)) {
                if (ec) { ec.clear(); continue; }
                if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }

                auto canon = std::filesystem::weakly_canonical(entry.path(), ec);
                const std::wstring key = to_lower_wide((ec ? entry.path() : canon).wstring());
                ec.clear();

                if (pinned_lower.count(key) > 0) continue;

                const auto sz = entry.file_size(ec);
                const int64_t file_bytes = ec ? 0 : static_cast<int64_t>(sz);
                ec.clear();
                if (std::filesystem::remove(entry.path(), ec) && !ec) {
                    ++removed_files;
                    freed_bytes += file_bytes;
                }
                ec.clear();
            }
        }
        send_json(res, {
            {"removed_files", removed_files},
            {"freed_bytes", freed_bytes}
        });
    }));

    server.Post("/api/library/pin-offline-batch", guarded([ctx, batch_pin](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_object(req, body, res)) return;

        const bool want_pin = body.value("pinned", true);
        std::vector<int64_t> ids;
        std::unordered_set<int64_t> seen_ids;

        if (body.contains("comic_ids") && body["comic_ids"].is_array()) {
            for (const auto& v : body["comic_ids"]) {
                if (v.is_number_integer()) {
                    const int64_t id = v.get<int64_t>();
                    if (id > 0 && seen_ids.insert(id).second) {
                        ids.push_back(id);
                    }
                }
            }
        }

        if (body.contains("shelf_id") && body["shelf_id"].is_number_integer()) {
            const int64_t shelf_id = body["shelf_id"].get<int64_t>();
            if (shelf_id > 0) {
                for (int64_t id : ctx.db.get_comic_ids_in_shelf(shelf_id)) {
                    if (id > 0 && seen_ids.insert(id).second) {
                        ids.push_back(id);
                    }
                }
            }
        }

        if (ids.empty()) {
            send_error(res, "Missing 'comic_ids' or valid 'shelf_id'", 400);
            return;
        }

        if (batch_pin->running.exchange(true)) {
            send_json(res, {{"started", false}, {"error", "A batch offline pin operation is already in progress"}}, 409);
            return;
        }

        batch_pin->pinned.store(want_pin);
        batch_pin->total.store(static_cast<int64_t>(ids.size()));
        batch_pin->completed.store(0);
        batch_pin->failed.store(0);

        std::lock_guard<std::mutex> lock(batch_pin->thread_mutex);
        if (batch_pin->worker.joinable()) {
            batch_pin->worker.join();
        }
        const std::filesystem::path cache_dir = ctx.paths.offline_cache_dir();
        Database& db_ref = ctx.db;
        batch_pin->worker = std::thread([batch_pin, &db_ref, cache_dir, ids = std::move(ids), want_pin]() {
            std::error_code ec;
            if (want_pin) {
                std::filesystem::create_directories(cache_dir, ec);
            }
            for (int64_t comic_id : ids) {
                ComicRecord comic;
                if (!db_ref.get_comic(comic_id, comic)) {
                    ++batch_pin->failed;
                    continue;
                }
                if (!want_pin) {
                    if (!comic.offline_path.empty()) {
                        std::filesystem::remove(utf8_to_wide(comic.offline_path), ec);
                    }
                    db_ref.update_comic_offline_pin(comic_id, false, "");
                    ++batch_pin->completed;
                } else {
                    if (comic.is_offline_pinned && !comic.offline_path.empty() &&
                        std::filesystem::exists(utf8_to_wide(comic.offline_path), ec)) {
                        ++batch_pin->completed;
                        continue;
                    }
                    const std::filesystem::path src(utf8_to_wide(comic.path));
                    const std::filesystem::path dst =
                        cache_dir / (L"comic_" + std::to_wstring(comic_id) + L"_" + src.filename().wstring());
                    std::error_code copy_ec;
                    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, copy_ec);
                    if (copy_ec) {
                        ++batch_pin->failed;
                    } else {
                        db_ref.update_comic_offline_pin(comic_id, true, wide_to_utf8(dst.wstring()));
                        ++batch_pin->completed;
                    }
                }
            }
            batch_pin->running.store(false);
        });

        send_json(res, {
            {"started", true},
            {"pinned", want_pin},
            {"total", batch_pin->total.load()}
        }, 202);
    }));

    server.Get("/api/library/folders", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        std::string parent_path = query_str(req, "parent_path");
        if (parent_path.empty()) {
            parent_path = query_str(req, "prefix");
        }
        const bool include_hidden = query_bool(req, "include_hidden", false);
        auto folders = ctx.db.get_library_folders(parent_path, include_hidden);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& f : folders) {
            arr.push_back({
                {"name", f.name},
                {"path", f.path},
                {"comic_count", f.comic_count},
                {"preview_comic_id", f.preview_comic_id}
            });
        }
        send_json(res, {
            {"parent_path", parent_path},
            {"prefix", parent_path},
            {"folders", arr}
        });
    }));

    server.Delete(R"(/api/library/folders/(\d+))", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t id = std::stoll(req.matches[1].str());
        const bool deleted = ctx.scanner.remove_watched_folder(id);
        if (deleted) ctx.refresh_jumplist();
        send_json(res, {{"deleted", deleted}});
    }));

    // ---- Shelves CRUD ----
    server.Get("/api/shelves", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        auto shelves = ctx.db.get_shelves();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : shelves) {
            arr.push_back({
                {"id", s.id},
                {"name", s.name},
                {"sort_order", s.sort_order},
                {"comic_count", s.comic_count},
                {"created_at", s.created_at}
            });
        }
        send_json(res, arr);
    }));

    server.Post("/api/shelves", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_body(req, body, res)) return;
        const std::string name = body.value("name", "");
        ShelfRecord created;
        if (!ctx.db.create_shelf(name, created)) {
            send_error(res, ctx.db.last_error(), 400);
            return;
        }
        send_json(res, {
            {"id", created.id},
            {"name", created.name},
            {"sort_order", created.sort_order},
            {"comic_count", 0}
        }, 201);
    }));

    auto handle_shelf_rename = guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t id = std::stoll(req.matches[1].str());
        nlohmann::json body;
        if (!parse_json_body(req, body, res)) return;
        const std::string name = body.value("name", "");
        if (!ctx.db.rename_shelf(id, name)) {
            send_error(res, ctx.db.last_error(), 400);
            return;
        }
        send_json(res, {{"id", id}, {"name", name}});
    });
    server.Put(R"(/api/shelves/(\d+))", handle_shelf_rename);
    server.Patch(R"(/api/shelves/(\d+))", handle_shelf_rename);

    server.Delete(R"(/api/shelves/(\d+))", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t id = std::stoll(req.matches[1].str());
        send_json(res, {{"deleted", ctx.db.delete_shelf(id)}});
    }));

    server.Post(R"(/api/shelves/(\d+)/comics)", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t shelf_id = std::stoll(req.matches[1].str());
        nlohmann::json body;
        if (!parse_json_body(req, body, res)) return;
        const int64_t comic_id = body.at("comic_id").get<int64_t>();
        send_json(res, {{"added", ctx.db.add_comic_to_shelf(shelf_id, comic_id)}});
    }));

    server.Delete(R"(/api/shelves/(\d+)/comics/(\d+))", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t shelf_id = std::stoll(req.matches[1].str());
        const int64_t comic_id = std::stoll(req.matches[2].str());
        send_json(res, {{"removed", ctx.db.remove_comic_from_shelf(shelf_id, comic_id)}});
    }));

    // ---- Comics Progress, Favorites, Bookmarks & Cloud Offline Pinning ----
    auto handle_progress = guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t comic_id = std::stoll(req.matches[1].str());
        nlohmann::json body;
        if (!parse_json_body(req, body, res)) return;
        const int current_page = body.contains("current_page") ? body["current_page"].get<int>() : -1;
        const int page_count   = body.contains("page_count")   ? body["page_count"].get<int>()   : 0;
        const std::string status = body.value("status", "");
        int is_fav = -1;
        if (body.contains("is_favorite") && body["is_favorite"].is_boolean()) {
            is_fav = body["is_favorite"].get<bool>() ? 1 : 0;
        }
        if (!ctx.db.update_comic_progress(comic_id, current_page, page_count, status, is_fav)) {
            send_error(res, ctx.db.last_error(), 400);
            return;
        }
        if ((current_page >= 0 || !status.empty()) && ctx.refresh_jumplist) {
            ctx.refresh_jumplist();
        }
        ComicRecord updated;
        if (ctx.db.get_comic(comic_id, updated)) {
            send_json(res, updated.to_json());
        } else {
            send_json(res, {{"updated", true}});
        }
    });
    server.Post(R"(/api/comics/(\d+)/progress)", handle_progress);
    server.Patch(R"(/api/comics/(\d+)/progress)", handle_progress);
    server.Put(R"(/api/comics/(\d+)/progress)", handle_progress);

    server.Get(R"(/api/comics/(\d+)/bookmarks)", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t comic_id = std::stoll(req.matches[1].str());
        auto bms = ctx.db.get_bookmarks(comic_id);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& b : bms) {
            arr.push_back({
                {"id", b.id},
                {"comic_id", b.comic_id},
                {"page_number", b.page_number},
                {"name", b.name},
                {"created_at", b.created_at}
            });
        }
        send_json(res, arr);
    }));

    server.Post(R"(/api/comics/(\d+)/bookmarks)", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t comic_id = std::stoll(req.matches[1].str());
        nlohmann::json body;
        if (!parse_json_body(req, body, res)) return;
        const int page_number = body.at("page_number").get<int>();
        const std::string name = body.value("name", "");
        BookmarkRecord created;
        if (!ctx.db.add_bookmark(comic_id, page_number, name, created)) {
            send_error(res, ctx.db.last_error(), 400);
            return;
        }
        send_json(res, {
            {"id", created.id},
            {"comic_id", created.comic_id},
            {"page_number", created.page_number},
            {"name", created.name}
        }, 201);
    }));

    server.Delete(R"(/api/comics/(\d+)/bookmarks/(\d+))", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t bm_id = std::stoll(req.matches[2].str());
        send_json(res, {{"deleted", ctx.db.delete_bookmark(bm_id)}});
    }));

    server.Post(R"(/api/comics/(\d+)/pin-offline)", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t comic_id = std::stoll(req.matches[1].str());

        bool want_pin = true;
        if (!req.body.empty()) {
            try {
                auto body = nlohmann::json::parse(req.body);
                if (body.is_object() && body.contains("pinned") && body["pinned"].is_boolean()) {
                    want_pin = body["pinned"].get<bool>();
                }
            } catch (...) {}
        }

        if (!want_pin) {
            unpin_offline(ctx.db, comic_id, res);
            return;
        }

        ComicRecord comic;
        if (!ctx.db.get_comic(comic_id, comic)) {
            send_error(res, "Comic not found", 404);
            return;
        }

        const std::filesystem::path src(utf8_to_wide(comic.path));
        const std::filesystem::path dst =
            ctx.paths.offline_cache_dir() / (L"comic_" + std::to_wstring(comic_id) + L"_" + src.filename().wstring());

        std::error_code copy_ec;
        std::filesystem::create_directories(ctx.paths.offline_cache_dir(), copy_ec);
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            send_error(res, "Failed to copy file to offline_cache: " + copy_ec.message(), 500);
            return;
        }

        const std::string offline_utf8 = wide_to_utf8(dst.wstring());
        ctx.db.update_comic_offline_pin(comic_id, true, offline_utf8);
        send_json(res, {{"id", comic_id}, {"is_offline_pinned", true}, {"offline_path", offline_utf8}});
    }));

    server.Delete(R"(/api/comics/(\d+)/pin-offline)", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t comic_id = std::stoll(req.matches[1].str());
        unpin_offline(ctx.db, comic_id, res);
    }));

    server.Post(R"(/api/comics/(\d+)/hidden)", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        int64_t comic_id = 0;
        if (!match_int64(req, 1, comic_id) || comic_id <= 0) {
            send_error(res, "Invalid comic id", 400);
            return;
        }
        nlohmann::json body;
        if (!parse_json_object(req, body, res)) return;
        bool hidden = false;
        if (body.contains("hidden") && body["hidden"].is_boolean()) {
            hidden = body["hidden"].get<bool>();
        } else if (body.contains("is_hidden") && body["is_hidden"].is_boolean()) {
            hidden = body["is_hidden"].get<bool>();
        } else {
            send_error(res, "Missing boolean 'hidden' field", 400);
            return;
        }
        if (!ctx.db.set_comic_hidden(comic_id, hidden)) {
            send_error(res, "Comic not found", 404);
            return;
        }
        send_json(res, {{"id", comic_id}, {"hidden", hidden}, {"is_hidden", hidden}});
    }));

    // ---- Batch Comic Actions ----
    server.Post("/api/comics/batch", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_object(req, body, res)) return;

        if (!body.contains("comic_ids") || !body["comic_ids"].is_array() || body["comic_ids"].empty()) {
            send_error(res, "Missing or empty 'comic_ids' array", 400);
            return;
        }
        std::vector<int64_t> ids;
        ids.reserve(body["comic_ids"].size());
        for (const auto& v : body["comic_ids"]) {
            if (v.is_number_integer()) {
                ids.push_back(v.get<int64_t>());
            }
        }
        if (ids.empty()) {
            send_error(res, "No valid integer comic_ids provided", 400);
            return;
        }

        const std::string action = body.value("action", "");
        int64_t updated = 0;
        int64_t failed = 0;

        if (action == "set_status") {
            const std::string status = body.value("status", "");
            if (status != "unread" && status != "reading" && status != "read") {
                send_error(res, "Invalid 'status' (expected unread, reading, or read)", 400);
                return;
            }
            updated = ctx.db.batch_update_status(ids, status);
            if (updated > 0 && ctx.refresh_jumplist) {
                ctx.refresh_jumplist();
            }
        } else if (action == "set_favorite") {
            const bool is_fav = body.value("is_favorite", true);
            updated = ctx.db.batch_update_favorite(ids, is_fav);
        } else if (action == "set_hidden") {
            const bool is_hidden = body.contains("hidden")
                ? body.value("hidden", true)
                : body.value("is_hidden", true);
            updated = ctx.db.batch_update_hidden(ids, is_hidden);
        } else if (action == "assign_shelf") {
            const int64_t shelf_id = body.value("shelf_id", int64_t{0});
            if (shelf_id <= 0) {
                send_error(res, "Missing valid 'shelf_id'", 400);
                return;
            }
            updated = ctx.db.batch_assign_shelf(shelf_id, ids);
        } else if (action == "remove_shelf") {
            const int64_t shelf_id = body.value("shelf_id", int64_t{0});
            if (shelf_id <= 0) {
                send_error(res, "Missing valid 'shelf_id'", 400);
                return;
            }
            updated = ctx.db.batch_remove_shelf(shelf_id, ids);
        } else if (action == "pin_offline") {
            const bool want_pin = body.value("pinned", true);
            std::error_code ec;
            if (want_pin) {
                std::filesystem::create_directories(ctx.paths.offline_cache_dir(), ec);
            }
            for (int64_t comic_id : ids) {
                ComicRecord comic;
                if (!ctx.db.get_comic(comic_id, comic)) {
                    ++failed;
                    continue;
                }
                if (!want_pin) {
                    if (!comic.offline_path.empty()) {
                        std::filesystem::remove(utf8_to_wide(comic.offline_path), ec);
                    }
                    ctx.db.update_comic_offline_pin(comic_id, false, "");
                    ++updated;
                } else {
                    const std::filesystem::path src(utf8_to_wide(comic.path));
                    const std::filesystem::path dst =
                        ctx.paths.offline_cache_dir() /
                        (L"comic_" + std::to_wstring(comic_id) + L"_" + src.filename().wstring());
                    std::error_code copy_ec;
                    std::filesystem::copy_file(
                        src, dst, std::filesystem::copy_options::overwrite_existing, copy_ec);
                    if (copy_ec) {
                        ++failed;
                    } else {
                        ctx.db.update_comic_offline_pin(comic_id, true, wide_to_utf8(dst.wstring()));
                        ++updated;
                    }
                }
            }
        } else {
            send_error(res, "Unknown batch action", 400);
            return;
        }

        send_json(res, {
            {"action", action},
            {"updated", updated},
            {"failed", failed}
        });
    }));

    // ---- Smartviews CRUD ----
    server.Get("/api/smartviews", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        auto views = ctx.db.get_smartviews();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& v : views) {
            nlohmann::json query_obj = nlohmann::json::object();
            try {
                query_obj = nlohmann::json::parse(v.query_json);
            } catch (...) {}
            arr.push_back({
                {"id", v.id},
                {"slug", v.slug},
                {"name", v.name},
                {"query", query_obj},
                {"is_builtin", v.is_builtin},
                {"sort_order", v.sort_order}
            });
        }
        send_json(res, arr);
    }));

    server.Post("/api/smartviews", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_body(req, body, res)) return;
        const std::string slug = body.at("slug").get<std::string>();
        const std::string name = body.at("name").get<std::string>();
        const std::string qjson = body.contains("query") ? body["query"].dump() : "{}";
        SmartViewRecord created;
        if (!ctx.db.create_smartview(slug, name, qjson, created)) {
            send_error(res, ctx.db.last_error(), 400);
            return;
        }
        send_json(res, {
            {"id", created.id},
            {"slug", created.slug},
            {"name", created.name},
            {"query", nlohmann::json::parse(created.query_json)},
            {"is_builtin", false}
        }, 201);
    }));

    server.Delete(R"(/api/smartviews/(\d+))", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const int64_t id = std::stoll(req.matches[1].str());
        send_json(res, {{"deleted", ctx.db.delete_smartview(id)}});
    }));

    // ---- Direct Google Drive & OneDrive Connector ----
    server.Post("/api/cloud/auth/start", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_object(req, body, res)) return;
        const std::string provider = body.value("provider", "");
        const std::string client_id = body.value("client_id", "");
        const std::string client_secret = body.value("client_secret", "");

        auto started = ctx.cloud.start_auth(provider, client_id, client_secret);
        if (!started.started) {
            send_error(res, started.error, 400);
            return;
        }
        send_json(res, {
            {"started", true},
            {"provider", started.provider},
            {"auth_url", started.auth_url},
            {"redirect_uri", started.redirect_uri}
        }, 202);
    }));

    server.Get("/api/cloud/status", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        send_json(res, ctx.cloud.get_status());
    }));

    server.Post("/api/cloud/disconnect", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_object(req, body, res)) return;
        const std::string provider = body.value("provider", "");
        if (!CloudClient::is_valid_provider(provider)) {
            send_error(res, "Unsupported provider (expected 'gdrive' or 'onedrive')", 400);
            return;
        }
        const bool ok = ctx.cloud.disconnect(provider);
        send_json(res, {{"disconnected", ok}, {"provider", provider}});
    }));

    server.Get("/api/cloud/list", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        const std::string provider = query_str(req, "provider");
        const std::string folder_id = query_str(req, "folder_id");
        auto listed = ctx.cloud.list_folder(provider, folder_id);
        if (!listed.ok) {
            send_error(res, listed.error, listed.http_status);
            return;
        }
        nlohmann::json items = nlohmann::json::array();
        for (const auto& entry : listed.items) {
            items.push_back({
                {"id", entry.id},
                {"name", entry.name},
                {"is_folder", entry.is_folder},
                {"size", entry.size},
                {"format", entry.format},
                {"modified_at", entry.modified_at}
            });
        }
        send_json(res, {
            {"provider", provider},
            {"folder_id", folder_id.empty() ? "root" : folder_id},
            {"items", items}
        });
    }));

    server.Post("/api/cloud/import", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_object(req, body, res)) return;
        const std::string provider = body.value("provider", "");
        if (!CloudClient::is_valid_provider(provider)) {
            send_error(res, "Unsupported provider (expected 'gdrive' or 'onedrive')", 400);
            return;
        }

        std::vector<CloudImportItem> items;
        if (body.contains("items") && body["items"].is_array()) {
            for (const auto& v : body["items"]) {
                if (v.is_object() && v.contains("id") && v["id"].is_string()) {
                    CloudImportItem item;
                    item.id = v["id"].get<std::string>();
                    item.name = v.value("name", "");
                    item.is_folder = v.value("is_folder", false);
                    if (!item.id.empty()) items.push_back(std::move(item));
                }
            }
        } else if (body.contains("folder_id") && body["folder_id"].is_string() && !body["folder_id"].get<std::string>().empty()) {
            CloudImportItem item;
            item.id = body["folder_id"].get<std::string>();
            item.name = body.value("folder_name", "");
            item.is_folder = true;
            items.push_back(std::move(item));
        }

        if (items.empty()) {
            send_error(res, "Missing 'items' array or 'folder_id' to import", 400);
            return;
        }

        auto started = ctx.cloud.start_import(provider, items);
        if (!started.started) {
            const int status = (started.error.find("already in progress") != std::string::npos) ? 409 : 400;
            send_error(res, started.error, status);
            return;
        }
        send_json(res, {
            {"started", true},
            {"provider", provider},
            {"queued", started.queued}
        }, 202);
    }));
}

} // namespace koma::api
