#pragma once
#include "memtable.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class Lookup { missing, found, deleted };

void write_table(const char *path, const Memtable &memtable);

class Table {
  public:
    explicit Table(const char *path); // loads the index
    ~Table();
    Table(const Table &) = delete;
    Table &operator=(const Table &) = delete;

    Lookup get(std::string_view key, std::string &value) const;

  private:
    struct BlockHandle {
        std::string first_key;
        std::uint64_t offset;
        std::uint64_t size;
    };

    [[noreturn]] void corrupt(const char *why) const;

    std::string path_;
    int fd_;
    std::vector<BlockHandle> index_;
};
