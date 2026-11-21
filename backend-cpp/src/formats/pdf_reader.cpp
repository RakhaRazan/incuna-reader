#include "formats/comic_reader.hpp"
#include "formats/image_utils.hpp"
#include "formats/pdfium_runtime.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace koma {

namespace detail {

std::mutex& pdfium_global_mutex() {
    static std::mutex m;
    return m;
}

namespace {
int& pdfium_ref_count() {
    static int count = 0;
    return count;
}
} // namespace

void PdfiumRuntime::acquire() {
    std::lock_guard<std::mutex> lock(pdfium_global_mutex());
    if (pdfium_ref_count() == 0) {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_pIsolate = nullptr;
        config.m_v8EmbedderSlot = 0;
        FPDF_InitLibraryWithConfig(&config);
    }
    ++pdfium_ref_count();
}

void PdfiumRuntime::release() {
    std::lock_guard<std::mutex> lock(pdfium_global_mutex());
    if (pdfium_ref_count() > 0) {
        --pdfium_ref_count();
        if (pdfium_ref_count() == 0) {
            FPDF_DestroyLibrary();
        }
    }
}

int win64_pdf_get_block(void* param,
                        unsigned long position,
                        unsigned char* pBuf,
                        unsigned long size) {
    if (!param || (!pBuf && size > 0)) return 0;
    auto* stream = static_cast<Win64PdfStream*>(param);
    if (stream->handle == INVALID_HANDLE_VALUE) return 0;
    if (size == 0) return 1;

    std::lock_guard<std::mutex> lock(stream->io_mutex);

    const int64_t offset64 = static_cast<int64_t>(position);
    if (offset64 < 0 || offset64 + static_cast<int64_t>(size) > stream->file_size) {
        return 0;
    }

    // 1. Fast path: serve directly from 64 KB read-ahead buffer (0 syscalls)
    if (stream->buf_start >= 0 &&
        offset64 >= stream->buf_start &&
        offset64 + static_cast<int64_t>(size) <= stream->buf_start + static_cast<int64_t>(stream->buf_valid)) {
        const size_t rel = static_cast<size_t>(offset64 - stream->buf_start);
        std::memcpy(pBuf, stream->read_buf.data() + rel, size);
        return 1;
    }

    // 2. Buffered read-ahead for small/medium reads (<= 64 KB)
    if (size <= Win64PdfStream::kReadAheadBytes) {
        if (stream->read_buf.size() < Win64PdfStream::kReadAheadBytes) {
            stream->read_buf.resize(Win64PdfStream::kReadAheadBytes);
        }

        LARGE_INTEGER li{};
        li.QuadPart = offset64;
        if (!SetFilePointerEx(stream->handle, li, nullptr, FILE_BEGIN)) {
            return 0;
        }

        const size_t want = static_cast<size_t>(
            std::min<int64_t>(static_cast<int64_t>(Win64PdfStream::kReadAheadBytes),
                              stream->file_size - offset64));
        size_t total_got = 0;
        while (total_got < size) {
            DWORD chunk = 0;
            const DWORD to_read = static_cast<DWORD>(want - total_got);
            if (!ReadFile(stream->handle, stream->read_buf.data() + total_got, to_read, &chunk, nullptr) ||
                chunk == 0) {
                stream->buf_start = -1;
                stream->buf_valid = 0;
                return 0;
            }
            total_got += chunk;
        }

        stream->buf_start = offset64;
        stream->buf_valid = total_got;
        std::memcpy(pBuf, stream->read_buf.data(), size);
        return 1;
    }

    // 3. Large contiguous read (> 64 KB): read directly into caller buffer
    LARGE_INTEGER li{};
    li.QuadPart = offset64;
    if (!SetFilePointerEx(stream->handle, li, nullptr, FILE_BEGIN)) {
        return 0;
    }

    unsigned long total_read = 0;
    while (total_read < size) {
        DWORD chunk = 0;
        const DWORD to_read = static_cast<DWORD>(size - total_read);
        if (!ReadFile(stream->handle, pBuf + total_read, to_read, &chunk, nullptr)) {
            return 0;
        }
        if (chunk == 0) {
            return 0; // Unexpected EOF
        }
        total_read += chunk;
    }
    return 1;
}

} // namespace detail

namespace {

using detail::PdfiumRuntime;
using detail::Win64PdfStream;
using detail::pdfium_global_mutex;
using detail::win64_pdf_get_block;

template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}
    ~ScopeExit() {
        if (active_) fn_();
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    void dismiss() { active_ = false; }

private:
    Fn fn_;
    bool active_ = true;
};

// RAII owner for FPDF_DOCUMENT ensuring FPDF_CloseDocument is always invoked under pdfium_global_mutex().
class ScopedPdfDocument {
public:
    ScopedPdfDocument() = default;
    explicit ScopedPdfDocument(FPDF_DOCUMENT doc) : doc_(doc) {}
    ~ScopedPdfDocument() { reset(); }

    ScopedPdfDocument(const ScopedPdfDocument&) = delete;
    ScopedPdfDocument& operator=(const ScopedPdfDocument&) = delete;

    void reset() {
        std::lock_guard<std::mutex> lock(pdfium_global_mutex());
        reset_locked(nullptr);
    }

    // Must be called while pdfium_global_mutex() is already held.
    void reset_locked(FPDF_DOCUMENT new_doc = nullptr) {
        if (doc_) {
            FPDF_CloseDocument(doc_);
        }
        doc_ = new_doc;
    }

    FPDF_DOCUMENT get() const { return doc_; }
    explicit operator bool() const { return doc_ != nullptr; }

private:
    FPDF_DOCUMENT doc_ = nullptr;
};

// RAII owner for FPDF_PAGE (must be constructed and destroyed while pdfium_global_mutex() is held).
class ScopedPdfPage {
public:
    explicit ScopedPdfPage(FPDF_PAGE page) : page_(page) {}
    ~ScopedPdfPage() {
        if (page_) {
            FPDF_ClosePage(page_);
        }
    }
    ScopedPdfPage(const ScopedPdfPage&) = delete;
    ScopedPdfPage& operator=(const ScopedPdfPage&) = delete;

    FPDF_PAGE get() const { return page_; }
    explicit operator bool() const { return page_ != nullptr; }

private:
    FPDF_PAGE page_ = nullptr;
};

// RAII owner for FPDF_BITMAP (must be constructed and destroyed while pdfium_global_mutex() is held).
class ScopedPdfBitmap {
public:
    explicit ScopedPdfBitmap(FPDF_BITMAP bmp) : bmp_(bmp) {}
    ~ScopedPdfBitmap() {
        if (bmp_) {
            FPDFBitmap_Destroy(bmp_);
        }
    }
    ScopedPdfBitmap(const ScopedPdfBitmap&) = delete;
    ScopedPdfBitmap& operator=(const ScopedPdfBitmap&) = delete;

    FPDF_BITMAP get() const { return bmp_; }
    explicit operator bool() const { return bmp_ != nullptr; }

private:
    FPDF_BITMAP bmp_ = nullptr;
};

std::string fpdf_error_string(unsigned long err) {
    switch (err) {
        case FPDF_ERR_SUCCESS:  return "Success";
        case FPDF_ERR_FILE:     return "File not found or could not be opened";
        case FPDF_ERR_FORMAT:   return "File is not in PDF format or is corrupted";
        case FPDF_ERR_PASSWORD: return "PDF requires a password";
        case FPDF_ERR_SECURITY: return "Unsupported PDF security scheme";
        case FPDF_ERR_PAGE:     return "Page not found or content error";
        default:                return "Unknown PDFium error (" + std::to_string(err) + ")";
    }
}

// Fast-path embedded image extraction for single-image manga PDF pages (Page 0).
// Must be called while pdfium_global_mutex() is held.
bool extract_single_image_thumbnail(FPDF_PAGE page,
                                    int max_w,
                                    int max_h,
                                    DecodedImage& out_decoded,
                                    int& out_orig_w,
                                    int& out_orig_h) {
    if (FPDFPage_GetRotation(page) != 0) {
        return false;
    }
    if (FPDFPage_CountObjects(page) != 1) {
        return false;
    }

    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, 0);
    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) {
        return false;
    }

    // Fall back to full page render if the image object is rotated, sheared, or flipped.
    FS_MATRIX mat{};
    if (FPDFPageObj_GetMatrix(obj, &mat)) {
        if (std::fabs(mat.b) > 1e-3f || std::fabs(mat.c) > 1e-3f || mat.a <= 0.0f || mat.d <= 0.0f) {
            return false;
        }
    }

    ScopedPdfBitmap img_bmp(FPDFImageObj_GetBitmap(obj));
    if (!img_bmp) {
        return false;
    }

    const int src_w = FPDFBitmap_GetWidth(img_bmp.get());
    const int src_h = FPDFBitmap_GetHeight(img_bmp.get());
    const int stride = FPDFBitmap_GetStride(img_bmp.get());
    const int fmt = FPDFBitmap_GetFormat(img_bmp.get());
    const auto* src_buf = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(img_bmp.get()));
    if (!src_buf || src_w <= 0 || src_h <= 0 || stride <= 0) {
        return false;
    }

    int bpp = 0;
    switch (fmt) {
        case FPDFBitmap_Gray: bpp = 1; break;
        case FPDFBitmap_BGR:  bpp = 3; break;
        case FPDFBitmap_BGRx:
        case FPDFBitmap_BGRA: bpp = 4; break;
        default: return false;
    }
    if (stride < src_w * bpp) {
        return false;
    }

    int dst_w = src_w;
    int dst_h = src_h;
    if (max_w > 0 && max_h > 0) {
        const double sx = static_cast<double>(max_w) / static_cast<double>(src_w);
        const double sy = static_cast<double>(max_h) / static_cast<double>(src_h);
        const double scale = std::min({1.0, sx, sy});
        dst_w = std::max(1, static_cast<int>(std::lround(src_w * scale)));
        dst_h = std::max(1, static_cast<int>(std::lround(src_h * scale)));
    } else if (max_w > 0 && max_w < src_w) {
        const double scale = static_cast<double>(max_w) / static_cast<double>(src_w);
        dst_w = max_w;
        dst_h = std::max(1, static_cast<int>(std::lround(src_h * scale)));
    }

    auto sample_rgb = [fmt](const uint8_t* row, int x, uint32_t& r, uint32_t& g, uint32_t& b) {
        if (fmt == FPDFBitmap_Gray) {
            const uint32_t v = row[x];
            r = v;
            g = v;
            b = v;
        } else if (fmt == FPDFBitmap_BGR) {
            const uint8_t* p = row + static_cast<size_t>(x) * 3;
            b = p[0];
            g = p[1];
            r = p[2];
        } else if (fmt == FPDFBitmap_BGRx) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            b = p[0];
            g = p[1];
            r = p[2];
        } else {
            // FPDFBitmap_BGRA: composite over opaque white
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            const uint32_t a = p[3];
            if (a == 255) {
                b = p[0];
                g = p[1];
                r = p[2];
            } else {
                const uint32_t inv = 255 - a;
                b = (p[0] * a + 255 * inv) / 255;
                g = (p[1] * a + 255 * inv) / 255;
                r = (p[2] * a + 255 * inv) / 255;
            }
        }
    };

    out_decoded.width = dst_w;
    out_decoded.height = dst_h;
    out_decoded.rgba.resize(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 4);
    uint8_t* dst = out_decoded.rgba.data();

    if (dst_w == src_w && dst_h == src_h) {
        for (int y = 0; y < dst_h; ++y) {
            const uint8_t* row = src_buf + static_cast<size_t>(y) * static_cast<size_t>(stride);
            uint8_t* out_row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_w) * 4;
            for (int x = 0; x < dst_w; ++x) {
                uint32_t r = 0, g = 0, b = 0;
                sample_rgb(row, x, r, g, b);
                out_row[x * 4 + 0] = static_cast<uint8_t>(r);
                out_row[x * 4 + 1] = static_cast<uint8_t>(g);
                out_row[x * 4 + 2] = static_cast<uint8_t>(b);
                out_row[x * 4 + 3] = 255;
            }
        }
    } else if (src_w >= dst_w * 2 && src_h >= dst_h * 2) {
        // Area-averaging (box) downsampler: crisp, anti-aliased, zero moire on high-DPI manga scans.
        for (int y = 0; y < dst_h; ++y) {
            const int y0 = static_cast<int>((static_cast<int64_t>(y) * src_h) / dst_h);
            const int y1 = std::min(src_h, std::max(y0 + 1, static_cast<int>((static_cast<int64_t>(y + 1) * src_h) / dst_h)));
            uint8_t* out_row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_w) * 4;

            for (int x = 0; x < dst_w; ++x) {
                const int x0 = static_cast<int>((static_cast<int64_t>(x) * src_w) / dst_w);
                const int x1 = std::min(src_w, std::max(x0 + 1, static_cast<int>((static_cast<int64_t>(x + 1) * src_w) / dst_w)));

                uint64_t sum_r = 0;
                uint64_t sum_g = 0;
                uint64_t sum_b = 0;
                const uint64_t count = static_cast<uint64_t>(y1 - y0) * static_cast<uint64_t>(x1 - x0);

                for (int sy = y0; sy < y1; ++sy) {
                    const uint8_t* row = src_buf + static_cast<size_t>(sy) * static_cast<size_t>(stride);
                    for (int sx = x0; sx < x1; ++sx) {
                        uint32_t r = 0, g = 0, b = 0;
                        sample_rgb(row, sx, r, g, b);
                        sum_r += r;
                        sum_g += g;
                        sum_b += b;
                    }
                }

                out_row[x * 4 + 0] = static_cast<uint8_t>((sum_r + count / 2) / count);
                out_row[x * 4 + 1] = static_cast<uint8_t>((sum_g + count / 2) / count);
                out_row[x * 4 + 2] = static_cast<uint8_t>((sum_b + count / 2) / count);
                out_row[x * 4 + 3] = 255;
            }
        }
    } else {
        // Smooth bilinear downsampling for modest scale factors (<2x).
        const double x_ratio = static_cast<double>(src_w - 1) / std::max(1, dst_w - 1);
        const double y_ratio = static_cast<double>(src_h - 1) / std::max(1, dst_h - 1);

        for (int y = 0; y < dst_h; ++y) {
            const double sy = y * y_ratio;
            const int y0 = static_cast<int>(sy);
            const int y1 = std::min(y0 + 1, src_h - 1);
            const double fy = sy - y0;
            const uint8_t* row0 = src_buf + static_cast<size_t>(y0) * static_cast<size_t>(stride);
            const uint8_t* row1 = src_buf + static_cast<size_t>(y1) * static_cast<size_t>(stride);
            uint8_t* out_row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_w) * 4;

            for (int x = 0; x < dst_w; ++x) {
                const double sx = x * x_ratio;
                const int x0 = static_cast<int>(sx);
                const int x1 = std::min(x0 + 1, src_w - 1);
                const double fx = sx - x0;

                uint32_t r00 = 0, g00 = 0, b00 = 0;
                uint32_t r10 = 0, g10 = 0, b10 = 0;
                uint32_t r01 = 0, g01 = 0, b01 = 0;
                uint32_t r11 = 0, g11 = 0, b11 = 0;
                sample_rgb(row0, x0, r00, g00, b00);
                sample_rgb(row0, x1, r10, g10, b10);
                sample_rgb(row1, x0, r01, g01, b01);
                sample_rgb(row1, x1, r11, g11, b11);

                const double r = (r00 * (1.0 - fx) + r10 * fx) * (1.0 - fy) + (r01 * (1.0 - fx) + r11 * fx) * fy;
                const double g = (g00 * (1.0 - fx) + g10 * fx) * (1.0 - fy) + (g01 * (1.0 - fx) + g11 * fx) * fy;
                const double b = (b00 * (1.0 - fx) + b10 * fx) * (1.0 - fy) + (b01 * (1.0 - fx) + b11 * fx) * fy;

                out_row[x * 4 + 0] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(r)), 0, 255));
                out_row[x * 4 + 1] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(g)), 0, 255));
                out_row[x * 4 + 2] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(b)), 0, 255));
                out_row[x * 4 + 3] = 255;
            }
        }
    }

    out_orig_w = src_w;
    out_orig_h = src_h;
    return true;
}

} // namespace

class PdfReader final : public ComicReader {
public:
    PdfReader() = default;
    ~PdfReader() override { close(); }

    bool open(const std::wstring& path) override {
        std::lock_guard<std::mutex> doc_lock(doc_mutex_);
        close_locked();

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
            last_error_ = "GetFileSizeEx failed or empty PDF file";
            return false;
        }

        PdfiumRuntime::acquire();
        pdfium_acquired_ = true;

        stream_ = std::make_unique<Win64PdfStream>();
        stream_->handle = h;
        stream_->file_size = size_li.QuadPart;

        file_access_.m_FileLen = (size_li.QuadPart > static_cast<int64_t>(std::numeric_limits<unsigned long>::max()))
            ? std::numeric_limits<unsigned long>::max()
            : static_cast<unsigned long>(size_li.QuadPart);
        file_access_.m_GetBlock = &win64_pdf_get_block;
        file_access_.m_Param = stream_.get();

        // RAII scope guard guarantees FPDF_CloseDocument, HANDLE close, and PdfiumRuntime::release
        // on every early exit (including corrupt or password-protected PDFs).
        ScopeExit cleanup([this]() { close_locked(); });

        unsigned long load_err = FPDF_ERR_SUCCESS;
        {
            std::lock_guard<std::mutex> pdf_lock(pdfium_global_mutex());
            // Zero-full-load: PDFium only seeks to trailer/xref via win64_pdf_get_block
            FPDF_DOCUMENT loaded = FPDF_LoadCustomDocument(&file_access_, nullptr);
            ScopeExit doc_guard([&loaded]() {
                // Always invoke FPDF_CloseDocument via RAII guard (safe no-op if loaded == nullptr)
                FPDF_CloseDocument(loaded);
                loaded = nullptr;
            });

            if (!loaded) {
                load_err = FPDF_GetLastError();
            } else {
                const int pages = FPDF_GetPageCount(loaded);
                if (pages > 0) {
                    doc_.reset_locked(loaded);
                    loaded = nullptr;
                    doc_guard.dismiss();
                    page_count_ = pages;
                }
            }
        }

        if (!doc_) {
            if (load_err != FPDF_ERR_SUCCESS) {
                last_error_ = "FPDF_LoadCustomDocument failed: " + fpdf_error_string(load_err);
            } else {
                last_error_ = "PDF contains 0 pages";
            }
            return false;
        }

        cleanup.dismiss();
        last_error_.clear();
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(doc_mutex_);
        close_locked();
    }

    bool is_open() const override {
        return static_cast<bool>(doc_) && page_count_ > 0;
    }

    int page_count() const override {
        return page_count_;
    }

    int64_t file_size() const override {
        return stream_ ? stream_->file_size : 0;
    }

    ComicFormat format() const override {
        return ComicFormat::Pdf;
    }

    std::string last_error() const override {
        return last_error_;
    }

    RenderedPage render_page(int index, int target_width, bool auto_crop) override {
        return render_page_internal(index, target_width, 0, auto_crop, /*is_thumbnail=*/false);
    }

    RenderedPage extract_cover(int max_w, int max_h) override {
        // Render or fast-extract page 0 into a crisp anti-aliased <= 220x310 thumbnail
        // encoded at JPEG quality 82.
        const int thumb_w = (max_w > 0) ? std::min(max_w, 220) : 220;
        const int thumb_h = (max_h > 0) ? std::min(max_h, 310) : 310;
        return render_page_internal(0, thumb_w, thumb_h, /*auto_crop=*/false, /*is_thumbnail=*/true);
    }

private:
    void close_locked() {
        doc_.reset();
        if (stream_) {
            if (stream_->handle != INVALID_HANDLE_VALUE) {
                CloseHandle(stream_->handle);
                stream_->handle = INVALID_HANDLE_VALUE;
            }
            stream_.reset();
        }
        if (pdfium_acquired_) {
            PdfiumRuntime::release();
            pdfium_acquired_ = false;
        }
        page_count_ = 0;
    }

    RenderedPage render_page_internal(int index,
                                      int target_width,
                                      int max_height,
                                      bool auto_crop,
                                      bool is_thumbnail) {
        if (!is_open() || index < 0 || index >= page_count_) {
            return {};
        }

        DecodedImage decoded;
        int orig_w = 0;
        int orig_h = 0;
        bool used_embedded_fast_path = false;

        {
            std::lock_guard<std::mutex> doc_lock(doc_mutex_);
            if (!doc_) return {};

            std::lock_guard<std::mutex> pdf_lock(pdfium_global_mutex());
            ScopedPdfPage page(FPDF_LoadPage(doc_.get(), index));
            if (!page) {
                return {};
            }

            // Fast-path for single-image manga PDF covers: extract embedded image bitmap
            // directly and downscale smoothly without running full PDF page composition.
            if (is_thumbnail && index == 0 &&
                extract_single_image_thumbnail(page.get(), target_width, max_height, decoded, orig_w, orig_h)) {
                used_embedded_fast_path = true;
            } else {
                const double pt_w = FPDF_GetPageWidth(page.get());
                const double pt_h = FPDF_GetPageHeight(page.get());
                if (pt_w <= 0.0 || pt_h <= 0.0) {
                    return {};
                }

                // Native 150 DPI baseline (150 / 72 = 2.0833x)
                constexpr double kDefaultDpiScale = 150.0 / 72.0;
                orig_w = std::max(1, static_cast<int>(std::lround(pt_w * kDefaultDpiScale)));
                orig_h = std::max(1, static_cast<int>(std::lround(pt_h * kDefaultDpiScale)));

                int render_w = orig_w;
                int render_h = orig_h;

                if (target_width > 0 && max_height > 0) {
                    const double sx = static_cast<double>(target_width) / pt_w;
                    const double sy = static_cast<double>(max_height) / pt_h;
                    const double s = std::min(sx, sy);
                    render_w = std::max(1, static_cast<int>(std::lround(pt_w * s)));
                    render_h = std::max(1, static_cast<int>(std::lround(pt_h * s)));
                } else if (target_width > 0) {
                    // Render directly at the requested viewport target_width (e.g., 1800px)
                    // instead of giant 300-DPI bitmaps to eliminate GPU texture upload stutter.
                    render_w = target_width;
                    render_h = std::max(1, static_cast<int>(std::lround(pt_h * (static_cast<double>(target_width) / pt_w))));
                    if (render_h > 8192) {
                        const double s = 8192.0 / static_cast<double>(render_h);
                        render_h = 8192;
                        render_w = std::max(1, static_cast<int>(std::lround(render_w * s)));
                    }
                } else {
                    // Cap default native auto-render width at 2400px to keep RAM tightly bounded
                    if (render_w > 2400) {
                        const double s = 2400.0 / static_cast<double>(render_w);
                        render_w = 2400;
                        render_h = std::max(1, static_cast<int>(std::lround(render_h * s)));
                    }
                }

                decoded.width = render_w;
                decoded.height = render_h;
                decoded.rgba.resize(static_cast<size_t>(render_w) * static_cast<size_t>(render_h) * 4);

                ScopedPdfBitmap bitmap(FPDFBitmap_CreateEx(
                    render_w, render_h, FPDFBitmap_BGRA, decoded.rgba.data(), render_w * 4));
                if (!bitmap) {
                    return {};
                }

                // Fill opaque white background and render page smoothly at target size.
                // Keep PDFium image/path smoothing enabled (no FPDF_RENDER_NO_SMOOTHIMAGE/PATH)
                // to eliminate nearest-neighbor moire/pixelation on manga thumbnails.
                const int render_flags = is_thumbnail ? 0 : FPDF_ANNOT;
                FPDFBitmap_FillRect(bitmap.get(), 0, 0, render_w, render_h, 0xFFFFFFFF);
                FPDF_RenderPageBitmap(bitmap.get(), page.get(), 0, 0, render_w, render_h, 0, render_flags);
            }
        }

        if (!used_embedded_fast_path) {
            // Convert BGRA -> RGBA in place
            const size_t pixel_count = static_cast<size_t>(decoded.width) * static_cast<size_t>(decoded.height);
            uint8_t* p = decoded.rgba.data();
            for (size_t i = 0; i < pixel_count; ++i) {
                std::swap(p[i * 4 + 0], p[i * 4 + 2]);
            }
        }

        if (auto_crop) {
            decoded = auto_crop_borders(decoded);
        }

        if (is_thumbnail) {
            return encode_rendered_page(decoded, index, orig_w, orig_h, /*prefer_webp=*/false, /*quality=*/82);
        }
        return encode_rendered_page(decoded, index, orig_w, orig_h, /*prefer_webp=*/false, /*quality=*/88);
    }

    mutable std::mutex doc_mutex_;
    std::unique_ptr<Win64PdfStream> stream_;
    FPDF_FILEACCESS file_access_{};
    ScopedPdfDocument doc_;
    bool pdfium_acquired_ = false;
    int page_count_ = 0;
    std::string last_error_;
};

std::unique_ptr<ComicReader> create_pdf_reader() {
    return std::make_unique<PdfReader>();
}

} // namespace koma
