#pragma once
#include "memtable.hpp"
#include "sstable.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Store {
  public:
    explicit Store(const char *dir); // creates dir if needed
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    void put(std::string_view key, std::string_view value);
    void remove(std::string_view key);
    std::optional<std::string> get(std::string_view key) const;

  private:
    void load_tables();
    void replay_wal();
    void insert(std::string_view key, std::optional<std::string_view> value);
    void flush();
    void compact();
    std::string table_path(std::uint32_t id) const;

    std::string dir_;
    int wal_fd_ = -1;
    Memtable memtable_;
    std::size_t memtable_bytes_ = 0;
    std::vector<std::unique_ptr<Table>> tables_; // newest first
    std::uint32_t next_table_id_ = 1;
};
