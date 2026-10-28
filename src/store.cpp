#include "store.hpp"
#include "crc32.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <print>
#include <unistd.h>

namespace {

constexpr std::uint8_t type_put = 1;
constexpr std::uint8_t type_delete = 2;

// crc u32 + type u8 + key_len u32 + value_len u32
constexpr std::size_t header_size = 13;

[[noreturn]] void fail(const char *what) {
    std::println(stderr, "{}: {}", what, std::strerror(errno));
    std::exit(1);
}

void append_u32(std::string &buf, std::uint32_t x) {
    buf.append(reinterpret_cast<const char *>(&x), sizeof x);
}

std::uint32_t read_u32(const char *p) {
    std::uint32_t x;
    std::memcpy(&x, p, sizeof x);
    return x;
}

void write_record(int fd, std::uint8_t type, std::string_view key, std::string_view value) {
    constexpr std::size_t max_len = std::numeric_limits<std::uint32_t>::max();
    if (key.size() > max_len || value.size() > max_len) {
        std::println(stderr, "write: key or value over 4 GiB");
        std::exit(1);
    }

    std::string buf;
    buf.reserve(header_size + key.size() + value.size());
    buf.append(4, '\0'); // crc placeholder, filled in below
    buf.push_back(static_cast<char>(type));
    append_u32(buf, static_cast<std::uint32_t>(key.size()));
    append_u32(buf, static_cast<std::uint32_t>(value.size()));
    buf.append(key);
    buf.append(value);

    std::uint32_t crc = crc32(std::string_view(buf).substr(4));
    std::memcpy(buf.data(), &crc, sizeof crc);

    ssize_t written = ::write(fd, buf.data(), buf.size());
    if (written == -1) {
        fail("write");
    }
    // on a regular file a short write means the disk is full
    if (static_cast<std::size_t>(written) != buf.size()) {
        std::println(stderr, "write: short write, disk full?");
        std::exit(1);
    }
}

} // namespace

Store::Store(const char *path) : fd_(::open(path, O_RDWR | O_CREAT | O_APPEND, 0644)) {
    if (fd_ == -1) {
        fail("open");
    }

    std::string log;
    char chunk[64 * 1024];
    for (;;) {
        ssize_t n = ::read(fd_, chunk, sizeof chunk);
        if (n == -1) {
            fail("read");
        }
        if (n == 0) {
            break;
        }
        log.append(chunk, static_cast<std::size_t>(n));
    }

    // TODO: corruption mid-log drops everything after it
    std::size_t offset = 0;
    while (offset < log.size()) {
        std::string_view rest = std::string_view(log).substr(offset);
        if (rest.size() < header_size) {
            break;
        }

        std::uint32_t crc = read_u32(rest.data());
        auto type = static_cast<std::uint8_t>(rest[4]);
        std::uint32_t key_len = read_u32(rest.data() + 5);
        std::uint32_t value_len = read_u32(rest.data() + 9);

        // summing the lengths could overflow on 32-bit
        std::size_t space = rest.size() - header_size;
        if (key_len > space || value_len > space - key_len) {
            break;
        }
        std::size_t record_size = header_size + key_len + value_len;
        if (crc32(rest.substr(4, record_size - 4)) != crc) {
            break;
        }

        std::string_view key = rest.substr(header_size, key_len);
        std::string_view value = rest.substr(header_size + key_len, value_len);
        if (type == type_put) {
            memtable_.insert_or_assign(std::string(key), std::string(value));
        } else if (type == type_delete) {
            if (auto it = memtable_.find(key); it != memtable_.end()) {
                memtable_.erase(it);
            }
        } else {
            // the crc checks out, so this is a bug. don't truncate
            std::println(stderr, "replay: unknown record type {} at offset {}", type, offset);
            std::exit(1);
        }
        offset += record_size;
    }

    if (offset < log.size() && ::ftruncate(fd_, static_cast<off_t>(offset)) == -1) {
        fail("ftruncate");
    }
}

Store::~Store() { ::close(fd_); }

void Store::put(std::string_view key, std::string_view value) {
    write_record(fd_, type_put, key, value);
    memtable_.insert_or_assign(std::string(key), std::string(value));
}

void Store::remove(std::string_view key) {
    write_record(fd_, type_delete, key, {});
    // TODO: keep a tombstone instead once there are SSTables
    if (auto it = memtable_.find(key); it != memtable_.end()) {
        memtable_.erase(it);
    }
}

std::optional<std::string> Store::get(std::string_view key) const {
    auto it = memtable_.find(key);
    if (it == memtable_.end()) {
        return std::nullopt;
    }
    return it->second;
}
