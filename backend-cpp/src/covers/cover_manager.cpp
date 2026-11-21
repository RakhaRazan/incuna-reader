#include "covers/cover_manager.hpp"

#include "formats/comic_reader.hpp"
#include "formats/image_utils.hpp"
#include "portable_paths.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstring>

namespace koma {

namespace {

bool is_webp_bytes(const std::vector<uint8_t>& d) {
    return d.size() >= 12 &&
           std::memcmp(d.data(), "RIFF", 4) == 0 &&
           std::memcmp(d.data() + 8, "WEBP", 4) == 0;
}

} // namespace

CoverManager::CoverManager(Database& db,
                           SettingsManager& settings,
                           std::filesystem::path covers_pack_path,
                           std::filesystem::path stream_cache_dir)
    : db_(db),
      settings_(settings),
      covers_pack_path_(std::move(covers_pack_path)),
      stream_cache_dir_(std::move(stream_cache_dir)) {}

CoverManager::~CoverManager() {
    stop();
}

void CoverManager::start() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (running_.load(std::memory_order_acquire)) return;
        running_.store(true, std::memory_order_release);
        stop_workers_ = false;
        workers_.reserve(kWorkerThreads);
        for (int i = 0; i < kWorkerThreads; ++i) {
            workers_.emplace_back(&CoverManager::worker_loop, this);
        }
    }

    if (is_persistent_pack_mode()) {
        start_pack_build();
    }
}

void CoverManager::stop() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    stop_pack_pipeline();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_workers_ = true;
        while (!viewport_queue_.empty()) {
            auto job = std::move(viewport_queue_.front());
            viewport_queue_.pop_front();
            if (job.promise) {
                CoverResult cancelled;
                cancelled.cancelled = true;
                job.promise->set_value(std::move(cancelled));
            }
        }
        cancelled_ids_.clear();

        std::lock_guard<std::mutex> st_lock(pack_status_mutex_);
        indexed_count_.store(0, std::memory_order_release);
        total_to_index_.store(0, std::memory_order_release);
        pack_status_.building = false;
        pack_status_.total = 0;
        pack_status_.processed = 0;
        pack_status_.succeeded = 0;
        pack_status_.failed = 0;
    }
    queue_cv_.notify_all();

    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
    flush_pending_pack_batch(/*force=*/true);
    running_.store(false, std::memory_order_release);
    lru_clear();
}

void CoverManager::clear_queue() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    stop_pack_pipeline();
    discard_pending_pack_batch();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!viewport_queue_.empty()) {
            auto job = std::move(viewport_queue_.front());
            viewport_queue_.pop_front();
            if (job.promise) {
                CoverResult cancelled;
                cancelled.cancelled = true;
                job.promise->set_value(std::move(cancelled));
            }
        }
        cancelled_ids_.clear();

        std::lock_guard<std::mutex> st_lock(pack_status_mutex_);
        indexed_count_.store(0, std::memory_order_release);
        total_to_index_.store(0, std::memory_order_release);
        pack_status_.building = false;
        pack_status_.total = 0;
        pack_status_.processed = 0;
        pack_status_.succeeded = 0;
        pack_status_.failed = 0;
    }

    lru_clear();
    queue_cv_.notify_all();
}

bool CoverManager::is_persistent_pack_mode() const {
    auto cfg = settings_.get();
    return cfg.contains("cover_mode") &&
           cfg["cover_mode"].is_string() &&
           cfg["cover_mode"].get<std::string>() == "persistent_pack";
}

bool CoverManager::is_in_active_viewport(int64_t comic_id) const {
    if (!has_viewport_filter_.load(std::memory_order_acquire)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(viewport_ids_mutex_);
    return active_viewport_ids_.count(comic_id) > 0;
}

bool CoverManager::lru_get(int64_t comic_id, CoverResult& out) {
    // Shared read lock: concurrent HTTP RAM cache hits complete in <0.2 ms without blocking behind HDD reads
    std::shared_lock<std::shared_mutex> lock(lru_mutex_);
    auto it = lru_map_.find(comic_id);
    if (it == lru_map_.end()) return false;

    out.found = true;
    out.mime_type = it->second->mime_type;
    out.data = it->second->data;
    out.source = "ram_lru";
    return true;
}

void CoverManager::lru_put(int64_t comic_id, const std::string& mime_type, const std::vector<uint8_t>& data) {
    if (data.empty() || data.size() > kMaxRamCacheBytes) return;

    std::unique_lock<std::shared_mutex> lock(lru_mutex_);
    auto existing = lru_map_.find(comic_id);
    if (existing != lru_map_.end()) {
        lru_bytes_ -= existing->second->data.size();
        lru_list_.erase(existing->second);
        lru_map_.erase(existing);
    }

    while (lru_bytes_ + data.size() > kMaxRamCacheBytes && !lru_list_.empty()) {
        const auto& tail = lru_list_.back();
        lru_bytes_ -= tail.data.size();
        lru_map_.erase(tail.comic_id);
        lru_list_.pop_back();
    }

    LruEntry entry;
    entry.comic_id = comic_id;
    entry.mime_type = mime_type;
    entry.data = data;
    lru_bytes_ += entry.data.size();
    lru_list_.push_front(std::move(entry));
    lru_map_[comic_id] = lru_list_.begin();
}

void CoverManager::lru_clear() {
    std::unique_lock<std::shared_mutex> lock(lru_mutex_);
    lru_list_.clear();
    lru_map_.clear();
    lru_bytes_ = 0;
}

size_t CoverManager::ram_cache_bytes() const {
    std::shared_lock<std::shared_mutex> lock(lru_mutex_);
    return lru_bytes_;
}

size_t CoverManager::ram_cache_count() const {
    std::shared_lock<std::shared_mutex> lock(lru_mutex_);
    return lru_list_.size();
}

bool CoverManager::read_from_pack(int64_t offset, int64_t size, CoverResult& out) {
    if (offset < 0 || size <= 0 || size > 32LL * 1024LL * 1024LL) return false;

    std::lock_guard<std::mutex> lock(pack_io_mutex_);
    HANDLE h = CreateFileW(
        covers_pack_path_.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER li{};
    li.QuadPart = offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return false;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(size));
    size_t total = 0;
    while (total < buf.size()) {
        DWORD got = 0;
        const DWORD to_read = static_cast<DWORD>(buf.size() - total);
        if (!ReadFile(h, buf.data() + total, to_read, &got, nullptr) || got == 0) {
            CloseHandle(h);
            return false;
        }
        total += got;
    }
    CloseHandle(h);

    out.found = true;
    out.mime_type = is_webp_bytes(buf) ? "image/webp" : "image/jpeg";
    out.data = std::move(buf);
    out.source = "covers_pack";
    return true;
}

bool CoverManager::append_to_pack(int64_t comic_id,
                                  const std::vector<uint8_t>& data,
                                  uint64_t expected_generation) {
    if (data.empty()) return false;

    std::lock_guard<std::mutex> lock(pack_io_mutex_);
    if (generation_.load(std::memory_order_acquire) != expected_generation) {
        return false;
    }

    const int64_t rel_offset = static_cast<int64_t>(pending_pack_bytes_.size());
    const int64_t sz = static_cast<int64_t>(data.size());
    pending_pack_bytes_.insert(pending_pack_bytes_.end(), data.begin(), data.end());
    pending_pack_updates_.push_back(CoverPackUpdate{comic_id, rel_offset, sz});

    if (pending_pack_updates_.size() >= kCoverDbBatchSize) {
        return flush_pending_pack_batch_locked(/*force=*/true);
    }
    return true;
}

void CoverManager::queue_failed_cover(int64_t comic_id, uint64_t expected_generation) {
    std::lock_guard<std::mutex> lock(pack_io_mutex_);
    if (generation_.load(std::memory_order_acquire) != expected_generation) {
        return;
    }
    pending_pack_updates_.push_back(CoverPackUpdate{comic_id, -1, -1});
    if (pending_pack_updates_.size() >= kCoverDbBatchSize) {
        flush_pending_pack_batch_locked(/*force=*/true);
    }
}

bool CoverManager::flush_pending_pack_batch(bool force) {
    std::lock_guard<std::mutex> lock(pack_io_mutex_);
    return flush_pending_pack_batch_locked(force);
}

bool CoverManager::flush_pending_pack_batch_locked(bool force) {
    if (pending_pack_updates_.empty()) return true;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pack_flush_).count();
    if (!force && pending_pack_updates_.size() < kCoverDbBatchSize && elapsed_ms < kCoverDbFlushIntervalMs) {
        return true;
    }

    int64_t base_offset = 0;
    if (!pending_pack_bytes_.empty()) {
        std::error_code ec;
        if (covers_pack_path_.has_parent_path()) {
            std::filesystem::create_directories(covers_pack_path_.parent_path(), ec);
        }

        HANDLE h = CreateFileW(
            covers_pack_path_.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            pending_pack_bytes_.clear();
            pending_pack_updates_.clear();
            last_pack_flush_ = now;
            return false;
        }

        LARGE_INTEGER zero{};
        LARGE_INTEGER end_pos{};
        if (!SetFilePointerEx(h, zero, &end_pos, FILE_END)) {
            CloseHandle(h);
            pending_pack_bytes_.clear();
            pending_pack_updates_.clear();
            last_pack_flush_ = now;
            return false;
        }
        base_offset = end_pos.QuadPart;

        size_t total_written = 0;
        bool write_ok = true;
        while (total_written < pending_pack_bytes_.size()) {
            const size_t remaining = pending_pack_bytes_.size() - total_written;
            const DWORD chunk_size = static_cast<DWORD>(std::min<size_t>(remaining, 16ULL * 1024ULL * 1024ULL));
            DWORD written = 0;
            if (!WriteFile(h, pending_pack_bytes_.data() + total_written, chunk_size, &written, nullptr) ||
                written == 0) {
                write_ok = false;
                break;
            }
            total_written += written;
        }
        CloseHandle(h);

        if (!write_ok) {
            pending_pack_bytes_.clear();
            pending_pack_updates_.clear();
            last_pack_flush_ = now;
            return false;
        }

        cached_pack_size_bytes_ = base_offset + static_cast<int64_t>(pending_pack_bytes_.size());
        for (auto& u : pending_pack_updates_) {
            if (u.offset >= 0 && u.size > 0) {
                u.offset += base_offset;
            }
        }
        pending_pack_bytes_.clear();
    }

    std::vector<CoverPackUpdate> to_commit;
    to_commit.swap(pending_pack_updates_);
    last_pack_flush_ = now;

    return db_.batch_update_comic_cover_pack(to_commit);
}

void CoverManager::discard_pending_pack_batch() {
    std::lock_guard<std::mutex> lock(pack_io_mutex_);
    pending_pack_bytes_.clear();
    pending_pack_updates_.clear();
    last_pack_flush_ = std::chrono::steady_clock::now();
}

bool CoverManager::has_pending_pack_batch() const {
    std::lock_guard<std::mutex> lock(pack_io_mutex_);
    return !pending_pack_updates_.empty();
}

bool CoverManager::try_windows_shell_cache(const std::wstring& path, CoverResult& out) {
    // Query Windows Explorer thumbnail cache strictly with SIIGBF_INCACHEONLY (0 disk thrashing)
    HRESULT hr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool need_uninit = SUCCEEDED(hr_init);

    IShellItemImageFactory* factory = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (need_uninit) CoUninitialize();
        return false;
    }

    HBITMAP hbmp = nullptr;
    SIZE sz{kCoverMaxWidth, kCoverMaxHeight};
    hr = factory->GetImage(sz, SIIGBF_INCACHEONLY | SIIGBF_THUMBNAILONLY, &hbmp);
    factory->Release();

    if (FAILED(hr) || !hbmp) {
        if (need_uninit) CoUninitialize();
        return false;
    }

    BITMAP bm{};
    if (GetObjectW(hbmp, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        DeleteObject(hbmp);
        if (need_uninit) CoUninitialize();
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -bm.bmHeight; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    DecodedImage decoded;
    decoded.width = bm.bmWidth;
    decoded.height = bm.bmHeight;
    decoded.rgba.resize(static_cast<size_t>(bm.bmWidth) * static_cast<size_t>(bm.bmHeight) * 4);

    HDC hdc = GetDC(nullptr);
    const int lines = GetDIBits(hdc, hbmp, 0, static_cast<UINT>(bm.bmHeight), decoded.rgba.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    DeleteObject(hbmp);
    if (need_uninit) CoUninitialize();

    if (lines <= 0) return false;

    // Convert BGRA -> RGBA
    const size_t px_count = static_cast<size_t>(decoded.width) * static_cast<size_t>(decoded.height);
    for (size_t i = 0; i < px_count; ++i) {
        std::swap(decoded.rgba[i * 4 + 0], decoded.rgba[i * 4 + 2]);
        decoded.rgba[i * 4 + 3] = 255;
    }

    auto encoded = encode_rendered_page(decoded, 0, decoded.width, decoded.height, /*prefer_webp=*/true, 82);
    if (!encoded.valid()) return false;

    out.found = true;
    out.mime_type = encoded.mime_type;
    out.data = std::move(encoded.data);
    out.source = "shell_cache";
    return true;
}

CoverResult CoverManager::extract_from_comic(const std::wstring& path, int max_w, int max_h) {
    CoverResult res;
    auto reader = create_comic_reader(path, stream_cache_dir_);
    if (!reader || !reader->open(path)) {
        return res;
    }
    auto cover = reader->extract_cover(max_w, max_h);
    reader->close();

    if (!cover.valid()) return res;
    res.found = true;
    res.mime_type = std::move(cover.mime_type);
    res.data = std::move(cover.data);
    res.source = "extracted";
    return res;
}

std::wstring CoverManager::resolve_effective_path(const ComicRecord& comic) {
    std::wstring effective_path = utf8_to_wide(comic.path);
    if (comic.is_offline_pinned && !comic.offline_path.empty()) {
        std::error_code ec;
        const std::wstring offline_w = utf8_to_wide(comic.offline_path);
        if (std::filesystem::exists(offline_w, ec)) {
            effective_path = offline_w;
        }
    }
    return effective_path;
}

std::wstring CoverManager::resolve_effective_path_row(const UnindexedCoverRow& row) {
    std::wstring effective_path = utf8_to_wide(row.path);
    if (row.is_offline_pinned && !row.offline_path.empty()) {
        std::error_code ec;
        const std::wstring offline_w = utf8_to_wide(row.offline_path);
        if (std::filesystem::exists(offline_w, ec)) {
            effective_path = offline_w;
        }
    }
    return effective_path;
}

CoverResult CoverManager::get_cover(int64_t comic_id) {
    CoverResult out;

    // 1. Check 64 MB RAM LRU cache first (0 ms)
    if (lru_get(comic_id, out)) {
        return out;
    }

    ComicRecord comic;
    if (!db_.get_comic(comic_id, comic)) {
        return out;
    }

    const bool persistent = is_persistent_pack_mode();

    // 2. In persistent_pack mode, check O(1) covers.pack offset first
    if (persistent) {
        if (comic.cover_pack_size == -1) {
            // Permanently failed (corrupt or password-protected); skip cleanly
            return out;
        }
        if (comic.cover_pack_offset >= 0 && comic.cover_pack_size > 0) {
            if (read_from_pack(comic.cover_pack_offset, comic.cover_pack_size, out)) {
                lru_put(comic_id, out.mime_type, out.data);
                return out;
            }
        }
    }

    // Resolve effective path (prefer local offline_path if pinned and exists)
    std::wstring effective_path = resolve_effective_path(comic);

    // 3. In ram_only mode, check Windows Explorer thumbnail cache (SIIGBF_INCACHEONLY)
    if (!persistent) {
        if (try_windows_shell_cache(effective_path, out)) {
            lru_put(comic_id, out.mime_type, out.data);
            return out;
        }
    }

    // 4. Enqueue at the FRONT of the high-priority viewport_queue_ so visible
    //    screen cards always cut ahead of background covers.pack indexing.
    if (!running_.load(std::memory_order_acquire)) {
        start();
    }

    auto promise = std::make_shared<std::promise<CoverResult>>();
    auto future = promise->get_future();

    // Auto-register direct GET /api/covers/{id} requests in active_viewport_ids_
    // so initial loads and folder preview_comic_id covers are never dropped before
    // POST /api/covers/viewport arrives.
    {
        std::lock_guard<std::mutex> vlock(viewport_ids_mutex_);
        active_viewport_ids_.insert(comic_id);
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        cancelled_ids_.erase(comic_id); // Fresh request clears any prior cancel
        ExtractJob job;
        job.comic_id = comic_id;
        job.file_path = std::move(effective_path);
        job.write_to_pack = persistent;
        job.generation = generation_.load(std::memory_order_acquire);
        job.promise = std::move(promise);
        viewport_queue_.push_front(std::move(job));
    }
    queue_cv_.notify_one();

    return future.get();
}

void CoverManager::cancel_ids(const std::vector<int64_t>& ids) {
    if (ids.empty()) return;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (int64_t id : ids) {
        cancelled_ids_.insert(id);
    }

    // Immediately abort matching queued viewport jobs
    for (auto it = viewport_queue_.begin(); it != viewport_queue_.end(); ) {
        if (cancelled_ids_.count(it->comic_id) > 0) {
            if (it->promise) {
                CoverResult c;
                c.cancelled = true;
                it->promise->set_value(std::move(c));
            }
            it = viewport_queue_.erase(it);
        } else {
            ++it;
        }
    }
}

void CoverManager::set_active_viewport(const std::vector<int64_t>& visible_ids) {
    // Only replace active_viewport_ids_ when an explicit non-empty list of visible IDs is provided
    if (visible_ids.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> vlock(viewport_ids_mutex_);
        active_viewport_ids_.clear();
        active_viewport_ids_.reserve(visible_ids.size());
        for (int64_t id : visible_ids) {
            active_viewport_ids_.insert(id);
        }
        has_viewport_filter_.store(true, std::memory_order_release);
    }

    // Immediately drop queued viewport tasks that scrolled off-screen before touching disk
    std::lock_guard<std::mutex> qlock(queue_mutex_);
    for (auto it = viewport_queue_.begin(); it != viewport_queue_.end(); ) {
        if (!is_in_active_viewport(it->comic_id)) {
            if (it->promise) {
                CoverResult c;
                c.cancelled = true;
                it->promise->set_value(std::move(c));
            }
            it = viewport_queue_.erase(it);
        } else {
            ++it;
        }
    }
}

void CoverManager::worker_loop() {
    while (true) {
        ExtractJob vp_job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            auto ready = [&]() {
                return stop_workers_ || !viewport_queue_.empty();
            };
            if (!ready()) {
                if (has_pending_pack_batch()) {
                    queue_cv_.wait_for(lock, std::chrono::milliseconds(500), ready);
                } else {
                    queue_cv_.wait(lock, ready);
                }
            }
            if (stop_workers_ && viewport_queue_.empty()) {
                return;
            }
            if (viewport_queue_.empty()) {
                lock.unlock();
                flush_pending_pack_batch(/*force=*/false);
                continue;
            }

            vp_job = std::move(viewport_queue_.front());
            viewport_queue_.pop_front();

            if (cancelled_ids_.erase(vp_job.comic_id) > 0 ||
                generation_.load(std::memory_order_acquire) != vp_job.generation ||
                !is_in_active_viewport(vp_job.comic_id)) {
                if (vp_job.promise) {
                    CoverResult c;
                    c.cancelled = true;
                    vp_job.promise->set_value(std::move(c));
                }
                continue;
            }
        }

        CoverResult res;
        if (!lru_get(vp_job.comic_id, res)) {
            // Mode 1 pre-seek viewport verification immediately before touching HDD
            if (!is_in_active_viewport(vp_job.comic_id)) {
                res.cancelled = true;
                if (vp_job.promise) {
                    vp_job.promise->set_value(std::move(res));
                }
                continue;
            }

            bool comic_exists = true;
            bool marked_failed = false;
            if (vp_job.write_to_pack) {
                ComicRecord fresh;
                comic_exists = db_.get_comic(vp_job.comic_id, fresh);
                if (comic_exists && fresh.cover_pack_size == -1) {
                    marked_failed = true;
                } else if (comic_exists && fresh.cover_pack_offset >= 0 && fresh.cover_pack_size > 0) {
                    if (read_from_pack(fresh.cover_pack_offset, fresh.cover_pack_size, res)) {
                        lru_put(vp_job.comic_id, res.mime_type, res.data);
                    }
                }
            }
            if (comic_exists && !marked_failed && !res.found) {
                if (!is_in_active_viewport(vp_job.comic_id)) {
                    res.cancelled = true;
                } else {
                    res = extract_from_comic(vp_job.file_path);
                    if (generation_.load(std::memory_order_acquire) != vp_job.generation) {
                        res = CoverResult{};
                        res.cancelled = true;
                    } else if (res.found) {
                        lru_put(vp_job.comic_id, res.mime_type, res.data);
                        if (vp_job.write_to_pack) {
                            append_to_pack(vp_job.comic_id, res.data, vp_job.generation);
                        }
                    } else if (vp_job.write_to_pack) {
                        queue_failed_cover(vp_job.comic_id, vp_job.generation);
                    }
                }
            }
        }

        if (vp_job.promise) {
            vp_job.promise->set_value(std::move(res));
        }
    }
}

void CoverManager::stop_pack_pipeline() {
    {
        std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
        stop_pack_pipeline_ = true;
        pack_rescan_requested_ = false;
    }
    pack_pipeline_cv_.notify_all();
    pack_space_cv_.notify_all();

    if (pack_reader_thread_.joinable()) {
        pack_reader_thread_.join();
    }
    for (auto& t : pack_decode_workers_) {
        if (t.joinable()) t.join();
    }
    pack_decode_workers_.clear();

    std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
    raw_cover_queue_.clear();
    pack_pipeline_active_ = false;
    pack_reader_done_ = false;
    active_pack_decoders_ = 0;
}

void CoverManager::pack_reader_loop(uint64_t gen) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    std::string last_path;
    while (true) {
        if (generation_.load(std::memory_order_acquire) != gen) break;
        {
            std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
            if (stop_pack_pipeline_) break;
        }

        // Fetch 1,000 unindexed comics in sequential disk order, releasing SQLite mutex immediately
        auto chunk = db_.get_unindexed_covers_chunk(kPackFetchChunkSize, last_path);
        if (chunk.empty()) {
            std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
            if (pack_rescan_requested_ && !stop_pack_pipeline_ &&
                generation_.load(std::memory_order_acquire) == gen) {
                pack_rescan_requested_ = false;
                last_path.clear();
                continue;
            }
            break;
        }

        for (const auto& row : chunk) {
            last_path = row.path;
            if (generation_.load(std::memory_order_acquire) != gen) break;
            {
                std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
                if (stop_pack_pipeline_) break;
            }

            RawCoverWork work;
            work.comic_id = row.id;
            work.file_path = resolve_effective_path_row(row);
            work.generation = gen;

            // Sequential 1-thread HDD read for CBZ/EPUB Page 0 raw bytes (zero CPU decode on disk thread)
            if (row.format == "cbz" || row.format == "epub") {
                extract_first_image_bytes_fast(work.file_path, work.raw_bytes);
            }

            std::unique_lock<std::mutex> lock(pack_pipeline_mutex_);
            pack_space_cv_.wait(lock, [&]() {
                return stop_pack_pipeline_ ||
                       generation_.load(std::memory_order_acquire) != gen ||
                       raw_cover_queue_.size() < kPackRawQueueMax;
            });
            if (stop_pack_pipeline_ || generation_.load(std::memory_order_acquire) != gen) {
                break;
            }
            raw_cover_queue_.push_back(std::move(work));
            lock.unlock();
            pack_pipeline_cv_.notify_one();
        }
    }

    {
        std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
        pack_reader_done_ = true;
    }
    pack_pipeline_cv_.notify_all();
}

void CoverManager::pack_decode_worker_loop(uint64_t gen) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    while (true) {
        RawCoverWork work;
        {
            std::unique_lock<std::mutex> lock(pack_pipeline_mutex_);
            pack_pipeline_cv_.wait(lock, [&]() {
                return stop_pack_pipeline_ ||
                       generation_.load(std::memory_order_acquire) != gen ||
                       !raw_cover_queue_.empty() ||
                       pack_reader_done_;
            });

            if (stop_pack_pipeline_ || generation_.load(std::memory_order_acquire) != gen) {
                break;
            }
            if (raw_cover_queue_.empty()) {
                if (pack_reader_done_) break;
                continue;
            }

            work = std::move(raw_cover_queue_.front());
            raw_cover_queue_.pop_front();
        }
        pack_space_cv_.notify_one();

        bool ok = false;
        CoverResult res;
        if (!work.raw_bytes.empty()) {
            // Decode & resize to 200x280 JPEG quality 78 in parallel on CPU worker
            auto rendered = process_raw_image_page(
                work.raw_bytes.data(),
                work.raw_bytes.size(),
                0,
                kPackThumbWidth,
                kPackThumbHeight,
                /*auto_crop=*/false,
                /*prefer_webp=*/false,
                kPackJpegQuality);
            if (rendered.valid()) {
                res.found = true;
                res.mime_type = std::move(rendered.mime_type);
                res.data = std::move(rendered.data);
                res.source = "extracted";
            }
        }
        if (!res.found) {
            res = extract_from_comic(work.file_path, kPackThumbWidth, kPackThumbHeight);
        }

        const bool stale = (generation_.load(std::memory_order_acquire) != gen);
        if (!stale) {
            if (res.found) {
                ok = append_to_pack(work.comic_id, res.data, gen);
                if (ok) {
                    lru_put(work.comic_id, res.mime_type, res.data);
                }
            } else {
                queue_failed_cover(work.comic_id, gen);
            }

            int64_t pack_sz = 0;
            {
                std::lock_guard<std::mutex> io_lock(pack_io_mutex_);
                pack_sz = cached_pack_size_bytes_;
            }

            std::lock_guard<std::mutex> st_lock(pack_status_mutex_);
            ++pack_status_.processed;
            indexed_count_.fetch_add(1, std::memory_order_acq_rel);
            if (ok) ++pack_status_.succeeded;
            else ++pack_status_.failed;
            pack_status_.pack_size_bytes = pack_sz;
        }
    }

    // Last decode worker flushes any remaining partial batch (<100) and marks build complete
    bool is_last_decoder = false;
    {
        std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
        if (active_pack_decoders_ > 0) {
            --active_pack_decoders_;
        }
        if (active_pack_decoders_ == 0) {
            is_last_decoder = true;
        }
    }

    if (is_last_decoder) {
        if (generation_.load(std::memory_order_acquire) == gen) {
            flush_pending_pack_batch(/*force=*/true);
            int64_t flushed_sz = 0;
            {
                std::lock_guard<std::mutex> io_lock(pack_io_mutex_);
                flushed_sz = cached_pack_size_bytes_;
            }
            std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
            std::lock_guard<std::mutex> st_lock(pack_status_mutex_);
            pack_status_.pack_size_bytes = flushed_sz;
            pack_pipeline_active_ = false;
            pack_status_.building = false;
        }
    }
}

bool CoverManager::start_pack_build() {
    // Ensure the 2-thread viewport worker pool is running
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!running_.load(std::memory_order_acquire)) {
            running_.store(true, std::memory_order_release);
            stop_workers_ = false;
            workers_.reserve(kWorkerThreads);
            for (int i = 0; i < kWorkerThreads; ++i) {
                workers_.emplace_back(&CoverManager::worker_loop, this);
            }
        }
    }

    std::lock_guard<std::mutex> build_lock(pack_build_mutex_);
    {
        std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
        if (pack_pipeline_active_) {
            pack_rescan_requested_ = true;
            const int64_t remaining = db_.count_unindexed_covers();
            std::lock_guard<std::mutex> st_lock(pack_status_mutex_);
            if (pack_status_.processed + remaining > pack_status_.total) {
                pack_status_.total = pack_status_.processed + remaining;
                total_to_index_.store(pack_status_.total, std::memory_order_release);
            }
            pack_pipeline_cv_.notify_all();
            return true;
        }
    }

    // Join any completed threads from a previous build run
    if (pack_reader_thread_.joinable()) {
        pack_reader_thread_.join();
    }
    for (auto& t : pack_decode_workers_) {
        if (t.joinable()) t.join();
    }
    pack_decode_workers_.clear();

    const int64_t unindexed = db_.count_unindexed_covers();
    if (unindexed <= 0) {
        std::lock_guard<std::mutex> st_lock(pack_status_mutex_);
        pack_status_.building = false;
        return false;
    }

    const uint64_t gen = generation_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(pack_pipeline_mutex_);
        raw_cover_queue_.clear();
        stop_pack_pipeline_ = false;
        pack_reader_done_ = false;
        pack_rescan_requested_ = false;
        pack_pipeline_active_ = true;
        active_pack_decoders_ = kPackDecodeWorkers;

        std::lock_guard<std::mutex> st_lock(pack_status_mutex_);
        pack_status_.building = true;
        pack_status_.total = unindexed;
        pack_status_.processed = 0;
        pack_status_.succeeded = 0;
        pack_status_.failed = 0;
        total_to_index_.store(unindexed, std::memory_order_release);
        indexed_count_.store(0, std::memory_order_release);
    }

    pack_decode_workers_.reserve(kPackDecodeWorkers);
    for (int i = 0; i < kPackDecodeWorkers; ++i) {
        pack_decode_workers_.emplace_back(&CoverManager::pack_decode_worker_loop, this, gen);
    }
    pack_reader_thread_ = std::thread(&CoverManager::pack_reader_loop, this, gen);
    return true;
}

CoverPackStatus CoverManager::get_pack_status() const {
    std::lock_guard<std::mutex> lock(pack_status_mutex_);
    CoverPackStatus st = pack_status_;
    std::error_code ec;
    if (std::filesystem::exists(covers_pack_path_, ec)) {
        st.pack_size_bytes = static_cast<int64_t>(std::filesystem::file_size(covers_pack_path_, ec));
    } else {
        st.pack_size_bytes = 0;
    }
    return st;
}

bool CoverManager::purge_pack() {
    clear_queue();

    {
        std::lock_guard<std::mutex> lock(pack_io_mutex_);
        std::error_code ec;
        std::filesystem::remove(covers_pack_path_, ec);
        cached_pack_size_bytes_ = 0;
    }

    const bool db_ok = db_.reset_all_cover_pack_offsets();

    {
        std::lock_guard<std::mutex> lock(pack_status_mutex_);
        pack_status_ = CoverPackStatus{};
    }
    return db_ok;
}

bool CoverManager::rebuild_pack() {
    clear_queue();

    {
        std::lock_guard<std::mutex> lock(pack_io_mutex_);
        std::error_code ec;
        if (covers_pack_path_.has_parent_path()) {
            std::filesystem::create_directories(covers_pack_path_.parent_path(), ec);
        }
        HANDLE h = CreateFileW(
            covers_pack_path_.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        } else {
            std::filesystem::remove(covers_pack_path_, ec);
        }
        cached_pack_size_bytes_ = 0;
    }

    const bool db_ok = db_.reset_all_cover_pack_offsets();

    {
        std::lock_guard<std::mutex> lock(pack_status_mutex_);
        pack_status_ = CoverPackStatus{};
    }

    if (!db_ok) return false;
    if (is_persistent_pack_mode()) {
        return start_pack_build();
    }
    return false;
}

} // namespace koma
