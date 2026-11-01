# Changelog

All notable changes to `storage` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-11-01

### Added

- `test/format_golden.cc` and `test/fixtures/`: one volume per codec written by the engine
  at `e4c5746`, before the 2026-10 changes, from a deterministic record set. The test reads
  every record back (whole and in 2 KB chunks) and requires the uncompressed volume to be
  reproduced byte for byte apart from its zero tail. Compressed payload bytes belong to the
  codec libraries, so those volumes are read, not compared. A writer that stamps the header's
  reserved field fails it; `test/test.cc` passes that build.
- `storage_bench` follows its none, LZ4 and zstd scenarios with a raw row: liblz4 or libzstd
  and `pwrite`/`pread` alone, no framing. The README states the numbers on an Apple M4 Pro
  and an Intel i9-9980HK: reads are 1.2x to 5.3x slower than the raw floor.

## 2026-10-31

### Changed

- The compressors pin moves to `fe0e712` (`40221d2` and the returned zstd string built
  with its constructor, which is 2x faster for a 64 KB frame on Apple M4).
- The compressors pin moves to `40221d2` (`9bd24af` and the zstd decode into a string).
  A zstd record is decoded into the reader's own `_record`, which keeps its capacity from
  record to record, so a read allocates nothing for the record. The returning decode
  allocated a fresh 1 MB string per record, and under glibc's default trim threshold the
  freed top of the heap went back to the kernel and was faulted in again: the 1 MB zstd
  read ran at 430-450 MB/s against 740 before `9bd24af`. It reads at 710-750 MB/s now
  (Linux x86_64 runner, `storage_bench`).
- The compressors pin moves to `9bd24af`. Its XXH32 computes the same digest on every
  host byte order (storage still writes its header, footer and record structs in host
  order, so a volume is not yet portable across byte orders). Its XXH32 is 2-6x faster
  on x86, which checksums every record, and the zstd and deflate one-shots no longer
  create a context or copy the result per call: on a 256 B record `decompress_zstd` takes
  32 ns instead of 1190 ns (Linux x86_64, `compressors_bench_oneshot`). The bytes
  written are identical.
- `storage_bench` on an Intel i9-9980HK (macOS, Apple clang 17), MB/s, before `9bd24af`
  against `40221d2`, two rounds: 256 B zstd write 34 to 39, read 44 to 47; 256 B deflate
  read 41 to 46; 1 MB zstd read 460 to 570; 1 MB deflate read 350 to 447; the lz4 and
  uncompressed rows are within 3% (1 MB zstd write 249-260 to 269 and, once, 190).
- `Storage::read(limit, args)` reads a small record from one stack chunk and a
  larger one straight into the result, which doubles as it fills. It used to
  build the result from 2 KB chunks, one `std::string` and two copies each. The
  chunked `read(buf, size, ...)` for streaming consumers and the on-disk format
  are unchanged.
- The compressors pin moves to the commit whose lz4 one-shots run without the
  stream classes (same bytes, 2-50x less fixed cost per call). Every lz4 record
  read and write goes through them.

## 2026-10-30

### Added

- `Storage::StreamWriter` writes one record in chunks, and `reclaim_volumes`
  drops volumes that hold no record (phase 7c L6). See
  [docs/mechanisms/streamed-write.md](docs/mechanisms/streamed-write.md).
- Standalone extraction from Xapiand's in-tree engine. The 30 `io::*` calls
  became `IO::*` on an injected policy; the static debouncer + thread pool +
  `opts.*` timings became the `StorageFsyncFn` hook; `throw
  Xapian::DatabaseNotFoundError` became `THROW(StorageNotFound)`; logging and
  `normalize_path` moved behind `STORAGE_TRACE_HEADER` and `storage_fs.h`.
  `write_file` buffers the whole file through `write()`, where the in-tree
  engine streamed it.

### Fixed

- `read()` bounded a record with a 32-bit product, so a record starting past
  4 GiB of byte offset read as EOF. It uses the 64-bit byte offset.

### Security

- The compressors pin moves to `dc4474b` (the LZ4 bounds fix), where
  `LZ4DecompressData` and `LZ4DecompressFile` reject a record length past the
  block buffer. The fuzz target no longer skips the codec flags byte, so a flip
  there reaches the codec decoders. The old crash input is regression seed `s8`:
  it aborts under ASan on the previous pin and runs clean on this one. 10 minutes
  of libFuzzer under ASan+UBSan with the flags byte included: 533k runs, no crash.
