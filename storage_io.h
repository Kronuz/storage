/*
 * Copyright (c) 2015-2026 Germán Méndez Bravo (Kronuz)
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

// DefaultIO: the storage engine's default IO policy.
//
// Storage<Header, BinHeader, BinFooter, IO> routes every file operation through
// its IO type parameter (IO::pwrite(fd, ...), IO::fsync(fd), ...). This is the
// default: a clean, portable, EINTR-safe POSIX layer with no external
// dependency. A host that already owns an instrumented IO layer (Xapiand has
// io.cc with retry/error-injection hooks) passes its own policy struct with the
// same 11 static methods, and the engine is unchanged.
//
// The contract each method must honor (the engine relies on it):
//   open    -> fd, or -1 on error (errno set)
//   close   -> 0, or -1
//   lseek   -> resulting offset, or -1
//   read    -> bytes read (0 at EOF), or -1; may return a short count
//   write   -> bytes written (loops internally), or -1
//   pread   -> bytes read at offset (may be short), or -1
//   pwrite  -> bytes written at offset (loops internally to the full count), -1
//   fsync   -> 0 or -1   (cheap durability)
//   full_fsync -> 0 or -1 (strong durability: F_FULLFSYNC where available)
//   fallocate  -> 0 or -1 (best-effort preallocation)
//   fadvise    -> 0 or -1 (advisory; safe no-op where unavailable)

#include <cerrno>           // for errno, EINTR
#include <cstddef>          // for size_t
#include <fcntl.h>          // for ::open, O_CLOEXEC, F_FULLFSYNC, F_PREALLOCATE
#include <sys/types.h>      // for off_t, ssize_t
#include <unistd.h>         // for ::read, ::write, ::pread, ::pwrite, ::lseek


// POSIX_FADV_* tokens so call sites compile everywhere. macOS/BSD lack
// posix_fadvise and these constants; define the Linux values as a fallback (only
// the token has to exist; DefaultIO::fadvise is a no-op where the call is not
// available). Guarded so a platform that already defines them wins.
#ifndef POSIX_FADV_NORMAL
#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_SEQUENTIAL 1
#define POSIX_FADV_RANDOM     2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4
#define POSIX_FADV_NOREUSE    5
#endif


namespace storage {

struct DefaultIO {
	static int open(const char* path, int oflag = O_RDONLY, int mode = 0644) noexcept {
		int fd;
		do {
			fd = ::open(path, oflag | O_CLOEXEC, mode);
		} while (fd == -1 && errno == EINTR);
		return fd;
	}

	static int close(int fd) noexcept {
		// Do NOT retry on EINTR: on most systems the descriptor is already
		// closed by the time the signal is delivered, so a retry would close an
		// unrelated fd that raced into the same number.
		return ::close(fd);
	}

	static off_t lseek(int fd, off_t offset, int whence) noexcept {
		return ::lseek(fd, offset, whence);
	}

	static ssize_t write(int fd, const void* buf, size_t nbyte) noexcept {
		const auto* p = static_cast<const char*>(buf);
		while (nbyte != 0) {
			ssize_t c = ::write(fd, p, nbyte);
			if (c == -1) {
				if (errno == EINTR) { continue; }
				size_t written = p - static_cast<const char*>(buf);
				return written == 0 ? -1 : static_cast<ssize_t>(written);
			}
			p += c;
			if (c == static_cast<ssize_t>(nbyte)) { break; }
			nbyte -= c;
		}
		return p - static_cast<const char*>(buf);
	}

	static ssize_t pwrite(int fd, const void* buf, size_t nbyte, off_t offset) noexcept {
		const auto* p = static_cast<const char*>(buf);
		while (nbyte != 0) {
			ssize_t c = ::pwrite(fd, p, nbyte, offset);
			if (c == -1) {
				if (errno == EINTR) { continue; }
				size_t written = p - static_cast<const char*>(buf);
				return written == 0 ? -1 : static_cast<ssize_t>(written);
			}
			p += c;
			if (c == static_cast<ssize_t>(nbyte)) { break; }
			nbyte -= c;
			offset += c;
		}
		return p - static_cast<const char*>(buf);
	}

	static ssize_t read(int fd, void* buf, size_t nbyte) noexcept {
		auto* p = static_cast<char*>(buf);
		while (nbyte != 0) {
			ssize_t c = ::read(fd, p, nbyte);
			if (c == -1) {
				if (errno == EINTR) { continue; }
				size_t got = p - static_cast<char*>(buf);
				return got == 0 ? -1 : static_cast<ssize_t>(got);
			}
			if (c == 0) { break; }  // EOF
			p += c;
			if (c == static_cast<ssize_t>(nbyte)) { break; }
			nbyte -= c;
		}
		return p - static_cast<char*>(buf);
	}

	static ssize_t pread(int fd, void* buf, size_t nbyte, off_t offset) noexcept {
		// Mirrors Xapiand io::pread: a single (EINTR-safe) pread; the caller
		// loops if it needs more, and tolerates a short count.
		auto* p = static_cast<char*>(buf);
		while (nbyte != 0) {
			ssize_t c = ::pread(fd, p, nbyte, offset);
			if (c == -1) {
				if (errno == EINTR) { continue; }
				size_t got = p - static_cast<char*>(buf);
				return got == 0 ? -1 : static_cast<ssize_t>(got);
			}
			p += c;
			break;  // a read need not fill the whole buffer
		}
		return p - static_cast<char*>(buf);
	}

	static int fsync(int fd) noexcept {
		int r;
#if defined(__APPLE__)
		// Plain fsync on macOS does not force the platter; that is what
		// full_fsync (F_FULLFSYNC) is for. The cheap path keeps fsync.
		do { r = ::fsync(fd); } while (r == -1 && errno == EINTR);
#else
		// Skip the metadata flush where the platform offers it.
		do { r = ::fdatasync(fd); } while (r == -1 && errno == EINTR);
#endif
		return r;
	}

	static int full_fsync(int fd) noexcept {
		int r;
#if defined(F_FULLFSYNC)
		do { r = ::fcntl(fd, F_FULLFSYNC, 0); } while (r == -1 && errno == EINTR);
#else
		do { r = ::fsync(fd); } while (r == -1 && errno == EINTR);
#endif
		return r;
	}

	static int fallocate(int fd, int /*mode*/, off_t offset, off_t len) noexcept {
#if defined(__APPLE__) && defined(F_PREALLOCATE)
		// Ask for a contiguous chunk first, fall back to fragmented, then make
		// the size stick with ftruncate (F_PREALLOCATE alone does not grow EOF).
		fstore_t store = {F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, offset + len, 0};
		int err;
		do { err = ::fcntl(fd, F_PREALLOCATE, &store); } while (err == -1 && errno == EINTR);
		if (err == -1) {
			store.fst_flags = F_ALLOCATEALL;
			do { err = ::fcntl(fd, F_PREALLOCATE, &store); } while (err == -1 && errno == EINTR);
		}
		if (err != -1) {
			int t;
			do { t = ::ftruncate(fd, offset + len); } while (t == -1 && errno == EINTR);
		}
		return err;
#else
		return ::posix_fallocate(fd, offset, len) == 0 ? 0 : -1;
#endif
	}

	static int fadvise([[maybe_unused]] int fd, [[maybe_unused]] off_t offset,
	                   [[maybe_unused]] off_t len, [[maybe_unused]] int advice) noexcept {
#if defined(__linux__)
		return ::posix_fadvise(fd, offset, len, advice) == 0 ? 0 : -1;
#else
		return 0;  // advisory only; a no-op is correct where unavailable
#endif
	}
};

}  // namespace storage
