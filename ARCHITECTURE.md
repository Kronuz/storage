# Architecture

The storage engine is an append-only, block-structured blob store. One volume is
one file; a file is a sequence of fixed-size blocks (`STORAGE_BLOCK_SIZE`, 4 KiB).
Block 0 holds the volume `Header`; records ("bins") are packed into the blocks
that follow, each aligned to `STORAGE_ALIGNMENT` (8 bytes).

## On-disk layout

```
volume file (v1)                     volume file (v2, dual-meta)
+------------------+  block 0        +------------------+  block 0  meta A
|     Header       |                 |  StorageMetaHead |   magic|version|txnid|
+------------------+  block 1 .. N   |  + consumer head |   offset|checksum ...
|  BinHeader       |   per record    +------------------+  block 1  meta B
|  payload         |                 |  StorageMetaHead |   (alternated per commit)
|  Footer          |                 +------------------+  block 2 .. N
|  (alignment pad) |                 |  BinHeader/payload|   records (same framing)
|  BinHeader ...   |                 |  Footer ...       |
+------------------+                 +------------------+
```

A record's **offset** (what `write()` returns and `read()`/`seek()` take) is its
position in 8-byte alignment units, so a 32-bit offset addresses up to 32 GiB per
volume. The **high-water mark** is the running append cursor: in v1 it lives in
the header's `head.offset`, written back to block 0 by `commit()`; in v2 it lives
in the meta's `offset`, written to the alternate meta block under a fsync barrier
(see "Durability and failure modes"). The engine tracks it in one place
(`hw_offset`) for both formats.

`Header`, `BinHeader`, and `Footer` are **template parameters**, not fixed
structs. The engine only calls their `init()` / `validate()` hooks, so a consumer
controls the exact framing: the reference structs in `storage.h` are minimal
(offset-only header, `flags`+`size` bin header, empty footer), while Xapiand's
WAL and data stores supply richer ones with magic numbers and XXH32 checksums. A
header that begins with a `StorageMetaHead meta` also opts the volume into the
crash-safe v2 format. That is how the same engine backs different on-disk formats.

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
- Reads are bounded by the committed high-water mark, so a preallocated or
  power-loss zero tail past that mark is `StorageEOF`, not a phantom empty record.
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

**Two on-disk formats: v1 (single header) and v2 (dual-meta, crash-safe).** A
header type opts a volume into **v2** simply by beginning with a `StorageMetaHead
meta` (magic `STORAGE_V2_MAGIC`, version, a monotonic `txnid`, the high-water
`offset`, and a whole-block `checksum`); the `storage_has_meta` trait picks the v2
path with `if constexpr`, so a **v1** header (the reference structs, `toy_wal`)
keeps the original single-in-place-header path unchanged. v2 is what new Xapiand
data volumes use; the engine still reads existing v1 volumes read-only through an
optional legacy-header template param.

**The v2 commit protocol.** A v2 volume keeps **two** meta blocks (block 0 and
block 1); record data starts at block 2. `commit()`:

1. `fsync`s the data blocks (barrier 1), so they are durable *before* the commit
   point that will reference them;
2. writes the *other* meta block with `txnid + 1`, the new high-water, and a fresh
   whole-block checksum;
3. `fsync`s that meta block (barrier 2).

`open()` reads both meta blocks and adopts the valid one with the highest `txnid`.
So a torn, lost, or bit-rotted write to the block being committed leaves the
previous committed meta (in the block it did not touch) intact, and a meta that
validates always points at durable data. `load_meta` also rejects a meta whose
high-water runs past EOF (a truncated tail) and a `version` it does not
understand. This is crash-consistent regardless of fsync timing; the async-fsync
hook keeps the same issue order (a single deferred fsync flushes data and meta
together, so a durable meta never outruns its data).

The whole matrix is proven in `test/test_crash.cc`: a `CrashIO` in-memory policy
logs every `pwrite`/`fsync`, so tests reconstruct the on-disk state after every
crash point (each write-log prefix, plus the next op torn to one 512 B sector) and
assert `open()` recovers to a consistent committed state -- never garbage, never a
crash -- alongside targeted torn-meta / both-corrupt / torn-data / truncated-tail
cases.

**Integrity of the records themselves is still opt-in.** The v2 meta is always
magic'd and checksummed by the engine, but per-record integrity comes from the
consumer's footer. The reference `StorageBinHeader`/`StorageBinFooter` carry no
per-record magic or checksum (the fields are present but commented out), so a
consumer using them as-is has no *record*-level corruption detection. Xapiand's
`WalBinHeader`/`WalBinFooter` (and the docstore's) do carry
`STORAGE_BIN_HEADER_MAGIC` + `STORAGE_BIN_FOOTER_MAGIC` + XXH32, so real records
are protected; the `toy_data_store` example shows the same on v2.

**Still open (follow-ups).**
- The Xapiand **WAL** is still on v1. It is a rescue log (temporary), and it
  already has record magic + checksum, prefix recovery, and corrupt-volume
  quarantine; moving it to v2 is delicate high-water/slot surgery best done on its
  own with a WAL crash test.
- **Tail-block write amplification:** `write()` re-`pwrite`s the current partial
  tail block on every call, so a 4 KiB block holding many small records is written
  once per record (page-cache writes, not fsync, but one syscall each). Since the
  tail block is not durable until `commit()`, only completed blocks need eager
  writes; flushing the final partial block in `commit()` would cut those syscalls
  without changing durability.
- **Integrity by default:** making the reference structs carry magic + checksum
  (so a new consumer is safe without opting in) is a reference-format change.

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
