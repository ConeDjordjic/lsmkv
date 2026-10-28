#include "crc32.hpp"
#include "store.hpp"

// so asserts still run in release builds
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string>

namespace fs = std::filesystem;

static std::string read_file(const char *path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static void test_put_writes_one_record() {
    const char *path = "store_test_put.log";
    fs::remove(path);

    {
        Store store(path);
        store.put("a", "1");
        assert(store.get("a") == "1");
    }

    // [type 1][key_len 1][value_len 1][a][1]
    std::string body;
    body.push_back(1);
    body.append("\x01\x00\x00\x00", 4);
    body.append("\x01\x00\x00\x00", 4);
    body.append("a1");

    std::uint32_t crc = crc32(body);
    std::string expected(4, '\0');
    std::memcpy(expected.data(), &crc, 4);
    expected += body;

    std::string actual = read_file(path);
    assert(actual.size() == 15);
    assert(actual == expected);

    fs::remove(path);
}

static void test_put_overwrites() {
    const char *path = "store_test_overwrite.log";
    fs::remove(path);

    {
        Store store(path);
        store.put("a", "1");
        store.put("a", "9");
        assert(store.get("a") == "9");
        assert(!store.get("b"));
    }

    fs::remove(path);
}

static void test_replay_after_reopen() {
    const char *path = "store_test_replay.log";
    fs::remove(path);

    {
        Store store(path);
        store.put("a", "1");
        store.put("b", "2");
        store.remove("b");
        store.put("a", "9");
    }
    {
        Store store(path);
        assert(store.get("a") == "9");
        assert(!store.get("b"));
    }

    fs::remove(path);
}

static void test_torn_tail_is_cut() {
    const char *path = "store_test_torn.log";
    fs::remove(path);

    {
        Store store(path);
        store.put("a", "1");
        store.put("b", "2");
    }
    fs::resize_file(path, 30 - 3);
    {
        Store store(path);
        assert(store.get("a") == "1");
        assert(!store.get("b"));
        assert(fs::file_size(path) == 15);
        store.put("c", "3");
    }
    {
        Store store(path);
        assert(store.get("a") == "1");
        assert(store.get("c") == "3");
    }

    fs::remove(path);
}

static void test_bad_crc_is_cut() {
    const char *path = "store_test_crc.log";
    fs::remove(path);

    {
        Store store(path);
        store.put("a", "1");
        store.put("b", "2");
    }
    std::string log = read_file(path);
    log[29] ^= 1; // flip a bit in b's value
    std::ofstream(path, std::ios::binary) << log;
    {
        Store store(path);
        assert(store.get("a") == "1");
        assert(!store.get("b"));
        assert(fs::file_size(path) == 15);
    }

    fs::remove(path);
}

static void test_huge_length_tail_is_cut() {
    const char *path = "store_test_huge.log";
    fs::remove(path);

    {
        Store store(path);
        store.put("a", "1");
    }
    // fake header with key_len = 0xFFFFFFFF
    std::ofstream(path, std::ios::binary | std::ios::app)
        << std::string("\0\0\0\0\x01\xFF\xFF\xFF\xFF\0\0\0\0", 13);
    {
        Store store(path);
        assert(store.get("a") == "1");
        assert(fs::file_size(path) == 15);
    }
    // key_len = 1, value_len = 0xFFFFFFFF
    std::ofstream(path, std::ios::binary | std::ios::app)
        << std::string("\0\0\0\0\x01\x01\0\0\0\xFF\xFF\xFF\xFFk", 14);
    {
        Store store(path);
        assert(store.get("a") == "1");
        assert(fs::file_size(path) == 15);
    }

    fs::remove(path);
}

int main() {
    test_put_writes_one_record();
    test_put_overwrites();
    test_replay_after_reopen();
    test_torn_tail_is_cut();
    test_bad_crc_is_cut();
    test_huge_length_tail_is_cut();
    std::println("all tests passed");
}
