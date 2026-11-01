# storage: a compressed, append-only, multi-volume blob store extracted from
# Xapiand.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
#
# Header-only engine: a Storage<Header, BinHeader, Footer, IO> template over an
# injected IO policy (DefaultIO by default) and an optional async-fsync hook. The
# on-disk format is byte-identical to the in-tree Xapiand engine. A record is
# stored raw or compressed with one of three codecs (LZ4, Zstandard, Deflate; the
# codec id is in the bin-header flags), all from the compressors library.

cmake_minimum_required(VERSION 3.16)
project(storage LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# --- Dependencies (the Kronuz family, tracked at tip) ---
#   compressors -> compressor_lz4.h, compressor_zstd.h, compressor_deflate.h, xxh32_stream.h
#                  (XXH32), Error/THROW
#   errno-names -> error::name / error::description           (error.hh)
#   strict-stox -> strict_stoull                              (strict_stox.hh)
#   stringified -> stringified                                (stringified.hh)
FetchContent_Declare(compressors GIT_REPOSITORY https://github.com/Kronuz/compressors.git GIT_TAG fe0e7127512ac14c57e9083733dcd48d2b701de8)
FetchContent_Declare(errno_names GIT_REPOSITORY https://github.com/Kronuz/errno-names.git GIT_TAG 0ec0801ff7b4d32412d33b2c7e041c6b55f38d14)
FetchContent_Declare(strict_stox GIT_REPOSITORY https://github.com/Kronuz/strict-stox.git GIT_TAG 5d979797b9799afcdacc0c004c229e2d61af1341)
FetchContent_Declare(stringified GIT_REPOSITORY https://github.com/Kronuz/stringified.git GIT_TAG 0053465ae99fb0f344f3c7a16dc88b4ec473ceb0)
FetchContent_MakeAvailable(compressors errno_names strict_stox stringified)

# --- Library target (header-only) ---
add_library(storage INTERFACE)
target_include_directories(storage INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(storage INTERFACE cxx_std_20)
add_library(storage::storage ALIAS storage)

# compressors exposes a namespaced ALIAS; errno-names' target is the plain
# errno_names; strict-stox / stringified expose namespaced ALIASes. All carry
# their include dirs transitively, so storage.h finds compressor_lz4.h /
# error.hh / strict_stox.hh / stringified.hh.
target_link_libraries(storage INTERFACE
	compressors::compressors
	errno_names
	strict_stox::strict_stox
	stringified::stringified
)

# Build the test + bench only when this repo is the top-level project, so
# consumers (e.g. via FetchContent) don't build them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	enable_testing()
	add_executable(storage_test test/test.cc)
	target_link_libraries(storage_test PRIVATE storage)
	add_test(NAME storage COMMAND storage_test)

	# On-disk identity: volumes written by the reference engine (one per codec, test/fixtures/)
	# must read back record for record, and the uncompressed volume must be reproduced byte for byte.
	add_executable(storage_format_golden test/format_golden.cc)
	target_link_libraries(storage_format_golden PRIVATE storage)
	target_compile_definitions(storage_format_golden PRIVATE STORAGE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
	add_test(NAME storage_format_golden COMMAND storage_format_golden)

	# The same suite built v2-ONLY (STORAGE_LEGACY_SUPPORT=0): proves the v1 path
	# compiles out cleanly and the v2 tests still pass without it. The legacy-read
	# test is #if'd out in this build.
	add_executable(storage_test_nolegacy test/test.cc)
	target_link_libraries(storage_test_nolegacy PRIVATE storage)
	target_compile_definitions(storage_test_nolegacy PRIVATE STORAGE_LEGACY_SUPPORT=0)
	add_test(NAME storage_nolegacy COMMAND storage_test_nolegacy)

	# The base path normalizer: os.path.normpath's form, with the current directory "." even for the
	# empty path (a Storage with no base path opens its volumes in the working directory, not at the root).
	add_executable(storage_test_path test/test_path.cc)
	target_link_libraries(storage_test_path PRIVATE storage)
	add_test(NAME storage_path COMMAND storage_test_path)

	# Crash-consistency + corruption matrix for the v2 format, driven through an
	# in-memory write-logging IO policy (simulated power loss).
	add_executable(storage_test_crash test/test_crash.cc)
	target_link_libraries(storage_test_crash PRIVATE storage)
	add_test(NAME storage_crash COMMAND storage_test_crash)

	# The streamed record write: round trips, compatibility with write(), misuse,
	# abandoned writers, a crash at every write, orphan reclaim. The big case writes
	# 4 GiB - 1 bytes and records past 4 GiB into sparse files.
	add_executable(storage_test_stream test/test_stream.cc)
	target_link_libraries(storage_test_stream PRIVATE storage)
	add_test(NAME storage_stream COMMAND storage_test_stream)
	add_test(NAME storage_stream_big COMMAND storage_test_stream big)

	# The same source with a 2 MiB volume cap: a record that cannot fit is refused.
	add_executable(storage_test_stream_full test/test_stream.cc)
	target_link_libraries(storage_test_stream_full PRIVATE storage)
	target_compile_definitions(storage_test_stream_full PRIVATE STREAM_FULL_TEST STORAGE_LAST_BLOCK_OFFSET=2097152)
	add_test(NAME storage_stream_full COMMAND storage_test_stream_full)

	# libFuzzer target (Clang): cmake -DSTORAGE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
	# The replay binary runs generated inputs through the same target as a test.
	option(STORAGE_FUZZ "Build the libFuzzer target" OFF)
	if(STORAGE_FUZZ)
		add_executable(storage_fuzz fuzz/fuzz_stream.cc)
		target_link_libraries(storage_fuzz PRIVATE storage)
		target_compile_options(storage_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
		target_link_options(storage_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

		add_executable(storage_fuzz_replay fuzz/fuzz_stream.cc fuzz/replay.cc)
		target_link_libraries(storage_fuzz_replay PRIVATE storage)
		add_test(NAME storage_fuzz_replay COMMAND storage_fuzz_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus)
	endif()

	# A runnable benchmark (not a test): ./build/storage_bench
	add_executable(storage_bench examples/bench.cc)
	target_link_libraries(storage_bench PRIVATE storage)

	# A realistic usage example that mirrors Xapiand's DataStorage (append-only,
	# compressed, checksummed WAL). It self-checks and is ALSO registered as a
	# ctest test, so it catches regressions in the write/commit/reopen/replay
	# round-trip, the zstd codec, and the header/footer integrity checks.
	add_executable(storage_toy_wal examples/toy_wal.cc)
	target_link_libraries(storage_toy_wal PRIVATE storage)
	add_test(NAME storage_toy_wal COMMAND storage_toy_wal)

	# The same, but for the DOCUMENT-STORE pattern (put/get by offset, tombstones,
	# multi-volume) on the v2 crash-safe (dual-meta) format. Showcases how to build
	# a Xapiand-style DataStorage and self-checks as a ctest.
	add_executable(storage_toy_data_store examples/toy_data_store.cc)
	target_link_libraries(storage_toy_data_store PRIVATE storage)
	add_test(NAME storage_toy_data_store COMMAND storage_toy_data_store)
endif()
