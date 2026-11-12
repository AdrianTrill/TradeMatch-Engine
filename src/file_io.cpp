#include "tradematch/detail/file_io.hpp"

#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tradematch::detail {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }
    const std::streamoff size = input.tellg();
    if (size < 0) {
        throw std::runtime_error("cannot read " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (size > 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return bytes;
}

std::FILE* open_file(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wide_mode;
    for (const char* character = mode; *character != '\0'; ++character) {
        wide_mode.push_back(static_cast<wchar_t>(*character));
    }
    // Unlike _wfopen_s, _wfsopen lets other handles read the file meanwhile, as POSIX always does.
    return _wfsopen(path.c_str(), wide_mode.c_str(), _SH_DENYNO);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool sync_file(std::FILE* file) noexcept {
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    const int descriptor = fileno(file);
#ifdef F_FULLFSYNC
    if (fcntl(descriptor, F_FULLFSYNC) == 0) {
        return true;
    }
    // Some file systems don't support F_FULLFSYNC; fall back to fsync.
#endif
    return fsync(descriptor) == 0;
#endif
}

void sync_directory([[maybe_unused]] const std::filesystem::path& directory) noexcept {
#ifndef _WIN32
    const int descriptor = open(directory.empty() ? "." : directory.c_str(), O_RDONLY);
    if (descriptor >= 0) {
        fsync(descriptor);
        close(descriptor);
    }
#endif
}

std::string last_error() {
    return std::generic_category().message(errno);
}

}  // namespace tradematch::detail
