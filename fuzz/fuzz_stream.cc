// libFuzzer target for the streamed record write and the volume bytes it leaves.
//
// Input: [u16 size][u8 chunk][u8 flags][u32 flip][u8 mask] then bytes. The bytes, cycled to `size`,
// are streamed in pieces of at most `chunk` (the input picks the cuts) into an in-memory volume.
//   - Unflipped, the record must read back equal.
//   - With flags&1 one byte of the volume image is XORed with `mask`: reading must then throw a
//     StorageException or return the original bytes (the footer checksum catches payload damage),
//     and never crash, over-allocate or read out of bounds.
//   - With flags&2 a volume file is written and reclaim_volumes, open and read run over it: the
//     rest of the input as it stands, or with flags&8 the built (and possibly flipped) image. Any
//     StorageException is fine, anything else is a bug. flags&4 makes the host keep every volume.
#include "storage.h"

#include "../test/crashio.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#pragma pack(push, 1)
struct FBinHeader {
	uint8_t magic; uint8_t flags; uint32_t size;
	void init(void*, void*, uint32_t size_, uint8_t flags_) { magic = STORAGE_BIN_HEADER_MAGIC; size = size_; flags = flags_; }
	void validate(void*, void*) {
		if (magic != STORAGE_BIN_HEADER_MAGIC) { THROW(StorageCorruptVolume, "bad header magic"); }
		if (flags & STORAGE_FLAG_DELETED) { THROW(StorageNotFound, "deleted"); }
	}
};
struct FBinFooter {
	uint32_t checksum; uint8_t magic;
	void init(void*, void*, uint32_t checksum_) { magic = STORAGE_BIN_FOOTER_MAGIC; checksum = checksum_; }
	void validate(void*, void*, uint32_t checksum_) {
		if (magic != STORAGE_BIN_FOOTER_MAGIC) { THROW(StorageCorruptVolume, "bad footer magic"); }
		if (checksum != checksum_) { THROW(StorageCorruptVolume, "bad checksum"); }
	}
};
#pragma pack(pop)

using MemStore = Storage<StorageHeader, FBinHeader, FBinFooter, CrashIO>;
using DiskStore = Storage<StorageHeader, FBinHeader, FBinFooter>;

static std::string read_all(MemStore& st, uint32_t off) {
	st.seek(off);
	std::string out;
	char buf[1500];
	while (size_t n = st.read(buf, sizeof(buf))) { out.append(buf, n); }
	return out;
}

static void over_disk(const std::string& bytes, bool keep_all) {
	static const std::string dir = [] { std::string d = "./fuzz_stream_dir.XXXXXX"; return std::string(mkdtemp(d.data())) + "/"; }();
	{ std::ofstream f(dir + "data.0", std::ios::binary | std::ios::trunc); f.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
	try {
		DiskStore d(dir, nullptr);
		d.reclaim_volumes("data", [&](unsigned long long) { return keep_all; });
		if (std::filesystem::exists(dir + "data.0")) {
			d.open("data.0", STORAGE_OPEN);
			d.seek(STORAGE_START_BLOCK_OFFSET);
			char buf[1500];
			while (d.read(buf, sizeof(buf))) { }
		}
	} catch (const StorageException&) {
	}
	std::filesystem::remove(dir + "data.0");
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	if (size < 9) { return 0; }
	uint16_t len; uint32_t flip;
	memcpy(&len, data, 2);
	uint8_t chunk = data[2], flags = data[3], mask = data[8];
	memcpy(&flip, data + 4, 4);
	const uint8_t* rest = data + 9;
	size_t rest_n = size - 9;

	if ((flags & 2) && !(flags & 8)) {  // the tail is a volume file as it stands
		over_disk(std::string(reinterpret_cast<const char*>(rest), rest_n), flags & 4);
		return 0;
	}

	std::string payload(len, '\0');
	for (size_t i = 0; rest_n && i < len; ++i) { payload[i] = static_cast<char>(rest[i % rest_n]); }

	crashio::state().reset();
	std::string key;
	{
		MemStore w("fz/", nullptr);
		w.open("v", STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE);
		MemStore::StreamWriter sw(w, payload.size());
		size_t pos = 0, i = 0;
		while (pos < payload.size()) {
			size_t pick = rest_n ? rest[i++ % rest_n] : 0;
			size_t n = std::min<size_t>(payload.size() - pos, 1 + (chunk + 1) * (pick + 1) % 6000);
			sw.append(payload.data() + pos, n);
			pos += n;
		}
		sw.commit();
		key = crashio::state().files.begin()->first;
	}
	auto& img = crashio::state().files[key];
	if (flags & 1) {
		size_t at = flip % img.size();
		if (at == STORAGE_BLOCK_SIZE + 1) { ++at; }  // the codec flags: damage there reaches the codecs (docs/status/phase7c-l6.md)
		img[at] ^= static_cast<char>(mask ? mask : 1);
	}
	if (flags & 2) {  // the built image, damaged or not, as a volume on disk
		over_disk(std::string(img.begin(), img.end()), flags & 4);
		return 0;
	}
	MemStore r("fz/", nullptr);
	try {
		r.open("v", STORAGE_OPEN);
		auto got = read_all(r, STORAGE_START_BLOCK_OFFSET);
		if (got != payload) { std::abort(); }  // a read that succeeds returns the original
	} catch (const StorageException&) {
		if ((flags & 1) == 0) { std::abort(); }  // an undamaged volume must read
	}
	return 0;
}
