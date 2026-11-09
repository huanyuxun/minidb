#pragma once
#include "minidb/platform.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace minidb {
using PageId = std::uint32_t;
inline constexpr PageId kInvalidPage = UINT32_MAX;
inline constexpr std::size_t kPageSize = 4096;
inline constexpr std::size_t kPageHeaderSize = 32;
using Page = std::array<std::byte, kPageSize>;
enum class PageType : std::uint16_t {
    Catalog = 1,
    Heap = 2,
    BTreeLeaf = 3,
    BTreeInternal = 4,
    Free = 5
};
PageType page_type(const Page &page);
void set_page_type(Page &page, PageType type);
std::uint64_t page_lsn(const Page &page);
std::uint32_t checksum(std::span<const std::byte> data);
void seal_page(Page &page, PageId id, std::uint64_t lsn);
void validate_page(const Page &page, PageId expected_id);

class DiskManager {
  public:
    explicit DiskManager(const std::filesystem::path &path);
    Page read(PageId id);
    void write(PageId id, const Page &page);
    std::uint32_t page_count() const;
    void sync();
    // Recovery may repair a final torn page before normal reads begin.
    std::uint64_t byte_size() const;

  private:
    platform::File file_;
};

class BufferPool;
class PageGuard {
  public:
    PageGuard() = default;
    ~PageGuard();
    PageGuard(PageGuard &&) noexcept;
    PageGuard &operator=(PageGuard &&) noexcept;
    PageGuard(const PageGuard &) = delete;
    PageGuard &operator=(const PageGuard &) = delete;
    const Page &page() const;
    const Page &operator*() const {
        return page();
    }

  private:
    friend class BufferPool;
    struct Frame {
        Page data{};
        PageId id{};
        std::size_t pins{};
        bool dirty{};
        std::uint64_t used{};
        std::mutex latch;
    };
    PageGuard(BufferPool *pool, std::shared_ptr<Frame> frame);
    void reset() noexcept;
    BufferPool *pool_{};
    std::shared_ptr<Frame> frame_;
    std::unique_lock<std::mutex> latch_;
};

struct BufferStats {
    std::uint64_t hits{}, misses{}, evictions{};
};
class BufferPool {
  public:
    explicit BufferPool(DiskManager &disk, std::size_t capacity = 64);
    PageGuard fetch(PageId id);
    // The caller MUST have made the corresponding WAL durable before put().
    void put(PageId id, const Page &page);
    void flush();
    BufferStats stats() const;
    std::size_t capacity() const {
        return capacity_;
    }

  private:
    friend class PageGuard;
    void unpin(const std::shared_ptr<PageGuard::Frame> &frame) noexcept;
    bool make_room();
    DiskManager &disk_;
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::unordered_map<PageId, std::shared_ptr<PageGuard::Frame>> frames_;
    std::uint64_t clock_{};
    BufferStats stats_;
};

class Pager;
class PageTransaction {
  public:
    ~PageTransaction();
    PageTransaction(const PageTransaction &) = delete;
    PageTransaction &operator=(const PageTransaction &) = delete;
    Page read(PageId id);
    void write(PageId id, const Page &page);
    PageId allocate(PageType type);
    void free(PageId id);
    void commit();
    void rollback() noexcept;
    std::uint64_t id() const {
        return id_;
    }
    std::size_t dirty_pages() const {
        return changed_.size();
    }

  private:
    friend class Pager;
    explicit PageTransaction(Pager &pager);
    void require_active() const;
    Pager &pager_;
    std::uint64_t id_{}, generation_{};
    PageId count_{};
    std::vector<PageId> free_;
    std::unordered_map<PageId, Page> changed_;
    bool active_{true};
};

class Pager {
  public:
    explicit Pager(const std::filesystem::path &path, std::size_t buffers = 64);
    ~Pager();
    std::unique_ptr<PageTransaction> begin();
    void checkpoint();
    std::uint32_t page_count() const;
    BufferStats stats() const {
        return pool_.stats();
    }

  private:
    friend class PageTransaction;
    void recover();
    void commit(PageTransaction &tx);
    void append_log(std::uint16_t type, std::uint64_t tx, PageId id,
                    std::span<const std::byte> payload);
    DiskManager disk_;
    platform::File wal_;
    BufferPool pool_;
    mutable std::mutex mutex_;
    std::uint64_t next_tx_{1}, next_lsn_{1}, generation_{}, wal_end_{};
    PageId count_{};
    std::vector<PageId> free_;
    bool poisoned_{};
};
} // namespace minidb
