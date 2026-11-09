#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace minidb {
using Value = std::variant<std::monostate, std::int64_t, double, std::string, bool>;
using Row = std::vector<Value>;
enum class Type { Integer, Double, Varchar };
struct Column {
    std::string name;
    Type type = Type::Integer;
    std::size_t max_length = 0;
    bool not_null = false;
    bool primary_key = false;
};
struct Expr;
using ExprPtr = std::shared_ptr<Expr>;
struct Expr {
    enum class Kind { Literal, Column, Unary, Binary, Function, Star };
    Kind kind = Kind::Literal;
    Value literal;
    std::string name;
    std::vector<ExprPtr> args;
};
struct SelectItem {
    ExprPtr expr;
    std::string alias;
};
struct Join {
    std::string table, alias;
    ExprPtr on;
};
struct OrderBy {
    ExprPtr expr;
    bool descending = false;
};
struct Statement {
    enum class Kind {
        CreateDatabase,
        CreateTable,
        DropTable,
        CreateIndex,
        Insert,
        Update,
        Delete,
        Select,
        Begin,
        Commit,
        Rollback,
        Explain,
        Checkpoint
    };
    Kind kind = Kind::Select;
    std::string name, table, index, column;
    std::vector<Column> columns;
    std::vector<std::string> insert_columns;
    std::vector<std::vector<ExprPtr>> values;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    std::vector<SelectItem> select;
    std::string from, from_alias;
    std::vector<Join> joins;
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    std::vector<OrderBy> order_by;
    std::optional<std::size_t> limit;
    std::size_t offset = 0;
    std::unique_ptr<Statement> explained;
};
struct Token {
    enum class Kind { Identifier, Number, String, Symbol, End };
    Kind kind;
    std::string text;
    std::size_t offset;
    bool quoted = false;
};
class SqlError : public std::runtime_error {
  public:
    explicit SqlError(const std::string &message) : std::runtime_error(message) {}
};
std::vector<Token> lex(std::string_view sql);
Statement parse(std::string_view sql);
using Resolver = std::function<Value(const std::string &)>;
Value eval(const ExprPtr &expression, const Resolver &resolve);
// NULL sorts before non-NULL. Non-NULL numeric values compare across integer/double.
// Incompatible non-NULL types throw SqlError (SQL predicates handle NULL separately).
int compare_values(const Value &left, const Value &right);
bool is_true(const Value &value);
bool is_null(const Value &value);
std::string value_string(const Value &value);
std::string expression_string(const ExprPtr &expression);
bool contains_aggregate(const ExprPtr &expression);
} // namespace minidb
