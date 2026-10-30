/*
 * crashio.h: an in-memory, write-logging IO policy for the crash tests. Include
 * after "storage.h".
 */
#pragma once

#include <map>
#include <string>
#include <vector>

// ---- CrashIO: an in-memory, write-logging IO policy ------------------------
namespace crashio {

struct Op { std::string path; off_t offset; std::vector<char> data; };

struct State {
	std::map<std::string, std::vector<char>> files;   // path -> current bytes
	std::map<int, std::string> fd_path;               // open fd -> path
	std::map<int, off_t> fd_pos;                       // open fd -> read/write cursor
	std::vector<Op> log;                               // every pwrite/write, in order
	std::vector<size_t> fsyncs;                        // log index at each fsync
	int next_fd = 100;

	void reset() {
		files.clear(); fd_path.clear(); fd_pos.clear();
		log.clear(); fsyncs.clear(); next_fd = 100;
	}
};

inline State& state() { static State s; return s; }

}  // namespace crashio

struct CrashIO {
	static int open(const char* path, int oflag = O_RDONLY, int /*mode*/ = 0644) noexcept {
		auto& s = crashio::state();
		std::string p(path);
		bool exists = s.files.count(p) != 0;
		if (!exists) {
			if ((oflag & O_CREAT) == 0) { errno = ENOENT; return -1; }
			s.files[p] = {};
		}
		int fd = s.next_fd++;
		s.fd_path[fd] = p;
		s.fd_pos[fd] = 0;
		return fd;
	}
	static int close(int fd) noexcept {
		auto& s = crashio::state();
		s.fd_path.erase(fd); s.fd_pos.erase(fd);
		return 0;
	}
	static off_t lseek(int fd, off_t offset, int whence) noexcept {
		auto& s = crashio::state();
		auto& bytes = s.files[s.fd_path[fd]];
		off_t pos = 0;
		if (whence == SEEK_SET) { pos = offset; }
		else if (whence == SEEK_END) { pos = static_cast<off_t>(bytes.size()) + offset; }
		else { pos = s.fd_pos[fd] + offset; }
		s.fd_pos[fd] = pos;
		return pos;
	}
	static ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) noexcept {
		auto& s = crashio::state();
		const std::string& p = s.fd_path[fd];
		auto& bytes = s.files[p];
		if (off + static_cast<off_t>(n) > static_cast<off_t>(bytes.size())) {
			bytes.resize(off + n, 0);
		}
		std::memcpy(bytes.data() + off, buf, n);
		s.log.push_back({p, off, std::vector<char>(static_cast<const char*>(buf), static_cast<const char*>(buf) + n)});
		return static_cast<ssize_t>(n);
	}
	static ssize_t write(int fd, const void* buf, size_t n) noexcept {
		auto& s = crashio::state();
		ssize_t r = pwrite(fd, buf, n, s.fd_pos[fd]);
		if (r > 0) { s.fd_pos[fd] += r; }
		return r;
	}
	static ssize_t pread(int fd, void* buf, size_t n, off_t off) noexcept {
		auto& s = crashio::state();
		auto& bytes = s.files[s.fd_path[fd]];
		if (off >= static_cast<off_t>(bytes.size())) { return 0; }
		size_t avail = bytes.size() - off;
		size_t k = n < avail ? n : avail;
		std::memcpy(buf, bytes.data() + off, k);
		return static_cast<ssize_t>(k);
	}
	static ssize_t read(int fd, void* buf, size_t n) noexcept {
		auto& s = crashio::state();
		ssize_t r = pread(fd, buf, n, s.fd_pos[fd]);
		if (r > 0) { s.fd_pos[fd] += r; }
		return r;
	}
	static int lock_shared(int) noexcept { return 0; }       // fake descriptors: nothing to lock
	static int lock_exclusive(int) noexcept { return 0; }
	static int ftruncate(int fd, off_t len) noexcept {
		auto& s = crashio::state();
		s.files[s.fd_path[fd]].resize(static_cast<size_t>(len), 0);
		return 0;
	}
	static int fsync(int /*fd*/) noexcept { crashio::state().fsyncs.push_back(crashio::state().log.size()); return 0; }
	static int full_fsync(int fd) noexcept { return fsync(fd); }
	static int fallocate(int, int, off_t, off_t) noexcept { return 0; }   // no-op: only real writes land
	static int fadvise(int, off_t, off_t, int) noexcept { return 0; }
};
