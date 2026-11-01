// storage::normalize_path (the vendored path normalizer behind Storage's base path): an empty path stays
// empty, so a Storage with no base path opens its volumes relative to the working directory instead of
// at the root of the filesystem; every other result equals the frozen loop it replaced.

#undef NDEBUG  // the checks must run in every build type

#include "storage.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

// The loop before the empty path stopped getting a closing '/', frozen: the oracle for every non-empty
// input, never edited. (For keep_slash it formed end - 1, so it is only called on non-empty input there.)
static std::size_t frozen_normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	int levels = 0;
	char* ret = dst;
	char ch = '\0';
	const char* last = keep_slash ? end - 1 : end;
	while (src <= last) {
		ch = src == end ? '/' : *src;
		++src;
		if (ch == '.' && (levels != 0 || dst == ret || *(dst - 1) == '/' )) {
			*dst++ = ch;
			++levels;
		} else if (ch == '/') {
			while (levels != 0 && dst > ret + 1) {
				if (*--dst == '/') {
					--levels;
				}
			}
			if (dst == ret || *(dst - 1) != '/') {
				*dst++ = ch;
			}
		} else {
			*dst++ = ch;
			levels = 0;
		}
	}
	if (ch == '.' && levels == 1) {
		ch = *--dst;
	}
	if (dst > ret + 1 && !keep_slash) {
		if (slashed) {
			if (ch != '/') {
				*dst++ = '/';
			}
		} else {
			if (ch == '/') {
				--dst;
			}
		}
	}
	return dst - ret;
}

static std::string frozen_normalize(std::string_view src, bool slashed, bool keep_slash) {
	std::string dst;
	dst.resize(src.size() + 1);
	dst.resize(frozen_normalize_path(src.data(), src.data() + src.size(), &dst[0], slashed, keep_slash));
	return dst;
}

// A Storage whose base path can be read without opening anything.
using RefStorage = Storage<StorageHeader, StorageBinHeader, StorageBinFooter>;
struct Probe : RefStorage {
	using RefStorage::RefStorage;
	const std::string& base() const { return base_path; }
};

int main() {
	// Every string of "a", "." and "/" up to length 7 and 20,000 random ones up to 24, in all four flag
	// combinations, equals the frozen loop; a relative path never becomes absolute.
	const char alphabet[] = {'a', '.', '/'};
	std::size_t compared = 0;
	auto compare = [&](const std::string& s) {
		for (int flags = 0; flags < 4; ++flags) {
			bool slashed = flags & 1, keep_slash = flags & 2;
			std::string now = storage::normalize_path(s, slashed, keep_slash);
			if (now != frozen_normalize(s, slashed, keep_slash)) {
				std::printf("FAIL normalize_path(\"%s\", %d, %d) = \"%s\", frozen \"%s\"\n", s.c_str(), slashed, keep_slash,
					now.c_str(), frozen_normalize(s, slashed, keep_slash).c_str());
				++failures;
			}
			if (s[0] != '/' && now.substr(0, 1) == "/") {
				std::printf("FAIL normalize_path(\"%s\", %d, %d) made a relative path absolute\n", s.c_str(), slashed, keep_slash);
				++failures;
			}
			++compared;
		}
	};
	for (int length = 1; length <= 7; ++length) {
		std::size_t total = 1;
		for (int i = 0; i < length; ++i) { total *= 3; }
		for (std::size_t n = 0; n < total; ++n) {
			std::string s;
			for (std::size_t m = n, i = 0; i < static_cast<std::size_t>(length); ++i, m /= 3) { s.push_back(alphabet[m % 3]); }
			compare(s);
		}
	}
	std::uint64_t x = 0x9e3779b97f4a7c15ULL;
	for (int round = 0; round < 20000; ++round) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		std::string s;
		for (std::uint64_t m = x, i = 0; i < 1 + x % 24; ++i, m = m * 6364136223846793005ULL + 1442695040888963407ULL) { s.push_back(alphabet[(m >> 33) % 3]); }
		compare(s);
	}
	CHECK(compared == 4 * (3279 + 20000));  // 3 + 9 + ... + 3^7 strings, and the random ones, in four combinations each

	// The empty path normalizes to nothing in all four combinations; the frozen loop returned the root
	// ("/") for the two without keep_slash, and that class alone differs. Oracle: std::filesystem
	// lexically_normal maps "" to "".
	const std::string oracle = std::filesystem::path("").lexically_normal().string();
	CHECK(oracle.empty());
	for (int flags = 0; flags < 4; ++flags) {
		bool slashed = flags & 1, keep_slash = flags & 2;
		CHECK(storage::normalize_path("", slashed, keep_slash) == oracle);
		CHECK(storage::normalize_path(std::string_view(), slashed, keep_slash) == oracle);
		if (!keep_slash) {
			CHECK(frozen_normalize("", slashed, keep_slash) == "/");
			CHECK(frozen_normalize("", slashed, keep_slash) != storage::normalize_path("", slashed, keep_slash));
		}
	}

	// The base path a Storage keeps: an empty one is no prefix, not the root.
	CHECK(Probe("", nullptr).base().empty());
	CHECK(Probe(std::string_view(), nullptr).base().empty());
	CHECK(Probe("vols", nullptr).base() == "vols/");
	CHECK(Probe("vols/", nullptr).base() == "vols/");
	CHECK(Probe("a//b/../c", nullptr).base() == "a/c/");
	CHECK(Probe("/var/data", nullptr).base() == "/var/data/");
	CHECK(Probe("/", nullptr).base() == "/");
	CHECK(Probe("./", nullptr).base() == "./");

	if (failures == 0) { std::puts("all storage path tests passed"); }
	return failures == 0 ? 0 : 1;
}
