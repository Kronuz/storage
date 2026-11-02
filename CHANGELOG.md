# Changelog

All notable changes to `storage` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Upgrading a pin from before 2026-10-30: [the consumer upgrade note](https://github.com/Kronuz/cpp-libraries/blob/main/docs/UPGRADING-2026-10.md#storage) lists what changes for callers, how to find affected inputs, the commits not to pin, and the speedup.

## 2026-11-01

### Fixed

- The vendored `normalize_path` returns the lexical normal form of Python's `os.path.normpath`,
  and the current directory has one spelling, `.`: the same change as in `fs`, which carries
  the same function. Before, the empty path was `/` (the closing `/` appended to nothing, so
  `Storage("", ...)` opened `/vol.0` at the root of the filesystem), `.` with `keep_slash`
  was `""`, and `x/..` was `x`: the pop loop never removed the first component of a relative
  path (`a/../b` was `a/b`). `""`, `.`, `./`, `x/..`, `x/../`, `./x/..` are now `.` (`./` with
  `slashed`), so a `Storage` with an empty, `.` or `a/..` base path keeps `./` and opens its
  volumes as `./vol.0` in the working directory; `""` is not a path that `open` or `stat`
  accepts. A `..` also removes the first component (`a/../b` is `b`), a relative path keeps
  the `..` it cannot remove (`../x` stays `../x`), the root absorbs it (`/..` is `/`), a
  leading `./` is dropped, and a component of three or more dots is a name. A run of leading
  `/` is one root (`//x` is `/x`; `os.path.normpath` keeps two). A default-constructed view is
  the empty path. `storage_path` checks a stack-of-components model against the function over
  every string of `a`, `b`, `.` and `/` up to length 8 and 20,000 random ones in all four flag
  combinations, that a second normalization changes nothing, that the results over the 87,380
  exhaustive strings hash to the digest of `os.path.normpath`'s own results, a table of named
  inputs, the `Storage` base path for each spelling, and that every non-empty input without a
  `.` gives the bytes the old loop gave. With the old `storage_fs.h` it fails.

### Changed

- The vendored `normalize_path` follows the fs library's one pass (fs `CHANGELOG.md`, 2026-11-01): each
  name is copied to the output as it is read and kept or dropped at its `/`, a `..` that pops no longer
  scans back over a `..` prefix, and the string overload builds a path of up to 510 bytes on the stack and
  copies it once. The bytes are the same as the loop of 8934dbc on every input: `test/test_path.cc`
  compares them in all four flag combinations over every string of `/`, `.`, `a`, `b` up to length 10 and
  of `/`, `.`, `a` up to length 12, 800,000 random strings of names and of bytes, and every path length from 0
  to 600, on exact-size heap blocks. A `Storage` calls it once, in its constructor, so the gain is the
  library's: on the fs bench (the same loop, M4 Pro and i9) the loop is 1.1x to 1.9x faster than at START
  on 12 of its 14 rows on the M4 and on all 14 on the i9; `np_keep_slash` ties on the M4 and `a/b/c/d` is
  0.92x of START there (fs issue #1).

- `Storage::read()` (a whole record) sizes its result once, from the record, and fills it in
  place: a compressed record is decoded straight into the result and an uncompressed one is
  read straight into it. `cc36919` had made the result grow by doubling `resize` and copy the
  decoded record in; each doubling zero-filled the new capacity (the last one, for a 1 MiB record,
  1 MiB of pages the read never touched), so a whole read of a compressed record was slower than
  before that commit on the i9 (0.66x lz4, 0.72x zstd, 0.75x deflate of START). A whole read of a
  1 MiB record now allocates the record's size, plus the compressed payload and what the codec
  needs: 1.05 MB (none), 1.69 (lz4), 1.22 (zstd), 2.27 (deflate), against 4.21, 5.89, 4.38 and
  6.48 MB. Reading 1 MiB records whole, ns per MiB (best of three rounds of 60 records, START
  `7b9abd1` / `0c19966` / now), Apple M4 Pro: none 338579 / 186992 / 146610, lz4 464940 / 453312 /
  404462, zstd 764114 / 748457 / 696613, deflate 519833 / 506242 / 456995; Intel i9-9980HK: none
  1009832 / 808666 / 305434, lz4 929661 / 1401993 / 591983, zstd 1198584 / 1672803 / 988217,
  deflate 1458653 / 1942153 / 1060176. Reading in 2 KB chunks, and the format, are unchanged.
  A record whose chunks a caller began to read is finished in chunks as before. The read is
  split into `begin_record`, `decompress_record` and `end_record`, which the chunked read
  uses too, so both end a damaged record with the same exception and message; a payload
  is read in pieces of at most 1 GiB (macOS refuses a larger count).
- The cost of `217e731` on the M4 is a second flush: `commit()` fsyncs the volume directory
  once after it created the file, between the data sync and the header write, so a record a
  commit acknowledged cannot vanish with its directory entry. On APFS that is 17 to 35 ms per
  new volume: the "small blobs" write rows (50000 x 256 B into a fresh 12 MB volume) took
  4.0 to 4.4 ms per MB without it and 7.8 to 8.4 with it (median of 15 volumes, two runs, Apple M4
  Pro; 0.70 to 0.92 of START in the gate), and 200 MiB volumes do not show it. Syncing the
  directory before the data sync instead (5.2 to 7.1) does not cut it; the durability fix stays. It is done only for a volume the storage created and only once, so a
  volume that exists pays nothing.
- The comment in `CMakeLists.txt` said that LZ4 is the only codec, and that zstd and deflate are
  a follow-up. A record is stored raw or with LZ4, Zstandard (the default) or Deflate; the codec
  id is in the bin-header flags (`storage.h`), and the comment and the dependency note
  (`compressor_lz4.h`, `compressor_zstd.h`, `compressor_deflate.h`) say so.
- The errno-names pin moves to `0ec0801`, whose `error::description` is the libc's text on glibc
  (it was `"Unknown error: N"` for every errno there); the engine's I/O error logs name the
  failure.

### Added

- `test/test_read.cc` (CTest `storage_read`): a whole `read()` against the chunked read and the
  data for every codec and sizes from 0 to 3 MiB + 7 around the 2 KB chunk, the compression
  threshold and the MiB, text and random bytes; a record read part way by chunks and finished
  whole; the records in sequence up to EOF; damaged volumes (a flipped payload, size, flags or
  footer byte, a truncation in the header, payload or footer) that end the same way either way,
  with the same exception and message; and a counting `operator new` that bounds what a whole
  read allocates by what the codec's own decode and the payload need (the test fails with
  `cc36919`'s read: 4 failures, 4.2 to 6.5 MB for a 1 MiB record).
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
