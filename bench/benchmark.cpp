#include "minidb/database.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#endif

namespace benchmark_platform {
// The engine's platform abstraction is independent of this benchmark-only resource probe.
std::uint64_t peak_rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        return 0;
    return static_cast<std::uint64_t>(memory.PeakWorkingSetSize);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}
} // namespace benchmark_platform
namespace {
using namespace minidb;
using Clock = std::chrono::steady_clock;
double micros(Clock::duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count();
}
struct Measurement {
    std::string name, latency_unit;
    std::size_t operations = 0;
    std::vector<double> latency;
    double total_us = 0;
    std::uint64_t checksum = 0;
};
std::size_t positive(const std::string &text) {
    std::size_t n = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || n == 0)
        throw std::runtime_error("expected positive integer: " + text);
    return n;
}
std::vector<std::size_t> list(const std::string &text) {
    std::vector<std::size_t> out;
    std::istringstream in(text);
    std::string item;
    while (std::getline(in, item, ','))
        out.push_back(positive(item));
    if (out.empty())
        throw std::runtime_error("empty numeric list");
    return out;
}
double percentile(std::vector<double> values, double p) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    return values[index];
}
void report(std::ostream &out, std::size_t rows, std::size_t buffers, const Measurement &m) {
    out << m.name << ',' << rows << ',' << buffers << ',' << m.operations << ',' << m.latency_unit
        << ',' << std::fixed << std::setprecision(3) << m.total_us / 1000.0 << ','
        << (m.total_us > 0 ? static_cast<double>(m.operations) * 1000000.0 / m.total_us : 0) << ','
        << percentile(m.latency, 0.50) << ',' << percentile(m.latency, 0.95) << ','
        << benchmark_platform::peak_rss_bytes() << ',' << m.checksum << '\n';
    out.flush();
}
Measurement insert(Connection &connection, const std::string &table,
                   const std::vector<std::size_t> &ids, std::size_t batch_size) {
    Measurement m{table == "sequential" ? "sequential_insert" : "random_insert", "committed_batch"};
    m.operations = ids.size();
    const auto all = Clock::now();
    for (std::size_t start = 0; start < ids.size(); start += batch_size) {
        const auto before = Clock::now();
        connection.execute("BEGIN");
        const auto end = std::min(ids.size(), start + batch_size);
        try {
            for (std::size_t i = start; i < end; ++i) {
                auto id = ids[i];
                auto sql = "INSERT INTO " + table + " VALUES (" + std::to_string(id) + "," +
                           std::to_string(id % 100) + "," + std::to_string(id % 1000) + ",'row_" +
                           std::to_string(id) + "')";
                connection.execute(sql);
            }
            connection.execute("COMMIT");
        } catch (...) {
            if (connection.in_transaction())
                connection.execute("ROLLBACK");
            throw;
        }
        m.latency.push_back(micros(Clock::now() - before));
    }
    m.total_us = micros(Clock::now() - all);
    m.checksum = ids.size();
    return m;
}
template <class Factory>
Measurement query(Connection &connection, const std::string &name, std::size_t count,
                  Factory make_sql) {
    Measurement m{name, "statement"};
    m.operations = count;
    const auto all = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        auto sql = make_sql(i);
        const auto before = Clock::now();
        auto result = connection.execute(sql);
        m.latency.push_back(micros(Clock::now() - before));
        m.checksum += result.rows.size();
    }
    m.total_us = micros(Clock::now() - all);
    return m;
}
void run(std::ostream &out, const std::filesystem::path &directory, std::size_t rows,
         std::size_t buffers, std::size_t queries, std::size_t batch_size) {
    const auto path =
        directory / ("n" + std::to_string(rows) + "_b" + std::to_string(buffers) + ".mdb");
    // Never overwrite existing user files. A unique run directory is created by main.
    Database database(path, Options{buffers, std::chrono::milliseconds{5000}});
    auto connection = database.connect();
    connection->execute(
        "CREATE TABLE sequential(id INTEGER PRIMARY KEY,g INTEGER,v INTEGER,name VARCHAR(64))");
    connection->execute(
        "CREATE TABLE randomized(id INTEGER PRIMARY KEY,g INTEGER,v INTEGER,name VARCHAR(64))");
    std::vector<std::size_t> ids(rows);
    std::iota(ids.begin(), ids.end(), std::size_t{1});
    report(out, rows, buffers, insert(*connection, "sequential", ids, batch_size));
    std::mt19937_64 rng(20261109);
    std::shuffle(ids.begin(), ids.end(), rng);
    report(out, rows, buffers, insert(*connection, "randomized", ids, batch_size));
    const auto index_start = Clock::now();
    connection->execute("CREATE INDEX idx_group ON sequential(g)");
    Measurement index{
        "create_secondary_index",           "statement", 1, {micros(Clock::now() - index_start)},
        micros(Clock::now() - index_start), rows};
    report(out, rows, buffers, index);
    auto count = connection->execute("SELECT COUNT(*) FROM sequential");
    if (count.rows.size() != 1 ||
        std::get<std::int64_t>(count.rows[0][0]) != static_cast<std::int64_t>(rows))
        throw std::runtime_error("benchmark row-count validation failed");
    report(out, rows, buffers, query(*connection, "primary_key_point", queries, [&](std::size_t i) {
               return "SELECT name FROM sequential WHERE id=" + std::to_string(ids[i % ids.size()]);
           }));
    report(out, rows, buffers, query(*connection, "primary_key_range", queries, [&](std::size_t i) {
               auto low = 1 + (i * 71) % rows;
               return "SELECT id FROM sequential WHERE id>=" + std::to_string(low) + " AND id<" +
                      std::to_string(low + 20);
           }));
    report(out, rows, buffers,
           query(*connection, "secondary_index_range", queries, [&](std::size_t i) {
               auto low = i % 90;
               return "SELECT id FROM sequential WHERE g>=" + std::to_string(low) + " AND g<" +
                      std::to_string(low + 5);
           }));
    report(out, rows, buffers,
           query(*connection, "full_table_scan", std::max(std::size_t{3}, queries / 10),
                 [](std::size_t) { return "SELECT * FROM sequential"; }));
    report(out, rows, buffers,
           query(*connection, "multiple_predicates", queries, [&](std::size_t i) {
               return "SELECT id,name FROM sequential WHERE g=" + std::to_string(i % 100) +
                      " AND v>500 ORDER BY id LIMIT 20";
           }));
    connection->execute("CHECKPOINT");
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::vector<std::size_t> sizes{1000, 10000}, buffers{8, 64};
        std::size_t queries = 100, batch = 200;
        std::filesystem::path root = "benchmark-data";
        std::string csv;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&] {
                if (i + 1 >= argc)
                    throw std::runtime_error("missing value for " + arg);
                return std::string(argv[++i]);
            };
            if (arg == "--help") {
                std::cout << "minidb_bench [--sizes 1000,10000] [--buffers 8,64] [--queries 100] "
                             "[--batch-size 200] [--directory PATH] [--csv FILE]\n";
                return 0;
            }
            if (arg == "--sizes")
                sizes = list(value());
            else if (arg == "--buffers")
                buffers = list(value());
            else if (arg == "--queries")
                queries = positive(value());
            else if (arg == "--batch-size")
                batch = positive(value());
            else if (arg == "--directory")
                root = value();
            else if (arg == "--csv")
                csv = value();
            else
                throw std::runtime_error("unknown argument " + arg);
        }
        std::filesystem::create_directories(root);
        std::filesystem::path directory;
        const auto nonce =
            std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        for (unsigned attempt = 0;; ++attempt) {
            directory = root / ("run_" + nonce + "_" + std::to_string(attempt));
            if (std::filesystem::create_directory(directory))
                break;
        }
        std::ofstream file;
        std::ostream *out = &std::cout;
        if (!csv.empty()) {
            file.open(csv);
            if (!file)
                throw std::runtime_error("cannot open CSV " + csv);
            out = &file;
        }
        *out << "operation,table_rows,buffer_pages,operations,latency_unit,total_ms,operations_per_"
                "second,p50_us,p95_us,process_peak_rss_bytes,checksum\n";
        std::cerr << "Benchmark files: " << directory.string()
                  << "\nInsert latency includes commit for each batch of up to " << batch
                  << " rows; throughput is rows/second. Query throughput is statements/second. RSS "
                     "is cumulative process peak.\n";
        for (auto rows : sizes)
            for (auto capacity : buffers) {
                std::cerr << "Running rows=" << rows << " buffers=" << capacity << '\n';
                run(*out, directory, rows, capacity, queries, batch);
            }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Benchmark error: " << e.what() << '\n';
        return 1;
    }
}
