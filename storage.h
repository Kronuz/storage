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
#include <cstring>               // for memset, memcpy
#include <dirent.h>              // for opendir, readdir, closedir (volume scan)
#include <errno.h>               // for errno
#include <functional>            // for std::function (the async-fsync hook)
#include <limits>                // for std::numeric_limits
#include <memory>
#include <string>                // for std::string
#include <string_view>           // for std::string_view
#include <unistd.h>

#include "compressor_lz4.h"      // for LZ4CompressFile/Data, LZ4Decompress*, XXH32 (compressors lib)
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


#define STORAGE_MAGIC 0x02DEBC47
#define STORAGE_BIN_HEADER_MAGIC 0x2A
#define STORAGE_BIN_FOOTER_MAGIC 0x42

#define STORAGE_BLOCK_SIZE (1024 * 4)
#define STORAGE_ALIGNMENT 8

#define STORAGE_BUFFER_CLEAR 1
#define STORAGE_BUFFER_CLEAR_CHAR '\0'

#define STORAGE_BLOCKS_GROWTH_FACTOR 1.3f
#define STORAGE_BLOCKS_MIN_FREE 4

#define STORAGE_LAST_BLOCK_OFFSET (static_cast<off_t>(std::numeric_limits<uint32_t>::max()) * STORAGE_ALIGNMENT)

#define STORAGE_START_BLOCK_OFFSET (STORAGE_BLOCK_SIZE / STORAGE_ALIGNMENT)

#define STORAGE_MIN_COMPRESS_SIZE 100


constexpr int STORAGE_OPEN             = 0x00;  // Open an existing database.
constexpr int STORAGE_WRITABLE         = 0x01;  // Opens as writable.
constexpr int STORAGE_CREATE           = 0x02;  // Automatically creates the database if it doesn't exist
constexpr int STORAGE_CREATE_OR_OPEN   = 0x03;  // Create database if it doesn't already exist.
constexpr int STORAGE_ASYNC_SYNC       = 0x04;  // fsync (or full_fsync) is async
constexpr int STORAGE_FULL_SYNC        = 0x08;  // Try to ensure changes are really written to disk.
constexpr int STORAGE_NO_SYNC          = 0x10;  // Don't attempt to ensure changes have hit disk.
constexpr int STORAGE_COMPRESS         = 0x20;  // Compress data in storage.

constexpr int STORAGE_FLAG_COMPRESSED  = 0x01;
constexpr int STORAGE_FLAG_DELETED     = 0x02;
constexpr int STORAGE_FLAG_MASK        = STORAGE_FLAG_COMPRESSED | STORAGE_FLAG_DELETED;



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


template <typename StorageHeader, typename StorageBinHeader, typename StorageBinFooter, typename IO = STORAGE_DEFAULT_IO>
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

	LZ4CompressData cmpData;
	LZ4CompressData::iterator cmpData_it;

	LZ4CompressFile cmpFile;
	LZ4CompressFile::iterator cmpFile_it;

	LZ4DecompressFile decFile;
	LZ4DecompressFile::iterator decFile_it;

	compressors::XXH32_state_t xxh_state;  // stack-resident; canonical XXH32 from the compressors lib
	uint32_t bin_hash;

	bool changed;

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
			free_blocks = static_cast<int>((file_size - header.head.offset * STORAGE_ALIGNMENT) / STORAGE_BLOCK_SIZE);
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
		  bin_hash(0),
		  changed(false),
		  base_path(storage::normalize_path(base_path_, true)) {
		memset(&header, 0, sizeof(header));
		if ((reinterpret_cast<char*>(&bin_header.size) - reinterpret_cast<char*>(&bin_header) + sizeof(bin_header.size)) > STORAGE_ALIGNMENT) {
			L_ERR("StorageBinHeader's size must be in the first {} bytes", STORAGE_ALIGNMENT - sizeof(bin_header.size));
			THROW(StorageException, "Invalid storage header");
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

		if unlikely(IO::write(fd, &header, sizeof(header)) != sizeof(header)) {
			close();
			L_ERR("IO error in {}: write: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
			THROW(StorageIOError, error::description(errno));
		}

		seek(STORAGE_START_BLOCK_OFFSET);
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

	bool reopen(void* args=nullptr) {
		L_CALL("Storage::reopen()");

		if unlikely(fd == -1) {
			close();
			L_ERR("IO error in {}: Cannot open storage file: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
			THROW(StorageIOError, error::description(errno));
		}

		auto read_size = IO::pread(fd, &header, sizeof(header), 0);
		if unlikely(read_size == -1) {
			close();
			L_ERR("IO error in {}: read: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
			THROW(StorageIOError, error::description(errno));
		} else if unlikely(read_size != sizeof(header)) {
			THROW(StorageCorruptVolume, "Incomplete bin data");
		}
		header.validate(param, args);

		if (flags & STORAGE_WRITABLE) {
			buffer_offset = header.head.offset * STORAGE_ALIGNMENT;
			size_t offset = (buffer_offset / STORAGE_BLOCK_SIZE) * STORAGE_BLOCK_SIZE;
			buffer_offset -= offset;
			if unlikely(IO::pread(fd, buffer_curr, STORAGE_BLOCK_SIZE, offset) == -1) {
				close();
				L_ERR("IO error in {}: pread: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
				THROW(StorageIOError, error::description(errno));
			}
		}

		seek(STORAGE_START_BLOCK_OFFSET);

		return false;
	}

	void close() {
		L_CALL("Storage::close()");

		cmpData.close();
		cmpFile.close();
		decFile.close();

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
		path.clear();
	}

	void seek(uint32_t offset) {
		L_CALL("Storage::seek()");

		if (offset > header.head.offset) {
			THROW(StorageEOF, "Storage EOF");
		}
		bin_offset = offset * STORAGE_ALIGNMENT;
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

		uint32_t curr_offset = header.head.offset;
		const char* orig_data = data;

		StorageBinHeader _bin_header;
		memset(&_bin_header, 0, sizeof(_bin_header));
		const char* bin_header_data = reinterpret_cast<const char*>(&_bin_header);
		size_t bin_header_data_size = sizeof(StorageBinHeader);

		StorageBinFooter _bin_footer;
		memset(&_bin_footer, 0, sizeof(_bin_footer));
		const char* bin_footer_data = reinterpret_cast<const char*>(&_bin_footer);
		size_t bin_footer_data_size = sizeof(StorageBinFooter);

		size_t it_size;
		bool compress = (flags & STORAGE_COMPRESS) && data_size > STORAGE_MIN_COMPRESS_SIZE;
		if (compress) {
			_bin_header.init(param, args, 0, STORAGE_FLAG_COMPRESSED);
			cmpData.reset(data, data_size, STORAGE_MAGIC);
			cmpData_it = cmpData.begin();
			it_size = cmpData_it.size();
			data = cmpData_it->data();
		} else {
			_bin_header.init(param, args, static_cast<uint32_t>(data_size), 0);
			it_size = data_size;
		}

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
			if (compress && !it_size) {
				++cmpData_it;
				data = cmpData_it->data();
				it_size = cmpData_it.size();
			}
			if (tmp_buffer_offset == STORAGE_BLOCK_SIZE) {
				write_buffer(&buffer, tmp_buffer_offset, block_offset);
			}
		}

		while (bin_footer_data_size) {
			// Update header size in buffer.
			if (compress) {
				buffer_header->size = static_cast<uint32_t>(cmpData.size());
				_bin_footer.init(param, args, cmpData.get_digest());
			} else {
				_bin_footer.init(param, args, xxh32_oneshot(orig_data, data_size, STORAGE_MAGIC));
			}

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
		header.head.offset += (((sizeof(StorageBinHeader) + buffer_header->size + sizeof(StorageBinFooter)) + STORAGE_ALIGNMENT - 1) / STORAGE_ALIGNMENT);

		changed = true;

		return curr_offset;
	}

	uint32_t write_file(std::string_view filename, void* args=nullptr) {
		L_CALL("Storage::write_file()");

		if unlikely(fd == -1) {
			close();
			L_DEBUG("IO error in {}: Closed storage", repr(path.empty() ? base_path : path));
			THROW(StorageClosedError, "Closed storage");
		}

		if ((flags & STORAGE_WRITABLE) == 0) {
			L_ERR("IO error in {}: Read-only storage", repr(path.empty() ? base_path : path));
			THROW(StorageIOError, "Read-only storage");
		}

		uint32_t curr_offset = header.head.offset;

		StorageBinHeader _bin_header;
		memset(&_bin_header, 0, sizeof(_bin_header));
		const char* bin_header_data = reinterpret_cast<const char*>(&_bin_header);
		size_t bin_header_data_size = sizeof(StorageBinHeader);

		StorageBinFooter _bin_footer;
		memset(&_bin_footer, 0, sizeof(_bin_footer));
		const char* bin_footer_data = reinterpret_cast<const char*>(&_bin_footer);
		size_t bin_footer_data_size = sizeof(StorageBinFooter);

		size_t it_size = 0;
		off_t file_size = 0;
		int fd_write = -1;
		char buf_read[STORAGE_BLOCK_SIZE];
		const char* data;

		bool compress = (flags & STORAGE_COMPRESS);
		if (compress) {
			_bin_header.init(param, args, 0, STORAGE_FLAG_COMPRESSED);
			cmpFile.reset(filename, STORAGE_MAGIC);
			cmpFile_it = cmpFile.begin();
			it_size = cmpFile_it.size();
			data = cmpFile_it->data();
		} else {
			stringified filename_string(filename);
			fd_write = IO::open(filename_string.c_str(), O_RDONLY, 0644);
			if unlikely(fd_write == -1) {
				close();
				L_ERR("IO error in {}: Cannot open file: {}", repr(path.empty() ? base_path : path), filename);
				THROW(StorageIOError, error::description(errno));
			}
			_bin_header.init(param, args, 0, 0);
			auto read_size = IO::read(fd_write, buf_read, sizeof(buf_read));
			if unlikely(read_size == -1) {
				close();
				L_ERR("IO error in {}: Cannot read file: {}", repr(path.empty() ? base_path : path), filename);
				THROW(StorageIOError, error::description(errno));
			}
			it_size = read_size;
			data = buf_read;
			file_size += it_size;
			compressors::XXH32_reset(&xxh_state, STORAGE_MAGIC);
			compressors::XXH32_update(&xxh_state, data, it_size);
		}

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
			if (!it_size) {
				if (compress) {
					++cmpFile_it;
					it_size = cmpFile_it.size();
					data = cmpFile_it->data();
				} else {
					auto read_size = IO::read(fd_write, buf_read, sizeof(buf_read));
					if unlikely(read_size == -1) {
						close();
						L_ERR("IO error in {}: Cannot read from file: {}", repr(path.empty() ? base_path : path), filename);
						THROW(StorageIOError, error::description(errno));
					}
					it_size = read_size;
					data = buf_read;
					file_size += it_size;
					compressors::XXH32_update(&xxh_state, data, it_size);
				}
			}
			if (tmp_buffer_offset == STORAGE_BLOCK_SIZE) {
				write_buffer(&buffer, tmp_buffer_offset, block_offset);
			}
		}

		if (!compress) {
			IO::close(fd_write);
			fd_write = -1;
		}

		while (bin_footer_data_size) {
			// Update header size in buffer.
			if (compress) {
				buffer_header->size = static_cast<uint32_t>(cmpFile.size());
				_bin_footer.init(param, args, cmpFile.get_digest());
			} else {
				buffer_header->size = static_cast<uint32_t>(file_size);
				_bin_footer.init(param, args, compressors::XXH32_digest(&xxh_state));
			}

			write_bin(&buffer, tmp_buffer_offset, &bin_footer_data, bin_footer_data_size);

			// Align the tmp_buffer_offset to the next storage alignment
			tmp_buffer_offset = static_cast<uint32_t>(((block_offset + tmp_buffer_offset + STORAGE_ALIGNMENT - 1) / STORAGE_ALIGNMENT) * STORAGE_ALIGNMENT - block_offset);
			if (tmp_buffer_offset == STORAGE_BLOCK_SIZE) {
				write_buffer(&buffer, tmp_buffer_offset, block_offset);
				continue;
			} else {
				if unlikely(IO::pwrite(fd, buffer, STORAGE_BLOCK_SIZE, block_offset) != STORAGE_BLOCK_SIZE) {
					close();
					L_ERR("IO error in {}: pwrite: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
					THROW(StorageIOError, error::description(errno));
				}
				break;
			}
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
		header.head.offset += (((sizeof(StorageBinHeader) + buffer_header->size + sizeof(StorageBinFooter)) + STORAGE_ALIGNMENT - 1) / STORAGE_ALIGNMENT);

		changed = true;

		return curr_offset;
	}

	size_t read(char* buf, size_t buf_size, uint32_t limit=-1, void* args=nullptr) {
		L_CALL("Storage::read() [1]");

		if (!buf_size) {
			return 0;
		}

		if (!bin_header.size) {
			off_t offset = IO::lseek(fd, bin_offset, SEEK_SET);
			if (offset >= header.head.offset * STORAGE_ALIGNMENT || offset >= limit * STORAGE_ALIGNMENT) {
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

			IO::fadvise(fd, bin_offset, bin_header.size, POSIX_FADV_WILLNEED);

			if (bin_header.flags & STORAGE_FLAG_COMPRESSED) {
				decFile.reset(fd, -1, bin_header.size, STORAGE_MAGIC);
				decFile_it = decFile.begin();
				bin_offset += bin_header.size;
			} else {
				compressors::XXH32_reset(&xxh_state, STORAGE_MAGIC);
			}
		}

		if (bin_header.flags & STORAGE_FLAG_COMPRESSED) {
			size_t _size = decFile_it.read(buf, buf_size);
			if (_size) {
				return _size;
			}
			bin_hash = decFile.get_digest();
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

		if unlikely(IO::pwrite(fd, &header, sizeof(header), 0) != sizeof(header)) {
			close();
			L_ERR("IO error in {}: pwrite: {} ({}): {}", repr(path.empty() ? base_path : path), error::name(errno), errno, error::description(errno));
			THROW(StorageIOError, error::description(errno));
		}

		if (!(flags & STORAGE_NO_SYNC)) {
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
