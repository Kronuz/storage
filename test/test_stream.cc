/*
 * test_stream: the streamed record write (Storage::StreamWriter).
 *
 *   test_stream        round trips around the block, chunk, 16 MiB and 32 MiB
 *                      sizes; byte equality with write(); interleaving with other
 *                      writes; misuse; abandoned writers; a crash at every write
 *                      and chunk boundary (CrashIO); orphan volume reclaim after a
 *                      killed writer.
 *   test_stream big    4 GiB - 1 byte, and records that push a later record past
 *                      4 GiB, in sparse files (SparseIO keeps all-zero blocks out
 *                      of the file, so the test needs about 150 MB of disk, not 8 GiB).
 */

#include "storage.h"

#include "crashio.h"

#include <sys/stat.h>
#include <sys/wait.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
#define CHECK_THROWS(stmt, exc) do { \
		bool _threw = false; \
		try { stmt; } catch (const exc&) { _threw = true; } catch (...) {} \
		if (!_threw) { std::printf("FAIL %s:%d: expected %s from: %s\n", __FILE__, __LINE__, #exc, #stmt); ++failures; } \
	} while (0)


// ---- a Xapiand-shaped record format: magic'd header, checksummed footer ------
#pragma pack(push, 1)
struct SBinHeader {
	uint8_t magic;
	uint8_t flags;
	uint32_t size;
	void init(void*, void*, uint32_t size_, uint8_t flags_) { magic = STORAGE_BIN_HEADER_MAGIC; size = size_; flags = flags_; }
	void validate(void*, void*) {
		if (magic != STORAGE_BIN_HEADER_MAGIC) { THROW(StorageCorruptVolume, "bad header magic"); }
		if (flags & STORAGE_FLAG_DELETED) { THROW(StorageNotFound, "deleted"); }
	}
};
struct SBinFooter {
	uint32_t checksum;
	uint8_t magic;
	void init(void*, void*, uint32_t checksum_) { magic = STORAGE_BIN_FOOTER_MAGIC; checksum = checksum_; }
	void validate(void*, void*, uint32_t checksum_) {
		if (magic != STORAGE_BIN_FOOTER_MAGIC) { THROW(StorageCorruptVolume, "bad footer magic"); }
		if (checksum != checksum_) { THROW(StorageCorruptVolume, "bad checksum"); }
	}
};
#pragma pack(pop)

template <typename IO>
using Store = Storage<StorageHeader, SBinHeader, SBinFooter, IO>;
using DStore = Store<STORAGE_DEFAULT_IO>;

constexpr int WR = STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE;


// ---- deterministic payloads --------------------------------------------------
static uint8_t pat(uint64_t i, uint64_t salt) {
	uint64_t x = (i + salt) * 0x9E3779B97F4A7C15ull;
	x ^= x >> 29;
	return static_cast<uint8_t>(x >> 32);
}

static void fill(char* out, uint64_t pos, size_t n, uint64_t salt) {
	for (size_t i = 0; i < n; ++i) { out[i] = static_cast<char>(pat(pos + i, salt)); }
}

static std::string payload(size_t n, uint64_t salt) {
	std::string s(n, '\0');
	fill(s.data(), 0, n, salt);
	return s;
}

// Next chunk length from a deterministic sequence around `chunk` (0 = fixed).
struct Chunker {
	size_t chunk;
	bool jitter;
	uint64_t state = 88172645463325252ull;
	size_t next() {
		if (!jitter) { return chunk; }
		state ^= state << 13; state ^= state >> 7; state ^= state << 17;
		return 1 + state % (2 * chunk);
	}
};

static uint32_t stream_in(DStore& st, uint64_t size, size_t chunk, bool jitter, uint64_t salt) {
	DStore::StreamWriter w(st, size);
	Chunker c{chunk, jitter};
	std::string buf;
	for (uint64_t pos = 0; pos < size;) {
		size_t n = static_cast<size_t>(std::min<uint64_t>(c.next(), size - pos));
		buf.resize(n);
		fill(buf.data(), pos, n, salt);
		w.append(buf);
		pos += n;
	}
	return w.commit();
}

// Reads the record at `off` in odd-sized pieces; true iff it is payload(size, salt).
template <typename S>
static bool reads_back(S& st, uint32_t off, uint64_t size, uint64_t salt) {
	try {
		st.seek(off);
		std::vector<char> got(5003), want(5003);
		uint64_t pos = 0;
		while (size_t n = st.read(got.data(), got.size())) {
			if (pos + n > size) { return false; }
			fill(want.data(), pos, n, salt);
			if (memcmp(got.data(), want.data(), n) != 0) { return false; }
			pos += n;
		}
		return pos == size;  // the final read(), which returned 0, verified the footer
	} catch (const StorageException&) {
		return false;
	}
}


static std::string make_dir(const char* tag) {
	std::string d = std::string("./stream_") + tag + ".XXXXXX";
	char* r = mkdtemp(d.data());
	if (!r) { std::perror("mkdtemp"); std::exit(2); }
	return std::string(r) + "/";
}

static std::vector<char> slurp(const std::string& path, uint64_t from, uint64_t n) {
	std::ifstream f(path, std::ios::binary);
	f.seekg(static_cast<std::streamoff>(from));
	std::vector<char> v(n);
	f.read(v.data(), static_cast<std::streamsize>(n));
	v.resize(static_cast<size_t>(f.gcount()));
	return v;
}


// ---- round trips ------------------------------------------------------------
static void test_roundtrips() {
	auto dir = make_dir("rt");
	constexpr uint64_t MiB = 1024 * 1024;
	std::vector<uint64_t> sizes = {0, 1, 7, 8, 9};
	for (uint64_t s = 4070; s <= 4110; ++s) { sizes.push_back(s); }   // the first block's edge
	for (uint64_t s = 8170; s <= 8200; ++s) { sizes.push_back(s); }   // the second's
	for (uint64_t d : {MiB - 1, MiB, MiB + 1, 16 * MiB - 1, 16 * MiB, 16 * MiB + 1, 32 * MiB - 1, 32 * MiB, 32 * MiB + 1}) { sizes.push_back(d); }

	int n = 0;
	for (uint64_t size : sizes) {
		std::vector<std::pair<size_t, bool>> chunkings = {{65536, false}, {4096, false}, {1000, true}};
		if (size >= MiB) { chunkings = {{MiB, false}, {65536, true}, {4096, false}}; }
		for (auto [chunk, jitter] : chunkings) {
			auto vol = "v" + std::to_string(n++);
			uint64_t salt = size * 31 + chunk;
			uint32_t off;
			{
				DStore w(dir, nullptr);
				w.open(vol, WR | STORAGE_COMPRESS_ZSTD);
				off = stream_in(w, size, chunk, jitter, salt);
				CHECK(reads_back(w, off, size, salt));          // by the writer itself
			}
			DStore r(dir, nullptr);
			r.open(vol, STORAGE_OPEN);
			CHECK(off == STORAGE_START_BLOCK_OFFSET);
			if (!reads_back(r, off, size, salt)) {
				std::printf("FAIL: round trip size=%llu chunk=%zu jitter=%d\n", (unsigned long long)size, chunk, jitter);
				++failures;
			}
			if (size <= 1 << 20) {
				r.seek(off);
				CHECK(r.read() == payload(size, salt));
			}
			std::filesystem::remove(dir + vol);
		}
	}
	std::filesystem::remove_all(dir);
}


// Existing readers read streamed records, and the bytes are write()'s bytes.
static void test_compat() {
	auto dir = make_dir("compat");
	for (uint64_t size : {0ull, 1ull, 100ull, 4085ull, 4090ull, 4096ull, 10000ull, 200000ull}) {
		uint64_t salt = size + 5;
		auto p = payload(size, salt);
		uint32_t woff, soff;
		{
			DStore a(dir, nullptr);
			a.open("wa", WR);  // no compression flag: write() stores raw
			woff = a.write(p);
			a.commit();
		}
		{
			DStore b(dir, nullptr);
			b.open("sb", WR);
			soff = stream_in(b, size, 777, false, salt);
		}
		CHECK(woff == soff);
		uint64_t rec = sizeof(SBinHeader) + size + sizeof(SBinFooter);
		CHECK(slurp(dir + "wa", woff * 8ull, rec) == slurp(dir + "sb", soff * 8ull, rec));
		CHECK(slurp(dir + "sb", soff * 8ull, 2)[1] == 0);   // flags: neither compressed nor deleted
	}

	// Interleaved with compressed and raw write()s in one volume; each record is
	// read by seek + the whole-record read().
	DStore v(dir, nullptr);
	v.open("mix", WR | STORAGE_COMPRESS_ZSTD);
	auto p1 = payload(50000, 1), p2 = payload(300, 2), p4 = payload(70000, 4), p5 = std::string("tail");
	uint32_t o1 = v.write(p1);
	uint32_t o2 = v.write(p2);
	uint32_t o3 = stream_in(v, 123457, 4096, false, 3);
	uint32_t o4 = v.write(p4);
	uint32_t o5 = stream_in(v, p5.size(), 2, false, 6);
	v.commit();
	DStore r(dir, nullptr);
	r.open("mix", STORAGE_OPEN);
	r.seek(o1); CHECK(r.read() == p1);
	r.seek(o2); CHECK(r.read() == p2);
	CHECK(reads_back(r, o3, 123457, 3));
	r.seek(o4); CHECK(r.read() == p4);
	CHECK(reads_back(r, o5, p5.size(), 6));
	std::filesystem::remove_all(dir);
}


// A header damaged to say "compressed" over an uncompressed payload: the codec fails, and
// the reader reports a Storage* exception (not the codec's own).
static void test_damaged_flags() {
	auto dir = make_dir("flags");
	for (uint8_t flags : {uint8_t(0x01), uint8_t(0x05), uint8_t(0x09)}) {   // LZ4, Zstandard, Deflate
		uint32_t off;
		{
			DStore w(dir, nullptr);
			w.open("d", WR);
			off = stream_in(w, 5000, 1000, false, 1);
		}
		{
			std::fstream f(dir + "d", std::ios::in | std::ios::out | std::ios::binary);
			f.seekp(off * 8ll + 1);
			f.put(static_cast<char>(flags));
		}
		DStore r(dir, nullptr);
		r.open("d", STORAGE_OPEN);
		CHECK_THROWS(({ r.seek(off); r.read(); }), StorageCorruptVolume);
		std::filesystem::remove(dir + "d");
	}
	std::filesystem::remove_all(dir);
}

static void test_limits_and_misuse() {
	auto dir = make_dir("misuse");
	DStore st(dir, nullptr);
	st.open("m", WR);

	// A size no 32-bit header can hold: refused, and the storage is untouched.
	CHECK(DStore::stream_max_size == 0xFFFFFFFFull);
	CHECK_THROWS(DStore::StreamWriter(st, DStore::stream_max_size + 1), StorageTooLarge);
	CHECK_THROWS(DStore::StreamWriter(st, ~0ull), StorageTooLarge);
	uint32_t o = st.write("after refusal");
	st.commit();
	st.seek(o); CHECK(st.read() == "after refusal");

	{
		DStore::StreamWriter w(st, 10);
		CHECK_THROWS(st.write("x"), StorageException);                       // no write() mid-stream
		CHECK_THROWS(DStore::StreamWriter(st, 1), StorageException);         // one stream at a time
		CHECK_THROWS(st.reopen(), StorageException);
		w.append("12345");
		CHECK_THROWS(w.append("1234567"), StorageException);                  // past the declared size
		CHECK(w.remaining() == 5);
		CHECK_THROWS(w.commit(), StorageException);                           // short
		w.append("67890");
		uint32_t so = w.commit();
		CHECK_THROWS(w.append("x"), StorageException);                        // a committed writer is spent
		st.seek(so); CHECK(st.read() == "1234567890");
		DStore other(dir, nullptr);   // published: another opener reads it now
		other.open("m", STORAGE_OPEN);
		other.seek(so); CHECK(other.read() == "1234567890");
	}
	// The storage takes write() again.
	uint32_t o2 = st.write("again");
	st.commit();
	st.seek(o2); CHECK(st.read() == "again");

	// A read-only storage and a closed one refuse.
	DStore ro(dir, nullptr);
	ro.open("m", STORAGE_OPEN);
	CHECK_THROWS(DStore::StreamWriter(ro, 1), StorageIOError);
	st.close();
	CHECK_THROWS(DStore::StreamWriter(st, 1), StorageClosedError);

	// close() mid-stream abandons the writer; the writer then fails cleanly.
	DStore c(dir, nullptr);
	c.open("c", WR);
	{
		DStore::StreamWriter w(c, 100);
		w.append(std::string(50, 'a'));
		c.close();
		CHECK_THROWS(w.append("b"), StorageClosedError);
		CHECK_THROWS(w.commit(), StorageClosedError);
	}
	std::filesystem::remove_all(dir);
}


// An abandoned writer leaves nothing a reader can see, and the storage is as it was.
static void test_abandon() {
	auto dir = make_dir("abandon");
	DStore st(dir, nullptr);
	st.open("a", WR);
	// A record sits in the tail block, uncommitted, when the stream begins.
	auto pa = payload(300, 11), pb = payload(500, 12);
	uint32_t oa = st.write(pa);
	st.commit();
	uint32_t oc = st.write(std::string("uncommitted tail"));
	{
		DStore::StreamWriter w(st, 100000);
		for (int i = 0; i < 20; ++i) {   // crosses many blocks, the first one included
			w.append(payload(4000, 100 + i));
			DStore reader(dir, nullptr);
			reader.open("a", STORAGE_OPEN);
			reader.seek(oa); CHECK(reader.read() == pa);
			CHECK_THROWS(({ reader.seek(oc); reader.read(); }), StorageEOF);   // nothing past oa is published
		}
		// destruction abandons it
	}
	uint32_t ob = st.write(pb);
	st.commit();
	DStore r(dir, nullptr);
	r.open("a", STORAGE_OPEN);
	r.seek(oa); CHECK(r.read() == pa);
	r.seek(oc); CHECK(r.read() == "uncommitted tail");
	r.seek(ob); CHECK(r.read() == pb);

	// An abandoned stream on a fresh volume publishes nothing: a reader finds the
	// volume empty.
	{
		DStore f(dir, nullptr);
		f.open("f", WR);
		DStore::StreamWriter w(f, 50000);
		w.append(payload(30000, 1));
		w.abort();
		DStore reader(dir, nullptr);
		reader.open("f", STORAGE_OPEN);
		CHECK_THROWS(({ reader.seek(STORAGE_START_BLOCK_OFFSET); reader.read(); }), StorageEOF);
	}
	std::filesystem::remove_all(dir);
}


// ---- crash consistency (CrashIO) ----------------------------------------------
using CStore = Store<CrashIO>;
static const char* CBASE = "crashvol/";

static std::vector<char> reconstruct(const std::vector<crashio::Op>& log, const std::string& path, size_t n_ops, size_t torn) {
	std::vector<char> img;
	auto apply = [&](const crashio::Op& op, size_t len) {
		if (op.path != path) { return; }
		if (op.offset + static_cast<off_t>(len) > static_cast<off_t>(img.size())) { img.resize(op.offset + len, 0); }
		std::memcpy(img.data() + op.offset, op.data.data(), len);
	};
	for (size_t i = 0; i < n_ops && i < log.size(); ++i) { apply(log[i], log[i].data.size()); }
	if (torn > 0 && n_ops < log.size()) { apply(log[n_ops], std::min(torn, log[n_ops].data.size())); }
	return img;
}

// What a reader of `img` sees: 0 = nothing of the streamed record, 1 = all of it;
// anything else (a partial or garbled record) is a failure. -1 = does not open.
static int streamed_visible(const std::string& key, const std::vector<char>& img, uint32_t prior_off,
                            const std::string& prior, uint32_t off, const std::string& want) {
	crashio::state().files[key] = img;
	CStore r(CBASE, nullptr);
	try {
		r.open("vol.0", STORAGE_OPEN);
	} catch (const StorageException&) {
		return -1;
	}
	try {
		r.seek(prior_off);
		if (r.read() != prior) { return 99; }
	} catch (const StorageException&) {
		return -1;   // before the first commit: nothing durable yet
	}
	try {
		r.seek(off);
		return r.read() == want ? 1 : 99;
	} catch (const StorageEOF&) {
		return 0;
	} catch (const StorageException&) {
		return 99;
	}
}

static void test_crash_sweep() {
	const std::string prior = "committed before the stream";
	const std::string body = payload(37001, 77);
	std::vector<std::string> snapshots;
	std::vector<size_t> after_append_ops;
	uint32_t prior_off, off;
	std::string key;
	crashio::state().reset();
	{
		CStore w(CBASE, nullptr);
		w.open("vol.0", WR | STORAGE_FULL_SYNC);
		prior_off = w.write(prior);
		w.commit();
		key = crashio::state().files.begin()->first;
		CStore::StreamWriter sw(w, body.size());
		for (size_t pos = 0; pos < body.size(); pos += 3000) {
			sw.append(std::string_view(body).substr(pos, 3000));
			after_append_ops.push_back(crashio::state().log.size());
		}
		off = sw.commit();
		w.close();
	}
	CHECK(off > prior_off);
	auto log = crashio::state().log;
	auto final_img = crashio::state().files[key];
	CHECK(streamed_visible(key, final_img, prior_off, prior, off, body) == 1);

	// The image at every chunk boundary (every append returned, commit not called)
	// shows none of the record.
	for (size_t ops : after_append_ops) {
		auto img = reconstruct(log, key, ops, 0);
		CHECK(streamed_visible(key, img, prior_off, prior, off, body) == 0);
	}
	// And at every write the sequence issued, whole or torn to a sector.
	int seen_visible = 0, seen_hidden = 0;
	for (size_t k = 0; k <= log.size(); ++k) {
		for (size_t torn : {size_t(0), size_t(512)}) {
			int v = streamed_visible(key, reconstruct(log, key, k, torn), prior_off, prior, off, body);
			CHECK(v == 0 || v == 1 || v == -1);
			(v == 1 ? seen_visible : seen_hidden)++;
		}
	}
	CHECK(seen_visible > 0 && seen_hidden > 0);
	// The record becomes visible only at the commit's header write, the last op.
	CHECK(streamed_visible(key, reconstruct(log, key, log.size() - 1, 0), prior_off, prior, off, body) == 0);
	CHECK(streamed_visible(key, reconstruct(log, key, log.size(), 0), prior_off, prior, off, body) == 1);
}


// ---- orphans after a crash ------------------------------------------------------
static void test_reclaim() {
	auto dir = make_dir("reclaim");
	{
		DStore s(dir, nullptr);
		// data.0: a committed record the host names.
		s.open("data.0", WR);
		stream_in(s, 1000, 100, false, 0);
		// data.3: a committed streamed record no locator names.
		s.open("data.3", WR);
		stream_in(s, 2000, 100, false, 0);
		s.close();
	}
	// data.1: a writer killed mid-upload (no destructors, no close).
	pid_t pid = fork();
	if (pid == 0) {
		DStore k(dir, nullptr);
		k.open("data.1", WR);
		DStore::StreamWriter w(k, 1u << 20);
		for (int i = 0; i < 10; ++i) { w.append(payload(50000, i)); }
		_exit(0);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status));
	// data.2: created but its header never written. data.4: not ours (garbage header).
	// other.7: a different pattern.
	{ std::ofstream(dir + "data.2"); }
	{ std::ofstream f(dir + "data.4", std::ios::binary); f << std::string(4096, 'x'); }
	{ std::ofstream f(dir + "other.7", std::ios::binary); f << "keep"; }
	CHECK(std::filesystem::file_size(dir + "data.1") > 4096);   // the killed writer wrote data

	DStore h(dir, nullptr);
	std::vector<unsigned long long> asked;
	auto removed = h.reclaim_volumes("data", [&](unsigned long long n) { asked.push_back(n); return n == 0; });
	CHECK((removed == std::vector<unsigned long long>{1, 2, 3}));
	CHECK((asked == std::vector<unsigned long long>{0, 3}));   // only volumes holding records
	CHECK(std::filesystem::exists(dir + "data.0"));
	CHECK(!std::filesystem::exists(dir + "data.1"));
	CHECK(!std::filesystem::exists(dir + "data.2"));
	CHECK(!std::filesystem::exists(dir + "data.3"));
	CHECK(std::filesystem::exists(dir + "data.4"));
	CHECK(std::filesystem::exists(dir + "other.7"));
	{
		DStore r(dir, nullptr);
		r.open("data.0", STORAGE_OPEN);
		CHECK(reads_back(r, STORAGE_START_BLOCK_OFFSET, 1000, 0));
	}
	CHECK(h.reclaim_volumes("data", [](unsigned long long) { return true; }).empty());   // idempotent
	std::filesystem::remove_all(dir);
}


// ---- sparse files: 4 GiB without 4 GiB of disk ------------------------------------
namespace sparse {
struct SparseIO : ::storage::DefaultIO {
	static ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) noexcept {
		static const char zero[STORAGE_BLOCK_SIZE] = {};
		const char* p = static_cast<const char*>(buf);
		for (size_t done = 0; done < n;) {
			size_t len = std::min<size_t>(n - done, STORAGE_BLOCK_SIZE);
			if (memcmp(p + done, zero, len) != 0) {
				if (::storage::DefaultIO::pwrite(fd, p + done, len, off + static_cast<off_t>(done)) != static_cast<ssize_t>(len)) { return -1; }
			}
			done += len;
		}
		struct stat sb;
		if (::fstat(fd, &sb) == 0 && sb.st_size < off + static_cast<off_t>(n)) {
			if (::ftruncate(fd, off + static_cast<off_t>(n)) != 0) { return -1; }
		}
		return static_cast<ssize_t>(n);
	}
	static inline off_t last_grow_end = 0;   // the end of the last preallocation asked for
	static int fallocate(int, int, off_t off, off_t len) noexcept { last_grow_end = off + len; return 0; }   // never allocate
	static int fsync(int) noexcept { return 0; }
	static int full_fsync(int) noexcept { return 0; }
};
using SStore = Store<SparseIO>;

// Zero except 16 bytes at every 256 MiB, at the very start, and the last 16.
static void fill_big(char* out, uint64_t pos, size_t n, uint64_t size) {
	memset(out, 0, n);
	auto mark = [&](uint64_t at) {
		for (uint64_t i = 0; i < 16; ++i) {
			uint64_t p = at + i;
			if (p >= pos && p < pos + n && p < size) { out[p - pos] = static_cast<char>(pat(p, 99)); }
		}
	};
	for (uint64_t at = 0; at < size; at += 256ull << 20) { mark(at); }
	mark(size >= 16 ? size - 16 : 0);
}

static uint32_t stream_big(SStore& st, uint64_t size) {
	SStore::StreamWriter w(st, size);
	std::vector<char> buf(64u << 20);
	for (uint64_t pos = 0; pos < size;) {
		size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - pos));
		fill_big(buf.data(), pos, n, size);
		w.append(buf.data(), n);
		pos += n;
	}
	return w.commit();
}

static bool big_reads_back(SStore& st, uint32_t off, uint64_t size) {
	try {
		st.seek(off);
		std::vector<char> got(1u << 20), want(1u << 20);
		uint64_t pos = 0;
		while (size_t n = st.read(got.data(), got.size())) {
			if (pos + n > size) { return false; }
			fill_big(want.data(), pos, n, size);
			if (memcmp(got.data(), want.data(), n) != 0) { return false; }
			pos += n;
		}
		return pos == size;
	} catch (const StorageException&) {
		return false;
	}
}

static uint64_t disk_bytes(const std::string& path) {
	struct stat sb;
	return ::stat(path.c_str(), &sb) == 0 ? static_cast<uint64_t>(sb.st_blocks) * 512 : 0;
}

static void test_big() {
	auto dir = make_dir("big");
	// The largest record: 4 GiB - 1 payload bytes, one past the limit refused.
	{
		SStore w(dir, nullptr);
		w.open("max", WR);
		CHECK_THROWS(SStore::StreamWriter(w, SStore::stream_max_size + 1), StorageTooLarge);
		uint32_t off = stream_big(w, SStore::stream_max_size);
		// The record's extent is reserved once, at begin: the file is that long and the
		// commit does not preallocate beyond it.
		auto extent = static_cast<off_t>((4096 + sizeof(SBinHeader) + SStore::stream_max_size + sizeof(SBinFooter) + 4095) / 4096 * 4096);
		CHECK(SparseIO::last_grow_end == extent);
		CHECK(static_cast<off_t>(std::filesystem::file_size(dir + "max")) == extent);
		w.close();
		SStore r(dir, nullptr);
		r.open("max", STORAGE_OPEN);
		CHECK(off == STORAGE_START_BLOCK_OFFSET);
		CHECK(big_reads_back(r, off, SStore::stream_max_size));
		CHECK(std::filesystem::file_size(dir + "max") > SStore::stream_max_size);
		std::printf("4 GiB - 1 record: file %llu bytes, %llu on disk\n",
			(unsigned long long)std::filesystem::file_size(dir + "max"), (unsigned long long)disk_bytes(dir + "max"));
		CHECK(disk_bytes(dir + "max") < (256ull << 20));
	}
	std::filesystem::remove(dir + "max");

	// Three 1.6 GiB records, then small ones that start past 4 GiB: a reader's
	// byte bound is 64-bit.
	{
		constexpr uint64_t big = 1600ull << 20;
		SStore w(dir, nullptr);
		w.open("far", WR);
		uint32_t o1 = stream_big(w, big), o2 = stream_big(w, big), o3 = stream_big(w, big);
		uint32_t o4 = stream_big(w, 5000), o5 = stream_big(w, 3);
		// A plain write() past 4 GiB commits and may grow the file (the size arithmetic is 64-bit).
		auto before = std::filesystem::file_size(dir + "far");
		SparseIO::last_grow_end = 0;
		uint32_t o6 = w.write(payload(100, 6));
		w.commit();
		CHECK(SparseIO::last_grow_end > static_cast<off_t>(before));
		w.close();
		CHECK(static_cast<uint64_t>(o4) * 8 > (4ull << 30));
		SStore r(dir, nullptr);
		r.open("far", STORAGE_OPEN);
		CHECK(big_reads_back(r, o1, big));
		CHECK(big_reads_back(r, o2, big));
		CHECK(big_reads_back(r, o3, big));
		CHECK(big_reads_back(r, o4, 5000));
		CHECK(big_reads_back(r, o5, 3));
		r.seek(o6); CHECK(r.read() == payload(100, 6));
	}
	std::filesystem::remove_all(dir);
}
}  // namespace sparse



// ---- the volume lock: reclaim never takes a volume a writer holds (L6-B1) ----------
static void test_reclaim_vs_live_writer() {
	auto dir = make_dir("live");
	// An upload in flight on a fresh volume: reclaim finds a header-only file and must leave it.
	{
		DStore w(dir, nullptr);
		w.open("data.5", WR);
		DStore h(dir, nullptr);
		CHECK(h.reclaim_volumes("data.", [](unsigned long long) { return false; }).empty());
		CHECK(std::filesystem::exists(dir + "data.5"));
		uint32_t off = stream_in(w, 9000, 4000, false, 2);
		CHECK(std::filesystem::exists(dir + "data.5"));
		DStore r(dir, nullptr);
		r.open("data.5", STORAGE_OPEN);
		CHECK(reads_back(r, off, 9000, 2));
	}
	// Mid-upload, between two chunks.
	{
		DStore w(dir, nullptr);
		w.open("data.1", WR);
		DStore::StreamWriter sw(w, 6000);
		auto p = payload(6000, 3);
		sw.append(p.data(), 3000);
		DStore h(dir, nullptr);
		CHECK(h.reclaim_volumes("data.", [](unsigned long long n) { return n == 5; }).empty());
		sw.append(p.data() + 3000, 3000);
		uint32_t off = sw.commit();
		// Committed but not yet named, and still open by the writer: still kept.
		CHECK(h.reclaim_volumes("data.", [](unsigned long long n) { return n == 5; }).empty());
		CHECK(std::filesystem::exists(dir + "data.1"));
		DStore r(dir, nullptr);
		r.open("data.1", STORAGE_OPEN);
		CHECK(reads_back(r, off, 6000, 3));
	}
	// Once the writers are closed, the unnamed volumes go.
	{
		DStore h(dir, nullptr);
		auto removed = h.reclaim_volumes("data.", [](unsigned long long) { return false; });
		CHECK((removed == std::vector<unsigned long long>{1, 5}));
	}
	// A reclaim holding a volume turns a writable open away instead of letting it be unlinked.
	{
		{ DStore s(dir, nullptr); s.open("data.2", WR); }
		int f = ::open((dir + "data.2").c_str(), O_RDONLY);
		CHECK(f != -1);
		CHECK(::storage::posix::lock_exclusive(f) == 0);
		DStore w(dir, nullptr);
		CHECK_THROWS(w.open("data.2", WR), StorageIOError);
		::close(f);
		w.open("data.2", WR);   // free again
	}
	std::filesystem::remove_all(dir);
}


// ---- reclaim touches only exact volume names (L6-N3) ---------------------------------
static void test_volume_names() {
	unsigned long long n = 99;
	CHECK(DStore::volume_number("data", "data.7", n) && n == 7);
	CHECK(DStore::volume_number("data.", "data.0", n) && n == 0);
	CHECK(DStore::volume_number("data.", "data.1234567890", n) && n == 1234567890ull);
	for (const char* bad : {"database.4", "data_backup.9", "data.bak.7", "data.+8", "data. 6", "data.007", "data.-1",
	                        "data.", "data", "data.7x", "data.99999999999999999999", "data.0x7", "xdata.7", "data.7.bak", "data7"}) {
		if (DStore::volume_number("data", bad, n)) { std::printf("FAIL: %s matched pattern data\n", bad); ++failures; }
	}
	CHECK(!DStore::volume_number("data.", "data.007", n));
	CHECK(!DStore::volume_number("data.", "data..7", n));

	auto dir = make_dir("names");
	for (const char* f : {"data.3", "database.4", "data_backup.9", "data.bak.7", "data.+8", "data.007", "data.-1"}) {
		std::ofstream(dir + f);   // empty files: removable if the name matched
	}
	DStore h(dir, nullptr);
	CHECK((h.reclaim_volumes("data") == std::vector<unsigned long long>{3}));
	for (const char* f : {"database.4", "data_backup.9", "data.bak.7", "data.+8", "data.007", "data.-1"}) {
		CHECK(std::filesystem::exists(dir + f));
	}
	CHECK(!std::filesystem::exists(dir + "data.3"));

	// DT_UNKNOWN (a filesystem that does not say) asks the file itself.
	int dfd = ::open(dir.c_str(), O_RDONLY);
	std::filesystem::create_directory(dir + "dir.1");
	std::filesystem::create_symlink("data.-1", dir + "link.1");
	CHECK(DStore::is_regular_entry(dfd, "data.007", DT_UNKNOWN));
	CHECK(!DStore::is_regular_entry(dfd, "dir.1", DT_UNKNOWN));
	CHECK(!DStore::is_regular_entry(dfd, "link.1", DT_UNKNOWN));
	CHECK(!DStore::is_regular_entry(dfd, "data.007", DT_DIR));
	CHECK(DStore::is_regular_entry(dfd, "whatever", DT_REG));
	::close(dfd);
	std::filesystem::remove_all(dir);
}


// ---- the directory is synced once, after the data and before the header (L6-N1) -------
struct EventIO : ::storage::DefaultIO {
	static inline std::vector<std::string> events;
	static void note(int fd) noexcept {
		struct stat sb;
		events.push_back(::fstat(fd, &sb) == 0 && S_ISDIR(sb.st_mode) ? "dir" : "file");
	}
	static int fsync(int fd) noexcept { note(fd); return ::storage::DefaultIO::fsync(fd); }
	static int full_fsync(int fd) noexcept { note(fd); return ::storage::DefaultIO::full_fsync(fd); }
};
using EStore = Store<EventIO>;

static void test_directory_sync() {
	auto dir = make_dir("dirsync");
	{
		EStore w(dir, nullptr);
		w.open("v", WR);
		EventIO::events.clear();
		EStore::StreamWriter sw(w, 100);
		sw.append(payload(100, 1));
		sw.commit();
		CHECK((EventIO::events == std::vector<std::string>{"file", "dir", "file"}));   // data, directory, header
		EventIO::events.clear();
		w.write("more");
		w.commit();
		CHECK((EventIO::events == std::vector<std::string>{"file", "file"}));          // the directory once only
	}
	{
		EStore w(dir, nullptr);
		w.open("v", WR);   // an existing volume: nothing new to sync
		EventIO::events.clear();
		w.write("again");
		w.commit();
		CHECK((EventIO::events == std::vector<std::string>{"file", "file"}));
	}
	{
		EStore w(dir, nullptr);   // an ordinary write() on a new volume pays it too
		w.open("w", WR);
		EventIO::events.clear();
		w.write("first");
		w.commit();
		CHECK((EventIO::events == std::vector<std::string>{"file", "dir", "file"}));
	}
	std::filesystem::remove_all(dir);
}


// ---- exact preallocation (L6-N4) ----------------------------------------------------
struct FullDiskIO : ::storage::DefaultIO {
	static inline int err = ENOSPC;
	static int fallocate(int, int, off_t, off_t) noexcept { errno = err; return -1; }
};
using FStore = Store<FullDiskIO>;

static void test_preallocation() {
	auto dir = make_dir("prealloc");
	auto extent = [](uint64_t size) { return (4096 + sizeof(SBinHeader) + size + sizeof(SBinFooter) + 4095) / 4096 * 4096; };
	// The file ends exactly where the record does: no 30% growth after the commit.
	for (uint64_t size : {0ull, 100ull, 4085ull, 100000ull, 3000000ull}) {
		DStore w(dir, nullptr);
		w.open("e", WR);
		uint32_t off = stream_in(w, size, 70000, false, 5);
		CHECK(std::filesystem::file_size(dir + "e") == extent(size));
		CHECK(reads_back(w, off, size, 5));
		std::filesystem::remove(dir + "e");
	}
	// An abandoned stream gives the reservation back.
	{
		DStore w(dir, nullptr);
		w.open("a", WR);
		CHECK(std::filesystem::file_size(dir + "a") == 4096);
		{
			DStore::StreamWriter sw(w, 1000000);
			sw.append(payload(300000, 1));
			CHECK(std::filesystem::file_size(dir + "a") >= extent(1000000));
		}
		CHECK(std::filesystem::file_size(dir + "a") == 4096);
		uint32_t o = w.write("after");
		w.commit();
		w.seek(o); CHECK(w.read() == "after");
	}
	// A later write() on a streamed volume still grows it as before.
	{
		DStore w(dir, nullptr);
		w.open("g", WR);
		stream_in(w, 50000, 4096, false, 1);
		auto sz = std::filesystem::file_size(dir + "g");
		for (int i = 0; i < 6; ++i) { w.write(payload(3000, i)); w.commit(); }
		CHECK(std::filesystem::file_size(dir + "g") > sz);
	}
	// A full disk refuses the upload at begin, with the file as it was and the storage usable.
	{
		FStore w(dir, nullptr);
		w.open("f", WR);
		CHECK_THROWS(FStore::StreamWriter(w, 100000), StorageIOError);
		CHECK(std::filesystem::file_size(dir + "f") == 4096);
		uint32_t o = w.write("still works");
		w.commit();
		w.seek(o); CHECK(w.read() == "still works");
		// A filesystem without fallocate only loses the reservation.
		FullDiskIO::err = EOPNOTSUPP;
		uint32_t so;
		{
			FStore::StreamWriter sw(w, 6000);
			sw.append(payload(6000, 4));
			so = sw.commit();
		}
		FullDiskIO::err = ENOSPC;
		CHECK(reads_back(w, so, 6000, 4));
	}
	// fallocate reserves len bytes, not offset + len (macOS counted from the allocated end).
	{
		constexpr off_t size = 8 << 20;
		int fd = ::open((dir + "fa").c_str(), O_RDWR | O_CREAT, 0644);
		std::vector<char> block(1 << 20, 'x');
		for (off_t at = 0; at < size; at += block.size()) { CHECK(::pwrite(fd, block.data(), block.size(), at) == (ssize_t)block.size()); }
		CHECK(::storage::DefaultIO::fallocate(fd, 0, size, size / 4) == 0);
		struct stat sb;
		::fstat(fd, &sb);
		CHECK(sb.st_size == size + size / 4);
		CHECK(static_cast<off_t>(sb.st_blocks) * 512 <= size + size / 4 + (1 << 20));
		::close(fd);
	}
	std::filesystem::remove_all(dir);
}

#ifdef STREAM_FULL_TEST
// A volume of 2 MiB (-DSTORAGE_LAST_BLOCK_OFFSET): a record that cannot end below
// the last block is refused at begin, before anything is written.
static void test_volume_full() {
	auto dir = make_dir("full");
	DStore st(dir, nullptr);
	st.open("f", WR);
	CHECK_THROWS(DStore::StreamWriter(st, 2u << 20), StorageEOF);
	CHECK_THROWS(DStore::StreamWriter(st, (2u << 20) - 4096), StorageEOF);
	CHECK(std::filesystem::file_size(dir + "f") == 4096);   // only the header block
	uint32_t a = stream_in(st, 1u << 20, 65536, false, 1);
	CHECK_THROWS(DStore::StreamWriter(st, (1u << 20) + 4096), StorageEOF);   // what is left is smaller
	uint32_t b = stream_in(st, 900000, 65536, false, 2);
	CHECK(reads_back(st, a, 1u << 20, 1));
	CHECK(reads_back(st, b, 900000, 2));
	CHECK_THROWS(DStore::StreamWriter(st, 200000), StorageEOF);
	uint32_t c = stream_in(st, 100, 10, false, 3);
	CHECK(reads_back(st, c, 100, 3));
	std::filesystem::remove_all(dir);
}
#endif

int main(int argc, char** argv) {
#ifdef STREAM_FULL_TEST
	test_volume_full();
	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("ok\n");
	return 0;
#endif
	if (argc > 1 && std::string(argv[1]) == "big") {
		sparse::test_big();
	} else {
		test_roundtrips();
		test_compat();
		test_limits_and_misuse();
		test_damaged_flags();
		test_abandon();
		test_crash_sweep();
		test_reclaim();
		test_reclaim_vs_live_writer();
		test_volume_names();
		test_directory_sync();
		test_preallocation();
	}
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("ok\n");
	return 0;
}
