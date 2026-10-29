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

    // for tests
    std::vector<std::size_t> level_counts() const;

  private:
    struct TableFile {
        std::uint32_t id;
        std::unique_ptr<Table> table;
    };
    struct Compaction;

    void load();
    void replay_wal();
    void insert(std::string_view key, std::optional<std::string_view> value);
    void after_write(std::size_t bytes);
    void flush();
    void write_manifest() const;
    void start_compaction();
    void compaction_step(std::size_t budget);
    void finish_compaction();
    const Table *find_table(std::size_t level, std::string_view key) const;
    bool deeper_levels_may_have(std::size_t level, std::string_view key) const;
    std::string table_path(std::uint32_t id) const;

    std::string dir_;
    int wal_fd_ = -1;
    Memtable memtable_;
    std::size_t memtable_bytes_ = 0;
    // l0 is newest first and can overlap, deeper levels are sorted and don't
    std::vector<std::vector<TableFile>> levels_;
    // round robin position per level
    std::vector<std::string> compact_pointer_;
    std::unique_ptr<Compaction> compaction_;
    std::uint32_t next_table_id_ = 1;
};
