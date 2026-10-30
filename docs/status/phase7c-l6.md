> Status: CURRENT (2026-10-30) workstream phase7c-l6 (developer); branch `phase7c/l6-streamed-write`, worktree `~/code/Kronuz/storage-wt/l6-streamed-write`.

# Phase 7c L6: the streamed record write

Design: Xapiand `docs/design/phase7c.md` D9 and section 5's L6; binding constraints from
`docs/reviews/phase7c-design.md` Re-check 1, R9 (F19). Owner ruling: blobs stay minimal and
format-neutral. How it works: `docs/mechanisms/streamed-write.md`.

## State

Built, tested, pushed; CI dispatched (see Done). Awaiting the decoder-safety review and the
architect's scoped check; not merged.

## API

```cpp
Storage::StreamWriter w(storage, size);   // begin: size known up front, <= Storage::stream_max_size
w.append(chunk);                          // blocking write of one chunk (a host calls it from a chore)
uint32_t off = w.commit();                // footer, pad, fsync data, publish the header; read(off) works
// abort() or destruction abandons it; nothing was published.
storage.reclaim_volumes("data", keep);    // at open, before writers: drops volumes with no record
```

Throws `StorageTooLarge` above 4 GiB - 1 (Xapiand maps it to 413; its framing bytes count),
`StorageEOF` when the record cannot end below the volume's last block.

## Done

Commits (storage repo): `storage:90aec94` the writer, reclaim, reader-bound fix, tests;
`storage:9ba91ed` volume-full test, mechanism note, ledger. Later commits on the branch fix what
the sanitizers and the fuzzer found (below) and add the fuzz target.

| Claim | Evidence (`test/test_stream.cc` unless noted) |
|---|---|
| Byte-equal read-back around block, chunk, 16 MiB cap, 2x cap | `test_roundtrips`: sizes 0..9, 4070..4110, 8170..8200, 1 MiB +-1, 16 MiB +-1, 32 MiB +-1; fixed and jittered chunks |
| Same bytes as `write()`, existing readers unchanged | `test_compat`: record bytes equal a raw `write()`; flags byte 0; mixed volume read by `seek`+`read()` |
| 4 GiB - 1 record, and 4 GiB + 1 refused | `storage_stream_big` (sparse; about 150 MB on disk): header-to-footer read-back; `test_limits_and_misuse` |
| A record starting past 4 GiB of byte offset reads | `storage_stream_big`: three 1.6 GiB records, then two small ones |
| Nothing visible before commit; crash at every chunk boundary and write | `test_crash_sweep` (CrashIO): image after each append, every log prefix, torn to 512 B: record absent, or whole after the header write only |
| Abandoned writer leaves nothing and the storage is intact | `test_abandon` (with an uncommitted record in the tail block), `test_limits_and_misuse` |
| Orphans reclaimed at open | `test_reclaim`: a writer killed by `_exit` mid-upload (fork), an unheadered file, an unnamed committed volume; foreign files kept |
| Volume-full refusal at begin | `storage_stream_full` (2 MiB cap) |

Mutants (each restored; all fail the named tests): checksum not accumulated, bulk write without
advance, no alignment pad, abort without tail restore, publish before commit, commit without
`Storage::commit`, limit off by one in both directions, `write()` during a stream, overrun,
short commit, compressed flag on a streamed header, 32-bit reader bound, no room check, three
reclaim variants plus "any header is empty", close leaving the writer attached, `growfile`
int overflow, codec error escaping. 22 of 22 killed (`.scratch/mutants.py` in the worktree).

Fixes found on the way, each with a test that fails when reverted:

1. `read()`: `limit * STORAGE_ALIGNMENT` wrapped in 32 bits; a record starting past 4 GiB of byte
   offset read as EOF. Now 64-bit.
2. `growfile()`: `total_blocks * STORAGE_BLOCK_SIZE` overflowed `int` once a volume passed about
   1.6 GiB (UBSan found it); no preallocation was requested. Now `off_t`.
3. `read()`: a codec error on a damaged record escaped as the codec's own exception. Now
   `StorageCorruptVolume`.

Sanitizers: ASan+UBSan build of the whole suite (all 9 tests incl. the fuzz replay) clean.
libFuzzer (`fuzz/fuzz_stream.cc`, ASan+UBSan): 200k runs clean over a built-then-damaged image,
reclaim and open. `cpp-libraries/tools/preflight.sh . mac`: mac-hardened ok; mac-asan-ubsan
needs `SDKROOT=$(xcrun --show-sdk-path)` here (Homebrew LLVM 23 cannot find the SDK headers
otherwise); with it, see the ASan line above.

## Findings outside this step

1. **Kronuz/compressors, `LZ4DecompressData::next()` (`compressor_lz4.cc`, near `read_bin`)**:
   `cmpBytes` (u16) is checked against the input but not against `cmpBuf`'s capacity, so a
   damaged LZ4 stream writes past `cmpBuf` (ASan heap-buffer-overflow, WRITE of 2304 bytes).
   Reachable from storage when a record's flags byte turns damaged into "LZ4 compressed" (a bit
   flip). Reproducer: `~/code/Kronuz/storage/.scratch/l6/compressors-lz4-heap-overflow.crash`
   (fuzz input). The fuzz target skips the flags byte until it is fixed in that repository.
2. `commit()` preallocates 30% of the file after every commit (`growfile`). For a streamed volume
   that is never appended again it is wasted disk (about 1.3 GiB after a 4 GiB upload). Not
   changed here; the host can pass `STORAGE_NO_SYNC`-style handling no better, so a
   "no preallocation" open flag is a design question for the owner.

## Next

1. Decoder-safety review and the architect's scoped check.
2. Xapiand X3 wires it: reserve a volume, `StreamWriter` in `flush` chores, `reclaim_volumes` at open.

## Blocked on

Nothing.

## Open points

1. `reclaim_volumes` needs the host's `keep(n)` for volumes that hold committed streamed records
   no locator names. Xapiand's open path supplies it.
2. Streamed records carry one footer XXH32 over the payload, no per-chunk checksum, so damage shows
   at the end of a read (as for every record).
