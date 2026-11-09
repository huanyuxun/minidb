#pragma once
#include "minidb/sql.hpp"
#include "minidb/storage.hpp"
#include <map>
namespace minidb {
struct IndexMetadata {
    std::string name;
    size_t column = 0;
    PageId root = kInvalidPage;
    bool unique = false;
};
struct TableMetadata {
    std::string name;
    std::vector<Column> columns;
    PageId heap = kInvalidPage;
    std::vector<IndexMetadata> indexes;
};
struct Catalog {
    std::string name;
    std::map<std::string, TableMetadata> tables;
};
Catalog load_catalog(PageTransaction &tx);
void save_catalog(PageTransaction &tx, const Catalog &catalog);
std::vector<std::byte> encode_row(const Row &row);
Row decode_row(const std::vector<std::byte> &bytes);
Value coerce(Value value, const Column &column);
std::string index_key(const Value &value);
size_t column_position(const TableMetadata &table, const std::string &name);
} // namespace minidb
