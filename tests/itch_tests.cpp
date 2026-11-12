#include "support/differential.hpp"
#include "tradematch/detail/file_io.hpp"
#include "tradematch/itch.hpp"
#include "tradematch/itch_feed.hpp"
#include "tradematch/matching_engine.hpp"
#include "tradematch/simulator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace {

namespace itch = tradematch::itch;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

// A file in the temporary directory, removed when it goes out of scope.
class TempFile {
public:
    TempFile()
        : path_(std::filesystem::temp_directory_path() /
                ("tradematch-itch-test-" + std::to_string(std::random_device{}()) + ".itch")) {}
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    void write(std::span<const std::uint8_t> bytes) const {
        std::FILE* file = tradematch::detail::open_file(path_, "wb");
        expect(file != nullptr, "The temporary file should open.");
        const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
        expect(written == bytes.size(), "The temporary file should take every byte.");
    }

    std::FILE* open() const {
        std::FILE* file = tradematch::detail::open_file(path_, "rb");
        expect(file != nullptr, "The temporary file should open.");
        return file;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> encoded(const itch::Message& message) {
    std::vector<std::uint8_t> bytes;
    itch::encode(message, bytes);
    return bytes;
}

// Applies each length-prefixed message in the buffer to the replica.
void apply_frames(itch::BookReplica& replica, std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        expect(bytes.size() >= 2, "A frame should start with its length.");
        const std::size_t length = (std::size_t{bytes[0]} << 8U) | bytes[1];
        expect(bytes.size() >= 2 + length, "A frame should be complete.");
        expect(replica.apply(bytes.subspan(2, length)), "Every published message should decode.");
        bytes = bytes.subspan(2 + length);
    }
}

void test_add_order_matches_the_specification() {
    itch::AddOrder add{itch::Header{1, 2, 0x1234'5678'9ABC}};
    add.order_reference = 0x0102'0304'0506'0708;
    add.side = 'B';
    add.shares = 100;
    add.stock = itch::make_stock("AAPL");
    add.price = 1'234'500;  // $123.45
    // Laid out by hand from the ITCH 5.0 specification.
    const std::vector<std::uint8_t> expected{
        'A',  0x00, 0x01, 0x00, 0x02, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,  // type, locate, tracking, timestamp
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,                    // order reference
        'B',  0x00, 0x00, 0x00, 0x64,                                      // side, shares
        'A',  'A',  'P',  'L',  ' ',  ' ',  ' ',  ' ',                     // stock
        0x00, 0x12, 0xD6, 0x44,                                            // price
    };
    expect(encoded(add) == expected, "An AddOrder should encode exactly as the specification lays it out.");

    const auto decoded = itch::decode(expected);
    expect(decoded && std::holds_alternative<itch::AddOrder>(*decoded) && std::get<itch::AddOrder>(*decoded) == add,
           "The specification's bytes should decode to the same order.");
    expect(decoded && itch::type_of(*decoded) == 'A' && itch::header_of(*decoded).timestamp == 0x1234'5678'9ABCU,
           "The type and header should be readable from any message.");
    expect(itch::timestamp_of(expected) == 0x1234'5678'9ABCU && !itch::timestamp_of(std::span{expected}.first(10)),
           "The timestamp should be readable without decoding, from any message long enough to carry one.");

    std::vector<std::uint8_t> framed;
    itch::encode_framed(add, framed);
    expect(framed.size() == 38U && framed[0] == 0U && framed[1] == 36U &&
               std::equal(framed.begin() + 2, framed.end(), expected.begin(), expected.end()),
           "A framed message should carry its length first, as in NASDAQ's files.");

    add.attribution = std::array<char, 4>{'G', 'S', 'C', 'O'};
    const auto attributed = encoded(add);
    expect(attributed.size() == 40U && attributed.front() == 'F' && attributed.back() == 'O',
           "An attributed add should be an 'F' message ending in its MPID.");
    expect(itch::decode(attributed) == itch::Message{add}, "An attributed add should round-trip.");
}

void test_every_type_has_its_specified_length() {
    // Every ITCH 5.0 message type with its length from the specification.
    constexpr std::string_view kTypes = "SRHYLVWKJhAFECXDUPQBINO";
    constexpr std::array<std::size_t, 23> kLengths{12, 39, 25, 20, 26, 35, 12, 28, 35, 21, 36, 40,
                                                   31, 36, 23, 19, 35, 44, 40, 19, 50, 20, 48};
    for (std::size_t index = 0; index < kTypes.size(); ++index) {
        const char type = kTypes[index];
        const std::string name{type};
        expect(itch::message_length(type) == kLengths.at(index), "Wrong length for type " + name);

        std::vector<std::uint8_t> bytes(kLengths.at(index));
        for (std::size_t position = 0; position < bytes.size(); ++position) {
            bytes[position] = static_cast<std::uint8_t>(position * 37U + 11U);
        }
        bytes.front() = static_cast<std::uint8_t>(type);
        const auto decoded = itch::decode(bytes);
        expect(decoded && itch::type_of(*decoded) == type, "Type " + name + " should decode.");
        expect(decoded && encoded(*decoded) == bytes, "Type " + name + " should re-encode to the same bytes.");
        expect(decoded == itch::decode(bytes), "Decoding the same " + name + " twice should give equal messages.");

        expect(!itch::decode(std::span{bytes}.first(bytes.size() - 1)), "A short " + name + " should be rejected.");
        bytes.push_back(0);
        expect(!itch::decode(bytes), "A long " + name + " should be rejected.");
    }
    for (const char unknown : {'Z', 'a', '\0'}) {
        const std::vector<std::uint8_t> bytes(19, static_cast<std::uint8_t>(unknown));
        expect(itch::message_length(unknown) == 0U && !itch::decode(bytes), "Unknown types should be rejected.");
    }
    expect(!itch::decode({}), "An empty message should be rejected.");

    bool threw = false;
    try {
        static_cast<void>(encoded(itch::OrderDelete{itch::Header{1, 0, std::uint64_t{1} << 48U}, 1}));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "A timestamp beyond 48 bits can't be encoded.");
}

void test_symbols_and_prices() {
    const itch::Stock stock = itch::make_stock("MSFT");
    expect(std::string_view(stock.data(), stock.size()) == "MSFT    " && itch::symbol_of(stock) == "MSFT",
           "Symbols should be padded with spaces, and trimmed again.");
    expect(itch::symbol_of(itch::make_stock("")).empty() && itch::symbol_of(itch::make_stock("ABCDEFGH")) == "ABCDEFGH",
           "Empty and full-width symbols should work.");
    bool threw = false;
    try {
        static_cast<void>(itch::make_stock("TOOLONGSYM"));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "A symbol longer than eight characters should be refused.");
    expect(itch::format_price(1'234'500) == "123.4500" && itch::format_price(1) == "0.0001" &&
               itch::format_price(0) == "0.0000",
           "Prices should print with four decimals.");
}

void test_file_reader_streams_messages() {
    std::vector<std::uint8_t> bytes;
    constexpr std::uint64_t kMessages = 20'000;  // about 420 KB, several of the reader's blocks
    for (std::uint64_t index = 0; index < kMessages; ++index) {
        itch::encode_framed(itch::OrderDelete{itch::Header{1, 0, index}, index}, bytes);
    }
    const TempFile whole;
    whole.write(bytes);
    std::FILE* file = whole.open();
    itch::FileReader reader(file, 70'000);
    std::uint64_t count = 0;
    while (const auto message = reader.next()) {
        const auto decoded = itch::decode(*message);
        expect(decoded && std::holds_alternative<itch::OrderDelete>(*decoded) &&
                   std::get<itch::OrderDelete>(*decoded).order_reference == count,
               "Messages should come back whole and in order.");
        ++count;
    }
    std::fclose(file);
    expect(count == kMessages && reader.bytes_read() == bytes.size(), "Every message should be read once.");

    const TempFile torn;
    torn.write(std::span{bytes}.first(bytes.size() - 5));
    file = torn.open();
    itch::FileReader torn_reader(file, 70'000);
    bool threw = false;
    try {
        while (torn_reader.next()) {
        }
    } catch (const std::runtime_error&) {
        threw = true;
    }
    std::fclose(file);
    expect(threw, "A file that ends partway through a message should be reported.");
}

// Applies one generated operation to the book and to the generator's view of it.
void apply_operation(tradematch::OrderBook& book,
                     tradematch::testing::ReferenceBook& generator_view,
                     const tradematch::testing::Operation& op) {
    using tradematch::testing::OpKind;
    switch (op.kind) {
        case OpKind::Submit:
            book.submit(op.order);
            generator_view.submit(op.order);
            break;
        case OpKind::Cancel:
            book.cancel(op.order.order_id);
            generator_view.cancel(op.order.order_id);
            break;
        case OpKind::Replace:
            book.replace(op.order);
            generator_view.replace(op.order);
            break;
        case OpKind::ExpireDay:
            book.expire_day_orders();
            generator_view.expire_day_orders();
            break;
        case OpKind::BeginAuction:
            book.begin_auction();
            generator_view.begin_auction();
            break;
        case OpKind::Uncross:
            book.uncross();
            generator_view.uncross();
            break;
        case OpKind::Snapshot:
            book.import_state(book.export_state());
            break;
    }
}

void test_feed_rebuilds_the_engine_exactly() {
    using tradematch::testing::OpKind;
    std::array<std::uint64_t, 256> totals{};
    for (std::uint64_t seed = 1; seed <= 30; ++seed) {
        tradematch::MatchingEngine engine;
        const auto symbol = engine.add_symbol("ACME");
        itch::FeedPublisher publisher(engine);
        engine.set_order_listener(&publisher);
        itch::BookReplica replica;
        tradematch::testing::ReferenceBook generator_view;
        tradematch::testing::OperationGenerator generator(seed);

        for (std::size_t step = 0; step < 2'000; ++step) {
            const auto op =
                step % 97 == 96 ? tradematch::testing::Operation{OpKind::Snapshot, {}} : generator.next(generator_view);
            apply_operation(engine.book(symbol), generator_view, op);
            apply_frames(replica, publisher.take_bytes());
            const std::string difference = itch::first_difference(engine, publisher, replica);
            expect(difference.empty(),
                   "seed " + std::to_string(seed) + ", step " + std::to_string(step) + ": " + difference);
        }
        expect(replica.stats().anomalies == 0U && replica.stats().invalid_messages == 0U,
               "The engine's own feed should never surprise the replica.");
        for (const char type : std::string_view{"RAEXDUCQ"}) {
            totals.at(static_cast<unsigned char>(type)) += publisher.message_count(type);
        }
    }
    std::cout << "      ";
    for (const char type : std::string_view{"RAEXDUCQ"}) {
        std::cout << ' ' << type << ' ' << totals.at(static_cast<unsigned char>(type));
    }
    std::cout << '\n';
    for (const char type : std::string_view{"RAEXDUCQ"}) {
        expect(totals.at(static_cast<unsigned char>(type)) > 0U,
               std::string{"The flow should publish '"} + type + "' messages.");
    }
}

void test_replica_volume_and_anomalies() {
    itch::BookReplica replica;
    const itch::Header header{1, 0, 1'000};
    const itch::Stock stock = itch::make_stock("TEST");
    itch::StockDirectory directory{header};
    directory.stock = stock;
    replica.apply(directory);
    const auto add = [&](std::uint64_t reference, char side, std::uint32_t shares, std::uint32_t price) {
        itch::AddOrder order{header};
        order.order_reference = reference;
        order.side = side;
        order.shares = shares;
        order.stock = stock;
        order.price = price;
        replica.apply(order);
    };
    const auto symbol = replica.find("TEST");
    if (!symbol) {
        throw TestFailure("A stock directory message should create the stock.");
    }
    const tradematch::OrderBook& book = replica.engine().book(*symbol);

    // Volume counts printable executions, non-displayed trades, and crosses, but not non-printable executions.
    add(1, 'B', 100, 100'000);
    replica.apply(itch::OrderExecuted{header, 1, 30, 1});
    replica.apply(itch::OrderExecutedWithPrice{header, 1, 10, 2, 'N', 99'000});
    replica.apply(itch::OrderExecutedWithPrice{header, 1, 5, 7, 'Y', 99'500});
    replica.apply(itch::NonCrossTrade{header, 0, 'B', 25, stock, 100'100, 3});
    replica.apply(itch::CrossTrade{header, 500, stock, 99'000, 4, 'O'});
    const itch::StockStats& stats = replica.stock_stats(*symbol);
    const auto executed = book.get_order(1);
    expect(executed && executed->remaining_quantity == 55U, "Executions should take shares off the order.");
    constexpr double kValue = 30 * 10.0 + 5 * 9.95 + 25 * 10.01 + 500 * 9.9;
    expect(stats.volume == 560U && stats.executions == 4U && stats.last_price == 99'000U &&
               stats.value > kValue - 0.001 && stats.value < kValue + 0.001,
           "Volume should count printable executions, non-displayed trades, and crosses.");

    // A partial cancel keeps the order's place in the queue; a replace sends it to the back with a new reference.
    add(2, 'B', 40, 100'000);
    replica.apply(itch::OrderCancel{header, 1, 20});
    expect(book.snapshot().bids.at(0).order_ids == std::vector<tradematch::OrderId>{1, 2},
           "A partial cancel should keep queue priority.");
    replica.apply(itch::OrderReplace{header, 1, 3, 70, 100'000});
    const auto replaced = book.get_order(3);
    expect(book.snapshot().bids.at(0).order_ids == std::vector<tradematch::OrderId>{2, 3} && replaced &&
               replaced->remaining_quantity == 70U && replaced->side == tradematch::Side::Buy,
           "A replace should re-enter at the back, keeping the side.");
    replica.apply(itch::OrderDelete{header, 2});
    replica.apply(itch::OrderExecuted{header, 3, 70, 5});
    expect(book.order_count() == 0U && replica.stats().live_orders == 0U && replica.stats().peak_orders == 2U,
           "Deleted and fully executed orders should leave the book.");
    expect(replica.stats().anomalies == 0U, "Consistent messages are no anomalies.");

    // Messages at odds with the book are counted and otherwise ignored.
    add(4, 'S', 10, 101'000);
    replica.apply(itch::OrderDelete{header, 999});                   // an unknown order
    replica.apply(itch::OrderExecuted{header, 999, 5, 6});           // an unknown order
    add(4, 'S', 10, 101'000);                                        // a reference already in use
    add(5, 'X', 10, 101'000);                                        // no such side
    replica.apply(itch::OrderReplace{header, 999, 6, 10, 101'000});  // an unknown original
    replica.apply(itch::OrderDelete{itch::Header{9, 0, 1'000}, 4});  // a stock that was never announced
    replica.apply(itch::OrderReplace{itch::Header{9, 0, 1'000}, 4, 8, 10, 101'000});  // the same
    replica.apply(itch::OrderCancel{header, 4, 50});                                  // more shares than it has
    expect(replica.stats().anomalies == 8U && book.order_count() == 0U,
           "Inconsistent messages should be counted, and an over-cancel should still remove the order.");

    const std::array<std::uint8_t, 3> garbage{'A', 1, 2};
    expect(!replica.apply(garbage) && replica.stats().invalid_messages == 1U,
           "Undecodable messages should be counted as invalid.");
    expect(replica.stats().messages.at('A') == 5U && replica.stats().messages.at('E') == 3U,
           "Messages should be counted by type.");

    // An add for a stock without a directory message names its stock, which is enough to create it.
    itch::AddOrder stray{itch::Header{2, 0, 2'000}};
    stray.order_reference = 7;
    stray.side = 'S';
    stray.shares = 5;
    stray.stock = itch::make_stock("STRAY");
    stray.price = 50'000;
    replica.apply(stray);
    const auto stray_symbol = replica.find("STRAY");
    expect(stray_symbol && replica.engine().book(*stray_symbol).order_count() == 1U,
           "An add should be able to introduce its stock.");
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// A comparison that never finds a difference would make every round-trip test pass, so each kind of difference is
// planted in a replica that otherwise matches.
void test_differences_are_found() {
    using tradematch::OrderType;
    using tradematch::Side;
    tradematch::MatchingEngine engine;
    const auto acme = engine.add_symbol("ACME");
    itch::FeedPublisher publisher(engine);
    engine.set_order_listener(&publisher);
    engine.submit(acme, {1, Side::Buy, OrderType::Limit, 100, 10'000});
    engine.submit(acme, {2, Side::Buy, OrderType::Limit, 50, 10'000});
    engine.submit(acme, {3, Side::Sell, OrderType::Limit, 70, 10'100});
    const std::vector<std::uint8_t> feed = publisher.take_bytes();
    const auto reference = [&](tradematch::OrderId order_id) {
        const auto found = publisher.reference_of(acme, order_id);
        if (!found) {
            throw TestFailure("Every resting order should have a feed reference.");
        }
        return *found;
    };
    const auto difference = [&](const std::function<void(itch::BookReplica&)>& tamper) {
        itch::BookReplica replica;
        apply_frames(replica, feed);
        tamper(replica);
        return itch::first_difference(engine, publisher, replica);
    };
    const itch::Header header{1, 0, 0};
    const itch::Stock stock = itch::make_stock("ACME");

    expect(difference([](itch::BookReplica&) {}).empty(), "An untouched replica should match.");
    expect(contains(itch::first_difference(engine, publisher, itch::BookReplica{}), "missing"),
           "A replica without the stock should be reported.");
    expect(contains(
               difference([&](itch::BookReplica& replica) { replica.apply(itch::OrderDelete{header, reference(3)}); }),
               "ask levels"),
           "A missing level should be reported.");
    expect(contains(difference([&](itch::BookReplica& replica) {
                        replica.apply(itch::OrderReplace{header, reference(3), 900, 70, 1'020'000});
                    }),
                    "prices differ"),
           "A level at another price should be reported.");
    expect(contains(difference([&](itch::BookReplica& replica) {
                        replica.apply(itch::OrderCancel{header, reference(3), 10});
                    }),
                    "quantities or order counts differ"),
           "A level with another quantity should be reported.");
    expect(contains(difference([&](itch::BookReplica& replica) {
                        replica.apply(itch::OrderReplace{header, reference(1), 901, 100, 1'000'000});
                    }),
                    "queues differ at position 0"),
           "Orders in another queue order should be reported.");
    expect(contains(difference([&](itch::BookReplica& replica) {
                        // The same references in the same order, and the same total, split differently: 60 and 90.
                        replica.apply(itch::OrderDelete{header, reference(2)});
                        itch::AddOrder add{header};
                        add.order_reference = reference(2);
                        add.side = 'B';
                        add.shares = 90;
                        add.stock = stock;
                        add.price = 1'000'000;
                        replica.apply(add);
                        replica.apply(itch::OrderCancel{header, reference(1), 40});
                    }),
                    "shows a different quantity"),
           "An order showing another quantity should be reported.");
}

template <typename Exception>
bool throws(const std::function<void()>& action) {
    try {
        action();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

void test_publisher_refuses_what_itch_cannot_carry() {
    using tradematch::OrderType;
    using tradematch::Side;
    tradematch::MatchingEngine engine;
    const auto acme = engine.add_symbol("ACME");
    expect(throws<std::invalid_argument>([&] { const itch::FeedPublisher refused(engine, 0); }),
           "A price multiplier of zero should be refused.");

    itch::FeedPublisher publisher(engine);
    engine.set_order_listener(&publisher);
    engine.submit(acme, {1, Side::Buy, OrderType::Limit, 100, 10'000});
    // ITCH prices are 32 bits of 1/10,000 dollar, so they stop at $429,496.7295.
    expect(throws<std::out_of_range>([&] { engine.submit(acme, {2, Side::Sell, OrderType::Limit, 1, 50'000'000}); }),
           "A price beyond the ITCH price field should be refused.");
    expect(!publisher.reference_of(acme, 2) && !publisher.reference_of(acme, 999) && !publisher.reference_of(42, 1),
           "Only displayed orders have references.");

    // Events that contradict what was published can only come from a broken source, and are refused.
    const tradematch::OrderEvent unknown{tradematch::OrderEvent::Type::Executed, Side::Buy, 77, 10'000, 5, 1};
    expect(throws<std::logic_error>([&] { publisher.on_order_events(acme, std::span{&unknown, 1}, {}); }),
           "An execution of an order that isn't displayed should be refused.");
    const tradematch::Trade trade{5, 1, 3, 10'000, 1, Side::Sell, false};
    const tradematch::OrderEvent misplaced{tradematch::OrderEvent::Type::Executed, Side::Buy, 1, 10'000, 1, 99};
    expect(throws<std::logic_error>(
               [&] { publisher.on_order_events(acme, std::span{&misplaced, 1}, std::span{&trade, 1}); }),
           "An execution naming a trade outside its request should be refused.");
}

void test_simulated_day_round_trips_through_a_file() {
    tradematch::SimulationConfig config;
    config.seed = 11;
    config.symbols = {"ACME", "GLOBEX"};
    config.continuous = 20'000'000'000;  // 20 simulated seconds
    config.measure_latency = false;
    tradematch::MarketSimulator simulator(config);
    itch::FeedPublisher publisher(simulator.engine());
    simulator.set_order_listener(&publisher);
    publisher.system_event('O');
    while (simulator.session() != tradematch::SessionPhase::Closed) {
        publisher.set_time(static_cast<std::uint64_t>(simulator.now() + config.step));
        publisher.set_cross_type(simulator.session() == tradematch::SessionPhase::OpeningAuction ? 'O' : 'C');
        simulator.step();
    }
    publisher.system_event('C');

    const TempFile file;
    file.write(publisher.take_bytes());
    std::FILE* input = file.open();
    itch::FileReader reader(input);
    itch::BookReplica replica;
    while (const auto message = reader.next()) {
        replica.apply(*message);
    }
    std::fclose(input);

    const std::string difference = itch::first_difference(simulator.engine(), publisher, replica);
    expect(difference.empty(), "The replayed day should rebuild the engine's books: " + difference);
    expect(replica.stats().anomalies == 0U && replica.stats().invalid_messages == 0U &&
               replica.stats().total_messages == publisher.message_count(),
           "Every message should apply cleanly.");
    for (const char type : std::string_view{"SRAEDUCQ"}) {
        expect(publisher.message_count(type) > 0U,
               std::string{"A simulated day should publish '"} + type + "' messages.");
    }
    std::cout << "       " << publisher.message_count() << " messages for a simulated day of "
              << simulator.stats().trades << " trades and " << simulator.stats().auctions << " auctions\n";
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

int run_all_tests() {
    const std::vector<TestCase> tests = {
        {"add_order_matches_the_specification", test_add_order_matches_the_specification},
        {"every_type_has_its_specified_length", test_every_type_has_its_specified_length},
        {"symbols_and_prices", test_symbols_and_prices},
        {"file_reader_streams_messages", test_file_reader_streams_messages},
        {"feed_rebuilds_the_engine_exactly", test_feed_rebuilds_the_engine_exactly},
        {"replica_volume_and_anomalies", test_replica_volume_and_anomalies},
        {"differences_are_found", test_differences_are_found},
        {"publisher_refuses_what_itch_cannot_carry", test_publisher_refuses_what_itch_cannot_carry},
        {"simulated_day_round_trips_through_a_file", test_simulated_day_round_trips_through_a_file},
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
        std::cout << "All ITCH tests passed (" << tests.size() << ").\n";
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
