#include "minidb/database.hpp"
#include <algorithm>
#include <cmath>
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
template <class F> void rejects(F &&fn, const std::string &what) {
    ++checks;
    try {
        fn();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("expected error: " + what);
}
std::int64_t integer(const Value &value) {
    return std::get<std::int64_t>(value);
}
double numeric(const Value &value) {
    if (auto p = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*p);
    return std::get<double>(value);
}
std::int64_t scalar(Connection &c, const std::string &sql) {
    auto r = c.execute(sql);
    check(r.rows.size() == 1 && r.rows[0].size() == 1, "scalar result shape: " + sql);
    return integer(r.rows[0][0]);
}
void remove_db(const std::filesystem::path &p) {
    std::filesystem::remove(p);
    std::filesystem::remove(p.string() + ".wal");
}
} // namespace
int main() {
    try {
        auto path = std::filesystem::temp_directory_path() / "minidb_integration_test.db";
        remove_db(path);
        {
            Database db(path, Options{3});
            auto c = db.connect();
            c->execute("CREATE DATABASE integration");
            c->execute("CREATE TABLE users (id INTEGER PRIMARY KEY,name VARCHAR(100) NOT NULL,age "
                       "INTEGER,score DOUBLE,dept INTEGER)");
            c->execute(
                "CREATE TABLE departments (id INTEGER PRIMARY KEY,name VARCHAR(40) NOT NULL)");
            auto inserted = c->execute("INSERT INTO users VALUES "
                                       "(1,'Alice',20,95.5,1),(2,'Bob',22,88,1),(3,'Cara',NULL,"
                                       "NULL,2),(4,'D',20,76.5,2),(5,'E',17,50,NULL)");
            check(inserted.affected_rows == 5, "multirow affected count");
            c->execute("INSERT INTO departments VALUES (1,'Engineering'),(2,'Operations')");
            auto result = c->execute("SELECT * FROM users ORDER BY id");
            check(result.columns.size() == 5 && result.rows.size() == 5, "star projection");
            check(std::get<std::string>(result.rows[0][1]) == "Alice", "string roundtrip");
            check(std::holds_alternative<double>(result.rows[1][3]), "integer coerced to DOUBLE");
            check(is_null(result.rows[2][2]) && is_null(result.rows[2][3]), "NULL roundtrip");
            check(scalar(*c, "SELECT COUNT(*) FROM users WHERE age=NULL") == 0,
                  "three-valued comparison");
            check(scalar(*c, "SELECT COUNT(*) FROM users WHERE age IS NULL") == 1, "IS NULL");
            check(
                scalar(*c,
                       "SELECT COUNT(*) FROM users WHERE (age>=20 AND score>80) OR name='Cara'") ==
                    3,
                "parenthesized AND OR");
            auto page = c->execute(
                "SELECT name,age FROM users WHERE age>=18 ORDER BY score DESC LIMIT 2 OFFSET 1");
            check(page.rows.size() == 2 && std::get<std::string>(page.rows[0][0]) == "Bob" &&
                      std::get<std::string>(page.rows[1][0]) == "D",
                  "filter sort offset limit");
            auto agg =
                c->execute("SELECT COUNT(*),COUNT(age),SUM(score),AVG(score),MIN(score),MAX(score) "
                           "FROM users");
            check(integer(agg.rows[0][0]) == 5 && integer(agg.rows[0][1]) == 4,
                  "COUNT NULL semantics");
            check(std::abs(numeric(agg.rows[0][2]) - 310.0) < 1e-9 &&
                      std::abs(numeric(agg.rows[0][3]) - 77.5) < 1e-9,
                  "SUM AVG");
            check(numeric(agg.rows[0][4]) == 50 && numeric(agg.rows[0][5]) == 95.5, "MIN MAX");
            auto groups = c->execute(
                "SELECT dept,COUNT(*),SUM(score) FROM users GROUP BY dept ORDER BY dept");
            check(groups.rows.size() == 3 && is_null(groups.rows[0][0]), "NULL grouping");
            check(integer(groups.rows[1][0]) == 1 && integer(groups.rows[1][1]) == 2 &&
                      numeric(groups.rows[1][2]) == 183.5,
                  "grouped aggregate");
            auto empty = c->execute("SELECT COUNT(*),SUM(score),AVG(score),MIN(score),MAX(score) "
                                    "FROM users WHERE id<0");
            check(empty.rows.size() == 1 && integer(empty.rows[0][0]) == 0 &&
                      is_null(empty.rows[0][1]) && is_null(empty.rows[0][2]) &&
                      is_null(empty.rows[0][3]) && is_null(empty.rows[0][4]),
                  "empty aggregation");
            auto join = c->execute("SELECT u.name,d.name FROM users AS u INNER JOIN departments AS "
                                   "d ON u.dept=d.id WHERE u.age>=20 ORDER BY u.id");
            check(join.rows.size() == 3 &&
                      std::get<std::string>(join.rows[0][1]) == "Engineering" &&
                      std::get<std::string>(join.rows[2][1]) == "Operations",
                  "join with aliases filter projection");
            rejects(
                [&] {
                    c->execute("SELECT id FROM users u INNER JOIN departments d ON u.dept=d.id");
                },
                "ambiguous column");
            rejects([&] { c->execute("SELECT missing FROM users"); }, "unknown column");
            rejects([&] { c->execute("SELECT FROM users"); }, "syntax error");
            rejects([&] { c->execute("INSERT INTO users VALUES(1,'Duplicate',1,1,1)"); },
                    "duplicate primary key");
            rejects([&] { c->execute("INSERT INTO users VALUES(NULL,'Null key',1,1,1)"); },
                    "NULL primary key");
            rejects([&] { c->execute("INSERT INTO users VALUES(6,NULL,1,1,1)"); }, "NOT NULL");
            rejects([&] { c->execute("INSERT INTO users VALUES(6,'Bad type','twenty',1,1)"); },
                    "INTEGER type");
            rejects(
                [&] {
                    c->execute("INSERT INTO users VALUES(6,'Bad length" + std::string(100, 'x') +
                               "',1,1,1)");
                },
                "VARCHAR length");
            rejects(
                [&] { c->execute("INSERT INTO users VALUES(8,'First',1,1,1),(8,'Second',1,1,1)"); },
                "batch atomicity duplicate");
            check(scalar(*c, "SELECT COUNT(*) FROM users") == 5, "failed insert changed table");
            c->execute("CREATE INDEX idx_users_age ON users(age)");
            auto indexed = c->execute("SELECT id FROM users WHERE age=20 ORDER BY id");
            check(indexed.rows.size() == 2 && integer(indexed.rows[0][0]) == 1 &&
                      integer(indexed.rows[1][0]) == 4,
                  "secondary index duplicates");
            check(indexed.plan.find("IndexScan") != std::string::npos, "indexed equality plan");
            auto ranged = c->execute("SELECT id FROM users WHERE age>=20 AND age<22 ORDER BY id");
            check(ranged.rows.size() == 2, "index range filtering");
            auto update = c->execute("UPDATE users SET age=age+1,score=score+2 WHERE dept=1");
            check(update.affected_rows == 2, "update affected rows");
            check(scalar(*c, "SELECT COUNT(*) FROM users WHERE age=20") == 1 &&
                      scalar(*c, "SELECT COUNT(*) FROM users WHERE age=21") == 1,
                  "update maintains index");
            rejects([&] { c->execute("UPDATE users SET id=1 WHERE id=2"); },
                    "UPDATE primary conflict");
            check(scalar(*c, "SELECT age FROM users WHERE id=2") == 23, "failed update rollback");
            check(c->execute("DELETE FROM users WHERE age<18").affected_rows == 1,
                  "delete affected rows");
            check(scalar(*c, "SELECT COUNT(*) FROM users WHERE age=17") == 0,
                  "delete maintains index");
            c->execute("BEGIN");
            check(c->in_transaction(), "begin state");
            c->execute("UPDATE users SET age=99 WHERE id=1");
            check(scalar(*c, "SELECT age FROM users WHERE id=1") == 99, "read own writes");
            c->execute("ROLLBACK");
            check(!c->in_transaction(), "rollback state");
            check(scalar(*c, "SELECT age FROM users WHERE id=1") == 21,
                  "rollback restored heap and index");
            check(scalar(*c, "SELECT COUNT(*) FROM users WHERE age=99") == 0,
                  "rollback index state");
            c->execute("BEGIN");
            c->execute("INSERT INTO users(id,name) VALUES (6,'Committed')");
            c->execute("COMMIT");
            check(!c->in_transaction(), "commit state");
            c->execute("BEGIN");
            c->execute("INSERT INTO users VALUES(7,'Aborted',7,7,1)");
            rejects([&] { c->execute("INSERT INTO users VALUES(1,'Failure',1,1,1)"); },
                    "explicit transaction failure");
            check(!c->in_transaction(), "error aborts transaction");
            check(scalar(*c, "SELECT COUNT(*) FROM users WHERE id=7") == 0,
                  "error rolled back prior statement");
            c->execute("BEGIN");
            c->execute("CREATE TABLE temporary(id INTEGER)");
            c->execute("ROLLBACK");
            auto names = c->tables();
            check(std::find(names.begin(), names.end(), "temporary") == names.end(),
                  "DDL rollback");
            {
                auto abandoned = db.connect();
                abandoned->execute("BEGIN");
                abandoned->execute("DELETE FROM users");
            }
            check(scalar(*c, "SELECT COUNT(*) FROM users") == 5, "connection destructor rollback");
            auto prepared = c->prepare("SELECT name FROM users WHERE id=1");
            check(std::get<std::string>(prepared.execute().rows[0][0]) == "Alice", "prepared API");
            c->execute("CREATE TABLE dropped (id INTEGER PRIMARY KEY, v VARCHAR(20))");
            c->execute("INSERT INTO dropped VALUES (1,'data')");
            c->execute("CREATE INDEX idx_dropped ON dropped(v)");
            c->execute("DROP TABLE dropped");
            rejects([&] { c->execute("SELECT * FROM dropped"); }, "drop table");
            check(c->schema().find("users") != std::string::npos, "schema API");
            c->execute("CHECKPOINT");
        }
        {
            Database db(path, Options{2});
            auto c = db.connect();
            check(scalar(*c, "SELECT COUNT(*) FROM users") == 5, "restart row count");
            check(scalar(*c, "SELECT COUNT(*) FROM departments") == 2, "restart second table");
            auto row = c->execute("SELECT name,age FROM users WHERE id=6");
            check(row.rows.size() == 1 && std::get<std::string>(row.rows[0][0]) == "Committed" &&
                      is_null(row.rows[0][1]),
                  "committed nullable row restart");
            auto indexed = c->execute("SELECT name FROM users WHERE age=21");
            check(indexed.rows.size() == 1 && indexed.plan.find("IndexScan") != std::string::npos,
                  "persistent secondary index");
            auto names = c->tables();
            check(names.size() == 2, "persistent DDL catalog");
            c->execute("DROP TABLE users");
            c->execute("DROP TABLE departments");
            check(c->tables().empty(), "drop all tables");
        }
        {
            Database db(path);
            auto c = db.connect();
            check(c->tables().empty(), "empty catalog restart");
        }
        remove_db(path);
        std::cout << "PASS integration " << checks << " checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL integration after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
