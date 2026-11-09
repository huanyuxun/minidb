#include "minidb/database.hpp"
#include "minidb/lock.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace minidb;
using namespace std::chrono_literals;
namespace {
int checks = 0;
void check(bool condition, const std::string &what) {
    ++checks;
    if (!condition)
        throw std::runtime_error(what);
}
template <class F> std::string failure(F &&fn) {
    try {
        fn();
    } catch (const std::exception &e) {
        return e.what();
    }
    throw std::runtime_error("expected operation to fail");
}
std::int64_t scalar(Connection &c, const std::string &sql) {
    auto result = c.execute(sql);
    if (result.rows.size() != 1 || result.rows[0].size() != 1)
        throw std::runtime_error("invalid scalar shape");
    return std::get<std::int64_t>(result.rows[0][0]);
}
void remove_db(const std::filesystem::path &p) {
    std::filesystem::remove(p);
    std::filesystem::remove(p.string() + ".wal");
}
void locks() {
    LockManager locks;
    locks.shared(1, 100ms);
    locks.shared(2, 100ms);
    auto before = std::chrono::steady_clock::now();
    auto message = failure([&] { locks.exclusive(3, 40ms); });
    check(message.find("LOCK_TIMEOUT") != std::string::npos, "exclusive lock timeout error");
    check(std::chrono::steady_clock::now() - before >= 30ms, "lock timeout returned too early");
    locks.release(2);
    locks.exclusive(1, 100ms); // Sole reader can upgrade.
    message = failure([&] { locks.shared(2, 30ms); });
    check(message.find("LOCK_TIMEOUT") != std::string::npos, "shared lock timeout");
    locks.release(1);
    locks.shared(2, 100ms);
    locks.release(2);
    // Two readers attempting an upgrade create a real wait cycle. The short
    // waiter times out and releases all locks; the surviving waiter proceeds.
    locks.shared(10, 100ms);
    locks.shared(11, 100ms);
    std::promise<void> started;
    auto ready = started.get_future();
    auto survivor = std::async(std::launch::async, [&] {
        started.set_value();
        try {
            locks.exclusive(10, 2000ms);
            locks.release(10);
            return true;
        } catch (...) {
            locks.release(10);
            throw;
        }
    });
    ready.get();
    message = failure([&] { locks.exclusive(11, 40ms); });
    locks.release(11);
    check(message.find("LOCK_TIMEOUT") != std::string::npos && survivor.get(),
          "upgrade deadlock timeout victim releases locks");
}
void conflicts(const std::filesystem::path &path) {
    remove_db(path);
    {
        Database db(path, Options{2, 120ms});
        auto a = db.connect(), b = db.connect(), c = db.connect();
        a->execute("CREATE TABLE counters(id INTEGER PRIMARY KEY,value INTEGER NOT NULL)");
        a->execute("INSERT INTO counters VALUES(1,0)");
        check(!failure([&] { Database duplicate(path); }).empty(), "exclusive database file open");
        a->execute("BEGIN");
        b->execute("BEGIN");
        check(scalar(*a, "SELECT value FROM counters WHERE id=1") == 0 &&
                  scalar(*b, "SELECT value FROM counters WHERE id=1") == 0,
              "simultaneous shared transactions");
        auto message = failure([&] { c->execute("UPDATE counters SET value=10 WHERE id=1"); });
        check(message.find("LOCK_TIMEOUT") != std::string::npos && !c->in_transaction(),
              "readers block writer and abort timed-out auto transaction");
        a->execute("COMMIT");
        b->execute("COMMIT");
        c->execute("UPDATE counters SET value=1 WHERE id=1");
        a->execute("BEGIN");
        a->execute("UPDATE counters SET value=100 WHERE id=1");
        message = failure([&] { b->execute("SELECT value FROM counters WHERE id=1"); });
        check(message.find("LOCK_TIMEOUT") != std::string::npos,
              "uncommitted writer blocks reader");
        a->execute("ROLLBACK");
        check(scalar(*b, "SELECT value FROM counters WHERE id=1") == 1,
              "no dirty read after rollback");
        a->execute("BEGIN");
        a->execute("UPDATE counters SET value=7 WHERE id=1");
        failure([&] { a->execute("INSERT INTO counters VALUES(1,99)"); });
        check(!a->in_transaction() && scalar(*b, "SELECT value FROM counters WHERE id=1") == 1,
              "error abort restores data and releases exclusive lock");
        {
            auto abandoned = db.connect();
            abandoned->execute("BEGIN");
            abandoned->execute("UPDATE counters SET value=8 WHERE id=1");
        }
        check(scalar(*b, "SELECT value FROM counters WHERE id=1") == 1,
              "destructor releases lock and rolls back");
        a->execute("BEGIN");
        b->execute("BEGIN");
        scalar(*a, "SELECT value FROM counters WHERE id=1");
        scalar(*b, "SELECT value FROM counters WHERE id=1");
        std::barrier start(2);
        auto upgrade = [&](Connection &connection) {
            start.arrive_and_wait();
            try {
                connection.execute("UPDATE counters SET value=value+1 WHERE id=1");
                connection.execute("COMMIT");
                return true;
            } catch (const std::exception &e) {
                if (std::string(e.what()).find("LOCK_TIMEOUT") == std::string::npos)
                    throw;
                if (connection.in_transaction())
                    throw std::runtime_error("deadlock victim retained transaction");
                return false;
            }
        };
        auto fa = std::async(std::launch::async, [&] { return upgrade(*a); });
        auto fb = std::async(std::launch::async, [&] { return upgrade(*b); });
        bool ca = fa.get(), cb = fb.get();
        check(!ca || !cb, "upgrade deadlock must choose at least one victim");
        check(scalar(*c, "SELECT value FROM counters WHERE id=1") == 1 + int(ca) + int(cb),
              "upgrade survivor transaction atomicity");
    }
    remove_db(path);
}
void parallel(const std::filesystem::path &path) {
    remove_db(path);
    {
        Database db(path, Options{3, 10000ms});
        auto c = db.connect();
        c->execute("CREATE TABLE writes(id INTEGER PRIMARY KEY,worker INTEGER,value INTEGER)");
        c->execute("CREATE INDEX idx_writes_worker ON writes(worker)");
        c->execute("CREATE TABLE counter(id INTEGER PRIMARY KEY,value INTEGER)");
        c->execute("INSERT INTO counter VALUES(1,0)");
        constexpr int workers = 4, each = 18;
        std::barrier start(workers + 3);
        std::vector<std::future<void>> jobs;
        for (int worker = 0; worker < workers; ++worker)
            jobs.push_back(std::async(std::launch::async, [&, worker] {
                auto conn = db.connect();
                start.arrive_and_wait();
                for (int i = 0; i < each; ++i) {
                    conn->execute("BEGIN");
                    conn->execute("INSERT INTO writes VALUES(" + std::to_string(worker * 1000 + i) +
                                  "," + std::to_string(worker) + "," + std::to_string(i) + ")");
                    conn->execute("UPDATE counter SET value=value+1 WHERE id=1");
                    conn->execute("COMMIT");
                }
            }));
        for (int reader = 0; reader < 3; ++reader)
            jobs.push_back(std::async(std::launch::async, [&] {
                auto conn = db.connect();
                start.arrive_and_wait();
                for (int i = 0; i < each; ++i) {
                    conn->execute("BEGIN");
                    auto rows = scalar(*conn, "SELECT COUNT(*) FROM writes");
                    auto count = scalar(*conn, "SELECT value FROM counter WHERE id=1");
                    if (rows != count)
                        throw std::runtime_error("reader observed non-atomic table update");
                    auto point = conn->execute("SELECT id FROM writes WHERE worker=1 ORDER BY id");
                    if (point.rows.size() > static_cast<std::size_t>(each))
                        throw std::runtime_error("concurrent index corruption");
                    conn->execute("COMMIT");
                }
            }));
        for (auto &job : jobs)
            job.get();
        check(scalar(*c, "SELECT COUNT(*) FROM writes") == workers * each,
              "concurrent writers row count");
        check(scalar(*c, "SELECT value FROM counter WHERE id=1") == workers * each,
              "no lost updates");
        for (int worker = 0; worker < workers; ++worker)
            check(scalar(*c, "SELECT COUNT(*) FROM writes WHERE worker=" +
                                 std::to_string(worker)) == each,
                  "concurrent secondary index maintenance");
        // Readers now run simultaneously without a writer; this exercises the
        // shared buffer pool with only three frames and independent sessions.
        jobs.clear();
        for (int reader = 0; reader < 6; ++reader)
            jobs.push_back(std::async(std::launch::async, [&, reader] {
                auto conn = db.connect();
                for (int i = 0; i < 30; ++i) {
                    auto result = conn->execute("SELECT value FROM writes WHERE worker=" +
                                                std::to_string(reader % workers) + " ORDER BY id");
                    if (result.rows.size() != each)
                        throw std::runtime_error("parallel read row count");
                    for (int n = 0; n < each; ++n)
                        if (std::get<std::int64_t>(result.rows[n][0]) != n)
                            throw std::runtime_error("parallel read contents");
                }
            }));
        for (auto &job : jobs)
            job.get();
        check(true, "parallel readers and bounded buffer pool");
    }
    {
        Database db(path, Options{2});
        auto c = db.connect();
        check(scalar(*c, "SELECT COUNT(*) FROM writes") == 72 &&
                  scalar(*c, "SELECT value FROM counter WHERE id=1") == 72,
              "concurrent transactions survive restart");
    }
    remove_db(path);
}
} // namespace
int main() {
    try {
        auto base = std::filesystem::temp_directory_path() / "minidb_concurrency_test";
        locks();
        conflicts(base.string() + "_conflicts.db");
        parallel(base.string() + "_parallel.db");
        std::cout << "PASS concurrency " << checks
                  << " checks: strict 2PL, lock timeout, upgrade deadlock, concurrent "
                     "writers/readers/indexes, rollback/restart\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL concurrency after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
