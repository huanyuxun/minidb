#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace minidb::platform {
// All OS handles, positional I/O, durable flushes and process locks live here.
class File {
  public:
    explicit File(const std::filesystem::path &path, bool exclusive_lock = false);
    ~File();
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    std::size_t read(std::uint64_t offset, std::span<std::byte> bytes);
    void write(std::uint64_t offset, std::span<const std::byte> bytes);
    std::uint64_t size() const;
    void resize(std::uint64_t bytes);
    void sync();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void crash_if(const char *point);
bool crash_selected(const char *point);
} // namespace minidb::platform
