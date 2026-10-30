> Status: CURRENT (2026-10-30) workstream phase7c-l6 (developer); branch `phase7c/l6-streamed-write`, worktree `~/code/Kronuz/storage-wt/l6-streamed-write`.

# Phase 7c L6: the streamed record write

Design: Xapiand `docs/design/phase7c.md` D9 and section 5's L6; binding constraints from
`docs/reviews/phase7c-design.md` Re-check 1, R9 (F19). Owner ruling: blobs stay minimal and
format-neutral. How it works: `docs/mechanisms/streamed-write.md`.

## State

Built and tested locally; pushed for CI. Awaiting the decoder-safety review and the architect's
scoped check; not merged.

## Done

See the commit list below and the proof table.

## Next

1. Fold in the decoder-safety review and the architect's scoped check.
2. Xapiand X3 wires it: reserve a volume, `StreamWriter` in `flush` chores, `reclaim_volumes` at open.

## Blocked on

Nothing.

## Open points for the owner and architect

1. `reclaim_volumes` needs the host's `keep(n)` for volumes that hold committed streamed records
   no locator names. Xapiand's open path supplies it.
2. Streamed records have no per-chunk checksum; the one footer XXH32 covers the whole record, so
   a corrupt chunk is found at the end of a read (as for every record).
