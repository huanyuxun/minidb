#include "minidb/database.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace minidb;
namespace {
int checks = 0;
void check(bool condition, const std::string &what) {
    ++checks;
    if (!condition)
        throw std::runtime_error(what);
}
template <class F> void rejects(F action, const std::string &what) {
    ++checks;
    try {
        action();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("expected error: " + what);
}
std::int64_t scalar(Connection &c, const std::string &sql) {
    return std::get<std::int64_t>(c.execute(sql).rows.at(0).at(0));
}
} // namespace
int main() {
    try {
        const auto path =
            std::filesystem::temp_directory_path() /
            ("minidb_regression_" +
             std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) +
             ".db");
        {
            Database db(path, Options{2});
            auto c = db.connect();
            c->execute("CREATE TABLE text_values(id INTEGER PRIMARY KEY,body TEXT)");
            c->execute("INSERT INTO text_values VALUES(1,'nonempty text payload')");
            check(std::get<std::string>(
                      c->execute("SELECT body FROM text_values").rows.at(0).at(0)) ==
                      "nonempty text payload",
                  "TEXT has no VARCHAR length bound");
            c->execute(
                "CREATE TABLE edge(id INTEGER PRIMARY KEY,v INTEGER,d DOUBLE,s VARCHAR(30))");
            rejects([&] { c->execute("SELECT missing FROM edge"); },
                    "unbound projection on empty table");
            rejects([&] { c->execute("SELECT id FROM edge WHERE missing=1"); },
                    "unbound filter on empty table");
            rejects([&] { c->execute("SELECT id FROM edge ORDER BY missing"); },
                    "unbound order on empty table");
            rejects([&] { c->execute("SELECT nonsense(id) FROM edge"); },
                    "unknown function on empty input");
            rejects([&] { c->execute("SELECT COALESCE() FROM edge"); },
                    "invalid scalar arity on empty input");
            c->execute("INSERT INTO edge "
                       "VALUES(1,9007199254740992,9007199254740992.0,'a'),(2,9007199254740993,"
                       "9007199254740992.0,'b'),(3,-1,-0.0,'c'),(4,0,0.0,NULL),(5,NULL,NULL,'d')");
            const std::vector<std::string> predicates{"v=9007199254740992.0",
                                                      "v>9007199254740992.0",
                                                      "v<1.5",
                                                      "v>=-0.5",
                                                      "v<=0.0",
                                                      "v=9223372036854775808.0",
                                                      "v IS NULL",
                                                      "v=0 OR v IS NULL",
                                                      "d=9007199254740993",
                                                      "d<9007199254740993",
                                                      "d=0.0",
                                                      "d=-0.0",
                                                      "d IS NULL",
                                                      "s<'c'",
                                                      "s='a'"};
            std::vector<std::vector<Row>> expected;
            for (const auto &predicate : predicates)
                expected.push_back(
                    c->execute("SELECT id FROM edge WHERE " + predicate + " ORDER BY id").rows);
            c->execute("CREATE INDEX idx_v ON edge(v)");
            c->execute("CREATE INDEX idx_d ON edge(d)");
            c->execute("CREATE INDEX idx_s ON edge(s)");
            for (std::size_t i = 0; i < predicates.size(); ++i)
                check(c->execute("SELECT id FROM edge WHERE " + predicates[i] + " ORDER BY id")
                              .rows == expected[i],
                      "index/scan semantic equivalence: " + predicates[i]);
            check(scalar(*c, "SELECT COUNT(*) FROM edge WHERE v=9007199254740992.0") == 1,
                  "INTEGER/DOUBLE exact large comparison");
            check(scalar(*c, "SELECT COUNT(*) FROM edge WHERE d=9007199254740993") == 0,
                  "DOUBLE/INTEGER exact large comparison");
            check(scalar(*c, "SELECT COUNT(*) FROM edge WHERE d=-0.0") == 2,
                  "normalized signed-zero index key");
            c->execute("CREATE TABLE swaps(id INTEGER PRIMARY KEY,a INTEGER,b INTEGER)");
            c->execute("INSERT INTO swaps VALUES(1,10,20)");
            auto joined =
                c->execute("SELECT * FROM edge e INNER JOIN swaps s ON e.id=s.id ORDER BY e.id");
            check(joined.rows.size() == 1 && joined.rows[0].size() == 7,
                  "JOIN star permits duplicate output names");
            c->execute("UPDATE swaps SET a=b,b=a WHERE id=1");
            auto swapped = c->execute("SELECT a,b FROM swaps").rows.at(0);
            check(std::get<std::int64_t>(swapped[0]) == 20 &&
                      std::get<std::int64_t>(swapped[1]) == 10,
                  "all UPDATE expressions use original row");
            rejects([&] { c->execute("SELECT id,COUNT(*) FROM edge"); },
                    "ungrouped column with aggregate");
            rejects([&] { c->execute("SELECT id,COUNT(*) FROM edge WHERE id<0"); },
                    "empty input cannot mask grouping error");
            rejects([&] { c->execute("SELECT SUM(COUNT(*)) FROM edge"); }, "nested aggregates");
            c->execute("BEGIN");
            c->execute("UPDATE swaps SET a=99 WHERE id=1");
            rejects([&] { c->execute("SELEC * FROM swaps"); },
                    "parse error aborts explicit transaction");
            check(!c->in_transaction(), "syntax error clears transaction state");
            check(scalar(*c, "SELECT a FROM swaps") == 20,
                  "syntax error rolls back previous statement");
            auto grouped =
                c->execute("SELECT s,COUNT(*) AS n FROM edge GROUP BY s ORDER BY n DESC,s");
            check(grouped.rows.size() == 5, "ORDER BY aggregate alias");
            check(c->execute("SELECT * FROM edge ORDER BY id LIMIT 0").rows.empty(), "LIMIT zero");
            c->execute("BEGIN");
            for (int i = 0; i < 50; ++i) {
                c->execute("CREATE TABLE catalog_stress_table_" + std::to_string(i) +
                           "(id INTEGER PRIMARY KEY,description VARCHAR(100),quantity INTEGER)");
            }
            c->execute("COMMIT");
            check(c->tables().size() == 53, "catalog spans multiple pages");
        }
        {
            Database db(path, Options{2});
            auto c = db.connect();
            check(c->tables().size() == 53, "multi-page catalog survives reopen");
            c->execute("BEGIN");
            for (int i = 0; i < 50; ++i)
                c->execute("DROP TABLE catalog_stress_table_" + std::to_string(i));
            c->execute("COMMIT");
            check(c->tables().size() == 3, "multi-page catalog shrinking");
        }
        {
            Database db(path, Options{2});
            auto c = db.connect();
            check(c->tables().size() == 3, "shrunk catalog survives reopen");
            check(scalar(*c, "SELECT COUNT(*) FROM edge") == 5, "shrinking catalog preserves rows");
        }
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".wal");
        std::cout << "PASS regression " << checks << " semantic edge cases\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL regression after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
