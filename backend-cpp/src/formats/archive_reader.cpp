#include "formats/comic_reader.hpp"
#include "formats/image_utils.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace koma {

namespace {

// Threshold above which solid .7z/.cbr archives progressively spill to disk stream_cache
constexpr int64_t kSolidStreamCacheThresholdBytes = 1024LL * 1024LL * 1024LL; // 1 GB

inline uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_le64(const uint8_t* p) {
    return static_cast<uint64_t>(read_le32(p)) |
           (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}

std::wstring to_lower_wide(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

bool win64_read_at(HANDLE h, int64_t offset, uint8_t* buf, size_t len) {
    if (h == INVALID_HANDLE_VALUE || offset < 0) return false;
    if (len == 0) return true;

    LARGE_INTEGER li{};
    li.QuadPart = offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        return false;
    }

    size_t total = 0;
    while (total < len) {
        const DWORD to_read = static_cast<DWORD>(std::min<size_t>(len - total, 16 * 1024 * 1024));
        DWORD got = 0;
        if (!ReadFile(h, buf + total, to_read, &got, nullptr) || got == 0) {
            return false;
        }
        total += got;
    }
    return true;
}

bool inflate_raw_deflate(const uint8_t* src, size_t src_len,
                         uint64_t expected_uncompressed,
                         std::vector<uint8_t>& dst) {
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        return false;
    }

    dst.clear();
    if (expected_uncompressed > 0 && expected_uncompressed < 256ULL * 1024ULL * 1024ULL) {
        dst.reserve(static_cast<size_t>(expected_uncompressed));
    }

    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
    zs.avail_in = static_cast<uInt>(src_len);

    uint8_t chunk[65536];
    int ret = Z_OK;
    while (ret == Z_OK) {
        zs.next_out = chunk;
        zs.avail_out = sizeof(chunk);
        ret = inflate(&zs, Z_NO_FLUSH);
        const size_t have = sizeof(chunk) - zs.avail_out;
        if (have > 0) {
            dst.insert(dst.end(), chunk, chunk + have);
        }
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK) {
            inflateEnd(&zs);
            dst.clear();
            return false;
        }
    }

    inflateEnd(&zs);
    return ret == Z_STREAM_END;
}

std::string normalize_zip_path(const std::string& p) {
    std::string out = p;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

std::string resolve_relative_zip_path(const std::string& base_dir, const std::string& href) {
    std::string combined = base_dir.empty() ? href : (base_dir + "/" + href);
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= combined.size()) {
        size_t slash = combined.find('/', start);
        std::string seg = (slash == std::string::npos)
            ? combined.substr(start)
            : combined.substr(start, slash - start);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string res;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) res.push_back('/');
        res += parts[i];
    }
    return res;
}

// 64-bit Win32 custom callback state for libarchive
struct Win64ArchiveStream {
    HANDLE handle = INVALID_HANDLE_VALUE;
    int64_t file_size = 0;
    std::vector<uint8_t> buffer;

    explicit Win64ArchiveStream(HANDLE h, int64_t sz)
        : handle(h), file_size(sz), buffer(65536) {}
};

la_ssize_t la_win64_read_cb(struct archive* /*a*/, void* client_data, const void** buff) {
    auto* st = static_cast<Win64ArchiveStream*>(client_data);
    if (!st || st->handle == INVALID_HANDLE_VALUE) return -1;

    DWORD bytes_read = 0;
    if (!ReadFile(st->handle, st->buffer.data(), static_cast<DWORD>(st->buffer.size()), &bytes_read, nullptr)) {
        return -1;
    }
    *buff = st->buffer.data();
    return static_cast<la_ssize_t>(bytes_read);
}

la_int64_t la_win64_skip_cb(struct archive* /*a*/, void* client_data, la_int64_t request) {
    auto* st = static_cast<Win64ArchiveStream*>(client_data);
    if (!st || st->handle == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER cur{};
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(st->handle, zero, &cur, FILE_CURRENT)) return 0;

    LARGE_INTEGER target{};
    target.QuadPart = std::clamp<int64_t>(cur.QuadPart + request, 0, st->file_size);
    LARGE_INTEGER actual{};
    if (!SetFilePointerEx(st->handle, target, &actual, FILE_BEGIN)) return 0;

    return static_cast<la_int64_t>(actual.QuadPart - cur.QuadPart);
}

la_int64_t la_win64_seek_cb(struct archive* /*a*/, void* client_data, la_int64_t offset, int whence) {
    auto* st = static_cast<Win64ArchiveStream*>(client_data);
    if (!st || st->handle == INVALID_HANDLE_VALUE) return ARCHIVE_FATAL;

    DWORD move_method = FILE_BEGIN;
    switch (whence) {
        case SEEK_SET: move_method = FILE_BEGIN; break;
        case SEEK_CUR: move_method = FILE_CURRENT; break;
        case SEEK_END: move_method = FILE_END; break;
        default: return ARCHIVE_FATAL;
    }

    LARGE_INTEGER li{};
    li.QuadPart = offset;
    LARGE_INTEGER new_pos{};
    if (!SetFilePointerEx(st->handle, li, &new_pos, move_method)) {
        return ARCHIVE_FATAL;
    }
    return static_cast<la_int64_t>(new_pos.QuadPart);
}

int la_win64_close_cb(struct archive* /*a*/, void* /*client_data*/) {
    // Handle lifecycle is owned by ArchiveReader
    return ARCHIVE_OK;
}

struct ZipEntryMeta {
    std::string name;
    uint16_t method = 0;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    uint64_t local_header_offset = 0;
};

} // namespace

class ArchiveReader final : public ComicReader {
public:
    explicit ArchiveReader(std::filesystem::path stream_cache_dir = {})
        : stream_cache_root_(std::move(stream_cache_dir)) {}

    ~ArchiveReader() override { close(); }

    bool open(const std::wstring& path) override {
        std::lock_guard<std::mutex> lock(io_mutex_);
        close_locked();

        source_path_ = path;
        format_ = detect_comic_format(path);

        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            format_ = ComicFormat::ImageFolder;
            return open_image_folder_locked(path);
        }

        HANDLE h = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            last_error_ = "CreateFileW failed (error " + std::to_string(GetLastError()) + ")";
            return false;
        }

        LARGE_INTEGER size_li{};
        if (!GetFileSizeEx(h, &size_li) || size_li.QuadPart <= 0) {
            CloseHandle(h);
            last_error_ = "GetFileSizeEx failed or empty file";
            return false;
        }

        file_handle_ = h;
        file_size_ = size_li.QuadPart;

        // 1. Standalone image
        if (format_ == ComicFormat::SingleImage) {
            mode_ = Mode::SingleImage;
            is_open_ = true;
            return true;
        }

        // 2. ZIP64 O(1) Central Directory fast path for CBZ / ZIP / EPUB
        if (format_ == ComicFormat::Cbz || format_ == ComicFormat::Epub) {
            if (open_zip64_locked()) {
                mode_ = Mode::Zip64Fast;
                is_open_ = true;
                return true;
            }
            // Fall through to libarchive if non-standard ZIP
        }

        // 3. libarchive 64-bit stream (CBR/RAR5, 7Z/CB7, CBT/TAR, or ZIP fallback)
        if (open_libarchive_locked()) {
            mode_ = Mode::LibArchive;
            is_open_ = true;
            return true;
        }

        close_locked();
        return false;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(io_mutex_);
        close_locked();
    }

    bool is_open() const override { return is_open_; }

    int page_count() const override {
        switch (mode_) {
            case Mode::SingleImage: return is_open_ ? 1 : 0;
            case Mode::ImageFolder: return static_cast<int>(folder_images_.size());
            case Mode::Zip64Fast:   return static_cast<int>(zip_pages_.size());
            case Mode::LibArchive:  return static_cast<int>(la_pages_.size());
            default:                return 0;
        }
    }

    int64_t file_size() const override { return file_size_; }
    ComicFormat format() const override { return format_; }
    std::string last_error() const override { return last_error_; }

    RenderedPage render_page(int index, int target_width, bool auto_crop) override {
        std::vector<uint8_t> raw;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (!is_open_ || index < 0 || index >= page_count()) {
                return {};
            }
            if (!extract_raw_page_locked(index, raw)) {
                return {};
            }
        }
        return process_raw_image_page(
            raw.data(), raw.size(), index, target_width, 0, auto_crop, /*prefer_webp=*/false, /*quality=*/88);
    }

    RenderedPage extract_cover(int max_w, int max_h) override {
        std::vector<uint8_t> raw;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (!is_open_ || page_count() <= 0) {
                return {};
            }
            if (!extract_raw_page_locked(0, raw)) {
                return {};
            }
        }
        return process_raw_image_page(
            raw.data(), raw.size(), 0, max_w, max_h, /*auto_crop=*/false, /*prefer_webp=*/true, /*quality=*/82);
    }

private:
    enum class Mode { None, SingleImage, ImageFolder, Zip64Fast, LibArchive };

    void close_locked() {
        close_la_cursor_locked();
        if (file_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
        }
        if (!session_cache_dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(session_cache_dir_, ec);
            session_cache_dir_.clear();
        }
        folder_images_.clear();
        zip_pages_.clear();
        la_pages_.clear();
        la_name_to_page_idx_.clear();
        disk_cached_pages_.clear();
        use_disk_stream_cache_ = false;
        file_size_ = 0;
        is_open_ = false;
        mode_ = Mode::None;
    }

    bool open_image_folder_locked(const std::wstring& dir_path) {
        std::error_code ec;
        std::vector<std::pair<std::string, std::filesystem::path>> items;
        for (const auto& entry : std::filesystem::directory_iterator(dir_path, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto fname = entry.path().filename().wstring();
            if (!is_supported_image_filename_wide(fname)) continue;
            // Convert filename to UTF-8 for natural sort
            int needed = WideCharToMultiByte(CP_UTF8, 0, fname.data(), static_cast<int>(fname.size()),
                                             nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>(std::max(0, needed)), '\0');
            if (needed > 0) {
                WideCharToMultiByte(CP_UTF8, 0, fname.data(), static_cast<int>(fname.size()),
                                    utf8.data(), needed, nullptr, nullptr);
            }
            items.emplace_back(std::move(utf8), entry.path());
            file_size_ += static_cast<int64_t>(entry.file_size(ec));
        }

        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            return natural_less(a.first, b.first);
        });

        folder_images_.reserve(items.size());
        for (auto& it : items) {
            folder_images_.push_back(std::move(it.second));
        }
        if (folder_images_.empty()) {
            last_error_ = "No supported images found in folder";
            return false;
        }
        mode_ = Mode::ImageFolder;
        is_open_ = true;
        return true;
    }

    // ---- ZIP64 O(1) Central Directory Parser ----
    bool open_zip64_locked() {
        if (file_size_ < 22) return false;

        // Search tail (up to 65557 bytes) for End of Central Directory signature 0x06054b50
        const int64_t tail_len = std::min<int64_t>(file_size_, 65557);
        const int64_t tail_start = file_size_ - tail_len;
        std::vector<uint8_t> tail(static_cast<size_t>(tail_len));
        if (!win64_read_at(file_handle_, tail_start, tail.data(), tail.size())) {
            return false;
        }

        int64_t eocd_pos_in_tail = -1;
        for (int64_t i = tail_len - 22; i >= 0; --i) {
            if (read_le32(tail.data() + i) == 0x06054b50U) {
                const uint16_t comment_len = read_le16(tail.data() + i + 20);
                if (i + 22 + comment_len == tail_len) {
                    eocd_pos_in_tail = i;
                    break;
                }
            }
        }
        if (eocd_pos_in_tail < 0) {
            return false;
        }

        const uint8_t* eocd = tail.data() + eocd_pos_in_tail;
        uint64_t entry_count = read_le16(eocd + 10);
        uint64_t cd_size = read_le32(eocd + 12);
        uint64_t cd_offset = read_le32(eocd + 16);

        // Check for ZIP64 EOCD Locator immediately preceding EOCD
        const int64_t eocd_abs_offset = tail_start + eocd_pos_in_tail;
        if (eocd_abs_offset >= 20) {
            uint8_t locator[20];
            if (win64_read_at(file_handle_, eocd_abs_offset - 20, locator, 20) &&
                read_le32(locator) == 0x07064b50U) {
                const uint64_t z64_eocd_offset = read_le64(locator + 8);
                uint8_t z64_eocd[56];
                if (z64_eocd_offset + 56 <= static_cast<uint64_t>(file_size_) &&
                    win64_read_at(file_handle_, static_cast<int64_t>(z64_eocd_offset), z64_eocd, 56) &&
                    read_le32(z64_eocd) == 0x06064b50U) {
                    entry_count = read_le64(z64_eocd + 32);
                    cd_size = read_le64(z64_eocd + 40);
                    cd_offset = read_le64(z64_eocd + 48);
                }
            }
        }

        if (cd_offset + cd_size > static_cast<uint64_t>(file_size_) || cd_size == 0 ||
            cd_size > 256ULL * 1024ULL * 1024ULL) {
            return false;
        }

        std::vector<uint8_t> cd_buf(static_cast<size_t>(cd_size));
        if (!win64_read_at(file_handle_, static_cast<int64_t>(cd_offset), cd_buf.data(), cd_buf.size())) {
            return false;
        }

        std::vector<ZipEntryMeta> all_entries;
        all_entries.reserve(static_cast<size_t>(std::min<uint64_t>(entry_count, 100000)));

        size_t pos = 0;
        while (pos + 46 <= cd_buf.size()) {
            const uint8_t* h = cd_buf.data() + pos;
            if (read_le32(h) != 0x02014b50U) break;

            const uint16_t method = read_le16(h + 10);
            uint64_t comp_size = read_le32(h + 20);
            uint64_t uncomp_size = read_le32(h + 24);
            const uint16_t name_len = read_le16(h + 28);
            const uint16_t extra_len = read_le16(h + 30);
            const uint16_t comment_len = read_le16(h + 32);
            uint64_t local_off = read_le32(h + 42);

            if (pos + 46 + name_len + extra_len + comment_len > cd_buf.size()) break;

            std::string name(reinterpret_cast<const char*>(h + 46), name_len);
            const uint8_t* extra = h + 46 + name_len;

            // Parse ZIP64 Extra Field (0x0001) when 32-bit sentinel values are present
            if (uncomp_size == 0xFFFFFFFFULL || comp_size == 0xFFFFFFFFULL || local_off == 0xFFFFFFFFULL) {
                size_t ep = 0;
                while (ep + 4 <= extra_len) {
                    const uint16_t tag = read_le16(extra + ep);
                    const uint16_t sz = read_le16(extra + ep + 2);
                    ep += 4;
                    if (ep + sz > extra_len) break;
                    if (tag == 0x0001U) {
                        size_t zp = ep;
                        if (uncomp_size == 0xFFFFFFFFULL && zp + 8 <= ep + sz) {
                            uncomp_size = read_le64(extra + zp);
                            zp += 8;
                        }
                        if (comp_size == 0xFFFFFFFFULL && zp + 8 <= ep + sz) {
                            comp_size = read_le64(extra + zp);
                            zp += 8;
                        }
                        if (local_off == 0xFFFFFFFFULL && zp + 8 <= ep + sz) {
                            local_off = read_le64(extra + zp);
                            zp += 8;
                        }
                        break;
                    }
                    ep += sz;
                }
            }

            if (!name.empty() && name.back() != '/') {
                ZipEntryMeta meta;
                meta.name = normalize_zip_path(name);
                meta.method = method;
                meta.compressed_size = comp_size;
                meta.uncompressed_size = uncomp_size;
                meta.local_header_offset = local_off;
                all_entries.push_back(std::move(meta));
            }

            pos += 46 + name_len + extra_len + comment_len;
        }

        // For EPUB, try reading spine order first; fall back to natural sort of all image entries
        if (format_ == ComicFormat::Epub) {
            std::vector<ZipEntryMeta> spine_images = extract_epub_spine_images_locked(all_entries);
            if (!spine_images.empty()) {
                zip_pages_ = std::move(spine_images);
                return true;
            }
        }

        // Collect all supported image entries and sort in natural page order (1, 2, ..., 10)
        zip_pages_.clear();
        for (auto& e : all_entries) {
            if (is_supported_image_filename(e.name)) {
                zip_pages_.push_back(std::move(e));
            }
        }

        if (zip_pages_.empty()) {
            last_error_ = "Archive contains no supported images";
            return false;
        }

        std::sort(zip_pages_.begin(), zip_pages_.end(), [](const ZipEntryMeta& a, const ZipEntryMeta& b) {
            return natural_less(a.name, b.name);
        });

        return true;
    }

    bool extract_zip_entry_locked(const ZipEntryMeta& entry, std::vector<uint8_t>& out) {
        if (entry.method != 0 && entry.method != 8) {
            return false;
        }
        uint8_t lfh[30];
        if (!win64_read_at(file_handle_, static_cast<int64_t>(entry.local_header_offset), lfh, 30)) {
            return false;
        }
        if (read_le32(lfh) != 0x04034b50U) {
            return false;
        }
        const uint16_t name_len = read_le16(lfh + 26);
        const uint16_t extra_len = read_le16(lfh + 28);
        const int64_t data_offset = static_cast<int64_t>(entry.local_header_offset) + 30 + name_len + extra_len;

        if (entry.compressed_size > 256ULL * 1024ULL * 1024ULL) {
            return false; // Sanity guard per single page
        }

        if (entry.method == 0) {
            out.resize(static_cast<size_t>(entry.compressed_size));
            return win64_read_at(file_handle_, data_offset, out.data(), out.size());
        }

        // Method 8: DEFLATE
        std::vector<uint8_t> compressed(static_cast<size_t>(entry.compressed_size));
        if (!win64_read_at(file_handle_, data_offset, compressed.data(), compressed.size())) {
            return false;
        }
        return inflate_raw_deflate(compressed.data(), compressed.size(), entry.uncompressed_size, out);
    }

    std::vector<ZipEntryMeta> extract_epub_spine_images_locked(const std::vector<ZipEntryMeta>& all_entries) {
        std::unordered_map<std::string, size_t> by_path;
        by_path.reserve(all_entries.size());
        for (size_t i = 0; i < all_entries.size(); ++i) {
            by_path[all_entries[i].name] = i;
        }

        auto read_text_entry = [&](const std::string& path, std::string& out_text) -> bool {
            auto it = by_path.find(normalize_zip_path(path));
            if (it == by_path.end()) return false;
            std::vector<uint8_t> bytes;
            if (!extract_zip_entry_locked(all_entries[it->second], bytes)) return false;
            out_text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return true;
        };

        std::string container_xml;
        if (!read_text_entry("META-INF/container.xml", container_xml)) {
            return {};
        }

        std::smatch m;
        std::regex rootfile_re(R"(full-path\s*=\s*["']([^"']+)["'])", std::regex::icase);
        if (!std::regex_search(container_xml, m, rootfile_re) || m.size() < 2) {
            return {};
        }
        const std::string opf_path = normalize_zip_path(m[1].str());
        const size_t slash = opf_path.find_last_of('/');
        const std::string opf_dir = (slash == std::string::npos) ? "" : opf_path.substr(0, slash);

        std::string opf_xml;
        if (!read_text_entry(opf_path, opf_xml)) {
            return {};
        }

        // Parse manifest items: id -> href
        std::unordered_map<std::string, std::string> manifest;
        std::regex item_re(R"(<item\b([^>]+)>)", std::regex::icase);
        std::regex id_re(R"(\bid\s*=\s*["']([^"']+)["'])", std::regex::icase);
        std::regex href_re(R"(\bhref\s*=\s*["']([^"']+)["'])", std::regex::icase);
        for (std::sregex_iterator it(opf_xml.begin(), opf_xml.end(), item_re), end; it != end; ++it) {
            const std::string attrs = (*it)[1].str();
            std::smatch mid, mhref;
            if (std::regex_search(attrs, mid, id_re) && std::regex_search(attrs, mhref, href_re)) {
                manifest[mid[1].str()] = resolve_relative_zip_path(opf_dir, mhref[1].str());
            }
        }

        // Walk spine itemrefs and extract referenced images in reading order
        std::vector<ZipEntryMeta> ordered;
        std::unordered_map<std::string, bool> seen;
        std::regex itemref_re(R"(<itemref\b[^>]*\bidref\s*=\s*["']([^"']+)["'])", std::regex::icase);
        std::regex img_src_re(R"((?:src|xlink:href)\s*=\s*["']([^"']+)["'])", std::regex::icase);

        for (std::sregex_iterator it(opf_xml.begin(), opf_xml.end(), itemref_re), end; it != end; ++it) {
            const std::string idref = (*it)[1].str();
            auto mit = manifest.find(idref);
            if (mit == manifest.end()) continue;

            const std::string doc_path = mit->second;
            if (is_supported_image_filename(doc_path)) {
                auto pit = by_path.find(doc_path);
                if (pit != by_path.end() && !seen[doc_path]) {
                    seen[doc_path] = true;
                    ordered.push_back(all_entries[pit->second]);
                }
                continue;
            }

            std::string xhtml;
            if (!read_text_entry(doc_path, xhtml)) continue;
            const size_t ds = doc_path.find_last_of('/');
            const std::string doc_dir = (ds == std::string::npos) ? "" : doc_path.substr(0, ds);

            for (std::sregex_iterator iit(xhtml.begin(), xhtml.end(), img_src_re), iend; iit != iend; ++iit) {
                const std::string img_path = resolve_relative_zip_path(doc_dir, (*iit)[1].str());
                if (!is_supported_image_filename(img_path)) continue;
                auto pit = by_path.find(img_path);
                if (pit != by_path.end() && !seen[img_path]) {
                    seen[img_path] = true;
                    ordered.push_back(all_entries[pit->second]);
                }
            }
        }

        return ordered;
    }

    // ---- libarchive 64-Bit Stream ----
    bool open_la_cursor_locked() {
        close_la_cursor_locked();

        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(file_handle_, zero, nullptr, FILE_BEGIN)) {
            return false;
        }

        la_stream_ = std::make_unique<Win64ArchiveStream>(file_handle_, file_size_);
        la_cursor_ = archive_read_new();
        if (!la_cursor_) return false;

        archive_read_support_format_all(la_cursor_);
        archive_read_support_filter_all(la_cursor_);
        archive_read_set_read_callback(la_cursor_, &la_win64_read_cb);
        archive_read_set_skip_callback(la_cursor_, &la_win64_skip_cb);
        archive_read_set_seek_callback(la_cursor_, &la_win64_seek_cb);
        archive_read_set_close_callback(la_cursor_, &la_win64_close_cb);
        archive_read_set_callback_data(la_cursor_, la_stream_.get());

        if (archive_read_open1(la_cursor_) != ARCHIVE_OK) {
            last_error_ = archive_error_string(la_cursor_)
                ? archive_error_string(la_cursor_)
                : "libarchive failed to open stream";
            close_la_cursor_locked();
            return false;
        }
        return true;
    }

    void close_la_cursor_locked() {
        if (la_cursor_) {
            archive_read_free(la_cursor_);
            la_cursor_ = nullptr;
        }
        la_stream_.reset();
    }

    bool open_libarchive_locked() {
        if (!open_la_cursor_locked()) {
            return false;
        }

        std::vector<std::string> names;
        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(la_cursor_, &entry) == ARCHIVE_OK) {
            const char* raw_name = archive_entry_pathname(entry);
            if (raw_name && archive_entry_filetype(entry) != AE_IFDIR) {
                std::string norm = normalize_zip_path(raw_name);
                if (is_supported_image_filename(norm)) {
                    names.push_back(std::move(norm));
                }
            }
            archive_read_data_skip(la_cursor_);
        }

        close_la_cursor_locked();

        if (names.empty()) {
            last_error_ = "Archive contains no supported images";
            return false;
        }

        std::sort(names.begin(), names.end(), natural_less);
        la_pages_ = std::move(names);
        la_name_to_page_idx_.clear();
        for (size_t i = 0; i < la_pages_.size(); ++i) {
            la_name_to_page_idx_[la_pages_[i]] = static_cast<int>(i);
        }

        // Enable progressive disk stream cache for solid .7z/.cbr archives > 1 GB
        if ((format_ == ComicFormat::Cb7 || format_ == ComicFormat::Cbr) &&
            file_size_ > kSolidStreamCacheThresholdBytes &&
            !stream_cache_root_.empty()) {
            const size_t h = std::hash<std::wstring>{}(source_path_) ^
                             static_cast<size_t>(GetTickCount64());
            session_cache_dir_ = stream_cache_root_ / (L"session_" + std::to_wstring(h));
            std::error_code ec;
            std::filesystem::create_directories(session_cache_dir_, ec);
            if (!ec) {
                use_disk_stream_cache_ = true;
                disk_cached_pages_.assign(la_pages_.size(), false);
            }
        }

        return true;
    }

    std::filesystem::path disk_cache_path_for_page(int index) const {
        return session_cache_dir_ / (L"page_" + std::to_wstring(index) + L".bin");
    }

    bool read_la_entry_data_locked(struct archive* a, std::vector<uint8_t>& out) {
        out.clear();
        const void* buff = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        while (true) {
            const int r = archive_read_data_block(a, &buff, &size, &offset);
            if (r == ARCHIVE_EOF) return true;
            if (r != ARCHIVE_OK) return false;
            if (size > 0 && buff) {
                const auto* bytes = static_cast<const uint8_t*>(buff);
                out.insert(out.end(), bytes, bytes + size);
            }
        }
    }

    bool extract_la_page_locked(int target_index, std::vector<uint8_t>& out) {
        // 1. Check progressive disk stream cache first (0 ms seek for solid >1 GB archives)
        if (use_disk_stream_cache_ &&
            target_index >= 0 &&
            static_cast<size_t>(target_index) < disk_cached_pages_.size() &&
            disk_cached_pages_[static_cast<size_t>(target_index)]) {
            std::ifstream ifs(disk_cache_path_for_page(target_index), std::ios::binary);
            if (ifs) {
                out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
                return !out.empty();
            }
        }

        const std::string& target_name = la_pages_[static_cast<size_t>(target_index)];

        // Try advancing active cursor first; if not found (backward jump), rewind once
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!la_cursor_) {
                if (!open_la_cursor_locked()) return false;
            }

            struct archive_entry* entry = nullptr;
            while (archive_read_next_header(la_cursor_, &entry) == ARCHIVE_OK) {
                const char* raw_name = archive_entry_pathname(entry);
                if (!raw_name || archive_entry_filetype(entry) == AE_IFDIR) {
                    archive_read_data_skip(la_cursor_);
                    continue;
                }

                const std::string norm = normalize_zip_path(raw_name);
                if (norm == target_name) {
                    if (!read_la_entry_data_locked(la_cursor_, out)) {
                        return false;
                    }
                    if (use_disk_stream_cache_) {
                        std::ofstream ofs(disk_cache_path_for_page(target_index), std::ios::binary);
                        if (ofs) {
                            ofs.write(reinterpret_cast<const char*>(out.data()),
                                      static_cast<std::streamsize>(out.size()));
                            disk_cached_pages_[static_cast<size_t>(target_index)] = true;
                        }
                    }
                    return true;
                }

                // For solid >1 GB archives, progressively spill passed pages to disk cache
                // so future jumps never re-decompress gigabytes from offset 0
                if (use_disk_stream_cache_) {
                    auto it = la_name_to_page_idx_.find(norm);
                    if (it != la_name_to_page_idx_.end() &&
                        !disk_cached_pages_[static_cast<size_t>(it->second)]) {
                        std::vector<uint8_t> tmp;
                        if (read_la_entry_data_locked(la_cursor_, tmp) && !tmp.empty()) {
                            std::ofstream ofs(disk_cache_path_for_page(it->second), std::ios::binary);
                            if (ofs) {
                                ofs.write(reinterpret_cast<const char*>(tmp.data()),
                                          static_cast<std::streamsize>(tmp.size()));
                                disk_cached_pages_[static_cast<size_t>(it->second)] = true;
                            }
                        }
                        continue;
                    }
                }

                archive_read_data_skip(la_cursor_);
            }

            // Reached EOF without finding target -> rewind stream once
            close_la_cursor_locked();
        }

        return false;
    }

    bool extract_raw_page_locked(int index, std::vector<uint8_t>& out) {
        switch (mode_) {
            case Mode::SingleImage: {
                if (file_size_ <= 0 || file_size_ > 256LL * 1024LL * 1024LL) return false;
                out.resize(static_cast<size_t>(file_size_));
                return win64_read_at(file_handle_, 0, out.data(), out.size());
            }
            case Mode::ImageFolder: {
                std::ifstream ifs(folder_images_[static_cast<size_t>(index)], std::ios::binary);
                if (!ifs) return false;
                out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
                return !out.empty();
            }
            case Mode::Zip64Fast: {
                const auto& meta = zip_pages_[static_cast<size_t>(index)];
                if (extract_zip_entry_locked(meta, out)) {
                    return true;
                }
                // Fallback to libarchive if ZIP uses non-STORE/DEFLATE compression (e.g. LZMA)
                if (la_pages_.empty()) {
                    la_pages_.reserve(zip_pages_.size());
                    for (const auto& z : zip_pages_) la_pages_.push_back(z.name);
                }
                return extract_la_page_locked(index, out);
            }
            case Mode::LibArchive: {
                return extract_la_page_locked(index, out);
            }
            default:
                return false;
        }
    }

    mutable std::mutex io_mutex_;
    std::wstring source_path_;
    std::filesystem::path stream_cache_root_;
    std::filesystem::path session_cache_dir_;
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
    int64_t file_size_ = 0;
    ComicFormat format_ = ComicFormat::Unknown;
    Mode mode_ = Mode::None;
    bool is_open_ = false;
    std::string last_error_;

    // Folder mode
    std::vector<std::filesystem::path> folder_images_;

    // ZIP64 O(1) mode
    std::vector<ZipEntryMeta> zip_pages_;

    // libarchive mode
    std::vector<std::string> la_pages_;
    std::unordered_map<std::string, int> la_name_to_page_idx_;
    std::unique_ptr<Win64ArchiveStream> la_stream_;
    struct archive* la_cursor_ = nullptr;
    bool use_disk_stream_cache_ = false;
    std::vector<bool> disk_cached_pages_;
};

ComicFormat detect_comic_format(const std::wstring& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return ComicFormat::ImageFolder;
    }

    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return ComicFormat::Unknown;
    const std::wstring ext = to_lower_wide(path.substr(dot));

    if (ext == L".pdf")  return ComicFormat::Pdf;
    if (ext == L".cbz" || ext == L".zip") return ComicFormat::Cbz;
    if (ext == L".cbr" || ext == L".rar") return ComicFormat::Cbr;
    if (ext == L".cb7" || ext == L".7z")  return ComicFormat::Cb7;
    if (ext == L".cbt" || ext == L".tar") return ComicFormat::Cbt;
    if (ext == L".epub") return ComicFormat::Epub;
    if (ext == L".jpg" || ext == L".jpeg" || ext == L".png" ||
        ext == L".gif" || ext == L".bmp"  || ext == L".webp") {
        return ComicFormat::SingleImage;
    }
    return ComicFormat::Unknown;
}

std::unique_ptr<ComicReader> create_archive_reader(const std::filesystem::path& stream_cache_dir) {
    return std::make_unique<ArchiveReader>(stream_cache_dir);
}

std::unique_ptr<ComicReader> create_comic_reader(
    const std::wstring& path,
    const std::filesystem::path& stream_cache_dir) {
    const ComicFormat fmt = detect_comic_format(path);
    if (fmt == ComicFormat::Pdf) {
        return create_pdf_reader();
    }
    return create_archive_reader(stream_cache_dir);
}

namespace {

std::string trim_meta(std::string s) {
    auto is_ws = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string to_lower_ascii_str(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool is_comicinfo_xml_path(const std::string& raw_name) {
    const std::string norm = normalize_zip_path(raw_name);
    const size_t slash = norm.find_last_of('/');
    const std::string base = (slash == std::string::npos) ? norm : norm.substr(slash + 1);
    return to_lower_ascii_str(base) == "comicinfo.xml";
}

std::string unescape_xml_entities(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '&') {
            const size_t semi = src.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 10) {
                const std::string ent = src.substr(i + 1, semi - i - 1);
                if (ent == "amp")       { out.push_back('&'); i = semi; continue; }
                if (ent == "lt")        { out.push_back('<'); i = semi; continue; }
                if (ent == "gt")        { out.push_back('>'); i = semi; continue; }
                if (ent == "quot")      { out.push_back('"'); i = semi; continue; }
                if (ent == "apos")      { out.push_back('\''); i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    try {
                        int code = 0;
                        if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                            code = std::stoi(ent.substr(2), nullptr, 16);
                        } else {
                            code = std::stoi(ent.substr(1), nullptr, 10);
                        }
                        if (code > 0 && code < 128) {
                            out.push_back(static_cast<char>(code));
                            i = semi;
                            continue;
                        }
                    } catch (...) {}
                }
            }
        }
        out.push_back(src[i]);
    }
    return out;
}

std::string decode_xml_bytes_to_utf8(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return {};
    // UTF-16LE BOM
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        const size_t wcount = (bytes.size() - 2) / 2;
        if (wcount == 0) return {};
        const auto* wptr = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wptr, static_cast<int>(wcount), nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wptr, static_cast<int>(wcount), out.data(), needed, nullptr, nullptr);
        return out;
    }
    // UTF-8 BOM
    size_t offset = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        offset = 3;
    }
    return std::string(reinterpret_cast<const char*>(bytes.data() + offset), bytes.size() - offset);
}

std::string extract_xml_tag_value(const std::string& xml, const std::string& xml_lower, const char* tag_lower) {
    const std::string open_prefix = std::string("<") + tag_lower;
    const std::string close_tag = std::string("</") + tag_lower + ">";

    size_t pos = 0;
    while ((pos = xml_lower.find(open_prefix, pos)) != std::string::npos) {
        const size_t after_name = pos + open_prefix.size();
        if (after_name >= xml_lower.size()) break;
        const char next_ch = xml_lower[after_name];
        if (next_ch != '>' && next_ch != ' ' && next_ch != '\t' && next_ch != '\r' && next_ch != '\n' && next_ch != '/') {
            pos = after_name;
            continue;
        }
        const size_t gt = xml_lower.find('>', after_name);
        if (gt == std::string::npos) break;
        if (gt > pos && xml_lower[gt - 1] == '/') {
            pos = gt + 1;
            continue; // Self-closing empty tag
        }
        const size_t end_pos = xml_lower.find(close_tag, gt + 1);
        if (end_pos == std::string::npos) break;
        return trim_meta(unescape_xml_entities(xml.substr(gt + 1, end_pos - (gt + 1))));
    }
    return {};
}

std::string merge_tags(const std::string& primary, const std::string& secondary) {
    if (primary.empty()) return secondary;
    if (secondary.empty()) return primary;

    std::vector<std::string> tokens;
    std::unordered_map<std::string, bool> seen;
    auto push_list = [&](const std::string& csv) {
        size_t start = 0;
        while (start <= csv.size()) {
            const size_t comma = csv.find(',', start);
            std::string tok = trim_meta(comma == std::string::npos ? csv.substr(start) : csv.substr(start, comma - start));
            if (!tok.empty()) {
                const std::string key = to_lower_ascii_str(tok);
                if (!seen[key]) {
                    seen[key] = true;
                    tokens.push_back(std::move(tok));
                }
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    };
    push_list(primary);
    push_list(secondary);

    std::string merged;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) merged += ", ";
        merged += tokens[i];
    }
    return merged;
}

bool parse_comicinfo_xml(const std::vector<uint8_t>& xml_bytes, ComicMetadata& out) {
    const std::string xml = decode_xml_bytes_to_utf8(xml_bytes);
    if (xml.empty()) return false;
    const std::string xml_lower = to_lower_ascii_str(xml);

    out.series   = extract_xml_tag_value(xml, xml_lower, "series");
    out.author   = extract_xml_tag_value(xml, xml_lower, "writer");
    const std::string tags  = extract_xml_tag_value(xml, xml_lower, "tags");
    const std::string genre = extract_xml_tag_value(xml, xml_lower, "genre");
    out.tags     = merge_tags(tags, genre);
    out.language = extract_xml_tag_value(xml, xml_lower, "languageiso");
    return !out.empty();
}

bool extract_zip64_comicinfo(HANDLE h, int64_t file_size, std::vector<uint8_t>& out_xml) {
    if (file_size < 22) return false;

    const int64_t tail_len = std::min<int64_t>(file_size, 65557);
    const int64_t tail_start = file_size - tail_len;
    std::vector<uint8_t> tail(static_cast<size_t>(tail_len));
    if (!win64_read_at(h, tail_start, tail.data(), tail.size())) {
        return false;
    }

    int64_t eocd_pos_in_tail = -1;
    for (int64_t i = tail_len - 22; i >= 0; --i) {
        if (read_le32(tail.data() + i) == 0x06054b50U) {
            const uint16_t comment_len = read_le16(tail.data() + i + 20);
            if (i + 22 + comment_len == tail_len) {
                eocd_pos_in_tail = i;
                break;
            }
        }
    }
    if (eocd_pos_in_tail < 0) return false;

    const uint8_t* eocd = tail.data() + eocd_pos_in_tail;
    uint64_t cd_size = read_le32(eocd + 12);
    uint64_t cd_offset = read_le32(eocd + 16);

    const int64_t eocd_abs_offset = tail_start + eocd_pos_in_tail;
    if (eocd_abs_offset >= 20) {
        uint8_t locator[20];
        if (win64_read_at(h, eocd_abs_offset - 20, locator, 20) &&
            read_le32(locator) == 0x07064b50U) {
            const uint64_t z64_eocd_offset = read_le64(locator + 8);
            uint8_t z64_eocd[56];
            if (z64_eocd_offset + 56 <= static_cast<uint64_t>(file_size) &&
                win64_read_at(h, static_cast<int64_t>(z64_eocd_offset), z64_eocd, 56) &&
                read_le32(z64_eocd) == 0x06064b50U) {
                cd_size = read_le64(z64_eocd + 40);
                cd_offset = read_le64(z64_eocd + 48);
            }
        }
    }

    if (cd_offset + cd_size > static_cast<uint64_t>(file_size) || cd_size == 0 ||
        cd_size > 64ULL * 1024ULL * 1024ULL) {
        return false;
    }

    std::vector<uint8_t> cd_buf(static_cast<size_t>(cd_size));
    if (!win64_read_at(h, static_cast<int64_t>(cd_offset), cd_buf.data(), cd_buf.size())) {
        return false;
    }

    size_t pos = 0;
    while (pos + 46 <= cd_buf.size()) {
        const uint8_t* hdr = cd_buf.data() + pos;
        if (read_le32(hdr) != 0x02014b50U) break;

        const uint16_t method = read_le16(hdr + 10);
        uint64_t comp_size = read_le32(hdr + 20);
        uint64_t uncomp_size = read_le32(hdr + 24);
        const uint16_t name_len = read_le16(hdr + 28);
        const uint16_t extra_len = read_le16(hdr + 30);
        const uint16_t comment_len = read_le16(hdr + 32);
        uint64_t local_off = read_le32(hdr + 42);

        if (pos + 46 + name_len + extra_len + comment_len > cd_buf.size()) break;

        std::string name(reinterpret_cast<const char*>(hdr + 46), name_len);
        if (is_comicinfo_xml_path(name)) {
            const uint8_t* extra = hdr + 46 + name_len;
            if (uncomp_size == 0xFFFFFFFFULL || comp_size == 0xFFFFFFFFULL || local_off == 0xFFFFFFFFULL) {
                size_t ep = 0;
                while (ep + 4 <= extra_len) {
                    const uint16_t tag = read_le16(extra + ep);
                    const uint16_t sz = read_le16(extra + ep + 2);
                    ep += 4;
                    if (ep + sz > extra_len) break;
                    if (tag == 0x0001U) {
                        size_t zp = ep;
                        if (uncomp_size == 0xFFFFFFFFULL && zp + 8 <= ep + sz) { uncomp_size = read_le64(extra + zp); zp += 8; }
                        if (comp_size == 0xFFFFFFFFULL && zp + 8 <= ep + sz)   { comp_size = read_le64(extra + zp); zp += 8; }
                        if (local_off == 0xFFFFFFFFULL && zp + 8 <= ep + sz)   { local_off = read_le64(extra + zp); zp += 8; }
                        break;
                    }
                    ep += sz;
                }
            }

            constexpr uint64_t kMaxXmlBytes = 2ULL * 1024ULL * 1024ULL;
            if ((method != 0 && method != 8) || comp_size > kMaxXmlBytes || uncomp_size > kMaxXmlBytes) {
                return false;
            }

            uint8_t lfh[30];
            if (!win64_read_at(h, static_cast<int64_t>(local_off), lfh, 30) || read_le32(lfh) != 0x04034b50U) {
                return false;
            }
            const uint16_t lfh_name_len = read_le16(lfh + 26);
            const uint16_t lfh_extra_len = read_le16(lfh + 28);
            const int64_t data_offset = static_cast<int64_t>(local_off) + 30 + lfh_name_len + lfh_extra_len;

            if (method == 0) {
                out_xml.resize(static_cast<size_t>(comp_size));
                return win64_read_at(h, data_offset, out_xml.data(), out_xml.size());
            }
            std::vector<uint8_t> compressed(static_cast<size_t>(comp_size));
            if (!win64_read_at(h, data_offset, compressed.data(), compressed.size())) {
                return false;
            }
            return inflate_raw_deflate(compressed.data(), compressed.size(), uncomp_size, out_xml);
        }

        pos += 46 + name_len + extra_len + comment_len;
    }

    return false;
}

bool extract_libarchive_comicinfo(HANDLE h, int64_t file_size, std::vector<uint8_t>& out_xml) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) {
        return false;
    }

    Win64ArchiveStream stream(h, file_size);
    struct archive* a = archive_read_new();
    if (!a) return false;

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    archive_read_set_read_callback(a, &la_win64_read_cb);
    archive_read_set_skip_callback(a, &la_win64_skip_cb);
    archive_read_set_seek_callback(a, &la_win64_seek_cb);
    archive_read_set_close_callback(a, &la_win64_close_cb);
    archive_read_set_callback_data(a, &stream);

    bool found = false;
    if (archive_read_open1(a) == ARCHIVE_OK) {
        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            const char* raw_name = archive_entry_pathname(entry);
            if (raw_name && archive_entry_filetype(entry) != AE_IFDIR && is_comicinfo_xml_path(raw_name)) {
                const la_int64_t sz = archive_entry_size(entry);
                if (sz <= 2LL * 1024LL * 1024LL) {
                    out_xml.clear();
                    const void* buff = nullptr;
                    size_t size = 0;
                    la_int64_t offset = 0;
                    bool read_ok = true;
                    while (true) {
                        const int r = archive_read_data_block(a, &buff, &size, &offset);
                        if (r == ARCHIVE_EOF) break;
                        if (r != ARCHIVE_OK) { read_ok = false; break; }
                        if (size > 0 && buff) {
                            if (out_xml.size() + size > 2ULL * 1024ULL * 1024ULL) {
                                read_ok = false;
                                break;
                            }
                            const auto* bytes = static_cast<const uint8_t*>(buff);
                            out_xml.insert(out_xml.end(), bytes, bytes + size);
                        }
                    }
                    found = read_ok && !out_xml.empty();
                }
                break;
            }
            archive_read_data_skip(a);
        }
    }

    archive_read_free(a);
    return found;
}

} // namespace

bool extract_archive_metadata(const std::wstring& path, ComicMetadata& out) {
    const ComicFormat fmt = detect_comic_format(path);
    if (fmt != ComicFormat::Cbz && fmt != ComicFormat::Cbr &&
        fmt != ComicFormat::Cb7 && fmt != ComicFormat::Epub &&
        fmt != ComicFormat::Cbt) {
        return false;
    }

    HANDLE h = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size_li{};
    if (!GetFileSizeEx(h, &size_li) || size_li.QuadPart <= 0) {
        CloseHandle(h);
        return false;
    }

    std::vector<uint8_t> xml_bytes;
    bool got_xml = false;
    if (fmt == ComicFormat::Cbz || fmt == ComicFormat::Epub) {
        got_xml = extract_zip64_comicinfo(h, size_li.QuadPart, xml_bytes);
    } else {
        got_xml = extract_libarchive_comicinfo(h, size_li.QuadPart, xml_bytes);
    }

    CloseHandle(h);
    if (!got_xml || xml_bytes.empty()) {
        return false;
    }

    return parse_comicinfo_xml(xml_bytes, out);
}

void parse_filename_metadata(const std::string& filename_stem, ComicMetadata& in_out) {
    if (filename_stem.empty()) return;
    if (!in_out.author.empty() && !in_out.tags.empty()) return;

    try {
        static const std::regex kFallbackRe(R"(^\s*\[([^\]]+)\]\s*([^\(\[]+)(?:\(([^\)]+)\))?)");
        std::smatch m;
        if (std::regex_search(filename_stem, m, kFallbackRe)) {
            if (in_out.author.empty() && m.size() > 1 && m[1].matched) {
                in_out.author = trim_meta(m[1].str());
            }
            if (in_out.tags.empty() && m.size() > 3 && m[3].matched) {
                in_out.tags = trim_meta(m[3].str());
            }
        }
    } catch (...) {
        // Ignore regex errors on malformed strings
    }
}

bool extract_first_image_bytes_fast(const std::wstring& path, std::vector<uint8_t>& out_raw) {
    out_raw.clear();
    const ComicFormat fmt = detect_comic_format(path);
    if (fmt != ComicFormat::Cbz && fmt != ComicFormat::Epub) {
        return false;
    }

    HANDLE h = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size_li{};
    if (!GetFileSizeEx(h, &size_li) || size_li.QuadPart < 22) {
        CloseHandle(h);
        return false;
    }
    const int64_t file_size = size_li.QuadPart;

    const int64_t tail_len = std::min<int64_t>(file_size, 65557);
    const int64_t tail_start = file_size - tail_len;
    std::vector<uint8_t> tail(static_cast<size_t>(tail_len));
    if (!win64_read_at(h, tail_start, tail.data(), tail.size())) {
        CloseHandle(h);
        return false;
    }

    int64_t eocd_pos_in_tail = -1;
    for (int64_t i = tail_len - 22; i >= 0; --i) {
        if (read_le32(tail.data() + i) == 0x06054b50U) {
            const uint16_t comment_len = read_le16(tail.data() + i + 20);
            if (i + 22 + comment_len == tail_len) {
                eocd_pos_in_tail = i;
                break;
            }
        }
    }
    if (eocd_pos_in_tail < 0) {
        CloseHandle(h);
        return false;
    }

    const uint8_t* eocd = tail.data() + eocd_pos_in_tail;
    uint64_t cd_size = read_le32(eocd + 12);
    uint64_t cd_offset = read_le32(eocd + 16);

    const int64_t eocd_abs_offset = tail_start + eocd_pos_in_tail;
    if (eocd_abs_offset >= 20) {
        uint8_t locator[20];
        if (win64_read_at(h, eocd_abs_offset - 20, locator, 20) &&
            read_le32(locator) == 0x07064b50U) {
            const uint64_t z64_eocd_offset = read_le64(locator + 8);
            uint8_t z64_eocd[56];
            if (z64_eocd_offset + 56 <= static_cast<uint64_t>(file_size) &&
                win64_read_at(h, static_cast<int64_t>(z64_eocd_offset), z64_eocd, 56) &&
                read_le32(z64_eocd) == 0x06064b50U) {
                cd_size = read_le64(z64_eocd + 40);
                cd_offset = read_le64(z64_eocd + 48);
            }
        }
    }

    if (cd_offset + cd_size > static_cast<uint64_t>(file_size) || cd_size == 0 ||
        cd_size > 64ULL * 1024ULL * 1024ULL) {
        CloseHandle(h);
        return false;
    }

    std::vector<uint8_t> cd_buf(static_cast<size_t>(cd_size));
    if (!win64_read_at(h, static_cast<int64_t>(cd_offset), cd_buf.data(), cd_buf.size())) {
        CloseHandle(h);
        return false;
    }

    bool has_best = false;
    ZipEntryMeta best;
    size_t pos = 0;
    while (pos + 46 <= cd_buf.size()) {
        const uint8_t* hdr = cd_buf.data() + pos;
        if (read_le32(hdr) != 0x02014b50U) break;

        const uint16_t method = read_le16(hdr + 10);
        uint64_t comp_size = read_le32(hdr + 20);
        uint64_t uncomp_size = read_le32(hdr + 24);
        const uint16_t name_len = read_le16(hdr + 28);
        const uint16_t extra_len = read_le16(hdr + 30);
        const uint16_t comment_len = read_le16(hdr + 32);
        uint64_t local_off = read_le32(hdr + 42);

        if (pos + 46 + name_len + extra_len + comment_len > cd_buf.size()) break;

        std::string name(reinterpret_cast<const char*>(hdr + 46), name_len);
        if (!name.empty() && name.back() != '/' && is_supported_image_filename(name)) {
            std::string norm = normalize_zip_path(name);
            if (!has_best || natural_less(norm, best.name)) {
                const uint8_t* extra = hdr + 46 + name_len;
                if (uncomp_size == 0xFFFFFFFFULL || comp_size == 0xFFFFFFFFULL || local_off == 0xFFFFFFFFULL) {
                    size_t ep = 0;
                    while (ep + 4 <= extra_len) {
                        const uint16_t tag = read_le16(extra + ep);
                        const uint16_t sz = read_le16(extra + ep + 2);
                        ep += 4;
                        if (ep + sz > extra_len) break;
                        if (tag == 0x0001U) {
                            size_t zp = ep;
                            if (uncomp_size == 0xFFFFFFFFULL && zp + 8 <= ep + sz) { uncomp_size = read_le64(extra + zp); zp += 8; }
                            if (comp_size == 0xFFFFFFFFULL && zp + 8 <= ep + sz)   { comp_size = read_le64(extra + zp); zp += 8; }
                            if (local_off == 0xFFFFFFFFULL && zp + 8 <= ep + sz)   { local_off = read_le64(extra + zp); zp += 8; }
                            break;
                        }
                        ep += sz;
                    }
                }
                best.name = std::move(norm);
                best.method = method;
                best.compressed_size = comp_size;
                best.uncompressed_size = uncomp_size;
                best.local_header_offset = local_off;
                has_best = true;
            }
        }
        pos += 46 + name_len + extra_len + comment_len;
    }

    if (!has_best || (best.method != 0 && best.method != 8) ||
        best.compressed_size == 0 || best.compressed_size > 64ULL * 1024ULL * 1024ULL) {
        CloseHandle(h);
        return false;
    }

    uint8_t lfh[30];
    if (!win64_read_at(h, static_cast<int64_t>(best.local_header_offset), lfh, 30) ||
        read_le32(lfh) != 0x04034b50U) {
        CloseHandle(h);
        return false;
    }
    const uint16_t lfh_name_len = read_le16(lfh + 26);
    const uint16_t lfh_extra_len = read_le16(lfh + 28);
    const int64_t data_offset = static_cast<int64_t>(best.local_header_offset) + 30 + lfh_name_len + lfh_extra_len;

    bool ok = false;
    if (best.method == 0) {
        out_raw.resize(static_cast<size_t>(best.compressed_size));
        ok = win64_read_at(h, data_offset, out_raw.data(), out_raw.size());
    } else {
        std::vector<uint8_t> compressed(static_cast<size_t>(best.compressed_size));
        if (win64_read_at(h, data_offset, compressed.data(), compressed.size())) {
            ok = inflate_raw_deflate(compressed.data(), compressed.size(), best.uncompressed_size, out_raw);
        }
    }

    CloseHandle(h);
    return ok && !out_raw.empty();
}

} // namespace koma
