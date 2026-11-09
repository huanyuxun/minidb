#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <stdexcept>
namespace minidb {
class LockManager {
  public:
    void shared(uint64_t tx, std::chrono::milliseconds timeout);
    void exclusive(uint64_t tx, std::chrono::milliseconds timeout);
    void release(uint64_t tx) noexcept;

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::set<uint64_t> readers_;
    uint64_t writer_ = 0;
    size_t waiting_writers_ = 0;
};
} // namespace minidb
