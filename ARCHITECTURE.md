# Architecture

The storage engine is an append-only, block-structured blob store. One volume is
one file; a file is a sequence of fixed-size blocks (`STORAGE_BLOCK_SIZE`, 4 KiB).
Block 0 holds the volume `Header`; records ("bins") are packed into the blocks
that follow, each aligned to `STORAGE_ALIGNMENT` (8 bytes).

## On-disk layout

```
volume file
+------------------+  block 0
|     Header       |   head.offset = next free position (in 8-byte units)
+------------------+  block 1 .. N
|  BinHeader       |   per record: flags + size
|  payload         |   raw bytes, or an LZ4 frame if STORAGE_FLAG_COMPRESSED
|  Footer          |   optional XXH32 checksum
|  (alignment pad) |
|  BinHeader ...   |   next record
+------------------+
```

A record's **offset** (what `write()` returns and `read()`/`seek()` take) is its
position in 8-byte alignment units, so a 32-bit offset addresses up to 32 GiB per
volume. `head.offset` in the volume header is the running append cursor;
`commit()` writes it back to block 0 and fsyncs.

`Header`, `BinHeader`, and `Footer` are **template parameters**, not fixed
structs. The engine only calls their `init()` / `validate()` hooks, so a consumer
controls the exact framing: the reference structs in `storage.h` are minimal
(offset-only header, `flags`+`size` bin header, empty footer), while Xapiand's
WAL and data stores supply richer ones with magic numbers and XXH32 checksums.
That is how the same engine backs different on-disk formats.

## The write path

`write()` builds the bin in a double-buffered 4 KiB window (`buffer0`/`buffer1`):
it copies the bin header, then the payload (streamed straight from the LZ4
compressor when compressing), updating `head.offset` as blocks fill. Completed
blocks are written with `pwrite`; the partially-filled tail block stays in the
buffer until the next write or `commit()`. `write_file()` is the same path fed
from a file descriptor instead of a memory buffer. The footer checksum is XXH32
over the *uncompressed* bytes (or the compressor's running digest when
compressing).

## The read path

`read()` seeks to the bin offset, reads and `validate()`s the bin header (which
throws `StorageNotFound` on a deleted record), then streams the payload —
decompressing through the LZ4 decompressor when the compressed flag is set —
into the caller's buffer. After the payload it reads and `validate()`s the
footer, comparing the stored checksum against the one accumulated over the bytes
just read; a mismatch throws `StorageCorruptVolume`.

## Compression and checksums

LZ4 is the only codec today. The bin header's `STORAGE_FLAG_COMPRESSED` bit marks
per-record whether the payload is an LZ4 frame, so a volume can mix compressed
and raw records (records below `STORAGE_MIN_COMPRESS_SIZE` are stored raw even
with `STORAGE_COMPRESS`). The checksum is XXH32 via the compressors library's
canonical streaming implementation, which produces the same digests as upstream
xxHash, so checksums are byte-identical to the in-tree engine.

A planned follow-up replaces the single compressed/raw bit with a **codec byte**
in the bin header (NONE/DEFLATE/LZ4/ZSTD), so a volume can use any codec the
compressors library provides while old LZ4 volumes still decode by their existing
marker.

## Extraction notes (what changed from the in-tree engine)

The engine logic (block layout, alignment, buffering, compression framing,
checksums) is verbatim. Only the couplings were severed:

- **IO** — the 30 `io::*` calls became `IO::*` on an injected policy
  (`DefaultIO` by default). This is the one structural change to the engine's
  call sites.
- **Async fsync** — the in-tree static debouncer + thread pool + `opts.*`
  timings became a single injectable `StorageFsyncFn`; with no hook,
  `STORAGE_ASYNC_SYNC` falls back to a synchronous fsync.
- **Exceptions** — the `Storage*` hierarchy derives from the `Error` the
  compressors library already brings in via `compressor_lz4.h` (one `Error`, no
  ODR clash); a host redirects it through `COMPRESSORS_EXCEPTION_HEADER`. The
  in-tree `throw Xapian::DatabaseNotFoundError` became `THROW(StorageNotFound)`.
- **Logging** — the `L_*` family is no-op by default, redirected via
  `STORAGE_TRACE_HEADER`.
- **Filesystem** — `normalize_path` is vendored (`storage_fs.h`); volume
  discovery does its own `readdir` instead of pulling in `fs.hh`.
