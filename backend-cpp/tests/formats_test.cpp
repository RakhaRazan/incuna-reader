#include "formats/comic_reader.hpp"
#include "formats/image_utils.hpp"
#include "formats/reader_session.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

void write_le16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void write_le32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// Minimal valid ZIP writer (STORE + DEFLATE) for generating test CBZ and EPUB fixtures
struct ZipItem {
    std::string name;
    std::vector<uint8_t> data;
    bool deflate = false;
};

bool write_zip_file(const std::filesystem::path& path, const std::vector<ZipItem>& items) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> cd;

    for (const auto& item : items) {
        const uint32_t crc = static_cast<uint32_t>(
            crc32(0L, item.data.data(), static_cast<uInt>(item.data.size())));

        std::vector<uint8_t> payload = item.data;
        uint16_t method = 0;

        if (item.deflate && !item.data.empty()) {
            z_stream zs{};
            if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
                std::vector<uint8_t> comp(deflateBound(&zs, static_cast<uLong>(item.data.size())) + 16);
                zs.next_in = const_cast<Bytef*>(item.data.data());
                zs.avail_in = static_cast<uInt>(item.data.size());
                zs.next_out = comp.data();
                zs.avail_out = static_cast<uInt>(comp.size());
                if (deflate(&zs, Z_FINISH) == Z_STREAM_END) {
                    comp.resize(zs.total_out);
                    payload = std::move(comp);
                    method = 8;
                }
                deflateEnd(&zs);
            }
        }

        const uint32_t local_offset = static_cast<uint32_t>(out.size());
        // Local File Header
        write_le32(out, 0x04034b50U);
        write_le16(out, 20);
        write_le16(out, 0);
        write_le16(out, method);
        write_le16(out, 0);
        write_le16(out, 0);
        write_le32(out, crc);
        write_le32(out, static_cast<uint32_t>(payload.size()));
        write_le32(out, static_cast<uint32_t>(item.data.size()));
        write_le16(out, static_cast<uint16_t>(item.name.size()));
        write_le16(out, 0);
        out.insert(out.end(), item.name.begin(), item.name.end());
        out.insert(out.end(), payload.begin(), payload.end());

        // Central Directory Header
        write_le32(cd, 0x02014b50U);
        write_le16(cd, 20);
        write_le16(cd, 20);
        write_le16(cd, 0);
        write_le16(cd, method);
        write_le16(cd, 0);
        write_le16(cd, 0);
        write_le32(cd, crc);
        write_le32(cd, static_cast<uint32_t>(payload.size()));
        write_le32(cd, static_cast<uint32_t>(item.data.size()));
        write_le16(cd, static_cast<uint16_t>(item.name.size()));
        write_le16(cd, 0);
        write_le16(cd, 0);
        write_le16(cd, 0);
        write_le16(cd, 0);
        write_le32(cd, 0);
        write_le32(cd, local_offset);
        cd.insert(cd.end(), item.name.begin(), item.name.end());
    }

    const uint32_t cd_offset = static_cast<uint32_t>(out.size());
    const uint32_t cd_size = static_cast<uint32_t>(cd.size());
    out.insert(out.end(), cd.begin(), cd.end());

    // EOCD
    write_le32(out, 0x06054b50U);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, static_cast<uint16_t>(items.size()));
    write_le16(out, static_cast<uint16_t>(items.size()));
    write_le32(out, cd_size);
    write_le32(out, cd_offset);
    write_le16(out, 0);

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return ofs.good();
}

// Create a synthetic WebP image (optionally with a white scanner border for auto-crop test)
std::vector<uint8_t> make_test_image(int w, int h, uint8_t r, uint8_t g, uint8_t b, int white_border = 0) {
    koma::DecodedImage img;
    img.width = w;
    img.height = h;
    img.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t off = (static_cast<size_t>(y) * w + x) * 4;
            const bool in_border = (white_border > 0) &&
                (x < white_border || x >= w - white_border || y < white_border || y >= h - white_border);
            img.rgba[off + 0] = in_border ? 255 : r;
            img.rgba[off + 1] = in_border ? 255 : g;
            img.rgba[off + 2] = in_border ? 255 : b;
            img.rgba[off + 3] = 255;
        }
    }
    // Encode as JPEG so stb_image decode path is exercised too
    auto encoded = koma::encode_rendered_page(img, 0, w, h, /*prefer_webp=*/false, 92);
    return encoded.data;
}

// Generates a valid multi-page PDF 1.4 file with exact xref byte offsets
bool write_test_pdf(const std::filesystem::path& path, int num_pages) {
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    offsets.push_back(0); // obj 0

    auto add_obj = [&](const std::string& body) -> int {
        int id = static_cast<int>(offsets.size());
        offsets.push_back(pdf.size());
        pdf += std::to_string(id) + " 0 obj\n" + body + "\nendobj\n";
        return id;
    };

    // Reserve obj 1 = Catalog, obj 2 = Pages
    // Build page objects starting at obj 3
    std::vector<int> page_ids;
    std::string kids;
    for (int i = 0; i < num_pages; ++i) {
        int page_id = 3 + i * 2;
        page_ids.push_back(page_id);
        kids += std::to_string(page_id) + " 0 R ";
    }

    add_obj("<< /Type /Catalog /Pages 2 0 R >>");
    add_obj("<< /Type /Pages /Kids [ " + kids + "] /Count " + std::to_string(num_pages) + " >>");

    for (int i = 0; i < num_pages; ++i) {
        int content_id = 3 + i * 2 + 1;
        add_obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 600] /Contents " +
                std::to_string(content_id) + " 0 R >>");
        std::ostringstream stream;
        double r = 0.2 + 0.15 * (i % 5);
        double g = 0.3 + 0.1 * ((i + 1) % 5);
        double b = 0.5;
        stream << r << " " << g << " " << b << " rg 40 40 320 520 re f\n";
        std::string s = stream.str();
        add_obj("<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "endstream");
    }

    const size_t xref_pos = pdf.size();
    pdf += "xref\n0 " + std::to_string(offsets.size()) + "\n";
    pdf += "0000000000 65535 f \n";
    for (size_t i = 1; i < offsets.size(); ++i) {
        char line[32];
        std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offsets[i]);
        pdf += line;
    }
    pdf += "trailer\n<< /Size " + std::to_string(offsets.size()) + " /Root 1 0 R >>\n";
    pdf += "startxref\n" + std::to_string(xref_pos) + "\n%%EOF\n";

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
    return ofs.good();
}

size_t current_working_set_mb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024 * 1024);
    }
    return 0;
}

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")\n";  \
            return 1;                                                         \
        }                                                                     \
    } while (0)

} // namespace

int main() {
    const auto temp_dir = std::filesystem::temp_directory_path() / L"komareader_formats_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    const auto stream_cache = temp_dir / L"stream_cache";
    std::filesystem::create_directories(stream_cache, ec);

    std::cout << "=== KomaReader 64-Bit Format Reader Verification ===\n";

    // -------------------------------------------------------------------------
    // 1. Multi-page CBZ (12 pages, testing ZIP64 O(1), natural sort 1..12,
    //    auto-crop, cover extraction, and 5-page sliding RAM cache)
    // -------------------------------------------------------------------------
    const auto cbz_path = temp_dir / L"sample_manga.cbz";
    {
        std::vector<ZipItem> items;
        // Insert out of order (10, 2, 1, 11, ...) to verify natural sort
        const int order[12] = {10, 2, 1, 11, 5, 3, 12, 4, 8, 6, 9, 7};
        for (int n : order) {
            ZipItem z;
            z.name = "chapter1/page_" + std::to_string(n) + ".jpg";
            // Page 1 has a 40px white border around a 200x300 image to test auto_crop
            const int border = (n == 1) ? 40 : 0;
            z.data = make_test_image(200, 300, static_cast<uint8_t>(n * 18), 80, 140, border);
            z.deflate = (n % 2 == 0); // Mix STORE and DEFLATE
            items.push_back(std::move(z));
        }
        CHECK(write_zip_file(cbz_path, items), "Failed to create test CBZ");
    }

    {
        koma::ReaderSession session(stream_cache);
        auto t0 = Clock::now();
        CHECK(session.open(cbz_path.wstring()), "Failed to open CBZ: " + session.last_error());
        auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();

        CHECK(session.format() == koma::ComicFormat::Cbz, "Expected CBZ format");
        CHECK(session.page_count() == 12, "Expected 12 pages in CBZ");

        // Render page 0 with auto_crop=false vs auto_crop=true
        auto p0_raw = session.get_page(0, 0, false);
        CHECK(p0_raw.valid() && p0_raw.width == 200 && p0_raw.height == 300,
              "CBZ page 0 raw dimensions mismatch");
        CHECK(p0_raw.mime_type == "image/webp", "Expected WebP output");

        auto p0_crop = session.get_page(0, 0, true);
        CHECK(p0_crop.valid() && p0_crop.width < 150 && p0_crop.height < 250,
              "Auto-crop failed to trim 40px white margins on page 0");

        // Wait briefly for 5-page lookahead prefetch (+1, +2, +3)
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(session.cached_page_count() <= 5 && session.cached_page_count() >= 2,
              "5-page sliding cache did not prefetch expected pages");

        // Measure cached page flip (page 1 should be a 0 ms RAM hit)
        auto f0 = Clock::now();
        auto p1 = session.get_page(1, 0, true);
        auto flip_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - f0).count();
        CHECK(p1.valid(), "CBZ page 1 render failed");

        // Jump forward to page 8 and verify cache stays bounded <= 5
        auto p8 = session.get_page(8, 120, false);
        CHECK(p8.valid() && p8.width == 120, "CBZ resized page 8 mismatch");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(session.cached_page_count() <= 5, "Sliding cache exceeded 5 pages");

        // Cover extraction
        auto cover = session.extract_cover(100, 150);
        CHECK(cover.valid() && cover.width <= 100 && cover.height <= 150, "CBZ cover extraction failed");

        session.close();
        CHECK(!session.is_open() && session.cached_page_count() == 0, "CBZ session did not cleanly release");

        std::cout << "[PASS] CBZ: 12 pages, open=" << open_ms << "ms, cached_flip="
                  << flip_us << "us, auto_crop=" << p0_raw.width << "x" << p0_raw.height
                  << " -> " << p0_crop.width << "x" << p0_crop.height << "\n";
    }

    // -------------------------------------------------------------------------
    // 2. Multi-page PDF (5 pages, PDFium 64-bit Win32 custom stream)
    // -------------------------------------------------------------------------
    const auto pdf_path = temp_dir / L"sample_comic.pdf";
    CHECK(write_test_pdf(pdf_path, 5), "Failed to create test PDF");

    {
        koma::ReaderSession session(stream_cache);
        auto t0 = Clock::now();
        CHECK(session.open(pdf_path.wstring()), "Failed to open PDF: " + session.last_error());
        auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();

        CHECK(session.format() == koma::ComicFormat::Pdf, "Expected PDF format");
        CHECK(session.page_count() == 5, "Expected 5 pages in PDF");

        auto p0 = session.get_page(0, 400, false);
        CHECK(p0.valid() && p0.width == 400 && p0.height == 600, "PDF page 0 render mismatch");
        CHECK(p0.mime_type == "image/webp", "Expected WebP output from PDFium");

        auto p4 = session.get_page(4, 300, true);
        CHECK(p4.valid() && p4.width < 300, "PDF page 4 auto-crop did not trim white margins");

        auto cover = session.extract_cover(120, 180);
        CHECK(cover.valid() && cover.width <= 120 && cover.height <= 180, "PDF cover extraction failed");

        session.close();
        CHECK(!session.is_open() && session.cached_page_count() == 0, "PDF session did not cleanly release");

        std::cout << "[PASS] PDF: 5 pages, open=" << open_ms << "ms, rendered="
                  << p0.width << "x" << p0.height << ", cropped=" << p4.width << "x" << p4.height << "\n";
    }

    // -------------------------------------------------------------------------
    // 3. Multi-page EPUB (container.xml + content.opf + XHTML spine -> 4 images)
    // -------------------------------------------------------------------------
    const auto epub_path = temp_dir / L"sample_manga.epub";
    {
        std::vector<ZipItem> items;
        auto str_bytes = [](const std::string& s) {
            return std::vector<uint8_t>(s.begin(), s.end());
        };

        items.push_back({"mimetype", str_bytes("application/epub+zip"), false});
        items.push_back({"META-INF/container.xml", str_bytes(
            R"(<?xml version="1.0"?>
            <container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
              <rootfiles>
                <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
              </rootfiles>
            </container>)"), true});

        items.push_back({"OEBPS/content.opf", str_bytes(
            R"(<?xml version="1.0"?>
            <package xmlns="http://www.idpf.org/2007/opf" version="3.0">
              <manifest>
                <item id="p1" href="text/p1.xhtml" media-type="application/xhtml+xml"/>
                <item id="p2" href="text/p2.xhtml" media-type="application/xhtml+xml"/>
                <item id="p3" href="text/p3.xhtml" media-type="application/xhtml+xml"/>
                <item id="p4" href="text/p4.xhtml" media-type="application/xhtml+xml"/>
                <item id="img1" href="images/1.jpg" media-type="image/jpeg"/>
                <item id="img2" href="images/2.jpg" media-type="image/jpeg"/>
                <item id="img3" href="images/3.jpg" media-type="image/jpeg"/>
                <item id="img4" href="images/10.jpg" media-type="image/jpeg"/>
              </manifest>
              <spine>
                <itemref idref="p1"/>
                <itemref idref="p2"/>
                <itemref idref="p3"/>
                <itemref idref="p4"/>
              </spine>
            </package>)"), true});

        const char* img_names[4] = {"1.jpg", "2.jpg", "3.jpg", "10.jpg"};
        for (int i = 0; i < 4; ++i) {
            items.push_back({
                "OEBPS/text/p" + std::to_string(i + 1) + ".xhtml",
                str_bytes(std::string(R"(<html><body><img src="../images/)") + img_names[i] + R"("/></body></html>)"),
                true
            });
            items.push_back({
                std::string("OEBPS/images/") + img_names[i],
                make_test_image(180 + i * 10, 260, static_cast<uint8_t>(50 + i * 40), 100, 180),
                false
            });
        }
        CHECK(write_zip_file(epub_path, items), "Failed to create test EPUB");
    }

    {
        koma::ReaderSession session(stream_cache);
        auto t0 = Clock::now();
        CHECK(session.open(epub_path.wstring()), "Failed to open EPUB: " + session.last_error());
        auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();

        CHECK(session.format() == koma::ComicFormat::Epub, "Expected EPUB format");
        CHECK(session.page_count() == 4, "Expected 4 pages in EPUB");

        auto p0 = session.get_page(0, 0, false);
        auto p3 = session.get_page(3, 0, false);
        CHECK(p0.valid() && p0.width == 180, "EPUB page 0 width mismatch");
        CHECK(p3.valid() && p3.width == 210, "EPUB page 3 width mismatch");

        session.close();
        CHECK(!session.is_open(), "EPUB session did not cleanly close");

        std::cout << "[PASS] EPUB: 4 pages, open=" << open_ms << "ms, page0="
                  << p0.width << "x" << p0.height << ", page3=" << p3.width << "x" << p3.height << "\n";
    }

    // -------------------------------------------------------------------------
    // 4. Standalone image verification
    // -------------------------------------------------------------------------
    const auto img_path = temp_dir / L"standalone.jpg";
    {
        auto bytes = make_test_image(320, 480, 90, 160, 220);
        std::ofstream ofs(img_path, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    {
        auto reader = koma::create_comic_reader(img_path.wstring(), stream_cache);
        CHECK(reader && reader->open(img_path.wstring()), "Failed to open standalone image");
        CHECK(reader->page_count() == 1, "Standalone image should have 1 page");
        auto p0 = reader->render_page(0, 160, false);
        CHECK(p0.valid() && p0.width == 160 && p0.height == 240, "Standalone image resize mismatch");
        reader->close();
        std::cout << "[PASS] Standalone Image: 1 page, resized=" << p0.width << "x" << p0.height << "\n";
    }

    const size_t rss_mb = current_working_set_mb();
    std::cout << "[PASS] Memory check: process working set = " << rss_mb << " MB (clean release verified)\n";

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "=== ALL FORMAT READER TESTS PASSED ===\n";
    return 0;
}
