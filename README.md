# lsmkv

LSM-tree key-value store in C++23. I wrote it to learn how storage engines like LevelDB and RocksDB work.

```cpp
Store db("data");
db.put("user:1", "ana");
db.remove("user:2");
std::optional<std::string> name = db.get("user:1");
```

Linux and x86-64 only, single-threaded.

## How it works

```
put(k, v)
  ├─ append to wal.log
  └─ insert into the memtable
       │ 4 MB
       ▼
     flush to an L0 table, clear the wal
       │ 4 tables
       ▼
     compact into L1 ... L6
```

Writes go to the WAL and to an in-memory hash map (the memtable). Once the memtable hits 4 MB it gets sorted and written out as a new table in L0. Deletes write a tombstone instead of removing anything, since the old value might be sitting in a table on disk.

Reads check the memtable, then the L0 tables newest first, then L1 to L6. Tables in L1 and below don't overlap, so there's only one table per level that could have the key. Each table has a bloom filter (10 bits per key), so most tables get skipped without reading from disk. If the filter says maybe, the table's index points to the 4 KB block the key would be in, and that block is read with `pread` and scanned.

Compaction is leveled and uses the same sizes as LevelDB. L0 gets compacted at 4 tables, L1 is 10 MB, and every level after that is 10x bigger. Output tables are 2 MB. The merge keeps the newest version of each key and drops tombstones once there's nothing older underneath them. If a table doesn't overlap anything in the next level, it just gets moved down.

There's no background thread. Compaction runs inside `put` in small steps, about 32 bytes of merging per byte written. If L0 gets to 12 tables, writes stall until compaction catches up.

## Files

```
data/
  wal.log       [crc u32][type u8][key_len u32][value_len u32][key][value] ...
  MANIFEST      list of live tables and their levels
  000123.sst    data blocks, bloom filter, index, key range, footer
```

Every log record, block, index and manifest is checksummed with crc32c (the SSE4.2 instruction).

## Crash safety

The WAL isn't fsynced on every write, so a power cut can lose the last few seconds of writes. If just the process crashes nothing is lost, since the kernel already has the data.

- A torn record at the end of the WAL fails its crc on startup and gets truncated.
- The MANIFEST decides which tables are live. It gets replaced by writing `MANIFEST.tmp`, fsyncing it and renaming it over the old one.
- On startup, any `.sst` file that isn't in the MANIFEST gets deleted. Those are leftovers from a flush or compaction that crashed halfway.
- The WAL only gets truncated after the new table and MANIFEST are fsynced. If it crashes in between, the WAL gets replayed into the memtable again, which is fine.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/store_test
```

Debug builds have ASan and UBSan turned on.

The tests are in `tests/store_test.cpp`. Most of them corrupt or truncate files to fake a crash. There's also a randomized one that runs 100k puts, deletes and gets against a `std::map` and reopens the store every 25k ops.
