#include "minidb/query.hpp"
#include "minidb/btree.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace minidb;
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("minidb_query_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    try {
        int checks = 0;
        auto check = [&](bool ok) { ++checks; if (!ok) throw std::runtime_error("query assertion " + std::to_string(checks)); };
        {
            Pager pager(path, 2);
            auto tx = pager.begin();
            Catalog catalog;
            TableMetadata t;
            t.name = "items";
            t.columns = {{"id", Type::Integer}, {"g", Type::Integer}, {"text", Type::Varchar, 32}};
            t.heap = HeapFile::create(*tx);
            auto root = BPlusTree::create(*tx);
            HeapFile heap(*tx, t.heap);
            BPlusTree tree(*tx, root);
            for (int64_t i = 0; i < 120; ++i) {
                Row row{i, i % 5, std::string("value")};
                tree.insert(index_key(i), heap.insert(encode_row(row)));
            }
            t.indexes.push_back({"pk", 0, tree.root(), true});
            catalog.tables.emplace(t.name, t);
            save_catalog(*tx, catalog);
            tx->commit();
        }
        {
            Pager pager(path, 2);
            auto tx = pager.begin();
            auto catalog = load_catalog(*tx);
            auto query = [&](const std::string& sql) { return execute_select(*tx, catalog, parse(sql)); };
            auto r = query("SELECT id FROM items WHERE id>=20 AND id<30 ORDER BY id DESC LIMIT 3 OFFSET 2");
            check(r.rows.size() == 3 && std::get<int64_t>(r.rows[0][0]) == 27);
            check(r.plan.find("IndexScan") != std::string::npos);
            r = query("SELECT g,COUNT(*),SUM(id),AVG(id) FROM items GROUP BY g ORDER BY g");
            check(r.rows.size() == 5 && std::get<int64_t>(r.rows[0][1]) == 24);
            check(std::get<int64_t>(r.rows[0][2]) == 1380 && std::get<double>(r.rows[0][3]) == 57.5);
            r = query("SELECT * FROM items a JOIN items b ON a.id=b.id WHERE a.id=1 ORDER BY a.id");
            check(r.rows.size() == 1 && r.rows[0].size() == 6);
            r = query("SELECT COUNT(*),SUM(id) FROM items WHERE id<0");
            check(std::get<int64_t>(r.rows[0][0]) == 0 && is_null(r.rows[0][1]));
            check(query("SELECT id FROM items WHERE id>5 AND id<4").rows.empty());
            check(query("SELECT COUNT(*)+1 FROM items").rows[0][0] == Value{int64_t(121)});
            check(query("SELECT 6*7 AS answer").rows[0][0] == Value{int64_t(42)});
            for (const auto& sql : {"SELECT missing FROM items WHERE id<0", "SELECT id,COUNT(*) FROM items", "SELECT SUM(COUNT(*)) FROM items", "SELECT weird(id) FROM items WHERE id<0", "SELECT * FROM items a JOIN items b ON a.id=b.id ORDER BY id"}) {
                bool threw = false; try { query(sql); } catch(const SqlError&) { threw = true; }
                check(threw);
            }
        }
        std::filesystem::remove(path);
        std::filesystem::remove(path.string()+".wal");
        std::cout << "PASS query " << checks << " persistent query planning checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL query: " << e.what() << '\n';
        return 1;
    }
}
