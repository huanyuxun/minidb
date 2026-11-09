#include "minidb/btree.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace minidb {
namespace {
constexpr std::size_t header = 40;
struct Entry {
    std::string key;
    RID rid;
    auto operator<=>(const Entry &) const = default;
};
struct Node {
    bool leaf{true};
    PageId next{kInvalidPage};
    std::vector<Entry> entries;
    std::vector<PageId> children;
};
std::uint16_t get16(const Page &p, std::size_t o) {
    return std::uint16_t(std::to_integer<unsigned>(p[o]) | std::to_integer<unsigned>(p[o + 1])
                                                               << 8);
}
std::uint32_t get32(const Page &p, std::size_t o) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= std::uint32_t(std::to_integer<unsigned>(p[o + i])) << (i * 8);
    return v;
}
void put16(Page &p, std::size_t o, std::uint16_t v) {
    for (unsigned i = 0; i < 2; ++i)
        p[o + i] = std::byte((v >> (i * 8)) & 255);
}
void put32(Page &p, std::size_t o, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[o + i] = std::byte((v >> (i * 8)) & 255);
}
std::size_t bytes(const Node &n) {
    std::size_t size = header;
    for (auto &e : n.entries)
        size += 8 + e.key.size() + (n.leaf ? 0 : 4);
    return size;
}
Node load(PageTransaction &tx, PageId id) {
    auto p = tx.read(id);
    Node n;
    auto type = page_type(p);
    if (type != PageType::BTreeLeaf && type != PageType::BTreeInternal)
        throw std::runtime_error("expected B+ tree page");
    n.leaf = type == PageType::BTreeLeaf;
    if (n.leaf)
        n.next = get32(p, 32);
    else
        n.children.push_back(get32(p, 32));
    auto count = get16(p, 36);
    std::size_t pos = header;
    if (count > (4096 - header) / 8)
        throw std::runtime_error("corrupt B+ tree count");
    for (unsigned i = 0; i < count; ++i) {
        if (pos + 2 > p.size())
            throw std::runtime_error("truncated B+ tree entry");
        auto len = get16(p, pos);
        pos += 2;
        if (len > BPlusTree::max_key_size || pos + len + 6 + (n.leaf ? 0 : 4) > p.size())
            throw std::runtime_error("corrupt B+ tree key length");
        std::string key;
        key.reserve(len);
        for (unsigned j = 0; j < len; ++j)
            key.push_back(char(std::to_integer<unsigned>(p[pos++])));
        auto page = get32(p, pos);
        pos += 4;
        auto slot = get16(p, pos);
        pos += 2;
        n.entries.push_back({std::move(key), {page, slot}});
        if (!n.leaf) {
            n.children.push_back(get32(p, pos));
            pos += 4;
        }
    }
    if (!std::is_sorted(n.entries.begin(), n.entries.end()) ||
        std::adjacent_find(n.entries.begin(), n.entries.end()) != n.entries.end())
        throw std::runtime_error("unordered B+ tree keys");
    return n;
}
void save(PageTransaction &tx, PageId id, const Node &n) {
    if (bytes(n) > 4096 || (!n.leaf && n.children.size() != n.entries.size() + 1))
        throw std::runtime_error("invalid B+ tree node size");
    auto p = tx.read(id);
    std::fill(p.begin() + 32, p.end(), std::byte{0});
    set_page_type(p, n.leaf ? PageType::BTreeLeaf : PageType::BTreeInternal);
    put32(p, 32, n.leaf ? n.next : n.children.at(0));
    put16(p, 36, static_cast<std::uint16_t>(n.entries.size()));
    std::size_t pos = header;
    for (std::size_t i = 0; i < n.entries.size(); ++i) {
        auto &e = n.entries[i];
        put16(p, pos, static_cast<std::uint16_t>(e.key.size()));
        pos += 2;
        for (unsigned char c : e.key)
            p[pos++] = std::byte(c);
        put32(p, pos, e.rid.page);
        pos += 4;
        put16(p, pos, e.rid.slot);
        pos += 2;
        if (!n.leaf) {
            put32(p, pos, n.children[i + 1]);
            pos += 4;
        }
    }
    tx.write(id, p);
}
Entry minimum(PageTransaction &tx, PageId id) {
    for (unsigned depth = 0; depth < 64; ++depth) {
        auto n = load(tx, id);
        if (n.leaf) {
            if (n.entries.empty())
                throw std::runtime_error("empty non-root B+ tree leaf");
            return n.entries.front();
        }
        id = n.children.at(0);
    }
    throw std::runtime_error("B+ tree depth exceeds safety limit");
}
void separators(PageTransaction &tx, Node &n) {
    if (n.leaf)
        return;
    n.entries.clear();
    for (std::size_t i = 1; i < n.children.size(); ++i)
        n.entries.push_back(minimum(tx, n.children[i]));
}
std::size_t child_index(const Node &n, const Entry &e) {
    return static_cast<std::size_t>(std::upper_bound(n.entries.begin(), n.entries.end(), e) -
                                    n.entries.begin());
}
// Pick a split by encoded byte size, not key count. This handles long and short
// variable keys sharing a node while guaranteeing that both siblings fit.
std::pair<Node, Node> partition(PageTransaction &tx, const Node &n) {
    std::optional<std::pair<Node, Node>> best;
    std::size_t score = std::numeric_limits<std::size_t>::max();
    auto count = n.leaf ? n.entries.size() : n.children.size();
    for (std::size_t cut = 1; cut < count; ++cut) {
        Node a, b;
        a.leaf = b.leaf = n.leaf;
        if (n.leaf) {
            a.entries.assign(n.entries.begin(), n.entries.begin() + cut);
            b.entries.assign(n.entries.begin() + cut, n.entries.end());
            b.next = n.next;
        } else {
            a.children.assign(n.children.begin(), n.children.begin() + cut);
            b.children.assign(n.children.begin() + cut, n.children.end());
            // Separators are already known; the boundary separator is promoted.
            a.entries.assign(n.entries.begin(), n.entries.begin() + cut - 1);
            b.entries.assign(n.entries.begin() + cut, n.entries.end());
        }
        auto as = bytes(a), bs = bytes(b);
        if (as > 4096 || bs > 4096)
            continue;
        auto imbalance = as > bs ? as - bs : bs - as;
        if (imbalance < score) {
            score = imbalance;
            best = std::make_pair(std::move(a), std::move(b));
        }
    }
    (void)tx;
    if (!best)
        throw std::runtime_error("B+ tree node cannot be split");
    return std::move(*best);
}
std::optional<PageId> insert_node(PageTransaction &tx, PageId id, const Entry &e,
                                  unsigned depth = 0) {
    if (depth > 64)
        throw std::runtime_error("B+ tree depth exceeds safety limit");
    auto n = load(tx, id);
    if (n.leaf) {
        auto it = std::lower_bound(n.entries.begin(), n.entries.end(), e);
        if (it != n.entries.end() && *it == e)
            throw std::runtime_error("duplicate B+ tree key/RID");
        n.entries.insert(it, e);
    } else {
        auto index = child_index(n, e);
        auto right = insert_node(tx, n.children[index], e, depth + 1);
        if (right)
            n.children.insert(n.children.begin() + index + 1, *right);
        separators(tx, n);
    }
    if (bytes(n) <= 4096) {
        save(tx, id, n);
        return {};
    }
    auto [a, b] = partition(tx, n);
    auto right = tx.allocate(n.leaf ? PageType::BTreeLeaf : PageType::BTreeInternal);
    if (n.leaf)
        a.next = right;
    save(tx, id, a);
    save(tx, right, b);
    return right;
}
Node combined(PageTransaction &tx, const Node &left, const Node &right) {
    if (left.leaf != right.leaf)
        throw std::runtime_error("B+ tree sibling type mismatch");
    Node result = left;
    if (result.leaf) {
        result.entries.insert(result.entries.end(), right.entries.begin(), right.entries.end());
        result.next = right.next;
    } else {
        result.children.insert(result.children.end(), right.children.begin(), right.children.end());
        separators(tx, result);
    }
    return result;
}
void rebalance(PageTransaction &tx, Node &parent, std::size_t index) {
    if (parent.children.size() < 2)
        return;
    auto child = load(tx, parent.children[index]);
    if (bytes(child) >= 4096 / 3 && (!child.leaf || !child.entries.empty()))
        return;
    auto li = index == 0 ? 0 : index - 1;
    auto ri = li + 1;
    auto left = load(tx, parent.children[li]), right = load(tx, parent.children[ri]);
    auto merged = combined(tx, left, right);
    if (bytes(merged) <= 4096) {
        save(tx, parent.children[li], merged);
        tx.free(parent.children[ri]);
        parent.children.erase(parent.children.begin() + ri);
    } else {
        auto [a, b] = partition(tx, merged);
        if (a.leaf)
            a.next = parent.children[ri];
        save(tx, parent.children[li], a);
        save(tx, parent.children[ri], b);
    }
}
struct EraseResult {
    bool found;
    std::optional<PageId> right;
};
EraseResult erase_node(PageTransaction &tx, PageId id, const Entry &e, unsigned depth = 0) {
    if (depth > 64)
        throw std::runtime_error("B+ tree depth exceeds safety limit");
    auto n = load(tx, id);
    if (n.leaf) {
        auto it = std::lower_bound(n.entries.begin(), n.entries.end(), e);
        if (it == n.entries.end() || *it != e)
            return {false, {}};
        n.entries.erase(it);
        save(tx, id, n);
        return {true, {}};
    }
    auto index = child_index(n, e);
    auto removed = erase_node(tx, n.children[index], e, depth + 1);
    if (!removed.found)
        return {false, {}};
    if (removed.right)
        n.children.insert(n.children.begin() + index + 1, *removed.right);
    else
        rebalance(tx, n, index);
    separators(tx, n);
    // Deleting a short minimum can replace a separator with a longer key.
    // Such a parent may overflow even though the number of entries decreased.
    if (bytes(n) > 4096) {
        auto [a, b] = partition(tx, n);
        auto right = tx.allocate(PageType::BTreeInternal);
        save(tx, id, a);
        save(tx, right, b);
        return {true, right};
    }
    save(tx, id, n);
    return {true, {}};
}
PageId find_leaf(PageTransaction &tx, PageId id, std::optional<Entry> e) {
    for (unsigned depth = 0; depth < 64; ++depth) {
        auto n = load(tx, id);
        if (n.leaf)
            return id;
        id = n.children[e ? child_index(n, *e) : 0];
    }
    throw std::runtime_error("B+ tree depth exceeds safety limit");
}
} // namespace
PageId BPlusTree::create(PageTransaction &tx) {
    auto id = tx.allocate(PageType::BTreeLeaf);
    save(tx, id, Node{});
    return id;
}
void BPlusTree::insert(const std::string &key, RID rid) {
    if (key.size() > max_key_size)
        throw std::runtime_error("index key exceeds 512 bytes");
    if (rid.page == kInvalidPage)
        throw std::runtime_error("invalid RID");
    auto right = insert_node(tx_, root_, {key, rid});
    if (right) {
        Node root;
        root.leaf = false;
        root.children = {root_, *right};
        separators(tx_, root);
        auto id = tx_.allocate(PageType::BTreeInternal);
        save(tx_, id, root);
        root_ = id;
    }
}
bool BPlusTree::erase(const std::string &key, RID rid) {
    auto removed = erase_node(tx_, root_, {key, rid});
    if (!removed.found)
        return false;
    if (removed.right) {
        Node parent;
        parent.leaf = false;
        parent.children = {root_, *removed.right};
        separators(tx_, parent);
        auto id = tx_.allocate(PageType::BTreeInternal);
        save(tx_, id, parent);
        root_ = id;
    }
    auto root = load(tx_, root_);
    while (!root.leaf && root.children.size() == 1) {
        auto old = root_;
        root_ = root.children[0];
        tx_.free(old);
        root = load(tx_, root_);
    }
    return true;
}
std::vector<RID> BPlusTree::equal(const std::string &key) {
    return range(key, key);
}
std::vector<RID> BPlusTree::range(std::optional<std::string> low, std::optional<std::string> high,
                                  bool low_inclusive, bool high_inclusive) {
    std::vector<RID> result;
    if (low && high && *low > *high)
        return result;
    auto id = find_leaf(tx_, root_, low ? std::optional<Entry>(Entry{*low, {0, 0}}) : std::nullopt);
    std::unordered_set<PageId> seen;
    while (id != kInvalidPage) {
        if (!seen.insert(id).second)
            throw std::runtime_error("cycle in B+ tree leaf chain");
        auto n = load(tx_, id);
        if (!n.leaf)
            throw std::runtime_error("non-leaf in B+ tree leaf chain");
        for (auto &e : n.entries) {
            if (low && (e.key < *low || (!low_inclusive && e.key == *low)))
                continue;
            if (high && (e.key > *high || (!high_inclusive && e.key == *high)))
                return result;
            result.push_back(e.rid);
        }
        id = n.next;
    }
    return result;
}
void BPlusTree::validate() {
    std::unordered_set<PageId> seen;
    std::vector<PageId> leaves;
    std::optional<unsigned> leaf_depth;
    std::function<std::pair<Entry, Entry>(PageId, unsigned)> visit =
        [&](PageId id, unsigned depth) -> std::pair<Entry, Entry> {
        if (depth > 64 || !seen.insert(id).second)
            throw std::runtime_error("cycle in B+ tree");
        auto n = load(tx_, id);
        if (n.leaf) {
            if (leaf_depth && *leaf_depth != depth)
                throw std::runtime_error("unbalanced B+ tree leaf depths");
            leaf_depth = depth;
            leaves.push_back(id);
            if (n.entries.empty()) {
                if (id != root_)
                    throw std::runtime_error("empty nonroot leaf");
                return {};
            }
            return {n.entries.front(), n.entries.back()};
        }
        if (n.children.empty())
            throw std::runtime_error("empty B+ tree internal page");
        auto bounds = visit(n.children[0], depth + 1);
        auto max = bounds.second;
        for (std::size_t i = 1; i < n.children.size(); ++i) {
            auto b = visit(n.children[i], depth + 1);
            if (b.first != n.entries[i - 1] || !(max < b.first))
                throw std::runtime_error("invalid B+ tree separator");
            max = b.second;
        }
        return {bounds.first, max};
    };
    visit(root_, 0);
    for (std::size_t i = 0; i < leaves.size(); ++i)
        if (load(tx_, leaves[i]).next != (i + 1 < leaves.size() ? leaves[i + 1] : kInvalidPage))
            throw std::runtime_error("broken B+ tree leaf link");
}
void BPlusTree::destroy() {
    std::unordered_set<PageId> seen;
    std::function<void(PageId, unsigned)> visit = [&](PageId id, unsigned depth) {
        if (depth > 64 || !seen.insert(id).second)
            throw std::runtime_error("cycle in B+ tree");
        auto n = load(tx_, id);
        if (!n.leaf)
            for (auto child : n.children)
                visit(child, depth + 1);
        tx_.free(id);
    };
    visit(root_, 0);
    root_ = kInvalidPage;
}
} // namespace minidb
