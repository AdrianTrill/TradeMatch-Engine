// Minimal driver for toolchains without libFuzzer (for example Apple clang or MSVC). It replays corpus files
// given on the command line, or feeds the fuzz target random inputs so the harness is exercised everywhere.
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

int run(int argc, char** argv) {
    std::uint64_t iterations = 1000;
    std::uint64_t seed = 1;
    std::vector<std::string> files;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--iterations" && index + 1 < argc) {
                iterations = std::stoull(argv[++index]);
            } else if (argument == "--seed" && index + 1 < argc) {
                seed = std::stoull(argv[++index]);
            } else {
                files.push_back(argument);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "invalid argument: " << error.what() << '\n';
        return 2;
    }

    if (!files.empty()) {
        for (const auto& path : files) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                std::cerr << "cannot open " << path << '\n';
                return 2;
            }
            const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        }
        std::cout << "[PASS] replayed " << files.size() << " corpus file(s)\n";
        return 0;
    }

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::size_t> size_distribution(0, 4096);
    std::uniform_int_distribution<unsigned> byte_distribution(0, 255);
    std::vector<std::uint8_t> buffer;

    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        buffer.resize(size_distribution(rng));
        for (auto& byte : buffer) {
            byte = static_cast<std::uint8_t>(byte_distribution(rng));
        }
        LLVMFuzzerTestOneInput(buffer.data(), buffer.size());
    }

    std::cout << "[PASS] fuzz target survived " << iterations << " random inputs (seed " << seed << ")\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
