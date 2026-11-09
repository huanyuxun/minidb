#include "minidb/btree.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>

using namespace minidb;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
std::string key(int value) {
    auto s = std::to_string(value);
    return std::string(8 - s.size(), '0') + s;
}
std::vector<std::byte> record(std::size_t n, unsigned value) {
    return std::vector<std::byte>(n, std::byte(value));
}
void remove_db(const std::filesystem::path &path) {
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".wal");
}
void slotted() {
    Page p{};
    SlottedPage::initialize(p);
    std::uint16_t a, b, c;
    check(SlottedPage::insert(p, record(100, 1), a), "slot insert");
    check(SlottedPage::insert(p, record(300, 2), b), "slot insert 2");
    SlottedPage::erase(p, a);
    check(SlottedPage::insert(p, record(500, 3), c) && c == a, "deleted slot not reused");
    check(SlottedPage::get(p, b) == record(300, 2), "compaction changed RID");
    check(SlottedPage::update(p, b, record(2500, 4)), "variable update");
    check(!SlottedPage::update(p, c, record(2500, 5)), "overflow update accepted");
    check(SlottedPage::get(p, c) == record(500, 3), "failed update mutated page");
    SlottedPage::validate(p);
    Page tiny{};
    SlottedPage::initialize(tiny);
    std::uint16_t tiny_slot;
    for (unsigned i = 0; i < 500; ++i)
        check(SlottedPage::insert(tiny, {}, tiny_slot), "empty record insert");
    for (unsigned i = 0; i < 500; ++i)
        SlottedPage::erase(tiny, static_cast<std::uint16_t>(i));
    check(SlottedPage::slots(tiny) == 0 && SlottedPage::insert(tiny, record(4052, 7), tiny_slot),
          "slot directory space reclamation");
    p[38] = std::byte{0};
    p[39] = std::byte{0};
    bool caught = false;
    try {
        SlottedPage::validate(p);
    } catch (...) {
        caught = true;
    }
    check(caught, "corrupt header not detected");
}
void heap(const std::filesystem::path &path) {
    remove_db(path);
    PageId first;
    std::vector<RID> ids;
    {
        Pager pager(path, 3);
        auto tx = pager.begin();
        first = HeapFile::create(*tx);
        HeapFile h(*tx, first);
        for (unsigned i = 0; i < 240; ++i)
            ids.push_back(h.insert(record(40 + i % 170, i % 255)));
        for (unsigned i = 0; i < ids.size(); i += 3)
            h.erase(ids[i]);
        for (unsigned i = 1; i < ids.size(); i += 3)
            ids[i] = h.update(ids[i], record(3000, i % 255));
        check(h.scan().size() == 160, "heap row count");
        for (unsigned i = 1; i < ids.size(); i += 3)
            check(h.get(ids[i]) == record(3000, i % 255), "heap relocation");
        tx->commit();
    }
    {
        Pager pager(path, 2);
        auto tx = pager.begin();
        HeapFile h(*tx, first);
        check(h.scan().size() == 160, "heap restart");
        h.erase(ids[1]);
        tx->rollback();
        auto tx2 = pager.begin();
        HeapFile h2(*tx2, first);
        check(h2.get(ids[1]) == record(3000, 1), "heap rollback");
        h2.destroy();
        tx2->commit();
    }
    remove_db(path);
}
void tree(const std::filesystem::path &path) {
    remove_db(path);
    PageId root;
    constexpr int count = 1800;
    std::vector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(1949);
    std::shuffle(order.begin(), order.end(), rng);
    auto longkey = [](int i) { return key(i) + std::string((i % 7) * 70, 'x'); };
    {
        Pager pager(path, 4);
        auto tx = pager.begin();
        root = BPlusTree::create(*tx);
        BPlusTree tree(*tx, root);
        for (int i : order)
            tree.insert(longkey(i),
                        RID{static_cast<PageId>(i + 10), static_cast<std::uint16_t>(i % 65535)});
        for (unsigned i = 0; i < 550; ++i)
            tree.insert("duplicate", RID{i + 5000, 0});
        tree.validate();
        root = tree.root();
        check(tree.range().size() == count + 550, "tree insert size");
        check(tree.equal("duplicate").size() == 550, "duplicates across leaves");
        check(tree.equal(longkey(777)) == std::vector<RID>{{787, 777}}, "tree point lookup");
        check(tree.range(longkey(100), longkey(200), false, false).size() == 99,
              "range boundaries");
        tx->commit();
    }
    {
        Pager pager(path, 2);
        auto tx = pager.begin();
        BPlusTree tree(*tx, root);
        tree.validate();
        check(tree.range().size() == count + 550, "tree restart");
        for (int i : order) {
            check(tree.erase(longkey(i), RID{static_cast<PageId>(i + 10),
                                             static_cast<std::uint16_t>(i % 65535)}),
                  "tree erase missing");
            if (i % 80 == 0)
                tree.validate();
        }
        for (unsigned i = 0; i < 550; ++i)
            check(tree.erase("duplicate", RID{i + 5000, 0}), "duplicate delete");
        tree.validate();
        check(tree.range().empty(), "tree should be empty");
        check(page_type(tx->read(tree.root())) == PageType::BTreeLeaf, "root did not collapse");
        root = tree.root();
        tx->commit();
    }
    {
        Pager pager(path, 3);
        auto tx = pager.begin();
        BPlusTree tree(*tx, root);
        tree.validate();
        check(tree.range().empty(), "empty tree restart");
        tree.insert("reused", {99, 1});
        tree.validate();
        tree.destroy();
        tx->commit();
    }
    remove_db(path);
}
void mixed_tree(const std::filesystem::path &path) {
    remove_db(path);
    std::mt19937 rng(811);
    std::set<int> model;
    auto mixedkey = [](int i) { return key(i) + std::string((i * 177) % 505, '\0'); };
    {
        Pager pager(path, 3);
        auto tx = pager.begin();
        auto root = BPlusTree::create(*tx);
        BPlusTree tree(*tx, root);
        for (unsigned step = 0; step < 2200; ++step) {
            auto i = static_cast<int>(rng() % 600);
            RID rid{static_cast<PageId>(i + 1), 0};
            if (rng() % 2 == 0) {
                if (model.insert(i).second)
                    tree.insert(mixedkey(i), rid);
            } else
                check(tree.erase(mixedkey(i), rid) == (model.erase(i) > 0),
                      "mixed deletion result");
            if (step % 100 == 0) {
                tree.validate();
                auto all = tree.range();
                check(all.size() == model.size(), "mixed count");
                std::size_t pos = 0;
                for (int entry : model)
                    check(all[pos++] == RID{static_cast<PageId>(entry + 1), 0}, "mixed order");
            }
        }
        tree.validate();
        tx->rollback();
    }
    remove_db(path);
}
} // namespace
int main() {
    try {
        auto temp = std::filesystem::temp_directory_path() / "minidb_structures_test";
        slotted();
        heap(temp.string() + "_heap.db");
        tree(temp.string() + "_tree.db");
        mixed_tree(temp.string() + "_mixed.db");
        std::cout << "PASS slotted page, heap relocation/reuse/restart/rollback, B+ tree "
                     "multi-level splits/duplicates/ranges/merge/root collapse/restart\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
