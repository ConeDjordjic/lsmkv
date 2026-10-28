#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

[[noreturn]] inline void fail(const char *what) {
    std::println(stderr, "{}: {}", what, std::strerror(errno));
    std::exit(1);
}

inline void append_u32(std::string &buf, std::uint32_t x) {
    buf.append(reinterpret_cast<const char *>(&x), sizeof x);
}

inline void append_u64(std::string &buf, std::uint64_t x) {
    buf.append(reinterpret_cast<const char *>(&x), sizeof x);
}

inline std::uint32_t read_u32(const char *p) {
    std::uint32_t x;
    std::memcpy(&x, p, sizeof x);
    return x;
}

inline std::uint64_t read_u64(const char *p) {
    std::uint64_t x;
    std::memcpy(&x, p, sizeof x);
    return x;
}

inline void write_all(int fd, std::string_view buf) {
    while (!buf.empty()) {
        ssize_t n = ::write(fd, buf.data(), buf.size());
        if (n == -1) {
            fail("write");
        }
        buf.remove_prefix(static_cast<std::size_t>(n));
    }
}

inline std::string pread_exact(int fd, std::size_t size, std::uint64_t offset) {
    std::string buf(size, '\0');
    std::size_t done = 0;
    while (done < size) {
        ssize_t n = ::pread(fd, buf.data() + done, size - done, static_cast<off_t>(offset + done));
        if (n == -1) {
            fail("pread");
        }
        if (n == 0) {
            std::println(stderr, "pread: unexpected end of file");
            std::exit(1);
        }
        done += static_cast<std::size_t>(n);
    }
    return buf;
}
