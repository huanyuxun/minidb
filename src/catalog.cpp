#include "minidb/catalog.hpp"
#include "minidb/codec.hpp"
#include <algorithm>
#include <cmath>
#include <set>
namespace minidb {
namespace {
constexpr uint32_t magic = 0x31415443;
std::vector<PageId> chain(PageTransaction &tx) {
    std::vector<PageId> out;
    std::set<PageId> seen;
    PageId id = 0;
    while (id != kInvalidPage) {
        if (!seen.insert(id).second)
            throw SqlError("CORRUPTION: catalog cycle");
        out.push_back(id);
        auto p = tx.read(id);
        if (page_type(p) != PageType::Catalog)
            throw SqlError("CORRUPTION: catalog page type");
        if (codec::get32(p, 32) == 0)
            break;
        if (codec::get32(p, 32) != magic)
            throw SqlError("CORRUPTION: catalog signature");
        id = codec::get32(p, 36);
    }
    return out;
}
} // namespace
Catalog load_catalog(PageTransaction &tx) {
    Catalog c;
    std::vector<std::byte> bytes;
    for (auto id : chain(tx)) {
        auto p = tx.read(id);
        if (codec::get32(p, 32) == 0)
            continue;
        auto n = codec::get32(p, 40);
        if (n > 4052)
            throw SqlError("CORRUPTION: catalog length");
        bytes.insert(bytes.end(), p.begin() + 44, p.begin() + 44 + n);
    }
    if (bytes.empty())
        return c;
    codec::Reader r{bytes};
    if (r.u32() != 1)
        throw SqlError("unsupported catalog version");
    c.name = r.str();
    auto n = r.u32();
    if (n > bytes.size() / 12)
        throw SqlError("CORRUPTION: table count");
    while (n--) {
        TableMetadata t;
        t.name = r.str();
        t.heap = r.u32();
        auto nc = r.u32();
        if (nc > 1024)
            throw SqlError("CORRUPTION: columns");
        while (nc--) {
            Column col;
            col.name = r.str();
            auto type = r.u8();
            if (type > 2)
                throw SqlError("CORRUPTION: column type");
            col.type = Type(type);
            col.max_length = r.u32();
            col.not_null = r.u8() != 0;
            col.primary_key = r.u8() != 0;
            t.columns.push_back(col);
        }
        auto ni = r.u32();
        if (ni > 1024)
            throw SqlError("CORRUPTION: indexes");
        while (ni--) {
            IndexMetadata idx;
            idx.name = r.str();
            idx.column = r.u32();
            idx.root = r.u32();
            idx.unique = r.u8() != 0;
            if (idx.column >= t.columns.size())
                throw SqlError("CORRUPTION: index column");
            t.indexes.push_back(idx);
        }
        if (!c.tables.emplace(t.name, t).second)
            throw SqlError("CORRUPTION: duplicate table");
    }
    if (r.pos != bytes.size())
        throw SqlError("CORRUPTION: catalog trailing data");
    return c;
}
void save_catalog(PageTransaction &tx, const Catalog &c) {
    codec::Writer w;
    w.u32(1);
    w.str(c.name);
    w.u32(uint32_t(c.tables.size()));
    for (const auto &[name, t] : c.tables) {
        w.str(name);
        w.u32(t.heap);
        w.u32(uint32_t(t.columns.size()));
        for (const auto &col : t.columns) {
            w.str(col.name);
            w.u8(uint8_t(col.type));
            w.u32(uint32_t(col.max_length));
            w.u8(col.not_null);
            w.u8(col.primary_key);
        }
        w.u32(uint32_t(t.indexes.size()));
        for (const auto &idx : t.indexes) {
            w.str(idx.name);
            w.u32(uint32_t(idx.column));
            w.u32(idx.root);
            w.u8(idx.unique);
        }
    }
    auto ids = chain(tx);
    auto needed = (w.data.size() + 4051) / 4052;
    while (ids.size() < needed)
        ids.push_back(tx.allocate(PageType::Catalog));
    for (size_t i = needed; i < ids.size(); ++i)
        tx.free(ids[i]);
    ids.resize(needed);
    for (size_t i = 0; i < ids.size(); ++i) {
        auto p = tx.read(ids[i]);
        std::fill(p.begin() + 32, p.end(), std::byte{});
        codec::put32(p, 32, magic);
        codec::put32(p, 36, i + 1 < ids.size() ? ids[i + 1] : kInvalidPage);
        auto n = std::min(size_t(4052), w.data.size() - i * 4052);
        codec::put32(p, 40, uint32_t(n));
        std::copy_n(w.data.begin() + i * 4052, n, p.begin() + 44);
        tx.write(ids[i], p);
    }
}
std::vector<std::byte> encode_row(const Row &row) {
    codec::Writer w;
    w.u32(uint32_t(row.size()));
    for (const auto &v : row) {
        w.u8(uint8_t(v.index()));
        if (auto integer = std::get_if<int64_t>(&v))
            w.u64(std::bit_cast<uint64_t>(*integer));
        else if (auto number = std::get_if<double>(&v))
            w.u64(std::bit_cast<uint64_t>(*number));
        else if (auto text = std::get_if<std::string>(&v))
            w.str(*text);
        else if (auto boolean = std::get_if<bool>(&v))
            w.u8(*boolean);
    }
    return w.data;
}
Row decode_row(const std::vector<std::byte> &bytes) {
    codec::Reader r{bytes};
    auto n = r.u32();
    if (n > bytes.size())
        throw SqlError("CORRUPTION: tuple columns");
    Row row;
    while (n--) {
        switch (r.u8()) {
        case 0:
            row.emplace_back(std::monostate{});
            break;
        case 1:
            row.emplace_back(std::bit_cast<int64_t>(r.u64()));
            break;
        case 2: {
            auto d = std::bit_cast<double>(r.u64());
            if (!std::isfinite(d))
                throw SqlError("CORRUPTION: nonfinite double");
            row.emplace_back(d);
            break;
        }
        case 3:
            row.emplace_back(r.str());
            break;
        case 4:
            row.emplace_back(r.u8() != 0);
            break;
        default:
            throw SqlError("CORRUPTION: tuple type");
        }
    }
    if (r.pos != bytes.size())
        throw SqlError("CORRUPTION: tuple trailing data");
    return row;
}
Value coerce(Value v, const Column &c) {
    if (is_null(v)) {
        if (c.not_null || c.primary_key)
            throw SqlError("NOT NULL constraint: " + c.name);
        return v;
    }
    switch (c.type) {
    case Type::Integer:
        if (!std::holds_alternative<int64_t>(v))
            throw SqlError("INTEGER required: " + c.name);
        break;
    case Type::Double:
        if (auto i = std::get_if<int64_t>(&v))
            v = double(*i);
        if (!std::holds_alternative<double>(v) || !std::isfinite(std::get<double>(v)))
            throw SqlError("finite DOUBLE required: " + c.name);
        break;
    case Type::Varchar:
        if (!std::holds_alternative<std::string>(v))
            throw SqlError("VARCHAR required: " + c.name);
        if (c.max_length && std::get<std::string>(v).size() > c.max_length)
            throw SqlError("VARCHAR length exceeded: " + c.name);
        break;
    }
    return v;
}
std::string index_key(const Value &v) {
    std::string key;
    uint64_t bits = 0;
    if (auto i = std::get_if<int64_t>(&v))
        bits = std::bit_cast<uint64_t>(*i) ^ (uint64_t(1) << 63);
    else if (auto d = std::get_if<double>(&v)) {
        double normalized = *d == 0 ? 0 : *d;
        bits = std::bit_cast<uint64_t>(normalized);
        bits = (bits >> 63) ? ~bits : bits ^ (uint64_t(1) << 63);
    } else if (auto s = std::get_if<std::string>(&v))
        return *s;
    else
        throw SqlError("unsupported index key");
    for (int i = 7; i >= 0; --i)
        key.push_back(char(uint8_t(bits >> (i * 8))));
    return key;
}
size_t column_position(const TableMetadata &table, const std::string &name) {
    for (size_t i = 0; i < table.columns.size(); ++i)
        if (table.columns[i].name == name)
            return i;
    throw SqlError("unknown column: " + name);
}
} // namespace minidb
