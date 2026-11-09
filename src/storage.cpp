#include "minidb/storage.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace minidb {
namespace {
constexpr std::uint32_t page_magic = 0x5042444d, wal_magic = 0x4c41574d;
constexpr std::size_t log_header = 40;
constexpr std::uint16_t log_begin = 1, log_page = 2, log_commit = 3, log_checkpoint = 4;
template <class T> T get(std::span<const std::byte> p, std::size_t offset) {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= T(std::to_integer<unsigned char>(p[offset + i])) << (8 * i);
    return value;
}
template <class T> void put(std::span<std::byte> p, std::size_t offset, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i)
        p[offset + i] = std::byte(static_cast<unsigned char>(value >> (8 * i)));
}
[[noreturn]] void corrupt(const std::string &detail) {
    throw std::runtime_error("CORRUPTION: " + detail);
}
} // namespace
std::uint32_t checksum(std::span<const std::byte> data) {
    std::uint32_t crc = UINT32_MAX;
    for (auto byte : data) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
PageType page_type(const Page &page) {
    return static_cast<PageType>(get<std::uint16_t>(page, 6));
}
void set_page_type(Page &page, PageType type) {
    put<std::uint16_t>(page, 6, static_cast<std::uint16_t>(type));
}
std::uint64_t page_lsn(const Page &page) {
    return get<std::uint64_t>(page, 16);
}
void seal_page(Page &page, PageId id, std::uint64_t lsn) {
    put<std::uint32_t>(page, 0, page_magic);
    put<std::uint16_t>(page, 4, 1);
    put<std::uint32_t>(page, 8, id);
    put<std::uint32_t>(page, 12, 0);
    put<std::uint64_t>(page, 16, lsn);
    put<std::uint64_t>(page, 24, 0);
    put<std::uint32_t>(page, 12, checksum(page));
}
void validate_page(const Page &page, PageId expected_id) {
    if (get<std::uint32_t>(page, 0) != page_magic || get<std::uint16_t>(page, 4) != 1 ||
        get<std::uint32_t>(page, 8) != expected_id)
        corrupt("page header / id " + std::to_string(expected_id));
    auto type = static_cast<unsigned>(page_type(page));
    if (type < 1 || type > 5)
        corrupt("unknown page type");
    Page copy = page;
    auto expected = get<std::uint32_t>(copy, 12);
    put<std::uint32_t>(copy, 12, 0);
    if (checksum(copy) != expected)
        corrupt("page checksum " + std::to_string(expected_id));
}
DiskManager::DiskManager(const std::filesystem::path &path) : file_(path, true) {}
Page DiskManager::read(PageId id) {
    Page page{};
    if (file_.read(std::uint64_t(id) * kPageSize, page) != kPageSize)
        corrupt("short page read " + std::to_string(id));
    validate_page(page, id);
    return page;
}
void DiskManager::write(PageId id, const Page &page) {
    validate_page(page, id);
    if (platform::crash_selected("data_write")) {
        file_.write(std::uint64_t(id) * kPageSize, std::span(page).first(kPageSize / 2));
        file_.sync();
        platform::crash_if("data_write");
    }
    file_.write(std::uint64_t(id) * kPageSize, page);
}
std::uint32_t DiskManager::page_count() const {
    auto size = file_.size();
    if (size % kPageSize)
        corrupt("database length is not page aligned");
    if (size / kPageSize >= kInvalidPage)
        corrupt("database too large");
    return static_cast<std::uint32_t>(size / kPageSize);
}
void DiskManager::sync() {
    file_.sync();
}
std::uint64_t DiskManager::byte_size() const {
    return file_.size();
}

PageGuard::PageGuard(BufferPool *pool, std::shared_ptr<Frame> frame)
    : pool_(pool), frame_(std::move(frame)), latch_(frame_->latch) {}
PageGuard::~PageGuard() {
    reset();
}
PageGuard::PageGuard(PageGuard &&other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), frame_(std::move(other.frame_)),
      latch_(std::move(other.latch_)) {}
PageGuard &PageGuard::operator=(PageGuard &&other) noexcept {
    if (this != &other) {
        reset();
        pool_ = std::exchange(other.pool_, nullptr);
        frame_ = std::move(other.frame_);
        latch_ = std::move(other.latch_);
    }
    return *this;
}
void PageGuard::reset() noexcept {
    if (pool_) {
        if (latch_.owns_lock())
            latch_.unlock();
        pool_->unpin(frame_);
        pool_ = nullptr;
        frame_.reset();
    }
}
const Page &PageGuard::page() const {
    if (!frame_)
        throw std::runtime_error("empty page guard");
    return frame_->data;
}
BufferPool::BufferPool(DiskManager &disk, std::size_t capacity) : disk_(disk), capacity_(capacity) {
    if (!capacity)
        throw std::invalid_argument("buffer pool capacity must be positive");
}
bool BufferPool::make_room() {
    if (frames_.size() < capacity_)
        return true;
    auto victim = frames_.end();
    for (auto it = frames_.begin(); it != frames_.end(); ++it)
        if (it->second->pins == 0 &&
            (victim == frames_.end() || it->second->used < victim->second->used))
            victim = it;
    if (victim == frames_.end())
        return false;
    if (victim->second->dirty)
        disk_.write(victim->first, victim->second->data);
    frames_.erase(victim);
    ++stats_.evictions;
    return true;
}
PageGuard BufferPool::fetch(PageId id) {
    std::unique_lock lock(mutex_);
    auto it = frames_.find(id);
    if (it != frames_.end()) {
        ++stats_.hits;
        ++it->second->pins;
        it->second->used = ++clock_;
        auto f = it->second;
        lock.unlock();
        return PageGuard(this, std::move(f));
    }
    ++stats_.misses;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!make_room()) {
        if (available_.wait_until(lock, deadline) == std::cv_status::timeout)
            throw std::runtime_error("BUFFER_POOL_EXHAUSTED: all frames pinned");
        it = frames_.find(id);
        if (it != frames_.end()) {
            ++it->second->pins;
            it->second->used = ++clock_;
            auto f = it->second;
            lock.unlock();
            return PageGuard(this, std::move(f));
        }
    }
    auto frame = std::make_shared<PageGuard::Frame>();
    frame->id = id;
    frame->data = disk_.read(id);
    frame->pins = 1;
    frame->used = ++clock_;
    frames_[id] = frame;
    lock.unlock();
    return PageGuard(this, std::move(frame));
}
void BufferPool::put(PageId id, const Page &page) {
    validate_page(page, id);
    std::unique_lock lock(mutex_);
    auto it = frames_.find(id);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (it == frames_.end() && !make_room()) {
        if (available_.wait_until(lock, deadline) == std::cv_status::timeout)
            throw std::runtime_error("BUFFER_POOL_EXHAUSTED: all frames pinned");
        it = frames_.find(id);
    }
    std::shared_ptr<PageGuard::Frame> frame;
    if (it == frames_.end()) {
        frame = std::make_shared<PageGuard::Frame>();
        frame->id = id;
        frames_[id] = frame;
    } else
        frame = it->second;
    ++frame->pins;
    frame->used = ++clock_;
    lock.unlock();
    {
        std::lock_guard latch(frame->latch);
        frame->data = page;
        frame->dirty = true;
    }
    unpin(frame);
}
void BufferPool::unpin(const std::shared_ptr<PageGuard::Frame> &frame) noexcept {
    std::lock_guard lock(mutex_);
    --frame->pins;
    available_.notify_all();
}
void BufferPool::flush() {
    // Do not acquire frame latches while holding the map lock: guards unpin only
    // after releasing their latch. Pin the snapshot so eviction cannot race it.
    std::vector<std::shared_ptr<PageGuard::Frame>> snapshot;
    {
        std::lock_guard lock(mutex_);
        for (auto &[id, frame] : frames_) {
            (void)id;
            ++frame->pins;
            snapshot.push_back(frame);
        }
    }
    std::size_t index = 0;
    try {
        for (; index < snapshot.size(); ++index) {
            auto &frame = snapshot[index];
            {
                std::lock_guard latch(frame->latch);
                if (frame->dirty) {
                    disk_.write(frame->id, frame->data);
                    frame->dirty = false;
                }
            }
            unpin(frame);
        }
    } catch (...) {
        for (; index < snapshot.size(); ++index)
            unpin(snapshot[index]);
        throw;
    }
}
BufferStats BufferPool::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

Pager::Pager(const std::filesystem::path &path, std::size_t buffers)
    : disk_(path), wal_(std::filesystem::path(path).concat(".wal")), pool_(disk_, buffers) {
    recover();
    if (disk_.byte_size() == 0) {
        Page page{};
        set_page_type(page, PageType::Catalog);
        seal_page(page, 0, 0);
        disk_.write(0, page);
        disk_.sync();
    }
    count_ = disk_.page_count();
    for (PageId id = 0; id < count_; ++id) {
        auto page = disk_.read(id);
        next_lsn_ = std::max(next_lsn_, page_lsn(page) + 1);
        if (page_type(page) == PageType::Free)
            free_.push_back(id);
        if (id == 0 && page_type(page) != PageType::Catalog)
            corrupt("page zero is not a catalog");
    }
}
Pager::~Pager() = default; // Every successful commit is forced; destructor cannot commit.
std::unique_ptr<PageTransaction> Pager::begin() {
    return std::unique_ptr<PageTransaction>(new PageTransaction(*this));
}
std::uint32_t Pager::page_count() const {
    std::lock_guard lock(mutex_);
    return count_;
}
void Pager::append_log(std::uint16_t type, std::uint64_t tx, PageId id,
                       std::span<const std::byte> payload) {
    std::vector<std::byte> record(log_header + payload.size());
    put<std::uint32_t>(record, 0, wal_magic);
    put<std::uint16_t>(record, 4, 1);
    put<std::uint16_t>(record, 6, type);
    put<std::uint32_t>(record, 8, static_cast<std::uint32_t>(record.size()));
    put<std::uint32_t>(record, 12, id);
    put<std::uint64_t>(record, 16, next_lsn_++);
    put<std::uint64_t>(record, 24, tx);
    std::copy(payload.begin(), payload.end(), record.begin() + log_header);
    put<std::uint32_t>(record, 32, checksum(record));
    if (type == log_page && platform::crash_selected("wal_partial")) {
        wal_.write(wal_end_, std::span(record).first(record.size() / 2));
        wal_.sync();
        platform::crash_if("wal_partial");
    }
    wal_.write(wal_end_, record);
    wal_end_ += record.size();
}
void Pager::recover() {
    const auto length = wal_.size();
    std::uint64_t offset = 0, last_lsn = 0, active_tx = 0;
    std::vector<std::pair<PageId, Page>> pending;
    while (length - offset >= log_header) {
        std::array<std::byte, log_header> header{};
        if (wal_.read(offset, header) != header.size())
            corrupt("WAL header read");
        if (get<std::uint32_t>(header, 0) != wal_magic || get<std::uint16_t>(header, 4) != 1)
            corrupt("WAL header/version");
        auto type = get<std::uint16_t>(header, 6);
        auto size = get<std::uint32_t>(header, 8);
        if (type < log_begin || type > log_checkpoint ||
            size != (type == log_page ? log_header + kPageSize : log_header))
            corrupt("WAL record type/size");
        if (size > length - offset)
            break; // Incomplete final record: never committed.
        std::vector<std::byte> record(size);
        if (wal_.read(offset, record) != size)
            corrupt("short WAL record");
        const auto crc = get<std::uint32_t>(record, 32);
        put<std::uint32_t>(record, 32, 0);
        if (checksum(record) != crc)
            corrupt("WAL record checksum");
        const auto lsn = get<std::uint64_t>(record, 16), tx = get<std::uint64_t>(record, 24);
        auto id = get<std::uint32_t>(record, 12);
        if (lsn <= last_lsn)
            corrupt("WAL nonmonotonic LSN");
        last_lsn = lsn;
        next_lsn_ = std::max(next_lsn_, lsn + 1);
        next_tx_ = std::max(next_tx_, tx + 1);
        if (type == log_begin) {
            if (active_tx)
                corrupt("nested WAL transaction");
            active_tx = tx;
            pending.clear();
            if (!tx)
                corrupt("zero WAL transaction");
        } else if (type == log_page) {
            if (!active_tx || active_tx != tx || id == kInvalidPage)
                corrupt("orphan WAL page");
            Page page{};
            std::copy(record.begin() + log_header, record.end(), page.begin());
            validate_page(page, id);
            if (page_lsn(page) != lsn)
                corrupt("WAL/page LSN mismatch");
            pending.emplace_back(id, std::move(page));
        } else if (type == log_commit) {
            if (!active_tx || active_tx != tx)
                corrupt("orphan WAL commit");
            // Full-page overwrite before disk-page validation repairs torn pages and is
            // naturally idempotent. Uncommitted images never reach the data file.
            for (const auto &[page_id, page] : pending)
                disk_.write(page_id, page);
            pending.clear();
            active_tx = 0;
        } else if (type == log_checkpoint) {
            if (active_tx || tx)
                corrupt("invalid WAL checkpoint");
        }
        offset += size;
    }
    disk_.sync(); // Truncation is safe ONLY after all redone pages are durable.
    // Preserve valid records until a checkpoint, dropping any incomplete tail and
    // an incomplete transaction as a unit so later transactions never nest in it.
    // Recovery itself is a checkpoint: all committed data is now on disk.
    wal_.resize(0);
    wal_.sync();
    wal_end_ = 0;
}
void Pager::commit(PageTransaction &tx) {
    std::lock_guard lock(mutex_);
    if (poisoned_)
        throw std::runtime_error("IO_ERROR: pager poisoned; reopen database");
    if (tx.generation_ != generation_)
        throw std::runtime_error("WRITE_CONFLICT: stale page transaction");
    if (tx.changed_.empty()) {
        tx.active_ = false;
        return;
    }
    try {
        append_log(log_begin, tx.id_, kInvalidPage, {});
        std::vector<PageId> ids;
        ids.reserve(tx.changed_.size());
        for (const auto &[id, page] : tx.changed_) {
            (void)page;
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        for (auto id : ids) {
            auto &page = tx.changed_.at(id);
            seal_page(page, id, next_lsn_);
            append_log(log_page, tx.id_, id, page);
        }
        wal_.sync();
        platform::crash_if("wal_flushed");
        platform::crash_if("before_commit");
        append_log(log_commit, tx.id_, kInvalidPage, {});
        wal_.sync();
        platform::crash_if("commit_flushed");
        // Only durable committed images may enter the shared dirty-page cache.
        for (auto id : ids)
            pool_.put(id, tx.changed_.at(id));
        pool_.flush();
        disk_.sync();
        count_ = tx.count_;
        free_ = tx.free_;
        ++generation_;
        tx.active_ = false;
        tx.changed_.clear();
    } catch (...) {
        poisoned_ = true;
        tx.active_ = false;
        throw;
    }
}
void Pager::checkpoint() {
    std::lock_guard lock(mutex_);
    if (poisoned_)
        throw std::runtime_error("IO_ERROR: pager poisoned; reopen database");
    try {
        pool_.flush();
        disk_.sync();
        append_log(log_checkpoint, 0, kInvalidPage, {});
        wal_.sync();
        platform::crash_if("checkpoint");
        wal_.resize(0);
        wal_.sync();
        wal_end_ = 0;
    } catch (...) {
        poisoned_ = true;
        throw;
    }
}
PageTransaction::PageTransaction(Pager &pager) : pager_(pager) {
    std::lock_guard lock(pager_.mutex_);
    if (pager_.poisoned_)
        throw std::runtime_error("IO_ERROR: pager poisoned; reopen database");
    id_ = pager_.next_tx_++;
    generation_ = pager_.generation_;
    count_ = pager_.count_;
    free_ = pager_.free_;
}
PageTransaction::~PageTransaction() {
    rollback();
}
void PageTransaction::require_active() const {
    if (!active_)
        throw std::runtime_error("TRANSACTION_CLOSED");
}
Page PageTransaction::read(PageId id) {
    require_active();
    if (id >= count_)
        throw std::out_of_range("page id outside transaction");
    auto it = changed_.find(id);
    if (it != changed_.end())
        return it->second;
    std::lock_guard lock(pager_.mutex_);
    if (pager_.poisoned_)
        throw std::runtime_error("IO_ERROR: pager poisoned; reopen database");
    if (generation_ != pager_.generation_)
        throw std::runtime_error("WRITE_CONFLICT: stale page transaction");
    auto guard = pager_.pool_.fetch(id);
    return guard.page();
}
void PageTransaction::write(PageId id, const Page &page) {
    require_active();
    if (id >= count_)
        throw std::out_of_range("write page id outside transaction");
    auto type = static_cast<unsigned>(page_type(page));
    if (type < 1 || type > 4)
        throw std::runtime_error("invalid writable page type; use free() to release a page");
    if (id == 0 && page_type(page) != PageType::Catalog)
        throw std::runtime_error("page zero must be catalog");
    if (std::find(free_.begin(), free_.end(), id) != free_.end())
        throw std::runtime_error("write to freed page");
    changed_[id] = page;
}
PageId PageTransaction::allocate(PageType type) {
    require_active();
    if (static_cast<unsigned>(type) < 1 || static_cast<unsigned>(type) > 4)
        throw std::invalid_argument("invalid allocated page type");
    PageId id;
    if (!free_.empty()) {
        id = free_.back();
        free_.pop_back();
    } else {
        if (count_ == kInvalidPage)
            throw std::runtime_error("database page limit");
        id = count_++;
    }
    Page page{};
    set_page_type(page, type);
    changed_[id] = page;
    return id;
}
void PageTransaction::free(PageId id) {
    require_active();
    if (!id || id >= count_)
        throw std::out_of_range("cannot free page zero or invalid page");
    if (std::find(free_.begin(), free_.end(), id) != free_.end())
        throw std::runtime_error("double free page");
    Page page{};
    set_page_type(page, PageType::Free);
    changed_[id] = page;
    free_.push_back(id);
}
void PageTransaction::commit() {
    require_active();
    pager_.commit(*this);
}
void PageTransaction::rollback() noexcept {
    if (active_) {
        changed_.clear();
        active_ = false;
    }
}
} // namespace minidb
