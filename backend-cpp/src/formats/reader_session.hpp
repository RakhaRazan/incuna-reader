#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "formats/comic_reader.hpp"

namespace koma {

// Active comic reader session with a 5-page sliding RAM lookahead cache:
//   window = [current - 1, current, current + 1, current + 2, current + 3]
// Background worker pre-renders adjacent pages so SATA 3 HDD page flips take 0 ms.
class ReaderSession {
public:
    explicit ReaderSession(std::filesystem::path stream_cache_dir = {});
    ~ReaderSession();

    ReaderSession(const ReaderSession&) = delete;
    ReaderSession& operator=(const ReaderSession&) = delete;

    bool open(const std::wstring& path);
    void close();

    bool is_open() const;
    int page_count() const;
    int64_t file_size() const;
    ComicFormat format() const;
    std::string session_id() const;
    std::wstring current_path() const;
    std::string last_error() const;

    // Fetches page from 5-page RAM cache (0 ms hit) or renders on demand,
    // then updates sliding window and wakes background lookahead worker.
    RenderedPage get_page(int index, int target_width = 0, bool auto_crop = false);

    // Extracts cover thumbnail from page 0.
    RenderedPage extract_cover(int max_w, int max_h);

    // Diagnostics for testing/telemetry
    size_t cached_page_count() const;

private:
    struct CacheEntry {
        int target_width = 0;
        bool auto_crop = false;
        RenderedPage page;
    };

    void evict_outside_window_locked(int center_page);
    void worker_loop();

    std::filesystem::path stream_cache_dir_;

    mutable std::mutex state_mutex_;
    std::unique_ptr<ComicReader> reader_;
    std::wstring current_path_;
    std::string session_id_;
    std::string last_error_;

    // 5-page sliding RAM cache keyed by page_index
    mutable std::mutex cache_mutex_;
    std::unordered_map<int, CacheEntry> cache_;
    std::condition_variable worker_cv_;
    std::thread worker_thread_;
    bool stop_worker_ = false;
    int prefetch_center_ = -1;
    int prefetch_target_width_ = 0;
    bool prefetch_auto_crop_ = false;
    uint64_t prefetch_generation_ = 0;
    std::atomic<int> active_foreground_requests_{0};
};

} // namespace koma
