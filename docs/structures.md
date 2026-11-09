# Heap and B+ tree structures

All integer fields are explicitly encoded little endian. The first 32 bytes of
every page belong to the pager (type, identity, LSN, checksum). No native C++
object layout, pointer, padding, or host endianness is persisted.

## Heap pages

Offset 32 contains a uint32 next-page ID; 36 a uint16 slot count; 38 a uint16
record-area boundary. The slot directory starts at 40. Each slot is a uint16
record offset and uint16 record length. Length 65535 means a deleted slot;
zero-length records are valid. Records grow from the end of the page downward.
Compaction preserves all live slot IDs, reclaims dead record bytes, and reuses
deleted slots. Updates that no longer fit allocate a replacement RID and remove
the old record; the database layer updates every affected index in the same
page transaction. A record can occupy at most 4052 bytes. Overflow-record pages
are not implemented. Insertion searches existing pages for reusable space.

## B+ tree pages

Leaf and internal pages have distinct page types. Offset 32 is the next leaf ID
for a leaf, or the leftmost child ID for an internal node. Offset 36 is a uint16
entry count; offsets 38–39 are reserved. Entries begin at offset 40.

Each entry is uint16 key length, binary key bytes, uint32 RID page, uint16 RID
slot. An internal entry additionally stores a uint32 right-child ID. Keys are
ordered by binary string order and then RID, permitting duplicates distributed
across leaf pages. An internal separator equals the minimum composite key in
its right subtree. Keys are limited to 512 bytes. SQL is responsible for an
order-preserving encoding of supported SQL values.

Insertion descends through separators, splits nodes by encoded byte size,
propagates splits to the parent, and creates a new root when necessary. Deletion
merges a small node with an adjacent sibling if the combined encoding fits,
otherwise redistributes their contents by byte size. Separators are refreshed
up the path and a one-child root collapses. Deletion can also split a parent
when replacing a short separator with a longer key overflows its byte capacity.
Leaf links are maintained by both split and merge operations. Range scans use
tree descent for their lower bound and traverse the leaf chain.

The caller must persist `root()` after modifications because splits and root
collapse can change it. All modifications use the same PageTransaction as the
heap and catalog, making table/index changes atomic. BPlusTree instances are
transaction-local; database locks protect concurrent structural operations.
`validate()` checks separator bounds, page types, ordering, cycles, and leaf
links. This is a diagnostic validation, not an automatic repair mechanism.
