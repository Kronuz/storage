/*
 * Tests for the storage library: round-trip (compressed and not), multi-write
 * offsets, multi-volume discovery, reopen, write_file, the delete flag, EOF, and
 * footer-checksum corruption detection (with a custom footer that actually keeps
 * a checksum, since the default reference footer stores none).
 */

#include "storage.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>      // for mkdtemp

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
#define CHECK_THROWS(stmt, exc) do { \
		bool _threw = false; \
		try { stmt; } catch (const exc&) { _threw = true; } catch (...) {} \
		if (!_threw) { std::printf("FAIL %s:%d: expected %s from: %s\n", __FILE__, __LINE__, #exc, #stmt); ++failures; } \
	} while (0)


// The default reference structs (the engine's StorageHeader/StorageBinHeader/
// StorageBinFooter) carry no magic and no checksum. That is the faithful
// reference format, but it cannot detect corruption, so this suite also defines
// a checksummed footer to exercise the validate() path.
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

using RefStorage = Storage<StorageHeader, StorageBinHeader, StorageBinFooter>;
using CkStorage  = Storage<StorageHeader, StorageBinHeader, CkBinFooter>;


static std::string make_tmpdir() {
	char tmpl[] = "/tmp/storage_test.XXXXXX";
	char* d = mkdtemp(tmpl);
	if (d == nullptr) { std::perror("mkdtemp"); std::exit(2); }
	return std::string(d) + "/";
}


int main() {
	std::string base = make_tmpdir();

	// ---- uncompressed round-trip ----
	{
		RefStorage w(base, nullptr);
		w.open("ref.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);
		std::string data = "hello storage";
		uint32_t off = w.write(data);
		w.commit();
		w.close();

		RefStorage r(base, nullptr);
		r.open("ref.0", STORAGE_OPEN);
		r.seek(off);
		CHECK(r.read() == data);
		r.close();
	}

	// ---- compressed round-trip (data > STORAGE_MIN_COMPRESS_SIZE) ----
	{
		RefStorage w(base, nullptr);
		w.open("ref.1", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS);
		std::string data;
		for (int i = 0; i < 500; ++i) { data += "the quick brown fox jumps over the lazy dog. "; }
		uint32_t off = w.write(data);
		w.commit();
		w.close();

		RefStorage r(base, nullptr);
		r.open("ref.1", STORAGE_OPEN);
		r.seek(off);
		CHECK(r.read() == data);
		r.close();
	}

	// ---- multiple sequential writes, each addressable by its offset ----
	{
		RefStorage w(base, nullptr);
		w.open("ref.2", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS);
		std::string a = "first record";
		std::string b(2000, 'x');     // compressible, large
		std::string c = "third record, short";
		uint32_t oa = w.write(a);
		uint32_t ob = w.write(b);
		uint32_t oc = w.write(c);
		w.commit();
		w.close();

		RefStorage r(base, nullptr);
		r.open("ref.2", STORAGE_OPEN);
		r.seek(oa); CHECK(r.read() == a);
		r.seek(ob); CHECK(r.read() == b);
		r.seek(oc); CHECK(r.read() == c);
		r.close();
	}

	// ---- checksummed footer: clean read passes, flipped byte is caught ----
	{
		std::string data(4096, '\0');
		for (size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<char>(i * 7 + 1); }

		CkStorage w(base, nullptr);
		w.open("ck.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);  // uncompressed -> footer over raw data
		uint32_t off = w.write(data);
		w.commit();
		w.close();

		CkStorage r(base, nullptr);
		r.open("ck.0", STORAGE_OPEN);
		r.seek(off);
		CHECK(r.read() == data);   // checksum matches
		r.close();

		// Corrupt one byte inside the bin payload (the first bin starts at block 1,
		// file offset STORAGE_BLOCK_SIZE; block 0 is the StorageHeader, so a byte
		// there would not change the data the footer checksums) and confirm
		// validate() trips.
		std::string path = base + "ck.0";
		FILE* f = std::fopen(path.c_str(), "r+b");
		CHECK(f != nullptr);
		if (f) {
			long pos = STORAGE_BLOCK_SIZE + 64;   // well inside the bin payload
			std::fseek(f, pos, SEEK_SET);
			int ch = std::fgetc(f);
			std::fseek(f, pos, SEEK_SET);
			std::fputc(ch ^ 0xFF, f);
			std::fclose(f);
		}

		CkStorage rc(base, nullptr);
		rc.open("ck.0", STORAGE_OPEN);
		rc.seek(off);
		CHECK_THROWS(rc.read(), StorageCorruptVolume);
		rc.close();
	}

	// ---- multi-volume discovery ----
	{
		std::string vbase = make_tmpdir();
		for (unsigned v : {3u, 7u, 1u, 42u}) {
			RefStorage w(vbase, nullptr);
			w.open("data." + std::to_string(v), STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);
			w.write("vol " + std::to_string(v));
			w.commit();
			w.close();
		}
		RefStorage s(vbase, nullptr);
		auto range = s.get_volumes_range("data.");
		CHECK(range.first == 1);
		CHECK(range.second == 42);
		// A pattern that matches nothing yields the empty sentinel (first > second).
		auto none = s.get_volumes_range("nope.");
		CHECK(none.first == std::numeric_limits<unsigned long long>::max());
		CHECK(none.second == 0);
	}

	// ---- write_file: store a file's contents, read them back ----
	{
		std::string src = base + "payload.bin";
		std::string contents;
		for (int i = 0; i < 1000; ++i) { contents += "payload line with some repetition\n"; }
		FILE* f = std::fopen(src.c_str(), "wb");
		CHECK(f != nullptr);
		if (f) { std::fwrite(contents.data(), 1, contents.size(), f); std::fclose(f); }

		RefStorage w(base, nullptr);
		w.open("wf.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS);
		uint32_t off = w.write_file(src);
		w.commit();
		w.close();

		RefStorage r(base, nullptr);
		r.open("wf.0", STORAGE_OPEN);
		r.seek(off);
		CHECK(r.read() == contents);
		r.close();
	}

	// ---- the delete flag marks a bin as not-found ----
	{
		// A bin header that pre-sets the DELETED flag: validate() must throw.
		struct DelHeader : StorageBinHeader {
			void init(void* p, void* a, uint32_t size_, uint8_t) {
				StorageBinHeader::init(p, a, size_, STORAGE_FLAG_DELETED);
			}
		};
		using DelStorage = Storage<StorageHeader, DelHeader, StorageBinFooter>;
		DelStorage w(base, nullptr);
		w.open("del.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);
		uint32_t off = w.write("tombstoned");
		w.commit();
		w.close();

		DelStorage r(base, nullptr);
		r.open("del.0", STORAGE_OPEN);
		r.seek(off);
		CHECK_THROWS(r.read(), StorageNotFound);
		r.close();
	}

	// ---- concurrency: independent instances on independent volumes ----
	// The engine carries no shared/static state (the in-tree static fsyncher is
	// gone, replaced by the per-instance async-fsync hook), so N threads each
	// driving their own Storage on their own volume must run race-free. This is
	// the scenario the TSAN build exercises.
	{
		std::string cbase = make_tmpdir();
		constexpr int N = 8;
		std::atomic<int> ok{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < N; ++t) {
			threads.emplace_back([&, t]() {
				std::string vol = "c." + std::to_string(t);
				std::string payload(3000, static_cast<char>('A' + t));
				std::vector<uint32_t> offs;
				{
					RefStorage w(cbase, nullptr);
					w.open(vol, STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS);
					for (int i = 0; i < 100; ++i) { offs.push_back(w.write(payload)); }
					w.commit();
					w.close();
				}
				RefStorage r(cbase, nullptr);
				r.open(vol, STORAGE_OPEN);
				bool good = true;
				for (uint32_t off : offs) { r.seek(off); if (r.read() != payload) { good = false; break; } }
				r.close();
				if (good) { ok.fetch_add(1, std::memory_order_relaxed); }
			});
		}
		for (auto& th : threads) { th.join(); }
		CHECK(ok.load() == N);
	}

	if (failures == 0) {
		std::printf("OK: all storage tests passed\n");
		return 0;
	}
	std::printf("FAILED: %d storage checks\n", failures);
	return 1;
}
