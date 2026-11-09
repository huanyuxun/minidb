#include "minidb/executor.hpp"
#include "minidb/sql.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace minidb;
namespace {
int checks = 0;
void check(bool condition, const std::string &what) {
    ++checks;
    if (!condition)
        throw std::runtime_error(what);
}
template <class F> void rejects(F fn, const std::string &what) {
    ++checks;
    try {
        fn();
    } catch (const SqlError &) {
        return;
    }
    throw std::runtime_error("expected rejection: " + what);
}
Value expression(const std::string &text) {
    auto s = parse("SELECT " + text);
    return eval(s.select.at(0).expr, [](const std::string &name) -> Value {
        if (name == "x")
            return std::int64_t{7};
        throw SqlError("unknown column " + name);
    });
}
void truth(const std::string &text, bool result) {
    check(std::get<bool>(expression(text)) == result, text);
}
} // namespace
int main() {
    try {
        auto tokens = lex("-- comment\n SELECT 'a''b', \"Mixed\" /* comment */ >= 1.2e-3;");
        check(tokens.size() == 8, "token count");
        check(tokens[1].text == "a'b", "escaped string");
        check(tokens[3].quoted && tokens[3].text == "Mixed", "quoted identifier");
        rejects([] { lex("'open"); }, "open string");
        rejects([] { lex("/* open"); }, "open comment");
        rejects([] { lex("1e+"); }, "exponent");
        rejects([] { lex("~"); }, "character");
        auto ddl = parse("CREATE TABLE users(id INTEGER PRIMARY KEY, name VARCHAR(100) NOT NULL, "
                         "age INTEGER, score DOUBLE)");
        check(ddl.kind == Statement::Kind::CreateTable && ddl.table == "users" &&
                  ddl.columns.size() == 4,
              "create AST");
        check(ddl.columns[0].primary_key && ddl.columns[0].not_null &&
                  ddl.columns[1].max_length == 100,
              "column metadata");
        check(parse("CREATE TABLE t(id INTEGER, PRIMARY KEY(id))").columns[0].primary_key,
              "table primary key");
        check(parse("CREATE DATABASE Test;").name == "test", "database name");
        auto idx = parse("CREATE INDEX idx_age ON users(age)");
        check(idx.index == "idx_age" && idx.column == "age" && idx.table == "users", "index AST");
        check(parse("DROP TABLE users").kind == Statement::Kind::DropTable, "drop AST");
        auto ins = parse("INSERT INTO users (id,name) VALUES (1,'Alice'),(2,'Bob')");
        check(ins.values.size() == 2 && ins.insert_columns.size() == 2, "insert AST");
        auto upd = parse("UPDATE users SET age=age+1, score=score*1.1 WHERE id=1");
        check(upd.assignments.size() == 2 && upd.where->name == "=", "update AST");
        check(parse("DELETE FROM users WHERE age<18").where->name == "<", "delete AST");
        auto sel = parse("SELECT u.name AS n, COUNT(*), SUM(u.score) FROM users AS u INNER JOIN "
                         "orders o ON u.id=o.uid WHERE (u.age>=18 AND u.score>80) OR u.age IS NULL "
                         "GROUP BY u.name ORDER BY n DESC LIMIT 10 OFFSET 2;");
        check(sel.select.size() == 3 && sel.joins.size() == 1 && sel.from_alias == "u" &&
                  sel.joins[0].alias == "o",
              "select sources");
        check(sel.where->name == "OR" && sel.where->args[0]->name == "AND", "precedence");
        check(sel.group_by.size() == 1 && sel.order_by[0].descending && sel.limit == 10 &&
                  sel.offset == 2,
              "clauses");
        check(parse("SELECT u.* FROM users u").select[0].expr->name == "u", "qualified star");
        check(parse("BEGIN TRANSACTION;").kind == Statement::Kind::Begin, "begin");
        check(parse("START TRANSACTION").kind == Statement::Kind::Begin, "start");
        check(parse("COMMIT").kind == Statement::Kind::Commit, "commit");
        check(parse("ROLLBACK").kind == Statement::Kind::Rollback, "rollback");
        check(parse("CHECKPOINT").kind == Statement::Kind::Checkpoint, "checkpoint");
        check(parse("EXPLAIN SELECT * FROM users").explained->kind == Statement::Kind::Select,
              "explain");
        for (const auto *sql :
             {"", "SELECT", "SELECT 1; SELECT 2", "CREATE TABLE t()",
              "CREATE TABLE t(a INTEGER, a INTEGER)",
              "CREATE TABLE t(a INTEGER PRIMARY KEY,b INTEGER PRIMARY KEY)",
              "CREATE TABLE t(a VARCHAR(0))", "CREATE TABLE t(a INTEGER,PRIMARY KEY(b))",
              "SELECT SUM(*) FROM t", "SELECT COUNT() FROM t", "SELECT SUM(COUNT(*)) FROM t",
              "SELECT 1 LIMIT -1", "SELECT 1 WHERE 1=1=1", "SELECT 9223372036854775808",
              "SELECT 1e999", "EXPLAIN INSERT INTO t VALUES(1)",
              "SELECT * FROM t LEFT JOIN u ON t.a=u.a"})
            rejects([&] { parse(sql); }, sql);
        check(std::get<std::int64_t>(expression("2+3*4")) == 14, "arithmetic precedence");
        check(std::get<std::int64_t>(expression("(2+3)*4")) == 20, "parentheses");
        check(std::get<std::int64_t>(expression("x+1")) == 8, "resolver");
        check(std::get<std::int64_t>(expression("7/2")) == 3, "integer division");
        check(std::get<double>(expression("7/2.0")) == 3.5, "double division");
        check(std::get<std::int64_t>(expression("7%3")) == 1, "modulo");
        truth("1=1.0", true);
        truth("'a'<'b'", true);
        truth("NOT 1=2 AND TRUE", true);
        truth("(1=2 OR 3=3) AND 4>2", true);
        truth("NULL IS NULL", true);
        truth("NULL IS NOT NULL", false);
        check(is_null(expression("NULL=1")), "null equality");
        check(is_null(expression("NULL+1")), "null arithmetic");
        truth("FALSE AND NULL", false);
        truth("TRUE OR NULL", true);
        truth("NULL AND FALSE", false);
        truth("NULL OR TRUE", true);
        check(is_null(expression("NULL AND TRUE")), "null and true");
        check(is_null(expression("NULL OR FALSE")), "null or false");
        check(is_null(expression("NOT NULL")), "not null");
        truth("FALSE AND 1/0=0", false);
        truth("TRUE OR 1/0=0", true);
        check(std::get<std::int64_t>(expression("COALESCE(NULL,7,1/0)")) == 7,
              "coalesce short circuit");
        check(std::get<std::string>(expression("'a'||'b'")) == "ab", "concatenation");
        truth("'alphabet' LIKE 'a%et'", true);
        truth("'abc' LIKE 'a_c'", true);
        truth("'abc' LIKE 'a_d'", false);
        check(std::get<std::int64_t>(expression("LENGTH('abc')")) == 3, "length");
        check(std::get<std::string>(expression("LOWER('AbC')")) == "abc", "lower");
        check(std::get<std::string>(expression("UPPER('AbC')")) == "ABC", "upper");
        check(std::get<std::int64_t>(expression("ABS(-7)")) == 7, "abs");
        check(std::get<std::int64_t>(expression("-9223372036854775808")) ==
                  std::numeric_limits<std::int64_t>::min(),
              "minimum integer");
        truth("9007199254740993 > 9007199254740992.0", true);
        truth("9223372036854775807 < 9223372036854775808.0", true);
        check(compare_values(Value{std::int64_t{7}}, Value{7.5}) < 0, "fraction comparison");
        check(compare_values(Value{std::int64_t{-7}}, Value{-7.5}) > 0,
              "negative fraction comparison");
        for (const auto *expr :
             {"1/0", "1%0", "9223372036854775807+1", "-9223372036854775808-1",
              "9223372036854775807*2", "-9223372036854775808/-1", "-(-9223372036854775808)",
              "ABS(-9223372036854775808)", "'a'+1", "'a'=1", "1.5%1", "FOO(1)", "FOO(NULL)",
              "LENGTH(1)", "missing", "SUM(1)"})
            rejects([&] { expression(expr); }, expr);
        check(expression_string(parse("SELECT COUNT(*)").select[0].expr) == "COUNT(*)",
              "expression rendering");
        check(contains_aggregate(parse("SELECT SUM(x)+1").select[0].expr), "aggregate detection");
        auto iterator =
            rows_executor({Row{std::int64_t{3}}, Row{std::int64_t{1}}, Row{std::int64_t{2}}});
        iterator = filter_executor(std::move(iterator),
                                   [](const Row &r) { return std::get<std::int64_t>(r[0]) > 1; });
        iterator = projection_executor(std::move(iterator), [](const Row &r) {
            return Row{std::get<std::int64_t>(r[0]) * 10};
        });
        iterator = sort_executor(std::move(iterator), [](const Row &a, const Row &b) {
            return compare_values(a[0], b[0]) < 0;
        });
        iterator = limit_executor(std::move(iterator), 1, 1);
        auto pipeline = collect(std::move(iterator));
        check(pipeline.size() == 1 && std::get<std::int64_t>(pipeline[0][0]) == 30,
              "filter/project/sort/limit pipeline");
        auto reusable =
            limit_executor(rows_executor({Row{std::int64_t{1}}, Row{std::int64_t{2}}}), 1, 1);
        Row output;
        reusable->Init();
        check(reusable->Next(output) && std::get<std::int64_t>(output[0]) == 2,
              "iterator first init");
        check(!reusable->Next(output), "limit exhausted");
        reusable->Close();
        reusable->Init();
        check(reusable->Next(output), "iterator reinit");
        reusable->Close();
        auto joined =
            collect(join_executor(rows_executor({Row{std::int64_t{1}}, Row{std::int64_t{2}}}),
                                  {Row{std::int64_t{2}}, Row{std::int64_t{3}}},
                                  [](const Row &r) { return compare_values(r[0], r[1]) == 0; }));
        check(joined.size() == 1 && joined[0].size() == 2 &&
                  std::get<std::int64_t>(joined[0][0]) == 2,
              "nested loop join");
        check(collect(limit_executor(rows_executor({Row{std::int64_t{1}}}), 0, 0)).empty(),
              "zero limit");
        check(collect(limit_executor(rows_executor({Row{std::int64_t{1}}}), 10, 1)).empty(),
              "offset beyond end");
        check(collect(join_executor(rows_executor({Row{std::int64_t{1}}}), {},
                                    [](const Row &) { return true; }))
                  .empty(),
              "empty join side");
        std::cout << "SQL lexer/parser/expression/executor checks: " << checks << " passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
