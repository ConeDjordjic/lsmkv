#pragma once
#include "memtable.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class Lookup { missing, found, deleted };

// keys must be added in sorted order
class TableWriter {
  public:
    explicit TableWriter(const char *path);
    ~TableWriter();
    TableWriter(const TableWriter &) = delete;
    TableWriter &operator=(const TableWriter &) = delete;

    // nullopt = tombstone
    void add(std::string_view key, std::optional<std::string_view> value);
    // tables older than a base table are garbage
    void finish(bool base);

  private:
    void finish_block();

    int fd_;
    std::uint64_t offset_ = 0;
    std::string block_;
    std::string first_key_;
    std::string index_;
    std::vector<std::uint64_t> hashes_;
};

void write_table(const char *path, const Memtable &memtable);

class Table {
  public:
    explicit Table(const char *path); // loads index + filter
    ~Table();
    Table(const Table &) = delete;
    Table &operator=(const Table &) = delete;

    Lookup get(std::string_view key, std::string &value) const;
    bool may_contain(std::string_view key) const;
    bool is_base() const { return base_; }
    const std::string &path() const { return path_; }

  private:
    friend class TableIterator;

    struct BlockHandle {
        std::string first_key;
        std::uint64_t offset;
        std::uint64_t size;
    };

    // checks the crc and strips it
    std::string read_block(std::size_t i) const;
    [[noreturn]] void corrupt(const char *why) const;

    std::string path_;
    int fd_;
    bool base_ = false;
    std::string filter_;
    std::vector<BlockHandle> index_;
};

struct Entry {
    std::string_view key;
    std::optional<std::string_view> value; // nullopt = tombstone
};

// entry() is invalidated by next()
class TableIterator {
  public:
    explicit TableIterator(const Table &table);
    TableIterator(const TableIterator &) = delete;
    TableIterator &operator=(const TableIterator &) = delete;

    bool valid() const { return valid_; }
    const Entry &entry() const { return entry_; }
    void next();

  private:
    const Table &table_;
    std::size_t block_ = 0;
    std::string buf_;
    std::string_view rest_;
    Entry entry_;
    bool valid_ = false;
};
