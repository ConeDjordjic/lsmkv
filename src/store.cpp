#include "store.hpp"
#include "crc32c.hpp"
#include "io.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <limits>
#include <print>
#include <set>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace {

constexpr std::uint8_t type_put = 1;
constexpr std::uint8_t type_delete = 2;

// crc u32 + type u8 + key_len u32 + value_len u32
constexpr std::size_t header_size = 13;

constexpr std::size_t memtable_limit = 4 * 1024 * 1024;

// same numbers as leveldb
constexpr std::size_t num_levels = 7;
constexpr std::size_t l0_compact_at = 4;
constexpr std::size_t l0_stall_at = 12;
constexpr std::uint64_t level1_max_bytes = 10 * 1024 * 1024; // x10 per level
constexpr std::uint64_t target_table_size = 2 * 1024 * 1024;

// compaction bytes per written byte
constexpr std::size_t compaction_pace = 32;

// manifest: [magic u32][count u32] then [id u32][level u32] per table, then [crc u32]
constexpr std::uint32_t manifest_magic = 0x4d6d736c; // "lsmM"

void write_record(int fd, std::uint8_t type, std::string_view key, std::string_view value) {
    // 0xFFFFFFFF is the tombstone marker in tables
    constexpr std::size_t max_len = std::numeric_limits<std::uint32_t>::max();
    if (key.size() > max_len || value.size() >= max_len) {
        std::println(stderr, "write: key or value over 4 GiB");
        std::exit(1);
    }

    std::string buf;
    buf.reserve(header_size + key.size() + value.size());
    buf.append(4, '\0'); // crc placeholder, filled in below
    buf.push_back(static_cast<char>(type));
    append_u32(buf, static_cast<std::uint32_t>(key.size()));
    append_u32(buf, static_cast<std::uint32_t>(value.size()));
    buf.append(key);
    buf.append(value);

    std::uint32_t crc = crc32c(std::string_view(buf).substr(4));
    std::memcpy(buf.data(), &crc, sizeof crc);

    write_all(fd, buf);
}

void sync_dir(const std::string &dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd == -1) {
        fail("open dir");
    }
    if (::fsync(fd) == -1) {
        fail("fsync dir");
    }
    ::close(fd);
}

std::uint64_t max_bytes(std::size_t level) {
    std::uint64_t bytes = level1_max_bytes;
    for (std::size_t i = 1; i < level; i++) {
        bytes *= 10;
    }
    return bytes;
}

bool overlaps(const Table &table, std::string_view lo, std::string_view hi) {
    return table.largest() >= lo && table.smallest() <= hi;
}

} // namespace

struct Store::Compaction {
    std::size_t output_level;
    std::vector<std::uint32_t> input_ids;
    // newest first, first one wins ties
    std::vector<std::unique_ptr<TableIterator>> inputs;
    std::unique_ptr<TableWriter> out;
    std::uint32_t out_id = 0;
    std::vector<std::uint32_t> outputs;
    std::string key;
};

Store::Store(const char *dir) : dir_(dir), levels_(num_levels), compact_pointer_(num_levels) {
    if (::mkdir(dir, 0755) == -1 && errno != EEXIST) {
        fail("mkdir");
    }
    load();

    wal_fd_ = ::open((dir_ + "/wal.log").c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (wal_fd_ == -1) {
        fail("open wal");
    }
    replay_wal();
    if (memtable_bytes_ >= memtable_limit) {
        flush();
    }
    start_compaction();
}

// load() deletes any unfinished compaction output
Store::~Store() { ::close(wal_fd_); }

void Store::load() {
    // range-for throws on errors
    std::set<std::uint32_t> on_disk;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir_, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.ends_with(".tmp")) {
            // left over from a crashed manifest write
            std::error_code ignored;
            std::filesystem::remove(it->path(), ignored);
            continue;
        }
        if (!name.ends_with(".sst")) {
            continue;
        }
        const char *end = name.data() + name.size() - 4;
        std::uint32_t id = 0;
        auto [ptr, err] = std::from_chars(name.data(), end, id);
        if (err != std::errc() || ptr != end) {
            std::println(stderr, "unexpected file {} in {}", name, dir_);
            std::exit(1);
        }
        on_disk.insert(id);
    }
    if (ec) {
        std::println(stderr, "list {}: {}", dir_, ec.message());
        std::exit(1);
    }
    if (!on_disk.empty()) {
        next_table_id_ = *on_disk.rbegin() + 1;
    }

    std::string path = dir_ + "/MANIFEST";
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        if (errno != ENOENT) {
            fail("open manifest");
        }
        if (!on_disk.empty()) {
            std::println(stderr, "{} has tables but no MANIFEST", dir_);
            std::exit(1);
        }
        // new store
        write_manifest();
        return;
    }
    struct stat st;
    if (::fstat(fd, &st) == -1) {
        fail("fstat manifest");
    }
    std::string manifest = pread_exact(fd, static_cast<std::size_t>(st.st_size), 0);
    ::close(fd);

    if (manifest.size() < 12 || read_u32(manifest.data()) != manifest_magic ||
        crc32c(std::string_view(manifest).substr(0, manifest.size() - 4)) !=
            read_u32(manifest.data() + manifest.size() - 4)) {
        std::println(stderr, "{} is corrupt", path);
        std::exit(1);
    }
    std::uint32_t count = read_u32(manifest.data() + 4);
    if (count != (manifest.size() - 12) / 8 || (manifest.size() - 12) % 8 != 0) {
        std::println(stderr, "{} is corrupt", path);
        std::exit(1);
    }

    std::set<std::uint32_t> live;
    for (std::uint32_t i = 0; i < count; i++) {
        std::uint32_t id = read_u32(manifest.data() + 8 + i * 8);
        std::uint32_t level = read_u32(manifest.data() + 12 + i * 8);
        if (level >= num_levels || !on_disk.contains(id) || live.contains(id)) {
            std::println(stderr, "{} is corrupt or table {} is missing", path, id);
            std::exit(1);
        }
        live.insert(id);
        levels_[level].push_back({id, std::make_unique<Table>(table_path(id).c_str())});
    }

    // orphans from a crash mid-compaction
    for (std::uint32_t id : on_disk) {
        if (!live.contains(id) && ::unlink(table_path(id).c_str()) == -1) {
            fail("unlink");
        }
    }

    std::sort(levels_[0].begin(), levels_[0].end(),
              [](const TableFile &a, const TableFile &b) { return a.id > b.id; });
    for (std::size_t level = 1; level < num_levels; level++) {
        std::sort(levels_[level].begin(), levels_[level].end(), [](const TableFile &a, const TableFile &b) {
            return a.table->smallest() < b.table->smallest();
        });
        for (std::size_t i = 1; i < levels_[level].size(); i++) {
            if (levels_[level][i - 1].table->largest() >= levels_[level][i].table->smallest()) {
                std::println(stderr, "{} is corrupt, tables overlap in level {}", path, level);
                std::exit(1);
            }
        }
    }
}

void Store::write_manifest() const {
    std::string buf;
    std::uint32_t count = 0;
    for (const auto &level : levels_) {
        count += static_cast<std::uint32_t>(level.size());
    }
    append_u32(buf, manifest_magic);
    append_u32(buf, count);
    for (std::size_t level = 0; level < num_levels; level++) {
        for (const auto &file : levels_[level]) {
            append_u32(buf, file.id);
            append_u32(buf, static_cast<std::uint32_t>(level));
        }
    }
    append_u32(buf, crc32c(buf));

    std::string path = dir_ + "/MANIFEST";
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        fail("open manifest");
    }
    write_all(fd, buf);
    if (::fsync(fd) == -1) {
        fail("fsync manifest");
    }
    ::close(fd);

    // tables have to be durable before the manifest points at them
    sync_dir(dir_);
    if (::rename(tmp.c_str(), path.c_str()) == -1) {
        fail("rename manifest");
    }
    sync_dir(dir_);
}

void Store::replay_wal() {
    std::string log;
    char chunk[64 * 1024];
    for (;;) {
        ssize_t n = ::read(wal_fd_, chunk, sizeof chunk);
        if (n == -1) {
            fail("read");
        }
        if (n == 0) {
            break;
        }
        log.append(chunk, static_cast<std::size_t>(n));
    }

    // TODO: corruption mid-log drops everything after it
    std::size_t offset = 0;
    while (offset < log.size()) {
        std::string_view rest = std::string_view(log).substr(offset);
        if (rest.size() < header_size) {
            break;
        }

        std::uint32_t crc = read_u32(rest.data());
        auto type = static_cast<std::uint8_t>(rest[4]);
        std::uint32_t key_len = read_u32(rest.data() + 5);
        std::uint32_t value_len = read_u32(rest.data() + 9);

        // summing the lengths could overflow on 32-bit
        std::size_t space = rest.size() - header_size;
        if (key_len > space || value_len > space - key_len) {
            break;
        }
        std::size_t record_size = header_size + key_len + value_len;
        if (crc32c(rest.substr(4, record_size - 4)) != crc) {
            break;
        }

        std::string_view key = rest.substr(header_size, key_len);
        std::string_view value = rest.substr(header_size + key_len, value_len);
        if (type == type_put) {
            insert(key, value);
        } else if (type == type_delete) {
            insert(key, std::nullopt);
        } else {
            // the crc checks out, so this is a bug. don't truncate
            std::println(stderr, "replay: unknown record type {} at offset {}", type, offset);
            std::exit(1);
        }
        offset += record_size;
    }

    if (offset < log.size() && ::ftruncate(wal_fd_, static_cast<off_t>(offset)) == -1) {
        fail("ftruncate");
    }
}

void Store::insert(std::string_view key, std::optional<std::string_view> value) {
    // overwrites get counted twice
    memtable_bytes_ += key.size() + (value ? value->size() : 0);
    if (value) {
        memtable_.insert_or_assign(std::string(key), std::string(*value));
    } else {
        memtable_.insert_or_assign(std::string(key), std::nullopt);
    }
}

void Store::after_write(std::size_t bytes) {
    if (memtable_bytes_ >= memtable_limit) {
        flush();
    }
    if (!compaction_) {
        return;
    }
    // stall writes until l0 catches up
    if (levels_[0].size() >= l0_stall_at) {
        while (compaction_ && levels_[0].size() >= l0_stall_at) {
            compaction_step(std::numeric_limits<std::size_t>::max());
        }
        return;
    }
    compaction_step(bytes * compaction_pace);
}

void Store::flush() {
    std::uint32_t id = next_table_id_++;
    std::string path = table_path(id);
    write_table(path.c_str(), memtable_);
    levels_[0].insert(levels_[0].begin(), {id, std::make_unique<Table>(path.c_str())});
    write_manifest();

    // fsync the truncate too, otherwise after a power cut the old wal replays over newer tables
    if (::ftruncate(wal_fd_, 0) == -1) {
        fail("ftruncate");
    }
    if (::fsync(wal_fd_) == -1) {
        fail("fsync wal");
    }
    memtable_.clear();
    memtable_bytes_ = 0;

    start_compaction();
}

// picks the level furthest over its limit
void Store::start_compaction() {
    if (compaction_) {
        return;
    }

    std::size_t level = 0;
    double best = static_cast<double>(levels_[0].size()) / l0_compact_at;
    for (std::size_t i = 1; i + 1 < num_levels; i++) {
        std::uint64_t bytes = 0;
        for (const auto &file : levels_[i]) {
            bytes += file.table->size();
        }
        double score = static_cast<double>(bytes) / static_cast<double>(max_bytes(i));
        if (score > best) {
            best = score;
            level = i;
        }
    }
    if (best < 1) {
        return;
    }

    // all of l0, or the next table round robin
    std::vector<const TableFile *> picked;
    if (level == 0) {
        for (const auto &file : levels_[0]) {
            picked.push_back(&file);
        }
    } else {
        const auto &files = levels_[level];
        auto it = std::find_if(files.begin(), files.end(), [&](const TableFile &f) {
            return f.table->largest() > compact_pointer_[level];
        });
        picked.push_back(it == files.end() ? &files.front() : &*it);
        compact_pointer_[level] = picked.front()->table->largest();
    }

    std::string lo = picked.front()->table->smallest();
    std::string hi = picked.front()->table->largest();
    for (const TableFile *file : picked) {
        lo = std::min(lo, file->table->smallest());
        hi = std::max(hi, file->table->largest());
    }
    std::vector<const TableFile *> below;
    for (const auto &file : levels_[level + 1]) {
        if (overlaps(*file.table, lo, hi)) {
            below.push_back(&file);
        }
    }

    // trivial move
    if (picked.size() == 1 && below.empty()) {
        std::uint32_t id = picked.front()->id;
        auto &from = levels_[level];
        auto it = std::find_if(from.begin(), from.end(), [&](const TableFile &f) { return f.id == id; });
        TableFile moved = std::move(*it);
        from.erase(it);
        auto &to = levels_[level + 1];
        auto at = std::upper_bound(to.begin(), to.end(), moved.table->smallest(),
                                   [](const std::string &k, const TableFile &f) { return k < f.table->smallest(); });
        to.insert(at, std::move(moved));
        write_manifest();
        start_compaction();
        return;
    }

    compaction_ = std::make_unique<Compaction>();
    compaction_->output_level = level + 1;
    for (const auto *group : {&picked, &below}) {
        for (const TableFile *file : *group) {
            compaction_->input_ids.push_back(file->id);
            compaction_->inputs.push_back(std::make_unique<TableIterator>(*file->table));
        }
    }
}

// merges ~budget bytes of input
void Store::compaction_step(std::size_t budget) {
    Compaction &c = *compaction_;
    std::size_t done = 0;
    while (done < budget) {
        TableIterator *best = nullptr;
        for (const auto &input : c.inputs) {
            if (input->valid() && (!best || input->entry().key < best->entry().key)) {
                best = input.get();
            }
        }
        if (!best) {
            finish_compaction();
            return;
        }

        const Entry &entry = best->entry();
        done += 8 + entry.key.size() + (entry.value ? entry.value->size() : 0);

        // drop tombstones when nothing older is below
        if (entry.value || deeper_levels_may_have(c.output_level, entry.key)) {
            if (!c.out) {
                c.out_id = next_table_id_++;
                c.out = std::make_unique<TableWriter>(table_path(c.out_id).c_str());
            }
            c.out->add(entry.key, entry.value);
            if (c.out->size() >= target_table_size) {
                c.out->finish();
                c.out.reset();
                c.outputs.push_back(c.out_id);
            }
        }

        c.key = entry.key;
        for (const auto &input : c.inputs) {
            if (input->valid() && input->entry().key == c.key) {
                input->next();
            }
        }
    }
}

void Store::finish_compaction() {
    std::unique_ptr<Compaction> c = std::move(compaction_);
    if (c->out) {
        c->out->finish();
        c->out.reset();
        c->outputs.push_back(c->out_id);
    }
    c->inputs.clear();

    std::vector<std::string> dead;
    for (auto &level : levels_) {
        std::erase_if(level, [&](const TableFile &f) {
            bool input = std::find(c->input_ids.begin(), c->input_ids.end(), f.id) != c->input_ids.end();
            if (input) {
                dead.push_back(f.table->path());
            }
            return input;
        });
    }

    auto &out = levels_[c->output_level];
    for (std::uint32_t id : c->outputs) {
        out.push_back({id, std::make_unique<Table>(table_path(id).c_str())});
    }
    std::sort(out.begin(), out.end(), [](const TableFile &a, const TableFile &b) {
        return a.table->smallest() < b.table->smallest();
    });

    // if we crash before the unlinks, load() deletes them
    write_manifest();
    for (const std::string &path : dead) {
        if (::unlink(path.c_str()) == -1) {
            fail("unlink");
        }
    }

    start_compaction();
}

const Table *Store::find_table(std::size_t level, std::string_view key) const {
    const auto &files = levels_[level];
    auto it = std::upper_bound(files.begin(), files.end(), key, [](std::string_view k, const TableFile &f) {
        return k < f.table->smallest();
    });
    if (it == files.begin()) {
        return nullptr;
    }
    const Table &table = *(it - 1)->table;
    return key <= table.largest() ? &table : nullptr;
}

bool Store::deeper_levels_may_have(std::size_t level, std::string_view key) const {
    for (std::size_t i = level + 1; i < num_levels; i++) {
        if (find_table(i, key)) {
            return true;
        }
    }
    return false;
}

std::string Store::table_path(std::uint32_t id) const { return std::format("{}/{:06}.sst", dir_, id); }

void Store::put(std::string_view key, std::string_view value) {
    write_record(wal_fd_, type_put, key, value);
    insert(key, value);
    after_write(key.size() + value.size());
}

void Store::remove(std::string_view key) {
    write_record(wal_fd_, type_delete, key, {});
    insert(key, std::nullopt);
    after_write(key.size());
}

std::optional<std::string> Store::get(std::string_view key) const {
    if (auto it = memtable_.find(key); it != memtable_.end()) {
        return it->second;
    }

    std::string value;
    auto check = [&](const Table &table) -> std::optional<std::optional<std::string>> {
        Lookup result = table.get(key, value);
        if (result == Lookup::found) {
            return std::optional<std::string>(value);
        }
        if (result == Lookup::deleted) {
            return std::optional<std::string>();
        }
        return std::nullopt;
    };

    for (const auto &file : levels_[0]) {
        if (auto result = check(*file.table)) {
            return *result;
        }
    }
    for (std::size_t level = 1; level < num_levels; level++) {
        if (const Table *table = find_table(level, key)) {
            if (auto result = check(*table)) {
                return *result;
            }
        }
    }
    return std::nullopt;
}

std::vector<std::size_t> Store::level_counts() const {
    std::vector<std::size_t> counts;
    for (const auto &level : levels_) {
        counts.push_back(level.size());
    }
    return counts;
}
