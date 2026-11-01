/*
 * Benchmark harness for the storage library (not a ctest; run ./build/storage_bench).
 *
 * Reports write and read throughput (MB/s) for small and large blobs, with and
 * without compression, plus the on-disk compression ratio on compressible data.
 * Single instance, single thread: this measures the engine's serial path, not
 * concurrency (the injected async-fsync hook is where a host adds that).
 *
 * Each compressed scenario is followed by a "raw" row: the same records through the
 * codec library and pwrite/pread alone, one file, no headers, footers or checksums,
 * records addressed from an in-memory table. The raw row is the floor the engine's
 * framing is measured against; it uses one LZ4 block or one zstd frame per record.
 */

#include "storage.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>      // for mkdtemp

#if __has_include(<lz4.h>) && __has_include(<zstd.h>)
#  include <lz4.h>
#  include <zstd.h>
#  define STORAGE_BENCH_RAW 1
#endif

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

#ifdef STORAGE_BENCH_RAW
// The floor for one codec (0 none, LZ4, zstd): compress each record, pwrite it at the end of one
// file, fsync once; then pread each record and decompress it. The page cache is warm, as for run().
static Result run_raw(const std::string& base, const char* file, size_t blob_size, size_t count, int codec_flag) {
	std::string blob = make_blob(blob_size);
	std::string path = base + "raw_" + file;
	int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd == -1) { std::perror("open"); std::exit(2); }

	const bool lz4 = codec_flag == STORAGE_COMPRESS_LZ4, zstd = codec_flag == STORAGE_COMPRESS_ZSTD;
	std::string packed(zstd ? ZSTD_compressBound(blob_size) : lz4 ? static_cast<size_t>(LZ4_compressBound(static_cast<int>(blob_size))) : blob_size, '\0');
	std::vector<std::pair<off_t, size_t>> records;
	records.reserve(count);
	double total_mb = (static_cast<double>(blob_size) * count) / (1024.0 * 1024.0);

	auto t0 = clk::now();
	off_t at = 0;
	for (size_t i = 0; i < count; ++i) {
		size_t n;
		if (zstd) { n = ZSTD_compress(packed.data(), packed.size(), blob.data(), blob_size, ZSTD_COMPRESS_LEVEL); }
		else if (lz4) { n = static_cast<size_t>(LZ4_compress_default(blob.data(), packed.data(), static_cast<int>(blob_size), static_cast<int>(packed.size()))); }
		else { n = blob_size; std::memcpy(packed.data(), blob.data(), n); }
		if (::pwrite(fd, packed.data(), n, at) != static_cast<ssize_t>(n)) { std::perror("pwrite"); std::exit(2); }
		records.emplace_back(at, n);
		at += static_cast<off_t>(n);
	}
	::fsync(fd);
	auto t1 = clk::now();

	std::string back(blob_size, '\0');
	auto t2 = clk::now();
	for (const auto& rec : records) {
		if (::pread(fd, packed.data(), rec.second, rec.first) != static_cast<ssize_t>(rec.second)) { std::perror("pread"); std::exit(2); }
		if (zstd) { ZSTD_decompress(back.data(), blob_size, packed.data(), rec.second); }
		else if (lz4) { LZ4_decompress_safe(packed.data(), back.data(), static_cast<int>(rec.second), static_cast<int>(blob_size)); }
		else { std::memcpy(back.data(), packed.data(), blob_size); }
	}
	auto t3 = clk::now();
	if (back != blob) { std::fprintf(stderr, "raw row %s: round trip differs\n", file); std::exit(1); }
	::close(fd);

	return Result{total_mb / secs(t0, t1), total_mb / secs(t2, t3), static_cast<double>(blob_size) * count / static_cast<double>(at)};
}
#endif

int main() {
	std::string base = make_tmpdir();
	std::printf("%-28s %12s %12s %10s\n", "scenario", "write MB/s", "read MB/s", "ratio");
	std::printf("%-28s %12s %12s %10s\n", "--------", "---------", "--------", "-----");

	struct Case { const char* name; const char* vol; size_t size; size_t count; int codec; };
	const Case cases[] = {
		{"small blobs, none",    "s_none.0", 256,          50000, 0},
		{"small blobs, lz4",     "s_lz4.0",  256,          50000, STORAGE_COMPRESS_LZ4},
		{"small blobs, zstd",    "s_zstd.0", 256,          50000, STORAGE_COMPRESS_ZSTD},
		{"small blobs, deflate", "s_defl.0", 256,          50000, STORAGE_COMPRESS_DEFLATE},
		{"large blobs, none",    "l_none.0", 1 << 20,         200, 0},
		{"large blobs, lz4",     "l_lz4.0",  1 << 20,         200, STORAGE_COMPRESS_LZ4},
		{"large blobs, zstd",    "l_zstd.0", 1 << 20,         200, STORAGE_COMPRESS_ZSTD},
		{"large blobs, deflate", "l_defl.0", 1 << 20,         200, STORAGE_COMPRESS_DEFLATE},
	};

	for (const auto& c : cases) {
		Result r = run(base, c.vol, c.size, c.count, c.codec);
		std::printf("%-28s %12.1f %12.1f %10.2f\n", c.name, r.write_mbs, r.read_mbs, r.ratio);
#ifdef STORAGE_BENCH_RAW
		if (c.codec != STORAGE_COMPRESS_DEFLATE) {
			std::string raw_name = std::string("  raw ") + (c.codec == 0 ? "pwrite/pread" : c.codec == STORAGE_COMPRESS_LZ4 ? "lz4" : "zstd");
			Result f = run_raw(base, c.vol, c.size, c.count, c.codec);
			std::printf("%-28s %12.1f %12.1f %10.2f\n", raw_name.c_str(), f.write_mbs, f.read_mbs, f.ratio);
		}
#endif
	}
	return 0;
}
