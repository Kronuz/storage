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
#include <vector>

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

// The class of paths whose result changed: a relative path where a ".." (or a longer run of dots, which pops
// one component fewer than it has dots) pops every component the output holds. The frozen loop never popped
// the first component, so "a/.." was "a" and "a/../b" was "a/b"; the output is now empty at that point
// ("." if nothing follows). Components are split at '/'; each one followed by '/' is resolved, and so is the
// last one unless keep_slash leaves it as it is. A leading run of dots stays as the "." the output starts
// with and is never popped. (The same model is in the fs test.)
static bool is_dots(std::string_view c) { return !c.empty() && c.find_first_not_of('.') == std::string_view::npos; }

static std::vector<std::string_view> split_components(std::string_view s) {
	std::vector<std::string_view> out;
	std::size_t at = 0;
	for (;;) {
		std::size_t slash = s.find('/', at);
		if (slash == std::string_view::npos) { out.push_back(s.substr(at)); return out; }
		out.push_back(s.substr(at, slash - at));
		at = slash + 1;
	}
}

static bool pops_first_component(std::string_view s, bool keep_slash) {
	if (s.empty() || s[0] == '/') { return false; }
	const auto comps = split_components(s);
	const std::size_t resolved = comps.size() - (keep_slash ? 1 : 0);
	std::size_t depth = 0;
	bool base = false, out_empty = true, hit = false;
	for (std::size_t i = 0; i < resolved; ++i) {
		const std::string_view c = comps[i];
		if (c.empty()) { continue; }
		if (!is_dots(c)) { ++depth; out_empty = false; continue; }
		if (out_empty) { base = true; out_empty = false; continue; }
		const std::size_t pops = c.size() - 1;
		if (pops == 0 || depth == 0) { continue; }
		if (pops >= depth && !base) { hit = true; depth = 0; out_empty = true; }
		else { depth = pops >= depth ? 0 : depth - pops; }
	}
	return hit;
}

// The oracle for the changed class where std::filesystem::path::lexically_normal and the library agree:
// components that are names or exactly "..", a result with no leading "..", and with keep_slash a last
// component that is not an unterminated "..". lexically_normal gives "." for what is left of "a/..", and
// the library's own trailing-slash policy is applied to its answer.
static bool oracle_applies(std::string_view s, bool keep_slash) {
	const auto comps = split_components(s);
	for (auto c : comps) {
		if (is_dots(c) && c != "..") { return false; }
	}
	if (keep_slash && is_dots(comps.back())) { return false; }
	const std::string ln = std::filesystem::path(std::string(s)).lexically_normal().generic_string();
	return ln != ".." && ln.compare(0, 3, "../") != 0;
}

static std::string oracle_normalize(std::string_view s, bool slashed, bool keep_slash) {
	std::string ln = std::filesystem::path(std::string(s)).lexically_normal().generic_string();
	while (ln.size() > 1 && ln.back() == '/') { ln.pop_back(); }
	return (keep_slash ? s.back() == '/' : slashed) ? ln + "/" : ln;
}

int main() {
	// Every string of "a", "." and "/" up to length 7 and 20,000 random ones up to 24, in all four flag
	// combinations, equals the frozen loop outside the changed class and the oracle inside it; a relative
	// path never becomes absolute.
	const char alphabet[] = {'a', '.', '/'};
	std::size_t compared = 0, changed = 0, applicable = 0, old_wrong = 0;
	int reported = 0;
	auto compare = [&](const std::string& s) {
		for (int flags = 0; flags < 4; ++flags) {
			bool slashed = flags & 1, keep_slash = flags & 2;
			std::string now = storage::normalize_path(s, slashed, keep_slash);
			std::string old = frozen_normalize(s, slashed, keep_slash);
			auto fail = [&](const char* what, const std::string& want) {
				if (reported++ < 20) {
					std::printf("FAIL normalize_path(\"%s\", %d, %d) = \"%s\": %s \"%s\"\n", s.c_str(), slashed, keep_slash, now.c_str(), what, want.c_str());
				}
				++failures;
			};
			if (!pops_first_component(s, keep_slash)) {
				if (now != old) { fail("frozen gives", old); }
			} else {
				++changed;
				if (oracle_applies(s, keep_slash)) {
					std::string want = oracle_normalize(s, slashed, keep_slash);
					++applicable;
					if (now != want) { fail("oracle gives", want); }
					if (old != want) { ++old_wrong; }
				}
			}
			if (s[0] != '/' && now.substr(0, 1) == "/") { fail("made a relative path absolute, from", s); }
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
	std::printf("normalize_path: %zu compared, %zu in the changed class, %zu with an oracle, the frozen loop wrong on %zu\n", compared, changed, applicable, old_wrong);
	// The changed class is not empty, the oracle reaches into it, and the frozen loop was wrong on some of what it reaches.
	CHECK(changed > 0);
	CHECK(applicable > 0);
	CHECK(old_wrong > 0);

	// The first component of a relative path is popped by a ".." like any other.
	CHECK(storage::normalize_path("a/..") == ".");
	CHECK(storage::normalize_path("a/..", true) == "./");
	CHECK(storage::normalize_path("a/../b") == "b");
	CHECK(storage::normalize_path("a/b/../..") == ".");
	CHECK(storage::normalize_path("a/b/..") == "a");
	CHECK(storage::normalize_path("/a/..") == "/");
	CHECK(storage::normalize_path("a/../b/", true) == "b/");

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
