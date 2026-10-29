#include "crc32c.hpp"
#include "store.hpp"

// so asserts still run in release builds
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <print>
#include <string>

namespace fs = std::filesystem;

static std::string read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static fs::path wal(const char *dir) { return fs::path(dir) / "wal.log"; }

static std::string key_at(int i) { return std::format("key{:05}", i); }

static std::string value_at(int i) { return std::string(1000, static_cast<char>('a' + i % 26)); }

static void test_put_writes_one_record() {
    const char *dir = "test_put";
    fs::remove_all(dir);

    {
        Store store(dir);
        store.put("a", "1");
        assert(store.get("a") == "1");
    }

    // [type 1][key_len 1][value_len 1][a][1]
    std::string body;
    body.push_back(1);
    body.append("\x01\x00\x00\x00", 4);
    body.append("\x01\x00\x00\x00", 4);
    body.append("a1");

    std::uint32_t crc = crc32c(body);
    std::string expected(4, '\0');
    std::memcpy(expected.data(), &crc, 4);
    expected += body;

    std::string actual = read_file(wal(dir));
    assert(actual.size() == 15);
    assert(actual == expected);

    fs::remove_all(dir);
}

static void test_put_overwrites() {
    const char *dir = "test_overwrite";
    fs::remove_all(dir);

    {
        Store store(dir);
        store.put("a", "1");
        store.put("a", "9");
        assert(store.get("a") == "9");
        assert(!store.get("b"));
    }

    fs::remove_all(dir);
}

static void test_replay_after_reopen() {
    const char *dir = "test_replay";
    fs::remove_all(dir);

    {
        Store store(dir);
        store.put("a", "1");
        store.put("b", "2");
        store.remove("b");
        store.put("a", "9");
    }
    {
        Store store(dir);
        assert(store.get("a") == "9");
        assert(!store.get("b"));
    }

    fs::remove_all(dir);
}

static void test_torn_tail_is_cut() {
    const char *dir = "test_torn";
    fs::remove_all(dir);

    {
        Store store(dir);
        store.put("a", "1");
        store.put("b", "2");
    }
    fs::resize_file(wal(dir), 30 - 3);
    {
        Store store(dir);
        assert(store.get("a") == "1");
        assert(!store.get("b"));
        assert(fs::file_size(wal(dir)) == 15);
        store.put("c", "3");
    }
    {
        Store store(dir);
        assert(store.get("a") == "1");
        assert(store.get("c") == "3");
    }

    fs::remove_all(dir);
}

static void test_bad_crc_is_cut() {
    const char *dir = "test_crc";
    fs::remove_all(dir);

    {
        Store store(dir);
        store.put("a", "1");
        store.put("b", "2");
    }
    std::string log = read_file(wal(dir));
    log[29] ^= 1; // flip a bit in b's value
    std::ofstream(wal(dir), std::ios::binary) << log;
    {
        Store store(dir);
        assert(store.get("a") == "1");
        assert(!store.get("b"));
        assert(fs::file_size(wal(dir)) == 15);
    }

    fs::remove_all(dir);
}

static void test_huge_length_tail_is_cut() {
    const char *dir = "test_huge";
    fs::remove_all(dir);

    {
        Store store(dir);
        store.put("a", "1");
    }
    // fake header with key_len = 0xFFFFFFFF
    std::ofstream(wal(dir), std::ios::binary | std::ios::app)
        << std::string("\0\0\0\0\x01\xFF\xFF\xFF\xFF\0\0\0\0", 13);
    {
        Store store(dir);
        assert(store.get("a") == "1");
        assert(fs::file_size(wal(dir)) == 15);
    }
    // key_len = 1, value_len = 0xFFFFFFFF
    std::ofstream(wal(dir), std::ios::binary | std::ios::app)
        << std::string("\0\0\0\0\x01\x01\0\0\0\xFF\xFF\xFF\xFFk", 14);
    {
        Store store(dir);
        assert(store.get("a") == "1");
        assert(fs::file_size(wal(dir)) == 15);
    }

    fs::remove_all(dir);
}

static void test_flush_to_table() {
    const char *dir = "test_flush";
    fs::remove_all(dir);

    {
        Store store(dir);
        for (int i = 0; i < 5000; i++) {
            store.put(key_at(i), value_at(i));
        }
        assert(fs::exists(fs::path(dir) / "000001.sst"));
        assert(fs::file_size(wal(dir)) < 4 * 1024 * 1024);

        // both already flushed
        store.put(key_at(0), "new");
        store.remove(key_at(1));
    }
    {
        Store store(dir);
        assert(store.get(key_at(0)) == "new");
        assert(!store.get(key_at(1)));
        for (int i = 2; i < 5000; i++) {
            assert(store.get(key_at(i)) == value_at(i));
        }
        assert(!store.get("aaa"));       // before the first key
        assert(!store.get("key99999"));  // after the last key
        assert(!store.get("key00002x")); // between two keys
    }

    fs::remove_all(dir);
}

static void test_tombstone_in_table_hides_older_value() {
    const char *dir = "test_tombstone";
    fs::remove_all(dir);

    {
        Store store(dir);
        for (int i = 0; i < 5000; i++) {
            store.put(key_at(i), value_at(i));
        }
        store.remove(key_at(0));
        // flush again so the tombstone lands in table 2
        for (int i = 0; i < 5000; i++) {
            store.put(std::format("other{:05}", i), value_at(i));
        }
        assert(fs::exists(fs::path(dir) / "000002.sst"));
    }
    {
        Store store(dir);
        assert(!store.get(key_at(0)));
        assert(store.get(key_at(1)) == value_at(1));
    }

    fs::remove_all(dir);
}

// crash after the table is written but before the wal truncate
static void test_crash_before_wal_truncate() {
    const char *dir = "test_crash_truncate";
    fs::remove_all(dir);

    {
        Store store(dir);
        for (int i = 0; i < 4000; i++) {
            store.put(key_at(i), value_at(i));
        }
    }
    fs::copy_file(wal(dir), "test_crash_truncate.wal", fs::copy_options::overwrite_existing);
    {
        Store store(dir);
        for (int i = 4000; i < 5000; i++) {
            store.put(key_at(i), value_at(i));
        }
        assert(fs::exists(fs::path(dir) / "000001.sst"));
    }
    fs::copy_file("test_crash_truncate.wal", wal(dir), fs::copy_options::overwrite_existing);
    {
        Store store(dir);
        for (int i = 0; i < 4000; i++) {
            assert(store.get(key_at(i)) == value_at(i));
        }
        store.remove(key_at(0));
    }
    {
        Store store(dir);
        assert(!store.get(key_at(0)));
        assert(store.get(key_at(1)) == value_at(1));
    }

    fs::remove("test_crash_truncate.wal");
    fs::remove_all(dir);
}

static void test_leftover_tmp_is_removed() {
    const char *dir = "test_tmp";
    fs::remove_all(dir);
    fs::create_directory(dir);
    std::ofstream(fs::path(dir) / "MANIFEST.tmp") << "half a manifest";

    {
        Store store(dir);
        assert(!fs::exists(fs::path(dir) / "MANIFEST.tmp"));
        store.put("a", "1");
        assert(store.get("a") == "1");
    }

    fs::remove_all(dir);
}

static void test_bloom_filter() {
    const char *path = "test_bloom.sst";
    Memtable memtable;
    for (int i = 0; i < 10000; i++) {
        memtable.insert_or_assign(key_at(i), "v");
    }
    write_table(path, memtable);

    Table table(path);
    for (int i = 0; i < 10000; i++) {
        assert(table.may_contain(key_at(i)));
    }
    int false_positives = 0;
    for (int i = 0; i < 10000; i++) {
        if (table.may_contain(std::format("missing{:05}", i))) {
            false_positives++;
        }
    }
    // should be around 1%
    assert(false_positives < 200);

    fs::remove(path);
}

static int count_tables(const char *dir) {
    int n = 0;
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".sst") {
            n++;
        }
    }
    return n;
}

static int total(const std::vector<std::size_t> &counts) {
    std::size_t n = 0;
    for (std::size_t c : counts) {
        n += c;
    }
    return static_cast<int>(n);
}

// random ops checked against a std::map, reopening every 25k ops
static void test_matches_model() {
    const char *dir = "test_model";
    fs::remove_all(dir);

    std::map<std::string, std::string> model;
    std::uint64_t rng = 42;
    auto next = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    auto store = std::make_unique<Store>(dir);
    for (int i = 0; i < 100000; i++) {
        std::string key = std::format("key{:05}", next() % 60000);
        std::uint64_t op = next() % 100;
        if (op < 70) {
            std::string value(200 + next() % 600, static_cast<char>('a' + i % 26));
            store->put(key, value);
            model[key] = value;
        } else if (op < 85) {
            store->remove(key);
            model.erase(key);
        } else {
            auto it = model.find(key);
            auto got = store->get(key);
            assert(it == model.end() ? !got : got == it->second);
        }
        assert(store->level_counts()[0] < 12);
        if (i % 25000 == 24999) {
            store.reset();
            store = std::make_unique<Store>(dir);
        }
    }

    for (int k = 0; k < 60000; k++) {
        std::string key = std::format("key{:05}", k);
        auto it = model.find(key);
        auto got = store->get(key);
        assert(it == model.end() ? !got : got == it->second);
    }
    assert(store->level_counts()[2] > 0);

    // reopen cleans up unfinished compaction output
    store.reset();
    store = std::make_unique<Store>(dir);
    assert(count_tables(dir) == total(store->level_counts()));

    store.reset();
    fs::remove_all(dir);
}

static void test_orphan_table_is_removed() {
    const char *dir = "test_orphan";
    fs::remove_all(dir);

    {
        Store store(dir);
        for (int i = 0; i < 5000; i++) {
            store.put(key_at(i), value_at(i));
        }
    }
    // fake orphan from a crashed compaction
    fs::copy_file(fs::path(dir) / "000001.sst", fs::path(dir) / "000099.sst");
    {
        Store store(dir);
        assert(!fs::exists(fs::path(dir) / "000099.sst"));
        assert(fs::exists(fs::path(dir) / "000001.sst"));
        assert(store.get(key_at(0)) == value_at(0));
    }

    fs::remove_all(dir);
}

int main() {
    test_put_writes_one_record();
    test_put_overwrites();
    test_replay_after_reopen();
    test_torn_tail_is_cut();
    test_bad_crc_is_cut();
    test_huge_length_tail_is_cut();
    test_flush_to_table();
    test_tombstone_in_table_hides_older_value();
    test_crash_before_wal_truncate();
    test_leftover_tmp_is_removed();
    test_bloom_filter();
    test_matches_model();
    test_orphan_table_is_removed();
    std::println("all tests passed");
}
