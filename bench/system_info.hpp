#ifndef TRADEMATCH_BENCH_SYSTEM_INFO_HPP
#define TRADEMATCH_BENCH_SYSTEM_INFO_HPP

#include <string>

#ifdef __APPLE__
#include <sys/sysctl.h>

#include <array>
#elif defined(__linux__)
#include <fstream>
#endif

namespace tradematch::bench {

inline std::string cpu_name() {
#ifdef __APPLE__
    std::array<char, 256> buffer{};
    std::size_t size = buffer.size();
    if (sysctlbyname("machdep.cpu.brand_string", buffer.data(), &size, nullptr, 0) == 0) {
        return std::string{buffer.data()};
    }
#elif defined(__linux__)
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.starts_with("model name")) {
            const auto colon = line.find(':');
            if (colon != std::string::npos && colon + 2 <= line.size()) {
                return line.substr(colon + 2);
            }
        }
    }
#endif
    return "unknown CPU";
}

inline std::string compiler_name() {
#if defined(__clang__) && defined(__apple_build_version__)
    return std::string{"Apple clang "} + __clang_version__;
#elif defined(__clang__)
    return std::string{"clang "} + __clang_version__;
#elif defined(__GNUC__)
    return std::string{"GCC "} + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown compiler";
#endif
}

constexpr bool optimized_build() noexcept {
#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && defined(NDEBUG))
    return true;
#else
    return false;
#endif
}

}  // namespace tradematch::bench

#endif
