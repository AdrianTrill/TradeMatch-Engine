#include "tradematch/cli.hpp"
#include "tradematch/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// A scratch directory, removed again when the test ends.
class TempDir {
public:
    TempDir() {
        std::random_device device;
        path_ = std::filesystem::temp_directory_path() / ("tradematch-cli-tests-" + std::to_string(device()));
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::string file(const char* name) const {
        return (path_ / name).string();
    }

private:
    std::filesystem::path path_;
};

void test_sessions_can_be_journaled_saved_and_loaded() {
    const TempDir dir;
    const std::string journal = dir.file("session.tmj");
    const std::string snapshot = dir.file("session.snapshot");
    std::uint64_t hash = 0;
    {
        std::ostringstream out;
        tradematch::CommandInterpreter cli(out);
        cli.execute("journal " + journal);
        for (const char* line : {"add ACME",
                                 "add GLOBEX",
                                 "use ACME",
                                 "buy 100 @ 100.00",
                                 "sell 40 @ 100.00",
                                 "buy GLOBEX 5 @ 20.00 day",
                                 "symbols",
                                 "help",
                                 "hash"}) {
            cli.execute(line);
        }
        cli.execute("save " + snapshot);
        expect(cli.errors() == 0U, "The session should run without errors:\n" + out.str());
        expect(contains(out.str(), "journaling to") && contains(out.str(), "* GLOBEX") &&
                   contains(out.str(), "state hash 0x") && contains(out.str(), "saved 2 symbols"),
               "The session should report what it did:\n" + out.str());
        cli.execute("load " + snapshot);
        cli.execute("journal " + dir.file("second.tmj"));
        expect(cli.errors() == 2U, "Loading or restarting the journal while journaling should be refused.");
        hash = tradematch::state_hash(cli.engine());
    }

    tradematch::MatchingEngine replayed;
    tradematch::recover(replayed, journal);
    expect(tradematch::state_hash(replayed) == hash, "The session's journal should replay to the same state.");

    std::ostringstream out;
    tradematch::CommandInterpreter cli(out);
    cli.execute("load " + snapshot);
    expect(tradematch::state_hash(cli.engine()) == hash, "Loading the snapshot should restore the same state.");
    cli.execute("buy ACME 10 @ 99.00");
    expect(contains(out.str(), "loaded 2 symbols with 2 resting orders") && contains(out.str(), "ACME #4 BUY"),
           "New orders after a load should continue after the highest loaded ID:\n" + out.str());
    cli.execute("add BETA");
    cli.execute("journal " + dir.file("late.tmj"));
    expect(cli.errors() == 1U, "A journal must start before the first symbol, or it couldn't replay.");
}

void test_sessions_end_on_quit() {
    std::ostringstream out;
    tradematch::CommandInterpreter cli(out);
    expect(cli.execute("") && cli.execute("   # only a comment") && cli.execute("symbols"),
           "Blank lines and comments should do nothing.");
    expect(contains(out.str(), "no symbols yet"), "symbols should say when there are none.");
    expect(!cli.execute("QUIT") && !cli.execute("exit"), "quit and exit should end the session.");
}

// Random lines built from the command vocabulary, plus malformed numbers. Whatever comes in, the interpreter must
// answer with output or an error, and never throw or crash; sanitizer builds check the "never crash" part.
void test_random_input_never_escapes() {
    const std::vector<std::string> vocabulary{"buy",
                                              "sell",
                                              "stop",
                                              "cancel",
                                              "replace",
                                              "book",
                                              "order",
                                              "auction",
                                              "begin",
                                              "show",
                                              "uncross",
                                              "expire",
                                              "trades",
                                              "hash",
                                              "add",
                                              "use",
                                              "symbols",
                                              "@",
                                              "market",
                                              "limit",
                                              "ACME",
                                              "GLOBEX",
                                              "100",
                                              "100.5",
                                              "100.25",
                                              "0",
                                              "-1",
                                              "7",
                                              "99999999999999999999",
                                              "1.",
                                              ".5",
                                              "1e5",
                                              "ioc",
                                              "fok",
                                              "day",
                                              "gtc",
                                              "post-only",
                                              "display=5",
                                              "display=",
                                              "owner=3",
                                              "stp=oldest",
                                              "stp=both",
                                              "id=7",
                                              "id=x",
                                              "tif=day",
                                              "=",
                                              "#",
                                              "\t"};
    std::mt19937_64 rng(5);
    std::ostringstream out;
    tradematch::CommandInterpreter cli(out);
    cli.execute("add ACME");
    cli.execute("add GLOBEX");
    for (int line_number = 0; line_number < 50'000; ++line_number) {
        std::string line;
        const auto words = 1 + rng() % 7;
        for (std::uint64_t word = 0; word < words; ++word) {
            line += vocabulary[rng() % vocabulary.size()] + ' ';
        }
        if (line.starts_with("quit") || line.starts_with("exit")) {
            continue;
        }
        cli.execute(line);
    }
    // Lines such as "add market" legitimately add symbols, so only the lower bound is fixed.
    expect(cli.errors() > 1'000U && cli.engine().symbol_count() >= 2U,
           "Most random lines are malformed and should be reported as errors.");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"sessions_can_be_journaled_saved_and_loaded", test_sessions_can_be_journaled_saved_and_loaded},
        {"sessions_end_on_quit", test_sessions_end_on_quit},
        {"random_input_never_escapes", test_random_input_never_escapes},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    if (failures == 0U) {
        std::cout << "All command line tests passed (" << tests.size() << ").\n";
        return 0;
    }
    std::cout << failures << " test(s) failed.\n";
    return 1;
}

}  // namespace

int main() {
    try {
        return run_all_tests();
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
