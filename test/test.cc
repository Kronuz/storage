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
// Like CHECK_THROWS, but also asserts the exception message contains `substr`.
// Used to distinguish *which* corruption path fired (e.g. the size-bounds guard
// vs. a downstream short read) without depending on allocation magnitude.
#define CHECK_THROWS_MSG(stmt, exc, substr) do { \
		bool _ok = false; \
		try { stmt; } \
		catch (const exc& _e) { _ok = std::string(_e.what()).find(substr) != std::string::npos; } \
		catch (...) {} \
		if (!_ok) { std::printf("FAIL %s:%d: expected %s containing \"%s\" from: %s\n", __FILE__, __LINE__, #exc, substr, #stmt); ++failures; } \
	} while (0)


// Truncate a volume file to `len` bytes (simulates a torn write / power loss
// that left the tail unwritten). Returns true on success.
static bool truncate_file(const std::string& path, off_t len) {
	return ::truncate(path.c_str(), len) == 0;
}

// Flip one byte at `pos` in a file (simulates bit rot / a scribble).
static void flip_byte(const std::string& path, long pos) {
	FILE* f = std::fopen(path.c_str(), "r+b");
	if (!f) { return; }
	std::fseek(f, pos, SEEK_SET);
	int ch = std::fgetc(f);
	std::fseek(f, pos, SEEK_SET);
	std::fputc(ch ^ 0xFF, f);
	std::fclose(f);
}


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

// Backward compatibility: a legacy v1 header (a magic + the required offset, like
// Xapiand's DataHeaderV1) and a v2 header that shares the same record format. A v2
// consumer can be given the v1 type as its legacy-header template param so it READS
// existing v1 volumes (read-only) while writing v2. This mirrors exactly how the
// Xapiand docstore migrates.
struct LegacyV1Header {
	struct Head { uint32_t magic; uint32_t offset; } head;
	char padding[STORAGE_BLOCK_SIZE - sizeof(Head)];
	void init(void*, void*) { head.magic = STORAGE_V1_MAGIC; head.offset = STORAGE_START_BLOCK_OFFSET; }
	void validate(void*, void*) {
		if (head.magic != STORAGE_V1_MAGIC) { THROW(StorageCorruptVolume, "bad v1 magic"); }
	}
};
struct V2Header {
	StorageMetaHead meta;
	char padding[STORAGE_BLOCK_SIZE - sizeof(StorageMetaHead)];
	void init(void*, void*) { }
	void validate(void*, void*) { }
};
using V1Store        = Storage<LegacyV1Header, StorageBinHeader, CkBinFooter>;                                   // writes v1
using V2ReadsV1Store = Storage<V2Header, StorageBinHeader, CkBinFooter, STORAGE_DEFAULT_IO, LegacyV1Header>;      // v2, reads v1


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

	// ---- codec-in-header: each codec round-trips, and the codec id is stored ----
	// Read the flags byte of the first bin (block 1, file offset STORAGE_BLOCK_SIZE;
	// the reference StorageBinHeader is { uint8_t flags; uint32_t size }).
	auto first_bin_flags = [](const std::string& path) -> int {
		FILE* f = std::fopen(path.c_str(), "rb");
		if (!f) { return -1; }
		std::fseek(f, STORAGE_BLOCK_SIZE, SEEK_SET);
		int c = std::fgetc(f);
		std::fclose(f);
		return c;
	};
	{
		std::string blob;
		for (int i = 0; i < 400; ++i) { blob += "codec comparison payload, quite compressible indeed. "; }

		struct CodecCase { const char* vol; int open_flag; uint8_t expect_codec; };
		const CodecCase codecs[] = {
			{"cd_lz4.0",     STORAGE_COMPRESS,         STORAGE_CODEC_LZ4},
			{"cd_zstd.0",    STORAGE_COMPRESS_ZSTD,    STORAGE_CODEC_ZSTD},
			{"cd_deflate.0", STORAGE_COMPRESS_DEFLATE, STORAGE_CODEC_DEFLATE},
		};
		for (const auto& c : codecs) {
			RefStorage w(base, nullptr);
			w.open(c.vol, STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | c.open_flag);
			uint32_t off = w.write(blob);
			w.commit();
			w.close();

			// The bin is compressed and tagged with the right codec in its flags.
			int flags = first_bin_flags(base + c.vol);
			CHECK(flags != -1);
			CHECK((flags & STORAGE_FLAG_COMPRESSED) != 0);
			CHECK(((flags & STORAGE_CODEC_MASK) >> STORAGE_CODEC_SHIFT) == c.expect_codec);

			// And it reads back identically (reader dispatches on the stored codec).
			RefStorage r(base, nullptr);
			r.open(c.vol, STORAGE_OPEN);
			r.seek(off);
			CHECK(r.read() == blob);
			r.close();
		}

		// Back-compat: plain STORAGE_COMPRESS must tag codec 0 (LZ4) so a volume
		// written before codec-in-header (flags had only bit 0 set) still reads.
		int lz4_flags = first_bin_flags(base + "cd_lz4.0");
		CHECK(((lz4_flags & STORAGE_CODEC_MASK) >> STORAGE_CODEC_SHIFT) == 0);

		// The on-disk LZ4 payload must be byte-identical to the lib's one-shot
		// compress_lz4 (which is what the in-tree streaming engine produced), so
		// existing volumes are unaffected by the refactor.
		{
			std::string expect = compress_lz4(blob);
			FILE* f = std::fopen((base + "cd_lz4.0").c_str(), "rb");
			CHECK(f != nullptr);
			if (f) {
				// payload starts after the 5-byte bin header at block 1.
				std::fseek(f, STORAGE_BLOCK_SIZE + static_cast<long>(sizeof(StorageBinHeader)), SEEK_SET);
				std::string got(expect.size(), '\0');
				size_t n = std::fread(&got[0], 1, expect.size(), f);
				std::fclose(f);
				CHECK(n == expect.size());
				CHECK(got == expect);
			}
		}
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

	// ==================================================================
	// Fault injection: "what if things go really bad" -- torn writes,
	// truncation, bit rot, a corrupt size field. The contract for an
	// append-only store: damage to the tail (or to one record) must be
	// DETECTED (a Storage* exception, never a crash / OOM / silent garbage),
	// and every record committed BEFORE the damage must remain readable.
	// ==================================================================

	// ---- corrupt size field is rejected by the bounds guard, no giant alloc ----
	// A flipped byte in the (unchecksummed) bin header's size field must not drive
	// an unbounded allocation on the compressed read path. The guard rejects it as
	// StorageCorruptVolume *before* payload.resize(), so a 0xFFFFFFFF size costs
	// nothing instead of attempting ~4 GB.
	{
		std::string data;
		for (int i = 0; i < 500; ++i) { data += "compressible size-guard payload, repeated. "; }

		CkStorage w(base, nullptr);
		w.open("sz.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS);
		uint32_t off = w.write(data);
		w.commit();
		w.close();

		// Reference/Ck bin header is packed { uint8_t flags; uint32_t size }; the
		// first bin sits at block 1, so the size field is at STORAGE_BLOCK_SIZE + 1.
		// Overwrite it with a value far larger than the whole volume, leaving the
		// COMPRESSED flag byte intact so read() still takes the resize() path.
		std::string path = base + "sz.0";
		FILE* f = std::fopen(path.c_str(), "r+b");
		CHECK(f != nullptr);
		if (f) {
			uint32_t huge = 0xFFFFFFFFu;
			std::fseek(f, STORAGE_BLOCK_SIZE + 1, SEEK_SET);
			std::fwrite(&huge, sizeof(huge), 1, f);
			std::fclose(f);
		}

		CkStorage r(base, nullptr);
		r.open("sz.0", STORAGE_OPEN);
		r.seek(off);
		CHECK_THROWS_MSG(r.read(), StorageCorruptVolume, "bounds");
		r.close();
	}

	// ---- torn write (truncation): damaged record is caught, prefix survives ----
	// Three records; truncate the file inside the third. Reading the first two
	// still works (append-only isolates them); reading the third fails cleanly
	// with a short-read StorageCorruptVolume, not a crash or garbage.
	{
		std::string a(1500, 'a'), b(1500, 'b'), c(1500, 'c');
		CkStorage w(base, nullptr);
		w.open("torn.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);  // uncompressed
		uint32_t oa = w.write(a);
		uint32_t ob = w.write(b);
		uint32_t oc = w.write(c);
		w.commit();
		w.close();

		std::string path = base + "torn.0";
		// Truncate deterministically INTO record c's payload -- relative to c's own
		// offset, not the physical file end (growfile() preallocates a zeroed tail,
		// so cutting from the end would only trim zeros and leave c intact). c's
		// bytes start at oc*STORAGE_ALIGNMENT; skip its bin header, then keep ~700
		// of the 1500 payload bytes so the record is genuinely torn.
		off_t c_payload = static_cast<off_t>(oc) * STORAGE_ALIGNMENT + sizeof(StorageBinHeader);
		CHECK(truncate_file(path, c_payload + 700));

		CkStorage r(base, nullptr);
		r.open("torn.0", STORAGE_OPEN);
		r.seek(oa); CHECK(r.read() == a);   // committed before the tear -> intact
		r.seek(ob); CHECK(r.read() == b);   // committed before the tear -> intact
		r.seek(oc); CHECK_THROWS(r.read(), StorageCorruptVolume);  // torn -> caught
		r.close();
	}

	// ---- bit rot in a middle record is isolated to that record ----
	// Corrupting one record's payload trips its footer checksum, but its
	// neighbours (independently addressed and checksummed) still read back.
	{
		std::string a(1500, 'p'), b(1500, 'q'), c(1500, 'r');
		CkStorage w(base, nullptr);
		w.open("rot.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);  // uncompressed
		uint32_t oa = w.write(a);
		uint32_t ob = w.write(b);
		uint32_t oc = w.write(c);
		w.commit();
		w.close();

		// Flip a byte inside record b's payload (b starts at ob*STORAGE_ALIGNMENT,
		// then its bin header, so +64 lands in the payload).
		flip_byte(base + "rot.0", static_cast<long>(ob) * STORAGE_ALIGNMENT + sizeof(StorageBinHeader) + 64);

		CkStorage r(base, nullptr);
		r.open("rot.0", STORAGE_OPEN);
		r.seek(oa); CHECK(r.read() == a);   // untouched neighbour
		r.seek(ob); CHECK_THROWS(r.read(), StorageCorruptVolume);  // rotted record
		r.seek(oc); CHECK(r.read() == c);   // untouched neighbour
		r.close();
	}

	// ---- the committed high-water mark bounds reads (no phantom zero records) ----
	// growfile() preallocates zeroed blocks past head.offset. A sequential read
	// must stop at StorageEOF exactly at the committed boundary and never mistake
	// the zero tail for empty records.
	{
		RefStorage w(base, nullptr);
		w.open("hw.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);
		w.write("only record");
		w.commit();
		w.close();

		RefStorage r(base, nullptr);
		r.open("hw.0", STORAGE_OPEN);
		r.seek(STORAGE_START_BLOCK_OFFSET);
		int records = 0;
		try {
			while (true) { r.read(); ++records; }
		} catch (const StorageEOF&) {}
		CHECK(records == 1);   // exactly what was written; the zero tail is not read
		r.close();
	}

	// ---- backward compat: a v2 consumer reads an existing v1 volume ----
	// Write a volume in the legacy v1 format, then open it with a v2 consumer that
	// lists the v1 header as its legacy type. It must detect v1, read every record,
	// and REFUSE a writable open (v1 volumes are read-only under v2).
	{
		V1Store w(base, nullptr);
		w.open("legacy.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);
		uint32_t oa = w.write("legacy record A");
		uint32_t ob = w.write(std::string(1200, 'L'));
		uint32_t oc = w.write("legacy record C");
		w.commit();
		w.close();

		V2ReadsV1Store r(base, nullptr);
		r.open("legacy.0", STORAGE_OPEN);   // detects v1 via the legacy fallback
		r.seek(oa); CHECK(r.read() == "legacy record A");
		r.seek(ob); CHECK(r.read() == std::string(1200, 'L'));
		r.seek(oc); CHECK(r.read() == "legacy record C");
		r.close();

		// A writable open of a v1 volume by a v2 consumer is refused (roll forward).
		V2ReadsV1Store w2(base, nullptr);
		CHECK_THROWS(w2.open("legacy.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE), StorageLegacyReadOnly);

		// A brand-new volume from the v2 consumer is written in v2 and round-trips.
		V2ReadsV1Store w3(base, nullptr);
		w3.open("v2new.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);
		uint32_t o = w3.write("a v2 record");
		w3.commit();
		w3.close();
		V2ReadsV1Store r3(base, nullptr);
		r3.open("v2new.0", STORAGE_OPEN);
		r3.seek(o); CHECK(r3.read() == "a v2 record");
		r3.close();
	}

	if (failures == 0) {
		std::printf("OK: all storage tests passed\n");
		return 0;
	}
	std::printf("FAILED: %d storage checks\n", failures);
	return 1;
}
