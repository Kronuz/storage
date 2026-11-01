// storage::normalize_path (the vendored path normalizer behind Storage's base path): the lexical normal
// form of os.path.normpath, with the current directory "." however it is spelled, the empty path included.
// A Storage with no base path keeps "./", so its volumes open relative to the working directory instead of
// at the root of the filesystem.

#undef NDEBUG  // the checks must run in every build type

#include "storage.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// The loop that was in this header at 8934dbc (the fs library's, a component at a time, a ".." found by
// scanning back for the last '/'), frozen: the specification the one-pass loop must give the bytes of, for
// every input and in all four flag combinations, never edited.
static std::size_t head_normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	char* const ret = dst;
	const bool absolute = src < end && *src == '/';
	const bool had_slash = src < end && end[-1] == '/';
	if (absolute) {
		*dst++ = '/';
	}
	char* const base = dst;  // the output after the root
	while (src < end) {
		const char* name = src;
		while (src < end && *src != '/') {
			++src;
		}
		const std::size_t size = static_cast<std::size_t>(src - name);
		if (src < end) {
			++src;
		}
		if (size == 0 || (size == 1 && name[0] == '.')) {
			continue;
		}
		if (size == 2 && name[0] == '.' && name[1] == '.') {
			char* last = dst;  // the start of the last component of the output
			while (last > base && last[-1] != '/') {
				--last;
			}
			if (dst > base && !(dst - last == 2 && last[0] == '.' && last[1] == '.')) {
				dst = last > base ? last - 1 : base;  // drop it and the '/' before it
				continue;
			}
			if (absolute) {
				continue;
			}
		}
		if (dst > base) {
			*dst++ = '/';
		}
		for (std::size_t i = 0; i < size; ++i) {
			*dst++ = name[i];
		}
	}
	if (dst == ret) {
		*dst++ = '.';
	}
	if ((keep_slash ? had_slash : slashed) && dst[-1] != '/') {
		*dst++ = '/';
	}
	return static_cast<std::size_t>(dst - ret);
}

// The two loops under test, by one name each (the same differential test as the fs library's).
static std::size_t np_ptr(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	return storage::normalize_path(src, end, dst, slashed, keep_slash);
}
static std::string np_str(std::string_view src, bool slashed, bool keep_slash) {
	return storage::normalize_path(src, slashed, keep_slash);
}

// A heap block of exactly `n` bytes (at least one is allocated, so a zero-size block is a real pointer): an
// AddressSanitizer build fails on a byte read or written past it.
struct Block {
	char* data;
	explicit Block(std::size_t n) : data(static_cast<char*>(std::malloc(n ? n : 1))) {}
	~Block() { std::free(data); }
	Block(const Block&) = delete;
	Block& operator=(const Block&) = delete;
};

struct DiffCounts {
	std::size_t compared = 0;
	int reported = 0;
};

// The input in the four flag combinations through both loops, with the input in a block of exactly its size
// and each output in a block of exactly `n + 2`, the room the contract gives; the empty input is also tried
// as a null range.
static void compare_one(const char* bytes, std::size_t n, DiffCounts& c) {
	Block src(n);
	if (n) { std::memcpy(src.data, bytes, n); }
	Block want(n + 2), got(n + 2);
	for (int flags = 0; flags < 4; ++flags) {
		const bool slashed = flags & 1, keep_slash = flags & 2;
		const std::size_t a = head_normalize_path(src.data, src.data + n, want.data, slashed, keep_slash);
		const std::size_t b = np_ptr(src.data, src.data + n, got.data, slashed, keep_slash);
		++c.compared;
		if (a != b || std::memcmp(want.data, got.data, a) != 0) {
			if (c.reported++ < 20) {
				std::printf("FAIL normalize_path(\"%.*s\" (%zu bytes), %d, %d) = \"%.*s\", the loop it replaced gives \"%.*s\"\n",
					static_cast<int>(n), bytes, n, slashed, keep_slash, static_cast<int>(b), got.data, static_cast<int>(a), want.data);
			}
			++failures;
		}
	}
	if (n == 0) {
		for (int flags = 0; flags < 4; ++flags) {
			const bool slashed = flags & 1, keep_slash = flags & 2;
			const std::size_t a = head_normalize_path(nullptr, nullptr, want.data, slashed, keep_slash);
			const std::size_t b = np_ptr(nullptr, nullptr, got.data, slashed, keep_slash);
			++c.compared;
			if (a != b || std::memcmp(want.data, got.data, a) != 0) {
				std::printf("FAIL normalize_path(nullptr, nullptr, %d, %d)\n", slashed, keep_slash);
				++failures;
			}
		}
	}
}

// Every string of `alphabet` (a list of one-byte symbols) from length 0 up to `max_length`.
static void exhaustive(const std::string& alphabet, int max_length, DiffCounts& c) {
	const std::size_t base = alphabet.size();
	char buffer[32];
	for (int length = 0; length <= max_length; ++length) {
		std::size_t total = 1;
		for (int i = 0; i < length; ++i) { total *= base; }
		for (std::size_t k = 0; k < total; ++k) {
			std::size_t m = k;
			for (int i = 0; i < length; ++i, m /= base) { buffer[i] = alphabet[m % base]; }
			compare_one(buffer, static_cast<std::size_t>(length), c);
		}
	}
}

struct Random {
	std::uint64_t x = 0x9e3779b97f4a7c15ULL;
	std::uint64_t next() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; }
	std::size_t below(std::size_t n) { return static_cast<std::size_t>((next() >> 11) % n); }
};

// Every exhaustive string has names of at most ten letters. These are a name of any length (up to forty
// letters), joined by one to three slashes, with "." and ".." and names of dots, a leading and a trailing run
// of slashes, now and then.
static void components(Random& r, int rounds, DiffCounts& c) {
	static const char* const names[] = {
		".", "..", "...", ".a", "a.", "..a", "a..", ".git", "a", "b", "ab", "abc", "abcdefg", "abcdefgh", "abcdefghi",
		"abcdefghijklmnop", "abcdefghijklmnopq", "0123456789abcdefghijklmnopqrstuvwxyzABCD", "\xaf", "\x2e\xaf", "\xff\xff\xff\xff\xff\xff\xff\xff",
	};
	const std::size_t count = sizeof(names) / sizeof(names[0]);
	std::string s;
	for (int round = 0; round < rounds; ++round) {
		s.clear();
		if (r.below(3) == 0) { s.append(1 + r.below(3), '/'); }
		const std::size_t parts = 1 + r.below(12);
		for (std::size_t i = 0; i < parts; ++i) {
			if (i) { s.append(1 + (r.below(8) == 0 ? r.below(3) : 0), '/'); }
			if (r.below(4) == 0) {
				s.append(1 + r.below(40), static_cast<char>("abz.0"[r.below(5)]));  // a name of one repeated byte
			} else {
				s += names[r.below(count)];
			}
		}
		if (r.below(3) == 0) { s.append(1 + r.below(3), '/'); }
		compare_one(s.data(), s.size(), c);
	}
}

// Bytes on either side of '/' (0x2f) and '.' (0x2e) in the table, high bytes, NUL and newline, among them.
static void bytes(Random& r, int rounds, DiffCounts& c) {
	static const unsigned char alphabet[] = {'/', '/', '/', '.', '.', '.', 'a', 'b', 0x00, 0x0a, 0x2d, 0x30, 0x2e, 0x2f, 0xaf, 0xae, 0x80, 0xff};
	std::string s;
	for (int round = 0; round < rounds; ++round) {
		s.clear();
		const std::size_t n = r.below(100);
		for (std::size_t i = 0; i < n; ++i) { s.push_back(static_cast<char>(alphabet[r.below(sizeof(alphabet))])); }
		compare_one(s.data(), s.size(), c);
	}
}

// The string_view overload builds its result in a buffer on the stack up to a size and on the heap above it:
// every length across that size, with inputs that give the longest result (a name and a trailing "." or a
// closing '/'), against the loop it replaced.
static void sizes(DiffCounts& c) {
	for (std::size_t n = 0; n <= 600; ++n) {
		std::string inputs[5];
		inputs[0].assign(n, 'a');                                          // one name
		inputs[1] = std::string(n ? n - 1 : 0, 'a') + (n ? "/" : "");      // one name and its closing '/'
		for (std::size_t i = 0; i < n; ++i) { inputs[2].push_back(i % 8 == 7 ? '/' : 'a'); }   // names of seven bytes
		for (std::size_t i = 0; i < n; ++i) { inputs[3].push_back(i % 2 ? '/' : '.'); }        // "./././" and the like
		for (std::size_t i = 0; i < n; ++i) { inputs[4].push_back("a/b/c/../"[i % 9]); }
		for (const std::string& s : inputs) {
			for (int flags = 0; flags < 4; ++flags) {
				const bool slashed = flags & 1, keep_slash = flags & 2;
				Block want(s.size() + 2);
				const std::size_t a = head_normalize_path(s.data(), s.data() + s.size(), want.data, slashed, keep_slash);
				const std::string got = np_str(std::string_view(s), slashed, keep_slash);
				++c.compared;
				if (got != std::string_view(want.data, a)) {
					std::printf("FAIL normalize_path(string_view of %zu bytes, %d, %d) differs from the loop it replaced\n", s.size(), slashed, keep_slash);
					++failures;
				}
			}
		}
	}
}

// The one-pass loop against the frozen one: every string of '/', '.', 'a' and 'b' up to length 10, every
// string of '/', '.' and 'a' up to length 12, random strings of components and of bytes, and every length of
// a path across the size the string overload keeps on the stack, on exact-size blocks.
static void test_head_differential() {
	DiffCounts c;
	exhaustive("/.ab", 10, c);
	exhaustive("/.a", 12, c);
	Random r;
	components(r, 400000, c);
	bytes(r, 400000, c);
	sizes(c);
	std::printf("normalize_path: %zu inputs and flags compared with the loop of 8934dbc\n", c.compared);
	CHECK(c.compared > 10000000);
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
	test_head_differential();
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
