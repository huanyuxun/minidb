#pragma once
#include "minidb/storage.hpp"
#include <compare>
#include <cstdint>
#include <utility>
#include <vector>

namespace minidb {
struct RID {
    PageId page{kInvalidPage};
    std::uint16_t slot{};
    auto operator<=>(const RID &) const = default;
};

// Slot numbers remain stable when records are compacted within a page.
class SlottedPage {
  public:
    static void initialize(Page &page, PageId next = kInvalidPage);
    static PageId next(const Page &page);
    static void set_next(Page &page, PageId next);
    static std::uint16_t slots(const Page &page);
    static bool occupied(const Page &page, std::uint16_t slot);
    static std::vector<std::byte> get(const Page &page, std::uint16_t slot);
    static bool insert(Page &page, const std::vector<std::byte> &record, std::uint16_t &slot);
    static void erase(Page &page, std::uint16_t slot);
    static bool update(Page &page, std::uint16_t slot, const std::vector<std::byte> &record);
    static void validate(const Page &page);
};

class HeapFile {
  public:
    static PageId create(PageTransaction &tx);
    HeapFile(PageTransaction &tx, PageId first) : tx_(tx), first_(first), insert_hint_(first) {}
    RID insert(const std::vector<std::byte> &record);
    std::vector<std::byte> get(RID rid);
    void erase(RID rid);
    RID update(RID rid, const std::vector<std::byte> &record);
    std::vector<std::pair<RID, std::vector<std::byte>>> scan();
    void destroy();
    PageId first() const {
        return first_;
    }

  private:
    PageTransaction &tx_;
    PageId first_;
    PageId insert_hint_;
};
} // namespace minidb
