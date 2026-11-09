#include "minidb/query.hpp"
#include "minidb/btree.hpp"
#include "minidb/executor.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace minidb {
namespace {
struct BoundColumn {
    std::string name, qualifier;
};
using Bindings = std::vector<BoundColumn>;
size_t resolve_column(const Bindings &cols, const std::string &name) {
    size_t found = cols.size();
    for (size_t i = 0; i < cols.size(); ++i)
        if (name == cols[i].name || name == cols[i].qualifier + "." + cols[i].name) {
            if (found != cols.size())
                throw SqlError("ambiguous column: " + name);
            found = i;
        }
    if (found == cols.size())
        throw SqlError("unknown column: " + name);
    return found;
}
Value evaluate(const ExprPtr &e, const Row &row, const Bindings &cols) {
    return eval(e, [&](const std::string &name) { return row.at(resolve_column(cols, name)); });
}
bool aggregate(const ExprPtr &e) {
    return e && e->kind == Expr::Kind::Function &&
           (e->name == "COUNT" || e->name == "SUM" || e->name == "AVG" || e->name == "MIN" ||
            e->name == "MAX");
}
void bind_expression(const ExprPtr &e, const Bindings &cols, bool allow_agg = true,
                     bool inside_agg = false) {
    if (!e)
        return;
    if (e->kind == Expr::Kind::Column)
        resolve_column(cols, e->name);
    if (e->kind == Expr::Kind::Function && !aggregate(e)) {
        if (e->name == "COALESCE") {
            if (e->args.empty())
                throw SqlError("COALESCE requires arguments");
        } else if (e->name == "ABS" || e->name == "LOWER" || e->name == "UPPER" ||
                   e->name == "LENGTH") {
            if (e->args.size() != 1)
                throw SqlError(e->name + " requires one argument");
        } else
            throw SqlError("unknown function: " + e->name);
    }
    if (aggregate(e)) {
        if (!allow_agg || inside_agg)
            throw SqlError("aggregate not allowed here");
        if (e->args.size() != 1)
            throw SqlError("aggregate requires one argument");
        if (e->args[0]->kind == Expr::Kind::Star) {
            if (e->name != "COUNT" || !e->args[0]->name.empty())
                throw SqlError("only COUNT(*) supports star");
            return;
        }
        inside_agg = true;
    }
    if (e->kind == Expr::Kind::Star)
        throw SqlError("unexpected star expression");
    for (const auto &a : e->args)
        bind_expression(a, cols, allow_agg, inside_agg);
}
ExprPtr literal(Value v) {
    auto e = std::make_shared<Expr>();
    e->literal = std::move(v);
    return e;
}
ExprPtr column_expr(std::string name) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::Kind::Column;
    e->name = std::move(name);
    return e;
}
bool same_group_expr(const ExprPtr &a, const ExprPtr &b, const Bindings &cols) {
    if (a->kind == Expr::Kind::Column && b->kind == Expr::Kind::Column)
        return resolve_column(cols, a->name) == resolve_column(cols, b->name);
    return expression_string(a) == expression_string(b);
}
void grouped_check(const ExprPtr &e, const std::vector<ExprPtr> &groups, const Bindings &cols) {
    if (aggregate(e))
        return;
    for (const auto &g : groups)
        if (same_group_expr(e, g, cols))
            return;
    if (e->kind == Expr::Kind::Column)
        throw SqlError("column must occur in GROUP BY or aggregate: " + e->name);
    for (const auto &a : e->args)
        grouped_check(a, groups, cols);
}
Value aggregate_value(const ExprPtr &e, const std::vector<Row> &rows, const Bindings &cols) {
    int64_t count = 0;
    Value value;
    bool star = e->args[0]->kind == Expr::Kind::Star;
    for (const auto &row : rows) {
        Value v = star ? Value{int64_t(1)} : evaluate(e->args[0], row, cols);
        if (is_null(v))
            continue;
        ++count;
        if (e->name == "COUNT")
            continue;
        if (e->name == "MIN" || e->name == "MAX") {
            if (is_null(value) ||
                (e->name == "MIN" ? compare_values(v, value) < 0 : compare_values(v, value) > 0))
                value = v;
        } else {
            if (!std::holds_alternative<int64_t>(v) && !std::holds_alternative<double>(v))
                throw SqlError("SUM/AVG requires numeric input");
            if (e->name == "AVG" && std::holds_alternative<int64_t>(v))
                v = double(std::get<int64_t>(v));
            if (is_null(value))
                value = v;
            else {
                auto add = std::make_shared<Expr>();
                add->kind = Expr::Kind::Binary;
                add->name = "+";
                add->args = {literal(value), literal(v)};
                value = eval(add, {});
            }
        }
    }
    if (e->name == "COUNT")
        return count;
    if (e->name == "AVG" && count)
        return std::get<double>(value) / double(count);
    return value;
}
ExprPtr fold_aggregates(const ExprPtr &e, const std::vector<Row> &rows, const Bindings &cols) {
    if (aggregate(e))
        return literal(aggregate_value(e, rows, cols));
    auto copy = std::make_shared<Expr>(*e);
    copy->args.clear();
    for (const auto &a : e->args)
        copy->args.push_back(fold_aggregates(a, rows, cols));
    return copy;
}
TableMetadata &table(Catalog &catalog, const std::string &name) {
    auto it = catalog.tables.find(name);
    if (it == catalog.tables.end())
        throw SqlError("unknown table: " + name);
    return it->second;
}
Bindings bindings(const TableMetadata &t, const std::string &alias) {
    Bindings cols;
    for (const auto &c : t.columns)
        cols.push_back({c.name, alias.empty() ? t.name : alias});
    return cols;
}
std::vector<Row> scan(PageTransaction &tx, TableMetadata &t, const Bindings &cols,
                      const ExprPtr &condition, std::string &plan) {
    HeapFile heap(tx, t.heap);
    std::vector<Row> rows;
    std::vector<ExprPtr> terms;
    std::function<void(const ExprPtr &)> flatten = [&](const ExprPtr &e) {
        if (!e)
            return;
        if (e->kind == Expr::Kind::Binary && e->name == "AND") {
            flatten(e->args[0]);
            flatten(e->args[1]);
        } else
            terms.push_back(e);
    };
    flatten(condition);
    for (auto &idx : t.indexes) {
        std::optional<std::string> low, high;
        bool li = true, hi = true, used = false;
        for (const auto &term : terms) {
            if (term->kind != Expr::Kind::Binary)
                continue;
            std::string op = term->name;
            if (op != "=" && op != "<" && op != "<=" && op != ">" && op != ">=")
                continue;
            auto col = term->args[0], val = term->args[1];
            if (col->kind != Expr::Kind::Column) {
                std::swap(col, val);
                if (op == "<")
                    op = ">";
                else if (op == ">")
                    op = "<";
                else if (op == "<=")
                    op = ">=";
                else if (op == ">=")
                    op = "<=";
            }
            if (col->kind != Expr::Kind::Column || val->kind != Expr::Kind::Literal ||
                is_null(val->literal))
                continue;
            size_t position;
            try {
                position = resolve_column(cols, col->name);
            } catch (const SqlError &) {
                continue;
            }
            if (position != idx.column)
                continue;
            Value v = val->literal;
            const auto &schema = t.columns[position];
            if (schema.type == Type::Integer && !std::holds_alternative<int64_t>(v))
                continue;
            if (schema.type == Type::Double) {
                if (auto i = std::get_if<int64_t>(&v)) {
                    if (*i > 9007199254740992LL || *i < -9007199254740992LL)
                        continue;
                    v = double(*i);
                }
                if (!std::holds_alternative<double>(v))
                    continue;
            }
            if (schema.type == Type::Varchar && !std::holds_alternative<std::string>(v))
                continue;
            auto key = index_key(v);
            used = true;
            if (op == "=" || op == ">" || op == ">=") {
                bool incl = op != ">";
                if (!low || key > *low) {
                    low = key;
                    li = incl;
                } else if (key == *low)
                    li = li && incl;
            }
            if (op == "=" || op == "<" || op == "<=") {
                bool incl = op != "<";
                if (!high || key < *high) {
                    high = key;
                    hi = incl;
                } else if (key == *high)
                    hi = hi && incl;
            }
        }
        if (used) {
            plan = "IndexScan(" + t.name + ", " + idx.name + ")";
            if (low && high && (*low > *high || (*low == *high && (!li || !hi))))
                return rows;
            for (auto rid : BPlusTree(tx, idx.root).range(low, high, li, hi))
                rows.push_back(decode_row(heap.get(rid)));
            return rows;
        }
    }
    plan = "SeqScan(" + t.name + ")";
    for (auto &[rid, bytes] : heap.scan()) {
        (void)rid;
        rows.push_back(decode_row(bytes));
    }
    return rows;
}
struct RowLess {
    bool operator()(const Row &a, const Row &b) const {
        for (size_t i = 0; i < a.size(); ++i) {
            int c = compare_values(a[i], b[i]);
            if (c)
                return c < 0;
        }
        return false;
    }
};
} // namespace
ResultSet execute_select(PageTransaction &tx, Catalog &catalog, const Statement &s) {
    ResultSet result;
    Bindings cols;
    ExecutorPtr input;
    std::string plan;
    if (s.from.empty()) {
        input = rows_executor({Row{}});
        plan = "ConstantRow";
    } else {
        auto &t = table(catalog, s.from);
        cols = bindings(t, s.from_alias);
        input = rows_executor(scan(tx, t, cols, s.joins.empty() ? s.where : ExprPtr{}, plan));
    }
    std::set<std::string> qualifiers;
    if (!s.from.empty())
        qualifiers.insert(s.from_alias.empty() ? s.from : s.from_alias);
    for (const auto &j : s.joins) {
        auto &t = table(catalog, j.table);
        auto name = j.alias.empty() ? j.table : j.alias;
        if (!qualifiers.insert(name).second)
            throw SqlError("duplicate table alias: " + name);
        auto rightcols = bindings(t, j.alias);
        cols.insert(cols.end(), rightcols.begin(), rightcols.end());
        bind_expression(j.on, cols, false);
        std::string rightplan;
        auto right = scan(tx, t, rightcols, {}, rightplan);
        auto joincols = cols;
        auto on = j.on;
        input = join_executor(std::move(input), std::move(right), [joincols, on](const Row &row) {
            return is_true(evaluate(on, row, joincols));
        });
        plan = "NestedLoopJoin(" + plan + ", " + rightplan + ")";
    }
    bind_expression(s.where, cols, false);
    if (s.where) {
        input = filter_executor(std::move(input), [cols, where = s.where](const Row &row) {
            return is_true(evaluate(where, row, cols));
        });
        plan = "Filter(" + plan + ")";
    }
    std::vector<SelectItem> items;
    for (const auto &item : s.select) {
        if (item.expr->kind != Expr::Kind::Star) {
            items.push_back(item);
            continue;
        }
        bool found = false;
        for (const auto &c : cols)
            if (item.expr->name.empty() || item.expr->name == c.qualifier) {
                items.push_back({column_expr(c.qualifier + "." + c.name), c.name});
                found = true;
            }
        if (!found)
            throw SqlError("star has no matching table");
    }
    std::map<std::string, ExprPtr> aliases;
    std::set<std::string> ambiguous_aliases;
    for (const auto &item : items) {
        bind_expression(item.expr, cols);
        result.columns.push_back(item.alias.empty() ? expression_string(item.expr) : item.alias);
        if (!item.alias.empty()) {
            if (aliases.contains(item.alias))
                ambiguous_aliases.insert(item.alias);
            aliases[item.alias] = item.expr;
        }
    }
    auto order = s.order_by;
    for (auto &o : order) {
        if (o.expr->kind == Expr::Kind::Column && ambiguous_aliases.contains(o.expr->name))
            throw SqlError("ambiguous output alias: " + o.expr->name);
        if (o.expr->kind == Expr::Kind::Column && aliases.contains(o.expr->name))
            o.expr = aliases.at(o.expr->name);
        if (o.expr->kind == Expr::Kind::Literal &&
            std::holds_alternative<int64_t>(o.expr->literal)) {
            auto n = std::get<int64_t>(o.expr->literal);
            if (n < 1 || uint64_t(n) > items.size())
                throw SqlError("ORDER BY position out of range");
            o.expr = items[size_t(n) - 1].expr;
        }
        bind_expression(o.expr, cols);
    }
    bool agg = !s.group_by.empty();
    for (const auto &i : items)
        agg = agg || contains_aggregate(i.expr);
    for (const auto &o : order)
        agg = agg || contains_aggregate(o.expr);
    for (const auto &g : s.group_by)
        bind_expression(g, cols, false);
    // Append hidden ORDER BY values to each projected row, then sort and strip.
    std::vector<Row> projected;
    if (agg) {
        for (const auto &i : items)
            grouped_check(i.expr, s.group_by, cols);
        for (const auto &o : order)
            grouped_check(o.expr, s.group_by, cols);
        std::map<Row, std::vector<Row>, RowLess> groups;
        auto raw = collect(std::move(input));
        if (s.group_by.empty())
            groups[Row{}] = {};
        for (auto &row : raw) {
            Row key;
            for (const auto &g : s.group_by)
                key.push_back(evaluate(g, row, cols));
            groups[std::move(key)].push_back(std::move(row));
        }
        for (const auto &[key, rows] : groups) {
            (void)key;
            Row empty(cols.size()), out;
            const Row &representative = rows.empty() ? empty : rows.front();
            for (const auto &item : items)
                out.push_back(
                    evaluate(fold_aggregates(item.expr, rows, cols), representative, cols));
            for (const auto &o : order)
                out.push_back(evaluate(fold_aggregates(o.expr, rows, cols), representative, cols));
            projected.push_back(std::move(out));
        }
        input = rows_executor(std::move(projected));
        plan = "Aggregation(" + plan + ")";
    } else
        input = projection_executor(std::move(input), [cols, items, order](const Row &row) {
            Row out;
            for (const auto &i : items)
                out.push_back(evaluate(i.expr, row, cols));
            for (const auto &o : order)
                out.push_back(evaluate(o.expr, row, cols));
            return out;
        });
    if (!order.empty()) {
        auto offset = items.size();
        input = sort_executor(std::move(input), [order, offset](const Row &a, const Row &b) {
            for (size_t i = 0; i < order.size(); ++i) {
                int c = compare_values(a[offset + i], b[offset + i]);
                if (c)
                    return order[i].descending ? c > 0 : c < 0;
            }
            return false;
        });
        plan = "Sort(" + plan + ")";
    }
    input = limit_executor(std::move(input), s.offset, s.limit);
    if (s.limit || s.offset)
        plan = "Limit(" + plan + ")";
    result.rows = collect(std::move(input));
    for (auto &row : result.rows)
        row.resize(items.size());
    result.plan = "Projection(" + plan + ")";
    return result;
}
} // namespace minidb
