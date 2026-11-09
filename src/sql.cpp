#include "minidb/sql.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace minidb {
namespace {
bool alpha(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' ||
           static_cast<unsigned char>(c) >= 128;
}
bool digit(char c) {
    return c >= '0' && c <= '9';
}
std::string upper(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
[[noreturn]] void syntax(std::size_t pos, const std::string &message) {
    throw SqlError("SQL syntax at byte " + std::to_string(pos) + ": " + message);
}
ExprPtr node(Expr::Kind k, std::string name = {}, std::vector<ExprPtr> args = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = k;
    e->name = std::move(name);
    e->args = std::move(args);
    return e;
}
ExprPtr literal(Value v) {
    auto e = node(Expr::Kind::Literal);
    e->literal = std::move(v);
    return e;
}
std::int64_t integer(const Token &t, bool negative = false) {
    std::string s = negative ? "-" + t.text : t.text;
    std::int64_t value{};
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size())
        syntax(t.offset, "integer out of range");
    return value;
}
class Parser {
    std::vector<Token> tokens_;
    std::size_t at_ = 0;
    const Token &peek() const {
        return tokens_.at(at_);
    }
    bool match(std::string_view value) const {
        const auto &t = peek();
        return !t.quoted && (t.kind == Token::Kind::Identifier || t.kind == Token::Kind::Symbol) &&
               upper(t.text) == value;
    }
    bool take(std::string_view value) {
        if (!match(value))
            return false;
        ++at_;
        return true;
    }
    void expect(std::string_view value) {
        if (!take(value))
            syntax(peek().offset,
                   "expected " + std::string(value) + ", found '" + peek().text + "'");
    }
    std::string identifier() {
        const auto t = peek();
        if (t.kind != Token::Kind::Identifier)
            syntax(t.offset, "expected identifier");
        ++at_;
        return t.quoted ? t.text : lower(t.text);
    }
    std::size_t size_literal() {
        const auto t = peek();
        if (t.kind != Token::Kind::Number || t.text.find_first_of(".eE") != std::string::npos)
            syntax(t.offset, "expected non-negative integer");
        ++at_;
        const auto n = integer(t);
        if (n < 0)
            syntax(t.offset, "expected non-negative integer");
        return static_cast<std::size_t>(n);
    }
    bool alias_possible() const {
        if (peek().kind != Token::Kind::Identifier)
            return false;
        if (peek().quoted)
            return true;
        static const std::unordered_set<std::string> stops = {
            "FROM",  "WHERE", "GROUP", "ORDER", "LIMIT", "OFFSET", "INNER",
            "JOIN",  "ON",    "LEFT",  "RIGHT", "FULL",  "CROSS",  "HAVING",
            "UNION", "ASC",   "DESC",  "AND",   "OR",    "SET",    "VALUES"};
        return !stops.contains(upper(peek().text));
    }
    std::string alias() {
        if (take("AS"))
            return identifier();
        return alias_possible() ? identifier() : std::string{};
    }
    ExprPtr expr() {
        return disjunction();
    }
    ExprPtr disjunction() {
        auto left = conjunction();
        while (take("OR"))
            left = node(Expr::Kind::Binary, "OR", {left, conjunction()});
        return left;
    }
    ExprPtr conjunction() {
        auto left = negation();
        while (take("AND"))
            left = node(Expr::Kind::Binary, "AND", {left, negation()});
        return left;
    }
    ExprPtr negation() {
        if (take("NOT"))
            return node(Expr::Kind::Unary, "NOT", {negation()});
        return comparison();
    }
    ExprPtr comparison() {
        auto left = additive();
        if (take("IS")) {
            bool inverse = take("NOT");
            expect("NULL");
            return node(Expr::Kind::Unary, inverse ? "IS NOT NULL" : "IS NULL", {left});
        }
        for (const auto *op : {"=", "!=", "<>", "<=", ">=", "<", ">", "LIKE"}) {
            if (take(op))
                return node(Expr::Kind::Binary, op, {left, additive()});
        }
        return left;
    }
    ExprPtr additive() {
        auto left = multiplicative();
        for (;;) {
            if (take("+"))
                left = node(Expr::Kind::Binary, "+", {left, multiplicative()});
            else if (take("-"))
                left = node(Expr::Kind::Binary, "-", {left, multiplicative()});
            else if (take("||"))
                left = node(Expr::Kind::Binary, "||", {left, multiplicative()});
            else
                return left;
        }
    }
    ExprPtr multiplicative() {
        auto left = unary();
        for (;;) {
            if (take("*"))
                left = node(Expr::Kind::Binary, "*", {left, unary()});
            else if (take("/"))
                left = node(Expr::Kind::Binary, "/", {left, unary()});
            else if (take("%"))
                left = node(Expr::Kind::Binary, "%", {left, unary()});
            else
                return left;
        }
    }
    ExprPtr unary() {
        if (take("+"))
            return node(Expr::Kind::Unary, "+", {unary()});
        if (take("-")) {
            if (peek().kind == Token::Kind::Number &&
                peek().text.find_first_of(".eE") == std::string::npos) {
                auto t = peek();
                ++at_;
                return literal(integer(t, true));
            }
            return node(Expr::Kind::Unary, "-", {unary()});
        }
        return primary();
    }
    ExprPtr primary() {
        if (take("(")) {
            auto e = expr();
            expect(")");
            return e;
        }
        if (take("NULL"))
            return literal(std::monostate{});
        if (take("TRUE"))
            return literal(true);
        if (take("FALSE"))
            return literal(false);
        if (take("*"))
            return node(Expr::Kind::Star);
        auto t = peek();
        if (t.kind == Token::Kind::String) {
            ++at_;
            return literal(t.text);
        }
        if (t.kind == Token::Kind::Number) {
            ++at_;
            if (t.text.find_first_of(".eE") == std::string::npos)
                return literal(integer(t));
            double d{};
            const auto parsed = std::from_chars(t.text.data(), t.text.data() + t.text.size(), d);
            if (parsed.ec != std::errc{} || parsed.ptr != t.text.data() + t.text.size() ||
                !std::isfinite(d))
                syntax(t.offset, "floating-point number out of range");
            return literal(d);
        }
        if (t.kind == Token::Kind::Identifier) {
            auto name = identifier();
            if (take("(")) {
                std::vector<ExprPtr> args;
                if (!take(")")) {
                    do {
                        args.push_back(expr());
                    } while (take(","));
                    expect(")");
                }
                const auto fname = upper(name);
                if (fname == "COUNT" || fname == "SUM" || fname == "AVG" || fname == "MIN" ||
                    fname == "MAX") {
                    if (args.size() != 1)
                        syntax(t.offset, "aggregate requires one argument");
                    if (args[0]->kind == Expr::Kind::Star && fname != "COUNT")
                        syntax(t.offset, "only COUNT accepts *");
                    if (contains_aggregate(args[0]))
                        syntax(t.offset, "nested aggregates are not supported");
                }
                return node(Expr::Kind::Function, fname, std::move(args));
            }
            if (take(".")) {
                if (take("*"))
                    return node(Expr::Kind::Star, name);
                name += "." + identifier();
            }
            return node(Expr::Kind::Column, name);
        }
        syntax(t.offset, "expected expression, found '" + t.text + "'");
    }
    void select(Statement &s) {
        s.kind = Statement::Kind::Select;
        do {
            auto e = expr();
            auto a = alias();
            s.select.push_back({e, a});
        } while (take(","));
        if (take("FROM")) {
            s.from = identifier();
            s.from_alias = alias();
            while (match("INNER") || match("JOIN")) {
                take("INNER");
                expect("JOIN");
                Join j;
                j.table = identifier();
                j.alias = alias();
                expect("ON");
                j.on = expr();
                s.joins.push_back(std::move(j));
            }
        }
        if (take("WHERE"))
            s.where = expr();
        if (take("GROUP")) {
            expect("BY");
            do {
                s.group_by.push_back(expr());
            } while (take(","));
        }
        if (take("ORDER")) {
            expect("BY");
            do {
                auto e = expr();
                bool desc = take("DESC");
                if (!desc)
                    take("ASC");
                s.order_by.push_back({e, desc});
            } while (take(","));
        }
        if (take("LIMIT")) {
            s.limit = size_literal();
            if (take("OFFSET"))
                s.offset = size_literal();
        }
    }
    void create(Statement &s) {
        if (take("DATABASE")) {
            s.kind = Statement::Kind::CreateDatabase;
            s.name = identifier();
            return;
        }
        if (take("INDEX")) {
            s.kind = Statement::Kind::CreateIndex;
            s.name = s.index = identifier();
            expect("ON");
            s.table = identifier();
            expect("(");
            s.column = identifier();
            expect(")");
            return;
        }
        expect("TABLE");
        s.kind = Statement::Kind::CreateTable;
        s.table = s.name = identifier();
        expect("(");
        std::string table_pk;
        do {
            if (take("PRIMARY")) {
                expect("KEY");
                expect("(");
                if (!table_pk.empty())
                    syntax(peek().offset, "multiple primary keys");
                table_pk = identifier();
                expect(")");
                continue;
            }
            Column c;
            c.name = identifier();
            if (take("INTEGER") || take("INT") || take("BIGINT"))
                c.type = Type::Integer;
            else if (take("DOUBLE") || take("REAL"))
                c.type = Type::Double;
            else if (take("VARCHAR")) {
                c.type = Type::Varchar;
                expect("(");
                c.max_length = size_literal();
                if (c.max_length == 0)
                    syntax(peek().offset, "VARCHAR size must be positive");
                expect(")");
            } else if (take("TEXT")) {
                c.type = Type::Varchar;
                c.max_length = 0;
            } else
                syntax(peek().offset, "expected INTEGER, DOUBLE or VARCHAR(n)");
            for (;;) {
                if (take("PRIMARY")) {
                    expect("KEY");
                    c.primary_key = c.not_null = true;
                } else if (take("NOT")) {
                    expect("NULL");
                    c.not_null = true;
                } else if (take("NULL")) {
                    if (c.not_null)
                        syntax(peek().offset, "conflicting NULL constraint");
                } else
                    break;
            }
            for (const auto &existing : s.columns)
                if (existing.name == c.name)
                    syntax(peek().offset, "duplicate column '" + c.name + "'");
            s.columns.push_back(std::move(c));
        } while (take(","));
        expect(")");
        if (s.columns.empty())
            syntax(peek().offset, "table requires at least one column");
        if (!table_pk.empty()) {
            bool found = false;
            for (auto &c : s.columns)
                if (c.name == table_pk) {
                    if (c.primary_key)
                        syntax(peek().offset, "duplicate primary key");
                    c.primary_key = c.not_null = true;
                    found = true;
                }
            if (!found)
                syntax(peek().offset, "unknown primary key column");
        }
        if (std::count_if(s.columns.begin(), s.columns.end(),
                          [](const Column &c) { return c.primary_key; }) > 1)
            syntax(peek().offset, "only one primary key column is supported");
    }
    Statement statement() {
        Statement s;
        if (take("CREATE"))
            create(s);
        else if (take("DROP")) {
            expect("TABLE");
            s.kind = Statement::Kind::DropTable;
            s.table = s.name = identifier();
        } else if (take("INSERT")) {
            expect("INTO");
            s.kind = Statement::Kind::Insert;
            s.table = identifier();
            if (take("(")) {
                do {
                    s.insert_columns.push_back(identifier());
                } while (take(","));
                expect(")");
            }
            expect("VALUES");
            do {
                expect("(");
                std::vector<ExprPtr> row;
                do {
                    row.push_back(expr());
                } while (take(","));
                expect(")");
                s.values.push_back(std::move(row));
            } while (take(","));
        } else if (take("UPDATE")) {
            s.kind = Statement::Kind::Update;
            s.table = identifier();
            expect("SET");
            do {
                auto name = identifier();
                expect("=");
                auto e = expr();
                s.assignments.emplace_back(name, e);
            } while (take(","));
            if (take("WHERE"))
                s.where = expr();
        } else if (take("DELETE")) {
            s.kind = Statement::Kind::Delete;
            expect("FROM");
            s.table = identifier();
            if (take("WHERE"))
                s.where = expr();
        } else if (take("SELECT"))
            select(s);
        else if (take("BEGIN")) {
            s.kind = Statement::Kind::Begin;
            take("TRANSACTION");
        } else if (take("START")) {
            expect("TRANSACTION");
            s.kind = Statement::Kind::Begin;
        } else if (take("COMMIT")) {
            s.kind = Statement::Kind::Commit;
            take("TRANSACTION");
        } else if (take("ROLLBACK")) {
            s.kind = Statement::Kind::Rollback;
            take("TRANSACTION");
        } else if (take("CHECKPOINT"))
            s.kind = Statement::Kind::Checkpoint;
        else if (take("EXPLAIN")) {
            s.kind = Statement::Kind::Explain;
            s.explained = std::make_unique<Statement>(statement());
            if (s.explained->kind != Statement::Kind::Select)
                syntax(peek().offset, "EXPLAIN requires SELECT");
        } else
            syntax(peek().offset, "expected SQL statement");
        return s;
    }

  public:
    explicit Parser(std::string_view sql) : tokens_(lex(sql)) {}
    Statement parse_one() {
        auto s = statement();
        take(";");
        if (peek().kind != Token::Kind::End)
            syntax(peek().offset, "unexpected token '" + peek().text + "'");
        return s;
    }
};
} // namespace

std::vector<Token> lex(std::string_view sql) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < sql.size()) {
        if (std::isspace(static_cast<unsigned char>(sql[i]))) {
            ++i;
            continue;
        }
        if (sql[i] == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            i += 2;
            while (i < sql.size() && sql[i] != '\n')
                ++i;
            continue;
        }
        if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            const auto start = i;
            i += 2;
            while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/'))
                ++i;
            if (i + 1 >= sql.size())
                syntax(start, "unterminated comment");
            i += 2;
            continue;
        }
        const auto start = i;
        if (sql[i] == '\'' || sql[i] == '"') {
            const char quote = sql[i++];
            std::string text;
            bool closed = false;
            while (i < sql.size()) {
                if (sql[i] == quote) {
                    ++i;
                    if (i < sql.size() && sql[i] == quote) {
                        text += quote;
                        ++i;
                    } else {
                        closed = true;
                        break;
                    }
                } else
                    text += sql[i++];
            }
            if (!closed)
                syntax(start, "unterminated quoted text");
            if (quote == '"' && text.empty())
                syntax(start, "empty quoted identifier");
            tokens.push_back({quote == '\'' ? Token::Kind::String : Token::Kind::Identifier,
                              std::move(text), start, quote == '"'});
            continue;
        }
        if (alpha(sql[i])) {
            ++i;
            while (i < sql.size() && (alpha(sql[i]) || digit(sql[i])))
                ++i;
            tokens.push_back(
                {Token::Kind::Identifier, std::string(sql.substr(start, i - start)), start});
            continue;
        }
        if (digit(sql[i]) || (sql[i] == '.' && i + 1 < sql.size() && digit(sql[i + 1]))) {
            while (i < sql.size() && digit(sql[i]))
                ++i;
            if (i < sql.size() && sql[i] == '.') {
                ++i;
                while (i < sql.size() && digit(sql[i]))
                    ++i;
            }
            if (i < sql.size() && (sql[i] == 'e' || sql[i] == 'E')) {
                ++i;
                if (i < sql.size() && (sql[i] == '+' || sql[i] == '-'))
                    ++i;
                const auto exponent = i;
                while (i < sql.size() && digit(sql[i]))
                    ++i;
                if (i == exponent)
                    syntax(start, "malformed numeric exponent");
            }
            tokens.push_back(
                {Token::Kind::Number, std::string(sql.substr(start, i - start)), start});
            continue;
        }
        const auto two = std::string(sql.substr(i, 2));
        if (two == "<=" || two == ">=" || two == "<>" || two == "!=" || two == "||") {
            tokens.push_back({Token::Kind::Symbol, two, start});
            i += 2;
            continue;
        }
        if (std::string_view("(),;.*+-/%=<> ").find(sql[i]) != std::string_view::npos) {
            tokens.push_back({Token::Kind::Symbol, std::string(1, sql[i++]), start});
            continue;
        }
        syntax(start, "unexpected character");
    }
    tokens.push_back({Token::Kind::End, "", sql.size()});
    return tokens;
}
Statement parse(std::string_view sql) {
    return Parser(sql).parse_one();
}

bool is_null(const Value &v) {
    return std::holds_alternative<std::monostate>(v);
}
namespace {
bool numeric(const Value &v) {
    return std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v);
}
double as_double(const Value &v) {
    if (auto i = std::get_if<std::int64_t>(&v))
        return static_cast<double>(*i);
    if (auto d = std::get_if<double>(&v))
        return *d;
    throw SqlError("expected numeric value");
}
int integer_double_compare(std::int64_t i, double d) {
    if (!std::isfinite(d))
        throw SqlError("non-finite numeric value");
    constexpr double boundary = 9223372036854775808.0;
    if (d >= boundary)
        return -1;
    if (d < -boundary)
        return 1;
    auto truncated = static_cast<std::int64_t>(d);
    if (i < truncated)
        return -1;
    if (i > truncated)
        return 1;
    const double truncated_double = static_cast<double>(truncated);
    return truncated_double < d ? -1 : truncated_double > d ? 1 : 0;
}
std::int64_t checked_add(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
        throw SqlError("integer overflow");
    return a + b;
}
std::int64_t checked_sub(std::int64_t a, std::int64_t b) {
    if ((b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) ||
        (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b))
        throw SqlError("integer overflow");
    return a - b;
}
std::int64_t checked_mul(std::int64_t a, std::int64_t b) {
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    if (a > 0 ? (b > 0 ? a > hi / b : b < lo / a)
              : (a < 0 ? (b > 0 ? a < lo / b : b < 0 && a < hi / b) : false))
        throw SqlError("integer overflow");
    return a * b;
}
Value arithmetic(const std::string &op, const Value &a, const Value &b) {
    if (!numeric(a) || !numeric(b))
        throw SqlError("arithmetic requires numeric operands");
    if (auto ia = std::get_if<std::int64_t>(&a))
        if (auto ib = std::get_if<std::int64_t>(&b)) {
            if (op == "+")
                return checked_add(*ia, *ib);
            if (op == "-")
                return checked_sub(*ia, *ib);
            if (op == "*")
                return checked_mul(*ia, *ib);
            if (*ib == 0)
                throw SqlError("division by zero");
            if (*ia == std::numeric_limits<std::int64_t>::min() && *ib == -1) {
                if (op == "%")
                    return std::int64_t{0};
                throw SqlError("integer overflow");
            }
            if (op == "/")
                return *ia / *ib;
            if (op == "%")
                return *ia % *ib;
        }
    if (op == "%")
        throw SqlError("remainder requires integer operands");
    auto x = as_double(a), y = as_double(b);
    double result;
    if (op == "+")
        result = x + y;
    else if (op == "-")
        result = x - y;
    else if (op == "*")
        result = x * y;
    else {
        if (y == 0)
            throw SqlError("division by zero");
        result = x / y;
    }
    if (!std::isfinite(result))
        throw SqlError("floating-point overflow");
    return result;
}
bool like(const std::string &text, const std::string &pattern) {
    // Linear-space dynamic programming avoids exponential wildcard backtracking.
    std::vector<bool> previous(text.size() + 1, false), current(text.size() + 1, false);
    previous[0] = true;
    for (char p : pattern) {
        current.assign(text.size() + 1, false);
        current[0] = p == '%' && previous[0];
        for (std::size_t i = 1; i <= text.size(); ++i)
            current[i] = p == '%' ? previous[i] || current[i - 1]
                                  : previous[i - 1] && (p == '_' || p == text[i - 1]);
        previous.swap(current);
    }
    return previous[text.size()];
}
} // namespace
int compare_values(const Value &a, const Value &b) {
    if (is_null(a))
        return is_null(b) ? 0 : -1;
    if (is_null(b))
        return 1;
    if (auto ai = std::get_if<std::int64_t>(&a)) {
        if (auto bi = std::get_if<std::int64_t>(&b))
            return *ai < *bi ? -1 : *ai > *bi ? 1 : 0;
        if (auto bd = std::get_if<double>(&b))
            return integer_double_compare(*ai, *bd);
    }
    if (auto ad = std::get_if<double>(&a)) {
        if (auto bi = std::get_if<std::int64_t>(&b))
            return -integer_double_compare(*bi, *ad);
        if (auto bd = std::get_if<double>(&b)) {
            if (!std::isfinite(*ad) || !std::isfinite(*bd))
                throw SqlError("non-finite numeric value");
            return *ad < *bd ? -1 : *ad > *bd ? 1 : 0;
        }
    }
    if (auto as = std::get_if<std::string>(&a))
        if (auto bs = std::get_if<std::string>(&b))
            return *as < *bs ? -1 : *as > *bs ? 1 : 0;
    if (auto ab = std::get_if<bool>(&a))
        if (auto bb = std::get_if<bool>(&b))
            return *ab == *bb ? 0 : *ab ? 1 : -1;
    throw SqlError("incompatible comparison types");
}
bool is_true(const Value &v) {
    if (is_null(v))
        return false;
    if (auto b = std::get_if<bool>(&v))
        return *b;
    if (auto i = std::get_if<std::int64_t>(&v))
        return *i != 0;
    if (auto d = std::get_if<double>(&v))
        return *d != 0;
    throw SqlError("predicate requires boolean or numeric value");
}
std::string value_string(const Value &v) {
    if (is_null(v))
        return "NULL";
    if (auto s = std::get_if<std::string>(&v))
        return *s;
    if (auto i = std::get_if<std::int64_t>(&v))
        return std::to_string(*i);
    if (auto b = std::get_if<bool>(&v))
        return *b ? "TRUE" : "FALSE";
    std::ostringstream out;
    out << std::setprecision(17) << std::get<double>(v);
    return out.str();
}
Value eval(const ExprPtr &e, const Resolver &resolve) {
    if (!e)
        throw SqlError("missing expression");
    if (e->kind == Expr::Kind::Literal)
        return e->literal;
    if (e->kind == Expr::Kind::Column)
        return resolve(e->name);
    if (e->kind == Expr::Kind::Star)
        throw SqlError("* is only valid in SELECT or COUNT(*)");
    if (e->kind == Expr::Kind::Unary) {
        auto a = eval(e->args.at(0), resolve);
        if (e->name == "IS NULL")
            return is_null(a);
        if (e->name == "IS NOT NULL")
            return !is_null(a);
        if (is_null(a))
            return std::monostate{};
        if (e->name == "NOT")
            return !is_true(a);
        if (!numeric(a))
            throw SqlError("unary arithmetic requires a number");
        if (e->name == "+")
            return a;
        if (auto i = std::get_if<std::int64_t>(&a))
            return checked_sub(0, *i);
        return -std::get<double>(a);
    }
    if (e->kind == Expr::Kind::Binary) {
        const auto &op = e->name;
        auto a = eval(e->args.at(0), resolve);
        if (op == "AND" && !is_null(a) && !is_true(a))
            return false;
        if (op == "OR" && !is_null(a) && is_true(a))
            return true;
        auto b = eval(e->args.at(1), resolve);
        if (op == "AND") {
            if (!is_null(b) && !is_true(b))
                return false;
            return is_null(a) || is_null(b) ? Value{std::monostate{}} : Value{true};
        }
        if (op == "OR") {
            if (!is_null(b) && is_true(b))
                return true;
            return is_null(a) || is_null(b) ? Value{std::monostate{}} : Value{false};
        }
        if (is_null(a) || is_null(b))
            return std::monostate{};
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%")
            return arithmetic(op, a, b);
        if (op == "||" || op == "LIKE") {
            auto x = std::get_if<std::string>(&a), y = std::get_if<std::string>(&b);
            if (!x || !y)
                throw SqlError(op + " requires string operands");
            if (op == "||")
                return *x + *y;
            return like(*x, *y);
        }
        const auto c = compare_values(a, b);
        if (op == "=")
            return c == 0;
        if (op == "!=" || op == "<>")
            return c != 0;
        if (op == "<")
            return c < 0;
        if (op == "<=")
            return c <= 0;
        if (op == ">")
            return c > 0;
        if (op == ">=")
            return c >= 0;
        throw SqlError("unknown operator " + op);
    }
    if (e->kind == Expr::Kind::Function) {
        if (e->name == "COALESCE") {
            if (e->args.empty())
                throw SqlError("COALESCE requires arguments");
            for (const auto &a : e->args) {
                auto v = eval(a, resolve);
                if (!is_null(v))
                    return v;
            }
            return std::monostate{};
        }
        if (e->name == "COUNT" || e->name == "SUM" || e->name == "AVG" || e->name == "MIN" ||
            e->name == "MAX")
            throw SqlError("aggregate requires aggregation executor");
        if (e->name != "ABS" && e->name != "LOWER" && e->name != "UPPER" && e->name != "LENGTH")
            throw SqlError("unknown function " + e->name);
        if (e->args.size() != 1)
            throw SqlError(e->name + " requires one argument");
        auto v = eval(e->args[0], resolve);
        if (is_null(v))
            return v;
        if (e->name == "ABS") {
            if (auto i = std::get_if<std::int64_t>(&v))
                return *i < 0 ? checked_sub(0, *i) : *i;
            return std::abs(as_double(v));
        }
        if (e->name == "LOWER" || e->name == "UPPER" || e->name == "LENGTH") {
            auto s = std::get_if<std::string>(&v);
            if (!s)
                throw SqlError(e->name + " requires a string");
            if (e->name == "LOWER")
                return lower(*s);
            if (e->name == "UPPER")
                return upper(*s);
            return static_cast<std::int64_t>(s->size());
        }
        throw SqlError("unknown function " + e->name);
    }
    throw SqlError("invalid expression kind");
}
bool contains_aggregate(const ExprPtr &e) {
    if (!e)
        return false;
    if (e->kind == Expr::Kind::Function &&
        (e->name == "COUNT" || e->name == "SUM" || e->name == "AVG" || e->name == "MIN" ||
         e->name == "MAX"))
        return true;
    return std::any_of(e->args.begin(), e->args.end(),
                       [](const ExprPtr &a) { return contains_aggregate(a); });
}
std::string expression_string(const ExprPtr &e) {
    if (!e)
        return "";
    if (e->kind == Expr::Kind::Literal) {
        if (auto s = std::get_if<std::string>(&e->literal)) {
            std::string out = "'";
            for (char c : *s) {
                out += c;
                if (c == '\'')
                    out += '\'';
            }
            return out + "'";
        }
        return value_string(e->literal);
    }
    if (e->kind == Expr::Kind::Column)
        return e->name;
    if (e->kind == Expr::Kind::Star)
        return e->name.empty() ? "*" : e->name + ".*";
    if (e->kind == Expr::Kind::Unary) {
        if (e->name == "IS NULL" || e->name == "IS NOT NULL")
            return "(" + expression_string(e->args[0]) + " " + e->name + ")";
        return e->name + " (" + expression_string(e->args[0]) + ")";
    }
    if (e->kind == Expr::Kind::Binary)
        return "(" + expression_string(e->args[0]) + " " + e->name + " " +
               expression_string(e->args[1]) + ")";
    std::string out = e->name + "(";
    for (std::size_t i = 0; i < e->args.size(); ++i) {
        if (i)
            out += ", ";
        out += expression_string(e->args[i]);
    }
    return out + ")";
}
} // namespace minidb
