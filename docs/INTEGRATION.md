# Integrating storage

The engine has two injection seams and a logging hook, so it depends on nothing
host-specific.

## Injection seams

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
