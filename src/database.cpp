#include "minidb/database.hpp"
#include "minidb/btree.hpp"
#include "minidb/catalog.hpp"
#include "minidb/heap.hpp"
#include "minidb/lock.hpp"
#include "minidb/query.hpp"
#include <algorithm>
#include <atomic>
#include <set>
#include <sstream>

namespace minidb {
namespace {
ResultSet message(std::string text, size_t affected = 0) {
    ResultSet r;
    r.message = std::move(text);
    r.affected_rows = affected;
    return r;
}
TableMetadata &table_at(Catalog &c, const std::string &name) {
    auto it = c.tables.find(name);
    if (it == c.tables.end())
        throw SqlError("unknown table: " + name);
    return it->second;
}
size_t resolve_column(const TableMetadata &t, const std::string &name) {
    for (size_t i = 0; i < t.columns.size(); ++i)
        if (t.columns[i].name == name)
            return i;
    auto dot = name.find('.');
    if (dot == std::string::npos)
        return column_position(t, name);
    if (name.substr(0, dot) != t.name)
        throw SqlError("unknown table qualifier: " + name);
    return column_position(t, name.substr(dot + 1));
}
void validate_expression(const ExprPtr &e, const TableMetadata *t) {
    if (!e)
        return;
    if (contains_aggregate(e))
        throw SqlError("aggregate not allowed in this expression");
    if (e->kind == Expr::Kind::Star)
        throw SqlError("star not allowed in this expression");
    if (e->kind == Expr::Kind::Function) {
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
    if (e->kind == Expr::Kind::Column) {
        if (!t)
            throw SqlError("column not allowed in VALUES: " + e->name);
        resolve_column(*t, e->name);
    }
    for (const auto &arg : e->args)
        validate_expression(arg, t);
}
Value evaluate(const ExprPtr &e, const TableMetadata &t, const Row &row) {
    return eval(e, [&](const std::string &name) { return row.at(resolve_column(t, name)); });
}
void remove_indexes(PageTransaction &tx, TableMetadata &t, const Row &row, RID rid) {
    for (auto &idx : t.indexes) {
        const auto &v = row.at(idx.column);
        if (is_null(v))
            continue;
        BPlusTree tree(tx, idx.root);
        if (!tree.erase(index_key(v), rid))
            throw SqlError("CORRUPTION: missing index entry");
        idx.root = tree.root();
    }
}
void add_indexes(PageTransaction &tx, TableMetadata &t, const Row &row, RID rid) {
    for (auto &idx : t.indexes) {
        const auto &v = row.at(idx.column);
        if (is_null(v))
            continue;
        BPlusTree tree(tx, idx.root);
        auto key = index_key(v);
        if (idx.unique && !tree.equal(key).empty())
            throw SqlError("PRIMARY KEY constraint: " + t.name + "." + t.columns[idx.column].name);
        tree.insert(key, rid);
        idx.root = tree.root();
    }
}
std::string quote_name(const std::string &name) {
    std::string out = "\"";
    for (char ch : name) {
        out += ch;
        if (ch == '\"')
            out += ch;
    }
    return out + "\"";
}
} // namespace
struct Database::Impl {
    Options options;
    Pager pager;
    LockManager locks;
    std::atomic<uint64_t> next_id{1};
    Impl(const std::filesystem::path &path, Options o) : options(o), pager(path, o.buffer_pages) {}
};
struct Connection::Impl {
    std::shared_ptr<Database::Impl> db;
    uint64_t id = 0;
    bool explicit_transaction = false, dirty_catalog = false;
    std::unique_ptr<PageTransaction> tx;
    Catalog catalog;
    explicit Impl(std::shared_ptr<Database::Impl> value) : db(std::move(value)) {}
    ~Impl() {
        abort();
    }
    void start(bool explicit_tx) {
        if (id)
            throw SqlError("transaction already active");
        id = db->next_id.fetch_add(1);
        if (id == 0)
            throw SqlError("transaction ID exhausted");
        explicit_transaction = explicit_tx;
    }
    void acquire(bool write) {
        if (!id)
            start(false);
        if (write)
            db->locks.exclusive(id, db->options.lock_timeout);
        else
            db->locks.shared(id, db->options.lock_timeout);
        if (!tx) {
            tx = db->pager.begin();
            catalog = load_catalog(*tx);
        }
    }
    void abort() noexcept {
        if (tx)
            tx->rollback();
        tx.reset();
        if (id)
            db->locks.release(id);
        id = 0;
        explicit_transaction = false;
        dirty_catalog = false;
        catalog = Catalog{};
    }
    void commit() {
        if (!id)
            throw SqlError("no active transaction");
        if (tx) {
            if (dirty_catalog)
                save_catalog(*tx, catalog);
            tx->commit();
        }
        tx.reset();
        db->locks.release(id);
        id = 0;
        explicit_transaction = false;
        dirty_catalog = false;
        catalog = Catalog{};
    }
    ResultSet apply(const Statement &s);
    ResultSet run(const Statement &s) {
        try {
            if (s.kind == Statement::Kind::Begin) {
                start(true);
                return message("BEGIN");
            }
            if (s.kind == Statement::Kind::Commit) {
                commit();
                return message("COMMIT");
            }
            if (s.kind == Statement::Kind::Rollback) {
                if (!id)
                    throw SqlError("no active transaction");
                abort();
                return message("ROLLBACK");
            }
            if (s.kind == Statement::Kind::Checkpoint) {
                if (id)
                    throw SqlError("CHECKPOINT is not allowed inside a transaction");
                start(false);
                db->locks.exclusive(id, db->options.lock_timeout);
                db->pager.checkpoint();
                commit();
                return message("CHECKPOINT");
            }
            const bool read =
                s.kind == Statement::Kind::Select || s.kind == Statement::Kind::Explain;
            acquire(!read);
            auto result = apply(s);
            if (!read)
                dirty_catalog = true;
            if (!explicit_transaction)
                commit();
            return result;
        } catch (...) {
            abort();
            throw;
        }
    }
};
ResultSet Connection::Impl::apply(const Statement &s) {
    using K = Statement::Kind;
    if (s.kind == K::Select)
        return execute_select(*tx, catalog, s);
    if (s.kind == K::Explain) {
        if (!s.explained || s.explained->kind != K::Select)
            throw SqlError("EXPLAIN supports SELECT only");
        auto result = execute_select(*tx, catalog, *s.explained);
        ResultSet out;
        out.columns = {"plan"};
        out.rows = {{result.plan}};
        out.plan = result.plan;
        return out;
    }
    if (s.kind == K::CreateDatabase) {
        if (!catalog.name.empty())
            throw SqlError("database already named: " + catalog.name);
        catalog.name = s.name;
        return message("CREATE DATABASE");
    }
    if (s.kind == K::CreateTable) {
        if (catalog.tables.contains(s.table))
            throw SqlError("table already exists: " + s.table);
        if (s.columns.empty() || s.columns.size() > 1024)
            throw SqlError("table must have 1..1024 columns");
        std::set<std::string> names;
        size_t primary = 0;
        TableMetadata table;
        table.name = s.table;
        table.columns = s.columns;
        for (auto &col : table.columns) {
            if (col.max_length > UINT32_MAX)
                throw SqlError("column length exceeds on-disk format limit");
            if (!names.insert(col.name).second)
                throw SqlError("duplicate column: " + col.name);
            if (col.primary_key) {
                ++primary;
                col.not_null = true;
            }
        }
        if (primary > 1)
            throw SqlError("only one single-column PRIMARY KEY supported");
        table.heap = HeapFile::create(*tx);
        for (size_t i = 0; i < table.columns.size(); ++i)
            if (table.columns[i].primary_key) {
                IndexMetadata idx;
                idx.name = "__pk_" + table.name;
                idx.column = i;
                idx.unique = true;
                idx.root = BPlusTree::create(*tx);
                table.indexes.push_back(idx);
            }
        catalog.tables.emplace(table.name, std::move(table));
        return message("CREATE TABLE");
    }
    if (s.kind == K::DropTable) {
        auto &table = table_at(catalog, s.table);
        HeapFile(*tx, table.heap).destroy();
        for (const auto &idx : table.indexes)
            BPlusTree(*tx, idx.root).destroy();
        catalog.tables.erase(s.table);
        return message("DROP TABLE");
    }
    if (s.kind == K::CreateIndex) {
        if (table_at(catalog, s.table).indexes.size() >= 1024)
            throw SqlError("at most 1024 indexes per table");
        for (const auto &[name, table] : catalog.tables) {
            (void)name;
            for (const auto &idx : table.indexes)
                if (idx.name == s.index)
                    throw SqlError("index already exists: " + s.index);
        }
        auto &table = table_at(catalog, s.table);
        IndexMetadata idx;
        idx.name = s.index;
        idx.column = column_position(table, s.column);
        idx.root = BPlusTree::create(*tx);
        BPlusTree tree(*tx, idx.root);
        for (const auto &[rid, bytes] : HeapFile(*tx, table.heap).scan()) {
            auto row = decode_row(bytes);
            if (!is_null(row.at(idx.column)))
                tree.insert(index_key(row[idx.column]), rid);
        }
        idx.root = tree.root();
        table.indexes.push_back(idx);
        return message("CREATE INDEX");
    }
    if (s.kind == K::Insert) {
        auto &table = table_at(catalog, s.table);
        std::vector<size_t> columns;
        if (s.insert_columns.empty()) {
            for (size_t i = 0; i < table.columns.size(); ++i)
                columns.push_back(i);
        } else {
            std::set<size_t> seen;
            for (const auto &name : s.insert_columns) {
                auto col = column_position(table, name);
                if (!seen.insert(col).second)
                    throw SqlError("duplicate INSERT column: " + name);
                columns.push_back(col);
            }
        }
        HeapFile heap(*tx, table.heap);
        size_t count = 0;
        for (const auto &expressions : s.values) {
            if (expressions.size() != columns.size())
                throw SqlError("INSERT column/value count mismatch");
            Row row(table.columns.size());
            for (size_t i = 0; i < expressions.size(); ++i) {
                validate_expression(expressions[i], nullptr);
                row[columns[i]] = eval(expressions[i], [](const std::string &n) -> Value {
                    throw SqlError("column not allowed in VALUES: " + n);
                });
            }
            for (size_t i = 0; i < row.size(); ++i)
                row[i] = coerce(std::move(row[i]), table.columns[i]);
            auto rid = heap.insert(encode_row(row));
            add_indexes(*tx, table, row, rid);
            ++count;
        }
        return message("INSERT", count);
    }
    if (s.kind == K::Update || s.kind == K::Delete) {
        auto &table = table_at(catalog, s.table);
        validate_expression(s.where, &table);
        std::vector<size_t> positions;
        std::set<size_t> assigned;
        for (const auto &[name, expr] : s.assignments) {
            auto pos = resolve_column(table, name);
            if (!assigned.insert(pos).second)
                throw SqlError("duplicate UPDATE assignment: " + name);
            validate_expression(expr, &table);
            positions.push_back(pos);
        }
        struct Change {
            RID rid;
            Row before;
            Row after;
        };
        std::vector<Change> changes;
        HeapFile heap(*tx, table.heap);
        for (const auto &[rid, bytes] : heap.scan()) {
            Row row = decode_row(bytes);
            if (s.where && !is_true(evaluate(s.where, table, row)))
                continue;
            Row next = row;
            if (s.kind == K::Update) {
                for (size_t i = 0; i < s.assignments.size(); ++i)
                    next[positions[i]] = evaluate(s.assignments[i].second, table, row);
                for (size_t i = 0; i < next.size(); ++i)
                    next[i] = coerce(std::move(next[i]), table.columns[i]);
            }
            changes.push_back({rid, std::move(row), std::move(next)});
        }
        // Remove every old index key before inserting replacements: uniqueness is
        // checked against the final statement state, and primary-key swaps are legal.
        for (const auto &ch : changes)
            remove_indexes(*tx, table, ch.before, ch.rid);
        for (const auto &ch : changes) {
            if (s.kind == K::Delete)
                heap.erase(ch.rid);
            else {
                auto rid = heap.update(ch.rid, encode_row(ch.after));
                add_indexes(*tx, table, ch.after, rid);
            }
        }
        return message(s.kind == K::Delete ? "DELETE" : "UPDATE", changes.size());
    }
    throw SqlError("unsupported statement");
}
Database::Database(const std::filesystem::path &path, Options options)
    : impl_(std::make_shared<Impl>(path, options)) {}
Database::~Database() = default;
std::unique_ptr<Connection> Database::connect() {
    return std::unique_ptr<Connection>(new Connection(impl_));
}
Connection::Connection(std::shared_ptr<Database::Impl> db)
    : impl_(std::make_unique<Impl>(std::move(db))) {}
Connection::~Connection() = default;
ResultSet Connection::execute(const std::string &sql) {
    try {
        return impl_->run(parse(sql));
    } catch (...) {
        impl_->abort();
        throw;
    }
}
ResultSet Connection::execute(const Statement &statement) {
    return impl_->run(statement);
}
PreparedStatement Connection::prepare(const std::string &sql) {
    try {
        return PreparedStatement(*this, parse(sql));
    } catch (...) {
        impl_->abort();
        throw;
    }
}
bool Connection::in_transaction() const {
    return impl_->id != 0;
}
std::vector<std::string> Connection::tables() {
    try {
        impl_->acquire(false);
        std::vector<std::string> names;
        for (const auto &[name, table] : impl_->catalog.tables) {
            (void)table;
            names.push_back(name);
        }
        if (!impl_->explicit_transaction)
            impl_->commit();
        return names;
    } catch (...) {
        impl_->abort();
        throw;
    }
}
std::string Connection::schema() {
    try {
        impl_->acquire(false);
        std::ostringstream out;
        for (const auto &[name, table] : impl_->catalog.tables) {
            out << "CREATE TABLE " << quote_name(name) << " (";
            for (size_t i = 0; i < table.columns.size(); ++i) {
                if (i)
                    out << ", ";
                const auto &col = table.columns[i];
                out << quote_name(col.name) << ' ';
                if (col.type == Type::Integer)
                    out << "INTEGER";
                else if (col.type == Type::Double)
                    out << "DOUBLE";
                else if (col.max_length == 0)
                    out << "TEXT";
                else
                    out << "VARCHAR(" << col.max_length << ')';
                if (col.primary_key)
                    out << " PRIMARY KEY";
                else if (col.not_null)
                    out << " NOT NULL";
            }
            out << ");\n";
            for (const auto &idx : table.indexes)
                if (!idx.unique)
                    out << "CREATE INDEX " << quote_name(idx.name) << " ON " << quote_name(name)
                        << '(' << quote_name(table.columns[idx.column].name) << ");\n";
        }
        if (!impl_->explicit_transaction)
            impl_->commit();
        return out.str();
    } catch (...) {
        impl_->abort();
        throw;
    }
}
} // namespace minidb
