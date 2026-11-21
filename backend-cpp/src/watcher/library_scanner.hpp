#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "db/database.hpp"

namespace koma {

class CoverManager;

struct ScanStatus {
    bool scanning = false;
    std::string current_folder;
    int queued_folders = 0;
    int64_t files_found = 0;
    int64_t files_indexed = 0;
    int64_t batches_committed = 0;
    int64_t remapped_paths = 0;
    std::string last_error;
};

// Non-blocking 30,000-file library scanner (500-file SQLite transactions),
// portable drive-letter auto-remapper, and Win32 ReadDirectoryChangesW auto-sync watcher.
class LibraryScanner {
public:
    static constexpr size_t kBatchSize = 500;

    LibraryScanner(Database& db,
                   std::filesystem::path exe_dir,
                   CoverManager* cover_manager = nullptr,
                   SettingsManager* settings = nullptr);
    ~LibraryScanner();

    LibraryScanner(const LibraryScanner&) = delete;
    LibraryScanner& operator=(const LibraryScanner&) = delete;

    void set_cover_manager(CoverManager* cover_manager);
    void set_settings(SettingsManager* settings);

    // Auto-remap drive-letter prefixes when portable folder + comic library move to a new drive.
    int64_t auto_remap_drives();

    // Non-blocking recursive folder import (Local, \\NAS, OneDrive, Google Drive, Dropbox).
    bool start_folder_import(const std::wstring& folder_path);
    void set_on_scan_complete(std::function<void()> cb);
    ScanStatus get_scan_status() const;

    // Wait for current background scan to finish (useful for tests/shutdown).
    void wait_for_scan();

    // Start/stop Win32 ReadDirectoryChangesW watchers for all auto_sync watched folders.
    void start_watching_all();
    void watch_folder(const std::wstring& folder_path);
    void unwatch_folder(const std::wstring& folder_path);
    void stop_all_watchers();

    // Remove a watched folder, stop its watcher, delete its comics, and clear pending cover jobs.
    bool remove_watched_folder(int64_t folder_id);

    // Helper: returns true if filename extension is an indexable comic archive/PDF/EPUB.
    static bool is_indexable_comic_file(const std::filesystem::path& p, std::string& out_format);

    // Returns true if the watched root folder exists and is currently mounted/reachable.
    static bool is_folder_online(const std::wstring& folder_path);

    // Prunes missing unpinned comics under root_folder ONLY if root_folder is online/mounted.
    int64_t prune_orphans_for_folder(const std::wstring& root_folder,
                                     const std::unordered_set<std::string>* seen_paths_lower = nullptr);

    // Prunes missing unpinned comics across all currently online watched folders.
    int64_t prune_all_orphans();

private:
    struct WatcherHandle {
        std::wstring folder_path;
        void* stop_event = nullptr; // Win32 HANDLE
        std::thread thread;
    };

    struct ScanJob {
        std::wstring root_folder;
        std::wstring lower_key;
        std::string utf8_folder;
        int64_t folder_id = 0;
    };

    void scan_worker_loop();
    void process_scan_job(const ScanJob& job);
    void watcher_thread_func(std::wstring folder_path, void* stop_event);

    Database& db_;
    std::filesystem::path exe_dir_;
    CoverManager* cover_manager_ = nullptr;
    SettingsManager* settings_ = nullptr;

    mutable std::mutex status_mutex_;
    ScanStatus status_;
    std::function<void()> on_scan_complete_;
    std::atomic<bool> cancel_scan_{false};

    // Single-worker FIFO folder import queue (never spawns concurrent scan threads).
    std::mutex scan_queue_mutex_;
    std::condition_variable scan_queue_cv_;
    std::condition_variable scan_done_cv_;
    std::deque<ScanJob> scan_queue_;
    std::wstring active_scan_lower_key_;
    bool scan_job_active_ = false;
    bool stop_scanner_ = false;
    std::thread scan_thread_;

    std::mutex watchers_mutex_;
    std::unordered_map<std::wstring, std::unique_ptr<WatcherHandle>> watchers_;
};

} // namespace koma
