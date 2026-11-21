#include "watcher/library_scanner.hpp"

#include "covers/cover_manager.hpp"
#include "formats/metadata_extractor.hpp"
#include "utils/string_utils.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace koma {

namespace {

bool is_network_path_wide(const std::wstring& p) {
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
        return true; // UNC \\NAS\...
    }
    if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) {
        std::wstring root = p.substr(0, 2) + L"\\";
        return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
    }
    return false;
}

std::string extract_drive_prefix(const std::string& path) {
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        std::string p;
        p.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(path[0]))));
        p.push_back(':');
        p.push_back(path[2]);
        return p;
    }
    return {};
}

void populate_comic_metadata(const std::filesystem::path& file_path,
                             const std::string& format,
                             const std::string& filename_stem,
                             ComicUpsert& item) {
    ComicMetadata meta;
    extract_comic_metadata(file_path, format, filename_stem, meta);
    item.series   = std::move(meta.series);
    item.author   = std::move(meta.author);
    item.tags     = std::move(meta.tags);
    item.language = std::move(meta.language);
}

} // namespace

LibraryScanner::LibraryScanner(Database& db,
                               std::filesystem::path exe_dir,
                               CoverManager* cover_manager,
                               SettingsManager* settings)
    : db_(db), exe_dir_(std::move(exe_dir)), cover_manager_(cover_manager), settings_(settings) {}

LibraryScanner::~LibraryScanner() {
    cancel_scan_.store(true);
    {
        std::lock_guard<std::mutex> lock(scan_queue_mutex_);
        stop_scanner_ = true;
        scan_queue_.clear();
    }
    scan_queue_cv_.notify_all();
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }
    stop_all_watchers();
}

bool LibraryScanner::is_indexable_comic_file(const std::filesystem::path& p, std::string& out_format) {
    const std::wstring ext = to_lower_wide(p.extension().wstring());
    if (ext == L".cbz" || ext == L".zip") { out_format = "cbz"; return true; }
    if (ext == L".cbr" || ext == L".rar") { out_format = "cbr"; return true; }
    if (ext == L".cb7" || ext == L".7z")  { out_format = "cb7"; return true; }
    if (ext == L".cbt" || ext == L".tar") { out_format = "cbt"; return true; }
    if (ext == L".pdf")                   { out_format = "pdf"; return true; }
    if (ext == L".epub")                  { out_format = "epub"; return true; }
    return false;
}

bool LibraryScanner::is_folder_online(const std::wstring& folder_path) {
    if (folder_path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(folder_path, ec) &&
           std::filesystem::is_directory(folder_path, ec);
}

int64_t LibraryScanner::prune_orphans_for_folder(
    const std::wstring& root_folder,
    const std::unordered_set<std::string>* seen_paths_lower) {
    // Unplugged Drive / Offline Network Guard: never prune if watched root is offline.
    if (!is_folder_online(root_folder)) {
        return 0;
    }

    const std::string root_utf8 = wide_to_utf8(root_folder);
    const auto candidates = db_.get_unpinned_comics_under_prefix(root_utf8);
    if (candidates.empty()) return 0;

    std::vector<int64_t> to_delete;
    std::error_code ec;
    for (const auto& [id, path_utf8] : candidates) {
        if (seen_paths_lower && seen_paths_lower->count(to_lower_ascii(path_utf8)) > 0) {
            continue;
        }
        if (!std::filesystem::exists(utf8_to_wide(path_utf8), ec)) {
            to_delete.push_back(id);
        }
    }

    if (to_delete.empty()) return 0;

    // Re-verify watched root is still mounted right before deleting from SQLite.
    if (!is_folder_online(root_folder)) {
        return 0;
    }

    const int64_t deleted = db_.delete_comics_by_ids_if_unpinned(to_delete);
    if (deleted > 0 && cover_manager_) {
        cover_manager_->clear_queue();
    }
    return deleted;
}

int64_t LibraryScanner::prune_all_orphans() {
    auto folders = db_.get_watched_folders();
    int64_t total_pruned = 0;
    for (const auto& f : folders) {
        const std::wstring wpath = utf8_to_wide(f.path);
        if (!is_folder_online(wpath)) {
            continue; // Preserve all records for unplugged drives or offline \\NAS shares
        }
        total_pruned += prune_orphans_for_folder(wpath, nullptr);
    }
    return total_pruned;
}

int64_t LibraryScanner::auto_remap_drives() {
    const std::string cur_exe_utf8 = wide_to_utf8(exe_dir_.wstring());
    const std::string cur_drive = extract_drive_prefix(cur_exe_utf8);
    if (cur_drive.empty()) return 0;

    auto folders = db_.get_watched_folders();
    std::unordered_set<std::string> checked_prefixes;
    int64_t total_remapped = 0;

    for (const auto& f : folders) {
        if (f.is_network) continue;
        const std::string old_drive = extract_drive_prefix(f.path);
        if (old_drive.empty() || old_drive == cur_drive) continue;
        if (!checked_prefixes.insert(old_drive).second) continue;

        std::error_code ec;
        const std::wstring old_w = utf8_to_wide(f.path);
        if (std::filesystem::exists(old_w, ec)) {
            continue; // Original drive path still exists; don't remap
        }

        const std::string candidate_utf8 = cur_drive + f.path.substr(old_drive.size());
        const std::wstring candidate_w = utf8_to_wide(candidate_utf8);
        if (std::filesystem::exists(candidate_w, ec)) {
            total_remapped += db_.remap_drive_prefix(old_drive, cur_drive);
        }
    }

    if (total_remapped > 0) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.remapped_paths += total_remapped;
    }
    return total_remapped;
}

bool LibraryScanner::start_folder_import(const std::wstring& folder_path) {
    std::error_code ec;
    std::filesystem::path norm_path(folder_path);
    if (!std::filesystem::is_directory(norm_path, ec)) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.last_error = "Folder does not exist or is not a directory";
        return false;
    }
    norm_path = std::filesystem::weakly_canonical(norm_path, ec);

    const std::wstring wnorm = norm_path.wstring();
    const std::wstring lower_key = to_lower_wide(wnorm);
    const std::string utf8_folder = wide_to_utf8(wnorm);
    const std::string folder_name = wide_to_utf8(norm_path.filename().wstring());
    const bool is_net = is_network_path_wide(wnorm);

    WatchedFolderRecord wf;
    db_.upsert_watched_folder(utf8_folder, folder_name.empty() ? utf8_folder : folder_name, is_net, wf);

    {
        std::lock_guard<std::mutex> qlock(scan_queue_mutex_);
        bool already_pending = (scan_job_active_ && active_scan_lower_key_ == lower_key);
        if (!already_pending) {
            for (const auto& queued : scan_queue_) {
                if (queued.lower_key == lower_key) {
                    already_pending = true;
                    break;
                }
            }
        }

        const bool was_idle = !scan_job_active_ && scan_queue_.empty();
        if (!already_pending) {
            scan_queue_.push_back(ScanJob{wnorm, lower_key, utf8_folder, wf.id});
        }

        {
            std::lock_guard<std::mutex> slock(status_mutex_);
            if (was_idle) {
                status_.files_found = 0;
                status_.files_indexed = 0;
                status_.batches_committed = 0;
                status_.current_folder = utf8_folder;
            }
            status_.scanning = true;
            status_.queued_folders = static_cast<int>(scan_queue_.size());
            status_.last_error.clear();
        }

        if (!scan_thread_.joinable()) {
            cancel_scan_.store(false);
            stop_scanner_ = false;
            scan_thread_ = std::thread(&LibraryScanner::scan_worker_loop, this);
        }
    }
    scan_queue_cv_.notify_one();

    // Start real-time ReadDirectoryChangesW watcher for local/synced folders
    if (!is_net) {
        watch_folder(wnorm);
    }

    return true;
}

void LibraryScanner::scan_worker_loop() {
    while (true) {
        ScanJob job;
        {
            std::unique_lock<std::mutex> lock(scan_queue_mutex_);
            scan_queue_cv_.wait(lock, [this]() {
                return stop_scanner_ || !scan_queue_.empty();
            });
            if (stop_scanner_) {
                return;
            }
            job = std::move(scan_queue_.front());
            scan_queue_.pop_front();
            scan_job_active_ = true;
            active_scan_lower_key_ = job.lower_key;

            std::lock_guard<std::mutex> slock(status_mutex_);
            status_.scanning = true;
            status_.current_folder = job.utf8_folder;
            status_.queued_folders = static_cast<int>(scan_queue_.size());
        }

        process_scan_job(job);

        bool queue_now_empty = false;
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> qlock(scan_queue_mutex_);
            scan_job_active_ = false;
            active_scan_lower_key_.clear();
            queue_now_empty = scan_queue_.empty();

            std::lock_guard<std::mutex> slock(status_mutex_);
            status_.queued_folders = static_cast<int>(scan_queue_.size());
            if (queue_now_empty) {
                status_.scanning = false;
            }
            cb = on_scan_complete_;
        }

        if (cb && !cancel_scan_.load()) {
            cb();
        }

        if (queue_now_empty) {
            scan_done_cv_.notify_all();
        }
    }
}

void LibraryScanner::process_scan_job(const ScanJob& job) {
    const std::wstring& root_folder = job.root_folder;
    const int64_t folder_id = job.folder_id;
    const bool extract_metadata = settings_
        ? settings_->get().value("enable_metadata_extractor", true)
        : true;

    std::vector<ComicUpsert> batch;
    batch.reserve(kBatchSize);
    std::unordered_set<std::string> seen_paths_lower;

    std::error_code ec;
    const std::filesystem::path root_path(root_folder);
    auto opts = std::filesystem::directory_options::skip_permission_denied;

    for (std::filesystem::recursive_directory_iterator it(root_path, opts, ec), end;
         it != end && !cancel_scan_.load();
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        std::string fmt;
        const auto& p = entry.path();
        if (!is_indexable_comic_file(p, fmt)) continue;

        ComicUpsert item;
        item.path = wide_to_utf8(p.wstring());
        item.folder_path = wide_to_utf8(p.parent_path().wstring());
        seen_paths_lower.insert(to_lower_ascii(item.path));
        std::error_code rel_ec;
        auto rel = std::filesystem::relative(p, root_path, rel_ec);
        item.relative_path = rel_ec ? wide_to_utf8(p.filename().wstring()) : wide_to_utf8(rel.wstring());
        item.title = wide_to_utf8(p.stem().wstring());
        item.format = std::move(fmt);
        item.file_size_bytes = static_cast<int64_t>(entry.file_size(rel_ec));
        if (extract_metadata) {
            populate_comic_metadata(p, item.format, item.title, item);
        }

        batch.push_back(std::move(item));

        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            ++status_.files_found;
        }

        if (batch.size() >= kBatchSize) {
            if (db_.upsert_comics_batch(batch)) {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.files_indexed += static_cast<int64_t>(batch.size());
                ++status_.batches_committed;
            }
            batch.clear();
        }
    }

    if (!batch.empty() && !cancel_scan_.load()) {
        if (db_.upsert_comics_batch(batch)) {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.files_indexed += static_cast<int64_t>(batch.size());
            ++status_.batches_committed;
        }
        batch.clear();
    }

    if (!cancel_scan_.load() && is_folder_online(root_folder)) {
        prune_orphans_for_folder(root_folder, &seen_paths_lower);
    }

    if (folder_id > 0) {
        db_.update_watched_folder_scanned(folder_id);
    }
}

void LibraryScanner::set_on_scan_complete(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    on_scan_complete_ = std::move(cb);
}

ScanStatus LibraryScanner::get_scan_status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void LibraryScanner::wait_for_scan() {
    std::unique_lock<std::mutex> lock(scan_queue_mutex_);
    scan_done_cv_.wait(lock, [this]() {
        return scan_queue_.empty() && !scan_job_active_;
    });
}

void LibraryScanner::start_watching_all() {
    auto folders = db_.get_watched_folders();
    for (const auto& f : folders) {
        if (f.auto_sync && !f.is_network) {
            watch_folder(utf8_to_wide(f.path));
        }
    }
}

void LibraryScanner::watch_folder(const std::wstring& folder_path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder_path, ec)) return;

    const std::wstring key = to_lower_wide(folder_path);
    std::lock_guard<std::mutex> lock(watchers_mutex_);
    if (watchers_.count(key) > 0) return;

    HANDLE stop_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_ev) return;

    auto handle = std::make_unique<WatcherHandle>();
    handle->folder_path = folder_path;
    handle->stop_event = stop_ev;
    handle->thread = std::thread(&LibraryScanner::watcher_thread_func, this, folder_path, stop_ev);
    watchers_[key] = std::move(handle);
}

void LibraryScanner::unwatch_folder(const std::wstring& folder_path) {
    const std::wstring key = to_lower_wide(folder_path);
    std::unique_ptr<WatcherHandle> handle;
    {
        std::lock_guard<std::mutex> lock(watchers_mutex_);
        auto it = watchers_.find(key);
        if (it == watchers_.end()) return;
        handle = std::move(it->second);
        watchers_.erase(it);
    }

    if (handle) {
        if (handle->stop_event) {
            SetEvent(static_cast<HANDLE>(handle->stop_event));
        }
        if (handle->thread.joinable()) {
            handle->thread.join();
        }
        if (handle->stop_event) {
            CloseHandle(static_cast<HANDLE>(handle->stop_event));
        }
    }
}

bool LibraryScanner::remove_watched_folder(int64_t folder_id) {
    std::wstring folder_path;
    for (const auto& wf : db_.get_watched_folders()) {
        if (wf.id == folder_id) {
            folder_path = utf8_to_wide(wf.path);
            break;
        }
    }

    if (!folder_path.empty()) {
        unwatch_folder(folder_path);
    }

    const bool deleted = db_.delete_watched_folder(folder_id);
    if (deleted && cover_manager_) {
        cover_manager_->clear_queue();
    }
    return deleted;
}

void LibraryScanner::stop_all_watchers() {
    std::unordered_map<std::wstring, std::unique_ptr<WatcherHandle>> to_stop;
    {
        std::lock_guard<std::mutex> lock(watchers_mutex_);
        to_stop.swap(watchers_);
    }

    for (auto& kv : to_stop) {
        if (kv.second->stop_event) {
            SetEvent(static_cast<HANDLE>(kv.second->stop_event));
        }
    }
    for (auto& kv : to_stop) {
        if (kv.second->thread.joinable()) {
            kv.second->thread.join();
        }
        if (kv.second->stop_event) {
            CloseHandle(static_cast<HANDLE>(kv.second->stop_event));
        }
    }
}

void LibraryScanner::watcher_thread_func(std::wstring folder_path, void* stop_event_ptr) {
    HANDLE stop_event = static_cast<HANDLE>(stop_event_ptr);
    HANDLE dir_handle = CreateFileW(
        folder_path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (dir_handle == INVALID_HANDLE_VALUE) {
        return;
    }

    HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!io_event) {
        CloseHandle(dir_handle);
        return;
    }

    std::vector<uint8_t> notify_buf(64 * 1024);
    const std::filesystem::path root_path(folder_path);
    std::wstring pending_old_rename;

    while (true) {
        ResetEvent(io_event);
        OVERLAPPED ov{};
        ov.hEvent = io_event;

        DWORD bytes_returned = 0;
        const BOOL ok = ReadDirectoryChangesW(
            dir_handle,
            notify_buf.data(),
            static_cast<DWORD>(notify_buf.size()),
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytes_returned,
            &ov,
            nullptr);
        if (!ok) break;

        HANDLE wait_handles[2] = {stop_event, io_event};
        const DWORD wait_res = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (wait_res == WAIT_OBJECT_0) {
            CancelIo(dir_handle);
            break;
        }
        if (wait_res != WAIT_OBJECT_0 + 1) {
            break;
        }

        if (!GetOverlappedResult(dir_handle, &ov, &bytes_returned, FALSE) || bytes_returned == 0) {
            continue;
        }

        size_t offset = 0;
        while (offset < bytes_returned) {
            const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(notify_buf.data() + offset);
            const std::wstring rel_name(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
            const std::filesystem::path full_path = root_path / rel_name;

            if (fni->Action == FILE_ACTION_ADDED || fni->Action == FILE_ACTION_MODIFIED) {
                std::string fmt;
                std::error_code ec;
                if (is_indexable_comic_file(full_path, fmt) && std::filesystem::is_regular_file(full_path, ec)) {
                    ComicUpsert item;
                    item.path = wide_to_utf8(full_path.wstring());
                    item.relative_path = wide_to_utf8(rel_name);
                    item.folder_path = wide_to_utf8(full_path.parent_path().wstring());
                    item.title = wide_to_utf8(full_path.stem().wstring());
                    item.format = std::move(fmt);
                    item.file_size_bytes = static_cast<int64_t>(std::filesystem::file_size(full_path, ec));
                    const bool extract_meta = settings_
                        ? settings_->get().value("enable_metadata_extractor", true)
                        : true;
                    if (extract_meta) {
                        populate_comic_metadata(full_path, item.format, item.title, item);
                    }
                    db_.upsert_comics_batch({item});
                }
            } else if (fni->Action == FILE_ACTION_REMOVED) {
                std::string fmt;
                if (is_indexable_comic_file(full_path, fmt)) {
                    db_.delete_comic_by_path(wide_to_utf8(full_path.wstring()));
                }
            } else if (fni->Action == FILE_ACTION_RENAMED_OLD_NAME) {
                pending_old_rename = full_path.wstring();
            } else if (fni->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                std::string fmt;
                if (!pending_old_rename.empty() && is_indexable_comic_file(full_path, fmt)) {
                    db_.rename_comic_path(
                        wide_to_utf8(pending_old_rename),
                        wide_to_utf8(full_path.wstring()),
                        wide_to_utf8(full_path.stem().wstring()));
                }
                pending_old_rename.clear();
            }

            if (fni->NextEntryOffset == 0) break;
            offset += fni->NextEntryOffset;
        }
    }

    CloseHandle(io_event);
    CloseHandle(dir_handle);
}

} // namespace koma
