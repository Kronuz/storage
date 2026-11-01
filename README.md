# storage

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/storage/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/storage/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Append-only blob store for C++20: stable record offsets, per-record compression, and a
crash-safe volume header. Write a record, get back its offset, and read it by that
offset later; records over 100 bytes are compressed with the codec you pick at open,
checksummed if your footer asks for it, and committed with the fsync policy you choose.
Reach for it for immutable records addressed by position: document stores, logs, and
write-ahead log segments.

![storage: a volume is a checksummed header block followed by 8-byte-aligned records](docs/storage.svg)

## ✨ Highlights

- **Stable offsets.** `write()` appends and returns the record's offset; a volume grows
  in `fallocate`d chunks up to 32 GiB (2³² 8-byte units).
- **Per-record codecs.** Zstandard, LZ4, or deflate, chosen at open; the codec id lives
  in each record, so one volume can mix them.
- **Crash-safe header.** Block 0 carries a magic, a commit counter, and an XXH32
  checksum, small enough to survive a torn 4 KB write.
- **Integrity hooks.** Your footer type receives an XXH32 of each record and can reject
  it as `StorageCorruptVolume`.
- **Durability flags.** `STORAGE_FULL_SYNC`, `STORAGE_NO_SYNC`, or `STORAGE_ASYNC_SYNC`
  to hand fsync to your own thread pool.
- **Pluggable framing and IO.** `Header`, `BinHeader`, and `Footer` are template
  parameters, and a fourth routes every file operation through your IO policy.
- **Reads Xapiand volumes.** Legacy v1 volumes open unchanged; build with
  `STORAGE_LEGACY_SUPPORT=0` to drop that path.

## 🚀 Quick start

```cpp
#include "storage.h"

#include <cstdio>

// The reference framing: a v2 header, a flags+size bin header, an empty footer.
using Blobs = Storage<StorageHeader, StorageBinHeader, StorageBinFooter>;

int main() {
	Blobs s("./", nullptr);
	s.open("vol.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS_ZSTD);
	uint32_t off = s.write("a record");    // the offset to read it back
	s.commit();                             // flush and fsync
	s.close();

	Blobs r("./", nullptr);
	r.open("vol.0", STORAGE_OPEN);
	r.seek(off);
	std::printf("%s\n", r.read().c_str());  // prints a record
}
```

Define your own `Header`, `BinHeader`, and `Footer` to add magic numbers, UUIDs, or
checksums; [examples/toy_wal.cc](examples/toy_wal.cc) and
[examples/toy_data_store.cc](examples/toy_data_store.cc) build a write-ahead log and a
document store that way.

## 📦 Installation

Requires a C++20 compiler, a POSIX system, and CMake 3.16 or newer. It brings
[compressors](https://github.com/Kronuz/compressors),
[errno-names](https://github.com/Kronuz/errno-names),
[strict-stox](https://github.com/Kronuz/strict-stox), and
[stringified](https://github.com/Kronuz/stringified) through FetchContent. CI builds and
tests it with GCC and Clang on Linux (x86_64, arm64), macOS (arm64, x86_64), and
FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(storage
  GIT_REPOSITORY https://github.com/Kronuz/storage.git
  GIT_TAG        7b4ce2c31082558671a574a9ab4fd7a7b1d58e56)  # or main, which tracks the tip
FetchContent_MakeAvailable(storage)

target_link_libraries(my_app PRIVATE storage::storage)
```

Then `#include "storage.h"`.

## 🎯 When to use it

Immutable records addressed by offset: document stores, logs, and write-ahead log
segments that are read back by position.

Not for:

- **Updates or deletes in place.** Records are never rewritten; compact by copying the
  live ones into a fresh volume.
- **Lookups by key.** An offset is the only address; keep your own index from key to
  offset.
- **Volumes moved across byte orders.** Headers, footers, and record structs are written
  in host byte order.

## ⚡ Performance

`storage_bench` writes and reads 50,000 records of 256 bytes and 200 records of 1 MiB
through one volume per codec, on one thread with a warm page cache. MB/s of
uncompressed data, write / read, the median of three runs; the ratio is record bytes
over file bytes.

| Scenario | Apple M4 Pro | Intel Core i9-9980HK | Ratio |
| --- | ---: | ---: | ---: |
| 256 B records, none | 191 / 190 | 113 / 71 | 0.75 |
| 256 B records, LZ4 | 141 / 173 | 81 / 69 | 0.98 |
| 256 B records, zstd | 59 / 55 | 38 / 46 | 1.23 |
| 1 MiB records, none | 346 / 4,876 | 321 / 1,029 | 0.77 |
| 1 MiB records, LZ4 | 427 / 1,949 | 321 / 658 | 1.84 |
| 1 MiB records, zstd | 454 / 1,262 | 280 / 557 | 3.81 |

Release builds with Apple clang 17 on both machines:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/storage_bench
```

Reads run 1.2x to 5.3x slower than the same records through liblz4 or libzstd and
`pread` alone, with no framing. [docs/perf/](docs/perf/README.md) has those raw rows,
deflate, and the run-to-run spread.

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): on-disk layout, write and read paths, durability
  and failure modes.
- [docs/INTEGRATION.md](docs/INTEGRATION.md): the IO policy, the async-fsync hook, and
  logging.
- [docs/mechanisms/streamed-write.md](docs/mechanisms/streamed-write.md): writing one
  record in chunks (`Storage::StreamWriter`) and reclaiming orphan volumes.
- [docs/perf/](docs/perf/README.md): the full benchmark table.
- [CHANGELOG.md](CHANGELOG.md): behavior changes and their measurements.
- [AGENTS.md](AGENTS.md): repository map and invariants.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#storage).

## 🛠️ Building and testing

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
./build/storage_bench
```

The tests cover round trips, multiple volumes, checksums, simulated crashes, a v2-only
build, and golden volumes on disk. `-DSTORAGE_FUZZ=ON` with Clang builds the libFuzzer
target.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) first, and the
[catalog's AGENTS.md](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md) for
the family's rules. CI must pass on every platform.

## 📄 License

MIT; see [LICENSE](LICENSE). Extracted from [Xapiand](https://github.com/Kronuz/Xapiand).
