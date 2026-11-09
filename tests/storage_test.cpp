#include "minidb/storage.hpp"
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace minidb;
namespace {
int checks = 0;
void check(bool b, const char *message) {
    ++checks;
    if (!b)
        throw std::runtime_error(message);
}
template <class F> void rejects(F fn, const char *message) {
    ++checks;
    try {
        fn();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error(message);
}
void clean(const std::filesystem::path &path) {
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".wal");
}
Page page(PageId id, unsigned value) {
    Page p{};
    set_page_type(p, PageType::Heap);
    p[100] = std::byte(value);
    seal_page(p, id, 1);
    return p;
}
void pages() {
    auto p = page(7, 42);
    validate_page(p, 7);
    check(page_type(p) == PageType::Heap && page_lsn(p) == 1, "header encode/decode");
    rejects([&] { validate_page(p, 8); }, "wrong id accepted");
    p[100] ^= std::byte{1};
    rejects([&] { validate_page(p, 7); }, "corrupted page accepted");
    const char *text = "123456789";
    check(checksum(std::span(reinterpret_cast<const std::byte *>(text), 9)) == 0xcbf43926,
          "CRC32 known vector");
}
void disk_buffer(const std::filesystem::path &path) {
    clean(path);
    {
        DiskManager disk(path);
        for (PageId id = 0; id < 12; ++id)
            disk.write(id, page(id, id));
        disk.sync();
        check(disk.page_count() == 12, "disk page count");
        check(disk.read(5)[100] == std::byte{5}, "disk read");
        rejects([&] { DiskManager other(path); }, "duplicate database open accepted");
        BufferPool pool(disk, 2);
        {
            auto first = pool.fetch(0);
            auto moved = std::move(first);
            check(moved.page()[100] == std::byte{0}, "guard move");
            auto second = pool.fetch(1);
            check(second.page()[100] == std::byte{1}, "two pinned frames");
        }
        {
            auto p = pool.fetch(2);
            check(p.page()[100] == std::byte{2}, "cache miss eviction");
        }
        auto replacement = page(1, 99);
        pool.put(1, replacement);
        pool.flush();
        disk.sync();
        check(disk.read(1)[100] == std::byte{99}, "dirty flush");
        std::vector<std::future<void>> readers;
        for (unsigned n = 0; n < 8; ++n)
            readers.push_back(std::async(std::launch::async, [&, n] {
                for (unsigned i = 0; i < 200; ++i) {
                    auto id = (i + n) % 12;
                    auto p = pool.fetch(id);
                    if (p.page()[100] != std::byte(id == 1 ? 99 : id))
                        throw std::runtime_error("concurrent buffer read");
                }
            }));
        for (auto &f : readers)
            f.get();
        check(pool.stats().evictions > 0 && pool.stats().hits > 0, "cache stats/concurrent read");
    }
    clean(path);
}
void transactions(const std::filesystem::path &path) {
    clean(path);
    PageId kept = 0;
    {
        Pager pager(path, 2);
        check(pager.page_count() == 1, "initial catalog page");
        {
            auto tx = pager.begin();
            check(page_type(tx->read(0)) == PageType::Catalog, "catalog initialized");
            auto id = tx->allocate(PageType::Heap);
            auto p = tx->read(id);
            p[100] = std::byte{17};
            tx->write(id, p);
            tx->rollback();
        }
        check(pager.page_count() == 1, "allocation rollback");
        {
            auto tx = pager.begin();
            kept = tx->allocate(PageType::Heap);
            auto p = tx->read(kept);
            p[100] = std::byte{88};
            tx->write(kept, p);
            tx->commit();
            rejects([&] { tx->read(kept); }, "closed tx accepted");
        }
        check(pager.page_count() == 2, "committed page count");
        {
            auto tx = pager.begin();
            auto p = tx->read(kept);
            p[100] = std::byte{33};
            tx->write(kept, p);
        }
        {
            auto tx = pager.begin();
            check(tx->read(kept)[100] == std::byte{88}, "destructor rollback");
        }
        auto old = pager.begin();
        {
            auto tx = pager.begin();
            auto p = tx->read(kept);
            p[101] = std::byte{9};
            tx->write(kept, p);
            tx->commit();
        }
        rejects([&] { old->read(kept); }, "stale read accepted");
        rejects([&] { old->commit(); }, "stale commit accepted");
        {
            auto tx = pager.begin();
            tx->free(kept);
            tx->commit();
        }
        {
            auto tx = pager.begin();
            auto reused = tx->allocate(PageType::BTreeLeaf);
            check(reused == kept, "freed page not reused");
            tx->commit();
        }
        pager.checkpoint();
        check(std::filesystem::file_size(path.string() + ".wal") == 0, "checkpoint WAL truncation");
    }
    {
        Pager pager(path, 1);
        auto tx = pager.begin();
        check(page_type(tx->read(kept)) == PageType::BTreeLeaf, "persistent allocation/page type");
        rejects([&] { tx->free(0); }, "freed catalog");
        rejects([&] { tx->read(9999); }, "out of range read");
    }
    // Valid WAL redo restores committed bytes even if a page is damaged after a
    // completed commit. Once checkpoint truncates WAL, corruption must be rejected.
    {
        Pager pager(path, 1);
        auto tx = pager.begin();
        auto p = tx->read(kept);
        p[100] = std::byte{66};
        tx->write(kept, p);
        tx->commit();
    }
    {
        platform::File f(path);
        std::byte bad{255};
        f.write(std::uint64_t(kept) * kPageSize + 100, std::span(&bad, 1));
        f.sync();
    }
    {
        Pager pager(path, 1);
        auto tx = pager.begin();
        check(tx->read(kept)[100] == std::byte{66}, "full-page redo repair");
        pager.checkpoint();
    }
    {
        platform::File f(path);
        std::byte bad{255};
        f.write(std::uint64_t(kept) * kPageSize + 100, std::span(&bad, 1));
        f.sync();
    }
    rejects([&] { Pager pager(path); }, "checkpointed corrupt page accepted");
    clean(path);
}
} // namespace
int main() {
    try {
        pages();
        auto dir = std::filesystem::temp_directory_path();
        disk_buffer(dir / "minidb_storage_buffer.db");
        transactions(dir / "minidb_storage_tx.db");
        std::cout << "PASS storage " << checks << " checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL storage after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
