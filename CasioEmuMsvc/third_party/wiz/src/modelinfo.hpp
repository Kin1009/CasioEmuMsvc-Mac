// modelinfo.hpp - just enough of a CasioEmuMsvc `config.bin` reader to find the
// ROM filename a model folder uses. Format mirrored from the emulator's own
// Binary.h / ModelInfo.h (little-endian; strings are u64-length-prefixed blobs).
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace wiz {

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& d, size_t pos = 0) : d(d), p(pos) {}

    std::vector<uint8_t> take(size_t n) {
        if (p + n > d.size()) throw std::runtime_error("unexpected end of data");
        std::vector<uint8_t> b(d.begin() + (long)p, d.begin() + (long)(p + n));
        p += n;
        return b;
    }
    uint8_t u8() { return take(1)[0]; }
    uint16_t u16() {
        auto b = take(2);
        return (uint16_t)(b[0] | (b[1] << 8));
    }
    int32_t i32() {
        auto b = take(4);
        uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
                     ((uint32_t)b[3] << 24);
        return (int32_t)v;
    }
    uint64_t u64() {
        auto b = take(8);
        uint64_t v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | b[(size_t)i];
        return v;
    }
    std::vector<uint8_t> blobbytes() { return take((size_t)u64()); }
    std::string string() {
        auto b = blobbytes();
        return std::string(b.begin(), b.end());
    }
    bool boolean() { return u8() != 0; }

    size_t p;

private:
    const std::vector<uint8_t>& d;
};

// Returns the ROM filename declared in a model's config.bin, or "rom.bin" if the
// blob cannot be parsed.
inline std::string model_rom_path(const std::vector<uint8_t>& buf) {
    try {
        Reader r(buf);
        r.string();  // version header
        r.u16();
        r.u16();
        r.boolean();
        r.u8();
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            r.i32();
            r.i32();
            r.i32();
            r.i32();
            r.i32();
            r.string();
        }
        n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            r.string();
            for (int k = 0; k < 8; k++) r.i32();
        }
        r.i32();
        r.i32();
        r.i32();
        r.string();  // interface path
        r.string();  // model name
        std::string rom = r.string();
        if (!rom.empty()) return rom;
    } catch (...) {
    }
    return "rom.bin";
}

}  // namespace wiz
