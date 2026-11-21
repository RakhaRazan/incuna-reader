#include "db/database.hpp"
#include "utils/string_utils.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace koma {

namespace {

constexpr const char* kComicSelectCols =
    "id, path, relative_path, title, format, file_size_bytes, page_count, "
    "current_page, status, is_favorite, is_offline_pinned, offline_path, "
    "cover_pack_offset, cover_pack_size, added_at, last_read_at, folder_path, COALESCE(is_hidden, 0), "
    "COALESCE(series, ''), COALESCE(author, ''), COALESCE(tags, ''), COALESCE(language, '')";

constexpr const char* kComicSelectColsAliased =
    "c.id, c.path, c.relative_path, c.title, c.format, c.file_size_bytes, c.page_count, "
    "c.current_page, c.status, c.is_favorite, c.is_offline_pinned, c.offline_path, "
    "c.cover_pack_offset, c.cover_pack_size, c.added_at, c.last_read_at, c.folder_path, COALESCE(c.is_hidden, 0), "
    "COALESCE(c.series, ''), COALESCE(c.author, ''), COALESCE(c.tags, ''), COALESCE(c.language, '')";

struct SqlBindVal {
    bool is_int = false;
    int64_t i = 0;
    std::string s;
};

void bind_sql_params(sqlite3_stmt* stmt, const std::vector<SqlBindVal>& binds) {
    for (size_t i = 0; i < binds.size(); ++i) {
        const int idx = static_cast<int>(i + 1);
        if (binds[i].is_int) {
            sqlite3_bind_int64(stmt, idx, binds[i].i);
        } else {
            sqlite3_bind_text(stmt, idx, binds[i].s.c_str(), -1, SQLITE_TRANSIENT);
        }
    }
}

std::string col_text(sqlite3_stmt* stmt, int idx) {
    const unsigned char* p = sqlite3_column_text(stmt, idx);
    return p ? reinterpret_cast<const char*>(p) : "";
}

nlohmann::json load_smartview_filter(sqlite3* db, int64_t smartview_id, const std::string& slug) {
    nlohmann::json sv_filter = nlohmann::json::object();
    sqlite3_stmt* sv_stmt = nullptr;
    if (smartview_id > 0) {
        if (sqlite3_prepare_v2(db, "SELECT query_json FROM smartviews WHERE id = ?1;", -1, &sv_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(sv_stmt, 1, smartview_id);
            if (sqlite3_step(sv_stmt) == SQLITE_ROW) {
                try { sv_filter = nlohmann::json::parse(col_text(sv_stmt, 0)); } catch (...) {}
            }
            sqlite3_finalize(sv_stmt);
        }
    } else if (!slug.empty()) {
        if (sqlite3_prepare_v2(db, "SELECT query_json FROM smartviews WHERE slug = ?1;", -1, &sv_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(sv_stmt, 1, slug.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(sv_stmt) == SQLITE_ROW) {
                try { sv_filter = nlohmann::json::parse(col_text(sv_stmt, 0)); } catch (...) {}
            }
            sqlite3_finalize(sv_stmt);
        }
    }
    return sv_filter;
}

void append_smartview_filters(const nlohmann::json& sv_filter,
                              bool include_status,
                              std::vector<std::string>& conds,
                              std::vector<SqlBindVal>& binds) {
    if (!sv_filter.is_object()) return;

    if (include_status && sv_filter.contains("status") && sv_filter["status"].is_string()) {
        const std::string st = sv_filter["status"].get<std::string>();
        if (!st.empty() && st != "ALL") {
            conds.push_back("c.status = ?");
            binds.push_back({false, 0, st});
        }
    }
    if (sv_filter.contains("is_favorite") && sv_filter["is_favorite"].is_boolean()) {
        conds.push_back("c.is_favorite = ?");
        binds.push_back({true, sv_filter["is_favorite"].get<bool>() ? 1 : 0, {}});
    } else if (sv_filter.contains("only_favorites") && sv_filter["only_favorites"].is_boolean() &&
               sv_filter["only_favorites"].get<bool>()) {
        conds.push_back("c.is_favorite = 1");
    }
    if (sv_filter.contains("is_offline_pinned") && sv_filter["is_offline_pinned"].is_boolean()) {
        conds.push_back("c.is_offline_pinned = ?");
        binds.push_back({true, sv_filter["is_offline_pinned"].get<bool>() ? 1 : 0, {}});
    } else if (sv_filter.contains("only_offline") && sv_filter["only_offline"].is_boolean() &&
               sv_filter["only_offline"].get<bool>()) {
        conds.push_back("c.is_offline_pinned = 1");
    }
    if (sv_filter.contains("min_file_size_bytes") && sv_filter["min_file_size_bytes"].is_number_integer()) {
        conds.push_back("c.file_size_bytes >= ?");
        binds.push_back({true, sv_filter["min_file_size_bytes"].get<int64_t>(), {}});
    } else if (sv_filter.contains("min_size_mb") && sv_filter["min_size_mb"].is_number()) {
        const int64_t mb = sv_filter["min_size_mb"].get<int64_t>();
        if (mb > 0) {
            conds.push_back("c.file_size_bytes >= ?");
            binds.push_back({true, mb * 1024LL * 1024LL, {}});
        }
    }
    if (sv_filter.contains("format") && sv_filter["format"].is_string()) {
        std::string fmt = to_lower_ascii(sv_filter["format"].get<std::string>());
        if (!fmt.empty() && fmt != "all") {
            if (fmt == "7z") fmt = "cb7";
            conds.push_back("c.format = ?");
            binds.push_back({false, 0, fmt});
        }
    }
    if (sv_filter.contains("path_contains") && sv_filter["path_contains"].is_string()) {
        const std::string needle = sv_filter["path_contains"].get<std::string>();
        if (!needle.empty()) {
            const std::string pat = "%" + escape_sql_like(needle) + "%";
            conds.push_back("(c.path LIKE ? ESCAPE '!' OR c.relative_path LIKE ? ESCAPE '!')");
            binds.push_back({false, 0, pat});
            binds.push_back({false, 0, pat});
        }
    }
}

ComicRecord read_comic_row(sqlite3_stmt* stmt) {
    ComicRecord c;
    c.id                = sqlite3_column_int64(stmt, 0);
    c.path              = col_text(stmt, 1);
    c.relative_path     = col_text(stmt, 2);
    c.title             = col_text(stmt, 3);
    c.format            = col_text(stmt, 4);
    c.file_size_bytes   = sqlite3_column_int64(stmt, 5);
    c.page_count        = sqlite3_column_int(stmt, 6);
    c.current_page      = sqlite3_column_int(stmt, 7);
    c.status            = col_text(stmt, 8);
    c.is_favorite       = sqlite3_column_int(stmt, 9) != 0;
    c.is_offline_pinned = sqlite3_column_int(stmt, 10) != 0;
    c.offline_path      = col_text(stmt, 11);
    c.cover_pack_offset = sqlite3_column_int64(stmt, 12);
    c.cover_pack_size   = sqlite3_column_int64(stmt, 13);
    c.added_at          = sqlite3_column_int64(stmt, 14);
    c.last_read_at      = sqlite3_column_int64(stmt, 15);
    c.folder_path       = col_text(stmt, 16);
    c.is_hidden         = sqlite3_column_int(stmt, 17) != 0;
    c.series            = col_text(stmt, 18);
    c.author            = col_text(stmt, 19);
    c.tags              = col_text(stmt, 20);
    c.language          = col_text(stmt, 21);
    return c;
}

std::string derive_folder_path(const std::string& utf8_path) {
    if (utf8_path.empty()) return {};
    const std::filesystem::path p(utf8_to_wide(utf8_path));
    return wide_to_utf8(p.parent_path().wstring());
}

// Converts raw user search text into a safe FTS5 prefix MATCH expression.
// E.g. "one piece 10" -> "\"one\"* \"piece\"* \"10\"*"
std::string build_safe_fts5_query(const std::string& raw) {
    std::string result;
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        if (!result.empty()) result += ' ';
        result += '"';
        for (char ch : token) {
            if (ch == '"') result += "\"\"";
            else result += ch;
        }
        result += "\"*";
        token.clear();
    };

    for (unsigned char c : raw) {
        if (std::isspace(c) || c == '*' || c == '^' || c == '(' || c == ')' || c == ':') {
            flush();
        } else {
            token.push_back(static_cast<char>(c));
        }
    }
    flush();
    return result;
}

} // namespace

// ============================================================================
// Database
// ============================================================================

Database::~Database() {
    close();
}

bool Database::open(const std::filesystem::path& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }

    std::error_code ec;
    if (db_path.has_parent_path()) {
        std::filesystem::create_directories(db_path.parent_path(), ec);
    }

    const std::string utf8_path = wide_to_utf8(db_path.wstring());
    int rc = sqlite3_open_v2(
        utf8_path.c_str(),
        &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = db_ ? sqlite3_errmsg(db_) : "Failed to allocate sqlite3 handle";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout(db_, 5000);

    if (!configure_pragmas()) return false;
    if (!init_schema()) return false;
    if (!seed_builtin_smartviews()) return false;

    return true;
}

void Database::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_ != nullptr;
}

std::string Database::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

bool Database::exec_sql(const char* sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        last_error_ = err_msg ? err_msg : sqlite3_errmsg(db_);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool Database::configure_pragmas() {
    static constexpr const char* kPragmas = R"SQL(
        PRAGMA journal_mode = WAL;
        PRAGMA synchronous = NORMAL;
        PRAGMA temp_store = MEMORY;
        PRAGMA foreign_keys = ON;
    )SQL";
    return exec_sql(kPragmas);
}

bool Database::init_schema() {
    static constexpr const char* kSchemaSql = R"SQL(
        CREATE TABLE IF NOT EXISTS comics (
            id                INTEGER PRIMARY KEY AUTOINCREMENT,
            path              TEXT    NOT NULL UNIQUE,
            relative_path     TEXT    NOT NULL DEFAULT '',
            folder_path       TEXT    NOT NULL DEFAULT '',
            title             TEXT    NOT NULL,
            format            TEXT    NOT NULL DEFAULT '',
            file_size_bytes   INTEGER NOT NULL DEFAULT 0,
            page_count        INTEGER NOT NULL DEFAULT 0,
            current_page      INTEGER NOT NULL DEFAULT 0,
            status            TEXT    NOT NULL DEFAULT 'unread' CHECK(status IN ('unread', 'reading', 'read')),
            is_favorite       INTEGER NOT NULL DEFAULT 0,
            is_offline_pinned INTEGER NOT NULL DEFAULT 0,
            offline_path      TEXT    NOT NULL DEFAULT '',
            cover_pack_offset INTEGER NOT NULL DEFAULT -1,
            cover_pack_size   INTEGER NOT NULL DEFAULT 0,
            added_at          INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
            last_read_at      INTEGER NOT NULL DEFAULT 0,
            is_hidden         INTEGER NOT NULL DEFAULT 0,
            series            TEXT    NOT NULL DEFAULT '',
            author            TEXT    NOT NULL DEFAULT '',
            tags              TEXT    NOT NULL DEFAULT '',
            language          TEXT    NOT NULL DEFAULT ''
        );

        CREATE INDEX IF NOT EXISTS idx_comics_status          ON comics(status);
        CREATE INDEX IF NOT EXISTS idx_comics_favorite        ON comics(is_favorite);
        CREATE INDEX IF NOT EXISTS idx_comics_offline_pinned  ON comics(is_offline_pinned);
        CREATE INDEX IF NOT EXISTS idx_comics_file_size       ON comics(file_size_bytes);
        CREATE INDEX IF NOT EXISTS idx_comics_last_read       ON comics(last_read_at DESC);

        CREATE VIRTUAL TABLE IF NOT EXISTS comics_fts USING fts5(
            title,
            path,
            relative_path,
            format,
            series,
            author,
            tags,
            content='comics',
            content_rowid='id',
            tokenize='unicode61'
        );

        CREATE TRIGGER IF NOT EXISTS comics_ai AFTER INSERT ON comics BEGIN
            INSERT INTO comics_fts(rowid, title, path, relative_path, format, series, author, tags)
            VALUES (new.id, new.title, new.path, new.relative_path, new.format, new.series, new.author, new.tags);
        END;

        CREATE TRIGGER IF NOT EXISTS comics_ad AFTER DELETE ON comics BEGIN
            INSERT INTO comics_fts(comics_fts, rowid, title, path, relative_path, format, series, author, tags)
            VALUES ('delete', old.id, old.title, old.path, old.relative_path, old.format, old.series, old.author, old.tags);
        END;

        CREATE TRIGGER IF NOT EXISTS comics_au AFTER UPDATE OF title, path, relative_path, format, series, author, tags ON comics BEGIN
            INSERT INTO comics_fts(comics_fts, rowid, title, path, relative_path, format, series, author, tags)
            VALUES ('delete', old.id, old.title, old.path, old.relative_path, old.format, old.series, old.author, old.tags);
            INSERT INTO comics_fts(rowid, title, path, relative_path, format, series, author, tags)
            VALUES (new.id, new.title, new.path, new.relative_path, new.format, new.series, new.author, new.tags);
        END;

        CREATE TABLE IF NOT EXISTS shelves (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            name       TEXT    NOT NULL UNIQUE,
            sort_order INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
        );

        CREATE TABLE IF NOT EXISTS comic_shelves (
            comic_id INTEGER NOT NULL REFERENCES comics(id) ON DELETE CASCADE,
            shelf_id INTEGER NOT NULL REFERENCES shelves(id) ON DELETE CASCADE,
            added_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
            PRIMARY KEY (comic_id, shelf_id)
        );

        CREATE INDEX IF NOT EXISTS idx_comic_shelves_shelf ON comic_shelves(shelf_id);

        CREATE TABLE IF NOT EXISTS bookmarks (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            comic_id    INTEGER NOT NULL REFERENCES comics(id) ON DELETE CASCADE,
            page_number INTEGER NOT NULL,
            name        TEXT    NOT NULL DEFAULT '',
            created_at  INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
        );

        CREATE INDEX IF NOT EXISTS idx_bookmarks_comic ON bookmarks(comic_id, page_number);

        CREATE TABLE IF NOT EXISTS watched_folders (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            path            TEXT    NOT NULL UNIQUE,
            name            TEXT    NOT NULL DEFAULT '',
            is_network      INTEGER NOT NULL DEFAULT 0,
            auto_sync       INTEGER NOT NULL DEFAULT 1,
            added_at        INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
            last_scanned_at INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS smartviews (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            slug       TEXT    NOT NULL UNIQUE,
            name       TEXT    NOT NULL,
            query_json TEXT    NOT NULL,
            is_builtin INTEGER NOT NULL DEFAULT 0,
            sort_order INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
        );
    )SQL";

    if (!exec_sql(kSchemaSql)) {
        return false;
    }

    // Migrate existing databases that predate the folder_path column
    bool has_folder_path = false;
    {
        sqlite3_stmt* info_stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA table_info(comics);", -1, &info_stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(info_stmt) == SQLITE_ROW) {
                if (col_text(info_stmt, 1) == "folder_path") {
                    has_folder_path = true;
                    break;
                }
            }
            sqlite3_finalize(info_stmt);
        }
    }
    if (!has_folder_path) {
        if (!exec_sql("ALTER TABLE comics ADD COLUMN folder_path TEXT NOT NULL DEFAULT '';")) {
            return false;
        }
    }

    if (!exec_sql("CREATE INDEX IF NOT EXISTS idx_comics_folder_path ON comics(folder_path);")) {
        return false;
    }

    // Safely migrate existing databases to add columns (ignoring duplicate-column errors)
    auto safe_add_column = [&](const char* alter_sql) -> bool {
        char* alter_err = nullptr;
        int alter_rc = sqlite3_exec(db_, alter_sql, nullptr, nullptr, &alter_err);
        if (alter_rc != SQLITE_OK) {
            std::string msg = alter_err ? alter_err : "";
            if (alter_err) sqlite3_free(alter_err);
            if (msg.find("duplicate column") == std::string::npos) {
                last_error_ = msg;
                return false;
            }
        }
        return true;
    };
    if (!safe_add_column("ALTER TABLE comics ADD COLUMN is_hidden INTEGER DEFAULT 0;")) return false;
    if (!exec_sql("CREATE INDEX IF NOT EXISTS idx_comics_hidden ON comics(is_hidden);")) return false;
    if (!safe_add_column("ALTER TABLE comics ADD COLUMN series TEXT DEFAULT '';")) return false;
    if (!safe_add_column("ALTER TABLE comics ADD COLUMN author TEXT DEFAULT '';")) return false;
    if (!safe_add_column("ALTER TABLE comics ADD COLUMN tags TEXT DEFAULT '';")) return false;
    if (!safe_add_column("ALTER TABLE comics ADD COLUMN language TEXT DEFAULT '';")) return false;

    if (!exec_sql("CREATE INDEX IF NOT EXISTS idx_comics_author ON comics(author);")) return false;
    if (!exec_sql("CREATE INDEX IF NOT EXISTS idx_comics_author_nocase ON comics(author COLLATE NOCASE);")) return false;
    if (!exec_sql("CREATE INDEX IF NOT EXISTS idx_comics_series ON comics(series);")) return false;

    // Upgrade legacy comics_fts virtual table if it predates author/tags indexing
    bool fts_has_author = false;
    bool fts_has_tags = false;
    {
        sqlite3_stmt* fts_info = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA table_info(comics_fts);", -1, &fts_info, nullptr) == SQLITE_OK) {
            while (sqlite3_step(fts_info) == SQLITE_ROW) {
                const std::string cname = col_text(fts_info, 1);
                if (cname == "author") fts_has_author = true;
                else if (cname == "tags") fts_has_tags = true;
            }
            sqlite3_finalize(fts_info);
        }
    }
    if (!fts_has_author || !fts_has_tags) {
        static constexpr const char* kRebuildFtsSql = R"SQL(
            DROP TRIGGER IF EXISTS comics_ai;
            DROP TRIGGER IF EXISTS comics_ad;
            DROP TRIGGER IF EXISTS comics_au;
            DROP TABLE IF EXISTS comics_fts;

            CREATE VIRTUAL TABLE comics_fts USING fts5(
                title,
                path,
                relative_path,
                format,
                series,
                author,
                tags,
                content='comics',
                content_rowid='id',
                tokenize='unicode61'
            );

            CREATE TRIGGER comics_ai AFTER INSERT ON comics BEGIN
                INSERT INTO comics_fts(rowid, title, path, relative_path, format, series, author, tags)
                VALUES (new.id, new.title, new.path, new.relative_path, new.format, new.series, new.author, new.tags);
            END;

            CREATE TRIGGER comics_ad AFTER DELETE ON comics BEGIN
                INSERT INTO comics_fts(comics_fts, rowid, title, path, relative_path, format, series, author, tags)
                VALUES ('delete', old.id, old.title, old.path, old.relative_path, old.format, old.series, old.author, old.tags);
            END;

            CREATE TRIGGER comics_au AFTER UPDATE OF title, path, relative_path, format, series, author, tags ON comics BEGIN
                INSERT INTO comics_fts(comics_fts, rowid, title, path, relative_path, format, series, author, tags)
                VALUES ('delete', old.id, old.title, old.path, old.relative_path, old.format, old.series, old.author, old.tags);
                INSERT INTO comics_fts(rowid, title, path, relative_path, format, series, author, tags)
                VALUES (new.id, new.title, new.path, new.relative_path, new.format, new.series, new.author, new.tags);
            END;

            INSERT INTO comics_fts(comics_fts) VALUES('rebuild');
        )SQL";
        if (!exec_sql(kRebuildFtsSql)) {
            return false;
        }
    }

    // Backfill folder_path for any legacy rows once inside a single transaction
    std::vector<std::pair<int64_t, std::string>> to_backfill;
    {
        sqlite3_stmt* sel = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id, path FROM comics WHERE folder_path = '';", -1, &sel, nullptr) == SQLITE_OK) {
            while (sqlite3_step(sel) == SQLITE_ROW) {
                const int64_t id = sqlite3_column_int64(sel, 0);
                const std::string p = col_text(sel, 1);
                std::string fp = derive_folder_path(p);
                if (!fp.empty()) {
                    to_backfill.emplace_back(id, std::move(fp));
                }
            }
            sqlite3_finalize(sel);
        }
    }
    if (!to_backfill.empty() && exec_sql("BEGIN IMMEDIATE;")) {
        sqlite3_stmt* upd = nullptr;
        bool ok = false;
        if (sqlite3_prepare_v2(db_, "UPDATE comics SET folder_path = ?1 WHERE id = ?2;", -1, &upd, nullptr) == SQLITE_OK) {
            ok = true;
            for (const auto& [id, fp] : to_backfill) {
                sqlite3_reset(upd);
                sqlite3_clear_bindings(upd);
                sqlite3_bind_text(upd, 1, fp.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(upd, 2, id);
                if (sqlite3_step(upd) != SQLITE_DONE) {
                    ok = false;
                    break;
                }
            }
            sqlite3_finalize(upd);
        }
        exec_sql(ok ? "COMMIT;" : "ROLLBACK;");
    }

    return true;
}

bool Database::seed_builtin_smartviews() {
    static constexpr const char* kSeedSql = R"SQL(
        INSERT OR IGNORE INTO smartviews (slug, name, query_json, is_builtin, sort_order) VALUES
            ('unread',         'Unread',            '{"status":"unread"}',                  1, 1),
            ('reading',        'Reading',           '{"status":"reading"}',                 1, 2),
            ('completed',      'Completed',         '{"status":"read"}',                    1, 3),
            ('favorites',      'Favorites',         '{"is_favorite":true}',                 1, 4),
            ('large_files',    'Large Files >1GB',  '{"min_file_size_bytes":1073741824}',   1, 5),
            ('offline_pinned', 'Offline Pinned',    '{"is_offline_pinned":true}',           1, 6);
    )SQL";
    return exec_sql(kSeedSql);
}

int64_t Database::count_comics() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    sqlite3_stmt* stmt = nullptr;
    int64_t total = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM comics;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return total;
}

bool Database::verify_fts5_available() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db_, "SELECT count(*) FROM comics_fts;", -1, &stmt, nullptr) == SQLITE_OK) {
        ok = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }
    return ok;
}

// ---- Comics & Library ----

bool Database::upsert_comics_batch(const std::vector<ComicUpsert>& batch) {
    if (batch.empty()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    if (!exec_sql("BEGIN IMMEDIATE;")) return false;

    static constexpr const char* kSql = R"SQL(
        INSERT INTO comics (path, relative_path, folder_path, title, format, file_size_bytes, series, author, tags, language)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)
        ON CONFLICT(path) DO UPDATE SET
            relative_path   = excluded.relative_path,
            folder_path     = excluded.folder_path,
            title           = excluded.title,
            format          = excluded.format,
            file_size_bytes = excluded.file_size_bytes,
            series          = CASE WHEN excluded.series != '' THEN excluded.series ELSE comics.series END,
            author          = CASE WHEN excluded.author != '' THEN excluded.author ELSE comics.author END,
            tags            = CASE WHEN excluded.tags != '' THEN excluded.tags ELSE comics.tags END,
            language        = CASE WHEN excluded.language != '' THEN excluded.language ELSE comics.language END;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        exec_sql("ROLLBACK;");
        return false;
    }

    bool ok = true;
    for (const auto& item : batch) {
        const std::string folder_path = item.folder_path.empty()
            ? derive_folder_path(item.path)
            : item.folder_path;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, item.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, item.relative_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, folder_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, item.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, item.format.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 6, item.file_size_bytes);
        sqlite3_bind_text(stmt, 7, item.series.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, item.author.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, item.tags.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, item.language.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            last_error_ = sqlite3_errmsg(db_);
            ok = false;
            break;
        }
    }

    sqlite3_finalize(stmt);
    if (ok) {
        return exec_sql("COMMIT;");
    }
    exec_sql("ROLLBACK;");
    return false;
}

bool Database::delete_comic_by_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "DELETE FROM comics WHERE path = ?1 AND COALESCE(is_offline_pinned, 0) = 0;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::rename_comic_path(const std::string& old_path,
                                 const std::string& new_path,
                                 const std::string& new_title) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const std::string new_folder_path = derive_folder_path(new_path);
    static constexpr const char* kSql =
        "UPDATE comics SET path = ?1, folder_path = ?2, title = ?3 WHERE path = ?4;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, new_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, new_folder_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, new_title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, old_path.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::get_comic(int64_t id, ComicRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const std::string sql = std::string("SELECT ") + kComicSelectCols + " FROM comics WHERE id = ?1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, id);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = read_comic_row(stmt);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

std::vector<ComicRecord> Database::get_adjacent_candidates(int64_t shelf_id, const std::string& path_prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ComicRecord> results;
    if (!db_) return results;

    if (shelf_id > 0) {
        const std::string shelf_sql =
            std::string("SELECT ") + kComicSelectColsAliased +
            " FROM comics c JOIN comic_shelves cs ON cs.comic_id = c.id WHERE cs.shelf_id = ?1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, shelf_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return results;
        sqlite3_bind_int64(stmt, 1, shelf_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(read_comic_row(stmt));
        }
        sqlite3_finalize(stmt);
        return results;
    }

    if (path_prefix.empty()) return results;

    const std::string pat = escape_sql_like(path_prefix) + "%";
    const std::string folder_sql =
        std::string("SELECT ") + kComicSelectCols + " FROM comics WHERE path LIKE ?1 ESCAPE '!';";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, folder_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return results;
    sqlite3_bind_text(stmt, 1, pat.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(read_comic_row(stmt));
    }
    sqlite3_finalize(stmt);
    return results;
}

bool Database::get_comic_by_path(const std::string& path, ComicRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const std::string sql = std::string("SELECT ") + kComicSelectCols + " FROM comics WHERE path = ?1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = read_comic_row(stmt);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

std::vector<ComicRecord> Database::get_recent_comics(int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ComicRecord> results;
    if (!db_ || limit <= 0) return results;

    const std::string sql =
        std::string("SELECT ") + kComicSelectCols +
        " FROM comics WHERE last_read_at > 0 ORDER BY last_read_at DESC, id DESC LIMIT ?1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return results;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(read_comic_row(stmt));
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<ComicRecord> Database::get_continue_reading(int limit, bool include_hidden) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ComicRecord> results;
    if (!db_ || limit <= 0) return results;

    const std::string sql =
        std::string("SELECT ") + kComicSelectCols +
        " FROM comics WHERE last_read_at > 0 AND status IN ('reading', 'read') "
        "AND (?2 = 1 OR COALESCE(is_hidden, 0) = 0) "
        "ORDER BY last_read_at DESC, id DESC LIMIT ?1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return results;
    sqlite3_bind_int(stmt, 1, std::clamp(limit, 1, 100));
    sqlite3_bind_int(stmt, 2, include_hidden ? 1 : 0);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(read_comic_row(stmt));
    }
    sqlite3_finalize(stmt);
    return results;
}

bool Database::get_random_unread_comic(const RandomComicQuery& q, ComicRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const nlohmann::json sv_filter = load_smartview_filter(db_, q.smartview_id, q.smartview);

    std::ostringstream from_where;
    from_where << " FROM comics c ";
    if (q.shelf_id > 0) {
        from_where << " JOIN comic_shelves cs ON cs.comic_id = c.id ";
    }

    std::vector<std::string> conds;
    std::vector<SqlBindVal> binds;

    conds.push_back("c.status = 'unread'");
    if (!q.include_hidden) {
        conds.push_back("COALESCE(c.is_hidden, 0) = 0");
    }
    if (q.shelf_id > 0) {
        conds.push_back("cs.shelf_id = ?");
        binds.push_back({true, q.shelf_id, {}});
    }
    if (!q.author.empty()) {
        conds.push_back("c.author = ? COLLATE NOCASE");
        binds.push_back({false, 0, q.author});
    }
    if (!q.tag.empty()) {
        conds.push_back("c.tags LIKE ? ESCAPE '!'");
        binds.push_back({false, 0, "%" + escape_sql_like(q.tag) + "%"});
    }
    append_smartview_filters(sv_filter, /*include_status=*/false, conds, binds);

    from_where << " WHERE ";
    for (size_t i = 0; i < conds.size(); ++i) {
        if (i > 0) from_where << " AND ";
        from_where << conds[i];
    }

    // 1. Fast indexed COUNT(*)
    int64_t total = 0;
    {
        const std::string count_sql = "SELECT COUNT(*)" + from_where.str() + ";";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, count_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
        bind_sql_params(stmt, binds);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    if (total <= 0) return false;

    // 2. Pick uniform random offset and fetch ID via LIMIT 1 OFFSET ? (<2ms)
    thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<int64_t> dist(0, total - 1);
    const int64_t offset = dist(rng);

    int64_t picked_id = 0;
    {
        const std::string pick_sql = "SELECT c.id" + from_where.str() + " LIMIT 1 OFFSET ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, pick_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
        bind_sql_params(stmt, binds);
        sqlite3_bind_int64(stmt, static_cast<int>(binds.size() + 1), offset);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            picked_id = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    if (picked_id <= 0) return false;

    // 3. Primary-key lookup for full ComicRecord
    const std::string row_sql = std::string("SELECT ") + kComicSelectCols + " FROM comics WHERE id = ?1;";
    sqlite3_stmt* row_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, row_sql.c_str(), -1, &row_stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(row_stmt, 1, picked_id);
    bool found = false;
    if (sqlite3_step(row_stmt) == SQLITE_ROW) {
        out = read_comic_row(row_stmt);
        found = true;
    }
    sqlite3_finalize(row_stmt);
    return found;
}

std::vector<UnindexedCoverRow> Database::get_unindexed_covers_chunk(int limit, const std::string& after_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UnindexedCoverRow> results;
    if (!db_ || limit <= 0) return results;

    static constexpr const char* kSql =
        "SELECT id, path, format, is_offline_pinned, offline_path "
        "FROM comics "
        "WHERE cover_pack_size = 0 AND (?1 = '' OR path > ?1) "
        "ORDER BY path ASC LIMIT ?2;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return results;
    sqlite3_bind_text(stmt, 1, after_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    results.reserve(static_cast<size_t>(std::min(limit, 1000)));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        UnindexedCoverRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.path = col_text(stmt, 1);
        row.format = col_text(stmt, 2);
        row.is_offline_pinned = sqlite3_column_int(stmt, 3) != 0;
        row.offline_path = col_text(stmt, 4);
        results.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return results;
}

int64_t Database::count_unindexed_covers() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    sqlite3_stmt* stmt = nullptr;
    int64_t total = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM comics WHERE cover_pack_size = 0;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return total;
}

LibraryPageResult Database::query_library(const LibraryQuery& q) {
    std::lock_guard<std::mutex> lock(mutex_);
    LibraryPageResult res;
    if (!db_) return res;

    // Resolve optional smartview JSON filter (by smartview_id or slug)
    const nlohmann::json sv_filter = load_smartview_filter(db_, q.smartview_id, q.smartview);

    std::string fts_expr = build_safe_fts5_query(q.search);

    std::ostringstream from_where;
    from_where << " FROM comics c ";
    if (!fts_expr.empty()) {
        from_where << " JOIN comics_fts f ON f.rowid = c.id ";
    }
    if (q.shelf_id > 0) {
        from_where << " JOIN comic_shelves cs ON cs.comic_id = c.id ";
    }

    std::vector<std::string> conds;
    std::vector<SqlBindVal> binds;

    if (!q.include_hidden) {
        conds.push_back("COALESCE(c.is_hidden, 0) = 0");
    }
    if (!fts_expr.empty()) {
        conds.push_back("comics_fts MATCH ?");
        binds.push_back({false, 0, fts_expr});
    }
    if (!q.folder_prefix.empty()) {
        std::string norm = q.folder_prefix;
        std::replace(norm.begin(), norm.end(), '\\', '/');
        while (norm.size() > 1 && norm.back() == '/') norm.pop_back();
        const bool is_unc = norm.size() >= 2 && norm[0] == '/' && norm[1] == '/';
        if (!is_unc) {
            while (!norm.empty() && norm.front() == '/') norm.erase(norm.begin());
        }
        if (!norm.empty()) {
            std::string bnorm = norm;
            std::replace(bnorm.begin(), bnorm.end(), '/', '\\');
            const bool is_abs = is_unc || (bnorm.size() >= 2 && bnorm[1] == ':');
            if (is_abs) {
                if (!fts_expr.empty()) {
                    // Recursive range scan when searching inside a folder
                    const std::string sub_lo = bnorm + "\\";
                    const std::string sub_hi = bnorm + "]"; // ASCII ']' == '\\' + 1
                    conds.push_back("(c.folder_path = ? OR (c.folder_path >= ? AND c.folder_path < ?))");
                    binds.push_back({false, 0, bnorm});
                    binds.push_back({false, 0, sub_lo});
                    binds.push_back({false, 0, sub_hi});
                } else if (bnorm.size() == 2 && bnorm[1] == ':') {
                    // Direct files at drive root (e.g., "H:\")
                    conds.push_back("(c.folder_path = ? OR c.folder_path = ?)");
                    binds.push_back({false, 0, bnorm});
                    binds.push_back({false, 0, bnorm + "\\"});
                } else {
                    // Direct children only in Folder View so subfolder comics never duplicate beside their folder card
                    conds.push_back("c.folder_path = ?");
                    binds.push_back({false, 0, bnorm});
                }
            } else {
                const std::string pat_s = escape_sql_like(norm) + "/%";
                const std::string pat_b = escape_sql_like(bnorm) + "\\%";
                conds.push_back(
                    "(c.relative_path LIKE ? ESCAPE '!' OR c.relative_path LIKE ? ESCAPE '!' "
                    "OR c.folder_path = ? OR c.path LIKE ? ESCAPE '!' OR c.path LIKE ? ESCAPE '!')");
                binds.push_back({false, 0, pat_s});
                binds.push_back({false, 0, pat_b});
                binds.push_back({false, 0, bnorm});
                binds.push_back({false, 0, pat_s});
                binds.push_back({false, 0, pat_b});
            }
        }
    }
    if (q.shelf_id > 0) {
        conds.push_back("cs.shelf_id = ?");
        binds.push_back({true, q.shelf_id, {}});
    }
    if (!q.author.empty()) {
        conds.push_back("c.author = ? COLLATE NOCASE");
        binds.push_back({false, 0, q.author});
    }
    if (!q.tag.empty()) {
        conds.push_back("c.tags LIKE ? ESCAPE '!'");
        binds.push_back({false, 0, "%" + escape_sql_like(q.tag) + "%"});
    }
    append_smartview_filters(sv_filter, /*include_status=*/true, conds, binds);

    if (!conds.empty()) {
        from_where << " WHERE ";
        for (size_t i = 0; i < conds.size(); ++i) {
            if (i > 0) from_where << " AND ";
            from_where << conds[i];
        }
    }

    const int safe_limit = std::clamp(q.limit > 0 ? q.limit : 120, 1, 500);
    const int safe_offset = std::max(0, q.offset);

    // 1. Total count
    {
        const std::string count_sql = "SELECT COUNT(*)" + from_where.str() + ";";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, count_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            bind_sql_params(stmt, binds);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                res.total_count = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        } else {
            last_error_ = sqlite3_errmsg(db_);
            return res;
        }
    }

    // 2. Paginated/windowed slice
    std::string order_by = " ORDER BY c.title COLLATE NOCASE ASC, c.id ASC ";
    if (q.sort == "title_desc")          order_by = " ORDER BY c.title COLLATE NOCASE DESC, c.id DESC ";
    else if (q.sort == "added_desc")     order_by = " ORDER BY c.added_at DESC, c.id DESC ";
    else if (q.sort == "last_read_desc") order_by = " ORDER BY c.last_read_at DESC, c.id DESC ";
    else if (q.sort == "size_desc")      order_by = " ORDER BY c.file_size_bytes DESC, c.id ASC ";

    const std::string select_sql =
        std::string("SELECT ") + kComicSelectColsAliased + " " +
        from_where.str() + order_by + " LIMIT ? OFFSET ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, select_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        bind_sql_params(stmt, binds);
        const int base = static_cast<int>(binds.size());
        sqlite3_bind_int(stmt, base + 1, safe_limit);
        sqlite3_bind_int(stmt, base + 2, safe_offset);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            res.items.push_back(read_comic_row(stmt));
        }
        sqlite3_finalize(stmt);
    } else {
        last_error_ = sqlite3_errmsg(db_);
    }

    return res;
}

bool Database::update_comic_progress(int64_t id,
                                     int current_page,
                                     int page_count,
                                     const std::string& status,
                                     int is_favorite_opt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    static constexpr const char* kSql = R"SQL(
        UPDATE comics SET
            current_page = CASE WHEN ?1 >= 0 THEN ?1 ELSE current_page END,
            page_count   = CASE WHEN ?2 > 0  THEN ?2 ELSE page_count END,
            status       = CASE WHEN ?3 != '' THEN ?3 ELSE status END,
            is_favorite  = CASE WHEN ?4 >= 0 THEN ?4 ELSE is_favorite END,
            last_read_at = CASE WHEN ?1 >= 0 OR ?3 != '' THEN strftime('%s', 'now') ELSE last_read_at END
        WHERE id = ?5;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(stmt, 1, current_page);
    sqlite3_bind_int(stmt, 2, page_count);
    sqlite3_bind_text(stmt, 3, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, is_favorite_opt);
    sqlite3_bind_int64(stmt, 5, id);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::batch_update_comic_cover_pack(const std::vector<CoverPackUpdate>& updates) {
    if (updates.empty()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    if (!exec_sql("BEGIN IMMEDIATE;")) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE comics SET cover_pack_offset = ?1, cover_pack_size = ?2 WHERE id = ?3;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        exec_sql("ROLLBACK;");
        return false;
    }

    bool ok = true;
    for (const auto& u : updates) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, u.offset);
        sqlite3_bind_int64(stmt, 2, u.size);
        sqlite3_bind_int64(stmt, 3, u.comic_id);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            last_error_ = sqlite3_errmsg(db_);
            ok = false;
            break;
        }
    }
    sqlite3_finalize(stmt);

    if (ok) {
        return exec_sql("COMMIT;");
    }
    exec_sql("ROLLBACK;");
    return false;
}

bool Database::reset_all_cover_pack_offsets() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    return exec_sql("UPDATE comics SET cover_pack_offset = -1, cover_pack_size = 0;");
}

bool Database::update_comic_offline_pin(int64_t id, bool pinned, const std::string& offline_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE comics SET is_offline_pinned = ?1, offline_path = ?2 WHERE id = ?3;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
    sqlite3_bind_text(stmt, 2, offline_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, id);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

int64_t Database::count_offline_pinned_comics() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM comics WHERE is_offline_pinned = 1;", -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

std::vector<std::string> Database::get_pinned_offline_paths() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> paths;
    if (!db_) return paths;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT offline_path FROM comics WHERE is_offline_pinned = 1 AND offline_path != '';",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return paths;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        paths.push_back(col_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return paths;
}

std::vector<std::pair<int64_t, std::string>> Database::get_unpinned_comics_under_prefix(const std::string& folder_prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<int64_t, std::string>> results;
    if (!db_ || folder_prefix.empty()) return results;

    std::string clean_prefix = folder_prefix;
    while (clean_prefix.size() > 1 && (clean_prefix.back() == '\\' || clean_prefix.back() == '/')) {
        if (clean_prefix.size() == 3 && clean_prefix[1] == ':') break; // Keep drive root like "C:\"
        clean_prefix.pop_back();
    }

    const bool ends_with_sep = (!clean_prefix.empty() && (clean_prefix.back() == '\\' || clean_prefix.back() == '/'));
    const std::string escaped = escape_sql_like(clean_prefix);
    const std::string pat_bslash = ends_with_sep ? (escaped + "%") : (escaped + "\\%");
    const std::string pat_slash  = ends_with_sep ? (escaped + "%") : (escaped + "/%");

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT id, path FROM comics "
            "WHERE COALESCE(is_offline_pinned, 0) = 0 "
            "AND (path LIKE ?1 ESCAPE '!' OR path LIKE ?2 ESCAPE '!');",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return results;
    }
    sqlite3_bind_text(stmt, 1, pat_bslash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pat_slash.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.emplace_back(sqlite3_column_int64(stmt, 0), col_text(stmt, 1));
    }
    sqlite3_finalize(stmt);
    return results;
}

int64_t Database::delete_comics_by_ids_if_unpinned(const std::vector<int64_t>& ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || ids.empty()) return 0;

    if (!exec_sql("BEGIN IMMEDIATE;")) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "DELETE FROM comics WHERE id = ?1 AND COALESCE(is_offline_pinned, 0) = 0;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        exec_sql("ROLLBACK;");
        return 0;
    }

    int64_t deleted = 0;
    for (int64_t id : ids) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            deleted += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(stmt);

    if (!exec_sql("COMMIT;")) {
        exec_sql("ROLLBACK;");
        return 0;
    }
    return deleted;
}

int64_t Database::remap_drive_prefix(const std::string& old_prefix, const std::string& new_prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || old_prefix.empty() || new_prefix.empty() || old_prefix == new_prefix) return 0;

    if (!exec_sql("BEGIN IMMEDIATE;")) return 0;

    int64_t updated = 0;
    const std::string like_pattern = old_prefix + "%";
    const int prefix_len = static_cast<int>(old_prefix.size());

    auto run_remap = [&](const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, new_prefix.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, prefix_len + 1);
            sqlite3_bind_text(stmt, 3, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                updated += sqlite3_changes(db_);
            }
            sqlite3_finalize(stmt);
        }
    };

    run_remap("UPDATE OR IGNORE comics SET path = ?1 || substr(path, ?2) WHERE path LIKE ?3;");
    run_remap("UPDATE OR IGNORE comics SET folder_path = ?1 || substr(folder_path, ?2) WHERE folder_path LIKE ?3;");
    run_remap("UPDATE OR IGNORE watched_folders SET path = ?1 || substr(path, ?2) WHERE path LIKE ?3;");

    exec_sql("COMMIT;");
    return updated;
}

// ---- Shelves ----

std::vector<ShelfRecord> Database::get_shelves() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ShelfRecord> results;
    if (!db_) return results;

    static constexpr const char* kSql = R"SQL(
        SELECT s.id, s.name, s.sort_order, COUNT(cs.comic_id), s.created_at
        FROM shelves s
        LEFT JOIN comic_shelves cs ON cs.shelf_id = s.id
        GROUP BY s.id
        ORDER BY s.sort_order ASC, s.name COLLATE NOCASE ASC;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ShelfRecord r;
        r.id          = sqlite3_column_int64(stmt, 0);
        r.name        = col_text(stmt, 1);
        r.sort_order  = sqlite3_column_int(stmt, 2);
        r.comic_count = sqlite3_column_int64(stmt, 3);
        r.created_at  = sqlite3_column_int64(stmt, 4);
        results.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return results;
}

bool Database::create_shelf(const std::string& name, ShelfRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || name.empty()) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO shelves (name) VALUES (?1);", -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    if (!ok) return false;

    out.id = sqlite3_last_insert_rowid(db_);
    out.name = name;
    out.sort_order = 0;
    out.comic_count = 0;
    return true;
}

bool Database::rename_shelf(int64_t id, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || name.empty()) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE shelves SET name = ?1 WHERE id = ?2;", -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, id);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::delete_shelf(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM shelves WHERE id = ?1;", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, id);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::add_comic_to_shelf(int64_t shelf_id, int64_t comic_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT OR IGNORE INTO comic_shelves (comic_id, shelf_id) VALUES (?1, ?2);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, comic_id);
    sqlite3_bind_int64(stmt, 2, shelf_id);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::remove_comic_from_shelf(int64_t shelf_id, int64_t comic_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "DELETE FROM comic_shelves WHERE comic_id = ?1 AND shelf_id = ?2;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, comic_id);
    sqlite3_bind_int64(stmt, 2, shelf_id);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<int64_t> Database::get_comic_ids_in_shelf(int64_t shelf_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int64_t> ids;
    if (!db_ || shelf_id <= 0) return ids;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT comic_id FROM comic_shelves WHERE shelf_id = ?1 ORDER BY comic_id ASC;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return ids;
    }
    sqlite3_bind_int64(stmt, 1, shelf_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return ids;
}

// ---- Bookmarks ----

std::vector<BookmarkRecord> Database::get_bookmarks(int64_t comic_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BookmarkRecord> results;
    if (!db_) return results;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT id, comic_id, page_number, name, created_at FROM bookmarks "
            "WHERE comic_id = ?1 ORDER BY page_number ASC, id ASC;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }
    sqlite3_bind_int64(stmt, 1, comic_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BookmarkRecord b;
        b.id          = sqlite3_column_int64(stmt, 0);
        b.comic_id    = sqlite3_column_int64(stmt, 1);
        b.page_number = sqlite3_column_int(stmt, 2);
        b.name        = col_text(stmt, 3);
        b.created_at  = sqlite3_column_int64(stmt, 4);
        results.push_back(std::move(b));
    }
    sqlite3_finalize(stmt);
    return results;
}

bool Database::add_bookmark(int64_t comic_id, int page_number, const std::string& name, BookmarkRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO bookmarks (comic_id, page_number, name) VALUES (?1, ?2, ?3);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, comic_id);
    sqlite3_bind_int(stmt, 2, page_number);
    sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    if (!ok) return false;

    out.id = sqlite3_last_insert_rowid(db_);
    out.comic_id = comic_id;
    out.page_number = page_number;
    out.name = name;
    return true;
}

bool Database::delete_bookmark(int64_t bookmark_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM bookmarks WHERE id = ?1;", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, bookmark_id);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

// ---- Smartviews ----

std::vector<SmartViewRecord> Database::get_smartviews() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SmartViewRecord> results;
    if (!db_) return results;

    const char* sql = "SELECT id, slug, name, query_json, is_builtin, sort_order "
                      "FROM smartviews ORDER BY sort_order ASC, id ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return results;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SmartViewRecord rec;
        rec.id = sqlite3_column_int64(stmt, 0);
        rec.slug = col_text(stmt, 1);
        rec.name = col_text(stmt, 2);
        rec.query_json = col_text(stmt, 3);
        if (rec.query_json.empty()) rec.query_json = "{}";
        rec.is_builtin = sqlite3_column_int(stmt, 4) != 0;
        rec.sort_order = sqlite3_column_int(stmt, 5);
        results.push_back(std::move(rec));
    }

    sqlite3_finalize(stmt);
    return results;
}

bool Database::create_smartview(const std::string& slug,
                                const std::string& name,
                                const std::string& query_json,
                                SmartViewRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || slug.empty() || name.empty()) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO smartviews (slug, name, query_json, is_builtin, sort_order) "
            "VALUES (?1, ?2, ?3, 0, 100);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, slug.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, query_json.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    if (!ok) return false;

    out.id = sqlite3_last_insert_rowid(db_);
    out.slug = slug;
    out.name = name;
    out.query_json = query_json;
    out.is_builtin = false;
    out.sort_order = 100;
    return true;
}

bool Database::delete_smartview(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "DELETE FROM smartviews WHERE id = ?1 AND is_builtin = 0;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

// ---- Watched Folders ----

bool Database::upsert_watched_folder(const std::string& path,
                                     const std::string& name,
                                     bool is_network,
                                     WatchedFolderRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || path.empty()) return false;

    static constexpr const char* kSql = R"SQL(
        INSERT INTO watched_folders (path, name, is_network, auto_sync)
        VALUES (?1, ?2, ?3, 1)
        ON CONFLICT(path) DO UPDATE SET
            name       = excluded.name,
            is_network = excluded.is_network;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, is_network ? 1 : 0);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    if (!ok) return false;

    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT id, path, name, is_network, auto_sync, added_at, last_scanned_at "
            "FROM watched_folders WHERE path = ?1;",
            -1, &sel, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sel, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sel) == SQLITE_ROW) {
            out.id = sqlite3_column_int64(sel, 0);
            out.path = col_text(sel, 1);
            out.name = col_text(sel, 2);
            out.is_network = sqlite3_column_int(sel, 3) != 0;
            out.auto_sync = sqlite3_column_int(sel, 4) != 0;
            out.added_at = sqlite3_column_int64(sel, 5);
            out.last_scanned_at = sqlite3_column_int64(sel, 6);
        }
        sqlite3_finalize(sel);
    }
    return true;
}

std::vector<WatchedFolderRecord> Database::get_watched_folders() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WatchedFolderRecord> results;
    if (!db_) return results;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT id, path, name, is_network, auto_sync, added_at, last_scanned_at "
            "FROM watched_folders ORDER BY id ASC;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WatchedFolderRecord f;
        f.id = sqlite3_column_int64(stmt, 0);
        f.path = col_text(stmt, 1);
        f.name = col_text(stmt, 2);
        f.is_network = sqlite3_column_int(stmt, 3) != 0;
        f.auto_sync = sqlite3_column_int(stmt, 4) != 0;
        f.added_at = sqlite3_column_int64(stmt, 5);
        f.last_scanned_at = sqlite3_column_int64(stmt, 6);
        results.push_back(std::move(f));
    }
    sqlite3_finalize(stmt);
    return results;
}

bool Database::update_watched_folder_scanned(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE watched_folders SET last_scanned_at = strftime('%s', 'now') WHERE id = ?1;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, id);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::delete_watched_folder(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    std::string folder_path;
    {
        sqlite3_stmt* sel = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT path FROM watched_folders WHERE id = ?1;", -1, &sel, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_int64(sel, 1, id);
        if (sqlite3_step(sel) == SQLITE_ROW) {
            const unsigned char* txt = sqlite3_column_text(sel, 0);
            if (txt) folder_path = reinterpret_cast<const char*>(txt);
        }
        sqlite3_finalize(sel);
    }

    if (!exec_sql("BEGIN IMMEDIATE;")) return false;

    bool ok = false;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM watched_folders WHERE id = ?1;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, id);
            ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
            sqlite3_finalize(stmt);
        }
    }

    if (ok && !folder_path.empty()) {
        std::string norm = folder_path;
        std::replace(norm.begin(), norm.end(), '/', '\\');
        while (norm.size() > 3 && norm.back() == '\\') {
            norm.pop_back();
        }
        for (char& c : norm) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::string prefix = norm;
        if (prefix.empty() || prefix.back() != '\\') {
            prefix.push_back('\\');
        }

        sqlite3_stmt* del_comics = nullptr;
        static constexpr const char* kDeleteComicsSql =
            "DELETE FROM comics "
            "WHERE LOWER(REPLACE(path, '/', '\\')) = ?1 "
            "   OR SUBSTR(LOWER(REPLACE(path, '/', '\\')), 1, ?2) = ?3;";
        if (sqlite3_prepare_v2(db_, kDeleteComicsSql, -1, &del_comics, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del_comics, 1, norm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(del_comics, 2, static_cast<sqlite3_int64>(prefix.size()));
            sqlite3_bind_text(del_comics, 3, prefix.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(del_comics) != SQLITE_DONE) {
                ok = false;
            }
            sqlite3_finalize(del_comics);
        } else {
            ok = false;
        }
    }

    if (ok) {
        exec_sql("COMMIT;");
    } else {
        exec_sql("ROLLBACK;");
    }
    return ok;
}

std::vector<LibraryFolderEntry> Database::get_library_folders(const std::string& prefix, bool include_hidden) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LibraryFolderEntry> out;
    if (!db_) return out;

    auto normalize_slashes = [](std::string s) {
        std::replace(s.begin(), s.end(), '\\', '/');
        const bool is_unc = s.size() >= 2 && s[0] == '/' && s[1] == '/';
        if (!is_unc) {
            while (!s.empty() && s.front() == '/') s.erase(s.begin());
        }
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    };
    auto leaf_name = [](const std::string& norm_path) -> std::string {
        const size_t pos = norm_path.rfind('/');
        if (pos == std::string::npos) return norm_path;
        std::string leaf = norm_path.substr(pos + 1);
        return leaf.empty() ? norm_path : leaf;
    };

    // Covering index scan on idx_comics_folder_path (<5ms for 30,000+ rows).
    // SQLite secondary indexes include rowid (comics.id), so MIN(id) stays index-only.
    struct FolderRow {
        std::string norm_dir;
        std::string lower_dir;
        int64_t count = 0;
        int64_t min_comic_id = 0;
    };
    std::vector<FolderRow> rows;
    {
        const char* sql = include_hidden
            ? "SELECT folder_path, COUNT(*), MIN(id) FROM comics WHERE folder_path != '' GROUP BY folder_path;"
            : "SELECT folder_path, COUNT(*), MIN(id) FROM comics WHERE folder_path != '' AND COALESCE(is_hidden, 0) = 0 GROUP BY folder_path;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return out;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string dir = normalize_slashes(col_text(stmt, 0));
            const int64_t cnt = sqlite3_column_int64(stmt, 1);
            const int64_t min_id = sqlite3_column_int64(stmt, 2);
            if (dir.empty() || cnt <= 0) continue;
            FolderRow row;
            row.lower_dir = to_lower_ascii(dir);
            row.norm_dir = std::move(dir);
            row.count = cnt;
            row.min_comic_id = min_id;
            rows.push_back(std::move(row));
        }
        sqlite3_finalize(stmt);
    }

    auto collect_immediate_children = [&](const std::string& base_norm) {
        const std::string lower_base = to_lower_ascii(base_norm);
        struct ChildAgg {
            std::string name;
            int64_t count = 0;
            int64_t preview_comic_id = 0;
        };
        std::map<std::string, ChildAgg> children;

        for (const auto& row : rows) {
            if (row.norm_dir.size() <= base_norm.size() + 1) continue;
            if (row.lower_dir.compare(0, lower_base.size(), lower_base) != 0 ||
                row.lower_dir[lower_base.size()] != '/') {
                continue;
            }
            const size_t start = base_norm.size() + 1;
            const size_t next_slash = row.norm_dir.find('/', start);
            const std::string seg = (next_slash == std::string::npos)
                ? row.norm_dir.substr(start)
                : row.norm_dir.substr(start, next_slash - start);
            if (seg.empty()) continue;

            const std::string key = to_lower_ascii(seg);
            auto it = children.find(key);
            if (it == children.end()) {
                children.emplace(key, ChildAgg{seg, row.count, row.min_comic_id});
            } else {
                it->second.count += row.count;
                if (row.min_comic_id > 0 &&
                    (it->second.preview_comic_id == 0 || row.min_comic_id < it->second.preview_comic_id)) {
                    it->second.preview_comic_id = row.min_comic_id;
                }
            }
        }

        std::vector<LibraryFolderEntry> res;
        res.reserve(children.size());
        for (const auto& kv : children) {
            LibraryFolderEntry entry;
            entry.name = kv.second.name;
            entry.path = base_norm + "/" + kv.second.name;
            entry.comic_count = kv.second.count;
            entry.preview_comic_id = kv.second.preview_comic_id;
            res.push_back(std::move(entry));
        }
        return res;
    };

    const std::string norm_parent = normalize_slashes(prefix);
    if (!norm_parent.empty()) {
        return collect_immediate_children(norm_parent);
    }

    // Root view (parent_path is empty): inspect watched root folders so we never expose raw drive splits like "H:\".
    struct WatchedRoot {
        std::string norm_path;
        std::string lower_path;
        std::string name;
    };
    std::vector<WatchedRoot> roots;
    {
        sqlite3_stmt* wf_stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT path, name FROM watched_folders ORDER BY id ASC;", -1, &wf_stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(wf_stmt) == SQLITE_ROW) {
                std::string norm = normalize_slashes(col_text(wf_stmt, 0));
                if (norm.empty()) continue;
                std::string name = col_text(wf_stmt, 1);
                if (name.empty()) name = leaf_name(norm);
                WatchedRoot r;
                r.lower_path = to_lower_ascii(norm);
                r.norm_path = std::move(norm);
                r.name = std::move(name);
                roots.push_back(std::move(r));
            }
            sqlite3_finalize(wf_stmt);
        }
    }

    // If exactly 1 root folder is watched, skip the redundant root card and return its immediate subfolders.
    if (roots.size() == 1) {
        return collect_immediate_children(roots[0].norm_path);
    }

    // If multiple root folders are watched, return each watched root with aggregated comic count & preview cover.
    if (roots.size() > 1) {
        out.reserve(roots.size());
        for (const auto& r : roots) {
            int64_t total_count = 0;
            int64_t preview_id = 0;
            for (const auto& row : rows) {
                const bool exact = (row.lower_dir == r.lower_path);
                const bool child = (row.lower_dir.size() > r.lower_path.size() &&
                                    row.lower_dir.compare(0, r.lower_path.size(), r.lower_path) == 0 &&
                                    row.lower_dir[r.lower_path.size()] == '/');
                if (exact || child) {
                    total_count += row.count;
                    if (row.min_comic_id > 0 && (preview_id == 0 || row.min_comic_id < preview_id)) {
                        preview_id = row.min_comic_id;
                    }
                }
            }
            LibraryFolderEntry entry;
            entry.name = r.name;
            entry.path = r.norm_path;
            entry.comic_count = total_count;
            entry.preview_comic_id = preview_id;
            out.push_back(std::move(entry));
        }
        return out;
    }

    // Fallback when no watched_folders are registered: return grouped leaf folders with preview IDs.
    struct FolderAgg {
        std::string name;
        std::string path;
        int64_t count = 0;
        int64_t preview_comic_id = 0;
    };
    std::map<std::string, FolderAgg> grouped;
    for (const auto& row : rows) {
        // Skip bare Windows drive roots like "H:"
        if (row.norm_dir.size() == 2 && row.norm_dir[1] == ':') continue;
        auto it = grouped.find(row.lower_dir);
        if (it == grouped.end()) {
            grouped.emplace(row.lower_dir, FolderAgg{leaf_name(row.norm_dir), row.norm_dir, row.count, row.min_comic_id});
        } else {
            it->second.count += row.count;
            if (row.min_comic_id > 0 &&
                (it->second.preview_comic_id == 0 || row.min_comic_id < it->second.preview_comic_id)) {
                it->second.preview_comic_id = row.min_comic_id;
            }
        }
    }
    out.reserve(grouped.size());
    for (const auto& kv : grouped) {
        LibraryFolderEntry entry;
        entry.name = kv.second.name;
        entry.path = kv.second.path;
        entry.comic_count = kv.second.count;
        entry.preview_comic_id = kv.second.preview_comic_id;
        out.push_back(std::move(entry));
    }
    return out;
}

int64_t Database::batch_update_status(const std::vector<int64_t>& ids, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || ids.empty()) return 0;
    if (status != "unread" && status != "reading" && status != "read") {
        last_error_ = "Invalid status";
        return 0;
    }
    if (!exec_sql("BEGIN IMMEDIATE;")) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE comics SET status = ?1, last_read_at = strftime('%s', 'now') WHERE id = ?2;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        exec_sql("ROLLBACK;");
        return 0;
    }

    int64_t updated = 0;
    for (int64_t id : ids) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, id);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            updated += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(stmt);
    exec_sql("COMMIT;");
    return updated;
}

int64_t Database::batch_update_favorite(const std::vector<int64_t>& ids, bool is_favorite) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || ids.empty()) return 0;
    if (!exec_sql("BEGIN IMMEDIATE;")) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE comics SET is_favorite = ?1 WHERE id = ?2;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        exec_sql("ROLLBACK;");
        return 0;
    }

    int64_t updated = 0;
    for (int64_t id : ids) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int(stmt, 1, is_favorite ? 1 : 0);
        sqlite3_bind_int64(stmt, 2, id);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            updated += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(stmt);
    exec_sql("COMMIT;");
    return updated;
}

bool Database::set_comic_hidden(int64_t id, bool is_hidden) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || id <= 0) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE comics SET is_hidden = ?1 WHERE id = ?2;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(stmt, 1, is_hidden ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, id);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return ok;
}

int64_t Database::batch_update_hidden(const std::vector<int64_t>& ids, bool is_hidden) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || ids.empty()) return 0;
    if (!exec_sql("BEGIN IMMEDIATE;")) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE comics SET is_hidden = ?1 WHERE id = ?2;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        exec_sql("ROLLBACK;");
        return 0;
    }

    int64_t updated = 0;
    for (int64_t id : ids) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int(stmt, 1, is_hidden ? 1 : 0);
        sqlite3_bind_int64(stmt, 2, id);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            updated += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(stmt);
    exec_sql("COMMIT;");
    return updated;
}

int64_t Database::batch_assign_shelf(int64_t shelf_id, const std::vector<int64_t>& ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || shelf_id <= 0 || ids.empty()) return 0;
    if (!exec_sql("BEGIN IMMEDIATE;")) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT OR IGNORE INTO comic_shelves (comic_id, shelf_id) VALUES (?1, ?2);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        exec_sql("ROLLBACK;");
        return 0;
    }

    int64_t updated = 0;
    for (int64_t id : ids) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_int64(stmt, 2, shelf_id);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            updated += 1;
        }
    }
    sqlite3_finalize(stmt);
    exec_sql("COMMIT;");
    return updated;
}

int64_t Database::batch_remove_shelf(int64_t shelf_id, const std::vector<int64_t>& ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || shelf_id <= 0 || ids.empty()) return 0;
    if (!exec_sql("BEGIN IMMEDIATE;")) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "DELETE FROM comic_shelves WHERE comic_id = ?1 AND shelf_id = ?2;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        exec_sql("ROLLBACK;");
        return 0;
    }

    int64_t updated = 0;
    for (int64_t id : ids) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_int64(stmt, 2, shelf_id);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            updated += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(stmt);
    exec_sql("COMMIT;");
    return updated;
}

// ============================================================================
// SettingsManager
// ============================================================================

SettingsManager::SettingsManager(std::filesystem::path settings_path)
    : path_(std::move(settings_path)), settings_(default_settings()) {}

nlohmann::json SettingsManager::default_settings() {
    return nlohmann::json{
        {"cover_mode", "persistent_pack"},
        {"language", "en"},
        {"theme", "dark"},
        {"default_manga_mode", false},
        {"smart_page_layout", true},
        {"auto_detect_borders", false},
        {"stack_pages_vertically", false},
        {"keep_screen_active", true},
        {"circular_progress_bar", true},
        {"display_folders_before_files", true},
        {"smart_rename", false},
        {"auto_refresh_library", true},
        {"show_hidden_elements", false},
        {"enable_loupe_magnifier", true},
        {"enable_manga_enhancer", false},
        {"enable_continue_reading", true},
        {"enable_auto_scroll", true},
        {"enable_metadata_extractor", true},
        {"enable_random_button", true},
        {"shortcuts", nlohmann::json{
            {"next_page", "ArrowRight"},
            {"prev_page", "ArrowLeft"},
            {"next_page_alt", "PageDown"},
            {"prev_page_alt", "PageUp"},
            {"space_next", " "},
            {"toggle_fullscreen", "f"},
            {"toggle_manga_mode", "m"},
            {"toggle_dual_page", "d"},
            {"toggle_bookmarks", "b"},
            {"zoom_in", "+"},
            {"zoom_out", "-"},
            {"zoom_reset", "0"},
            {"next_volume", "Enter"},
            {"close_reader", "Escape"},
            {"focus_search", "/"}
        }}
    };
}

bool SettingsManager::load_or_create() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = default_settings();

    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) {
        std::ifstream ifs(path_, std::ios::binary);
        if (ifs.is_open()) {
            try {
                nlohmann::json parsed;
                ifs >> parsed;
                if (parsed.is_object()) {
                    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
                        if (it.key() == "shortcuts" && it.value().is_object() && settings_["shortcuts"].is_object()) {
                            for (auto sit = it.value().begin(); sit != it.value().end(); ++sit) {
                                settings_["shortcuts"][sit.key()] = sit.value();
                            }
                        } else {
                            settings_[it.key()] = it.value();
                        }
                    }
                    return save_locked();
                }
            } catch (...) {
                // Fall back to defaults on corrupt JSON
            }
        }
    }

    return save_locked();
}

nlohmann::json SettingsManager::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

nlohmann::json SettingsManager::update(const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (patch.is_object()) {
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            if (it.key() == "shortcuts" && it.value().is_object() && settings_.contains("shortcuts") && settings_["shortcuts"].is_object()) {
                for (auto sit = it.value().begin(); sit != it.value().end(); ++sit) {
                    settings_["shortcuts"][sit.key()] = sit.value();
                }
            } else {
                settings_[it.key()] = it.value();
            }
        }
        save_locked();
    }
    return settings_;
}

bool SettingsManager::save_locked() const {
    std::error_code ec;
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
    }

    std::filesystem::path tmp_path = path_;
    tmp_path += L".tmp";

    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs << settings_.dump(2) << "\n";
        ofs.flush();
        if (!ofs.good()) return false;
    }

    if (!MoveFileExW(tmp_path.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::rename(tmp_path, path_, ec);
        if (ec) return false;
    }
    return true;
}

} // namespace koma
