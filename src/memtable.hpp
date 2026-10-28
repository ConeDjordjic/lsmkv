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

// nullopt = tombstone
using Memtable =
    std::unordered_map<std::string, std::optional<std::string>, StringHash, std::equal_to<>>;
