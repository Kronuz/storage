// Replays generated inputs through the fuzz target without libFuzzer, so the same checks run as a test.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#include <random>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
	std::mt19937_64 rng(20261030);
	std::vector<std::uint8_t> input;
	std::size_t runs = 0;
	for (std::size_t tail : {0, 1, 7, 100, 4000}) {
		for (std::uint16_t len : {0, 1, 5, 4085, 4090, 4096, 9000, 40000}) {
			for (std::uint8_t flags : {0, 1, 2, 3, 6, 10, 11, 14}) {
				for (int round = 0; round < (flags & 2 ? 3 : 6); ++round) {
					input.resize(9 + tail);
					for (auto& b : input) { b = static_cast<std::uint8_t>(rng()); }
					std::memcpy(input.data(), &len, 2);
					input[3] = flags;
					LLVMFuzzerTestOneInput(input.data(), input.size());
					++runs;
				}
			}
		}
	}
	if (argc > 1) {  // the committed corpus, crashers included
		for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
			std::ifstream f(e.path(), std::ios::binary);
			input.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
			LLVMFuzzerTestOneInput(input.data(), input.size());
			++runs;
		}
	}
	std::printf("storage fuzz replay: %zu inputs, ok\n", runs);
}
