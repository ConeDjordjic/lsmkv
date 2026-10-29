#include "sstable.hpp"
#include "crc32c.hpp"
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
// filter: [k u8][bits]                            bloom filter over every key
// index:  [key_len u32][first key][offset u64][size u64]...    size includes the crc
// footer: [filter_offset u64][index_offset u64][flags u32][crc u32][magic u32]
//         crc covers the filter, index and footer up to the crc

namespace {

constexpr std::size_t block_size = 4096;
constexpr std::size_t footer_size = 28;
constexpr std::uint32_t table_magic = 0x32646e72; // "rnd2"
constexpr std::uint32_t tombstone = 0xFFFFFFFF;   // value_len of a deleted key
constexpr std::uint32_t flag_base = 1;

// ~1% false positives
constexpr std::size_t bloom_bits_per_key = 10;
constexpr std::uint8_t bloom_probes = 7;

// fnv-1a + murmur finalizer. part of the disk format, don't change it
std::uint64_t hash_key(std::string_view key) {
    std::uint64_t h = 0xcbf29ce484222325;
    for (char c : key) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccd;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53;
    h ^= h >> 33;
    return h;
}

std::string build_filter(const std::vector<std::uint64_t> &hashes) {
    std::size_t bytes = (std::max<std::size_t>(64, hashes.size() * bloom_bits_per_key) + 7) / 8;
    std::uint64_t bits = bytes * 8;
    std::string filter(1 + bytes, '\0');
    filter[0] = static_cast<char>(bloom_probes);
    for (std::uint64_t h : hashes) {
        // double hashing
        std::uint64_t delta = (h >> 33) | (h << 31);
        for (int i = 0; i < bloom_probes; i++) {
            std::uint64_t bit = h % bits;
            filter[1 + bit / 8] = static_cast<char>(filter[1 + bit / 8] | (1 << (bit % 8)));
            h += delta;
        }
    }
    return filter;
}

// false if the entry is malformed
bool parse_entry(std::string_view &entries, Entry &entry) {
    if (entries.size() < 8) {
        return false;
    }
    std::uint32_t key_len = read_u32(entries.data());
    std::uint32_t value_len = read_u32(entries.data() + 4);
    bool deleted = value_len == tombstone;
    std::size_t stored_len = deleted ? 0 : value_len;
    entries.remove_prefix(8);
    if (key_len > entries.size() || stored_len > entries.size() - key_len) {
        return false;
    }

    entry.key = entries.substr(0, key_len);
    if (deleted) {
        entry.value = std::nullopt;
    } else {
        entry.value = entries.substr(key_len, stored_len);
    }
    entries.remove_prefix(key_len + stored_len);
    return true;
}

} // namespace

TableWriter::TableWriter(const char *path) : fd_(::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) {
    if (fd_ == -1) {
        fail("open table");
    }
}

TableWriter::~TableWriter() { ::close(fd_); }

void TableWriter::add(std::string_view key, std::optional<std::string_view> value) {
    if (block_.empty()) {
        first_key_ = key;
    }
    append_u32(block_, static_cast<std::uint32_t>(key.size()));
    append_u32(block_, value ? static_cast<std::uint32_t>(value->size()) : tombstone);
    block_ += key;
    if (value) {
        block_ += *value;
    }
    hashes_.push_back(hash_key(key));
    if (block_.size() >= block_size) {
        finish_block();
    }
}

void TableWriter::finish_block() {
    append_u32(block_, crc32c(block_));
    write_all(fd_, block_);

    append_u32(index_, static_cast<std::uint32_t>(first_key_.size()));
    index_ += first_key_;
    append_u64(index_, offset_);
    append_u64(index_, block_.size());

    offset_ += block_.size();
    block_.clear();
}

void TableWriter::finish(bool base) {
    if (!block_.empty()) {
        finish_block();
    }

    std::string meta = build_filter(hashes_);
    std::uint64_t filter_offset = offset_;
    std::uint64_t index_offset = offset_ + meta.size();
    meta += index_;
    append_u64(meta, filter_offset);
    append_u64(meta, index_offset);
    append_u32(meta, base ? flag_base : 0);
    append_u32(meta, crc32c(meta));
    append_u32(meta, table_magic);

    write_all(fd_, meta);
    if (::fsync(fd_) == -1) {
        fail("fsync table");
    }
}

void write_table(const char *path, const Memtable &memtable) {
    std::vector<const Memtable::value_type *> sorted;
    sorted.reserve(memtable.size());
    for (const auto &entry : memtable) {
        sorted.push_back(&entry);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto *a, const auto *b) { return a->first < b->first; });

    TableWriter writer(path);
    for (const auto *entry : sorted) {
        if (entry->second) {
            writer.add(entry->first, *entry->second);
        } else {
            writer.add(entry->first, std::nullopt);
        }
    }
    writer.finish(false);
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
    std::uint64_t filter_offset = read_u64(footer.data());
    std::uint64_t index_offset = read_u64(footer.data() + 8);
    std::uint32_t flags = read_u32(footer.data() + 16);
    std::uint32_t meta_crc = read_u32(footer.data() + 20);
    if (read_u32(footer.data() + 24) != table_magic || filter_offset > index_offset ||
        index_offset > size - footer_size) {
        corrupt("bad footer");
    }

    std::string meta = pread_exact(fd_, size - filter_offset, filter_offset);
    if (crc32c(std::string_view(meta).substr(0, meta.size() - 8)) != meta_crc) {
        corrupt("bad meta crc");
    }
    base_ = (flags & flag_base) != 0;
    filter_ = meta.substr(0, index_offset - filter_offset);

    std::string_view rest =
        std::string_view(meta).substr(index_offset - filter_offset, size - footer_size - index_offset);
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
        if (offset > filter_offset || block_size_on_disk > filter_offset - offset) {
            corrupt("block outside the data area");
        }
        index_.push_back({std::string(rest.substr(0, key_len)), offset, block_size_on_disk});
        rest.remove_prefix(key_len + 16);
    }
}

Table::~Table() { ::close(fd_); }

bool Table::may_contain(std::string_view key) const {
    if (filter_.size() < 2) {
        return true;
    }
    int probes = static_cast<unsigned char>(filter_[0]);
    std::uint64_t bits = (filter_.size() - 1) * 8;
    std::uint64_t h = hash_key(key);
    std::uint64_t delta = (h >> 33) | (h << 31);
    for (int i = 0; i < probes; i++) {
        std::uint64_t bit = h % bits;
        if ((static_cast<unsigned char>(filter_[1 + bit / 8]) & (1u << (bit % 8))) == 0) {
            return false;
        }
        h += delta;
    }
    return true;
}

std::string Table::read_block(std::size_t i) const {
    const BlockHandle &block = index_[i];
    if (block.size < 4) {
        corrupt("bad block size");
    }
    std::string buf = pread_exact(fd_, block.size, block.offset);
    std::size_t n = buf.size() - 4;
    if (crc32c(std::string_view(buf).substr(0, n)) != read_u32(buf.data() + n)) {
        corrupt("bad block crc");
    }
    buf.resize(n);
    return buf;
}

Lookup Table::get(std::string_view key, std::string &value) const {
    if (!may_contain(key)) {
        return Lookup::missing;
    }

    // last block with first_key <= key
    auto it = std::upper_bound(index_.begin(), index_.end(), key,
                               [](std::string_view k, const BlockHandle &b) { return k < b.first_key; });
    if (it == index_.begin()) {
        return Lookup::missing;
    }

    std::string buf = read_block(static_cast<std::size_t>(it - index_.begin() - 1));
    std::string_view rest = buf;
    Entry entry;
    while (!rest.empty()) {
        if (!parse_entry(rest, entry)) {
            corrupt("bad entry");
        }
        if (entry.key == key) {
            if (!entry.value) {
                return Lookup::deleted;
            }
            value.assign(*entry.value);
            return Lookup::found;
        }
        if (entry.key > key) {
            return Lookup::missing; // sorted, so we're past it
        }
    }
    return Lookup::missing;
}

void Table::corrupt(const char *why) const {
    std::println(stderr, "table {} is corrupt: {}", path_, why);
    std::exit(1);
}

TableIterator::TableIterator(const Table &table) : table_(table) { next(); }

void TableIterator::next() {
    while (rest_.empty()) {
        if (block_ == table_.index_.size()) {
            valid_ = false;
            return;
        }
        buf_ = table_.read_block(block_++);
        rest_ = buf_;
    }
    if (!parse_entry(rest_, entry_)) {
        table_.corrupt("bad entry");
    }
    valid_ = true;
}
