#include "sstable.hpp"
#include "crc32.hpp"
#include "io.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

// block:  [entry]...[crc u32]                    crc covers the entries
// entry:  [key_len u32][value_len u32][key][value]
// index:  [key_len u32][first key][offset u64][size u64]...    size includes the crc
// footer: [index_offset u64][index_crc u32][magic u32]

namespace {

constexpr std::size_t block_size = 4096;
constexpr std::size_t footer_size = 16;
constexpr std::uint32_t table_magic = 0x646e6572; // "rend"
constexpr std::uint32_t tombstone = 0xFFFFFFFF;   // value_len of a deleted key

} // namespace

void write_table(const char *path, const Memtable &memtable) {
    std::vector<const Memtable::value_type *> sorted;
    sorted.reserve(memtable.size());
    for (const auto &entry : memtable) {
        sorted.push_back(&entry);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto *a, const auto *b) { return a->first < b->first; });

    std::string out;
    std::string index;
    std::size_t block_start = 0;
    std::string_view first_key;

    auto finish_block = [&] {
        append_u32(out, crc32(std::string_view(out).substr(block_start)));
        append_u32(index, static_cast<std::uint32_t>(first_key.size()));
        index += first_key;
        append_u64(index, block_start);
        append_u64(index, out.size() - block_start);
        block_start = out.size();
    };

    for (const auto *entry : sorted) {
        const std::string &key = entry->first;
        const std::optional<std::string> &value = entry->second;
        if (out.size() == block_start) {
            first_key = key;
        }
        append_u32(out, static_cast<std::uint32_t>(key.size()));
        append_u32(out, value ? static_cast<std::uint32_t>(value->size()) : tombstone);
        out += key;
        if (value) {
            out += *value;
        }
        if (out.size() - block_start >= block_size) {
            finish_block();
        }
    }
    if (out.size() > block_start) {
        finish_block();
    }

    std::uint64_t index_offset = out.size();
    out += index;
    append_u64(out, index_offset);
    append_u32(out, crc32(index));
    append_u32(out, table_magic);

    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        fail("open table");
    }
    write_all(fd, out);
    if (::fsync(fd) == -1) {
        fail("fsync table");
    }
    ::close(fd);
}

Table::Table(const char *path) : path_(path), fd_(::open(path, O_RDONLY)) {
    if (fd_ == -1) {
        fail("open table");
    }

    struct stat st;
    if (::fstat(fd_, &st) == -1) {
        fail("fstat table");
    }
    auto size = static_cast<std::uint64_t>(st.st_size);
    if (size < footer_size) {
        corrupt("too small");
    }

    std::string footer = pread_exact(fd_, footer_size, size - footer_size);
    std::uint64_t index_offset = read_u64(footer.data());
    std::uint32_t index_crc = read_u32(footer.data() + 8);
    if (read_u32(footer.data() + 12) != table_magic || index_offset > size - footer_size) {
        corrupt("bad footer");
    }

    std::string index = pread_exact(fd_, size - footer_size - index_offset, index_offset);
    if (crc32(index) != index_crc) {
        corrupt("bad index crc");
    }

    std::string_view rest = index;
    while (!rest.empty()) {
        if (rest.size() < 4) {
            corrupt("bad index");
        }
        std::uint32_t key_len = read_u32(rest.data());
        rest.remove_prefix(4);
        if (key_len > rest.size() || rest.size() - key_len < 16) {
            corrupt("bad index");
        }
        std::uint64_t offset = read_u64(rest.data() + key_len);
        std::uint64_t block_size_on_disk = read_u64(rest.data() + key_len + 8);
        if (offset > index_offset || block_size_on_disk > index_offset - offset) {
            corrupt("block outside the data area");
        }
        index_.push_back({std::string(rest.substr(0, key_len)), offset, block_size_on_disk});
        rest.remove_prefix(key_len + 16);
    }
}

Table::~Table() { ::close(fd_); }

Lookup Table::get(std::string_view key, std::string &value) const {
    // last block with first_key <= key
    auto it = std::upper_bound(index_.begin(), index_.end(), key,
                               [](std::string_view k, const BlockHandle &b) { return k < b.first_key; });
    if (it == index_.begin()) {
        return Lookup::missing;
    }
    const BlockHandle &block = *(it - 1);
    if (block.size < 4) {
        corrupt("bad block size");
    }

    std::string buf = pread_exact(fd_, block.size, block.offset);
    std::string_view entries = std::string_view(buf).substr(0, buf.size() - 4);
    if (crc32(entries) != read_u32(buf.data() + entries.size())) {
        corrupt("bad block crc");
    }

    while (!entries.empty()) {
        if (entries.size() < 8) {
            corrupt("bad entry");
        }
        std::uint32_t key_len = read_u32(entries.data());
        std::uint32_t value_len = read_u32(entries.data() + 4);
        bool deleted = value_len == tombstone;
        std::size_t stored_len = deleted ? 0 : value_len;
        entries.remove_prefix(8);
        if (key_len > entries.size() || stored_len > entries.size() - key_len) {
            corrupt("bad entry");
        }

        std::string_view k = entries.substr(0, key_len);
        if (k == key) {
            if (deleted) {
                return Lookup::deleted;
            }
            value.assign(entries.substr(key_len, stored_len));
            return Lookup::found;
        }
        if (k > key) {
            return Lookup::missing; // sorted, so we're past it
        }
        entries.remove_prefix(key_len + stored_len);
    }
    return Lookup::missing;
}

void Table::corrupt(const char *why) const {
    std::println(stderr, "table {} is corrupt: {}", path_, why);
    std::exit(1);
}
