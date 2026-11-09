#pragma once
#include "minidb/heap.hpp"
#include <optional>
#include <string>
#include <vector>

namespace minidb {
// Keys are binary strings ordered lexicographically. SQL encodes values so that
// this byte order is their logical order. RID breaks ties for nonunique indexes.
class BPlusTree {
  public:
    static constexpr std::size_t max_key_size = 512;
    static PageId create(PageTransaction &tx);
    BPlusTree(PageTransaction &tx, PageId root) : tx_(tx), root_(root) {}
    PageId root() const {
        return root_;
    }
    void insert(const std::string &key, RID rid);
    bool erase(const std::string &key, RID rid);
    std::vector<RID> equal(const std::string &key);
    std::vector<RID> range(std::optional<std::string> low = {},
                           std::optional<std::string> high = {}, bool low_inclusive = true,
                           bool high_inclusive = true);
    void validate();
    void destroy();

  private:
    PageTransaction &tx_;
    PageId root_;
};
} // namespace minidb
