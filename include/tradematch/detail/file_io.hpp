#ifndef TRADEMATCH_DETAIL_FILE_IO_HPP
#define TRADEMATCH_DETAIL_FILE_IO_HPP

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tradematch::detail {

// Reads a whole file. Throws std::runtime_error if it can't be read.
std::vector<std::uint8_t> read_file(const std::filesystem::path& path);

// std::fopen, with Unicode paths on Windows. Returns nullptr on failure.
std::FILE* open_file(const std::filesystem::path& path, const char* mode);

// Forces data the operating system has accepted for `file` onto stable storage: F_FULLFSYNC on macOS, where plain
// fsync can leave it in the drive's cache, fsync elsewhere on POSIX, and _commit on Windows.
bool sync_file(std::FILE* file) noexcept;

// Makes a rename or newly created file in `directory` durable on POSIX. A no-op on Windows.
void sync_directory(const std::filesystem::path& directory) noexcept;

// The text of the last C library error, for exception messages.
std::string last_error();

}  // namespace tradematch::detail

#endif
