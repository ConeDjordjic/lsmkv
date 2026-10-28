#pragma once
#include <cstdint>
#include <string_view>

inline std::uint32_t crc32(std::string_view data) {
    std::uint32_t crc = 0xFFFFFFFF;
    for (char c : data) {
        crc ^= static_cast<unsigned char>(c);
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
