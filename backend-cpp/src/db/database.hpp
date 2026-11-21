#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace koma {

struct SmartViewRecord {
    int64_t id = 0;
    std::string slug;
    std::string name;
    std::string query_json;
    bool is_builtin = false;
    int sort_order = 0;
};

struct ComicRecord {
    int64_t id = 0;
    std::string path;
    std::string relative_path;
    std::string folder_path;
    std::string title;
    std::string format;
    int64_t file_size_bytes = 0;
    int page_count = 0;
    int current_page = 0;
    std::string status = "unread";
    bool is_favorite = false;
    bool is_offline_pinned = false;
    std::string offline_path;
    int64_t cover_pack_offset = -1;
    int64_t cover_pack_size = 0;
    int64_t added_at = 0;
    int64_t last_read_at = 0;
    bool is_hidden = false;
    std::string series;
    std::string author;
    std::string tags;
    std::string language;

    nlohmann::json to_json() const {
        return nlohmann::json{
            {"id", id},
            {"path", path},
            {"relative_path", relative_path},
            {"folder_path", folder_path},
            {"title", title},
            {"format", format},
            {"file_size_bytes", file_size_bytes},
            {"page_count", page_count},
            {"current_page", current_page},
            {"status", status},
            {"is_favorite", is_favorite},
            {"is_offline_pinned", is_offline_pinned},
            {"offline_path", offline_path},
            {"has_cover_pack", cover_pack_offset >= 0 && cover_pack_size > 0},
            {"added_at", added_at},
            {"last_read_at", last_read_at},
            {"is_hidden", is_hidden},
            {"series", series},
            {"author", author},
            {"tags", tags},
            {"language", language}
        };
    }
};

struct ComicUpsert {
    std::string path;
    std::string relative_path;
    std::string title;
    std::string format;
    int64_t file_size_bytes = 0;
    std::string folder_path;
    std::string series;
    std::string author;
    std::string tags;
    std::string language;
};

struct CoverPackUpdate {
    int64_t comic_id = 0;
    int64_t offset = -1;
    int64_t size = 0;
};

struct UnindexedCoverRow {
    int64_t id = 0;
    std::string path;
    std::string format;
    bool is_offline_pinned = false;
    std::string offline_path;
};

struct LibraryQuery {
    std::string search;
    int64_t shelf_id = 0;
    std::string smartview;
    int64_t smartview_id = 0;
    std::string folder_prefix;
    std::string author;
    std::string tag;
    std::string sort = "title_asc";
    int offset = 0;
    int limit = 120;
    bool include_hidden = false;
};

struct RandomComicQuery {
    int64_t shelf_id = 0;
    std::string smartview;
    int64_t smartview_id = 0;
    std::string author;
    std::string tag;
    bool include_hidden = false;
};

struct LibraryFolderEntry {
    std::string name;
    std::string path;
    int64_t comic_count = 0;
    int64_t preview_comic_id = 0;
};

struct LibraryPageResult {
    int64_t total_count = 0;
    std::vector<ComicRecord> items;
};

struct ShelfRecord {
    int64_t id = 0;
    std::string name;
    int sort_order = 0;
    int64_t comic_count = 0;
    int64_t created_at = 0;
};

struct BookmarkRecord {
    int64_t id = 0;
    int64_t comic_id = 0;
    int page_number = 0;
    std::string name;
    int64_t created_at = 0;
};

struct WatchedFolderRecord {
    int64_t id = 0;
    std::string path;
    std::string name;
    bool is_network = false;
    bool auto_sync = true;
    int64_t added_at = 0;
    int64_t last_scanned_at = 0;
};

class Database {
public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const std::filesystem::path& db_path);
    void close();
    bool is_open() const;

    std::string last_error() const;

    // Diagnostics
    int64_t count_comics();
    bool verify_fts5_available();

    // Comics & Library
    bool upsert_comics_batch(const std::vector<ComicUpsert>& batch);
    bool delete_comic_by_path(const std::string& path);
    bool rename_comic_path(const std::string& old_path,
                           const std::string& new_path,
                           const std::string& new_title);
    bool get_comic(int64_t id, ComicRecord& out);
    bool get_comic_by_path(const std::string& path, ComicRecord& out);
    std::vector<ComicRecord> get_adjacent_candidates(int64_t shelf_id, const std::string& path_prefix);
    std::vector<ComicRecord> get_recent_comics(int limit = 8);
    std::vector<ComicRecord> get_continue_reading(int limit = 10, bool include_hidden = false);
    bool get_random_unread_comic(const RandomComicQuery& q, ComicRecord& out);
    std::vector<ComicRecord> get_comics_needing_cover_pack();
    std::vector<UnindexedCoverRow> get_unindexed_covers_chunk(int limit = 1000, const std::string& after_path = "");
    int64_t count_unindexed_covers();
    LibraryPageResult query_library(const LibraryQuery& q);
    std::vector<LibraryFolderEntry> get_library_folders(const std::string& prefix = "", bool include_hidden = false);
    int64_t batch_update_status(const std::vector<int64_t>& ids, const std::string& status);
    int64_t batch_update_favorite(const std::vector<int64_t>& ids, bool is_favorite);
    int64_t batch_update_hidden(const std::vector<int64_t>& ids, bool is_hidden);
    bool set_comic_hidden(int64_t id, bool is_hidden);
    int64_t batch_assign_shelf(int64_t shelf_id, const std::vector<int64_t>& ids);
    int64_t batch_remove_shelf(int64_t shelf_id, const std::vector<int64_t>& ids);
    bool update_comic_progress(int64_t id,
                               int current_page,
                               int page_count,
                               const std::string& status,
                               int is_favorite_opt /* -1 = keep */);
    bool update_comic_cover_pack(int64_t id, int64_t offset, int64_t size);
    bool batch_update_comic_cover_pack(const std::vector<CoverPackUpdate>& updates);
    bool reset_all_cover_pack_offsets();
    bool update_comic_offline_pin(int64_t id, bool pinned, const std::string& offline_path);
    int64_t count_offline_pinned_comics();
    std::vector<std::string> get_pinned_offline_paths();
    std::vector<std::pair<int64_t, std::string>> get_unpinned_comics_under_prefix(const std::string& folder_prefix);
    int64_t delete_comics_by_ids_if_unpinned(const std::vector<int64_t>& ids);
    int64_t remap_drive_prefix(const std::string& old_prefix, const std::string& new_prefix);

    // Shelves
    std::vector<ShelfRecord> get_shelves();
    bool create_shelf(const std::string& name, ShelfRecord& out);
    bool rename_shelf(int64_t id, const std::string& name);
    bool delete_shelf(int64_t id);
    bool add_comic_to_shelf(int64_t shelf_id, int64_t comic_id);
    bool remove_comic_from_shelf(int64_t shelf_id, int64_t comic_id);
    std::vector<int64_t> get_comic_ids_in_shelf(int64_t shelf_id);

    // Bookmarks
    std::vector<BookmarkRecord> get_bookmarks(int64_t comic_id);
    bool add_bookmark(int64_t comic_id, int page_number, const std::string& name, BookmarkRecord& out);
    bool delete_bookmark(int64_t bookmark_id);

    // Smartviews
    std::vector<SmartViewRecord> get_smartviews();
    bool get_smartview_by_slug(const std::string& slug, SmartViewRecord& out);
    bool create_smartview(const std::string& slug,
                          const std::string& name,
                          const std::string& query_json,
                          SmartViewRecord& out);
    bool delete_smartview(int64_t id);

    // Watched Folders
    bool upsert_watched_folder(const std::string& path,
                               const std::string& name,
                               bool is_network,
                               WatchedFolderRecord& out);
    std::vector<WatchedFolderRecord> get_watched_folders();
    bool update_watched_folder_scanned(int64_t id);
    bool delete_watched_folder(int64_t id);

private:
    bool exec_sql(const char* sql);
    bool configure_pragmas();
    bool init_schema();
    bool seed_builtin_smartviews();

    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
    std::string last_error_;
};

class SettingsManager {
public:
    explicit SettingsManager(std::filesystem::path settings_path);

    bool load_or_create();
    nlohmann::json get() const;
    nlohmann::json update(const nlohmann::json& patch);

    static nlohmann::json default_settings();

private:
    bool save_locked() const;

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    nlohmann::json settings_;
};

} // namespace koma
