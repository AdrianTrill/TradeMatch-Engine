// The interactive command line, and the runner for scripted scenarios.
//
//   tradematch_cli                       an interactive session (piped input runs as a script)
//   tradematch_cli SCRIPT...             runs scripts and prints their transcripts
//   tradematch_cli --check SCRIPT...     compares each transcript with the script's .expected file
//   tradematch_cli --update SCRIPT...    rewrites the .expected files
#include "tradematch/cli.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

enum class Mode : std::uint8_t {
    Run,
    Check,
    Update
};

bool stdin_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read " + path.string());
    }
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::erase(text, '\r');  // a Windows checkout may have converted line endings
    return text;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        lines.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

// Echoes every command after a "> " prompt, so the transcript reads like the session that produced it. Comments and
// blank lines are kept as they are.
std::string run_script(std::string_view script) {
    std::ostringstream transcript;
    tradematch::CommandInterpreter interpreter(transcript);
    for (const std::string& line : split_lines(script)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            transcript << line << '\n';
            continue;
        }
        transcript << "> " << line << '\n';
        if (!interpreter.execute(line)) {
            break;
        }
    }
    return transcript.str();
}

std::filesystem::path expected_path(const std::filesystem::path& script) {
    auto path = script;
    path.replace_extension(".expected");
    return path;
}

// Returns true when the transcript matches; otherwise prints the first difference.
bool check(const std::filesystem::path& script, const std::string& transcript) {
    const auto golden = expected_path(script);
    if (!std::filesystem::exists(golden)) {
        std::cout << "[FAIL] " << script.string() << ": " << golden.string()
                  << " is missing (create it with --update)\n";
        return false;
    }
    const auto expected = split_lines(read_text(golden));
    const auto actual = split_lines(transcript);
    for (std::size_t index = 0; index < std::max(expected.size(), actual.size()); ++index) {
        const std::string want = index < expected.size() ? expected[index] : "<end of file>";
        const std::string got = index < actual.size() ? actual[index] : "<end of file>";
        if (want != got) {
            std::cout << "[FAIL] " << script.string() << ": line " << index + 1 << " differs\n"
                      << "  expected: " << want << "\n"
                      << "  actual:   " << got << '\n';
            return false;
        }
    }
    std::cout << "[PASS] " << script.string() << '\n';
    return true;
}

int interactive() {
    const bool terminal = stdin_is_terminal();
    if (!terminal) {
        std::cout << run_script(
            std::string{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()});
        return 0;
    }
    std::cout << "TradeMatch command line. Type help for the commands, quit to leave.\n";
    tradematch::CommandInterpreter interpreter(std::cout);
    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line) || !interpreter.execute(line)) {
            break;
        }
    }
    std::cout << '\n';
    return 0;
}

int run(int argc, char** argv) {
    Mode mode = Mode::Run;
    std::vector<std::filesystem::path> scripts;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--check") {
            mode = Mode::Check;
        } else if (arg == "--update") {
            mode = Mode::Update;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: tradematch_cli [--check | --update] [SCRIPT...]\n"
                         "With no script, starts an interactive session; type help there for the commands.\n";
            return 0;
        } else {
            scripts.emplace_back(arg);
        }
    }
    if (scripts.empty()) {
        if (mode != Mode::Run) {
            throw std::invalid_argument("--check and --update need at least one script");
        }
        return interactive();
    }

    bool passed = true;
    for (const auto& script : scripts) {
        const std::string transcript = run_script(read_text(script));
        if (mode == Mode::Run) {
            std::cout << transcript;
        } else if (mode == Mode::Update) {
            std::ofstream output(expected_path(script), std::ios::binary);
            output << transcript;
            if (!output) {
                throw std::runtime_error("cannot write " + expected_path(script).string());
            }
            std::cout << "updated " << expected_path(script).string() << '\n';
        } else {
            passed = check(script, transcript) && passed;
        }
    }
    return passed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
