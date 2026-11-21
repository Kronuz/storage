# Source layout

storage is a header-only library: `storage.h` is the include path every user keeps.

| File | Holds |
|---|---|
| `storage.h` | The `Storage<Header, BinHeader, Footer, IO, LegacyHeader>` class template, and the includes of its dependencies |
| `storage_defs.h` | What the class builds on: the logging and IO-policy hooks, format constants and open flags, codec dispatch, volume lock helpers, the `Storage*` exception classes, `StorageMetaHead` and the reference header structs |
| `storage_io.h` | The default POSIX IO policy (`storage::DefaultIO`) |
| `storage_fs.h` | Path normalization |
| `storage_trace.h` | No-op logging macros |

## Kept whole

`storage.h` (about 1,370 lines) is one class template. Its members (open, volume scan and reclaim,
append, commit, read, the v1 fallback) share the volume state, and the v1/v2 paths are chosen
inside them with `if constexpr`, so no member can be read without the others. A cut would put a
fragment of the class body in a second file.
