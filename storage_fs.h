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
// wants. An empty path stays empty: it names no directory, and the root is not
// its normal form. Header-only, zero dependency.

#include <string>           // for std::string
#include <string_view>     // for std::string_view

namespace storage {

inline std::size_t normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	int levels = 0;
	char* ret = dst;
	char ch = '\0';
	const char* begin = src;
	// The first component of the output so far is dots only (a leading "." or ".."), and is still open.
	bool first_dots = true;
	bool first_open = true;
	// A ".." popped every component, so the path is "." unless something follows.
	bool emptied = false;
	// Visit every byte of [src, end), then the closing '/' of the last component unless keep_slash.
	// An empty path has no last component, so it gets no closing '/' (that would name the root).
	// There is no end - 1, so a default-constructed view (a null src) is the empty path.
	bool closing = !keep_slash && src < end;
	while (src < end || closing) {
		bool from_input = true;
		if (src < end) {
			ch = *src++;
		} else {
			ch = '/';
			closing = false;
			from_input = false;
		}
		if (ch == '.' && (levels != 0 || dst == ret || *(dst - 1) == '/' )) {
			*dst++ = ch;
			++levels;
		} else if (ch == '/') {
			// A ".." pops back to the '/' before the component it removes. The root is never popped,
			// nor the dots of a leading "." or ".."; the first component of a relative path is popped
			// like any other, and nothing is left (the path is then "." or what follows).
			const bool open = dst != ret;
			char* const floor = open && (*ret == '/' || first_dots) ? ret + 1 : ret;
			while (levels != 0 && dst > floor) {
				if (*--dst == '/') {
					--levels;
				}
			}
			if (open && dst == ret) {
				levels = 0;
				emptied = true;
				first_dots = true;
				first_open = true;
			} else if (dst == ret ? (from_input && src == begin + 1) : (*(dst - 1) != '/')) {
				*dst++ = ch;
				if (dst - ret > 1) {
					first_open = false;
				}
			}
		} else {
			*dst++ = ch;
			levels = 0;
			if (first_open && ch != '.') {
				first_dots = false;
			}
		}
	}
	if (ch == '.' && levels == 1) {
		ch = *--dst;
	}
	if (dst == ret && emptied) {
		*dst++ = '.';
		if (slashed || (keep_slash && end[-1] == '/')) {
			*dst++ = '/';
		}
	} else if (dst > ret + 1 && !keep_slash) {
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

inline std::string normalize_path(std::string_view src, bool slashed = false, bool keep_slash = false) {
	std::size_t src_size = src.size();
	const char* src_str = src.data();
	std::string dst;
	dst.resize(src_size + 1);
	auto dst_size = normalize_path(src_str, src_str + src_size, &dst[0], slashed, keep_slash);
	dst.resize(dst_size);
	return dst;
}

}  // namespace storage
