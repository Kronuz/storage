/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// The definitions Storage<> builds on: the logging and IO-policy hooks, the on-disk
// format constants and open flags, codec dispatch, the exception classes and the
// reference header structs. The engine class itself is in storage.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <sys/types.h>

#include "compressor_lz4.h"      // for compress_lz4 / decompress_lz4 + XXH32, Error / THROW
#include "compressor_zstd.h"     // for compress_zstd / decompress_zstd
#include "compressor_deflate.h"  // for compress_deflate / decompress_deflate

// Logging hooks. No-ops by default (storage_trace.h); a host redirects the whole
// L_* family to its own logger by defining STORAGE_TRACE_HEADER to a header path.
#ifdef STORAGE_TRACE_HEADER
#  include STORAGE_TRACE_HEADER
#else
#  include "storage_trace.h"
#endif

// Exception surface (Error base + THROW macro): the engine reuses the one the
// compressors library already brings in through compressor_lz4.h above, rather
// than defining a second `class Error` in the global namespace (that would be an
// ODR clash). So the redirect knob is the compressors lib's
// COMPRESSORS_EXCEPTION_HEADER: standalone it resolves to the compressors lib's
// vendored minimal exception.h; a host (Xapiand) defines it to its own located
// exceptions, and the Storage* hierarchy below derives from that single Error.
// The Storage* classes are defined further down, after compressor_lz4.h has made
// Error / THROW available.

// IO policy header. Defaults to storage_io.h, which defines storage::DefaultIO (a
// portable EINTR-safe POSIX layer). A host with an instrumented IO layer points
// STORAGE_IO_HEADER at its own header, which both declares its policy type and
// #defines STORAGE_DEFAULT_IO to it (so the include below brings the type into
// scope before the template default uses it).
#ifdef STORAGE_IO_HEADER
#  include STORAGE_IO_HEADER
#else
#  include "storage_io.h"
#endif

// The engine routes every file operation through an injected IO policy (the 4th
// template parameter). It defaults to storage::DefaultIO; a host overrides the
// default for all instantiations at once via STORAGE_DEFAULT_IO (typically set by
// the STORAGE_IO_HEADER above).
#ifndef STORAGE_DEFAULT_IO
#define STORAGE_DEFAULT_IO ::storage::DefaultIO
#endif


// Enable reading legacy v1 volumes. On by default; define it to 0 to build a
// v2-only engine (the legacy header type and the v1 read fallback are compiled
// out, and every Storage<> header is then required to be v2). Existing v1 support
// is confined to the blocks guarded by this macro, so it is also easy to delete.
#ifndef STORAGE_LEGACY_SUPPORT
#define STORAGE_LEGACY_SUPPORT 1
#endif

// Volume header magic. STORAGE_MAGIC identifies the current (v2) format: a single
// checksummed header (StorageMetaHead) at block 0, crash-safe by sector atomicity.
#define STORAGE_MAGIC 0x584B5632  // "2VKX" on little-endian disk; Xapiand/Kronuz v2
#define STORAGE_BIN_HEADER_MAGIC 0x2A
#define STORAGE_BIN_FOOTER_MAGIC 0x42

// The seed for record footer checksums: a fixed constant used by every record.
// v1 and v2 records are byte-identical, so a record validates the same way under
// either format. This is a standalone value (historically it happened to match
// the v1 header magic), independent of the legacy format below.
#define STORAGE_CHECKSUM_SEED 0x02DEBC47

#if STORAGE_LEGACY_SUPPORT
// _LEGACY_STORAGE_MAGIC identifies the legacy v1 format: a bare offset header
// carrying this magic, with no header checksum. Kept only so old volumes can
// still be read, and confined to STORAGE_LEGACY_SUPPORT so a v2-only build drops
// every trace of v1 (this magic included).
#define _LEGACY_STORAGE_MAGIC 0x02DEBC47
#endif  // STORAGE_LEGACY_SUPPORT

#define STORAGE_BLOCK_SIZE (1024 * 4)
#define STORAGE_ALIGNMENT 8

#define STORAGE_BUFFER_CLEAR 1
#define STORAGE_BUFFER_CLEAR_CHAR '\0'

#define STORAGE_BLOCKS_GROWTH_FACTOR 1.3f
#define STORAGE_BLOCKS_MIN_FREE 4

#ifndef STORAGE_LAST_BLOCK_OFFSET  // overridable, so a test can fill a volume cheaply
#define STORAGE_LAST_BLOCK_OFFSET (static_cast<off_t>(std::numeric_limits<uint32_t>::max()) * STORAGE_ALIGNMENT)
#endif

#define STORAGE_START_BLOCK_OFFSET (STORAGE_BLOCK_SIZE / STORAGE_ALIGNMENT)

// Both v1 and v2 keep a single header at block 0 and start record data at block 1.
// v2 differs only in that its header carries a StorageMetaHead (magic + version +
// checksum): the header is crash-safe by single-sector atomicity as long as the
// bytes that change between commits fit in block 0's first 512-byte sector (they
// do for a small header like the docstore's), so a torn 4 KB write to block 0
// still leaves a valid old-or-new header. Offsets count STORAGE_ALIGNMENT units.

#define STORAGE_MIN_COMPRESS_SIZE 100


constexpr int STORAGE_OPEN             = 0x00;  // Open an existing database.
constexpr int STORAGE_WRITABLE         = 0x01;  // Opens as writable.
constexpr int STORAGE_CREATE           = 0x02;  // Automatically creates the database if it doesn't exist
constexpr int STORAGE_CREATE_OR_OPEN   = 0x03;  // Create database if it doesn't already exist.
constexpr int STORAGE_ASYNC_SYNC       = 0x04;  // fsync (or full_fsync) is async: commit() returns before the data is durable, so never use it for a record acknowledged to a client
constexpr int STORAGE_FULL_SYNC        = 0x08;  // Try to ensure changes are really written to disk.
constexpr int STORAGE_NO_SYNC          = 0x10;  // Don't attempt to ensure changes have hit disk.
constexpr int STORAGE_COMPRESS_LZ4     = 0x20;  // Compress with LZ4 (legacy: fastest, lowest ratio).
constexpr int STORAGE_COMPRESS_ZSTD    = 0x40;  // Compress with Zstandard (the default: best size/speed balance).
constexpr int STORAGE_COMPRESS_DEFLATE = 0x80;  // Compress with Deflate (zlib).
constexpr int STORAGE_COMPRESS_MASK    = STORAGE_COMPRESS_LZ4 | STORAGE_COMPRESS_ZSTD | STORAGE_COMPRESS_DEFLATE;

constexpr int STORAGE_FLAG_COMPRESSED  = 0x01;
constexpr int STORAGE_FLAG_DELETED     = 0x02;
constexpr int STORAGE_FLAG_MASK        = STORAGE_FLAG_COMPRESSED | STORAGE_FLAG_DELETED;

// Per-record codec, stored in the bin-header flags (bits 2-4) when
// STORAGE_FLAG_COMPRESSED is set. LZ4 is id 0 so volumes written before
// codec-in-header (when LZ4 was the only codec) keep reading with no migration;
// this id is a fixed on-disk value, not the runtime default (which is Zstd). The
// mask sits above STORAGE_FLAG_MASK and below the free high bits.
constexpr uint8_t STORAGE_CODEC_SHIFT   = 2;
constexpr uint8_t STORAGE_CODEC_MASK    = 0x1C;   // bits 2,3,4
constexpr uint8_t STORAGE_CODEC_LZ4     = 0;      // fixed on-disk id (pre-codec volumes)
constexpr uint8_t STORAGE_CODEC_ZSTD    = 1;
constexpr uint8_t STORAGE_CODEC_DEFLATE = 2;


// Codec dispatch. The compressors library's one-shot helpers produce the same
// block framing as its streaming classes, so an LZ4 record written here is
// byte-identical to one written by the in-tree streaming engine. NONE returns the
// data unchanged (the caller only compresses when a codec is selected).
inline std::string storage_compress(uint8_t codec, std::string_view data) {
	switch (codec) {
		case STORAGE_CODEC_ZSTD:    return compress_zstd(data);
		case STORAGE_CODEC_DEFLATE: return compress_deflate(data);
		case STORAGE_CODEC_LZ4:
		default:                    return compress_lz4(data);
	}
}

inline std::string storage_decompress(uint8_t codec, std::string_view data) {
	switch (codec) {
		case STORAGE_CODEC_ZSTD:    return decompress_zstd(data);
		case STORAGE_CODEC_DEFLATE: return decompress_deflate(data);
		case STORAGE_CODEC_LZ4:
		default:                    return decompress_lz4(data);
	}
}

// The same into `out`, which a reader keeps from record to record: zstd decodes into its
// capacity, so a steady stream of reads allocates nothing for the record.
inline void storage_decompress(uint8_t codec, std::string_view data, std::string& out) {
	if (codec == STORAGE_CODEC_ZSTD) {
		decompress_zstd(data, out);
	} else {
		out = storage_decompress(codec, data);
	}
}

// The codec to write a record with, given the open flags; -1 = no compression.
// If several are set, Zstandard (the default) wins, then Deflate, then LZ4.
inline int storage_write_codec(int flags) {
	if (flags & STORAGE_COMPRESS_ZSTD)    { return STORAGE_CODEC_ZSTD; }
	if (flags & STORAGE_COMPRESS_DEFLATE) { return STORAGE_CODEC_DEFLATE; }
	if (flags & STORAGE_COMPRESS_LZ4)     { return STORAGE_CODEC_LZ4; }
	return -1;
}

// Volume locks and truncation: the IO policy's own when it defines them (storage::DefaultIO does;
// the crash-test policy fakes them), else the POSIX calls on the real descriptor.
template <typename IO> inline int storage_lock_shared(int fd) noexcept {
	if constexpr (requires { IO::lock_shared(fd); }) { return IO::lock_shared(fd); }
	else { return ::storage::posix::lock_shared(fd); }
}
template <typename IO> inline int storage_lock_exclusive(int fd) noexcept {
	if constexpr (requires { IO::lock_exclusive(fd); }) { return IO::lock_exclusive(fd); }
	else { return ::storage::posix::lock_exclusive(fd); }
}
template <typename IO> inline int storage_ftruncate(int fd, off_t len) noexcept {
	if constexpr (requires { IO::ftruncate(fd, len); }) { return IO::ftruncate(fd, len); }
	else { return ::storage::posix::ftruncate(fd, len); }
}



class StorageException : public Error {
public:
	template<typename... Args>
	StorageException(Args&&... args) : Error(std::forward<Args>(args)...) { }
};


class StorageIOError : public StorageException {
public:
	template<typename... Args>
	StorageIOError(Args&&... args) : StorageException(std::forward<Args>(args)...) { }
};


class StorageClosedError : public StorageIOError {
public:
	template<typename... Args>
	StorageClosedError(Args&&... args) : StorageIOError(std::forward<Args>(args)...) { }
};


class StorageNotFound : public StorageException {
public:
	template<typename... Args>
	StorageNotFound(Args&&... args) : StorageException(std::forward<Args>(args)...) { }
};


class StorageEOF : public StorageException {
public:
	template<typename... Args>
	StorageEOF(Args&&... args) : StorageException(std::forward<Args>(args)...) { }
};


class StorageNoFile : public StorageException {
public:
	template<typename... Args>
	StorageNoFile(Args&&... args) : StorageException(std::forward<Args>(args)...) { }
};


class StorageCorruptVolume : public StorageException {
public:
	template<typename... Args>
	StorageCorruptVolume(Args&&... args) : StorageException(std::forward<Args>(args)...) { }
};


// Thrown by a streamed write whose declared size cannot be a record: the payload
// size field is 32 bits (see Storage::stream_max_size).
class StorageTooLarge : public StorageException {
public:
	template<typename... Args>
	StorageTooLarge(Args&&... args) : StorageException(std::forward<Args>(args)...) { }
};


// Thrown when a writable open targets a legacy v1 volume. New writes always go to
// fresh v2 volumes, so a consumer catches this to roll forward to the next volume.
class StorageLegacyReadOnly : public StorageIOError {
public:
	template<typename... Args>
	StorageLegacyReadOnly(Args&&... args) : StorageIOError(std::forward<Args>(args)...) { }
};


// Async-fsync hook. The engine fsyncs in-thread by default. A host that wants to
// offload fsync to a batched/background implementation (Xapiand uses a debounced
// thread pool keyed on the fd) installs one of these via the Storage constructor;
// then, with STORAGE_ASYNC_SYNC set, commit() hands (fd, full_fsync) to the hook
// instead of blocking on the syscall. With no hook installed, STORAGE_ASYNC_SYNC
// degrades gracefully to a synchronous fsync. A host plugs its own debouncer and
// thread pool in here.
using StorageFsyncFn = std::function<void(int fd, bool full_fsync)>;


// v2 crash-safe header prefix. A header type opts a volume into the v2 format
// simply by embedding a StorageMetaHead named `meta` as its FIRST member (see the
// storage_has_meta trait below); the rest of the header (uuid, revision, slots,
// ...) follows and is covered by the same checksum. Legacy v1 headers have no
// `meta` and keep the original path.
//
// v2 keeps a SINGLE header at block 0, rewritten in place on commit, data at block
// 1 -- same layout as v1. Its crash safety comes from single-sector (512 B)
// write atomicity: as long as the header bytes that change between commits fit in
// block 0's first sector (28 B of meta + a small uuid, for the docstore), a torn
// 4 KB block-0 write still yields a valid old-or-new header (the trailing bytes are
// invariant padding). The magic + checksum additionally catch bit rot and, for a
// consumer whose header spans sectors and CAN tear (the WAL's slot array), turn a
// torn header into a clean StorageCorruptVolume that the consumer quarantines.
// `txnid` is a monotonic commit counter (informational / debugging).
#pragma pack(push, 1)
struct StorageMetaHead {
	uint32_t magic;      // STORAGE_MAGIC (identifies a v2 header)
	uint16_t version;    // meta format version (currently 2)
	uint16_t reserved;   // reserved for flags; always zero
	uint64_t txnid;      // monotonic commit counter
	uint64_t offset;     // committed high-water mark, in STORAGE_ALIGNMENT units
	uint32_t checksum;   // XXH32 over the whole block with these 4 bytes treated as zero

	static constexpr uint16_t CURRENT_VERSION = 2;
};
#pragma pack(pop)

// Compile-time detection of a v2 header: it has a member `meta` of type
// StorageMetaHead. Used with `if constexpr` so the v1 and v2 paths share one
// engine with no runtime cost and no change to existing v1 consumers.
template <typename T, typename = void>
struct storage_has_meta : std::false_type { };
template <typename T>
struct storage_has_meta<T, std::void_t<decltype(std::declval<T&>().meta)>>
	: std::is_same<std::remove_cv_t<std::remove_reference_t<decltype(std::declval<T&>().meta)>>, StorageMetaHead> { };
template <typename T>
inline constexpr bool storage_has_meta_v = storage_has_meta<T>::value;


// ===========================================================================
// Reference header structs.
//
// RECOMMENDED (v2): a header that begins with a StorageMetaHead opts the volume
// into the crash-safe v2 format -- magic + version + checksum + the data->header
// fsync barrier in commit(). Real consumers add their own fields (uuid, revision,
// ...) after the meta; StorageHeader is the minimal ready-made one.
// ===========================================================================
struct StorageHeader {
	StorageMetaHead meta;
	char padding[STORAGE_BLOCK_SIZE - sizeof(StorageMetaHead)];

	void init(void* /*param*/, void* /*args*/) { }      // the engine stamps meta
	void validate(void* /*param*/, void* /*args*/) { }  // the engine validates meta
};
static_assert(sizeof(StorageHeader) == STORAGE_BLOCK_SIZE, "v2 header must be one block");


// ---- LEGACY v1 -- do not use for new volumes ------------------------------
// _LegacyStorageHeader is the legacy single-header format: a bare high-water offset
// with NO magic and NO checksum, so it cannot detect a corrupt or torn header.
// It is kept ONLY so a v2-capable consumer can still READ pre-existing v1 volumes
// (pass it as the Storage<> `StorageLegacyHeader` template param). New volumes
// must use a StorageMetaHead-prefixed header (see StorageHeader above); v1
// support may be dropped in a future version, and can be compiled out today by
// defining STORAGE_LEGACY_SUPPORT to 0.
// ---------------------------------------------------------------------------
#if STORAGE_LEGACY_SUPPORT
struct _LegacyStorageHeader {
	struct StorageHeaderHead {
		uint32_t offset;  // required (the high-water mark)
	} head;

	char padding[(STORAGE_BLOCK_SIZE - sizeof(_LegacyStorageHeader::StorageHeaderHead)) / sizeof(char)];

	void init(void* /*param*/, void* /*args*/) {
		head.offset = STORAGE_START_BLOCK_OFFSET;
	}

	void validate(void* /*param*/, void* /*args*/) { }
};
#endif  // STORAGE_LEGACY_SUPPORT


#pragma pack(push, 1)
struct StorageBinHeader {
	// uint8_t magic;
	uint8_t flags;  // required
	uint32_t size;  // required

	void init(void* /*param*/, void* /*args*/, uint32_t size_, uint8_t flags_) {
		// magic = STORAGE_BIN_HEADER_MAGIC;
		size = size_;
		flags = (0 & ~STORAGE_FLAG_MASK) | flags_;
	}

	void validate(void* /*param*/, void* /*args*/) {
		// if (magic != STORAGE_BIN_HEADER_MAGIC) {
		// 	THROW(StorageCorruptVolume, "Bad bin header magic number");
		// }
		if (flags & STORAGE_FLAG_DELETED) {
			THROW(StorageNotFound, "Bin deleted");
		}
	}
};


struct StorageBinFooter {
	// uint32_t checksum;
	// uint8_t magic;

	void init(void* /*param*/, void* /*args*/, uint32_t  /*checksum_*/) {
		// magic = STORAGE_BIN_FOOTER_MAGIC;
		// checksum = checksum_;
	}

	void validate(void* /*param*/, void* /*args*/, uint32_t /*checksum_*/) {
		// if (magic != STORAGE_BIN_FOOTER_MAGIC) {
		// 	THROW(StorageCorruptVolume, "Bad bin footer magic number");
		// }
		// if (checksum != checksum_) {
		// 	THROW(StorageCorruptVolume, "Bad bin checksum");
		// }
	}
};
#pragma pack(pop)


