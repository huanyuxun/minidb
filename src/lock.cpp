#include "minidb/lock.hpp"
namespace minidb {
void LockManager::shared(uint64_t tx, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (writer_ == tx || readers_.contains(tx))
        return;
    if (!changed_.wait_for(lock, timeout, [&] { return writer_ == 0 && waiting_writers_ == 0; }))
        throw std::runtime_error("LOCK_TIMEOUT: shared database lock");
    readers_.insert(tx);
}
void LockManager::exclusive(uint64_t tx, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (writer_ == tx)
        return;
    ++waiting_writers_;
    bool ready = changed_.wait_for(lock, timeout, [&] {
        return writer_ == 0 &&
               (readers_.empty() || (readers_.size() == 1 && readers_.contains(tx)));
    });
    --waiting_writers_;
    if (!ready) {
        changed_.notify_all();
        throw std::runtime_error("LOCK_TIMEOUT: exclusive lock / upgrade deadlock victim");
    }
    readers_.erase(tx);
    writer_ = tx;
}
void LockManager::release(uint64_t tx) noexcept {
    std::lock_guard lock(mutex_);
    readers_.erase(tx);
    if (writer_ == tx)
        writer_ = 0;
    changed_.notify_all();
}
} // namespace minidb
