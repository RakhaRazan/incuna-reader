#include "covers/cover_manager.hpp"
#include "db/database.hpp"
#include "formats/image_utils.hpp"
#include "portable_paths.hpp"
#include "watcher/library_scanner.hpp"

#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

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

bool create_sample_cbz(const std::filesystem::path& path, uint8_t r, uint8_t g, uint8_t b) {
    koma::DecodedImage img;
    img.width = 240;
    img.height = 340;
    img.rgba.resize(240ULL * 340ULL * 4ULL);
    for (size_t i = 0; i < 240ULL * 340ULL; ++i) {
        img.rgba[i * 4 + 0] = r;
        img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b;
        img.rgba[i * 4 + 3] = 255;
    }
    auto enc = koma::encode_rendered_page(img, 0, 240, 340, false, 85);
    if (!enc.valid()) return false;

    const std::string name = "001.jpg";
    const uint32_t crc = static_cast<uint32_t>(crc32(0L, enc.data.data(), static_cast<uInt>(enc.data.size())));
    const uint32_t sz = static_cast<uint32_t>(enc.data.size());

    std::vector<uint8_t> out;
    write_le32(out, 0x04034b50U);
    write_le16(out, 20);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le32(out, crc);
    write_le32(out, sz);
    write_le32(out, sz);
    write_le16(out, static_cast<uint16_t>(name.size()));
    write_le16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), enc.data.begin(), enc.data.end());

    const uint32_t cd_off = static_cast<uint32_t>(out.size());
    std::vector<uint8_t> cd;
    write_le32(cd, 0x02014b50U);
    write_le16(cd, 20);
    write_le16(cd, 20);
    write_le16(cd, 0);
    write_le16(cd, 0);
    write_le16(cd, 0);
    write_le16(cd, 0);
    write_le32(cd, crc);
    write_le32(cd, sz);
    write_le32(cd, sz);
    write_le16(cd, static_cast<uint16_t>(name.size()));
    write_le16(cd, 0);
    write_le16(cd, 0);
    write_le16(cd, 0);
    write_le16(cd, 0);
    write_le32(cd, 0);
    write_le32(cd, 0);
    cd.insert(cd.end(), name.begin(), name.end());

    const uint32_t cd_sz = static_cast<uint32_t>(cd.size());
    out.insert(out.end(), cd.begin(), cd.end());

    write_le32(out, 0x06054b50U);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, 1);
    write_le16(out, 1);
    write_le32(out, cd_sz);
    write_le32(out, cd_off);
    write_le16(out, 0);

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return ofs.good();
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
    std::cout << "=== Incuna Reader Library, Watcher & 2-Mode Cover Verification ===\n";

    const auto root = std::filesystem::temp_directory_path() / L"incunareader_library_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto comics_dir = root / L"comics";
    const auto sub_dir = comics_dir / L"OnePiece";
    const auto data_dir = root / L"data";
    const auto stream_cache = data_dir / L"stream_cache";
    const auto offline_cache = data_dir / L"offline_cache";
    const auto covers_pack = data_dir / L"covers.pack";

    std::filesystem::create_directories(sub_dir, ec);
    std::filesystem::create_directories(stream_cache, ec);
    std::filesystem::create_directories(offline_cache, ec);

    // Create 4 real CBZ comics across nested folders
    CHECK(create_sample_cbz(sub_dir / L"One Piece Vol 01.cbz", 220, 60, 60), "Create CBZ 1");
    CHECK(create_sample_cbz(sub_dir / L"One Piece Vol 02.cbz", 60, 180, 90), "Create CBZ 2");
    CHECK(create_sample_cbz(comics_dir / L"Berserk Deluxe 01.cbz", 40, 40, 45), "Create CBZ 3");
    CHECK(create_sample_cbz(comics_dir / L"Akira Complete.cbz", 200, 140, 30), "Create CBZ 4");

    koma::Database db;
    CHECK(db.open(data_dir / L"library.db"), "Open SQLite DB");
    CHECK(db.verify_fts5_available(), "FTS5 should be available");

    koma::SettingsManager settings(data_dir / L"settings.json");
    CHECK(settings.load_or_create(), "Create settings.json");
    CHECK(settings.get().value("cover_mode", "") == "ram_only", "Default cover_mode must be ram_only");

    // -------------------------------------------------------------------------
    // 1. Non-blocking recursive folder scan + 500-file batch upsert
    // -------------------------------------------------------------------------
    koma::LibraryScanner scanner(db, root);
    CHECK(scanner.start_folder_import(comics_dir.wstring()), "Start folder import");
    scanner.wait_for_scan();

    auto scan_st = scanner.get_scan_status();
    CHECK(!scan_st.scanning, "Scan should have finished");
    CHECK(scan_st.files_found == 4 && scan_st.files_indexed == 4, "Expected 4 comics indexed");
    CHECK(db.count_comics() == 4, "DB comic count should be 4");
    std::cout << "[PASS] Folder Scanner: indexed " << scan_st.files_indexed << " comics in "
              << scan_st.batches_committed << " batch(es)\n";

    // -------------------------------------------------------------------------
    // 2. FTS5 search, Shelves, Progress, Favorites, Smartviews, Bookmarks
    // -------------------------------------------------------------------------
    {
        koma::LibraryQuery q;
        q.search = "One Pie";
        auto res = db.query_library(q);
        CHECK(res.total_count == 2 && res.items.size() == 2, "FTS5 prefix search failed");
        std::cout << "[PASS] FTS5 Search: 'One Pie' matched " << res.total_count << " comics\n";

        const int64_t comic_id = res.items[0].id;

        // Shelves CRUD
        koma::ShelfRecord shelf;
        CHECK(db.create_shelf("Shonen Favorites", shelf), "Create shelf");
        CHECK(db.add_comic_to_shelf(shelf.id, comic_id), "Add comic to shelf");

        koma::LibraryQuery sq;
        sq.shelf_id = shelf.id;
        auto sres = db.query_library(sq);
        CHECK(sres.total_count == 1 && sres.items[0].id == comic_id, "Shelf filter failed");

        // Progress + Favorite + Smartview
        CHECK(db.update_comic_progress(comic_id, 15, 200, "reading", 1), "Update progress & favorite");
        koma::LibraryQuery fq;
        fq.smartview = "favorites";
        auto fres = db.query_library(fq);
        CHECK(fres.total_count == 1 && fres.items[0].is_favorite && fres.items[0].current_page == 15,
              "Favorites smartview or progress failed");

        // Named Bookmarks persistence
        koma::BookmarkRecord bm1, bm2;
        CHECK(db.add_bookmark(comic_id, 12, "Arlong Park", bm1), "Add bookmark 1");
        CHECK(db.add_bookmark(comic_id, 88, "Enies Lobby", bm2), "Add bookmark 2");
        auto bms = db.get_bookmarks(comic_id);
        CHECK(bms.size() == 2 && bms[0].name == "Arlong Park" && bms[1].page_number == 88,
              "Bookmark persistence check failed");
        CHECK(db.delete_bookmark(bm1.id), "Delete bookmark 1");
        CHECK(db.get_bookmarks(comic_id).size() == 1, "Bookmark delete failed");

        std::cout << "[PASS] Shelves, Smartviews, Progress & Bookmarks verified\n";
    }

    // -------------------------------------------------------------------------
    // 3. Win32 ReadDirectoryChangesW real-time Explorer auto-sync
    // -------------------------------------------------------------------------
    {
        const auto live_file = comics_dir / L"Trigun Vol 01.cbz";
        const auto renamed_file = comics_dir / L"Trigun Maximum Vol 01.cbz";
        CHECK(create_sample_cbz(live_file, 180, 50, 20), "Create live CBZ for watcher");

        // Wait up to 2s for ReadDirectoryChangesW to upsert
        bool synced_add = false;
        for (int i = 0; i < 40; ++i) {
            if (db.count_comics() == 5) { synced_add = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(synced_add, "ReadDirectoryChangesW did not auto-sync added comic");

        std::filesystem::rename(live_file, renamed_file, ec);
        bool synced_rename = false;
        for (int i = 0; i < 40; ++i) {
            koma::ComicRecord r;
            if (db.get_comic_by_path(koma::wide_to_utf8(renamed_file.wstring()), r) &&
                r.title == "Trigun Maximum Vol 01") {
                synced_rename = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(synced_rename, "ReadDirectoryChangesW did not auto-sync renamed comic");

        std::filesystem::remove(renamed_file, ec);
        bool synced_del = false;
        for (int i = 0; i < 40; ++i) {
            if (db.count_comics() == 4) { synced_del = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(synced_del, "ReadDirectoryChangesW did not auto-sync deleted comic");
        std::cout << "[PASS] Win32 ReadDirectoryChangesW: live add, rename & delete synced\n";
    }

    scanner.stop_all_watchers();

    // -------------------------------------------------------------------------
    // 4. 2-Mode Cover Manager: Mode 1 (ram_only) & Mode 2 (persistent_pack)
    // -------------------------------------------------------------------------
    {
        koma::CoverManager covers(db, settings, covers_pack, stream_cache);
        covers.start();

        auto all = db.query_library({});
        CHECK(all.items.size() == 4, "Expected 4 comics for cover test");
        const int64_t cid = all.items[0].id;

        // Mode 1: ram_only (0 MB disk footprint)
        auto c1 = covers.get_cover(cid);
        CHECK(c1.found && !c1.data.empty(), "Mode 1 cover extraction failed");
        CHECK(!std::filesystem::exists(covers_pack, ec), "Mode 1 must NOT create covers.pack (0 MB disk)");

        auto c2 = covers.get_cover(cid);
        CHECK(c2.found && c2.source == "ram_lru", "Mode 1 second fetch should hit RAM LRU");
        CHECK(covers.ram_cache_bytes() > 0 && covers.ram_cache_bytes() <= koma::CoverManager::kMaxRamCacheBytes,
              "RAM LRU byte accounting invalid");
        std::cout << "[PASS] Cover Mode 1 (ram_only): 0 MB disk, RAM LRU hit ("
                  << covers.ram_cache_bytes() << " bytes)\n";

        // Mode 2: persistent_pack
        settings.update({{"cover_mode", "persistent_pack"}});
        CHECK(covers.start_pack_build(), "Start covers.pack build");
        for (int i = 0; i < 100; ++i) {
            auto ps = covers.get_pack_status();
            if (!ps.building && ps.processed == 4) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }

        auto ps = covers.get_pack_status();
        CHECK(!ps.building && ps.succeeded == 4 && ps.pack_size_bytes > 0,
              "covers.pack build did not complete for all 4 comics");
        CHECK(std::filesystem::exists(covers_pack, ec), "covers.pack file should exist on disk");

        // Verify O(1) read from covers.pack (fresh CoverManager with empty RAM LRU)
        covers.stop();
        {
            koma::CoverManager fresh_covers(db, settings, covers_pack, stream_cache);
            fresh_covers.start();
            auto packed = fresh_covers.get_cover(cid);
            CHECK(packed.found && packed.source == "covers_pack",
                  "Expected O(1) read from covers.pack");
            std::cout << "[PASS] Cover Mode 2 (persistent_pack): built " << ps.succeeded
                      << " covers (" << ps.pack_size_bytes << " bytes), O(1) read verified\n";

            // Clean revert back to Mode 1 via purge_pack()
            CHECK(fresh_covers.purge_pack(), "purge_pack failed");
            CHECK(!std::filesystem::exists(covers_pack, ec), "covers.pack should be deleted after purge");
            koma::ComicRecord rec;
            CHECK(db.get_comic(cid, rec) && rec.cover_pack_offset == -1 && rec.cover_pack_size == 0,
                  "SQLite cover_pack offsets should be reset after purge");
            fresh_covers.stop();
            std::cout << "[PASS] Cover Pack Purge: covers.pack deleted & SQLite offsets reset\n";
        }
    }

    // -------------------------------------------------------------------------
    // 5. Unplugged Drive / Offline Network Guard & Offline Pin Protection
    // -------------------------------------------------------------------------
    {
        CHECK(koma::LibraryScanner::is_folder_online(comics_dir.wstring()), "Online folder should report online");
        const std::wstring offline_nas = L"\\\\NONEXISTENT_NAS_SERVER\\Comics";
        CHECK(!koma::LibraryScanner::is_folder_online(offline_nas), "Offline NAS should report offline");

        koma::WatchedFolderRecord nas_wf;
        CHECK(db.upsert_watched_folder(koma::wide_to_utf8(offline_nas), "Offline NAS", true, nas_wf),
              "Upsert offline NAS watched folder");
        CHECK(db.upsert_comics_batch({
            {"\\\\NONEXISTENT_NAS_SERVER\\Comics\\NAS_Vol_01.cbz", "NAS_Vol_01.cbz", "NAS Vol 01", "cbz", 1024},
            {"\\\\NONEXISTENT_NAS_SERVER\\Comics\\NAS_Vol_02.cbz", "NAS_Vol_02.cbz", "NAS Vol 02", "cbz", 2048}
        }), "Insert comics on offline NAS");

        const int64_t count_before = db.count_comics();
        CHECK(count_before == 6, "Expected 6 comics before prune");

        // Pin one online comic and delete its source file; also delete one unpinned online comic
        auto all_now = db.query_library({});
        int64_t pinned_id = 0;
        std::filesystem::path pinned_src;
        std::filesystem::path unpinned_src;
        for (const auto& c : all_now.items) {
            if (c.title == "Akira Complete") {
                pinned_id = c.id;
                pinned_src = koma::utf8_to_wide(c.path);
            } else if (c.title == "Berserk Deluxe 01") {
                unpinned_src = koma::utf8_to_wide(c.path);
            }
        }
        CHECK(pinned_id > 0 && !pinned_src.empty() && !unpinned_src.empty(), "Find test comics");

        const auto pinned_cache = offline_cache / L"akira_cached.cbz";
        std::filesystem::copy_file(pinned_src, pinned_cache, std::filesystem::copy_options::overwrite_existing, ec);
        CHECK(!ec, "Copy pinned comic to offline cache");
        CHECK(db.update_comic_offline_pin(pinned_id, true, koma::wide_to_utf8(pinned_cache.wstring())),
              "Mark comic offline pinned");

        std::filesystem::remove(pinned_src, ec);
        std::filesystem::remove(unpinned_src, ec);

        const int64_t pruned = scanner.prune_all_orphans();
        CHECK(pruned == 1, "Only the 1 unpinned missing comic on the online drive should be pruned");
        CHECK(db.count_comics() == 5, "Offline NAS comics and pinned comic must be preserved");

        koma::ComicRecord pinned_rec;
        CHECK(db.get_comic(pinned_id, pinned_rec) && pinned_rec.is_offline_pinned,
              "Pinned comic must remain in SQLite even when source is deleted");

        std::cout << "[PASS] Offline Drive Guard & Pin Protection: pruned " << pruned
                  << " unpinned orphan, preserved offline NAS & pinned comics\n";
    }

    db.close();
    std::filesystem::remove_all(root, ec);
    std::cout << "=== ALL LIBRARY, WATCHER & 2-MODE COVER TESTS PASSED ===\n";
    return 0;
}
