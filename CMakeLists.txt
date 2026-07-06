# storage: a compressed, append-only, multi-volume blob store extracted from
# Xapiand.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
#
# Header-only engine: a Storage<Header, BinHeader, Footer, IO> template over an
# injected IO policy (DefaultIO by default) and an optional async-fsync hook. The
# on-disk format is byte-identical to the in-tree Xapiand engine; LZ4 is the only
# codec for now (a codec-in-header refactor for zstd/deflate is a follow-up).

cmake_minimum_required(VERSION 3.16)
project(storage LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# --- Dependencies (the Kronuz family, tracked at tip) ---
#   compressors -> compressor_lz4.h (LZ4*), xxh32_stream.h (XXH32), Error/THROW
#   errno-names -> error::name / error::description           (error.hh)
#   strict-stox -> strict_stoull                              (strict_stox.hh)
#   stringified -> stringified                                (stringified.hh)
FetchContent_Declare(compressors GIT_REPOSITORY https://github.com/Kronuz/compressors.git GIT_TAG main)
FetchContent_Declare(errno_names GIT_REPOSITORY https://github.com/Kronuz/errno-names.git GIT_TAG main)
FetchContent_Declare(strict_stox GIT_REPOSITORY https://github.com/Kronuz/strict-stox.git GIT_TAG main)
FetchContent_Declare(stringified GIT_REPOSITORY https://github.com/Kronuz/stringified.git GIT_TAG main)
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

	# The same suite built v2-ONLY (STORAGE_LEGACY_SUPPORT=0): proves the v1 path
	# compiles out cleanly and the v2 tests still pass without it. The legacy-read
	# test is #if'd out in this build.
	add_executable(storage_test_nolegacy test/test.cc)
	target_link_libraries(storage_test_nolegacy PRIVATE storage)
	target_compile_definitions(storage_test_nolegacy PRIVATE STORAGE_LEGACY_SUPPORT=0)
	add_test(NAME storage_nolegacy COMMAND storage_test_nolegacy)

	# Crash-consistency + corruption matrix for the v2 format, driven through an
	# in-memory write-logging IO policy (simulated power loss).
	add_executable(storage_test_crash test/test_crash.cc)
	target_link_libraries(storage_test_crash PRIVATE storage)
	add_test(NAME storage_crash COMMAND storage_test_crash)

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
