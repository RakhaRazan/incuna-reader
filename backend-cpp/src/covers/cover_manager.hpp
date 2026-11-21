#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "db/database.hpp"

namespace koma {

struct CoverResult {
    bool found = false;
    bool cancelled = false;
    std::string mime_type;
    std::vector<uint8_t> data;
    std::string source; // "ram_lru", "covers_pack", "shell_cache", "extracted"
};

struct CoverPackStatus {
    bool building = false;
    int64_t total = 0;
    int64_t processed = 0;
    int64_t succeeded = 0;
    int64_t failed = 0;
    int64_t pack_size_bytes = 0;
};

// 2-Mode Cover Thumbnail Engine:
// - Mode 1 (`persistent_pack`, default): Single concatenated <exe_dir>/data/covers.pack
//   indexed by (cover_pack_offset, cover_pack_size) in SQLite for O(1) reads,
//   with automatic 2-thread SATA-friendly background building and on-demand fallback.
// - Mode 2 (`ram_only`, optional, 0 MB disk): 64 MB bounded RAM LRU + Windows
//   IShellItemImageFactory (SIIGBF_INCACHEONLY) + 2-thread anti-thrashing HDD worker pool
//   with scroll cancellation.
class CoverManager {
public:
    static constexpr size_t kMaxRamCacheBytes = 64ULL * 1024ULL * 1024ULL; // 64 MB
    static constexpr int kCoverMaxWidth = 220;
    static constexpr int kCoverMaxHeight = 310;
    static constexpr int kPackThumbWidth = 200;
    static constexpr int kPackThumbHeight = 280;
    static constexpr int kPackJpegQuality = 78;
    static constexpr int kWorkerThreads = 2; // Strictly 2 viewport threads to avoid SATA 3 HDD thrashing
    static constexpr int kPackDecodeWorkers = 3; // 1 sequential HDD reader + 3 parallel CPU decode workers
    static constexpr int kPackFetchChunkSize = 1000;
    static constexpr size_t kPackRawQueueMax = 24;
    static constexpr size_t kCoverDbBatchSize = 100;
    static constexpr int64_t kCoverDbFlushIntervalMs = 1500;

    CoverManager(Database& db,
                 SettingsManager& settings,
                 std::filesystem::path covers_pack_path,
                 std::filesystem::path stream_cache_dir);
    ~CoverManager();

    CoverManager(const CoverManager&) = delete;
    CoverManager& operator=(const CoverManager&) = delete;

    void start();
    void stop();

    // Fetch cover thumbnail for comic_id according to active cover_mode.
    CoverResult get_cover(int64_t comic_id);

    // Abort queued extraction requests when the user scrolls rapidly past them.
    void cancel_ids(const std::vector<int64_t>& ids);

    // Update active visible viewport comic IDs for zero-seek Mode 1 pre-seek cancellation.
    void set_active_viewport(const std::vector<int64_t>& visible_ids);

    // Clear all pending viewport & background cover queues, reset indexing counters,
    // and bump the generation counter so any in-flight worker discards stale results.
    void clear_queue();

    // Mode 2: Build covers.pack in background for all comics missing pack offsets.
    bool start_pack_build();
    CoverPackStatus get_pack_status() const;

    // Mode 2 -> Mode 1 clean revert: stops builder, deletes covers.pack, resets SQLite offsets.
    bool purge_pack();

    // Reset all cover_pack_size = 0, truncate covers.pack, and trigger a fresh background rebuild.
    bool rebuild_pack();

    // Diagnostics
    size_t ram_cache_bytes() const;
    size_t ram_cache_count() const;
    uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    int64_t indexed_count() const { return indexed_count_.load(std::memory_order_acquire); }
    int64_t total_to_index() const { return total_to_index_.load(std::memory_order_acquire); }

private:
    struct LruEntry {
        int64_t comic_id = 0;
        std::string mime_type;
        std::vector<uint8_t> data;
    };

    struct ExtractJob {
        int64_t comic_id = 0;
        std::wstring file_path;
        bool write_to_pack = false;
        uint64_t generation = 0;
        std::shared_ptr<std::promise<CoverResult>> promise;
    };

    struct RawCoverWork {
        int64_t comic_id = 0;
        std::wstring file_path;
        std::vector<uint8_t> raw_bytes;
        uint64_t generation = 0;
    };

    bool is_persistent_pack_mode() const;
    bool is_in_active_viewport(int64_t comic_id) const;
    bool lru_get(int64_t comic_id, CoverResult& out);
    void lru_put(int64_t comic_id, const std::string& mime_type, const std::vector<uint8_t>& data);
    void lru_clear();

    bool read_from_pack(int64_t offset, int64_t size, CoverResult& out);
    bool append_to_pack(int64_t comic_id,
                        const std::vector<uint8_t>& data,
                        uint64_t expected_generation);
    void queue_failed_cover(int64_t comic_id, uint64_t expected_generation);
    bool flush_pending_pack_batch(bool force = true);
    bool flush_pending_pack_batch_locked(bool force);
    void discard_pending_pack_batch();
    bool has_pending_pack_batch() const;
    bool try_windows_shell_cache(const std::wstring& path, CoverResult& out);
    CoverResult extract_from_comic(const std::wstring& path, int max_w = kCoverMaxWidth, int max_h = kCoverMaxHeight);
    static std::wstring resolve_effective_path(const ComicRecord& comic);
    static std::wstring resolve_effective_path_row(const UnindexedCoverRow& row);

    void worker_loop();
    void stop_pack_pipeline();
    void pack_reader_loop(uint64_t gen);
    void pack_decode_worker_loop(uint64_t gen);

    Database& db_;
    SettingsManager& settings_;
    std::filesystem::path covers_pack_path_;
    std::filesystem::path stream_cache_dir_;

    // 64 MB bounded RAM LRU cache (fine-grained shared_mutex for <0.2ms concurrent RAM hits)
    mutable std::shared_mutex lru_mutex_;
    std::list<LruEntry> lru_list_;
    std::unordered_map<int64_t, std::list<LruEntry>::iterator> lru_map_;
    size_t lru_bytes_ = 0;

    // Active viewport IDs for Mode 1 pre-seek cancellation
    mutable std::mutex viewport_ids_mutex_;
    std::unordered_set<int64_t> active_viewport_ids_;
    std::atomic<bool> has_viewport_filter_{false};

    // High-priority 2-thread SATA-friendly viewport worker pool (LIFO front-push)
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<ExtractJob> viewport_queue_;
    std::unordered_set<int64_t> cancelled_ids_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    bool stop_workers_ = false;

    // Mode 2 covers.pack indexing pipeline: 1 sequential HDD reader + 3 parallel CPU decode workers
    std::mutex pack_build_mutex_;
    std::mutex pack_pipeline_mutex_;
    std::condition_variable pack_pipeline_cv_;
    std::condition_variable pack_space_cv_;
    std::deque<RawCoverWork> raw_cover_queue_;
    bool stop_pack_pipeline_ = false;
    bool pack_reader_done_ = false;
    bool pack_pipeline_active_ = false;
    bool pack_rescan_requested_ = false;
    int active_pack_decoders_ = 0;
    std::thread pack_reader_thread_;
    std::vector<std::thread> pack_decode_workers_;

    // Generation & indexing progress counters
    std::atomic<uint64_t> generation_{0};
    std::atomic<int64_t> indexed_count_{0};
    std::atomic<int64_t> total_to_index_{0};

    // covers.pack file I/O lock, in-memory write buffer & synchronized SQLite batch
    // (both covers.pack stream and SQLite offsets flush together once every 50 covers).
    mutable std::mutex pack_io_mutex_;
    std::vector<uint8_t> pending_pack_bytes_;
    std::vector<CoverPackUpdate> pending_pack_updates_;
    std::chrono::steady_clock::time_point last_pack_flush_{std::chrono::steady_clock::now()};
    int64_t cached_pack_size_bytes_ = 0;

    mutable std::mutex pack_status_mutex_;
    CoverPackStatus pack_status_;
};

} // namespace koma
