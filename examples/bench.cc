/*
 * Benchmark harness for the storage library (not a ctest; run ./build/storage_bench).
 *
 * Reports write and read throughput (MB/s) for small and large blobs, with and
 * without compression, plus the on-disk compression ratio on compressible data.
 * Single instance, single thread: this measures the engine's serial path, not
 * concurrency (the injected async-fsync hook is where a host adds that).
 */

#include "storage.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>      // for mkdtemp

using RefStorage = Storage<StorageHeader, StorageBinHeader, StorageBinFooter>;
using clk = std::chrono::steady_clock;

static std::string make_tmpdir() {
	char tmpl[] = "/tmp/storage_bench.XXXXXX";
	char* d = mkdtemp(tmpl);
	if (d == nullptr) { std::perror("mkdtemp"); std::exit(2); }
	return std::string(d) + "/";
}

static double secs(clk::time_point a, clk::time_point b) {
	return std::chrono::duration<double>(b - a).count();
}

// A blob with realistic, English-like redundancy: words drawn pseudo-randomly
// from a small dictionary. This compresses like real text (a few x), not like a
// single repeated phrase (which would give absurd, unrepresentative ratios). The
// LCG is deterministic so runs are comparable.
static std::string make_blob(size_t size) {
	static const char* words[] = {
		"the", "storage", "engine", "writes", "a", "record", "and", "returns",
		"its", "offset", "compressed", "block", "volume", "header", "footer",
		"checksum", "codec", "lz4", "zstd", "deflate", "data", "read", "write",
		"append", "only", "fixed", "size", "aligned", "buffer", "stream",
	};
	constexpr size_t nwords = sizeof(words) / sizeof(words[0]);
	std::string s;
	s.reserve(size + 16);
	uint64_t state = 0x9E3779B97F4A7C15ull;
	while (s.size() < size) {
		state = state * 6364136223846793005ull + 1442695040888963407ull;  // LCG
		s += words[(state >> 33) % nwords];
		s += ' ';
	}
	s.resize(size);
	return s;
}

struct Result { double write_mbs; double read_mbs; double ratio; };

static Result run(const std::string& base, const char* vol, size_t blob_size, size_t count, int codec_flag) {
	int flags = STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | codec_flag;
	std::string blob = make_blob(blob_size);
	std::vector<uint32_t> offsets;
	offsets.reserve(count);

	double total_mb = (static_cast<double>(blob_size) * count) / (1024.0 * 1024.0);

	// ---- write ----
	auto t0 = clk::now();
	{
		RefStorage w(base, nullptr);
		w.open(vol, flags);
		for (size_t i = 0; i < count; ++i) { offsets.push_back(w.write(blob)); }
		w.commit();
		w.close();
	}
	auto t1 = clk::now();

	// on-disk size (the single volume file)
	std::string path = base + vol;
	long on_disk = 0;
	if (FILE* f = std::fopen(path.c_str(), "rb")) { std::fseek(f, 0, SEEK_END); on_disk = std::ftell(f); std::fclose(f); }

	// ---- read ----
	auto t2 = clk::now();
	{
		RefStorage r(base, nullptr);
		r.open(vol, STORAGE_OPEN);
		for (uint32_t off : offsets) { r.seek(off); volatile size_t n = r.read().size(); (void)n; }
		r.close();
	}
	auto t3 = clk::now();

	double logical = static_cast<double>(blob_size) * count;
	return Result{
		total_mb / secs(t0, t1),
		total_mb / secs(t2, t3),
		on_disk > 0 ? logical / static_cast<double>(on_disk) : 0.0,
	};
}

int main() {
	std::string base = make_tmpdir();
	std::printf("%-28s %12s %12s %10s\n", "scenario", "write MB/s", "read MB/s", "ratio");
	std::printf("%-28s %12s %12s %10s\n", "--------", "---------", "--------", "-----");

	struct Case { const char* name; const char* vol; size_t size; size_t count; int codec; };
	const Case cases[] = {
		{"small blobs, none",    "s_none.0", 256,          50000, 0},
		{"small blobs, lz4",     "s_lz4.0",  256,          50000, STORAGE_COMPRESS},
		{"small blobs, zstd",    "s_zstd.0", 256,          50000, STORAGE_COMPRESS_ZSTD},
		{"small blobs, deflate", "s_defl.0", 256,          50000, STORAGE_COMPRESS_DEFLATE},
		{"large blobs, none",    "l_none.0", 1 << 20,         200, 0},
		{"large blobs, lz4",     "l_lz4.0",  1 << 20,         200, STORAGE_COMPRESS},
		{"large blobs, zstd",    "l_zstd.0", 1 << 20,         200, STORAGE_COMPRESS_ZSTD},
		{"large blobs, deflate", "l_defl.0", 1 << 20,         200, STORAGE_COMPRESS_DEFLATE},
	};

	for (const auto& c : cases) {
		Result r = run(base, c.vol, c.size, c.count, c.codec);
		std::printf("%-28s %12.1f %12.1f %10.2f\n", c.name, r.write_mbs, r.read_mbs, r.ratio);
	}
	return 0;
}
