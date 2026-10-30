> Status: CURRENT (2026-10-30) code review of Kronuz/storage `phase7c/l6-streamed-write` (storage:90aec94..43aff45, LZ4 pin-and-fuzz commit ignored), focus durability.

# Phase 7c L6 (streamed record write): code review

**Verdict: APPROVE WITH CONDITIONS.** The write path, commit order, cursor and alignment hold. One blocker: `reclaim_volumes` deletes a volume a live writer owns, and the writer's `commit()` still returns success (L6-B1). Preallocation: nothing blocks the lead's ruling; see L6-N4.

Probes live in the worktree `~/code/Kronuz/storage-wt/l6-streamed-write/.scratch/review-l6/` (kept): `probe.cc` + `build.sh` (ASan+UBSan, P1-P8), `falloc.cc`, `falloc2.cc`, and the sanitizer build under `build/`. `test_stream` (default mode) passes under ASan+UBSan in that build.

## What holds (probed, not redone)

- Mixed volumes (P6, `probe.cc`): 40 seeds x 400 operations of `write()`, streamed record, abandoned stream (half appended, `abort()`), `commit()` and close/reopen, sizes around 0, 8, the 4 KiB block (4064..4096) and 100 KB, random chunking. Every record reads back byte-equal after reopen, and each record's offset equals the offset a `write()`-only volume gives for the same payload sequence. So the cursor (`StreamWriter::commit`, storage.h:~1385) and 8-byte alignment stay in step with `write()`.
- Publication order (storage.h:1149-1196, called from the writer's `commit()`): tail block `pwrite`, then `do_sync`, `write_meta`, `do_sync`. The header moves only after the data sync, and the `hw_offset` advance is in memory until then. A durable header always points at durable data. Same barrier as `write()`.
- Reclaim of a committed volume (P1): `keep == nullptr` keeps it, `keep` true keeps it, a throwing `keep` propagates with the file intact; only header-only and zero-length files go without a `keep`. Symlinks and directories named like volumes are skipped (`DT_REG`), a hard link loses only the name reclaim unlinks, a mode-0 file is kept (unreadable = Unknown), an unwritable directory makes `unlink` fail silently (nothing reported removed, no throw) (P5).

## Blockers

### L6-B1. `reclaim_volumes` deletes a live writer's volume and the writer's `commit()` still succeeds

Probe `probe.cc` P2 and P3:

```
P2: Storage A open("data.5") fresh (header = START); Storage B reclaim_volumes("data.", keep=false)
    reclaim removed 1 volume(s); A stream+commit() returned offset 512; file exists afterwards: 0
P3: StreamWriter mid-upload (3000 of 6000 bytes appended); reclaim; append, commit() returns; file gone
```

`volume_state` (storage.h:~1455) calls any file whose header shows the initial mark "Empty", and `reclaim_volumes` unlinks it with no check that anyone holds it (the stat-then-unlink is also not atomic). A fresh volume looks Empty from `open()` (`initialize_file` writes the START header) until its first commit, which is exactly the state of every in-flight upload. The writer keeps its fd to the unlinked inode, `Storage::commit()` fsyncs and writes the header there, and returns success: an acknowledged commit that no reader or restart can find.

The note says reclaim "runs at open, before any writer", but nothing enforces it, and X3's uploads reserve volumes from an atomic `next_volume` while other storages on the same directory exist (readers open in `query` chores; a shard reopened by the registry while an upload is in flight reaches the same call). The `keep` predicate cannot help: Empty volumes are removed without asking it, and an in-flight upload's committed-but-unnamed volume is rejected by `keep` (D9 names the locator only after the last chunk).

Required fix (any one that makes misuse fail instead of lose data):
1. An advisory lock: a writable `open()` takes `flock(fd, LOCK_SH | LOCK_NB)` for its lifetime (the lock belongs to the open file description, so it also protects within one process), and `volume_state`/`reclaim_volumes` open the file and skip it unless `flock(LOCK_EX | LOCK_NB)` succeeds, holding that lock across the `unlink`. A test: probe P2 and P3 as regression tests, expecting the volume to survive and the writer's commit to be readable.
2. Or, if the lock is rejected, a hard contract in the API comment and the note that `reclaim_volumes` must be called only when the process has no writer or upload on the directory, plus X3 owning a test for it. This does not make the loss impossible, so prefer 1.

## Conditions (for landing, or recorded for X3)

1. L6-B1 fixed, with P2/P3 as tests.
2. (X3) `keep(n)` must be true for every volume a committed locator names **and** every locator the WAL or an unapplied batch can still replay. The library cannot know; state it in the API comment (it says only "the host knows").
3. (X3 or this branch) Directory durability, L6-N1.

## Non-blocking notes

### L6-N1. No directory fsync anywhere; a streamed volume is a fresh file every time

`grep fsync storage.h`: only `do_sync` (the file fd, storage.h:1127-1146). `open()` creates the file with `O_CREAT` (storage.h:673) and nothing syncs the parent directory, so the new volume's directory entry is not guaranteed durable when `commit()` returns. On Linux ext4/xfs the file fsync normally commits the create in the same journal transaction, so this is not a measured loss; POSIX does not promise it, and macOS `fsync` is not even flushed to media unless `STORAGE_FULL_SYNC` is set. A normal `write()` volume has this gap once per volume; a streamed upload pays it per upload, and the record is acknowledged to a client. Suggested: when `open()` created the file, `commit()` fsyncs the directory fd once (flag `created` is already computed at storage.h:~665). Not run: a power-cut test (no way to test on this host); CrashIO models writes and tears, not directory entries.

### L6-N2. `STORAGE_ASYNC_SYNC` returns from `commit()` before the data is durable

P8: with the async hook, `commit()` calls the hook twice (data barrier, then header) and returns before either has completed, so "after `commit()` returns, the record is durable" holds only with the synchronous flags, and the data-before-header order is then the hook's job. This is `Storage::commit` behavior shared with `write()`, not new, but X3 must not use the async flag for uploads it acknowledges, or must wait for the hook's completion. State it in the note.

### L6-N3. `reclaim_volumes` matches names more loosely than a destructive call should (P4)

With `pattern = "data"` (no dot), P4 removed empty files named `database.4`, `data_backup.9`, `data.bak.7`, `data.+8`, `data. 6`, `data.007`, and `data.-1`, the last reported as volume 18446744073709551615 because `strict_stoull` accepts a sign and spaces. Same matcher as `get_volumes_range` (storage.h:1522-1543), so it is consistent, but there it only picks a number. Here a wrong `n` goes to `keep`, and `data.007` and `data.7` share `n == 7`. Require the name to be exactly `<pattern>` followed by decimal digits (no sign, no leading zeros), and test that `pattern` must end in the separator or be matched whole. Also `DT_UNKNOWN` (some NFS/XFS setups) skips every file, so reclaim silently does nothing there; fall back to `fstatat`.

### L6-N4. Preallocation: nothing blocks "exact size, no flag"; four things to do

P7: after a 64 MiB streamed commit the file is 83.0 MiB (+30.0%), and on this macOS host 163 MiB are allocated (`probe`, `st_blocks`). `Storage::commit()` ends with `growfile()` (storage.h:1193), which grows by `STORAGE_BLOCKS_GROWTH_FACTOR` when `free_blocks <= 4`, so any commit after a stream triggers it.
1. The writer knows `hdr + size + footer + tail pad` at begin; `fallocate` that (rounded up to a block, plus the tail block the commit writes) in the constructor, after the room check (`StorageEOF`) and before the first `pwrite`. Make an `fallocate` failure at begin throw (`StorageIOError`) rather than warn, so a full disk refuses the upload before the first byte; today `growfile` only warns (storage.h:518-521).
2. Skip `growfile` for the stream's own commit (a private "sealed" member, set by the writer, cleared after) and nowhere else: `growfile` re-derives `free_blocks` from the file size, so a later `write()` on the same volume (the shared writer reopening the highest volume after a restart, F19) still grows normally. No format or header change.
3. `abort()` should `ftruncate` back to the pre-begin size (or the host deletes the volume, as D9 says for a disconnect); after a crash the file has the header-only mark and `reclaim_volumes` removes it, size not mattering.
4. Pre-existing, in the way on macOS: `IO::fallocate` (`storage_io.h:180-190`) passes `F_PEOFPOSMODE` with length `offset + len`, so it allocates `offset + len` bytes from EOF, not `len`. `falloc2.cc`: asked for 16 MiB at offset 64 MiB, 80 MiB were allocated. That is the 2x in P7 and would make an "exact" preallocation of a file with a header block still over-allocate on APFS. Fix `len` (library `storage_io.h`, not this branch's diff).

### L6-N5. Smaller

1. `assert` on the cursor in `commit()` (storage.h:~1385) vanishes under `NDEBUG`; Release has no check that `hw_offset` equals the record's aligned end. Cheap to make a runtime check that closes the storage on mismatch.
2. A failed `append` (ENOSPC in `put`) calls `io_fail`, which `close()`s the storage: the uncommitted bytes are abandoned, earlier committed records stay (the close commits the old mark). Correct, but the host sees every later call as `StorageClosedError`; document that a failed append ends the storage.
3. `reclaim_volumes` throws `StorageNotFound` when the directory cannot be opened, which also covers EACCES; the message is misleading.

## For the architect (design)

D9 names the locator only after the last chunk, so between `commit()` and the document's apply the volume is "committed but unnamed" and reclaim-eligible. That is safe only if reclaim cannot run in that window (L6-B1 lock; host: no reclaim while an upload group is live). If X3 wants reclaim at times other than first open, it needs the lock, not a convention.

## Not run

1. Power-loss or directory-entry crash testing (CrashIO does not model it).
2. TSan, libFuzzer, the 4 GiB-1 `storage_stream_big`, the full ctest suite, other platforms (CI said green; not re-verified).
3. Xapiand's `keep` predicate and open path (X3 does not exist yet).
4. FreeBSD/Linux `fallocate` behavior (L6-N4 measured on macOS/APFS only).
