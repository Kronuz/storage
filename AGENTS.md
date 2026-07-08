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
storage_fs.h       Vendored normalize_path (pure-string path normalizer).
storage_trace.h    No-op L_* logging hooks (redirect via STORAGE_TRACE_HEADER).
likely.h           Vendored likely()/unlikely() branch hints.
test/test.cc       ctest: round-trip, multi-volume, checksum, write_file,
                   delete flag, and an 8-thread concurrency case (for TSAN).
examples/bench.cc  Runnable throughput / compression-ratio benchmark.
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
  ASAN and TSAN builds (Homebrew LLVM) after engine changes.

## Build / test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build && ./build/storage_bench
# sanitizers (Homebrew LLVM):
cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -g" && cmake --build build-asan && ./build-asan/storage_test
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread  -g" && cmake --build build-tsan && ./build-tsan/storage_test
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
