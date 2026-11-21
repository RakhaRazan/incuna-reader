#include "api/reader_routes.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api/api_helpers.hpp"
#include "covers/cover_manager.hpp"
#include "db/database.hpp"
#include "formats/image_utils.hpp"
#include "formats/reader_session.hpp"
#include "portable_paths.hpp"
#include "watcher/library_scanner.hpp"
#include "win_jumplist.hpp"

namespace koma::api {

StartupLaunch parse_startup_args(PWSTR lpCmdLine) {
    StartupLaunch result;
    if (!lpCmdLine || lpCmdLine[0] == L'\0') return result;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
    if (!argv || argc <= 0) {
        if (argv) LocalFree(argv);
        return result;
    }

    std::wstring candidate;
    for (int i = 0; i < argc; ++i) {
        if (!argv[i] || argv[i][0] == L'\0') continue;
        std::wstring arg = argv[i];
        if (arg == L"--page" || arg == L"-p") {
            if (i + 1 < argc && argv[i + 1]) {
                try { result.page = std::stoi(argv[++i]); } catch (...) {}
            }
        } else if (arg.rfind(L"--page=", 0) == 0) {
            try { result.page = std::stoi(arg.substr(7)); } catch (...) {}
        } else if (arg[0] != L'-' && candidate.empty()) {
            candidate = arg;
        }
    }
    LocalFree(argv);

    if (candidate.empty()) return result;

    std::error_code ec;
    std::filesystem::path p(candidate);
    if (std::filesystem::exists(p, ec)) {
        p = std::filesystem::weakly_canonical(p, ec);
    }
    result.file_path = wide_to_utf8(p.wstring());
    return result;
}

void register_reader_routes(httplib::Server& server, ReaderRouteContext ctx) {
    // ---- Health, Settings, Startup File & Jump List ----
    server.Get("/api/health", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        nlohmann::json body{
            {"status", ctx.db_ok ? "ok" : "degraded"},
            {"version", "0.1.0"},
            {"portable_root", ctx.paths.exe_dir_utf8()},
            {"db_path", ctx.paths.library_db_utf8()},
            {"db_open", ctx.db.is_open()},
            {"fts5", ctx.db.verify_fts5_available()},
            {"comics_count", ctx.db.count_comics()},
            {"smartviews_count", static_cast<int>(ctx.db.get_smartviews().size())},
            {"cover_ram_bytes", ctx.cover_manager.ram_cache_bytes()}
        };
        if (!ctx.db_ok) {
            body["db_error"] = ctx.db.last_error();
        }
        send_json(res, body);
    }));

    server.Get("/api/settings", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        send_json(res, ctx.settings.get());
    }));

    auto handle_settings_update = guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json patch;
        if (!parse_json_object(req, patch, res)) return;
        const auto prev = ctx.settings.get();
        const std::string prev_mode = prev.value("cover_mode", "persistent_pack");
        auto updated = ctx.settings.update(patch);
        const std::string new_mode = updated.value("cover_mode", "persistent_pack");

        // Clean revert: switching from persistent_pack back to ram_only purges covers.pack
        if (prev_mode == "persistent_pack" && new_mode == "ram_only") {
            ctx.cover_manager.purge_pack();
        }
        send_json(res, updated);
    });
    server.Post("/api/settings", handle_settings_update);
    server.Put("/api/settings", handle_settings_update);
    server.Patch("/api/settings", handle_settings_update);

    server.Get("/api/startup-file", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(ctx.startup_mutex);
        ComicRecord rec;
        const bool in_db = !ctx.startup.file_path.empty() && ctx.db_ok &&
                           ctx.db.get_comic_by_path(ctx.startup.file_path, rec);
        const int page = ctx.startup.page > 0 ? ctx.startup.page : (in_db ? rec.current_page : 0);
        send_json(res, {
            {"path", ctx.startup.file_path},
            {"has_file", !ctx.startup.file_path.empty()},
            {"page", page},
            {"comic_id", in_db ? rec.id : 0},
            {"title", in_db ? rec.title : ""}
        });
    }));

    server.Delete("/api/startup-file", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(ctx.startup_mutex);
        ctx.startup.file_path.clear();
        ctx.startup.page = 0;
        send_json(res, {{"cleared", true}});
    }));

    server.Get("/api/jumplist", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        auto recent = ctx.db.get_recent_comics(8);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : recent) {
            JumpListEntry e{c.id, c.title, c.path, c.current_page > 0 ? c.current_page : 1, c.page_count};
            arr.push_back({
                {"comic_id", e.comic_id},
                {"title", e.title},
                {"path", e.path},
                {"current_page", e.current_page},
                {"page_count", e.page_count},
                {"label", wide_to_utf8(WinJumpList::format_display_title(e))}
            });
        }
        send_json(res, {{"app_id", wide_to_utf8(WinJumpList::kAppUserModelId)}, {"items", arr}});
    }));

    // ---- Native Windows Explorer Dialogs ----
    server.Post("/api/dialog/pick-folder", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        const std::wstring wpath = ctx.pick_folder_on_ui ? ctx.pick_folder_on_ui() : std::wstring{};
        if (wpath.empty()) {
            send_json(res, {{"cancelled", true}, {"path", ""}, {"started", false}});
            return;
        }
        const std::string utf8_path = wide_to_utf8(wpath);
        if (!ctx.scanner.start_folder_import(wpath)) {
            auto st = ctx.scanner.get_scan_status();
            send_json(res, {
                {"cancelled", false},
                {"path", utf8_path},
                {"started", false},
                {"error", st.last_error}
            }, 409);
            return;
        }
        send_json(res, {
            {"cancelled", false},
            {"path", utf8_path},
            {"started", true}
        }, 202);
    }));

    server.Post("/api/dialog/pick-files", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        const std::vector<std::wstring> wpaths = ctx.pick_files_on_ui ? ctx.pick_files_on_ui() : std::vector<std::wstring>{};
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& wp : wpaths) {
            arr.push_back(wide_to_utf8(wp));
        }
        send_json(res, {
            {"cancelled", wpaths.empty()},
            {"paths", arr}
        });
    }));

    // ---- 2-Mode Cover Thumbnail Endpoints ----
    server.Get(R"(/api/covers/(\d+))", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        int64_t comic_id = 0;
        if (!match_int64(req, 1, comic_id)) {
            send_error(res, "Invalid comic id", 400);
            return;
        }
        auto cover = ctx.cover_manager.get_cover(comic_id);
        if (cover.cancelled) {
            res.set_header("Retry-After", "1");
            res.set_header("Cache-Control", "no-store");
            send_error(res, "Cover request aborted during scroll", 503);
            return;
        }
        if (!cover.found || cover.data.empty()) {
            res.set_header("Cache-Control", "no-store");
            send_error(res, "Cover unavailable", 404);
            return;
        }
        res.set_header("X-Cover-Source", cover.source);
        res.set_header("Cache-Control", "max-age=86400");
        res.set_content(
            reinterpret_cast<const char*>(cover.data.data()),
            cover.data.size(),
            cover.mime_type);
    }));

    server.Post("/api/covers/cancel", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_body(req, body, res)) return;
        std::vector<int64_t> ids;
        if (body.is_object() && body.contains("ids") && body["ids"].is_array()) {
            for (const auto& v : body["ids"]) {
                if (v.is_number_integer()) ids.push_back(v.get<int64_t>());
            }
        } else if (body.is_array()) {
            for (const auto& v : body) {
                if (v.is_number_integer()) ids.push_back(v.get<int64_t>());
            }
        }
        ctx.cover_manager.cancel_ids(ids);
        send_json(res, {{"cancelled", ids.size()}});
    }));

    server.Post("/api/covers/viewport", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_body(req, body, res)) return;
        std::vector<int64_t> ids;
        if (body.is_object() && body.contains("visible_ids") && body["visible_ids"].is_array()) {
            ids.reserve(body["visible_ids"].size());
            for (const auto& v : body["visible_ids"]) {
                if (v.is_number_integer()) ids.push_back(v.get<int64_t>());
            }
        } else if (body.is_array()) {
            ids.reserve(body.size());
            for (const auto& v : body) {
                if (v.is_number_integer()) ids.push_back(v.get<int64_t>());
            }
        }
        ctx.cover_manager.set_active_viewport(ids);
        send_json(res, {{"active_count", ids.size()}});
    }));

    server.Post("/api/covers/pack/build", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        const bool started = ctx.cover_manager.start_pack_build();
        auto st = ctx.cover_manager.get_pack_status();
        send_json(res, {
            {"started", started},
            {"building", st.building},
            {"total", st.total},
            {"processed", st.processed},
            {"succeeded", st.succeeded},
            {"failed", st.failed},
            {"pack_size_bytes", st.pack_size_bytes}
        }, started ? 202 : 200);
    }));

    server.Get("/api/covers/pack/status", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        auto st = ctx.cover_manager.get_pack_status();
        send_json(res, {
            {"building", st.building},
            {"total", st.total},
            {"processed", st.processed},
            {"succeeded", st.succeeded},
            {"failed", st.failed},
            {"pack_size_bytes", st.pack_size_bytes}
        });
    }));

    server.Post("/api/covers/pack/purge", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        const bool ok = ctx.cover_manager.purge_pack();
        send_json(res, {{"purged", ok}});
    }));

    server.Post("/api/covers/pack/rebuild", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        const bool started = ctx.cover_manager.rebuild_pack();
        auto st = ctx.cover_manager.get_pack_status();
        send_json(res, {
            {"rebuilt", true},
            {"started", started},
            {"building", st.building},
            {"total", st.total},
            {"processed", st.processed},
            {"succeeded", st.succeeded},
            {"failed", st.failed},
            {"pack_size_bytes", st.pack_size_bytes}
        }, started ? 202 : 200);
    }));

    // ---- Reader Session Endpoints ----
    server.Post("/api/reader/open", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_body(req, body, res)) return;
        std::string utf8_path;
        int64_t comic_id = 0;

        if (body.is_object() && body.contains("comic_id") && body["comic_id"].is_number_integer()) {
            comic_id = body["comic_id"].get<int64_t>();
            ComicRecord rec;
            if (ctx.db.get_comic(comic_id, rec)) {
                utf8_path = rec.path;
                // Transparently serve from local offline_cache if pinned and available
                if (rec.is_offline_pinned && !rec.offline_path.empty()) {
                    std::error_code of_ec;
                    if (std::filesystem::exists(utf8_to_wide(rec.offline_path), of_ec)) {
                        utf8_path = rec.offline_path;
                    }
                }
            }
        }

        if (utf8_path.empty() && body.is_object() && body.contains("path") && body["path"].is_string()) {
            utf8_path = body["path"].get<std::string>();
            ComicRecord rec;
            if (ctx.db.get_comic_by_path(utf8_path, rec) && rec.is_offline_pinned && !rec.offline_path.empty()) {
                std::error_code of_ec;
                if (!std::filesystem::exists(utf8_to_wide(utf8_path), of_ec) &&
                    std::filesystem::exists(utf8_to_wide(rec.offline_path), of_ec)) {
                    utf8_path = rec.offline_path;
                }
            }
        }

        if (utf8_path.empty()) {
            send_error(res, "Missing 'path' or valid 'comic_id'", 400);
            return;
        }

        const std::wstring wide_path = utf8_to_wide(utf8_path);
        if (!ctx.reader_session.open(wide_path)) {
            send_json(res, {{"error", ctx.reader_session.last_error()}, {"path", utf8_path}}, 422);
            return;
        }

        if (comic_id > 0) {
            ctx.db.update_comic_progress(comic_id, -1, ctx.reader_session.page_count(), "", -1);
            if (ctx.refresh_jumplist) {
                ctx.refresh_jumplist();
            }
        }

        send_json(res, {
            {"session_id", ctx.reader_session.session_id()},
            {"path", utf8_path},
            {"format", format_to_string(ctx.reader_session.format())},
            {"page_count", ctx.reader_session.page_count()},
            {"file_size", ctx.reader_session.file_size()}
        });
    }));

    server.Get(R"(/api/reader/page/(\d+))", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        if (!ctx.reader_session.is_open()) {
            send_error(res, "No active reader session", 404);
            return;
        }

        int page_index = -1;
        if (!match_int(req, 1, page_index)) {
            send_error(res, "Invalid page index", 400);
            return;
        }

        int target_width = 1800;
        if (req.has_param("max_width")) {
            target_width = std::max(0, query_int(req, "max_width", 1800));
        } else if (req.has_param("width")) {
            target_width = std::max(0, query_int(req, "width", 1800));
        } else if (req.has_param("target_width")) {
            target_width = std::max(0, query_int(req, "target_width", 1800));
        }
        const bool auto_crop = query_bool(req, "auto_crop", false);

        auto rendered = ctx.reader_session.get_page(page_index, target_width, auto_crop);
        if (!rendered.valid()) {
            send_error(res, "Failed to render requested page", 404);
            return;
        }

        res.set_header("X-Page-Index", std::to_string(rendered.page_index));
        res.set_header("X-Page-Width", std::to_string(rendered.width));
        res.set_header("X-Page-Height", std::to_string(rendered.height));
        res.set_header("X-Original-Width", std::to_string(rendered.original_width));
        res.set_header("X-Original-Height", std::to_string(rendered.original_height));
        res.set_header("Cache-Control", "no-store");
        res.set_content(
            reinterpret_cast<const char*>(rendered.data.data()),
            rendered.data.size(),
            rendered.mime_type);
    }));

    server.Post("/api/reader/close", guarded([ctx](const httplib::Request&, httplib::Response& res) {
        ctx.reader_session.close();
        send_json(res, {{"closed", true}});
    }));

    // ---- Adjacent Volume Lookup (Natural Sort within Folder or Shelf) ----
    server.Get(R"(/api/comics/(\d+)/adjacent)", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        int64_t comic_id = 0;
        if (!match_int64(req, 1, comic_id)) {
            send_error(res, "Invalid comic id", 400);
            return;
        }
        ComicRecord target;
        if (!ctx.db.get_comic(comic_id, target)) {
            send_error(res, "Comic not found", 404);
            return;
        }

        const int64_t shelf_id = query_int64(req, "shelf_id", 0);
        auto lower_wide = [](std::wstring s) {
            for (wchar_t& c : s) {
                if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            }
            return s;
        };

        std::vector<ComicRecord> candidates;
        if (shelf_id > 0) {
            candidates = ctx.db.get_adjacent_candidates(shelf_id, "");
        } else {
            const std::filesystem::path target_parent =
                std::filesystem::path(utf8_to_wide(target.path)).parent_path().lexically_normal();
            const std::wstring target_parent_lower = lower_wide(target_parent.wstring());
            const std::string parent_utf8 = wide_to_utf8(target_parent.wstring());

            auto raw = ctx.db.get_adjacent_candidates(0, parent_utf8);
            candidates.reserve(raw.size());
            for (auto& c : raw) {
                const std::filesystem::path p =
                    std::filesystem::path(utf8_to_wide(c.path)).parent_path().lexically_normal();
                if (lower_wide(p.wstring()) == target_parent_lower) {
                    candidates.push_back(std::move(c));
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const ComicRecord& a, const ComicRecord& b) {
            if (natural_less(a.title, b.title)) return true;
            if (natural_less(b.title, a.title)) return false;
            if (natural_less(a.path, b.path)) return true;
            if (natural_less(b.path, a.path)) return false;
            return a.id < b.id;
        });

        int64_t prev_id = 0;
        int64_t next_id = 0;
        nlohmann::json prev_json = nullptr;
        nlohmann::json next_json = nullptr;

        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].id == comic_id) {
                if (i > 0) {
                    prev_id = candidates[i - 1].id;
                    prev_json = {
                        {"id", candidates[i - 1].id},
                        {"title", candidates[i - 1].title},
                        {"path", candidates[i - 1].path},
                        {"current_page", candidates[i - 1].current_page},
                        {"page_count", candidates[i - 1].page_count}
                    };
                }
                if (i + 1 < candidates.size()) {
                    next_id = candidates[i + 1].id;
                    next_json = {
                        {"id", candidates[i + 1].id},
                        {"title", candidates[i + 1].title},
                        {"path", candidates[i + 1].path},
                        {"current_page", candidates[i + 1].current_page},
                        {"page_count", candidates[i + 1].page_count}
                    };
                }
                break;
            }
        }

        send_json(res, {
            {"comic_id", comic_id},
            {"shelf_id", shelf_id},
            {"prev_id", prev_id > 0 ? nlohmann::json(prev_id) : nlohmann::json(nullptr)},
            {"next_id", next_id > 0 ? nlohmann::json(next_id) : nlohmann::json(nullptr)},
            {"prev", prev_json},
            {"next", next_json}
        });
    }));

    // ---- Per-User HKCU File Type Association (Zero UAC Required) ----
    server.Post("/api/system/register-file-types", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        bool include_pdf = false;
        if (!req.body.empty()) {
            try {
                auto body = nlohmann::json::parse(req.body);
                if (body.is_object() && body.contains("include_pdf") && body["include_pdf"].is_boolean()) {
                    include_pdf = body["include_pdf"].get<bool>();
                }
            } catch (...) {}
        }

        const std::wstring exe_w = ctx.paths.exe_path().wstring();
        const std::wstring prog_id = L"IncunaReader.Comic";
        const std::wstring prog_desc = L"Incuna Reader Comic Archive";
        const std::wstring icon_val = L"\"" + exe_w + L"\",0";
        const std::wstring cmd_val = L"\"" + exe_w + L"\" \"%1\"";

        auto set_hkcu_sz = [](const std::wstring& subkey, const wchar_t* value_name, const std::wstring& data) -> LONG {
            HKEY hkey = nullptr;
            LONG rc = RegCreateKeyExW(
                HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hkey, nullptr);
            if (rc != ERROR_SUCCESS) return rc;
            const DWORD bytes = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
            rc = RegSetValueExW(
                hkey, value_name, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(data.c_str()), bytes);
            RegCloseKey(hkey);
            return rc;
        };

        auto set_hkcu_none = [](const std::wstring& subkey, const wchar_t* value_name) -> LONG {
            HKEY hkey = nullptr;
            LONG rc = RegCreateKeyExW(
                HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hkey, nullptr);
            if (rc != ERROR_SUCCESS) return rc;
            rc = RegSetValueExW(hkey, value_name, 0, REG_NONE, nullptr, 0);
            RegCloseKey(hkey);
            return rc;
        };

        LONG rc = set_hkcu_sz(L"Software\\Classes\\" + prog_id, nullptr, prog_desc);
        if (rc == ERROR_SUCCESS) {
            rc = set_hkcu_sz(L"Software\\Classes\\" + prog_id + L"\\DefaultIcon", nullptr, icon_val);
        }
        if (rc == ERROR_SUCCESS) {
            rc = set_hkcu_sz(L"Software\\Classes\\" + prog_id + L"\\shell\\open\\command", nullptr, cmd_val);
        }
        if (rc != ERROR_SUCCESS) {
            send_error(res, "Failed to write HKCU ProgID registry keys", 500);
            return;
        }

        std::vector<std::wstring> exts = {L".cbz", L".cbr", L".cb7", L".cbt", L".epub"};
        if (include_pdf) {
            exts.push_back(L".pdf");
        }

        nlohmann::json registered_exts = nlohmann::json::array();
        for (const auto& ext : exts) {
            const std::wstring ext_key = L"Software\\Classes\\" + ext;
            if (set_hkcu_sz(ext_key, nullptr, prog_id) != ERROR_SUCCESS) {
                send_error(res, "Failed to register extension in HKCU", 500);
                return;
            }
            set_hkcu_none(ext_key + L"\\OpenWithProgids", prog_id.c_str());
            registered_exts.push_back(wide_to_utf8(ext));
        }

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

        send_json(res, {
            {"registered", true},
            {"exe_path", wide_to_utf8(exe_w)},
            {"extensions", registered_exts},
            {"include_pdf", include_pdf}
        });
    }));

    // ---- Win32 Keep Screen Active ----
    server.Post("/api/system/keep-awake", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_object(req, body, res)) return;
        if (!body.contains("active") || !body["active"].is_boolean()) {
            send_error(res, "Missing boolean 'active' field", 400);
            return;
        }
        const bool active = body["active"].get<bool>();
        bool applied = false;
        if (ctx.set_keep_awake) {
            applied = ctx.set_keep_awake(active);
        } else {
            const EXECUTION_STATE flags = active
                ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
                : ES_CONTINUOUS;
            applied = (SetThreadExecutionState(flags) != 0);
        }
        send_json(res, {{"active", active}, {"applied", applied}});
    }));

    // ---- Win32 True Borderless Fullscreen ----
    server.Post("/api/system/fullscreen", guarded([ctx](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json body;
        if (!parse_json_object(req, body, res)) return;
        if (!body.contains("active") || !body["active"].is_boolean()) {
            send_error(res, "Missing boolean 'active' field", 400);
            return;
        }
        const bool active = body["active"].get<bool>();
        const bool is_fs = ctx.set_fullscreen ? ctx.set_fullscreen(active) : false;
        send_json(res, {{"active", is_fs}});
    }));
}

} // namespace koma::api
