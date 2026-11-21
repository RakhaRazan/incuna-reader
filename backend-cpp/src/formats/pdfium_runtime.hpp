#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace koma::detail {

// Dedicated global mutex guarding ALL PDFium C API calls.
// PDFium is not thread-safe across concurrent worker threads even on separate documents.
std::mutex& pdfium_global_mutex();

// Thread-safe reference-counted PDFium library lifecycle manager.
class PdfiumRuntime {
public:
    static void acquire();
    static void release();
};

// 64-bit Win32 custom read stream context for PDFium FPDF_FILEACCESS.
// Includes a 64 KB sequential read-ahead buffer to avoid thousands of tiny disk syscalls on SATA 3 HDDs.
struct Win64PdfStream {
    static constexpr size_t kReadAheadBytes = 64 * 1024;
    HANDLE handle = INVALID_HANDLE_VALUE;
    int64_t file_size = 0;
    std::mutex io_mutex;
    std::vector<uint8_t> read_buf;
    int64_t buf_start = -1;
    size_t buf_valid = 0;
};

// FPDF_CUSTOM_READ callback backed by Win32 64-bit SetFilePointerEx + 64 KB buffered ReadFile.
int win64_pdf_get_block(void* param,
                        unsigned long position,
                        unsigned char* pBuf,
                        unsigned long size);

} // namespace koma::detail
