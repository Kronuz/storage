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

The bin header's `STORAGE_FLAG_COMPRESSED` bit marks whether a record's payload is
compressed, and bits 2-4 hold the **codec id** (LZ4 = 0, ZSTD = 1, DEFLATE = 2).
The open flags pick the write codec (`STORAGE_COMPRESS` for LZ4,
`STORAGE_COMPRESS_ZSTD`, `STORAGE_COMPRESS_DEFLATE`); `read()` dispatches on the
stored id. Because LZ4 is codec 0, a volume written before codec-in-header (whose
records have the compressed bit set and the codec bits zero) reads back as LZ4
with no migration, and a single volume can mix codecs and raw records (records
below `STORAGE_MIN_COMPRESS_SIZE` are stored raw even with a compress flag set).

Compression goes through the compressors library's one-shot helpers
(`compress_lz4`/`compress_zstd`/`compress_deflate`), which produce the same block
framing as its streaming classes. So an LZ4 record written here is byte-identical
to one the in-tree streaming engine wrote. `write()` compresses the whole record
up front into one contiguous payload; `read()` reads the payload and decompresses
it once into a buffer, serving the result in chunks. Records are bounded, so the
buffering is cheap; it also removes the codec-specific streaming-from-fd classes
the engine used to need (Zstandard has no such class in the compressors library).

The checksum is XXH32 over the *uncompressed* bytes (the same for a raw or
compressed record), via the compressors library's canonical XXH32, which matches
upstream xxHash digests — so checksums are byte-identical to the in-tree engine.

## Durability and failure modes

An append-only store is easy to write and easy to read; the hard part is what
happens when a write is interrupted or a volume is damaged. The contract here:
damage must be **detected** (a `Storage*` exception, never a crash, an unbounded
allocation, or silently returned garbage), and every record committed **before**
the damage must remain readable. What backs that contract, and where the current
gaps are:

**What the reader guarantees.** `read()` throws, never misbehaves, on a torn or
corrupt record:

- A short read of the header, payload, or footer (a truncated tail from a torn
  write or power loss) throws `StorageCorruptVolume` (`Incomplete bin ...`).
- Reads are bounded by the committed high-water mark (`header.head.offset`), so a
  preallocated or power-loss zero tail past that mark is `StorageEOF`, not a
  phantom empty record.
- The size field is validated against that same high-water mark before it is
  trusted for I/O or allocation, so a garbled size (the bin header is not
  checksummed, and one flipped byte can turn it into ~4 GB) is a clean
  `StorageCorruptVolume`, not a multi-gigabyte allocation.
- `seek()` clears the per-record read state, so a read after an interrupted or
  partially-consumed prior read starts clean instead of reinterpreting the next
  record with a stale header.
- When the consumer supplies a checksummed footer (see below), bit rot inside a
  record trips `StorageCorruptVolume` and is isolated to that record; its
  neighbours, independently addressed, still read.

**Integrity is opt-in, and off by default.** The reference `StorageHeader`,
`StorageBinHeader`, and `StorageBinFooter` carry **no magic and no checksum** (the
fields are present but commented out). They are the faithful on-disk reference,
but a consumer that uses them as-is runs with no corruption detection, and a torn
tail can read back as silent garbage. A consumer that wants the guarantees above
must supply header/footer types that carry a magic and a footer checksum. Xapiand
does (`WalBinHeader`/`WalBinFooter` carry `STORAGE_BIN_HEADER_MAGIC` +
`STORAGE_BIN_FOOTER_MAGIC` + XXH32), so real WAL and docstore records are
protected; its replay recovers the good prefix and quarantines a corrupt volume
rather than deleting it.

**The commit protocol, and its ordering gap.** `write()` `pwrite`s data blocks
past the current high-water mark and advances `head.offset` in memory. `commit()`
then `pwrite`s the header (block 0, the new high-water mark) and issues a single
`fsync`/`full_fsync` (or hands the fd to the async-fsync hook). That single fsync
covers both the data and the header, but nothing orders the data *before* the
header the disk persists: on power loss mid-fsync the high-water mark can land on
disk pointing past data that did not. The per-record footer checksum is the only
thing that catches this, which is another reason the reference defaults (no
checksum) are unsafe. Two things would close it, both still open:

- A write barrier: `fsync` the data, *then* `pwrite` the header, *then* `fsync`
  the header, so the commit point is never durable before the data it references.
  (Interacts with the async-fsync hook, which deliberately defers the syscall.)
- An LMDB-style dual meta block: keep two header blocks, alternate on commit, each
  with a monotonic id + its own checksum, and on open pick the newest that
  validates. This makes a torn header write harmless regardless of fsync timing,
  and it also closes the last unprotected field, since today the header itself
  (most importantly `head.offset`) carries no checksum. It is a header-format
  change, so it needs a format-version bump.

**Efficiency note.** `write()` re-`pwrite`s the current partial tail block on every
call, so a 4 KiB block holding many small records is written once per record
(page-cache writes, not fsync, but still one syscall each). Since the tail block
is not durable until `commit()` anyway, only completed blocks need to be written
eagerly; flushing the final partial block in `commit()` would cut those syscalls
without changing durability. Open.


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
