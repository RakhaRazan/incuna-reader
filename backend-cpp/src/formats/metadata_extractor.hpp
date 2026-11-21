#pragma once

#include <filesystem>
#include <string>

namespace koma {

struct ComicMetadata {
    std::string series;
    std::string author;
    std::string tags;
    std::string language;

    bool empty() const {
        return series.empty() && author.empty() && tags.empty() && language.empty();
    }
};

// Fast metadata extraction (<1ms for PDF via FPDF_GetMetaText; ZIP64 O(1) CD or libarchive for ComicInfo.xml).
bool extract_pdf_metadata(const std::wstring& path, ComicMetadata& out);
bool extract_archive_metadata(const std::wstring& path, ComicMetadata& out);

// Fallback filename parser: extracts [Author/Circle] and (Tag/Event) from filename stems when internal metadata is empty.
void parse_filename_metadata(const std::string& filename_stem, ComicMetadata& in_out);

// Unified extractor: reads internal PDF/ComicInfo.xml metadata and fills missing author/tags from filename stem.
void extract_comic_metadata(const std::filesystem::path& file_path,
                            const std::string& format,
                            const std::string& filename_stem,
                            ComicMetadata& out);

} // namespace koma
