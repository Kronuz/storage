// On-disk identity. test/fixtures/ holds one volume per codec (none, lz4, zstd, deflate) written by
// the reference engine as it stood before the 2026-10 changes. This test checks that
//   - every record of every volume reads back, through the whole-record read() and through 2 KB
//     chunks, and equals the record regenerated from its seed;
//   - the uncompressed volume is reproduced byte for byte: header, bin headers, footers, padding and
//     block layout (apart from the zero tail of preallocated blocks). A compressed volume's payload
//     bytes belong to the codec library, so those volumes are read, not compared.
//
//   storage_format_golden             check
//   storage_format_golden --write DIR write the four volumes into DIR (regenerates the fixtures)
//
// Regenerate the fixtures only for a deliberate, versioned format change.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include "storage.h"

#ifndef STORAGE_GOLDEN_DIR
#error "STORAGE_GOLDEN_DIR must name the fixtures directory"
#endif

using RefStorage = Storage<StorageHeader, StorageBinHeader, StorageBinFooter>;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

// Random, text and run-length data (kinds 0, 1, 2), regenerated from a seed.
static std::string make(size_t n, int kind, uint64_t seed) {
	std::string s;
	s.reserve(n);
	uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
	static const char* w[] = {"the ", "storage ", "record ", "volume ", "lz4 ", "zstd ", "block\n", "0123 "};
	while (s.size() < n) {
		x = x * 6364136223846793005ull + 1442695040888963407ull;
		switch (kind) {
			case 0: s.push_back(static_cast<char>(x >> 56)); break;
			case 1: s += w[(x >> 40) % 8]; break;
			default: s.append(1 + (x >> 58), static_cast<char>('a' + (x >> 33) % 3)); break;
		}
	}
	s.resize(n);
	return s;
}

struct Record { size_t size; int kind; };

// Sizes around the lz4 block (2 KB) and the page (4 KB), an empty record, and one past 64 KB.
// The uncompressed volume keeps only the records up to 4 KB, to stay small.
static std::vector<Record> records(bool small_only) {
	std::vector<Record> all{{0, 1}, {1, 0}, {99, 1}, {100, 2}, {2047, 0}, {2048, 1}, {2049, 2}, {4096, 0}, {65537, 2}};
	std::vector<Record> out;
	for (const auto& r : all) {
		if (!small_only || r.size <= 4096) {
			out.push_back(r);
		}
	}
	return out;
}

struct Volume { const char* name; int flag; bool small_only; };
static const Volume volumes[] = {
	{"none.0", 0, true},
	{"lz4.0", STORAGE_COMPRESS_LZ4, false},
	{"zstd.0", STORAGE_COMPRESS_ZSTD, false},
	{"deflate.0", STORAGE_COMPRESS_DEFLATE, false},
};

static std::string slurp(const std::string& path) {
	std::string out;
	if (FILE* f = std::fopen(path.c_str(), "rb")) {
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
			out.append(buf, n);
		}
		std::fclose(f);
	}
	return out;
}

static std::string trim_zero_tail(std::string s) {
	while (!s.empty() && s.back() == '\0') {
		s.pop_back();
	}
	return s;
}

static void write_volume(const std::string& dir, const Volume& v) {
	RefStorage w(dir, nullptr);
	w.open(v.name, STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | STORAGE_NO_SYNC | v.flag);
	uint64_t seed = 1;
	for (const auto& r : records(v.small_only)) {
		w.write(make(r.size, r.kind, seed++));
	}
	w.commit();
	w.close();
}

static void read_volume(const std::string& dir, const Volume& v) {
	RefStorage r(dir, nullptr);
	r.open(v.name, STORAGE_OPEN);

	r.seek(STORAGE_START_BLOCK_OFFSET);   // whole records, in write order
	uint64_t seed = 1;
	for (const auto& rec : records(v.small_only)) {
		const std::string want = make(rec.size, rec.kind, seed++);
		const std::string got = r.read();
		if (got != want) {
			std::printf("FAIL %s: record of %zu bytes (kind %d) reads back as %zu bytes that differ\n", v.name, rec.size, rec.kind, got.size());
			++failures;
		}
	}

	r.seek(STORAGE_START_BLOCK_OFFSET);   // the same records, 2 KB at a time
	seed = 1;
	for (const auto& rec : records(v.small_only)) {
		const std::string want = make(rec.size, rec.kind, seed++);
		std::string got;
		char buf[2048];
		while (size_t n = r.read(buf, sizeof(buf))) {
			got.append(buf, n);
		}
		if (got != want) {
			std::printf("FAIL %s: chunked read of the %zu-byte record (kind %d) differs\n", v.name, rec.size, rec.kind);
			++failures;
		}
	}
	r.close();
}

static std::string make_tmpdir() {
	char tmpl[] = "storage_golden.XXXXXX";
	char* d = ::mkdtemp(tmpl);
	if (d == nullptr) {
		std::perror("mkdtemp");
		std::exit(2);
	}
	return std::string(d) + "/";
}

int main(int argc, char** argv) {
	if (argc == 3 && std::string(argv[1]) == "--write") {
		const std::string dir = std::string(argv[2]) + "/";
		for (const auto& v : volumes) {
			write_volume(dir, v);
			std::printf("wrote %s%s\n", dir.c_str(), v.name);
		}
		return 0;
	}

	const std::string fixtures = std::string(STORAGE_GOLDEN_DIR) + "/";
	const std::string scratch = make_tmpdir();
	for (const auto& v : volumes) {
		CHECK(!slurp(fixtures + v.name).empty());
		read_volume(fixtures, v);
	}

	// The writer reproduces the uncompressed volume byte for byte. The file's zero tail (blocks
	// preallocated ahead of the records, which a filesystem may or may not grant) is not compared.
	write_volume(scratch, volumes[0]);
	const std::string expected = trim_zero_tail(slurp(fixtures + volumes[0].name));
	const std::string written = trim_zero_tail(slurp(scratch + volumes[0].name));
	CHECK(!expected.empty());
	CHECK(written.size() == expected.size());
	CHECK(written == expected);
	if (written != expected) {
		for (size_t i = 0; i < std::min(written.size(), expected.size()); ++i) {
			if (written[i] != expected[i]) {
				std::printf("first difference at byte %zu: wrote 0x%02x, the fixture has 0x%02x\n", i,
					static_cast<unsigned>(static_cast<unsigned char>(written[i])), static_cast<unsigned>(static_cast<unsigned char>(expected[i])));
				break;
			}
		}
	}

	// A compressed volume written now reads back too (the payload bytes are the codec library's).
	for (size_t i = 1; i < sizeof(volumes) / sizeof(volumes[0]); ++i) {
		write_volume(scratch, volumes[i]);
		read_volume(scratch, volumes[i]);
	}

	for (const auto& v : volumes) {
		::unlink((scratch + v.name).c_str());
	}
	::rmdir(scratch.substr(0, scratch.size() - 1).c_str());
	if (failures == 0) {
		std::printf("OK: all storage format checks passed\n");
		return 0;
	}
	std::printf("FAILED: %d storage format checks\n", failures);
	return 1;
}
