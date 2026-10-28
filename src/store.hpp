#pragma once
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// transparent hash so find() takes a string_view without allocating
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};

class Store {
  public:
    explicit Store(const char *path); // opens the log and replays it
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    void put(std::string_view key, std::string_view value);
    void remove(std::string_view key);
    std::optional<std::string> get(std::string_view key) const;

  private:
    int fd_;
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> memtable_;
};
