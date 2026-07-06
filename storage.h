/*
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

#pragma once

#include <cassert>               // for assert
#include <cstddef>               // for offsetof (v2 header layout assert)
#include <cstdint>               // for uint32_t / uint64_t (meta header fields)
#include <cstring>               // for memset, memcpy
#include <dirent.h>              // for opendir, readdir, closedir (volume scan)
#include <errno.h>               // for errno
#include <functional>            // for std::function (the async-fsync hook)
#include <limits>                // for std::numeric_limits
#include <memory>
#include <string>                // for std::string
#include <string_view>           // for std::string_view
#include <type_traits>           // for the v2-header detection trait (if constexpr)
#include <utility>               // for std::declval
#include <unistd.h>

#include "compressor_lz4.h"      // for compress_lz4 / decompress_lz4 + XXH32 (compressors lib)
#include "compressor_zstd.h"     // for compress_zstd / decompress_zstd
#include "compressor_deflate.h"  // for compress_deflate / decompress_deflate
#include "error.hh"              // for error::name, error::description (errno-names lib)
#include "likely.h"              // for likely, unlikely (vendored)
#include "storage_fs.h"          // for storage::normalize_path (vendored)
#include "strict_stox.hh"        // for strict_stoull (strict-stox lib)
#include "stringified.hh"        // for stringified (stringified lib)

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


// Volume header magic. STORAGE_V1_MAGIC identifies the legacy single-header format
// (one in-place header block at offset 0); STORAGE_V2_MAGIC identifies the v2
// dual-meta format (two alternating, checksummed meta blocks -- see StorageMetaHead).
// The value 0x02DEBC47 is unchanged from the original STORAGE_MAGIC, so existing v1
// volumes and record checksums (which seed XXH32 with it) stay byte-compatible.
#define STORAGE_V1_MAGIC 0x02DEBC47
#define STORAGE_V2_MAGIC 0x584B5632  // "2VKX" on little-endian disk; Xapiand/Kronuz v2
#define STORAGE_BIN_HEADER_MAGIC 0x2A
#define STORAGE_BIN_FOOTER_MAGIC 0x42

// The seed for record footer checksums. A fixed constant (the v1 magic value) shared
// by v1 and v2 records, which are byte-identical, so a record written under either
// format validates the same way.
#define STORAGE_CHECKSUM_SEED STORAGE_V1_MAGIC

#define STORAGE_BLOCK_SIZE (1024 * 4)
#define STORAGE_ALIGNMENT 8

#define STORAGE_BUFFER_CLEAR 1
#define STORAGE_BUFFER_CLEAR_CHAR '\0'

#define STORAGE_BLOCKS_GROWTH_FACTOR 1.3f
#define STORAGE_BLOCKS_MIN_FREE 4

#define STORAGE_LAST_BLOCK_OFFSET (static_cast<off_t>(std::numeric_limits<uint32_t>::max()) * STORAGE_ALIGNMENT)

#define STORAGE_START_BLOCK_OFFSET (STORAGE_BLOCK_SIZE / STORAGE_ALIGNMENT)

// v2 dual-meta layout: blocks 0 and 1 are the two alternating meta snapshots, so
// record data starts at block 2 (v1 keeps a single header block and starts at
// block 1). Offsets are counted in STORAGE_ALIGNMENT units, matching head.offset.
#define STORAGE_META_BLOCKS 2
#define STORAGE_START_BLOCK_OFFSET_V2 ((STORAGE_META_BLOCKS * STORAGE_BLOCK_SIZE) / STORAGE_ALIGNMENT)

#define STORAGE_MIN_COMPRESS_SIZE 100


constexpr int STORAGE_OPEN             = 0x00;  // Open an existing database.
constexpr int STORAGE_WRITABLE         = 0x01;  // Opens as writable.
constexpr int STORAGE_CREATE           = 0x02;  // Automatically creates the database if it doesn't exist
constexpr int STORAGE_CREATE_OR_OPEN   = 0x03;  // Create database if it doesn't already exist.
constexpr int STORAGE_ASYNC_SYNC       = 0x04;  // fsync (or full_fsync) is async
constexpr int STORAGE_FULL_SYNC        = 0x08;  // Try to ensure changes are really written to disk.
constexpr int STORAGE_NO_SYNC          = 0x10;  // Don't attempt to ensure changes have hit disk.
constexpr int STORAGE_COMPRESS         = 0x20;  // Compress data in storage (LZ4 -- the back-compat default).
constexpr int STORAGE_COMPRESS_ZSTD    = 0x40;  // Compress with Zstandard instead of LZ4.
constexpr int STORAGE_COMPRESS_DEFLATE = 0x80;  // Compress with Deflate instead of LZ4.
constexpr int STORAGE_COMPRESS_MASK    = STORAGE_COMPRESS | STORAGE_COMPRESS_ZSTD | STORAGE_COMPRESS_DEFLATE;

constexpr int STORAGE_FLAG_COMPRESSED  = 0x01;
constexpr int STORAGE_FLAG_DELETED     = 0x02;
constexpr int STORAGE_FLAG_MASK        = STORAGE_FLAG_COMPRESSED | STORAGE_FLAG_DELETED;

// Per-record codec, stored in the bin-header flags (bits 2-4) when
// STORAGE_FLAG_COMPRESSED is set. LZ4 is 0 so volumes written before
// codec-in-header (LZ4 was the only codec then) keep reading as LZ4 with no
// migration. The mask sits above STORAGE_FLAG_MASK and below the free high bits.
constexpr uint8_t STORAGE_CODEC_SHIFT   = 2;
constexpr uint8_t STORAGE_CODEC_MASK    = 0x1C;   // bits 2,3,4
constexpr uint8_t STORAGE_CODEC_LZ4     = 0;      // back-compat default
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

// The codec to write a record with, given the open flags; -1 = no compression.
// The explicit zstd/deflate selectors win over the plain (LZ4) STORAGE_COMPRESS.
inline int storage_write_codec(int flags) {
	if (flags & STORAGE_COMPRESS_ZSTD)    { return STORAGE_CODEC_ZSTD; }
	if (flags & STORAGE_COMPRESS_DEFLATE) { return STORAGE_CODEC_DEFLATE; }
	if (flags & STORAGE_COMPRESS)         { return STORAGE_CODEC_LZ4; }
	return -1;
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


// Async-fsync hook. The engine fsyncs in-thread by default. A host that wants to
// offload fsync to a batched/background implementation (Xapiand uses a debounced
// thread pool keyed on the fd) installs one of these via the Storage constructor;
// then, with STORAGE_ASYNC_SYNC set, commit() hands (fd, full_fsync) to the hook
// instead of blocking on the syscall. With no hook installed, STORAGE_ASYNC_SYNC
// degrades gracefully to a synchronous fsync. This is the seam that replaces the
// in-tree debouncer + thread pool + opts the standalone engine no longer carries.
using StorageFsyncFn = std::function<void(int fd, bool full_fsync)>;


// v2 crash-safe meta block. A v2 volume keeps TWO of these (block 0 and block 1)
// and alternates which one a commit writes; open() picks the one with the highest
// `txnid` whose `checksum` validates. A header type opts a volume into the v2
// format simply by embedding a StorageMetaHead named `meta` as its first member
// (see the storage_has_meta trait below); everything else about that header type
// (uuid, revision, slots, ...) follows the meta and is covered by the same
// whole-block checksum. Legacy v1 headers have no `meta` and keep the original
// single-in-place-header path unchanged.
#pragma pack(push, 1)
struct StorageMetaHead {
	uint32_t magic;      // STORAGE_V2_MAGIC (identifies a v2 meta block)
	uint16_t version;    // meta format version (currently 2)
	uint16_t reserved;   // reserved for flags; zero for now
	uint64_t txnid;      // monotonic commit counter; open picks the highest valid one
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


struct StorageHeader {
	struct StorageHeaderHead {
		// uint32_t magic;
		uint32_t offset;  // required
		// char uuid[36];
	} head;

	char padding[(STORAGE_BLOCK_SIZE - sizeof(StorageHeader::StorageHeaderHead)) / sizeof(char)];

	void init(void* /*param*/, void* /*args*/) {
		head.offset = STORAGE_START_BLOCK_OFFSET;
		// head.magic = STORAGE_MAGIC;
		// strncpy(head.uuid, "00000000-0000-0000-0000-000000000000", sizeof(head.uuid));
	}

	void validate(void* /*param*/, void* /*args*/) {
		// if (head.magic != STORAGE_MAGIC) {
		// 	THROW(StorageCorruptVolume, "Bad header magic number");
		// }
		// if (strncasecmp(head.uuid, "00000000-0000-0000-0000-000000000000", sizeof(head.uuid))) {
		// 	THROW(StorageCorruptVolume, "UUID mismatch");
		// }
	}
};


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


// StorageLegacyHeader (optional, defaults to void): when a v2-capable consumer
// must also READ pre-existing v1 volumes, it passes the old v1 header type here.
// open() then falls back to it for a volume that is not a valid v2 volume. Left
// void, a non-v2 volume is treated as corrupt. Legacy volumes are read-only.
template <typename StorageHeader, typename StorageBinHeader, typename StorageBinFooter,
          typename IO = STORAGE_DEFAULT_IO, typename StorageLegacyHeader = void>
class Storage {
	void* param;

	StorageFsyncFn _async_fsync;  // optional host hook; null => synchronous fsync

	std::string path;
	int flags;
	int fd;

	int free_blocks;

	char buffer0[STORAGE_BLOCK_SIZE];
	char buffer1[STORAGE_BLOCK_SIZE];
	char* buffer_curr;
	uint32_t buffer_offset;

	off_t bin_offset;
	StorageBinHeader bin_header;
	StorageBinFooter bin_footer;

	size_t bin_size;

	// Decompressed payload of the bin currently being read, with a serving cursor.
	// read() decompresses the whole record once (one-shot codec dispatch) and
	// hands it out in chunks from here. Records are bounded, so this stays small.
	std::string _record;
	size_t _record_offset;

	compressors::XXH32_state_t xxh_state;  // stack-resident; canonical XXH32 from the compressors lib
	uint32_t bin_hash;

	bool changed;

	// ---- v1/v2 seam ------------------------------------------------------------
	// hw_offset is the authoritative committed high-water mark (in STORAGE_ALIGNMENT
	// units) for BOTH formats -- loaded from header.head.offset (v1) or the winning
	// meta's offset (v2) on open, bumped by write(), and written back on commit().
	// vol_format / vol_start describe the CURRENTLY OPEN volume at runtime (not the
	// consumer's compile-time capability): a v2-capable consumer can still open an
	// existing v1 volume read-only, where data starts at block 1, not block 2.
	uint64_t hw_offset;
	int vol_format;        // 1 or 2: the format of the open volume
	uint32_t vol_start;    // where record data begins, in STORAGE_ALIGNMENT units

	// v2 dual-meta bookkeeping. meta_slot is which of the two meta blocks (0/1)
	// holds the current committed snapshot; a commit writes the OTHER one and bumps
	// meta_txnid, so a torn write to the block being committed leaves the previous
	// committed meta intact and open() falls back to it.
	uint64_t meta_txnid;
	int meta_slot;

	// True iff this consumer's header opts into the v2 dual-meta format.
	static constexpr bool is_v2 = storage_has_meta_v<StorageHeader>;

	// XXH32 over a whole meta block with the 4-byte checksum field treated as zero
	// (so the checksum can live inside the block it protects). v2-only.
	static uint32_t meta_checksum(const StorageHeader& h) noexcept {
		StorageHeader tmp = h;
		tmp.meta.checksum = 0;
		return xxh32_oneshot(&tmp, sizeof(StorageHeader), STORAGE_CHECKSUM_SEED);
	}

	// Build the in-memory meta (consumer fields already populated), stamp it with
	// the given txnid + current high-water, checksum it, and write it to meta block
	// `slot` (0 or 1). v2-only.
	void write_meta(int slot, uint64_t txnid) {
		header.meta.magic = STORAGE_V2_MAGIC;
		header.meta.version = StorageMetaHead::CURRENT_VERSION;
		header.meta.reserved = 0;
		header.meta.txnid = txnid;
		header.meta.offset = hw_offset;
		header.meta.checksum = 0;
		header.meta.checksum = meta_checksum(header);
		if unlikely(IO::pwrite(fd, &header, sizeof(header), static_cast<off_t>(slot) * STORAGE_BLOCK_SIZE) != sizeof(header)) {
			close();
			L_ERR("IO error in {}: pwrite meta: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
			THROW(StorageIOError, error::description(errno));
		}
	}

	// One-shot XXH32 over a whole buffer, via the compressors lib's canonical
	// streaming XXH32 (same digest as upstream xxHash / lz4's bundled copy, so
	// footer checksums stay byte-identical to the in-tree engine).
	static uint32_t xxh32_oneshot(const void* data, size_t size, uint32_t seed) noexcept {
		compressors::XXH32_state_t state;
		compressors::XXH32_reset(&state, seed);
		compressors::XXH32_update(&state, data, size);
		return compressors::XXH32_digest(&state);
	}

	void growfile() {
		if (free_blocks <= STORAGE_BLOCKS_MIN_FREE) {
			off_t file_size = IO::lseek(fd, 0, SEEK_END);
			if unlikely(file_size == -1) {
				close();
				L_ERR("IO error in {}: lseek: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			}
			free_blocks = static_cast<int>((file_size - static_cast<off_t>(hw_offset) * STORAGE_ALIGNMENT) / STORAGE_BLOCK_SIZE);
			if (free_blocks <= STORAGE_BLOCKS_MIN_FREE) {
				int total_blocks = static_cast<int>(file_size / STORAGE_BLOCK_SIZE);
				total_blocks = total_blocks < STORAGE_BLOCKS_MIN_FREE ? STORAGE_BLOCKS_MIN_FREE : total_blocks * STORAGE_BLOCKS_GROWTH_FACTOR;
				off_t new_size = total_blocks * STORAGE_BLOCK_SIZE;
				if (new_size > STORAGE_LAST_BLOCK_OFFSET) {
					new_size = STORAGE_LAST_BLOCK_OFFSET;
				}
				if (new_size > file_size) {
					if unlikely(IO::fallocate(fd, 0, file_size, new_size - file_size) == -1) {
						L_WARNING_ONCE("Cannot grow storage file: {} ({}): {}", error::name(errno), errno, error::description(errno));
					}
				}
			}
		}
	}

	void write_buffer(char** buffer_, uint32_t& buffer_offset_, off_t& block_offset_) {
		buffer_offset_ = 0;
		if (*buffer_ == buffer_curr) {
			*buffer_ = buffer_curr == buffer0 ? buffer1 : buffer0;
			goto do_update;
		} else {
			goto do_write;
		}

	do_write:
		if unlikely(IO::pwrite(fd, *buffer_, STORAGE_BLOCK_SIZE, block_offset_) != STORAGE_BLOCK_SIZE) {
			close();
			L_ERR("IO error in {}: pwrite: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
			THROW(StorageIOError, error::description(errno));
		}

	do_update:
		block_offset_ += STORAGE_BLOCK_SIZE;
		if (block_offset_ >= STORAGE_LAST_BLOCK_OFFSET) {
			THROW(StorageEOF, "Storage EOF");
		}
		--free_blocks;
#if STORAGE_BUFFER_CLEAR
		memset(*buffer_, STORAGE_BUFFER_CLEAR_CHAR, STORAGE_BLOCK_SIZE);
#endif
	}

	void write_bin(char** buffer_, uint32_t& buffer_offset_, const char** data_bin_, size_t& size_bin_) {
		size_t size = STORAGE_BLOCK_SIZE - buffer_offset_;
		if (size > size_bin_) {
			size = size_bin_;
		}
		memcpy(*buffer_ + buffer_offset_, *data_bin_, size);
		size_bin_ -= size;
		*data_bin_ += size;
		buffer_offset_ += size;
	}

protected:
	StorageHeader header;
	std::string base_path;


public:
	Storage(std::string_view base_path_, void* param_, StorageFsyncFn async_fsync = nullptr)
		: param(param_),
		  _async_fsync(std::move(async_fsync)),
		  flags(0),
		  fd(-1),
		  free_blocks(0),
		  buffer_curr(buffer0),
		  buffer_offset(0),
		  bin_offset(0),
		  bin_size(0),
		  _record_offset(0),
		  bin_hash(0),
		  changed(false),
		  hw_offset(0),
		  vol_format(is_v2 ? 2 : 1),
		  vol_start(is_v2 ? STORAGE_START_BLOCK_OFFSET_V2 : STORAGE_START_BLOCK_OFFSET),
		  meta_txnid(0),
		  meta_slot(0),
		  base_path(storage::normalize_path(base_path_, true)) {
		memset(&header, 0, sizeof(header));
		if ((reinterpret_cast<char*>(&bin_header.size) - reinterpret_cast<char*>(&bin_header) + sizeof(bin_header.size)) > STORAGE_ALIGNMENT) {
			L_ERR("StorageBinHeader's size must be in the first {} bytes", STORAGE_ALIGNMENT - sizeof(bin_header.size));
			THROW(StorageException, "Invalid storage header");
		}
		if constexpr (is_v2) {
			static_assert(sizeof(StorageHeader) == STORAGE_BLOCK_SIZE,
				"a v2 storage header must be exactly one block");
			static_assert(offsetof(StorageHeader, meta) == 0,
				"a v2 header must begin with its StorageMetaHead meta");
		}
	}

	~Storage() noexcept {
		try {
			close();
		} catch (...) {
			L_EXC("Unhandled exception in destructor");
		}
	}

	void initialize_file(void* args) {
		L_CALL("Storage::initialize_file()");

		if unlikely(fd == -1) {
			close();
			L_DEBUG("IO error in {}: Closed storage", repr(path.empty() ? base_path : path));
			THROW(StorageClosedError, "Closed storage");
		}

		memset(&header, 0, sizeof(header));
		header.init(param, args);

		if constexpr (is_v2) {
			// A fresh v2 volume: data begins after the two meta blocks. Write both
			// meta blocks so open() always finds a valid pair; block 0 wins with
			// the higher txnid. The consumer's init() set its own fields (uuid,
			// etc.); write_meta() stamps the engine-owned meta over them.
			vol_format = 2;
			vol_start = STORAGE_START_BLOCK_OFFSET_V2;
			hw_offset = vol_start;
			write_meta(1, 0);   // older snapshot
			write_meta(0, 1);   // current snapshot
			meta_slot = 0;
			meta_txnid = 1;
		} else {
			vol_format = 1;
			vol_start = STORAGE_START_BLOCK_OFFSET;
			if unlikely(IO::write(fd, &header, sizeof(header)) != sizeof(header)) {
				close();
				L_ERR("IO error in {}: write: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			}
			hw_offset = header.head.offset;
		}

		seek(vol_start);
	}

	bool open(std::string_view relative_path, int flags_=STORAGE_CREATE_OR_OPEN, void* args=nullptr) {
		L_CALL("Storage::open({}, {}, <args>)", repr(relative_path), flags_);

		bool created = false;
		auto path_ = base_path;
		path_.append(relative_path);

		if (path != path_ || flags != flags_) {
			close();

			path = path_;
			flags = flags_;

#if STORAGE_BUFFER_CLEAR
			if (flags & STORAGE_WRITABLE) {
				memset(buffer_curr, STORAGE_BUFFER_CLEAR_CHAR, STORAGE_BLOCK_SIZE);
			}
#endif

			fd = IO::open(path.c_str(), (flags & STORAGE_WRITABLE) ? O_RDWR : O_RDONLY, 0644);
			if unlikely(fd == -1 || IO::lseek(fd, 0, SEEK_END) == 0) {
				if (fd != -1) {
					IO::close(fd);
					fd = -1;
				}
				if (flags & STORAGE_CREATE) {
					fd = IO::open(path.c_str(), (flags & STORAGE_WRITABLE) ? O_RDWR | O_CREAT : O_RDONLY | O_CREAT, 0644);
					if unlikely(fd == -1) {
						close();
						L_ERR("IO error in {}: open: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
						THROW(StorageIOError, error::description(errno));
					}
					initialize_file(args);
					created = true;
				}
				return created;
			}
		}

		return reopen();
	}

	// Read meta block `slot` and return true iff it is a valid v2 meta: full read,
	// correct magic, a version we understand, a matching whole-block checksum, and
	// a high-water mark that fits within the physical file (a truncated tail makes
	// the meta invalid so open() falls back to the other one). v2-only.
	bool load_meta(int slot, StorageHeader& out) {
		auto n = IO::pread(fd, &out, sizeof(out), static_cast<off_t>(slot) * STORAGE_BLOCK_SIZE);
		if (n != static_cast<ssize_t>(sizeof(out))) {
			return false;
		}
		if (out.meta.magic != STORAGE_V2_MAGIC) {
			return false;
		}
		if (out.meta.version > StorageMetaHead::CURRENT_VERSION) {
			return false;  // a newer format we must not misread
		}
		uint32_t stored = out.meta.checksum;
		if (meta_checksum(out) != stored) {
			return false;  // torn or bit-rotted meta
		}
		off_t fsize = IO::lseek(fd, 0, SEEK_END);
		if (fsize != -1 && static_cast<uint64_t>(out.meta.offset) * STORAGE_ALIGNMENT > static_cast<uint64_t>(fsize)) {
			return false;  // high-water beyond EOF (truncated tail): distrust this meta
		}
		return true;
	}

	bool reopen(void* args=nullptr) {
		L_CALL("Storage::reopen()");

		if unlikely(fd == -1) {
			close();
			L_ERR("IO error in {}: Cannot open storage file: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
			THROW(StorageIOError, error::description(errno));
		}

		if constexpr (is_v2) {
			// Read BOTH meta blocks; the volume is v2 iff at least one is a valid
			// meta. Adopt the valid one with the highest txnid (the last successful
			// commit); a torn/rotted/truncated meta simply loses the tie.
			StorageHeader m0;
			StorageHeader m1;
			bool ok0 = load_meta(0, m0);
			bool ok1 = load_meta(1, m1);
			if (ok0 || ok1) {
				bool pick0 = ok0 && (!ok1 || m0.meta.txnid >= m1.meta.txnid);
				header = pick0 ? m0 : m1;
				meta_slot = pick0 ? 0 : 1;
				meta_txnid = header.meta.txnid;
				hw_offset = header.meta.offset;
				vol_format = 2;
				vol_start = STORAGE_START_BLOCK_OFFSET_V2;
				header.validate(param, args);
			} else if constexpr (!std::is_void_v<StorageLegacyHeader>) {
				// Not a v2 volume: fall back to reading the legacy v1 header
				// (read-only). Throws if it is not a valid v1 volume either.
				StorageLegacyHeader legacy;
				auto n = IO::pread(fd, &legacy, sizeof(legacy), 0);
				if unlikely(n != static_cast<ssize_t>(sizeof(legacy))) {
					THROW(StorageCorruptVolume, "Incomplete legacy header");
				}
				legacy.validate(param, args);
				hw_offset = legacy.head.offset;
				vol_format = 1;
				vol_start = STORAGE_START_BLOCK_OFFSET;
			} else {
				THROW(StorageCorruptVolume, "No valid v2 meta block");
			}
		} else {
			auto read_size = IO::pread(fd, &header, sizeof(header), 0);
			if unlikely(read_size == -1) {
				close();
				L_ERR("IO error in {}: read: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			} else if unlikely(read_size != static_cast<ssize_t>(sizeof(header))) {
				THROW(StorageCorruptVolume, "Incomplete bin data");
			}
			header.validate(param, args);
			hw_offset = header.head.offset;
			vol_format = 1;
			vol_start = STORAGE_START_BLOCK_OFFSET;
		}

		if (flags & STORAGE_WRITABLE) {
			if (is_v2 && vol_format == 1) {
				// A v2 consumer may READ a legacy v1 volume, but never write it;
				// new writes always go to fresh v2 volumes.
				close();
				THROW(StorageIOError, "Cannot write a legacy v1 volume");
			}
			buffer_offset = hw_offset * STORAGE_ALIGNMENT;
			size_t offset = (buffer_offset / STORAGE_BLOCK_SIZE) * STORAGE_BLOCK_SIZE;
			buffer_offset -= offset;
			if unlikely(IO::pread(fd, buffer_curr, STORAGE_BLOCK_SIZE, offset) == -1) {
				close();
				L_ERR("IO error in {}: pread: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			}
		}

		seek(vol_start);

		return false;
	}

	void close() {
		L_CALL("Storage::close()");

		_record.clear();
		_record_offset = 0;

		if (fd != -1) {
			if (flags & STORAGE_WRITABLE) {
				commit();
			}
			IO::close(fd);
			fd = -1;
		}

		free_blocks = 0;
		bin_offset = 0;
		bin_size = 0;
		bin_header.size = 0;
		buffer_offset = 0;
		flags = 0;
		hw_offset = 0;
		vol_format = is_v2 ? 2 : 1;
		vol_start = is_v2 ? STORAGE_START_BLOCK_OFFSET_V2 : STORAGE_START_BLOCK_OFFSET;
		meta_txnid = 0;
		meta_slot = 0;
		path.clear();
	}

	void seek(uint32_t offset) {
		L_CALL("Storage::seek()");

		if (offset > hw_offset) {
			THROW(StorageEOF, "Storage EOF");
		}
		bin_offset = static_cast<off_t>(offset) * STORAGE_ALIGNMENT;

		// Reset the per-record read state so seek() is always a clean entry point.
		// read() carries state across calls while streaming one record in chunks
		// (bin_header.size != 0 means "mid record"); if a prior read stopped early
		// -- an exception on a corrupt/torn record, or a partial char* read the
		// caller never drained -- that stale state would otherwise make the read at
		// the new offset reinterpret the next record with the previous header,
		// yielding a spurious checksum failure or silent garbage. A seek is always
		// the start of a fresh record, so clear it.
		bin_header.size = 0;
		bin_size = 0;
		_record.clear();
		_record_offset = 0;
	}

	uint32_t write(const char *data, size_t data_size, void* args=nullptr) {
		L_CALL("Storage::write() [1]");

		if unlikely(fd == -1) {
			close();
			L_DEBUG("IO error in {}: Closed storage", repr(path.empty() ? base_path : path));
			THROW(StorageClosedError, "Closed storage");
		}

		if ((flags & STORAGE_WRITABLE) == 0) {
			L_ERR("IO error in {}: Read-only storage", repr(path.empty() ? base_path : path));
			THROW(StorageIOError, "Read-only storage");
		}

		uint32_t curr_offset = static_cast<uint32_t>(hw_offset);
		const char* orig_data = data;

		StorageBinHeader _bin_header;
		memset(&_bin_header, 0, sizeof(_bin_header));
		const char* bin_header_data = reinterpret_cast<const char*>(&_bin_header);
		size_t bin_header_data_size = sizeof(StorageBinHeader);

		StorageBinFooter _bin_footer;
		memset(&_bin_footer, 0, sizeof(_bin_footer));
		const char* bin_footer_data = reinterpret_cast<const char*>(&_bin_footer);
		size_t bin_footer_data_size = sizeof(StorageBinFooter);

		// Pick the codec from the open flags and compress the whole record up
		// front (one-shot). The payload is then a single contiguous buffer for any
		// codec, and LZ4 bytes are identical to the in-tree streaming engine (same
		// CompressData blocks), so existing volumes are unaffected. The codec id
		// rides in the bin-header flags so read() can dispatch on it.
		std::string compressed;
		size_t payload_size = data_size;
		int wcodec = storage_write_codec(flags);
		if (wcodec >= 0 && data_size > STORAGE_MIN_COMPRESS_SIZE) {
			compressed = storage_compress(static_cast<uint8_t>(wcodec), std::string_view(data, data_size));
			data = compressed.data();
			payload_size = compressed.size();
			_bin_header.init(param, args, static_cast<uint32_t>(payload_size),
				static_cast<uint8_t>(STORAGE_FLAG_COMPRESSED | (wcodec << STORAGE_CODEC_SHIFT)));
		} else {
			_bin_header.init(param, args, static_cast<uint32_t>(payload_size), 0);
		}
		size_t it_size = payload_size;

		char* buffer = buffer_curr;
		uint32_t tmp_buffer_offset = buffer_offset;
		StorageBinHeader* buffer_header = reinterpret_cast<StorageBinHeader*>(buffer + tmp_buffer_offset);

		off_t block_offset = ((curr_offset * STORAGE_ALIGNMENT) / STORAGE_BLOCK_SIZE) * STORAGE_BLOCK_SIZE;
		off_t tmp_block_offset = block_offset;

		while (bin_header_data_size) {
			write_bin(&buffer, tmp_buffer_offset, &bin_header_data, bin_header_data_size);
			if (tmp_buffer_offset == STORAGE_BLOCK_SIZE) {
				write_buffer(&buffer, tmp_buffer_offset, block_offset);
				continue;
			}
			break;
		}

		while (it_size) {
			write_bin(&buffer, tmp_buffer_offset, &data, it_size);
			if (tmp_buffer_offset == STORAGE_BLOCK_SIZE) {
				write_buffer(&buffer, tmp_buffer_offset, block_offset);
			}
		}

		while (bin_footer_data_size) {
			// The footer checksum is XXH32 over the *uncompressed* input (the bin
			// header already carries the on-disk payload size), so it is identical
			// for raw and compressed records, and read() validates it after
			// decompressing.
			_bin_footer.init(param, args, xxh32_oneshot(orig_data, data_size, STORAGE_CHECKSUM_SEED));

			write_bin(&buffer, tmp_buffer_offset, &bin_footer_data, bin_footer_data_size);

			// Align the tmp_buffer_offset to the next storage alignment
			tmp_buffer_offset = static_cast<uint32_t>(((block_offset + tmp_buffer_offset + STORAGE_ALIGNMENT - 1) / STORAGE_ALIGNMENT) * STORAGE_ALIGNMENT - block_offset);
			if (tmp_buffer_offset == STORAGE_BLOCK_SIZE) {
				write_buffer(&buffer, tmp_buffer_offset, block_offset);
				continue;
			}
			if unlikely(IO::pwrite(fd, buffer, STORAGE_BLOCK_SIZE, block_offset) != STORAGE_BLOCK_SIZE) {
				close();
				L_ERR("IO error in {}: pwrite: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			}
			break;
		}

		// Write the first used buffer.
		if (buffer != buffer_curr) {
			if unlikely(IO::pwrite(fd, buffer_curr, STORAGE_BLOCK_SIZE, tmp_block_offset) != STORAGE_BLOCK_SIZE) {
				close();
				L_ERR("IO error in {}: pwrite: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			}
			buffer_curr = buffer;
		}

		buffer_offset = tmp_buffer_offset;
		hw_offset += (((sizeof(StorageBinHeader) + buffer_header->size + sizeof(StorageBinFooter)) + STORAGE_ALIGNMENT - 1) / STORAGE_ALIGNMENT);

		changed = true;

		return curr_offset;
	}

	uint32_t write_file(std::string_view filename, void* args=nullptr) {
		L_CALL("Storage::write_file()");

		// Read the whole file, then store it through write(): one code path, so
		// codec dispatch and checksums are shared with write() and there is no
		// second copy of the block-writing machinery. The in-tree engine streamed
		// here to avoid buffering the file, but the storage use is bounded blobs,
		// so the buffer is acceptable; a consumer with huge files should chunk
		// them into multiple write() calls.
		stringified filename_string(filename);
		int fd_read = IO::open(filename_string.c_str(), O_RDONLY, 0644);
		if unlikely(fd_read == -1) {
			close();
			L_ERR("IO error in {}: Cannot open file: {}", repr(path.empty() ? base_path : path), filename);
			THROW(StorageIOError, error::description(errno));
		}

		std::string contents;
		char buf_read[STORAGE_BLOCK_SIZE];
		ssize_t read_size;
		while ((read_size = IO::read(fd_read, buf_read, sizeof(buf_read))) > 0) {
			contents.append(buf_read, static_cast<size_t>(read_size));
		}
		if unlikely(read_size == -1) {
			IO::close(fd_read);
			close();
			L_ERR("IO error in {}: Cannot read file: {}", repr(path.empty() ? base_path : path), filename);
			THROW(StorageIOError, error::description(errno));
		}
		IO::close(fd_read);

		return write(contents.data(), contents.size(), args);
	}

	size_t read(char* buf, size_t buf_size, uint32_t limit=-1, void* args=nullptr) {
		L_CALL("Storage::read() [1]");

		if (!buf_size) {
			return 0;
		}

		if (!bin_header.size) {
			off_t offset = IO::lseek(fd, bin_offset, SEEK_SET);
			if (offset >= static_cast<off_t>(hw_offset) * STORAGE_ALIGNMENT || offset >= limit * STORAGE_ALIGNMENT) {
				THROW(StorageEOF, "Storage EOF");
			}

			auto read_size = IO::read(fd, &bin_header, sizeof(StorageBinHeader));
			if unlikely(read_size == -1) {
				close();
				L_ERR("IO error in {}: read: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			} else if unlikely(read_size != sizeof(StorageBinHeader)) {
				THROW(StorageCorruptVolume, "Incomplete bin header");
			}
			bin_offset += read_size;
			bin_header.validate(param, args);

			// Guard the size field before trusting it for I/O or allocation. A
			// record's payload plus its footer can never extend past the volume's
			// committed high-water mark (hw_offset, counted in STORAGE_ALIGNMENT
			// units -- the same bound seek()/EOF use). The bin header is not
			// checksummed, so a single flipped byte can turn `size` into ~4 GB;
			// without this check the payload.resize(size) on the compressed path
			// below would attempt a multi-gigabyte allocation, turning a corrupt
			// volume into an OOM / std::bad_alloc instead of a clean, catchable
			// StorageCorruptVolume. The bound also caps the uncompressed read loop.
			// It is computed from in-memory state (zero syscalls) and never rejects
			// a valid record (hw_offset is always rounded up past the last footer).
			if (static_cast<uint64_t>(bin_offset) + bin_header.size + sizeof(StorageBinFooter)
					> hw_offset * STORAGE_ALIGNMENT) {
				THROW(StorageCorruptVolume, "Bin size exceeds volume bounds");
			}

			IO::fadvise(fd, bin_offset, bin_header.size, POSIX_FADV_WILLNEED);

			if (bin_header.flags & STORAGE_FLAG_COMPRESSED) {
				// Read the whole compressed payload and decompress it once; the
				// chunks are served from _record below. The codec rides in the
				// bin-header flags (legacy volumes wrote LZ4 = codec 0).
				uint8_t codec = (bin_header.flags & STORAGE_CODEC_MASK) >> STORAGE_CODEC_SHIFT;
				std::string payload;
				payload.resize(bin_header.size);
				auto payload_read = IO::read(fd, payload.data(), bin_header.size);
				if unlikely(payload_read == -1) {
					close();
					L_ERR("IO error in {}: read: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
					THROW(StorageIOError, error::description(errno));
				} else if unlikely(static_cast<size_t>(payload_read) != bin_header.size) {
					THROW(StorageCorruptVolume, "Incomplete bin data");
				}
				bin_offset += bin_header.size;
				_record = storage_decompress(codec, std::string_view(payload.data(), payload.size()));
				_record_offset = 0;
			} else {
				compressors::XXH32_reset(&xxh_state, STORAGE_CHECKSUM_SEED);
			}
		}

		if (bin_header.flags & STORAGE_FLAG_COMPRESSED) {
			size_t avail = _record.size() - _record_offset;
			if (avail) {
				size_t n = buf_size < avail ? buf_size : avail;
				memcpy(buf, _record.data() + _record_offset, n);
				_record_offset += n;
				return n;
			}
			// Whole record served; the footer checksum is over the decompressed
			// bytes, which equal the original input.
			bin_hash = xxh32_oneshot(_record.data(), _record.size(), STORAGE_CHECKSUM_SEED);
		} else {
			if (buf_size > bin_header.size - bin_size) {
				buf_size = bin_header.size - bin_size;
			}

			if (buf_size) {
				auto read_size = IO::read(fd, buf, buf_size);
				if unlikely(read_size == -1) {
					close();
					L_ERR("IO error in {}: read: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
					THROW(StorageIOError, error::description(errno));
				} else if unlikely(static_cast<size_t>(read_size) != buf_size) {
					THROW(StorageCorruptVolume, "Incomplete bin data");
				}
				bin_offset += read_size;
				bin_size += read_size;
				compressors::XXH32_update(&xxh_state, buf, read_size);
				return read_size;
			}
			bin_hash = compressors::XXH32_digest(&xxh_state);
		}

		auto read_size = IO::read(fd, &bin_footer, sizeof(StorageBinFooter));
		if unlikely(read_size == -1) {
			close();
			L_ERR("IO error in {}: read: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
			THROW(StorageIOError, error::description(errno));
		} else if unlikely(read_size != sizeof(StorageBinFooter)) {
			THROW(StorageCorruptVolume, "Incomplete bin footer");
		}
		bin_offset += read_size;
		bin_footer.validate(param, args, bin_hash);

		// Align the bin_offset to the next storage alignment
		bin_offset = ((bin_offset + STORAGE_ALIGNMENT - 1) / STORAGE_ALIGNMENT) * STORAGE_ALIGNMENT;

		bin_header.size = 0;
		bin_size = 0;

		return 0;
	}

	// fsync / full_fsync the fd, or hand it to the host's async-fsync hook. Shared
	// by commit(): v2 calls it twice to form the data->meta write barrier.
	void do_sync() {
		if (flags & STORAGE_NO_SYNC) {
			return;
		}
		bool full = (flags & STORAGE_FULL_SYNC) != 0;
		if ((flags & STORAGE_ASYNC_SYNC) && _async_fsync) {
			// Offload to the host's batched/background fsync (it owns the
			// retry/error reporting for the deferred syscall).
			_async_fsync(fd, full);
		} else if (full) {
			if unlikely(IO::full_fsync(fd) == -1) {
				close();
				L_ERR("IO error in {}: full_fsync: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			}
		} else {
			if unlikely(IO::fsync(fd) == -1) {
				close();
				L_ERR("IO error in {}: fsync: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			}
		}
	}

	void commit() {
		L_CALL("Storage::commit()");

		if (!changed) {
			return;
		}

		if unlikely(fd == -1) {
			close();
			L_DEBUG("IO error in {}: Closed storage", repr(path.empty() ? base_path : path));
			THROW(StorageClosedError, "Closed storage");
		}

		if unlikely((flags & STORAGE_WRITABLE) == 0) {
			L_ERR("IO error in {}: Read-only storage", repr(path.empty() ? base_path : path));
			THROW(StorageIOError, "Read-only storage");
		}

		changed = false;

		if constexpr (is_v2) {
			// Crash-safe commit, in two ordered barriers:
			//   1. fsync the data blocks so they are durable BEFORE the commit
			//      point that will reference them.
			//   2. write the OTHER meta block (higher txnid) and fsync it. A torn
			//      or lost write here leaves the previous committed meta (in the
			//      block we did NOT touch) intact, so open() falls back to it and
			//      the volume never loses its last consistent state.
			// A durable meta therefore always points at durable data.
			do_sync();
			int next_slot = 1 - meta_slot;
			uint64_t next_txnid = meta_txnid + 1;
			write_meta(next_slot, next_txnid);
			do_sync();
			meta_slot = next_slot;
			meta_txnid = next_txnid;
		} else {
			header.head.offset = static_cast<uint32_t>(hw_offset);
			if unlikely(IO::pwrite(fd, &header, sizeof(header), 0) != sizeof(header)) {
				close();
				L_ERR("IO error in {}: pwrite: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			}
			do_sync();
		}

		growfile();
	}

	uint32_t write(std::string_view data, void* args=nullptr) {
		L_CALL("Storage::write() [2]");

		return write(data.data(), data.size(), args);
	}

	std::string read(uint32_t limit=-1, void* args=nullptr) {
		L_CALL("Storage::read() [2]");

		std::string ret;

		char buf[LZ4_BLOCK_SIZE];
		while (auto read_size = read(buf, sizeof(buf), limit, args)) {
			ret += std::string(buf, read_size);
		}

		return ret;
	}

	std::pair<unsigned long long, unsigned long long>
	get_volumes_range(std::string_view pattern, unsigned long long min=0, unsigned long long max=std::numeric_limits<unsigned long long>::max()) {
		// Figure out highest and lowest volume files available for a given file pattern
		L_CALL("Storage::get_volumes_range()");

		DIR* dir = ::opendir(base_path.c_str());
		if (dir == nullptr) {
			L_DEBUG("Could not open the directory {}: {} ({}): {}", repr(base_path), error::name(errno), errno, error::description(errno));
			THROW(StorageNotFound, "Couldn't open storage directory");
		}

		unsigned long long first_volume = std::numeric_limits<unsigned long long>::max();
		unsigned long long last_volume = 0;

		// Volume files are named "<pattern><n>" (e.g. "data.0", "data.1"); scan the
		// directory for regular files whose name starts with the pattern and pull
		// the numeric suffix after the last '.'. (Replaces fs.hh's find_file_dir;
		// the standalone engine does its own readdir.)
		while (struct dirent* ent = ::readdir(dir)) {
			if (ent->d_type != DT_REG) {
				continue;
			}
			std::string_view filename(ent->d_name);
			if (filename.size() < pattern.size() || filename.compare(0, pattern.size(), pattern) != 0) {
				continue;
			}
			auto found = filename.find_last_of(".");
			if (found != std::string_view::npos) {
				int errno_save;
				unsigned long long file_volume = static_cast<unsigned long long>(strict_stoull(&errno_save, filename.substr(found + 1)));
				if (errno_save == 0) {
					if (file_volume < first_volume && first_volume >= min) {
						first_volume = file_volume;
					}

					if (file_volume > last_volume && file_volume <= max) {
						last_volume = file_volume;
					}
				}
			}
		}

		::closedir(dir);

		return {first_volume, last_volume};
	}

	bool closed() noexcept {
		return fd == -1;
	}
};
