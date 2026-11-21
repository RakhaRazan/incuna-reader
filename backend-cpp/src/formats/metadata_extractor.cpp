#include "formats/metadata_extractor.hpp"

#include "formats/archive_io.hpp"
#include "formats/comic_reader.hpp"
#include "formats/pdfium_runtime.hpp"
#include "utils/string_utils.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <fpdf_doc.h>
#include <fpdfview.h>

#include <cwchar>
#include <limits>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <vector>

namespace koma {

namespace {

template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}
    ~ScopeExit() { fn_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    Fn fn_;
};

std::string read_pdf_meta_tag_locked(FPDF_DOCUMENT doc, const char* tag) {
    if (!doc || !tag) return {};
    const unsigned long bytes = FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (bytes <= 2) return {};

    std::vector<wchar_t> wbuf((bytes + 1) / 2 + 1, L'\0');
    const unsigned long written = FPDF_GetMetaText(doc, tag, wbuf.data(), bytes);
    if (written <= 2) return {};

    const size_t wlen = wcsnlen(wbuf.data(), wbuf.size());
    if (wlen == 0) return {};

    return trim_ascii(wide_to_utf8(std::wstring(wbuf.data(), wlen)));
}

bool is_comicinfo_xml_path(const std::string& raw_name) {
    const std::string norm = detail::normalize_zip_path(raw_name);
    const size_t slash = norm.find_last_of('/');
    const std::string base = (slash == std::string::npos) ? norm : norm.substr(slash + 1);
    return to_lower_ascii(base) == "comicinfo.xml";
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
        return wide_to_utf8(std::wstring(wptr, wcount));
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
        return trim_ascii(unescape_xml_entities(xml.substr(gt + 1, end_pos - (gt + 1))));
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
            std::string tok = trim_ascii(comma == std::string::npos ? csv.substr(start) : csv.substr(start, comma - start));
            if (!tok.empty()) {
                const std::string key = to_lower_ascii(tok);
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
    const std::string xml_lower = to_lower_ascii(xml);

    out.series   = extract_xml_tag_value(xml, xml_lower, "series");
    out.author   = extract_xml_tag_value(xml, xml_lower, "writer");
    const std::string tags  = extract_xml_tag_value(xml, xml_lower, "tags");
    const std::string genre = extract_xml_tag_value(xml, xml_lower, "genre");
    out.tags     = merge_tags(tags, genre);
    out.language = extract_xml_tag_value(xml, xml_lower, "languageiso");
    return !out.empty();
}

bool extract_zip64_comicinfo(HANDLE h, int64_t file_size, std::vector<uint8_t>& out_xml) {
    std::vector<uint8_t> cd_buf;
    if (!detail::read_zip64_central_directory(h, file_size, cd_buf)) {
        return false;
    }

    constexpr uint64_t kMaxXmlBytes = 2ULL * 1024ULL * 1024ULL;
    size_t pos = 0;
    while (pos + 46 <= cd_buf.size()) {
        const uint8_t* hdr = cd_buf.data() + pos;
        if (detail::read_le32(hdr) != 0x02014b50U) break;

        const uint16_t method = detail::read_le16(hdr + 10);
        uint64_t comp_size = detail::read_le32(hdr + 20);
        uint64_t uncomp_size = detail::read_le32(hdr + 24);
        const uint16_t name_len = detail::read_le16(hdr + 28);
        const uint16_t extra_len = detail::read_le16(hdr + 30);
        const uint16_t comment_len = detail::read_le16(hdr + 32);
        uint64_t local_off = detail::read_le32(hdr + 42);

        if (pos + 46 + name_len + extra_len + comment_len > cd_buf.size()) break;

        const std::string name(reinterpret_cast<const char*>(hdr + 46), name_len);
        if (is_comicinfo_xml_path(name)) {
            detail::parse_zip64_extra_fields(hdr + 46 + name_len, extra_len, uncomp_size, comp_size, local_off);
            return detail::read_zip_entry_bytes(h, method, local_off, comp_size, uncomp_size, kMaxXmlBytes, out_xml);
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

    detail::Win64ArchiveStream stream(h, file_size);
    struct archive* a = archive_read_new();
    if (!a) return false;

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    archive_read_set_read_callback(a, &detail::la_win64_read_cb);
    archive_read_set_skip_callback(a, &detail::la_win64_skip_cb);
    archive_read_set_seek_callback(a, &detail::la_win64_seek_cb);
    archive_read_set_close_callback(a, &detail::la_win64_close_cb);
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

bool extract_pdf_metadata(const std::wstring& path, ComicMetadata& out) {
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

    ScopeExit handle_guard([&h]() {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
    });

    LARGE_INTEGER size_li{};
    if (!GetFileSizeEx(h, &size_li) || size_li.QuadPart <= 0) {
        return false;
    }

    detail::PdfiumRuntime::acquire();
    ScopeExit runtime_guard([]() { detail::PdfiumRuntime::release(); });

    detail::Win64PdfStream stream;
    stream.handle = h;
    stream.file_size = size_li.QuadPart;

    FPDF_FILEACCESS access{};
    access.m_FileLen = (size_li.QuadPart > static_cast<int64_t>(std::numeric_limits<unsigned long>::max()))
        ? std::numeric_limits<unsigned long>::max()
        : static_cast<unsigned long>(size_li.QuadPart);
    access.m_GetBlock = &detail::win64_pdf_get_block;
    access.m_Param = &stream;

    std::lock_guard<std::mutex> pdf_lock(detail::pdfium_global_mutex());
    FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&access, nullptr);
    if (!doc) {
        return false;
    }
    ScopeExit doc_guard([&doc]() {
        if (doc) {
            FPDF_CloseDocument(doc);
            doc = nullptr;
        }
    });

    out.author = read_pdf_meta_tag_locked(doc, "Author");
    out.series = read_pdf_meta_tag_locked(doc, "Subject");
    out.tags   = read_pdf_meta_tag_locked(doc, "Keywords");
    return !out.empty();
}

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
                in_out.author = trim_ascii(m[1].str());
            }
            if (in_out.tags.empty() && m.size() > 3 && m[3].matched) {
                in_out.tags = trim_ascii(m[3].str());
            }
        }
    } catch (...) {
        // Ignore regex errors on malformed strings
    }
}

void extract_comic_metadata(const std::filesystem::path& file_path,
                            const std::string& format,
                            const std::string& filename_stem,
                            ComicMetadata& out) {
    if (format == "pdf") {
        extract_pdf_metadata(file_path.wstring(), out);
    } else if (format == "cbz" || format == "cbr" || format == "cb7" || format == "epub" || format == "cbt") {
        extract_archive_metadata(file_path.wstring(), out);
    }

    if (out.author.empty() || out.tags.empty()) {
        parse_filename_metadata(filename_stem, out);
    }
}

} // namespace koma
