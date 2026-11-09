#include "minidb/heap.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace minidb {
namespace {
constexpr std::size_t data_begin = 40;
constexpr std::uint16_t dead = 0xffff;
std::uint16_t u16(const Page &p, std::size_t o) {
    return std::uint16_t(std::to_integer<unsigned>(p[o]) |
                         (std::to_integer<unsigned>(p[o + 1]) << 8));
}
std::uint32_t u32(const Page &p, std::size_t o) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= std::uint32_t(std::to_integer<unsigned>(p[o + i])) << (8 * i);
    return v;
}
void put16(Page &p, std::size_t o, std::uint16_t v) {
    for (unsigned i = 0; i < 2; ++i)
        p[o + i] = std::byte((v >> (8 * i)) & 255);
}
void put32(Page &p, std::size_t o, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[o + i] = std::byte((v >> (8 * i)) & 255);
}
using Slots = std::vector<std::optional<std::vector<std::byte>>>;
Slots unpack(const Page &p) {
    SlottedPage::validate(p);
    Slots records;
    for (std::uint16_t i = 0; i < SlottedPage::slots(p); ++i) {
        if (SlottedPage::occupied(p, i))
            records.push_back(SlottedPage::get(p, i));
        else
            records.emplace_back();
    }
    return records;
}
bool pack(Page &p, const Slots &records) {
    std::size_t bytes = data_begin + records.size() * 4;
    for (auto &r : records)
        if (r)
            bytes += r->size();
    if (bytes > p.size())
        return false;
    Page result = p;
    std::fill(result.begin() + data_begin, result.end(), std::byte{0});
    std::size_t end = p.size();
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i]) {
            end -= records[i]->size();
            std::copy(records[i]->begin(), records[i]->end(), result.begin() + end);
            put16(result, data_begin + 4 * i, static_cast<std::uint16_t>(end));
            put16(result, data_begin + 4 * i + 2, static_cast<std::uint16_t>(records[i]->size()));
        } else {
            put16(result, data_begin + 4 * i, 0);
            put16(result, data_begin + 4 * i + 2, dead);
        }
    }
    put16(result, 36, static_cast<std::uint16_t>(records.size()));
    put16(result, 38, static_cast<std::uint16_t>(end));
    p = result;
    return true;
}
void check_size(std::size_t size) {
    if (size > 4096 - data_begin - 4)
        throw std::runtime_error("record exceeds single-page limit (4052 bytes)");
}
} // namespace
void SlottedPage::initialize(Page &page, PageId next_page) {
    std::fill(page.begin() + 32, page.end(), std::byte{0});
    set_page_type(page, PageType::Heap);
    put32(page, 32, next_page);
    put16(page, 36, 0);
    put16(page, 38, 4096);
}
PageId SlottedPage::next(const Page &p) {
    return u32(p, 32);
}
void SlottedPage::set_next(Page &p, PageId next_page) {
    put32(p, 32, next_page);
}
std::uint16_t SlottedPage::slots(const Page &p) {
    return u16(p, 36);
}
bool SlottedPage::occupied(const Page &p, std::uint16_t slot) {
    if (slot >= slots(p))
        return false;
    if (data_begin + 4 * std::size_t(slot) + 4 > p.size())
        throw std::runtime_error("corrupt slot directory");
    return u16(p, data_begin + 4 * slot + 2) != dead;
}
void SlottedPage::validate(const Page &p) {
    if (page_type(p) != PageType::Heap)
        throw std::runtime_error("expected heap page");
    auto count = slots(p);
    auto begin = data_begin + std::size_t(count) * 4;
    if (begin > p.size() || u16(p, 38) < begin || u16(p, 38) > p.size())
        throw std::runtime_error("corrupt heap header");
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (std::size_t i = 0; i < count; ++i) {
        auto len = u16(p, data_begin + 4 * i + 2);
        if (len == dead)
            continue;
        auto pos = u16(p, data_begin + 4 * i);
        if (pos < u16(p, 38) || std::size_t(pos) + len > p.size())
            throw std::runtime_error("corrupt heap record bounds");
        if (len)
            ranges.emplace_back(pos, std::size_t(pos) + len);
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second)
            throw std::runtime_error("overlapping heap records");
}
std::vector<std::byte> SlottedPage::get(const Page &p, std::uint16_t slot) {
    validate(p);
    if (!occupied(p, slot))
        throw std::runtime_error("record not found");
    auto pos = u16(p, data_begin + 4 * slot), len = u16(p, data_begin + 4 * slot + 2);
    return {p.begin() + pos, p.begin() + pos + len};
}
bool SlottedPage::insert(Page &p, const std::vector<std::byte> &record, std::uint16_t &slot) {
    check_size(record.size());
    auto records = unpack(p);
    auto found = std::find(records.begin(), records.end(), std::nullopt);
    auto index = static_cast<std::size_t>(found - records.begin());
    if (found == records.end())
        records.push_back(record);
    else
        *found = record;
    if (!pack(p, records))
        return false;
    slot = static_cast<std::uint16_t>(index);
    return true;
}
void SlottedPage::erase(Page &p, std::uint16_t slot) {
    auto records = unpack(p);
    if (slot >= records.size() || !records[slot])
        throw std::runtime_error("record not found");
    records[slot].reset();
    // Deleted trailing directory entries have no live RID to preserve. Trim
    // them so a page of many tiny deleted records can accept a large record.
    while (!records.empty() && !records.back())
        records.pop_back();
    pack(p, records);
}
bool SlottedPage::update(Page &p, std::uint16_t slot, const std::vector<std::byte> &record) {
    check_size(record.size());
    auto records = unpack(p);
    if (slot >= records.size() || !records[slot])
        throw std::runtime_error("record not found");
    records[slot] = record;
    return pack(p, records);
}
PageId HeapFile::create(PageTransaction &tx) {
    auto id = tx.allocate(PageType::Heap);
    auto p = tx.read(id);
    SlottedPage::initialize(p);
    tx.write(id, p);
    return id;
}
RID HeapFile::insert(const std::vector<std::byte> &record) {
    check_size(record.size());
    // Bulk inserts usually continue on the most recently used page. A failed
    // hint falls back to a full free-space search so deleted space is reused.
    if (insert_hint_ != first_ && insert_hint_ != kInvalidPage) {
        auto p = tx_.read(insert_hint_);
        std::uint16_t slot;
        if (SlottedPage::insert(p, record, slot)) {
            tx_.write(insert_hint_, p);
            return {insert_hint_, slot};
        }
    }
    auto id = first_;
    std::unordered_set<PageId> seen;
    while (id != kInvalidPage) {
        if (!seen.insert(id).second)
            throw std::runtime_error("cycle in heap chain");
        auto p = tx_.read(id);
        std::uint16_t slot;
        if (SlottedPage::insert(p, record, slot)) {
            tx_.write(id, p);
            insert_hint_ = id;
            return {id, slot};
        }
        auto next = SlottedPage::next(p);
        if (next == kInvalidPage) {
            auto added = create(tx_);
            SlottedPage::set_next(p, added);
            tx_.write(id, p);
            id = added;
        } else
            id = next;
    }
    throw std::runtime_error("invalid heap root");
}
std::vector<std::byte> HeapFile::get(RID rid) {
    return SlottedPage::get(tx_.read(rid.page), rid.slot);
}
void HeapFile::erase(RID rid) {
    auto p = tx_.read(rid.page);
    SlottedPage::erase(p, rid.slot);
    tx_.write(rid.page, p);
}
RID HeapFile::update(RID rid, const std::vector<std::byte> &record) {
    auto p = tx_.read(rid.page);
    if (SlottedPage::update(p, rid.slot, record)) {
        tx_.write(rid.page, p);
        return rid;
    }
    auto replacement = insert(record);
    erase(rid);
    return replacement;
}
std::vector<std::pair<RID, std::vector<std::byte>>> HeapFile::scan() {
    std::vector<std::pair<RID, std::vector<std::byte>>> result;
    std::unordered_set<PageId> seen;
    for (auto id = first_; id != kInvalidPage;) {
        if (!seen.insert(id).second)
            throw std::runtime_error("cycle in heap chain");
        auto p = tx_.read(id);
        SlottedPage::validate(p);
        for (std::uint16_t s = 0; s < SlottedPage::slots(p); ++s)
            if (SlottedPage::occupied(p, s))
                result.emplace_back(RID{id, s}, SlottedPage::get(p, s));
        id = SlottedPage::next(p);
    }
    return result;
}
void HeapFile::destroy() {
    std::unordered_set<PageId> seen;
    for (auto id = first_; id != kInvalidPage;) {
        if (!seen.insert(id).second)
            throw std::runtime_error("cycle in heap chain");
        auto p = tx_.read(id);
        SlottedPage::validate(p);
        auto next = SlottedPage::next(p);
        tx_.free(id);
        id = next;
    }
    first_ = kInvalidPage;
    insert_hint_ = kInvalidPage;
}
} // namespace minidb
