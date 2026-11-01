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

inline std::string normalize_path(std::string_view src, bool slashed = false, bool keep_slash = false) {
	std::size_t src_size = src.size();
	const char* src_str = src.data();
	std::string dst;
	dst.resize(src_size + 2);
	auto dst_size = normalize_path(src_str, src_str + src_size, &dst[0], slashed, keep_slash);
	dst.resize(dst_size);
	return dst;
}

}  // namespace storage
