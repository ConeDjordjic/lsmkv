#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#if !defined(__x86_64__)
#error "crc32c uses the sse4.2 crc32 instruction, it needs a port for other cpus"
#endif
#include <nmmintrin.h>

// crc32c (castagnoli)
__attribute__((target("sse4.2"))) inline std::uint32_t crc32c(std::string_view data) {
    const char *p = data.data();
    std::size_t n = data.size();
    std::uint64_t wide = 0xFFFFFFFF;
    for (; n >= 8; p += 8, n -= 8) {
        std::uint64_t word;
        std::memcpy(&word, p, sizeof word);
        wide = _mm_crc32_u64(wide, word);
    }
    auto crc = static_cast<std::uint32_t>(wide);
    for (; n > 0; p++, n--) {
        crc = _mm_crc32_u8(crc, static_cast<unsigned char>(*p));
    }
    return ~crc;
}
