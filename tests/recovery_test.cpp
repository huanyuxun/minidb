#include "minidb/database.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace minidb;
namespace test_platform {
void fault(const std::string &point) {
#ifdef _WIN32
    if (_putenv_s("MINIDB_CRASH_POINT", point.c_str()) != 0)
        throw std::runtime_error("set fault environment failed");
#else
    if (setenv("MINIDB_CRASH_POINT", point.c_str(), 1) != 0)
        throw std::runtime_error("set fault environment failed");
#endif
}
int child(const std::filesystem::path &executable, const std::filesystem::path &database,
          const std::string &point) {
#ifdef _WIN32
    // Windows paths cannot contain quotes. Escape a trailing slash before the
    // closing quote as required by CommandLineToArgvW/CRT command-line rules.
    auto quote = [](const std::wstring &arg) {
        std::wstring out = L"\"";
        std::size_t slashes = 0;
        for (wchar_t c : arg) {
            if (c == L'\\') {
                ++slashes;
                continue;
            }
            out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
            slashes = 0;
            if (c == L'"')
                out += L'\\';
            out += c;
        }
        out.append(slashes * 2, L'\\');
        return out + L'"';
    };
    std::wstring cmd = quote(executable.wstring()) + L" --child " + quote(database.wstring()) +
                       L" " + quote(std::wstring(point.begin(), point.end()));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        throw std::runtime_error("CreateProcess failed " + std::to_string(GetLastError()));
    CloseHandle(process.hThread);
    DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 99);
        CloseHandle(process.hProcess);
        throw std::runtime_error("recovery child timed out");
    }
    DWORD status = 0;
    if (!GetExitCodeProcess(process.hProcess, &status)) {
        CloseHandle(process.hProcess);
        throw std::runtime_error("GetExitCodeProcess failed");
    }
    CloseHandle(process.hProcess);
    return static_cast<int>(status);
#else
    pid_t pid = fork();
    if (pid < 0)
        throw std::runtime_error("fork failed");
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), "--child", database.c_str(), point.c_str(),
              static_cast<char *>(nullptr));
        std::_Exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        throw std::runtime_error("waitpid failed");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}
} // namespace test_platform
namespace {
int checks = 0;
void check(bool condition, const std::string &message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
std::int64_t scalar(Connection &connection, const std::string &sql) {
    auto result = connection.execute(sql);
    if (result.rows.size() != 1 || result.rows[0].size() != 1)
        throw std::runtime_error("bad scalar result");
    return std::get<std::int64_t>(result.rows[0][0]);
}
void baseline(const std::filesystem::path &path) {
    Database db(path, Options{2});
    auto c = db.connect();
    c->execute("CREATE TABLE recovery(id INTEGER PRIMARY KEY,v INTEGER,text VARCHAR(500))");
    c->execute("CREATE INDEX idx_v ON recovery(v)");
    c->execute("INSERT INTO recovery VALUES(1,10,'before')");
    c->execute("CHECKPOINT");
}
void verify(const std::filesystem::path &path, bool committed) {
    // Open twice: recovery must be idempotent, including index maintenance.
    for (int pass = 0; pass < 2; ++pass) {
        Database db(path, Options{2});
        auto c = db.connect();
        check(scalar(*c, "SELECT COUNT(*) FROM recovery") == (committed ? 41 : 1),
              "row atomicity after crash");
        check(scalar(*c, "SELECT v FROM recovery WHERE id=1") == (committed ? 20 : 10),
              "updated value after crash");
        check(scalar(*c, "SELECT COUNT(*) FROM recovery WHERE v=20") == (committed ? 1 : 0),
              "updated secondary index after crash");
        check(scalar(*c, "SELECT COUNT(*) FROM recovery WHERE v>=100") == (committed ? 40 : 0),
              "multi-page secondary range after crash");
        auto result = c->execute("SELECT text FROM recovery WHERE id=1");
        check(std::get<std::string>(result.rows[0][0]) == (committed ? "after" : "before"),
              "record image after crash");
    }
}
void corrupt_byte(const std::filesystem::path &path, std::streamoff offset) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot open corruption target");
    file.seekg(offset);
    char b = 0;
    file.read(&b, 1);
    if (!file)
        throw std::runtime_error("corruption offset beyond file");
    b = static_cast<char>(static_cast<unsigned char>(b) ^ 0x5a);
    file.seekp(offset);
    file.write(&b, 1);
    file.flush();
    if (!file)
        throw std::runtime_error("corruption write failed");
}
void rejects_open(const std::filesystem::path &path, const std::string &reason) {
    bool rejected = false;
    try {
        Database db(path);
        auto c = db.connect();
        c->execute("SELECT * FROM recovery");
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, reason);
}
int child_main(const std::filesystem::path &path, const std::string &point) {
    if (point == "probe_open") {
        try {
            Database db(path);
        } catch (const std::exception &) {
            return 87;
        }
        return 1;
    }
    Database db(path, Options{2});
    auto c = db.connect();
    c->execute("BEGIN");
    c->execute("UPDATE recovery SET v=20,text='after' WHERE id=1");
    for (int id = 2; id <= 41; ++id)
        c->execute("INSERT INTO recovery VALUES(" + std::to_string(id) + "," +
                   std::to_string(100 + id) + ",'" + std::string(400, 'x') + "')");
    if (point == "uncommitted")
        std::_Exit(86);
    if (point == "checkpoint") {
        c->execute("COMMIT");
        test_platform::fault(point);
        c->execute("CHECKPOINT");
    } else {
        test_platform::fault(point);
        c->execute("COMMIT");
    }
    throw std::runtime_error("requested fault did not terminate child: " + point);
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--child")
            return child_main(argv[2], argv[3]);
        auto executable = std::filesystem::absolute(argv[0]);
        auto base =
            std::filesystem::temp_directory_path() /
            ("minidb_recovery_" +
             std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(base))
            throw std::runtime_error("cannot create isolated test directory");
        const std::vector<std::pair<std::string, bool>> cases{
            {"uncommitted", false},   {"wal_partial", false},   {"wal_flushed", false},
            {"before_commit", false}, {"commit_flushed", true}, {"data_write", true},
            {"checkpoint", true}};
        for (const auto &[point, committed] : cases) {
            auto path = base / (point + ".db");
            baseline(path);
            check(test_platform::child(executable, path, point) == 86,
                  "fault process exit: " + point);
            verify(path, committed);
            std::cout << "PASS crash " << point << '\n';
        }
        {
            auto path = base / "process_lock.db";
            baseline(path);
            Database held(path);
            check(test_platform::child(executable, path, "probe_open") == 87,
                  "cross-process exclusive database file lock");
        }
        {
            auto path = base / "repair_corrupt_page.db";
            baseline(path);
            check(test_platform::child(executable, path, "commit_flushed") == 86,
                  "prepare WAL page repair");
            // Ensure the existing heap page really is checksum-invalid. A
            // half-write of a mostly-zero catalog page can coincidentally leave
            // a valid page, so this is a separate explicit redo repair check.
            corrupt_byte(path, 4096 + 2048 + 20);
            verify(path, true);
        }
        {
            auto path = base / "corrupt_page.db";
            baseline(path);
            corrupt_byte(path, 4096 + 100);
            rejects_open(path, "checkpointed page corruption must fail closed");
        }
        {
            auto path = base / "corrupt_wal.db";
            baseline(path);
            check(test_platform::child(executable, path, "commit_flushed") == 86,
                  "prepare committed WAL corruption");
            corrupt_byte(path.string() + ".wal", 100);
            rejects_open(path, "complete WAL record checksum mismatch must fail closed");
        }
        {
            auto path = base / "torn_tail.db";
            baseline(path);
            check(test_platform::child(executable, path, "commit_flushed") == 86,
                  "prepare torn WAL tail");
            {
                std::ofstream wal(path.string() + ".wal", std::ios::binary | std::ios::app);
                wal.write("partial", 7);
            }
            verify(path, true);
        }
        // All paths were generated beneath our isolated directory.
        std::filesystem::remove_all(base);
        std::cout
            << "PASS recovery " << checks
            << " checks, 7 real process crash boundaries, corruption and exclusive file locking\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL recovery after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
