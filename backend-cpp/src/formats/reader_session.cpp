#include "formats/reader_session.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

namespace koma {

ReaderSession::ReaderSession(std::filesystem::path stream_cache_dir)
    : stream_cache_dir_(std::move(stream_cache_dir)) {}

ReaderSession::~ReaderSession() {
    close();
}

bool ReaderSession::open(const std::wstring& path) {
    close();

    auto new_reader = create_comic_reader(path, stream_cache_dir_);
    if (!new_reader) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "Unsupported comic format";
        return false;
    }

    if (!new_reader->open(path)) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = new_reader->last_error();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        reader_ = std::shared_ptr<ComicReader>(std::move(new_reader));
        current_path_ = path;
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        session_id_ = "sess_" + std::to_string(now_ms);
        last_error_.clear();
    }

    {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        cache_.clear();
        stop_worker_ = false;
        prefetch_center_ = -1;
        prefetch_generation_ = 0;
    }

    worker_thread_ = std::thread(&ReaderSession::worker_loop, this);
    return true;
}

void ReaderSession::close() {
    {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        stop_worker_ = true;
        ++prefetch_generation_;
    }
    worker_cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        cache_.clear();
        prefetch_center_ = -1;
    }

    std::shared_ptr<ComicReader> old_reader;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        old_reader = std::move(reader_);
        current_path_.clear();
        session_id_.clear();
    }
    if (old_reader) {
        old_reader->close();
    }
}

bool ReaderSession::is_open() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return reader_ && reader_->is_open();
}

int ReaderSession::page_count() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return reader_ ? reader_->page_count() : 0;
}

int64_t ReaderSession::file_size() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return reader_ ? reader_->file_size() : 0;
}

ComicFormat ReaderSession::format() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return reader_ ? reader_->format() : ComicFormat::Unknown;
}

std::string ReaderSession::session_id() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return session_id_;
}

std::wstring ReaderSession::current_path() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_path_;
}

std::string ReaderSession::last_error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

size_t ReaderSession::cached_page_count() const {
    std::lock_guard<std::mutex> clock(cache_mutex_);
    return cache_.size();
}

void ReaderSession::evict_outside_window_locked(int center_page) {
    // Sliding 5-page RAM window: [center - 1, center, center + 1, center + 2, center + 3]
    const int min_page = center_page - 1;
    const int max_page = center_page + 3;
    for (auto it = cache_.begin(); it != cache_.end(); ) {
        if (it->first < min_page || it->first > max_page) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

RenderedPage ReaderSession::get_page(int index, int target_width, bool auto_crop) {
    std::shared_ptr<ComicReader> local_reader;
    int total_pages = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!reader_ || !reader_->is_open()) return {};
        local_reader = reader_;
        total_pages = reader_->page_count();
    }

    if (index < 0 || index >= total_pages) {
        return {};
    }

    // 1. Check 5-page RAM cache (0 ms hit)
    {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        auto it = cache_.find(index);
        if (it != cache_.end() &&
            it->second.target_width == target_width &&
            it->second.auto_crop == auto_crop &&
            it->second.page.valid()) {
            RenderedPage hit = it->second.page;
            evict_outside_window_locked(index);
            prefetch_center_ = index;
            prefetch_target_width_ = target_width;
            prefetch_auto_crop_ = auto_crop;
            ++prefetch_generation_;
            worker_cv_.notify_one();
            return hit;
        }
    }

    // 2. Cache miss -> pause background lookahead & render requested page immediately
    {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        ++prefetch_generation_;
    }
    active_foreground_requests_.fetch_add(1, std::memory_order_acq_rel);
    struct ForegroundGuard {
        std::atomic<int>& counter;
        std::condition_variable& cv;
        ~ForegroundGuard() {
            if (counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                cv.notify_one();
            }
        }
    } fg_guard{active_foreground_requests_, worker_cv_};

    RenderedPage rendered = local_reader->render_page(index, target_width, auto_crop);
    if (!rendered.valid()) {
        return {};
    }

    // 3. Store in 5-page RAM cache and schedule lookahead (+1, +2, +3, -1)
    {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        CacheEntry entry;
        entry.target_width = target_width;
        entry.auto_crop = auto_crop;
        entry.page = rendered;
        cache_[index] = std::move(entry);
        evict_outside_window_locked(index);

        prefetch_center_ = index;
        prefetch_target_width_ = target_width;
        prefetch_auto_crop_ = auto_crop;
        ++prefetch_generation_;
    }
    worker_cv_.notify_one();

    return rendered;
}

RenderedPage ReaderSession::extract_cover(int max_w, int max_h) {
    std::shared_ptr<ComicReader> local_reader;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!reader_ || !reader_->is_open()) return {};
        local_reader = reader_;
    }
    return local_reader->extract_cover(max_w, max_h);
}

void ReaderSession::worker_loop() {
    // Keep background 5-page lookahead below normal priority so active HTTP page fetches
    // and thumbnail checks always preempt it on SATA 3 HDDs.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    uint64_t last_handled_gen = 0;

    while (true) {
        int center = -1;
        int target_w = 0;
        bool auto_crop = false;
        uint64_t gen = 0;

        {
            std::unique_lock<std::mutex> clock(cache_mutex_);
            worker_cv_.wait(clock, [&]() {
                return stop_worker_ ||
                       (active_foreground_requests_.load(std::memory_order_acquire) == 0 &&
                        prefetch_generation_ != last_handled_gen &&
                        prefetch_center_ >= 0);
            });
            if (stop_worker_) return;

            center = prefetch_center_;
            target_w = prefetch_target_width_;
            auto_crop = prefetch_auto_crop_;
            gen = prefetch_generation_;
            last_handled_gen = gen;
        }

        std::shared_ptr<ComicReader> local_reader;
        int total_pages = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!reader_ || !reader_->is_open()) continue;
            local_reader = reader_;
            total_pages = reader_->page_count();
        }

        // Priority lookahead order: +1, +2, +3, -1 (completing [center-1 .. center+3])
        const std::array<int, 4> offsets = {1, 2, 3, -1};
        for (int off : offsets) {
            const int candidate = center + off;
            if (candidate < 0 || candidate >= total_pages) continue;

            {
                std::unique_lock<std::mutex> clock(cache_mutex_);
                if (stop_worker_ || prefetch_generation_ != gen) break;
                if (active_foreground_requests_.load(std::memory_order_acquire) > 0) {
                    worker_cv_.wait(clock, [&]() {
                        return stop_worker_ ||
                               prefetch_generation_ != gen ||
                               active_foreground_requests_.load(std::memory_order_acquire) == 0;
                    });
                    if (stop_worker_ || prefetch_generation_ != gen) break;
                }
                auto it = cache_.find(candidate);
                if (it != cache_.end() &&
                    it->second.target_width == target_w &&
                    it->second.auto_crop == auto_crop &&
                    it->second.page.valid()) {
                    continue;
                }
            }

            RenderedPage prefetched = local_reader->render_page(candidate, target_w, auto_crop);
            if (!prefetched.valid()) continue;

            std::lock_guard<std::mutex> clock(cache_mutex_);
            if (stop_worker_ || prefetch_generation_ != gen) break;

            CacheEntry entry;
            entry.target_width = target_w;
            entry.auto_crop = auto_crop;
            entry.page = std::move(prefetched);
            cache_[candidate] = std::move(entry);
            evict_outside_window_locked(center);
        }
    }
}

} // namespace koma
