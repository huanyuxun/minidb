#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace minidb::codec {
struct Writer {
    std::vector<std::byte> data;
    void u8(uint8_t v) {
        data.push_back(std::byte(v));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i)
            u8(uint8_t(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i)
            u8(uint8_t(v >> (8 * i)));
    }
    void str(const std::string &s) {
        if (s.size() > UINT32_MAX)
            throw std::runtime_error("string too large");
        u32(uint32_t(s.size()));
        for (unsigned char c : s)
            u8(c);
    }
};
struct Reader {
    std::span<const std::byte> data;
    size_t pos = 0;
    uint8_t u8() {
        if (pos >= data.size())
            throw std::runtime_error("CORRUPTION: truncated encoding");
        return std::to_integer<uint8_t>(data[pos++]);
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= uint32_t(u8()) << (i * 8);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= uint64_t(u8()) << (i * 8);
        return v;
    }
    std::string str() {
        auto n = u32();
        if (n > data.size() - pos)
            throw std::runtime_error("CORRUPTION: invalid string length");
        std::string s;
        s.reserve(n);
        while (n--)
            s.push_back(char(u8()));
        return s;
    }
};
inline void put32(std::span<std::byte> p, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        p[off + i] = std::byte(uint8_t(v >> (i * 8)));
}
inline uint32_t get32(std::span<const std::byte> p, size_t off) {
    Reader r{p.subspan(off, 4)};
    return r.u32();
}
} // namespace minidb::codec
