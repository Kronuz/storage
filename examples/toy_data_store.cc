/*
 * toy_data_store: a runnable, self-checking example that builds a document store
 * on top of `storage` exactly the way Xapiand's DataStorage (src/database/shard.cc)
 * does -- documents addressed by offset, a magic + checksum on every record, a
 * DELETED tombstone flag, compression, and multiple volumes -- but on the v2
 * CRASH-SAFE format (a checksummed StorageMetaHead header, see storage.h).
 *
 * Opting a volume into v2 is a one-liner: the header type begins with a
 * `StorageMetaHead meta;`. commit() then fsyncs the data, rewrites the single
 * header at block 0 (new high-water + checksum), and fsyncs it -- a data->header
 * barrier. The header is crash-safe by single-sector atomicity: its changing bytes
 * (the meta) fit in block 0's first 512 B sector, so a torn block-0 write on power
 * loss still yields a valid old-or-new header.
 *
 * It doubles as a REGRESSION TEST: every step asserts and main() returns non-zero
 * on any failure, so CMake registers it with ctest. The companion crash-injection
 * matrix in test/test_crash.cc drives this exact header/record shape through
 * simulated power loss.
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


constexpr uint8_t TOY_DS_BIN_MAGIC    = 0xD5;
constexpr uint8_t TOY_DS_FOOTER_MAGIC = 0x5D;

// The v2 header. It MUST begin with `StorageMetaHead meta;` (that is what opts the
// volume into the v2 format and what the engine stamps + checksums). After
// it come the consumer's own fields -- here a uuid, exactly like Xapiand's
// DataHeader carries one to reject a foreign volume -- padded to a full block. The
// whole block (meta + uuid + pad) is covered by the meta checksum.
struct DataStoreHeader {
	StorageMetaHead meta;   // engine-owned: magic, version, txnid, high-water, checksum
	char uuid[36];
	char padding[STORAGE_BLOCK_SIZE - sizeof(StorageMetaHead) - 36];

	void init(void* param, void* /*args*/) {
		const char* u = param != nullptr ? static_cast<const char*>(param) : "toy-data-store-00000000000000000000";
		std::memset(uuid, 0, sizeof(uuid));
		std::strncpy(uuid, u, sizeof(uuid));
		// meta is stamped by the engine (write_meta); nothing to set here.
	}

	// The engine has already checked the meta magic + whole-block checksum before
	// calling this; validate() is the consumer's domain check (a foreign volume).
	void validate(void* param, void* /*args*/) {
		if (param != nullptr && std::strncmp(uuid, static_cast<const char*>(param), sizeof(uuid)) != 0) {
			THROW(StorageCorruptVolume, "Data store UUID mismatch");
		}
	}
};
static_assert(sizeof(DataStoreHeader) == STORAGE_BLOCK_SIZE, "header must be one block");

#pragma pack(push, 1)
struct DsBinHeader {
	uint8_t magic;
	uint8_t flags;   // carries the codec id + the DELETED tombstone flag
	uint32_t size;   // must live in the first STORAGE_ALIGNMENT bytes

	void init(void* /*param*/, void* /*args*/, uint32_t size_, uint8_t flags_) {
		magic = TOY_DS_BIN_MAGIC;
		size = size_;
		flags = (0 & ~STORAGE_FLAG_MASK) | flags_;
	}

	void validate(void* /*param*/, void* /*args*/) {
		if (magic != TOY_DS_BIN_MAGIC) {
			THROW(StorageCorruptVolume, "Bad data-store bin-header magic");
		}
		if (flags & STORAGE_FLAG_DELETED) {
			THROW(StorageNotFound, "Document deleted");
		}
	}
};

struct DsBinFooter {
	uint32_t checksum;
	uint8_t magic;

	void init(void* /*param*/, void* /*args*/, uint32_t checksum_) {
		magic = TOY_DS_FOOTER_MAGIC;
		checksum = checksum_;
	}

	void validate(void* /*param*/, void* /*args*/, uint32_t checksum_) {
		if (magic != TOY_DS_FOOTER_MAGIC) {
			THROW(StorageCorruptVolume, "Bad data-store footer magic");
		}
		if (checksum != checksum_) {
			THROW(StorageCorruptVolume, "Bad data-store record checksum");
		}
	}
};
#pragma pack(pop)


// The store, exactly like Xapiand's `DataStorage : public Storage<DataHeader,
// DataBinHeader, DataBinFooter>`. Because DataStoreHeader begins with a
// StorageMetaHead, this is automatically a v2 (crash-safe) store.
class ToyDataStore : public Storage<DataStoreHeader, DsBinHeader, DsBinFooter> {
public:
	explicit ToyDataStore(std::string_view base, void* param = nullptr)
		: Storage<DataStoreHeader, DsBinHeader, DsBinFooter>(base, param) { }
};


static std::string make_tmpdir() {
	char tmpl[] = "/tmp/toy_data_store.XXXXXX";
	char* d = mkdtemp(tmpl);
	if (d == nullptr) { std::perror("mkdtemp"); std::exit(2); }
	return std::string(d) + "/";
}


int main() {
	const std::string base = make_tmpdir();
	char uuid[] = "toy-data-store-11111111111111111111";

	// A realistic mix: small docs stay raw (< STORAGE_MIN_COMPRESS_SIZE), large and
	// compressible ones exercise the LZ4 codec path.
	std::vector<std::string> docs = {
		"{\"id\":1,\"body\":\"hello\"}",
		"{\"id\":2,\"body\":\"" + std::string(4096, 'a') + "\"}",   // big + compressible
		"{\"id\":3,\"body\":\"" + std::string(2048, 'b') + "\"}",
		"{\"id\":4,\"body\":\"world\"}",
	};
	std::vector<uint32_t> offs;

	// ---- put phase: store each document, keep its offset (the locator) ----
	{
		ToyDataStore ds(base, uuid);
		ds.open("data.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS_LZ4 | STORAGE_FULL_SYNC);
		for (const auto& d : docs) {
			offs.push_back(ds.write(d));
		}
		ds.commit();
	}

	// ---- get phase: reopen and fetch each document by offset ----
	{
		ToyDataStore ds(base, uuid);
		ds.open("data.0", STORAGE_OPEN);
		for (size_t i = 0; i < docs.size(); ++i) {
			ds.seek(offs[i]);
			CHECK(ds.read() == docs[i]);   // magic + checksum + codec validated inside read()
		}
	}

	// ---- a foreign volume (wrong uuid) is rejected on open ----
	{
		ToyDataStore ds(base, const_cast<char*>("some-other-uuid-2222222222222222222"));
		CHECK_THROWS(ds.open("data.0", STORAGE_OPEN), StorageCorruptVolume);
	}

	// ---- the v2 commit rewrites the header: append + a 2nd commit, reopen
	//      still reads the newest snapshot AND every earlier document ----
	uint32_t off5 = 0;
	{
		ToyDataStore ds(base, uuid);
		ds.open("data.0", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS_LZ4 | STORAGE_FULL_SYNC);
		off5 = ds.write("{\"id\":5,\"body\":\"second commit\"}");
		ds.commit();
	}
	{
		ToyDataStore ds(base, uuid);
		ds.open("data.0", STORAGE_OPEN);
		ds.seek(offs.front()); CHECK(ds.read() == docs.front());   // survived the 2nd commit
		ds.seek(off5);         CHECK(ds.read() == "{\"id\":5,\"body\":\"second commit\"}");
	}

	// ---- a second volume, independent of the first (docstore spans many) ----
	{
		ToyDataStore ds(base, uuid);
		ds.open("data.1", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_COMPRESS_LZ4 | STORAGE_FULL_SYNC);
		uint32_t o = ds.write("{\"id\":99,\"vol\":1}");
		ds.commit();
		ds.close();
		ds.open("data.1", STORAGE_OPEN);
		ds.seek(o);
		CHECK(ds.read() == "{\"id\":99,\"vol\":1}");
	}

	if (failures == 0) {
		std::puts("toy_data_store: all checks passed (put/get by offset + compress + v2 crash-safe commit + reopen + multi-volume)");
	}
	return failures == 0 ? 0 : 1;
}
