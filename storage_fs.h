/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

// Minimal filesystem helper for the storage library: a pure-string path
// normalizer, from Xapiand's fs.cc and the same algorithm as the fs library's
// (the only thing storage.h needed from fs.hh once volume discovery is done
// inline with readdir). It resolves "." / ".." textually (no stat, no symlink
// resolution) and applies the trailing-slash policy the engine's base_path
// wants. The current directory is "." however it is spelled, the empty path
// included: "." and "./" open and stat, "" does not. Header-only, zero
// dependency.

#include <string>           // for std::string
#include <string_view>     // for std::string_view

namespace storage {

// The lexical normal form of a path, as Python's os.path.normpath makes it: empty and "." components are
// dropped, a ".." removes the component before it, a relative path keeps the ".." it cannot remove
// ("../x" stays), the root absorbs a ".." ("/.." is "/"), a run of leading '/' is one root, and the
// current directory is "." however it is spelled ("", ".", "./", "x/..", "./x/.."). A component of three or
// more dots is a name. The trailing '/' is the flags' policy: none; `slashed` adds one; `keep_slash`
// keeps the input's (the root is always "/"). `dst` holds (end - src) + 2 bytes. Returns the length.
inline std::size_t normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	char* const ret = dst;
	const bool absolute = src < end && *src == '/';
	const bool had_slash = src < end && end[-1] == '/';
	if (absolute) {
		*dst++ = '/';
	}
	char* const base = dst;  // the output after the root
	char* floor = base;      // the end of the ".." the output starts with: a ".." pops nothing at or below it
	// One pass. Each name is copied to `dst` as it is read, behind the '/' that will separate it (written
	// first; the name's first byte overwrites it when the output is empty), and kept by moving `dst` past
	// it. An empty name, ".", and a ".." that pops are dropped by leaving `dst` where it was. The output is
	// never longer than the input read so far, so what is written ahead of `dst` stays inside its bytes.
	while (src < end) {
		if (*src == '/') {  // an empty name
			++src;
			continue;
		}
		*dst = '/';
		char* const w = dst + (dst > base);
		char* p = w;
		const char* const name = src;
		do {
			*p++ = *src++;
		} while (src < end && *src != '/');
		if (src < end) {
			++src;
		}
		if (*name == '.') {  // ".", "..", or a name that begins with a dot (any other name is kept)
			const std::size_t size = static_cast<std::size_t>(p - w);
			if (size == 1) {
				continue;
			}
			if (size == 2 && name[1] == '.') {
				if (dst > floor) {
					char* last = dst;  // the start of the last component of the output
					while (last > floor && last[-1] != '/') {
						--last;
					}
					dst = last > floor ? last - 1 : floor;  // drop it and the '/' before it
					continue;
				}
				if (absolute) {
					continue;
				}
				dst = floor = p;  // a relative path keeps a ".." it cannot pop
				continue;
			}
		}
		dst = p;
	}
	if (dst == ret) {
		*dst++ = '.';
	}
	if ((keep_slash ? had_slash : slashed) && dst[-1] != '/') {
		*dst++ = '/';
	}
	return static_cast<std::size_t>(dst - ret);
}

inline std::string normalize_path(std::string_view src, bool slashed = false, bool keep_slash = false) {
	const std::size_t size = src.size();
	const char* const begin = src.data();
	// A path of a usual size is built on the stack and copied once into a string of its own size: a string
	// resized to size + 2 and shrunk would be zero-filled, and past the small-string size (15 bytes with
	// libstdc++) it would allocate for a result that fits in the string.
	constexpr std::size_t stack_size = 512;
	if (size + 2 <= stack_size) {
		char buffer[stack_size];
		return std::string(buffer, normalize_path(begin, begin + size, buffer, slashed, keep_slash));
	}
	std::string dst;
	dst.resize(size + 2);
	dst.resize(normalize_path(begin, begin + size, &dst[0], slashed, keep_slash));
	return dst;
}

}  // namespace storage
