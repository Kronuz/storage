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

// Blob with `ratio`-ish redundancy: a repeated phrase compresses well, random
// bytes do not. Use the phrase form so the compressed numbers are meaningful.
static std::string make_blob(size_t size) {
	static const char* phrase = "the quick brown fox jumps over the lazy dog. ";
	std::string s;
	s.reserve(size);
	while (s.size() < size) { s += phrase; }
	s.resize(size);
	return s;
}

struct Result { double write_mbs; double read_mbs; double ratio; };

static Result run(const std::string& base, const char* vol, size_t blob_size, size_t count, bool compress) {
	int flags = STORAGE_CREATE_OR_OPEN | STORAGE_WRITABLE | (compress ? STORAGE_COMPRESS : 0);
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

	struct Case { const char* name; const char* vol; size_t size; size_t count; bool compress; };
	const Case cases[] = {
		{"small blobs, raw",     "s_raw.0",  256,          50000, false},
		{"small blobs, lz4",     "s_lz4.0",  256,          50000, true},
		{"large blobs, raw",     "l_raw.0",  1 << 20,         200, false},
		{"large blobs, lz4",     "l_lz4.0",  1 << 20,         200, true},
	};

	for (const auto& c : cases) {
		Result r = run(base, c.vol, c.size, c.count, c.compress);
		std::printf("%-28s %12.1f %12.1f %10.2f\n", c.name, r.write_mbs, r.read_mbs, r.ratio);
	}
	return 0;
}
