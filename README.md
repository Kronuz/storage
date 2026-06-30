# storage

A compressed, append-only, multi-volume blob store, extracted and modernized
from [Xapiand](https://github.com/Kronuz/Xapiand).

It stores opaque binary records in fixed-size blocks inside volume files, returns
a stable offset for each record, and reads any record back by that offset. Large
records are transparently LZ4-compressed; every record carries a header and an
(optional) footer checksum. The on-disk format is **byte-identical** to the
in-tree Xapiand engine, so existing volumes read unchanged.

The engine is one header-only template:

```cpp
template <typename Header, typename BinHeader, typename Footer,
          typename IO = storage::DefaultIO>
class Storage;
```

`Header`, `BinHeader`, and `Footer` are the on-disk record framing (you supply
them; init/validate hooks let you add magic, UUIDs, or checksums). `IO` is the
file-operation policy. Both are injection seams, so the engine itself depends on
nothing Xapiand-specific.

```cpp
#include "storage.h"

// The library's reference framing (offset-only header, flags+size bin header,
// empty footer). Define your own to add magic / checksums.
using Blobs = Storage<StorageHeader, StorageBinHeader, StorageBinFooter>;

Blobs s("/var/data/", nullptr);
s.open("vol.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS);
uint32_t off = s.write("a record");   // returns the offset to read it back
s.commit();                            // flush + fsync (see the sync flags)
s.close();

Blobs r("/var/data/", nullptr);
r.open("vol.0", STORAGE_OPEN);
r.seek(off);
std::string data = r.read();           // == "a record"
```

## What you get

- **Append-only volumes.** `write()` appends a record and returns its offset;
  `write_file()` appends a file's contents. Volumes grow in `fallocate`d chunks
  and roll at a 32-bit block ceiling.
- **Transparent compression.** With `STORAGE_COMPRESS`, records over
  `STORAGE_MIN_COMPRESS_SIZE` are LZ4-compressed; the bin header records whether
  a given record is compressed, so reads dispatch automatically. (LZ4 only for
  now; a codec-in-header refactor for zstd/deflate is a planned follow-up.)
- **Integrity.** Each record can carry an XXH32 footer checksum, validated on
  read (the reference `StorageBinFooter` stores none; supply a footer that does).
  A flipped byte surfaces as `StorageCorruptVolume`.
- **Multi-volume discovery.** `get_volumes_range("data.")` scans the base
  directory and returns the lowest/highest volume numbers for a name pattern.
- **Durability controls.** `STORAGE_FULL_SYNC` (F_FULLFSYNC where available),
  `STORAGE_NO_SYNC`, and `STORAGE_ASYNC_SYNC` (offload to the host's batched
  fsync via the hook below).

## The two injection seams

**IO policy (`IO`, the 4th template parameter).** Every file operation routes
through it: `open/close/lseek/read/write/pread/pwrite/fsync/full_fsync/fallocate/
fadvise`. The default, `storage::DefaultIO`, is a clean, portable, EINTR-safe
POSIX layer. A host with its own instrumented IO (retry policy, error injection,
metrics) passes its own struct with the same 11 static methods. To override the
default for *all* instantiations at once, point `STORAGE_IO_HEADER` at a header
that declares your policy type and `#define`s `STORAGE_DEFAULT_IO` to it (so the
type is in scope before the template default uses it) — Xapiand does exactly
this to route every storage IO through its `io.cc`.

**Async-fsync hook (constructor argument).** By default `commit()` fsyncs
in-thread. Pass a `StorageFsyncFn` (`void(int fd, bool full_fsync)`) to offload
it; with `STORAGE_ASYNC_SYNC` set, `commit()` hands the fd to your hook instead
of blocking on the syscall. Xapiand wires this to a debounced thread pool. With
no hook installed, `STORAGE_ASYNC_SYNC` degrades to a synchronous fsync.

## Logging

The engine emits diagnostics through an `L_*` family that is **no-op by
default** (`storage_trace.h`), so it has no logging dependency. To route them to
your logger, define `STORAGE_TRACE_HEADER` to a header that provides the macros,
or define the `L_*` macros before including `storage.h`.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build          # round-trip, multi-volume, checksum, concurrency
./build/storage_bench           # write/read throughput + compression ratio
```

Header-only; consume it via FetchContent and link `storage::storage`. Its
dependencies (the Kronuz family: `compressors`, `errno-names`, `strict-stox`,
`stringified`) come along transitively. The test suite is ASAN- and TSAN-clean.

## License

MIT. See the headers.
