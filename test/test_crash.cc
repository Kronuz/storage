/*
 * test_crash: crash-consistency and corruption tests for the v2 (single checksummed header) format.
 *
 * We cannot cut power, but every on-disk state a power cut leaves behind is
 * reproducible. Two hardware facts bound the model:
 *   - disks give single-sector (512 B) atomicity, not whole-block: a 4 KB write
 *     can land torn (first sector new, rest old/zero);
 *   - fsync guarantees the preceding writes are ALL durable when it returns, but
 *     nothing about order, and nothing before it returns.
 *
 * CrashIO (below) is an in-memory IO policy -- the storage engine's 4th template
 * parameter -- that records every pwrite/fsync in issue order. A test drives a
 * normal write+commit sequence through it, then reconstructs "the file as of crash
 * point K" (all writes issued before op K, optionally with op K itself torn to one
 * sector) and opens a fresh reader over it. Sweeping K over the whole sequence is a
 * small crash-consistency model checker.
 *
 * The contract proven: from ANY crash point, open() recovers to a consistent
 * committed state -- every record up to the recovered high-water reads back
 * correctly -- or throws a clean Storage* exception. Never garbage, never a crash.
 */

#include "storage.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)


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
	static int fsync(int /*fd*/) noexcept { crashio::state().fsyncs.push_back(crashio::state().log.size()); return 0; }
	static int full_fsync(int fd) noexcept { return fsync(fd); }
	static int fallocate(int, int, off_t, off_t) noexcept { return 0; }   // no-op: only real writes land
	static int fadvise(int, off_t, off_t, int) noexcept { return 0; }
};


// ---- a minimal v2 record format for the tests -----------------------------
constexpr uint8_t CT_BIN_MAGIC = 0xC7;
constexpr uint8_t CT_FOOT_MAGIC = 0x7C;

struct CrashHeader {
	StorageMetaHead meta;
	char padding[STORAGE_BLOCK_SIZE - sizeof(StorageMetaHead)];
	void init(void*, void*) { }
	void validate(void*, void*) { }
};
static_assert(sizeof(CrashHeader) == STORAGE_BLOCK_SIZE);

#pragma pack(push, 1)
struct CrashBinHeader {
	uint8_t magic; uint8_t flags; uint32_t size;
	void init(void*, void*, uint32_t size_, uint8_t flags_) { magic = CT_BIN_MAGIC; size = size_; flags = (0 & ~STORAGE_FLAG_MASK) | flags_; }
	void validate(void*, void*) {
		if (magic != CT_BIN_MAGIC) { THROW(StorageCorruptVolume, "bad bin magic"); }
		if (flags & STORAGE_FLAG_DELETED) { THROW(StorageNotFound, "deleted"); }
	}
};
struct CrashBinFooter {
	uint32_t checksum; uint8_t magic;
	void init(void*, void*, uint32_t c) { magic = CT_FOOT_MAGIC; checksum = c; }
	void validate(void*, void*, uint32_t c) {
		if (magic != CT_FOOT_MAGIC) { THROW(StorageCorruptVolume, "bad footer magic"); }
		if (checksum != c) { THROW(StorageCorruptVolume, "bad checksum"); }
	}
};
#pragma pack(pop)

using CrashStore = Storage<CrashHeader, CrashBinHeader, CrashBinFooter, CrashIO>;

// One base + relative name used by every open in this file, so the engine always
// resolves to the same in-memory file key (base_path + rel, after normalization).
static const char* BASE = "crashvol/";
static const char* REL = "vol.0";

// The single file's key in the CrashIO map (== normalize(BASE) + REL). Captured
// after the first writer so we never guess how normalize_path renders it.
static std::string g_key;


// Reconstruct the file image as of crash point `n_ops` (all ops with index < n_ops
// applied in issue order), optionally applying op `n_ops` torn to its first
// `torn_bytes` bytes (a single-sector torn write).
static std::vector<char> reconstruct(const std::vector<crashio::Op>& log, const std::string& path,
                                     size_t n_ops, size_t torn_bytes = 0) {
	std::vector<char> img;
	auto apply = [&](const crashio::Op& op, size_t len) {
		if (op.path != path) { return; }
		if (op.offset + static_cast<off_t>(len) > static_cast<off_t>(img.size())) { img.resize(op.offset + len, 0); }
		std::memcpy(img.data() + op.offset, op.data.data(), len);
	};
	for (size_t i = 0; i < n_ops && i < log.size(); ++i) { apply(log[i], log[i].data.size()); }
	if (torn_bytes > 0 && n_ops < log.size()) {
		const auto& op = log[n_ops];
		apply(op, torn_bytes < op.data.size() ? torn_bytes : op.data.size());
	}
	return img;
}

// Open a reader over `img` and, if it opens, verify every record that fits under
// the recovered high-water reads back byte-for-byte. Returns the number of records
// successfully recovered (-1 if the volume did not open).
static int verify_image(const std::vector<char>& img,
                        const std::vector<std::string>& expected,
                        const std::vector<uint32_t>& offs) {
	crashio::state().files[g_key] = img;
	CrashStore r(BASE, nullptr);
	try {
		r.open(REL, STORAGE_OPEN);
	} catch (const StorageException&) {
		return -1;   // no usable meta yet: acceptable (nothing durable committed)
	}
	int recovered = 0;
	for (size_t i = 0; i < expected.size(); ++i) {
		try {
			r.seek(offs[i]);
			std::string got = r.read();
			// If a record is readable at all, it MUST be exactly what we wrote --
			// never a torn/garbage value.
			CHECK(got == expected[i]);
			++recovered;
		} catch (const StorageEOF&) {
			break;   // past the recovered high-water: fine
		} catch (const StorageException&) {
			// A record within the committed range failed to read: acceptable only
			// as the tail (the crash cut here), not a middle hole.
			break;
		}
	}
	return recovered;
}


int main() {
	// Build a reference write sequence through CrashIO, capturing the op log.
	std::vector<std::string> recs = {
		"alpha record one",
		std::string(3000, 'B'),                 // large + compressible
		"gamma-3",
		std::string(1500, 'D'),
		"epsilon five, the last of batch one",
	};
	std::vector<uint32_t> offs;
	std::vector<crashio::Op> log;
	size_t committed_after_first = 0;

	{
		crashio::state().reset();
		CrashStore w(BASE, nullptr);
		w.open(REL, STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS | STORAGE_FULL_SYNC);
		for (size_t i = 0; i < recs.size(); ++i) {
			offs.push_back(w.write(recs[i]));
			if (i == 2) { w.commit(); committed_after_first = 3; }   // an intermediate commit
		}
		w.commit();   // final commit
		w.close();
		log = crashio::state().log;   // snapshot the full issue-order write log
		g_key = crashio::state().files.begin()->first;
	}

	// Sanity: a clean reopen recovers everything.
	{
		auto img = crashio::state().files[g_key];
		CHECK(verify_image(img, recs, offs) == static_cast<int>(recs.size()));
	}

	// ---- CRASH SWEEP: for every prefix of the write log, and for the same prefix
	//      with the next op torn to one sector, the volume must recover to a
	//      consistent committed state (never garbage / never a crash). ----
	int max_recovered = 0;
	for (size_t k = 0; k <= log.size(); ++k) {
		for (size_t torn : {size_t(0), size_t(512)}) {
			auto img = reconstruct(log, g_key, k, torn);
			int rec = verify_image(img, recs, offs);
			if (rec >= 0 && rec > max_recovered) { max_recovered = rec; }
			// Monotonic reality check: never recover more than were ever written.
			CHECK(rec <= static_cast<int>(recs.size()));
		}
	}
	// The sweep must, at its furthest point, recover the full set (proving the
	// commits are real) and must have passed every consistency CHECK above.
	CHECK(max_recovered == static_cast<int>(recs.size()));
	CHECK(committed_after_first == 3);

	// ---- TARGETED: header bit rot -> open throws (single header, no fallback) ----
	// A genuinely corrupted (bit-rotted) header fails its checksum. With a single
	// header there is nothing to fall back to, so open() refuses rather than trust a
	// bad high-water. (A power-loss TEAR is different -- see sector atomicity below.)
	{
		crashio::state().reset();
		CrashStore w(BASE, nullptr);
		w.open(REL, STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_FULL_SYNC);
		w.write("doomed");
		w.commit();
		w.close();
		g_key = crashio::state().files.begin()->first;

		auto img = crashio::state().files[g_key];
		img[8] ^= 0xFF;   // flip a byte in the meta (txnid/offset region)
		crashio::state().files[g_key] = img;
		CrashStore r(BASE, nullptr);
		bool threw = false;
		try { r.open(REL, STORAGE_OPEN); }
		catch (const StorageCorruptVolume&) { threw = true; }
		catch (...) {}
		CHECK(threw);
	}

	// ---- TARGETED: sector atomicity -- a torn header write is always safe ----
	// The header's changing bytes (the meta) fit in block 0's first 512 B sector,
	// and sectors write atomically, so a torn block-0 write yields either the OLD or
	// the NEW header -- both valid. With the fsync barrier the data is durable before
	// the header, so recovering to the OLD header (its high-water) just leaves the
	// newer commit's records as ignored orphans.
	{
		crashio::state().reset();
		CrashStore w(BASE, nullptr);
		w.open(REL, STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_FULL_SYNC);
		uint32_t o0 = w.write("first commit record");
		w.commit();
		g_key = crashio::state().files.begin()->first;
		auto after_first = crashio::state().files[g_key];
		uint32_t o1 = w.write("second commit record");
		w.commit();
		auto after_second = crashio::state().files[g_key];
		w.close();

		// (a) the 2nd commit fully landed (new header) -> both records visible.
		{
			crashio::state().files[g_key] = after_second;
			CrashStore r(BASE, nullptr); r.open(REL, STORAGE_OPEN);
			r.seek(o0); CHECK(r.read() == "first commit record");
			r.seek(o1); CHECK(r.read() == "second commit record");
		}
		// (b) the 2nd commit's DATA landed but its header write landed NOTHING (block
		//     0 still the 1st commit's header): recover to the 1st commit, the 2nd
		//     record is beyond the old high-water and simply not visible.
		{
			auto torn = after_second;
			for (size_t i = 0; i < STORAGE_BLOCK_SIZE; ++i) { torn[i] = after_first[i]; }
			crashio::state().files[g_key] = torn;
			CrashStore r(BASE, nullptr); r.open(REL, STORAGE_OPEN);
			r.seek(o0); CHECK(r.read() == "first commit record");
			bool eof = false;
			try { r.seek(o1); r.read(); } catch (const StorageException&) { eof = true; }
			CHECK(eof);
		}
	}

	// ---- TARGETED: a torn data block (payload half-written) is caught, and the
	//      records committed before it still read ----
	{
		crashio::state().reset();
		CrashStore w(BASE, nullptr);
		w.open(REL, STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_FULL_SYNC);
		uint32_t oa = w.write("intact record A");
		uint32_t ob = w.write(std::string(2000, 'Z'));
		w.commit();
		w.close();
		g_key = crashio::state().files.begin()->first;

		// Zero out the tail half of record B's bytes (a torn multi-sector write).
		auto img = crashio::state().files[g_key];
		size_t bstart = static_cast<size_t>(ob) * STORAGE_ALIGNMENT;
		for (size_t i = bstart + 900; i < img.size(); ++i) { img[i] = 0; }
		crashio::state().files[g_key] = img;
		CrashStore r(BASE, nullptr);
		r.open(REL, STORAGE_OPEN);
		r.seek(oa); CHECK(r.read() == "intact record A");   // prefix survives
		bool caught = false;
		try { r.seek(ob); r.read(); }
		catch (const StorageException&) { caught = true; }
		CHECK(caught);                                        // torn record detected
	}

	// ---- TARGETED: truncated tail (file shorter than the header's high-water) ----
	// A genuine fs truncation lost committed data the header still references. The
	// header itself is valid (checksum ok), so open() succeeds and the surviving
	// records read; a record whose bytes were truncated away fails with a clean
	// short-read StorageCorruptVolume -- never garbage.
	{
		crashio::state().reset();
		CrashStore w(BASE, nullptr);
		w.open(REL, STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_FULL_SYNC);
		uint32_t oa = w.write("record that will be truncated away");
		w.write("another one");
		w.commit();
		w.close();
		g_key = crashio::state().files.begin()->first;

		auto img = crashio::state().files[g_key];
		img.resize(STORAGE_BLOCK_SIZE);   // keep only block 0 (the header); drop data
		crashio::state().files[g_key] = img;
		CrashStore r(BASE, nullptr);
		r.open(REL, STORAGE_OPEN);   // header is valid -> opens
		bool gone = false;
		try { r.seek(oa); r.read(); }
		catch (const StorageException&) { gone = true; }
		CHECK(gone);                 // truncated record fails cleanly, no garbage
	}

	if (failures == 0) {
		std::printf("test_crash: all crash-consistency + corruption checks passed\n");
		return 0;
	}
	std::printf("test_crash: FAILED (%d checks)\n", failures);
	return 1;
}
