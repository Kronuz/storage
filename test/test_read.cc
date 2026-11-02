/*
 * The read path: Storage::read() (a whole record) against the chunked read(buf, size), the oracle it
 * must agree with, and against the data written.
 *
 *   - every codec and sizes around the 2 KB chunk, the compression threshold and the MiB, text and
 *     random bytes, whole against chunked at several chunk sizes, then a record read part way by
 *     chunks and finished whole, then the records in sequence up to EOF;
 *   - damaged volumes (a flipped payload or footer byte, a flipped size, a truncated file): whole and
 *     chunked reads end the same way, the same exception with the same message;
 *   - allocation: a whole read sizes its result once from the record. It allocates about the
 *     record's size (plus its compressed payload), where a result that doubles as it fills
 *     allocates several times that.
 */

#include "storage.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <typeinfo>
#include <vector>
#include <unistd.h>      // for mkdtemp, truncate

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
#define CHECK_THROWS(stmt, exc) do { \
		bool _threw = false; \
		try { stmt; } catch (const exc&) { _threw = true; } catch (...) {} \
		if (!_threw) { std::printf("FAIL %s:%d: expected %s from: %s\n", __FILE__, __LINE__, #exc, #stmt); ++failures; } \
	} while (0)


// ---- Allocation accounting: the bytes of the allocations of at least 4 KB while it is on ----
// ThreadSanitizer's runtime defines operator new itself, and a second definition does not link:
// the accounting, and the test that reads it, are left out there.

#if defined(__SANITIZE_THREAD__)
#define STORAGE_TEST_COUNT_ALLOC 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define STORAGE_TEST_COUNT_ALLOC 0
#endif
#endif
#ifndef STORAGE_TEST_COUNT_ALLOC
#define STORAGE_TEST_COUNT_ALLOC 1
#endif

#if STORAGE_TEST_COUNT_ALLOC
static std::atomic<bool> g_count_on{false};
static std::atomic<size_t> g_big_bytes{0};
static std::atomic<size_t> g_big_allocs{0};

static void* counted_alloc(size_t n) {
	if (g_count_on.load(std::memory_order_relaxed) && n >= 4096) {
		g_big_bytes.fetch_add(n, std::memory_order_relaxed);
		g_big_allocs.fetch_add(1, std::memory_order_relaxed);
	}
	void* p = std::malloc(n ? n : 1);
	if (!p) { throw std::bad_alloc(); }
	return p;
}

void* operator new(size_t n) { return counted_alloc(n); }
void* operator new[](size_t n) { return counted_alloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
#endif  // STORAGE_TEST_COUNT_ALLOC


#pragma pack(push, 1)
struct CkBinFooter {
	uint32_t checksum;
	void init(void*, void*, uint32_t checksum_) { checksum = checksum_; }
	void validate(void*, void*, uint32_t checksum_) {
		if (checksum != checksum_) {
			THROW(StorageCorruptVolume, "Bad bin checksum");
		}
	}
};
#pragma pack(pop)

using CkStorage = Storage<StorageHeader, StorageBinHeader, CkBinFooter>;   // v2, checksummed footer


struct Codec { const char* name; int flag; };
static const Codec codecs[] = {
	{"none", 0},
	{"lz4", STORAGE_COMPRESS_LZ4},
	{"zstd", STORAGE_COMPRESS_ZSTD},
	{"deflate", STORAGE_COMPRESS_DEFLATE},
};

static std::string make_tmpdir() {
	char tmpl[] = "/tmp/storage_read_test.XXXXXX";
	char* d = mkdtemp(tmpl);
	if (d == nullptr) { std::perror("mkdtemp"); std::exit(2); }
	return std::string(d) + "/";
}

// Text-like data (compresses a few times over) or random bytes (does not compress).
static std::string make_data(size_t size, bool text, uint64_t seed) {
	static const char* words[] = {"the", "storage", "engine", "writes", "a", "record", "and", "returns", "its", "offset", "compressed", "block", "volume", "header"};
	std::mt19937_64 rng(seed);
	std::string s;
	s.reserve(size + 16);
	while (s.size() < size) {
		if (text) {
			s += words[rng() % (sizeof(words) / sizeof(words[0]))];
			s += ' ';
		} else {
			uint64_t v = rng();
			s.append(reinterpret_cast<const char*>(&v), sizeof(v));
		}
	}
	s.resize(size);
	return s;
}

// How a read ended: its bytes' length and hash, or the exception's type and message.
template <class F>
static std::string outcome(F&& f) {
	try {
		std::string s = f();
		return "ok " + std::to_string(s.size()) + " " + std::to_string(std::hash<std::string>{}(s));
	} catch (const std::exception& e) {
		// The message ends with the throw site (" (in function at file:line)"), which differs between
		// the two read paths and is not part of the outcome.
		std::string what = e.what();
		what = what.substr(0, what.find(" (in "));
		return std::string("throws ") + typeid(e).name() + ": " + what;
	}
}

static std::string chunked(CkStorage& r, size_t bs) {
	std::string acc;
	std::vector<char> buf(bs);
	while (size_t n = r.read(buf.data(), buf.size(), uint32_t(-1), nullptr)) {
		acc.append(buf.data(), n);
	}
	return acc;
}

struct Record { uint32_t off; std::string data; };

static std::vector<Record> write_volume(const std::string& base, const std::string& vol, int flag, const std::vector<std::string>& datas) {
	std::vector<Record> records;
	CkStorage w(base, nullptr);
	w.open(vol, STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | flag);
	for (const auto& d : datas) { records.push_back({w.write(d), d}); }
	w.commit();
	w.close();
	return records;
}

static std::vector<std::string> matrix_datas() {
	std::vector<std::string> datas;
	uint64_t seed = 1;
	for (size_t size : {size_t(0), size_t(1), size_t(99), size_t(100), size_t(101), size_t(2047), size_t(2048), size_t(2049), size_t(4095), size_t(4096),
	                    size_t(4097), size_t(65535), size_t(65536), size_t(65537), size_t(1 << 20) - 1, size_t(1 << 20), size_t(1 << 20) + 1, size_t(3 << 20) + 7}) {
		datas.push_back(make_data(size, true, seed++));
		datas.push_back(make_data(size, false, seed++));
	}
	return datas;
}


// Whole against chunked against the data, every codec and size.
static void test_matrix(const std::string& base) {
	const auto datas = matrix_datas();
	for (const auto& c : codecs) {
		const std::string vol = std::string("matrix_") + c.name + ".0";
		const auto records = write_volume(base, vol, c.flag, datas);

		CkStorage r(base, nullptr);
		r.open(vol, STORAGE_OPEN);
		for (const auto& rec : records) {
			r.seek(rec.off);
			const std::string whole = r.read();
			if (whole != rec.data) { std::printf("FAIL %s: whole read of %zu bytes differs from the data\n", c.name, rec.data.size()); ++failures; }
			for (size_t bs : {size_t(1) << 11, size_t(4097), size_t(100000)}) {
				r.seek(rec.off);
				if (chunked(r, bs) != rec.data) { std::printf("FAIL %s: chunked(%zu) read of %zu bytes differs\n", c.name, bs, rec.data.size()); ++failures; }
			}
			if (rec.data.size() > 10) {  // a few bytes by chunk, the rest whole
				r.seek(rec.off);
				char head[10];
				CHECK(r.read(head, sizeof(head), uint32_t(-1), nullptr) == sizeof(head));
				const std::string rest = r.read();
				if (std::string(head, sizeof(head)) + rest != rec.data) { std::printf("FAIL %s: partial then whole read of %zu bytes differs\n", c.name, rec.data.size()); ++failures; }
				// and the record after it reads clean (the state was reset)
				r.seek(rec.off);
				CHECK(r.read() == rec.data);
			}
		}

		// In sequence, no seek between records, then EOF.
		r.seek(records.front().off);
		for (const auto& rec : records) {
			if (r.read() != rec.data) { std::printf("FAIL %s: sequential read of %zu bytes differs\n", c.name, rec.data.size()); ++failures; }
		}
		CHECK_THROWS(r.read(), StorageEOF);
		r.close();
	}
}


static void flip_byte(const std::string& path, long pos) {
	FILE* f = std::fopen(path.c_str(), "r+b");
	if (!f) { return; }
	std::fseek(f, pos, SEEK_SET);
	int ch = std::fgetc(f);
	std::fseek(f, pos, SEEK_SET);
	std::fputc(ch ^ 0x55, f);
	std::fclose(f);
}

static std::string slurp(const std::string& path) {
	std::string s;
	if (FILE* f = std::fopen(path.c_str(), "rb")) {
		char buf[65536];
		while (size_t n = std::fread(buf, 1, sizeof(buf), f)) { s.append(buf, n); }
		std::fclose(f);
	}
	return s;
}

static void spit(const std::string& path, const std::string& s) {
	if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(s.data(), 1, s.size(), f); std::fclose(f); }
}


// A damaged volume: the whole read and the chunked read of the same record end the same way.
static void test_damage(const std::string& base) {
	const std::vector<std::string> datas = {make_data(3000, true, 11), make_data(70000, true, 12), make_data(70000, false, 13), make_data(5000, false, 14)};
	for (const auto& c : codecs) {
		const std::string vol = std::string("damage_") + c.name + ".0";
		const auto records = write_volume(base, vol, c.flag, datas);
		const std::string pristine = slurp(base + vol);
		const std::string tmp = base + "damaged.0";

		struct Damage { const char* what; std::string bytes; };
		for (const auto& rec : records) {
			const size_t start = size_t(rec.off) * STORAGE_ALIGNMENT;  // the bin header
			std::vector<Damage> damages;
			auto flipped = [&](size_t pos) { std::string b = pristine; b[pos] ^= 0x55; return b; };
			damages.push_back({"payload first byte", flipped(start + sizeof(StorageBinHeader))});
			damages.push_back({"payload middle byte", flipped(start + sizeof(StorageBinHeader) + 40)});
			damages.push_back({"size low byte", flipped(start + 1)});
			damages.push_back({"size high byte", flipped(start + 4)});
			damages.push_back({"flags", flipped(start)});
			{  // the footer: the four bytes after the payload
				std::string probe = pristine;
				uint32_t size;
				std::memcpy(&size, probe.data() + start + 1, sizeof(size));
				damages.push_back({"footer byte", flipped(start + sizeof(StorageBinHeader) + size)});
				damages.push_back({"truncated in the footer", pristine.substr(0, start + sizeof(StorageBinHeader) + size + 2)});
				damages.push_back({"truncated in the payload", pristine.substr(0, start + sizeof(StorageBinHeader) + size / 2)});
				damages.push_back({"truncated in the bin header", pristine.substr(0, start + 3)});
			}
			for (const auto& d : damages) {
				spit(tmp, d.bytes);
				std::string a, b;
				{
					CkStorage r(base, nullptr);
					try { r.open("damaged.0", STORAGE_OPEN); } catch (const std::exception&) { a = b = "open failed"; }
					if (a.empty()) {
						try { r.seek(rec.off); } catch (const std::exception&) {}
						a = outcome([&] { return r.read(); });
					}
				}
				{
					CkStorage r(base, nullptr);
					try { r.open("damaged.0", STORAGE_OPEN); } catch (const std::exception&) { b = "open failed"; }
					if (b.empty()) {
						try { r.seek(rec.off); } catch (const std::exception&) {}
						b = outcome([&] { return chunked(r, 2048); });
					}
				}
				if (a != b) {
					std::printf("FAIL %s, record at %u, %s:\n   whole:   %s\n   chunked: %s\n", c.name, rec.off, d.what, a.c_str(), b.c_str());
					++failures;
				}
			}
		}
		std::remove((base + "damaged.0").c_str());
	}
}


#if STORAGE_TEST_COUNT_ALLOC
// The bytes (in allocations of 4 KB or more) that f() allocates.
template <class F>
static size_t allocated_by(F&& f) {
	g_big_bytes = 0;
	g_big_allocs = 0;
	g_count_on = true;
	f();
	g_count_on = false;
	return g_big_bytes;
}

// A whole read of a 1 MiB record allocates its result once, sized from the record: no more than the
// codec's own decode of the payload allocates (the result and what the codec needs besides) plus the
// payload read from the file. A result that doubles as it fills allocates about four times the record,
// and a chunk copy of the decoded record besides.
static void test_allocation(const std::string& base) {
	const size_t size = 1 << 20;
	const std::string data = make_data(size, true, 99);
	const uint8_t ids[] = {0, STORAGE_CODEC_LZ4, STORAGE_CODEC_ZSTD, STORAGE_CODEC_DEFLATE};
	for (size_t k = 0; k < sizeof(codecs) / sizeof(codecs[0]); ++k) {
		const auto& c = codecs[k];
		const std::string vol = std::string("alloc_") + c.name + ".0";
		const auto records = write_volume(base, vol, c.flag, {data});
		CkStorage r(base, nullptr);
		r.open(vol, STORAGE_OPEN);
		r.seek(records[0].off);
		CHECK(r.read() == data);  // warms the codecs' reused contexts

		// What the codec itself allocates to decode this payload, and the payload's size.
		size_t decode = 0, payload = 0;
		if (c.flag) {
			const std::string packed = storage_compress(ids[k], data);
			payload = packed.size();
			std::string out = storage_decompress(ids[k], packed);  // warms
			decode = allocated_by([&] { out = storage_decompress(ids[k], packed); });
			CHECK(out == data);
		}

		size_t bytes = 0;
		for (int i = 0; i < 3; ++i) {
			r.seek(records[0].off);
			std::string got;
			bytes = allocated_by([&] { got = r.read(); });
			CHECK(got == data);
		}
		const size_t bound = (c.flag ? decode + payload : size) + 16384;
		std::printf("%-8s a whole read of %zu bytes allocated %zu bytes (the codec's decode %zu, the payload %zu)\n", c.name, size, bytes, decode, payload);
		if (bytes > bound) { std::printf("FAIL %s: a whole read allocated %zu bytes, more than %zu\n", c.name, bytes, bound); ++failures; }
		r.close();
	}
}
#else
static void test_allocation(const std::string&) {
	std::printf("allocation test skipped: the thread sanitizer defines operator new\n");
}
#endif


int main() {
	const std::string base = make_tmpdir();
	test_matrix(base);
	test_damage(base);
	test_allocation(base);
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("all storage read tests passed\n");
	return 0;
}
