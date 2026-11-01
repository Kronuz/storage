# AGENTS

Orientation for anyone (human or agent) working on this library.

## What this is

A header-only, append-only, compressed, multi-volume blob store extracted from
Xapiand. The whole engine is `storage.h`; everything else is a small support
header or a test. Read `ARCHITECTURE.md` for the on-disk format and the write/read
paths before changing the engine.

## File map

```
storage.h          The engine: Storage<Header, BinHeader, Footer, IO> template.
storage_io.h       DefaultIO: the portable, EINTR-safe POSIX IO policy.
storage_fs.h       Vendored normalize_path (pure-string path normalizer; os.path.normpath's form, the current directory is "." even for an empty path).
storage_trace.h    No-op L_* logging hooks (redirect via STORAGE_TRACE_HEADER).
likely.h           Vendored likely()/unlikely() branch hints.
test/test.cc       ctest: round-trip, multi-volume, checksum, write_file,
                   delete flag, and an 8-thread concurrency case (for TSAN).
test/test_crash.cc ctest: torn and interrupted commits against the v2 header.
test/crashio.h     CrashIO: an in-memory, write-logging IO policy for crash tests.
test/test_stream.cc ctest: the streamed record write (StreamWriter), orphan reclaim, sparse 4 GiB runs.
test/format_golden.cc ctest: volumes written by the reference engine (test/fixtures/, one per codec) read back record for record; the uncompressed volume is reproduced byte for byte. `--write DIR` regenerates the fixtures.
fuzz/              libFuzzer target and replay for the streamed write (-DSTORAGE_FUZZ=ON).
examples/bench.cc  Runnable throughput / compression-ratio benchmark, with raw codec + pwrite/pread rows as the floor.
examples/toy_wal.cc, toy_data_store.cc
                   Minimal v2 consumers (a WAL and a docstore).
docs/INTEGRATION.md IO policy, async-fsync hook, logging.
docs/storage.d2    README diagram; render: d2 --layout elk --sketch --theme 302 docs/storage.d2 docs/storage.svg
```

`Error` and `THROW` are NOT defined here: they come from the compressors library
(via `compressor_lz4.h`). Defining a second `class Error` would be an ODR clash.

## Dependencies (Kronuz family, via FetchContent at tip)

`compressors` (LZ4 + XXH32 + Error/THROW), `errno-names` (`error::name/
description`), `strict-stox` (`strict_stoull`), `stringified`. All header-or-
static; their include dirs propagate transitively, so `storage.h`'s
`#include "compressor_lz4.h"` / `error.hh` / `strict_stox.hh` / `stringified.hh`
just resolve.

## Conventions / invariants

- **Byte-identical on-disk format.** The engine logic is verbatim from Xapiand.
  Do not change block layout, alignment, buffering, compression framing, or the
  XXH32 seed/algorithm without a deliberate format-version plan — existing
  volumes must keep reading.
- **Two injection seams, nothing else host-specific.** IO goes through the `IO`
  policy; async fsync goes through the `StorageFsyncFn` hook. Resist adding new
  direct dependencies to the engine; extend a seam instead.
- **`STORAGE_DEFAULT_IO`** lets a host swap the default IO policy for every
  instantiation without touching call sites (Xapiand uses this).
- **Keep it ASAN/TSAN clean.** The engine has no shared/static state (the
  in-tree static fsyncher is gone). The concurrency test guards that; run the
  ASan and TSan builds (below) after engine changes.

## Build / test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build && ./build/storage_bench
```

Sanitizers, fuzzing and the benchmark are in the next section.

## Verification and benchmarks

The method shared by the Kronuz libraries is in
[docs/PERFORMANCE.md](https://github.com/Kronuz/cpp-libraries/blob/main/docs/PERFORMANCE.md).

- **Tests.** `ctest` runs ten tests. Oracles: `storage_format_golden` reads the volumes in
  `test/fixtures/` (written by the engine at `e4c5746`) and reproduces the uncompressed one byte
  for byte; `storage_crash` and `storage_stream` replay power loss through `test/crashio.h`;
  `storage_path` checks the path normalizer against a model and the digest of Python's
  `os.path.normpath` over every string of `a`, `b`, `.` and `/` up to 8 bytes.
- **Sanitizers.** CI (`.github/workflows/ci.yml`, which calls the shared
  [cmake.yml](https://github.com/Kronuz/cpp-libraries/blob/main/.github/workflows/cmake.yml))
  runs every test under ASan+UBSan and under TSan (`storage` has an 8-thread case), clang on Linux:

  ```sh
  CC=clang CXX=clang++ cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build build-asan && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
  CC=clang CXX=clang++ cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g -fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
  cmake --build build-tsan && TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" ctest --test-dir build-tsan --output-on-failure
  ```

- **Fuzzing.** `-DSTORAGE_FUZZ=ON`, with a clang that has libFuzzer, builds `storage_fuzz`
  (`fuzz/fuzz_stream.cc`: stream a record, flip a byte of the volume, reopen) and the CTest test
  `storage_fuzz_replay`, which runs the seeds in `fuzz/corpus/` (`s8` is the LZ4 bounds
  regression). Keep new inputs apart from the seeds:
  `./build-fuzz/storage_fuzz -max_total_time=600 new-corpus/ fuzz/corpus/`.
- **Benchmark.** `storage_bench` (`examples/bench.cc`, built with the tests, no option) prints
  write and read MB/s and the compression ratio per codec, each none, LZ4 and zstd row followed
  by a raw floor (the codec and `pwrite`/`pread` alone). Numbers and machines:
  `docs/perf/README.md`. To compare two commits, build it at each and run both with
  `tools/ab_compare.py` from a clone of [cpp-libraries](https://github.com/Kronuz/cpp-libraries)
  beside this one; it runs A, B and A again (the A/A control) every round, and `--column 2` reads
  the read MB/s (`--column 1` the write):

  ```sh
  git worktree add ../storage-base BASE_COMMIT
  cmake -S ../storage-base -B build-base -DCMAKE_BUILD_TYPE=Release && cmake --build build-base --target storage_bench
  python3 ../cpp-libraries/tools/ab_compare.py run --rounds 7 --format table --column 2 --higher-better \
    build-base/storage_bench build/storage_bench
  ```

## Codecs

Codec-in-header is implemented: the bin-header flags carry the compressed bit
(0x01) and the codec id in bits 2-4 (LZ4 = 0, ZSTD = 1, DEFLATE = 2). Open flags
pick the write codec (`STORAGE_COMPRESS_ZSTD` — the default — `STORAGE_COMPRESS_LZ4`
/ `STORAGE_COMPRESS_DEFLATE`); `read()` dispatches on the stored id. LZ4 = 0 keeps
pre-codec volumes readable. Compression uses the compressors library's one-shot
helpers (same framing as its streaming classes, so LZ4 is byte-identical to the
in-tree engine); `write()`/`read()` buffer the whole record rather than streaming
(records are bounded, and Zstandard has no streaming-from-fd class). `bench.cc`
compares the codecs. Adding a codec = a constant + two `switch` arms in
`storage_compress`/`storage_decompress` + an open-flag selector.
