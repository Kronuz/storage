> Status: CURRENT (2026-10-30) the streamed record write as built on `phase7c/l6-streamed-write`; workstream phase7c-l6, maintainer developer.

# Streamed record write

`Storage::StreamWriter` writes one record whose payload arrives in chunks, without holding it
whole. A host (Xapiand's upload path) calls `append` from a chore per chunk and `commit` from the
last one.

```
StreamWriter w(storage, size);   // begin: bin header written
w.append(chunk) ...              // each chunk to the volume, blocking
offset = w.commit();             // footer, pad, fsync data, publish the header
```

## Format: unchanged

The record is the ordinary framing: `StorageBinHeader`, payload, `StorageBinFooter`, padded to
`STORAGE_ALIGNMENT`, at the volume's append cursor. Differences from `write()` are in what is
chosen, not in the layout:

- Flags are 0: the payload is stored uncompressed. The header's `size` is the stored size, and a
  compressed size is not known when the header is written. `read()` already serves flags 0, so
  every existing reader reads a streamed record unchanged.
- `size` is a `uint32_t`, so a payload is at most `stream_max_size` (4 GiB - 1) bytes, framing the
  host adds included. `begin` throws `StorageTooLarge` above it; the host maps that to its own
  413. A record must also end below the volume's last block (`StorageEOF` at begin, before any
  byte is written).
- The footer's checksum is the XXH32 of the payload, accumulated per chunk with the same seed
  as `write()`. The footer type validates it on read as for any record.

For the same payload and no compression, a streamed record and a `write()` record are
byte-identical from header to footer.

## Write path and cursor

The writer fills the storage's own tail block (`buffer_curr`, at `buffer_offset`), starting at the
append cursor `hw_offset`. A full block is written with `pwrite` at its block offset; a chunk that
starts on a block boundary writes its whole blocks straight from the caller's memory. At commit
the footer is appended, the rest of the tail block is zeroed up to the next 8 bytes, the tail
block is written, and `hw_offset` advances by the record's length in alignment units. The
8-byte alignment and the block grid are those of `write()`; `commit` asserts the new cursor
equals the offset plus the record's aligned length.

## Visibility and crashes

The volume header carries the high-water mark and moves only in `Storage::commit()`, which
`StreamWriter::commit` calls after the cursor advance: fsync the data, rewrite block 0 with the
new mark, fsync it (the v2 protocol). Until the header write, a reader, or a restart, sees the
volume as it was. `test_stream` proves it with `CrashIO`: the image after every chunk, and after
every write the sequence issued (whole or torn to a sector), shows the record fully or not at
all, and fully only after the header write.

An abandoned writer (`abort()`, destruction, an exception out of the constructor) restores the
saved tail block, `buffer_offset` and free-block count. The bytes it wrote lie past the
high-water mark; the next write overwrites them. `Storage::close()` detaches a live writer;
its later calls throw `StorageClosedError`. While a writer is open, `write()` and `reopen()`
throw, and a second writer is refused: the host streams into a volume of its own.

## Orphans

A crash mid-upload leaves a volume file whose header still holds the initial mark (or no
header). A crash after `commit` but before the host names the record leaves a volume with a
record nobody references. `Storage::reclaim_volumes(pattern, keep)` runs at open, before any
writer: it unlinks volumes holding no record, and volumes holding records that `keep(n)`
rejects (the host knows which volumes its committed locators name). Files whose header is not a
valid v2 header, legacy v1 volumes among them, are never removed or offered to `keep`.

## Limits and non-goals

- Reads are unchanged: a streamed record reads back in chunks through `read(buf, n)`, as any
  uncompressed record.
- No compression, no resume, no shared-volume interleaving (one stream at a time per storage).
- `read()` bounds a record by the 64-bit byte offset (it used a 32-bit product, so a record
  starting past 4 GiB of byte offset read as EOF).
- Whether a blob engine replaces the host's `stored` store is a separate design; this adds one
  write path and no format.

Proof: `test/test_stream.cc` (`storage_stream`, `storage_stream_big`, `storage_stream_full`).
