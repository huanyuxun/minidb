#include "minidb/platform.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace minidb::platform {
namespace {
[[noreturn]] void fail(const char *operation) {
#ifdef _WIN32
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
#else
    throw std::system_error(errno, std::generic_category(), operation);
#endif
}
} // namespace
struct File::Impl {
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int handle{-1};
#endif
    mutable std::mutex mutex;
};
File::File(const std::filesystem::path &path, bool exclusive_lock)
    : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    impl_->handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE)
        fail("open database file");
    if (exclusive_lock) {
        OVERLAPPED ov{};
        if (!LockFileEx(impl_->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                        MAXDWORD, MAXDWORD, &ov)) {
            const auto error = GetLastError();
            CloseHandle(impl_->handle);
            SetLastError(error);
            fail("database already locked");
        }
    }
#else
    const bool existed = std::filesystem::exists(path);
    impl_->handle = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (impl_->handle < 0)
        fail("open database file");
    if (exclusive_lock && ::flock(impl_->handle, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        ::close(impl_->handle);
        errno = error;
        fail("database already locked");
    }
    if (!existed) {
        auto parent = path.parent_path();
        if (parent.empty())
            parent = ".";
        const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            ::fsync(directory);
            ::close(directory);
        }
    }
#endif
}
File::~File() {
#ifdef _WIN32
    if (impl_->handle != INVALID_HANDLE_VALUE)
        CloseHandle(impl_->handle);
#else
    if (impl_->handle >= 0)
        ::close(impl_->handle);
#endif
}
std::size_t File::read(std::uint64_t offset, std::span<std::byte> bytes) {
    std::lock_guard guard(impl_->mutex);
    std::size_t done = 0;
#ifdef _WIN32
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN))
        fail("seek read");
#endif
    while (done < bytes.size()) {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(impl_->handle, bytes.data() + done,
                      static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, MAXDWORD)),
                      &count, nullptr))
            fail("read file");
#else
        const auto count = ::pread(impl_->handle, bytes.data() + done, bytes.size() - done,
                                   static_cast<off_t>(offset + done));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            fail("read file");
        }
#endif
        if (count == 0)
            break;
        done += static_cast<std::size_t>(count);
    }
    return done;
}
void File::write(std::uint64_t offset, std::span<const std::byte> bytes) {
    std::lock_guard guard(impl_->mutex);
    std::size_t done = 0;
#ifdef _WIN32
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN))
        fail("seek write");
#endif
    while (done < bytes.size()) {
#ifdef _WIN32
        DWORD count = 0;
        if (!WriteFile(impl_->handle, bytes.data() + done,
                       static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, MAXDWORD)),
                       &count, nullptr))
            fail("write file");
#else
        const auto count = ::pwrite(impl_->handle, bytes.data() + done, bytes.size() - done,
                                    static_cast<off_t>(offset + done));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            fail("write file");
        }
#endif
        if (count == 0)
            throw std::runtime_error("zero-length file write");
        done += static_cast<std::size_t>(count);
    }
}
std::uint64_t File::size() const {
    std::lock_guard guard(impl_->mutex);
#ifdef _WIN32
    LARGE_INTEGER bytes{};
    if (!GetFileSizeEx(impl_->handle, &bytes))
        fail("file size");
    return static_cast<std::uint64_t>(bytes.QuadPart);
#else
    struct stat st{};
    if (::fstat(impl_->handle, &st) != 0)
        fail("file size");
    return static_cast<std::uint64_t>(st.st_size);
#endif
}
void File::resize(std::uint64_t bytes) {
    std::lock_guard guard(impl_->mutex);
#ifdef _WIN32
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(bytes);
    if (!SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(impl_->handle))
        fail("truncate file");
#else
    if (::ftruncate(impl_->handle, static_cast<off_t>(bytes)) != 0)
        fail("truncate file");
#endif
}
void File::sync() {
    std::lock_guard guard(impl_->mutex);
#ifdef _WIN32
    if (!FlushFileBuffers(impl_->handle))
        fail("flush file");
#else
    while (::fsync(impl_->handle) != 0) {
        if (errno != EINTR)
            fail("flush file");
    }
#endif
}
bool crash_selected(const char *point) {
#ifdef _WIN32
    char configured[128]{};
    const auto count =
        GetEnvironmentVariableA("MINIDB_CRASH_POINT", configured, sizeof(configured));
    return count > 0 && count < sizeof(configured) && std::strcmp(configured, point) == 0;
#else
    const char *configured = std::getenv("MINIDB_CRASH_POINT");
    return configured && std::strcmp(configured, point) == 0;
#endif
}
void crash_if(const char *point) {
    if (crash_selected(point))
        std::_Exit(86);
}
} // namespace minidb::platform
