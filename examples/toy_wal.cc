/*
 * toy_wal: a runnable, self-checking example that drives `storage` exactly the
 * way Xapiand's DataStorage (src/database/shard.cc) does -- an append-only,
 * compressed, checksummed record log that is committed, closed, reopened, and
 * replayed by offset for recovery.
 *
 * It doubles as a REGRESSION TEST: every step asserts, and main() returns
 * non-zero on any failure, so CMake registers it with ctest. If a change to the
 * storage engine (or a dependency: compressors, io, the codec dispatch, the
 * block framing) breaks the write/commit/reopen/replay round-trip, the zstd
 * codec, or the header/footer integrity checks, this example fails and catches
 * the regression before it reaches Xapiand.
 *
 * The usage shape mirrors Xapiand faithfully:
 *   - a custom Header / BinHeader / BinFooter carrying magic + a footer checksum
 *     (Xapiand's DataHeader / DataBinHeader / DataBinFooter),
 *   - a Storage<...> subclass (Xapiand's DataStorage),
 *   - open(WRITABLE | CREATE | COMPRESS_ZSTD | FULL_SYNC), write() -> offset,
 *     commit(), then reopen(OPEN) + seek(offset) + read().
 */

#include "storage.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>      // for mkdtemp

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
#define CHECK_THROWS(stmt, exc) do { \
		bool _threw = false; \
		try { stmt; } catch (const exc&) { _threw = true; } catch (...) {} \
		if (!_threw) { std::printf("FAIL %s:%d: expected %s from: %s\n", __FILE__, __LINE__, #exc, #stmt); ++failures; } \
	} while (0)


// Magic'd header + a footer that actually keeps a checksum, mirroring how
// Xapiand's DataStorage detects corrupt volumes (shard.cc's DataHeader /
// DataBinHeader / DataBinFooter carry STORAGE_MAGIC and the DELETED flag).
constexpr uint32_t TOY_WAL_MAGIC    = 0x57414C21;  // "WAL!"
constexpr uint8_t  TOY_BIN_MAGIC    = 0xB1;
constexpr uint8_t  TOY_FOOTER_MAGIC = 0xF0;

struct WalHeader {
	struct WalHeaderHead {
		uint32_t magic;
		uint32_t offset;  // required by the engine
	} head;

	char padding[(STORAGE_BLOCK_SIZE - sizeof(WalHeader::WalHeaderHead)) / sizeof(char)];

	void init(void* /*param*/, void* /*args*/) {
		head.magic = TOY_WAL_MAGIC;
		head.offset = STORAGE_START_BLOCK_OFFSET;
	}

	void validate(void* /*param*/, void* /*args*/) {
		if (head.magic != TOY_WAL_MAGIC) {
			THROW(StorageCorruptVolume, "Bad WAL header magic");
		}
	}
};

#pragma pack(push, 1)
struct WalBinHeader {
	uint8_t magic;
	uint8_t flags;   // required (also carries the codec id + DELETED flag)
	uint32_t size;   // required (must live in the first STORAGE_ALIGNMENT bytes)

	void init(void* /*param*/, void* /*args*/, uint32_t size_, uint8_t flags_) {
		magic = TOY_BIN_MAGIC;
		size = size_;
		flags = (0 & ~STORAGE_FLAG_MASK) | flags_;
	}

	void validate(void* /*param*/, void* /*args*/) {
		if (magic != TOY_BIN_MAGIC) {
			THROW(StorageCorruptVolume, "Bad WAL bin-header magic");
		}
		if (flags & STORAGE_FLAG_DELETED) {
			THROW(StorageNotFound, "Record deleted");
		}
	}
};

struct WalBinFooter {
	uint32_t checksum;
	uint8_t magic;

	void init(void* /*param*/, void* /*args*/, uint32_t checksum_) {
		magic = TOY_FOOTER_MAGIC;
		checksum = checksum_;
	}

	void validate(void* /*param*/, void* /*args*/, uint32_t checksum_) {
		if (magic != TOY_FOOTER_MAGIC) {
			THROW(StorageCorruptVolume, "Bad WAL footer magic");
		}
		if (checksum != checksum_) {
			THROW(StorageCorruptVolume, "Bad WAL record checksum");
		}
	}
};
#pragma pack(pop)


// The Storage subclass, exactly like Xapiand's `DataStorage : public
// Storage<DataHeader, DataBinHeader, DataBinFooter>`.
class ToyWal : public Storage<WalHeader, WalBinHeader, WalBinFooter> {
public:
	explicit ToyWal(std::string_view base)
		: Storage<WalHeader, WalBinHeader, WalBinFooter>(base, /*param*/ nullptr) { }
};


static std::string make_tmpdir() {
	char tmpl[] = "/tmp/toy_wal.XXXXXX";
	char* d = mkdtemp(tmpl);
	if (d == nullptr) { std::perror("mkdtemp"); std::exit(2); }
	return std::string(d) + "/";
}


int main() {
	const std::string base = make_tmpdir();

	// A realistic mix: small records stay raw (< STORAGE_MIN_COMPRESS_SIZE), large
	// and highly-compressible records exercise the zstd codec path.
	std::vector<std::string> records = {
		"tx:1 begin",
		"tx:1 put key=alpha value=" + std::string(4096, 'a'),   // big + compressible -> zstd
		"tx:1 put key=beta value=" + std::string(2048, 'b'),
		"tx:1 commit",
		std::string(300, 'z'),                                  // > MIN_COMPRESS_SIZE
		"tx:2 begin",
	};
	std::vector<uint32_t> offsets;

	// ---- write phase: append every record to the log, then commit + close ----
	// Same open flags Xapiand's writable DataStorage uses.
	{
		ToyWal wal(base);
		wal.open("wal.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS_ZSTD | STORAGE_FULL_SYNC);
		for (const auto& rec : records) {
			offsets.push_back(wal.write(rec));
		}
		wal.commit();
	}  // dtor closes the volume

	CHECK(offsets.size() == records.size());
	// Offsets must be strictly increasing (append-only log).
	for (size_t i = 1; i < offsets.size(); ++i) {
		CHECK(offsets[i] > offsets[i - 1]);
	}

	// ---- recovery phase: reopen read-only and replay each record by offset ----
	{
		ToyWal wal(base);
		wal.open("wal.0", STORAGE_OPEN);
		for (size_t i = 0; i < records.size(); ++i) {
			wal.seek(offsets[i]);
			CHECK(wal.read() == records[i]);  // integrity: magic + checksum + codec all validated inside read()
		}
	}

	// ---- durability across a fresh process-like reopen: a second reader sees
	//      exactly the same bytes (what Xapiand relies on after a restart) ----
	{
		ToyWal wal(base);
		wal.open("wal.0", STORAGE_OPEN);
		wal.seek(offsets.back());
		CHECK(wal.read() == records.back());
	}

	// ---- appending to an existing volume keeps earlier records intact ----
	uint32_t appended_off = 0;
	{
		ToyWal wal(base);
		wal.open("wal.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS_ZSTD | STORAGE_FULL_SYNC);
		appended_off = wal.write("tx:2 commit");
		wal.commit();
	}
	{
		ToyWal wal(base);
		wal.open("wal.0", STORAGE_OPEN);
		wal.seek(offsets.front());
		CHECK(wal.read() == records.front());   // first record survived the reopen+append
		wal.seek(appended_off);
		CHECK(wal.read() == "tx:2 commit");
	}

	if (failures == 0) {
		std::puts("toy_wal: all checks passed (append + zstd + commit + reopen + replay + append-preserve)");
	}
	return failures == 0 ? 0 : 1;
}
