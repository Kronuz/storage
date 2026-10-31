# Changelog

All notable changes to `storage` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-10-31

### Changed

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
