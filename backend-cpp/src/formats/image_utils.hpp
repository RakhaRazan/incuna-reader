#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "formats/comic_reader.hpp"

namespace koma {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // 4 bytes per pixel (R, G, B, A)

    bool valid() const {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    }
};

// Returns true if filename has a supported image extension (.jpg, .jpeg, .png, .gif, .bmp, .webp).
bool is_supported_image_filename(const std::string& filename);
bool is_supported_image_filename_wide(const std::wstring& filename);

// Natural sort comparator (case-insensitive, numeric runs compared numerically: 1, 2, ..., 10).
bool natural_less(const std::string& a, const std::string& b);

// Decodes JPEG, PNG, GIF, BMP (stb_image) or WEBP (libwebp) into RGBA8888.
DecodedImage decode_image_bytes(const uint8_t* data, size_t size);

// Trims uniform white/black scanner margins while protecting against over-cropping.
DecodedImage auto_crop_borders(const DecodedImage& src);

// High-quality aspect-preserving resize.
// If target_width > 0 and max_height <= 0: scales width to target_width.
// If both target_width > 0 and max_height > 0: fits inside (target_width, max_height) without upscaling.
DecodedImage resize_image(const DecodedImage& src, int target_width, int max_height = 0);

// Encodes RGBA image to WebP (preferred) or JPEG fallback.
RenderedPage encode_rendered_page(
    const DecodedImage& img,
    int page_index,
    int original_width,
    int original_height,
    bool prefer_webp = true,
    int quality = 85);

// Full pipeline: decode -> optional auto-crop -> optional resize -> encode WebP/JPEG.
RenderedPage process_raw_image_page(
    const uint8_t* raw_data,
    size_t raw_size,
    int page_index,
    int target_width,
    int max_height,
    bool auto_crop,
    bool prefer_webp = true,
    int quality = 85);

} // namespace koma
