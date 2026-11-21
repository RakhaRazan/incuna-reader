#include "formats/image_utils.hpp"
#include "utils/string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include <webp/decode.h>
#include <webp/encode.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4267 4456 4457 4996)
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace koma {

namespace {

bool is_webp_magic(const uint8_t* data, size_t size) {
    return size >= 12 &&
           std::memcmp(data, "RIFF", 4) == 0 &&
           std::memcmp(data + 8, "WEBP", 4) == 0;
}

void stbi_vec_write_cb(void* context, void* data, int size) {
    if (!context || !data || size <= 0) return;
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

inline bool pixel_is_uniform_border(uint8_t r, uint8_t g, uint8_t b,
                                    uint8_t ref_r, uint8_t ref_g, uint8_t ref_b,
                                    bool is_white_or_black) {
    if (is_white_or_black) {
        // Pure/near white or pure/near black scanner margin
        const bool near_white = (r >= 240 && g >= 240 && b >= 240);
        const bool near_black = (r <= 18 && g <= 18 && b <= 18);
        if (ref_r >= 240 && ref_g >= 240 && ref_b >= 240) return near_white;
        if (ref_r <= 18 && ref_g <= 18 && ref_b <= 18) return near_black;
    }
    const int dr = std::abs(static_cast<int>(r) - static_cast<int>(ref_r));
    const int dg = std::abs(static_cast<int>(g) - static_cast<int>(ref_g));
    const int db = std::abs(static_cast<int>(b) - static_cast<int>(ref_b));
    return dr <= 18 && dg <= 18 && db <= 18;
}

} // namespace

bool is_supported_image_filename(const std::string& filename) {
    if (filename.empty()) return false;
    // Skip macOS resource forks and hidden dotfiles
    if (filename.find("__MACOSX/") != std::string::npos ||
        filename.find("__MACOSX\\") != std::string::npos) {
        return false;
    }
    const size_t slash = filename.find_last_of("/\\");
    const std::string base = (slash == std::string::npos) ? filename : filename.substr(slash + 1);
    if (base.empty() || base[0] == '.') return false;

    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = to_lower_ascii(base.substr(dot));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
           ext == ".gif" || ext == ".bmp"  || ext == ".webp";
}

bool is_supported_image_filename_wide(const std::wstring& filename) {
    if (filename.empty()) return false;
    const size_t slash = filename.find_last_of(L"/\\");
    const std::wstring base = (slash == std::wstring::npos) ? filename : filename.substr(slash + 1);
    if (base.empty() || base[0] == L'.') return false;
    const size_t dot = base.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = to_lower_wide(base.substr(dot));
    return ext == L".jpg" || ext == L".jpeg" || ext == L".png" ||
           ext == L".gif" || ext == L".bmp"  || ext == L".webp";
}

DecodedImage decode_image_bytes(const uint8_t* data, size_t size) {
    DecodedImage img;
    if (!data || size == 0) return img;

    // 1. WebP fast path via libwebp
    if (is_webp_magic(data, size)) {
        int w = 0;
        int h = 0;
        uint8_t* decoded = WebPDecodeRGBA(data, size, &w, &h);
        if (decoded && w > 0 && h > 0) {
            img.width = w;
            img.height = h;
            const size_t byte_count = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
            img.rgba.assign(decoded, decoded + byte_count);
            WebPFree(decoded);
            return img;
        }
        if (decoded) WebPFree(decoded);
    }

    // 2. JPEG / PNG / GIF / BMP via stb_image
    if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return img;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        data, static_cast<int>(size), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return img;
    }

    img.width = w;
    img.height = h;
    const size_t byte_count = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    img.rgba.assign(pixels, pixels + byte_count);
    stbi_image_free(pixels);
    return img;
}

DecodedImage auto_crop_borders(const DecodedImage& src) {
    if (!src.valid() || src.width < 64 || src.height < 64) {
        return src;
    }

    const int w = src.width;
    const int h = src.height;
    const uint8_t* px = src.rgba.data();

    auto sample_pixel = [&](int x, int y, uint8_t& r, uint8_t& g, uint8_t& b) {
        const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
        r = px[off];
        g = px[off + 1];
        b = px[off + 2];
    };

    // Determine corner reference color (only crop if border is uniform white or black)
    uint8_t tl_r = 0, tl_g = 0, tl_b = 0;
    sample_pixel(0, 0, tl_r, tl_g, tl_b);
    const bool corner_white = (tl_r >= 240 && tl_g >= 240 && tl_b >= 240);
    const bool corner_black = (tl_r <= 18 && tl_g <= 18 && tl_b <= 18);

    uint8_t br_r = 0, br_g = 0, br_b = 0;
    sample_pixel(w - 1, h - 1, br_r, br_g, br_b);
    const bool br_white = (br_r >= 240 && br_g >= 240 && br_b >= 240);
    const bool br_black = (br_r <= 18 && br_g <= 18 && br_b <= 18);

    if (!corner_white && !corner_black && !br_white && !br_black) {
        return src;
    }

    const uint8_t ref_r = (corner_white || corner_black) ? tl_r : br_r;
    const uint8_t ref_g = (corner_white || corner_black) ? tl_g : br_g;
    const uint8_t ref_b = (corner_white || corner_black) ? tl_b : br_b;

    const int max_crop_y = static_cast<int>(h * 0.35);
    const int max_crop_x = static_cast<int>(w * 0.35);
    const int step_x = std::max(1, w / 256);
    const int step_y = std::max(1, h / 256);

    auto row_is_border = [&](int y) {
        int total = 0;
        int matching = 0;
        for (int x = 0; x < w; x += step_x) {
            uint8_t r, g, b;
            sample_pixel(x, y, r, g, b);
            ++total;
            if (pixel_is_uniform_border(r, g, b, ref_r, ref_g, ref_b, true)) {
                ++matching;
            }
        }
        return total > 0 && (matching * 100 >= total * 98);
    };

    auto col_is_border = [&](int x, int y_top, int y_bot) {
        int total = 0;
        int matching = 0;
        for (int y = y_top; y <= y_bot; y += step_y) {
            uint8_t r, g, b;
            sample_pixel(x, y, r, g, b);
            ++total;
            if (pixel_is_uniform_border(r, g, b, ref_r, ref_g, ref_b, true)) {
                ++matching;
            }
        }
        return total > 0 && (matching * 100 >= total * 98);
    };

    int top = 0;
    while (top < max_crop_y && row_is_border(top)) ++top;

    int bottom = h - 1;
    while (bottom > h - 1 - max_crop_y && bottom > top + 32 && row_is_border(bottom)) --bottom;

    int left = 0;
    while (left < max_crop_x && col_is_border(left, top, bottom)) ++left;

    int right = w - 1;
    while (right > w - 1 - max_crop_x && right > left + 32 && col_is_border(right, top, bottom)) --right;

    const int new_w = right - left + 1;
    const int new_h = bottom - top + 1;

    // Require meaningful crop (>= 2px) and minimum remaining size
    if (new_w < 32 || new_h < 32 || (left == 0 && top == 0 && new_w == w && new_h == h)) {
        return src;
    }

    DecodedImage cropped;
    cropped.width = new_w;
    cropped.height = new_h;
    cropped.rgba.resize(static_cast<size_t>(new_w) * static_cast<size_t>(new_h) * 4);

    for (int y = 0; y < new_h; ++y) {
        const size_t src_off = (static_cast<size_t>(top + y) * static_cast<size_t>(w) + static_cast<size_t>(left)) * 4;
        const size_t dst_off = static_cast<size_t>(y) * static_cast<size_t>(new_w) * 4;
        std::memcpy(cropped.rgba.data() + dst_off, px + src_off, static_cast<size_t>(new_w) * 4);
    }

    return cropped;
}

DecodedImage resize_image(const DecodedImage& src, int target_width, int max_height) {
    if (!src.valid()) return {};
    if (target_width <= 0 && max_height <= 0) return src;

    int dst_w = src.width;
    int dst_h = src.height;

    if (target_width > 0 && max_height > 0) {
        // Fit within (target_width, max_height) without upscaling
        const double sx = static_cast<double>(target_width) / static_cast<double>(src.width);
        const double sy = static_cast<double>(max_height) / static_cast<double>(src.height);
        const double scale = std::min({1.0, sx, sy});
        dst_w = std::max(1, static_cast<int>(std::lround(src.width * scale)));
        dst_h = std::max(1, static_cast<int>(std::lround(src.height * scale)));
    } else if (target_width > 0) {
        if (target_width >= src.width) {
            return src; // Avoid wasteful upscaling
        }
        const double scale = static_cast<double>(target_width) / static_cast<double>(src.width);
        dst_w = target_width;
        dst_h = std::max(1, static_cast<int>(std::lround(src.height * scale)));
    } else if (max_height > 0 && max_height < src.height) {
        const double scale = static_cast<double>(max_height) / static_cast<double>(src.height);
        dst_h = max_height;
        dst_w = std::max(1, static_cast<int>(std::lround(src.width * scale)));
    }

    if (dst_w == src.width && dst_h == src.height) {
        return src;
    }

    DecodedImage dst;
    dst.width = dst_w;
    dst.height = dst_h;
    dst.rgba.resize(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 4);

    if (src.width >= dst_w * 2 && src.height >= dst_h * 2) {
        // Area-averaging (box) downsampler for >=2x shrink: crisp, anti-aliased, zero moire on manga scans.
        for (int y = 0; y < dst_h; ++y) {
            const int y0 = static_cast<int>((static_cast<int64_t>(y) * src.height) / dst_h);
            const int y1 = std::min(src.height, std::max(y0 + 1, static_cast<int>((static_cast<int64_t>(y + 1) * src.height) / dst_h)));
            for (int x = 0; x < dst_w; ++x) {
                const int x0 = static_cast<int>((static_cast<int64_t>(x) * src.width) / dst_w);
                const int x1 = std::min(src.width, std::max(x0 + 1, static_cast<int>((static_cast<int64_t>(x + 1) * src.width) / dst_w)));

                uint64_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
                const uint64_t count = static_cast<uint64_t>(y1 - y0) * static_cast<uint64_t>(x1 - x0);
                for (int sy = y0; sy < y1; ++sy) {
                    const uint8_t* row = src.rgba.data() + static_cast<size_t>(sy) * static_cast<size_t>(src.width) * 4;
                    for (int sx = x0; sx < x1; ++sx) {
                        const uint8_t* p = row + static_cast<size_t>(sx) * 4;
                        sum_r += p[0];
                        sum_g += p[1];
                        sum_b += p[2];
                        sum_a += p[3];
                    }
                }
                const size_t out = (static_cast<size_t>(y) * dst_w + x) * 4;
                dst.rgba[out + 0] = static_cast<uint8_t>((sum_r + count / 2) / count);
                dst.rgba[out + 1] = static_cast<uint8_t>((sum_g + count / 2) / count);
                dst.rgba[out + 2] = static_cast<uint8_t>((sum_b + count / 2) / count);
                dst.rgba[out + 3] = static_cast<uint8_t>((sum_a + count / 2) / count);
            }
        }
        return dst;
    }

    // Bilinear resampling (fast, crisp, zero extra dependencies)
    const double x_ratio = static_cast<double>(src.width - 1) / std::max(1, dst_w - 1);
    const double y_ratio = static_cast<double>(src.height - 1) / std::max(1, dst_h - 1);

    for (int y = 0; y < dst_h; ++y) {
        const double sy = y * y_ratio;
        const int y0 = static_cast<int>(sy);
        const int y1 = std::min(y0 + 1, src.height - 1);
        const double fy = sy - y0;

        for (int x = 0; x < dst_w; ++x) {
            const double sx = x * x_ratio;
            const int x0 = static_cast<int>(sx);
            const int x1 = std::min(x0 + 1, src.width - 1);
            const double fx = sx - x0;

            const size_t i00 = (static_cast<size_t>(y0) * src.width + x0) * 4;
            const size_t i10 = (static_cast<size_t>(y0) * src.width + x1) * 4;
            const size_t i01 = (static_cast<size_t>(y1) * src.width + x0) * 4;
            const size_t i11 = (static_cast<size_t>(y1) * src.width + x1) * 4;
            const size_t out = (static_cast<size_t>(y) * dst_w + x) * 4;

            for (int c = 0; c < 4; ++c) {
                const double top = src.rgba[i00 + c] * (1.0 - fx) + src.rgba[i10 + c] * fx;
                const double bot = src.rgba[i01 + c] * (1.0 - fx) + src.rgba[i11 + c] * fx;
                const double val = top * (1.0 - fy) + bot * fy;
                dst.rgba[out + c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(val)), 0, 255));
            }
        }
    }

    return dst;
}

RenderedPage encode_rendered_page(
    const DecodedImage& img,
    int page_index,
    int original_width,
    int original_height,
    bool prefer_webp,
    int quality) {
    RenderedPage page;
    if (!img.valid()) return page;

    page.page_index = page_index;
    page.width = img.width;
    page.height = img.height;
    page.original_width = original_width > 0 ? original_width : img.width;
    page.original_height = original_height > 0 ? original_height : img.height;

    if (prefer_webp) {
        uint8_t* webp_out = nullptr;
        const size_t webp_size = WebPEncodeRGBA(
            img.rgba.data(),
            img.width,
            img.height,
            img.width * 4,
            static_cast<float>(std::clamp(quality, 10, 100)),
            &webp_out);
        if (webp_size > 0 && webp_out != nullptr) {
            page.mime_type = "image/webp";
            page.data.assign(webp_out, webp_out + webp_size);
            WebPFree(webp_out);
            return page;
        }
        if (webp_out) WebPFree(webp_out);
    }

    // Fallback to JPEG via stb_image_write (convert RGBA -> RGB)
    std::vector<uint8_t> rgb(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 3);
    const size_t pixel_count = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    for (size_t i = 0; i < pixel_count; ++i) {
        rgb[i * 3 + 0] = img.rgba[i * 4 + 0];
        rgb[i * 3 + 1] = img.rgba[i * 4 + 1];
        rgb[i * 3 + 2] = img.rgba[i * 4 + 2];
    }

    page.mime_type = "image/jpeg";
    stbi_write_jpg_to_func(
        stbi_vec_write_cb,
        &page.data,
        img.width,
        img.height,
        3,
        rgb.data(),
        std::clamp(quality, 10, 100));
    return page;
}

RenderedPage process_raw_image_page(
    const uint8_t* raw_data,
    size_t raw_size,
    int page_index,
    int target_width,
    int max_height,
    bool auto_crop,
    bool prefer_webp,
    int quality) {
    if (!raw_data || raw_size == 0) {
        return {};
    }

    // Zero-reencode fast path when image is already within target bounds, no auto-crop requested,
    // and payload is already compact (<= 2 MB) in a browser-native format (JPEG / WebP / PNG).
    if (!auto_crop && raw_size <= 2ULL * 1024ULL * 1024ULL) {
        int probe_w = 0;
        int probe_h = 0;
        std::string native_mime;
        if (is_webp_magic(raw_data, raw_size)) {
            if (WebPGetInfo(raw_data, raw_size, &probe_w, &probe_h)) {
                native_mime = "image/webp";
            }
        } else if (raw_size >= 3 && raw_data[0] == 0xFF && raw_data[1] == 0xD8 && raw_data[2] == 0xFF) {
            int comp = 0;
            if (stbi_info_from_memory(raw_data, static_cast<int>(raw_size), &probe_w, &probe_h, &comp)) {
                native_mime = "image/jpeg";
            }
        } else if (raw_size >= 8 && raw_data[0] == 0x89 && raw_data[1] == 'P' && raw_data[2] == 'N' && raw_data[3] == 'G') {
            int comp = 0;
            if (stbi_info_from_memory(raw_data, static_cast<int>(raw_size), &probe_w, &probe_h, &comp)) {
                native_mime = "image/png";
            }
        }

        if (!native_mime.empty() && probe_w > 0 && probe_h > 0 &&
            (target_width <= 0 || probe_w <= target_width) &&
            (max_height <= 0 || probe_h <= max_height)) {
            RenderedPage passthrough;
            passthrough.page_index = page_index;
            passthrough.width = probe_w;
            passthrough.height = probe_h;
            passthrough.original_width = probe_w;
            passthrough.original_height = probe_h;
            passthrough.mime_type = std::move(native_mime);
            passthrough.data.assign(raw_data, raw_data + raw_size);
            return passthrough;
        }
    }

    DecodedImage decoded = decode_image_bytes(raw_data, raw_size);
    if (!decoded.valid()) {
        return {};
    }

    const int orig_w = decoded.width;
    const int orig_h = decoded.height;

    if (auto_crop) {
        decoded = auto_crop_borders(decoded);
    }
    if (target_width > 0 || max_height > 0) {
        decoded = resize_image(decoded, target_width, max_height);
    }

    return encode_rendered_page(decoded, page_index, orig_w, orig_h, prefer_webp, quality);
}

} // namespace koma
