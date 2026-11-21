#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <archive.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace koma::detail {

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

inline bool win64_read_at(HANDLE h, int64_t offset, uint8_t* buf, size_t len) {
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

inline bool inflate_raw_deflate(const uint8_t* src, size_t src_len,
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

inline std::string normalize_zip_path(const std::string& p) {
    std::string out = p;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

// 64-bit Win32 custom callback state for libarchive.
struct Win64ArchiveStream {
    HANDLE handle = INVALID_HANDLE_VALUE;
    int64_t file_size = 0;
    std::vector<uint8_t> buffer;

    explicit Win64ArchiveStream(HANDLE h, int64_t sz)
        : handle(h), file_size(sz), buffer(65536) {}
};

inline la_ssize_t la_win64_read_cb(struct archive* /*a*/, void* client_data, const void** buff) {
    auto* st = static_cast<Win64ArchiveStream*>(client_data);
    if (!st || st->handle == INVALID_HANDLE_VALUE) return -1;

    DWORD bytes_read = 0;
    if (!ReadFile(st->handle, st->buffer.data(), static_cast<DWORD>(st->buffer.size()), &bytes_read, nullptr)) {
        return -1;
    }
    *buff = st->buffer.data();
    return static_cast<la_ssize_t>(bytes_read);
}

inline la_int64_t la_win64_skip_cb(struct archive* /*a*/, void* client_data, la_int64_t request) {
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

inline la_int64_t la_win64_seek_cb(struct archive* /*a*/, void* client_data, la_int64_t offset, int whence) {
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

inline int la_win64_close_cb(struct archive* /*a*/, void* /*client_data*/) {
    // Handle lifecycle is owned by caller.
    return ARCHIVE_OK;
}

// Reads the ZIP/ZIP64 Central Directory buffer into `out_cd` in O(1) seeks from EOF.
inline bool read_zip64_central_directory(HANDLE h, int64_t file_size, std::vector<uint8_t>& out_cd) {
    out_cd.clear();
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

    out_cd.resize(static_cast<size_t>(cd_size));
    if (!win64_read_at(h, static_cast<int64_t>(cd_offset), out_cd.data(), out_cd.size())) {
        out_cd.clear();
        return false;
    }
    return true;
}

// Parses optional ZIP64 extended information extra field (tag 0x0001).
inline void parse_zip64_extra_fields(const uint8_t* extra,
                                     uint16_t extra_len,
                                     uint64_t& uncomp_size,
                                     uint64_t& comp_size,
                                     uint64_t& local_off) {
    if (uncomp_size != 0xFFFFFFFFULL && comp_size != 0xFFFFFFFFULL && local_off != 0xFFFFFFFFULL) {
        return;
    }
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

// Reads and inflates a single STORE(0) or DEFLATE(8) entry from a ZIP file handle.
inline bool read_zip_entry_bytes(HANDLE h,
                                 uint16_t method,
                                 uint64_t local_off,
                                 uint64_t comp_size,
                                 uint64_t uncomp_size,
                                 uint64_t max_bytes,
                                 std::vector<uint8_t>& out) {
    out.clear();
    if ((method != 0 && method != 8) || comp_size == 0 ||
        comp_size > max_bytes || uncomp_size > max_bytes) {
        return false;
    }

    uint8_t lfh[30];
    if (!win64_read_at(h, static_cast<int64_t>(local_off), lfh, 30) ||
        read_le32(lfh) != 0x04034b50U) {
        return false;
    }
    const uint16_t lfh_name_len = read_le16(lfh + 26);
    const uint16_t lfh_extra_len = read_le16(lfh + 28);
    const int64_t data_offset = static_cast<int64_t>(local_off) + 30 + lfh_name_len + lfh_extra_len;

    if (method == 0) {
        out.resize(static_cast<size_t>(comp_size));
        return win64_read_at(h, data_offset, out.data(), out.size());
    }

    std::vector<uint8_t> compressed(static_cast<size_t>(comp_size));
    if (!win64_read_at(h, data_offset, compressed.data(), compressed.size())) {
        return false;
    }
    return inflate_raw_deflate(compressed.data(), compressed.size(), uncomp_size, out);
}

} // namespace koma::detail
