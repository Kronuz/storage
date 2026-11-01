// storage::normalize_path (the vendored path normalizer behind Storage's base path): the lexical normal
// form of os.path.normpath, with the current directory "." however it is spelled, the empty path included.
// A Storage with no base path keeps "./", so its volumes open relative to the working directory instead of
// at the root of the filesystem.

#undef NDEBUG  // the checks must run in every build type

#include "storage.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

// The loop before the component-based normalizer, frozen: the oracle for every non-empty input that has no
// '.' (the new loop must give the same bytes there), never edited. It formed end - 1 for keep_slash, so it
// is only called on non-empty input.
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

// The reference: Python's os.path.normpath written out as a stack of components (the table and the digest
// below are os.path.normpath's own output), with the library's two differences: any run of leading '/' is
// one root ("//x" is "/x"; POSIX lets normpath keep two), and the trailing '/' is the flags' policy.
// (The same model is in the fs test.)
static std::string model_normalize(std::string_view s, bool slashed, bool keep_slash) {
	const bool absolute = !s.empty() && s[0] == '/';
	std::vector<std::string> stack;
	std::size_t at = 0;
	while (at <= s.size()) {
		const std::size_t slash = s.find('/', at);
		const std::string_view name = s.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
		at = slash == std::string_view::npos ? s.size() + 1 : slash + 1;
		if (name.empty() || name == ".") { continue; }
		if (name == "..") {
			if (!stack.empty() && stack.back() != "..") { stack.pop_back(); continue; }
			if (absolute) { continue; }
		}
		stack.emplace_back(name);
	}
	std::string core = absolute ? "/" : "";
	for (std::size_t i = 0; i < stack.size(); ++i) {
		if (i != 0) { core += '/'; }
		core += stack[i];
	}
	if (core.empty()) { core = "."; }
	if ((keep_slash ? !s.empty() && s.back() == '/' : slashed) && core.back() != '/') { core += '/'; }
	return core;
}

static std::uint64_t fnv1a(std::uint64_t h, std::string_view bytes) {
	for (unsigned char c : bytes) { h ^= c; h *= 0x100000001b3ULL; }
	return h;
}

struct Counts {
	std::size_t compared = 0, dotted = 0, changed = 0;
	std::uint64_t digest = 0xcbf29ce484222325ULL;
	int reported = 0;
};

// One input in the four flag combinations: equal to the reference; equal to the frozen loop when the input
// has no '.'; a second normalization changes nothing (one canonical form). The digest takes the results of
// the first three combinations in order (slashed with keep_slash is keep_slash).
static void compare(const std::string& s, Counts& n) {
	const bool dotted = s.find('.') != std::string::npos;
	for (int flags = 0; flags < 4; ++flags) {
		const bool slashed = flags & 1, keep_slash = flags & 2;
		const std::string now = storage::normalize_path(s, slashed, keep_slash);
		const std::string want = model_normalize(s, slashed, keep_slash);
		auto fail = [&](const char* what, const std::string& expected) {
			if (n.reported++ < 20) {
				std::printf("FAIL normalize_path(\"%s\", %d, %d) = \"%s\": %s \"%s\"\n", s.c_str(), slashed, keep_slash, now.c_str(), what, expected.c_str());
			}
			++failures;
		};
		if (now != want) { fail("reference gives", want); }
		if (!s.empty() && !dotted) {
			const std::string old = frozen_normalize(s, slashed, keep_slash);
			if (now != old) { fail("frozen gives", old); }
		}
		const std::string again = storage::normalize_path(now, slashed, keep_slash);
		if (again != now) { fail("a second normalization gives", again); }
		if (flags < 3) { n.digest = fnv1a(n.digest, now + "\n"); }
		++n.compared;
	}
	if (dotted) { ++n.dotted; }
}

// os.path.normpath's results for named inputs. Columns: input, default, slashed, keep_slash.
static void test_table() {
	struct Row { const char* in; const char* plain; const char* slashed; const char* kept; };
	static const Row rows[] = {
		{"", ".", "./", "."},
		{".", ".", "./", "."},
		{"./", ".", "./", "./"},
		{"./.", ".", "./", "."},
		{"x/..", ".", "./", "."},
		{"x/../", ".", "./", "./"},
		{"./x/..", ".", "./", "."},
		{"x/y/..", "x", "x/", "x"},
		{"x/y/../..", ".", "./", "."},
		{"x/../..", "..", "../", ".."},
		{"../x", "../x", "../x/", "../x"},
		{"../../x", "../../x", "../../x/", "../../x"},
		{"x", "x", "x/", "x"},
		{"x/", "x", "x/", "x/"},
		{"./x", "x", "x/", "x"},
		{"x//y", "x/y", "x/y/", "x/y"},
		{"...", "...", ".../", "..."},
		{".a/..", ".", "./", "."},
		{"..", "..", "../", ".."},
		{"a/b/../../c/d", "c/d", "c/d/", "c/d"},
		{"/", "/", "/", "/"},
		{"//", "/", "/", "/"},
		{"/..", "/", "/", "/"},
		{"/../..", "/", "/", "/"},
		{"/x", "/x", "/x/", "/x"},
		{"/x/", "/x", "/x/", "/x/"},
		{"/x/..", "/", "/", "/"},
		{"/x/../", "/", "/", "/"},
		{"/x/../y", "/y", "/y/", "/y"},
		{"//x", "/x", "/x/", "/x"},
		{"/x//y/./z/../..", "/x", "/x/", "/x"},
		{"/..x", "/..x", "/..x/", "/..x"},
	};
	for (const Row& row : rows) {
		const std::string plain = storage::normalize_path(row.in), slashed = storage::normalize_path(row.in, true), kept = storage::normalize_path(row.in, false, true);
		if (plain != row.plain || slashed != row.slashed || kept != row.kept) {
			std::printf("FAIL normalize_path(\"%s\"): \"%s\" \"%s\" \"%s\", expected \"%s\" \"%s\" \"%s\"\n", row.in, plain.c_str(), slashed.c_str(), kept.c_str(), row.plain, row.slashed, row.kept);
			++failures;
		}
	}
	// One canonical form of the current directory, whatever its spelling, and a usable path for open and stat.
	for (const char* spelling : {"", ".", "./", "././", "x/..", "x/../", "./x/..", "x/y/../..", "a/./b/../.."}) {
		CHECK(storage::normalize_path(spelling) == ".");
		CHECK(storage::normalize_path(std::string_view(spelling), true) == "./");
	}
	// A default-constructed view (a null data pointer) is the empty path: "." as well.
	for (int flags = 0; flags < 4; ++flags) {
		const bool slashed = flags & 1, keep_slash = flags & 2;
		CHECK(storage::normalize_path(std::string_view(), slashed, keep_slash) == storage::normalize_path("", slashed, keep_slash));
	}
}

int main() {
	// Every string of "a", "b", "." and "/" up to length 8 and 20,000 random ones up to 24, in all four flag
	// combinations, equals the reference, and its results over the exhaustive strings hash to the digest of
	// os.path.normpath's (the same 87,380 strings and 262,140 results as in the fs test).
	const char alphabet[] = {'a', 'b', '.', '/'};
	Counts n;
	for (int length = 1; length <= 8; ++length) {
		std::size_t total = 1;
		for (int i = 0; i < length; ++i) { total *= 4; }
		for (std::size_t k = 0; k < total; ++k) {
			std::string s;
			for (std::size_t m = k, i = 0; i < static_cast<std::size_t>(length); ++i, m /= 4) { s.push_back(alphabet[m % 4]); }
			compare(s, n);
		}
	}
	CHECK(n.digest == 0xef8fe9971d37494bULL);
	std::uint64_t x = 0x9e3779b97f4a7c15ULL;
	for (int round = 0; round < 20000; ++round) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		std::string s;
		for (std::uint64_t m = x, i = 0; i < 1 + x % 24; ++i, m = m * 6364136223846793005ULL + 1442695040888963407ULL) { s.push_back(alphabet[(m >> 33) % 4]); }
		compare(s, n);
	}
	std::printf("normalize_path: %zu compared, %zu have a '.'\n", n.compared, n.dotted);
	CHECK(n.compared == 4 * (87380 + 20000));

	test_table();

	// The base path a Storage keeps: an empty one is the working directory, "./", not the root.
	CHECK(Probe("", nullptr).base() == "./");
	CHECK(Probe(std::string_view(), nullptr).base() == "./");
	CHECK(Probe(".", nullptr).base() == "./");
	CHECK(Probe("x/..", nullptr).base() == "./");
	CHECK(Probe("vols", nullptr).base() == "vols/");
	CHECK(Probe("vols/", nullptr).base() == "vols/");
	CHECK(Probe("a//b/../c", nullptr).base() == "a/c/");
	CHECK(Probe("/var/data", nullptr).base() == "/var/data/");
	CHECK(Probe("/", nullptr).base() == "/");
	CHECK(Probe("./", nullptr).base() == "./");
	CHECK(Probe("../vols", nullptr).base() == "../vols/");
	// What the base path is for: a volume name appended to it names a file in that directory.
	CHECK(Probe("", nullptr).base() + "vol.0" == "./vol.0");

	if (failures == 0) { std::puts("all storage path tests passed"); }
	return failures == 0 ? 0 : 1;
}
