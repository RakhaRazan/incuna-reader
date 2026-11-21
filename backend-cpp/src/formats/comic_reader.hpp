#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "formats/metadata_extractor.hpp"

namespace koma {

enum class ComicFormat {
    Unknown,
    Pdf,
    Cbz,
    Cbr,
    Cb7,
    Cbt,
    Epub,
    SingleImage,
    ImageFolder
};

inline const char* format_to_string(ComicFormat fmt) {
    switch (fmt) {
        case ComicFormat::Pdf:         return "pdf";
        case ComicFormat::Cbz:         return "cbz";
        case ComicFormat::Cbr:         return "cbr";
        case ComicFormat::Cb7:         return "cb7";
        case ComicFormat::Cbt:         return "cbt";
        case ComicFormat::Epub:        return "epub";
        case ComicFormat::SingleImage: return "image";
        case ComicFormat::ImageFolder: return "folder";
        default:                       return "unknown";
    }
}

struct RenderedPage {
    int page_index = -1;
    int width = 0;
    int height = 0;
    int original_width = 0;
    int original_height = 0;
    std::string mime_type;          // "image/webp" or "image/jpeg"
    std::vector<uint8_t> data;      // Encoded image bytes

    bool valid() const { return !data.empty() && width > 0 && height > 0; }
};

class ComicReader {
public:
    virtual ~ComicReader() = default;

    virtual bool open(const std::wstring& path) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual int page_count() const = 0;
    virtual int64_t file_size() const = 0;
    virtual ComicFormat format() const = 0;
    virtual std::string last_error() const = 0;

    // Renders page `index` (0-based).
    // target_width <= 0 preserves native/default resolution.
    // auto_crop == true trims uniform white/black margins.
    virtual RenderedPage render_page(int index, int target_width = 0, bool auto_crop = false) = 0;

    // Extracts cover thumbnail from page 0 fitted inside (max_w, max_h).
    virtual RenderedPage extract_cover(int max_w, int max_h) = 0;
};

// Concrete reader factory functions
std::unique_ptr<ComicReader> create_pdf_reader();
std::unique_ptr<ComicReader> create_archive_reader(const std::filesystem::path& stream_cache_dir = {});

// Auto-detects format by path/extension and returns the appropriate ComicReader (not yet opened).
std::unique_ptr<ComicReader> create_comic_reader(
    const std::wstring& path,
    const std::filesystem::path& stream_cache_dir = {});

// Detects ComicFormat from filesystem path.
ComicFormat detect_comic_format(const std::wstring& path);

// Fast O(1) ZIP64 extraction of Page 0 raw image bytes for sequential HDD cover-pack pipelines
// (returns false for non-ZIP64 formats like PDF/CBR/7Z so caller falls back to ComicReader).
bool extract_first_image_bytes_fast(const std::wstring& path, std::vector<uint8_t>& out_raw);

} // namespace koma
