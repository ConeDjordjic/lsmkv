#include "store.hpp"
#include "crc32.hpp"
#include "io.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <print>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace {

constexpr std::uint8_t type_put = 1;
constexpr std::uint8_t type_delete = 2;

// crc u32 + type u8 + key_len u32 + value_len u32
constexpr std::size_t header_size = 13;

constexpr std::size_t memtable_limit = 4 * 1024 * 1024;

void write_record(int fd, std::uint8_t type, std::string_view key, std::string_view value) {
    // 0xFFFFFFFF is the tombstone marker in tables
    constexpr std::size_t max_len = std::numeric_limits<std::uint32_t>::max();
    if (key.size() > max_len || value.size() >= max_len) {
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

    write_all(fd, buf);
}

void sync_dir(const std::string &dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd == -1) {
        fail("open dir");
    }
    if (::fsync(fd) == -1) {
        fail("fsync dir");
    }
    ::close(fd);
}

} // namespace

Store::Store(const char *dir) : dir_(dir) {
    if (::mkdir(dir, 0755) == -1 && errno != EEXIST) {
        fail("mkdir");
    }
    load_tables();

    wal_fd_ = ::open((dir_ + "/wal.log").c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (wal_fd_ == -1) {
        fail("open wal");
    }
    replay_wal();
    if (memtable_bytes_ >= memtable_limit) {
        flush();
    }
}

Store::~Store() { ::close(wal_fd_); }

void Store::load_tables() {
    std::vector<std::uint32_t> ids;
    // range-for throws on errors
    std::error_code ec;
    std::filesystem::directory_iterator it(dir_, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.ends_with(".sst.tmp")) {
            // flush crashed before the rename, the wal still has the data
            std::error_code ignored;
            std::filesystem::remove(it->path(), ignored);
            continue;
        }
        if (!name.ends_with(".sst")) {
            continue;
        }
        const char *end = name.data() + name.size() - 4;
        std::uint32_t id = 0;
        auto [ptr, err] = std::from_chars(name.data(), end, id);
        if (err != std::errc() || ptr != end) {
            std::println(stderr, "unexpected file {} in {}", name, dir_);
            std::exit(1);
        }
        ids.push_back(id);
    }
    if (ec) {
        std::println(stderr, "list {}: {}", dir_, ec.message());
        std::exit(1);
    }

    std::sort(ids.begin(), ids.end(), std::greater<>());
    for (std::uint32_t id : ids) {
        tables_.push_back(std::make_unique<Table>(table_path(id).c_str()));
    }
    if (!ids.empty()) {
        next_table_id_ = ids.front() + 1;
    }
}

void Store::replay_wal() {
    std::string log;
    char chunk[64 * 1024];
    for (;;) {
        ssize_t n = ::read(wal_fd_, chunk, sizeof chunk);
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
            insert(key, value);
        } else if (type == type_delete) {
            insert(key, std::nullopt);
        } else {
            // the crc checks out, so this is a bug. don't truncate
            std::println(stderr, "replay: unknown record type {} at offset {}", type, offset);
            std::exit(1);
        }
        offset += record_size;
    }

    if (offset < log.size() && ::ftruncate(wal_fd_, static_cast<off_t>(offset)) == -1) {
        fail("ftruncate");
    }
}

void Store::insert(std::string_view key, std::optional<std::string_view> value) {
    // overwrites get counted twice
    memtable_bytes_ += key.size() + (value ? value->size() : 0);
    if (value) {
        memtable_.insert_or_assign(std::string(key), std::string(*value));
    } else {
        memtable_.insert_or_assign(std::string(key), std::nullopt);
    }
}

void Store::flush() {
    std::string path = table_path(next_table_id_);
    std::string tmp = path + ".tmp";
    write_table(tmp.c_str(), memtable_);
    if (::rename(tmp.c_str(), path.c_str()) == -1) {
        fail("rename");
    }
    sync_dir(dir_);

    // fsync the truncate too, otherwise after a power cut the old wal replays over newer tables
    if (::ftruncate(wal_fd_, 0) == -1) {
        fail("ftruncate");
    }
    if (::fsync(wal_fd_) == -1) {
        fail("fsync wal");
    }
    tables_.insert(tables_.begin(), std::make_unique<Table>(path.c_str()));
    next_table_id_++;
    memtable_.clear();
    memtable_bytes_ = 0;
}

std::string Store::table_path(std::uint32_t id) const { return std::format("{}/{:06}.sst", dir_, id); }

void Store::put(std::string_view key, std::string_view value) {
    write_record(wal_fd_, type_put, key, value);
    insert(key, value);
    if (memtable_bytes_ >= memtable_limit) {
        flush();
    }
}

void Store::remove(std::string_view key) {
    write_record(wal_fd_, type_delete, key, {});
    insert(key, std::nullopt);
    if (memtable_bytes_ >= memtable_limit) {
        flush();
    }
}

std::optional<std::string> Store::get(std::string_view key) const {
    if (auto it = memtable_.find(key); it != memtable_.end()) {
        return it->second;
    }

    std::string value;
    for (const auto &table : tables_) {
        Lookup result = table->get(key, value);
        if (result == Lookup::found) {
            return value;
        }
        if (result == Lookup::deleted) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}
