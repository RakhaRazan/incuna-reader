#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace koma {

class CoverManager;
class Database;
class SettingsManager;

struct CloudFileEntry {
    std::string id;
    std::string name;
    bool is_folder = false;
    int64_t size = 0;
    std::string format;
    std::string modified_at;
};

struct CloudImportItem {
    std::string id;
    std::string name;
    bool is_folder = false;
};

struct CloudAuthStartResult {
    bool started = false;
    std::string provider;
    std::string auth_url;
    std::string redirect_uri;
    std::string error;
};

struct CloudListResult {
    bool ok = false;
    int http_status = 200;
    std::string error;
    std::vector<CloudFileEntry> items;
};

struct CloudImportStartResult {
    bool started = false;
    int64_t queued = 0;
    std::string error;
};

// Direct portable Google Drive & OneDrive OAuth2 + REST client backed by native WinHTTP.
// Tokens are persisted strictly in <exe_dir>/data/cloud_tokens.json and imported comics
// are streamed directly to <exe_dir>/data/offline_cache/<provider>/.
class CloudClient {
public:
    CloudClient(Database& db,
                CoverManager& cover_manager,
                SettingsManager& settings,
                std::filesystem::path tokens_path,
                std::filesystem::path offline_cache_dir);
    ~CloudClient();

    CloudClient(const CloudClient&) = delete;
    CloudClient& operator=(const CloudClient&) = delete;

    void stop();

    // Start OAuth2 PKCE loopback flow and open the system browser.
    CloudAuthStartResult start_auth(const std::string& provider,
                                    const std::string& client_id,
                                    const std::string& client_secret = "");

    // Current connection state for both providers plus active auth/import progress.
    nlohmann::json get_status() const;

    // Remove stored OAuth tokens for the given provider ("gdrive" | "onedrive").
    bool disconnect(const std::string& provider);

    // List folders and comic files (.cbz/.cbr/.cb7/.cbt/.pdf/.epub) under folder_id.
    CloudListResult list_folder(const std::string& provider, const std::string& folder_id);

    // Download selected files/folders into <exe_dir>/data/offline_cache/<provider>/,
    // index into SQLite with is_offline_pinned = 1, and trigger covers.pack indexing.
    CloudImportStartResult start_import(const std::string& provider,
                                        const std::vector<CloudImportItem>& items);

    static bool is_valid_provider(const std::string& provider);

private:
    struct ProviderToken {
        std::string client_id;
        std::string client_secret;
        std::string access_token;
        std::string refresh_token;
        int64_t expires_at = 0;
    };

    void load_tokens();
    bool save_tokens_locked() const;
    bool ensure_valid_access_token(const std::string& provider, std::string& out_token, std::string& out_error);
    bool exchange_auth_code(const std::string& provider,
                            const std::string& client_id,
                            const std::string& client_secret,
                            const std::string& code,
                            const std::string& redirect_uri,
                            const std::string& code_verifier,
                            std::string& out_error);

    bool download_gdrive_file(const std::string& access_token,
                              const std::string& file_id,
                              const std::filesystem::path& dest_path,
                              int64_t& out_bytes,
                              std::string& out_error);
    bool download_onedrive_file(const std::string& access_token,
                                const std::string& item_id,
                                const std::filesystem::path& dest_path,
                                int64_t& out_bytes,
                                std::string& out_error);

    void collect_import_files_recursive(const std::string& provider,
                                        const std::string& folder_id,
                                        const std::filesystem::path& rel_subdir,
                                        int depth,
                                        std::vector<std::pair<CloudFileEntry, std::filesystem::path>>& out_files);

    Database& db_;
    CoverManager& cover_manager_;
    SettingsManager& settings_;
    std::filesystem::path tokens_path_;
    std::filesystem::path offline_cache_dir_;
    bool wsa_initialized_ = false;

    mutable std::mutex token_mutex_;
    std::mutex gdrive_refresh_mutex_;
    std::mutex onedrive_refresh_mutex_;
    std::unordered_map<std::string, ProviderToken> tokens_;

    // Background OAuth2 loopback listener state
    mutable std::mutex auth_mutex_;
    std::atomic<bool> auth_in_progress_{false};
    std::atomic<bool> auth_cancel_{false};
    std::string auth_provider_;
    std::string auth_error_;
    std::thread auth_thread_;

    // Background import worker state
    mutable std::mutex import_mutex_;
    std::atomic<bool> import_running_{false};
    std::atomic<bool> import_cancel_{false};
    std::string import_provider_;
    std::atomic<int64_t> import_total_{0};
    std::atomic<int64_t> import_completed_{0};
    std::atomic<int64_t> import_failed_{0};
    std::string import_current_file_;
    std::string import_last_error_;
    std::thread import_thread_;
};

} // namespace koma
